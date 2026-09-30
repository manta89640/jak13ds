/*!
 * @file native_collide_mesh.cpp
 * (AI-assisted)
 * Native versions of the mips2c functions in collide_mesh.cpp. See game/mips2c/mips2c_native.h for
 * the rules (same float operations, grouped the same way, as the mips2c code).
 */

#include <cmath>

#include "game/kernel/jak1/kscheme.h"
#include "game/mips2c/jak1_functions/native_functions.h"

namespace Mips2C::jak1::native {

namespace {

u32 sym_addr(const char* name) {
  return ::jak1::intern_from_c(name).offset;
}

inline bool sign_bit(float f) {
  return (s32)f2u(f) < 0;
}

//! vmulaw acc = m3 * 1, vmaddax acc += m0 * v.x, vmadday, vmaddz out = acc + m2 * v.z (all lanes)
inline void transform4(float out[4], const float m[4][4], const float v[4]) {
  for (int i = 0; i < 4; i++) {
    float acc = m[3][i] * 1.f;
    acc += m[0][i] * v[0];
    acc += m[1][i] * v[1];
    out[i] = acc + m[2][i] * v[2];
  }
}

/*!
 * The mesh's vertices (16 bytes each, at mesh + 12, count at mesh + 8) moved by bone, to out: 32
 * bytes per vertex, the floats, then as ints (with inv, the ints are of the floats moved again by
 * inv). Whole groups of 4, at least one, like the original.
 */
void mesh_vertices(u32 mesh, u32 bone, u32 inv, u32 out) {
  u32 src = gload<u32>(mesh + 12);
  s64 n = gload<u32>(mesh + 8);
  float m[4][4], m2[4][4];
  for (int i = 0; i < 4; i++) {
    gload_q(m[i], bone + 16 * i);
  }
  if (inv) {
    for (int i = 0; i < 4; i++) {
      gload_q(m2[i], inv + 16 * i);
    }
  }
  do {
    for (int k = 0; k < 4; k++) {
      float v[4], p[4];
      gload_q(v, src + 16 * k);
      transform4(p, m, v);
      s32 q[4];
      if (inv) {
        float r[4];
        transform4(r, m2, p);
        for (int i = 0; i < 4; i++) {
          q[i] = (s32)r[i];
        }
      } else {
        for (int i = 0; i < 4; i++) {
          q[i] = (s32)p[i];
        }
      }
      gstore_q(out + 32 * k, p);
      gstore_q(out + 32 * k + 16, q);
    }
    src += 64;
    out += 128;
    n -= 4;
  } while (n > 0);
}

/*!
 * Methods 11 and 12 of collide-mesh: (this tris result sphere best) -> best
 * The sphere against the mesh's triangles in a collide-mesh-cache (96 bytes each: vertices, the
 * normal with the pat in w, the int bounding box): for each triangle whose box overlaps the
 * sphere's, the signed distance from closest-pt-in-triangle to the sphere's surface. The nearest
 * (below best, facing the sphere within 45 degrees) goes to result (a collide-tri-result).
 * Method 11 only takes spheres that penetrate a triangle. Method 12 pads the box by 122.88,
 * only takes triangles whose pat mode (bits 3-5) is 0 or 16, and distances in (-1024, 122.88).
 */
u64 mesh_sphere(const NativeArgs& args, bool method_12) {
  static const u32 closest_sym = sym_addr("closest-pt-in-triangle");
  const u32 mesh = (u32)args.a[0];
  u32 tri = (u32)args.a[1];
  const u32 result = (u32)args.a[2];
  const u32 sphere = (u32)args.a[3];
  u64 best = args.a[4];
  const u32 closest = args.stack - 16;

  float s[4];
  gload_q(s, sphere);
  if (method_12) {
    s[3] = s[3] + u2f(0x42f5c28f);  // 122.88
  }
  s32 smin[3], smax[3];
  for (int i = 0; i < 3; i++) {
    smin[i] = (s32)(s[i] - s[3]);
    smax[i] = (s32)(s[i] + s[3]);
  }

  for (u32 n = gload<u32>(mesh + 4); n != 0; n--, tri += 96) {
    if (method_12) {
      const u32 mode = gload<u32>(tri + 60) & 56;
      if (mode != 0 && mode != 16) {
        continue;
      }
    }
    s32 tmin[4], tmax[4];
    gload_q(tmin, (tri + 64) & ~15u);
    gload_q(tmax, (tri + 80) & ~15u);
    bool outside = false;
    for (int i = 0; i < 3; i++) {
      outside |= (tmin[i] > smax[i]) | (smin[i] > tmax[i]);
    }
    if (outside) {
      continue;
    }
    // the triangle as the original's 64-bit register (the argument plus 96 per triangle)
    const u64 tri64 = args.a[1] + (u64)(tri - (u32)args.a[1]);
    const u64 call_args[8] = {closest,   args.a[3], tri64,     tri64 + 48,
                              args.a[4], args.a[5], args.a[6], args.a[7]};
    native_call_goal(gload<u32>(closest_sym), call_args, args);

    float c[4], sp[4], nrm[4];
    gload_q(c, closest);
    gload_q(sp, sphere);
    gload_q(nrm, tri + 48);
    const u32 pat = gload<u32>(tri + 60);
    float d[4], dn[4], dd[4];
    for (int i = 0; i < 4; i++) {
      d[i] = sp[i] - c[i];
    }
    for (int i = 0; i < 4; i++) {
      dn[i] = d[i] * nrm[i];
      dd[i] = d[i] * d[i];
    }
    nrm[3] = 1.f;
    dn[1] = dn[1] + dn[0];
    dd[0] = dd[0] + dd[1];
    dn[1] = dn[1] + dn[2];
    dd[0] = dd[0] + dd[2];
    const float len = 0.f + std::sqrt(std::abs(dd[0]));
    const float radius = gload<float>(sphere + 12);
    float dist = sign_bit(dn[1]) ? 0.f - len : len;
    dist = dist - radius;
    const float q = 1.f / len;
    const float cur = u2f((u32)best);
    if (method_12) {
      if (cur < dist || u2f(0x42f5c28f) <= dist || dist <= -1024.f) {
        continue;
      }
    } else {
      if (cur < dist || 0.f <= dist) {
        continue;
      }
    }
    float t[4];
    for (int i = 0; i < 4; i++) {
      const float dir = d[i] * q;
      t[i] = dir * nrm[i];
    }
    t[0] = t[0] + t[1];
    t[0] = t[0] + t[2];
    if (t[0] < 0.707f) {
      continue;
    }
    best = f2gpr(dist);
    u32 verts[3][4];
    for (int k = 0; k < 3; k++) {
      gload_q(verts[k], tri + 16 * k);
    }
    for (int k = 0; k < 3; k++) {
      gstore_q(result + 16 * k, verts[k]);
    }
    gstore_q(result + 48, c);
    gstore_q(result + 64, nrm);
    gstore<u32>(result + 80, pat);
  }
  return best;
}

u64 method_11_collide_mesh_impl(const NativeArgs& args) {
  return mesh_sphere(args, false);
}

u64 method_12_collide_mesh_impl(const NativeArgs& args) {
  return mesh_sphere(args, true);
}

}  // namespace

/*!
 * (method 14 collide-mesh) (this bone out): the mesh's vertices moved by bone, as floats and ints,
 * for filling the collide cache (collide-shape-prim-mesh 12 / 14).
 */
u64 method_14_collide_mesh_impl(const NativeArgs& args) {
  mesh_vertices((u32)args.a[0], (u32)args.a[1], 0, (u32)args.a[2]);
  return 0;
}

/*!
 * (method 15 collide-mesh) (this bone inv-mat out): like method 14, but the ints are of the
 * vertices moved again by inv-mat (collide-shape-prim-mesh 13).
 */
u64 method_15_collide_mesh_impl(const NativeArgs& args) {
  mesh_vertices((u32)args.a[0], (u32)args.a[1], (u32)args.a[2], (u32)args.a[3]);
  return 0;
}

// scratch: the point closest-pt-in-triangle writes
const NativeImpl method_11_collide_mesh = MIPS2C_NATIVE_IMPL(method_11_collide_mesh_impl, 0, 16);
const NativeImpl method_12_collide_mesh = MIPS2C_NATIVE_IMPL(method_12_collide_mesh_impl, 0, 16);
const NativeImpl method_14_collide_mesh = MIPS2C_NATIVE_IMPL(method_14_collide_mesh_impl, 0, 0);
const NativeImpl method_15_collide_mesh = MIPS2C_NATIVE_IMPL(method_15_collide_mesh_impl, 0, 0);

}  // namespace Mips2C::jak1::native
