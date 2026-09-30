/*!
 * @file goalc_kernel.cpp
 * Jak 1 kernel context primitives for GOAL code compiled to C.
 *
 * The native kernel switches between the kernel and process threads with hand written assembly
 * (gkernel.gc, gstate.gc). In C mode, the same operations are done here with goalc_ctx
 * (setjmp/longjmp style contexts) and goalc_call_on_stack:
 *
 * - The kernel side of a switch (thread-resume, reset-and-call) saves a KernelFrame in its own C
 *   frame on the kernel stack. Returning to the kernel (return-from-thread, suspend, deactivate)
 *   restores the innermost KernelFrame.
 * - A suspending thread saves its context and copies its stack [sp, stack-top) to host memory
 *   (a SuspendedThread, found through the thread object), instead of to the backup stack in the
 *   process heap. Resuming copies the stack back to the same addresses and restores the context.
 *   Compiled C frames only point into their own stack (or GOAL memory), so they survive this.
 * - catch saves a context in its C frame. throw restores it.
 *
 * Structure layouts (see gkernel-h.gc). For basics, the field at layout offset X is at obj - 4 + X.
 */

#include "goalc_kernel.h"

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "common/log/log.h"
#include "common/symbols.h"
#include "common/util/Assert.h"

#include "game/kernel/common/goalc_runtime.h"
#include "game/kernel/common/kperf.h"
#include "game/kernel/jak1/kscheme.h"

#include "fmt/format.h"

namespace jak1 {

namespace {

// thread / cpu-thread
constexpr u32 THREAD_PROCESS = 8 - 4;
constexpr u32 THREAD_PC = 24 - 4;
constexpr u32 THREAD_SP = 28 - 4;
constexpr u32 THREAD_STACK_TOP = 32 - 4;
constexpr u32 THREAD_STACK_SIZE = 36 - 4;
constexpr u32 CPU_THREAD_RREG = 40 - 4;
constexpr u32 CPU_THREAD_STACK = 128 - 4;

// process
constexpr u32 PROCESS_NAME = 4 - 4;
constexpr u32 PROCESS_STATUS = 36 - 4;
constexpr u32 PROCESS_TOP_THREAD = 48 - 4;
constexpr u32 PROCESS_STACK_FRAME_TOP = 92 - 4;

// stack-frame / catch-frame
constexpr u32 FRAME_TYPE = 0 - 4;
constexpr u32 FRAME_NAME = 4 - 4;
constexpr u32 FRAME_NEXT = 8 - 4;
constexpr u32 CATCH_FRAME_SP = 12 - 4;
constexpr u32 CATCH_FRAME_RA = 16 - 4;

constexpr u32 CATCH_MAGIC = 0x63746368;               // "hctc"


u32& word(u32 goal_addr) {
  return *(u32*)(goalc_mem + goal_addr);
}

u8* host(u32 goal_addr) {
  return goalc_mem + goal_addr;
}

u32 goal_addr_of(const void* host_ptr) {
  auto diff = (uintptr_t)host_ptr - (uintptr_t)goalc_mem;
  ASSERT_MSG(diff < 0x80000000, "goalc: host address is not in GOAL memory");
  return (u32)diff;
}

// symbols (GOAL addresses of the symbol value slots), set by goalc_kernel_init.
struct KernelSymbols {
  u32 kernel_sp = 0;
  u32 running = 0;
  u32 suspended = 0;
  u32 return_from_thread_dead = 0;
  u32 set_to_run_bootstrap = 0;
} g_syms;

// the innermost kernel context, active while a thread runs.
struct KernelFrame {
  goalc_ctx ctx;
  u64 value = 0;
  KernelFrame* prev = nullptr;
};
KernelFrame* g_kernel_frame = nullptr;

struct CatchContext {
  goalc_ctx ctx;
  u64 value = 0;
  u32 magic = 0;
};

// Arguments for the function started by goalc_call_on_stack. The new stack may overlap the frames
// of the caller (enter-state resets the stack it runs on), so the arguments go through a global.
struct PendingCall {
  u32 f = 0;
  u64 args[8] = {};
} g_pending;

u64 call_goal_fn(u32 f, const u64* a) {
  return ((goalc_fn8)goalc_fn(f))(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
}

[[noreturn]] void return_to_kernel(u64 value) {
  KernelFrame* kf = g_kernel_frame;
  if (!kf) {
    lg::die("goalc: tried to return to the kernel, but there is no kernel context");
    abort();
  }
  kf->value = value;
  goalc_ctx_restore(&kf->ctx, 1);
}

void set_kernel_sp() {
  if (g_syms.kernel_sp) {
    word(g_syms.kernel_sp) = (u32)goalc_get_sp();
  }
}

//! Name of a symbol or string, for error messages.
std::string goal_name(u32 obj) {
  if (!obj || (obj & 7) != 4) {
    return fmt::format("#x{:x}", obj);
  }
  u32 type = word(obj - 4);
  if (type == *(s7 + jak1_symbols::FIX_SYM_SYMBOL_TYPE)) {
    return info(Ptr<Symbol>(obj))->str->data();
  }
  if (type == *(s7 + jak1_symbols::FIX_SYM_STRING_TYPE)) {
    return Ptr<String>(obj)->data();
  }
  return fmt::format("#x{:x}", obj);
}

// Suspended threads: their context and stack live in host memory, not in the process heap.
// Compiled C frames are bigger than native ones by an amount that depends on the host and the
// code (up to ~12x for vector heavy functions on ARM32), so the game's backup stack sizes are
// useless, and making them big enough for the worst case costs too much process heap.
// The slot is found through the thread: pc is GOALC_SUSPENDED_PC and rreg 0 holds
// (token << 32) | slot index. Slots are freed on resume and by __goalc-thread-release, which the
// GOAL kernel calls when a suspended thread is thrown away (deactivate, set-to-run).
struct SuspendedThread {
  goalc_ctx ctx;
  std::vector<u8> stack;
  u32 token = 0;
  bool used = false;
  u32 owner = 0;  // thread address when it suspended (for error messages; relocation moves threads)
  u32 index = 0;  // in g_suspended
};
std::vector<std::unique_ptr<SuspendedThread>> g_suspended;
std::vector<u32> g_free_slots;
u32 g_next_token = 1;
u32 g_slots_in_use = 0;

//! the biggest stack a thread may suspend with
constexpr u32 MAX_SUSPENDED_STACK = 64 * 1024;

u64& thread_rreg0(u32 thread) {
  return *(u64*)host(thread + CPU_THREAD_RREG);
}

u32 alloc_slot() {
  u32 idx;
  if (!g_free_slots.empty()) {
    idx = g_free_slots.back();
    g_free_slots.pop_back();
  } else {
    idx = (u32)g_suspended.size();
    g_suspended.push_back(std::make_unique<SuspendedThread>());
    g_suspended.back()->index = idx;
  }
  auto& slot = *g_suspended[idx];
  slot.used = true;
  slot.token = g_next_token++;
  if (!g_next_token) {
    g_next_token = 1;
  }
  g_slots_in_use++;
  return idx;
}

//! The slot of a suspended thread, or null if the thread isn't suspended (in C mode).
SuspendedThread* thread_slot(u32 thread) {
  if (word(thread + THREAD_PC) != GOALC_SUSPENDED_PC) {
    return nullptr;
  }
  u64 handle = thread_rreg0(thread);
  u32 idx = (u32)handle;
  u32 token = (u32)(handle >> 32);
  if (idx >= g_suspended.size()) {
    return nullptr;
  }
  auto* slot = g_suspended[idx].get();
  if (!slot->used || slot->token != token) {
    return nullptr;
  }
  return slot;
}

void free_slot(SuspendedThread* slot) {
  slot->used = false;
  slot->token = 0;
  // keep the stack buffer for the next user, unless it's unusually big
  if (slot->stack.capacity() > 8192) {
    std::vector<u8>().swap(slot->stack);
  }
  g_free_slots.push_back(slot->index);
  g_slots_in_use--;
}

[[noreturn]] void call_return_from_thread_dead(u64 value) {
  // the native code installs return-from-thread-dead as the return address.
  if (g_syms.return_from_thread_dead) {
    u32 f = word(g_syms.return_from_thread_dead);
    if (f) {
      u64 args[8] = {value, 0, 0, 0, 0, 0, 0, 0};
      value = call_goal_fn(f, args);
    }
  }
  // return-from-thread-dead returns to the kernel itself, this is just in case it doesn't.
  return_to_kernel(value);
}

// These run at the top of a thread's stack, and everything they keep on the stack counts against
// the backup stack of a suspending thread. So the arguments are passed straight from g_pending.

//! Entry for reset-and-call: call the function, then return to the kernel with its result.
u64 reset_and_call_entry(void*) {
  u64 result = ((goalc_fn8)goalc_fn(g_pending.f))(
      g_pending.args[0], g_pending.args[1], g_pending.args[2], g_pending.args[3],
      g_pending.args[4], g_pending.args[5], g_pending.args[6], g_pending.args[7]);
  return_to_kernel(result);
}

//! Entry for threads that end with return-from-thread-dead (set-to-run, enter-state).
u64 dead_on_return_entry(void*) {
  u64 result = ((goalc_fn8)goalc_fn(g_pending.f))(
      g_pending.args[0], g_pending.args[1], g_pending.args[2], g_pending.args[3],
      g_pending.args[4], g_pending.args[5], g_pending.args[6], g_pending.args[7]);
  call_return_from_thread_dead(result);
}

u32 stack_copy_size(u32 stack_top, u32 sp) {
  // the native code copies 8 bytes at a time, from stack-top down to (and including) sp.
  return (stack_top - sp + 7) & ~7u;
}

}  // namespace

u64 goalc_k_return_to_kernel(u64 value) {
  return_to_kernel(value);
}

/*!
 * reset-and-call: run func on the thread's stack (from the top). When func returns (or anything
 * returns to the kernel), return here. pp, status and top-thread are set up by the GOAL caller.
 */
u64 goalc_k_reset_and_call(u64 thread, u64 func) {
  KernelFrame kf;
  kf.prev = g_kernel_frame;
  g_kernel_frame = &kf;
  if (goalc_ctx_save(&kf.ctx) == 0) {
    set_kernel_sp();
    u32 t = (u32)thread;
    g_pending.f = (u32)func;
    memset(g_pending.args, 0, sizeof(g_pending.args));
    // the native code jumps to func with the arguments of reset-and-call still in place.
    g_pending.args[0] = thread;
    g_pending.args[1] = func;
    goalc_call_on_stack(host(word(t + THREAD_STACK_TOP)), reset_and_call_entry, nullptr);
    ASSERT_NOT_REACHED();
  }
  g_kernel_frame = kf.prev;
  return kf.value;
}

}  // namespace jak1

/*!
 * thread-suspend for a cpu-thread (called through goalc_suspend_entry, so caller_ctx is the context
 * of the GOAL thread-suspend method at the call). pp holds the thread. Saves the context and the
 * stack, then returns to the kernel. Resuming the thread returns from goalc_suspend_entry.
 */
u64 goalc_suspend_impl(goalc_ctx* caller_ctx) {
  using namespace jak1;
  u32 thread = (u32)goalc_pp;
  u32 sp = goal_addr_of((void*)goalc_ctx_sp(caller_ctx));
  u32 proc = word(thread + THREAD_PROCESS);
  u32 stack_top = word(thread + THREAD_STACK_TOP);
  u32 copy_size = stack_copy_size(stack_top, sp);
  if (copy_size > MAX_SUSPENDED_STACK) {
    lg::error("goalc: thread-suspend of {} with {} bytes of stack used",
              goal_name(word(proc + PROCESS_NAME)), copy_size);
    goalc_print_backtrace(caller_ctx);
    goalc_break();
    abort();
  }

  // a thread that is suspended again without being resumed (shouldn't happen) reuses its slot
  SuspendedThread* slot = thread_slot(thread);
  u32 idx;
  if (slot) {
    idx = (u32)thread_rreg0(thread);
  } else {
    idx = alloc_slot();
    slot = g_suspended[idx].get();
  }
  memcpy(&slot->ctx, caller_ctx, sizeof(goalc_ctx));
  slot->owner = thread;
  slot->stack.resize(copy_size);
  memcpy(slot->stack.data(), host(stack_top - copy_size), copy_size);
  kperf::count(kperf::Counter::SUSPEND, 1);
  kperf::count(kperf::Counter::SUSPEND_BYTES, copy_size);

  thread_rreg0(thread) = ((u64)slot->token << 32) | idx;
  word(thread + THREAD_PC) = GOALC_SUSPENDED_PC;
  word(thread + THREAD_SP) = sp;
  word(proc + PROCESS_STATUS) = g_syms.suspended;

  goalc_pp = 0;
  return_to_kernel(0);
}

namespace jak1 {

/*!
 * thread-resume for a cpu-thread. Called by the kernel. Returns when the thread returns to the
 * kernel (suspend, return-from-thread, deactivate...).
 */
u64 goalc_k_thread_resume(u64 thread_arg) {
  KernelFrame kf;
  kf.prev = g_kernel_frame;
  g_kernel_frame = &kf;
  if (goalc_ctx_save(&kf.ctx) == 0) {
    set_kernel_sp();
    u32 thread = (u32)thread_arg;
    u32 proc = word(thread + THREAD_PROCESS);
    u32 pc = word(thread + THREAD_PC);
    u32 sp = word(thread + THREAD_SP);
    u32 stack_top = word(thread + THREAD_STACK_TOP);

    if (pc == GOALC_SUSPENDED_PC) {
      SuspendedThread* slot = thread_slot(thread);
      if (!slot) {
        u64 handle = thread_rreg0(thread);
        u32 idx = (u32)handle;
        std::string detail = "no such slot";
        if (idx < g_suspended.size()) {
          auto* s = g_suspended[idx].get();
          detail = fmt::format("slot {} used {} token {} (thread has {}), last owner #x{:x}", idx,
                               s->used, s->token, (u32)(handle >> 32), s->owner);
        }
        lg::die("goalc: thread-resume of thread #x{:x} ({}) which has no saved context: {}", thread,
                goal_name(word(proc + PROCESS_NAME)), detail);
      }
      // restore the stack
      u32 copy_size = stack_copy_size(stack_top, sp);
      ASSERT(copy_size == slot->stack.size());
      memcpy(host(stack_top - copy_size), slot->stack.data(), copy_size);
      word(proc + PROCESS_TOP_THREAD) = thread;
      word(proc + PROCESS_STATUS) = g_syms.running;
      word(thread + THREAD_PC) = 0;
      goalc_pp = proc;
      // the slot memory stays valid until the next suspend, which can't happen before the restore
      free_slot(slot);
      goalc_ctx_restore(&slot->ctx, 1);
    } else {
      // A thread that was set up with set-to-run: pc is a GOAL function (set-to-run-bootstrap),
      // which natively gets jumped to with sp at thread.sp (the stack top).
      if (!pc) {
        lg::die("goalc: thread-resume of thread #x{:x} which has no pc", thread);
      }
      word(proc + PROCESS_TOP_THREAD) = thread;
      word(proc + PROCESS_STATUS) = g_syms.running;
      goalc_pp = proc;
      memset(g_pending.args, 0, sizeof(g_pending.args));
      if (g_syms.set_to_run_bootstrap && pc == word(g_syms.set_to_run_bootstrap)) {
        // do what set-to-run-bootstrap does directly, to save a frame on the thread's stack:
        // call the function in rreg 0 with the arguments in rreg 1-6.
        const u64* rreg = (const u64*)host(thread + CPU_THREAD_RREG);
        g_pending.f = (u32)rreg[0];
        for (int i = 0; i < 6; i++) {
          g_pending.args[i] = rreg[i + 1];
        }
      } else {
        g_pending.f = pc;
      }
      goalc_call_on_stack(host(sp), dead_on_return_entry, nullptr);
    }
    ASSERT_NOT_REACHED();
  }
  g_kernel_frame = kf.prev;
  return kf.value;
}

/*!
 * enter-state's "reset the stack and jump to the state code": call func with 4 arguments at the
 * top of the given stack. When it returns, return-from-thread-dead is called. Never returns.
 */
u64 goalc_k_reset_stack_and_call(u64 stack_top, u64 func, u64 a0, u64 a1, u64 a2, u64 a3) {
  g_pending.f = (u32)func;
  memset(g_pending.args, 0, sizeof(g_pending.args));
  g_pending.args[0] = a0;
  g_pending.args[1] = a1;
  g_pending.args[2] = a2;
  g_pending.args[3] = a3;
  goalc_call_on_stack(host((u32)stack_top), dead_on_return_entry, nullptr);
  ASSERT_NOT_REACHED();
  return 0;
}

/*!
 * new method of catch-frame: set up a catch frame at allocation, push it on the process's frame
 * stack, and call func with the 6 parameters in param_block. A throw to this frame makes this
 * return the thrown value.
 */
u64 goalc_k_catch(u64 allocation, u64 type, u64 name, u64 func, u64 param_block) {
  u32 frame = (u32)allocation + 4;
  word(frame + FRAME_TYPE) = (u32)type;
  word(frame + FRAME_NAME) = (u32)name;

  CatchContext cc;
  cc.magic = CATCH_MAGIC;
  word(frame + CATCH_FRAME_RA) = goal_addr_of(&cc);
  word(frame + CATCH_FRAME_SP) = (u32)goalc_get_sp();

  u32 pp = (u32)goalc_pp;
  word(frame + FRAME_NEXT) = word(pp + PROCESS_STACK_FRAME_TOP);
  word(pp + PROCESS_STACK_FRAME_TOP) = frame;

  if (goalc_ctx_save(&cc.ctx) == 0) {
    const u64* params = (const u64*)host((u32)param_block);
    u64 args[8] = {params[0], params[1], params[2], params[3], params[4], params[5], 0, 0};
    u64 result = call_goal_fn((u32)func, args);
    // pop, using the pp at the time of the return (like the native code)
    u32 pp_now = (u32)goalc_pp;
    word(pp_now + PROCESS_STACK_FRAME_TOP) =
        word(word(pp_now + PROCESS_STACK_FRAME_TOP) + FRAME_NEXT);
    cc.magic = 0;
    return result;
  }
  // thrown to. throw already popped the frames.
  cc.magic = 0;
  return cc.value;
}

/*!
 * throw-dispatch: pop frames through the catch frame and resume its catch with value.
 */
u64 goalc_k_throw(u64 frame_arg, u64 value) {
  u32 frame = (u32)frame_arg;
  u32 pp = (u32)goalc_pp;
  word(pp + PROCESS_STACK_FRAME_TOP) = word(frame + FRAME_NEXT);
  auto* cc = (CatchContext*)host(word(frame + CATCH_FRAME_RA));
  if (cc->magic != CATCH_MAGIC) {
    lg::die("goalc: throw to catch frame #x{:x} which is not active", frame);
  }
  cc->value = value;
  goalc_ctx_restore(&cc->ctx, 1);
}

/*!
 * The GOAL kernel no longer needs a suspended thread (deactivate, set-to-run): free its saved
 * context and stack.
 */
u64 goalc_k_thread_release(u64 thread) {
  if ((u32)thread && (u32)thread != (u32)goalc_st) {
    SuspendedThread* slot = thread_slot((u32)thread);
    if (slot) {
      lg::debug("goalc: releasing the saved state of suspended thread #x{:x} (suspended as #x{:x})",
                (u32)thread, slot->owner);
      free_slot(slot);
      word((u32)thread + THREAD_PC) = 0;
    }
  }
  return 0;
}

u32 goalc_suspended_thread_count() {
  return g_slots_in_use;
}

void goalc_kernel_set_symbols(u32 kernel_sp,
                              u32 running,
                              u32 suspended,
                              u32 return_from_thread_dead,
                              u32 set_to_run_bootstrap) {
  g_kernel_frame = nullptr;
  g_suspended.clear();
  g_free_slots.clear();
  g_slots_in_use = 0;
  g_syms.kernel_sp = kernel_sp;
  g_syms.running = running;
  g_syms.suspended = suspended;
  g_syms.return_from_thread_dead = return_from_thread_dead;
  g_syms.set_to_run_bootstrap = set_to_run_bootstrap;
}

void goalc_kernel_init() {
  g_kernel_frame = nullptr;
  g_suspended.clear();
  g_free_slots.clear();
  g_slots_in_use = 0;
  g_syms.kernel_sp = intern_from_c("*kernel-sp*").offset;
  g_syms.running = intern_from_c("running").offset;
  g_syms.suspended = intern_from_c("suspended").offset;
  g_syms.return_from_thread_dead = intern_from_c("return-from-thread-dead").offset;
  g_syms.set_to_run_bootstrap = intern_from_c("set-to-run-bootstrap").offset;


  make_function_symbol_from_c("__goalc-return-to-kernel", goalc_k_return_to_kernel);
  make_function_symbol_from_c("__goalc-reset-and-call", goalc_k_reset_and_call);
  make_function_symbol_from_c("__goalc-thread-suspend", goalc_suspend_entry);
  make_function_symbol_from_c("__goalc-thread-resume", goalc_k_thread_resume);
  make_function_symbol_from_c("__goalc-thread-release", goalc_k_thread_release);
  make_function_symbol_from_c("__goalc-reset-stack-and-call",
                              goalc_k_reset_stack_and_call);
  make_function_symbol_from_c("__goalc-catch", goalc_k_catch);
  make_function_symbol_from_c("__goalc-throw", goalc_k_throw);
}

}  // namespace jak1
