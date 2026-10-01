/*!
 * @file native_collide_cache.cpp
 * (AI-assisted)
 * Native versions of the mips2c functions in collide_cache.cpp. See game/mips2c/mips2c_native.h for
 * the rules (same float operations, grouped the same way, as the mips2c code).
 */

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "game/kernel/jak1/kscheme.h"
#include "game/mips2c/jak1_functions/native_functions.h"

extern const u32* max_tri_count;  // collide_cache.cpp

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
void add_mesh_tris(u32 cache,
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

}  // namespace Mips2C::jak1::native
