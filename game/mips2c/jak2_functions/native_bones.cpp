/*!
 * @file native_bones.cpp
 * (AI-assisted)
 * Jak 2: native version of new-bones-mtx-calc-asm (bones.gc), ported from Jak 1's
 * (jak1_functions/native_bones.cpp: the same GOAL code; the C++ and the 3DS's VFP assembly are
 * the same, only Jak 2's bone is 80 bytes, without Jak 1's bone-cache). See there for the details.
 *
 * (pc-new-bones-mtx-calc output joints bones cam count) -> symbol: always on (no config.ini switch
 * since Oct 18); #f only for debugging (config.ini mips2c_native_off lists pc-new-bones-mtx-calc
 * or is all; host: OPENGOAL_BONES_NATIVE=0 or OPENGOAL_MIPS2C_NATIVE=0): new-bones-mtx-calc-asm
 * then runs its own code.
 *
 * It replaces a GOAL function, so OPENGOAL_MIPS2C_VERIFY has no mips2c version to compare it with.
 * Instead, in verify mode (host) every call computes the native result into a buffer, then calls
 * new-bones-mtx-calc-asm itself (the nested call returns #f here, so the GOAL loop runs and writes
 * the output) and compares the bytes: "bones verify" lines, a summary at exit.
 */

#include <cstdlib>
#include <cstring>
#include <vector>

#include "common/log/log.h"

#include "game/kernel/jak2/kscheme.h"
#include "game/mips2c/jak2_functions/native_functions.h"
#include "game/mips2c/mips2c_native.h"
#include "game/mips2c/mips2c_private.h"

#ifdef __3DS__
extern "C" int ctr_config_get(const char* key, char* out, int size);  // platform/3ds/port
#endif

namespace Mips2C::jak2::native {

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
    const char* all = getenv("OPENGOAL_MIPS2C_NATIVE");
    if (all && all[0] == '0') {
      g_bones_native = 0;
    }
#endif
    lg::info("[bones] Jak 2 native new-bones-mtx-calc-asm {}",
             g_bones_native ? "on" : "off (mips2c_native_off / OPENGOAL_*_NATIVE=0)");
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

#if defined(__arm__) && defined(__ARM_FP) && !defined(__aarch64__)
#define BONES_VFP_ASM 1
/*!
 * The same on the 3DS's VFP (ARM11, VFPv2: 32 single registers, no FMA; VMLA / VNMLS round the
 * product, then the sum, so they give the same bits as the separate operations above). GCC spills
 * about half of the matrices to the stack in the C++ version (~360 instructions per bone, the
 * goalc C ~420); here everything stays in registers but tmat0 / tmat1 (32 bytes of scratch), and
 * independent lanes are interleaved for VFP11's 8-cycle multiply-add latency: ~240 instructions.
 * Registers: the bone in s16-s31, bind pose rows 0/1 then 2/3 in s0-s7, tmat2/3 in s8-s15, then
 * tmat0/1 back in s16-s23; the normal matrix in s24-s31, s0; the camera transposed (ct: lane l's
 * column of cam0..cam3 at ct + 16 * l).
 */
inline void bone_mtx_vfp(const float* n, const float* t, float* o, const float* ct, float* scratch) {
  const u32 one = 0x3f800000;  // 1.0f (vf0.w)
  asm volatile(
      "vldmia %[n], {s16-s31}\n"
      "vldmia %[t], {s0-s7}\n"
      "vmul.f32 s8, s16, s0\n"
      "vmul.f32 s12, s16, s4\n"
      "vmul.f32 s9, s17, s0\n"
      "vmul.f32 s13, s17, s4\n"
      "vmul.f32 s10, s18, s0\n"
      "vmul.f32 s14, s18, s4\n"
      "vmul.f32 s11, s19, s0\n"
      "vmul.f32 s15, s19, s4\n"
      "vmla.f32 s8, s20, s1\n"
      "vmla.f32 s12, s20, s5\n"
      "vmla.f32 s9, s21, s1\n"
      "vmla.f32 s13, s21, s5\n"
      "vmla.f32 s10, s22, s1\n"
      "vmla.f32 s14, s22, s5\n"
      "vmla.f32 s11, s23, s1\n"
      "vmla.f32 s15, s23, s5\n"
      "vmla.f32 s8, s24, s2\n"
      "vmla.f32 s12, s24, s6\n"
      "vmla.f32 s9, s25, s2\n"
      "vmla.f32 s13, s25, s6\n"
      "vmla.f32 s10, s26, s2\n"
      "vmla.f32 s14, s26, s6\n"
      "vmla.f32 s11, s27, s2\n"
      "vmla.f32 s15, s27, s6\n"
      "vmla.f32 s8, s28, s3\n"
      "vmla.f32 s12, s28, s7\n"
      "vmla.f32 s9, s29, s3\n"
      "vmla.f32 s13, s29, s7\n"
      "vmla.f32 s10, s30, s3\n"
      "vmla.f32 s14, s30, s7\n"
      "vmla.f32 s11, s31, s3\n"
      "vmla.f32 s15, s31, s7\n"
      "vstmia %[m], {s8-s15}\n"
      "vldmia %[t2], {s0-s7}\n"
      "vmul.f32 s8, s16, s0\n"
      "vmul.f32 s12, s16, s4\n"
      "vmul.f32 s9, s17, s0\n"
      "vmul.f32 s13, s17, s4\n"
      "vmul.f32 s10, s18, s0\n"
      "vmul.f32 s14, s18, s4\n"
      "vmul.f32 s11, s19, s0\n"
      "vmul.f32 s15, s19, s4\n"
      "vmla.f32 s8, s20, s1\n"
      "vmla.f32 s12, s20, s5\n"
      "vmla.f32 s9, s21, s1\n"
      "vmla.f32 s13, s21, s5\n"
      "vmla.f32 s10, s22, s1\n"
      "vmla.f32 s14, s22, s5\n"
      "vmla.f32 s11, s23, s1\n"
      "vmla.f32 s15, s23, s5\n"
      "vmla.f32 s8, s24, s2\n"
      "vmla.f32 s12, s24, s6\n"
      "vmla.f32 s9, s25, s2\n"
      "vmla.f32 s13, s25, s6\n"
      "vmla.f32 s10, s26, s2\n"
      "vmla.f32 s14, s26, s6\n"
      "vmla.f32 s11, s27, s2\n"
      "vmla.f32 s15, s27, s6\n"
      "vmla.f32 s8, s28, s3\n"
      "vmla.f32 s12, s28, s7\n"
      "vmla.f32 s9, s29, s3\n"
      "vmla.f32 s13, s29, s7\n"
      "vmla.f32 s10, s30, s3\n"
      "vmla.f32 s14, s30, s7\n"
      "vmla.f32 s11, s31, s3\n"
      "vmla.f32 s15, s31, s7\n"
      "vldmia %[m], {s16-s23}\n"
      "vmul.f32 s24, s9, s22\n"
      "vmul.f32 s25, s10, s20\n"
      "vmul.f32 s26, s8, s21\n"
      "vnmls.f32 s24, s21, s10\n"
      "vnmls.f32 s25, s22, s8\n"
      "vnmls.f32 s26, s20, s9\n"
      "vmul.f32 s27, s17, s10\n"
      "vmul.f32 s28, s18, s8\n"
      "vmul.f32 s29, s16, s9\n"
      "vnmls.f32 s27, s9, s18\n"
      "vnmls.f32 s28, s10, s16\n"
      "vnmls.f32 s29, s8, s17\n"
      "vmul.f32 s30, s21, s18\n"
      "vmul.f32 s31, s22, s16\n"
      "vmul.f32 s0, s20, s17\n"
      "vnmls.f32 s30, s17, s22\n"
      "vnmls.f32 s31, s18, s20\n"
      "vnmls.f32 s0, s16, s21\n"
      "vmul.f32 s1, s24, s16\n"
      "vmla.f32 s1, s25, s17\n"
      "vmla.f32 s1, s26, s18\n"
      "vmov s2, %[one]\n"
      "vdiv.f32 s1, s2, s1\n"
      "vmul.f32 s24, s24, s1\n"
      "vmul.f32 s25, s25, s1\n"
      "vmul.f32 s26, s26, s1\n"
      "vmul.f32 s27, s27, s1\n"
      "vmul.f32 s28, s28, s1\n"
      "vmul.f32 s29, s29, s1\n"
      "vmul.f32 s30, s30, s1\n"
      "vmul.f32 s31, s31, s1\n"
      "vmul.f32 s0, s0, s1\n"
      "add r12, %[ct], #0\n"
      "vldmia r12, {s4-s6}\n"
      "vmul.f32 s1, s4, s24\n"
      "vmul.f32 s2, s4, s27\n"
      "vmul.f32 s3, s4, s30\n"
      "vmla.f32 s1, s5, s25\n"
      "vmla.f32 s2, s5, s28\n"
      "vmla.f32 s3, s5, s31\n"
      "vmla.f32 s1, s6, s26\n"
      "vmla.f32 s2, s6, s29\n"
      "vmla.f32 s3, s6, s0\n"
      "vstr s1, [%[o], #64]\n"
      "vstr s2, [%[o], #80]\n"
      "vstr s3, [%[o], #96]\n"
      "add r12, %[ct], #16\n"
      "vldmia r12, {s4-s6}\n"
      "vmul.f32 s1, s4, s24\n"
      "vmul.f32 s2, s4, s27\n"
      "vmul.f32 s3, s4, s30\n"
      "vmla.f32 s1, s5, s25\n"
      "vmla.f32 s2, s5, s28\n"
      "vmla.f32 s3, s5, s31\n"
      "vmla.f32 s1, s6, s26\n"
      "vmla.f32 s2, s6, s29\n"
      "vmla.f32 s3, s6, s0\n"
      "vstr s1, [%[o], #68]\n"
      "vstr s2, [%[o], #84]\n"
      "vstr s3, [%[o], #100]\n"
      "add r12, %[ct], #32\n"
      "vldmia r12, {s4-s6}\n"
      "vmul.f32 s1, s4, s24\n"
      "vmul.f32 s2, s4, s27\n"
      "vmul.f32 s3, s4, s30\n"
      "vmla.f32 s1, s5, s25\n"
      "vmla.f32 s2, s5, s28\n"
      "vmla.f32 s3, s5, s31\n"
      "vmla.f32 s1, s6, s26\n"
      "vmla.f32 s2, s6, s29\n"
      "vmla.f32 s3, s6, s0\n"
      "vstr s1, [%[o], #72]\n"
      "vstr s2, [%[o], #88]\n"
      "vstr s3, [%[o], #104]\n"
      "add r12, %[ct], #48\n"
      "vldmia r12, {s4-s6}\n"
      "vmul.f32 s1, s4, s24\n"
      "vmul.f32 s2, s4, s27\n"
      "vmul.f32 s3, s4, s30\n"
      "vmla.f32 s1, s5, s25\n"
      "vmla.f32 s2, s5, s28\n"
      "vmla.f32 s3, s5, s31\n"
      "vmla.f32 s1, s6, s26\n"
      "vmla.f32 s2, s6, s29\n"
      "vmla.f32 s3, s6, s0\n"
      "vstr s1, [%[o], #76]\n"
      "vstr s2, [%[o], #92]\n"
      "vstr s3, [%[o], #108]\n"
      "add r12, %[ct], #0\n"
      "vldmia r12, {s0-s7}\n"
      "vmul.f32 s24, s0, s16\n"
      "vmul.f32 s28, s4, s16\n"
      "vmul.f32 s25, s0, s20\n"
      "vmul.f32 s29, s4, s20\n"
      "vmul.f32 s26, s0, s8\n"
      "vmul.f32 s30, s4, s8\n"
      "vmul.f32 s27, s0, s12\n"
      "vmul.f32 s31, s4, s12\n"
      "vmla.f32 s24, s1, s17\n"
      "vmla.f32 s28, s5, s17\n"
      "vmla.f32 s25, s1, s21\n"
      "vmla.f32 s29, s5, s21\n"
      "vmla.f32 s26, s1, s9\n"
      "vmla.f32 s30, s5, s9\n"
      "vmla.f32 s27, s1, s13\n"
      "vmla.f32 s31, s5, s13\n"
      "vmla.f32 s24, s2, s18\n"
      "vmla.f32 s28, s6, s18\n"
      "vmla.f32 s25, s2, s22\n"
      "vmla.f32 s29, s6, s22\n"
      "vmla.f32 s26, s2, s10\n"
      "vmla.f32 s30, s6, s10\n"
      "vmla.f32 s27, s2, s14\n"
      "vmla.f32 s31, s6, s14\n"
      "vmla.f32 s24, s3, s19\n"
      "vmla.f32 s28, s7, s19\n"
      "vmla.f32 s25, s3, s23\n"
      "vmla.f32 s29, s7, s23\n"
      "vmla.f32 s26, s3, s11\n"
      "vmla.f32 s30, s7, s11\n"
      "vmla.f32 s27, s3, s15\n"
      "vmla.f32 s31, s7, s15\n"
      "vstr s24, [%[o], #0]\n"
      "vstr s28, [%[o], #4]\n"
      "vstr s25, [%[o], #16]\n"
      "vstr s29, [%[o], #20]\n"
      "vstr s26, [%[o], #32]\n"
      "vstr s30, [%[o], #36]\n"
      "vstr s27, [%[o], #48]\n"
      "vstr s31, [%[o], #52]\n"
      "add r12, %[ct], #32\n"
      "vldmia r12, {s0-s7}\n"
      "vmul.f32 s24, s0, s16\n"
      "vmul.f32 s28, s4, s16\n"
      "vmul.f32 s25, s0, s20\n"
      "vmul.f32 s29, s4, s20\n"
      "vmul.f32 s26, s0, s8\n"
      "vmul.f32 s30, s4, s8\n"
      "vmul.f32 s27, s0, s12\n"
      "vmul.f32 s31, s4, s12\n"
      "vmla.f32 s24, s1, s17\n"
      "vmla.f32 s28, s5, s17\n"
      "vmla.f32 s25, s1, s21\n"
      "vmla.f32 s29, s5, s21\n"
      "vmla.f32 s26, s1, s9\n"
      "vmla.f32 s30, s5, s9\n"
      "vmla.f32 s27, s1, s13\n"
      "vmla.f32 s31, s5, s13\n"
      "vmla.f32 s24, s2, s18\n"
      "vmla.f32 s28, s6, s18\n"
      "vmla.f32 s25, s2, s22\n"
      "vmla.f32 s29, s6, s22\n"
      "vmla.f32 s26, s2, s10\n"
      "vmla.f32 s30, s6, s10\n"
      "vmla.f32 s27, s2, s14\n"
      "vmla.f32 s31, s6, s14\n"
      "vmla.f32 s24, s3, s19\n"
      "vmla.f32 s28, s7, s19\n"
      "vmla.f32 s25, s3, s23\n"
      "vmla.f32 s29, s7, s23\n"
      "vmla.f32 s26, s3, s11\n"
      "vmla.f32 s30, s7, s11\n"
      "vmla.f32 s27, s3, s15\n"
      "vmla.f32 s31, s7, s15\n"
      "vstr s24, [%[o], #8]\n"
      "vstr s28, [%[o], #12]\n"
      "vstr s25, [%[o], #24]\n"
      "vstr s29, [%[o], #28]\n"
      "vstr s26, [%[o], #40]\n"
      "vstr s30, [%[o], #44]\n"
      "vstr s27, [%[o], #56]\n"
      "vstr s31, [%[o], #60]\n"
      :
      : [n] "r"(n), [t] "r"(t), [t2] "r"(t + 8), [o] "r"(o), [ct] "r"(ct), [m] "r"(scratch),
        [one] "r"(one)
      : "r12", "memory", "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7", "d8", "d9", "d10", "d11",
        "d12", "d13", "d14", "d15");
}
#endif


constexpr u32 kBoneStride = 80;   // Jak 2's bone: transform, scale (Jak 1: 96, + bone-cache)
constexpr u32 kJointStride = 80;  // joint (basic): bind-pose at +12
constexpr u32 kPrisStride = 128;  // pris-mtx: t-mtx, then n-mtx's three rows

//! the native loop: bone i + 1 with joint i's bind pose into output i + 1, for i < n
void bones_native_loop(u32 output, u32 joints, u32 bones, u32 cam, s64 n) {
  // the camera matrix isn't in the output (math-camera or the identity matrix): loaded once
  float c[16];
  const bones_f32* cp = fptr(cam);
  for (int k = 0; k < 16; k++) {
    c[k] = cp[k];
  }
  MIPS2C_NATIVE_LOG_STORE(output + kPrisStride, n > 0 ? (u32)(kPrisStride * n) : 0);
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
  const float* b = (const float*)(mem + bones + kBoneStride);
  const float* j = (const float*)(mem + joints + 12);
  float* o = (float*)(mem + output + kPrisStride);
  for (s64 i = 0; i < n; i++) {
    bone_mtx_vfp(b, j, o, ct, scratch);
    b += kBoneStride / 4;
    j += kJointStride / 4;
    o += kPrisStride / 4;
  }
#else
  for (s64 i = 0; i < n; i++) {
    bone_mtx(output + kPrisStride * (u32)(i + 1), bones + kBoneStride * (u32)(i + 1),
             joints + kJointStride * (u32)i + 12, c);
  }
#endif
}

#if MIPS2C_WRITE_LOG
// verify mode (host): see the top
int g_verify = -1;
bool g_nested = false;
u64 g_verify_calls = 0, g_verify_bones = 0, g_verify_mismatches = 0;
std::vector<u8> g_verify_buf;

bool bones_verify_on() {
  if (g_verify < 0) {
    const char* v = getenv("OPENGOAL_MIPS2C_VERIFY");
    g_verify = (v && v[0] && v[0] != '0') ? 1 : 0;
    if (g_verify) {
      atexit([] {
        lg::info("bones verify: {} calls, {} bones compared, {} mismatches", g_verify_calls,
                 g_verify_bones, g_verify_mismatches);
      });
    }
  }
  return g_verify == 1;
}

//! the native into a buffer, the GOAL loop into the output, compared
void bones_verify(const NativeArgs& args, u32 output, u32 joints, u32 bones, u32 cam, s64 n) {
  const u32 size = n > 0 ? (u32)(kPrisStride * n) : 0;
  const u32 out = output + kPrisStride;
  std::vector<u8> before(g_ee_main_mem + out, g_ee_main_mem + out + size);
  bones_native_loop(output, joints, bones, cam, n);
  g_verify_buf.assign(g_ee_main_mem + out, g_ee_main_mem + out + size);
  memcpy(g_ee_main_mem + out, before.data(), size);
  static const u32 goal_fn_sym = sym_val_addr("new-bones-mtx-calc-asm");
  g_nested = true;
  native_call_goal(gload<u32>(goal_fn_sym), args.a, args);
  g_nested = false;
  g_verify_calls++;
  g_verify_bones += n > 0 ? n : 0;
  if (memcmp(g_ee_main_mem + out, g_verify_buf.data(), size)) {
    g_verify_mismatches++;
    if (g_verify_mismatches <= 10) {
      u32 k = 0;
      while (g_ee_main_mem[out + k] == g_verify_buf[k]) {
        k++;
      }
      u32 gw, nw;
      memcpy(&gw, g_ee_main_mem + out + (k & ~3u), 4);
      memcpy(&nw, g_verify_buf.data() + (k & ~3u), 4);
      lg::error("bones verify MISMATCH (call {}, {} bones): byte {} of the output: GOAL {:08x} "
                "native {:08x}",
                g_verify_calls, n, k, gw, nw);
    }
  }
}
#endif

}  // namespace

/*!
 * (pc-new-bones-mtx-calc output joints bones cam count): new-bones-mtx-calc-asm's loop. Bone i + 1
 * (bones stride 80, transform first) with joint i's bind pose (joints stride 80, at +12) into
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
#if MIPS2C_WRITE_LOG
  if (bones_verify_on()) {
    if (g_nested) {
      return args.st;  // #f: the GOAL loop runs (the reference)
    }
    bones_verify(args, output, joints, bones, cam, n);
    return goal_true(args.st);
  }
#endif
  bones_native_loop(output, joints, bones, cam, n);
  return goal_true(args.st);  // #t
}

const NativeImpl pc_new_bones_mtx_calc =
    MIPS2C_NATIVE_IMPL_GOAL(pc_new_bones_mtx_calc_impl, NATIVE_V0_UNDEFINED);

}  // namespace Mips2C::jak2::native

namespace Mips2C::jak2::pc_new_bones_mtx_calc {
void link() {
  gLinkedFunctionTable.reg("pc-new-bones-mtx-calc", native::pc_new_bones_mtx_calc.as_exec, 64,
                           &native::pc_new_bones_mtx_calc);
}
}  // namespace Mips2C::jak2::pc_new_bones_mtx_calc
