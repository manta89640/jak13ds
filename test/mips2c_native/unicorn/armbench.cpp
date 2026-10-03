/*!
 * @file armbench.cpp
 * (AI-assisted)
 * Bare-metal ARM driver for armbench.py: runs one mips2c function or its native version on a
 * case the differential test harness wrote with --dump (GOAL memory, arguments, symbols), built
 * with devkitARM and the 3DS build's flags, run under Unicorn (ARM1176, VFP in the 3DS's mode).
 * It stands in for run.sh arm and bench where qemu-arm and the arm-linux toolchain are missing
 * (macOS). Only the functions whose GOAL callees are faked below can run: shadow-execute.
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
  u32 status;  // out: 1 done, 2 assert, 3 unknown GOAL call
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
u64 goalc_call_goal8(u32 f, const u64* args, u64 pp) {
  (void)args;
  (void)pp;
  if (f == sym_value("flush-cache")) {
    return 0;
  }
  if (f == sym_value("camera-pos")) {
    return sym_value("*shadow-test-camera*");
  }
  fail(3);
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

const u32* native_stub_slot(const char*) {
  static u32 zero = 0;
  return &zero;
}
}  // namespace Mips2C

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
  goalc_st = JOB->st;

  const Mips2C::Entry* e = nullptr;
  for (u32 i = 0; i < Mips2C::g_num_entries; i++) {
    if (!strcmp(Mips2C::g_entries[i].name, JOB->function)) {
      e = &Mips2C::g_entries[i];
    }
  }
  if (!e) {
    fail(4);
  }
  if (JOB->version == 0) {
    alignas(16) static Mips2C::ExecutionContext ctx;
    memset((void*)&ctx, 0, sizeof(ctx));
    const int regs[8] = {Mips2C::a0, Mips2C::a1, Mips2C::a2, Mips2C::a3,
                         Mips2C::t0, Mips2C::t1, Mips2C::t2, Mips2C::t3};
    for (int i = 0; i < 8; i++) {
      ctx.gprs[regs[i]].du64[0] = JOB->args[i];
    }
    ctx.gprs[Mips2C::s6].du64[0] = JOB->pp;
    ctx.gprs[Mips2C::s7].du64[0] = JOB->st;
    ctx.gprs[Mips2C::sp].du64[0] = JOB->stack_top;
    JOB->v0 = e->exec(&ctx);
  } else {
    JOB->v0 =
        e->native->fn(Mips2C::NativeArgs{JOB->args, JOB->pp, JOB->st, JOB->stack_top});
  }
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
