#pragma once

#include "common/common_types.h"
#include "common/goal_constants.h"

/*
 * Jak 1 EE memory layout.
 *
 * Default (PC):                          Small memory (OPENGOAL_SMALL_MEMORY, 3DS):
 *   0x0000000 kernel data, symbols         0x0000000 kernel data, symbols
 *   0x013fd20 global heap                  0x013fd20 global heap (42.5 MB)
 *   0x3eb82e0 (end, doubled)               0x2bc0000 kernel stack (256 kB)
 *   0x5000000 debug heap                   0x2c00000 debug heap (4 MB, only with MasterDebug)
 *   0x7ff0000 kernel stack (64 kB)         0x3000000 end (48 MB)
 *   0x8000000 end (128 MB)
 *
 * The small layout is not the original 32 MB one: with the PC port's level heaps and DMA buffers,
 * Jak 1 needs about 36 MB of global heap (in C mode, where code isn't in GOAL memory). The debug
 * heap is only big enough for the listener (-debug-mem), not for debug segments (-debug).
 *
 * The kernel stack is the stack the GOAL kernel (and the C runtime called from it) runs on. In C
 * mode, compiled code frames are bigger than native ones, which is why it's bigger in small mode.
 */

//! Toggle to use more memory. To simulate the original game's memory layout, set this to false.
// Make sure this matches the const in gcommon.gc (it follows OPENGOAL_SMALL_MEMORY there).
constexpr bool BIG_MEMORY = !OPENGOAL_SMALL_MEMORY;

//! First free address for the GOAL heap
constexpr u32 HEAP_START = 0x13fd20;

#if OPENGOAL_SMALL_MEMORY
//! Size of the kernel stack.
constexpr u32 GOAL_KERNEL_STACK_SIZE = 0x40000;
//! Top of the kernel stack (GOAL address).
constexpr u32 GOAL_KERNEL_STACK_TOP = 0x2c00000;
//! Where to end the global heap so it doesn't overlap with the stack.
constexpr u32 GLOBAL_HEAP_END = GOAL_KERNEL_STACK_TOP - GOAL_KERNEL_STACK_SIZE;
//! Where to place the debug heap
constexpr u32 DEBUG_HEAP_START = GOAL_KERNEL_STACK_TOP;
//! End of the debug heap
constexpr u32 DEBUG_HEAP_END = EE_MAIN_MEM_SIZE;
#else
//! How much space to leave for the stack when creating the debug heap
// In the game, it's 16 kB, but we increase it to 64 kB.
// ASAN builds + fmt stuff uses a _ton_ of stack when no optimizations are on and we
// need more.
constexpr u32 DEBUG_HEAP_SPACE_FOR_STACK = 0x10000;
//! Size of the kernel stack, as told to GOAL (*stack-size*). It really has
//! DEBUG_HEAP_SPACE_FOR_STACK.
constexpr u32 GOAL_KERNEL_STACK_SIZE = 0x4000;
//! Top of the kernel stack (GOAL address).
constexpr u32 GOAL_KERNEL_STACK_TOP = EE_MAIN_MEM_SIZE;
//! Where to end the global heap so it doesn't overlap with the stack.
constexpr u32 GLOBAL_HEAP_END = 0x1ffc000 + (BIG_MEMORY ? (0x1ffc000 - HEAP_START) : 0);  // doubled
//! Where to place the debug heap
constexpr u32 DEBUG_HEAP_START = 0x5000000;
//! End of the debug heap
constexpr u32 DEBUG_HEAP_END = EE_MAIN_MEM_SIZE - DEBUG_HEAP_SPACE_FOR_STACK;
#endif
static_assert(GOAL_KERNEL_STACK_TOP <= (u32)EE_MAIN_MEM_SIZE);
static_assert(DEBUG_HEAP_START <= DEBUG_HEAP_END);

//! Location of kglobalheap, kdebugheap kheapinfo structures.
constexpr u32 GLOBAL_HEAP_INFO_ADDR = 0x13AD00;
constexpr u32 DEBUG_HEAP_INFO_ADDR = 0x13AD10;
constexpr u32 LINK_CONTROL_NAME_ADDR = 0x13AD80;

namespace jak2 {
constexpr u32 DEBUG_HEAP_SIZE = 0x2f00000;
}

namespace jak3 {
constexpr u32 DEBUG_HEAP_SIZE = 0x2f00000;
}

namespace jakx {
constexpr u32 DEBUG_HEAP_SIZE = 0x2f00000;
}
