/*!
 * @file native_bones.cpp
 * (AI-assisted)
 * Native version of new-bones-mtx-calc-asm (bones.gc): the skinning matrices of one bone
 * calculation (every merc model drawn, every frame). See game/mips2c/mips2c_native.h.
 *
 * In C mode the GOAL function is a VU0 macro-mode routine compiled lane by lane: per bone a 4x4
 * product (bone x bind pose), three cross products, a dot product and a divide (the inverse
 * transpose for the normals), then both matrices times the camera; ~400 ARM instructions per bone,
 * 0.27 ms per frame in Snowy Mountain on New 3DS hardware. The native does the same float
 * operations in the same order, one product or sum per statement (so no compiler fuses them on
 * hosts with FMA; the 3DS's VFP has none), and leaves out what no output depends on: the w lanes
 * of the normal matrix before the camera product, the reloads of the camera per bone. The results
 * are bit-identical to goalc's C code (test/mips2c_native: tests_bones.cpp against the goalc
 * reference, and unicorn/run.sh in the 3DS's VFP mode).
 *
 * (pc-new-bones-mtx-calc output joints bones cam count) -> symbol: always on (no config.ini switch
 * since Oct 18); #f only for debugging (config.ini mips2c_native_off lists pc-new-bones-mtx-calc
 * or is all; host: OPENGOAL_BONES_NATIVE=0): new-bones-mtx-calc-asm then runs its own code.
 */

#include <cstdlib>
#include <cstring>

#include "game/kernel/jak1/kscheme.h"
#include "game/mips2c/jak1_functions/native_functions.h"
#include "game/mips2c/mips2c_native.h"
#include "game/mips2c/mips2c_private.h"
#include "game/mips2c/native_bones_vfp.h"

#ifdef __3DS__
extern "C" int ctr_config_get(const char* key, char* out, int size);  // platform/3ds/port
#endif

namespace Mips2C::jak1::native {

namespace {

int g_bones_native = -1;  // -1: config not read yet

bool bones_native_on() {
  if (g_bones_native < 0) {
    g_bones_native = 1;
#ifdef __3DS__
    // mips2c_native_off = all (or on, 1...) or a list with this name: off too
    char list[256];
    if (ctr_config_get("mips2c_native_off", list, sizeof(list)) &&
        (strstr(list, "pc-new-bones-mtx-calc") || !strcmp(list, "all") || !strcmp(list, "on") ||
         !strcmp(list, "1") || !strcmp(list, "yes") || !strcmp(list, "true"))) {
      g_bones_native = 0;
    }
#else
    const char* env = getenv("OPENGOAL_BONES_NATIVE");
    if (env && env[0] == '0') {
      g_bones_native = 0;
    }
#endif
    printf("[bones] native new-bones-mtx-calc-asm %s\n",
           g_bones_native ? "on" : "off (mips2c_native_off / OPENGOAL_BONES_NATIVE)");
  }
  return g_bones_native == 1;
}

#if defined(__GNUC__) || defined(__clang__)
typedef float __attribute__((may_alias, aligned(4))) bones_f32;
#else
typedef float bones_f32;
#endif

inline const bones_f32* fptr(u32 addr) {
  return (const bones_f32*)(g_ee_main_mem + addr);
}
inline bones_f32* fptr_w(u32 addr) {
  return (bones_f32*)(g_ee_main_mem + addr);
}

/*!
 * One bone: out (pris-mtx) from the bone's transform b and the joint's bind pose j, with the
 * camera c (row-major, c[4 * row + lane]).
 */
inline void bone_mtx(u32 out_addr, u32 b_addr, u32 j_addr, const float* __restrict c) {
  const bones_f32* __restrict nb = fptr(b_addr);  // nmat0..3 (the bone)
  const bones_f32* __restrict tb = fptr(j_addr);  // tmat0..3 (the bind pose)
  // tmat_r = bone x bind pose: acc = n0 * t_r.x, acc += n1 * t_r.y, n2 * t_r.z, n3 * t_r.w
  float m[16];
  {
    float n[16];
    for (int k = 0; k < 16; k++) {
      n[k] = nb[k];
    }
    for (int r = 0; r < 4; r++) {
      const float tx = tb[4 * r + 0], ty = tb[4 * r + 1], tz = tb[4 * r + 2], tw = tb[4 * r + 3];
      for (int l = 0; l < 4; l++) {
        float x = n[l] * tx;
        const float p1 = n[4 + l] * ty;
        x = x + p1;
        const float p2 = n[8 + l] * tz;
        x = x + p2;
        const float p3 = n[12 + l] * tw;
        m[4 * r + l] = x + p3;
      }
    }
  }
  // nmat0 = tmat1 x tmat2, nmat1 = tmat2 x tmat0, nmat2 = tmat0 x tmat1 (outer products: lane x
  // a.y * b.z - b.y * a.z, ...; lane w isn't used)
  float q[9];
  {
    const int pairs[3][2] = {{1, 2}, {2, 0}, {0, 1}};
    for (int k = 0; k < 3; k++) {
      const float* a = m + 4 * pairs[k][0];
      const float* b = m + 4 * pairs[k][1];
      const float ax = a[1] * b[2];
      const float bx = b[1] * a[2];
      q[3 * k + 0] = ax - bx;
      const float ay = a[2] * b[0];
      const float by = b[2] * a[0];
      q[3 * k + 1] = ay - by;
      const float az = a[0] * b[1];
      const float bz = b[0] * a[1];
      q[3 * k + 2] = az - bz;
    }
    // dot nmat0 . tmat0 = (x + y) + z, then 1 / dot (vf0.w / acc.x)
    const float dx = q[0] * m[0];
    const float dy = q[1] * m[1];
    const float dz = q[2] * m[2];
    float d = dx + dy;
    d = d + dz;
    const float s = 1.f / d;
    for (int k = 0; k < 9; k++) {
      q[k] = q[k] * s;
    }
  }
  bones_f32* __restrict o = fptr_w(out_addr);
  // t-mtx row r = camera x tmat_r: acc = cam0 * m_r.x, += cam1 * m_r.y, cam2 * m_r.z, cam3 * m_r.w
  // (lane by lane: the camera's column and tmat in registers)
  for (int l = 0; l < 4; l++) {
    const float c0 = c[l], c1 = c[4 + l], c2 = c[8 + l], c3 = c[12 + l];
    for (int r = 0; r < 4; r++) {
      float x = c0 * m[4 * r + 0];
      const float p1 = c1 * m[4 * r + 1];
      x = x + p1;
      const float p2 = c2 * m[4 * r + 2];
      x = x + p2;
      const float p3 = c3 * m[4 * r + 3];
      o[4 * r + l] = x + p3;
    }
  }
  // n-mtx row k = camera x nmat_k (x, y, z)
  for (int l = 0; l < 4; l++) {
    const float c0 = c[l], c1 = c[4 + l], c2 = c[8 + l];
    for (int k = 0; k < 3; k++) {
      float x = c0 * q[3 * k + 0];
      const float p1 = c1 * q[3 * k + 1];
      x = x + p1;
      const float p2 = c2 * q[3 * k + 2];
      o[16 + 4 * k + l] = x + p2;
    }
  }
}

// (AI-assisted) bone_mtx_vfp (and, vfpvec, its short-vector version): native_bones_vfp.h, shared
// by both games' natives
#ifdef BONES_VFP_ASM
using native_bones_vfp::bone_mtx_vfp;
#endif

}  // namespace

/*!
 * (pc-new-bones-mtx-calc output joints bones cam count): new-bones-mtx-calc-asm's loop. Bone i + 1
 * (bones stride 96, transform first) with joint i's bind pose (joints stride 80, at +12) into
 * output i + 1 (pris-mtx, 128 bytes: t-mtx then n-mtx's three rows), for i < count - 1.
 */
u64 pc_new_bones_mtx_calc_impl(const NativeArgs& args) {
  if (!bones_native_on()) {
    return args.st;  // #f: the GOAL code runs
  }
  const u32 output = (u32)args.a[0];
  const u32 joints = (u32)args.a[1];
  const u32 bones = (u32)args.a[2];
  const u32 cam = (u32)args.a[3];
  const s64 n = (s64)(s32)(u32)args.a[4] - 1;
  // the camera matrix isn't in the output (math-camera or the identity matrix): loaded once
  float c[16];
  const bones_f32* cp = fptr(cam);
  for (int k = 0; k < 16; k++) {
    c[k] = cp[k];
  }
  MIPS2C_NATIVE_LOG_STORE(output + 128, n > 0 ? (u32)(128 * n) : 0);
#ifdef BONES_VFP_ASM
  // the camera's columns (lane l of cam0..cam3) together, for the lane by lane products
  alignas(8) float ct[16];
  for (int l = 0; l < 4; l++) {
    for (int k = 0; k < 4; k++) {
      ct[4 * l + k] = c[4 * k + l];
    }
  }
  alignas(8) float scratch[8];
  u8* const mem = g_ee_main_mem;
  const float* b = (const float*)(mem + bones + 96);
  const float* j = (const float*)(mem + joints + 12);
  float* o = (float*)(mem + output + 128);
  if (native_bones_vfp::vector_on()) {
    // (AI-assisted, vfpvec) config.ini vfp_vector: the first part as VFP short vectors (the same
    // bits; native_bones_vfp.h)
    if (n > 0) {
      native_bones_vfp::bone_mtx_vfp_vec(b, j, o, ct, n, 96);
    }
  } else {
    for (s64 i = 0; i < n; i++) {
      bone_mtx_vfp(b, j, o, ct, scratch);
      b += 24;
      j += 20;
      o += 32;
    }
  }
#else
  for (s64 i = 0; i < n; i++) {
    bone_mtx(output + 128 * (u32)(i + 1), bones + 96 * (u32)(i + 1), joints + 80 * (u32)i + 12, c);
  }
#endif
  return args.st + 8;  // #t
}

const NativeImpl pc_new_bones_mtx_calc =
    MIPS2C_NATIVE_IMPL_GOAL(pc_new_bones_mtx_calc_impl, NATIVE_V0_UNDEFINED);

}  // namespace Mips2C::jak1::native

namespace Mips2C::jak1::pc_new_bones_mtx_calc {
void link() {
  gLinkedFunctionTable.reg("pc-new-bones-mtx-calc", native::pc_new_bones_mtx_calc.as_exec, 64,
                           &native::pc_new_bones_mtx_calc);
}
}  // namespace Mips2C::jak1::pc_new_bones_mtx_calc
