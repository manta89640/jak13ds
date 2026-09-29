#pragma once

#include <cstdlib>

#include "common/common_types.h"
#include "common/util/simd_config.h"

u32 crc32(const u8* data, size_t size);

#include <cstring>

#if defined(__aarch64__) && !defined(OPENGOAL_SCALAR_SIMD)
#include <arm_acle.h>
// Computes CRC32C
inline u32 crc32(const u8* data, size_t size) {
  u32 result = 0xffffffff;
  while (size >= 4) {
    result = __crc32cw(result, *reinterpret_cast<const u32*>(data));
    data += 4;
    size -= 4;
  }
  while (size) {
    result = __crc32cb(result, *data);
    data++;
    size--;
  }
  return ~result;
}
#elif !defined(OPENGOAL_SCALAR_SIMD) && !defined(__arm__)
#include <immintrin.h>
// Computes CRC32C
inline u32 crc32(const u8* data, size_t size) {
  u32 result = 0xffffffff;
  while (size >= 4) {
    u32 x;
    memcpy(&x, data, 4);
    data += 4;
    size -= 4;
    result = _mm_crc32_u32(result, x);
  }
  while (size) {
    result = _mm_crc32_u8(result, *data);
    data++;
    size--;
  }
  return ~result;
}
#else
// Portable CRC32C (Castagnoli, reflected polynomial 0x82F63B78), same results as the hardware
// instructions used above. Slicing-by-1 table, built at compile time.
namespace crc32_detail {
struct Crc32cTable {
  u32 t[256];
  constexpr Crc32cTable() : t() {
    for (u32 i = 0; i < 256; i++) {
      u32 c = i;
      for (int k = 0; k < 8; k++) {
        c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : (c >> 1);
      }
      t[i] = c;
    }
  }
};
inline constexpr Crc32cTable kCrc32cTable{};
}  // namespace crc32_detail

// Computes CRC32C
inline u32 crc32(const u8* data, size_t size) {
  u32 result = 0xffffffff;
  while (size) {
    result = crc32_detail::kCrc32cTable.t[(result ^ *data) & 0xff] ^ (result >> 8);
    data++;
    size--;
  }
  return ~result;
}
#endif
