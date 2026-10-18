/*!
 * @file native_collide_cache.cpp
 * (AI-assisted)
 * Native versions of the mips2c functions in collide_cache.cpp. See game/mips2c/mips2c_native.h for
 * the rules (same float operations, grouped the same way, as the mips2c code).
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "game/kernel/jak1/kscheme.h"
#include "game/mips2c/jak1_functions/native_functions.h"

extern const u32* max_tri_count;  // collide_cache.cpp
extern u32 g_link_generation;      // (AI-assisted) game/kernel/common/klink.h

namespace Mips2C::jak1 {
const u32* collide_vu0_buffer();                // collide_cache.cpp
void upload_collide_frag(u64 data, u64 count);  // collide_cache.cpp
}  // namespace Mips2C::jak1

namespace Mips2C::jak1::native {

namespace {

constexpr u32 kMiss = 0xccbebc20;  // -100000000.0

/*!
 * (method 9 collide-cache-prim) = resolve-moving-sphere-tri
 * (this result sphere move max-t pat-mask?) -> float
 * Sweeps the sphere (radius in w) along move against this prim's triangles in the collide cache
 * and keeps the nearest hit (under max-t, 2.0 if max-t is negative) in result: the triangle's
 * vertices, the touching point, the normal and the pat. With bit 0 of the 5th argument, only
 * triangles facing against the move and with the sphere in front of them count.
 * Returns the hit's t, or -100000000.0 if nothing was hit.
 */
u64 method_9_collide_cache_prim_impl(const NativeArgs& args) {
  const u32 prim = (u32)args.a[0];
  const u32 result = (u32)args.a[1];
  const u32 sphere = (u32)args.a[2];
  const u32 move = (u32)args.a[3];
  float best = u2f((u32)args.a[4]);
  const u32 flags = (u32)args.a[5];
  // mips2c-style scratch for the two vectors moving-sphere-triangle-intersect writes
  const u32 out_point = args.stack - 32;
  const u32 out_normal = args.stack - 16;

  if (!(0.f <= best)) {
    best = 2.f;
  }
  const float start = best;
  u32 tri = gload<u32>(prim + 32) + 4908 + ((u32)gload<u16>(prim + 40) << 6);
  static const u32 mst_sym = ::jak1::intern_from_c("moving-sphere-triangle-intersect").offset;
  static const u32* mst_stub = native_stub_slot("moving-sphere-triangle-intersect");

  for (u32 i = gload<u16>(prim + 42); i != 0; i--, tri += 64) {
    const u64 call_args[8] = {sphere,    move,      f2gpr(gload<float>(sphere + 12)),
                              tri,       out_point, out_normal,
                              args.a[6], args.a[7]};
    const u32 fn = gload<u32>(mst_sym);
    const float t = u2f((u32)(fn == *mst_stub ? moving_sphere_triangle_intersect_impl(NativeArgs{
                                                    call_args, args.pp, args.st, out_point})
                                              : native_call_goal(fn, call_args, args)));
    float mv[4], normal[4];
    gload_q(mv, move);
    gload_q(normal, out_normal);
    if (t < 0.f) {
      continue;
    }
    float point[4];
    gload_q(point, out_point);
    if (best <= t) {
      continue;
    }
    if (flags & 1) {
      // moving against the triangle, and the sphere in front of it
      float c[4], m[4], d[4], s[4];
      gload_q(c, sphere);
      for (int k = 0; k < 4; k++) {
        m[k] = mv[k] * normal[k];
        d[k] = c[k] - point[k];
      }
      m[0] = m[0] + m[1];
      for (int k = 0; k < 4; k++) {
        s[k] = d[k] * normal[k];
      }
      m[0] = m[0] + m[2];
      s[0] = s[0] + s[1];
      if (0.f <= m[0]) {
        continue;
      }
      s[0] = s[0] + s[2];
      if (s[0] < 0.f) {
        continue;
      }
    }
    u32 verts[3][4];
    for (int k = 0; k < 3; k++) {
      gload_q(verts[k], tri + 16 * k);
    }
    const u32 pat = gload<u32>(tri + 48);
    best = t;
    gstore_q(result + 48, point);
    gstore_q(result + 64, normal);
    for (int k = 0; k < 3; k++) {
      gstore_q(result + 16 * k, verts[k]);
    }
    gstore<u32>(result + 80, pat);
  }
  return f2gpr(best == start ? u2f(kMiss) : best);
}

// ---------------------------------------------------------------------------
// Filling the collide cache from background meshes
// ---------------------------------------------------------------------------
// __pc-upload-collide-frag unpacks a collide-frag-mesh's vertices (16-bit, offset by 2^27 as
// floats) into collide_vu0_buffer. Method 32 moves them to the scratchpad (32 bytes per vertex:
// the position as ints, then as floats), method 29 transforms them into the second half of the
// scratchpad (ints only), methods 26 / 27 walk the triangle strips and add the triangles whose
// bounding box overlaps the query box to the cache.

//! acc = m3 * 1, acc += m0 * v.x, acc += m1 * v.y, out = acc + m2 * v.z (all lanes)
inline void transform_point(float out[4], const float m[4][4], const float v[4]) {
  for (int i = 0; i < 4; i++) {
    float acc = m[3][i] * 1.f;
    acc += m[0][i] * v[0];
    acc += m[1][i] * v[1];
    out[i] = acc + m[2][i] * v[2];
  }
}

//! vftoi0
inline void ftoi0(s32 out[4], const float in[4]) {
  for (int i = 0; i < 4; i++) {
    out[i] = (s32)in[i];
  }
}

//! the 4 s16 of a 64-bit value moved to the top of 32-bit lanes (pextlh with 0), then shifted right
//! arithmetically (psraw)
inline void unpack_halves(s32 out[4], u64 v, int shift) {
  for (int i = 0; i < 4; i++) {
    out[i] = (s32)(u32)(((v >> (16 * i)) & 0xffff) << 16) >> shift;
  }
}

u32 sym_addr(const char* name) {
  return ::jak1::intern_from_c(name).offset;
}

u32 fake_spad() {
  static const u32 sym = sym_addr("*fake-scratchpad-data*");
  return gload<u32>(sym);
}

/*!
 * (method 32 collide-cache) = unpack-background-collide-mesh (this mesh transform start)
 * Moves the vertices of mesh from collide_vu0_buffer to the scratchpad, relative to the mesh's
 * base position and, if transform isn't #f, through its compressed matrix. Writes whole groups of
 * 4 (8 without transform), like the original.
 */
u64 method_32_collide_cache_impl(const NativeArgs& args) {
  const u32 mesh = (u32)args.a[1];
  const u32* buffer = collide_vu0_buffer();
  const u32 spad = fake_spad();
  ASSERT(args.a[3] == 0);

  s32 base_i[4];
  gload_q(base_i, mesh + 12);
  float offset[4];
  for (int i = 0; i < 4; i++) {
    offset[i] = u2f(i < 3 ? 0x4d000000 : 0) - (float)base_i[i];
  }
  const int count = gload<u8>(mesh + 24);

  auto vertex = [&](int k, float v[4]) {
    memcpy(v, buffer + 4 * k, 16);
    for (int i = 0; i < 3; i++) {
      v[i] = v[i] - offset[i];
    }
  };
  auto store = [&](int k, const float f[4]) {
    s32 n[4];
    ftoi0(n, f);
    gstore_q(spad + 32 * k + 16, f);
    gstore_q(spad + 32 * k, n);
  };

  if (args.a[2] == args.st) {
    const int groups = count <= 8 ? 1 : (count + 7) / 8;
    for (int k = 0; k < groups * 8; k++) {
      float v[4];
      vertex(k, v);
      store(k, v);
    }
    return 0;
  }

  const u32 xf = (u32)args.a[2];
  s32 trans_i[4], r0[4], r1[4], r2[4];
  unpack_halves(trans_i, gload<u64>(xf + 52), 10);
  unpack_halves(r0, gload<u64>(xf + 28), 16);
  unpack_halves(r1, gload<u64>(xf + 36), 16);
  unpack_halves(r2, gload<u64>(xf + 44), 16);
  float trans_add[4];
  gload_q(trans_add, xf + 12);
  float m[4][4];
  for (int i = 0; i < 4; i++) {
    m[3][i] = (float)trans_i[i];
    m[0][i] = ((float)r0[i]) * (1.f / 4096.f);
    m[1][i] = ((float)r1[i]) * (1.f / 4096.f);
    m[2][i] = ((float)r2[i]) * (1.f / 4096.f);
  }
  for (int i = 0; i < 3; i++) {
    m[3][i] = m[3][i] + trans_add[i];
  }
  const int groups = count <= 4 ? 1 : (count + 3) / 4;
  for (int k = 0; k < groups * 4; k++) {
    float v[4], out[4];
    vertex(k, v);
    transform_point(out, m, v);
    store(k, out);
  }
  return 0;
}

/*!
 * (method 29 collide-cache) (this mesh)
 * Transforms the scratchpad vertices of mesh by the matrix in *collide-work* (at 48) into ints in
 * the second half of the scratchpad, in whole groups of 4.
 */
u64 method_29_collide_cache_impl(const NativeArgs& args) {
  static const u32 collide_work_sym = sym_addr("*collide-work*");
  const u32 cw = gload<u32>(collide_work_sym);
  const u32 spad = fake_spad();
  const int count = gload<u8>((u32)args.a[1] + 24);
  float m[4][4];
  for (int i = 0; i < 4; i++) {
    gload_q(m[i], cw + 48 + 16 * i);
  }
  const int groups = count <= 4 ? 1 : (count + 3) / 4;
  for (int k = 0; k < groups * 4; k++) {
    float v[4], out[4];
    s32 n[4];
    gload_q(v, spad + 32 * k + 16);
    transform_point(out, m, v);
    ftoi0(n, out);
    gstore_q(spad + 4096 + 32 * k, n);
  }
  return 0;
}

//! the original only prints this with *cheat-mode* set (and never sets the "already" flag)
void print_too_many_tris(u64 st) {
  static const u32 printed_sym = sym_addr("*already-printed-exeeded-max-cache-tris*");
  static const u32 cheat_sym = sym_addr("*cheat-mode*");
  if ((s64)gload<s32>(printed_sym) != (s64)st) {
    return;
  }
  if ((s64)gload<s32>(cheat_sym) != (s64)(st + 8)) {  // #t
    return;
  }
  printf(
      "exceeded maximum collide cache tris (should print on screen but too lazy for that now)\n");
}

struct Vec4i {
  s32 v[4];
};

//! lq of an aligned quadword (the scratchpad vertices: spad + 32 * i, int part at 0 or 4096)
inline Vec4i load_vec4i(u32 addr) {
  Vec4i v;
  gload_q(v.v, addr);
  return v;
}

//! lq then sq of a quadword, both aligned
inline void copy_quad(u32 dst, u32 src) {
  gcopy_q(dst, src);
}

//! sq r0 then sw pat: the pat word of a collide-cache-tri, the rest of its quadword zero
inline void store_pat_quad(u32 addr, u32 pat) {
  const u32 t[4] = {pat, 0, 0, 0};
  gstore_q(addr, t);
}

//! Is the bounding box of three int vertices outside [bmin, bmax] in x, y or z?
inline bool outside_box(const Vec4i& a,
                        const Vec4i& b,
                        const Vec4i& c,
                        const Vec4i& bmin,
                        const Vec4i& bmax) {
  bool out = false;
  for (int i = 0; i < 3; i++) {
    const s32 lo = std::min(std::min(a.v[i], b.v[i]), c.v[i]);
    const s32 hi = std::max(std::max(a.v[i], b.v[i]), c.v[i]);
    out |= lo > bmax.v[i] || bmin.v[i] > hi;
  }
  return out;
}

/*!
 * Methods 26 and 27: add the triangles of mesh whose int vertices (at int_offset in the
 * scratchpad) overlap [bmin, bmax] and whose pat isn't in the cache's ignore-mask.
 * Strip format: 3 vertex indices (a negative first one ends the mesh), then one byte per more
 * triangle: 0 starts a new strip, n > 0 adds vertex n - 1 as a strip (flipping the winding),
 * n < 0 adds vertex -n - 1 keeping the first vertex (a fan). One pat index byte per triangle.
 */
#ifndef __3DS__
//! (AI-assisted) OPENGOAL_FRAG_SKIP_VERIFY=1 (host): pc-collide-frag-may-hit? never skips, but
//! remembers the mesh it would have skipped; add_mesh_tris then checks that it adds no triangle.
struct FragSkipVerify {
  bool on = false;
  u32 predicted_mesh = 0;
  u64 calls = 0, predicted = 0, mismatches = 0;
};
FragSkipVerify g_frag_skip_verify;
#endif

/*!
 * (AI-assisted) logic_micro: add_mesh_tris with an outcode per vertex instead of the min / max of
 * each triangle's three vertices: bit 2i set when the vertex is above bmax in axis i, bit 2i+1 when
 * below bmin. A triangle's box misses [bmin, bmax] exactly when its three vertices are all above
 * (or all below) in one axis: the AND of their outcodes isn't 0. Each vertex's outcode is made the
 * first time a triangle uses it (the scratchpad vertices don't change during the call), so a
 * triangle costs a byte load instead of three 3-lane min / max. Same triangles, same order, same
 * overflow handling.
 */
void add_mesh_tris_micro(u32 cache,
                         u32 mesh,
                         const Vec4i& bmin,
                         const Vec4i& bmax,
                         u32 int_offset,
                         u64 st) {
  const s64 max_tris = (s32)*max_tri_count;
  s64 num_tris = gload<u32>(cache);
  auto overflow = [&]() {
    gstore<u32>(cache, (u32)max_tris);
    print_too_many_tris(st);
  };
  if (max_tris - num_tris < 0) {
    overflow();
    return;
  }
  const s32 x0 = bmin.v[0], y0 = bmin.v[1], z0 = bmin.v[2];
  const s32 x1 = bmax.v[0], y1 = bmax.v[1], z1 = bmax.v[2];
  // indices are s8 >= 0 (or n - 1 for 1 <= n <= 128): 0..127
  u8 oc[128];
  memset(oc, 0xff, sizeof(oc));
  const u8* const mem = g_ee_main_mem;
  u32 spad = 0;
  auto compute = [&](s32 i) -> u32 {
    s32 v[3];
    memcpy(v, mem + spad + (u32)i * 32 + int_offset, 12);
    return (u32)(v[0] > x1) | ((u32)(v[0] < x0) << 1) | ((u32)(v[1] > y1) << 2) |
           ((u32)(v[1] < y0) << 3) | ((u32)(v[2] > z1) << 4) | ((u32)(v[2] < z0) << 5);
  };
  auto code = [&](s32 i) -> u32 {
    if ((u32)i >= 128) {
      return compute(i);  // (a negative first-triangle index: never in the game's meshes)
    }
    u32 c = oc[i];
    if (c == 0xff) {
      c = compute(i);
      oc[i] = (u8)c;
    }
    return c;
  };

  u32 tri_out = cache + 4908 + (u32)num_tris * 64;
  u32 strip = gload<u32>(mesh) + ((u32)gload<u8>(mesh + 25) << 4);
  u32 pat_index = strip + gload<u16>(mesh + 8);
  const u32 pats = gload<u32>(mesh + 4);
  const u32 ignore_mask = gload<u32>(cache + 8);

  for (;;) {
    // new strip
    const u32 strip_spad = fake_spad();
    if (strip_spad != spad) {
      // (the same address every strip in practice; the outcodes belong to one scratchpad)
      spad = strip_spad;
      memset(oc, 0xff, sizeof(oc));
    }
    const s32 i0 = gload<s8>(strip);
    const s32 i1 = gload<s8>(strip + 1);
    const s32 i2 = gload<s8>(strip + 2);
    if (i0 < 0) {
      gstore<u32>(cache, (u32)num_tris);
      return;
    }
    u32 p0 = spad + i0 * 32;
    u32 p1 = spad + i1 * 32;
    u32 p2 = spad + i2 * 32;
    u32 ca = code(i0), cb = code(i1), cc = code(i2);
    const u32 pat_addr = pats + ((u32)gload<u8>(pat_index) << 2);
    if (!(ca & cb & cc)) {
      if (num_tris == max_tris) {
        overflow();
        return;
      }
      const u32 pat = gload<u32>(pat_addr);
      if (!(pat & ignore_mask)) {
        store_pat_quad(tri_out + 48, pat);
        num_tris++;
        copy_quad(tri_out, p0 + 16);
        copy_quad(tri_out + 16, p1 + 16);
        copy_quad(tri_out + 32, p2 + 16);
        tri_out += 64;
      }
    }
    pat_index++;
    strip += 3;
    s32 flip = 16;

    for (;;) {
      s32 n = gload<s8>(strip);
      strip++;
      if (n == 0) {
        break;
      }
      if (n < 0) {
        n = -n;
      } else {
        ca = cb;
        p0 = p1;
        flip = -flip;
      }
      p1 = p2;
      cb = cc;
      p2 = spad + (n - 1) * 32;
      const u32 pidx = gload<u8>(pat_index);
      cc = code(n - 1);
      pat_index++;
      if (ca & cb & cc) {
        continue;
      }
      const u32 pat = gload<u32>(pats + (pidx << 2));
      if (pat & ignore_mask) {
        continue;
      }
      if (num_tris == max_tris) {
        overflow();
        return;
      }
      // p1 in the middle, p0 and p2 on either side depending on the winding
      store_pat_quad(tri_out + 48, pat);
      num_tris++;
      copy_quad(tri_out + 16, p1 + 16);
      copy_quad(tri_out + 16 - flip, p0 + 16);
      copy_quad(tri_out + 16 + flip, p2 + 16);
      tri_out += 64;
    }
  }
}

void add_mesh_tris(u32 cache,
                   u32 mesh,
                   const Vec4i& bmin,
                   const Vec4i& bmax,
                   u32 int_offset,
                   u64 st) {
  static const bool micro = logic_micro_on();
#ifndef __3DS__
  struct VerifyAtEnd {
    bool on = false;
    u32 cache = 0, mesh = 0;
    u32 before = 0;
    ~VerifyAtEnd() {
      if (on && gload<u32>(cache) != before) {
        g_frag_skip_verify.mismatches++;
        printf("frag skip verify MISMATCH: mesh #x%x added %d triangles\n", mesh,
               (int)(gload<u32>(cache) - before));
      }
    }
  } verify_at_end;
  if (g_frag_skip_verify.on && g_frag_skip_verify.predicted_mesh == mesh) {
    g_frag_skip_verify.predicted_mesh = 0;
    verify_at_end.on = true;
    verify_at_end.cache = cache;
    verify_at_end.mesh = mesh;
    verify_at_end.before = gload<u32>(cache);
  }
#endif
  if (micro) {
    add_mesh_tris_micro(cache, mesh, bmin, bmax, int_offset, st);
    return;
  }
  const s64 max_tris = (s32)*max_tri_count;
  s64 num_tris = gload<u32>(cache);
  auto overflow = [&]() {
    gstore<u32>(cache, (u32)max_tris);
    print_too_many_tris(st);
  };
  if (max_tris - num_tris < 0) {
    overflow();
    return;
  }
  u32 tri_out = cache + 4908 + (u32)num_tris * 64;
  u32 strip = gload<u32>(mesh) + ((u32)gload<u8>(mesh + 25) << 4);
  u32 pat_index = strip + gload<u16>(mesh + 8);
  const u32 pats = gload<u32>(mesh + 4);
  const u32 ignore_mask = gload<u32>(cache + 8);

  for (;;) {
    // new strip
    const u32 spad = fake_spad();
    const s32 i0 = gload<s8>(strip);
    const s32 i1 = gload<s8>(strip + 1);
    const s32 i2 = gload<s8>(strip + 2);
    if (i0 < 0) {
      gstore<u32>(cache, (u32)num_tris);
      return;
    }
    u32 p0 = spad + i0 * 32;
    u32 p1 = spad + i1 * 32;
    u32 p2 = spad + i2 * 32;
    Vec4i a = load_vec4i(p0 + int_offset);
    Vec4i b = load_vec4i(p1 + int_offset);
    Vec4i c = load_vec4i(p2 + int_offset);
    const u32 pat_addr = pats + ((u32)gload<u8>(pat_index) << 2);
    if (!outside_box(a, b, c, bmin, bmax)) {
      if (num_tris == max_tris) {
        overflow();
        return;
      }
      const u32 pat = gload<u32>(pat_addr);
      if (!(pat & ignore_mask)) {
        store_pat_quad(tri_out + 48, pat);
        num_tris++;
        copy_quad(tri_out, p0 + 16);
        copy_quad(tri_out + 16, p1 + 16);
        copy_quad(tri_out + 32, p2 + 16);
        tri_out += 64;
      }
    }
    pat_index++;
    strip += 3;
    s32 flip = 16;

    for (;;) {
      s32 n = gload<s8>(strip);
      strip++;
      if (n == 0) {
        break;
      }
      if (n < 0) {
        n = -n;
      } else {
        a = b;
        p0 = p1;
        flip = -flip;
      }
      p1 = p2;
      b = c;
      p2 = spad + (n - 1) * 32;
      const u32 pidx = gload<u8>(pat_index);
      c = load_vec4i(p2 + int_offset);
      pat_index++;
      if (outside_box(a, b, c, bmin, bmax)) {
        continue;
      }
      const u32 pat = gload<u32>(pats + (pidx << 2));
      if (pat & ignore_mask) {
        continue;
      }
      if (num_tris == max_tris) {
        overflow();
        return;
      }
      // p1 in the middle, p0 and p2 on either side depending on the winding
      store_pat_quad(tri_out + 48, pat);
      num_tris++;
      copy_quad(tri_out + 16, p1 + 16);
      copy_quad(tri_out + 16 - flip, p0 + 16);
      copy_quad(tri_out + 16 + flip, p2 + 16);
      tri_out += 64;
    }
  }
}

/*!
 * (method 26 collide-cache) = load-mesh-from-spad-in-box (this mesh)
 * Adds the triangles of mesh (method 32 put its vertices in the scratchpad) that overlap the
 * cache's collide-box4w.
 */
u64 method_26_collide_cache_impl(const NativeArgs& args) {
  const u32 cache = (u32)args.a[0];
  const Vec4i bmin = gload<Vec4i>(cache + 60);
  const Vec4i bmax = gload<Vec4i>(cache + 76);
  add_mesh_tris(cache, (u32)args.a[1], bmin, bmax, 0, args.st);
  return 0;
}

/*!
 * (method 27 collide-cache) (this mesh)
 * Transforms mesh's scratchpad vertices with method 29, then adds its triangles that overlap the
 * box in *collide-work* (at 16).
 */
u64 method_27_collide_cache_impl(const NativeArgs& args) {
  static const u32 collide_work_sym = sym_addr("*collide-work*");
  static const u32* method_29_stub = native_stub_slot("(method 29 collide-cache)");
  const u32 cache = (u32)args.a[0];
  const u32 mesh = (u32)args.a[1];
  // (method 29 this mesh), through the method table like the original
  const u32 method_29 = gload<u32>(gload<u32>(cache - 4) + 132);
  if (method_29 == *method_29_stub) {
    method_29_collide_cache_impl(args);
  } else {
    native_call_goal(method_29, args.a, args);
  }
  const u32 cw = gload<u32>(collide_work_sym);
  const Vec4i bmin = gload<Vec4i>(cw + 16);
  const Vec4i bmax = gload<Vec4i>(cw + 32);
  add_mesh_tris(cache, mesh, bmin, bmax, 4096, args.st);
  return 0;
}

// ---------------------------------------------------------------------------
// Foreground meshes and other primitives
// ---------------------------------------------------------------------------

//! out.xyz = (acc = a.yzx * b.zxy) - b.yzx * a.zxy: vopmula acc, a, b then vopmsub out, b, a.
//! out.w is not written.
inline void opm(float out[4], const float a[4], const float b[4]) {
  const float a0 = a[1] * b[2];
  const float a1 = a[2] * b[0];
  const float a2 = a[0] * b[1];
  const float n0 = a0 - b[1] * a[2];
  const float n1 = a1 - b[2] * a[0];
  const float n2 = a2 - b[0] * a[1];
  out[0] = n0;
  out[1] = n1;
  out[2] = n2;
}

inline bool sign_bit(float f) {
  return (s32)f2u(f) < 0;
}

//! vrsqrt Q, vf0.w, x
inline float rsqrt_q(float x) {
  const float s = std::sqrt(std::abs(x));
  return s == 0 ? 0.f : 1.f / s;
}

//! the printf of an overflowing cache in methods 12 / 13 / 14 of collide-shape-prim-mesh
void print_too_many_tris_short(u64 st) {
  static const u32 printed_sym = sym_addr("*already-printed-exeeded-max-cache-tris*");
  static const u32 cheat_sym = sym_addr("*cheat-mode*");
  if ((s64)gload<s32>(printed_sym) != (s64)st) {
    return;
  }
  if ((s64)gload<s32>(cheat_sym) != (s64)(st + 8)) {  // #t
    return;
  }
  printf("too many tris\n");
}

/*!
 * Methods 12, 13 and 14 of collide-shape-prim-mesh: add the triangles of this prim's mesh to the
 * cache. The mesh's method (14, or 15 with *collide-work*'s matrix) puts its vertices, moved by
 * the joint's bone, on the scratchpad (32 bytes each: floats, then ints). Triangles whose int
 * bounding box overlaps the box (the cache's, or *collide-work*'s) and whose pat isn't ignored are
 * added, then the prim, if it has any.
 */
void add_prim_mesh(const NativeArgs& args, bool collide_work_box) {
  static const u32 collide_work_sym = sym_addr("*collide-work*");
  const u32 prim = (u32)args.a[0];
  const u32 cache = (u32)args.a[1];
  const u32 mesh = gload<u32>(prim + 68);
  if (mesh == (u32)args.st) {
    return;
  }
  const u32 num_prims = gload<u32>(cache + 4);
  if (num_prims == 100) {
    printf("too many prims\n");
    return;
  }
  const u32 prim_out = cache + 108 + 48 * num_prims;

  // (method 14/15 mesh bone spad [inv-mat])
  const u32 spad = fake_spad();
  const u32 node_list = gload<u32>(gload<u32>(gload<u32>(prim) + 136) + 112);
  const u32 bone = gload<u32>(node_list + ((u32)(s32)gload<s8>(prim + 8) << 5) + 28);
  static const u32* method_14_stub = native_stub_slot("(method 14 collide-mesh)");
  static const u32* method_15_stub = native_stub_slot("(method 15 collide-mesh)");
  const u32 method = gload<u32>(gload<u32>(mesh - 4) + (collide_work_box ? 76 : 72));
  if (collide_work_box) {
    const u64 call_args[8] = {mesh,      bone,      gload<u32>(collide_work_sym) + 48u,
                              spad,      args.a[4], args.a[5],
                              args.a[6], args.a[7]};
    if (method == *method_15_stub) {
      method_15_collide_mesh_impl(NativeArgs{call_args, args.pp, args.st, args.stack});
    } else {
      native_call_goal(method, call_args, args);
    }
  } else {
    const u64 call_args[8] = {mesh,      bone,      spad,      args.a[3],
                              args.a[4], args.a[5], args.a[6], args.a[7]};
    if (method == *method_14_stub) {
      method_14_collide_mesh_impl(NativeArgs{call_args, args.pp, args.st, args.stack});
    } else {
      native_call_goal(method, call_args, args);
    }
  }

  // extra quad of the triangles: pat, then the prim index
  const u32 prim_index = gload<u32>(cache + 4);
  const u32 max_tris = (u32)(s32)*max_tri_count;
  u32 num_tris = gload<u32>(cache);
  if ((s64)(s32)max_tris - (s64)num_tris < 0) {
    print_too_many_tris_short(args.st);
    return;
  }
  const u32 first = num_tris;
  u32 ntri = gload<u32>(mesh + 4);
  s32 bmin[4], bmax[4];
  if (collide_work_box) {
    const u32 cw = gload<u32>(collide_work_sym);
    gload_q(bmin, (cw + 16) & ~15u);
    gload_q(bmax, (cw + 32) & ~15u);
  } else {
    gload_q(bmin, (cache + 60) & ~15u);
    gload_q(bmax, (cache + 76) & ~15u);
  }
  const u32 ignore_mask = gload<u32>(cache + 8);
  const u32 extra[4] = {0, prim_index, 0, 0};
  u32 out = cache + 4908 + (num_tris << 6);
  u32 tri = mesh + 28;
  for (; ntri != 0; ntri--, tri += 8) {
    const u32 p0 = spad + ((u32)gload<u8>(tri) << 5);
    const u32 p1 = spad + ((u32)gload<u8>(tri + 1) << 5);
    const u32 p2 = spad + ((u32)gload<u8>(tri + 2) << 5);
    s32 a[4], b[4], c[4];
    gload_q(a, p0 + 16);
    gload_q(b, p1 + 16);
    gload_q(c, p2 + 16);
    bool outside = false;
    for (int i = 0; i < 3; i++) {
      const s32 lo = std::min(std::min(a[i], b[i]), c[i]);
      const s32 hi = std::max(std::max(a[i], b[i]), c[i]);
      outside |= (lo > bmax[i]) | (bmin[i] > hi);
    }
    const u32 pat = gload<u32>(tri + 4);
    if (outside) {
      continue;
    }
    gstore_q(out + 48, extra);
    if (num_tris == max_tris) {
      print_too_many_tris_short(args.st);
      return;
    }
    gstore<u32>(out + 48, pat);
    copy_quad(out, p0);
    if (pat & ignore_mask) {
      continue;
    }
    copy_quad(out + 16, p1);
    num_tris++;
    copy_quad(out + 32, p2);
    out += 64;
  }

  const u32 added = num_tris - first;
  const u32 old_prims = gload<u32>(cache + 4);
  if (added == 0) {
    return;
  }
  u32 core0[4], core1[4];
  gload_q(core0, (prim + 12) & ~15u);
  gload_q(core1, (prim + 28) & ~15u);
  const u32 extra_quad[4] = {cache, prim, (first & 0xffff) | (added << 16), 0};
  gstore_q(prim_out + 32, extra_quad);
  gstore_q(prim_out, core0);
  gstore_q(prim_out + 16, core1);
  gstore<u32>(cache + 4, old_prims + 1);
  gstore<u32>(cache, num_tris);
}

u64 method_12_collide_shape_prim_mesh_impl(const NativeArgs& args) {
  add_prim_mesh(args, false);
  return 0;
}

u64 method_13_collide_shape_prim_mesh_impl(const NativeArgs& args) {
  add_prim_mesh(args, true);
  return 0;
}

/*!
 * (method 30 collide-cache) = puyp-mesh (this work prim)
 * The y probe (collide-puyp-work: best-u, ignore-pat, tri-out, start-pos, move-dist) against this
 * prim's triangles: the nearest crossing of the probe's line with a triangle whose front faces the
 * probe, inside its edges, below best-u. Updates best-u and tri-out.
 */
u64 method_30_collide_cache_impl(const NativeArgs& args) {
  const u32 cache = (u32)args.a[0];
  const u32 work = (u32)args.a[1];
  const u32 prim = (u32)args.a[2];
  u32 tri = cache + 4908 + ((u32)gload<u16>(prim + 40) << 6);
  u32 n = gload<u16>(prim + 42);
  float best = gload<float>(work);
  float start[4], mv[4];
  gload_q(start, work + 16);
  gload_q(mv, work + 32);
  const u32 ignore = gload<u32>(work + 4);
  const u32 tri_out = gload<u32>(work + 8);

  for (; n != 0; n--, tri += 64) {
    float v0[4], v1[4], v2[4];
    gload_q(v0, tri);
    gload_q(v1, tri + 16);
    gload_q(v2, tri + 32);
    float e5[4], e6[4], e7[4];
    for (int i = 0; i < 4; i++) {
      e5[i] = v1[i] - v0[i];
      e6[i] = v1[i] - v2[i];
      e7[i] = v1[i] - start[i];
    }
    if (gload<u32>(tri + 48) & ignore) {
      continue;
    }
    float nrm[4];
    opm(nrm, e6, e5);
    float n2[3], d7[3], d11[3];
    for (int i = 0; i < 3; i++) {
      n2[i] = nrm[i] * nrm[i];
      d7[i] = e7[i] * nrm[i];
      d11[i] = mv[i] * nrm[i];
    }
    float len2 = n2[0] + n2[1];
    float dist = d7[0] + d7[1];
    float mdot = d11[0] + d11[1];
    len2 = len2 + n2[2];
    dist = dist + d7[2];
    mdot = mdot + d11[2];
    const float q = rsqrt_q(len2);
    const float u = dist / mdot;
    if ((f2u(mdot) << 1) == 0) {
      continue;  // moving along the plane
    }
    nrm[0] = nrm[0] * q;
    nrm[1] = nrm[1] * q;
    nrm[2] = nrm[2] * q;
    nrm[3] = 1.f;
    if (sign_bit(u) || best <= u) {
      continue;
    }
    // the crossing point
    float p[4], d14[4], d15[4], d16[4];
    for (int i = 0; i < 4; i++) {
      const float acc = mv[i] * u;
      p[i] = acc + start[i] * 1.f;
    }
    for (int i = 0; i < 4; i++) {
      d14[i] = v1[i] - p[i];
      d15[i] = p[i] - v2[i];
      d16[i] = p[i] - v0[i];
    }
    float c17[4], c18[4], c19[4], m21[3];
    opm(c17, e6, d14);
    for (int i = 0; i < 3; i++) {
      m21[i] = mv[i] * nrm[i];
    }
    float mn = m21[0] + m21[1];
    opm(c18, d14, e5);
    mn = mn + m21[2];
    if (0.f <= mn) {
      continue;  // not moving against the front
    }
    opm(c19, d15, d16);
    for (int i = 0; i < 3; i++) {
      c17[i] = c17[i] * nrm[i];
      c18[i] = c18[i] * nrm[i];
      c19[i] = c19[i] * nrm[i];
    }
    c17[1] = c17[1] + c17[0];
    c18[1] = c18[1] + c18[0];
    c19[1] = c19[1] + c19[0];
    c17[1] = c17[1] + c17[2];
    c18[1] = c18[1] + c18[2];
    c19[1] = c19[1] + c19[2];
    if (sign_bit(c17[1]) || sign_bit(c18[1]) || sign_bit(c19[1])) {
      continue;  // outside an edge
    }
    best = u;
    gstore<float>(work, u);
    gstore_q(tri_out, v0);
    gstore_q(tri_out + 16, v1);
    gstore_q(tri_out + 32, v2);
    gstore_q(tri_out + 64, nrm);
    gstore<u32>(tri_out + 80, gload<u32>(tri + 48));
    gstore_q(tri_out + 48, p);
  }
  return 0;
}

/*!
 * (method 10 collide-cache-prim) = resolve-moving-sphere-sphere
 * (this result sphere move max-t action) -> float
 * moving-sphere-sphere-intersect against this prim's sphere. On a hit (under max-t if max-t isn't
 * negative, and moving against the sphere with bit 0 of action) writes a triangle tangent to the
 * sphere at the touching point to result, with the normal and the prim's pat. Returns t or
 * -100000000.0.
 */
u64 method_10_collide_cache_prim_impl(const NativeArgs& args) {
  static const u32 mssi_sym = sym_addr("moving-sphere-sphere-intersect");
  static const u32* mssi_stub = native_stub_slot("moving-sphere-sphere-intersect");
  const u32 prim = (u32)args.a[0];
  const u32 result = (u32)args.a[1];
  const u32 out_point = args.stack - 16;
  const u32 fn = gload<u32>(mssi_sym);
  float t;
  if (fn == *mssi_stub) {
    t = moving_sphere_sphere_intersect_v((u32)args.a[2], (u32)args.a[3], prim, out_point, args);
  } else {
    const u64 call_args[8] = {args.a[2], args.a[3], args.a[0], out_point,
                              args.a[4], args.a[5], args.a[6], args.a[7]};
    t = u2f((u32)native_call_goal(fn, call_args, args));
  }
  const float max_t = u2f((u32)args.a[4]);
  float pt[4], center[4];
  gload_q(pt, out_point);
  gload_q(center, prim);
  if (t < 0.f) {
    return f2gpr(t);
  }
  float d[4] = {pt[0] - center[0], pt[1] - center[1], pt[2] - center[2], 1.f};
  // with a negative max-t the original skips "andi a0, s4, 1": a0 still holds the sphere pointer
  bool facing_test;
  if (max_t < 0.f) {
    facing_test = args.a[2] != 0;
  } else {
    if (max_t <= t) {
      return f2gpr(-100000000.f);
    }
    facing_test = (args.a[5] & 1) != 0;
  }
  if (facing_test) {
    float mv[4], p[4];
    gload_q(mv, (u32)args.a[3]);
    for (int i = 0; i < 4; i++) {
      p[i] = mv[i] * d[i];
    }
    p[1] = p[1] + p[0];
    p[1] = p[1] + p[2];
    if (!sign_bit(p[1])) {
      return f2gpr(-100000000.f);  // moving away
    }
  }

  // the normal, and a triangle tangent to the sphere around the touching point
  float sq[4];
  for (int i = 0; i < 4; i++) {
    sq[i] = d[i] * d[i];
  }
  gstore_q(result + 48, pt);
  float len2 = 1.f * sq[0];
  len2 += 1.f * sq[1];
  len2 = len2 + 1.f * sq[2];
  const float q = rsqrt_q(len2);
  const u32 pat = gload<u32>(gload<u32>(prim + 36) + 68);
  for (int i = 0; i < 3; i++) {
    d[i] = d[i] * q;
  }
  const float sq14[2] = {d[0] * d[0], d[1] * d[1]};
  gstore_q(result + 64, d);
  gstore<u32>(result + 80, pat);
  float t2[4] = {0.f, 0.f, 0.f, 1.f};
  const float s14 = sq14[0] + sq14[1];
  if ((f2u(std::abs(d[0])) | f2u(std::abs(d[1]))) == 0) {
    t2[0] = 0.f + d[2];
  } else {
    t2[0] = 0.f - d[1];
    const float q2 = rsqrt_q(s14);
    t2[1] = 0.f + d[0];
    t2[0] = t2[0] * q2;
    t2[1] = t2[1] * q2;
  }
  float t3[4];
  opm(t3, d, t2);
  static const u32 corners[3][4] = {{0, 0x45800000, 0, 0x3f800000},
                                    {0, 0xc5800000, 0x45800000, 0x3f800000},
                                    {0, 0xc5800000, 0xc5800000, 0x3f800000}};
  for (int k = 0; k < 3; k++) {
    float c[4];
    memcpy(c, corners[k], 16);
    float out[4];
    for (int i = 0; i < 3; i++) {
      float acc = pt[i] * 1.f;
      acc += d[i] * c[0];
      acc += t2[i] * c[1];
      out[i] = acc + t3[i] * c[2];
    }
    out[3] = c[3];
    gstore_q(result + 16 * k, out);
  }
  return f2gpr(t);
}

/*!
 * (method 10 collide-puss-work) (this sphere params) -> symbol
 * Does the sphere overlap one of the params' spheres (in the work's sphere list)? Tests the
 * bounding boxes of all of them first.
 */
u64 method_10_collide_puss_work_impl(const NativeArgs& args) {
  const u32 work = (u32)args.a[0];
  const u32 num = gload<u32>((u32)args.a[2] + 4);
  float s[4];
  gload_q(s, (u32)args.a[1]);
  if (num == 0) {
    return args.st;
  }
  s32 bmin[4], bmax[4], mn[3], mx[3];
  gload_q(bmin, (work + 64) & ~15u);
  gload_q(bmax, (work + 80) & ~15u);
  for (int i = 0; i < 3; i++) {
    mn[i] = (s32)(s[i] - s[3]);
    mx[i] = (s32)(s[i] + s[3]);
  }
  bool outside = false;
  for (int i = 0; i < 3; i++) {
    outside |= (bmin[i] > mx[i]) | (mn[i] > bmax[i]);
  }
  if (outside) {
    return args.st;
  }
  s[3] = 1.f - s[3];
  u32 sphere = work + 96;
  for (u32 k = 0; k != num; k++, sphere += 48) {
    float p[4];
    gload_q(p, sphere);
    float d[4];
    for (int i = 0; i < 4; i++) {
      d[i] = p[i] - s[i];
    }
    for (int i = 0; i < 4; i++) {
      d[i] = d[i] * d[i];
    }
    float acc = 1.f * d[0];
    acc += 1.f * d[1];
    acc += 1.f * d[2];
    const float r = acc - 1.f * d[3];
    // blez of lanes x and y (both r) as a 64-bit int
    const u64 v = ((u64)f2u(r) << 32) | f2u(r);
    if ((s64)v <= 0) {
      return args.st + 8;  // #t
    }
  }
  return args.st;
}

/*!
 * (method 9 collide-puss-work) (this prim params) -> symbol
 * Does one of the params' spheres (in the work's list) touch one of this prim's triangles?
 * Stores each triangle's int bounding box and (for those that overlap the spheres' box) its
 * normal in the work, and calls closest-pt-in-triangle for the spheres whose boxes overlap it.
 */
u64 method_9_collide_puss_work_impl(const NativeArgs& args) {
  static const u32 closest_sym = sym_addr("closest-pt-in-triangle");
  static const u32* closest_stub = native_stub_slot("closest-pt-in-triangle");
  const u32 work = (u32)args.a[0];
  const u32 prim = (u32)args.a[1];
  u32 tri = gload<u32>(prim + 32) + 4908 + ((u32)gload<u16>(prim + 40) << 6);
  u32 ntri = gload<u16>(prim + 42);
  for (; ntri != 0; ntri--, tri += 64) {
    float v0[4], v1[4], v2[4];
    gload_q(v0, tri);
    gload_q(v1, tri + 16);
    gload_q(v2, tri + 32);
    float e4[4], e5[4];
    for (int i = 0; i < 4; i++) {
      e4[i] = v1[i] - v0[i];
      e5[i] = v2[i] - v0[i];
    }
    s32 bmin[4], bmax[4];
    gload_q(bmin, (work + 64) & ~15u);
    gload_q(bmax, (work + 80) & ~15u);
    s32 tmin[4], tmax[4];
    for (int i = 0; i < 4; i++) {
      tmin[i] = (s32)std::min(std::min(v0[i], v1[i]), v2[i]);
      tmax[i] = (s32)std::max(std::max(v0[i], v1[i]), v2[i]);
    }
    float nrm[4];
    opm(nrm, e4, e5);
    nrm[3] = 1.f;
    float sq[4];
    for (int i = 0; i < 4; i++) {
      sq[i] = nrm[i] * nrm[i];
    }
    gstore_q(work + 32, tmin);
    float len2 = 1.f * sq[0];
    len2 += 1.f * sq[1];
    gstore_q(work + 48, tmax);
    len2 = len2 + 1.f * sq[2];
    bool outside = false;
    for (int i = 0; i < 3; i++) {
      outside |= (bmin[i] > tmax[i]) | (tmin[i] > bmax[i]);
    }
    const float q = rsqrt_q(len2);
    if (outside) {
      continue;
    }
    for (int i = 0; i < 3; i++) {
      nrm[i] = nrm[i] * q;
    }
    const u32 num_spheres = gload<u32>((u32)args.a[2] + 4);
    gstore_q(work + 16, nrm);

    u32 sphere = work + 96;
    for (u32 k = 0; k != num_spheres; k++, sphere += 48) {
      s32 smin[4], smax[4], wmin[4], wmax[4];
      gload_q(smin, (sphere + 16) & ~15u);
      gload_q(wmax, (work + 48) & ~15u);
      gload_q(smax, (sphere + 32) & ~15u);
      gload_q(wmin, (work + 32) & ~15u);
      bool out = false;
      for (int i = 0; i < 3; i++) {
        out |= (smin[i] > wmax[i]) | (wmin[i] > smax[i]);
      }
      if (out) {
        continue;
      }
      const u32 fn = gload<u32>(closest_sym);
      if (fn == *closest_stub) {
        closest_pt_in_triangle_v(work, sphere, tri, work + 16);
      } else {
        const u64 call_args[8] = {work,      sphere,    tri,       work + 16u,
                                  args.a[4], args.a[5], args.a[6], args.a[7]};
        native_call_goal(fn, call_args, args);
      }
      float pt[4], sp[4];
      gload_q(pt, work);
      gload_q(sp, sphere);
      float d[4];
      for (int i = 0; i < 3; i++) {
        d[i] = pt[i] - sp[i];
      }
      const float r2 = sp[3] * sp[3];
      for (int i = 0; i < 3; i++) {
        d[i] = d[i] * d[i];
      }
      float acc = 1.f * d[0];
      acc += 1.f * d[1];
      float dist = acc + 1.f * d[2];
      dist = dist - r2;
      // pcpyud: lanes z and w as a 64-bit int
      const u64 v = ((u64)f2u(dist) << 32) | f2u(d[2]);
      if (!((s64)v > 0)) {
        return args.st + 8;  // #t
      }
    }
  }
  return args.st;
}

u64 pc_upload_collide_frag_impl(const NativeArgs& args) {
  upload_collide_frag(args.a[0], args.a[2]);
  return 0;
}

// ---------------------------------------------------------------------------
// (AI-assisted) 3DS: skipping collide fragments that can't add a triangle
// ---------------------------------------------------------------------------
// fill-from-background uploads, unpacks and transforms every fragment whose bounding sphere
// touches the query, then methods 26-28 test the box of each triangle. Most fragments add nothing
// (a ground probe of an enemy in Misty: ~15 fragments, ~0.3 triangles each).
// pc-collide-frag-may-hit? first tests the box of the fragment's vertices, from the packed vertex
// data. It only says no when, on one axis, every vertex is outside the query box: then the box of
// every triangle is too, and the fill would add nothing (it would only leave other vertices in the
// scratchpad).

//! the float method 32 makes of the packed value u (offset as in method 32): monotonic in u
inline float unpacked_coord(u32 u, float offset) {
  return u2f(0x4d000000 + u) - offset;
}

//! The interval of out[i] = m[3][i] + m[0][i] * v[0] + m[1][i] * v[1] + m[2][i] * v[2]
//! (transform_point) over v in [lo, hi], widened by more than transform_point's float rounding.
inline void transform_interval(double olo[3],
                               double ohi[3],
                               const float m[4][4],
                               const double lo[3],
                               const double hi[3]) {
  for (int i = 0; i < 3; i++) {
    const double c = m[3][i];
    double a = c, b = c, mag = std::fabs(c);
    for (int j = 0; j < 3; j++) {
      const double p = (double)m[j][i] * lo[j];
      const double q = (double)m[j][i] * hi[j];
      a += std::min(p, q);
      b += std::max(p, q);
      mag += std::max(std::fabs(p), std::fabs(q));
    }
    // 3 multiplies and 3 adds that round to float (2^-24 each): 2^-18 is more than 10x that
    const double margin = mag * 0x1p-18 + 2.0;
    olo[i] = a - margin;
    ohi[i] = b + margin;
  }
}

//! Can (s32) of a float in [lo, hi] be in [bmin, bmax]? ((s32) truncates: below bmin - 1 gives
//! less than bmin, above bmax + 1 more than bmax.) NaN: yes.
inline bool interval_may_hit(double lo, double hi, s32 bmin, s32 bmax) {
  return !(hi < (double)bmin - 1.0 || lo > (double)bmax + 1.0);
}

//! min and max of the packed values on each axis of count vertices at data
inline void scan_vertex_box(const u16* data, int count, u32 ulo[3], u32 uhi[3]) {
  for (int i = 0; i < 3; i++) {
    ulo[i] = 0xffff;
    uhi[i] = 0;
  }
  for (int k = 0; k < count; k++) {
    for (int i = 0; i < 3; i++) {
      const u32 u = data[3 * k + i];
      ulo[i] = std::min(ulo[i], u);
      uhi[i] = std::max(uhi[i], u);
    }
  }
}

/*!
 * (AI-assisted) The vertex boxes of the meshes, cached: the packed vertices of a collide-frag-mesh
 * never change, but scanning them was most of the cost of each call (0.30 ms per frame for 56-127
 * calls on New 3DS hardware in Snowy Mountain). Direct-mapped by mesh address. An entry is only
 * used for the same mesh, vertex data address and count, and the same g_link_generation (a level
 * that was loaded since could have put another mesh at that address). The cached values are the
 * scan's, so results are the same.
 * Always on (no config.ini switch since Oct 18). Host debugging: OPENGOAL_FRAG_CACHE=0 scans
 * every time.
 * OPENGOAL_FRAG_CACHE_VERIFY=1 (host): scan every time and compare with the cache.
 */
struct FragBoxCache {
  struct Entry {
    u32 mesh = 0, data = 0, gen = 0;
    u16 count = 0;
    u16 lo[3] = {}, hi[3] = {};
  };
  static constexpr u32 kSize = 1024;
  Entry e[kSize];
  int on = -1;  // -1: not read yet
  bool verify = false;
  u64 hits = 0, misses = 0, mismatches = 0;
};
FragBoxCache g_frag_box_cache;

bool frag_box_cache_on() {
  auto& c = g_frag_box_cache;
  if (c.on < 0) {
    c.on = 1;
#ifndef __3DS__
    const char* env = getenv("OPENGOAL_FRAG_CACHE");
    if (env && env[0] == '0') {
      c.on = 0;
    }
    const char* venv = getenv("OPENGOAL_FRAG_CACHE_VERIFY");
    c.verify = venv && venv[0] == '1';
#endif
    if (!c.on) {
      printf("[collide] pc-collide-frag-may-hit? vertex box cache off\n");
    }
  }
  return c.on == 1;
}

void frag_vertex_box(u32 mesh, int count, u32 ulo[3], u32 uhi[3]) {
  const u32 data_addr = gload<u32>(mesh);
  const u16* data = (const u16*)gptr(data_addr);
  if (!frag_box_cache_on()) {
    scan_vertex_box(data, count, ulo, uhi);
    return;
  }
  auto& c = g_frag_box_cache;
  auto& e = c.e[(mesh >> 4) & (FragBoxCache::kSize - 1)];
  if (e.mesh == mesh && e.data == data_addr && e.count == (u16)count &&
      e.gen == g_link_generation) {
    for (int i = 0; i < 3; i++) {
      ulo[i] = e.lo[i];
      uhi[i] = e.hi[i];
    }
    c.hits++;
    if (c.verify) {
      u32 vlo[3], vhi[3];
      scan_vertex_box(data, count, vlo, vhi);
      if (memcmp(vlo, ulo, sizeof(vlo)) || memcmp(vhi, uhi, sizeof(vhi))) {
        c.mismatches++;
        printf("frag box cache MISMATCH: mesh #x%x\n", mesh);
      }
      if ((c.hits & 0xfff) == 0) {
        printf("frag box cache verify: %llu hits, %llu misses, %llu mismatches\n",
               (unsigned long long)c.hits, (unsigned long long)c.misses,
               (unsigned long long)c.mismatches);
      }
    }
    return;
  }
  c.misses++;
  scan_vertex_box(data, count, ulo, uhi);
  e.mesh = mesh;
  e.data = data_addr;
  e.gen = g_link_generation;
  e.count = (u16)count;
  for (int i = 0; i < 3; i++) {
    e.lo[i] = (u16)ulo[i];
    e.hi[i] = (u16)uhi[i];
  }
}

/*!
 * (pc-collide-frag-may-hit? cache mesh inst mode) -> symbol
 * Can methods 26-28 add a triangle of mesh (a collide-frag-mesh, of instance inst or #f)?
 * mode 0: the cache's collide-box4w (methods 26 and 28, world ints). mode 1: the box and matrix of
 * *collide-work* (method 27, in the frame of method 29). Other modes: #t.
 */
u64 pc_collide_frag_may_hit_impl(const NativeArgs& args) {
  const u64 yes = args.st + 8;  // #t
  const u32 cache = (u32)args.a[0];
  const u32 mesh = (u32)args.a[1];
  const u32 inst = (u32)args.a[2];
  const int mode = (int)args.a[3];
  const int count = gload<u8>(mesh + 24);
  if ((mode != 0 && mode != 1) || count == 0) {
    return yes;
  }

  // min and max of the packed values on each axis (what __pc-upload-collide-frag unpacks)
  u32 ulo[3], uhi[3];
  frag_vertex_box(mesh, count, ulo, uhi);
  // the floats method 32 makes of them (the same operations)
  s32 base_i[4];
  gload_q(base_i, mesh + 12);
  float flo[3], fhi[3];
  for (int i = 0; i < 3; i++) {
    const float offset = u2f(0x4d000000) - (float)base_i[i];
    flo[i] = unpacked_coord(ulo[i], offset);
    fhi[i] = unpacked_coord(uhi[i], offset);
  }

  bool may_hit = true;
  const bool transformed = inst != (u32)args.st;
  if (mode == 0 && !transformed) {
    // exact: the ints are (s32) of these floats, also monotonic
    const Vec4i bmin = gload<Vec4i>(cache + 60);
    const Vec4i bmax = gload<Vec4i>(cache + 76);
    for (int i = 0; i < 3; i++) {
      if ((s32)fhi[i] < bmin.v[i] || (s32)flo[i] > bmax.v[i]) {
        may_hit = false;
      }
    }
  } else {
    double wlo[3], whi[3];
    for (int i = 0; i < 3; i++) {
      wlo[i] = flo[i];
      whi[i] = fhi[i];
    }
    if (transformed) {
      // the instance's matrix, as method 32 makes it
      const u32 xf = inst;
      s32 trans_i[4], r0[4], r1[4], r2[4];
      unpack_halves(trans_i, gload<u64>(xf + 52), 10);
      unpack_halves(r0, gload<u64>(xf + 28), 16);
      unpack_halves(r1, gload<u64>(xf + 36), 16);
      unpack_halves(r2, gload<u64>(xf + 44), 16);
      float trans_add[4];
      gload_q(trans_add, xf + 12);
      float m[4][4];
      for (int i = 0; i < 4; i++) {
        m[3][i] = (float)trans_i[i];
        m[0][i] = ((float)r0[i]) * (1.f / 4096.f);
        m[1][i] = ((float)r1[i]) * (1.f / 4096.f);
        m[2][i] = ((float)r2[i]) * (1.f / 4096.f);
      }
      for (int i = 0; i < 3; i++) {
        m[3][i] = m[3][i] + trans_add[i];
      }
      const double llo[3] = {wlo[0], wlo[1], wlo[2]}, lhi[3] = {whi[0], whi[1], whi[2]};
      transform_interval(wlo, whi, m, llo, lhi);
    }
    Vec4i bmin, bmax;
    if (mode == 0) {
      bmin = gload<Vec4i>(cache + 60);
      bmax = gload<Vec4i>(cache + 76);
    } else {
      // method 29: into the query's frame with the matrix in *collide-work*; the box is there too
      static const u32 collide_work_sym = sym_addr("*collide-work*");
      const u32 cw = gload<u32>(collide_work_sym);
      float m[4][4];
      for (int i = 0; i < 4; i++) {
        gload_q(m[i], cw + 48 + 16 * i);
      }
      const double llo[3] = {wlo[0], wlo[1], wlo[2]}, lhi[3] = {whi[0], whi[1], whi[2]};
      transform_interval(wlo, whi, m, llo, lhi);
      bmin = gload<Vec4i>(cw + 16);
      bmax = gload<Vec4i>(cw + 32);
    }
    for (int i = 0; i < 3; i++) {
      if (!interval_may_hit(wlo[i], whi[i], bmin.v[i], bmax.v[i])) {
        may_hit = false;
      }
    }
  }

#ifndef __3DS__
  if (g_frag_skip_verify.on) {
    auto& v = g_frag_skip_verify;
    v.calls++;
    v.predicted_mesh = may_hit ? 0 : mesh;
    if (!may_hit) {
      v.predicted++;
    }
    if ((v.calls & 0x3fff) == 0) {
      printf("frag skip verify: %llu calls, %llu skips predicted (%.1f%%), %llu mismatches\n",
             (unsigned long long)v.calls, (unsigned long long)v.predicted,
             100.0 * (double)v.predicted / (double)v.calls, (unsigned long long)v.mismatches);
    }
    return yes;
  }
#endif
  return may_hit ? yes : args.st;
}


// ---------------------------------------------------------------------------
// (AI-assisted) 3DS: the box queries of the collide-cache fills, in C++
// ---------------------------------------------------------------------------
// fill-using-bounding-box (Jak's physics and the enemies' ground probes, every frame) walks the
// drawable trees of every active level with GOAL methods (collide-with-box: a virtual call per
// node, collide-cache-using-box-test and drawable-sphere-box-intersect? calls per node), then
// fill-from-foreground-using-box walks the four collide-shape lists. In C mode their 128-bit PS2
// ops (pcgtw, ppach) cost ~200 ARM instructions per shape. These natives do the same tests and
// the same stores in the same order:
//   pc-bsp-collide-with-box: (method collide-with-box bsp-header), after its *collide-stats* count
//   pc-fill-from-foreground-using-box: (method fill-from-foreground-using-box collide-cache)
// Always on (no config.ini switch since Oct 18). Both return #f when off (host debugging:
// OPENGOAL_COLLIDE_NATIVE=0): the GOAL code then runs. The node types are recognized by their type; other types (and the
// collide-shape-prim methods other than sphere and group) are called through their method table
// like the original. instance-sphere-box-intersect? stays GOAL (called for the collide fragments
// of the ties that pass the box tests).
// OPENGOAL_COLLIDE_NATIVE_VERIFY=1 (host): every call runs the GOAL version first (the guards
// return #f while it runs), keeps what it wrote (the collide-cache object or *collide-list*), puts
// the old bytes back, runs the native one and compares.

struct CollideNative {
  int on = -1;  // -1: not read yet
  bool verify = false;
  bool in_goal = false;  // verify: the GOAL version is running
  u64 checks = 0, mismatches = 0;
  bool overflow_logged = false;
};
CollideNative g_collide_native;

bool collide_native_on() {
  auto& c = g_collide_native;
  if (c.on < 0) {
    c.on = 1;
#ifndef __3DS__
    const char* env = getenv("OPENGOAL_COLLIDE_NATIVE");
    if (env && env[0] == '0') {
      c.on = 0;
    }
    const char* venv = getenv("OPENGOAL_COLLIDE_NATIVE_VERIFY");
    c.verify = venv && venv[0] == '1';
#endif
    printf("[collide] native box fills (pc-bsp-collide-with-box, "
           "pc-fill-from-foreground-using-box) %s%s\n",
           c.on ? "on" : "off (OPENGOAL_COLLIDE_NATIVE=0)", c.verify ? ", verify" : "");
  }
  return c.on == 1 && !c.in_goal;
}

//! vftoi0 as goalc's C does it (gc_cvtt): truncate, saturate, NaN gives 0
inline s32 cvtt(float f) {
#if defined(__arm__) && defined(__ARM_FP)
  // VFP's round-towards-zero conversion saturates and gives 0 for NaN (as gc_cvtt on ARM): the
  // compares below would each wait for the VFP pipeline in VMRS
  float r;
  __asm__("vcvt.s32.f32 %0, %1" : "=t"(r) : "t"(f));
  s32 i;
  memcpy(&i, &r, 4);
  return i;
#endif
  if (f != f) {
    return 0;
  }
  if (f >= 2147483648.0f) {
    return INT32_MAX;
  }
  if (f <= -2147483648.0f) {
    return INT32_MIN;
  }
  return (s32)f;
}

//! Is the box of the sphere s (ftoi of xyz - w and xyz + w) outside [bmin, bmax] in x, y or z?
//! (pcgtw lo, max; pcgtw min, hi; por; ppach; shl 16: lanes x y z)
inline bool sphere_outside_box4w(const Vec4f& s, const Vec4i& bmin, const Vec4i& bmax) {
  const float sv[3] = {s.x, s.y, s.z};
  bool out = false;
  for (int i = 0; i < 3; i++) {
    const float lo = sv[i] - s.w;
    const float hi = sv[i] + s.w;
    out |= (cvtt(lo) > bmax.v[i]) | (bmin.v[i] > cvtt(hi));
  }
  return out;
}

//! the 1.0 that collide-cache-using-box-test multiplies by (vf0 from memory in goalc's C: the
//! multiplies happen, so a NaN comes out as the FPU makes it)
volatile float g_collide_one = 1.0f;

struct BoxWalk {
  const NativeArgs* args;
  float one;
  Vec4f neg_r;  // *collide-work* collide-sphere-neg-r
  Vec4i bmin, bmax;
  u32 box4w;  // its address (instance-sphere-box-intersect?'s argument)
  u32 list;
  u32 t_draw_node, t_ia_node, t_tree_cfrag, t_ia_cfrag, t_cfrag, t_tree_tie, t_ia_tie, t_tie;
  u32 drawable_method;  // (method collide-with-box drawable): does nothing
  u32 isbi;             // instance-sphere-box-intersect?
};

//! the value of a symbol whose address was looked up once (sym_addr hashes the name)
inline u32 sym_value(u32 sym) {
  return gload<u32>(sym);
}

//! collide-cache-using-box-test: is the bounding sphere at addr within the query's sphere?
inline bool box_test(const BoxWalk& w, u32 addr) {
  const Vec4f b = gload<Vec4f>(addr);
  float d[4] = {b.x - w.neg_r.x, b.y - w.neg_r.y, b.z - w.neg_r.z, b.w - w.neg_r.w};
  for (int i = 0; i < 4; i++) {
    d[i] = d[i] * d[i];
  }
  float acc = w.one * d[0];
  float t = w.one * d[1];
  acc = acc + t;
  t = w.one * d[2];
  acc = acc + t;
  t = w.one * d[3];
  const float r = acc - t;
  // (<= (the-as int (.mov vf1)) 0): lanes x and y (equal) as a 64-bit int
  const u64 bits = f2u(r);
  return (s64)((bits << 32) | bits) <= 0;
}

void add_list_item(BoxWalk& w, u32 mesh, u32 inst) {
  const s32 n = gload<s32>(w.list);
  if (n >= 256 && !g_collide_native.overflow_logged) {
    // the original doesn't check either (it writes past *collide-list*): only log it
    g_collide_native.overflow_logged = true;
    printf("[collide] WARNING: *collide-list* overflow (%d items, 256 fit)\n", n + 1);
  }
  const u32 item = w.list + 16 + ((u32)n << 4);
  gstore<u32>(item, mesh);
  gstore<u32>(item + 4, inst);
  gstore<u32>(w.list, (u32)(n + 1));
}

void box_walk(BoxWalk& w, u32 obj, s64 count) {
  const u32 type = gload<u32>(obj - 4);
  if (type == w.t_draw_node) {
    for (s64 i = 0; i < count; i++, obj += 32) {
      if (box_test(w, obj + 12)) {
        box_walk(w, gload<u32>(obj + 4), gload<u8>(obj + 2));
      }
    }
  } else if (type == w.t_ia_node || type == w.t_ia_cfrag) {
    box_walk(w, obj + 32, gload<s16>(obj + 2));
  } else if (type == w.t_ia_tie) {
    const s16 n = gload<s16>(obj + 2);
    if (n != 0) {
      box_walk(w, obj + 32, n);
    }
  } else if (type == w.t_tree_cfrag) {
    box_walk(w, gload<u32>(obj + 28), gload<s16>(obj + 2));
  } else if (type == w.t_tree_tie) {
    const s16 n = gload<s16>(obj + 2);
    if (n != 0) {
      box_walk(w, gload<u32>(obj + 28), n);
    }
  } else if (type == w.t_cfrag) {
    for (s64 i = 0; i < count; i++, obj += 32) {
      if (box_test(w, obj + 12) &&
          !sphere_outside_box4w(gload<Vec4f>(obj + 12), w.bmin, w.bmax)) {
        add_list_item(w, gload<u32>(obj + 4), (u32)w.args->st);
      }
    }
  } else if (type == w.t_tie) {
    for (s64 i = 0; i < count; i++, obj += 64) {
      if ((gload<u16>(obj + 42) & 1) || !box_test(w, obj + 12) ||
          sphere_outside_box4w(gload<Vec4f>(obj + 12), w.bmin, w.bmax)) {
        continue;
      }
      const u32 frags = gload<u32>(gload<u32>(obj + 8) + 136);
      if (frags == 0) {
        continue;
      }
      u32 frag = frags + 32;
      for (s64 j = 0; j < gload<s16>(frags + 2); j++, frag += 32) {
        const u64 call_args[8] = {frag, obj, w.box4w, w.args->a[3],
                                  w.args->a[4], w.args->a[5], w.args->a[6], w.args->a[7]};
        if ((u32)native_call_goal(w.isbi, call_args, *w.args) != (u32)w.args->st) {
          add_list_item(w, gload<u32>(frag + 4), obj);
        }
      }
    }
  } else {
    const u32 method = gload<u32>(type + 60);
    if (method != w.drawable_method) {
      const u64 call_args[8] = {obj,          (u64)count,   w.list,       w.args->a[3],
                                w.args->a[4], w.args->a[5], w.args->a[6], w.args->a[7]};
      native_call_goal(method, call_args, *w.args);
    }
  }
}

void bsp_box_walk(const NativeArgs& args) {
  static const u32 collide_work_sym = sym_addr("*collide-work*");
  BoxWalk w;
  w.args = &args;
  w.one = g_collide_one;
  const u32 cw = gload<u32>(collide_work_sym);
  w.neg_r = gload<Vec4f>(cw);
  w.box4w = cw + 16;
  w.bmin = gload<Vec4i>(cw + 16);
  w.bmax = gload<Vec4i>(cw + 32);
  w.list = (u32)args.a[2];
  static const u32 s_draw_node = sym_addr("draw-node");
  static const u32 s_ia_node = sym_addr("drawable-inline-array-node");
  static const u32 s_tree_cfrag = sym_addr("drawable-tree-collide-fragment");
  static const u32 s_ia_cfrag = sym_addr("drawable-inline-array-collide-fragment");
  static const u32 s_cfrag = sym_addr("collide-fragment");
  static const u32 s_tree_tie = sym_addr("drawable-tree-instance-tie");
  static const u32 s_ia_tie = sym_addr("drawable-inline-array-instance-tie");
  static const u32 s_tie = sym_addr("instance-tie");
  static const u32 s_drawable = sym_addr("drawable");
  static const u32 s_isbi = sym_addr("instance-sphere-box-intersect?");
  w.t_draw_node = sym_value(s_draw_node);
  w.t_ia_node = sym_value(s_ia_node);
  w.t_tree_cfrag = sym_value(s_tree_cfrag);
  w.t_ia_cfrag = sym_value(s_ia_cfrag);
  w.t_cfrag = sym_value(s_cfrag);
  w.t_tree_tie = sym_value(s_tree_tie);
  w.t_ia_tie = sym_value(s_ia_tie);
  w.t_tie = sym_value(s_tie);
  w.drawable_method = gload<u32>(sym_value(s_drawable) + 60);
  w.isbi = sym_value(s_isbi);
  const u32 trees = gload<u32>((u32)args.a[0] + 36);
  for (s64 i = 0; i < gload<s16>(trees + 2); i++) {
    const u32 tree = gload<u32>(trees + 28 + 4 * (u32)i);
    // the bsp-header method passes its own count on
    box_walk(w, tree, (s64)args.a[1]);
  }
}

#ifndef __3DS__
//! verify mode: run the GOAL version (method of obj's type at method_offset), then the native one
//! from the same memory, and compare [lo, lo + size)
template <typename F>
void collide_native_verify(const char* what,
                           const NativeArgs& args,
                           u32 method_offset,
                           u32 lo,
                           u32 size,
                           F&& native) {
  auto& c = g_collide_native;
  std::vector<u8> before(g_ee_main_mem + lo, g_ee_main_mem + lo + size);
  const u32 method = gload<u32>(gload<u32>((u32)args.a[0] - 4) + method_offset);
  c.in_goal = true;
  native_call_goal(method, args.a, args);
  c.in_goal = false;
  std::vector<u8> goal(g_ee_main_mem + lo, g_ee_main_mem + lo + size);
  memcpy(g_ee_main_mem + lo, before.data(), size);
  native();
  c.checks++;
  if (memcmp(goal.data(), g_ee_main_mem + lo, size)) {
    u32 off = 0;
    while (goal[off] == g_ee_main_mem[lo + off]) {
      off++;
    }
    c.mismatches++;
    printf("collide native verify MISMATCH in %s: byte 0x%x of %u (goal %02x native %02x)\n", what,
           off, size, goal[off], g_ee_main_mem[lo + off]);
  }
  if ((c.checks & 0x3fff) == 0) {
    printf("collide native verify: %llu checks, %llu mismatches\n",
           (unsigned long long)c.checks, (unsigned long long)c.mismatches);
  }
}
#endif

/*!
 * (pc-bsp-collide-with-box bsp count list) -> symbol
 * (method collide-with-box bsp-header) after its *collide-stats* count: the box walk of every
 * drawable tree into list. #f when off.
 */
u64 pc_bsp_collide_with_box_impl(const NativeArgs& args) {
  if (!collide_native_on()) {
    return args.st;
  }
#ifndef __3DS__
  if (g_collide_native.verify) {
    collide_native_verify("pc-bsp-collide-with-box", args, 60, (u32)args.a[2], 16 + 16 * 300,
                          [&] { bsp_box_walk(args); });
    return args.st + 8;
  }
#endif
  bsp_box_walk(args);
  return args.st + 8;
}

struct FgBox {
  const NativeArgs* args;
  u32 cache;
  u64 collide_with;
  Vec4i bmin, bmax;
  u32 t_sphere, t_group;
};

void add_fg_prim_using_box(const FgBox& f, u32 prim);

//! (method add-fg-prim-using-box collide-shape-prim-sphere)
void add_fg_sphere(const FgBox& f, u32 prim, u32 method) {
  const u32 cache = f.cache;
  const u32 n = gload<u32>(cache + 4);
  if (n == 100) {
    // the original prints an error: let it
    const u64 call_args[8] = {prim,          cache,         f.args->a[2], f.args->a[3],
                              f.args->a[4], f.args->a[5], f.args->a[6], f.args->a[7]};
    native_call_goal(method, call_args, *f.args);
    return;
  }
  const u32 slot = cache + 108 + n * 48;
  const u32 zero[4] = {0, 0, 0, 0};
  gstore_q(slot + 32, zero);
  gstore<u32>(slot + 36, prim);
  gcopy_q(slot, prim + 12);
  gcopy_q(slot + 16, prim + 28);
  gstore<u32>(cache + 4, n + 1);
  gstore<u32>(slot + 32, cache);
}

//! (method add-fg-prim-using-box collide-shape-prim-group)
void add_fg_group(const FgBox& f, u32 prim) {
  const u32 n = gload<u32>(prim + 68);
  for (u32 k = 0; k < n; k++) {
    const u32 child = gload<u32>(prim + 76 + 4 * k);
    if (!(f.collide_with & gload<u64>(child + 28))) {
      continue;
    }
    if (sphere_outside_box4w(gload<Vec4f>(child + 12), f.bmin, f.bmax)) {
      continue;
    }
    add_fg_prim_using_box(f, child);
  }
}

void add_fg_prim_using_box(const FgBox& f, u32 prim) {
  static const u32* mesh_stub = native_stub_slot("(method 12 collide-shape-prim-mesh)");
  const u32 type = gload<u32>(prim - 4);
  const u32 method = gload<u32>(type + 64);
  if (type == f.t_sphere) {
    add_fg_sphere(f, prim, method);
  } else if (type == f.t_group) {
    add_fg_group(f, prim);
  } else {
    const u64 call_args[8] = {prim,          f.cache,       f.args->a[2], f.args->a[3],
                              f.args->a[4], f.args->a[5], f.args->a[6], f.args->a[7]};
    if (method == *mesh_stub) {
      method_12_collide_shape_prim_mesh_impl(
          NativeArgs{call_args, f.args->pp, f.args->st, f.args->stack});
    } else {
      native_call_goal(method, call_args, *f.args);
    }
  }
}

void fg_box_walk_list(const FgBox& f, u32 list_sym) {
  const u32 list = gload<u32>(list_sym);
  const u32 end = list + 44;  // alive-list-end
  u32 conn = gload<u32>(list + 28);
  u32 next = gload<u32>(conn);
  const u32 proc = gload<u32>(f.cache + 12);
  while (conn != end) {
    const u32 shape = (u32)gload<s32>(conn + 20);  // param1
    const u32 prim = gload<u32>(shape + 156);      // root-prim
    if ((f.collide_with & gload<u64>(prim + 28)) &&
        !sphere_outside_box4w(gload<Vec4f>(prim + 12), f.bmin, f.bmax) &&
        proc != gload<u32>(shape + 136)) {
      add_fg_prim_using_box(f, prim);
    }
    conn = next;
    next = gload<u32>(next);
  }
}

void fg_box_fill(const NativeArgs& args) {
  static const u32 player_sym = sym_addr("*collide-player-list*");
  static const u32 hit_by_player_sym = sym_addr("*collide-hit-by-player-list*");
  static const u32 usually_sym = sym_addr("*collide-usually-hit-by-player-list*");
  static const u32 hit_by_others_sym = sym_addr("*collide-hit-by-others-list*");
  FgBox f;
  f.args = &args;
  f.cache = (u32)args.a[0];
  f.collide_with = gload<u64>(f.cache + 92);
  f.bmin = gload<Vec4i>(f.cache + 60);
  f.bmax = gload<Vec4i>(f.cache + 76);
  static const u32 s_sphere = sym_addr("collide-shape-prim-sphere");
  static const u32 s_group = sym_addr("collide-shape-prim-group");
  f.t_sphere = sym_value(s_sphere);
  f.t_group = sym_value(s_group);
  if (f.collide_with & 16) {
    fg_box_walk_list(f, player_sym);
  }
  if (f.collide_with & 14) {
    if (f.collide_with & 2) {
      fg_box_walk_list(f, hit_by_player_sym);
    }
    if (f.collide_with & 4) {
      fg_box_walk_list(f, usually_sym);
    }
    if (f.collide_with & 8) {
      fg_box_walk_list(f, hit_by_others_sym);
    }
  }
}

/*!
 * (pc-fill-from-foreground-using-box cache) -> symbol
 * (method fill-from-foreground-using-box collide-cache). #f when off.
 */
u64 pc_fill_from_foreground_using_box_impl(const NativeArgs& args) {
  if (!collide_native_on()) {
    return args.st;
  }
#ifndef __3DS__
  if (g_collide_native.verify) {
    const u32 cache = (u32)args.a[0];
    const u32 size = gload<u16>(gload<u32>(cache - 4) + 8);
    collide_native_verify("pc-fill-from-foreground-using-box", args, 16 + 4 * 22, cache - 4,
                          size, [&] { fg_box_fill(args); });
    return args.st + 8;
  }
#endif
  fg_box_fill(args);
  return args.st + 8;
}
}  // namespace

// scratch: the two vectors, then moving-sphere-triangle-intersect's (called directly)
const NativeImpl method_9_collide_cache_prim =
    MIPS2C_NATIVE_IMPL(method_9_collide_cache_prim_impl, 0, 48);
const NativeImpl method_26_collide_cache = MIPS2C_NATIVE_IMPL(method_26_collide_cache_impl, 0, 0);
const NativeImpl method_27_collide_cache = MIPS2C_NATIVE_IMPL(method_27_collide_cache_impl, 0, 0);
const NativeImpl method_29_collide_cache = MIPS2C_NATIVE_IMPL(method_29_collide_cache_impl, 0, 0);
const NativeImpl method_32_collide_cache = MIPS2C_NATIVE_IMPL(method_32_collide_cache_impl, 0, 0);
// method 28 is the same code as method 26
const NativeImpl method_28_collide_cache = MIPS2C_NATIVE_IMPL(method_26_collide_cache_impl, 0, 0);
const NativeImpl method_30_collide_cache = MIPS2C_NATIVE_IMPL(method_30_collide_cache_impl, 0, 0);
// scratch: the point moving-sphere-sphere-intersect writes
const NativeImpl method_10_collide_cache_prim =
    MIPS2C_NATIVE_IMPL(method_10_collide_cache_prim_impl, 0, 16);
const NativeImpl method_9_collide_puss_work =
    MIPS2C_NATIVE_IMPL(method_9_collide_puss_work_impl, 0, 0);
const NativeImpl method_10_collide_puss_work =
    MIPS2C_NATIVE_IMPL(method_10_collide_puss_work_impl, 0, 0);
const NativeImpl method_12_collide_shape_prim_mesh =
    MIPS2C_NATIVE_IMPL(method_12_collide_shape_prim_mesh_impl, 0, 0);
const NativeImpl method_13_collide_shape_prim_mesh =
    MIPS2C_NATIVE_IMPL(method_13_collide_shape_prim_mesh_impl, 0, 0);
// method 14 is the same code as method 12
const NativeImpl method_14_collide_shape_prim_mesh =
    MIPS2C_NATIVE_IMPL(method_12_collide_shape_prim_mesh_impl, 0, 0);
const NativeImpl pc_upload_collide_frag = MIPS2C_NATIVE_IMPL(pc_upload_collide_frag_impl, 0, 0);
const NativeImpl pc_collide_frag_may_hit = MIPS2C_NATIVE_IMPL_GOAL(pc_collide_frag_may_hit_impl, 0);
// (AI-assisted) the box fills (see above): no mips2c version, OPENGOAL_COLLIDE_NATIVE=0 (host) turns them off
const NativeImpl pc_bsp_collide_with_box =
    MIPS2C_NATIVE_IMPL(pc_bsp_collide_with_box_impl, NATIVE_CALLS_GOAL, 0);
const NativeImpl pc_fill_from_foreground_using_box =
    MIPS2C_NATIVE_IMPL_GOAL(pc_fill_from_foreground_using_box_impl, NATIVE_CALLS_GOAL);

}  // namespace Mips2C::jak1::native

namespace Mips2C::jak1::pc_collide_frag_may_hit {
void link() {
#ifndef __3DS__
  {
    const char* env = getenv("OPENGOAL_FRAG_SKIP_VERIFY");
    native::g_frag_skip_verify.on = env && env[0] == '1';
  }
#endif
  gLinkedFunctionTable.reg("pc-collide-frag-may-hit?", native::pc_collide_frag_may_hit.as_exec, 64,
                           &native::pc_collide_frag_may_hit);
  gLinkedFunctionTable.reg("pc-fill-from-foreground-using-box",
                           native::pc_fill_from_foreground_using_box.as_exec, 64,
                           &native::pc_fill_from_foreground_using_box);
}
}  // namespace Mips2C::jak1::pc_collide_frag_may_hit

// (AI-assisted) bsp.gc uses it, and is loaded before collide-cache.gc
namespace Mips2C::jak1::pc_bsp_collide_with_box {
void link() {
  gLinkedFunctionTable.reg("pc-bsp-collide-with-box", native::pc_bsp_collide_with_box.as_exec, 64,
                           &native::pc_bsp_collide_with_box);
}
}  // namespace Mips2C::jak1::pc_bsp_collide_with_box
