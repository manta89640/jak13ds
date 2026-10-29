/*!
 * @file native_misc.cpp
 * Native versions of small GOAL kernel functions (Jak 1).
 */

#include "game/mips2c/jak1_functions/native_functions.h"

namespace Mips2C::jak1::native {
/*!
 * (mem-copy! dst src size) -> dst: gcommon.gc's increasing-address byte copy, with memmove when
 * the ranges don't overlap that way (dst <= src, or no overlap). A copy forward onto an
 * overlapping higher dst repeats the source bytes, as the GOAL loop does, so that case stays a
 * byte loop.
 */
u64 mem_copy_impl(const NativeArgs& args) {
  const u32 dst = (u32)args.a[0];
  const u32 src = (u32)args.a[1];
  const s64 n = (s64)args.a[2];
  if (n > 0) {
    MIPS2C_NATIVE_LOG_STORE(dst, (u32)n);
    u8* const m = g_ee_main_mem;
    if (dst <= src || (u64)(dst - src) >= (u64)n) {
      memmove(m + dst, m + src, (size_t)n);
    } else {
      for (s64 i = 0; i < n; i++) {
        m[dst + i] = m[src + i];
      }
    }
  }
  return args.a[0];
}

const NativeImpl mem_copy = MIPS2C_NATIVE_IMPL(mem_copy_impl, 0, 0);
}  // namespace Mips2C::jak1::native

namespace Mips2C::jak1::mem_copy {
//! the reference for the native mem-copy! (verify mode compares the two): the GOAL loop
u64 execute(void* ctxt) {
  auto* c = (ExecutionContext*)ctxt;
  const u32 dst = c->gprs[a0].du32[0];
  const u32 src = c->gprs[a1].du32[0];
  const s64 n = c->gprs[a2].ds64[0];
  for (s64 i = 0; i < n; i++) {
    MIPS2C_LOG_STORE(dst + i, 1);
    g_ee_main_mem[dst + i] = g_ee_main_mem[src + i];
  }
  c->gprs[v0].du64[0] = c->gprs[a0].du64[0];
  return c->gprs[v0].du64[0];
}

void link() {
  gLinkedFunctionTable.reg("mem-copy!", execute, 0, &native::mem_copy);
}
}  // namespace Mips2C::jak1::mem_copy
