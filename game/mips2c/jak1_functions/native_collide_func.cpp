/*!
 * @file native_collide_func.cpp
 * (AI-assisted)
 * Native versions of the mips2c functions in collide_func.cpp, and of GOAL functions of
 * collide-func.gc they call for every triangle (ray-sphere-intersect, ray-cylinder-intersect,
 * moving-sphere-sphere-intersect: the same float operations as the C code goalc makes of them).
 * See game/mips2c/mips2c_native.h for the rules (same float operations, grouped the same way, as
 * the mips2c code).
 */

#include <algorithm>
#include <cmath>

#include "game/kernel/jak1/kscheme.h"
#include "game/mips2c/jak1_functions/native_functions.h"
#include "game/mips2c/mips2c_table.h"

namespace Mips2C::jak1::native {

namespace {

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

//! lanes x and y of a VU register as a 64-bit GPR value (qmfc2, then 64-bit integer ops)
inline u64 low64(const float v[4]) {
  return ((u64)f2u(v[1]) << 32) | f2u(v[0]);
}

constexpr u64 kMiss = 0xffffffffccbebc20;  // -100000000.0, sign-extended

u32 sym_addr(const char* name) {
  return ::jak1::intern_from_c(name).offset;
}

constexpr float kMissF = -100000000.f;

/*!
 * pc-port-raw-ray-sphere-implementation (the GOAL function as goalc compiles it): a sphere of
 * radius rad at the origin, the probe from o along u (lanes x, y, z). Returns the fraction of u to
 * the sphere, 0 if o is inside, -100000000.0 if behind, missed or too far. The checks only use
 * sign bits, so NaNs go where the GOAL code sends them.
 */
inline float raw_ray_sphere(float rad, const float o[4], const float u[4]) {
  const float uu0 = u[0] * u[0];
  const float uu1 = u[1] * u[1];
  const float uu2 = u[2] * u[2];
  const float rr = rad * rad;
  const float oo0 = o[0] * o[0];
  const float oo1 = o[1] * o[1];
  const float oo2 = o[2] * o[2];
  const float uo0 = u[0] * o[0];
  const float uo1 = u[1] * o[1];
  const float uo2 = u[2] * o[2];
  float a = uu0 + uu1;
  a = a + uu2;
  float c = oo1 + oo0;
  const float c2 = oo2 - rr;
  c = c + c2;
  float b = uo1 + uo0;
  b = b + uo2;
  if (sign_bit(c)) {
    return 0.f;  // inside
  }
  if (a == 0.f || !sign_bit(b)) {
    return kMissF;  // no direction, or moving away
  }
  const float bb = b * b;
  const float ca = c * a;
  const float disc = bb - ca;
  if (sign_bit(disc)) {
    return kMissF;
  }
  const float n = b + a;
  const float nn = n * n;
  const float fin = disc - nn;
  if (sign_bit(n) && sign_bit(fin)) {
    return kMissF;  // too far
  }
  const float q = 1.f / a;
  const float inv = 1.f * q;
  const float root = std::sqrt(disc);
  const float root1 = 1.f * root;
  float t = b + root1;
  t = t * inv;
  return 0.f - t;
}

/*!
 * ray-sphere-intersect with the vectors loaded: the fraction of dir from origin to the sphere,
 * 0 inside, -100000000.0 on a miss.
 */
inline __attribute__((always_inline)) float ray_sphere_intersect_v(const float origin[4],
                                                                   const float dir[4],
                                                                   const float center[4],
                                                                   float radius) {
  const float o[4] = {origin[0] - center[0], origin[1] - center[1], origin[2] - center[2], 0.f};
  return raw_ray_sphere(radius, o, dir);
}

/*!
 * ray-cylinder-intersect with the vectors loaded: a cylinder of radius rad from cyl_origin along
 * the unit vector axis, length len. On a hit returns the fraction of dir and stores the point on
 * the axis in pt_out (also on some misses, like the GOAL code: after the hit is found to be
 * on the cylinder's side of its start, before the check against its end).
 */
inline __attribute__((always_inline)) float ray_cylinder_intersect_v(const float origin[4],
                                                                     const float dir[4],
                                                                     const float cyl_origin[4],
                                                                     const float axis[4],
                                                                     float rad,
                                                                     float len,
                                                                     u32 pt_out) {
  float v[3];
  for (int i = 0; i < 3; i++) {
    v[i] = origin[i] - cyl_origin[i];
  }
  const float va0 = v[0] * axis[0];
  const float va1 = v[1] * axis[1];
  const float va2 = v[2] * axis[2];
  const float da0 = dir[0] * axis[0];
  const float da1 = dir[1] * axis[1];
  const float da2 = dir[2] * axis[2];
  // distances along the axis of the start and of the end of the probe
  float h = va1 + va0;
  h = h + va2;
  float dh = da1 + da0;
  dh = dh + da2;
  const float h_end = dh + h;
  if (sign_bit(h) && sign_bit(h_end)) {
    return kMissF;
  }
  const float h_over = h - len;
  const float h_end_over = h_end - len;
  if (!(sign_bit(h_over) || sign_bit(h_end_over))) {
    return kMissF;
  }
  // the probe projected on the cylinder's base plane, against a sphere
  float p[4], q[4];
  for (int i = 0; i < 3; i++) {
    const float ah = axis[i] * h;
    p[i] = v[i] - ah;
    const float adh = axis[i] * dh;
    q[i] = dir[i] - adh;
  }
  p[3] = 0.f;
  q[3] = 0.f;
  const float t = raw_ray_sphere(rad, p, q);
  if (sign_bit(t)) {
    return kMissF;
  }
  const float dht = dh * t;
  const float h_hit = h + dht;
  if (sign_bit(h_hit)) {
    return kMissF;
  }
  float pt[4];
  for (int i = 0; i < 3; i++) {
    const float ah = axis[i] * h_hit;
    pt[i] = cyl_origin[i] + ah;
  }
  pt[3] = cyl_origin[3];
  gstore_q(pt_out, pt);
  const float h_hit_over = h_hit - len;
  if (!sign_bit(h_hit_over)) {
    return kMissF;
  }
  return t;
}

}  // namespace

/*!
 * (ray-sphere-intersect ray-origin ray-dir sph-origin radius) -> float
 * The GOAL function, compiled to C by goalc on the 3DS, calls pc-port-raw-ray-sphere-implementation
 * with the vectors on the stack.
 */
// inlined into its direct GOAL entry (native_as_goal), so the arguments stay in registers
__attribute__((always_inline)) inline u64 ray_sphere_intersect_impl(const NativeArgs& args) {
  float origin[4], dir[4], center[4];
  gload_q(origin, (u32)args.a[0]);
  gload_q(center, (u32)args.a[2]);
  gload_q(dir, (u32)args.a[1]);
  return f2gpr(ray_sphere_intersect_v(origin, dir, center, u2f((u32)args.a[3])));
}

/*!
 * (ray-cylinder-intersect ray-origin ray-dir cyl-origin cyl-axis cyl-rad cyl-len pt-out) -> float
 */
// inlined into its direct GOAL entry (native_as_goal), so the arguments stay in registers
__attribute__((always_inline)) inline u64 ray_cylinder_intersect_impl(const NativeArgs& args) {
  float origin[4], dir[4], cyl_origin[4], axis[4];
  gload_q(origin, (u32)args.a[0]);
  gload_q(cyl_origin, (u32)args.a[2]);
  gload_q(dir, (u32)args.a[1]);
  gload_q(axis, (u32)args.a[3]);
  return f2gpr(ray_cylinder_intersect_v(origin, dir, cyl_origin, axis, u2f((u32)args.a[4]),
                                        u2f((u32)args.a[5]), (u32)args.a[6]));
}

/*!
 * (moving-sphere-sphere-intersect sphere move other out-point) -> float
 * ray-sphere-intersect of sphere's center along move against other, with the sum of the radii. On
 * a hit (t >= 0, or NaN) stores the touching point in out-point: the center at t, plus the
 * direction to other scaled to sphere's radius (vector-normalize! and vector+*! inlined).
 */
float moving_sphere_sphere_intersect_v(u32 sphere,
                                       u32 move,
                                       u32 other,
                                       u32 out,
                                       const NativeArgs& caller) {
  static const u32 sphere_sym = sym_addr("ray-sphere-intersect");
  static const u32* sphere_stub = native_stub_slot("ray-sphere-intersect");
  const float r = gload<float>(sphere + 12) + gload<float>(other + 12);
  const u32 fn = gload<u32>(sphere_sym);
  float t;
  if (fn == *sphere_stub) {
    float origin[4], dir[4], center[4];
    gload_q(origin, sphere);
    gload_q(center, other);
    gload_q(dir, move);
    t = ray_sphere_intersect_v(origin, dir, center, r);
  } else {
    // redefined in GOAL
    const u64 call_args[8] = {sphere, move, other, f2gpr(r), 0, 0, 0, 0};
    t = u2f((u32)native_call_goal(fn, call_args, caller));
  }
  if (t < 0.f) {
    return t;
  }

  // (vector-normalize! (vector-! (new-stack-vector0) other sphere) (-> sphere w))
  float s[4], o[4];
  gload_q(o, other);
  gload_q(s, sphere);
  float d[3];
  for (int i = 0; i < 3; i++) {
    d[i] = o[i] - s[i];
  }
  const float radius = gload<float>(sphere + 12);
  const float d0 = d[0] * d[0];
  const float d1 = d[1] * d[1];
  const float d2 = d[2] * d[2];
  float len2 = 1.f * d0;
  len2 = len2 + 1.f * d1;
  len2 = len2 + 1.f * d2;
  const float len = 1.f * std::sqrt(len2);
  float scale;
  if (0.f == len) {
    scale = u2f(0x7f7fffff ^ (f2u(radius) & 0x80000000) ^ (f2u(len) & 0x80000000));
  } else {
    scale = radius / len;
  }
  for (int i = 0; i < 3; i++) {
    d[i] = d[i] * scale;
  }

  // (vector+*! out sphere move t)
  float m[4], p[4];
  gload_q(s, sphere);
  gload_q(m, move);
  for (int i = 0; i < 3; i++) {
    const float mt = m[i] * t;
    const float s1 = s[i] * 1.f;
    p[i] = mt + s1;
  }
  p[3] = 1.f;
  gstore_q(out, p);
  // (vector+! out out normal)
  gload_q(p, out);
  for (int i = 0; i < 3; i++) {
    p[i] = p[i] + d[i];
  }
  p[3] = 1.f;
  gstore_q(out, p);
  return t;
}

// inlined into its direct GOAL entry (native_as_goal), so the arguments stay in registers
__attribute__((always_inline)) inline u64 moving_sphere_sphere_intersect_impl(
    const NativeArgs& args) {
  return f2gpr(moving_sphere_sphere_intersect_v((u32)args.a[0], (u32)args.a[1], (u32)args.a[2],
                                                (u32)args.a[3], args));
}

/*!
 * (collide-do-primitives pos move radius tri out-point) -> float
 * moving-sphere-triangle-intersect's test for a sphere that touches the triangle's plane outside
 * the triangle: the sphere swept along move against the corners (ray-sphere-intersect) and the
 * edges (ray-cylinder-intersect) of the triangle. Returns the smallest t, or -100000000.0 if it is
 * above 1.0 or nothing was hit, and stores the touching point (the corner, or the point on the
 * edge) in out-point.
 */
u64 collide_do_primitives_impl(const NativeArgs& args) {
  static const u32 sphere_sym = sym_addr("ray-sphere-intersect");
  static const u32 cylinder_sym = sym_addr("ray-cylinder-intersect");
  static const u32* sphere_stub = native_stub_slot("ray-sphere-intersect");
  static const u32* cylinder_stub = native_stub_slot("ray-cylinder-intersect");
  const u64 tri = args.a[3];
  const u32 out = (u32)args.a[4];
  // the edge direction is passed by address
  const u32 dir = args.stack - 16;

  float best = 2.f;
  // vf31: the point of the best hit. If nothing is hit, the mips2c version stores whatever the
  // register held (zero in verify mode).
  u8 point[16] = {};

  // corners
  for (int i = 0; i < 3; i++) {
    const u32 fn = gload<u32>(sphere_sym);
    float t;
    if (fn == *sphere_stub) {
      float pos[4], move[4], corner[4];
      gload_q(pos, (u32)args.a[0]);
      gload_q(corner, (u32)tri + 16 * i);
      gload_q(move, (u32)args.a[1]);
      t = ray_sphere_intersect_v(pos, move, corner, u2f((u32)args.a[2]));
    } else {
      const u64 call_args[8] = {args.a[0], args.a[1], tri + 16 * i, args.a[2],
                                args.a[4], args.a[5], args.a[6],    args.a[7]};
      t = u2f((u32)native_call_goal(fn, call_args, args));
    }
    // (the first hit is taken even if it's above 2.0)
    if (t < 0.f || (i > 0 && !(t < best))) {
      continue;
    }
    best = t;
    memcpy(point, gptr((u32)tri + 16 * i), 16);
  }

  // edges: v0 -> v1, v1 -> v2, v2 -> v0
  for (int i = 0; i < 3; i++) {
    const u32 a = (u32)tri + 16 * i;
    const u32 b = (u32)tri + 16 * ((i + 1) % 3);
    float d[4], sq[4];
    const Vec4f va = gload_vec(a);
    const Vec4f vb = gload_vec(b);
    d[0] = vb.x - va.x;
    d[1] = vb.y - va.y;
    d[2] = vb.z - va.z;
    d[3] = vb.w - va.w;
    for (int k = 0; k < 4; k++) {
      sq[k] = d[k] * d[k];
    }
    float len2 = sq[0] + sq[1];
    len2 = len2 + sq[2];
    // vrsqrt and sqrt.s
    const float len = std::sqrt(std::abs(len2));
    const float q = len == 0 ? 0.f : 1.f / len;
    for (int k = 0; k < 4; k++) {
      d[k] = d[k] * q;
    }
    const u32 fn = gload<u32>(cylinder_sym);
    float t;
    if (fn == *cylinder_stub) {
      float pos[4], move[4];
      const float corner[4] = {va.x, va.y, va.z, va.w};
      gload_q(pos, (u32)args.a[0]);
      gload_q(move, (u32)args.a[1]);
      t = ray_cylinder_intersect_v(pos, move, corner, d, u2f((u32)args.a[2]), len, out);
    } else {
      gstore_bytes(dir, d, 16);
      const u64 call_args[8] = {args.a[0], args.a[1],  tri + 16 * i, dir,
                                args.a[2], f2gpr(len), args.a[4],    args.a[7]};
      t = u2f((u32)native_call_goal(fn, call_args, args));
    }
    if (t < 0.f || !(t < best)) {
      continue;
    }
    best = t;
    memcpy(point, gptr(out), 16);
  }

  if (1.f < best) {
    best = u2f((u32)kMiss);
  }
  gstore_bytes(out, point, 16);
  return f2gpr(best);
}

/*!
 * (moving-sphere-triangle-intersect pos move radius tri out-point out-normal)
 * Where along move (0 to 1) a sphere of radius at pos first touches the triangle, or -100000000.0.
 * Once the bounding boxes overlap, writes the triangle's normal to out-normal. On a hit inside the
 * triangle writes the touching point to out-point; when the sphere touches the plane outside the
 * triangle, returns what collide-do-primitives (edges and corners) finds.
 */
u64 moving_sphere_triangle_intersect_impl(const NativeArgs& args) {
  const u32 pos_addr = (u32)args.a[0];
  const u32 move_addr = (u32)args.a[1];
  const float radius = u2f((u32)args.a[2]);
  const u32 tri = (u32)args.a[3];
  const u32 out_point = (u32)args.a[4];
  const u32 out_normal = (u32)args.a[5];
  const float one = 1.f;  // vf0.w
  const float vf0[4] = {0.f, 0.f, 0.f, 1.f};

  float pos[4], move[4], v0[4], v1[4], v2[4];
  gload_q(pos, pos_addr);
  gload_q(move, move_addr);
  gload_q(v0, tri);
  gload_q(v1, tri + 16);
  gload_q(v2, tri + 32);

  // relative to v1: the other two corners, the start and the end of the move
  float e0[4], e2[4], p[4], pe[4];
  for (int i = 0; i < 4; i++) {
    const float end = pos[i] + move[i];
    e0[i] = v0[i] - v1[i];
    e2[i] = v2[i] - v1[i];
    p[i] = pos[i] - v1[i];
    pe[i] = end - v1[i];
  }

  // normal (not normalized)
  float n[4];
  opm(n, e2, e0);
  n[3] = 0;  // the register's w is stale here; it is only used after being set to 1

  // bounding boxes of the triangle and of the swept sphere must overlap
  float tri_min[4], tri_max[4], sweep_min[4], sweep_max[4], gap_hi[4], gap_lo[4];
  for (int i = 0; i < 4; i++) {
    tri_min[i] = std::min(e0[i], vf0[i]);
    tri_max[i] = std::max(e0[i], vf0[i]);
    sweep_min[i] = std::min(p[i], pe[i]);
    sweep_max[i] = std::max(p[i], pe[i]);
  }
  for (int i = 0; i < 4; i++) {
    tri_min[i] = std::min(tri_min[i], e2[i]);
    tri_max[i] = std::max(tri_max[i], e2[i]);
    sweep_min[i] = sweep_min[i] - radius;
    sweep_max[i] = sweep_max[i] + radius;
  }
  for (int i = 0; i < 4; i++) {
    gap_hi[i] = tri_max[i] - sweep_min[i];
    gap_lo[i] = sweep_max[i] - tri_min[i];
  }
  const float nx2 = n[0] * n[0];
  const float ny2 = n[1] * n[1];
  const float nz2 = n[2] * n[2];
  float len2 = nx2 + ny2;
  len2 = len2 + nz2;
  // vrsqrt Q, vf0.w, len2
  const float s = std::sqrt(std::abs(len2));
  const float q = s == 0 ? 0.f : one / s;
  // like the original: a 64-bit or of lanes x and y, so z of gap_hi isn't tested
  if ((s32)(f2u(gap_hi[0]) | f2u(gap_lo[0])) < 0 || (s32)(f2u(gap_hi[1]) | f2u(gap_lo[1])) < 0 ||
      (s32)f2u(gap_lo[2]) < 0) {
    return kMiss;
  }

  // distances to the plane along the normal
  float np[3] = {n[0] * p[0], n[1] * p[1], n[2] * p[2]};
  float nmx = n[0] * move[0];
  const float nmy = n[1] * move[1];
  const float nmz = n[2] * move[2];
  nmx = nmx + nmy;
  np[1] = vf0[1] - np[1];
  nmx = nmx + nmz;
  np[1] = np[1] - np[0];
  np[1] = np[1] - np[2];
  np[0] = vf0[0] + vf0[0];
  for (int i = 0; i < 3; i++) {
    n[i] = n[i] * q;
  }
  n[3] = one;
  nmx = nmx * q;
  // ta = (np + radius) / nm, tb = (np - radius) / nm (lanes x and y)
  float ta[2] = {np[0] * q, np[1] * q};
  float tb[2] = {np[0] * q, np[1] * q};
  gstore_q(out_normal, n);
  const float q2 = one / nmx;  // vdiv
  ta[1] = ta[1] + radius;
  tb[1] = tb[1] - radius;
  for (int i = 0; i < 2; i++) {
    ta[i] = ta[i] * q2;
    tb[i] = tb[i] * q2;
  }

  float pt[4];  // the sphere's center when it touches the plane
  const float* center = pt;
  u64 result;
  if (sign_bit(ta[1]) || sign_bit(tb[1])) {
    if (sign_bit(ta[1]) && sign_bit(tb[1])) {
      return kMiss;
    }
    // touching the plane at the start
    center = p;
    result = 0;
  } else {
    // the smaller of the two (as 64-bit integers, like the original)
    const bool use_b = (s64)(low64(ta) - low64(tb)) >= 0;
    const float t = use_b ? tb[1] : ta[1];
    if (!sign_bit(t - one)) {
      return kMiss;
    }
    for (int i = 0; i < 4; i++) {
      const float acc = p[i] * one;
      pt[i] = acc + move[i] * t;
    }
    result = f2gpr(t);
  }

  // is the center over the triangle? (edge cross products dotted with the normal)
  float d2[4], d0[4], c0[4], c1[4], c2[4];
  for (int i = 0; i < 3; i++) {
    d2[i] = center[i] - e2[i];
    d0[i] = center[i] - e0[i];
  }
  opm(c0, e2, center);
  c0[3] = gap_lo[3];  // the register held gap_lo before; its w is written out below
  opm(c1, center, e0);
  opm(c2, d2, d0);
  for (int i = 0; i < 3; i++) {
    c0[i] = c0[i] * n[i];
    c1[i] = c1[i] * n[i];
    c2[i] = c2[i] * n[i];
  }
  c0[3] = c0[3] * n[3];
  c0[1] = c0[1] + c0[0];
  c1[1] = c1[1] + c1[0];
  c2[1] = c2[1] + c2[0];
  c0[1] = c0[1] + c0[2];
  c1[1] = c1[1] + c1[2];
  c2[1] = c2[1] + c2[2];
  const u64 or01 = low64(c0) | low64(c1);
  if ((s64)(or01 | low64(c2)) < 0) {
    // outside: test the edges and corners
    static const u32 sym = sym_addr("collide-do-primitives");
    static const u32* stub = native_stub_slot("collide-do-primitives");
    const u64 call_args[8] = {args.a[0], args.a[1], args.a[2], args.a[3],
                              args.a[4], or01,      low64(c1), args.a[7]};
    const u32 fn = gload<u32>(sym);
    if (fn == *stub) {
      return collide_do_primitives_impl(NativeArgs{call_args, args.pp, args.st, args.stack});
    }
    return native_call_goal(fn, call_args, args);
  }

  // the touching point: the center projected on the plane (n x (center x n)), plus v1
  float a[4], b[4], out[4];
  opm(a, center, n);
  a[3] = c0[3];
  opm(b, n, a);
  b[3] = a[3];
  for (int i = 0; i < 4; i++) {
    out[i] = b[i] + v1[i];
  }
  gstore_q(out_point, out);
  return result;
}

const NativeImpl collide_do_primitives = MIPS2C_NATIVE_IMPL(collide_do_primitives_impl, 0, 16);
// scratch: collide-do-primitives', which it calls directly
const NativeImpl moving_sphere_triangle_intersect =
    MIPS2C_NATIVE_IMPL(moving_sphere_triangle_intersect_impl, 0, 16);
// GOAL code calls these directly too (MIPS2C_NATIVE_IMPL_GOAL)
const NativeImpl ray_sphere_intersect = MIPS2C_NATIVE_IMPL_GOAL(ray_sphere_intersect_impl, 0);
const NativeImpl ray_cylinder_intersect = MIPS2C_NATIVE_IMPL_GOAL(ray_cylinder_intersect_impl, 0);
const NativeImpl moving_sphere_sphere_intersect =
    MIPS2C_NATIVE_IMPL_GOAL(moving_sphere_sphere_intersect_impl, 0);

}  // namespace Mips2C::jak1::native

// GOAL functions replaced by their native version (def-mips2c in collide-func.gc). There is no
// mips2c version: the execute function (used by the native GOAL backends) runs the native one.
namespace Mips2C::jak1 {
namespace ray_sphere_intersect {
void link() {
  gLinkedFunctionTable.reg("ray-sphere-intersect", native::ray_sphere_intersect.as_exec, 64,
                           &native::ray_sphere_intersect);
}
}  // namespace ray_sphere_intersect
namespace ray_cylinder_intersect {
void link() {
  gLinkedFunctionTable.reg("ray-cylinder-intersect", native::ray_cylinder_intersect.as_exec, 64,
                           &native::ray_cylinder_intersect);
}
}  // namespace ray_cylinder_intersect
namespace moving_sphere_sphere_intersect {
void link() {
  gLinkedFunctionTable.reg("moving-sphere-sphere-intersect",
                           native::moving_sphere_sphere_intersect.as_exec, 64,
                           &native::moving_sphere_sphere_intersect);
}
}  // namespace moving_sphere_sphere_intersect
}  // namespace Mips2C::jak1
