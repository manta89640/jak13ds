/*!
 * @file native_ocean.cpp
 * Native versions of Jak 2's ocean mips2c functions: render-ocean-quad (ocean.cpp, with
 * draw-large-polygon-ocean and the clip-polygon-against-*-hyperplane passes of sky.cpp inlined:
 * only these mips2c functions call them) and (method 14/15/16 ocean) (ocean_vu0.cpp: the wave
 * heights and the wave texture's vertices). Ported from Jak 1's (jak1_functions/native_ocean.cpp).
 * Same float operations, grouped the same way as the mips2c code (mips2c_native.h).
 */

#include <algorithm>

#include "game/mips2c/jak2_functions/native_functions.h"

namespace Mips2C::jak2 {
extern ExecutionContext ocean_regs_vfs;
}

namespace Mips2C::jak2::native {

namespace {

//! vitof15 of each s8 height moved to the top byte of a word (pextlb, pextlh): the height * 512
struct HeightFloats {
  float f[256];
  HeightFloats() {
    for (u32 b = 0; b < 256; b++) {
      f[b] = ((float)(s32)(b << 24)) * (1.f / 32768.f);
    }
  }
};

/*!
 * (method 14 ocean) (interp-wave this dest frame scale): 32x32 wave heights at dest between two of
 * the 64 frames of *ocean-wave-frames* (1024 s8 heights each): (int)frame and the next one
 * (& 63), weighted by (1 - frac, frac) * scale. The two weights are left in the ocean at 144, 148.
 */
u64 method_14_ocean_impl(const NativeArgs& args) {
  static const u32 frames_sym = sym_val_addr("*ocean-wave-frames*");
  static const HeightFloats heights;

  const u32 obj = (u32)args.a[0];
  const u32 dest = (u32)args.a[1];
  const float frame = u2f((u32)args.a[2]);
  const float scale = u2f((u32)args.a[3]);
  const s32 i = (s32)frame;
  const u32 frames = gload<u32>(frames_sym);
  const u8* frame_a = gptr(frames + ((u32)i << 10));
  const u8* frame_b = gptr(frames + ((((u32)i + 1) & 63) << 10));

  gstore<float>(obj + 148, frame - (float)i);
  gstore<float>(obj + 144, 1.f - gload<float>(obj + 148));
  gstore<float>(obj + 144, gload<float>(obj + 144) * scale);
  gstore<float>(obj + 148, gload<float>(obj + 148) * scale);
  const float weight_a = gload<float>(obj + 144);
  const float weight_b = gload<float>(obj + 148);

  // vmulax: acc = a * weight_a (for each of the 256 heights), vmaddy: acc + b * weight_b
  float acc[256];
  for (u32 b = 0; b < 256; b++) {
    acc[b] = heights.f[b] * weight_a;
  }
  float row[32];
  for (u32 r = 0; r < 1024; r += 32) {
    for (u32 k = 0; k < 32; k++) {
      row[k] = acc[frame_a[r + k]] + heights.f[frame_b[r + k]] * weight_b;
    }
    gstore_bytes(dest + 4 * r, row, sizeof(row));
  }
  return 0;
}

//! (method 15 ocean) (this heights heights2): heights (1024 floats) += heights2's first 16 floats,
//! every 16 (the PS2 code doesn't advance the heights2 pointer)
u64 method_15_ocean_impl(const NativeArgs& args) {
  const u32 dst = (u32)args.a[1];
  float add[16];
  gload_q(add, (u32)args.a[2]);
  gload_q(add + 4, (u32)args.a[2] + 16);
  gload_q(add + 8, (u32)args.a[2] + 32);
  gload_q(add + 12, (u32)args.a[2] + 48);
  float row[64];
  for (u32 off = 0; off < 4096; off += 256) {
    for (u32 k = 0; k < 64; k++) {
      row[k] = gload<float>(dst + off + 4 * k) + add[k & 15];
    }
    gstore_bytes(dst + off, row, sizeof(row));
  }
  return 0;
}

inline void store_color_w1(u32 addr, const float c[4]) {
  const float v[4] = {c[0], c[1], c[2], 1.f};
  gstore_q(addr, v);
}

/*!
 * (method 16 ocean) (generate-verts this verts heights): the wave texture's vertices from the
 * 32x32 heights, as Jak 1's ocean-generate-verts (8 quadwords per 4 vertices: color, the normals'
 * x, color, y, color, z, color, w = the heights). The lights: sewerb's light group when that level
 * is loaded, else the time of day's (vu-lights at 8144), the colors scaled down to a sum of 1.25,
 * times *ocean-generate-verts-vector*, w 128 (ambient 0). The colors' four lanes are lit (no
 * 1 / (z - 1) in w as in Jak 1).
 */
u64 method_16_ocean_impl(const NativeArgs& args) {
  static const u32 level_sym = sym_val_addr("*level*");
  static const u32 tod_sym = sym_val_addr("*time-of-day-context*");
  static const u32 block_sym = sym_val_addr("ocean-vu0-block");
  static const u32 sewerb_sym = sym_val_addr("sewerb") + 1;
  static const u32 upload_sym = sym_val_addr("upload-vu0-program");
  static const u32 lights_sym = sym_val_addr("vu-lights<-light-group!");
  static const u32 scale_sym = sym_val_addr("*ocean-generate-verts-vector*");

  const u32 obj = (u32)args.a[0];
  u32 dest = (u32)args.a[1];
  const u32 heights = (u32)args.a[2];
  {
    const u64 a[8] = {gload<u32>(block_sym), obj + 8296, args.a[2], args.a[3],
                      args.a[4],             args.a[5],  args.a[6], args.a[7]};
    native_call_goal(gload<u32>(upload_sym), a, args);
  }
  u32 group;
  {
    const u32 level = gload<u32>(level_sym);
    const u32 level_get = gload<u32>(gload<u32>(level - 4) + 52);
    const u64 a[8] = {level, sewerb_sym, args.a[2], args.a[3],
                      args.a[4], args.a[5], args.a[6], args.a[7]};
    const u64 lev = native_call_goal(level_get, a, args);
    group = lev != args.st ? (u32)lev + 716 : gload<u32>(tod_sym) + 140;
  }
  {
    const u64 a[8] = {obj + 8144, group, args.a[2], args.a[3],
                      args.a[4],  args.a[5], args.a[6], args.a[7]};
    native_call_goal(gload<u32>(lights_sym), a, args);
  }

  float c[4][4];  // the three colors and the ambient
  for (int i = 0; i < 4; i++) {
    gload_q(c[i], obj + 8192 + 16 * i);
  }
  float sum[3];
  for (int i = 0; i < 3; i++) {
    sum[i] = c[0][i] + c[1][i];
  }
  for (int i = 0; i < 3; i++) {
    sum[i] = sum[i] + c[2][i];
  }
  for (int i = 0; i < 3; i++) {
    sum[i] = sum[i] + c[3][i];
  }
  if (1.25f < sum[0] || 1.25f < sum[1] || 1.25f < sum[2]) {
    float big, base;
    if (sum[1] < sum[0]) {
      big = sum[0];
      base = c[3][0];
    } else {
      big = sum[1];
      base = c[3][1];
    }
    if (big < sum[2]) {
      big = sum[2];
      base = c[3][2];
    }
    const float num = 1.25f - base;
    const float den = big - base;
    const float s = num / den;
    for (int k = 0; k < 3; k++) {
      for (int i = 0; i < 3; i++) {
        c[k][i] = c[k][i] * s;
      }
      store_color_w1(obj + 8192 + 16 * k, c[k]);
    }
  }
  {
    float v[4];
    gload_q(v, gload<u32>(scale_sym));
    for (int k = 0; k < 4; k++) {
      for (int i = 0; i < 3; i++) {
        c[k][i] = c[k][i] * v[i];
      }
      store_color_w1(obj + 8192 + 16 * k, c[k]);
    }
  }
  c[0][3] = c[1][3] = c[2][3] = 128.f;
  c[3][3] = 0.f;
  for (int k = 0; k < 4; k++) {
    gstore<float>(obj + 8192 + 16 * k + 12, c[k][3]);
  }

  float k12[4], l0[4], l1[4], l2[4];
  gload_q(k12, obj + 8096);  // vf12
  gload_q(l0, obj + 8144);
  gload_q(l1, obj + 8160);
  gload_q(l2, obj + 8176);
  // (acc = vf01 * vf11: the ambient times 1.0)
  float amb1[4];
  for (int i = 0; i < 4; i++) {
    amb1[i] = 1.f * c[3][i];
  }

  for (u32 r = 0; r < 32; r++) {
    const u32 row = heights + 128 * r;
    const u32 next = heights + 128 * ((r + 1) & 31);
    for (u32 g = 0; g < 8; g++) {
      // vf02: 4 heights of the row, vf03: the 4 after them (the row's first 4 for the last
      // group), vf04: the next row's 4 under vf02 (only x is read)
      float a[4], b[4], n[4];
      gload_q(a, row + 16 * g);
      gload_q(b, row + 16 * ((g + 1) & 7));
      gload_q(n, next + 16 * g);
      float nx[4], ny[4], nz[4], nw[4], col[4][4];
      for (int v = 0; v < 4; v++) {
        // mula.x acc, vf12, vf02.(v + 1) / vf04.x; mula.z acc, vf12, vf03.v;
        // msub.xz vf24+v, vf12, vf02.v
        const float accx = k12[0] * (v < 3 ? a[v + 1] : n[0]);
        const float accz = k12[2] * b[v];
        nx[v] = accx - k12[0] * a[v];
        nz[v] = accz - k12[2] * a[v];
      }
      for (int v = 0; v < 4; v++) {
        const float x2 = nx[v] * nx[v];
        const float z2 = nz[v] * nz[v];
        float y = 1.f - x2;
        ny[v] = y - z2;
        nw[v] = 1.f * a[v];
      }
      float d[4][3];
      for (int v = 0; v < 4; v++) {
        // the light directions' dot products: acc = vf05 * x + vf06 * y, + vf07 * z, then
        // clamped to 0 (maxx with vf00.x)
        for (int i = 0; i < 3; i++) {
          float acc = l0[i] * nx[v];
          acc += l1[i] * ny[v];
          d[v][i] = vu_max(acc + l2[i] * nz[v], 0.f);
        }
      }
      for (int v = 0; v < 4; v++) {
        // acc = vf01 * vf11, + vf08 * d.x + vf09 * d.y, + vf10 * d.z, then at most vf12.y
        for (int i = 0; i < 4; i++) {
          float acc = amb1[i];
          acc += c[0][i] * d[v][0];
          acc += c[1][i] * d[v][1];
          col[v][i] = vu_min(acc + c[2][i] * d[v][2], k12[1]);
        }
      }
      gstore_q(dest, col[0]);
      gstore_q(dest + 16, nx);
      gstore_q(dest + 32, col[1]);
      gstore_q(dest + 48, ny);
      gstore_q(dest + 64, col[2]);
      gstore_q(dest + 80, nz);
      gstore_q(dest + 96, col[3]);
      gstore_q(dest + 112, nw);
      dest += 128;
    }
  }
  return 0;
}

// ---------------------------------------------------------------------------------------------
// render-ocean-quad with draw-large-polygon-ocean and the polygon clippers
// ---------------------------------------------------------------------------------------------

//! a polygon vertex as the mips2c code keeps it in the scratchpad: position, stq, color
struct PolyVert {
  float p[4], st[4], c[4];
};
static_assert(sizeof(PolyVert) == 48);
// a clip pass keeps its k inside vertices and adds at most 2 * min(k, n - k) crossings, so at most
// 1.5 n: 4 -> 6 -> 9 -> 13 -> 19 over the four planes, and the first vertex again at the end
constexpr int kPolyMax = 20;

inline void store_vert(u32 addr, const PolyVert& v) {
  gstore_q(addr, v.p);
  gstore_q(addr + 16, v.st);
  gstore_q(addr + 32, v.c);
}

//! from + (to - from) * t into r, the three quadwords (vmulaw, then vmaddx: one expression)
inline void lerp_into(PolyVert& r, const PolyVert& from, const PolyVert& to, float t) {
  for (int i = 0; i < 4; i++) {
    const float d = to.p[i] - from.p[i];
    const float acc = from.p[i] * 1.f;
    r.p[i] = acc + d * t;
  }
  for (int i = 0; i < 4; i++) {
    const float d = to.st[i] - from.st[i];
    const float acc = from.st[i] * 1.f;
    r.st[i] = acc + d * t;
  }
  for (int i = 0; i < 4; i++) {
    const float d = to.c[i] - from.c[i];
    const float acc = from.c[i] * 1.f;
    r.c[i] = acc + d * t;
  }
}

//! -w by its sign bit (neg.s)
inline float neg_bits(float w) {
  return u2f(f2u(w) ^ 0x80000000u);
}

/*!
 * clip-polygon-against-positive-hyperplane (neg false: inside when !(w < x)) or -negative-
 * (neg true: inside when !(x < -w)), x = component comp of the position: the polygon in[0..n]
 * (in[n] = in[0]) to out[0..count] (out[count] = out[0]), what the mips2c version stores in the
 * scratchpad. Sutherland-Hodgman: an inside vertex is kept when its edge is processed; an edge
 * crossing the plane adds the intersection. *clipped: set when an edge crossed.
 */
template <int comp, bool neg>
int clip_polygon(const PolyVert* in, int n, PolyVert* out, bool* clipped) {
  int count = 0;
  const PolyVert* prev = &in[0];
  float pa = neg ? neg_bits(prev->p[3]) : prev->p[3], px = prev->p[comp];
  bool prev_out = neg ? px < pa : pa < px;
  for (int e = 1; e <= n; e++) {
    const PolyVert* next = &in[e];
    const float na = neg ? neg_bits(next->p[3]) : next->p[3], nx = next->p[comp];
    const bool next_out = neg ? nx < na : na < nx;
    if (!prev_out) {
      out[count++] = *prev;
      if (next_out) {
        const float fp = pa - px, fn = na - nx;
        const float den = fn - fp;
        lerp_into(out[count++], *next, *prev, fn / den);
        *clipped = true;
      }
    } else if (!next_out) {
      const float fp = pa - px, fn = na - nx;
      const float den = fp - fn;
      lerp_into(out[count++], *prev, *next, fp / den);
      *clipped = true;
    }
    prev = next;
    pa = na;
    px = nx;
    prev_out = next_out;
  }
  if (count > 0) {
    out[count] = out[0];
  }
  return count;
}

//! the first `slots` vertices of a buffer into the scratchpad at addr (48 bytes each)
inline void store_verts(u32 addr, const PolyVert* v, int slots) {
  for (int i = 0; i < slots; i++) {
    store_vert(addr + 48 * i, v[i]);
  }
}

/*!
 * (render-ocean-quad vertices dma-buf): one far ocean quad (4 ocean-vertex: position, stq,
 * color), transformed by the camera matrix of init-ocean-far-regs (vf28..vf31, kept in
 * ocean_regs_vfs), clipped in the scratchpad against x and y = +-w of the clip scaled position
 * (vf14), then as a GIF packet (vf27 with nloop = vertices) of ST, RGBAQ, XYZF2: the GS position
 * (* Q + vf25, z >= 0, the fog clamped to vf13.y..z), Q = vf13.x / w (not clipped: the original
 * positions) or 1 / w of the clip scaled one times vf26 (clipped). #t when something is drawn.
 * A quad inside all four planes skips the passes (each gives back its input); the scratchpad
 * buffers end with what the passes' stores leave there.
 */
u64 render_ocean_quad_impl(const NativeArgs& args) {
  static const u32 spad_sym = sym_val_addr("*fake-scratchpad-data*");
  const auto& vf = ocean_regs_vfs.vfs;
  const u32 src = (u32)args.a[0];
  const u32 dma = (u32)args.a[1];
  const u32 spad = gload<u32>(spad_sym);
  const u32 t4 = spad | 12288, t5 = spad | 14336;

  float m[4][4], k14[4];
  for (int i = 0; i < 4; i++) {
    m[0][i] = vf[31].f[i];
    m[1][i] = vf[30].f[i];
    m[2][i] = vf[29].f[i];
    m[3][i] = vf[28].f[i];
    k14[i] = vf[14].f[i];
  }
  float pos[4][4];
  PolyVert poly_a[kPolyMax], poly_b[kPolyMax];
  for (int v = 0; v < 4; v++) {
    float p[4];
    gload_q(p, src + 48 * v);
    for (int i = 0; i < 4; i++) {
      float acc = m[0][i] * p[0];
      acc += m[1][i] * p[1];
      acc += m[2][i] * p[2];
      pos[v][i] = acc + m[3][i] * p[3];
    }
    PolyVert& q = poly_a[v];
    for (int i = 0; i < 4; i++) {
      q.p[i] = pos[v][i] * k14[i];
    }
    gload_q(q.st, src + 48 * v + 16);
    gload_q(q.c, src + 48 * v + 32);
  }
  poly_a[4] = poly_a[0];
  // slots of t4 (poly_a) and t5 (poly_b) the stores wrote: t4 0..4 first
  int slots_a = 5, slots_b = 0;
  auto note = [](int& slots, int n) {
    if (n > 0) {
      slots = std::max(slots, n + 1);
    }
  };

  bool inside = true;
  u32 out_all = 15;
  for (int v = 0; v < 4; v++) {
    const float* p = poly_a[v].p;
    const float w = p[3], nw = neg_bits(p[3]);
    const u32 code = (u32)(w < p[0]) | ((u32)(w < p[1]) << 1) | ((u32)(p[0] < nw) << 2) |
                     ((u32)(p[1] < nw) << 3);
    inside &= code == 0;
    out_all &= code;
  }
  // all four outside one of the planes: the clipping leaves nothing (only the scratchpad copies,
  // which nothing reads, would differ)
  if (out_all) {
    return args.st;
  }

  bool clipped = false;
  int n = 4;
  if (inside) {
    store_verts(t5, poly_a, 5);
  } else {
    // draw-large-polygon-ocean
    auto bail = [&]() {
      store_verts(t5, poly_b, slots_b);
      store_verts(t4, poly_a, slots_a);
      return args.st;
    };
    n = clip_polygon<0, false>(poly_a, 4, poly_b, &clipped);
    note(slots_b, n);
    if (!n) {
      return bail();
    }
    n = clip_polygon<1, false>(poly_b, n, poly_a, &clipped);
    note(slots_a, n);
    if (!n) {
      return bail();
    }
    n = clip_polygon<0, true>(poly_a, n, poly_b, &clipped);
    note(slots_b, n);
    if (!n) {
      return bail();
    }
    n = clip_polygon<1, true>(poly_b, n, poly_a, &clipped);
    note(slots_a, n);
    if (!n) {
      return bail();
    }
    store_verts(t5, poly_b, slots_b);
  }

  if (!clipped) {
    // the original positions again (the 4 vertices and the first at the end)
    for (int v = 0; v < 4; v++) {
      memcpy(poly_a[v].p, pos[v], 16);
    }
    memcpy(poly_a[4].p, pos[0], 16);
  }
  store_verts(t4, poly_a, slots_a);

  u32 out = gload<u32>(dma + 4);
  gstore_q(out, vf[27].f);
  gstore<u32>(out, (u32)n);
  out += 16;
  const float fog_lo = vf[13].f[1], fog_hi = vf[13].f[2];
  for (int v = 0; v < n; v++) {
    const PolyVert& q = poly_a[v];
    float p[4], st[4];
    s32 col[4];
    float qq;
    if (clipped) {
      qq = 1.f / q.p[3];
      for (int i = 0; i < 4; i++) {
        p[i] = q.p[i] * vf[26].f[i];
      }
    } else {
      qq = vf[13].f[0] / q.p[3];
      for (int i = 0; i < 4; i++) {
        p[i] = q.p[i];
      }
    }
    for (int i = 0; i < 4; i++) {
      st[i] = q.st[i];
      col[i] = (s32)q.c[i];
    }
    for (int i = 0; i < 3; i++) {
      p[i] = p[i] * qq;
    }
    for (int i = 0; i < 4; i++) {
      st[i] = st[i] * qq;
      p[i] = p[i] + vf[25].f[i];
    }
    p[2] = std::max(p[2], 0.f);
    p[3] = std::min(p[3], fog_hi);
    p[3] = std::max(p[3], fog_lo);
    s32 xyzf[4];
    for (int i = 0; i < 4; i++) {
      xyzf[i] = (s32)(p[i] * 16.f);
    }
    gstore_q(out, st);
    gstore_q(out + 16, col);
    gstore_q(out + 32, xyzf);
    out += 48;
  }
  gstore<u32>(dma + 4, out);
  return goal_true(args.st);
}

}  // namespace

const NativeImpl method_14_ocean = MIPS2C_NATIVE_IMPL(method_14_ocean_impl, 0, 0);
const NativeImpl method_15_ocean = MIPS2C_NATIVE_IMPL(method_15_ocean_impl, 0, 0);
const NativeImpl method_16_ocean = MIPS2C_NATIVE_IMPL(method_16_ocean_impl, NATIVE_CALLS_GOAL, 0);
const NativeImpl render_ocean_quad = MIPS2C_NATIVE_IMPL_GOAL(render_ocean_quad_impl, 0);

}  // namespace Mips2C::jak2::native
