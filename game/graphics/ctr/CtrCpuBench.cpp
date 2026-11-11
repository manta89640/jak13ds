/*!
 * @file CtrCpuBench.cpp
 * (AI-assisted, cpubench) See CtrCpuBench.h. The primitive costs of the ARM11 and its VFP11, and
 * (added by the commits that change them) A/B pairs of the renderer's and the natives' kernels.
 */

#include "CtrCpuBench.h"

#include <cstdio>
#include <cstring>

#ifdef __3DS__
extern "C" unsigned long long svcGetSystemTick(void);  // libctru
#else
#include <chrono>
#endif

namespace ctr_cpu_bench {
namespace {

#define CB_NOINLINE __attribute__((noinline))

uint64_t now() {
#ifdef __3DS__
  return svcGetSystemTick();
#else
  return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
#endif
}

//! ns per tick of now()
constexpr double kTickNs =
#ifdef __3DS__
    1e9 / 268111856.0;  // SYSCLOCK_ARM11
#else
    1.0;
#endif

uint32_t fbits(float f) {
  uint32_t u;
  memcpy(&u, &f, 4);
  return u;
}

// inputs the compiler can't see through
volatile float g_fa = 0.99987f, g_fb = 0.25f, g_ft = 0.5f;
volatile int32_t g_ib = 7;

// a table of floats in [0, 1) without any equal to g_ft, from a fixed LCG (the same everywhere)
constexpr int kTab = 256;
float g_tab[kTab];
int32_t g_itab[kTab];
void fill_tables() {
  uint32_t s = 12345;
  for (int i = 0; i < kTab; i++) {
    s = s * 1664525u + 1013904223u;
    float v = (float)(s >> 8) * (1.f / 16777216.f);
    if (v == 0.5f) {
      v = 0.25f;
    }
    g_tab[i] = v;
    g_itab[i] = (int32_t)(s >> 4);
  }
}

// ---- the VFP's pipeline: a dependent chain against independent ones (same operations) ----

uint32_t vfp_chain(uint32_t n) {
  const float a = g_fa, b = g_fb;
  float x = 1.f;
  for (uint32_t i = 0; i < n; i++) {
    x = x * a + b;
    x = x * a + b;
    x = x * a + b;
    x = x * a + b;
    x = x * a + b;
    x = x * a + b;
    x = x * a + b;
    x = x * a + b;
  }
  return fbits(x);
}

uint32_t vfp_indep(uint32_t n) {
  const float a = g_fa, b = g_fb;
  float x0 = 1.f, x1 = 1.f, x2 = 1.f, x3 = 1.f, x4 = 1.f, x5 = 1.f, x6 = 1.f, x7 = 1.f;
  for (uint32_t i = 0; i < n; i++) {
    x0 = x0 * a + b;
    x1 = x1 * a + b;
    x2 = x2 * a + b;
    x3 = x3 * a + b;
    x4 = x4 * a + b;
    x5 = x5 * a + b;
    x6 = x6 * a + b;
    x7 = x7 * a + b;
  }
  // every chain is the same: a sum keeps all eight live
  return fbits(x0) + fbits(x1) + fbits(x2) + fbits(x3) + fbits(x4) + fbits(x5) + fbits(x6) +
         fbits(x7);
}

// ---- a float compare that a branch reads (vcmp + vmrs) against the sign bit in a core register ----

uint32_t vfp_cmp_branch(uint32_t n) {
  const float t = g_ft;
  uint32_t c = 0;
  for (uint32_t i = 0; i < n; i++) {
    const float v = g_tab[i & (kTab - 1)] * g_fa;
    if (v > t) {
      c += i & 3;
    }
  }
  return c;
}

uint32_t vfp_sign_bit(uint32_t n) {
  const float t = g_ft;
  uint32_t c = 0;
  for (uint32_t i = 0; i < n; i++) {
    const float v = g_tab[i & (kTab - 1)] * g_fa;
    // v > t <=> t - v < 0 (no table value equals t, so no +-0 here)
    const uint32_t m = 0u - (fbits(t - v) >> 31);
    c += (i & 3) & m;
  }
  return c;
}

// ---- divide and square root against a multiply ----

uint32_t vfp_div(uint32_t n) {
  float s = 0.f;
  const float a = g_fb;
  for (uint32_t i = 0; i < n; i++) {
    s += a / (g_tab[i & (kTab - 1)] + 1.f);
  }
  return fbits(s);
}

uint32_t vfp_mul(uint32_t n) {
  float s = 0.f;
  const float a = g_fb;
  for (uint32_t i = 0; i < n; i++) {
    s += a * (g_tab[i & (kTab - 1)] + 1.f);
  }
  return fbits(s);
}

uint32_t vfp_sqrt(uint32_t n) {
  float s = 0.f;
  for (uint32_t i = 0; i < n; i++) {
    s += __builtin_sqrtf(g_tab[i & (kTab - 1)] + 1.f);
  }
  return fbits(s);
}

// ---- integer divide: ARMv6K has none (a library call) against a constant divisor ----

uint32_t int_div_var(uint32_t n) {
  const int32_t d = g_ib;
  uint32_t s = 0;
  for (uint32_t i = 0; i < n; i++) {
    s += (uint32_t)(g_itab[i & (kTab - 1)] / d);
  }
  return s;
}

uint32_t int_div_const(uint32_t n) {
  uint32_t s = 0;
  for (uint32_t i = 0; i < n; i++) {
    s += (uint32_t)(g_itab[i & (kTab - 1)] / 7);
  }
  return s;
}

// ---- (AI-assisted, idiv) the rewritten divides: CtrVram's sampled_hash offsets ((k * 37) %
// stride per sample against a stepped remainder) and CtrTexAnimJak2's scaled texel indices (two
// divides per texel against a column table and a row per row) ----

uint32_t mod_per_item(uint32_t n) {
  const uint32_t stride = 1000u + (uint32_t)g_ib;
  uint32_t s = 0;
  for (uint32_t k = 0; k < n; k++) {
    s += (k * 37u) % stride;
  }
  return s;
}

uint32_t mod_stepped(uint32_t n) {
  const uint32_t stride = 1000u + (uint32_t)g_ib;
  const uint32_t step = 37u % stride;
  uint32_t s = 0, r = 0;
  for (uint32_t k = 0; k < n; k++) {
    s += r;
    r += step;
    r = r >= stride ? r - stride : r;
  }
  return s;
}

// a 64 x 64 output from a 48 x 40 source (n images)
uint32_t texel_div(uint32_t n) {
  const int sw = 48 + (g_ib & 0), sh = 40, w = 64, h = 64;
  uint32_t s = 0;
  for (uint32_t r = 0; r < n; r++) {
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        s += (uint32_t)((x * sw / w) + (y * sh / h) * sw) * (uint32_t)(x + 1);
      }
    }
  }
  return s;
}

uint32_t texel_table(uint32_t n) {
  const int sw = 48 + (g_ib & 0), sh = 40, w = 64, h = 64;
  uint16_t col[64];
  uint32_t s = 0;
  for (uint32_t r = 0; r < n; r++) {
    for (int x = 0; x < w; x++) {
      col[x] = (uint16_t)(x * sw / w);
    }
    for (int y = 0; y < h; y++) {
      const int row = (y * sh / h) * sw;
      for (int x = 0; x < w; x++) {
        s += (uint32_t)(row + col[x]) * (uint32_t)(x + 1);
      }
    }
  }
  return s;
}

// ---- a call through a register (GOAL C code calls every function so) against a direct call ----

CB_NOINLINE uint32_t callee(uint32_t x) {
  return x * 2654435761u + 1u;
}
uint32_t (*volatile g_callee)(uint32_t) = callee;

uint32_t call_direct(uint32_t n) {
  uint32_t x = 1;
  for (uint32_t i = 0; i < n; i++) {
    x = callee(x);
  }
  return x;
}

uint32_t call_indirect(uint32_t n) {
  uint32_t x = 1;
  uint32_t (*f)(uint32_t) = g_callee;
  for (uint32_t i = 0; i < n; i++) {
    x = f(x);
  }
  return x;
}

// ---- memory: 256 KB read in order, with a prefetch ahead, and one word per 32-byte line ----

constexpr uint32_t kMemWords = 64 * 1024;
uint32_t g_mem[kMemWords] __attribute__((aligned(32)));

uint32_t mem_seq(uint32_t n) {
  uint32_t s = 0;
  for (uint32_t r = 0; r < n; r++) {
    for (uint32_t i = 0; i < kMemWords; i += 8) {
      s += g_mem[i] + g_mem[i + 1] + g_mem[i + 2] + g_mem[i + 3] + g_mem[i + 4] + g_mem[i + 5] +
           g_mem[i + 6] + g_mem[i + 7];
    }
  }
  return s;
}

uint32_t mem_seq_pld(uint32_t n) {
  uint32_t s = 0;
  for (uint32_t r = 0; r < n; r++) {
    for (uint32_t i = 0; i < kMemWords; i += 8) {
      __builtin_prefetch(&g_mem[(i + 64) & (kMemWords - 1)]);
      s += g_mem[i] + g_mem[i + 1] + g_mem[i + 2] + g_mem[i + 3] + g_mem[i + 4] + g_mem[i + 5] +
           g_mem[i + 6] + g_mem[i + 7];
    }
  }
  return s;
}

uint32_t mem_line(uint32_t n) {
  uint32_t s = 0;
  for (uint32_t r = 0; r < n; r++) {
    for (uint32_t i = 0; i < kMemWords; i += 8) {
      s += g_mem[i];
    }
  }
  return s;
}

// ---- (AI-assisted, signbits) the run loop's sphere against 3 planes: a vcmp / vmrs and an early
// out per plane (before) against one branch on the AND of sign bits (CtrLevel.cpp run_loop) ----

constexpr int kSph = 512;
float g_sph[kSph][4];  // x, y, z, -radius
float g_pl[3][4];      // po, k0, k1, k2
void fill_spheres() {
  uint32_t s = 777;
  auto rnd = [&]() {
    s = s * 1664525u + 1013904223u;
    return (float)(s >> 8) * (1.f / 16777216.f) * 2.f - 1.f;
  };
  // plane 0: z >= -r exactly representable (po 0, k = 0, 0, 1); planes 1, 2 general
  const float pl[3][4] = {{0.f, 0.f, 0.f, 1.f}, {0.25f, 0.6f, -0.8f, 0.f}, {0.1f, -0.36f, 0.f, 0.93f}};
  memcpy(g_pl, pl, sizeof(pl));
  for (int i = 0; i < kSph; i++) {
    g_sph[i][0] = rnd();
    g_sph[i][1] = rnd();
    g_sph[i][2] = rnd();
    g_sph[i][3] = -0.25f * (rnd() + 1.f);
    if (i % 16 == 3) {
      g_sph[i][2] = g_sph[i][3];  // exactly on plane 0: neither in view by `> rad`
    }
  }
  uint32_t nan_bits = 0x7fc00000u;  // a positive quiet NaN: not in view by `> rad`
  memcpy(&g_sph[kSph - 1][0], &nan_bits, 4);
}

}  // namespace
}  // namespace ctr_cpu_bench
#include "game/graphics/ctr/CtrSimd.h"
#include "game/graphics/ctr/CtrCull.h"
namespace ctr_cpu_bench {
namespace {

// ---- (AI-assisted, armsimd) the time of day's palette blend (CtrLevel.cpp tod_refresh_colors,
// 4 palettes): three byte loads and three saturations a color (before) against ldr + uxtb16 for
// R and B and one usat16 for both ----
constexpr int kTodColors = 256, kTodPal = 4;
uint8_t g_tod[kTodColors * kTodPal * 3 + 4];
uint32_t g_todw[kTodPal] = {40, 90, 100, 26};
void fill_tod() {
  for (int i = 0; i < kTodColors * kTodPal * 3 + 4; i++) {
    g_tod[i] = (uint8_t)(g_itab[i & (kTab - 1)] >> 3);
  }
}
uint32_t tod_bytes(uint32_t n) {
  uint32_t h = 0;
  for (uint32_t r = 0; r < n; r++) {
    for (int c = 0; c < kTodColors; c++) {
      uint32_t rb = 0, g = 0;
      for (int k = 0; k < kTodPal; k++) {
        const uint8_t* x = g_tod + (c * kTodPal + k) * 3;
        rb += ((uint32_t)x[0] | (uint32_t)x[2] << 16) * g_todw[k];
        g += (uint32_t)x[1] * g_todw[k];
      }
      const uint32_t lo = (rb & 0xffff) >> 6, hi = rb >> 22, gg = g >> 6;
      h = h * 31 + ((lo < 255 ? lo : 255) | (gg < 255 ? gg : 255) << 8 | (hi < 255 ? hi : 255) << 16);
    }
  }
  return h;
}
uint32_t tod_simd(uint32_t n) {
  uint32_t h = 0;
  for (uint32_t r = 0; r < n; r++) {
    for (int c = 0; c < kTodColors; c++) {
      uint32_t rb = 0, g = 0;
      for (int k = 0; k < kTodPal; k++) {
        const uint8_t* x = g_tod + (c * kTodPal + k) * 3;
        rb += ctr_rb_of_rgb(x) * g_todw[k];
        g += (uint32_t)x[1] * g_todw[k];
      }
      const uint32_t gg = g >> 6;
      h = h * 31 + (ctr_sat_rb6(rb) | (gg < 255 ? gg : 255) << 8);
    }
  }
  return h;
}

// ---- (AI-assisted, fdiv) the normal cone test with square roots and divides (before) and with
// multiplies only (CtrCull.h cone_away), over 512 cones and views ----
constexpr int kCones = 512;
uint32_t g_cone[kCones];
float g_cview[kCones][7];  // c[3], r, eye[3]
void fill_cones() {
  uint32_t s = 99;
  auto rnd = [&]() {
    s = s * 1664525u + 1013904223u;
    return (float)(s >> 8) * (1.f / 16777216.f) * 2.f - 1.f;
  };
  for (int i = 0; i < kCones; i++) {
    float axis[3] = {rnd(), rnd(), rnd()};
    g_cone[i] = ctr_cull::cone_pack(axis, 0.2f + 0.75f * (rnd() * 0.5f + 0.5f));
    for (int k = 0; k < 7; k++) {
      g_cview[i][k] = rnd() * 50.f;
    }
    g_cview[i][3] = 2.f + 3.f * (rnd() * 0.5f + 0.5f);
  }
}
uint32_t cones_ref(uint32_t n) {
  uint32_t c = 0;
  for (uint32_t k = 0; k < n; k++) {
    const int i = (int)(k & (kCones - 1));
    c += ctr_cull::cone_away_ref(g_cone[i], g_cview[i], g_cview[i][3], g_cview[i] + 4) ? i : 0;
  }
  return c;
}
uint32_t cones_mul(uint32_t n) {
  uint32_t c = 0;
  for (uint32_t k = 0; k < n; k++) {
    const int i = (int)(k & (kCones - 1));
    c += ctr_cull::cone_away(g_cone[i], g_cview[i], g_cview[i][3], g_cview[i] + 4) ? i : 0;
  }
  return c;
}

uint32_t planes3_branch(uint32_t n) {
  uint32_t seen = 0;
  for (uint32_t k = 0; k < n; k++) {
    const float* sp = g_sph[k & (kSph - 1)];
    const float x = sp[0], y = sp[1], z = sp[2], rad = sp[3];
    bool s = true;
    for (int i = 0; i < 3; i++) {
      if (!(g_pl[i][0] + g_pl[i][1] * x + g_pl[i][2] * y + g_pl[i][3] * z > rad)) {
        s = false;
        break;
      }
    }
    seen += s ? (k & 7) + 1 : 0;
  }
  return seen;
}

uint32_t planes3_signs(uint32_t n) {
  uint32_t seen = 0;
  for (uint32_t k = 0; k < n; k++) {
    const float* sp = g_sph[k & (kSph - 1)];
    const float x = sp[0], y = sp[1], z = sp[2], rad = sp[3];
    uint32_t all = ~0u;
    for (int i = 0; i < 3; i++) {
      all &= ctr_float_bits(rad - (g_pl[i][0] + g_pl[i][1] * x + g_pl[i][2] * y + g_pl[i][3] * z));
    }
    seen += (all >> 31) ? (k & 7) + 1 : 0;
  }
  return seen;
}

// ---- (AI-assisted, sched) a mat4 x vec4 loop (sprites, shadows, particles have its shape): as
// GCC schedules it (four independent chains per point against the VFP's 8-cycle latency) and
// unrolled by two (eight chains; platform/3ds/tools/vfp11_cycles.py: ~29% fewer cycles a point) ----
constexpr int kPts = 256;
float g_pts[kPts * 4] __attribute__((aligned(16)));
float g_xo[kPts * 4] __attribute__((aligned(16)));
float g_m[16];
void fill_points() {
  for (int i = 0; i < kPts * 4; i++) {
    g_pts[i] = g_tab[i & (kTab - 1)] - 0.5f;
  }
  for (int i = 0; i < 16; i++) {
    g_m[i] = g_tab[(7 * i + 3) & (kTab - 1)];
  }
}
uint32_t xform_hash() {
  uint32_t h = 2166136261u;
  for (int i = 0; i < kPts * 4; i++) {
    h = (h ^ fbits(g_xo[i])) * 16777619u;
  }
  return h;
}
// the loop through __restrict pointers (not inlined, so the restrict holds), as GCC compiles it
// and with -funroll-loops (GCC only: the optimize attribute; -funroll-loops also renames
// registers, so the two points of a body can overlap, which `#pragma GCC unroll` alone doesn't)
#define CB_XFORM_LOOP                                                   \
  for (int i = 0; i < kPts; i++) {                                      \
    const float x = v[4 * i], y = v[4 * i + 1], z = v[4 * i + 2];       \
    o[4 * i] = m[0] * x + m[4] * y + m[8] * z + m[12];                  \
    o[4 * i + 1] = m[1] * x + m[5] * y + m[9] * z + m[13];              \
    o[4 * i + 2] = m[2] * x + m[6] * y + m[10] * z + m[14];             \
    o[4 * i + 3] = m[3] * x + m[7] * y + m[11] * z + m[15];             \
  }
CB_NOINLINE void xform_once(const float* __restrict m, const float* __restrict v,
                            float* __restrict o) {
  CB_XFORM_LOOP
}
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("unroll-loops")))
#endif
CB_NOINLINE void xform_once_unrolled(const float* __restrict m, const float* __restrict v,
                                     float* __restrict o) {
  CB_XFORM_LOOP
}
uint32_t xform_plain(uint32_t n) {
  for (uint32_t r = 0; r < n; r++) {
    xform_once(g_m, g_pts, g_xo);
  }
  return xform_hash();
}
uint32_t xform_unrolled(uint32_t n) {
  for (uint32_t r = 0; r < n; r++) {
    xform_once_unrolled(g_m, g_pts, g_xo);
  }
  return xform_hash();
}

}  // namespace
}  // namespace ctr_cpu_bench
#include "game/mips2c/native_bones_vfp.h"
namespace ctr_cpu_bench {
namespace {

#ifdef BONES_VFP_ASM
// ---- (AI-assisted, vfpvec) pc-new-bones-mtx-calc's bone matrices: the scalar VFP version against
// the short-vector one (native_bones_vfp.h), 32 Jak II bones (80 bytes apart) per iteration ----
constexpr int kBones = 32;
float g_bone[kBones * 20] __attribute__((aligned(16)));
float g_joint[kBones * 20] __attribute__((aligned(16)));
float g_ct[16] __attribute__((aligned(8)));
float g_bout[kBones * 32] __attribute__((aligned(16)));
void fill_bones() {
  uint32_t s = 4242;
  auto rnd = [&]() {
    s = s * 1664525u + 1013904223u;
    return (float)(s >> 8) * (1.f / 16777216.f) * 2.f - 1.f;
  };
  for (float& f : g_bone) f = rnd();
  for (float& f : g_joint) f = rnd();
  for (float& f : g_ct) f = rnd();
}
uint32_t bones_hash() {
  uint32_t h = 2166136261u;
  for (int i = 0; i < kBones; i++) {
    for (int k = 0; k < 28; k++) {  // t-mtx and n-mtx's three rows (the 4th words aren't written)
      h = (h ^ fbits(g_bout[32 * i + (k < 16 ? k : 16 + 4 * ((k - 16) / 3) + (k - 16) % 3)])) *
          16777619u;
    }
  }
  return h;
}
uint32_t bones_scalar(uint32_t n) {
  alignas(8) float scratch[8];
  for (uint32_t r = 0; r < n; r++) {
    for (int i = 0; i < kBones; i++) {
      native_bones_vfp::bone_mtx_vfp(g_bone + 20 * i, g_joint + 20 * i, g_bout + 32 * i, g_ct,
                                     scratch);
    }
  }
  return bones_hash();
}
uint32_t bones_vector(uint32_t n) {
  for (uint32_t r = 0; r < n; r++) {
    native_bones_vfp::bone_mtx_vfp_vec(g_bone, g_joint, g_bout, g_ct, kBones, 80);
  }
  return bones_hash();
}
#endif

// ---- (AI-assisted, pld) the run loop's walk over its arrays (8-byte runs, 8-byte spheres, 4-byte
// LOD entries; 64K runs: 1.25 MB, more than the New 3DS's L2 share a frame keeps) without and
// with a PLD 16 runs ahead (CtrLevel.cpp run_loop) ----
struct BRun {
  uint16_t draw, vis;
  uint32_t count;
};
struct BSph {
  int16_t c[3];
  uint16_t r;
};
constexpr uint32_t kRuns = 64 * 1024;
BRun* g_runs = nullptr;
BSph* g_rsph = nullptr;
uint32_t* g_rlod = nullptr;
void fill_runs() {
  if (!g_runs) {
    g_runs = new BRun[kRuns];
    g_rsph = new BSph[kRuns];
    g_rlod = new uint32_t[kRuns];
  }
  for (uint32_t i = 0; i < kRuns; i++) {
    g_runs[i] = {(uint16_t)(i & 7), (uint16_t)(i * 7), (uint32_t)(i % 97) * 3};
    g_rsph[i] = {{(int16_t)i, (int16_t)(i * 3), (int16_t)(i * 5)}, (uint16_t)(i & 255)};
    g_rlod[i] = i * 2654435761u;
  }
}
template <bool PLD>
uint32_t runs_scan(uint32_t n) {
  uint32_t s = 0;
  for (uint32_t k = 0; k < n; k++) {
    for (uint32_t r = 0; r < kRuns; r++) {
      if (PLD && (r & 3) == 0) {
        __builtin_prefetch(g_runs + r + 16);
        __builtin_prefetch(g_rsph + r + 16);
        __builtin_prefetch(g_rlod + r + 16);
      }
      const int v = g_rsph[r].c[0] + g_rsph[r].c[1] - g_rsph[r].c[2] > (int)g_rsph[r].r;
      s += v ? g_runs[r].count : (g_rlod[r] & 1);
    }
  }
  return s;
}
uint32_t runs_plain(uint32_t n) {
  return runs_scan<false>(n);
}
uint32_t runs_pld(uint32_t n) {
  return runs_scan<true>(n);
}

const Case kCases[] = {
    // name, kernel, iterations on the console, pair
    {"vfp.chain8", vfp_chain, 200000, nullptr},
    {"vfp.indep8", vfp_indep, 200000, nullptr},
    {"vfp.cmp_branch", vfp_cmp_branch, 1000000, nullptr},
    {"vfp.sign_bit", vfp_sign_bit, 1000000, "vfp.cmp_branch"},
    {"vfp.div", vfp_div, 500000, nullptr},
    {"vfp.mul", vfp_mul, 500000, nullptr},
    {"vfp.sqrt", vfp_sqrt, 500000, nullptr},
    {"int.div_var", int_div_var, 500000, nullptr},
    {"int.div_const", int_div_const, 500000, "int.div_var"},
    {"int.mod_per_item", mod_per_item, 500000, nullptr},
    {"int.mod_stepped", mod_stepped, 500000, "int.mod_per_item"},
    {"int.texel_div64", texel_div, 200, nullptr},
    {"int.texel_table64", texel_table, 200, "int.texel_div64"},
    {"call.direct", call_direct, 1000000, nullptr},
    {"call.indirect", call_indirect, 1000000, "call.direct"},
    {"mem.seq256k", mem_seq, 20, nullptr},
    {"mem.seq256k_pld", mem_seq_pld, 20, "mem.seq256k"},
    {"mem.line256k", mem_line, 20, nullptr},
    {"mem.runs64k", runs_plain, 20, nullptr},
    {"mem.runs64k_pld", runs_pld, 20, "mem.runs64k"},
    {"cull.planes3_branch", planes3_branch, 1000000, nullptr},
    {"cull.planes3_signs", planes3_signs, 1000000, "cull.planes3_branch"},
    {"tod.blend4_bytes", tod_bytes, 500, nullptr},
    {"tod.blend4_simd", tod_simd, 500, "tod.blend4_bytes"},
    {"cull.cone_sqrt_div", cones_ref, 300000, nullptr},
    {"cull.cone_mul", cones_mul, 300000, "cull.cone_sqrt_div"},
    {"vfp.xform256", xform_plain, 2000, nullptr},
    {"vfp.xform256_unrolled", xform_unrolled, 2000, "vfp.xform256"},
#ifdef BONES_VFP_ASM
    {"bones.mtx_scalar32", bones_scalar, 5000, nullptr},
    {"bones.mtx_vector32", bones_vector, 5000, "bones.mtx_scalar32"},
#endif
};

}  // namespace

const Case* cases(int* count) {
  *count = (int)(sizeof(kCases) / sizeof(kCases[0]));
  return kCases;
}

int run(double scale, void (*line)(const char* text, void* ctx), void* ctx) {
  fill_tables();
  fill_spheres();
  fill_points();
  fill_cones();
  fill_tod();
  fill_runs();
#ifdef BONES_VFP_ASM
  fill_bones();
#endif
  for (uint32_t i = 0; i < kMemWords; i++) {
    g_mem[i] = i * 2654435761u;
  }
  int count = 0;
  const Case* cs = cases(&count);
  uint32_t checks[256];
  int mismatches = 0;
  char buf[256];
  for (int k = 0; k < count && k < 256; k++) {
    const Case& c = cs[k];
    uint32_t n = (uint32_t)(c.iters * scale);
    n = n < 1 ? 1 : n;
    // the best of three runs (the first warms the caches and the branch predictor)
    double best = 1e300;
    uint32_t check = 0;
    for (int rep = 0; rep < 3; rep++) {
      const uint64_t t0 = now();
      check = c.fn(n);
      const uint64_t t1 = now();
      const double ns = (double)(t1 - t0) * kTickNs / n;
      best = ns < best ? ns : best;
    }
    checks[k] = check;
    const char* verdict = "";
    if (c.pair) {
      verdict = " (no pair)";
      for (int j = 0; j < k; j++) {
        if (strcmp(cs[j].name, c.pair) == 0) {
          const bool same = checks[j] == check;
          verdict = same ? " same as pair" : " MISMATCH with pair";
          mismatches += same ? 0 : 1;
          break;
        }
      }
    }
    // cycles at the New 3DS's 804 MHz
    snprintf(buf, sizeof(buf), "cpu.%-24s %10.2f ns/iter %9.1f cycles  check %08lx%s", c.name,
             best, best * 0.804, (unsigned long)check, verdict);
    line(buf, ctx);
  }
  return mismatches;
}

}  // namespace ctr_cpu_bench
