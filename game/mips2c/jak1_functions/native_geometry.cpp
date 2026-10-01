/*!
 * @file native_geometry.cpp
 * (AI-assisted)
 * Native version of closest-pt-in-triangle (geometry.gc), which the collision natives call for
 * every triangle near a sphere: the same float operations as the C code goalc makes of the GOAL
 * function (the vector-segment-distance-point! it calls is inlined). See
 * game/mips2c/mips2c_native.h for the rules.
 */

#include <cmath>
#include <cstring>

#include "game/mips2c/jak1_functions/native_functions.h"
#include "game/mips2c/mips2c_table.h"

namespace Mips2C::jak1::native {

namespace {

inline bool sign_bit(float f) {
  return (s32)f2u(f) < 0;
}

//! a x b as the outer product of goalc (x = a.y * b.z - b.y * a.z, ...)
inline void cross(float out[3], const float a[4], const float b[4]) {
  const float p0 = a[1] * b[2];
  const float p1 = a[2] * b[0];
  const float p2 = a[0] * b[1];
  const float m0 = b[1] * a[2];
  const float m1 = b[2] * a[0];
  const float m2 = b[0] * a[1];
  out[0] = p0 - m0;
  out[1] = p1 - m1;
  out[2] = p2 - m2;
}

//! (c . n) as the GOAL code adds the lanes: (y + x) + z
inline float dot_yxz(const float c[3], const float n[4]) {
  const float x = c[0] * n[0];
  const float y = c[1] * n[1];
  const float z = c[2] * n[2];
  float d = y + x;
  d = d + z;
  return d;
}

/*!
 * (vector-segment-distance-point! point p1 p2 closest): the distance from point to the segment
 * p1-p2. The closest point of the segment (w = 1.0) goes to closest. Loads the vectors from GOAL
 * memory when called, like the GOAL function.
 */
float segment_distance_point(u32 point, u32 p1, u32 p2, float closest[4]) {
  float a[4], b[4], p[4];
  gload_q(a, p1);
  gload_q(b, p2);
  gload_q(p, point);
  float d[3], e[3];
  for (int i = 0; i < 3; i++) {
    d[i] = b[i] - a[i];
    e[i] = p[i] - a[i];
  }
  const float d0 = d[0] * d[0];
  const float d1 = d[1] * d[1];
  const float d2 = d[2] * d[2];
  float len2 = 1.f * d0;
  len2 = len2 + 1.f * d1;
  len2 = len2 + 1.f * d2;
  const float len = 0.f + std::sqrt(len2);
  const float inv = 1.f / len;
  for (int i = 0; i < 3; i++) {
    d[i] = d[i] * inv;
  }
  const float de0 = d[0] * e[0];
  const float de1 = d[1] * e[1];
  const float de2 = d[2] * e[2];
  float u = de0 + de1;
  u = u + de2;
  if (!(u >= 0.f)) {
    u = 0.f;
  } else if (!(len >= u)) {
    u = len;
  }
  float r[3];
  for (int i = 0; i < 3; i++) {
    const float s = d[i] * u;
    closest[i] = a[i] + s;
    r[i] = e[i] - s;
  }
  closest[3] = 1.f;
  const float r0 = r[0] * r[0];
  const float r1 = r[1] * r[1];
  const float r2 = r[2] * r[2];
  float dist2 = 1.f * r0;
  dist2 = dist2 + 1.f * r1;
  dist2 = dist2 + 1.f * r2;
  return 0.f + std::sqrt(dist2);
}

//! the closer to point of the segments a-b and a-c (a-b first, kept on a tie or NaN)
template <typename Store>
void closer_of_two(const Store& store, u32 point, u32 a, u32 b, u32 c) {
  float closest[4];
  const float d_ab = segment_distance_point(point, a, b, closest);
  store(closest);
  const float d_ac = segment_distance_point(point, a, c, closest);
  if (!(d_ac >= d_ab)) {
    store(closest);
  }
}

/*!
 * (closest-pt-in-triangle out point tri normal): the point of the triangle (tri: 3 vertices)
 * closest to point. Inside (the edge cross products all on the normal's side) the point projected
 * on the triangle's plane, else the closest point of the nearest edge(s). store(const float[4])
 * writes out (once, or twice when two edges are compared).
 */
template <typename Store>
void closest_pt_in_triangle_t(const Store& store, u32 point, u32 tri, u32 normal) {
  float v0[4], v1[4], v2[4], p[4], n[4];
  gload_q(v1, tri + 16);
  gload_q(v2, tri + 32);
  gload_q(p, point);
  gload_q(v0, tri);
  gload_q(n, normal);
  float e12[4], e1p[4], e10[4], ep2[4], ep0[4];
  for (int i = 0; i < 3; i++) {
    e12[i] = v1[i] - v2[i];
    e1p[i] = v1[i] - p[i];
    e10[i] = v1[i] - v0[i];
    ep2[i] = p[i] - v2[i];
    ep0[i] = p[i] - v0[i];
  }
  float c0[3], c1[3], c2[3];
  cross(c0, e1p, e10);
  cross(c1, e12, e1p);
  cross(c2, ep2, ep0);
  const u32 outside = (u32)sign_bit(dot_yxz(c0, n)) | ((u32)sign_bit(dot_yxz(c1, n)) << 1) |
                      ((u32)sign_bit(dot_yxz(c2, n)) << 2);
  switch (outside) {
    case 0: {
      // n x ((point - v0) x n) + v0. The outer products of goalc also write w: here
      // (n.x * a.x) - (a.x * n.x), with a = (point - v0) x n.
      float a[4], b[3];
      cross(a, ep0, n);
      cross(b, n, a);
      const float w0 = n[0] * a[0];
      const float w1 = a[0] * n[0];
      const float r[4] = {b[0] + v0[0], b[1] + v0[1], b[2] + v0[2], w0 - w1};
      store(r);
      break;
    }
    case 1: {
      float closest[4];
      segment_distance_point(point, tri, tri + 16, closest);
      store(closest);
      break;
    }
    case 2: {
      float closest[4];
      segment_distance_point(point, tri + 16, tri + 32, closest);
      store(closest);
      break;
    }
    case 3:
      closer_of_two(store, point, tri + 16, tri, tri + 32);
      break;
    case 4: {
      float closest[4];
      segment_distance_point(point, tri + 32, tri, closest);
      store(closest);
      break;
    }
    case 5:
      closer_of_two(store, point, tri, tri + 16, tri + 32);
      break;
    default:
      closer_of_two(store, point, tri + 32, tri, tri + 16);
      break;
  }
}

}  // namespace

void closest_pt_in_triangle_v(u32 out, u32 point, u32 tri, u32 normal) {
  closest_pt_in_triangle_t([out](const float* r) { gstore_q(out, r); }, point, tri, normal);
}

void closest_pt_in_triangle_h(float out[4], u32 point, u32 tri, u32 normal) {
  closest_pt_in_triangle_t([out](const float* r) { memcpy(out, r, 16); }, point, tri, normal);
}

u64 closest_pt_in_triangle_impl(const NativeArgs& args) {
  closest_pt_in_triangle_v((u32)args.a[0], (u32)args.a[1], (u32)args.a[2], (u32)args.a[3]);
  return 0;
}

const NativeImpl closest_pt_in_triangle = MIPS2C_NATIVE_IMPL(closest_pt_in_triangle_impl, 0, 0);

}  // namespace Mips2C::jak1::native

// a GOAL function replaced by its native version (def-mips2c in geometry.gc): no mips2c version
namespace Mips2C::jak1::closest_pt_in_triangle {
void link() {
  gLinkedFunctionTable.reg("closest-pt-in-triangle", native::closest_pt_in_triangle.as_exec, 64,
                           &native::closest_pt_in_triangle);
}
}  // namespace Mips2C::jak1::closest_pt_in_triangle
