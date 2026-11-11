/*!
 * @file CtrOcean.cpp
 * See CtrOcean.h.
 */

#include "CtrOcean.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "common/dma/dma.h"
#include "common/dma/gs.h"

#include "game/graphics/ctr/CtrDirect.h"
#include "game/graphics/ctr/CtrVram.h"
#include "game/graphics/ctr/ctr_gpu.h"

namespace {

// Jak II's sky reflection: the env map's first pass (the view from above) and the frame it was
// drawn in
constexpr int kEnvSize = 64;
int g_env_first_rt = -1;
u64 g_env_first_frame = ~0ull;

// does this GIF packet (a dma-buffer-add-gs-set: PACKED, A+D) write the GS register reg? Its
// value to *value
bool gif_sets(const u8* data, u32 size, u8 reg, u64* value = nullptr) {
  if (size < 16) {
    return false;
  }
  GifTag tag(data);
  if (tag.flg() != GifTag::Format::PACKED) {
    return false;
  }
  const u32 nreg = tag.nreg();
  u32 offset = 16;
  for (u32 loop = 0; loop < tag.nloop(); loop++) {
    for (u32 r = 0; r < nreg; r++) {
      if (offset + 16 > size) {
        return false;
      }
      if (tag.reg(r) == GifTag::RegisterDescriptor::AD && data[offset + 8] == reg) {
        if (value) {
          memcpy(value, data + offset, 8);
        }
        return true;
      }
      offset += 16;
    }
  }
  return false;
}

}  // namespace

void CtrOceanEnvJak2::draw_first_pass(CtrDirect* direct, DmaFollower& dma, CtrRenderState& rs) {
  int frames = 0;
  bool active = false;
  while (dma.current_tag_offset() != rs.next_bucket) {
    DmaFollower peek = dma;
    auto next = peek.read_and_advance();
    u64 scissor = 0;
    const bool has_scissor =
        next.size_bytes &&
        gif_sets(next.data, next.size_bytes, (u8)GsRegisterAddress::SCISSOR_1, &scissor);
    if (has_scissor && GsScissor(scissor).x1() == 127) {
      break;  // the wave texture's (OceanEnvmap::handle_ocean_envmap_jak2)
    }
    auto data = dma.read_and_advance();
    if (!data.size_bytes || (has_scissor && GsScissor(scissor).x1() != kEnvSize - 1)) {
      continue;  // (a reset of the display state: not drawn)
    }
    if (gif_sets(data.data, data.size_bytes, (u8)GsRegisterAddress::FRAME_1)) {
      frames++;
      if (frames == 1) {
        if (g_env_first_rt < 0) {
          g_env_first_rt = ctr_gpu_rt_create(kEnvSize, kEnvSize, kEnvSize);
        }
        if (g_env_first_rt >= 0 && ctr_gpu_rt_begin(g_env_first_rt, 0, 0, kEnvSize, kEnvSize)) {
          direct->reset_state();
          active = true;
        }
      } else if (active) {
        direct->flush();
        ctr_gpu_rt_end();
        active = false;
        g_env_first_frame = rs.frame_idx;
      }
    }
    if (active) {
      direct->render_vif(data.vif0(), data.vif1(), data.data, data.size_bytes);
    }
  }
  if (active) {
    direct->flush();
    ctr_gpu_rt_end();
    g_env_first_frame = rs.frame_idx;
  }
}

CtrOceanEnvWarpJak2::~CtrOceanEnvWarpJak2() {
  if (m_rt >= 0) {
    ctr_gpu_rt_delete(m_rt);
  }
}

bool CtrOceanEnvWarpJak2::first_pass_fresh(const CtrRenderState& rs) {
  return g_env_first_frame != ~0ull && rs.frame_idx >= g_env_first_frame &&
         rs.frame_idx - g_env_first_frame <= 8;
}

/*!
 * Jak II: this frame's env map for the sky reflection (64 x 64, the PS2's ocean-envmap texture):
 * ocean-method-83's warp of the first pass's view from above (64 columns around the heading; rows
 * 0..31 from the zenith to the horizon, 32..63 the same mirrored, the frame's last sprite) times
 * env-color (GS modulate, as the PS2's env pass draws it), or without the sky the
 * environment-ocean texture times env-color. Made again when either changes.
 */
bool CtrOceanEnvWarpJak2::update(const float ec[3], bool sky, u64 static_tex0,
                                 const CtrRenderState& rs) {
  const bool fresh = first_pass_fresh(rs);
  int src = -1;
  float src_uv[6] = {0, 0, 1, 0, 0, 1};  // the source's s, t at (0, 0), per unit of u, of v
  if (sky) {
    if (!fresh || g_env_first_rt < 0) {
      return false;
    }
    src = g_env_first_rt;
    float s0, t0, s1, t1, s2, t2;
    ctr_gpu_rt_uv(src, 0, 0, &s0, &t0);
    ctr_gpu_rt_uv(src, kEnvSize, 0, &s1, &t1);
    ctr_gpu_rt_uv(src, 0, kEnvSize, &s2, &t2);
    const float uv[6] = {s0, t0, s1 - s0, t1 - t0, s2 - s0, t2 - t0};
    memcpy(src_uv, uv, sizeof(uv));
  } else {
    const CtrTexture* t = static_tex0 && rs.vram ? rs.vram->get_texture(static_tex0) : nullptr;
    if (!t || t->handle < 0) {
      return false;
    }
    src = t->handle;
  }
  if (m_rt < 0) {
    m_rt = ctr_gpu_rt_create(kEnvSize, kEnvSize, kEnvSize);
    if (m_rt < 0) {
      return false;
    }
    float s0, t0, s1, t1, s2, t2;
    ctr_gpu_rt_uv(m_rt, 0, 0, &s0, &t0);
    ctr_gpu_rt_uv(m_rt, kEnvSize, 0, &s1, &t1);
    ctr_gpu_rt_uv(m_rt, 0, kEnvSize, &s2, &t2);
    const float uv[6] = {s0, t0, s1 - s0, t1 - t0, s2 - s0, t2 - t0};
    memcpy(m_uv, uv, sizeof(uv));
    m_made_from = ~0ull;
    m_static = 0;
  }
  const bool color_same = ec[0] == m_color[0] && ec[1] == m_color[1] && ec[2] == m_color[2];
  if (color_same && (sky ? m_made_from == g_env_first_frame : m_static == static_tex0)) {
    return true;  // made already
  }
  if (!ctr_gpu_rt_begin(m_rt, 0, 0, kEnvSize, kEnvSize)) {
    return false;
  }
  // env-color halved: the PC's env pass (CommonOceanRenderer bucket 4: the vertex color as it is,
  // 0x80 = 0.5), the reference the 3DS is compared with (the PS2's GS modulate: 0x80 = 1)
  auto col = [](float c) { return (u8)std::clamp(0.5f * c, 0.f, 255.f); };
  const u8 cr = col(ec[0]), cg = col(ec[1]), cb = col(ec[2]);
  auto vert = [&](float lx, float ly, float u, float v) {
    ctr_vertex o;
    o.x = lx * (2.f / kEnvSize) - 1.f;
    o.y = ly * (2.f / kEnvSize) - 1.f;
    o.z = 0.5f;
    o.s = src_uv[0] + src_uv[2] * u + src_uv[4] * v;
    o.t = src_uv[1] + src_uv[3] * u + src_uv[5] * v;
    o.r = cr;
    o.g = cg;
    o.b = cb;
    o.a = 0x80;
    return o;
  };
  ctr_draw_state st;
  memset(&st, 0, sizeof(st));
  st.tex = src;
  st.tcc = 0;
  st.filter = 1;
  st.clamp_s = st.clamp_t = sky ? 1 : 0;
  st.blend = CTR_BLEND_OFF;
  st.atest = CTR_TEST_ALWAYS;
  st.ztest = CTR_TEST_ALWAYS;
  st.zwrite = 0;
  std::vector<ctr_vertex> v;
  if (sky) {
    // ocean-method-83: column i from the center (row 0) to the edge (row 32) at the angle
    // 180 - i * 360 / 64 degrees: s, t = 0.5 + 0.5 * r * (sin, cos); rows 32..64 mirrored
    v.reserve(kEnvSize * 12);
    for (int i = 0; i < kEnvSize; i++) {
      const float a = 3.14159265f - (float)i * (6.2831853f / kEnvSize);
      const float es = 0.5f + 0.5f * std::sin(a), et = 0.5f + 0.5f * std::cos(a);
      const ctr_vertex c0 = vert((float)i, 0, 0.5f, 0.5f), c1 = vert((float)i + 1, 0, 0.5f, 0.5f);
      const ctr_vertex e0 = vert((float)i, 32, es, et), e1 = vert((float)i + 1, 32, es, et);
      const ctr_vertex d0 = vert((float)i, 64, 0.5f, 0.5f), d1 = vert((float)i + 1, 64, 0.5f, 0.5f);
      v.insert(v.end(), {c0, c1, e1, c0, e1, e0, e0, e1, d1, e0, d1, d0});
    }
  } else {
    const ctr_vertex a = vert(0, 0, 0, 0), b = vert(kEnvSize, 0, 1, 0),
                     c = vert(kEnvSize, kEnvSize, 1, 1), d = vert(0, kEnvSize, 0, 1);
    v.insert(v.end(), {a, b, c, a, c, d});
  }
  ctr_gpu_draw(&st, v.data(), (int)v.size());
  ctr_gpu_rt_end();
  memcpy(m_color, ec, sizeof(m_color));
  m_made_from = sky ? g_env_first_frame : ~0ull;
  m_static = sky ? 0 : static_tex0;
  return true;
}
