/*!
 * @file armbench.cpp
 * (AI-assisted)
 * Bare-metal ARM driver for armbench.py: runs one mips2c function or its native version on a
 * case the differential test harness wrote with --dump (GOAL memory, arguments, symbols), built
 * with devkitARM and the 3DS build's flags, run under Unicorn (ARM1176, VFP in the 3DS's mode).
 * It stands in for run.sh arm and bench where qemu-arm and the arm-linux toolchain are missing
 * (macOS). Only the functions whose GOAL callees are faked below or linked here can run:
 * shadow-execute, moving-sphere-triangle-intersect and collide-do-primitives (their callees are
 * the functions linked here, called through their stubs like the harness does: the mips2c version
 * in the mips2c run, through C mode's adapter, the native one in the native run).
 *
 * Memory (armbench.py maps it): the program at 0x100000, the C stack below 0x0e100000, the job at
 * 0x0f000000, GOAL memory at 0x10000000.
 */

#include "game/kernel/jak1/kscheme.h"
#include "game/mips2c/jak1_functions/native_functions.h"
#include "game/mips2c/mips2c_native.h"
#include "game/mips2c/mips2c_table.h"

u8* g_ee_main_mem = (u8*)0x10000000;
u8* g_ee_main_mem_exec = (u8*)0x10000000;
bool g_goalc_enabled = true;
uint64_t goalc_pp = 0;
uint64_t goalc_st = 0;

//! filled by armbench.py, results written back
struct Job {
  u32 version;  // 0: mips2c, 1: native
  u32 nsyms;
  u64 args[8];
  u64 pp;
  u64 st;
  u32 stack_top;
  u32 status;  // out: 1 done, 2 assert, 3 unknown GOAL call, 4 unknown function, 5 too many
               // stub slots
  u64 v0;      // out
  char function[64];
  struct Sym {
    u32 addr;
    char name[60];
  } syms[1500];
};
static_assert(sizeof(Job::Sym) == 64);
#define JOB ((Job*)0x0f000000)

extern "C" void armbench_done();

[[noreturn]] static void fail(u32 status) {
  JOB->status = status;
  armbench_done();
  __builtin_unreachable();
}

[[noreturn]] void private_assert_failed(const char*, const char*, int, const char*, const char*) {
  fail(2);
}
[[noreturn]] void private_assert_failed(const char*,
                                        const char*,
                                        int,
                                        const char*,
                                        const std::string_view&) {
  fail(2);
}

static u32 sym_lookup(const char* name) {
  for (u32 i = 0; i < JOB->nsyms; i++) {
    if (!strcmp(JOB->syms[i].name, name)) {
      return JOB->syms[i].addr;
    }
  }
  return 0x7f00;  // not in the case: an unused word of the symbol table area
}

static u32 sym_value(const char* name) {
  u32 v;
  memcpy(&v, g_ee_main_mem + sym_lookup(name), 4);
  return v;
}

namespace jak1 {
Ptr<Symbol> intern_from_c(const char* name) {
  return Ptr<Symbol>(sym_lookup(name));
}
}  // namespace jak1

//! the GOAL functions the tests fake (tests_shadow.cpp's)
static bool call_fake(u32 f, u64* v0) {
  if (f == sym_value("flush-cache")) {
    *v0 = 0;
    return true;
  }
  if (f == sym_value("camera-pos")) {
    *v0 = sym_value("*shadow-test-camera*");
    return true;
  }
  return false;
}

namespace Mips2C {
Rng gRng;
LinkedFunctionTable gLinkedFunctionTable;

namespace {
struct Entry {
  const char* name;
  u64 (*exec)(void*);
  u32 stack;
  const NativeImpl* native;
};
Entry g_entries[64];
u32 g_num_entries = 0;
}  // namespace

void LinkedFunctionTable::reg(const std::string& name,
                              u64 (*exec)(void*),
                              u32 goal_stack_size,
                              const NativeImpl* native) {
  static std::string names[64];
  names[g_num_entries] = name;
  g_entries[g_num_entries] = Entry{names[g_num_entries].c_str(), exec, goal_stack_size, native};
  g_num_entries++;
}

u32 LinkedFunctionTable::get(const std::string&) {
  return 0;
}

u64 native_call_goal(u32 fn, const u64 args[8], const NativeArgs& caller) {
  return goalc_call_goal8(fn, args, caller.pp);
}

/*!
 * (AI-assisted) The harness binds the symbol of a function it calls through its stub to that stub
 * (bind_mips2c_symbol): natives call such a function directly, as in the game.
 */
const u32* native_stub_slot(const char* name) {
  static const char* names[32];
  static u32 slots[32];
  static u32 n = 0;
  for (u32 i = 0; i < n; i++) {
    if (!strcmp(names[i], name)) {
      return &slots[i];
    }
  }
  if (n == 32) {
    fail(5);
  }
  names[n] = name;
  slots[n] = sym_value(name);
  return &slots[n++];
}

namespace {
// the GOAL stack below the running function (like the harness's g_sp)
u32 g_sp = 0;

//! the mips2c version, v0 from the context like C mode's adapter (mips2c_goalc_adapter)
u64 run_mips2c(const Entry& e, const u64* args, u64 pp) {
  alignas(16) ExecutionContext ctx;
  memset((void*)&ctx, 0, sizeof(ctx));
  const int regs[8] = {a0, a1, a2, a3, t0, t1, t2, t3};
  for (int i = 0; i < 8; i++) {
    ctx.gprs[regs[i]].du64[0] = args[i];
  }
  ctx.gprs[s6].du64[0] = pp;
  ctx.gprs[s7].du64[0] = JOB->st;
  const u32 sp_ = g_sp;
  ctx.gprs[sp].du64[0] = sp_;
  g_sp -= ((e.stack + 15) & ~15u) + 64;
  e.exec(&ctx);
  g_sp = sp_;
  return ctx.gprs[v0].du64[0];
}

u64 run_native(const Entry& e, const u64* args, u64 pp) {
  if (e.native->goal) {
    // the entry GOAL code calls in C mode (no scratch space)
    goalc_pp = pp;
    return e.native->goal(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7]);
  }
  const u32 stack = g_sp;
  g_sp -= ((e.native->scratch + 15) & ~15u) + 64;
  const u64 r = e.native->fn(NativeArgs{args, pp, JOB->st, stack});
  g_sp = stack;
  return r;
}
}  // namespace
}  // namespace Mips2C

//! calls of GOAL functions: the fakes, or a linked function through its stub (the value of the
//! symbol of the same name)
u64 goalc_call_goal8(u32 f, const u64* args, u64 pp) {
  u64 v0;
  if (call_fake(f, &v0)) {
    return v0;
  }
  for (u32 i = 0; i < Mips2C::g_num_entries; i++) {
    const auto& e = Mips2C::g_entries[i];
    if (sym_lookup(e.name) != 0x7f00 && sym_value(e.name) == f) {
      return JOB->version == 1 && e.native ? Mips2C::run_native(e, args, pp)
                                           : Mips2C::run_mips2c(e, args, pp);
    }
  }
  fail(3);
}

namespace Mips2C::jak1 {
#define LINK(ns)      \
  namespace ns {      \
  extern void link(); \
  }
LINK(shadow_execute)
LINK(shadow_xform_verts)
LINK(shadow_init_vars)
LINK(shadow_calc_dual_verts)
LINK(shadow_scissor_top)
LINK(shadow_scissor_edges)
LINK(shadow_find_facing_single_tris)
LINK(shadow_find_single_edges)
LINK(shadow_find_facing_double_tris)
LINK(shadow_find_double_edges)
LINK(shadow_add_verts)
LINK(shadow_add_facing_single_tris)
LINK(shadow_add_single_edges)
LINK(shadow_add_double_tris)
LINK(shadow_add_double_edges)
LINK(collide_do_primitives)
LINK(moving_sphere_triangle_intersect)
LINK(ray_sphere_intersect)
LINK(ray_cylinder_intersect)
LINK(moving_sphere_sphere_intersect)
#undef LINK
}  // namespace Mips2C::jak1

extern "C" int ctr_mem_asm;  // platform/3ds/port/ctr_mem_c.c: ctr_mem.S's memcpy / memset on

extern "C" void armbench_main() {
  ctr_mem_asm = 1;
  using namespace Mips2C::jak1;
  shadow_execute::link();
  shadow_xform_verts::link();
  shadow_init_vars::link();
  shadow_calc_dual_verts::link();
  shadow_scissor_top::link();
  shadow_scissor_edges::link();
  shadow_find_facing_single_tris::link();
  shadow_find_single_edges::link();
  shadow_find_facing_double_tris::link();
  shadow_find_double_edges::link();
  shadow_add_verts::link();
  shadow_add_facing_single_tris::link();
  shadow_add_single_edges::link();
  shadow_add_double_tris::link();
  shadow_add_double_edges::link();
  // (AI-assisted) the native GOAL collision functions have no other version: their execute
  // function is the native code (as in the game)
  collide_do_primitives::link();
  moving_sphere_triangle_intersect::link();
  ray_sphere_intersect::link();
  ray_cylinder_intersect::link();
  moving_sphere_sphere_intersect::link();
  goalc_st = JOB->st;
  goalc_pp = JOB->pp;

  const Mips2C::Entry* e = nullptr;
  for (u32 i = 0; i < Mips2C::g_num_entries; i++) {
    if (!strcmp(Mips2C::g_entries[i].name, JOB->function)) {
      e = &Mips2C::g_entries[i];
    }
  }
  if (!e) {
    fail(4);
  }
  Mips2C::g_sp = JOB->stack_top;
  JOB->v0 = JOB->version == 0 ? Mips2C::run_mips2c(*e, JOB->args, JOB->pp)
                              : Mips2C::run_native(*e, JOB->args, JOB->pp);
  JOB->status = 1;
}

extern "C" {
extern void (*__init_array_start[])();
extern void (*__init_array_end[])();

__attribute__((noinline, used)) void armbench_done() {
  for (;;) {
    asm volatile("" ::: "memory");
  }
}

__attribute__((naked, used, section(".text.start"))) void _start() {
  asm volatile(
      "ldr sp, =0x0e100000\n"
      "bl armbench_start\n"
      "b armbench_done\n");
}

__attribute__((used)) void armbench_start() {
  for (auto* f = __init_array_start; f != __init_array_end; f++) {
    (*f)();
  }
  armbench_main();
}
}
