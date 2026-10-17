/*!
 * @file native_ocean.cpp
 * (AI-assisted)
 * Native versions of the mips2c functions in ocean_vu0.cpp that the game logic needs. See
 * game/mips2c/mips2c_native.h for the rules (same float operations, grouped the same way, as the
 * mips2c code).
 */

#include <algorithm>

#include "game/kernel/jak1/kscheme.h"
#include "game/mips2c/jak1_functions/native_functions.h"

namespace Mips2C::jak1::native {

namespace {

u32 sym_addr(const char* name) {
  return ::jak1::intern_from_c(name).offset;
}

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
 * (ocean-interp-wave dest frame): the ocean's wave heights for this frame (32x32 floats at dest),
 * between two of the 64 frames of *ocean-wave-frames* (1024 s8 heights each): frame / 32 and the
 * next one, weighted by frame % 32. The two weights (times 0.333) are also left in *ocean-work* at
 * 60 and 64. Called every frame: ocean-get-height (boats, floating things, Jak in the water) reads
 * the result.
 */
u64 ocean_interp_wave_impl(const NativeArgs& args) {
  static const u32 frames_sym = sym_addr("*ocean-wave-frames*");
  static const u32 work_sym = sym_addr("*ocean-work*");
  static const HeightFloats heights;

  const u32 dest = (u32)args.a[0];
  const u64 frame = args.a[1];
  const s64 i = (s64)frame >> 5;
  const u8* frame_a = gptr(gload<u32>(frames_sym) + (u32)((i & 63) << 10));
  const u8* frame_b = gptr(gload<u32>(frames_sym) + (u32)(((i + 1) & 63) << 10));

  const u32 work = gload<u32>(work_sym);
  gstore<float>(work + 64, 0.03125f * (float)(s32)(frame & 31));
  gstore<float>(work + 60, 1.f - gload<float>(work + 64));
  gstore<float>(work + 60, (float)0.333 * gload<float>(work + 60));
  gstore<float>(work + 64, (float)0.333 * gload<float>(work + 64));
  const float weight_a = gload<float>(work + 60);
  const float weight_b = gload<float>(work + 64);

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

/*!
 * (ocean-generate-verts dest heights): the ocean texture's vertices (*ocean-verts*, ocean.gc
 * draw-ocean) from this frame's 32x32 wave heights: per row of heights 8 groups of 4 vertices
 * (vcallms 0, ocean-vu0-block), each stored as 8 quadwords: color 0, the 4 normals' x, color 1,
 * y, color 2, z, color 3, w (the heights). The normal from the height differences (scales x, z of
 * *ocean-vu0-work*), lit by *time-of-day-context*'s lights (vu-lights<-light-group!, colors
 * scaled by *ocean-generate-verts-vector*), color w = 1 / (normal z - 1). Only with config.ini
 * ocean = full on the 3DS (draw-ocean).
 * (AI-assisted) The mips2c version emulates the VU0 macro mode in memory (about 1M ARM
 * instructions per call); this does each vertex's lanes as plain floats, four vertices side by
 * side (independent chains for the VFP pipeline), with the same operations grouped the same way.
 */
u64 ocean_generate_verts_impl(const NativeArgs& args) {
  static const u32 work_sym = sym_addr("*ocean-vu0-work*");
  static const u32 tod_sym = sym_addr("*time-of-day-context*");
  static const u32 block_sym = sym_addr("ocean-vu0-block");
  static const u32 upload_sym = sym_addr("upload-vu0-program");
  static const u32 lights_sym = sym_addr("vu-lights<-light-group!");
  static const u32 vmul_sym = sym_addr("vector*!");
  static const u32 scale_sym = sym_addr("*ocean-generate-verts-vector*");

  u32 dest = (u32)args.a[0];
  const u32 heights = (u32)args.a[1];
  {
    const u32 work = gload<u32>(work_sym);
    const u64 a[8] = {gload<u32>(block_sym), work + 160, args.a[2], args.a[3],
                      args.a[4],             args.a[5],  args.a[6], args.a[7]};
    native_call_goal(gload<u32>(upload_sym), a, args);
  }
  {
    const u64 a[8] = {gload<u32>(work_sym) + 48, gload<u32>(tod_sym) + 156, args.a[2], args.a[3],
                      args.a[4],                 args.a[5],                 args.a[6], args.a[7]};
    native_call_goal(gload<u32>(lights_sym), a, args);
  }
  for (u32 off = 96; off <= 144; off += 16) {
    const u32 v = gload<u32>(work_sym) + off;
    const u64 a[8] = {v, v, gload<u32>(scale_sym), args.a[3], args.a[4], args.a[5], args.a[6],
                      args.a[7]};
    native_call_goal(gload<u32>(vmul_sym), a, args);
  }

  const u32 work = gload<u32>(work_sym);
  float k[4], l0[4], l1[4], l2[4], c0[4], c1[4], c2[4], amb[4];
  gload_q(k, work);  // vf12
  gload_q(l0, work + 48);
  gload_q(l1, work + 64);
  gload_q(l2, work + 80);
  gload_q(c0, work + 96);
  gload_q(c1, work + 112);
  gload_q(c2, work + 128);
  gload_q(amb, work + 144);
  // (acc = vf01 * vf11: the ambient times 1.0)
  float amb1[3];
  for (int i = 0; i < 3; i++) {
    amb1[i] = 1.f * amb[i];
  }

  for (u32 r = 0; r < 32; r++) {
    const u32 row = heights + 128 * r;
    const u32 next = heights + 128 * ((r + 1) & 31);
    for (u32 g = 0; g < 8; g++) {
      // vf02: 4 heights of the row, vf03: the 4 after them (the row's first 4 for the last
      // group), vf04: the next row's 4 under vf02 (only x is read)
      float a[4], b[4], c[4];
      gload_q(a, row + 16 * g);
      gload_q(b, row + 16 * ((g + 1) & 7));
      gload_q(c, next + 16 * g);
      float nx[4], ny[4], nz[4], nw[4], col[4][4];
      for (int v = 0; v < 4; v++) {
        // mula.x acc, vf12, vf02.(v + 1) / vf04.x; mula.z acc, vf12, vf03.v;
        // msub.xz vf24+v, vf12, vf02.v
        const float accx = k[0] * (v < 3 ? a[v + 1] : c[0]);
        const float accz = k[2] * b[v];
        nx[v] = accx - k[0] * a[v];
        nz[v] = accz - k[2] * a[v];
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
        // the light directions' dot products: acc = vf05 * x + vf06 * y, + vf07 * z (before the
        // subw.z below), then clamped to 0 (maxx with vf00.x)
        for (int i = 0; i < 3; i++) {
          float acc = l0[i] * nx[v];
          acc += l1[i] * ny[v];
          d[v][i] = vu_max(acc + l2[i] * nz[v], 0.f);
        }
        nz[v] = nz[v] - 1.f;
      }
      for (int v = 0; v < 4; v++) {
        col[v][3] = 1.f * (1.f / nz[v]);  // div Q, vf00.w, vf2x.z; mulw.w vf2x, vf00, Q
      }
      for (int v = 0; v < 4; v++) {
        // acc = vf01 * vf11, + vf08 * d.x + vf09 * d.y, + vf10 * d.z, then at most vf12.y
        for (int i = 0; i < 3; i++) {
          float acc = amb1[i];
          acc += c0[i] * d[v][0];
          acc += c1[i] * d[v][1];
          col[v][i] = vu_min(acc + c2[i] * d[v][2], k[1]);
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
// render-ocean-quad (ocean.cpp), with draw-large-polygon-ocean and the polygon clippers
// (clip-polygon-against-positive/negative-hyperplane, sky_tng.cpp)
// ---------------------------------------------------------------------------------------------

//! a polygon vertex as the mips2c code keeps it in the scratchpad: position, stq, color
struct PolyVert {
  float p[4], st[4], c[4];
};
static_assert(sizeof(PolyVert) == 48);

inline void store_vert(u32 addr, const PolyVert& v) {
  gstore_q(addr, v.p);
  gstore_q(addr + 16, v.st);
  gstore_q(addr + 32, v.c);
}

/*!
 * clip-polygon-against-positive-hyperplane (neg false: inside when !(w < x)) or -negative-
 * (neg true: inside when !(x < -w)), x = component comp of the position: the polygon in[0..n]
 * (in[n] = in[0]) to out[0..count] (out[count] = out[0]), stored at out_addr like the mips2c
 * version (48 bytes per vertex). Sutherland-Hodgman: an inside vertex is kept when its edge
 * is processed; an edge crossing the plane adds prev + (next - prev) * fp / (fp - fn) (outside
 * to inside) or next + (prev - next) * fn / (fn - fp) (inside to outside), f = a - x with
 * a = w or -w (the mips2c's sub.s, div.s and vmaddx). *clipped: set when an edge crossed.
 */
int clip_polygon(const PolyVert* in, int n, PolyVert* out, u32 out_addr, int comp, bool neg,
                 bool* clipped) {
  int count = 0;
  auto a_of = [neg](const PolyVert& v) {
    float w = v.p[3];
    if (neg) {
      u32 u;
      memcpy(&u, &w, 4);
      u ^= 0x80000000u;  // neg.s
      memcpy(&w, &u, 4);
    }
    return w;
  };
  auto outside = [neg](float a, float x) { return neg ? x < a : a < x; };
  auto emit = [&](const PolyVert& v) {
    out[count] = v;
    store_vert(out_addr + 48 * count, v);
    count++;
  };
  // from + (to - from) * t, the three quadwords
  auto lerp = [&](const PolyVert& from, const PolyVert& to, float t) {
    PolyVert r;
    for (int i = 0; i < 4; i++) {
      const float d = to.p[i] - from.p[i];
      const float acc = from.p[i] * 1.f;  // (vmulaw, then vmaddx: one expression)
      r.p[i] = acc + d * t;
    }
    for (int i = 0; i < 4; i++) {
      const float d = to.st[i] - from.st[i];
      const float acc = from.st[i] * 1.f;  // (vmulaw, then vmaddx: one expression)
      r.st[i] = acc + d * t;
    }
    for (int i = 0; i < 4; i++) {
      const float d = to.c[i] - from.c[i];
      const float acc = from.c[i] * 1.f;  // (vmulaw, then vmaddx: one expression)
      r.c[i] = acc + d * t;
    }
    return r;
  };
  const PolyVert* prev = &in[0];
  float pa = a_of(*prev), px = prev->p[comp];
  bool prev_out = outside(pa, px);
  for (int e = 1; e <= n; e++) {
    const PolyVert* next = &in[e];
    const float na = a_of(*next), nx = next->p[comp];
    const bool next_out = outside(na, nx);
    if (!prev_out) {
      emit(*prev);
      if (next_out) {
        const float fp = pa - px, fn = na - nx;
        const float den = fn - fp;
        emit(lerp(*next, *prev, fn / den));
        *clipped = true;
      }
    } else if (!next_out) {
      const float fp = pa - px, fn = na - nx;
      const float den = fp - fn;
      emit(lerp(*prev, *next, fp / den));
      *clipped = true;
    }
    prev = next;
    pa = na;
    px = nx;
    prev_out = next_out;
  }
  // the first vertex again at the end (when nothing is left: its slot onto itself)
  if (count > 0) {
    out[count] = out[0];
    store_vert(out_addr + 48 * count, out[0]);
  }
  return count;
}

/*!
 * (render-ocean-quad vertices dma-buf): one far ocean quad (4 ocean-vertex: position, stq,
 * color), transformed by the camera matrix of init-ocean-far-regs (vf28..vf31, kept in
 * ocean_regs_vfs), clipped in the scratchpad against x and y = +-w of the clip scaled position
 * (vf14), then as a GIF packet (vf27 with nloop = vertices) of ST, RGBAQ, XYZF2: the GS
 * position (* Q + vf25, z >= 0, the fog clamped to vf13.y..z), Q = vf13.x / w (not clipped:
 * the original positions) or 1 / w of the clip scaled one times vf26 (clipped; stq + vf24).
 * #t when something is drawn.
 */
u64 render_ocean_quad_impl(const NativeArgs& args) {
  static const u32 spad_sym = sym_addr("*fake-scratchpad-data*");
  const auto& vf = ocean_regs_vfs.vfs;
  const u32 src = (u32)args.a[0];
  const u32 dma = (u32)args.a[1];
  const u32 spad = gload<u32>(spad_sym);
  const u32 t4 = spad | 12288, t5 = spad | 14336;

  // the 4 positions transformed (vf15..vf18), the polygon of clip scaled ones (* vf14)
  float m[4][4], k14[4];
  for (int i = 0; i < 4; i++) {
    m[0][i] = vf[31].f[i];
    m[1][i] = vf[30].f[i];
    m[2][i] = vf[29].f[i];
    m[3][i] = vf[28].f[i];
    k14[i] = vf[14].f[i];
  }
  float pos[4][4];
  PolyVert poly_a[16], poly_b[16];
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
  // (the mips2c stores: stq and colors, then the positions)
  for (int v = 0; v < 5; v++) {
    gstore_q(t4 + 48 * v + 16, poly_a[v].st);
    gstore_q(t4 + 48 * v + 32, poly_a[v].c);
  }
  for (int v : {0, 4, 1, 2, 3}) {
    gstore_q(t4 + 48 * v, poly_a[v].p);
  }

  // draw-large-polygon-ocean
  bool clipped = false;
  int n = clip_polygon(poly_a, 4, poly_b, t5, 0, false, &clipped);
  if (!n) {
    return args.st;
  }
  n = clip_polygon(poly_b, n, poly_a, t4, 1, false, &clipped);
  if (!n) {
    return args.st;
  }
  n = clip_polygon(poly_a, n, poly_b, t5, 0, true, &clipped);
  if (!n) {
    return args.st;
  }
  n = clip_polygon(poly_b, n, poly_a, t4, 1, true, &clipped);
  if (!n) {
    return args.st;
  }

  u32 out = gload<u32>(dma + 4);
  gstore_q(out, vf[27].f);
  gstore<u32>(out, (u32)n);
  out += 16;
  const float fog_lo = vf[13].f[1], fog_hi = vf[13].f[2];
  if (!clipped) {
    // the original positions again (the 4 vertices and the first at the end)
    gstore_q(t4, pos[0]);
    gstore_q(t4 + 192, pos[0]);
    gstore_q(t4 + 48, pos[1]);
    gstore_q(t4 + 96, pos[2]);
    gstore_q(t4 + 144, pos[3]);
    for (int v = 0; v < 4; v++) {
      memcpy(poly_a[v].p, pos[v], 16);
    }
  }
  for (int v = 0; v < n; v++) {
    const PolyVert& q = poly_a[v];
    float p[4], st[4];
    s32 col[4];
    float qq;
    if (clipped) {
      qq = 1.f / q.p[3];
      for (int i = 0; i < 4; i++) {
        p[i] = q.p[i] * vf[26].f[i];
        st[i] = q.st[i] + vf[24].f[i];
      }
    } else {
      qq = vf[13].f[0] / q.p[3];
      for (int i = 0; i < 4; i++) {
        p[i] = q.p[i];
        st[i] = q.st[i];
      }
    }
    for (int i = 0; i < 4; i++) {
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
  return args.st + 8;
}

// ---------------------------------------------------------------------------------------------
// (AI-assisted) logic_micro: render-ocean-quad with the same results and scratchpad contents in
// fewer instructions (~140 calls per frame with ocean = full).
// - Half of the far quads are inside all four planes (host count, the 5 fps60 checkpoints with the
//   ocean): then every clip pass gives back its input (each vertex inside, no edge crosses), so
//   the passes are skipped and both scratchpad buffers get the copies those passes would store.
// - The other quads are clipped as before, but the passes don't store each vertex to the
//   scratchpad: poly_a / poly_b hold exactly what the stores leave in t4 / t5 (each store went to
//   the same slot of the array), so their written slots are stored once at the end.
// ---------------------------------------------------------------------------------------------

//! from + (to - from) * t into r, the three quadwords (lerp in clip_polygon)
inline void lerp_into(PolyVert& r, const PolyVert& from, const PolyVert& to, float t) {
  for (int i = 0; i < 4; i++) {
    const float d = to.p[i] - from.p[i];
    const float acc = from.p[i] * 1.f;  // (vmulaw, then vmaddx: one expression)
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
  u32 u;
  memcpy(&u, &w, 4);
  u ^= 0x80000000u;
  memcpy(&w, &u, 4);
  return w;
}

//! clip_polygon without the stores: out[0..count] is what clip_polygon stores at out_addr.
template <int comp, bool neg>
int clip_polygon_nostore(const PolyVert* in, int n, PolyVert* out, bool* clipped) {
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

u64 render_ocean_quad_micro(const NativeArgs& args) {
  static const u32 spad_sym = sym_addr("*fake-scratchpad-data*");
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
  PolyVert poly_a[16], poly_b[16];
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
  // slots of t4 (poly_a) and t5 (poly_b) the original stores wrote: t4 0..4 first
  int slots_a = 5, slots_b = 0;
  auto note = [](int& slots, int n) {
    if (n > 0) {
      slots = std::max(slots, n + 1);
    }
  };

  // every vertex inside x, y <= w and x, y >= -w (the passes' own tests): nothing is clipped
  bool inside = true;
  for (int v = 0; v < 4; v++) {
    const float* p = poly_a[v].p;
    const float w = p[3], nw = neg_bits(p[3]);
    inside &= !(w < p[0]) & !(w < p[1]) & !(p[0] < nw) & !(p[1] < nw);
  }

  bool clipped = false;
  int n = 4;
  if (inside) {
    // each pass gives back its input: t5 and t4 both end with poly_a's 5 vertices
    store_verts(t5, poly_a, 5);
  } else {
    // draw-large-polygon-ocean
    auto bail = [&]() {
      store_verts(t5, poly_b, slots_b);
      store_verts(t4, poly_a, slots_a);
      return args.st;
    };
    n = clip_polygon_nostore<0, false>(poly_a, 4, poly_b, &clipped);
    note(slots_b, n);
    if (!n) {
      return bail();
    }
    n = clip_polygon_nostore<1, false>(poly_b, n, poly_a, &clipped);
    note(slots_a, n);
    if (!n) {
      return bail();
    }
    n = clip_polygon_nostore<0, true>(poly_a, n, poly_b, &clipped);
    note(slots_b, n);
    if (!n) {
      return bail();
    }
    n = clip_polygon_nostore<1, true>(poly_b, n, poly_a, &clipped);
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
        st[i] = q.st[i] + vf[24].f[i];
      }
    } else {
      qq = vf[13].f[0] / q.p[3];
      for (int i = 0; i < 4; i++) {
        p[i] = q.p[i];
        st[i] = q.st[i];
      }
    }
    for (int i = 0; i < 4; i++) {
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
  return args.st + 8;
}

u64 render_ocean_quad_select(const NativeArgs& args) {
  static const bool micro = logic_micro_on();
  return micro ? render_ocean_quad_micro(args) : render_ocean_quad_impl(args);
}

}  // namespace

const NativeImpl ocean_interp_wave = MIPS2C_NATIVE_IMPL(ocean_interp_wave_impl, 0, 0);
const NativeImpl ocean_generate_verts =
    MIPS2C_NATIVE_IMPL(ocean_generate_verts_impl, NATIVE_CALLS_GOAL, 0);
const NativeImpl render_ocean_quad = MIPS2C_NATIVE_IMPL_GOAL(render_ocean_quad_select, 0);

}  // namespace Mips2C::jak1::native
