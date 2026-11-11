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
    {"call.direct", call_direct, 1000000, nullptr},
    {"call.indirect", call_indirect, 1000000, "call.direct"},
    {"mem.seq256k", mem_seq, 20, nullptr},
    {"mem.seq256k_pld", mem_seq_pld, 20, "mem.seq256k"},
    {"mem.line256k", mem_line, 20, nullptr},
};

}  // namespace

const Case* cases(int* count) {
  *count = (int)(sizeof(kCases) / sizeof(kCases[0]));
  return kCases;
}

int run(double scale, void (*line)(const char* text, void* ctx), void* ctx) {
  fill_tables();
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
