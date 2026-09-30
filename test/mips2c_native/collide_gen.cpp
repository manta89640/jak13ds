/*!
 * @file collide_gen.cpp
 * (AI-assisted)
 * Generators for the collision structures shared by the collide tests.
 */

#include "collide_gen.h"

namespace tests {

using namespace harness;

u32 collide_cache_type() {
  static u32 type = 0;
  if (!type) {
    type = make_type("collide-cache", 33);
    for (int m : {26, 27, 28, 29, 30, 32}) {
      set_method(type, m, mips2c_stub(fmt::format("(method {} collide-cache)", m)));
    }
  }
  return type;
}

void set_max_tris(u32 n) {
  set_sym("*collide-cache-max-tris*", n);
}

u32 gen_cache(Gen& g, u32 max_tris) {
  const u32 cache = alloc_basic(collide_cache_type(), kCacheTris + 64 * (max_tris + 2));
  // header and prims: random bytes (the functions only read what they need)
  for (u32 i = 0; i < kCacheTris; i += 4) {
    st<u32>(cache + i, g.u32_());
  }
  st<u32>(cache + 0, (u32)g.range(0, (s32)max_tris));
  return cache;
}

void gen_cache_tri(Gen& g, u32 tri, const float center[4], float size) {
  float v[3][4];
  for (int k = 0; k < 3; k++) {
    for (int i = 0; i < 3; i++) {
      v[k][i] = center[i] + g.f_edge(-size, size);
    }
    v[k][3] = g.chance(0.5f) ? 1.f : g.f(-2.f, 2.f);
  }
  switch (g.range(0, 12)) {
    case 0:
      memcpy(v[1], v[0], 16);
      break;
    case 1:
      for (int i = 0; i < 3; i++) {
        v[2][i] = v[0][i] + (v[1][i] - v[0][i]) * 0.5f;
      }
      break;
    case 2:
      v[1][1] = v[0][1];
      v[2][1] = v[0][1];
      break;
    default:
      break;
  }
  for (int k = 0; k < 3; k++) {
    st_bytes(tri + 16 * k, v[k], 16);
  }
  for (int i = 48; i < 64; i += 4) {
    st<u32>(tri + i, g.u32_());
  }
}

u32 gen_frag_mesh(Gen& g, int vertex_count) {
  const u32 mesh = alloc_basic(0, 32);
  const int vertex_qwc = (vertex_count * 6 + 15) / 16;

  // strips
  std::vector<s8> strips;
  std::vector<u8> pat_idx;
  const int num_strips = g.range(1, 8);
  const int num_pats = g.range(1, 40);
  auto idx = [&]() { return g.range(0, vertex_count - 1); };
  for (int s = 0; s < num_strips; s++) {
    strips.push_back((s8)idx());
    strips.push_back((s8)idx());
    strips.push_back((s8)idx());
    pat_idx.push_back((u8)g.range(0, num_pats - 1));
    const int more = g.range(0, 12);
    for (int m = 0; m < more; m++) {
      const int i = idx();
      strips.push_back(g.chance(0.6f) ? (s8)(i + 1) : (s8)(-i - 1));
      pat_idx.push_back((u8)g.range(0, num_pats - 1));
    }
    strips.push_back(0);
  }
  strips.push_back((s8)g.range(-128, -1));  // end
  strips.push_back(0);
  strips.push_back(0);

  const u32 data = alloc(vertex_qwc * 16 + (u32)strips.size() + (u32)pat_idx.size() + 16);
  for (int i = 0; i < vertex_qwc * 8; i++) {
    st<u16>(data + 2 * i, (u16)g.u32_());
  }
  const u32 strip_addr = data + vertex_qwc * 16;
  st_bytes(strip_addr, strips.data(), (u32)strips.size());
  st_bytes(strip_addr + (u32)strips.size(), pat_idx.data(), (u32)pat_idx.size());

  const u32 pats = alloc(4 * 256);
  for (int i = 0; i < 256; i++) {
    st<u32>(pats + 4 * i, g.u32_() & (g.chance(0.5f) ? 0xff : 0xffffffff));
  }

  st<u32>(mesh + 0, data);                  // packed-data
  st<u32>(mesh + 4, pats);                  // pat-array
  st<u16>(mesh + 8, (u16)strips.size());    // strip-data-len
  st<u16>(mesh + 10, (u16)pat_idx.size());  // poly-count
  for (int i = 0; i < 3; i++) {             // base-trans (ints)
    st<s32>(mesh + 12 + 4 * i, g.range(-200000, 200000));
  }
  st<u8>(mesh + 24, (u8)vertex_count);  // vertex-count (base-trans w)
  st<u8>(mesh + 25, (u8)vertex_qwc);    // vertex-data-qwc
  st<u8>(mesh + 26, (u8)(vertex_qwc + (strips.size() + pat_idx.size() + 15) / 16));
  st<u8>(mesh + 27, 0);
  return mesh;
}

void gen_spad_vertices(Gen& g, int count, s32 range) {
  for (int k = 0; k < count; k++) {
    s32 v[4];
    float f[4];
    for (int i = 0; i < 4; i++) {
      v[i] = g.range(-range, range);
      f[i] = (float)v[i] + g.f(-0.5f, 0.5f);
    }
    st_bytes(kSpad + 32 * k, v, 16);
    st_bytes(kSpad + 32 * k + 16, f, 16);
  }
}

u32 gen_collide_work(Gen& g, s32 box_range) {
  const u32 cw = alloc(112);
  // collide-sphere-neg-r
  st_vec(cw, g.f(-1e5f, 1e5f), g.f(-1e5f, 1e5f), g.f(-1e5f, 1e5f), -g.f(0, 2e4f));
  // collide-box4w: min at 16, max at 32
  for (int i = 0; i < 4; i++) {
    s32 a = g.range(-box_range, box_range), b = g.range(-box_range, box_range);
    if (g.chance(0.9f) && a > b) {
      std::swap(a, b);
    }
    st<s32>(cw + 16 + 4 * i, a);
    st<s32>(cw + 32 + 4 * i, b);
  }
  // inv-mat: a rotation-ish matrix with a translation
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      const float v = r == 3 ? g.f(-(float)box_range, (float)box_range)
                             : (r == c ? g.f(0.5f, 1.5f) : g.f(-0.7f, 0.7f));
      st<float>(cw + 48 + 16 * r + 4 * c, g.chance(0.02f) ? 0.f : v);
    }
  }
  set_sym("*collide-work*", cw);
  return cw;
}

}  // namespace tests
