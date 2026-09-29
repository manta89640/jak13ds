#pragma once

/*!
 * @file goalc_runtime.h
 * Runtime support for GOAL code compiled to C (goalc --instruction-set c).
 * See docs/3ds-port/c_backend.md. The interface shared with generated code is goal_c_abi.h; this
 * header is internal to the C++ runtime.
 *
 * Nothing in here is used unless the runtime is in "C mode" (goalc_enabled()).
 */

#include <cstddef>
#include <cstdint>

#include "common/common_types.h"

#include "game/kernel/common/goal_c_abi.h"

// Hosts without a native GOAL backend can only run C-compiled GOAL code.
#if defined(__x86_64__) || defined(_M_X64) || defined(__aarch64__)
#define GOALC_HAS_NATIVE_BACKEND 1
#else
#define GOALC_HAS_NATIVE_BACKEND 0
#endif

// ---------------------------------------------------------------------------
// C mode switch
// ---------------------------------------------------------------------------

extern bool g_goalc_enabled;

//! Is the runtime running GOAL code compiled to C?
inline bool goalc_enabled() {
  return !GOALC_HAS_NATIVE_BACKEND || g_goalc_enabled;
}

//! Select C mode. Must happen before the GOAL heap and symbol table are initialized.
void goalc_set_enabled(bool enable);

//! Called when the GOAL heap is (re)initialized: resets function ids and sets goalc_mem/goalc_st.
void goalc_runtime_init(u8* mem, u32 st);

//! Set the stack used when a thread other than the EE thread calls into GOAL (host address range).
void goalc_set_foreign_stack(u8* bottom, u32 size);

// ---------------------------------------------------------------------------
// Function ids
// ---------------------------------------------------------------------------

//! Adapter used to expose a host function whose signature is not goalc_fn8.
//! args points to the 8 GOAL arguments (a writable array on the host/GOAL stack).
using goalc_adapter = u64 (*)(void* fn, u64 extra, u64* args);

//! Get an id for a host function callable as goalc_fn8. The same function always gets the same id.
u32 goalc_fn_id_for_host(void* fn);
//! Get an id for a host function called through an adapter. Deduplicated on (adapter, fn, extra).
u32 goalc_fn_id_for_adapted(goalc_adapter adapter, void* fn, u64 extra);
//! Allocate a contiguous range of ids for n host functions (used for modules).
u32 goalc_alloc_fn_ids(void* const* fns, u32 n);
//! Number of ids in use (including the reserved id 0).
u32 goalc_fn_count();

//! Write a function stub (id, index) at a GOAL address (the function value).
void goalc_write_stub(u32 goal_addr, u32 id, u32 index);

// adapters for kernel functions
u64 goalc_adapter_arg3_pp(void* fn, u64 extra, u64* args);
u64 goalc_adapter_stack_args(void* fn, u64 extra, u64* args);

//! Host functions for the GOAL `nothing` and `zero-func` functions.
u64 goalc_nothing_fn(u64 a0, u64, u64, u64, u64, u64, u64, u64);
u64 goalc_zero_fn(u64, u64, u64, u64, u64, u64, u64, u64);

// ---------------------------------------------------------------------------
// Modules
// ---------------------------------------------------------------------------

//! Find a module by hash: static registry first, then out/<game>/cmod/<hash>.so. Null if missing.
const GoalCModule* goalc_find_module(u64 hash);

//! Callback used by the linker to intern a symbol. Returns the st-relative offset of the symbol.
using goalc_sym_resolver = s32 (*)(const char* name);

/*!
 * Process a LINK_C_MODULE link table entry (the data after the kind byte).
 * seg_bases are the GOAL addresses of the 3 segments (0 if absent), seg_data is the host address
 * of the segment this link table belongs to. Returns the number of bytes consumed.
 */
u32 goalc_link_module_entry(const u8* link,
                            const u32 seg_bases[3],
                            u8* seg_data,
                            goalc_sym_resolver resolver,
                            const char* object_name);

// ---------------------------------------------------------------------------
// Calling GOAL
// ---------------------------------------------------------------------------

//! Is the host stack pointer currently inside GOAL memory?
bool goalc_on_goal_stack();

//! C-mode implementation of call_goal. Switches to the GOAL stack if needed.
u64 goalc_call_goal(u32 f, u64 a, u64 b, u64 c, u64 st);
//! C-mode implementation of call_goal_on_stack. stack_top is a host address in GOAL memory.
u64 goalc_call_goal_on_stack(u32 f, u8* stack_top, u64 st);
//! Call a GOAL function with 8 arguments, with pp set for the duration of the call (mips2c jalr).
u64 goalc_call_goal8(u32 f, const u64* args, u64 pp);

//! Host stack pointer as a GOAL address, for use in the caller's own frame.
#if defined(__aarch64__) || defined(__arm__)
#define GOALC_READ_HOST_SP(dst) asm volatile("mov %0, sp" : "=r"(dst))
#elif defined(__x86_64__)
#define GOALC_READ_HOST_SP(dst) asm volatile("mov %%rsp, %0" : "=r"(dst))
#else
#define GOALC_READ_HOST_SP(dst)          \
  do {                                   \
    volatile char goalc_sp_probe_;       \
    dst = (uintptr_t)&goalc_sp_probe_;   \
  } while (0)
#endif

// ---------------------------------------------------------------------------
// Low level context switching (goalc_context.cpp, per-architecture assembly)
// ---------------------------------------------------------------------------

//! Saved callee-saved registers, stack pointer and resume address of a host context.
//! Large enough for AArch64 (168 bytes), ARM32 with VFP (104 bytes) and x86-64 (64 bytes).
struct alignas(16) goalc_ctx {
  u64 regs[22];
};
constexpr size_t GOALC_CTX_SIZE = sizeof(goalc_ctx);
static_assert(GOALC_CTX_SIZE == 176);

extern "C" {
//! Save the current context. Returns 0 when saving, and the (nonzero) value passed to
//! goalc_ctx_restore when resumed. Like setjmp, the function calling this must not have returned
//! before the context is restored (or its frame must be restored at the same address).
int goalc_ctx_save(goalc_ctx* ctx) __attribute__((returns_twice));
//! Resume a context saved by goalc_ctx_save. val is forced to 1 if 0.
[[noreturn]] void goalc_ctx_restore(const goalc_ctx* ctx, int val);
//! Call fn(arg) with the host stack pointer set to stack_top (aligned down to 16 bytes).
//! Returns what fn returns, back on the original stack.
u64 goalc_call_on_stack(void* stack_top, u64 (*fn)(void*), void* arg);

//! Save the context of the caller (sp and resume address at the call site, like goalc_ctx_save)
//! in a goalc_ctx on the stack, then call goalc_suspend_impl with it. Restoring a copy of that
//! context returns from goalc_suspend_entry. Unlike a C function calling goalc_ctx_save, the
//! frame of this function doesn't have to be preserved, which keeps suspended stacks small.
u64 goalc_suspend_entry();
//! Implemented by the game kernel (Jak 1: goalc_kernel.cpp).
u64 goalc_suspend_impl(goalc_ctx* caller_ctx) __attribute__((visibility("hidden")));
}

//! Stack pointer stored in a saved context (host address).
uintptr_t goalc_ctx_sp(const goalc_ctx* ctx);
