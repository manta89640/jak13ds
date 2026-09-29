#pragma once

// Decides whether SIMD code paths should use portable scalar code (OPENGOAL_SCALAR_SIMD).
// This is the case on targets with neither x86 SSE nor ARM NEON (e.g. the Nintendo 3DS ARM11,
// ARMv6K + VFPv2), or when the build defines OPENGOAL_SCALAR_SIMD explicitly (to test the scalar
// paths on a PC). Only defines a macro; include common/util/simd_util.h for the intrinsics.

#if !defined(OPENGOAL_SCALAR_SIMD) &&                                                     \
    !(defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86) || \
      defined(__aarch64__) || defined(_M_ARM64) || defined(__ARM_NEON))
#define OPENGOAL_SCALAR_SIMD 1
#endif
