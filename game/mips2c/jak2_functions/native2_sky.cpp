/*!
 * @file native2_sky.cpp
 * (AI-assisted)
 * Native versions of Jak 2's sky mips2c functions (sky.cpp): render-sky-quad and render-sky-tri
 * (with draw-large-polygon and the two clip-polygon-against-*-hyperplane passes inlined: only these
 * mips2c functions call them), (method 32 sky-work) (stars-transform-asm) and (method 33 sky-work)
 * (stars-dma). Same float operations, grouped the same way as the mips2c code (mips2c_native.h),
 * so the scratchpad buffers, the DMA data and sky_regs_vfs are bit-identical.
 */

#include <algorithm>

#include "native2.h"
#include "game/mips2c/native_goalc_ops.h"

namespace Mips2C::jak2::native2 {

namespace {

constexpr float kOne = 1.f;  // vf0.w
constexpr float kZero = 0.f;  // vf0.x/y/z

//! a sky-vertex in the clip buffers: position, texture coordinates, color (48 bytes)
struct SkyVert {
  float p[4];
  float t[4];
  float c[4];
};

inline void load_vert(SkyVert& v, u32 addr) {
  gload_q(v.p, addr);
  gload_q(v.t, addr + 16);
  gload_q(v.c, addr + 32);
}

inline void store_vert(u32 addr, const SkyVert& v) {
  gstore_q(addr, v.p);
  gstore_q(addr + 16, v.t);
  gstore_q(addr + 32, v.c);
}

inline const float* vf(int i) {
  return sky_regs_vfs.vfs[i].f;
}

/*!
 * The plane test of a clip pass. Positive: outside when w < c, distance d = w - c. Negative:
 * w' = -w (neg.s: sign bit flipped), outside when c < w', d = w' - c.
 */
template <bool NEG>
inline bool clip_test(const SkyVert& v, u32 comp, float& d) {
  float w = v.p[3];
  if (NEG) {
    w = u2f(f2u(w) ^ 0x80000000u);
  }
  const float c = v.p[comp];
  d = w - c;
  return NEG ? (c < w) : (w < c);
}

//! vmulaw acc, O, vf0 then vmaddx r, (I - O), t: r = O * 1 + (I - O) * t per lane
inline void clip_lerp(float* r, const float* o, const float* in, float t) {
  for (int i = 0; i < 4; i++) {
    const float diff = in[i] - o[i];
    const float acc = o[i] * kOne;
    r[i] = acc + diff * t;
  }
}

/*!
 * clip-polygon-against-positive/negative-hyperplane: n edges of the polygon at in (n + 1 vertices,
 * the last one a copy of the first) against the plane of component comp (0: x, 1: y). The kept
 * vertices and the intersections go to out, then a copy of the first output vertex after them.
 * Returns the number of output vertices. Reads and writes in the order of the mips2c code.
 */
template <bool NEG>
u32 clip_pass(u32 in, u32 out, u32 n, u32 comp) {
  const u32 out0 = out;
  u32 count = 0;
  SkyVert p;
  load_vert(p, in);
  in += 48;
  float pd;
  bool pout = clip_test<NEG>(p, comp, pd);
  for (u32 k = 0; k < n; k++) {
    SkyVert q;
    load_vert(q, in);
    in += 48;
    float qd;
    const bool qout = clip_test<NEG>(q, comp, qd);
    if (!pout) {
      store_vert(out, p);
      out += 48;
      count++;
    }
    if (pout != qout) {
      const SkyVert& o = pout ? p : q;
      const SkyVert& iv = pout ? q : p;
      const float d_out = pout ? pd : qd;
      const float d_in = pout ? qd : pd;
      const float den = d_out - d_in;
      const float t = d_out / den;
      SkyVert r;
      clip_lerp(r.p, o.p, iv.p, t);
      clip_lerp(r.t, o.t, iv.t, t);
      clip_lerp(r.c, o.c, iv.c, t);
      store_vert(out, r);
      out += 48;
      count++;
    }
    p = q;
    pd = qd;
    pout = qout;
  }
  // the first output vertex again after the last one (onto itself when nothing was output)
  SkyVert first;
  load_vert(first, out0);
  store_vert(out, first);
  return count;
}

/*!
 * draw-large-polygon: clips the polygon at t4 (n vertices + the closing copy) against the four
 * planes (t4 -> t5 -> t4 -> t5 -> t4) and adds it to the dma-buffer: vf27 with the vertex count in
 * its first word, then per vertex the texture coordinates * Q, the color as integers and the
 * screen position (vftoi4). Returns #t, or #f when a pass leaves nothing.
 */
u64 draw_large_polygon(u32 dma, u32 n, u32 t4, u32 t5, u64 st) {
  n = clip_pass<false>(t4, t5, n, 0);
  if (n == 0) {
    return st;
  }
  n = clip_pass<false>(t5, t4, n, 1);
  if (n == 0) {
    return st;
  }
  n = clip_pass<true>(t4, t5, n, 0);
  if (n == 0) {
    return st;
  }
  n = clip_pass<true>(t5, t4, n, 1);
  if (n == 0) {
    return st;
  }

  const float* vf13 = vf(13);
  const float* vf23 = vf(23);
  const float* vf24 = vf(24);
  const float* vf25 = vf(25);
  const float* vf26 = vf(26);
  u32 a3 = gload<u32>(dma + 4);
  {
    u32 tag[4];
    memcpy(tag, vf(27), 16);
    tag[0] = n;  // sw t0, -16(a3) after sqc2 vf27
    gstore_q(a3, tag);
  }
  a3 += 16;
  u32 a2 = t4;
  for (u32 k = 0; k < n; k++) {
    float v1[4], v2[4], v3[4];
    gload_q(v1, a2);
    gload_q(v2, a2 + 16);
    gload_q(v3, a2 + 32);
    const float q = kOne / v1[3];  // vdiv Q, vf0.w, vf1.w
    for (int i = 0; i < 4; i++) {
      v1[i] = v1[i] * vf26[i];
    }
    s32 c3[4];
    for (int i = 0; i < 4; i++) {
      c3[i] = (s32)v3[i];
    }
    for (int i = 0; i < 4; i++) {
      v2[i] = v2[i] + vf24[i];
    }
    for (int i = 0; i < 3; i++) {
      v1[i] = v1[i] * q;
    }
    gstore_q(a3 + 16, c3);
    for (int i = 0; i < 4; i++) {
      v2[i] = v2[i] * q;
    }
    a2 += 48;
    a3 += 48;
    for (int i = 0; i < 4; i++) {
      v1[i] = v1[i] + vf25[i];
    }
    v1[2] = v1[2] * kZero;              // vmulz.z vf1, vf1, vf0
    v1[3] = std::min(v1[3], vf13[2]);  // vminiz.w vf1, vf1, vf13
    v1[2] = v1[2] + vf23[0];           // vaddx.z vf1, vf1, vf23
    v1[3] = std::max(v1[3], vf13[1]);  // vmaxy.w vf1, vf1, vf13
    gstore_q(a3 - 48, v2);
    s32 c1[4];
    for (int i = 0; i < 4; i++) {
      c1[i] = (s32)(v1[i] * 16.f);
    }
    gstore_q(a3 - 16, c1);
  }
  gstore<u32>(dma + 4, a3);
  return st + 4;
}

//! the scratchpad buffers render-sky-quad/tri use: (*fake-scratchpad-data* | 0x3000 / 0x3800)
inline void sky_buffers(u32& t4, u32& t5) {
  static const u32 spad_sym = sym_value_addr("*fake-scratchpad-data*");
  const u32 spad = gload<u32>(spad_sym);
  t4 = spad | 12288;
  t5 = spad | 14336;
}

/*!
 * render-sky-quad / render-sky-tri: the vertices' positions transformed by vf28-vf31 (z = 0),
 * stored with their texture coordinates and colors at t4 (the first vertex again after the last
 * one), then draw-large-polygon.
 */
template <u32 N>
u64 render_sky_poly(const NativeArgs& args) {
  u32 t4, t5;
  sky_buffers(t4, t5);
  const u32 in = (u32)args.a[0];
  const float* vf28 = vf(28);
  const float* vf29 = vf(29);
  const float* vf30 = vf(30);
  const float* vf31 = vf(31);
  SkyVert first;
  for (u32 k = 0; k < N; k++) {
    SkyVert v;
    load_vert(v, in + 48 * k);
    float acc[4];
    for (int i = 0; i < 4; i++) {
      acc[i] = vf31[i] * v.p[0];
    }
    for (int i = 0; i < 4; i++) {
      acc[i] += vf30[i] * v.p[1];
    }
    for (int i = 0; i < 4; i++) {
      acc[i] += vf29[i] * v.p[2];
    }
    float r[4];
    r[0] = acc[0] + vf28[0] * v.p[3];
    r[1] = acc[1] + vf28[1] * v.p[3];
    r[2] = kZero;  // vmove.z vf, vf0
    r[3] = acc[3] + vf28[3] * v.p[3];
    memcpy(v.p, r, 16);
    store_vert(t4 + 48 * k, v);
    if (k == 0) {
      first = v;
    }
  }
  store_vert(t4 + 48 * N, first);
  return draw_large_polygon((u32)args.a[1], N, t4, t5, args.st);
}

u64 render_sky_quad_impl(const NativeArgs& args) {
  return render_sky_poly<4>(args);
}

u64 render_sky_tri_impl(const NativeArgs& args) {
  return render_sky_poly<3>(args);
}

//! store 4 floats into a vf register of sky_regs_vfs
inline void set_vf(int i, const void* v) {
  memcpy(sky_regs_vfs.vfs[i].f, v, 16);
}

/*!
 * (method 32 sky-work) stars-transform-asm: 128 times, the byte sum of three of the 8 quadwords at
 * 816 (a lagged generator) is stored over a fourth; its 16 bytes become 4 vectors (byte << 24,
 * vitof15) transformed by the matrix at 1488 and mirrored to y >= 0 (by the sign bit of y), stored
 * at 1888. Then vector-normalize! of the 512 vectors to 50000.0. sky_regs_vfs gets vf29-vf31 and
 * the last vf15-vf18, as the mips2c version leaves them.
 */
u64 method_32_sky_work_impl(const NativeArgs& args) {
  static const u32 normalize_sym = sym_value_addr("vector-normalize!");
  const u32 gp = (u32)args.a[0];
  float m31[4], m30[4], m29[4];
  gload_q(m31, gp + 1488);
  gload_q(m30, gp + 1504);
  gload_q(m29, gp + 1520);
  float r[4][4];  // vf15, vf16, vf17, vf18 of the last iteration
  u32 a1 = 0;
  u32 out = gp;
  for (int it = 0; it < 128; it++) {
    u8 q0[16], q1[16], q2[16];
    gload_q(q0, (gp + 816 + a1 * 16) & ~15u);
    gload_q(q1, (gp + 816 + ((a1 + 3) & 7) * 16) & ~15u);
    gload_q(q2, (gp + 816 + ((a1 + 5) & 7) * 16) & ~15u);
    const u32 dst = (gp + 816 + ((a1 + 7) & 7) * 16) & ~15u;
    a1 = (a1 + 1) & 7;
    u8 s[16];
    for (int b = 0; b < 16; b++) {
      s[b] = (u8)((u8)(q0[b] + q1[b]) + q2[b]);
    }
    gstore_bytes(dst, s, 16);
    // vf15: bytes 4-7, vf16: 0-3, vf17: 12-15, vf18: 8-11
    static constexpr int kBase[4] = {4, 0, 12, 8};
    for (int g = 0; g < 4; g++) {
      float x[4];
      for (int j = 0; j < 4; j++) {
        x[j] = ((float)(s32)((u32)s[kBase[g] + j] << 24)) * (1.f / 32768.f);
      }
      float acc[4];
      for (int i = 0; i < 4; i++) {
        acc[i] = m31[i] * x[0];
      }
      for (int i = 0; i < 4; i++) {
        acc[i] += m30[i] * x[1];
      }
      float* v = r[g];
      for (int i = 0; i < 4; i++) {
        v[i] = acc[i] + m29[i] * x[2];
      }
      // bgez on the low 64 bits: the sign bit of y
      if (f2u(v[1]) & 0x80000000u) {
        v[0] = kZero - v[0];
        v[1] = kZero - v[1];
        v[2] = kZero - v[2];
      }
    }
    gstore_q(out + 1888, r[0]);
    gstore_q(out + 1904, r[1]);
    gstore_q(out + 1920, r[2]);
    gstore_q(out + 1936, r[3]);
    out += 64;
  }
  // (AI-assisted) vector-normalize! inlined (native_goalc_ops.h: goalc's C of the GOAL function, the
  // same bits) while the symbol holds the function it held at the first call; otherwise the calls
  static const u32 normalize_fn = gload<u32>(normalize_sym);
  if (gload<u32>(normalize_sym) == normalize_fn) {
    for (u32 k = 0; k < 512; k++) {
      const u32 v = gp + 1888 + k * 16;
      float x[4];
      gload_q(x, v);
      goalc_ops::vector_normalize(x, 50000.f);
      gstore<float>(v, x[0]);
      gstore<float>(v + 4, x[1]);
      gstore<float>(v + 8, x[2]);
    }
  } else {
    for (u32 k = 0; k < 512; k++) {
      const u64 call_args[8] = {gp + 1888 + k * 16, 0x47435000, 0, 0, 0, 0, 0, 0};  // 50000.0
      native_call_goal(gload<u32>(normalize_sym), call_args, args);
    }
  }
  set_vf(31, m31);
  set_vf(30, m30);
  set_vf(29, m29);
  set_vf(15, r[0]);
  set_vf(16, r[1]);
  set_vf(17, r[2]);
  set_vf(18, r[3]);
  return 0;
}

/*!
 * (method 33 sky-work) stars-dma (sky-work, dma-buffer): with the star texture (id 0x47500500),
 * the GIF data at 0/16 and the adgif shader (72 in its last word), then for each of the 512 stars
 * at 1888: transformed by vf28-vf31, projected (vf14, Q), offset by vf25, added to the corners at
 * 384/400 (vftoi4, stored at 1552/1568), and those inside the screen window go out as 7 quadwords
 * (vf8, vf11, the color at 528 + 16 * (i & 15), vf2, corner 1, vf5, corner 2, z = 0).
 * sky_regs_vfs gets vf1-vf5, vf8, vf11, vf15, vf16 as the mips2c version leaves them.
 */
u64 method_33_sky_work_impl(const NativeArgs& args) {
  static const u32 lookup_sym = sym_value_addr("lookup-texture-by-id-fast");
  static const u32 adgif_sym = sym_value_addr("adgif-shader<-texture-simple!");
  const u32 s5 = (u32)args.a[0];
  const u32 gp = (u32)args.a[1];
  gstore<u32>(s5 + 1868, gp);
  u64 call_args[8] = {0x47500500, args.a[1], args.a[2], args.a[3],
                      args.a[4],  args.a[5], args.a[6], args.a[7]};
  const u64 tex = native_call_goal(gload<u32>(lookup_sym), call_args, args);
  if (tex == args.st) {
    return 0;
  }
  {
    const u32 base = gload<u32>(gload<u32>(s5 + 1868) + 4);
    gcopy_q(base & ~15u, s5 & ~15u);
    gcopy_q((base + 16) & ~15u, (s5 + 16) & ~15u);
    const u32 s4 = base + 32;
    call_args[0] = s4;
    call_args[1] = tex;
    call_args[3] = args.st;  // a3 = #f (delay slot of the texture check)
    native_call_goal(gload<u32>(adgif_sym), call_args, args);
    gstore<u64>(s4 + 64, 72);
    const u32 buf = gload<u32>(s5 + 1868);
    const u32 next = gload<u32>(buf + 4) + 112;
    gstore<u32>(gload<u32>(s5 + 1868) + 4, next);
  }
  u32 a3 = gload<u32>(gp + 4);
  float vf2[4], vf5[4], vf8[4], vf11[4];
  gload_q(vf2, s5 + 784);
  gload_q(vf5, s5 + 800);
  gload_q(vf8, s5 + 32);
  gload_q(vf11, s5 + 48);
  float c1[4], c4[4];  // the corners at 384 / 400 (the same every iteration)
  gload_q(c1, s5 + 384);
  gload_q(c4, s5 + 400);
  const float* vf14 = vf(14);
  const float* vf25 = vf(25);
  const float* vf28 = vf(28);
  const float* vf29 = vf(29);
  const float* vf30 = vf(30);
  const float* vf31 = vf(31);
  float v15[4], v16[4], v3[4];
  s32 i1[4], i4[4];
  s32 w1[4], w4[4];  // the corners as stored at 1552 / 1568 (before z = 0)
  u32 a1 = 0;
  for (u32 k = 0; k < 512; k++) {
    gload_q(v15, s5 + 1888 + k * 16);
    float acc[4];
    for (int i = 0; i < 4; i++) {
      acc[i] = vf31[i] * v15[0];
    }
    for (int i = 0; i < 4; i++) {
      acc[i] += vf30[i] * v15[1];
    }
    for (int i = 0; i < 4; i++) {
      acc[i] += vf29[i] * v15[2];
    }
    for (int i = 0; i < 4; i++) {
      v15[i] = acc[i] + vf28[i] * kOne;  // vmaddw.xyzw vf15, vf28, vf0
    }
    for (int i = 0; i < 4; i++) {
      v16[i] = v15[i] * vf14[i];
    }
    gload_q(v3, s5 + 528 + a1);
    const float q = kOne / v16[3];
    a1 = (a1 + 16) & 255;
    for (int i = 0; i < 3; i++) {
      v15[i] = v15[i] * q;
    }
    for (int i = 0; i < 4; i++) {
      v15[i] = v15[i] + vf25[i];
    }
    float p1[4], p4[4];
    for (int i = 0; i < 3; i++) {
      p1[i] = c1[i] + v15[i];
      p4[i] = c4[i] + v15[i];
    }
    p1[3] = c1[3];
    p4[3] = c4[3];
    for (int i = 0; i < 4; i++) {
      i1[i] = (s32)(p1[i] * 16.f);
      i4[i] = (s32)(p4[i] * 16.f);
    }
    memcpy(w1, i1, 16);
    memcpy(w4, i4, 16);
    if (i1[0] < 36864 && i1[1] < 36096 && 28672 < i4[0] && 29440 < i4[1]) {
      i1[2] = 0;  // vsub.z vf1, vf0, vf0 (+0.0)
      i4[2] = 0;
      gstore_q(a3, vf8);
      gstore_q(a3 + 16, vf11);
      gstore_q(a3 + 32, v3);
      gstore_q(a3 + 48, vf2);
      gstore_q(a3 + 64, i1);
      gstore_q(a3 + 80, vf5);
      gstore_q(a3 + 96, i4);
      a3 += 112;
    }
  }
  // the mips2c version stores the corners at 1552 / 1568 every iteration: the last ones stay
  gstore_q(s5 + 1552, w1);
  gstore_q(s5 + 1568, w4);
  gstore<u32>(gp + 4, a3);
  set_vf(1, i1);
  set_vf(2, vf2);
  set_vf(3, v3);
  set_vf(4, i4);
  set_vf(5, vf5);
  set_vf(8, vf8);
  set_vf(11, vf11);
  set_vf(15, v15);
  set_vf(16, v16);
  return 0;
}

}  // namespace

// (AI-assisted) no GOAL calls, no scratch space
const NativeImpl render_sky_quad = MIPS2C_NATIVE_IMPL(render_sky_quad_impl, 0, 0);
const NativeImpl render_sky_tri = MIPS2C_NATIVE_IMPL(render_sky_tri_impl, 0, 0);
// (AI-assisted) call GOAL functions (vector-normalize!; the texture lookup and adgif setup)
const NativeImpl method_32_sky_work = MIPS2C_NATIVE_IMPL(method_32_sky_work_impl, NATIVE_CALLS_GOAL, 0);
const NativeImpl method_33_sky_work = MIPS2C_NATIVE_IMPL(method_33_sky_work_impl, NATIVE_CALLS_GOAL, 0);

}  // namespace Mips2C::jak2::native2
