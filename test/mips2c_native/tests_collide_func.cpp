/*!
 * @file tests_collide_func.cpp
 * (AI-assisted)
 * Tests for collide_func.cpp: moving-sphere-triangle-intersect, collide-do-primitives.
 */

#include "fakes.h"

namespace tests {

using namespace harness;

namespace {

//! a collide-cache-tri (3 vertices, then the pat) near a random point. Sometimes degenerate.
u32 gen_tri(Gen& g, float center[4]) {
  const u32 tri = alloc(64);
  rand_vec(g, center, -60000.f, 60000.f, 1.f);
  float v[3][4];
  const float size = g.pick(std::vector<float>{2000.f, 12000.f, 40000.f});
  for (int k = 0; k < 3; k++) {
    for (int i = 0; i < 3; i++) {
      v[k][i] = center[i] + g.f_edge(-size, size);
    }
    v[k][3] = g.chance(0.5f) ? 1.f : g.f(-2.f, 2.f);
  }
  switch (g.range(0, 12)) {
    case 0:  // two equal vertices
      memcpy(v[1], v[0], 16);
      break;
    case 1:  // collinear
      for (int i = 0; i < 3; i++) {
        v[2][i] = v[0][i] + (v[1][i] - v[0][i]) * 0.5f;
      }
      break;
    case 2:  // flat in y (floors)
      v[1][1] = v[0][1];
      v[2][1] = v[0][1];
      break;
    default:
      break;
  }
  for (int k = 0; k < 3; k++) {
    st_bytes(tri + 16 * k, v[k], 16);
  }
  st<u32>(tri + 48, g.u32_());
  return tri;
}

//! a sphere center and move near the triangle
void gen_sweep(Gen& g, const float center[4], float pos[4], float move[4], float& radius) {
  radius = g.pick(std::vector<float>{0.f, 800.f, 2048.f, 4096.f, 12000.f});
  if (g.chance(0.3f)) {
    radius = g.f(0.f, 16000.f);
  }
  for (int i = 0; i < 3; i++) {
    pos[i] = center[i] + g.f_edge(-30000.f, 30000.f);
  }
  pos[3] = g.chance(0.5f) ? radius : g.f(-5000.f, 5000.f);
  const int kind = g.range(0, 5);
  for (int i = 0; i < 3; i++) {
    if (kind == 0) {
      move[i] = 0.f;
    } else if (kind <= 2) {
      // toward the triangle's center, overshooting or not
      move[i] = (center[i] - pos[i]) * g.f(0.2f, 2.5f);
    } else {
      move[i] = g.f_edge(-40000.f, 40000.f);
    }
  }
  move[3] = g.chance(0.5f) ? 0.f : g.f(-3.f, 3.f);
}

void gen_msti(Case& c) {
  float center[4], pos[4], move[4], radius;
  const u32 tri = gen_tri(c.g, center);
  gen_sweep(c.g, center, pos, move, radius);
  c.args[0] = alloc_vec(pos);
  c.args[1] = alloc_vec(move);
  c.args[2] = f_bits(radius);
  c.args[3] = tri;
  c.args[4] = alloc(16);  // out-point
  c.args[5] = alloc(16);  // out-normal
  c.args[6] = c.g.u32_();
  c.args[7] = c.g.u32_();
}

}  // namespace

void register_collide_func_tests() {
  add_test({"moving-sphere-triangle-intersect", "moving-sphere-triangle-intersect", 20000,
            [] {
              add_collide_fakes();
              bind_mips2c_symbol("collide-do-primitives");
              bind_mips2c_symbol("moving-sphere-triangle-intersect");
            },
            gen_msti,
            {}});

  add_test({"collide-do-primitives", "collide-do-primitives", 20000,
            [] {
              add_collide_fakes();
              bind_mips2c_symbol("collide-do-primitives");
            },
            [](Case& c) {
              gen_msti(c);
              // args 5-7 are whatever the caller had in t1-t3
              c.args[5] = c.g.u32_();
            },
            {}});
}

}  // namespace tests
