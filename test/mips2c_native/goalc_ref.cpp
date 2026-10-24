/*!
 * @file goalc_ref.cpp
 * (AI-assisted)
 * GOAL functions compiled to C by goalc as reference versions (see goalc_ref/goalc_ref.h): loads
 * the fixture modules into GOAL memory like the linker does, and runs their functions on a stack
 * in GOAL memory (compiled GOAL code passes stack addresses to other functions as GOAL
 * addresses, so the host stack must be in GOAL memory, like on the 3DS).
 *
 * Calls from that code to functions of the harness (fakes, mips2c/native functions) go through
 * trampolines: every harness function stub holds the host address of a trampoline (goal_c_abi.h)
 * that calls the harness's dispatch.
 */

#include "goalc_ref/goalc_ref.h"

#include <array>
#include <exception>
#include <map>
#include <stdexcept>
#include <utility>

#include "harness.h"

#include "game/kernel/common/goalc_runtime.h"

// the runtime state the generated code uses (goal_c_abi.h)
extern "C" {
uint8_t* goalc_mem = nullptr;
uint64_t goalc_st = 0;
uint64_t goalc_pp = 0;
void** goalc_fn_table = nullptr;

uint64_t goalc_get_sp(void) {
  u8 here;
  return (u64)(u32)(&here - goalc_mem);
}

void goalc_break(void) {
  throw std::runtime_error("goalc_break");
}

#ifndef GOALC_REF_NONE
extern const GoalcRefModule goalc_ref_collide_func;
extern const GoalcRefModule goalc_ref_geometry;
extern const GoalcRefModule goalc_ref_vector;
#endif
extern const GoalcRefModule goalc_ref_joint;
extern const GoalcRefModule goalc_ref_quaternion;
extern const GoalcRefModule goalc_ref_bones;  // (AI-assisted)
}

// referenced by goalc_context.cpp (the kernel's suspend), never called here
u64 goalc_suspend_impl(goalc_ctx*) {
  abort();
}

namespace harness {
namespace {

struct RefFn {
  std::string name;
  u32 value;  // GOAL function value
  int arity;
  void* host;
};
std::map<std::string, RefFn> g_ref_fns;
std::map<u32, RefFn*> g_ref_by_value;

// trampolines: GOAL function value -> harness dispatch
constexpr int kMaxTrampolines = 512;
u32 g_tramp_target[kMaxTrampolines];
int g_tramp_count = 0;

// (AI-assisted) the calling convention of GOALC_ABI_VERSION >= 3 (goal_c_abi.h): the low halves
// of the arguments in a_i, the upper halves in h_i (garbage for arguments the caller doesn't pass
// wide)
template <int I>
u64 trampoline(u32 a0,
               u32 a1,
               u32 a2,
               u32 a3,
               u32 a4,
               u32 a5,
               u32 a6,
               u32 a7,
               float h0,
               float h1,
               float h2,
               float h3,
               float h4,
               float h5,
               float h6,
               float h7) {
  const u64 args[8] = {goalc_arg64(a0, h0), goalc_arg64(a1, h1), goalc_arg64(a2, h2),
                       goalc_arg64(a3, h3), goalc_arg64(a4, h4), goalc_arg64(a5, h5),
                       goalc_arg64(a6, h6), goalc_arg64(a7, h7)};
  return dispatch_call(g_tramp_target[I], args, goalc_pp);
}

template <int... Is>
constexpr std::array<goalc_fnx, sizeof...(Is)> make_trampolines(std::integer_sequence<int, Is...>) {
  return {&trampoline<Is>...};
}
const auto g_trampolines = make_trampolines(std::make_integer_sequence<int, kMaxTrampolines>{});

struct RefCall {
  void* fn;
  const u64* args;
  std::exception_ptr error;
};

u64 ref_entry(void* p) {
  auto* c = (RefCall*)p;
  try {
    const u64* a = c->args;
    return ((goalc_fnx)c->fn)((u32)a[0], (u32)a[1], (u32)a[2], (u32)a[3], (u32)a[4], (u32)a[5],
                              (u32)a[6], (u32)a[7], goalc_hi_bits(a[0]), goalc_hi_bits(a[1]),
                              goalc_hi_bits(a[2]), goalc_hi_bits(a[3]), goalc_hi_bits(a[4]),
                              goalc_hi_bits(a[5]), goalc_hi_bits(a[6]), goalc_hi_bits(a[7]));
  } catch (...) {
    // can't unwind through goalc_call_on_stack
    c->error = std::current_exception();
    return 0;
  }
}

bool g_on_goal_stack = false;

}  // namespace

void write_trampoline(u32 fn_value) {
  if (g_tramp_count == kMaxTrampolines) {
    fprintf(stderr, "harness: out of trampolines\n");
    abort();
  }
  g_tramp_target[g_tramp_count] = fn_value;
  void* host = (void*)g_trampolines[g_tramp_count++];
  memcpy(hptr(fn_value), &host, sizeof(void*));
}

void load_goalc_references() {
  goalc_mem = g_ee_main_mem;
  goalc_st = kSymTable;
  goalc_pp = kProcess;
#ifdef GOALC_REF_NONE
  // (AI-assisted) built without the old fixtures (run.sh with MIPS2C_TEST_NO_GOALC_REF=1: they
  // are for another GOALC_ABI_VERSION): the tests against them fail, the others run
  const std::initializer_list<const GoalcRefModule*> modules = {
      &goalc_ref_joint, &goalc_ref_quaternion, &goalc_ref_bones};
#else
  const std::initializer_list<const GoalcRefModule*> modules = {
      &goalc_ref_collide_func, &goalc_ref_geometry, &goalc_ref_vector, &goalc_ref_joint,
      &goalc_ref_quaternion, &goalc_ref_bones};
#endif
  for (const GoalcRefModule* m : modules) {
    const u32 seg = alloc_static(m->seg0_size, 16);
    st_bytes(seg, m->seg0, m->seg0_size);
    m->seg_base[0] = seg;
    m->seg_base[1] = 0;  // the debug and top level segments are not loaded
    m->seg_base[2] = 0;
    for (u32 k = 0; k < m->n_syms; k++) {
      m->sym_offsets[k] = (s32)(sym(m->sym_names[k]) - kSymTable);
    }
    if (m->after_link) {
      m->after_link();  // (AI-assisted) ABI 6 fixtures
    }
    for (u32 i = 0; i < m->n_funcs; i++) {
      const auto& f = m->funcs[i];
      const u32 value = seg + f.value;
      memcpy(hptr(value), &f.fn, sizeof(void*));
      g_ref_fns[f.name] = RefFn{f.name, value, (int)f.arity, f.fn};
    }
  }
  for (auto& [name, f] : g_ref_fns) {
    g_ref_by_value[f.value] = &f;
  }
}

bool goalc_reference_at(u32 value, std::string* name, int* arity, void** host) {
  auto it = g_ref_by_value.find(value);
  if (it == g_ref_by_value.end()) {
    return false;
  }
  *name = it->second->name;
  *arity = it->second->arity;
  *host = it->second->host;
  return true;
}

void* goalc_reference(const std::string& name) {
  auto it = g_ref_fns.find(name);
  if (it == g_ref_fns.end()) {
    // (AI-assisted) e.g. a fixture left out (MIPS2C_TEST_NO_GOALC_REF): the runner skips a test
    // whose setup needs it
    throw NoGoalcReference("no goalc reference for " + name);
  }
  return it->second.host;
}

void bind_goalc_reference(const std::string& name) {
  goalc_reference(name);
  set_sym(name, g_ref_fns[name].value);
}

u64 call_goalc(void* fn, const u64* args, u64 pp, u32 stack_top) {
  RefCall c{fn, args, nullptr};
  const u64 saved_pp = goalc_pp;
  goalc_pp = pp;
  u64 v0;
  if (g_on_goal_stack) {
    // called (through a trampoline) by code already running on the GOAL stack
    v0 = ref_entry(&c);
  } else {
    g_on_goal_stack = true;
    v0 = goalc_call_on_stack(hptr(stack_top), ref_entry, &c);
    g_on_goal_stack = false;
  }
  goalc_pp = saved_pp;
  if (c.error) {
    std::rethrow_exception(c.error);
  }
  return v0;
}

}  // namespace harness
