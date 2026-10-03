/*!
 * @file CtrOceanNative.cpp
 * (AI-assisted)
 * The ocean-mid VU program's drawing calls as plain C++ for the 3DS (CtrOceanVu.h, CtrOceanFull.h).
 *
 * The emulated program (OceanMid_PS2.cpp) spends most of its time on things the 3DS doesn't need:
 * the VU register file and masks, the divide per vertex, clipping triangles at the guard band
 * (skipped with the GS's ADC bit, or cut by the program's clipper, L38 / L45) and the env map pass
 * (not drawn on the 3DS). Here only the textured triangles are made, in clip space: the vertex is
 * W * (GS x, GS y, GS z, 1) (one matrix per tile, from the tile matrix, the GS's Q = fog.x / w and
 * the hvdf offset), so the GPU divides and clips (ctr_gpu_ocean_full_draw_clip). Same triangles,
 * colors, texture coordinates and fog as the program's for those it draws whole; the ones it
 * clips are drawn by the GPU's clipper instead (and the far tiles' ones it drops near the screen
 * edge are drawn).
 *
 * The inputs are read from the emulated VU memory where the DMA put them (CtrOceanMidVu::run), at
 * the program's addresses: the constants (733...), the grid and texture coordinates made by
 * call 0 (236, 317) and the per call uploads (the double buffer at xtop).
 */

#include <cstring>

#include "game/graphics/ctr/CtrOceanBatch.h"
#include "game/graphics/ctr/CtrOceanVu.h"

namespace {

// ocean-mid VU memory (OceanMid_PS2.cpp)
constexpr int kGridXz = 236;  // 9 x 9 (x, 0, z, 1), made by call 0
constexpr int kHmge = 733, kHvdf = 735, kFogConst = 736, kIndexTable = 757, kCorners = 765;

inline u32 lane(const Vf& v, int i) {
  u32 r;
  memcpy(&r, &v.data[i], 4);
  return r;
}

//! GS color (the low byte of each lane) as the vertex's RGBA
inline void put_color(ctr_ocean_full_vertex* o, u32 r, u32 g, u32 b, u32 a) {
  o->r = (u8)r;
  o->g = (u8)g;
  o->b = (u8)b;
  o->a = (u8)a;
}

/*!
 * The clip-space transform of one call: out = col[0] * x + col[1] * y + col[2] * z + col[3] for a
 * point (x, y, z, 1), from the VU's tile matrix (vf12..vf15: P = M * p, then the GS position
 * P.xyz * Q + hvdf with Q = fog.x / P.w). out = (P.x + hvdf.x * W, ..., W) with W = P.w / fog.x
 * (= P.w * hmge.w, > 0 in front of the camera) is W * (GS x, y, z, 1) (x, y times 16: 12.4 like
 * XYZF2). The fog value is P.w + hvdf.w = W * fog.x + hvdf.w, clamped to fog.y .. fog.z, as 12.4
 * (ftoi4) like the GS's XYZF2 (clamping after ftoi4 gives the same: both are monotonic).
 */
struct ClipXform {
  float col[4][4];
  float fog_k, fog_add;   // 16 * (W * fog.x + hvdf.w)
  s32 fog_min, fog_max;   // ftoi4 of fog.y, fog.z

  ClipXform(const Vf* m, const Vf* vu) {
    const float inv_q = vu[kHmge].w();  // 1 / fog.x
    const Vf& hv = vu[kHvdf];
    for (int j = 0; j < 4; j++) {
      const float w = m[j].w() * inv_q;
      col[j][0] = 16.f * (m[j].x() + hv.x() * w);
      col[j][1] = 16.f * (m[j].y() + hv.y() * w);
      col[j][2] = m[j].z() + hv.z() * w;
      col[j][3] = w;
    }
    fog_k = 16.f * vu[kFogConst].x();
    fog_add = 16.f * hv.w();
    fog_min = (s32)(16.f * vu[kFogConst].y());
    fog_max = (s32)(16.f * vu[kFogConst].z());
  }

  //! fog amount of a vertex with this clip-space w
  float fog(float w) const {
    s32 f = (s32)(w * fog_k + fog_add);
    f = f < fog_min ? fog_min : f;
    f = f > fog_max ? fog_max : f;
    return kOceanFog.amount[(f >> 4) & 0xff];
  }
};

}  // namespace

/*!
 * The index table (constants, 757 + row) the tile program reads: row i of the 8 the call draws
 * starts at grid point 9 * (7 - i) and color 12 * (7 - i). Checked once per frame; when it isn't
 * that (not seen), the emulated program draws.
 */
bool CtrOceanMidVu::native_ready() {
  if (m_native_ok < 0) {
    m_native_ok = 1;
    for (int i = 0; i < 8; i++) {
      const Vf& e = m_vu_data[kIndexTable + i];
      if (lane(e, 0) != 9u * (7 - i) || lane(e, 1) != 12u * (7 - i)) {
        m_native_ok = 0;
      }
    }
  }
  return m_native_ok == 1;
}

/*!
 * call 46 / 73 (OceanMid::run_call46_vu2c / run_call73_vu2c with L26 / L32): one mid ocean tile,
 * 8 x 8 squares of the 9 x 9 grid, as strips of rows. The 8 mask bytes (vf04, uploaded at 116 and
 * 117) say which squares the transition / near ocean draws instead: a set bit leaves the square
 * out, a byte of 0xff the row. The grid's vertices are shared by the rows (one vertex per grid
 * point: t counts the rows, the texture repeats).
 */
void CtrOceanMidVu::native_tile() {
  const u16 base = xtop();
  const Vf* in = m_vu_data + base;
  const ClipXform xf(in + 4, m_vu_data);

  // the rows' masks in the order the program draws them (vi07 = 7 .. 0)
  u8 mask[8];
  int rows = 0;
  for (int k = 0; k < 8; k++) {
    mask[k] = (u8)lane(in[116 + k / 4], k % 4);
    rows += mask[k] != 0xff;
  }
  if (!rows) {
    return;
  }

  ctr_ocean_full_vertex* v;
  u16* idx;
  int first;
  if (!m_batch->reserve_clip(81, 6 * 8 * rows, &v, &idx, &first)) {
    return;
  }

  // grid point (r, c) = (x[c], 0, z[r], 1): out = col0 * x[c] + (col2 * z[r] + col3)
  float cx[9][4];
  for (int c = 0; c < 9; c++) {
    const float x = m_vu_data[kGridXz + c].x();
    for (int i = 0; i < 4; i++) {
      cx[c][i] = xf.col[0][i] * x;
    }
  }
  ctr_ocean_full_vertex* o = v;
  for (int r = 0; r < 9; r++) {
    const float z = m_vu_data[kGridXz + 9 * r].z();
    const float r0 = xf.col[2][0] * z + xf.col[3][0];
    const float r1 = xf.col[2][1] * z + xf.col[3][1];
    const float r2 = xf.col[2][2] * z + xf.col[3][2];
    const float r3 = xf.col[2][3] * z + xf.col[3][3];
    const Vf* color = in + 8 + 12 * r;
    for (int c = 0; c < 9; c++, o++) {
      o->x = r0 + cx[c][0];
      o->y = r1 + cx[c][1];
      o->z = r2 + cx[c][2];
      o->q = r3 + cx[c][3];
      o->s = (float)c;
      o->t = (float)r;
      o->fog = xf.fog(o->q);
      put_color(o, lane(color[c], 0), lane(color[c], 1), lane(color[c], 2), lane(color[c], 3));
    }
  }

  // the rows' strips: grid rows g and g + 1, squares c = 0..7 (both triangles in strip order)
  u16* out = idx;
  for (int k = 0; k < 8; k++) {
    if (mask[k] == 0xff) {
      continue;
    }
    const int top = first + 9 * k;  // (the index table: row vi07 = 7 - k is grid row k)
    for (int c = 0; c < 8; c++) {
      if (mask[k] & (1 << c)) {
        continue;
      }
      const u16 t0 = (u16)(top + c), b0 = (u16)(top + c + 9);
      out[0] = t0;
      out[1] = b0;
      out[2] = (u16)(t0 + 1);
      out[3] = b0;
      out[4] = (u16)(t0 + 1);
      out[5] = (u16)(b0 + 1);
      out += 6;
    }
  }
  m_batch->commit(81, (int)(out - idx));
}

/*!
 * call 107 / 275 (OceanMid::run_call107_vu2c / run_call275_vu2c, L45 / L43): the transition
 * between a mid ocean square and the near ocean. Its n vertices (8.x) are weights of the
 * square's corners (17 + i), for the position (765..768: 96 m squares), the texture coordinates
 * (9..12) and the color (13..16). 107: a strip of quads (vertices 2i, 2i + 1, 2i + 3, 2i + 2;
 * mask 8.y: bit i leaves quad i out); 275: one polygon (a fan from vertex 0).
 */
void CtrOceanMidVu::native_transition(bool fan) {
  const u16 base = xtop();
  const Vf* in = m_vu_data + base;
  const int n = in[8].x_as_u16();
  const u32 quad_mask = in[8].y_as_u16();
  if (n < 4 || n > 16) {
    return;  // (the tables have 7 or 10)
  }
  const int tris = fan ? n - 2 : 2 * ((n - 2) / 2);  // (n >= 4: at least one quad)
  ctr_ocean_full_vertex* v;
  u16* idx;
  int first;
  if (!m_batch->reserve_clip(n, 3 * tris, &v, &idx, &first)) {
    return;
  }
  const ClipXform xf(in + 4, m_vu_data);
  const Vf* corner = m_vu_data + kCorners;
  for (int i = 0; i < n; i++) {
    const Vf& w = in[17 + i];
    float p[4], st[2], col[4];
    for (int k = 0; k < 4; k++) {
      p[k] = corner[0][k] * w.x() + corner[1][k] * w.y() + corner[2][k] * w.z() +
             corner[3][k] * w.w();
      col[k] = in[13][k] * w.x() + in[14][k] * w.y() + in[15][k] * w.z() + in[16][k] * w.w();
    }
    for (int k = 0; k < 2; k++) {
      st[k] = in[9][k] * w.x() + in[10][k] * w.y() + in[11][k] * w.z() + in[12][k] * w.w();
    }
    ctr_ocean_full_vertex* o = v + i;
    // (the program transforms (p.x, p.y, p.z, 1))
    float c[4];
    for (int k = 0; k < 4; k++) {
      c[k] = xf.col[0][k] * p[0] + xf.col[1][k] * p[1] + xf.col[2][k] * p[2] + xf.col[3][k];
    }
    o->x = c[0];
    o->y = c[1];
    o->z = c[2];
    o->q = c[3];
    o->s = st[0];
    o->t = st[1];
    o->fog = xf.fog(c[3]);
    put_color(o, (u32)(s32)col[0], (u32)(s32)col[1], (u32)(s32)col[2], (u32)(s32)col[3]);
  }
  u16* out = idx;
  if (fan) {
    for (int i = 1; i + 1 < n; i++) {
      out[0] = (u16)first;
      out[1] = (u16)(first + i);
      out[2] = (u16)(first + i + 1);
      out += 3;
    }
  } else {
    // (the program's loop: (n - 2) / 2 quads, at least one)
    int left = n - 2;
    for (int q = 0;; q++) {
      left -= 2;
      if (!(quad_mask & (1u << q))) {
        const u16 a = (u16)(first + 2 * q);
        out[0] = a;
        out[1] = (u16)(a + 1);
        out[2] = (u16)(a + 3);
        out[3] = a;
        out[4] = (u16)(a + 3);
        out[5] = (u16)(a + 2);
        out += 6;
      }
      if (left <= 0) {
        break;
      }
    }
  }
  m_batch->commit(n, (int)(out - idx));
}
