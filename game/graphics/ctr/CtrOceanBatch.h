#pragma once

/*!
 * @file CtrOceanBatch.h
 * (AI-assisted)
 * The full ocean's (CtrOceanFull.h) vertices on their way to the GPU: the GS packets of the far
 * ocean and the emulated VU programs (GS screen positions), and the native programs' vertices
 * (clip space, CtrOceanNative.cpp).
 */

#include <algorithm>
#include <cstring>
#include <cmath>
#include <vector>

#include "common/common_types.h"
#include "common/dma/gs.h"

#include "game/graphics/ctr/ctr_gpu.h"

//! GS fog value F -> fog amount (255 - F) / 255 (CommonOceanRenderer: fog = 255 - F)
struct CtrOceanFogTable {
  float amount[256];
  CtrOceanFogTable() {
    for (int i = 0; i < 256; i++) {
      amount[i] = (255 - i) / 255.f;
    }
  }
};
inline const CtrOceanFogTable kOceanFog;

/*!
 * The GS packets the ocean's VU programs (and the far ocean) send, as one indexed triangle list
 * written into the frame's vertex memory (CommonOceanRenderer, without its OpenGL buffers). Only
 * the textured pass is kept: the near ocean's alpha and env map passes and the mid ocean's env map
 * pass are skipped while reading (their vertices aren't converted).
 * (AI-assisted, j1ocean) The native programs' vertices carry the env map pass too (es, et): their
 * draws with it (reserve_clip with env) add the sky reflection in the same draw
 * (ctr_gpu_ocean_full_draw_env, with the texture of set_env).
 */
class CtrOceanBatch {
 public:
  static constexpr int kChunkVerts = 4096;  // vertices per reservation (one draw)

  void begin(const ctr_draw_state& st, const u8 fog[3]) {
    m_state = st;
    memcpy(m_fog, fog, 3);
  }
  //! (AI-assisted, j1ocean) the sky reflection's texture for the env draws (-1: drawn without)
  void set_env(int tex) { m_env_tex = tex; }
  //! the env texture's layout (Jak II's render target): its s = m[0] + m[1] u + m[2] v, t = m[3] +
  //! m[4] u + m[5] v at the PS2's env map coordinates u, v; nullptr: s, t = u, v
  void set_env_map(const float* m) {
    m_env_mapped = m != nullptr;
    if (m) {
      memcpy(m_env_map, m, sizeof(m_env_map));
    }
  }
  const float* env_map() const { return m_env_mapped ? m_env_map : nullptr; }
  void end() { flush(); }

  /*!
   * Room for n vertices and m indices in clip space (ctr_gpu_ocean_full_draw_clip, the native
   * programs): written at *v and *idx (vertex numbers from *first on), then commit(n, m). false:
   * no vertex memory left this frame for this piece (it alone is not drawn).
   */
  bool reserve_clip(int n, int m, ctr_ocean_full_vertex** v, u16** idx, int* first,
                    bool env = false) {
    if (!mode(env && m_env_tex >= 0 ? kModeEnv : kModeClip) || !room(n + 2 * m, 4 * m)) {
      return false;
    }
    *v = m_v + m_nv;
    *idx = m_i + m_ni;
    *first = m_nv;
    return true;
  }
  void commit(int n, int m) {
    // (AI-assisted) The shader now projects positions to W=1 for GS-affine color and fog.
    // Clip depth crossings before that divide. Triangles inside every depth plane keep the tile's
    // indexed vertices; only a crossing triangle is cut into new ones (at most six).
    ctr_ocean_full_vertex* const tv = m_v + m_nv;
    if ((int)m_codes.size() < n) {
      m_codes.resize(n);
    }
    u8* const code = m_codes.data();
    u8 any = 0;
    for (int i = 0; i < n; i++) {
      const auto& v = tv[i];
      const u8 c = (u8)(!(depth_plane(v, 0) >= 0) | !(depth_plane(v, 1) >= 0) << 1 |
                        !(depth_plane(v, 2) >= 0) << 2);
      code[i] = c;
      any |= c;
    }
    if (any && m) {
      m_clip_indices.assign(m_i + m_ni, m_i + m_ni + m);
      const int first = m_nv;
      m_nv += n;
      const u16* const ix = m_clip_indices.data();
      for (int i = 0; i + 2 < m; i += 3) {
        const int a = ix[i] - first, b = ix[i + 1] - first, c = ix[i + 2] - first;
        const u32 out = code[a] | code[b] | code[c];
        if (!out) {
          m_i[m_ni++] = ix[i];
          m_i[m_ni++] = ix[i + 1];
          m_i[m_ni++] = ix[i + 2];
          continue;
        }
        // all three outside the first plane any of them is outside of: that plane leaves nothing
        if (code[a] & code[b] & code[c] & (out & -out)) {
          continue;
        }
        clip_triangle(tv[a], tv[b], tv[c]);
      }
      vertices += m_nv - first;
      return;
    }
    m_nv += n;
    m_ni += m;
    vertices += n;
  }

  //! xgkick of the ocean-near program (CommonOceanRenderer::kick_from_near)
  void kick_near(const u8* data, u32 bytes) {
    u32 offset = 0;
    for (int tags = 0; tags < 64 && offset + 16 <= bytes; tags++) {
      GifTag tag(data + offset);
      offset += 16;
      const u32 nloop = tag.nloop();
      if (tag.nreg() == 3) {
        if (offset + 48 * nloop > bytes) {
          return;
        }
        if (m_near_bucket == kTextured) {
          prim(data + offset, nloop, GsPrim(tag.prim()).kind() != GsPrim::Kind::TRI_STRIP);
        }
        offset += 48 * nloop;
      } else if (tag.nreg() == 1) {
        if (offset + 16 * nloop > bytes) {
          return;
        }
        near_adgif(data + offset, nloop);
        offset += 16 * nloop;
      } else {
        return;
      }
      if (tag.eop()) {
        return;
      }
    }
  }

  //! xgkick of the ocean-mid program (CommonOceanRenderer::kick_from_mid): drawn without
  //! blending = the textured pass; with = the env map pass
  void kick_mid(const u8* data, u32 bytes) {
    u32 offset = 0;
    for (int tags = 0; tags < 64 && offset + 16 <= bytes; tags++) {
      GifTag tag(data + offset);
      offset += 16;
      if (tag.flg() != GifTag::Format::PACKED) {
        return;
      }
      const u32 nloop = tag.nloop();
      if (tag.nreg() == 1) {
        offset += 16 * nloop;  // the adgif: the env map texture (not drawn)
      } else if (tag.nreg() == 3) {
        if (offset + 48 * nloop > bytes) {
          return;
        }
        const GsPrim prim_reg(tag.prim());
        if (!prim_reg.abe()) {
          prim(data + offset, nloop, prim_reg.kind() != GsPrim::Kind::TRI_STRIP);
        }
        offset += 48 * nloop;
      } else {
        return;
      }
      if (tag.eop()) {
        return;
      }
    }
  }

  //! the far ocean: GIF packets of draw-large-polygon-ocean (fans of ST, RGBAQ, XYZF2) after the
  //! A+D register setup of ocean-init-buffer
  void far_gif(const u8* data, u32 bytes) {
    u32 offset = 0;
    while (offset + 16 <= bytes) {
      GifTag tag(data + offset);
      offset += 16;
      const u32 nloop = tag.nloop();
      const u32 nreg = tag.nreg() ? tag.nreg() : 16;
      if (tag.flg() != GifTag::Format::PACKED) {
        return;  // (not in this bucket)
      }
      if (nreg == 3 && tag.pre()) {
        if (offset + 48 * nloop > bytes) {
          return;
        }
        prim(data + offset, nloop, GsPrim(tag.prim()).kind() != GsPrim::Kind::TRI_STRIP);
      }
      offset += 16 * nreg * nloop;
    }
  }

  //! a polygon of GS-space vertices (as a far ocean GS packet's: x, y 12.4, z, q, s * q, t * q,
  //! fog amount), drawn as a fan
  void gs_fan(const ctr_ocean_full_vertex* in, int n) {
    if (n < 3 || !mode(kModeGs) || !room(n, 3 * n)) {
      return;
    }
    const int first = m_nv;
    memcpy(m_v + m_nv, in, sizeof(ctr_ocean_full_vertex) * (size_t)n);
    u16* idx = m_i + m_ni;
    for (int i = 2; i < n; i++) {
      idx[0] = (u16)first;
      idx[1] = (u16)(first + i - 1);
      idx[2] = (u16)(first + i);
      idx += 3;
    }
    m_nv += n;
    m_ni += 3 * (n - 2);
    vertices += n;
  }

  // statistics
  int vertices = 0, triangles = 0, draws = 0, dropped = 0;
  int env_draws = 0;  // (AI-assisted, j1ocean) draws with the sky reflection
  // (AI-assisted) transition pieces the native program refused (vertex count out of range), and
  // both counts since the last warning in the log (CtrOceanFull::frame_stats)
  int rejected = 0;
  int dropped_unlogged = 0, rejected_unlogged = 0;
  void reject() {
    rejected++;
    rejected_unlogged++;
  }

 private:
  static float depth_plane(const ctr_ocean_full_vertex& v, int plane) {
    return plane == 0 ? v.q - 1e-8f : plane == 1 ? v.z : 16777215.f * v.q - v.z;
  }
  static ctr_ocean_full_vertex mix(const ctr_ocean_full_vertex& a,
                                   const ctr_ocean_full_vertex& b, float t) {
    ctr_ocean_full_vertex o{};
#define OCEAN_MIX(c) o.c = a.c + t * (b.c - a.c)
    OCEAN_MIX(x); OCEAN_MIX(y); OCEAN_MIX(z); OCEAN_MIX(q);
    OCEAN_MIX(s); OCEAN_MIX(t); OCEAN_MIX(fog); OCEAN_MIX(es); OCEAN_MIX(et);
#undef OCEAN_MIX
    o.r = round_u8(a.r + t * (b.r - a.r));
    o.g = round_u8(a.g + t * (b.g - a.g));
    o.b = round_u8(a.b + t * (b.b - a.b));
    o.a = round_u8(a.a + t * (b.a - a.a));
    return o;
  }
  // lround, then clamped to a byte: half away from zero, exact in double
  static u8 round_u8(float x) {
    return (u8)std::clamp((long)((double)x + (x < 0 ? -0.5 : 0.5)), 0l, 255l);
  }
  //! a triangle cut by the depth planes in turn (Sutherland-Hodgman), drawn as a fan of new
  //! vertices
  void clip_triangle(const ctr_ocean_full_vertex& a, const ctr_ocean_full_vertex& b,
                     const ctr_ocean_full_vertex& c) {
    ctr_ocean_full_vertex made[24];
    int nmade = 0;
    const ctr_ocean_full_vertex* poly[8] = {&a, &b, &c};
    const ctr_ocean_full_vertex* tmp[8];
    int count = 3;
    for (int plane = 0; plane < 3 && count; plane++) {
      int out = 0;
      for (int k = 0; k < count && out < 7; k++) {
        const auto& p = *poly[k];
        const auto& q = *poly[k + 1 < count ? k + 1 : 0];  // (AI-assisted, idiv) no divide
        const float dp = depth_plane(p, plane), dq = depth_plane(q, plane);
        if (dp >= 0) tmp[out++] = &p;
        if ((dp >= 0) != (dq >= 0)) {
          made[nmade] = mix(p, q, dp / (dp - dq));
          tmp[out++] = &made[nmade++];
        }
      }
      count = out;
      std::copy(tmp, tmp + count, poly);
    }
    if (count >= 3) {
      const int base = m_nv;
      for (int k = 0; k < count; k++) {
        m_v[m_nv++] = *poly[k];
      }
      for (int k = 1; k + 1 < count; k++) {
        m_i[m_ni++] = (u16)base;
        m_i[m_ni++] = (u16)(base + k);
        m_i[m_ni++] = (u16)(base + k + 1);
      }
    }
  }
  std::vector<u8> m_codes;  // per vertex of the tile committed: the depth planes it is outside of
  std::vector<u16> m_clip_indices;
  enum : u32 { kTextured = 0, kAlpha = 1, kEnvMap = 2 };

  //! CommonOceanRenderer::handle_near_adgif: which of the 3 passes the next packets are
  void near_adgif(const u8* data, u32 count) {
    for (u32 i = 0; i < count; i++) {
      u64 value;
      memcpy(&value, data + 16 * i, 8);
      const auto addr = (GsRegisterAddress)data[16 * i + 8];
      switch (addr) {
        case GsRegisterAddress::CLAMP_1:
          if (value & 0b001) {
            m_near_bucket = kEnvMap;
          }
          break;
        case GsRegisterAddress::TEX0_1:
          if (!GsTex0(value).tcc()) {
            m_near_bucket = kTextured;
          }
          break;
        case GsRegisterAddress::ALPHA_1: {
          GsAlpha reg(value);
          if (reg.a_mode() == GsAlpha::BlendMode::SOURCE &&
              reg.b_mode() == GsAlpha::BlendMode::ZERO_OR_FIXED &&
              reg.c_mode() == GsAlpha::BlendMode::DEST &&
              reg.d_mode() == GsAlpha::BlendMode::DEST) {
            m_near_bucket = kEnvMap;
          }
        } break;
        case GsRegisterAddress::FRAME_1:
          if (value >> 32) {
            m_near_bucket = kAlpha;
          }
          break;
        default:
          break;
      }
    }
  }

  /*!
   * nloop vertices of ST, RGBAQ, XYZF2 (packed): a strip or a fan. Like the GS, a vertex with the
   * ADC bit set joins the primitive without drawing a triangle.
   */
  void prim(const u8* data, u32 nloop, bool fan) {
    if (nloop < 3) {
      return;
    }
    if (!mode(kModeGs) || !room((int)nloop, 3 * (int)nloop)) {
      return;
    }
    const int first = m_nv;
    ctr_ocean_full_vertex* v = m_v + m_nv;
    u16* idx = m_i + m_ni;
    for (u32 i = 0; i < nloop; i++, v++) {
      const u8* p = data + 48 * i;
      // (AI-assisted) the words read where they are, not copied to the stack first
      u32 w[12];
      for (int k : {0, 1, 2, 4, 5, 6, 7, 8, 9, 10, 11}) {
        memcpy(&w[k], p + 4 * k, 4);
      }
      // ST (with Q): floats; Q > 0 for the GPU's 1 / Q (a far vertex near the horizon has a tiny
      // Q): as integers, so no float compare (positive floats order like their bits)
      memcpy(&v->s, &w[0], 8);
      const u32 q_bits = (s32)w[2] < 0x2B8CBCCC ? 0x2B8CBCCCu : w[2];  // 1e-12
      memcpy(&v->q, &q_bits, 4);
      v->r = (u8)w[4];
      v->g = (u8)w[5];
      v->b = (u8)w[6];
      v->a = (u8)w[7];
      // XYZF2: x, y 12.4 in the low halves of words 8 and 9, z bits 4..27 of word 10, F bits
      // 4..11 of word 11, ADC bit 15 of word 11
      v->x = (float)(w[8] & 0xffff);
      v->y = (float)(w[9] & 0xffff);
      v->z = (float)((w[10] >> 4) & 0xffffff);
      v->fog = kOceanFog.amount[(w[11] >> 4) & 0xff];
      v->es = v->et = 0.f;  // (AI-assisted, j1ocean) no sky reflection in these draws
      if (i >= 2 && !(w[11] & 0x8000)) {
        const int k = first + (int)i;
        idx[0] = (u16)(fan ? first : k - 2);
        idx[1] = (u16)(k - 1);
        idx[2] = (u16)k;
        idx += 3;
      }
    }
    m_nv += (int)nloop;
    m_ni = (int)(idx - m_i);
    vertices += (int)nloop;
  }

  //! GS vertices, clip space, or clip space with the sky reflection: a change ends the draw
  enum : int { kModeGs = 0, kModeClip = 1, kModeEnv = 2 };
  bool mode(int m) {
    if (m != m_mode) {
      flush();
      m_mode = m;
    }
    return true;
  }

  bool room(int n, int m) {
    if (m_open && m_nv + n <= m_max_v && m_ni + m <= m_max_i) {
      return true;
    }
    flush();
    // the next chunk, smaller when the frame's vertex memory is short, down to this piece's own
    // size; only a piece that doesn't fit even so is left out (the next one tries again)
    const int need = std::max(n, (m + 2) / 3);
    for (int size = std::max(kChunkVerts, need);; size = std::max(size / 4, need)) {
      if (ctr_gpu_ocean_full_reserve(size, 3 * size, &m_v, &m_i)) {
        m_open = true;
        m_max_v = size;
        m_max_i = 3 * size;
        m_nv = m_ni = 0;
        return true;
      }
      if (size == need) {
        break;
      }
    }
    dropped++;
    dropped_unlogged++;
    return false;
  }

  void flush() {
    if (!m_open) {
      return;
    }
    m_open = false;
    if (m_mode == kModeEnv) {
      ctr_gpu_ocean_full_draw_env(&m_state, m_nv, m_ni, m_fog[0], m_fog[1], m_fog[2], m_env_tex);
      env_draws++;
    } else if (m_mode == kModeClip) {
      ctr_gpu_ocean_full_draw_clip(&m_state, m_nv, m_ni, m_fog[0], m_fog[1], m_fog[2]);
    } else {
      ctr_gpu_ocean_full_draw(&m_state, m_nv, m_ni, m_fog[0], m_fog[1], m_fog[2]);
    }
    if (m_ni >= 3) {
      draws++;
      triangles += m_ni / 3;
    }
    m_nv = m_ni = 0;
  }

  ctr_draw_state m_state;
  u8 m_fog[3] = {0, 0, 0};
  bool m_open = false;
  int m_mode = kModeGs;
  int m_env_tex = -1;  // (AI-assisted, j1ocean)
  bool m_env_mapped = false;
  float m_env_map[6] = {0, 1, 0, 0, 0, 1};
  ctr_ocean_full_vertex* m_v = nullptr;
  u16* m_i = nullptr;
  int m_nv = 0, m_ni = 0, m_max_v = 0, m_max_i = 0;
  u32 m_near_bucket = kTextured;  // CommonOceanRenderer::m_current_bucket (kept between kicks)
};

