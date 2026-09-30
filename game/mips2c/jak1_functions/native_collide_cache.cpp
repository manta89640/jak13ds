/*!
 * @file native_collide_cache.cpp
 * (AI-assisted)
 * Native versions of the mips2c functions in collide_cache.cpp. See game/mips2c/mips2c_native.h for
 * the rules (same float operations, grouped the same way, as the mips2c code).
 */

#include <algorithm>
#include <cstdio>

#include "game/kernel/jak1/kscheme.h"
#include "game/mips2c/jak1_functions/native_functions.h"

extern const u32* max_tri_count;  // collide_cache.cpp

namespace Mips2C::jak1 {
const u32* collide_vu0_buffer();  // collide_cache.cpp
}

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
    const u64 call_args[8] = {sphere, move, f2gpr(gload<float>(sphere + 12)),
                              tri,    out_point, out_normal, args.a[6], args.a[7]};
    const u32 fn = gload<u32>(mst_sym);
    const float t = u2f((u32)(fn == *mst_stub
                                  ? moving_sphere_triangle_intersect_impl(
                                        NativeArgs{call_args, args.pp, args.st, 0})
                                  : native_call_goal(fn, call_args, args)));
    float mv[4], normal[4];
    memcpy(mv, gptr(move), 16);
    memcpy(normal, gptr(out_normal), 16);
    if (t < 0.f) {
      continue;
    }
    float point[4];
    memcpy(point, gptr(out_point), 16);
    if (best <= t) {
      continue;
    }
    if (flags & 1) {
      // moving against the triangle, and the sphere in front of it
      float c[4], m[4], d[4], s[4];
      memcpy(c, gptr(sphere), 16);
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
    u8 verts[48];
    memcpy(verts, gptr(tri), 48);
    const u32 pat = gload<u32>(tri + 48);
    best = t;
    gstore_bytes(result + 48, point, 16);
    gstore_bytes(result + 64, normal, 16);
    gstore_bytes(result, verts, 48);
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
  memcpy(base_i, gptr(mesh + 12), 16);
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
    gstore_bytes(spad + 32 * k + 16, f, 16);
    gstore_bytes(spad + 32 * k, n, 16);
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
  memcpy(trans_add, gptr(xf + 12), 16);
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
  memcpy(m, gptr(cw + 48), 64);
  const int groups = count <= 4 ? 1 : (count + 3) / 4;
  for (int k = 0; k < groups * 4; k++) {
    float v[4], out[4];
    s32 n[4];
    memcpy(v, gptr(spad + 32 * k + 16), 16);
    transform_point(out, m, v);
    ftoi0(n, out);
    gstore_bytes(spad + 4096 + 32 * k, n, 16);
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
  printf("exceeded maximum collide cache tris (should print on screen but too lazy for that now)\n");
}

struct Vec4i {
  s32 v[4];
};

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
  const u8 zero[16] = {};

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
    Vec4i a = gload<Vec4i>(p0 + int_offset);
    Vec4i b = gload<Vec4i>(p1 + int_offset);
    Vec4i c = gload<Vec4i>(p2 + int_offset);
    const u32 pat_addr = pats + ((u32)gload<u8>(pat_index) << 2);
    if (!outside_box(a, b, c, bmin, bmax)) {
      if (num_tris == max_tris) {
        overflow();
        return;
      }
      const u32 pat = gload<u32>(pat_addr);
      if (!(pat & ignore_mask)) {
        gstore_bytes(tri_out + 48, zero, 16);
        num_tris++;
        gstore<u32>(tri_out + 48, pat);
        u8 v[48];
        memcpy(v, gptr(p0 + 16), 16);
        memcpy(v + 16, gptr(p1 + 16), 16);
        memcpy(v + 32, gptr(p2 + 16), 16);
        gstore_bytes(tri_out, v, 48);
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
      c = gload<Vec4i>(p2 + int_offset);
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
      gstore_bytes(tri_out + 48, zero, 16);
      num_tris++;
      gstore<u32>(tri_out + 48, pat);
      u8 v1[16], v0[16], v2[16];
      memcpy(v1, gptr(p1 + 16), 16);
      memcpy(v0, gptr(p0 + 16), 16);
      memcpy(v2, gptr(p2 + 16), 16);
      gstore_bytes(tri_out + 16, v1, 16);
      gstore_bytes(tri_out + 16 - flip, v0, 16);
      gstore_bytes(tri_out + 16 + flip, v2, 16);
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

}  // namespace

const NativeImpl method_9_collide_cache_prim =
    MIPS2C_NATIVE_IMPL(method_9_collide_cache_prim_impl, 0, 32);
const NativeImpl method_26_collide_cache = MIPS2C_NATIVE_IMPL(method_26_collide_cache_impl, 0, 0);
const NativeImpl method_27_collide_cache = MIPS2C_NATIVE_IMPL(method_27_collide_cache_impl, 0, 0);
const NativeImpl method_29_collide_cache = MIPS2C_NATIVE_IMPL(method_29_collide_cache_impl, 0, 0);
const NativeImpl method_32_collide_cache = MIPS2C_NATIVE_IMPL(method_32_collide_cache_impl, 0, 0);

}  // namespace Mips2C::jak1::native
