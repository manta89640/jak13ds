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

#include "game/graphics/ctr/CtrSettings.h"
#include "game/graphics/ctr/CtrVram.h"
#include "game/graphics/ctr/ctr_gpu.h"

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

void CtrSky::blend(DmaFollower& dma, CtrRenderState& rs) {
  // SkyBlendCPU::do_sky_blends: each copy is an adgif (the source texture) and a sprite whose
  // color is the weight (0x80 = 1.0); the first one of a frame draws (no ABE), the others add.
  // The sprite's second corner tells sky (32 px) from clouds (64 px).
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
    const bool first = !GsPrim(tag.prim()).abe();
    u32 coord, intensity;
    memcpy(&coord, draw.data + 5 * 16, 4);
    memcpy(&intensity, draw.data + 16, 4);
    const int idx = coord == 0x200 ? 0 : (coord == 0x400 ? 1 : -1);
    if (idx < 0) {
      continue;
    }
    int w = 0, h = 0;
    if (!rs.vram->decode_for_cpu(tex0, &m_decode, &w, &h) || w != kSize[idx] ||
        h != kSize[idx]) {
      continue;
    }
    auto& out = m_rgba[idx];
    const size_t n = (size_t)w * h * 4;
    out.resize(n);
    const u8* in = (const u8*)m_decode.data();
    if (first) {
      for (size_t i = 0; i < n; i++) {
        out[i] = (u8)std::min<u32>(255, (in[i] * intensity) >> 7);
      }
    } else {
      for (size_t i = 0; i < n; i++) {
        out[i] = (u8)std::min<u32>(255, out[i] + std::min<u32>(255, (in[i] * intensity) >> 7));
      }
    }
    m_dirty[idx] = true;
    m_valid[idx] = true;
    m_stats.blends++;
  }
}

void CtrSky::draw(const u8* packet, CtrRenderState& rs) {
  SkyPacket p;
  memcpy(&p, packet, sizeof(p));
  // the textures blended last frame (like the PS2: they sit in VRAM until this frame's sky)
  for (int i = 0; i < 2; i++) {
    if (!m_dirty[i]) {
      continue;
    }
    if (m_tex[i] < 0) {
      m_tex[i] = ctr_gpu_tex_create(kSize[i], kSize[i], m_rgba[i].data());
    } else {
      ctr_gpu_tex_update(m_tex[i], m_rgba[i].data());
    }
    m_dirty[i] = false;
  }
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
  st.ztest = CTR_TEST_ALWAYS;
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
      st.blend = CTR_BLEND_ALPHA;
      ctr_gpu_draw_clip(&st, tris.data(), (int)tris.size());
      m_stats.draws++;
    }
  }
  if (p.cloud_drawn) {
    // two cloud layers of 9 quads, added (alpha b=2 d=1), the texture scrolling (set-tex-offset)
    const SkyVertex* clouds = verts_at(p.clouds, 72);
    if (clouds && m_valid[1] && m_tex[1] >= 0) {
      tris.clear();
      for (int layer = 0; layer < 2; layer++) {
        const u32 off = layer ? p.off1 : p.off0;
        const float so = (float)(off & 0xffff) / 65536.f, to = (float)(off >> 16) / 65536.f;
        for (int q = 0; q < 9; q++) {
          const SkyVertex* v = clouds + 36 * layer + 4 * q;
          ctr_clip_vertex c[4];
          for (int k = 0; k < 4; k++) {
            c[k] = vert(v[k], so, to, 0.f, true);
          }
          // render-sky-quad: a fan 0 1 2 3
          for (int k : {0, 1, 2, 0, 2, 3}) {
            tris.push_back(c[k]);
          }
        }
      }
      st.tex = m_tex[1];
      st.tcc = 1;
      st.filter = 1;
      st.clamp_s = st.clamp_t = 0;
      st.blend = CTR_BLEND_ADD;
      ctr_gpu_draw_clip(&st, tris.data(), (int)tris.size());
      m_stats.draws++;
    }
    // below the horizon: 4 flat triangles in the erase color (giftag-base), in front of the sky
    if (const SkyVertex* base = verts_at(p.base, 12)) {
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
    lg::debug("[ctr] sky: {} texture blends, {} draws", m_stats.blends, m_stats.draws);
    m_stats = Stats();
  }
}

void CtrSkyBlendRenderer::render(DmaFollower& dma, CtrRenderState& rs) {
  // SkyBlendHandler::render: jump, then the sky copies between the GS setup (8 qw) and the
  // restore (alpha 2 qw, GS 8 qw), then the tfrag trans part (drawn from the .c3l files)
  if (dma.current_tag_offset() != rs.next_bucket) {
    dma.read_and_advance();
  }
  if (dma.current_tag_offset() != rs.next_bucket && dma.current_tag().kind != DmaTag::Kind::CALL &&
      dma.current_tag().qwc == 8) {
    dma.read_and_advance();  // set-display-gs-state
    m_sky->blend(dma, rs);
  }
  while (dma.current_tag_offset() != rs.next_bucket) {
    dma.read_and_advance();
  }
}
