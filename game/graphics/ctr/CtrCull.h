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

/*
 * (AI-assisted, depthreuse) The depth of an earlier frame (config.ini `depth_reuse`,
 * docs/3ds-port/culling.md "4."): the top screen in 8x8 blocks (50 x 30, rows from the top), each
 * block's farthest and nearest depth as the GPU stored it (0 far .. 1 near: 0.5 (1 + z/w) of the
 * GL clip space the level renderer draws in), and that frame's camera. A sphere all of whose
 * points were farther than the farthest depth of every block it covered was hidden then.
 */
constexpr int kDepthBX = 50, kDepthBY = 30;

struct DepthSnap {
  float m[16];  // world -> GL clip, row major: clip_r = m[4r] p.x + m[4r+1] p.y + m[4r+2] p.z + m[4r+3]
  float eye[3];
  float zfar[kDepthBX * kDepthBY];
  float znear[kDepthBX * kDepthBY];
};

inline void depth_clip(const float* m, const float* p, float* c) {
  for (int r = 0; r < 4; r++) {
    c[r] = m[4 * r] * p[0] + m[4 * r + 1] * p[1] + m[4 * r + 2] * p[2] + m[4 * r + 3];
  }
}

//! The stored depth of a point at view distance d straight along the view (the w gradient).
inline float depth_at_distance(const DepthSnap& s, float d) {
  const float* g = s.m + 12;
  const float gl = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
  if (!(gl > 0)) {
    return 1.f;
  }
  const float p[3] = {s.eye[0] + d * g[0] / gl, s.eye[1] + d * g[1] / gl, s.eye[2] + d * g[2] / gl};
  float c[4];
  depth_clip(s.m, p, c);
  return c[3] > 0 ? 0.5f * (1.f + c[2] / c[3]) : 1.f;
}

/*!
 * Was the sphere hidden in that frame: all of it on screen there, behind the farthest depth of the
 * blocks it covered (by `eps`), with nothing in those blocks nearer than stored depth `z_limit`
 * (things near the camera may have moved since: Jak, an enemy)? 2: behind all of its blocks, 1:
 * behind some of them (its pieces may be), 0: none (or reaching off screen, or behind the camera).
 */
inline int depth_test(const DepthSnap& s, const float* c, float r, float z_limit, float eps) {
  float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
  for (int k = 0; k < 8; k++) {
    const float p[3] = {c[0] + (k & 1 ? r : -r), c[1] + (k & 2 ? r : -r), c[2] + (k & 4 ? r : -r)};
    float q[4];
    depth_clip(s.m, p, q);
    if (!(q[3] > 1e-3f)) {
      return 0;
    }
    // (AI-assisted, fdiv) one reciprocal per corner, not two divides (19 cycles each on the VFP11)
    const float iw = 1.f / q[3];
    const float sx = (q[0] * iw + 1.f) * (kDepthBX * 4.f);
    const float sy = (1.f - q[1] * iw) * (kDepthBY * 4.f);
    x0 = std::fmin(x0, sx);
    x1 = std::fmax(x1, sx);
    y0 = std::fmin(y0, sy);
    y1 = std::fmax(y1, sy);
  }
  // all of it on screen in that frame: the depth of a part off screen then is unknown (a turn or a
  // roll since may have brought it in view)
  if (!(x0 >= 1.f && y0 >= 1.f && x1 <= kDepthBX * 8.f - 1.f && y1 <= kDepthBY * 8.f - 1.f)) {
    return 0;
  }
  const int bx0 = std::max(0, (int)std::floor(x0 / 8.f));
  const int bx1 = std::min(kDepthBX - 1, (int)std::floor(x1 / 8.f));
  const int by0 = std::max(0, (int)std::floor(y0 / 8.f));
  const int by1 = std::min(kDepthBY - 1, (int)std::floor(y1 / 8.f));
  // the sphere's nearest point: the least w
  const float* g = s.m + 12;
  const float gl = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
  if (!(gl > 0)) {
    return 0;
  }
  const float p[3] = {c[0] - r * g[0] / gl, c[1] - r * g[1] / gl, c[2] - r * g[2] / gl};
  float q[4];
  depth_clip(s.m, p, q);
  if (!(q[3] > 1e-3f)) {
    return 0;
  }
  const float zn = 0.5f * (1.f + q[2] / q[3]);
  bool all = true, any = false;
  for (int by = by0; by <= by1; by++) {
    for (int bx = bx0; bx <= bx1; bx++) {
      const int b = by * kDepthBX + bx;
      const bool h = zn < s.zfar[b] - eps && s.znear[b] < z_limit;
      all = all && h;
      any = any || h;
    }
  }
  return all ? 2 : (any ? 1 : 0);
}
inline bool depth_hidden(const DepthSnap& s, const float* c, float r, float z_limit, float eps) {
  return depth_test(s, c, r, z_limit, eps) == 2;
}

/*!
 * How much to grow a sphere at distance d from the eye before testing it against a copy taken from
 * an eye `move` away, when everything that may hide it in the copy is at least `d_min` from that
 * eye: the move shifts such a surface against the sphere by at most move / d_min radians, which is
 * move x d / d_min at the sphere (plus `base`).
 */
inline float depth_grow(float d, float move, float d_min, float base) {
  return base + move * d / std::fmax(d_min, 1.f);
}

//! The camera position of a world -> clip matrix: where x, y and w are 0.
inline bool depth_eye(const float* m, float* eye) {
  const float* a = m;       // x row
  const float* b = m + 4;   // y row
  const float* c = m + 12;  // w row
  const float det = a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) +
                    a[2] * (b[0] * c[1] - b[1] * c[0]);
  if (!(std::fabs(det) > 1e-20f)) {
    return false;
  }
  const float r0 = -a[3], r1 = -b[3], r2 = -c[3];
  eye[0] = (r0 * (b[1] * c[2] - b[2] * c[1]) - a[1] * (r1 * c[2] - b[2] * r2) +
            a[2] * (r1 * c[1] - b[1] * r2)) / det;
  eye[1] = (a[0] * (r1 * c[2] - b[2] * r2) - r0 * (b[0] * c[2] - b[2] * c[0]) +
            a[2] * (b[0] * r2 - r1 * c[0])) / det;
  eye[2] = (a[0] * (b[1] * r2 - r1 * c[1]) - a[1] * (b[0] * r2 - r1 * c[0]) +
            r0 * (b[0] * c[1] - b[1] * c[0])) / det;
  return true;
}

//! The blocks of a depth buffer of kDepthBX x kDepthBY blocks' pixels (rows from the top).
inline void depth_blocks(const float* depth, float* zfar, float* znear) {
  constexpr int w = kDepthBX * 8;
  for (int by = 0; by < kDepthBY; by++) {
    for (int bx = 0; bx < kDepthBX; bx++) {
      float lo = 1e30f, hi = -1e30f;
      for (int y = by * 8; y < by * 8 + 8; y++) {
        for (int x = bx * 8; x < bx * 8 + 8; x++) {
          lo = std::fmin(lo, depth[y * w + x]);
          hi = std::fmax(hi, depth[y * w + x]);
        }
      }
      zfar[by * kDepthBX + bx] = lo;
      znear[by * kDepthBX + bx] = hi;
    }
  }
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
