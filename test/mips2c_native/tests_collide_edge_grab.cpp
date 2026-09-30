/*!
 * @file tests_collide_edge_grab.cpp
 * (AI-assisted)
 * Tests for collide_edge_grab.cpp: collide-edge-work methods 15, 16 and 18, collide-edge-hold-list
 * method 10.
 */

#include <array>

#include "collide_gen.h"

namespace tests {

using namespace harness;

namespace {

constexpr u32 kWorkSize = 7808 + 2112;  // up to the end of the hold list

void setup_edge() {
  add_collide_fakes();
  collide_cache_type();
  // collide-edge-work: method 17 should-add-to-list? (this item edge), method 19
  // check-grab-for-collisions (this item info)
  const u32 work_type = make_type("collide-edge-work", 20);
  set_method(work_type, 17, add_goal_fn("should-add-to-list?", 3, [](const u64* a) -> u64 {
               const u32 item = (u32)a[1];
               float c[4];
               memcpy(c, hptr(item + 16), 16);
               // a rating from the point, and a yes/no from its bits
               st<float>(item + 4, c[0] * 0.001f + c[2] * 0.0003f);
               st<u32>(item + 12, (u32)a[2]);
               return (Mips2C::f2u(c[0]) ^ Mips2C::f2u(c[1])) & 4 ? st() : true_sym();
             }));
  set_method(work_type, 19, add_goal_fn("check-grab-for-collisions", 3, [](const u64* a) -> u64 {
               const u32 item = (u32)a[1];
               const u32 info = (u32)a[2];
               const u32 h = ld<u32>(item + 16) * 2654435761u + ld<u32>(item + 24);
               st<u32>(info, h);
               return (h >> 13) % 7 == 0 ? true_sym() : st();
             }));
  const u32 list_type = make_type("collide-edge-hold-list", 11);
  set_method(list_type, 10, mips2c_stub("(method 10 collide-edge-hold-list)"));
}

//! a cache of triangles around center, most of them flat-ish floors
u32 gen_edge_cache(Gen& g, const float center[4], float size, int count) {
  const u32 cache = gen_cache(g, 460);
  st<u32>(cache, (u32)count);
  // shared points so that triangles share vertices and edges
  std::vector<std::array<float, 4>> pts(12);
  for (auto& p : pts) {
    p = {center[0] + g.f_edge(-size, size), center[1] + g.f(-size * 0.1f, size * 0.1f),
         center[2] + g.f_edge(-size, size), g.chance(0.8f) ? 1.f : g.f(-2, 2)};
  }
  for (int i = 0; i < count; i++) {
    const u32 tri = cache + kCacheTris + 64 * i;
    if (g.chance(0.7f)) {
      int k[3] = {g.range(0, 11), g.range(0, 11), g.range(0, 11)};
      for (int j = 0; j < 3; j++) {
        std::array<float, 4> p = pts[k[j]];
        if (g.chance(0.1f)) {
          p[0] += g.f(-30.f, 30.f);  // merged (within sqrt(1677.7216)) or not
        }
        st_bytes(tri + 16 * j, p.data(), 16);
      }
      for (int j = 48; j < 64; j += 4) {
        st<u32>(tri + j, g.u32_());
      }
    } else {
      gen_cache_tri(g, tri, center, size);
    }
    const u32 mode = g.pick(std::vector<u32>{0, 0, 0, 16, 8, 56});
    u32 pat = (g.u32_() & ~(56u | 5u)) | mode;
    if (g.chance(0.05f)) {
      pat |= g.chance(0.5f) ? 1 : 4;  // noentity / noedge
    }
    if (g.chance(0.1f)) {
      pat = (pat & ~(63u << 14)) | (2u << 14);
    }
    st<u32>(tri + 48, pat);
  }
  return cache;
}

// (method 16 collide-edge-work) work
void gen_edge_16(Case& c) {
  auto& g = c.g;
  float center[4];
  rand_vec(g, center, -60000.f, 60000.f, 1.f);
  const float size = g.pick(std::vector<float>{4000.f, 20000.f});
  const int count = g.chance(0.05f) ? 0 : g.range(1, g.chance(0.1f) ? 80 : 30);
  const u32 work = alloc(kWorkSize);
  for (u32 i = 0; i < 640; i += 4) {
    st<u32>(work + i, g.u32_());
  }
  const u32 cache = gen_edge_cache(g, center, size, count);
  st<u32>(work, cache);
  if (g.chance(0.05f)) {
    // many grabbable floors: more than the 48 the work holds
    const int n = g.range(45, 90);
    st<u32>(cache, (u32)n);
    for (int i = 0; i < n; i++) {
      const u32 tri = cache + kCacheTris + 64 * i;
      const float x = center[0] + g.f(-size, size), z = center[2] + g.f(-size, size);
      const float a = g.f(100.f, 2000.f), b = -g.f(100.f, 2000.f);
      st_vec(tri, x, center[1], z, 1.f);
      st_vec(tri + 16, x + a, center[1], z, 1.f);
      st_vec(tri + 32, x, center[1], z + b, 1.f);
      st<u32>(tri + 48, g.u32_() & ~(56u | 5u | (63u << 14)));
    }
  }
  for (int i = 0; i < 3; i++) {
    const s32 lo = (s32)(center[i] + g.f(-size * 1.2f, size * 0.5f));
    st<s32>(work + 96 + 4 * i, lo);
    st<s32>(work + 112 + 4 * i, lo + (s32)g.f(0.f, size * 2.f));
  }
  c.args[0] = work;
  for (int i = 1; i < 8; i++) {
    c.args[i] = g.u32_();
  }
}

// (method 15 collide-edge-work) work: edges of the grabbable tris
void gen_edge_15(Case& c) {
  auto& g = c.g;
  float center[4];
  rand_vec(g, center, -60000.f, 60000.f, 1.f);
  const float size = g.pick(std::vector<float>{4000.f, 20000.f});
  const int count = g.chance(0.05f) ? 0 : g.range(1, g.chance(0.1f) ? 48 : 20);
  const u32 cache = gen_edge_cache(g, center, size, count + 4);
  const u32 work = alloc(kWorkSize);
  for (u32 i = 0; i < 640; i += 4) {
    st<u32>(work + i, g.u32_());
  }
  st<u32>(work, cache);
  const u32 cshape = alloc_basic(0, 64);
  st_vec(cshape + 12, center[0] + g.f(-size, size), center[1] + g.f(-size, size),
         center[2] + g.f(-size, size), 1.f);
  st<u32>(work + 4, cshape);
  st<float>(work + 68, center[1] + g.f(-size, size));
  // already found vertices and edges, sometimes nearly full
  const u32 nverts = g.chance(0.8f) ? 0 : (u32)g.range(0, 64);
  const u32 nedges = g.chance(0.8f) ? 0 : (u32)g.range(0, 96);
  st<u32>(work + 8, nverts);
  st<u32>(work + 12, nedges);
  for (u32 k = 0; k < 64; k++) {
    st_vec(work + 640 + 16 * k, center[0] + g.f(-size, size), center[1], center[2] + g.f(-size, size),
           1.f);
  }
  for (u32 k = 0; k < 96; k++) {
    for (u32 i = 0; i < 48; i += 4) {
      st<u32>(work + 1664 + 48 * k + i, g.u32_());
    }
  }
  st<u32>(work + 16, (u32)count);
  for (int i = 0; i < count; i++) {
    st<u32>(work + 6272 + 32 * i, cache + kCacheTris + 64 * i);
  }
  c.args[0] = work;
  for (int i = 1; i < 8; i++) {
    c.args[i] = g.u32_();
  }
}

//! a hold list: n items (48 bytes: next, rating, split, edge, center-pt, outward-pt) sorted by
//! rating, in the list's items
u32 gen_hold_list(Gen& g, u32 list, int n, u32 work) {
  st<u32>(list, (u32)n);                     // num-allocs
  st<u32>(list + 4, (u32)g.range(0, 16));    // num-attempts
  std::vector<float> ratings(n);
  for (auto& r : ratings) {
    r = g.f(-100.f, 100.f);
  }
  std::sort(ratings.begin(), ratings.end());
  if (n > 2 && g.chance(0.3f)) {
    ratings[1] = ratings[0];  // equal ratings
  }
  u32 prev = 0;
  for (int k = 0; k < n; k++) {
    const u32 item = list + 16 + 48 * k;
    for (u32 i = 0; i < 48; i += 4) {
      st<u32>(item + i, g.u32_());
    }
    st<float>(item + 4, ratings[k]);
    st<s8>(item + 8, (s8)g.pick(std::vector<int>{0, 0, 1, -1, 2, -2, 3, -3}));
    // the edge: two of the work's vertices and a direction
    const u32 edge = work + 1664 + 48 * g.range(0, 95);
    st<u32>(edge + 8, work + 640 + 16 * g.range(0, 63));
    st<u32>(edge + 12, work + 640 + 16 * g.range(0, 63));
    float d[3] = {g.f(-1, 1), g.f(-0.2f, 0.2f), g.f(-1, 1)};
    const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) + 1e-6f;
    st_vec(edge + 32, d[0] / len, d[1] / len, d[2] / len, 1.f);
    st<u32>(item + 12, edge);
    st_vec(item + 16, g.f(-20000.f, 20000.f), g.f(-20000.f, 20000.f), g.f(-20000.f, 20000.f),
           1.f);
    if (prev) {
      st<u32>(prev, item);
    } else {
      st<u32>(list + 8, item);
    }
    prev = item;
  }
  if (prev) {
    st<u32>(prev, st());
  } else {
    st<u32>(list + 8, st());
  }
  return list;
}

// (method 10 collide-edge-hold-list) list item
void gen_hold_10(Case& c) {
  auto& g = c.g;
  const u32 work = alloc(kWorkSize);
  const u32 list = work + 7808;
  const int n = g.range(0, 12);
  gen_hold_list(g, list, n, work);
  const u32 item = list + 16 + 48 * std::min(n, 31);
  st<float>(item + 4, g.chance(0.2f) && n ? ld<float>(list + 16 + 48 * g.range(0, n - 1) + 4)
                                          : g.f(-120.f, 120.f));
  st_vec(item + 16, g.f(-1e4f, 1e4f), g.f(-1e4f, 1e4f), g.f(-1e4f, 1e4f), 1.f);
  c.args[0] = list;
  c.args[1] = item;
  for (int i = 2; i < 8; i++) {
    c.args[i] = g.u32_();
  }
}

// (method 18 collide-edge-work) work list info
void gen_edge_18(Case& c) {
  auto& g = c.g;
  const u32 work = alloc(kWorkSize);
  for (u32 k = 0; k < 64; k++) {
    st_vec(work + 640 + 16 * k, g.f(-20000.f, 20000.f), g.f(-20000.f, 20000.f),
           g.f(-20000.f, 20000.f), 1.f);
  }
  st<float>(work + 168, g.f(0.f, 3000.f));
  st<float>(work + 172, g.f(0.f, 3000.f));
  const u32 list = work + 7808;
  const int n = g.chance(0.1f) ? 0 : g.range(1, g.chance(0.2f) ? 31 : 8);
  gen_hold_list(g, list, n, work);
  if (g.chance(0.1f)) {
    st<u32>(list, 32);  // no room for new items
  }
  c.args[0] = work;
  c.args[1] = list;
  c.args[2] = alloc(64);
  for (int i = 3; i < 8; i++) {
    c.args[i] = g.u32_();
  }
}

}  // namespace

void register_collide_edge_grab_tests() {
  add_test({"(method 16 collide-edge-work)", "(method 16 collide-edge-work)", 6000, setup_edge,
            gen_edge_16, {}});
  add_test({"(method 15 collide-edge-work)", "(method 15 collide-edge-work)", 6000, setup_edge,
            gen_edge_15, {}});
  add_test({"(method 10 collide-edge-hold-list)", "(method 10 collide-edge-hold-list)", 6000,
            setup_edge, gen_hold_10, {}});
  add_test({"(method 18 collide-edge-work)", "(method 18 collide-edge-work)", 8000, setup_edge,
            gen_edge_18, {}});
}

}  // namespace tests
