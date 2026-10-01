#pragma once

/*!
 * @file fakes.h
 * (AI-assisted)
 * Fake GOAL functions and shared generators for the differential tests. The fakes don't have to
 * match the game's functions: both versions call the same fake. They are close enough to the
 * real ones that the results (hits, misses, stores) drive the tested code through its branches.
 * GOAL functions that have a native version are not faked: their reference is the C code goalc
 * makes of them (goalc_ref/).
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

/*!
 * Set the symbols of the GOAL collision functions with a native version (ray-sphere-intersect,
 * ray-cylinder-intersect, moving-sphere-sphere-intersect, closest-pt-in-triangle). Their reference
 * version is goalc's C code (goalc_ref/). With compiled_goal, the symbols hold that code instead of
 * the native functions' stubs, like after a GOAL redefinition: natives then call them through the
 * symbol (logged and compared like calls to fakes) instead of directly.
 */
void bind_collide_functions(bool compiled_goal = false);

}  // namespace tests
