/*!
 * @file native_collide_cache.cpp
 * (AI-assisted)
 * Native versions of Jak 2's mips2c functions in collide_cache.cpp: methods 9 and 10 of
 * collide-cache-prim and of collide-puss-work. Ported from Jak 1's
 * (jak1_functions/native_collide_cache.cpp); what Jak 2's code does differently is noted at each.
 * moving-sphere-sphere-intersect and closest-pt-in-triangle are GOAL functions in Jak 2: called
 * through their symbols like the mips2c code does. See game/mips2c/mips2c_native.h for the rules.
 * The collide-puss-work methods are not registered: no Jak 2 GOAL code calls them, so verify mode
 * never checked them.
 */

#include <algorithm>
#include <cmath>

#include "game/mips2c/jak2_functions/native_functions.h"

namespace Mips2C::jak2::native {

namespace {

constexpr u32 kMiss = 0xccbebc20;  // -100000000.0

//! out.xyz = (acc = a.yzx * b.zxy) - b.yzx * a.zxy: vopmula acc, a, b then vopmsub out, b, a.
//! out.w is not written.
inline void opm(float out[4], const float a[4], const float b[4]) {
  const float a0 = a[1] * b[2];
  const float a1 = a[2] * b[0];
  const float a2 = a[0] * b[1];
  const float n0 = a0 - b[1] * a[2];
  const float n1 = a1 - b[2] * a[0];
  const float n2 = a2 - b[0] * a[1];
  out[0] = n0;
  out[1] = n1;
  out[2] = n2;
}

inline bool sign_bit(float f) {
  return (s32)f2u(f) < 0;
}

//! vrsqrt Q, vf0.w, x
inline float rsqrt_q(float x) {
  const float s = std::sqrt(std::abs(x));
  return s == 0 ? 0.f : 1.f / s;
}

/*!
 * (method 9 collide-cache-prim) = resolve-moving-sphere-tri
 * (this result sphere move max-t pat-mask?) -> float
 * Sweeps the sphere (radius in w) along move against this prim's triangles in the collide cache
 * and keeps the nearest hit (under max-t, 2.0 if max-t is negative) in result: the triangle's
 * vertices, the touching point, the normal and the pat (Jak 2: and the triangle's word at 52, at
 * result + 84). With bit 0 of the 5th argument, only triangles facing against the move and with
 * the sphere in front of them count. Returns the hit's t, or -100000000.0 if nothing was hit.
 */
u64 method_9_collide_cache_prim_impl(const NativeArgs& args) {
  const u32 prim = (u32)args.a[0];
  const u32 result = (u32)args.a[1];
  const u32 sphere = (u32)args.a[2];
  const u32 move = (u32)args.a[3];
  float best = u2f((u32)args.a[4]);
  const u32 flags = (u32)args.a[5];
  // mips2c-style scratch for the two vectors moving-sphere-triangle-intersect writes
  const u32 out_point = args.stack - 32;
  const u32 out_normal = args.stack - 16;

  if (!(0.f <= best)) {
    best = 2.f;
  }
  const float start = best;
  u32 tri = gload<u32>(prim + 32) + 4908 + ((u32)gload<u16>(prim + 40) << 6);
  static const u32 mst_sym = sym_val_addr("moving-sphere-triangle-intersect");
  static const u32* mst_stub = native_stub_slot("moving-sphere-triangle-intersect");

  for (u32 i = gload<u16>(prim + 42); i != 0; i--, tri += 64) {
    const u64 call_args[8] = {sphere,    move,      f2gpr(gload<float>(sphere + 12)),
                              tri,       out_point, out_normal,
                              args.a[6], args.a[7]};
    const u32 fn = gload<u32>(mst_sym);
    const float t = u2f((u32)(fn == *mst_stub ? moving_sphere_triangle_intersect_impl(NativeArgs{
                                                    call_args, args.pp, args.st, out_point})
                                              : native_call_goal(fn, call_args, args)));
    float mv[4], normal[4];
    gload_q(mv, move);
    gload_q(normal, out_normal);
    if (t < 0.f) {
      continue;
    }
    float point[4];
    gload_q(point, out_point);
    if (best <= t) {
      continue;
    }
    if (flags & 1) {
      // moving against the triangle, and the sphere in front of it
      float c[4], m[4], d[4], s[4];
      gload_q(c, sphere);
      for (int k = 0; k < 4; k++) {
        m[k] = mv[k] * normal[k];
        d[k] = c[k] - point[k];
      }
      m[0] = m[0] + m[1];
      for (int k = 0; k < 4; k++) {
        s[k] = d[k] * normal[k];
      }
      m[0] = m[0] + m[2];
      s[0] = s[0] + s[1];
      if (0.f <= m[0]) {
        continue;
      }
      s[0] = s[0] + s[2];
      if (s[0] < 0.f) {
        continue;
      }
    }
    u32 verts[3][4];
    for (int k = 0; k < 3; k++) {
      gload_q(verts[k], tri + 16 * k);
    }
    const u32 pat = gload<u32>(tri + 48);
    const u32 w52 = gload<u32>(tri + 52);
    best = t;
    gstore_q(result + 48, point);
    gstore_q(result + 64, normal);
    for (int k = 0; k < 3; k++) {
      gstore_q(result + 16 * k, verts[k]);
    }
    gstore<u32>(result + 80, pat);
    gstore<u32>(result + 84, w52);
  }
  return f2gpr(best == start ? u2f(kMiss) : best);
}

/*!
 * (method 10 collide-cache-prim) = resolve-moving-sphere-sphere
 * (this result sphere move max-t action) -> float
 * moving-sphere-sphere-intersect against this prim's sphere. On a hit (under max-t if max-t isn't
 * negative, and moving against the sphere with bit 0 of action) writes a triangle tangent to the
 * sphere at the touching point to result, with the normal, the pat (Jak 2: the word at 60 of the
 * prim's prim-core pointer at 36) and that pointer (result + 84). Returns t or -100000000.0.
 * (Jak 1's version tested the sphere pointer instead of action with a negative max-t; Jak 2's
 * tests action always.)
 */
u64 method_10_collide_cache_prim_impl(const NativeArgs& args) {
  static const u32 mssi_sym = sym_val_addr("moving-sphere-sphere-intersect");
  const u32 prim = (u32)args.a[0];
  const u32 result = (u32)args.a[1];
  const u32 out_point = args.stack - 16;
  const u64 call_args[8] = {args.a[2], args.a[3], args.a[0], out_point,
                            args.a[4], args.a[5], args.a[6], args.a[7]};
  const float t = u2f((u32)native_call_goal(gload<u32>(mssi_sym), call_args, args));
  const float max_t = u2f((u32)args.a[4]);
  float pt[4], center[4];
  gload_q(pt, out_point);
  gload_q(center, prim);
  if (t < 0.f) {
    return f2gpr(t);
  }
  float d[4] = {pt[0] - center[0], pt[1] - center[1], pt[2] - center[2], 1.f};
  if (!(max_t < 0.f) && max_t <= t) {
    return f2gpr(-100000000.f);
  }
  if (args.a[5] & 1) {
    float mv[4], p[4];
    gload_q(mv, (u32)args.a[3]);
    for (int i = 0; i < 4; i++) {
      p[i] = mv[i] * d[i];
    }
    p[1] = p[1] + p[0];
    p[1] = p[1] + p[2];
    if (!sign_bit(p[1])) {
      return f2gpr(-100000000.f);  // moving away
    }
  }

  // the normal, and a triangle tangent to the sphere around the touching point
  float sq[4];
  for (int i = 0; i < 4; i++) {
    sq[i] = d[i] * d[i];
  }
  gstore_q(result + 48, pt);
  float len2 = 1.f * sq[0];
  len2 += 1.f * sq[1];
  len2 = len2 + 1.f * sq[2];
  const float q = rsqrt_q(len2);
  const u32 core = gload<u32>(prim + 36);
  const u32 pat = gload<u32>(core + 60);
  for (int i = 0; i < 3; i++) {
    d[i] = d[i] * q;
  }
  const float sq14[2] = {d[0] * d[0], d[1] * d[1]};
  gstore_q(result + 64, d);
  gstore<u32>(result + 80, pat);
  gstore<u32>(result + 84, core);
  float t2[4] = {0.f, 0.f, 0.f, 1.f};
  const float s14 = sq14[0] + sq14[1];
  if ((f2u(std::abs(d[0])) | f2u(std::abs(d[1]))) == 0) {
    t2[0] = 0.f + d[2];
  } else {
    t2[0] = 0.f - d[1];
    const float q2 = rsqrt_q(s14);
    t2[1] = 0.f + d[0];
    t2[0] = t2[0] * q2;
    t2[1] = t2[1] * q2;
  }
  float t3[4];
  opm(t3, d, t2);
  static const u32 corners[3][4] = {{0, 0x45800000, 0, 0x3f800000},
                                    {0, 0xc5800000, 0x45800000, 0x3f800000},
                                    {0, 0xc5800000, 0xc5800000, 0x3f800000}};
  for (int k = 0; k < 3; k++) {
    float c[4];
    memcpy(c, corners[k], 16);
    float out[4];
    for (int i = 0; i < 3; i++) {
      float acc = pt[i] * 1.f;
      acc += d[i] * c[0];
      acc += t2[i] * c[1];
      out[i] = acc + t3[i] * c[2];
    }
    out[3] = c[3];
    gstore_q(result + 16 * k, out);
  }
  return f2gpr(t);
}

/*!
 * (method 10 collide-puss-work) (this sphere params) -> symbol
 * Does the sphere overlap one of the params' spheres (in the work's sphere list)? Tests the
 * bounding boxes of all of them first. (Jak 2: the sphere count is at params + 116.)
 */
u64 method_10_collide_puss_work_impl(const NativeArgs& args) {
  const u32 work = (u32)args.a[0];
  const u32 num = gload<u32>((u32)args.a[2] + 116);
  float s[4];
  gload_q(s, (u32)args.a[1]);
  if (num == 0) {
    return args.st;
  }
  s32 bmin[4], bmax[4], mn[3], mx[3];
  gload_q(bmin, (work + 64) & ~15u);
  gload_q(bmax, (work + 80) & ~15u);
  for (int i = 0; i < 3; i++) {
    mn[i] = (s32)(s[i] - s[3]);
    mx[i] = (s32)(s[i] + s[3]);
  }
  bool outside = false;
  for (int i = 0; i < 3; i++) {
    outside |= (bmin[i] > mx[i]) | (mn[i] > bmax[i]);
  }
  if (outside) {
    return args.st;
  }
  s[3] = 1.f - s[3];
  u32 sphere = work + 96;
  for (u32 k = 0; k != num; k++, sphere += 48) {
    float p[4];
    gload_q(p, sphere);
    float d[4];
    for (int i = 0; i < 4; i++) {
      d[i] = p[i] - s[i];
    }
    for (int i = 0; i < 4; i++) {
      d[i] = d[i] * d[i];
    }
    float acc = 1.f * d[0];
    acc += 1.f * d[1];
    acc += 1.f * d[2];
    const float r = acc - 1.f * d[3];
    // blez of lanes x and y (both r) as a 64-bit int
    const u64 v = ((u64)f2u(r) << 32) | f2u(r);
    if ((s64)v <= 0) {
      return goal_true(args.st);
    }
  }
  return args.st;
}

/*!
 * (method 9 collide-puss-work) (this prim params) -> symbol
 * Does one of the params' spheres (in the work's list) touch one of this prim's triangles?
 * Stores each triangle's int bounding box and (for those that overlap the spheres' box) its
 * normal in the work, and calls closest-pt-in-triangle for the spheres whose boxes overlap it.
 * (Jak 2: the sphere count is at params + 116.)
 */
u64 method_9_collide_puss_work_impl(const NativeArgs& args) {
  static const u32 closest_sym = sym_val_addr("closest-pt-in-triangle");
  const u32 work = (u32)args.a[0];
  const u32 prim = (u32)args.a[1];
  u32 tri = gload<u32>(prim + 32) + 4908 + ((u32)gload<u16>(prim + 40) << 6);
  u32 ntri = gload<u16>(prim + 42);
  for (; ntri != 0; ntri--, tri += 64) {
    float v0[4], v1[4], v2[4];
    gload_q(v0, tri);
    gload_q(v1, tri + 16);
    gload_q(v2, tri + 32);
    float e4[4], e5[4];
    for (int i = 0; i < 4; i++) {
      e4[i] = v1[i] - v0[i];
      e5[i] = v2[i] - v0[i];
    }
    s32 bmin[4], bmax[4];
    gload_q(bmin, (work + 64) & ~15u);
    gload_q(bmax, (work + 80) & ~15u);
    s32 tmin[4], tmax[4];
    for (int i = 0; i < 4; i++) {
      tmin[i] = (s32)std::min(std::min(v0[i], v1[i]), v2[i]);
      tmax[i] = (s32)std::max(std::max(v0[i], v1[i]), v2[i]);
    }
    float nrm[4];
    opm(nrm, e4, e5);
    nrm[3] = 1.f;
    float sq[4];
    for (int i = 0; i < 4; i++) {
      sq[i] = nrm[i] * nrm[i];
    }
    gstore_q(work + 32, tmin);
    float len2 = 1.f * sq[0];
    len2 += 1.f * sq[1];
    gstore_q(work + 48, tmax);
    len2 = len2 + 1.f * sq[2];
    bool outside = false;
    for (int i = 0; i < 3; i++) {
      outside |= (bmin[i] > tmax[i]) | (tmin[i] > bmax[i]);
    }
    const float q = rsqrt_q(len2);
    if (outside) {
      continue;
    }
    for (int i = 0; i < 3; i++) {
      nrm[i] = nrm[i] * q;
    }
    const u32 num_spheres = gload<u32>((u32)args.a[2] + 116);
    gstore_q(work + 16, nrm);

    u32 sphere = work + 96;
    for (u32 k = 0; k != num_spheres; k++, sphere += 48) {
      s32 smin[4], smax[4], wmin[4], wmax[4];
      gload_q(smin, (sphere + 16) & ~15u);
      gload_q(wmax, (work + 48) & ~15u);
      gload_q(smax, (sphere + 32) & ~15u);
      gload_q(wmin, (work + 32) & ~15u);
      bool out = false;
      for (int i = 0; i < 3; i++) {
        out |= (smin[i] > wmax[i]) | (wmin[i] > smax[i]);
      }
      if (out) {
        continue;
      }
      const u64 call_args[8] = {work,      sphere,    tri,       work + 16u,
                                args.a[4], args.a[5], args.a[6], args.a[7]};
      native_call_goal(gload<u32>(closest_sym), call_args, args);
      float pt[4], sp[4];
      gload_q(pt, work);
      gload_q(sp, sphere);
      float d[4];
      for (int i = 0; i < 3; i++) {
        d[i] = pt[i] - sp[i];
      }
      const float r2 = sp[3] * sp[3];
      for (int i = 0; i < 3; i++) {
        d[i] = d[i] * d[i];
      }
      float acc = 1.f * d[0];
      acc += 1.f * d[1];
      float dist = acc + 1.f * d[2];
      dist = dist - r2;
      // pcpyud: lanes z and w as a 64-bit int
      const u64 v = ((u64)f2u(dist) << 32) | f2u(d[2]);
      if (!((s64)v > 0)) {
        return goal_true(args.st);
      }
    }
  }
  return args.st;
}

}  // namespace

// scratch: the two vectors, then moving-sphere-triangle-intersect's (called directly)
const NativeImpl method_9_collide_cache_prim =
    MIPS2C_NATIVE_IMPL(method_9_collide_cache_prim_impl, 0, 48);
// scratch: the point moving-sphere-sphere-intersect writes
const NativeImpl method_10_collide_cache_prim =
    MIPS2C_NATIVE_IMPL(method_10_collide_cache_prim_impl, 0, 16);
const NativeImpl method_9_collide_puss_work =
    MIPS2C_NATIVE_IMPL(method_9_collide_puss_work_impl, 0, 0);
const NativeImpl method_10_collide_puss_work =
    MIPS2C_NATIVE_IMPL(method_10_collide_puss_work_impl, 0, 0);

}  // namespace Mips2C::jak2::native
