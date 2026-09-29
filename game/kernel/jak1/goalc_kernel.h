#pragma once

/*!
 * @file goalc_kernel.h
 * Jak 1 kernel context primitives for GOAL code compiled to C.
 * These replace the assembly in goal_src/jak1/kernel/gkernel.gc and gstate.gc when
 * INSTRUCTION_SET is 'c. See docs/3ds-port/c_backend.md.
 */

#include "common/common_types.h"

namespace jak1 {

//! Bytes reserved after a main cpu-thread's backup stack for its saved context.
//! Must match GOALC_THREAD_CTX_SIZE in goal_src/jak1/kernel/gkernel-h.gc.
constexpr u32 GOALC_THREAD_CTX_SIZE = 192;

//! Value of the thread pc field of a thread suspended in C mode.
constexpr u32 GOALC_SUSPENDED_PC = 0xc0de5000;

//! Register the C-mode kernel primitives as GOAL functions. Call after the symbol table exists.
void goalc_kernel_init();

// the primitives, exposed for testing. Arguments and return values are GOAL values.
u64 goalc_k_return_to_kernel(u64 value);
u64 goalc_k_reset_and_call(u64 thread, u64 func);
u64 goalc_k_thread_suspend();
u64 goalc_k_thread_resume(u64 thread);
u64 goalc_k_reset_stack_and_call(u64 stack_top, u64 func, u64 a0, u64 a1, u64 a2, u64 a3);
u64 goalc_k_catch(u64 allocation, u64 type, u64 name, u64 func, u64 param_block);
u64 goalc_k_throw(u64 frame, u64 value);

}  // namespace jak1
