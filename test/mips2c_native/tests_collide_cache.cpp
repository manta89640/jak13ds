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

// tests_collide_mesh.cpp
u32 collide_mesh_type();
u32 gen_real_collide_mesh(Gen& g, int nverts, int ntris, float range);
u32 gen_bone_matrix(Gen& g);

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

void gen_print_syms(Gen& g);
void gen_method_26_28(Case& c);

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

//! symbols for the overflow printfs
void gen_print_syms(Gen& g) {
  set_sym("*already-printed-exeeded-max-cache-tris*", g.chance(0.5f) ? st() : true_sym());
  set_sym("*cheat-mode*", g.chance(0.3f) ? true_sym() : st());
}

//! (method 28 collide-cache): the same as 26
void gen_method_26_28(Case& c) {
  int n;
  gen_mesh_and_cache(c, n);
  gen_print_syms(c.g);
}

//! a cache with count triangles near center at first, and a prim for them
u32 gen_prim_tris(Gen& g, u32 cache, const float center[4], float size, int& first, int& count) {
  first = g.range(0, 40);
  count = g.chance(0.05f) ? 0 : g.range(1, 20);
  for (int i = 0; i < count + 1; i++) {
    gen_cache_tri(g, cache + kCacheTris + 64 * (first + i), center, size);
  }
  const u32 prim = alloc(48);
  for (int i = 0; i < 48; i += 4) {
    st<u32>(prim + i, g.u32_());
  }
  st<u32>(prim + 32, cache);
  st<u16>(prim + 40, (u16)first);
  st<u16>(prim + 42, (u16)count);
  return prim;
}

// (method 30 collide-cache) cache work prim: the y probe
void gen_method_30(Case& c) {
  auto& g = c.g;
  const u32 cache = gen_cache(g, 460);
  float center[4];
  rand_vec(g, center, -60000.f, 60000.f, 1.f);
  int first, count;
  const float size = g.pick(std::vector<float>{3000.f, 15000.f, 40000.f});
  const u32 prim = gen_prim_tris(g, cache, center, size, first, count);
  // flatter triangles, like floors
  for (int i = 0; i < count; i++) {
    const u32 tri = cache + kCacheTris + 64 * (first + i);
    if (g.chance(0.6f)) {
      for (int k = 0; k < 3; k++) {
        st<float>(tri + 16 * k + 4, center[1] + g.f(-size * 0.2f, size * 0.2f));
      }
    }
    if (g.chance(0.5f)) {
      st<u32>(tri + 48, g.u32_() & 0xff);
    }
  }
  const u32 work = alloc(48);
  st<float>(work, g.pick(std::vector<float>{1.f, 0.5f, 2.f, 1e10f}));
  st<u32>(work + 4, g.chance(0.5f) ? 0 : g.u32_() & 0x3f);
  const u32 out = alloc(96);
  st<u32>(work + 8, out);
  const float h = g.f(0.f, 3.f * size);
  st_vec(work + 16, center[0] + g.f_edge(-size, size), center[1] + h,
         center[2] + g.f_edge(-size, size), g.f(-1, 1));
  if (g.chance(0.8f)) {
    st_vec(work + 32, g.chance(0.2f) ? g.f(-100, 100) : 0.f, -g.f(0.f, 4.f * size),
           g.chance(0.2f) ? g.f(-100, 100) : 0.f, g.f(-1, 1));
  } else {
    st_vec(work + 32, g.f_edge(-size, size), g.f_edge(-size, size), g.f_edge(-size, size), 0.f);
  }
  c.args[0] = cache;
  c.args[1] = work;
  c.args[2] = prim;
  for (int i = 3; i < 8; i++) {
    c.args[i] = g.u32_();
  }
}

// (method 10 collide-cache-prim) prim result sphere move max-t action
void gen_method_10_prim(Case& c) {
  auto& g = c.g;
  float center[4];
  rand_vec(g, center, -60000.f, 60000.f, g.f(0.f, 20000.f));
  const u32 prim = alloc(48);
  for (int i = 0; i < 48; i += 4) {
    st<u32>(prim + i, g.u32_());
  }
  st_bytes(prim, center, 16);
  const u32 shape_prim = alloc(80);
  st<u32>(shape_prim + 68, g.u32_());
  st<u32>(prim + 36, shape_prim);
  const u32 sphere = alloc(32);
  const float r = g.f(0.f, 12000.f);
  float pos[4], move[4];
  for (int i = 0; i < 3; i++) {
    pos[i] = center[i] + g.f_edge(-50000.f, 50000.f);
    move[i] = g.chance(0.6f) ? (center[i] - pos[i]) * g.f(0.2f, 2.f) : g.f_edge(-40000.f, 40000.f);
  }
  if (g.chance(0.05f)) {
    // straight up or down: the tangent vector's special case
    pos[0] = center[0];
    pos[2] = center[2];
  }
  pos[3] = r;
  move[3] = g.f(-1, 1);
  st_bytes(sphere, pos, 16);
  c.args[0] = prim;
  c.args[1] = alloc(96);
  c.args[2] = sphere;
  c.args[3] = alloc_vec(move);
  c.args[4] = f_bits(g.pick(std::vector<float>{-1.f, 0.f, 0.3f, 1.f, 2.f}));
  c.args[5] = g.u32_();
  c.args[6] = g.u32_();
  c.args[7] = g.u32_();
}

//! a collide-puss-work with n spheres near center (bsphere, then its int bounding box)
u32 gen_puss_work(Gen& g, int n, const float center[4], float size) {
  const u32 work = alloc(96 + 48 * (n + 4));
  for (int i = 0; i < 96; i += 4) {
    st<u32>(work + i, g.u32_());
  }
  s32 bmin[4] = {INT32_MAX, INT32_MAX, INT32_MAX, 0},
      bmax[4] = {INT32_MIN, INT32_MIN, INT32_MIN, 0};
  for (int k = 0; k < n + 4; k++) {
    const u32 s = work + 96 + 48 * k;
    float p[4];
    for (int i = 0; i < 3; i++) {
      p[i] = center[i] + g.f_edge(-size, size);
    }
    p[3] = g.f(0.f, size * 0.3f);
    st_bytes(s, p, 16);
    for (int i = 0; i < 4; i++) {
      const s32 lo = (s32)(p[i] - p[3]), hi = (s32)(p[i] + p[3]);
      st<s32>(s + 16 + 4 * i, g.chance(0.05f) ? g.range(-100000, 100000) : lo);
      st<s32>(s + 32 + 4 * i, hi);
      if (k < n && i < 3) {
        bmin[i] = std::min(bmin[i], lo);
        bmax[i] = std::max(bmax[i], hi);
      }
    }
  }
  if (g.chance(0.1f)) {
    for (int i = 0; i < 3; i++) {
      bmin[i] = g.range(-100000, 100000);
      bmax[i] = bmin[i] + g.range(-100, 50000);
    }
  }
  st_bytes(work + 64, bmin, 16);
  st_bytes(work + 80, bmax, 16);
  return work;
}

// (method 9 collide-puss-work) work prim params
void gen_puss_9(Case& c) {
  auto& g = c.g;
  const u32 cache = gen_cache(g, 460);
  float center[4];
  rand_vec(g, center, -60000.f, 60000.f, 1.f);
  const float size = g.pick(std::vector<float>{3000.f, 15000.f, 40000.f});
  int first, count;
  const u32 prim = gen_prim_tris(g, cache, center, size, first, count);
  const int n = g.chance(0.05f) ? 0 : g.range(1, 8);
  const u32 params = alloc(32);
  st<u32>(params + 4, (u32)n);
  c.args[0] = gen_puss_work(g, n, center, size);
  c.args[1] = prim;
  c.args[2] = params;
  for (int i = 3; i < 8; i++) {
    c.args[i] = g.u32_();
  }
}

// (method 10 collide-puss-work) work sphere params
void gen_puss_10(Case& c) {
  auto& g = c.g;
  float center[4];
  rand_vec(g, center, -60000.f, 60000.f, 1.f);
  const float size = g.pick(std::vector<float>{3000.f, 15000.f, 40000.f});
  const int n = g.chance(0.05f) ? 0 : g.range(1, 9);
  const u32 params = alloc(32);
  st<u32>(params + 4, (u32)n);
  c.args[0] = gen_puss_work(g, n, center, size);
  float s[4];
  for (int i = 0; i < 3; i++) {
    s[i] = center[i] + g.f_edge(-size, size);
  }
  s[3] = g.f(0.f, size * 0.5f);
  c.args[1] = alloc_vec(s);
  c.args[2] = params;
  for (int i = 3; i < 8; i++) {
    c.args[i] = g.u32_();
  }
}

//! the mesh methods that put the mesh's vertices on the scratchpad: copy the vertices the test
//! made (count at mesh + 8, data at mesh + 12)
void add_mesh_method_fakes() {
  static bool done = false;
  if (done) {
    return;
  }
  done = true;
  auto copy = [](u32 mesh) {
    const u32 n = ld<u32>(mesh + 8);
    const u32 data = ld<u32>(mesh + 12);
    memcpy(hptr(kSpad), hptr(data), 32 * n);
  };
  const u32 m14 = add_goal_fn("(method 14 collide-mesh) fake", 3, [copy](const u64* a) -> u64 {
    copy((u32)a[0]);
    return 0;
  });
  const u32 m15 = add_goal_fn("(method 15 collide-mesh) fake", 4, [copy](const u64* a) -> u64 {
    copy((u32)a[0]);
    return 0;
  });
  const u32 type = make_type("collide-mesh-fake", 16);
  set_method(type, 14, m14);
  set_method(type, 15, m15);
}

// methods 12, 13, 14 of collide-shape-prim-mesh: prim cache
void gen_prim_mesh(Case& c) {
  auto& g = c.g;
  gen_print_syms(g);
  const u32 max_tris = g.chance(0.6f) ? 460 : (u32)g.range(1, 60);
  set_max_tris(max_tris);
  const u32 cache = gen_cache(g, max_tris + 64);
  st<u32>(cache + 4, g.chance(0.05f) ? 100 : (u32)g.range(0, 99));  // num-prims
  const u32 nt = ld<u32>(cache);
  if (g.chance(0.1f)) {
    st<u32>(cache, max_tris + (u32)g.range(0, 3));
  } else if (g.chance(0.2f)) {
    st<u32>(cache, max_tris - std::min<u32>(max_tris, (u32)g.range(0, 5)));
  } else {
    st<u32>(cache, std::min(nt, max_tris));
  }
  st<u32>(cache + 8, g.chance(0.5f) ? 0 : g.u32_() & g.u32_());
  const s32 range = 2000;
  for (int i = 0; i < 4; i++) {
    s32 a = g.range(-range, range), b = g.range(-range, range);
    if (a > b && g.chance(0.9f)) {
      std::swap(a, b);
    }
    st<s32>(cache + 60 + 4 * i, a);
    st<s32>(cache + 76 + 4 * i, b);
  }
  gen_collide_work(g, range);

  // the mesh: triangles (3 vertex indices, pat), and the vertices its method writes
  const int nverts = g.range(3, 64);
  const int ntris = g.chance(0.05f) ? 0 : g.range(1, 60);
  const bool real_mesh = g.chance(0.5f);
  u32 mesh;
  if (real_mesh) {
    // collide-mesh with the mips2c / native methods 14 and 15
    mesh = gen_real_collide_mesh(g, nverts, ntris, (float)range);
  } else {
    const u32 type = sym_value("collide-mesh-fake");
    mesh = alloc_basic(type, 28 + 8 * 64);
    st<u32>(mesh + 4, (u32)ntris);
    st<u32>(mesh + 8, (u32)nverts);
    const u32 verts = alloc(32 * nverts);
    for (int k = 0; k < nverts; k++) {
      for (int i = 0; i < 4; i++) {
        st<float>(verts + 32 * k + 4 * i, g.f(-1e5f, 1e5f));
        st<s32>(verts + 32 * k + 16 + 4 * i, g.range(-range, range));
      }
    }
    st<u32>(mesh + 12, verts);
    for (int t = 0; t < ntris; t++) {
      for (int k = 0; k < 3; k++) {
        st<u8>(mesh + 28 + 8 * t + k, (u8)g.range(0, nverts - 1));
      }
      st<u8>(mesh + 28 + 8 * t + 3, (u8)g.u32_());
      st<u32>(mesh + 28 + 8 * t + 4, g.u32_() & (g.chance(0.5f) ? 0xff : 0xffffffff));
    }
  }

  // the prim and the joint it follows: prim cshape -> process (at 136) -> node-list (at 112)
  const s32 joint = g.range(-1, 6);
  const u32 node_list = alloc(32 * 10) + 32;
  for (int k = -1; k < 8; k++) {
    // the bone: a matrix for the real mesh methods, only passed on to the fake ones
    st<u32>(node_list + 32 * k + 28, real_mesh ? gen_bone_matrix(g) : g.u32_());
  }
  const u32 proc = alloc(128);
  st<u32>(proc + 112, node_list);
  const u32 cshape = alloc(160);
  st<u32>(cshape + 136, proc);
  const u32 prim = alloc_basic(0, 80);
  for (int i = 0; i < 80; i += 4) {
    st<u32>(prim + i, g.u32_());
  }
  st<u32>(prim, cshape);
  st<s8>(prim + 8, (s8)joint);
  st<u32>(prim + 68, g.chance(0.05f) ? st() : mesh);
  c.args[0] = prim;
  c.args[1] = cache;
  for (int i = 2; i < 8; i++) {
    c.args[i] = g.u32_();
  }
}

void setup_prim_mesh() {
  setup_cache_common();
  add_mesh_method_fakes();
  collide_mesh_type();
}

void setup_mssi() {
  setup_cache_common();
}

}  // namespace

void register_collide_cache_tests() {
  add_test({"(method 9 collide-cache-prim)",
            "(method 9 collide-cache-prim)",
            4000,
            setup_cache_common,
            gen_method_9_prim,
            {}});

  add_test({"(method 26 collide-cache)",
            "(method 26 collide-cache)",
            6000,
            setup_cache_common,
            gen_method_26_28,
            {}});

  add_test({"(method 29 collide-cache)",
            "(method 29 collide-cache)",
            3000,
            setup_cache_common,
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

  add_test({"(method 27 collide-cache)",
            "(method 27 collide-cache)",
            6000,
            setup_cache_common,
            [](Case& c) {
              int n;
              gen_mesh_and_cache(c, n);
              gen_collide_work(c.g, 1000);
              gen_print_syms(c.g);
            },
            {}});

  add_test({"(method 32 collide-cache)",
            "(method 32 collide-cache)",
            4000,
            setup_cache_common,
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

  add_test({"(method 28 collide-cache)",
            "(method 28 collide-cache)",
            6000,
            setup_cache_common,
            gen_method_26_28,
            {}});
  add_test({"(method 30 collide-cache)",
            "(method 30 collide-cache)",
            8000,
            setup_cache_common,
            gen_method_30,
            {}});
  add_test({"(method 10 collide-cache-prim)",
            "(method 10 collide-cache-prim)",
            8000,
            setup_mssi,
            gen_method_10_prim,
            {}});
  add_test({"(method 9 collide-puss-work)",
            "(method 9 collide-puss-work)",
            6000,
            setup_mssi,
            gen_puss_9,
            {}});
  add_test({"(method 10 collide-puss-work)",
            "(method 10 collide-puss-work)",
            8000,
            setup_mssi,
            gen_puss_10,
            {}});
  for (int m : {12, 13, 14}) {
    const std::string name = fmt::format("(method {} collide-shape-prim-mesh)", m);
    add_test({name, name, 5000, setup_prim_mesh, gen_prim_mesh, {}});
  }
  add_test({"__pc-upload-collide-frag",
            "__pc-upload-collide-frag",
            3000,
            setup_cache_common,
            [](Case& c) {
              auto& g = c.g;
              const int n = g.range(0, 128);
              const u32 data = alloc(6 * 128 + 16);
              for (int i = 0; i < 3 * 128; i++) {
                st<u16>(data + 2 * i, (u16)g.u32_());
              }
              u32* buf = vu0_buffer();
              for (int i = 0; i < 1024; i++) {
                buf[i] = g.u32_();
              }
              c.args[0] = data;
              c.args[1] = g.u32_();
              c.args[2] = (u64)n;
            },
            {{vu0_buffer(), 4096}}});
}

}  // namespace tests
