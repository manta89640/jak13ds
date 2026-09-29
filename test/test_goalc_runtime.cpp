// Tests for the runtime side of the GOAL -> C backend (game/kernel/common/goalc_runtime.h and
// game/kernel/jak1/goalc_kernel.h): context switching, stack copying, catch/throw, function ids.
// These run on the host with a fake GOAL memory, without compiled GOAL code.

#include <algorithm>
#include <cstring>
#include <vector>

#include "common/common_types.h"
#include "common/link_types.h"

#include "game/kernel/common/goalc_runtime.h"
#include "game/kernel/jak1/goalc_kernel.h"
#include "gtest/gtest.h"

#if defined(__aarch64__) || (defined(__x86_64__) && !defined(_WIN32))

namespace {

// ---------------------------------------------------------------------------
// Low level: goalc_ctx_save/restore and goalc_call_on_stack
// ---------------------------------------------------------------------------

goalc_ctx g_ctx;
int g_restore_count = 0;

[[noreturn]] void jump_back(int val) {
  g_restore_count++;
  goalc_ctx_restore(&g_ctx, val);
}

TEST(GoalcContext, SaveRestore) {
  volatile int local = 12;
  volatile double fp = 1.25;
  g_restore_count = 0;
  int r = goalc_ctx_save(&g_ctx);
  if (r == 0) {
    local = 13;
    fp = 2.5;
    jump_back(7);
  }
  EXPECT_EQ(r, 7);
  EXPECT_EQ(local, 13);
  EXPECT_EQ(fp, 2.5);
  EXPECT_EQ(g_restore_count, 1);

  // restoring with 0 returns 1, like longjmp
  r = goalc_ctx_save(&g_ctx);
  if (r == 0) {
    jump_back(0);
  }
  EXPECT_EQ(r, 1);
}

alignas(16) u8 g_side_stack[64 * 1024];

u64 on_side_stack(void* arg) {
  uintptr_t sp;
  GOALC_READ_HOST_SP(sp);
  *(uintptr_t*)arg = sp;
  return 0x1234567890abcdefull;
}

TEST(GoalcContext, CallOnStack) {
  uintptr_t sp = 0;
  u64 result = goalc_call_on_stack(g_side_stack + sizeof(g_side_stack) - 8, on_side_stack, &sp);
  EXPECT_EQ(result, 0x1234567890abcdefull);
  EXPECT_GT(sp, (uintptr_t)g_side_stack);
  EXPECT_LE(sp, (uintptr_t)g_side_stack + sizeof(g_side_stack) - 16);
  EXPECT_EQ(sp % 16, 0u);
}

// ---------------------------------------------------------------------------
// Fake GOAL memory with Jak 1 kernel structures
// ---------------------------------------------------------------------------

constexpr u32 kMemSize = 4 * 1024 * 1024;
constexpr u32 kStackSize = 64 * 1024;

// layout offsets from the object pointer (see goalc_kernel.cpp)
constexpr u32 THREAD_PROCESS = 4;
constexpr u32 THREAD_PC = 20;
constexpr u32 THREAD_SP = 24;
constexpr u32 THREAD_STACK_TOP = 28;
constexpr u32 THREAD_STACK_SIZE = 32;
constexpr u32 THREAD_RREG = 36;
constexpr u32 PROCESS_STATUS = 32;
constexpr u32 PROCESS_TOP_THREAD = 44;
constexpr u32 PROCESS_STACK_FRAME_TOP = 88;
constexpr u32 FRAME_NEXT = 4;

struct FakeGoal {
  std::vector<u8> storage;
  u8* mem = nullptr;
  u32 next = 0x1000;

  // symbols
  u32 sym_kernel_sp = 0;
  u32 sym_running = 0;
  u32 sym_suspended = 0;
  u32 sym_bootstrap = 0;

  u32 bootstrap_fn = 0;
  u32 exec_stack_top = 0;
  u32 kernel_stack_top = 0;

  FakeGoal() {
    storage.resize(kMemSize + 16);
    mem = (u8*)(((uintptr_t)storage.data() + 15) & ~uintptr_t(15));
    memset(mem, 0, kMemSize);
    goalc_runtime_init(mem, alloc(64));
    sym_kernel_sp = alloc(8);
    sym_running = alloc(8);
    sym_suspended = alloc(8);
    sym_bootstrap = alloc(8);
    // any function value works as the bootstrap marker, it's compared, not called.
    bootstrap_fn = make_function((void*)goalc_zero_fn);
    w(sym_bootstrap) = bootstrap_fn;
    jak1::goalc_kernel_set_symbols(sym_kernel_sp, sym_running, sym_suspended, 0, sym_bootstrap);
    exec_stack_top = alloc(kStackSize) + kStackSize;
    kernel_stack_top = alloc(kStackSize) + kStackSize;
  }

  u32 alloc(u32 size) {
    u32 result = next;
    next = (next + size + 15) & ~15u;
    EXPECT_LT(next, kMemSize);
    return result;
  }

  u32& w(u32 addr) { return *(u32*)(mem + addr); }

  u32 make_function(void* host_fn) {
    u32 f = alloc(16) + 4;
    goalc_write_stub(f, goalc_fn_id_for_host(host_fn), 0);
    return f;
  }

  u32 make_process() { return alloc(128) + 4; }

  //! a main cpu-thread, with room for the context after the backup stack.
  u32 make_thread(u32 proc, u32 backup_size) {
    u32 thread = alloc(128 + backup_size + jak1::GOALC_THREAD_CTX_SIZE) + 4;
    w(thread + THREAD_PROCESS) = proc;
    w(thread + THREAD_STACK_TOP) = exec_stack_top;
    w(thread + THREAD_SP) = exec_stack_top;
    w(thread + THREAD_STACK_SIZE) = backup_size;
    w(proc + PROCESS_TOP_THREAD) = thread;
    return thread;
  }

  //! like set-to-run
  void set_to_run(u32 thread, u32 func, u64 a0) {
    u64* rreg = (u64*)(mem + thread + THREAD_RREG);
    rreg[0] = func;
    rreg[1] = a0;
    w(thread + THREAD_PC) = bootstrap_fn;
    w(thread + THREAD_SP) = w(thread + THREAD_STACK_TOP);
  }

  u32 goal_addr(const void* p) { return (u32)((const u8*)p - mem); }
};

FakeGoal* g_goal = nullptr;

// ---------------------------------------------------------------------------
// Threads that suspend, sharing one execution stack
// ---------------------------------------------------------------------------

constexpr int kThreads = 3;
constexpr int kIterations = 5;
u32 g_threads[kThreads];
u32 g_procs[kThreads];
int g_progress[kThreads];
bool g_ok[kThreads];
u64 g_max_used = 0;

void suspend_now(int which) {
  // like the suspend macro: pp is the thread when calling the suspend hook
  goalc_pp = g_threads[which];
  goalc_suspend_entry();
  // the kernel sets pp to the process on resume
  if (goalc_pp != g_procs[which]) {
    g_ok[which] = false;
  }
}

u64 thread_body(u64 which_u, u64, u64, u64, u64, u64, u64, u64) {
  int which = (int)which_u;
  // locals that must survive the stack being overwritten by the other threads
  volatile u64 pattern[16];
  for (int i = 0; i < 16; i++) {
    pattern[i] = 0x1111111111111111ull * (u64)(which + 1) + (u64)i;
  }
  double fp = 3.5 * (which + 1);
  u64 in_register = 0xabc0000000000000ull | (u64)which;
  // pointer to our own stack, which only works if the stack comes back to the same address
  volatile u64* self_ptr = &pattern[3];

  uintptr_t sp;
  GOALC_READ_HOST_SP(sp);
  if (sp < (uintptr_t)g_goal->mem || sp >= (uintptr_t)g_goal->mem + kMemSize) {
    g_ok[which] = false;
  }

  for (int i = 0; i < kIterations; i++) {
    g_progress[which] = i;
    suspend_now(which);
    for (int j = 0; j < 16; j++) {
      if (pattern[j] != 0x1111111111111111ull * (u64)(which + 1) + (u64)j) {
        g_ok[which] = false;
      }
    }
    if (*self_ptr != pattern[3] || fp != 3.5 * (which + 1) ||
        in_register != (0xabc0000000000000ull | (u64)which)) {
      g_ok[which] = false;
    }
    fp += 0.0;
    in_register ^= 0;
  }
  g_progress[which] = kIterations;
  return 100 + (u64)which;
}

u64 kernel_run_threads(void*) {
  u64 results[kThreads] = {};
  for (int round = 0; round < kIterations + 1; round++) {
    for (int t = 0; t < kThreads; t++) {
      u64 r = jak1::goalc_k_thread_resume(g_threads[t]);
      if (round < kIterations) {
        EXPECT_EQ(r, 0u);  // suspended
        EXPECT_EQ(g_goal->w(g_procs[t] + PROCESS_STATUS), g_goal->sym_suspended);
        EXPECT_EQ(g_goal->w(g_threads[t] + THREAD_PC), jak1::GOALC_SUSPENDED_PC);
        u64 used = g_goal->w(g_threads[t] + THREAD_STACK_TOP) - g_goal->w(g_threads[t] + THREAD_SP);
        g_max_used = std::max(g_max_used, used);
        // wreck the execution stack, the thread must not depend on it while suspended
        memset(g_goal->mem + g_goal->exec_stack_top - 4096, 0xcd, 4096);
      } else {
        results[t] = r;  // returned
      }
      EXPECT_EQ(g_progress[t], std::min(round, kIterations));
    }
  }
  for (int t = 0; t < kThreads; t++) {
    EXPECT_EQ(results[t], 100u + t);
  }
  return 0;
}

TEST(GoalcKernel, SuspendResumeSharedStack) {
  FakeGoal goal;
  g_goal = &goal;
  u32 body = goal.make_function((void*)thread_body);
  for (int t = 0; t < kThreads; t++) {
    g_procs[t] = goal.make_process();
    g_threads[t] = goal.make_thread(g_procs[t], 4096);
    goal.set_to_run(g_threads[t], body, t);
    g_progress[t] = -1;
    g_ok[t] = true;
  }
  g_max_used = 0;
  goalc_call_on_stack(goal.mem + goal.kernel_stack_top, kernel_run_threads, nullptr);
  for (int t = 0; t < kThreads; t++) {
    EXPECT_TRUE(g_ok[t]) << "thread " << t;
    EXPECT_EQ(g_progress[t], kIterations);
  }
  EXPECT_GT(g_max_used, 0u);
  EXPECT_LT(g_max_used, 4096u);
  // *kernel-sp* was set to somewhere on the kernel stack
  u32 ksp = goal.w(goal.sym_kernel_sp);
  EXPECT_LT(ksp, goal.kernel_stack_top);
  EXPECT_GT(ksp, goal.kernel_stack_top - kStackSize);
  g_goal = nullptr;
}

// ---------------------------------------------------------------------------
// reset-and-call, return-to-kernel and reset-stack-and-call
// ---------------------------------------------------------------------------

u32 g_reset_thread = 0;
u64 g_seen_args[4];

u64 returns_55(u64 a0, u64 a1, u64, u64, u64, u64, u64, u64) {
  g_seen_args[0] = a0;
  g_seen_args[1] = a1;
  return 55;
}

void nested_return_to_kernel() {
  jak1::goalc_k_return_to_kernel(66);
}

u64 returns_to_kernel(u64, u64, u64, u64, u64, u64, u64, u64) {
  nested_return_to_kernel();
  return 1;  // not reached
}

u64 after_reset(u64 a0, u64 a1, u64 a2, u64 a3, u64, u64, u64, u64) {
  g_seen_args[0] = a0;
  g_seen_args[1] = a1;
  g_seen_args[2] = a2;
  g_seen_args[3] = a3;
  uintptr_t sp;
  GOALC_READ_HOST_SP(sp);
  // we should be close to the top of the stack again
  EXPECT_GT(sp, (uintptr_t)g_goal->mem + g_goal->exec_stack_top - 1024);
  return 77;
}

u32 g_after_reset_fn = 0;

void deep_recursion(int n) {
  volatile char pad[256];
  pad[0] = (char)n;
  if (n > 0) {
    deep_recursion(n - 1);
  } else {
    jak1::goalc_k_reset_stack_and_call(g_goal->exec_stack_top, g_after_reset_fn, 1, 2, 3, 4);
  }
  (void)pad[0];
}

u64 resets_its_stack(u64, u64, u64, u64, u64, u64, u64, u64) {
  deep_recursion(10);
  return 1;  // not reached
}

u64 kernel_reset_tests(void*) {
  u32 f55 = g_goal->make_function((void*)returns_55);
  EXPECT_EQ(jak1::goalc_k_reset_and_call(g_reset_thread, f55), 55u);
  EXPECT_EQ(g_seen_args[0], g_reset_thread);  // the native code leaves the arguments in place
  EXPECT_EQ(g_seen_args[1], f55);

  u32 f66 = g_goal->make_function((void*)returns_to_kernel);
  EXPECT_EQ(jak1::goalc_k_reset_and_call(g_reset_thread, f66), 66u);

  // reset-stack-and-call from a thread, the function's return goes to the kernel (there is no
  // return-from-thread-dead in this test)
  g_after_reset_fn = g_goal->make_function((void*)after_reset);
  u32 freset = g_goal->make_function((void*)resets_its_stack);
  EXPECT_EQ(jak1::goalc_k_reset_and_call(g_reset_thread, freset), 77u);
  EXPECT_EQ(g_seen_args[0], 1u);
  EXPECT_EQ(g_seen_args[1], 2u);
  EXPECT_EQ(g_seen_args[2], 3u);
  EXPECT_EQ(g_seen_args[3], 4u);

  // nesting works too
  return 0;
}

TEST(GoalcKernel, ResetAndCall) {
  FakeGoal goal;
  g_goal = &goal;
  u32 proc = goal.make_process();
  g_reset_thread = goal.make_thread(proc, 512);
  goalc_call_on_stack(goal.mem + goal.kernel_stack_top, kernel_reset_tests, nullptr);
  g_goal = nullptr;
}

// ---------------------------------------------------------------------------
// catch / throw
// ---------------------------------------------------------------------------

u32 g_catch_frame = 0;
u32 g_catch_proc = 0;

void throw_from_deep(int n) {
  volatile char pad[128];
  pad[0] = (char)n;
  if (n > 0) {
    throw_from_deep(n - 1);
  } else {
    jak1::goalc_k_throw(g_catch_frame, 42);
  }
  (void)pad[0];
}

u64 catch_body_throws(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64, u64) {
  EXPECT_EQ(a0 + a1 + a2 + a3 + a4 + a5, 21u);
  // the frame is on top of the process's frames
  EXPECT_EQ(g_goal->w(g_catch_proc + PROCESS_STACK_FRAME_TOP), g_catch_frame);
  throw_from_deep(5);
  return 1;  // not reached
}

u64 catch_body_returns(u64 a0, u64, u64, u64, u64, u64, u64, u64) {
  return a0 * 2;
}

u64 kernel_catch_tests(void*) {
  goalc_pp = g_catch_proc;
  u32 old_top = g_goal->alloc(16) + 4;
  g_goal->w(g_catch_proc + PROCESS_STACK_FRAME_TOP) = old_top;

  alignas(16) u8 frame_mem[256];
  u64 params[6] = {1, 2, 3, 4, 5, 6};
  u32 alloc = g_goal->goal_addr(frame_mem);
  g_catch_frame = alloc + 4;

  volatile double fp = 9.75;
  u32 thrower = g_goal->make_function((void*)catch_body_throws);
  u64 r = jak1::goalc_k_catch(alloc, 0x1234, 0x5678, thrower, g_goal->goal_addr(params));
  EXPECT_EQ(r, 42u);
  EXPECT_EQ(fp, 9.75);
  EXPECT_EQ(g_goal->w(g_catch_proc + PROCESS_STACK_FRAME_TOP), old_top);
  EXPECT_EQ(g_goal->w(g_catch_frame + FRAME_NEXT), old_top);
  EXPECT_EQ(g_goal->w(g_catch_frame - 4), 0x1234u);  // type
  EXPECT_EQ(g_goal->w(g_catch_frame), 0x5678u);      // name

  u32 returner = g_goal->make_function((void*)catch_body_returns);
  r = jak1::goalc_k_catch(alloc, 0x1234, 0x5678, returner, g_goal->goal_addr(params));
  EXPECT_EQ(r, 2u);
  EXPECT_EQ(g_goal->w(g_catch_proc + PROCESS_STACK_FRAME_TOP), old_top);
  return 0;
}

TEST(GoalcKernel, CatchThrow) {
  FakeGoal goal;
  g_goal = &goal;
  g_catch_proc = goal.make_process();
  goalc_call_on_stack(goal.mem + goal.kernel_stack_top, kernel_catch_tests, nullptr);
  g_goal = nullptr;
}

// ---------------------------------------------------------------------------
// Function ids and stubs
// ---------------------------------------------------------------------------

u64 add8(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7) {
  return a0 + a1 + a2 + a3 + a4 + a5 + a6 + a7;
}

u64 takes_pp(u64 a0, u64 a1, u64 a2, u64 pp) {
  return a0 + a1 + a2 + pp;
}

u64 takes_array(u64* args) {
  u64 result = 0;
  for (int i = 0; i < 8; i++) {
    result = result * 10 + args[i];
  }
  return result;
}

TEST(GoalcRuntime, FunctionIds) {
  FakeGoal goal;
  u32 f = goal.make_function((void*)add8);
  EXPECT_EQ(((goalc_fn8)goalc_fn(f))(1, 2, 3, 4, 5, 6, 7, 8), 36u);
  // same host function, same id
  EXPECT_EQ(goalc_fn_id_for_host((void*)add8), goalc_fn_id_for_host((void*)add8));
  // id 0 is reserved
  EXPECT_NE(goalc_fn_id_for_host((void*)add8), 0u);

  goalc_pp = 1000;
  u32 id_pp = goalc_fn_id_for_adapted(goalc_adapter_arg3_pp, (void*)takes_pp, 0);
  EXPECT_EQ(id_pp, goalc_fn_id_for_adapted(goalc_adapter_arg3_pp, (void*)takes_pp, 0));
  u32 fpp = goal.alloc(16) + 4;
  goalc_write_stub(fpp, id_pp, 0);
  EXPECT_EQ(((goalc_fn8)goalc_fn(fpp))(1, 2, 3, 99, 0, 0, 0, 0), 1006u);

  u32 id_stack = goalc_fn_id_for_adapted(goalc_adapter_stack_args, (void*)takes_array, 0);
  EXPECT_NE(id_stack, id_pp);
  u32 fstack = goal.alloc(16) + 4;
  goalc_write_stub(fstack, id_stack, 0);
  EXPECT_EQ(((goalc_fn8)goalc_fn(fstack))(1, 2, 3, 4, 5, 6, 7, 8), 12345678u);

  EXPECT_EQ(goalc_nothing_fn(5, 0, 0, 0, 0, 0, 0, 0), 5u);
  EXPECT_EQ(goalc_zero_fn(5, 0, 0, 0, 0, 0, 0, 0), 0u);
}

// a statically registered module
u64 module_fn0(u64 a0, u64, u64, u64, u64, u64, u64, u64) {
  return a0 + 1;
}
u64 module_fn1(u64 a0, u64, u64, u64, u64, u64, u64, u64) {
  return a0 + 2;
}
void* const g_module_funcs[2] = {(void*)module_fn0, (void*)module_fn1};
const char* const g_module_syms[2] = {"foo", "bar"};
int32_t g_module_sym_offsets[2];
uint32_t g_module_seg_base[3];
const GoalCModule g_module = {0x1122334455667788ull, "test-module", 2, g_module_funcs,
                              2, g_module_syms, g_module_sym_offsets, g_module_seg_base};

s32 resolve_test_symbol(const char* name) {
  return name[0] == 'f' ? 8 : -16;
}

TEST(GoalcRuntime, LinkModule) {
  FakeGoal goal;
  bool was_enabled = g_goalc_enabled;
  goalc_set_enabled(true);
  goalc_register_module(&g_module);
  EXPECT_EQ(goalc_find_module(0x1122334455667788ull), &g_module);

  // a segment with two stubs (the type tag comes first), and its link entry
  u32 seg = goal.alloc(64);
  u32 stub_offsets[2] = {4, 20};
  for (u32 off : stub_offsets) {
    goal.w(seg + off) = 0xffffffff;
  }
  std::vector<u8> link;
  auto put = [&](const void* data, size_t size) {
    link.insert(link.end(), (const u8*)data, (const u8*)data + size);
  };
  u64 hash = g_module.hash;
  u32 n = 2;
  put(&hash, 8);
  put(&n, 4);
  u32 entries[4] = {stub_offsets[0], 1, stub_offsets[1], 0};
  put(entries, sizeof(entries));
  link.push_back(0);  // something after the entry

  u32 seg_bases[3] = {seg, 0, 0x5000};
  u32 consumed = goalc_link_module_entry(link.data(), seg_bases, goal.mem + seg,
                                         resolve_test_symbol, "test-module");
  EXPECT_EQ(consumed, 8u + 4 + 16);
  EXPECT_EQ(g_module_seg_base[0], seg);
  EXPECT_EQ(g_module_seg_base[1], 0u);
  EXPECT_EQ(g_module_seg_base[2], 0x5000u);
  EXPECT_EQ(g_module_sym_offsets[0], 8);
  EXPECT_EQ(g_module_sym_offsets[1], -16);
  EXPECT_EQ(((goalc_fn8)goalc_fn(seg + 4))(10, 0, 0, 0, 0, 0, 0, 0), 12u);   // func 1
  EXPECT_EQ(((goalc_fn8)goalc_fn(seg + 20))(10, 0, 0, 0, 0, 0, 0, 0), 11u);  // func 0

  // linking again (same hash) reuses the ids
  u32 count = goalc_fn_count();
  goalc_link_module_entry(link.data(), seg_bases, goal.mem + seg, resolve_test_symbol,
                          "test-module");
  EXPECT_EQ(goalc_fn_count(), count);
  g_goalc_enabled = was_enabled;
}

}  // namespace

#endif
