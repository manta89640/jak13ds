/*!
 * @file native2_sky_light.cpp
 * (AI-assisted)
 * Jak 2 sky (sky-tng.gc) lighting in C++: the GOAL methods call these first and run their own code
 * only when the native returns #f.
 *
 *  - pc-adjust-cloud-lighting: (method adjust-cloud-lighting sky-work): the cloud lights from the
 *    sun and moon positions, then cloud-vtx-light-update (twice) and cloud-vtx-tex-update for each
 *    of the 100 cloud vertices.
 *  - pc-adjust-haze-lighting: (method adjust-haze-lighting sky-work): the haze lights, then
 *    apply-haze-light for each of the 36 haze vertices.
 *
 * Both are goalc's C of the GOAL code (out/jak2/cmod, the sky-tng module) with vector-dot,
 * vector-normalize!, vector4-scale!, vector4-madd! and vector--float*! inline: the same float
 * operations in the same order, each on its own (no fused multiply-adds), vf0.w products kept
 * (goalc_ops::one()), goalc's fmin / fmax (`a < b ? a : b`, `a > b ? a : b`). Only the lanes that
 * reach memory are computed. The functions and methods they run inline must hold what they held at
 * the first call (symbol values, sky-work's method table), otherwise the GOAL code runs. Verify mode
 * (OPENGOAL_MIPS2C_VERIFY=1) runs the GOAL code on every call and compares (native_verify_inline:
 * the lights and the vertex arrays).
 */

#include <cstring>

#include "game/kernel/jak2/kscheme.h"
#include "game/mips2c/jak2_functions/native2.h"
#include "game/mips2c/native_goalc_ops.h"

namespace Mips2C::jak2::native2 {

namespace {

//! a symbol's value address and the function it held at the first call
struct FnSym {
  const char* name;
  u32 val = 0;
  u32 fn = 0;
  bool resolved = false;
  explicit FnSym(const char* n) : name(n) {}
  bool unchanged() {
    if (!resolved) {
      resolved = true;
      val = sym_value_addr(name);
      fn = gload<u32>(val);
    }
    return fn != 0 && gload<u32>(val) == fn;
  }
};

//! a method table entry of a type and the function it held at the first call
struct MethodSlot {
  const char* type;
  u32 idx;
  u32 slot = 0;
  u32 fn = 0;
  bool resolved = false;
  MethodSlot(const char* t, u32 i) : type(t), idx(i) {}
  bool unchanged() {
    if (!resolved) {
      resolved = true;
      const u32 ty = gload<u32>(sym_value_addr(type));
      if (ty) {
        slot = ty + 16 + 4 * idx;
        fn = gload<u32>(slot);
      }
    }
    return fn != 0 && gload<u32>(slot) == fn;
  }
};

bool native_enabled(const char* name, int& state) {
  if (state < 0) {
    state = native_turned_off(name) ? 0 : 1;
  }
  return state == 1;
}

FnSym s_normalize("vector-normalize!");
FnSym s_scale("vector4-scale!");
FnSym s_madd("vector4-madd!");
FnSym s_minus_float_mul("vector--float*!");
MethodSlot m_light_update("sky-work", 18);
MethodSlot m_tex_update("sky-work", 19);
MethodSlot m_haze_light("sky-work", 24);

u32 cloud_verts_val() {
  static const u32 v = sym_value_addr("*cloud-vert-array*");
  return v;
}
u32 haze_verts_val() {
  static const u32 v = sym_value_addr("*haze-vert-array*");
  return v;
}

inline float fmin_gc(float a, float b) {
  return a < b ? a : b;
}
inline float fmax_gc(float a, float b) {
  return a > b ? a : b;
}

// sky-work fields
constexpr u32 kSun0Pos = 1168;
constexpr u32 kSun1Pos = 1232;
constexpr u32 kMoonPos = 1296;
constexpr u32 kAmbiColor = 0x530;
constexpr u32 kAmbiColorLower = 0x540;
constexpr u32 kSun0Color = 0x550;
constexpr u32 kSun1Color = 0x560;
constexpr u32 kMoonColor = 0x570;
constexpr u32 kSun0ColorLower = 0x580;
constexpr u32 kCloudLights = 0x630;
constexpr u32 kHazeLights = 0x6d0;
constexpr u32 kCloudLightsSize = 160;
constexpr u32 kHazeLightsSize = 128;

//! vector4-scale!: dst = a * s (all lanes)
void scale4(u32 dst, u32 a, float s) {
  GC_NATIVE_FP_EXACT
  float v[4];
  gload_q(v, a);
  for (int l = 0; l < 4; l++) {
    v[l] = v[l] * s;
  }
  gstore_q(dst, v);
}

//! vector4-madd!: dst = a * vf0.w + b * s (all lanes)
void madd4(u32 dst, u32 a, u32 b, float s, float one) {
  GC_NATIVE_FP_EXACT
  float va[4], vb[4];
  gload_q(va, a);
  gload_q(vb, b);
  for (int l = 0; l < 4; l++) {
    const float t = va[l] * one;
    const float u = vb[l] * s;
    va[l] = t + u;
  }
  gstore_q(dst, va);
}

//! vector-dot (inline in the GOAL code): ((0.0 + a.x * b.x) + a.y * b.y) + a.z * b.z
inline float dot3(const float a[4], const float b[4]) {
  GC_NATIVE_FP_EXACT
  float r = 0.0f;
  float p = a[0] * b[0];
  r = r + p;
  p = a[1] * b[1];
  r = r + p;
  p = a[2] * b[2];
  r = r + p;
  return r;
}

//! the light normals (copies of the sun and moon positions, normalized) and their scales
//! fmax(0, fmin(1, 8 * (0.125 + n.y))) at scale_off
void setup_normals(u32 sky, u32 lights, u32 scale_off) {
  GC_NATIVE_FP_EXACT
  const u32 src[3] = {kSun0Pos, kSun1Pos, kMoonPos};
  for (u32 k = 0; k < 3; k++) {
    gcopy_q(lights + 16 * k, sky + src[k]);
  }
  for (u32 k = 0; k < 3; k++) {
    float v[4];
    gload_q(v, lights + 16 * k);
    goalc_ops::vector_normalize(v, 1.0f);
    gstore_q(lights + 16 * k, v);
  }
  for (u32 k = 0; k < 3; k++) {
    float t = goalc_ops::bitsf(0x3e000000u) + gload<float>(lights + 16 * k + 4);
    t = 8.0f * t;
    gstore<float>(lights + scale_off + 4 * k, fmax_gc(0.0f, fmin_gc(1.0f, t)));
  }
}

//! 1.0 - fmax(0, fmin(0.75, 4 * moon.y))
float ambient_scale(u32 lights) {
  GC_NATIVE_FP_EXACT
  const float t = 4.0f * gload<float>(lights + 36);
  const float m = fmax_gc(0.0f, fmin_gc(0.75f, t));
  return 1.0f - m;
}

//! cloud-vtx-light-update / apply-haze-light: out.xyz = clamp(128 * (ambi + sun0 * f0 + sun1 * f1 +
//! moon * f2), 0, 255), f = fmax(0, light normal . nrm)
void vtx_light(u32 out,
               u32 nrm,
               u32 lights,
               u32 ambi,
               u32 sun0c,
               u32 sun1c,
               u32 moonc,
               float one) {
  GC_NATIVE_FP_EXACT
  float n[4], l0[4], l1[4], l2[4];
  gload_q(n, nrm);
  gload_q(l0, lights);
  gload_q(l1, lights + 16);
  gload_q(l2, lights + 32);
  const float f0 = fmax_gc(0.0f, dot3(l0, n));
  const float f1 = fmax_gc(0.0f, dot3(l1, n));
  const float f2 = fmax_gc(0.0f, dot3(l2, n));
  float s[4], c0[4], c1[4], c2[4];
  gload_q(s, ambi);
  gload_q(c0, sun0c);
  gload_q(c1, sun1c);
  gload_q(c2, moonc);
  for (int l = 0; l < 3; l++) {
    float t = s[l] * one;
    float u = c0[l] * f0;
    float v = t + u;
    t = v * one;
    u = c1[l] * f1;
    v = t + u;
    t = v * one;
    u = c2[l] * f2;
    v = t + u;
    v = v * 128.0f;
    gstore<float>(out + 4 * l, fmax_gc(0.0f, fmin_gc(255.0f, v)));
  }
}

//! cloud-vtx-tex-update: out = stq, then out.x / out.y += clamp of the light offsets' x / z
void vtx_tex(u32 out, u32 stq, u32 pos, u32 lights, float one) {
  GC_NATIVE_FP_EXACT
  const float f28 = goalc_ops::bitsf(0x3b800000u);  // 0.00390625
  const float f30 = goalc_ops::bitsf(0x3c800000u);  // 0.015625
  gcopy_q(out, stq);
  float p[4], l0[4], l1[4], l2[4];
  gload_q(p, pos);
  gload_q(l0, lights);
  gload_q(l1, lights + 16);
  gload_q(l2, lights + 32);
  const float s0 = gload<float>(lights + 144) * f28;
  float s1 = 0.25f * f28;
  s1 = s1 * gload<float>(lights + 148);
  const float s2 = gload<float>(lights + 152) * f28;
  float r[2];
  const int lanes[2] = {0, 2};
  for (int k = 0; k < 2; k++) {
    const int l = lanes[k];
    // vector--float*!: p * vf0.w - n * 9
    float t = p[l] * one;
    float u = l0[l] * 9.0f;
    const float a = t - u;
    t = p[l] * one;
    u = l1[l] * 9.0f;
    const float b = t - u;
    t = p[l] * one;
    u = l2[l] * 9.0f;
    const float c = t - u;
    float v = a * s0;
    t = v * one;
    u = b * s1;
    v = t + u;
    t = v * one;
    u = c * s2;
    v = t + u;
    r[k] = v;
  }
  for (int k = 0; k < 2; k++) {
    const float m = fmax_gc(fmin_gc(r[k], f30), 0.0f - f30);
    const float o = gload<float>(out + 4 * k) + m;
    gstore<float>(out + 4 * k, o);
  }
}

void adjust_cloud_lighting(u32 sky) {
  GC_NATIVE_FP_EXACT
  const float one = goalc_ops::one();
  const u32 verts = gload<u32>(cloud_verts_val());
  const u32 L = sky + kCloudLights;
  setup_normals(sky, L, 144);
  const float f30 = ambient_scale(L);
  float t = 8.0f * gload<float>(L + 4);
  t = -1.0f + t;
  float f28 = fmax_gc(0.0f, fmin_gc(1.0f, t));
  f28 = goalc_ops::bitsf(0x3eaaa64cu) * f28;  // 0.3333
  scale4(L + 0x40, sky + kAmbiColorLower, f30);
  madd4(L + 0x40, L + 0x40, sky + kSun0ColorLower, f28, one);
  scale4(L + 0x30, sky + kAmbiColor, f30);
  madd4(L + 0x30, L + 0x30, sky + kSun0ColorLower, f28, one);
  scale4(L + 0x50, sky + kSun0Color, gload<float>(L + 144));
  scale4(L + 0x60, sky + kSun1Color, 0.5f * gload<float>(L + 148));
  scale4(L + 0x70, sky + kMoonColor, gload<float>(L + 152));
  scale4(L + 0x80, sky + kSun0ColorLower, gload<float>(L + 144));
  for (s64 i = 0; i < 100; i++) {
    const u32 v = (u32)((u64)(s64)(s32)((u32)0x70 * (u32)i) + verts);
    vtx_light(v + 0x20, v + 0x30, L, L + 0x30, L + 0x50, L + 0x60, L + 0x70, one);
    vtx_light(v + 0x50, v + 0x60, L, L + 0x40, L + 0x80, L + 0x60, L + 0x70, one);
    vtx_tex(v + 0x40, v + 0x10, v, L, one);
  }
}

void adjust_haze_lighting(u32 sky) {
  GC_NATIVE_FP_EXACT
  const float one = goalc_ops::one();
  const u32 verts = gload<u32>(haze_verts_val());
  const u32 H = sky + kHazeLights;
  setup_normals(sky, H, 112);
  const float f0 = ambient_scale(H);
  scale4(H + 48, sky + kAmbiColor, f0);
  scale4(H + 64, sky + kSun0Color, gload<float>(H + 112));
  scale4(H + 80, sky + kSun1Color, 0.5f * gload<float>(H + 116));
  scale4(H + 96, sky + kMoonColor, gload<float>(H + 120));
  for (s64 i = 0; i < 36; i++) {
    const u32 v = (u32)((u64)(s64)(s32)((u32)0x30 * (u32)i) + verts);
    vtx_light(v + 0x20, v + 0x10, H, H + 48, H + 64, H + 80, H + 96, one);
  }
}

bool common_inline_ok() {
  return s_normalize.unchanged() && s_scale.unchanged() && s_madd.unchanged();
}

}  // namespace

/*!
 * (pc-adjust-cloud-lighting this): #t after the update, #f when the GOAL code must run.
 */
u64 pc_adjust_cloud_lighting_impl(const NativeArgs& args) {
  static int on = -1;
  const u64 f = args.st;
  const u64 t = args.st + 4;
  if (!native_enabled("pc-adjust-cloud-lighting", on) || !common_inline_ok() ||
      !s_minus_float_mul.unchanged() || !m_light_update.unchanged() || !m_tex_update.unchanged()) {
    return f;
  }
  const u32 sky = (u32)args.a[0];
#if MIPS2C_WRITE_LOG
  static bool nested = false;
  if (nested) {
    return f;
  }
  if (native_verify_inline_on()) {
    static MethodSlot goal_fn("sky-work", 20);
    goal_fn.unchanged();
    native_verify_inline(
        "adjust-cloud-lighting",
        {{sky + kCloudLights, kCloudLightsSize}, {gload<u32>(cloud_verts_val()), 100 * 0x70}},
        [&]() -> u64 {
          adjust_cloud_lighting(sky);
          return 0;
        },
        [&]() -> u64 {
          nested = true;
          const u64 a[8] = {sky, 0, 0, 0, 0, 0, 0, 0};
          native_call_goal(goal_fn.fn, a, args);
          nested = false;
          return 0;
        });
    return t;
  }
#endif
  adjust_cloud_lighting(sky);
  return t;
}

/*!
 * (pc-adjust-haze-lighting this): #t after the update, #f when the GOAL code must run.
 */
u64 pc_adjust_haze_lighting_impl(const NativeArgs& args) {
  static int on = -1;
  const u64 f = args.st;
  const u64 t = args.st + 4;
  if (!native_enabled("pc-adjust-haze-lighting", on) || !common_inline_ok() ||
      !m_haze_light.unchanged()) {
    return f;
  }
  const u32 sky = (u32)args.a[0];
#if MIPS2C_WRITE_LOG
  static bool nested = false;
  if (nested) {
    return f;
  }
  if (native_verify_inline_on()) {
    static MethodSlot goal_fn("sky-work", 25);
    goal_fn.unchanged();
    native_verify_inline(
        "adjust-haze-lighting",
        {{sky + kHazeLights, kHazeLightsSize}, {gload<u32>(haze_verts_val()), 36 * 0x30}},
        [&]() -> u64 {
          adjust_haze_lighting(sky);
          return 0;
        },
        [&]() -> u64 {
          nested = true;
          const u64 a[8] = {sky, 0, 0, 0, 0, 0, 0, 0};
          native_call_goal(goal_fn.fn, a, args);
          nested = false;
          return 0;
        });
    return t;
  }
#endif
  adjust_haze_lighting(sky);
  return t;
}

const NativeImpl pc_adjust_cloud_lighting =
    MIPS2C_NATIVE_IMPL_GOAL(pc_adjust_cloud_lighting_impl, 0);
const NativeImpl pc_adjust_haze_lighting = MIPS2C_NATIVE_IMPL_GOAL(pc_adjust_haze_lighting_impl, 0);

}  // namespace Mips2C::jak2::native2

namespace Mips2C::jak2::pc_sky_light_natives {
void link() {
  gLinkedFunctionTable.reg("pc-adjust-cloud-lighting", native2::pc_adjust_cloud_lighting.as_exec,
                           64, &native2::pc_adjust_cloud_lighting);
  gLinkedFunctionTable.reg("pc-adjust-haze-lighting", native2::pc_adjust_haze_lighting.as_exec, 64,
                           &native2::pc_adjust_haze_lighting);
}
}  // namespace Mips2C::jak2::pc_sky_light_natives
