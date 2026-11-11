#pragma once

/*!
 * @file CtrCull.h
 * (AI-assisted) Culling math of the 3DS level renderer that has no 3DS dependencies: used by
 * CtrLevel.cpp (draw_level), by the level converter (tools/ctr_level_converter) and by
 * test/ctr_render. World units are the game's (4096 = 1 meter).
 */

#include "game/graphics/ctr/CtrSimd.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

namespace ctr_cull {

inline float dot3(const float* a, const float* b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
inline void cross3(const float* a, const float* b, float* out) {
  out[0] = a[1] * b[2] - a[2] * b[1];
  out[1] = a[2] * b[0] - a[0] * b[2];
  out[2] = a[0] * b[1] - a[1] * b[0];
}

/*!
 * (AI-assisted, sizecull) An upper bound of how many pixels across a sphere looks: `focal` pixels per
 * unit of size at unit depth, `fwd` the view's unit direction. A sphere of center depth z, distance
 * d spans at most 2 r f d / (z (z - r)) along the screen (the off-axis stretch d / z included).
 * Reaching to the camera's plane: huge.
 */
//! (AI-assisted, fdiv) sphere_px(...) < lim, without its square root and divide: with positive
//! z - r both sides of 2 r f d < lim z (z - r) are non-negative, so they compare squared.
inline bool sphere_px_below(const float* c, float r, const float* eye, const float* fwd,
                            float focal, float lim) {
  const float v[3] = {c[0] - eye[0], c[1] - eye[1], c[2] - eye[2]};
  const float z = dot3(v, fwd);
  if (!(z - r > 1e-3f * (r + 1.f)) || !(lim > 0)) {
    return false;
  }
  const float a = 2.f * r * focal;
  const float b = lim * z * (z - r);
  return a * a * dot3(v, v) < b * b;
}

inline float sphere_px(const float* c, float r, const float* eye, const float* fwd, float focal) {
  const float v[3] = {c[0] - eye[0], c[1] - eye[1], c[2] - eye[2]};
  const float z = dot3(v, fwd);
  if (!(z - r > 1e-3f * (r + 1.f))) {
    return 1e30f;
  }
  const float d = std::sqrt(dot3(v, v));
  return 2.f * r * focal * d / (z * (z - r));
}

/*
 * (AI-assisted, facecull) Normal cones (config.ini `cone_cull`, docs/3ds-port/culling.md "10."): a
 * run of back face culled triangles whose normals ((p1 - p0) x (p2 - p0), the converter's outward
 * side) are all within angle a of an axis. Seen from the camera every triangle faces away when, with
 * the run's sphere (c, r) and v = c - eye at angle t from the axis, |v| cos(t + a) > r: each normal
 * n is within t + a of v's direction, so n . (p - eye) >= n . v - r > 0 for every point p of the
 * run. Packed in a u32: the axis as three s8 (x 127), the cosine of a (s8, x 127, rounded down);
 * cos byte -128: no cone.
 */
constexpr uint32_t kNoCone = 0x80000000u;

inline uint32_t cone_pack(const float* axis, float cos_a) {
  uint32_t out = 0;
  for (int k = 0; k < 3; k++) {
    const int q = (int)std::lround(std::fmax(-127.f, std::fmin(127.f, axis[k] * 127.f)));
    out |= (uint32_t)(uint8_t)(int8_t)q << (8 * k);
  }
  const int qc = (int)std::floor(std::fmax(-127.f, std::fmin(127.f, cos_a * 127.f)));
  return out | (uint32_t)(uint8_t)(int8_t)qc << 24;
}
inline bool cone_unpack(uint32_t p, float* axis, float* cos_a) {
  const int8_t qc = (int8_t)(uint8_t)(p >> 24);
  if (qc == -128) {
    return false;
  }
  float l = 0;
  for (int k = 0; k < 3; k++) {
    axis[k] = (float)(int8_t)(uint8_t)(p >> (8 * k)) / 127.f;
    l += axis[k] * axis[k];
  }
  if (!(l > 0)) {
    return false;
  }
  l = 1.f / std::sqrt(l);
  for (int k = 0; k < 3; k++) {
    axis[k] *= l;
  }
  *cos_a = qc / 127.f;
  return true;
}

/*!
 * The cone of triangles (9 floats each: three corners), its half angle grown by `grow` radians and
 * checked against the packed (quantized) axis. kNoCone when it is 80 degrees or wider, or there is
 * no triangle with an area.
 */
inline uint32_t make_cone(const float* tris, size_t n, float grow) {
  double sum[3] = {0, 0, 0};
  size_t used = 0;
  for (size_t i = 0; i < n; i++) {
    const float* t = tris + 9 * i;
    const double e1[3] = {(double)t[3] - t[0], (double)t[4] - t[1], (double)t[5] - t[2]};
    const double e2[3] = {(double)t[6] - t[0], (double)t[7] - t[1], (double)t[8] - t[2]};
    const double c[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                         e1[0] * e2[1] - e1[1] * e2[0]};
    const double l = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
    if (!(l > 0)) {
      continue;
    }
    for (int k = 0; k < 3; k++) {
      sum[k] += c[k] / l;
    }
    used++;
  }
  const double sl = std::sqrt(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
  if (!used || !(sl > 1e-9)) {
    return kNoCone;
  }
  float axis[3] = {(float)(sum[0] / sl), (float)(sum[1] / sl), (float)(sum[2] / sl)};
  // the axis as it will be read, and the widest normal from it
  float qa[3], dummy;
  if (!cone_unpack(cone_pack(axis, 1.f), qa, &dummy)) {
    return kNoCone;
  }
  double min_dot = 1;
  for (size_t i = 0; i < n; i++) {
    const float* t = tris + 9 * i;
    const double e1[3] = {(double)t[3] - t[0], (double)t[4] - t[1], (double)t[5] - t[2]};
    const double e2[3] = {(double)t[6] - t[0], (double)t[7] - t[1], (double)t[8] - t[2]};
    const double c[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                         e1[0] * e2[1] - e1[1] * e2[0]};
    const double l = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
    if (!(l > 0)) {
      continue;
    }
    min_dot = std::min(min_dot, (c[0] * qa[0] + c[1] * qa[1] + c[2] * qa[2]) / l);
  }
  const double a = std::acos(std::max(-1.0, std::min(1.0, min_dot))) + grow;
  if (a >= 80.0 * 3.14159265358979 / 180.0) {
    return kNoCone;
  }
  return cone_pack(qa, (float)std::cos(a));
}

//! 1 / sqrt(x) for x > 0 without a divide or a square root (the VFP11 takes 19 cycles for each and
//! holds its multiply-add pipeline meanwhile): the bit trick's estimate (~3.4%) and two Newton
//! steps (each squares the error: ~5e-6).
inline float rsqrt_nr(float x) {
  uint32_t i = ctr_float_bits(x);
  i = 0x5f375a86u - (i >> 1);
  float y;
  memcpy(&y, &i, 4);
  const float h = 0.5f * x;
  y = y * (1.5f - h * y * y);
  y = y * (1.5f - h * y * y);
  return y;
}

//! The test before (AI-assisted, fdiv): a cone_unpack (5 divides and a square root) and three
//! square roots and a divide. Kept for the tests (cull_test.cpp) and cpu_bench.
inline bool cone_away_ref(uint32_t cone, const float* c, float r, const float* eye) {
  float a[3], ca;
  if (!cone_unpack(cone, a, &ca)) {
    return false;
  }
  const float v[3] = {c[0] - eye[0], c[1] - eye[1], c[2] - eye[2]};
  const float d = std::sqrt(dot3(v, v));
  if (!(d > r)) {
    return false;
  }
  const float ct = dot3(v, a) / d;
  const float st = std::sqrt(std::fmax(0.f, 1.f - ct * ct));
  const float sa = std::sqrt(std::fmax(0.f, 1.f - ca * ca));
  return d * (ct * ca - st * sa) > r;
}

//! Does every triangle of the run (inside sphere c, r) face away from the eye? (AI-assisted, fdiv)
//! With p = v . axis and d2 = |v|^2, |v| cos(t + a) > r is p cos a - r > sqrt(d2 - p^2) sin a:
//! L = p cos a - r > 0 and L^2 > (d2 - p^2)(1 - cos^2 a). Only multiplies (the axis normalized by
//! rsqrt_nr); the same as cone_away_ref up to rounding, well inside the cone's 3 degree margin.
inline bool cone_away(uint32_t cone, const float* c, float r, const float* eye) {
  const int8_t qc = (int8_t)(uint8_t)(cone >> 24);
  if (qc == -128) {
    return false;
  }
  const float ax = (float)(int8_t)(uint8_t)cone, ay = (float)(int8_t)(uint8_t)(cone >> 8),
              az = (float)(int8_t)(uint8_t)(cone >> 16);
  const float l2 = ax * ax + ay * ay + az * az;
  if (!(l2 > 0)) {
    return false;
  }
  const float v[3] = {c[0] - eye[0], c[1] - eye[1], c[2] - eye[2]};
  const float d2 = dot3(v, v);
  if (!(d2 > r * r)) {
    return false;
  }
  const float ca = (float)qc * (1.f / 127.f);
  const float p = (v[0] * ax + v[1] * ay + v[2] * az) * rsqrt_nr(l2);
  const float L = p * ca - r;
  if (!(L > 0)) {
    return false;
  }
  return L * L > std::fmax(0.f, d2 - p * p) * (1.f - ca * ca);
}

}  // namespace ctr_cull
