#pragma once

/*!
 * @file goalc_kernel.h
 * Jak 2 kernel context primitives (AI-assisted; adapted from the Jak 1 version) for GOAL code compiled to C.
 * These replace the assembly in goal_src/jak2/kernel/gkernel.gc and gstate.gc when
 * INSTRUCTION_SET is 'c. See docs/3ds-port/c_backend.md.
 */

#include "common/common_types.h"

#include "game/kernel/common/goalc_runtime.h"

namespace jak2 {

//! Value of the thread pc field of a thread suspended in C mode.
constexpr u32 GOALC_SUSPENDED_PC = 0xc0de5000;

//! Register the C-mode kernel primitives as GOAL functions. Call after the symbol table exists.
void goalc_kernel_init();


// the primitives, exposed for testing. Arguments and return values are GOAL values.
u64 goalc_k_return_to_kernel(u64 value);
u64 goalc_k_reset_and_call(u64 thread, u32 func);
u64 goalc_k_thread_resume(u64 thread);
u64 goalc_k_reset_stack_and_call(u32 stack_top, u32 func, u64 a0, u64 a1, u64 a2, u64 a3);
u64 goalc_k_catch(u32 allocation, u32 type, u32 name, u32 func, u32 param_block);
u64 goalc_k_throw(u64 frame, u64 value);
u64 goalc_k_thread_release(u64 thread);
//! suspend for Jak 2 (goalc_suspend_impl dispatches here when the game is Jak 2)
u64 goalc_suspend_impl_jak2(goalc_ctx* caller_ctx);
//! number of threads suspended right now (for tests and leak checks)
u32 goalc_suspended_thread_count();

}  // namespace jak2
