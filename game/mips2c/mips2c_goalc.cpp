/*!
 * @file mips2c_goalc.cpp
 * Calling mips2c functions from GOAL code compiled to C (see docs/3ds-port/c_backend.md).
 */

#include <string>
#include <unordered_map>

#include "game/kernel/common/goalc_runtime.h"
#include "game/kernel/common/kperf.h"
#include "game/mips2c/mips2c_private.h"

namespace Mips2C {

namespace {
// execute function -> "m2c:<name>", for the per-section frame timing (kperf)
std::unordered_map<void*, std::string> g_names;
}  // namespace

// set in verify mode (mips2c_native.cpp): start every mips2c function with all registers zero, so
// that functions reading registers they never set (for example collide-do-primitives storing vf31
// when nothing was hit) give the same result in both runs.
bool g_mips2c_clear_context = false;

void mips2c_goalc_set_name(void* fn, const std::string& name) {
  g_names[fn] = "m2c:" + name;
}

/*!
 * Does what the native _mips2c_call trampolines do: build the MIPS register context on the stack
 * (which is in GOAL memory in C mode), with a fake GOAL stack of stack_size bytes below it, and
 * return v0. fn is the mips2c execute function.
 */
u64 mips2c_goalc_adapter(void* fn, u64 stack_size, u64* args) {
  const char* perf_name = "m2c:?";
  if (kperf::g_sections_enabled) {
    auto name = g_names.find(fn);
    if (name != g_names.end()) {
      perf_name = name->second.c_str();
    }
  }
  kperf::SectionScope perf_section(perf_name);
  u64 stack_bytes = (stack_size + 15) & ~u64(15);
  u8* buf = (u8*)__builtin_alloca(sizeof(ExecutionContext) + stack_bytes + 16);
  auto ctx_addr = ((uintptr_t)buf + stack_bytes + 15) & ~uintptr_t(15);
  auto* ctx = (ExecutionContext*)ctx_addr;
  if (g_mips2c_clear_context) {
    memset((void*)ctx, 0, sizeof(ExecutionContext));
  }
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
