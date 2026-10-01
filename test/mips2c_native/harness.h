#pragma once

/*!
 * @file harness.h
 * (AI-assisted)
 * Differential test harness for the native versions of Jak 1 mips2c functions
 * (game/mips2c/mips2c_native.h).
 *
 * The harness links the real mips2c execute functions and the native versions, without the rest
 * of the runtime: GOAL memory, the symbol table, types and the GOAL functions they call are fakes
 * set up by each test. For every generated case it
 *   1. snapshots GOAL memory, the VU0 random generator and the test's host state,
 *   2. runs the mips2c version, records its v0, the calls it made to (fake) GOAL functions, and
 *      the resulting memory,
 *   3. restores the snapshot and runs the native version the same way,
 *   4. compares v0, every byte of GOAL memory outside the stack, the GOAL calls (function and
 *      the arguments the callee uses), the random generator and the host state.
 *
 * Unlike OPENGOAL_MIPS2C_VERIFY (which checks the game's real data), the inputs are generated, so
 * the tests also cover rare branches, and run on the 3DS's CPU architecture under qemu-arm.
 * See run.sh.
 */

#include <cstdio>
#include <functional>
#include <random>
#include <string>
#include <vector>

#include "game/mips2c/mips2c_native.h"

namespace harness {

// ---------------------------------------------------------------------------
// GOAL memory layout
// ---------------------------------------------------------------------------
constexpr u32 kMemSize = 0x180000;
constexpr u32 kSymTable = 0x8000;  // s7 (#f). #t is s7 + 8, other symbols from s7 + 0x100.
constexpr u32 kSymEnd = 0x10000;
constexpr u32 kSpad = 0x10000;  // *fake-scratchpad-data*: 64 kB aligned, like the game's
constexpr u32 kSpadSize = 0x10000;
constexpr u32 kStatic = 0x20000;  // types, function objects: kept across cases
constexpr u32 kStaticEnd = 0x30000;
constexpr u32 kHeap = 0x30000;  // test data, reset for every case
constexpr u32 kHeapEnd = 0x120000;
constexpr u32 kStackBottom = 0x120000;  // [kStackBottom, kStackTop): stacks, not compared
constexpr u32 kStackTop = 0x170000;
constexpr u32 kProcess = 0x170004;  // pp passed to the functions (a fake process)

inline u32 st() {
  return kSymTable;
}
inline u32 true_sym() {
  return kSymTable + 8;
}

//! GOAL address of a symbol (created on first use)
u32 sym(const std::string& name);
void set_sym(const std::string& name, u32 value);
u32 sym_value(const std::string& name);

//! A type object (basic, with n methods) in the static area. Methods are 0 until set.
u32 make_type(const std::string& name, int num_methods);
void set_method(u32 type, int id, u32 fn);

//! Allocate in the per-case heap. align: 16 by default. Basic objects: use alloc_basic.
u32 alloc(u32 size, u32 align = 16);
//! a basic (type tag at addr - 4), the object 16-byte aligned + 4 like GOAL heap objects
u32 alloc_basic(u32 type, u32 size);
//! Bytes left in the per-case heap
u32 heap_left();

// ---------------------------------------------------------------------------
// GOAL functions
// ---------------------------------------------------------------------------
using GoalFn = std::function<u64(const u64* args)>;

/*!
 * A fake GOAL function: calls to its address run fn and are logged with the first `arity`
 * arguments (the ones the callee uses). Returns the address of the function object.
 * If `name` is a symbol, set its value too with set_sym.
 */
u32 add_goal_fn(const std::string& name, int arity, GoalFn fn);

//! the stub (function object) of a registered mips2c function
u32 mips2c_stub(const std::string& name);
//! Bind the symbol of the same name to a registered mips2c function (like def-mips2c)
void bind_mips2c_symbol(const std::string& name);

//! Is the native version running (false: the mips2c version, or setup)?
bool in_native_run();

// ---------------------------------------------------------------------------
// GOAL functions compiled to C by goalc (goalc_ref.cpp, goalc_ref/goalc_ref.h)
// ---------------------------------------------------------------------------
//! Load the fixture modules (once, before linking)
void load_goalc_references();
//! the host function of a GOAL function compiled to C
void* goalc_reference(const std::string& name);
//! Set the symbol of the same name to the compiled GOAL function (like the module's top level)
void bind_goalc_reference(const std::string& name);
//! The reference version of the registered native function `name` (which replaces a GOAL
//! function) is its compiled GOAL code: tests compare the native version with that, and calls of
//! the "mips2c version" from other functions run it.
void use_goalc_reference(const std::string& name);

// ---------------------------------------------------------------------------
// memory helpers (GOAL addresses)
// ---------------------------------------------------------------------------
template <typename T>
inline T ld(u32 addr) {
  return Mips2C::gload<T>(addr);
}
template <typename T>
inline void st(u32 addr, T v) {
  memcpy(g_ee_main_mem + addr, &v, sizeof(T));
}
inline void st_vec(u32 addr, float x, float y, float z, float w) {
  const float v[4] = {x, y, z, w};
  memcpy(g_ee_main_mem + addr, v, 16);
}
inline void st_bytes(u32 addr, const void* src, u32 size) {
  memcpy(g_ee_main_mem + addr, src, size);
}
inline u8* hptr(u32 addr) {
  return g_ee_main_mem + addr;
}

// ---------------------------------------------------------------------------
// random input generation
// ---------------------------------------------------------------------------
struct Gen {
  std::mt19937 rng;
  explicit Gen(u32 seed) : rng(seed) {}
  u32 u32_() { return rng(); }
  //! integer in [lo, hi]
  s32 range(s32 lo, s32 hi) { return lo + (s32)(rng() % (u32)(hi - lo + 1)); }
  bool chance(float p) { return uniform() < p; }
  float uniform() { return (rng() >> 8) * (1.f / 16777216.f); }
  float f(float lo, float hi) { return lo + (hi - lo) * uniform(); }
  //! a float in [lo, hi], sometimes an exact or special value (0, -0, lo, hi, 1, -1, tiny)
  float f_edge(float lo, float hi) {
    if (!chance(0.08f)) {
      return f(lo, hi);
    }
    switch (range(0, 7)) {
      case 0:
        return 0.f;
      case 1:
        return -0.f;
      case 2:
        return lo;
      case 3:
        return hi;
      case 4:
        return 1.f;
      case 5:
        return -1.f;
      case 6:
        return 1e-30f;
      default:
        return -1e-30f;
    }
  }
  template <typename T>
  const T& pick(const std::vector<T>& v) {
    return v[rng() % v.size()];
  }
};

// ---------------------------------------------------------------------------
// tests
// ---------------------------------------------------------------------------
struct Case {
  Gen& g;
  u64 args[8] = {};
};

struct Test {
  std::string name;  //! test name (for filtering)
  std::string fn;    //! the mips2c function it runs (registered name)
  int cases = 2000;
  //! once, after all mips2c functions are linked: fake functions, types, symbols
  std::function<void()> setup;
  //! per case: fill the heap (reset before) and the arguments
  std::function<void(Case&)> gen;
  //! host memory the functions write (compared like GOAL memory), optional
  std::vector<std::pair<void*, size_t>> host_state;
};

void add_test(Test t);
//! A copy of the registered test `name`, with extra setup after the test's and a fraction of its
//! cases
void add_test_variant(const std::string& name,
                      const std::string& new_name,
                      std::function<void()> extra_setup,
                      double case_fraction = 1.0);

struct RunOptions {
  std::string filter;
  double case_scale = 1.0;
  u32 seed = 1;
  bool self_check = false;  //! the second run is the mips2c version again (checks the harness)
  int max_reports = 3;
  //! benchmark: run only this version ("mips2c" or "native") on every case, compare nothing
  std::string bench;
  //! run only this case (-1: all)
  int only_case = -1;
};
//! Runs the selected tests, returns the number that failed
int run_tests(const RunOptions& opt);

//! static (kept across cases) GOAL memory
u32 alloc_static(u32 size, u32 align);

// used by the runtime replacements in harness.cpp
//! Call compiled GOAL code with the host stack at stack_top (GOAL address), unless it already is
//! in GOAL memory
u64 call_goalc(void* fn, const u64* args, u64 pp, u32 stack_top);
//! Make the stub at fn_value callable from compiled GOAL code (it calls dispatch_call)
void write_trampoline(u32 fn_value);
//! Is value a compiled GOAL function (a function value bound by bind_goalc_reference)?
bool goalc_reference_at(u32 value, std::string* name, int* arity, void** host);
void register_mips2c(const std::string& name,
                     u64 (*exec)(void*),
                     u32 stack_size,
                     const Mips2C::NativeImpl* native);
const u32* stub_slot_ptr(const char* name);
u64 dispatch_call(u32 f, const u64* args, u64 pp);

//! Call a GOAL function (fake or mips2c) from a test generator, e.g. to fill a cache first.
u64 call(u32 fn, const u64 args[8]);

}  // namespace harness
