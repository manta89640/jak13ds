/*!
 * @file CtrSprite.cpp
 * (AI-assisted)
 * See CtrSprite.h. DMA: Sprite3::render_jak1 / distort_dma; math: sprite3_3d.vert.
 */

#include "CtrSprite.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "common/dma/dma.h"
#include "common/log/log.h"
#include "common/util/FileUtil.h"

#include "game/graphics/ctr/CtrDirect.h"
#include "game/graphics/ctr/CtrSettings.h"
#include "game/graphics/ctr/CtrVram.h"

namespace {

// SpriteFrameDataJak1 (sprite_common.h), which can't be included here (OpenGL renderer headers)
struct FrameDataJak1 {
  math::Vector4f xy_array[8];
  math::Vector4f st_array[4];
  math::Vector4f xyz_array[4];
  math::Vector4f hmge_scale;
  float pfog0;
  float deg_to_rad;
  float min_scale;
  float inv_area;
  u64 adgif_giftag[2];
  u64 sprite_2d_giftag[2];
  u64 sprite_2d_giftag2[2];
  math::Vector4f sincos[5];
  math::Vector4f basis_x;
  math::Vector4f basis_y;
  u64 sprite_3d_giftag[2];
  u64 screen_shader[10];
  u64 clipped_giftag[2];
  math::Vector4f inv_hmge_scale;
  math::Vector4f stq_offset;
  math::Vector4f stq_scale;
  math::Vector4f rgba_plain;
  u64 warp_giftag[2];
  float fog_min;
  float fog_max;
  float max_scale;
  float bonus;
};
static_assert(sizeof(FrameDataJak1) == 0x290);

constexpr u32 kProgSprites2dGrp0 = 3;
constexpr u32 kHudMatrixBytes = 16 * 4 + 16 + 75 * 16;

bool looks_like_2d_chunk_start(const DmaFollower& dma) {
  return dma.current_tag().qwc == 1 && dma.current_tag().kind == DmaTag::Kind::CNT;
}

bool looks_like_distort_frame_data(const DmaFollower& dma) {
  return dma.current_tag().kind == DmaTag::Kind::CNT &&
         dma.current_tag_vifcode0().kind == VifCode::Kind::NOP &&
         dma.current_tag_vifcode1().kind == VifCode::Kind::UNPACK_V4_32;
}

// GLSL matrix_transform: column major, m[3] + m[0] * x + m[1] * y + m[2] * z
math::Vector4f transform(const float* m, const math::Vector4f& p) {
  math::Vector4f r;
  for (int i = 0; i < 4; i++) {
    r[i] = m[12 + i] + m[i] * p.x() + m[4 + i] * p.y() + m[8 + i] * p.z();
  }
  return r;
}

s32 as_int(float f) {
  s32 r;
  memcpy(&r, &f, 4);
  return r;
}

// (AI-assisted) all ten words of two sprites' AdGif data equal (newlib's memcmp took ~11
// instructions per word, for every sprite)
template <typename AdGif>
bool same_adgif(const AdGif& a, const AdGif& b) {
  return ((a.tex0_data ^ b.tex0_data) | (a.tex0_addr ^ b.tex0_addr) | (a.tex1_data ^ b.tex1_data) |
          (a.tex1_addr ^ b.tex1_addr) | (a.mip_data ^ b.mip_data) | (a.mip_addr ^ b.mip_addr) |
          (a.clamp_data ^ b.clamp_data) | (a.clamp_addr ^ b.clamp_addr) |
          (a.alpha_data ^ b.alpha_data) | (a.alpha_addr ^ b.alpha_addr)) == 0;
}

u8 color_byte(float f) {
  return (u8)((int)f & 0xff);
}

// sin and cos of an angle in radians, to ~1e-5: sprite rotations don't need libm's (slow) ones
void fast_sincos(float a, float* s, float* c) {
  constexpr float kInvTwoPi = 0.159154943f, kTwoPi = 6.28318531f, kPi = 3.14159265f,
                  kHalfPi = 1.57079633f;
  // [-pi, pi]. (AI-assisted) floor without the libm call: (int) rounds towards 0
  const float y = a * kInvTwoPi + 0.5f;
  int n = (int)y;
  if ((float)n > y) {
    n--;
  }
  a -= kTwoPi * (float)n;
  // cos(a) = sin(a + pi/2); both from sin on [-pi/2, pi/2]
  auto sin_small = [](float x) {
    const float x2 = x * x;
    return x * (1.f + x2 * (-1.66666667e-1f +
                            x2 * (8.33333333e-3f + x2 * (-1.98412698e-4f + x2 * 2.75573192e-6f))));
  };
  auto sin_any = [&](float x) {  // x in [-pi, pi]
    if (x > kHalfPi) {
      x = kPi - x;
    } else if (x < -kHalfPi) {
      x = -kPi - x;
    }
    return sin_small(x);
  };
  *s = sin_any(a);
  float b = a + kHalfPi;
  if (b > kPi) {
    b -= kTwoPi;
  }
  *c = sin_any(b);
}
}  // namespace

CtrSpriteRenderer::CtrSpriteRenderer(std::string name, int id, CtrVram* vram)
    : CtrBucketRenderer(std::move(name), id),
      m_vram(vram),
      m_direct(std::make_unique<CtrDirect>(vram)) {
  memset(m_camera, 0, sizeof(m_camera));
  memset(m_hud_matrix, 0, sizeof(m_hud_matrix));
}

CtrSpriteRenderer::~CtrSpriteRenderer() = default;

void CtrSpriteRenderer::render(DmaFollower& dma, CtrRenderState& rs) {
  auto skip_rest = [&]() {
    while (dma.current_tag_offset() != rs.next_bucket) {
      dma.read_and_advance();
    }
  };
  if (dma.current_tag_offset() == rs.next_bucket) {
    return;
  }
  dma.read_and_advance();  // jump from the bucket to the sprite data
  if (dma.current_tag().kind == DmaTag::Kind::CALL) {
    skip_rest();  // the sprite renderer didn't run
    return;
  }
  // (AI-assisted) this frame's counts, for the slow frame line at the end
  const double t_start = ctr_gpu_time_ms();
  const Stats stats0 = m_stats;
  const CtrVram::Stats vram0 = m_vram->stats();
  m_distort_ms = 0;
  build_sprites(dma, rs);
  log_slow_frame(rs, ctr_gpu_time_ms() - t_start, stats0, vram0);
}

void CtrSpriteRenderer::log_slow_frame(CtrRenderState& rs,
                                       double ms,
                                       const Stats& s0,
                                       const CtrVram::Stats& v0) {
  // (AI-assisted) a frame whose sprites took long: what it had (counters at info level, at most
  // one line per 5 s; the frames in between are counted)
  const CtrVram::Stats& v = m_vram->stats();
  const int frame_uploads = v.uploads - m_vram_uploads_mark;
  m_vram_uploads_mark = v.uploads;
  if (ms > 3.0) {
    m_slow_frames++;
    const double now = ctr_gpu_time_ms();
    if (now - m_last_slow_log >= 5000.0) {
      m_last_slow_log = now;
      const Stats& s = m_stats;
      lg::info(
          "[ctr] sprite build {:.1f} ms (distort {:.1f} ms; {} frames over 3 ms since the last "
          "line): {} 2d ({} made smaller), {} hud, {} 3d, {} distort sprites, {} draws; "
          "textures {} decoded, {} revived, {} cached lookups; VRAM chunks {} written, {} already "
          "there; {} page uploads this frame",
          ms, m_distort_ms, m_slow_frames, s.sprites_2d - s0.sprites_2d, s.capped - s0.capped,
          s.sprites_hud - s0.sprites_hud, s.sprites_3d - s0.sprites_3d, s.distort - s0.distort,
          s.draws - s0.draws, v.decoded - v0.decoded, v.revived - v0.revived,
          v.cached - v0.cached, v.chunks_written - v0.chunks_written,
          v.chunks_skipped - v0.chunks_skipped, frame_uploads);
      m_slow_frames = 0;
    }
  }
  if (rs.log_now) {
    lg::debug("[ctr] sprite: {} 2d ({} made smaller), {} hud, {} 3d sprites, {} distort, {} draws "
              "(since the last stats)",
              m_stats.sprites_2d, m_stats.capped, m_stats.sprites_hud, m_stats.sprites_3d,
              m_stats.distort, m_stats.draws);
    m_stats = Stats();
  }
}

void CtrSpriteRenderer::build_sprites(DmaFollower& dma, CtrRenderState& rs) {
  auto skip_rest = [&]() {
    while (dma.current_tag_offset() != rs.next_bucket) {
      dma.read_and_advance();
    }
  };
  m_last_tex = -1;  // textures may have changed since the last frame
  m_last_tex_valid = false;
  m_have_last_ad = false;
  m_world_left = ctr_settings().sprites ? ctr_settings().max_sprites : 0;
  // direct GS data sent before the sprites
  m_direct->reset_state();
  while (dma.current_tag().qwc != 7 && dma.current_tag_offset() != rs.next_bucket) {
    auto d = dma.read_and_advance();
    if (d.size_bytes) {
      m_direct->render_vif(d.vif0(), d.vif1(), d.data, d.size_bytes);
    }
  }
  m_direct->flush();
  if (dma.current_tag_offset() == rs.next_bucket) {
    return;
  }

  // the distorter first (like Sprite3::render_jak1): it reads the screen as drawn so far
  if (read_distorter(dma, rs) && ctr_settings().sprites && ctr_settings().distort) {
    const double td = ctr_gpu_time_ms();
#ifndef __3DS__
    ctr_gpu_soft_bucket("sprite-distort");
#endif
    draw_distorter();
    m_distort_ms = ctr_gpu_time_ms() - td;
  }

  // frame setup: direct data (3 qw), frame data, mscalf, base/offset
  auto direct_setup = dma.read_and_advance();
  auto frame_data = dma.read_and_advance();
  if (direct_setup.size_bytes != 3 * 16 || frame_data.size_bytes != (int)sizeof(FrameDataJak1)) {
    skip_rest();
    return;
  }
  FrameDataJak1 fd;
  memcpy(&fd, frame_data.data, sizeof(fd));
  memcpy(m_xy_array, fd.xy_array, sizeof(m_xy_array));
  memcpy(m_st_array, fd.st_array, sizeof(m_st_array));
  memcpy(m_xyz_array, fd.xyz_array, sizeof(m_xyz_array));
  m_pfog0 = fd.pfog0;
  m_deg_to_rad = fd.deg_to_rad;
  m_min_scale = fd.min_scale;
  m_inv_area = fd.inv_area;
  m_max_scale = fd.max_scale;
  m_fog_min = fd.fog_min;
  m_fog_max = fd.fog_max;
  m_basis_x = fd.basis_x;
  m_basis_y = fd.basis_y;
  {
    // largest distance of a sprite corner from its center, per unit of scale
    float b = 0, xy = 0;
    for (int c = 0; c < 2; c++) {
      b = std::max({b, std::abs(m_basis_x[c]), std::abs(m_basis_y[c])});
    }
    for (auto& a : m_xy_array) {
      xy = std::max({xy, std::abs(a.x()), std::abs(a.y())});
    }
    m_corner_reach = b * xy;
  }
  m_size_cap = ctr_settings().sprite_max_size * 224.f;
  for (int k = 0; k < 4; k++) {
    for (int p = 0; p < 2; p++) {
      const float st_mul = p ? 2.f : 1.f, st_add = p ? -1.f : 0.f;
      m_st_out[p][k][0] = m_st_array[k].x() * st_mul + st_add;
      m_st_out[p][k][1] = m_st_array[k].y() * st_mul + st_add;
    }
  }
  dma.read_and_advance();  // mscalf
  dma.read_and_advance();  // base / offset

  // 3D matrix data: camera + hvdf offset
  auto matrix_data = dma.read_and_advance();
  if (matrix_data.size_bytes != 5 * 16) {
    skip_rest();
    return;
  }
  memcpy(m_camera, matrix_data.data, 64);
  memcpy(&m_hvdf_offset, matrix_data.data + 64, 16);

  // group 0: world space 2D sprites and 3D sprites
  while (looks_like_2d_chunk_start(dma)) {
    u32 count, program;
    if (!read_chunk(dma, &count, &program)) {
      skip_rest();
      return;
    }
    draw_chunk(count, program == kProgSprites2dGrp0 ? MODE_2D : MODE_3D);
  }

#ifndef __3DS__
  ctr_gpu_soft_bucket("sprite-world");
#endif
  flush();
  // fake shadow (nop + flushe)
  if (dma.current_tag_offset() != rs.next_bucket) {
    dma.read_and_advance();
  }

  // group 1: HUD
  if (dma.current_tag_offset() != rs.next_bucket) {
    auto mat = dma.read_and_advance();
    if (mat.size_bytes == (int)kHudMatrixBytes) {
      memcpy(m_hud_matrix, mat.data, 64);
      memcpy(m_hud_hvdf, mat.data + 64, 76 * 16);
      while (looks_like_2d_chunk_start(dma)) {
        u32 count, program;
        if (!read_chunk(dma, &count, &program)) {
          break;
        }
        draw_chunk(count, MODE_HUD);
      }
    }
  }
#ifndef __3DS__
  ctr_gpu_soft_bucket("sprite-hud");
#endif
  flush();
  skip_rest();
}

bool CtrSpriteRenderer::read_distorter(DmaFollower& dma, CtrRenderState& rs) {
  // setup (direct, 7 qw), sine table aspect (PC, 1 qw), sine tables, then batches of sprite data,
  // each ending with a mscalf (see Sprite3::distort_dma). Everything is read even when something
  // doesn't look right (then nothing is drawn).
  m_distort_count = 0;
  bool ok = true;
  auto more = [&]() { return dma.current_tag_offset() != rs.next_bucket; };
  for (int i = 0; i < 3 && more(); i++) {
    auto t = dma.read_and_advance();
    if (i == 0) {
      ok &= t.size_bytes == 7 * 16;
    } else if (i == 2) {
      if (t.size_bytes == (int)sizeof(DistortTables)) {
        memcpy(&m_distort_tables, t.data, sizeof(DistortTables));
      } else {
        ok = false;
      }
    }
  }
  if (m_distort_sprites.empty()) {
    m_distort_sprites.resize(kMaxDistortSprites);
  }
  int idx = 0;
  u32 total = 0;
  while (looks_like_distort_frame_data(dma)) {
    while (looks_like_distort_frame_data(dma)) {
      const u32 dest = dma.current_tag_vifcode1().immediate & 0x3ff;
      auto t = dma.read_and_advance();
      if (dest == 511) {
        u32 n;
        memcpy(&n, t.data, 4);
        total += n;
      } else {
        const int n = t.size_bytes / (int)sizeof(DistortSprite);
        const int fit = std::max(0, std::min(n, kMaxDistortSprites - idx));
        memcpy(&m_distort_sprites[idx], t.data, fit * sizeof(DistortSprite));
        idx += fit;
      }
    }
    if (dma.current_tag_vifcode0().kind == VifCode::Kind::MSCALF) {
      dma.read_and_advance();
    }
  }
  m_distort_count = ok ? std::min<int>(idx, (int)total) : 0;
  return ok;
}

void CtrSpriteRenderer::draw_distorter() {
  if (m_distort_count <= 0) {
    return;
  }
  m_distort_verts.clear();
  const auto& c = m_distort_tables.color;
  const u8 r = (u8)std::min<u32>(c.x(), 255), g = (u8)std::min<u32>(c.y(), 255),
           b = (u8)std::min<u32>(c.z(), 255), a = (u8)std::min<u32>(c.w(), 255);
  constexpr float kInvX = 1.f / 256.f, kInvY = 1.f / 112.f, kInvZ = 1.f / 16777215.f;
  // GS position + the GS frame texture coordinates of the screen point to show -> vertex
  auto vert = [&](const math::Vector3f& p, const math::Vector2f& st) {
    ctr_vertex v;
    v.x = (p.x() - 2048.f) * kInvX;
    v.y = (2048.f - p.y()) * kInvY;
    v.z = std::clamp(p.z() * kInvZ, 0.f, 1.f);
    // the GS frame texture is 512 x 256 texels from (1792, 2048 - 112): screen x = 2s - 1, and
    // the 224 visible rows are t * 256 from the top
    ctr_gpu_screen_uv(st.x() * 2.f - 1.f, 1.f - st.y() * (256.f * kInvY), &v.s, &v.t);
    v.r = r;
    v.g = g;
    v.b = b;
    v.a = a;
    return v;
  };
  ctr_vertex strip[5 * 11];
  // (AI-assisted) each fan is a triangle strip (the VU program's): all of them go in one strip
  // draw, joined by repeating the last vertex of one and the first of the next (the triangles in
  // between have no area). As a triangle list it was 3 (n - 2) vertices per fan instead of n + 2:
  // up to 256 x 159 vertices (~1 MB of the frame's vertex buffer, and the GPU's vertex work)
  m_distort_verts.reserve(256 * (5 * 11 + 2));
  for (int i = 0; i < m_distort_count && i < 256; i++) {
    const DistortSprite& sp = m_distort_sprites[i];
    const u32 slices = sp.flag;
    if (slices < 3 || slices > 11) {
      continue;
    }
    int e = (int)m_distort_tables.ientry[slices - 3].x() - 352;
    if (e < 0 || e + 2 * (int)slices + 2 > 128) {
      continue;
    }
    // the triangle strip of the VU program (Sprite3::distort_setup): per slice two points on the
    // inner circle (undistorted), two on the outer one (texture radius z), and the center
    const ctr_vertex center = vert(sp.xyz, sp.st);
    int n = 0;
    for (u32 k = 0; k < slices; k++) {
      const math::Vector3f v06 = m_distort_tables.entry[e].xyz();
      const math::Vector2f v07 = m_distort_tables.entry[e + 1].xy();
      const math::Vector3f v08 = m_distort_tables.entry[e + 2].xyz();
      const math::Vector2f v09 = m_distort_tables.entry[e + 3].xy();
      e += 2;
      strip[n++] = vert(v06 * sp.rgba.x() + sp.xyz, v07 * sp.rgba.x() + sp.st);
      strip[n++] = vert(v08 * sp.rgba.x() + sp.xyz, v09 * sp.rgba.x() + sp.st);
      strip[n++] = vert(v06 * sp.rgba.y() + sp.xyz, v07 * sp.rgba.z() + sp.st);
      strip[n++] = vert(v08 * sp.rgba.y() + sp.xyz, v09 * sp.rgba.z() + sp.st);
      strip[n++] = center;
    }
    if (!m_distort_verts.empty()) {
      const ctr_vertex last = m_distort_verts.back();
      m_distort_verts.push_back(last);
      m_distort_verts.push_back(strip[0]);
    }
    m_distort_verts.insert(m_distort_verts.end(), strip, strip + n);
    m_stats.distort++;
  }
  if (m_distort_verts.empty()) {
    return;
  }
  ctr_draw_state st;
  memset(&st, 0, sizeof(st));
  st.tex = ctr_gpu_copy_screen();
  if (st.tex < 0) {
    return;
  }
  st.tcc = 0;     // the copy's alpha means nothing: alpha from the color (GS: RGB framebuffer)
  st.filter = 1;  // tex1 mmag
  st.clamp_s = st.clamp_t = 1;
  st.blend = CTR_BLEND_ALPHA;
  st.atest = CTR_TEST_ALWAYS;
  st.ztest = CTR_TEST_GEQUAL;
  st.zwrite = 0;
  ctr_gpu_draw_strip(&st, m_distort_verts.data(), (int)m_distort_verts.size());
  m_stats.draws++;
}

bool CtrSpriteRenderer::read_chunk(DmaFollower& dma, u32* count, u32* program) {
  // header, vector data, adgif data, program call
  auto header = dma.read_and_advance();
  u32 h[4];
  memcpy(h, header.data, 16);
  *count = std::min<u32>(h[0], kSpritesPerChunk);
  auto vec = dma.read_and_advance();
  memcpy(m_vec, vec.data, std::min<size_t>(vec.size_bytes, sizeof(m_vec)));
  auto adgif = dma.read_and_advance();
  memcpy(m_adgif, adgif.data, std::min<size_t>(adgif.size_bytes, sizeof(m_adgif)));
  auto run = dma.read_and_advance();
  if (run.vifcode1().kind != VifCode::Kind::MSCAL) {
    return false;
  }
  *program = run.vifcode1().immediate;
  return vec.size_bytes >= *count * sizeof(VecData) &&
         adgif.size_bytes >= *count * sizeof(AdGif);
}

CtrSpriteRenderer::Bucket& CtrSpriteRenderer::bucket_for(const ctr_draw_state& st) {
  if (m_last_bucket < m_bucket_count &&
      !memcmp(&m_buckets[m_last_bucket].state, &st, sizeof(st))) {
    return m_buckets[m_last_bucket];
  }
  for (size_t i = 0; i < m_bucket_count; i++) {
    if (!memcmp(&m_buckets[i].state, &st, sizeof(st))) {
      m_last_bucket = i;
      return m_buckets[i];
    }
  }
  if (m_bucket_count == m_buckets.size()) {
    m_buckets.emplace_back();
  }
  auto& b = m_buckets[m_bucket_count];
  b.state = st;
  b.verts.clear();  // keeps its capacity: no allocations once warmed up
  b.clip.clear();
  m_last_bucket = m_bucket_count++;
  return b;
}

void CtrSpriteRenderer::flush() {
  for (size_t i = 0; i < m_bucket_count; i++) {
    auto& b = m_buckets[i];
    if (!b.verts.empty()) {
      // 4 vertices per sprite (ctr_gpu_draw_quads makes the two triangles)
      ctr_gpu_draw_quads(&b.state, b.verts.data(), (int)(b.verts.size() / 4));
      m_stats.draws++;
    }
    if (!b.clip.empty()) {
      ctr_gpu_draw_clip(&b.state, b.clip.data(), (int)b.clip.size());
      m_stats.draws++;
    }
    b.verts.clear();
    b.clip.clear();
  }
  m_bucket_count = 0;
  m_last_bucket = 0;
  m_have_last_ad = false;  // m_last_bucket_ptr pointed into a bucket
}

void CtrSpriteRenderer::draw_chunk(u32 count, Mode mode) {
  constexpr float kInvX = 1.f / 256.f, kInvY = 1.f / 112.f, kInvZ = 1.f / 16777215.f;
  // (AI-assisted) MODE_3D: a corner's GS position is t * (pfog0 / w) + hvdf, and its clip space
  // position that times w, so the division cancels: c = t * k + w * o (see the corner loop)
  const float k3x = m_pfog0 * (1.f / 256.f), o3x = (m_hvdf_offset.x() - 2048.f) * (1.f / 256.f);
  const float k3y = -m_pfog0 * (1.f / 112.f), o3y = (2048.f - m_hvdf_offset.y()) * (1.f / 112.f);
  const float k3z = m_pfog0 * (1.f / 8388607.5f),
              o3z = m_hvdf_offset.z() * (1.f / 8388607.5f) - 1.f;
  for (u32 i = 0; i < count; i++) {
    if (mode != MODE_HUD) {
      if (m_world_left <= 0) {
        return;
      }
      m_world_left--;
    }
    const VecData& v = m_vec[i];
    const AdGif& ad = m_adgif[i];

    // sprite3_3d.vert
    const math::Vector4f pos(v.xyz_sx.x(), v.xyz_sx.y(), v.xyz_sx.z(), 1.f);
    const float sx = v.xyz_sx.w();
    const float sy = v.flag_rot_sy.w();
    const s32 flag = as_int(v.flag_rot_sy.x());
    const s32 matrix = as_int(v.flag_rot_sy.y());
    math::Vector4f tp = transform(mode == MODE_HUD ? m_hud_matrix : m_camera, pos);
    if (tp.w() == 0) {
      continue;
    }
    float Q = m_pfog0 / tp.w();
    float scale_y = sy * Q;
    float scale_x = sx * Q;
    float alpha_scale = std::min(scale_x * scale_y * m_inv_area, 1.f);
    // (AI-assisted) a world sprite that ends up (nearly) transparent is skipped before any corner
    // math: the game fades sprites out as they get small on screen, so these are mostly sub-pixel
    // particles, and each one still cost a quad of blending on the 3DS GPU
    if (mode != MODE_HUD && color_byte(v.rgba.w()) * alpha_scale < 2.f) {
      continue;
    }

    float corners[4][3];  // x, y, z
    ctr_clip_vertex clip[4];  // MODE_3D: the corners in clip space (sprite3_3d.vert's output)
    if (mode == MODE_3D) {
      // rotation from the quaternion in flag_rot_sy.xyz
      const float qx = v.flag_rot_sy.x(), qy = v.flag_rot_sy.y(), qz = v.flag_rot_sy.z();
      const float qr = std::sqrt(std::abs(1.f - (qx * qx + qy * qy + qz * qz)));
      float rot[3][3];  // rot[col][row]
      rot[0][0] = 1.f - 2.f * (qy * qy + qz * qz);
      rot[1][0] = 2.f * (qx * qy - qz * qr);
      rot[2][0] = 2.f * (qx * qz + qy * qr);
      rot[0][1] = 2.f * (qx * qy + qz * qr);
      rot[1][1] = 1.f - 2.f * (qx * qx + qz * qz);
      rot[2][1] = 2.f * (qy * qz - qx * qr);
      rot[0][2] = 2.f * (qx * qz - qy * qr);
      rot[1][2] = 2.f * (qy * qz + qx * qr);
      rot[2][2] = 1.f - 2.f * (qx * qx + qy * qy);
      // (AI-assisted) Like sprite3_3d.vert: the GS screen position times w, in clip space, so the
      // GPU clips a sprite that reaches behind the camera (projected on the CPU, such a corner was
      // mirrored: the sprite popped away or stretched over the screen)
      bool ok = true;
      int out_left = 0, out_right = 0, out_bottom = 0, out_top = 0, behind = 0;
      for (int k = 0; k < 4; k++) {
        const auto& off = m_xyz_array[k];
        math::Vector4f p = pos;
        for (int r = 0; r < 3; r++) {
          p[r] += rot[0][r] * off.x() * sx + rot[1][r] * off.y() + rot[2][r] * off.z() * sy;
        }
        math::Vector4f t = transform(m_camera, p) * -1.f;
        const float w = t.w();
        if (w == 0) {
          ok = false;
          break;
        }
        // GS x = t.x * pfog0 / w + hvdf.x; clip x = (GS x - 2048) / 256 * w (y, z alike): no
        // divide per corner (AI-assisted)
        auto& c = clip[k];
        c.x = t.x() * k3x + w * o3x;
        c.y = t.y() * k3y + w * o3y;
        c.z = t.z() * k3z + w * o3z;
        c.w = w;
        out_left += c.x < -c.w;
        out_right += c.x > c.w;
        out_bottom += c.y < -c.w;
        out_top += c.y > c.w;
        behind += c.w < 0;
      }
      // all four corners beyond one side of the screen, or behind the camera: nothing to draw
      if (!ok || out_left == 4 || out_right == 4 || out_bottom == 4 || out_top == 4 ||
          behind == 4) {
        continue;
      }
      m_stats.sprites_3d++;
    } else {
      math::Vector4f base = tp;
      for (int r = 0; r < 3; r++) {
        base[r] *= Q;
      }
      const math::Vector4f& hvdf =
          mode == MODE_HUD ? m_hud_hvdf[(matrix >= 0 && matrix < 76) ? matrix : 0] : m_hvdf_offset;
      base += hvdf;
      if (mode == MODE_HUD) {
        scale_y = std::max(scale_y, m_min_scale);
        scale_x = std::max(scale_x, m_min_scale);
        m_stats.sprites_hud++;
      } else {
        scale_y = std::clamp(scale_y, m_min_scale, m_max_scale);
        scale_x = std::clamp(scale_x, m_min_scale, m_max_scale);
        // (AI-assisted) config.ini sprite_max_size: a particle or glow close to the camera covers
        // a big part of the screen, and every covered pixel is blended (the 3DS GPU's fill rate):
        // at most that fraction of the screen height (448 GS units) across
        const float cap = m_size_cap;
        const float half = std::max(std::abs(scale_x), std::abs(scale_y)) * m_corner_reach;
        if (cap > 0.f && half > cap) {
          const float k = cap / half;
          scale_x *= k;
          scale_y *= k;
          m_stats.capped++;
        }
        m_stats.sprites_2d++;
      }
      if (mode == MODE_2D) {
        // quick reject: center further off screen than the sprite can reach
        const float reach = (std::abs(scale_x) + std::abs(scale_y)) * m_corner_reach;
        if (base.x() + reach < 1792.f || base.x() - reach > 2304.f ||
            base.y() + reach < 1824.f || base.y() - reach > 2272.f) {
          continue;
        }
      }
      const float angle = v.flag_rot_sy.z() * m_deg_to_rad;
      float s = 0.f, c = 1.f;
      if (angle != 0.f) {
        fast_sincos(angle, &s, &c);
      }
      // (AI-assisted) the two axes and the center straight in ctr_gpu coordinates: the GS screen
      // mapping (CtrDirect::handle_xyz) folded in once instead of per corner
      const float ax = (m_basis_x[0] * c - m_basis_y[0] * s) * scale_x * kInvX;
      const float ay = (m_basis_x[1] * c - m_basis_y[1] * s) * scale_x * -kInvY;
      const float az = (m_basis_x[2] * c - m_basis_y[2] * s) * scale_x * kInvZ;
      const float bx = (m_basis_x[0] * s + m_basis_y[0] * c) * scale_y * kInvX;
      const float by = (m_basis_x[1] * s + m_basis_y[1] * c) * scale_y * -kInvY;
      const float bz = (m_basis_x[2] * s + m_basis_y[2] * c) * scale_y * kInvZ;
      const float cx = (base.x() - 2048.f) * kInvX, cy = (2048.f - base.y()) * kInvY,
                  cz = base.z() * kInvZ;
      for (int k = 0; k < 4; k++) {
        const auto& xy = m_xy_array[(k + (flag & 15)) & 7];
        corners[k][0] = cx + ax * xy.x() + bx * xy.y();
        corners[k][1] = cy + ay * xy.x() + by * xy.y();
        corners[k][2] = cz + az * xy.x() + bz * xy.y();
      }
    }

    if (mode != MODE_3D) {
      // bounding box against the visible GS area (about 1792..2304 x 1824..2272: x -1..1,
      // y -2..2 in ctr_gpu coordinates)
      float x0 = corners[0][0], x1 = x0, y0 = corners[0][1], y1 = y0;
      for (int k = 1; k < 4; k++) {
        x0 = std::min(x0, corners[k][0]);
        x1 = std::max(x1, corners[k][0]);
        y0 = std::min(y0, corners[k][1]);
        y1 = std::max(y1, corners[k][1]);
      }
      if (x1 < -1.f || x0 > 1.f || y1 < -2.f || y0 > 2.f) {
        continue;
      }
    }
    // Resolve GS state only for sprites that survived the screen bounds test.
    // Texture lookup can decode/upload a texture, and offscreen sprites never use it.
    if (!m_have_last_ad || !same_adgif(ad, m_last_ad)) {
      m_last_ad = ad;
      m_have_last_ad = true;
      ctr_draw_state& st = m_last_state;
      memset(&st, 0, sizeof(st));
      GsTex0 tex0(ad.tex0_data);
      if (ad.tex0_data != m_last_tex0 || !m_last_tex_valid) {
        const CtrTexture* tex = m_vram->get_texture(ad.tex0_data);
        m_last_tex0 = ad.tex0_data;
        m_last_tex = tex ? tex->handle : -1;
        m_last_tex_valid = true;  // also for unsupported formats: don't retry every sprite
      }
      st.tex = m_last_tex;
      st.tcc = tex0.tcc();
#ifndef __3DS__
      {
        // debugging on PC: OPENGOAL_SPRITE_DUMP=<dir> writes the first sprite textures
        static const char* dump_dir = getenv("OPENGOAL_SPRITE_DUMP");
        static std::vector<u64> dumped;
        if (dump_dir && dumped.size() < 200 &&
            std::find(dumped.begin(), dumped.end(), ad.tex0_data) == dumped.end()) {
          dumped.push_back(ad.tex0_data);
          std::vector<u32> rgba;
          int w = 0, h = 0;
          if (m_vram->decode_for_cpu(ad.tex0_data, &rgba, &w, &h) && w > 0 && h > 0) {
            u32 amin = 255, amax = 0;
            for (u32 c : rgba) {
              amin = std::min(amin, c >> 24);
              amax = std::max(amax, c >> 24);
            }
            lg::info("[sprite dump {}] handle {} tex0 {:x} {}x{} psm {} tcc {} alpha {:x} blend {} alpha {}..{}",
                     dumped.size(), st.tex, ad.tex0_data, w, h, (int)tex0.psm(), tex0.tcc(),
                     ad.alpha_data, (int)ctr_blend_from_gs_alpha(ad.alpha_data), amin, amax);
            try {
              file_util::write_rgba_png(
                  fs::path(dump_dir) / fmt::format("sprite_{:02d}.png", dumped.size()),
                  rgba.data(), w, h);
            } catch (std::exception&) {
            }
          }
        }
      }
#endif
      st.filter = (ad.tex1_data >> 5) & 1;  // MMAG
      bool zwrite = false;
      if ((u8)ad.clamp_addr == (u8)GsRegisterAddress::ZBUF_1) {
        zwrite = !GsZbuf(ad.clamp_data).zmsk();
      } else {
        st.clamp_s = (ad.clamp_data & 0b001) != 0;
        st.clamp_t = (ad.clamp_data & 0b100) != 0;
      }
      st.blend = ctr_blend_from_gs_alpha(ad.alpha_data);
      st.fix = GsAlpha(ad.alpha_data).fix();
      // default mode: z test GEQUAL without writes, alpha blending, alpha test GEQUAL 38 with
      // FB_ONLY (only matters for depth writes)
      st.ztest = CTR_TEST_GEQUAL;
      st.zwrite = zwrite;
      st.atest = zwrite ? CTR_TEST_GEQUAL : CTR_TEST_ALWAYS;
      st.aref = 38;
      // (AI-assisted) a radial glow texture: from the procedural texture unit (no texels read);
      // its texture coordinates then go from -1 to 1 (see m_proc below)
      if (mode != MODE_HUD && ctr_gpu_tex_radial(st.tex)) {
        st.flags |= CTR_STATE_PROCTEX;
      }
      m_last_bucket_ptr = &bucket_for(st);
    }

    // vertex k is corner k of a strip 0, 1, 3, 2 (Sprite3::do_block_common): the order of
    // ctr_gpu_draw_quads
    const u8 r = color_byte(v.rgba.x()), g = color_byte(v.rgba.y()), b = color_byte(v.rgba.z());
    const u8 a = (u8)(color_byte(v.rgba.w()) * alpha_scale);
    // procedural texture (radial glow): texture coordinates centered on 0
    const bool proc = (m_last_bucket_ptr->state.flags & CTR_STATE_PROCTEX) != 0;
    const float(*st_out)[2] = m_st_out[proc ? 1 : 0];
    if (mode == MODE_3D) {
      // the triangles (0, 1, 3) and (3, 1, 2) of the strip, in clip space
      for (int k = 0; k < 4; k++) {
        clip[k].s = st_out[k][0];
        clip[k].t = st_out[k][1];
        clip[k].r = r;
        clip[k].g = g;
        clip[k].b = b;
        clip[k].a = a;
      }
      ctr_clip_vertex* cv = m_last_bucket_ptr->clip.grow(6);
      cv[0] = clip[0];
      cv[1] = clip[1];
      cv[2] = clip[3];
      cv[3] = clip[3];
      cv[4] = clip[1];
      cv[5] = clip[2];
      continue;
    }
    ctr_vertex* out = m_last_bucket_ptr->verts.grow(4);
    for (int k = 0; k < 4; k++) {
      // (already in ctr_gpu coordinates)
      out[k].x = corners[k][0];
      out[k].y = corners[k][1];
      out[k].z = std::clamp(corners[k][2], 0.f, 1.f);
      out[k].s = st_out[k][0];
      out[k].t = st_out[k][1];
      out[k].r = r;
      out[k].g = g;
      out[k].b = b;
      out[k].a = a;
    }
  }
}
