#pragma once

/*!
 * @file fakes.h
 * (AI-assisted)
 * Fake GOAL functions and shared generators for the differential tests. The fakes don't have to
 * match the game's functions: both versions call the same fake. They are close enough to the
 * real ones that the results (hits, misses, stores) drive the tested code through its branches.
 */

#include <cmath>

#include "harness.h"

namespace tests {

using harness::Gen;

inline float bits_f(u64 v) {
  return Mips2C::u2f((u32)v);
}
inline u64 f_bits(float f) {
  return Mips2C::f2gpr(f);
}

//! a vector with lanes in [lo, hi]
inline void rand_vec(Gen& g, float out[4], float lo, float hi, float w) {
  for (int i = 0; i < 3; i++) {
    out[i] = g.f_edge(lo, hi);
  }
  out[3] = w;
}

inline u32 alloc_vec(float x, float y, float z, float w) {
  const u32 a = harness::alloc(16);
  harness::st_vec(a, x, y, z, w);
  return a;
}

inline u32 alloc_vec(const float v[4]) {
  return alloc_vec(v[0], v[1], v[2], v[3]);
}

//! ray-sphere-intersect (origin dir center radius): fraction of dir to the sphere, 0 inside,
//! -100000000.0 on a miss
inline float fake_ray_sphere(const float o_in[4], const float d[4], const float c[4], float r) {
  float o[3] = {o_in[0] - c[0], o_in[1] - c[1], o_in[2] - c[2]};
  const float dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
  const float cc = o[0] * o[0] + o[1] * o[1] + o[2] * o[2] - r * r;
  const float b = d[0] * o[0] + d[1] * o[1] + d[2] * o[2];
  if (cc < 0) {
    return 0.f;
  }
  if (dd == 0 || b >= 0) {
    return -100000000.f;
  }
  const float disc = b * b - cc * dd;
  if (disc < 0) {
    return -100000000.f;
  }
  const float t = -(b + std::sqrt(disc)) / dd;
  if (t > 1.f) {
    return -100000000.f;
  }
  return t;
}

void add_collide_fakes();

}  // namespace tests
