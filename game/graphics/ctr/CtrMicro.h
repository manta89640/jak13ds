#pragma once

/*!
 * @file CtrMicro.h
 * (AI-assisted)
 * Small CPU shortcuts of the 3DS renderers (merc, sprites, ...), each giving bit for bit what the
 * code it replaces gave: test/ctr_micro checks them against copies of that code.
 */

#include <cstdint>
#include <cstring>

namespace ctr_micro {

/*
 * The GPU's float24 uniform words, bit for bit ctr_gpu's raw_f24 / ctr_gpu_pack_skin_rows_f24
 * (all 2^32 inputs), in fewer instructions: the rounding carry goes into the exponent by itself
 * when the sign-less bits are rounded and shifted as one number, then one range check of the
 * result's exponent.
 */

//! a float's bits -> float24 (sign, 7 bit exponent, 16 bit mantissa rounded to nearest; 0 under
//! the range or for zeros / denormals, the largest exponent with mantissa 0 over it, inf, NaN)
inline uint32_t f24_from_bits(uint32_t i) {
  const uint32_t u = ((i & 0x7fffffffu) + 0x40u) >> 7;  // 8 bit exponent, 16 bit mantissa
  const uint32_t e = (u >> 16) - 64u;                     // the float24 exponent (wraps below 0)
  uint32_t r = u - (64u << 16);
  if (__builtin_expect(e > 0x7fu, 0)) {
    r = (int32_t)e < 0 ? 0u : 0x7f0000u;
  }
  return (i >> 31 << 23) | r;
}

inline uint32_t bits_at(const float* p) {
  uint32_t i;
  memcpy(&i, p, 4);
  return i;
}

/*!
 * ctr_gpu_pack_skin_rows_f24: palette_count 3x4 row-major bone rows -> 9 words per bone, row by
 * row (row r of bone b at out[3 * (r * palette_count + b)]), each row as C3D_FVec (w z y x).
 */
inline void pack_skin_rows_f24(const float* bones, int palette_count, uint32_t* out) {
  const int stride = 3 * palette_count;
  for (int b = 0; b < palette_count; b++, bones += 12) {
    uint32_t* o = out + 3 * b;
    for (int row = 0; row < 3; row++, o += stride) {
      const float* v = bones + 4 * row;
      const uint32_t x = f24_from_bits(bits_at(v)), y = f24_from_bits(bits_at(v + 1)),
                     z = f24_from_bits(bits_at(v + 2)), w = f24_from_bits(bits_at(v + 3));
      o[0] = (w << 8) | (z >> 16);
      o[1] = (z << 16) | (y >> 8);
      o[2] = (y << 24) | x;
    }
  }
}

}  // namespace ctr_micro
