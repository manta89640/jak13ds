/*!
 * @file tests_collide_mesh.cpp
 * (AI-assisted)
 * Tests for collide_mesh.cpp: methods 11, 12, 14 and 15 of collide-mesh.
 */

#include "collide_gen.h"

namespace tests {

using namespace harness;

u32 collide_mesh_type();

namespace {

//! a matrix: rotation-ish rows, then a translation
u32 gen_matrix(Gen& g, float trans) {
  const u32 m = alloc(64);
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      const float v =
          r == 3 ? g.f_edge(-trans, trans) : (r == c ? g.f(0.5f, 1.5f) : g.f(-0.7f, 0.7f));
      st<float>(m + 16 * r + 4 * c, g.chance(0.02f) ? 0.f : v);
    }
  }
  return m;
}

//! a collide-mesh with n vertices (floats at +12, count at +8) and ntris triangles at +28
u32 gen_mesh(Gen& g, u32 type, int n, int ntris, float range) {
  const u32 mesh = alloc_basic(type, 28 + 8 * std::max(ntris, 1));
  const u32 verts = alloc(16 * (n + 12));
  for (int k = 0; k < n + 12; k++) {
    st_vec(verts + 16 * k, g.f_edge(-range, range), g.f_edge(-range, range),
           g.f_edge(-range, range), g.chance(0.8f) ? 1.f : g.f(-2, 2));
  }
  st<u32>(mesh + 4, (u32)ntris);
  st<u32>(mesh + 8, (u32)n);
  st<u32>(mesh + 12, verts);
  for (int t = 0; t < ntris; t++) {
    for (int k = 0; k < 3; k++) {
      st<u8>(mesh + 28 + 8 * t + k, (u8)g.range(0, std::max(n - 1, 0)));
    }
    st<u8>(mesh + 28 + 8 * t + 3, (u8)g.u32_());
    st<u32>(mesh + 28 + 8 * t + 4, g.u32_() & (g.chance(0.5f) ? 0xff : 0xffffffff));
  }
  return mesh;
}

// methods 11 and 12: (this tris result sphere best)
void gen_mesh_sphere(Case& c) {
  auto& g = c.g;
  float center[4];
  rand_vec(g, center, -60000.f, 60000.f, 1.f);
  const float size = g.pick(std::vector<float>{2000.f, 8000.f, 30000.f});
  const int ntris = g.chance(0.05f) ? 0 : g.range(1, 24);
  const u32 mesh = alloc_basic(collide_mesh_type(), 32);
  st<u32>(mesh + 4, (u32)ntris);
  const u32 tris = alloc(96 * (ntris + 1));
  for (int t = 0; t < ntris; t++) {
    const u32 tri = tris + 96 * t;
    float v[3][4];
    for (int k = 0; k < 3; k++) {
      for (int i = 0; i < 3; i++) {
        v[k][i] = center[i] + g.f_edge(-size, size);
      }
      v[k][3] = g.chance(0.5f) ? 1.f : g.f(-2, 2);
      st_bytes(tri + 16 * k, v[k], 16);
    }
    // normal (unit, sometimes not), the pat in w
    float n[3] = {g.f(-1, 1), g.f(-1, 1), g.f(-1, 1)};
    const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) + 1e-6f;
    for (int i = 0; i < 3; i++) {
      st<float>(tri + 48 + 4 * i, g.chance(0.9f) ? n[i] / len : n[i]);
    }
    const u32 mode = g.pick(std::vector<u32>{0, 0, 16, 8, 24, 32, 56});
    st<u32>(tri + 60, (g.u32_() & ~56u) | mode);
    for (int i = 0; i < 4; i++) {
      const float lo = std::min(std::min(v[0][i], v[1][i]), v[2][i]);
      const float hi = std::max(std::max(v[0][i], v[1][i]), v[2][i]);
      st<s32>(tri + 64 + 4 * i, (s32)lo);
      st<s32>(tri + 80 + 4 * i, (s32)hi);
    }
  }
  float s[4];
  for (int i = 0; i < 3; i++) {
    s[i] = center[i] + g.f_edge(-size, size);
  }
  s[3] = g.f(0.f, size * 0.6f);
  c.args[0] = mesh;
  c.args[1] = tris;
  c.args[2] = alloc(96);
  c.args[3] = alloc_vec(s);
  c.args[4] = g.chance(0.2f) ? ((u64)g.u32_() << 32) | Mips2C::f2u(1e10f)
                             : f_bits(g.pick(std::vector<float>{1e10f, 1000.f, 0.f, -300.f}));
  for (int i = 5; i < 8; i++) {
    c.args[i] = g.u32_();
  }
}

// methods 14 and 15: (this bone out) / (this bone inv-mat out)
void gen_mesh_vertices(Case& c, bool inv) {
  auto& g = c.g;
  const int n = g.chance(0.05f) ? 0 : g.range(1, 64);
  c.args[0] = gen_mesh(g, collide_mesh_type(), n, 0, 30000.f);
  c.args[1] = gen_matrix(g, 1e5f);
  if (inv) {
    c.args[2] = gen_matrix(g, 1e4f);
    c.args[3] = kSpad;
  } else {
    c.args[2] = kSpad;
    c.args[3] = g.u32_();
  }
  for (int i = 4; i < 8; i++) {
    c.args[i] = g.u32_();
  }
}

}  // namespace

u32 collide_mesh_type() {
  static u32 type = 0;
  if (!type) {
    type = make_type("collide-mesh", 16);
    for (int m : {11, 12, 14, 15}) {
      set_method(type, m, mips2c_stub(fmt::format("(method {} collide-mesh)", m)));
    }
  }
  return type;
}

void register_collide_mesh_tests() {
  auto setup = [] {
    add_collide_fakes();
    collide_mesh_type();
  };
  add_test({"(method 11 collide-mesh)", "(method 11 collide-mesh)", 8000, setup, gen_mesh_sphere,
            {}});
  add_test({"(method 12 collide-mesh)", "(method 12 collide-mesh)", 8000, setup, gen_mesh_sphere,
            {}});
  add_test({"(method 14 collide-mesh)", "(method 14 collide-mesh)", 4000, setup,
            [](Case& c) { gen_mesh_vertices(c, false); }, {}});
  add_test({"(method 15 collide-mesh)", "(method 15 collide-mesh)", 4000, setup,
            [](Case& c) { gen_mesh_vertices(c, true); }, {}});
}

//! for the collide-shape-prim-mesh tests: a real collide-mesh (its methods are the mips2c ones)
u32 gen_real_collide_mesh(Gen& g, int nverts, int ntris, float range) {
  return gen_mesh(g, collide_mesh_type(), nverts, ntris, range);
}

u32 gen_bone_matrix(Gen& g) {
  return gen_matrix(g, 3000.f);
}

}  // namespace tests
