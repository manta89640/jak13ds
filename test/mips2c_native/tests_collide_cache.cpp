/*!
 * @file tests_collide_cache.cpp
 * (AI-assisted)
 * Tests for collide_cache.cpp.
 */

#include "collide_gen.h"

namespace Mips2C::jak1 {
const u32* collide_vu0_buffer();  // collide_cache.cpp
}

namespace tests {

using namespace harness;

namespace {

u32* vu0_buffer() {
  return const_cast<u32*>(Mips2C::jak1::collide_vu0_buffer());
}

void setup_cache_common() {
  add_collide_fakes();
  collide_cache_type();
  bind_mips2c_symbol("collide-do-primitives");
  bind_mips2c_symbol("moving-sphere-triangle-intersect");
  set_max_tris(460);
}

u32 random_max_tris(Gen& g) {
  if (g.chance(0.7f)) {
    return 460;
  }
  return (u32)g.range(1, 60);
}

// (method 9 collide-cache-prim) prim result sphere move max-t action
void gen_method_9_prim(Case& c) {
  auto& g = c.g;
  const u32 max_tris = 460;
  set_max_tris(max_tris);
  const u32 cache = gen_cache(g, max_tris);
  float center[4];
  rand_vec(g, center, -60000.f, 60000.f, 1.f);
  const int first = g.range(0, 40);
  const int count = g.chance(0.05f) ? 0 : g.range(1, 24);
  const float size = g.pick(std::vector<float>{3000.f, 15000.f, 40000.f});
  for (int i = 0; i < count; i++) {
    gen_cache_tri(g, cache + kCacheTris + 64 * (first + i), center, size);
  }
  const u32 prim = alloc(48);
  for (int i = 0; i < 48; i += 4) {
    st<u32>(prim + i, g.u32_());
  }
  st<u32>(prim + 32, cache);
  st<u16>(prim + 40, (u16)first);
  st<u16>(prim + 42, (u16)count);

  float pos[4], move[4];
  const float radius = g.pick(std::vector<float>{0.f, 1200.f, 4096.f, 9000.f});
  for (int i = 0; i < 3; i++) {
    pos[i] = center[i] + g.f_edge(-30000.f, 30000.f);
    move[i] = g.chance(0.5f) ? (center[i] - pos[i]) * g.f(0.2f, 2.f) : g.f(-40000.f, 40000.f);
  }
  pos[3] = radius;
  move[3] = g.chance(0.5f) ? 0.f : 1.f;
  const u32 sphere = alloc(32);
  st_bytes(sphere, pos, 16);
  for (int i = 16; i < 32; i += 4) {
    st<u32>(sphere + i, g.u32_());
  }
  c.args[0] = prim;
  c.args[1] = alloc(96);  // collide-tri-result
  c.args[2] = sphere;
  c.args[3] = alloc_vec(move);
  const float max_t = g.pick(std::vector<float>{-1.f, 0.f, 0.3f, 1.f, 2.f, 5.f});
  c.args[4] = f_bits(g.chance(0.2f) ? g.f(-1.f, 3.f) : max_t);
  c.args[5] = g.chance(0.5f) ? 1 : g.u32_();
  c.args[6] = g.u32_();
  c.args[7] = g.u32_();
}

//! a cache and a mesh whose scratchpad vertices are there, for methods 26 / 27
u32 gen_mesh_and_cache(Case& c, int& vertex_count) {
  auto& g = c.g;
  const u32 max_tris = random_max_tris(g);
  set_max_tris(max_tris);
  const u32 cache = gen_cache(g, max_tris);
  if (g.chance(0.1f)) {
    // already over the limit
    st<u32>(cache, max_tris + (u32)g.range(0, 3));
  } else if (g.chance(0.2f)) {
    st<u32>(cache, max_tris - (u32)g.range(0, std::min<s32>(3, (s32)max_tris)));
  }
  st<u32>(cache + 8, g.chance(0.5f) ? 0 : g.u32_() & g.u32_());  // ignore-mask
  vertex_count = g.range(3, 127);
  const u32 mesh = gen_frag_mesh(g, vertex_count);
  // box: overlap some triangles
  const s32 range = 1000;
  for (int i = 0; i < 4; i++) {
    s32 a = g.range(-range, range), b = g.range(-range, range);
    if (a > b && g.chance(0.9f)) {
      std::swap(a, b);
    }
    st<s32>(cache + 60 + 4 * i, a);
    st<s32>(cache + 76 + 4 * i, b);
  }
  gen_spad_vertices(g, 128, range);
  c.args[0] = cache;
  c.args[1] = mesh;
  for (int i = 2; i < 8; i++) {
    c.args[i] = g.u32_();
  }
  return cache;
}

}  // namespace

void register_collide_cache_tests() {
  add_test({"(method 9 collide-cache-prim)", "(method 9 collide-cache-prim)", 4000,
            setup_cache_common, gen_method_9_prim, {}});

  add_test({"(method 26 collide-cache)", "(method 26 collide-cache)", 6000, setup_cache_common,
            [](Case& c) {
              int n;
              gen_mesh_and_cache(c, n);
            },
            {}});

  add_test({"(method 29 collide-cache)", "(method 29 collide-cache)", 3000, setup_cache_common,
            [](Case& c) {
              int n;
              gen_mesh_and_cache(c, n);
              gen_collide_work(c.g, 1000);
              // floats in the scratchpad are what method 29 transforms
              for (int k = 0; k < 128; k++) {
                for (int i = 0; i < 4; i++) {
                  st<float>(kSpad + 32 * k + 16 + 4 * i, c.g.f_edge(-3000.f, 3000.f));
                }
              }
            },
            {}});

  add_test({"(method 27 collide-cache)", "(method 27 collide-cache)", 6000, setup_cache_common,
            [](Case& c) {
              int n;
              gen_mesh_and_cache(c, n);
              gen_collide_work(c.g, 1000);
            },
            {}});

  add_test(
      {"(method 32 collide-cache)", "(method 32 collide-cache)", 4000, setup_cache_common,
       [](Case& c) {
         auto& g = c.g;
         const int n = g.range(1, 127);
         const u32 mesh = gen_frag_mesh(g, n);
         // the unpacked vertices from __pc-upload-collide-frag (and older ones after them)
         u32* buf = vu0_buffer();
         for (int i = 0; i < 1024; i += 4) {
           buf[i] = 0x4d000000 + (g.u32_() & 0xffff);
           buf[i + 1] = 0x4d000000 + (g.u32_() & 0xffff);
           buf[i + 2] = 0x4d000000 + (g.u32_() & 0xffff);
           buf[i + 3] = 0x3f800000;
         }
         u32 xf = st();
         if (g.chance(0.6f)) {
           xf = alloc_basic(0, 64);
           for (int i = 0; i < 64; i += 2) {
             st<s16>(xf + i, (s16)g.range(-4096, 4096));
           }
           st_vec(xf + 12, g.f(-1e5f, 1e5f), g.f(-1e5f, 1e5f), g.f(-1e5f, 1e5f), g.f(-2, 2));
         }
         c.args[0] = alloc_basic(collide_cache_type(), 16);
         c.args[1] = mesh;
         c.args[2] = xf;
         c.args[3] = 0;
       },
       {{vu0_buffer(), 4096}}});
}

}  // namespace tests
