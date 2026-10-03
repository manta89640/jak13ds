#pragma once

/*!
 * @file CtrSimd.h
 * (AI-assisted)
 * Texel loops for the 3DS renderer that work on 4 bytes per 32-bit word (ARMv6 SIMD where it
 * helps). Also built on PC (the software renderer): the same results from plain C.
 */

#include <stddef.h>
#include <stdint.h>

#if defined(__ARM_FEATURE_SIMD32)
#include <arm_acle.h>
#endif

/* per byte min(255, a + b) */
static inline uint32_t ctr_add_sat_u8x4(uint32_t a, uint32_t b) {
#if defined(__ARM_FEATURE_SIMD32)
  return __uqadd8(a, b);
#else
  const uint32_t s = (a & 0x7f7f7f7fu) + (b & 0x7f7f7f7fu);  // no carries between bytes
  const uint32_t carry = ((a & b) | (s & (a | b))) & 0x80808080u;
  return (s ^ ((a ^ b) & 0x80808080u)) | ((carry >> 7) * 0xffu);
#endif
}

/* per byte (b * k2) >> 8, for k2 <= 256: two bytes per multiply (the products stay < 65536) */
static inline uint32_t ctr_scale_u8x4(uint32_t w, uint32_t k2) {
  const uint32_t lo = ((w & 0x00ff00ffu) * k2) & 0xff00ff00u;
  const uint32_t hi = (((w >> 8) & 0x00ff00ffu) * k2) & 0xff00ff00u;
  return (lo >> 8) | hi;
}

/* out[i] = min(255, out[i] + min(255, (in[i] * intensity) >> 7)) for n bytes (CtrSky's blend);
 * out and in 4 byte aligned */
static inline void ctr_add_scaled_texels(uint8_t* out, const uint8_t* in, size_t n,
                                         uint32_t intensity) {
  size_t i = 0;
  if (intensity <= 128) {
    // (in * intensity) >> 7 is at most 255: (in * 2 * intensity) >> 8, no clamp
    uint32_t* o = (uint32_t*)out;
    const uint32_t* s = (const uint32_t*)in;
    const uint32_t k2 = 2 * intensity;
    for (; i + 4 <= n; i += 4) {
      *o = ctr_add_sat_u8x4(*o, ctr_scale_u8x4(*s, k2));
      o++;
      s++;
    }
  }
  for (; i < n; i++) {
    uint32_t v = (in[i] * intensity) >> 7;
    v = out[i] + (v < 255 ? v : 255);
    out[i] = (uint8_t)(v < 255 ? v : 255);
  }
}

/* RGBA texels: rgb = (rgb * min(a, 0x80)) >> 7, a kept (CtrSky's clouds); 4 byte aligned */
static inline void ctr_premultiply_texels(uint8_t* out, const uint8_t* in, size_t texels) {
  uint32_t* o = (uint32_t*)out;
  const uint32_t* s = (const uint32_t*)in;
  for (size_t i = 0; i < texels; i++) {
    const uint32_t w = s[i];
    const uint32_t a = w >> 24;
    const uint32_t k2 = 2 * (a < 0x80 ? a : 0x80);
    const uint32_t rb = (((w & 0x00ff00ffu) * k2) & 0xff00ff00u) >> 8;
    const uint32_t g = (w & 0x0000ff00u) * k2 >> 8 & 0x0000ff00u;
    o[i] = rb | g | (w & 0xff000000u);
  }
}
