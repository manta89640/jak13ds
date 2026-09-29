#pragma once

// (AI-assisted)
// Portable scalar emulation of the small subset of SSE intrinsics used by the runtime.
// Only included by common/util/simd_util.h when OPENGOAL_SCALAR_SIMD is set, which is the case on
// targets with neither x86 SSE nor ARM NEON (e.g. the Nintendo 3DS ARM11, ARMv6K + VFPv2).
//
// Semantics follow the Intel definitions (NaN handling of min/max, saturation of packus/adds),
// so results match the x86 path. Exceptions:
//  - _mm_rsqrt_ss is an approximation on x86 (and NEON); here it is computed as 1/sqrtf, which is
//    more precise.
//  - no FMA contraction is done by these helpers (each op is its own function), but the compiler
//    may still contract across inlined calls with -ffp-contract=fast on targets with FMA.
//    ARMv6K/VFPv2 has no fused multiply-add, so the 3DS is not affected.
//
// Do not include this together with <immintrin.h> / sse2neon in the same translation unit.
// Code that is hot on the 3DS should use explicit scalar loops instead (see game/common/vu.h).

#include <cmath>
#include <cstdint>
#include <cstring>

#define OPENGOAL_SIMD_INLINE static inline __attribute__((always_inline))

struct alignas(16) __m128 {
  float f[4];
};

struct alignas(16) __m128i {
  uint8_t b[16];
};

namespace og_simd_scalar {
OPENGOAL_SIMD_INLINE void get16(const __m128i& v, int16_t* out) {
  memcpy(out, v.b, 16);
}
OPENGOAL_SIMD_INLINE __m128i put16(const int16_t* in) {
  __m128i r;
  memcpy(r.b, in, 16);
  return r;
}
OPENGOAL_SIMD_INLINE uint8_t sat_u8(int v) {
  return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v);
}
OPENGOAL_SIMD_INLINE int16_t sat_s16(int v) {
  return v < -32768 ? -32768 : (v > 32767 ? 32767 : (int16_t)v);
}
}  // namespace og_simd_scalar

// ---------------- float ----------------

OPENGOAL_SIMD_INLINE __m128 _mm_load_ps(const float* p) {
  __m128 r;
  memcpy(r.f, p, 16);
  return r;
}

OPENGOAL_SIMD_INLINE __m128 _mm_loadu_ps(const float* p) {
  __m128 r;
  memcpy(r.f, p, 16);
  return r;
}

OPENGOAL_SIMD_INLINE void _mm_store_ps(float* p, __m128 a) {
  memcpy(p, a.f, 16);
}

OPENGOAL_SIMD_INLINE void _mm_storeu_ps(float* p, __m128 a) {
  memcpy(p, a.f, 16);
}

OPENGOAL_SIMD_INLINE __m128 _mm_set1_ps(float a) {
  return __m128{{a, a, a, a}};
}

OPENGOAL_SIMD_INLINE __m128 _mm_setr_ps(float a, float b, float c, float d) {
  return __m128{{a, b, c, d}};
}

OPENGOAL_SIMD_INLINE __m128 _mm_set_ss(float a) {
  return __m128{{a, 0.f, 0.f, 0.f}};
}

OPENGOAL_SIMD_INLINE float _mm_cvtss_f32(__m128 a) {
  return a.f[0];
}

OPENGOAL_SIMD_INLINE __m128 _mm_add_ps(__m128 a, __m128 b) {
  __m128 r;
  for (int i = 0; i < 4; i++) {
    r.f[i] = a.f[i] + b.f[i];
  }
  return r;
}

OPENGOAL_SIMD_INLINE __m128 _mm_mul_ps(__m128 a, __m128 b) {
  __m128 r;
  for (int i = 0; i < 4; i++) {
    r.f[i] = a.f[i] * b.f[i];
  }
  return r;
}

// Intel: dst = (a > b) ? a : b  (returns b if either is NaN)
OPENGOAL_SIMD_INLINE __m128 _mm_max_ps(__m128 a, __m128 b) {
  __m128 r;
  for (int i = 0; i < 4; i++) {
    r.f[i] = a.f[i] > b.f[i] ? a.f[i] : b.f[i];
  }
  return r;
}

// Intel: dst = (a < b) ? a : b  (returns b if either is NaN)
OPENGOAL_SIMD_INLINE __m128 _mm_min_ps(__m128 a, __m128 b) {
  __m128 r;
  for (int i = 0; i < 4; i++) {
    r.f[i] = a.f[i] < b.f[i] ? a.f[i] : b.f[i];
  }
  return r;
}

// SSE4.1 blend: lane i comes from b if bit i of imm is set.
OPENGOAL_SIMD_INLINE __m128 _mm_blend_ps(__m128 a, __m128 b, const int imm) {
  __m128 r;
  for (int i = 0; i < 4; i++) {
    r.f[i] = (imm & (1 << i)) ? b.f[i] : a.f[i];
  }
  return r;
}

OPENGOAL_SIMD_INLINE __m128 _mm_rsqrt_ss(__m128 a) {
  __m128 r = a;
  r.f[0] = 1.f / std::sqrt(a.f[0]);
  return r;
}

// ---------------- integer ----------------

OPENGOAL_SIMD_INLINE __m128i _mm_loadu_si128(const __m128i* p) {
  __m128i r;
  memcpy(r.b, p, 16);
  return r;
}

OPENGOAL_SIMD_INLINE void _mm_storeu_si128(__m128i* p, __m128i a) {
  memcpy(p, a.b, 16);
}

// load 64 bits into the low half, zero the upper half
OPENGOAL_SIMD_INLINE __m128i _mm_loadu_si64(const void* p) {
  __m128i r{};
  memcpy(r.b, p, 8);
  return r;
}

// store the low 64 bits
OPENGOAL_SIMD_INLINE void _mm_storel_epi64(__m128i* p, __m128i a) {
  memcpy(p, a.b, 8);
}

OPENGOAL_SIMD_INLINE __m128i _mm_setr_epi16(int16_t e0,
                                             int16_t e1,
                                             int16_t e2,
                                             int16_t e3,
                                             int16_t e4,
                                             int16_t e5,
                                             int16_t e6,
                                             int16_t e7) {
  const int16_t v[8] = {e0, e1, e2, e3, e4, e5, e6, e7};
  return og_simd_scalar::put16(v);
}

OPENGOAL_SIMD_INLINE __m128i _mm_set_epi16(int16_t e7,
                                            int16_t e6,
                                            int16_t e5,
                                            int16_t e4,
                                            int16_t e3,
                                            int16_t e2,
                                            int16_t e1,
                                            int16_t e0) {
  const int16_t v[8] = {e0, e1, e2, e3, e4, e5, e6, e7};
  return og_simd_scalar::put16(v);
}

OPENGOAL_SIMD_INLINE __m128i _mm_set1_epi16(int16_t a) {
  const int16_t v[8] = {a, a, a, a, a, a, a, a};
  return og_simd_scalar::put16(v);
}

// zero extend the low 8 u8s to 8 u16s
OPENGOAL_SIMD_INLINE __m128i _mm_cvtepu8_epi16(__m128i a) {
  int16_t v[8];
  for (int i = 0; i < 8; i++) {
    v[i] = (int16_t)a.b[i];
  }
  return og_simd_scalar::put16(v);
}

// low 16 bits of the product
OPENGOAL_SIMD_INLINE __m128i _mm_mullo_epi16(__m128i a, __m128i b) {
  int16_t x[8], y[8], v[8];
  og_simd_scalar::get16(a, x);
  og_simd_scalar::get16(b, y);
  for (int i = 0; i < 8; i++) {
    v[i] = (int16_t)(uint16_t)((uint32_t)(uint16_t)x[i] * (uint32_t)(uint16_t)y[i]);
  }
  return og_simd_scalar::put16(v);
}

OPENGOAL_SIMD_INLINE __m128i _mm_adds_epi16(__m128i a, __m128i b) {
  int16_t x[8], y[8], v[8];
  og_simd_scalar::get16(a, x);
  og_simd_scalar::get16(b, y);
  for (int i = 0; i < 8; i++) {
    v[i] = og_simd_scalar::sat_s16((int)x[i] + (int)y[i]);
  }
  return og_simd_scalar::put16(v);
}

OPENGOAL_SIMD_INLINE __m128i _mm_adds_epu8(__m128i a, __m128i b) {
  __m128i r;
  for (int i = 0; i < 16; i++) {
    int s = (int)a.b[i] + (int)b.b[i];
    r.b[i] = s > 255 ? 255 : (uint8_t)s;
  }
  return r;
}

// logical shift right of each u16
OPENGOAL_SIMD_INLINE __m128i _mm_srli_epi16(__m128i a, int imm) {
  int16_t x[8], v[8];
  og_simd_scalar::get16(a, x);
  for (int i = 0; i < 8; i++) {
    v[i] = imm > 15 ? 0 : (int16_t)(uint16_t)((uint16_t)x[i] >> imm);
  }
  return og_simd_scalar::put16(v);
}

OPENGOAL_SIMD_INLINE __m128i _mm_min_epu16(__m128i a, __m128i b) {
  int16_t x[8], y[8], v[8];
  og_simd_scalar::get16(a, x);
  og_simd_scalar::get16(b, y);
  for (int i = 0; i < 8; i++) {
    v[i] = (uint16_t)x[i] < (uint16_t)y[i] ? x[i] : y[i];
  }
  return og_simd_scalar::put16(v);
}

OPENGOAL_SIMD_INLINE __m128i _mm_min_epi16(__m128i a, __m128i b) {
  int16_t x[8], y[8], v[8];
  og_simd_scalar::get16(a, x);
  og_simd_scalar::get16(b, y);
  for (int i = 0; i < 8; i++) {
    v[i] = x[i] < y[i] ? x[i] : y[i];
  }
  return og_simd_scalar::put16(v);
}

// signed s16 -> u8 with saturation. low 8 bytes from a, high 8 bytes from b.
OPENGOAL_SIMD_INLINE __m128i _mm_packus_epi16(__m128i a, __m128i b) {
  int16_t x[8], y[8];
  og_simd_scalar::get16(a, x);
  og_simd_scalar::get16(b, y);
  __m128i r;
  for (int i = 0; i < 8; i++) {
    r.b[i] = og_simd_scalar::sat_u8(x[i]);
    r.b[i + 8] = og_simd_scalar::sat_u8(y[i]);
  }
  return r;
}

#undef OPENGOAL_SIMD_INLINE
