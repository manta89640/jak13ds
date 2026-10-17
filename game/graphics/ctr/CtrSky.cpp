/*!
 * @file CtrSky.cpp
 * (AI-assisted)
 * See CtrSky.h.
 */

#include "CtrSky.h"

#include <algorithm>
#include <cstring>

#include "common/dma/dma.h"
#include "common/dma/gs.h"
#include "common/log/log.h"

#include "game/graphics/ctr/CtrLevel.h"
#include "game/graphics/ctr/CtrSettings.h"
#include "game/graphics/ctr/CtrSimd.h"
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
    // page at the same VRAM address (same tex0) is a different texture
    u64 key = tex0 ^ (salt << 1);
    {
      GsTex0 kt(tex0);
      const u64 p0 = (u64)(uintptr_t)rs.vram->upload_source(kt.tbp0());
      const u64 p1 = (u64)(uintptr_t)rs.vram->upload_source(kt.cbp());
      key = (key ^ p0) * 1099511628211ull;
      key = (key ^ p1) * 1099511628211ull;
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
    m_sky->blend(dma, rs, (u64)(uintptr_t)this);
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
