/*!
 * @file mips2c_goalc.cpp
 * Calling mips2c functions from GOAL code compiled to C (see docs/3ds-port/c_backend.md).
 */

#include "game/kernel/common/goalc_runtime.h"
#include "game/mips2c/mips2c_private.h"

namespace Mips2C {

/*!
 * Does what the native _mips2c_call trampolines do: build the MIPS register context on the stack
 * (which is in GOAL memory in C mode), with a fake GOAL stack of stack_size bytes below it, and
 * return v0. fn is the mips2c execute function.
 */
u64 mips2c_goalc_adapter(void* fn, u64 stack_size, u64* args) {
  u64 stack_bytes = (stack_size + 15) & ~u64(15);
  u8* buf = (u8*)__builtin_alloca(sizeof(ExecutionContext) + stack_bytes + 16);
  auto ctx_addr = ((uintptr_t)buf + stack_bytes + 15) & ~uintptr_t(15);
  auto* ctx = (ExecutionContext*)ctx_addr;
  const int arg_regs[8] = {a0, a1, a2, a3, t0, t1, t2, t3};
  for (int i = 0; i < 8; i++) {
    ctx->gprs[arg_regs[i]].du64[0] = args[i];
  }
  ctx->gprs[s6].du64[0] = goalc_pp;
  ctx->gprs[s7].du64[0] = goalc_st;
  ctx->gprs[sp].du64[0] = ctx_addr - (uintptr_t)goalc_mem;
  ((u64(*)(void*))fn)(ctx);
  return ctx->gprs[v0].du64[0];
}

}  // namespace Mips2C
