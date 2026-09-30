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

u8 color_byte(float f) {
  return (u8)((int)f & 0xff);
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

  skip_distorter(dma, rs);

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
  flush();
  skip_rest();

  if (rs.log_now) {
    lg::debug("[ctr] sprite: {} 2d, {} hud, {} 3d sprites, {} draws (last 300 frames)",
              m_stats.sprites_2d, m_stats.sprites_hud, m_stats.sprites_3d, m_stats.draws);
    m_stats = Stats();
  }
}

void CtrSpriteRenderer::skip_distorter(DmaFollower& dma, CtrRenderState& rs) {
  // setup (direct, 7 qw), sine table aspect (PC), sine tables, then batches of sprite data, each
  // ending with a mscalf (see Sprite3::distort_dma)
  for (int i = 0; i < 3 && dma.current_tag_offset() != rs.next_bucket; i++) {
    dma.read_and_advance();
  }
  while (looks_like_distort_frame_data(dma)) {
    while (looks_like_distort_frame_data(dma)) {
      dma.read_and_advance();
    }
    if (dma.current_tag_vifcode0().kind == VifCode::Kind::MSCALF) {
      dma.read_and_advance();
    }
  }
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
  return vec.size_bytes >= (int)(*count * sizeof(VecData)) &&
         adgif.size_bytes >= (int)(*count * sizeof(AdGif));
}

std::vector<ctr_vertex>& CtrSpriteRenderer::bucket_for(const ctr_draw_state& st) {
  if (m_last_bucket < m_bucket_count &&
      !memcmp(&m_buckets[m_last_bucket].state, &st, sizeof(st))) {
    return m_buckets[m_last_bucket].verts;
  }
  for (size_t i = 0; i < m_bucket_count; i++) {
    if (!memcmp(&m_buckets[i].state, &st, sizeof(st))) {
      m_last_bucket = i;
      return m_buckets[i].verts;
    }
  }
  if (m_bucket_count == m_buckets.size()) {
    m_buckets.emplace_back();
  }
  auto& b = m_buckets[m_bucket_count];
  b.state = st;
  b.verts.clear();  // keeps its capacity: no allocations once warmed up
  m_last_bucket = m_bucket_count++;
  return b.verts;
}

void CtrSpriteRenderer::flush() {
  for (size_t i = 0; i < m_bucket_count; i++) {
    auto& b = m_buckets[i];
    if (!b.verts.empty()) {
      ctr_gpu_draw(&b.state, b.verts.data(), (int)b.verts.size());
      m_stats.draws++;
    }
    b.verts.clear();
  }
  m_bucket_count = 0;
  m_last_bucket = 0;
  m_have_last_ad = false;  // m_last_verts pointed into a bucket
}

void CtrSpriteRenderer::draw_chunk(u32 count, Mode mode) {
  for (u32 i = 0; i < count; i++) {
    if (mode != MODE_HUD) {
      if (m_world_left <= 0) {
        return;
      }
      m_world_left--;
    }
    const VecData& v = m_vec[i];
    const AdGif& ad = m_adgif[i];

    // GS state from the adgif (Sprite3::handle_tex0 etc.). Consecutive sprites mostly share it.
    if (!m_have_last_ad || memcmp(&ad, &m_last_ad, sizeof(AdGif)) != 0) {
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
        if (dump_dir && dumped.size() < 48 &&
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
            lg::info("[sprite dump {}] tex0 {:x} {}x{} psm {} tcc {} alpha {:x} blend {} alpha {}..{}",
                     dumped.size(), ad.tex0_data, w, h, (int)tex0.psm(), tex0.tcc(),
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
      m_last_verts = &bucket_for(st);
    }
    const ctr_draw_state& st = m_last_state;

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

    math::Vector4f corners[4];
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
      bool ok = true;
      for (int k = 0; k < 4; k++) {
        const auto& off = m_xyz_array[k];
        math::Vector4f p = pos;
        for (int r = 0; r < 3; r++) {
          p[r] += rot[0][r] * off.x() * sx + rot[1][r] * off.y() + rot[2][r] * off.z() * sy;
        }
        math::Vector4f t = transform(m_camera, p) * -1.f;
        if (t.w() == 0) {
          ok = false;
          break;
        }
        const float q = m_pfog0 / t.w();
        for (int r = 0; r < 3; r++) {
          corners[k][r] = t[r] * q + m_hvdf_offset[r];
        }
      }
      if (!ok) {
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
      const float s = angle == 0.f ? 0.f : std::sin(angle);
      const float c = angle == 0.f ? 1.f : std::cos(angle);
      const math::Vector4f r12 = (m_basis_x * c - m_basis_y * s) * scale_x;
      const math::Vector4f r13 = (m_basis_x * s + m_basis_y * c) * scale_y;
      for (int k = 0; k < 4; k++) {
        const auto& xy = m_xy_array[(k + (flag & 15)) & 7];
        corners[k] = base + r12 * xy.x() + r13 * xy.y();
      }
    }

    {
      // bounding box against the visible GS area (about 1792..2304 x 1824..2272)
      float x0 = corners[0].x(), x1 = x0, y0 = corners[0].y(), y1 = y0;
      for (int k = 1; k < 4; k++) {
        x0 = std::min(x0, corners[k].x());
        x1 = std::max(x1, corners[k].x());
        y0 = std::min(y0, corners[k].y());
        y1 = std::max(y1, corners[k].y());
      }
      if (x1 < 1792.f || x0 > 2304.f || y1 < 1824.f || y0 > 2272.f) {
        continue;
      }
    }
    ctr_vertex out[4];
    for (int k = 0; k < 4; k++) {
      // GS screen -> ctr_gpu coordinates (see CtrDirect::handle_xyz)
      out[k].x = (corners[k].x() - 2048.f) / 256.f;
      out[k].y = -(corners[k].y() - 2048.f) / 112.f;
      out[k].z = std::clamp(corners[k].z() / 16777215.f, 0.f, 1.f);
      out[k].s = m_st_array[k].x();
      out[k].t = m_st_array[k].y();
      out[k].r = color_byte(v.rgba.x());
      out[k].g = color_byte(v.rgba.y());
      out[k].b = color_byte(v.rgba.z());
      out[k].a = (u8)(color_byte(v.rgba.w()) * alpha_scale);
    }
    // vertex k is corner k of a strip 0, 1, 3, 2 (Sprite3::do_block_common)
    static const int kTri[6] = {0, 1, 3, 3, 1, 2};
    auto& verts = *m_last_verts;
    const size_t n = verts.size();
    verts.resize(n + 6);
    for (int k = 0; k < 6; k++) {
      verts[n + k] = out[kTri[k]];
    }
  }
}
