/*!
 * @file tests_goal_collide.cpp
 * (AI-assisted)
 * Tests for the native versions of GOAL collision functions (native_collide_func.cpp,
 * native_geometry.cpp): ray-sphere-intersect, ray-cylinder-intersect,
 * moving-sphere-sphere-intersect, closest-pt-in-triangle. The reference version is the C code
 * goalc makes of the GOAL functions (goalc_ref/).
 */

#include <algorithm>

#include "fakes.h"

namespace tests {

using namespace harness;

namespace {

const float kOrigin[4] = {0.f, 0.f, 0.f, 1.f};

//! a coordinate: usually in [-range, range], sometimes a special value (0, -0, tiny, huge)
float coord(Gen& g, float range) {
  if (g.chance(0.04f)) {
    switch (g.range(0, 5)) {
      case 0:
        return 0.f;
      case 1:
        return -0.f;
      case 2:
        return g.chance(0.5f) ? 1e-30f : -1e-30f;
      case 3:
        return g.chance(0.5f) ? 3e19f : -3e19f;  // squares overflow
      case 4:
        return g.f(-1.f, 1.f);
      default:
        return range;
    }
  }
  return g.f(-range, range);
}

void rand_point(Gen& g, float out[4], const float center[4], float range) {
  for (int i = 0; i < 3; i++) {
    out[i] = center[i] + coord(g, range);
  }
  out[3] = g.chance(0.5f) ? 1.f : g.f(-3.f, 3.f);
}

//! a unit vector (sometimes not quite, or zero)
void rand_dir(Gen& g, float out[4]) {
  float len2 = 0;
  for (int i = 0; i < 3; i++) {
    out[i] = g.f(-1.f, 1.f);
    len2 += out[i] * out[i];
  }
  const float s = len2 > 0 ? 1.f / std::sqrt(len2) : 0.f;
  for (int i = 0; i < 3; i++) {
    out[i] *= s;
  }
  switch (g.range(0, 15)) {
    case 0:
      out[0] = out[1] = out[2] = 0.f;
      break;
    case 1:
      out[0] = out[2] = 0.f;
      out[1] = g.chance(0.5f) ? 1.f : -1.f;
      break;
    case 2:
      for (int i = 0; i < 3; i++) {
        out[i] *= g.f(0.5f, 2.f);
      }
      break;
    default:
      break;
  }
  out[3] = g.chance(0.5f) ? 0.f : g.f(-2.f, 2.f);
}

//! a probe from near the sphere/cylinder at center: aimed at it, past it, or anywhere
void rand_probe(Gen& g, const float center[4], float radius, float origin[4], float dir[4]) {
  const float range = radius * g.pick(std::vector<float>{0.5f, 1.5f, 4.f, 20.f});
  rand_point(g, origin, center, range);
  const int kind = g.range(0, 6);
  for (int i = 0; i < 3; i++) {
    if (kind == 0) {
      dir[i] = 0.f;
    } else if (kind <= 3) {
      // toward the center (and a bit off), short or long
      dir[i] = (center[i] - origin[i]) * g.f(0.3f, 2.5f) + coord(g, radius * 0.5f);
    } else if (kind == 4) {
      dir[i] = (origin[i] - center[i]) * g.f(0.1f, 2.f);  // away
    } else {
      dir[i] = coord(g, range * 2.f);
    }
  }
  dir[3] = g.chance(0.5f) ? 0.f : g.f(-2.f, 2.f);
}

float rand_radius(Gen& g) {
  switch (g.range(0, 9)) {
    case 0:
      return 0.f;
    case 1:
      return -g.f(0.f, 4096.f);
    case 2:
      return g.f(0.f, 10.f);
    default:
      return g.f(100.f, 20000.f);
  }
}

void gen_ray_sphere(Case& c) {
  float center[4], origin[4], dir[4];
  const float radius = rand_radius(c.g);
  rand_point(c.g, center, kOrigin, 100000.f);
  rand_probe(c.g, center, std::max(std::abs(radius), 100.f), origin, dir);
  center[3] = c.g.f(-1.f, 1.f);
  c.args[0] = alloc_vec(origin);
  c.args[1] = alloc_vec(dir);
  c.args[2] = alloc_vec(center);
  c.args[3] = f_bits(radius) | (c.g.chance(0.1f) ? 0x1234567800000000ull : 0);
  for (int i = 4; i < 8; i++) {
    c.args[i] = c.g.u32_();
  }
}

void gen_ray_cylinder(Case& c) {
  float co[4], axis[4], origin[4], dir[4];
  const float radius = rand_radius(c.g);
  const float len = c.g.chance(0.1f) ? c.g.f(-1000.f, 0.f) : c.g.f(0.f, 40000.f);
  rand_point(c.g, co, kOrigin, 100000.f);
  rand_dir(c.g, axis);
  // the probe near some point of the cylinder's axis
  float mid[4];
  const float along = c.g.f(-0.3f, 1.3f) * len;
  for (int i = 0; i < 3; i++) {
    mid[i] = co[i] + axis[i] * along;
  }
  mid[3] = 1.f;
  rand_probe(c.g, mid, std::max(std::abs(radius), 100.f), origin, dir);
  c.args[0] = alloc_vec(origin);
  c.args[1] = alloc_vec(dir);
  c.args[2] = alloc_vec(co);
  c.args[3] = alloc_vec(axis);
  c.args[4] = f_bits(radius);
  c.args[5] = f_bits(len);
  c.args[6] = alloc(16);  // pt-out
  st_vec((u32)c.args[6], 7.f, 7.f, 7.f, 7.f);
  c.args[7] = c.g.u32_();
}

void gen_moving_sphere_sphere(Case& c) {
  float s[4], other[4], move[4];
  rand_point(c.g, other, kOrigin, 100000.f);
  other[3] = rand_radius(c.g);
  const float r = rand_radius(c.g);
  rand_probe(c.g, other, std::max(std::abs(other[3]) + std::abs(r), 100.f), s, move);
  s[3] = r;
  if (c.g.chance(0.05f)) {
    // same center
    memcpy(s, other, 12);
  }
  c.args[0] = alloc_vec(s);
  c.args[1] = alloc_vec(move);
  c.args[2] = alloc_vec(other);
  c.args[3] = alloc(16);  // out
  st_vec((u32)c.args[3], 7.f, 7.f, 7.f, 7.f);
  for (int i = 4; i < 8; i++) {
    c.args[i] = c.g.u32_();
  }
}

//! a triangle (3 vertices, then its normal) and a point near it, in any of the regions
void gen_closest_pt(Case& c) {
  float center[4];
  rand_point(c.g, center, kOrigin, 100000.f);
  float v[3][4];
  const float size = c.g.pick(std::vector<float>{10.f, 2000.f, 20000.f});
  for (int k = 0; k < 3; k++) {
    rand_point(c.g, v[k], center, size);
  }
  switch (c.g.range(0, 12)) {
    case 0:
      memcpy(v[1], v[0], 16);  // two equal vertices
      break;
    case 1:
      for (int i = 0; i < 3; i++) {  // collinear
        v[2][i] = v[0][i] + (v[1][i] - v[0][i]) * 0.5f;
      }
      break;
    case 2:
      memcpy(v[1], v[0], 16);  // a point
      memcpy(v[2], v[0], 16);
      break;
    case 3:
    case 4:
      // a huge coordinate: lengths overflow, distances are inf or NaN, which is where the order
      // in which two edges are compared shows
      v[c.g.range(0, 2)][c.g.range(0, 2)] = c.g.chance(0.5f) ? 3e19f : -3e19f;
      break;
    default:
      break;
  }
  // the normal: (v1 - v0) x (v2 - v0) normalized, its opposite, or anything
  // (inputs stay finite: with NaNs of both signs in the inputs, which NaN comes out of an
  // operation depends on the order of its operands, which the compiler may swap)
  float n[4];
  const double a[3] = {(double)v[1][0] - v[0][0], (double)v[1][1] - v[0][1],
                       (double)v[1][2] - v[0][2]};
  const double b[3] = {(double)v[2][0] - v[0][0], (double)v[2][1] - v[0][1],
                       (double)v[2][2] - v[0][2]};
  double nd[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
  const double len = std::sqrt(nd[0] * nd[0] + nd[1] * nd[1] + nd[2] * nd[2]);
  for (int i = 0; i < 3; i++) {
    n[i] = len > 0 && std::isfinite(len) ? (float)(nd[i] / len) : (i == 1 ? 1.f : 0.f);
  }
  if (c.g.chance(0.2f)) {
    for (int i = 0; i < 3; i++) {
      n[i] = -n[i];
    }
  } else if (c.g.chance(0.1f)) {
    rand_dir(c.g, n);
  }
  n[3] = c.g.chance(0.5f) ? 1.f : bits_f(c.g.u32_() & 0x3f);  // the pat, in the game
  const u32 tri = alloc(64);
  for (int k = 0; k < 3; k++) {
    st_bytes(tri + 16 * k, v[k], 16);
  }
  st_bytes(tri + 48, n, 16);
  // the point: a mix of the vertices (inside or out), off the plane
  float w[3] = {c.g.f(-0.5f, 1.5f), c.g.f(-0.5f, 1.5f), 0};
  if (c.g.chance(0.4f)) {
    // inside
    w[0] = c.g.f(0.f, 1.f);
    w[1] = c.g.f(0.f, 1.f - w[0]);
  }
  w[2] = 1.f - w[0] - w[1];
  float p[4];
  const float off = coord(c.g, size);
  for (int i = 0; i < 3; i++) {
    p[i] = v[0][i] * w[0] + v[1][i] * w[1] + v[2][i] * w[2] + n[i] * off;
  }
  p[3] = 1.f;
  if (c.g.chance(0.05f)) {
    rand_point(c.g, p, center, size * 4);
  }
  if (c.g.chance(0.03f)) {
    memcpy(p, v[c.g.range(0, 2)], 16);  // on a vertex
  }
  const u32 out = alloc(16);
  st_vec(out, 7.f, 7.f, 7.f, 7.f);
  c.args[0] = out;
  c.args[1] = alloc_vec(p);
  c.args[2] = tri;
  // the normal: the triangle's (collide-mesh) or a copy elsewhere (collide-puss-work)
  c.args[3] = c.g.chance(0.5f) ? tri + 48 : alloc_vec(n);
  for (int i = 4; i < 8; i++) {
    c.args[i] = c.g.u32_();
  }
}

}  // namespace

void register_goal_collide_tests() {
  add_test({"ray-sphere-intersect",
            "ray-sphere-intersect",
            30000,
            [] { bind_collide_functions(); },
            gen_ray_sphere,
            {}});
  add_test({"ray-cylinder-intersect",
            "ray-cylinder-intersect",
            30000,
            [] { bind_collide_functions(); },
            gen_ray_cylinder,
            {}});
  add_test({"moving-sphere-sphere-intersect",
            "moving-sphere-sphere-intersect",
            30000,
            [] { bind_collide_functions(); },
            gen_moving_sphere_sphere,
            {}});
  add_test({"closest-pt-in-triangle",
            "closest-pt-in-triangle",
            40000,
            [] { bind_collide_functions(); },
            gen_closest_pt,
            {}});

  // the natives that call these, with the functions redefined in GOAL (here the compiled GOAL
  // code itself): they must call them through the symbol, like the original, instead of directly
  for (const char* name :
       {"moving-sphere-triangle-intersect", "collide-do-primitives",
        "(method 10 collide-cache-prim)", "(method 9 collide-puss-work)",
        "(method 11 collide-mesh)", "(method 12 collide-mesh)", "moving-sphere-sphere-intersect"}) {
    add_test_variant(
        name, std::string(name) + " (GOAL callees)", [] { bind_collide_functions(true); }, 0.25);
  }
}

}  // namespace tests
