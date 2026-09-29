#pragma once

/*!
 * @file common_types.h
 * Common Integer Types.
 */

#include <cstdint>

using u8 = uint8_t;
using u16 = uint16_t;
#ifdef __3DS__
// newlib on arm-none-eabi defines int32_t as long. Use int like every PC ABI does, so s32/u32
// stay interchangeable with int/unsigned (function pointer types, pointer comparisons).
using u32 = unsigned int;
#else
using u32 = uint32_t;
#endif
using u64 = uint64_t;
using s8 = int8_t;
using s16 = int16_t;
#ifdef __3DS__
using s32 = int;
#else
using s32 = int32_t;
#endif
using s64 = int64_t;
static_assert(sizeof(u32) == 4 && sizeof(s32) == 4, "32-bit types");

struct u128 {
  union {
    u64 du64[2];
    s64 ds64[2];
    u32 du32[4];
    s32 ds32[4];
    u16 du16[8];
    s16 ds16[8];
    u8 du8[16];
    s8 ds8[16];
    float f[4];
  };
};
static_assert(sizeof(u128) == 16, "u128");

#if defined __linux || defined __linux__ || defined __APPLE__
#define OS_POSIX
#endif

// The 3DS (devkitARM newlib + libctru) provides the POSIX subset used by the runtime:
// pthreads, clock_gettime, unistd, BSD sockets (soc:U). No mmap/mprotect, no signals.
#ifdef __3DS__
#define OS_POSIX
#endif
