#pragma once

// SIMD backend selection:
//  - x86 / x86-64: native SSE/AVX intrinsics
//  - AArch64 (and 32-bit ARM with NEON): SSE intrinsics mapped to NEON by sse2neon
//  - OPENGOAL_SCALAR_SIMD (see simd_config.h; e.g. Nintendo 3DS, ARMv6K without NEON): portable
//    scalar code. Hot code has hand-written scalar fallbacks guarded by OPENGOAL_SCALAR_SIMD;
//    everything else uses the scalar SSE emulation in simd_scalar.h.

#include "common/util/simd_config.h"

#if defined(OPENGOAL_SCALAR_SIMD)
#include "common/util/simd_scalar.h"
#elif defined(__aarch64__) || defined(__ARM_NEON)
#include "third-party/sse2neon/sse2neon.h"
#else
#include <immintrin.h>
#endif
