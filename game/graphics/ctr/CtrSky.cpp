/*!
 * @file CtrSky.cpp
 * (AI-assisted)
 * See CtrSky.h.
 */

#include "CtrSky.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>

#include "common/dma/dma.h"
#include "common/dma/gs.h"
#include "common/log/log.h"

#include "game/graphics/ctr/CtrLevel.h"
#include "game/graphics/ctr/CtrSettings.h"
#include "game/graphics/ctr/CtrSimd.h"
#include "game/graphics/ctr/CtrSkyJak2Packet.h"
#include "game/graphics/ctr/CtrVram.h"
#include "game/graphics/ctr/ctr_gpu.h"

namespace {
bool settings_asm_tex() {
  return ctr_settings().asm_tex;
}
}  // namespace

namespace {

// render-sky-3ds's packet (goal_src/jak1/engine/gfx/sky/sky-tng.gc)
struct SkyPacket {
  float camera[4][4];  // *math-camera* camera-temp (4 vectors)
  float hvdf[4];       // hvdf-off
  float hmge[4];       // hmge-scale
  u32 sky_drawn, cloud_drawn;
  u32 off0, off1;      // cloud texture scroll: s | t << 16, per layer
  u32 roof, clouds, base, pad;  // EE addresses of render-sky-tng's polygons (sky-vertex arrays)
};
static_assert(sizeof(SkyPacket) == CtrSky::kPacketBytes);

struct SkyVertex {
  float pos[4];
  float stq[4];
  float col[4];
};
static_assert(sizeof(SkyVertex) == 48);

// an adgif transfer (gif tag + 5 A+D): its TEX0
bool adgif_tex0(const DmaTransfer& t, u64* tex0) {
  if (t.size_bytes != 96) {
    return false;
  }
  for (int i = 0; i < 5; i++) {
    const u8* ad = t.data + 16 + 16 * i;
    if (ad[8] == (u8)GsRegisterAddress::TEX0_1) {
      memcpy(tex0, ad, 8);
      return true;
    }
  }
  return false;
}

}  // namespace

CtrSky::~CtrSky() {
  for (int t : m_tex) {
    if (t >= 0) {
      ctr_gpu_tex_delete(t);
    }
  }
}

void CtrSky::blend(DmaFollower& dma, CtrRenderState& rs, u64 salt) {
  const double t_start = ctr_gpu_time_ms();  // (AI-assisted, title-popin) statistics
  // (AI-assisted, sky-flicker) copies left from a frame without a sky draw since: blended now
  if (m_copies_frame != rs.frame_idx) {
    finish_blend();
    m_copies_frame = rs.frame_idx;
  }
  // SkyBlendCPU::do_sky_blends: each copy is an adgif (the source texture) and a sprite whose
  // color is the weight (0x80 = 1.0); the first one of a frame draws (no ABE), the others add.
  // The sprite's second corner tells sky (32 px) from clouds (64 px).
  // (AI-assisted, sky-flicker) Here only the copies are collected and their sources decoded (the
  // VRAM copy holds this level's pages now); finish_blend adds them up once the frame's copies of
  // both levels are in.
  while (dma.current_tag().qwc == 6 && dma.current_tag_offset() != rs.next_bucket) {
    auto setup = dma.read_and_advance();
    if (dma.current_tag_offset() == rs.next_bucket) {
      break;
    }
    auto draw = dma.read_and_advance();
    u64 tex0;
    if (draw.size_bytes != 96 || !adgif_tex0(setup, &tex0) || !rs.vram) {
      continue;
    }
    GifTag tag(draw.data);
    u32 coord, intensity;
    memcpy(&coord, draw.data + 5 * 16, 4);
    memcpy(&intensity, draw.data + 16, 4);
    const int idx = coord == 0x200 ? 0 : (coord == 0x400 ? 1 : -1);
    if (idx < 0) {
      continue;
    }
    constexpr u32 kRedecodeFrames = 300;
    // (AI-assisted) keyed by the uploads the texels and the CLUT came from too: another level's
    // page at the same VRAM address (same tex0) is a different texture. (AI-assisted) And by their
    // data's hashes: a level loaded into the same slot puts its sky page at the same EE address,
    // and its old level's decoded source was blended for up to kRedecodeFrames (a wrong sky)
    u64 key = tex0 ^ (salt << 1);
    {
      GsTex0 kt(tex0);
      u64 h0 = 0, h1 = 0;
      const u64 p0 = (u64)(uintptr_t)rs.vram->upload_source(kt.tbp0(), &h0);
      const u64 p1 = (u64)(uintptr_t)rs.vram->upload_source(kt.cbp(), &h1);
      key = (key ^ p0) * 1099511628211ull;
      key = (key ^ p1) * 1099511628211ull;
      key = (key ^ h0) * 1099511628211ull;
      key = (key ^ h1) * 1099511628211ull;
    }
    if (m_sources.size() > 64 && !m_sources.count(key)) {
      // old levels' sources (this frame's copies so far find theirs missing: no blend this frame)
      m_sources.clear();
    }
    Source& src = m_sources[key];
    if (src.frame == 0 || (u32)rs.frame_idx - src.frame >= kRedecodeFrames) {
      src.frame = (u32)rs.frame_idx | 1;
      int w = 0, h = 0;
      src.ok = rs.vram->decode_for_cpu(tex0, &src.rgba, &w, &h);
      src.w = w;
      src.h = h;
      GsTex0 t(tex0);
      if (std::find(m_logged.begin(), m_logged.end(), tex0) == m_logged.end() &&
          m_logged.size() < 64) {
        // once per source texture: what the blend reads (a wrong sky shows up here)
        m_logged.push_back(tex0);
        lg::info("[ctr] sky source {}: tex0 {:x} psm {} cpsm {} tbp {} cbp {} {}x{} {} first texel {:08x}",
                 idx ? "clouds" : "sky", tex0, (int)t.psm(), t.cpsm(), t.tbp0(), t.cbp(), w, h,
                 src.ok ? "ok" : "NOT DECODED", src.ok && !src.rgba.empty() ? src.rgba[0] : 0);
      }
      // make-sky-textures draws with TEXA ta0 = ta1 = 0x80 (set-display-gs-state): 16-bit texels
      // and 16-bit CLUT entries are opaque, whatever their A bit (CtrVram decodes with the usual
      // TEXA)
      const bool indexed = t.psm() == GsTex0::PSM::PSMT8 || t.psm() == GsTex0::PSM::PSMT4 ||
                           t.psm() == GsTex0::PSM::PSMT8H || t.psm() == GsTex0::PSM::PSMT4HH ||
                           t.psm() == GsTex0::PSM::PSMT4HL;
      if (src.ok && (t.psm() == GsTex0::PSM::PSMCT16 || t.psm() == GsTex0::PSM::PSMCT16S ||
                     (indexed && t.cpsm() != 0))) {
        for (auto& c : src.rgba) {
          c = (c & 0xffffffu) | (0x80u << 24);
        }
      }
    }
    m_copies.push_back({key, intensity, idx, !GsPrim(tag.prim()).abe()});
  }
  m_stats.blend_ms += ctr_gpu_time_ms() - t_start;
}

// (AI-assisted, sky-flicker) the collected frame's copies (both levels) into the two textures
void CtrSky::finish_blend() {
  if (m_copies_frame == ~0ull) {
    return;
  }
  const u64 frame = m_copies_frame;
  m_copies_frame = ~0ull;
  // (AI-assisted) blended (and uploaded, with the clouds' mip levels) every 4th frame only: the
  // time of day colors change over minutes, the clouds move by their texture offsets. It cost
  // ~3 ms of the render thread per frame on the Misty zoomer course (Azahar).
  // (AI-assisted, title-popin) Except while the weights move fast: the title runs the day ~20x
  // faster and the sky stepped visibly every 4th frame (Azahar). A texture is blended again in any
  // frame where its copies' weights (0x80 = 1.0) moved by kSkyFastDelta in all since its last blend.
  // (AI-assisted, sky-flicker) Decided over all the frame's copies of the texture, both levels'.
  constexpr u32 kSkyFastDelta = 1;
  for (int i = 0; i < 2; i++) {
    auto& last = m_last_weights[i];
    u32 delta = 0;
    size_t n = 0;
    for (const auto& c : m_copies) {
      if (c.idx == i) {
        delta += n < last.size() && last[n].first == c.key
                     ? (u32)std::abs((int)c.intensity - (int)last[n].second)
                     : 0x80u;
        n++;
      }
    }
    if (n == 0) {
      continue;  // not made this frame (no level with a sky): the last one stays
    }
    delta += n != last.size() ? 0x80u : 0u;
    if (m_valid[i] && (frame & 3) != 0 && delta < kSkyFastDelta) {
      continue;
    }
    // the whole blend from zero (the adds that follow a first copy would otherwise pile up over
    // the frames: a saturated, technicolour sky); a source that can't be read keeps the last one
    auto& out = m_work[i];
    const size_t bytes = (size_t)kSize[i] * kSize[i] * 4;
    out.assign(bytes, 0);
    bool ok = true;
    for (const auto& c : m_copies) {
      if (c.idx != i) {
        continue;
      }
      const auto it = m_sources.find(c.key);
      if (it == m_sources.end() || !it->second.ok || it->second.w != kSize[i] ||
          it->second.h != kSize[i]) {
        ok = false;
        break;
      }
      if (c.first) {
        std::fill(out.begin(), out.end(), 0);  // drawn, not added (no ABE)
      }
      // out[i] = min(255, out[i] + min(255, (in[i] * intensity) >> 7)), 4 bytes at a time
      if (settings_asm_tex()) {
        ctr_add_scaled_texels(out.data(), (const u8*)it->second.rgba.data(), bytes, c.intensity);
      } else {
        // (AI-assisted) asm_tex = off (hardware hang bisect): the Oct 2 byte loop
        const u8* in = (const u8*)it->second.rgba.data();
        for (size_t k = 0; k < bytes; k++) {
          out[k] = (u8)std::min<u32>(255, out[k] + std::min<u32>(255, (in[k] * c.intensity) >> 7));
        }
      }
      m_stats.blends++;
    }
    if (!ok) {
      continue;
    }
    m_stats.fast_blends += (frame & 3) != 0 && m_valid[i];
    last.clear();
    for (const auto& c : m_copies) {
      if (c.idx == i) {
        last.emplace_back(c.key, c.intensity);
      }
    }
    m_rgba[i].swap(out);
    m_dirty[i] = true;
    m_valid[i] = true;
  }
  m_copies.clear();
}

void CtrSky::draw(const u8* packet, CtrRenderState& rs) {
  if (!ctr_settings().sky) {
    return;
  }
  SkyPacket p;
  memcpy(&p, packet, sizeof(p));
  const double t_up = ctr_gpu_time_ms();  // (AI-assisted, title-popin) statistics
  m_stats.frames++;
  // (AI-assisted) last frame's blends (both levels' copies): kept if they completed, otherwise
  // the last good ones stay. (sky-flicker) A frame draws the sky before its own sky blend buckets.
  if (m_copies_frame != rs.frame_idx) {
    finish_blend();
  }
  // the textures blended last frame (like the PS2: they sit in VRAM until this frame's sky)
  for (int i = 0; i < 2; i++) {
    if (!m_dirty[i]) {
      continue;
    }
    const u8* texels = m_rgba[i].data();
    if (i == 1) {
      // (AI-assisted) the clouds' rgb multiplied by their alpha (0x80 = 1.0): both layers are
      // then added in one pass (ctr_gpu_draw_clip2), which has no texture alpha of its own
      m_premul.resize(m_rgba[1].size());
      if (settings_asm_tex()) {
        ctr_premultiply_texels(m_premul.data(), m_rgba[1].data(), m_rgba[1].size() / 4);
      } else {
        for (size_t k = 0; k + 3 < m_rgba[1].size(); k += 4) {  // (AI-assisted) asm_tex = off
          const u32 a = std::min<u32>(m_rgba[1][k + 3], 0x80);
          m_premul[k] = (u8)((m_rgba[1][k] * a) >> 7);
          m_premul[k + 1] = (u8)((m_rgba[1][k + 1] * a) >> 7);
          m_premul[k + 2] = (u8)((m_rgba[1][k + 2] * a) >> 7);
          m_premul[k + 3] = m_rgba[1][k + 3];
        }
      }
      texels = m_premul.data();
    }
    if (m_tex[i] < 0) {
      // (AI-assisted) the clouds with mip levels: they repeat ~9 times towards the horizon while
      // they scroll, and without mips that shimmered (flickering clouds); the sky is stretched
      m_tex[i] = i == 1 ? ctr_gpu_tex_create_mipmapped(kSize[i], kSize[i], texels)
                        : ctr_gpu_tex_create(kSize[i], kSize[i], texels);
    } else {
      ctr_gpu_tex_update(m_tex[i], texels);
    }
    m_dirty[i] = false;
  }
  m_stats.blend_ms += ctr_gpu_time_ms() - t_up;
  constexpr u32 kEeBytes = 128 * 1024 * 1024;
  auto verts_at = [&](u32 addr, int count) -> const SkyVertex* {
    if (!addr || addr + (u64)count * sizeof(SkyVertex) > kEeBytes) {
      return nullptr;
    }
    return (const SkyVertex*)(rs.ee_mem + addr);
  };
  // render-sky-tng's math (draw-large-polygon): camera-temp * (direction, 0), divided by
  // w * hmge.w, plus hvdf -> GS screen; as ctr clip space with w' = w * hmge.w
  const float hw = p.hmge[3] != 0.f ? p.hmge[3] : 1.f;
  auto vert = [&](const SkyVertex& v, float s_off, float t_off, float gs_z, bool textured) {
    float c[4];
    for (int j = 0; j < 4; j++) {
      c[j] = p.camera[0][j] * v.pos[0] + p.camera[1][j] * v.pos[1] + p.camera[2][j] * v.pos[2];
    }
    const float w = c[3] * hw;
    ctr_clip_vertex o;
    o.x = (c[0] + (p.hvdf[0] - 2048.f) * w) / 256.f;
    o.y = -(c[1] + (p.hvdf[1] - 2048.f) * w) / 112.f;
    // (AI-assisted) GS z 0 put the polygons exactly on the far clip plane (z = -w): rounding
    // clipped random parts of the sky and clouds away from frame to frame (flicker). The sky has
    // no depth test or write, so any depth inside the volume draws the same.
    gs_z = std::max(gs_z, 65536.f);
    o.z = (2.f * gs_z / 16777215.f - 1.f) * w;
    o.w = w;
    const float q = v.stq[2] != 0.f ? v.stq[2] : 1.f;
    o.s = textured ? (v.stq[0] + s_off) / q : 0.f;
    o.t = textured ? (v.stq[1] + t_off) / q : 0.f;
    o.r = (u8)std::clamp(v.col[0], 0.f, 255.f);
    o.g = (u8)std::clamp(v.col[1], 0.f, 255.f);
    o.b = (u8)std::clamp(v.col[2], 0.f, 255.f);
    o.a = (u8)std::clamp(v.col[3], 0.f, 255.f);
    return o;
  };
  ctr_draw_state st;
  memset(&st, 0, sizeof(st));
  // (AI-assisted) sky_last: after the opaque geometry, only where it left the cleared depth
  st.ztest = m_depth_tested ? CTR_TEST_GEQUAL : CTR_TEST_ALWAYS;
  st.zwrite = 0;
  st.atest = CTR_TEST_ALWAYS;
  std::vector<ctr_clip_vertex> tris;

  // the roof: 4 triangles with the sky texture, alpha blended over the clear color
  if (p.sky_drawn && m_valid[0] && m_tex[0] >= 0) {
    if (const SkyVertex* roof = verts_at(p.roof, 12)) {
      tris.clear();
      for (int i = 0; i < 12; i++) {
        tris.push_back(vert(roof[i], 0.f, 0.f, 0.f, true));
      }
      st.tex = m_tex[0];
      st.tcc = 1;
      st.filter = 1;
      st.clamp_s = st.clamp_t = 1;
      // (AI-assisted) alpha blended over the clear color: the screen holds nothing else there yet
      // (the sky is drawn first), so the GPU computes it without reading the framebuffer
      st.blend = CTR_BLEND_OVER_CLEAR;
      ctr_gpu_draw_clip(&st, tris.data(), (int)tris.size());
      m_stats.draws++;
    }
  }
  // (AI-assisted) the clouds whenever the sky is drawn, from the last complete cloud texture
  // (the game only blends them in frames it makes the sky textures)
  if (p.cloud_drawn || p.sky_drawn) {
    // two cloud layers of 9 quads, added (alpha b=2 d=1), the texture scrolling (set-tex-offset)
    const SkyVertex* clouds = verts_at(p.clouds, 72);
    // (AI-assisted) config clouds = off: the sky without its cloud pass (an added layer over the
    // sky's part of the screen)
    if (clouds && m_valid[1] && m_tex[1] >= 0 && ctr_settings().clouds) {
      // (AI-assisted) both layers in one pass, one texture on two texture units: the first
      // layer's quads with the second layer's texture coordinates too (the layers are the same
      // quads, the second one tilted by a few degrees; their colors are per layer, the alpha fade
      // towards the horizon the same). Each layer was an added pass over most of the sky.
      const float so0 = (float)(p.off0 & 0xffff) / 65536.f, to0 = (float)(p.off0 >> 16) / 65536.f;
      const float so1 = (float)(p.off1 & 0xffff) / 65536.f, to1 = (float)(p.off1 >> 16) / 65536.f;
      m_tris2.clear();
      for (int q = 0; q < 9; q++) {
        const SkyVertex* v0 = clouds + 4 * q;
        const SkyVertex* v1 = clouds + 36 + 4 * q;
        ctr_clip_vertex2 c[4];
        for (int k = 0; k < 4; k++) {
          const ctr_clip_vertex a = vert(v0[k], so0, to0, 0.f, true);
          const ctr_clip_vertex b = vert(v1[k], so1, to1, 0.f, true);
          c[k] = {a.x, a.y, a.z, a.w, a.s, a.t, b.s, b.t, 0x80, 0x80, 0x80, a.a};
        }
        // render-sky-quad: a fan 0 1 2 3
        for (int k : {0, 1, 2, 0, 2, 3}) {
          m_tris2.push_back(c[k]);
        }
      }
      auto layer_color = [](const SkyVertex& v) {
        u32 c = 0;
        for (int j = 0; j < 3; j++) {
          c |= (u32)std::clamp(v.col[j], 0.f, 255.f) << (8 * j);
        }
        return c;
      };
      st.tex = m_tex[1];
      st.tcc = 1;
      st.filter = 1;
      st.clamp_s = st.clamp_t = 0;
      st.blend = CTR_BLEND_ONE_ONE;
      ctr_gpu_draw_clip2(&st, layer_color(clouds[0]), layer_color(clouds[36]), m_tris2.data(),
                         (int)m_tris2.size());
      m_stats.draws++;
    }
    // below the horizon: 4 flat triangles in the erase color (giftag-base), in front of the sky.
    // (AI-assisted) Not drawn: on the PS2 the far ocean covers them; the 3DS ocean and level fade
    // into the fog color by the draw distance and nothing covers this dark blue there (a dark band
    // at the horizon). The frame is cleared to the fog color instead.
    if (const SkyVertex* base = false ? verts_at(p.base, 12) : nullptr) {
      tris.clear();
      for (int i = 0; i < 12; i++) {
        tris.push_back(vert(base[i], 0.f, 0.f, 256.f, false));
      }
      memset(&st, 0, sizeof(st));
      st.tex = -1;
      st.ztest = CTR_TEST_ALWAYS;
      st.atest = CTR_TEST_ALWAYS;
      st.blend = CTR_BLEND_ALPHA;  // (the sky's alpha state; the colors' alpha is 0x80)
      ctr_gpu_draw_clip(&st, tris.data(), (int)tris.size());
      m_stats.draws++;
    }
  }
  if (rs.log_now) {
    lg::info("[ctr] sky: {} texture blends ({} between the 4th frames: the weights moving fast), "
             "{} draws, blends and uploads {:.2f} ms per frame", m_stats.blends,
             m_stats.fast_blends, m_stats.draws, m_stats.blend_ms / std::max(1, m_stats.frames));
    m_stats = Stats();
  }
}

// (AI-assisted) skip_clear: the roof's 4 triangles (as draw() makes them) in screen space, all
// in front of the camera, contain a grid of points over the screen (a little beyond its edges)
bool CtrSky::roof_covers_screen(const u8* packet, const CtrRenderState& rs) const {
  if (!ctr_settings().sky) {
    return false;
  }
  SkyPacket p;
  memcpy(&p, packet, sizeof(p));
  if (!p.sky_drawn || !m_valid[0] || m_tex[0] < 0 || !p.roof ||
      p.roof + 12ull * sizeof(SkyVertex) > 128ull * 1024 * 1024) {
    return false;
  }
  const SkyVertex* roof = (const SkyVertex*)(rs.ee_mem + p.roof);
  const float hw = p.hmge[3] != 0.f ? p.hmge[3] : 1.f;
  float sx[12], sy[12];
  for (int i = 0; i < 12; i++) {
    float c[4];
    for (int j = 0; j < 4; j++) {
      c[j] = p.camera[0][j] * roof[i].pos[0] + p.camera[1][j] * roof[i].pos[1] +
             p.camera[2][j] * roof[i].pos[2];
    }
    const float w = c[3] * hw;
    if (!(w > 1e-3f)) {
      return false;  // behind the camera: clipped, its screen shape isn't simple
    }
    sx[i] = (c[0] + (p.hvdf[0] - 2048.f) * w) / 256.f / w;
    sy[i] = -(c[1] + (p.hvdf[1] - 2048.f) * w) / 112.f / w;
  }
  constexpr int kNx = 25, kNy = 15;
  constexpr float kMargin = 1.02f;
  for (int gy = 0; gy < kNy; gy++) {
    for (int gx = 0; gx < kNx; gx++) {
      const float x = kMargin * (2.f * gx / (kNx - 1) - 1.f);
      const float y = kMargin * (2.f * gy / (kNy - 1) - 1.f);
      bool in = false;
      for (int t = 0; t < 4 && !in; t++) {
        const float* ax = &sx[3 * t];
        const float* ay = &sy[3 * t];
        float d[3];
        for (int e = 0; e < 3; e++) {
          const int n = (e + 1) % 3;
          d[e] = (ax[n] - ax[e]) * (y - ay[e]) - (ay[n] - ay[e]) * (x - ax[e]);
        }
        in = (d[0] > 0 && d[1] > 0 && d[2] > 0) || (d[0] < 0 && d[1] < 0 && d[2] < 0);
      }
      if (!in) {
        return false;
      }
    }
  }
  return true;
}

namespace {

// Jak II's 416 line frame (CtrDirect's GS y to clip y)
constexpr float kJak2YScale = 1.f / 208.f;
// GS z 65536: inside the clip volume (CtrSky::draw)
constexpr float kDomeZ = 2.f * 65536.f / 16777215.f - 1.f;
constexpr int kHazeVerts = 72, kHazeIndices = 36 * 6;
constexpr int kCloudVerts = 200, kCloudIndices = 162 * 6;

u8 color_byte(float f) {
  return (u8)std::clamp(f, 0.f, 255.f);
}

// draw-large-polygon's projection of the head as the clip program's matrix (row major, clip =
// M (x y z 1)); the dome's vertices have w = 0 (sky-data.gc), so rows[3] has no part
void dome_matrix(const ctr_sky_jak2::Head& h, float m[16]) {
  const float hx = h.hvdf[0] - 2048.f, hy = h.hvdf[1] - 2048.f;
  for (int c = 0; c < 3; c++) {
    const float w = h.rows[c][3] * h.scale[2];
    m[c] = (h.rows[c][0] * h.scale[0] + hx * w) * (1.f / 256.f);
    m[4 + c] = -(h.rows[c][1] * h.scale[1] + hy * w) * kJak2YScale;
    m[8 + c] = kDomeZ * w;
    m[12 + c] = w;
  }
  m[3] = m[7] = m[11] = m[15] = 0.f;
}

// render-sky-quad's fan 0 1 2 3 of each quad. Haze: quad i is the ring's vertex i (2 i), the one
// above it (2 i + 1), above the next and the next. Clouds: per layer the 9 x 9 quads of its
// 10 x 10 vertices, corners i, i + 1, i + 11, i + 10.
struct DomeIndices {
  u16 haze[kHazeIndices];
  u16 clouds[kCloudIndices];
  DomeIndices() {
    u16* o = haze;
    for (int i = 0; i < 36; i++) {
      const int n = (i + 1) % 36;
      const u16 q[4] = {(u16)(2 * i), (u16)(2 * i + 1), (u16)(2 * n + 1), (u16)(2 * n)};
      for (int k : {0, 1, 2, 0, 2, 3}) {
        *o++ = q[k];
      }
    }
    o = clouds;
    for (int layer = 0; layer < 2; layer++) {
      for (int r = 0; r < 9; r++) {
        for (int c = 0; c < 9; c++) {
          const int i = 100 * layer + 10 * r + c;
          const u16 q[4] = {(u16)i, (u16)(i + 1), (u16)(i + 11), (u16)(i + 10)};
          for (int k : {0, 1, 2, 0, 2, 3}) {
            *o++ = q[k];
          }
        }
      }
    }
  }
};

const DomeIndices& dome_indices() {
  static const DomeIndices indices;
  return indices;
}

}  // namespace

bool CtrSkyJak2::packet_prim(u16 kind, const u8* data, u32 size, u64* prim) {
  size_t want = 0;
  if (kind == ctr_sky_jak2::kStars) {
    want = sizeof(ctr_sky_jak2::Stars);
  } else if (kind == ctr_sky_jak2::kHaze) {
    want = sizeof(ctr_sky_jak2::Haze);
  } else if (kind == ctr_sky_jak2::kClouds) {
    want = sizeof(ctr_sky_jak2::Clouds);
  }
  if (!want || size != want) {
    return false;
  }
  *prim = GifTag(data + offsetof(ctr_sky_jak2::Head, giftag)).prim();
  return true;
}

void CtrSkyJak2::begin(bool log_now) {
  m_items.clear();
  m_stats.frames++;
  if (log_now) {
    lg::info("[ctr] sky packets: {} frames, {} star quads, {} haze and {} cloud draws, {} without "
             "vertex room",
             m_stats.frames, m_stats.stars, m_stats.haze, m_stats.clouds, m_stats.no_room);
    m_stats = Stats();
  }
}

int CtrSkyJak2::add(u16 kind, const u8* data, const ctr_draw_state& state) {
  m_items.push_back({kind, data, state});
  return (int)m_items.size() - 1;
}

void CtrSkyJak2::draw(int item, bool depth_tested) {
  if (item < 0 || item >= (int)m_items.size()) {
    return;
  }
  const Item& it = m_items[item];
  ctr_draw_state st = it.st;
  if (depth_tested) {
    st.ztest = CTR_TEST_GEQUAL;
    st.zwrite = 0;
  }
  switch (it.kind) {
    case ctr_sky_jak2::kStars:
      draw_stars(it.data, st);
      break;
    case ctr_sky_jak2::kHaze:
      draw_haze(it.data, st);
      break;
    case ctr_sky_jak2::kClouds:
      draw_clouds(it.data, st);
      break;
    default:
      break;
  }
}

// stars-transform-asm's generator: 128 times the byte sum of three of the seed's 8 quadwords over
// a fourth; its 16 bytes are 4 vectors (byte << 24 as a float / 32768)
void CtrSkyJak2::make_dirs(const u32 seed[8][4]) {
  memcpy(m_seed, seed, sizeof(m_seed));
  u8 q[8][16];
  memcpy(q, seed, sizeof(q));
  u32 a1 = 0;
  for (int it = 0; it < 128; it++) {
    u8 s[16];
    for (int b = 0; b < 16; b++) {
      s[b] = (u8)(q[a1][b] + q[(a1 + 3) & 7][b] + q[(a1 + 5) & 7][b]);
    }
    memcpy(q[(a1 + 7) & 7], s, 16);
    a1 = (a1 + 1) & 7;
    static constexpr int kBase[4] = {4, 0, 12, 8};
    for (int g = 0; g < 4; g++) {
      float x[3];
      for (int j = 0; j < 3; j++) {
        x[j] = (float)(s32)((u32)s[kBase[g] + j] << 24) * (1.f / 32768.f);
      }
      const float len = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
      float* d = m_dirs[4 * it + g];
      for (int j = 0; j < 3; j++) {
        d[j] = len > 0.f ? x[j] / len : 0.f;
      }
    }
  }
  m_dirs_valid = true;
}

void CtrSkyJak2::draw_stars(const u8* data, const ctr_draw_state& st) {
  ctr_sky_jak2::Stars p;
  memcpy(&p, data, sizeof(p));
  if (!m_dirs_valid || memcmp(m_seed, p.seed, sizeof(m_seed)) != 0) {
    make_dirs(p.seed);
  }
  const ctr_sky_jak2::Head& h = p.head;
  // stars-transform-asm and stars-dma in one step: the camera rows applied to star-mat's rows
  // (lanes x, y, w), times vector-normalize!'s 50000
  float a[3][3], up[3];
  for (int j = 0; j < 3; j++) {
    const float* r = p.mat[j];
    static constexpr int kLane[3] = {0, 1, 3};
    for (int l = 0; l < 3; l++) {
      const int k = kLane[l];
      a[j][l] = 50000.f * (h.rows[0][k] * r[0] + h.rows[1][k] * r[1] + h.rows[2][k] * r[2]);
    }
    up[j] = r[1];
  }
  u8 col[16][4];
  for (int i = 0; i < 16; i++) {
    for (int c = 0; c < 4; c++) {
      col[i][c] = (u8)(p.colors[i][c] & 0xff);
    }
  }
  const float q0 = p.st[0][2] != 0.f ? 1.f / p.st[0][2] : 1.f;
  const float q1 = p.st[1][2] != 0.f ? 1.f / p.st[1][2] : 1.f;
  const float s0 = p.st[0][0] * q0, t0 = p.st[0][1] * q0;
  const float s1 = p.st[1][0] * q1, t1 = p.st[1][1] * q1;
  m_quads.clear();
  for (int k = 0; k < 512; k++) {
    const float* d = m_dirs[k];
    float px = a[0][0] * d[0] + a[1][0] * d[1] + a[2][0] * d[2];
    float py = a[0][1] * d[0] + a[1][1] * d[1] + a[2][1] * d[2];
    float pw = a[0][2] * d[0] + a[1][2] * d[1] + a[2][2] * d[2];
    // mirrored to the upper half of the sky
    if (std::signbit(up[0] * d[0] + up[1] * d[1] + up[2] * d[2])) {
      px = -px;
      py = -py;
      pw = -pw;
    }
    const float w = (pw + h.rows[3][3]) * h.scale[2];
    if (!(w > 0.f)) {
      continue;  // behind the camera: its mirror image would be under the sky's base
    }
    const float inv = 1.f / w;
    const float sx = (px + h.rows[3][0]) * h.scale[0] * inv + h.hvdf[0];
    const float sy = (py + h.rows[3][1]) * h.scale[1] * inv + h.hvdf[1];
    const float x0 = sx + p.corner[0][0], y0 = sy + p.corner[0][1];
    const float x1 = sx + p.corner[1][0], y1 = sy + p.corner[1][1];
    if (!(x0 > 0.f && y0 > 0.f && x1 < 4096.f && y1 < 4096.f)) {
      continue;
    }
    // stars-dma's window test on the GS's 12.4 corners
    const s32 ix0 = (s32)(x0 * 16.f), iy0 = (s32)(y0 * 16.f);
    const s32 ix1 = (s32)(x1 * 16.f), iy1 = (s32)(y1 * 16.f);
    if (!(ix0 < 36864 && iy0 < 36096 && 28672 < ix1 && 29440 < iy1)) {
      continue;
    }
    // the sprite as CtrDirect makes it: corners 1, (2, 1), 2, (1, 2)
    const float cx0 = ((float)ix0 * (1.f / 16.f) - 2048.f) * (1.f / 256.f);
    const float cx1 = ((float)ix1 * (1.f / 16.f) - 2048.f) * (1.f / 256.f);
    const float cy0 = (2048.f - (float)iy0 * (1.f / 16.f)) * kJak2YScale;
    const float cy1 = (2048.f - (float)iy1 * (1.f / 16.f)) * kJak2YScale;
    const u8* c = col[k & 15];
    m_quads.push_back({cx0, cy0, 0.f, s0, t0, c[0], c[1], c[2], c[3]});
    m_quads.push_back({cx1, cy0, 0.f, s1, t0, c[0], c[1], c[2], c[3]});
    m_quads.push_back({cx1, cy1, 0.f, s1, t1, c[0], c[1], c[2], c[3]});
    m_quads.push_back({cx0, cy1, 0.f, s0, t1, c[0], c[1], c[2], c[3]});
  }
  if (!m_quads.empty()) {
    ctr_gpu_draw_quads(&st, m_quads.data(), (int)m_quads.size() / 4);
    m_stats.stars += (int)m_quads.size() / 4;
  }
}

void CtrSkyJak2::draw_haze(const u8* data, const ctr_draw_state& st) {
  ctr_sky_jak2::Head h;
  memcpy(&h, data, sizeof(h));
  ctr_clip_vertex* v;
  u16* idx;
  if (!ctr_gpu_clip_reserve(kHazeVerts, kHazeIndices, &v, &idx)) {
    m_stats.no_room++;
    return;
  }
  // haze-vtx-to-sky: the ring's vertex with alpha 128 and the one above it (y = 3) with alpha 0
  for (int i = 0; i < 36; i++) {
    ctr_sky_jak2::HazeVertex hv;
    memcpy(&hv, data + sizeof(h) + i * sizeof(hv), sizeof(hv));
    const u8 r = color_byte(hv.col[0]), g = color_byte(hv.col[1]), b = color_byte(hv.col[2]);
    v[2 * i] = {hv.pos[0], hv.pos[1], hv.pos[2], 1.f, 0.f, 0.f, r, g, b, 128};
    v[2 * i + 1] = {hv.pos[0], 3.f, hv.pos[2], 1.f, 0.f, 0.f, r, g, b, 0};
  }
  memcpy(idx, dome_indices().haze, sizeof(dome_indices().haze));
  float m[16];
  dome_matrix(h, m);
  const int first = 0, count = kHazeIndices;
  ctr_gpu_clip_draw_ranges_mtx(m, 1, &st, &first, &count, 1, kHazeVerts, kHazeIndices);
  m_stats.haze++;
}

void CtrSkyJak2::draw_clouds(const u8* data, const ctr_draw_state& st) {
  ctr_sky_jak2::Head h;
  memcpy(&h, data, sizeof(h));
  ctr_clip_vertex* v;
  u16* idx;
  if (!ctr_gpu_clip_reserve(kCloudVerts, kCloudIndices, &v, &idx)) {
    m_stats.no_room++;
    return;
  }
  // cloud-vtx1-to-sky and cloud-vtx2-to-sky: each vertex once per layer; the texture coordinates
  // as draw-large-polygon and the GS make them ((st + offset) Q / (q Q))
  for (int i = 0; i < 100; i++) {
    ctr_sky_jak2::CloudVertex cv;
    memcpy(&cv, data + sizeof(h) + i * sizeof(cv), sizeof(cv));
    const float* stq[2] = {cv.stq, cv.stq2};
    const float* col[2] = {cv.col, cv.col2};
    for (int layer = 0; layer < 2; layer++) {
      const float q = stq[layer][2] + h.tex_off[2];
      const float inv = q != 0.f ? 1.f / q : 1.f;
      v[100 * layer + i] = {cv.pos[0],
                            cv.pos[1],
                            cv.pos[2],
                            1.f,
                            (stq[layer][0] + h.tex_off[0]) * inv,
                            (stq[layer][1] + h.tex_off[1]) * inv,
                            color_byte(col[layer][0]),
                            color_byte(col[layer][1]),
                            color_byte(col[layer][2]),
                            color_byte(col[layer][3])};
    }
  }
  memcpy(idx, dome_indices().clouds, sizeof(dome_indices().clouds));
  float m[16];
  dome_matrix(h, m);
  const int first = 0, count = kCloudIndices;
  ctr_gpu_clip_draw_ranges_mtx(m, 1, &st, &first, &count, 1, kCloudVerts, kCloudIndices);
  m_stats.clouds++;
}

CtrSkyBlendRenderer::CtrSkyBlendRenderer(std::string name,
                                         int id,
                                         CtrSky* sky,
                                         std::unique_ptr<CtrTfragRenderer> tfrag)
    : CtrBucketRenderer(std::move(name), id), m_sky(sky), m_tfrag(std::move(tfrag)) {}

CtrSkyBlendRenderer::~CtrSkyBlendRenderer() = default;

void CtrSkyBlendRenderer::render(DmaFollower& dma, CtrRenderState& rs) {
  // SkyBlendHandler::render: jump, then the sky copies between the GS setup (8 qw) and the
  // restore (alpha 2 qw, GS 8 qw), then the tfrag trans part (drawn from the .c3l files)
  if (dma.current_tag_offset() != rs.next_bucket) {
    dma.read_and_advance();
  }
  if (dma.current_tag_offset() != rs.next_bucket && dma.current_tag().kind != DmaTag::Kind::CALL &&
      dma.current_tag().qwc == 8) {
    dma.read_and_advance();  // set-display-gs-state
    if (ctr_settings().sky) {
      m_sky->blend(dma, rs, (u64)(uintptr_t)this);
    } else {
      // (AI-assisted) sky = off: nothing is drawn from the blend (CtrSky::draw), so no sources
      // decoded or blended either (~3 ms on the Misty zoomer course); the copies (2 transfers
      // each, as blend reads them) are skipped
      while (dma.current_tag().qwc == 6 && dma.current_tag_offset() != rs.next_bucket) {
        dma.read_and_advance();
        if (dma.current_tag_offset() == rs.next_bucket) {
          break;
        }
        dma.read_and_advance();
      }
    }
  }
  // (AI-assisted) the trans tfrag's camera: draws the level background if no normal tfrag bucket
  // did this frame (CtrTfragRenderer draws a level once per frame)
  if (m_tfrag) {
    m_tfrag->render(dma, rs);
  }
  while (dma.current_tag_offset() != rs.next_bucket) {
    dma.read_and_advance();
  }
}
