/*!
 * @file harness.cpp
 * (AI-assisted)
 * The fake runtime of the differential tests (see harness.h): GOAL memory, symbols, the mips2c
 * function table, calls to GOAL functions, and the test runner.
 */

#include "harness.h"

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <unordered_map>

#include "game/kernel/jak1/kscheme.h"
#include "game/mips2c/mips2c_table.h"

#include "fmt/format.h"

// ---------------------------------------------------------------------------
// runtime globals the mips2c code uses
// ---------------------------------------------------------------------------
u8* g_ee_main_mem = nullptr;
u8* g_ee_main_mem_exec = nullptr;
bool g_goalc_enabled = true;

namespace {
struct AssertFailed : std::runtime_error {
  using std::runtime_error::runtime_error;
};
}  // namespace

// ASSERT: throw, so the runner can report the case (common/util/Assert.cpp aborts)
[[noreturn]] void private_assert_failed(const char* expr,
                                        const char* file,
                                        int line,
                                        const char* function,
                                        const char* msg) {
  throw AssertFailed(fmt::format("{}:{} in {}: {} {}", file, line, function, expr, msg));
}
[[noreturn]] void private_assert_failed(const char* expr,
                                        const char* file,
                                        int line,
                                        const char* function,
                                        const std::string_view& msg) {
  throw AssertFailed(fmt::format("{}:{} in {}: {} {}", file, line, function, expr, msg));
}

namespace harness {
namespace {

enum class Mode { SETUP, MIPS2C, NATIVE };
Mode g_mode = Mode::SETUP;

struct Mips2cEntry {
  std::string name;
  u64 (*exec)(void*);
  u32 stack_size;
  const Mips2C::NativeImpl* native;
  u32 stub;
};
std::map<std::string, Mips2cEntry> g_mips2c;
std::unordered_map<u32, Mips2cEntry*> g_mips2c_by_stub;
// native_stub_slot values (stable addresses)
std::map<std::string, std::unique_ptr<u32>> g_stub_slots;

struct FakeFn {
  std::string name;
  int arity;
  GoalFn fn;
};
std::unordered_map<u32, FakeFn> g_fakes;

std::map<std::string, u32> g_symbols;
u32 g_next_sym = kSymTable + 0x100;
u32 g_static_top = kStatic;
u32 g_heap_top = kHeap;
u32 g_sp = kStackTop;

struct CallRec {
  std::string fn;
  std::vector<u64> args;
  u64 pp;
};
std::vector<CallRec> g_calls;

u32 static_alloc(u32 size, u32 align) {
  g_static_top = (g_static_top + align - 1) & ~(align - 1);
  const u32 r = g_static_top;
  g_static_top += size;
  if (g_static_top > kStaticEnd) {
    fprintf(stderr, "harness: static area full\n");
    abort();
  }
  return r;
}

u32& stub_slot(const std::string& name) {
  auto& p = g_stub_slots[name];
  if (!p) {
    p = std::make_unique<u32>(0);
  }
  return *p;
}

u64 run_mips2c(Mips2cEntry& e, const u64* args, u64 pp) {
  alignas(16) Mips2C::ExecutionContext ctx;
  memset((void*)&ctx, 0, sizeof(ctx));
  const int regs[8] = {Mips2C::a0, Mips2C::a1, Mips2C::a2, Mips2C::a3,
                       Mips2C::t0, Mips2C::t1, Mips2C::t2, Mips2C::t3};
  for (int i = 0; i < 8; i++) {
    ctx.gprs[regs[i]].du64[0] = args[i];
  }
  ctx.gprs[Mips2C::s6].du64[0] = pp;
  ctx.gprs[Mips2C::s7].du64[0] = kSymTable;
  const u32 sp = g_sp;
  ctx.gprs[Mips2C::sp].du64[0] = sp;
  g_sp -= ((e.stack_size + 15) & ~15u) + 64;
  if (g_sp < kStackBottom) {
    throw AssertFailed("harness: GOAL stack overflow");
  }
  u64 v0;
  try {
    v0 = e.exec(&ctx);
  } catch (...) {
    g_sp = sp;
    throw;
  }
  g_sp = sp;
  return v0;
}

u64 run_native(Mips2cEntry& e, const u64* args, u64 pp) {
  const u32 stack = g_sp;
  g_sp -= ((e.native->scratch + 15) & ~15u) + 64;
  if (g_sp < kStackBottom) {
    throw AssertFailed("harness: GOAL stack overflow");
  }
  u64 v0;
  try {
    v0 = e.native->fn(Mips2C::NativeArgs{args, pp, kSymTable, stack});
  } catch (...) {
    g_sp = stack;
    throw;
  }
  g_sp = stack;
  return v0;
}

u64 dispatch(u32 f, const u64* args, u64 pp) {
  auto m = g_mips2c_by_stub.find(f);
  if (m != g_mips2c_by_stub.end()) {
    auto& e = *m->second;
    if (g_mode == Mode::NATIVE && e.native) {
      return run_native(e, args, pp);
    }
    return run_mips2c(e, args, pp);
  }
  auto it = g_fakes.find(f);
  if (it == g_fakes.end()) {
    throw AssertFailed(fmt::format("harness: call to unknown GOAL function 0x{:x}", f));
  }
  auto& fake = it->second;
  CallRec rec{fake.name, {}, pp};
  for (int i = 0; i < fake.arity; i++) {
    // addresses on the stack differ between the two versions: only their contents matter, and
    // those show up in what the callee returns or stores
    const bool on_stack = args[i] >= kStackBottom && args[i] < kStackTop;
    rec.args.push_back(on_stack ? 0x57ac57ac57ac57acull : args[i]);
  }
  g_calls.push_back(std::move(rec));
  return fake.fn(args);
}

}  // namespace

// ---------------------------------------------------------------------------
// setup API
// ---------------------------------------------------------------------------
u32 sym(const std::string& name) {
  auto it = g_symbols.find(name);
  if (it != g_symbols.end()) {
    return it->second;
  }
  const u32 addr = g_next_sym;
  g_next_sym += 8;
  if (g_next_sym > kSymEnd) {
    fprintf(stderr, "harness: symbol table full\n");
    abort();
  }
  g_symbols[name] = addr;
  st<u32>(addr, 0);
  return addr;
}

void set_sym(const std::string& name, u32 value) {
  st<u32>(sym(name), value);
}

u32 sym_value(const std::string& name) {
  return ld<u32>(sym(name));
}

u32 make_type(const std::string& name, int num_methods) {
  const u32 t = static_alloc(16 + 4 * num_methods + 16, 16) + 4;
  st<u32>(t - 4, 0);                 // type of type: not needed
  st<u32>(t, sym(name));             // symbol
  st<u16>(t + 14, (u16)num_methods);  // allocated-length
  set_sym(name, t);
  return t;
}

void set_method(u32 type, int id, u32 fn) {
  st<u32>(type + 16 + 4 * id, fn);
}

u32 alloc(u32 size, u32 align) {
  g_heap_top = (g_heap_top + align - 1) & ~(align - 1);
  const u32 r = g_heap_top;
  g_heap_top += size;
  if (g_heap_top > kHeapEnd) {
    throw AssertFailed("harness: test heap full");
  }
  return r;
}

u32 alloc_basic(u32 type, u32 size) {
  const u32 r = alloc(size + 4, 16) + 4;
  st<u32>(r - 4, type);
  return r;
}

u32 heap_left() {
  return kHeapEnd - g_heap_top;
}

u32 add_goal_fn(const std::string& name, int arity, GoalFn fn) {
  const u32 addr = static_alloc(16, 16) + 4;
  g_fakes[addr] = FakeFn{name, arity, std::move(fn)};
  return addr;
}

u32 mips2c_stub(const std::string& name) {
  auto it = g_mips2c.find(name);
  if (it == g_mips2c.end()) {
    fprintf(stderr, "harness: mips2c function %s is not linked\n", name.c_str());
    abort();
  }
  return it->second.stub;
}

void bind_mips2c_symbol(const std::string& name) {
  set_sym(name, mips2c_stub(name));
}

bool in_native_run() {
  return g_mode == Mode::NATIVE;
}

u64 call(u32 fn, const u64 args[8]) {
  return dispatch(fn, args, kProcess);
}

u64 dispatch_call(u32 f, const u64* args, u64 pp) {
  return dispatch(f, args, pp);
}

void register_mips2c(const std::string& name,
                     u64 (*exec)(void*),
                     u32 stack_size,
                     const Mips2C::NativeImpl* native) {
  auto& e = g_mips2c[name];
  if (!e.stub) {
    e.stub = static_alloc(16, 16) + 4;
  }
  e.name = name;
  e.exec = exec;
  e.stack_size = stack_size;
  e.native = native;
  g_mips2c_by_stub[e.stub] = &e;
  stub_slot(name) = e.stub;
}

const u32* stub_slot_ptr(const char* name) {
  return &stub_slot(name);
}

// ---------------------------------------------------------------------------
// runner
// ---------------------------------------------------------------------------
namespace {

std::vector<Test>& tests() {
  static std::vector<Test> t;
  return t;
}

const char* g_current_test = "";
int g_current_case = -1;

void on_signal(int sig) {
  fprintf(stderr, "\nharness: signal %d in test %s, case %d (%s run)\n", sig, g_current_test,
          g_current_case,
          g_mode == Mode::NATIVE ? "native" : (g_mode == Mode::MIPS2C ? "mips2c" : "setup"));
  _exit(3);
}

std::string describe16(const u8* p) {
  std::string s;
  for (int i = 0; i < 16; i += 4) {
    u32 u;
    float f;
    memcpy(&u, p + i, 4);
    memcpy(&f, p + i, 4);
    s += fmt::format(" {:08x}({:g})", u, f);
  }
  return s;
}

struct Snapshot {
  std::vector<u8> mem;
  Mips2C::Rng rng;
  std::vector<std::vector<u8>> host;
};

void take(Snapshot& s, const Test& t) {
  s.mem.assign(g_ee_main_mem, g_ee_main_mem + kMemSize);
  s.rng = Mips2C::gRng;
  s.host.resize(t.host_state.size());
  for (size_t i = 0; i < t.host_state.size(); i++) {
    auto [p, n] = t.host_state[i];
    s.host[i].assign((u8*)p, (u8*)p + n);
  }
}

void restore(const Snapshot& s, const Test& t) {
  memcpy(g_ee_main_mem, s.mem.data(), kMemSize);
  Mips2C::gRng = s.rng;
  for (size_t i = 0; i < t.host_state.size(); i++) {
    memcpy(t.host_state[i].first, s.host[i].data(), t.host_state[i].second);
  }
}

bool rng_equal(const Mips2C::Rng& a, const Mips2C::Rng& b) {
  return !memcmp(&a.R, &b.R, 4) && a.extra_random_generator == b.extra_random_generator;
}

struct Result {
  bool threw = false;
  std::string error;
  u64 v0 = 0;
  std::vector<CallRec> calls;
};

Result run_one(Mode mode, Mips2cEntry& e, const u64* args) {
  Result r;
  g_calls.clear();
  g_sp = kStackTop;
  g_mode = mode;
  try {
    r.v0 = mode == Mode::NATIVE ? run_native(e, args, kProcess) : run_mips2c(e, args, kProcess);
  } catch (const AssertFailed& ex) {
    r.threw = true;
    r.error = ex.what();
  }
  g_mode = Mode::SETUP;
  r.calls = std::move(g_calls);
  return r;
}

std::string describe_call(const CallRec& c) {
  std::string s = c.fn + "(";
  for (size_t i = 0; i < c.args.size(); i++) {
    s += fmt::format("{}{:x}", i ? " " : "", c.args[i]);
  }
  return s + ")";
}

}  // namespace

void add_test(Test t) {
  tests().push_back(std::move(t));
}

int run_tests(const RunOptions& opt) {
  signal(SIGSEGV, on_signal);
  signal(SIGBUS, on_signal);
  signal(SIGFPE, on_signal);
  signal(SIGABRT, on_signal);

  // setup of every selected test, then a snapshot of the clean state
  std::vector<Test*> selected;
  for (auto& t : tests()) {
    if (opt.filter.empty() || t.name.find(opt.filter) != std::string::npos) {
      selected.push_back(&t);
    }
  }
  int total_fail = 0;
  for (auto* t : selected) {
    g_current_test = t->name.c_str();
    auto entry_it = g_mips2c.find(t->fn);
    if (entry_it == g_mips2c.end()) {
      printf("%-40s  mips2c function %s not linked\n", t->name.c_str(), t->fn.c_str());
      total_fail++;
      continue;
    }
    auto& e = entry_it->second;
    if (!e.native && !opt.self_check) {
      printf("%-40s  %s has no native version\n", t->name.c_str(), t->fn.c_str());
      total_fail++;
      continue;
    }
    const Mips2C::NativeImpl* saved_native = e.native;
    if (opt.self_check) {
      // the "native" run is the mips2c version again
      e.native = nullptr;
    }

    g_heap_top = kHeap;
    g_current_case = -1;
    if (t->setup) {
      t->setup();
    }
    Snapshot clean, before, after_m;
    take(clean, *t);

    const int n = std::max(1, (int)(t->cases * opt.case_scale));
    const u32 name_hash0 = (u32)std::hash<std::string>()(t->name);
    if (!opt.bench.empty()) {
      // the same cases as the test, one version only, no snapshots
      const Mode mode = opt.bench == "native" ? Mode::NATIVE : Mode::MIPS2C;
      int ran = 0;
      for (int i = 0; i < n; i++) {
        g_current_case = i;
        g_heap_top = kHeap;
        Gen g(name_hash0 ^ (opt.seed * 0x9e3779b9u) ^ (u32)i * 0x85ebca6bu);
        Case c{g};
        try {
          t->gen(c);
        } catch (const AssertFailed&) {
          continue;
        }
        if (!run_one(mode, e, c.args).threw) {
          ran++;
        }
      }
      printf("%-40s  %6d cases run (%s)\n", t->name.c_str(), ran, opt.bench.c_str());
      e.native = saved_native;
      continue;
    }
    int invalid = 0, fails = 0;
    u64 calls_seen = 0;
    const u32 name_hash = (u32)std::hash<std::string>()(t->name);
    for (int i = 0; i < n; i++) {
      g_current_case = i;
      restore(clean, *t);
      g_heap_top = kHeap;
      Gen g(name_hash ^ (opt.seed * 0x9e3779b9u) ^ (u32)i * 0x85ebca6bu);
      Case c{g};
      try {
        t->gen(c);
      } catch (const AssertFailed& ex) {
        invalid++;
        continue;
      }
      take(before, *t);

      Result rm = run_one(Mode::MIPS2C, e, c.args);
      if (rm.threw) {
        // the generator made an input the original can't handle: not a valid case
        if (invalid++ < 3) {
          printf("  [%s case %d] mips2c version failed: %s\n", t->name.c_str(), i,
                 rm.error.c_str());
        }
        continue;
      }
      take(after_m, *t);
      restore(before, *t);
      Result rn = opt.self_check ? run_one(Mode::MIPS2C, e, c.args)
                                 : run_one(Mode::NATIVE, e, c.args);
      calls_seen += rm.calls.size();

      std::string why;
      if (rn.threw) {
        why = "native version failed: " + rn.error;
      } else if (!(e.native && (e.native->flags & Mips2C::NATIVE_V0_UNDEFINED)) &&
                 !(saved_native && (saved_native->flags & Mips2C::NATIVE_V0_UNDEFINED)) &&
                 rm.v0 != rn.v0) {
        why = fmt::format("v0: mips2c {:x} native {:x}", rm.v0, rn.v0);
      } else if (rm.calls.size() != rn.calls.size()) {
        why = fmt::format("{} GOAL calls by mips2c, {} by native", rm.calls.size(),
                          rn.calls.size());
        for (size_t k = 0; k < std::max(rm.calls.size(), rn.calls.size()) && k < 6; k++) {
          why += fmt::format("\n      {:<50} | {}",
                             k < rm.calls.size() ? describe_call(rm.calls[k]) : "-",
                             k < rn.calls.size() ? describe_call(rn.calls[k]) : "-");
        }
      } else {
        for (size_t k = 0; k < rm.calls.size(); k++) {
          if (rm.calls[k].fn != rn.calls[k].fn || rm.calls[k].args != rn.calls[k].args ||
              rm.calls[k].pp != rn.calls[k].pp) {
            why = fmt::format("GOAL call {}: mips2c {} native {}", k, describe_call(rm.calls[k]),
                              describe_call(rn.calls[k]));
            break;
          }
        }
      }
      if (why.empty() && !rng_equal(after_m.rng, Mips2C::gRng)) {
        why = "VU0 random generator state differs";
      }
      if (why.empty()) {
        for (size_t h = 0; h < t->host_state.size(); h++) {
          if (memcmp(after_m.host[h].data(), t->host_state[h].first, t->host_state[h].second)) {
            why = fmt::format("host state {} differs", h);
            break;
          }
        }
      }
      if (why.empty()) {
        // GOAL memory, except the stacks
        int listed = 0;
        u32 ndiff = 0;
        std::string where;
        const bool same =
            !memcmp(after_m.mem.data(), g_ee_main_mem, kStackBottom) &&
            !memcmp(after_m.mem.data() + kStackTop, g_ee_main_mem + kStackTop, kMemSize - kStackTop);
        for (u32 a = 0; !same && a < kMemSize; a += 16) {
          if (a >= kStackBottom && a < kStackTop) {
            continue;
          }
          if (memcmp(after_m.mem.data() + a, g_ee_main_mem + a, 16)) {
            ndiff++;
            if (listed++ < 4) {
              where += fmt::format("\n      0x{:06x} mips2c{}\n               native{}", a,
                                   describe16(after_m.mem.data() + a),
                                   describe16(g_ee_main_mem + a));
            }
          }
        }
        if (ndiff) {
          why = fmt::format("{} quadwords of memory differ:{}", ndiff, where);
        }
      }
      if (!why.empty()) {
        if (fails++ < opt.max_reports) {
          printf("  [%s case %d] MISMATCH: %s\n    args:", t->name.c_str(), i, why.c_str());
          for (auto a : c.args) {
            printf(" %llx", (unsigned long long)a);
          }
          printf("\n");
        }
      }
    }
    e.native = saved_native;
    printf("%-40s  %6d cases  %6d invalid  %8llu GOAL calls  %s\n", t->name.c_str(), n, invalid,
           (unsigned long long)calls_seen, fails ? fmt::format("{} MISMATCHES", fails).c_str()
                                                : "ok");
    if (fails) {
      total_fail++;
    }
    if (invalid * 4 > n) {
      printf("  (more than a quarter of the cases were invalid: fix the generator)\n");
      total_fail++;
    }
  }
  g_current_test = "";
  return total_fail;
}

}  // namespace harness

// ---------------------------------------------------------------------------
// runtime functions the mips2c and native code call
// ---------------------------------------------------------------------------
namespace Mips2C {
Rng gRng;
LinkedFunctionTable gLinkedFunctionTable;
#if MIPS2C_WRITE_LOG
bool g_write_log_on = false;
bool g_native_log_on = false;
void write_log_record(u32, u32) {}
void native_log_record(u32, u32) {}
#endif

void LinkedFunctionTable::reg(const std::string& name,
                              u64 (*exec)(void*),
                              u32 goal_stack_size,
                              const NativeImpl* native) {
  harness::register_mips2c(name, exec, goal_stack_size, native);
}

u32 LinkedFunctionTable::get(const std::string& name) {
  return harness::mips2c_stub(name);
}

u64 native_call_goal(u32 fn, const u64 args[8], const NativeArgs& caller) {
  return goalc_call_goal8(fn, args, caller.pp);
}

const u32* native_stub_slot(const char* name) {
  return harness::stub_slot_ptr(name);
}
}  // namespace Mips2C

u64 goalc_call_goal8(u32 f, const u64* args, u64 pp) {
  return harness::dispatch_call(f, args, pp);
}

// the native backends' trampolines: never used (C mode is on), but referenced by jalr
extern "C" {
#if defined(__aarch64__)
u64 _call_goal8_asm_arm64(void*, u64*, u64, u64, u64, void*, void*) {
  abort();
}
#elif defined(__x86_64__)
u64 _call_goal8_asm_systemv(void*, u64*, u64, u64, u64, void*) {
  abort();
}
#endif
}

namespace jak1 {
Ptr<Symbol> intern_from_c(const char* name) {
  return Ptr<Symbol>(harness::sym(name));
}
}  // namespace jak1
