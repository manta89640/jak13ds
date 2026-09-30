/*!
 * @file mips2c_native.cpp
 * (AI-assisted)
 * Calling native replacements of mips2c functions, and checking them against the mips2c version
 * (OPENGOAL_MIPS2C_VERIFY=1). See mips2c_native.h.
 *
 * Verify mode (C backend, host builds): every top-level call of a function with a native version
 *   1. runs the mips2c version with every store to GOAL memory logged (address, size, old bytes);
 *      mips2c functions it calls run their mips2c version too,
 *   2. saves the bytes it wrote and v0, then puts the old bytes back,
 *   3. runs the native version on the same arguments, its stores logged too (gstore*); native
 *      functions it calls run native too,
 *   4. compares v0 and the bytes at every address the mips2c version wrote, and checks that the
 *      native version stored nothing anywhere else.
 * The stack below the caller (scratch space of both versions) is not compared. Stores made by GOAL
 * functions that either version calls are not logged; they run the same code in both.
 * Functions that call GOAL code (NATIVE_CALLS_GOAL) are checked on one call in
 * OPENGOAL_MIPS2C_VERIFY_FULL_EVERY (default 8) instead, by saving, restoring and comparing all of
 * GOAL memory (see verify_call_full). The other calls run the native version only.
 */

#include "mips2c_native.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/goal_constants.h"
#include "common/log/log.h"

#include "game/common/ee_mem_write.h"
#include "game/kernel/common/goalc_runtime.h"
#include "game/kernel/common/kperf.h"
#include "game/kernel/jak1/kscheme.h"
#include "game/runtime.h"

extern std::mt19937 extra_random_generator;  // pc-rand (kmachine.cpp)

namespace Mips2C {

u64 mips2c_goalc_adapter(void* fn, u64 stack_size, u64* args);
extern bool g_mips2c_clear_context;

namespace {

struct NativeEntry {
  std::string name;
  std::string perf_name;
  u64 (*exec)(void*);
  u32 stack_size;
  const NativeImpl* impl;
  // verify mode
  u64 calls = 0;
  u64 full_checks = 0;
  u64 bytes_compared = 0;
  u64 mismatches = 0;
  u64 seen = 0;  // calls, including the ones not checked (NATIVE_CALLS_GOAL)
};

std::vector<NativeEntry*> g_entries;
std::unordered_map<std::string, NativeEntry*> g_entries_by_name;
// name -> GOAL address of its stub. Values are stable (unordered_map nodes).
std::unordered_map<std::string, u32> g_stub_addrs;

int g_verify = -1;  // -1: not read yet

bool verify_enabled() {
  if (g_verify < 0) {
#if MIPS2C_WRITE_LOG
    const char* v = getenv("OPENGOAL_MIPS2C_VERIFY");
    g_verify = (v && v[0] && v[0] != '0') ? 1 : 0;
#else
    g_verify = 0;
#endif
  }
  return g_verify == 1;
}

bool natives_disabled() {
#ifdef __3DS__
  return false;
#else
  static int off = -1;
  if (off < 0) {
    const char* v = getenv("OPENGOAL_MIPS2C_NATIVE");
    off = (v && v[0] == '0') ? 1 : 0;
  }
  return off == 1;
#endif
}

//! Call a native function from C mode (the host stack is in GOAL memory), with its scratch space
//! on the stack.
u64 run_native(const NativeImpl* impl, u64* args) {
  u32 stack = 0;
  if (impl->scratch) {
    u8* buf = (u8*)__builtin_alloca(impl->scratch + 16);
    stack = (u32)((((uintptr_t)buf + impl->scratch + 15) & ~uintptr_t(15)) -
                  (uintptr_t)g_ee_main_mem);
  }
  return impl->fn(NativeArgs{args, goalc_pp, goalc_st, stack});
}

u64 native_adapter(void* fn, u64 idx, u64* args) {
  (void)fn;
  auto* e = g_entries[idx];
  kperf::SectionScope perf_section(e->perf_name.c_str());
  return run_native(e->impl, args);
}

#if MIPS2C_WRITE_LOG
// ---------------------------------------------------------------------------
// verify mode
// ---------------------------------------------------------------------------

enum class VerifyState { NONE, IN_MIPS2C, IN_NATIVE };
VerifyState g_state = VerifyState::NONE;

struct LogEntry {
  u32 addr;
  u32 size;
  u32 old_offset;  // into g_log_bytes
};
std::vector<LogEntry> g_log;         // stores of the mips2c version
std::vector<u8> g_log_bytes;         // old bytes of every logged store
std::vector<u8> g_after_bytes;       // bytes after the mips2c run, for every log entry
std::vector<LogEntry> g_native_log;  // stores of the native version (old_offset unused)
std::vector<std::pair<u32, u32>> g_mips2c_ranges;  // merged [start, end) of g_log
u64 g_total_calls = 0;
u64 g_total_mismatches = 0;
u64 g_reports = 0;

// the stack below the caller is scratch space for both versions
constexpr u32 kStackScratch = 256 * 1024;

/*!
 * Bottom of the stack scratch space below sp (the stack of the verified call: not compared).
 * Processes can run with their stack on the fake scratchpad (above its first 16 kB, which is data
 * that collide / particle code writes), so the window stops above that data.
 */
u32 scratch_low(u32 sp) {
  u32 lo = sp > kStackScratch ? sp - kStackScratch : 0;
  if (g_game_version == GameVersion::Jak1) {
    static const u32 spad_sym = ::jak1::intern_from_c("*fake-scratchpad-data*").offset;
    const u32 spad = gload<u32>(spad_sym);
    const u32 spad_data_end = spad + 16 * 1024;
    if (spad < sp && spad_data_end > lo) {
      lo = std::min(spad_data_end, sp);
    }
  }
  return lo;
}

//! " (symbol NAME)" if addr is the value of a symbol (Jak 1)
std::string symbol_at(u32 addr) {
  if (g_game_version != GameVersion::Jak1) {
    return "";
  }
  const u32 sym = addr & ~3u;
  if (sym + 0x10000 < goalc_st || sym > goalc_st + 0x10000 || ((sym - goalc_st) & 7)) {
    return "";
  }
  auto inf = ::jak1::info(Ptr<::jak1::Symbol>(sym));
  if (!inf->str.offset) {
    return "";
  }
  return fmt::format(" (symbol {})", inf->str->data());
}

void print_summary() {
  std::string s;
  for (auto* e : g_entries) {
    if (!e->calls) {
      continue;
    }
    s += fmt::format(" [{}: {} calls, {} KB compared, {} mismatches]", e->name, e->calls,
                     e->bytes_compared / 1024, e->mismatches);
  }
  lg::info("mips2c verify: {} calls, {} mismatches:{}", g_total_calls, g_total_mismatches, s);
}

std::string describe_bytes(const u8* p, u32 n) {
  std::string s;
  for (u32 i = 0; i < n; i += 4) {
    u32 u = 0;
    memcpy(&u, p + i, std::min(4u, n - i));
    float f;
    memcpy(&f, &u, 4);
    s += fmt::format(" {:08x}({})", u, f);
  }
  return s;
}

void report(NativeEntry* e, const u64* args, const std::string& what) {
  e->mismatches++;
  g_total_mismatches++;
  if (g_reports++ < 40) {
    lg::error(
        "mips2c verify MISMATCH in {} (call {}), args {:x} {:x} {:x} {:x} {:x} {:x} {:x} {:x}: {}",
        e->name, e->calls, args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7],
        what);
  }
}

// OPENGOAL_MIPS2C_VERIFY_SELF=1: the second run is the mips2c version again (tests the checker)
bool g_verify_self = false;

u64 second_run(NativeEntry* e, u64* args) {
  if (g_verify_self) {
    return mips2c_goalc_adapter((void*)e->exec, e->stack_size, args);
  }
  return run_native(e->impl, args);
}

u64 verify_call(NativeEntry* e, u64* args_in) {
  u64 args[8];
  memcpy(args, args_in, sizeof(args));
  uintptr_t host_sp;
  GOALC_READ_HOST_SP(host_sp);
  const u32 sp = (u32)(host_sp - (uintptr_t)g_ee_main_mem);
  const u32 scratch_lo = scratch_low(sp);
  auto in_scratch = [&](u32 addr, u32 size) { return addr + size > scratch_lo && addr < sp; };

  e->calls++;
  g_total_calls++;

  // 1. mips2c, with the stores logged
  g_log.clear();
  g_log_bytes.clear();
  g_write_log_on = true;
  g_state = VerifyState::IN_MIPS2C;
  const u64 v0_mips2c = mips2c_goalc_adapter((void*)e->exec, e->stack_size, args);
  g_state = VerifyState::NONE;
  g_write_log_on = false;

  // 2. save what it wrote, put the old bytes back
  g_after_bytes.resize(g_log_bytes.size());
  for (auto& l : g_log) {
    memcpy(g_after_bytes.data() + l.old_offset, g_ee_main_mem + l.addr, l.size);
  }
  for (size_t i = g_log.size(); i-- > 0;) {
    auto& l = g_log[i];
    memcpy(g_ee_main_mem + l.addr, g_log_bytes.data() + l.old_offset, l.size);
  }

  // 3. native, with its stores logged too
  g_native_log.clear();
  g_native_log_on = true;
  g_state = VerifyState::IN_NATIVE;
  const u64 v0_native = second_run(e, args);
  g_state = VerifyState::NONE;
  g_native_log_on = false;

  // 4. compare: v0, the bytes at every address mips2c wrote, and no native store elsewhere
  bool ok = true;
  if (!(e->impl->flags & NATIVE_V0_UNDEFINED) && v0_mips2c != v0_native) {
    report(e, args,
           fmt::format("v0: mips2c {:x} ({}) native {:x} ({})", v0_mips2c, u2f((u32)v0_mips2c),
                       v0_native, u2f((u32)v0_native)));
    ok = false;
  }
  g_mips2c_ranges.clear();
  for (auto& l : g_log) {
    if (in_scratch(l.addr, l.size)) {
      continue;
    }
    g_mips2c_ranges.push_back({l.addr, l.addr + l.size});
    e->bytes_compared += l.size;
    const u8* want = g_after_bytes.data() + l.old_offset;
    if (ok && memcmp(g_ee_main_mem + l.addr, want, l.size)) {
      report(e, args,
             fmt::format("store of {} bytes at 0x{:x}: mips2c{} native{}", l.size, l.addr,
                         describe_bytes(want, l.size),
                         describe_bytes(g_ee_main_mem + l.addr, l.size)));
      ok = false;
    }
  }
  std::sort(g_mips2c_ranges.begin(), g_mips2c_ranges.end());
  size_t merged = 0;
  for (auto& r : g_mips2c_ranges) {
    if (merged && r.first <= g_mips2c_ranges[merged - 1].second) {
      g_mips2c_ranges[merged - 1].second = std::max(g_mips2c_ranges[merged - 1].second, r.second);
    } else {
      g_mips2c_ranges[merged++] = r;
    }
  }
  g_mips2c_ranges.resize(merged);
  for (auto& l : g_native_log) {
    if (!ok) {
      break;
    }
    if (in_scratch(l.addr, l.size)) {
      continue;
    }
    // [addr, addr + size) must be inside one merged range
    auto it = std::upper_bound(g_mips2c_ranges.begin(), g_mips2c_ranges.end(),
                               std::pair<u32, u32>{l.addr, 0xffffffff});
    if (it == g_mips2c_ranges.begin() || (it - 1)->second < l.addr + l.size) {
      report(e, args,
             fmt::format("native stored {} bytes at 0x{:x} where mips2c didn't store:{}", l.size,
                         l.addr, describe_bytes(g_ee_main_mem + l.addr, l.size)));
      ok = false;
    }
  }
  if ((g_total_calls % 4096) == 0) {
    static u64 last_print = 0;
    u64 now = kperf::now_us();
    if (now - last_print > 20000000) {
      last_print = now;
      print_summary();
    }
  }
  return v0_native;
}

// ---------------------------------------------------------------------------
// full-memory verify, for functions that call GOAL code (NATIVE_CALLS_GOAL)
// ---------------------------------------------------------------------------
// GOAL code doesn't log its stores, so all of GOAL memory is saved before the mips2c run, restored
// before the native run, and compared after it. The IOP thread writes into GOAL memory too (DMA,
// RPC results): those writes go through iop_write_ee_mem, which records them while a check is
// running, so they are put back after the restore and left out of the comparison.

std::mutex g_iop_mutex;
std::atomic<bool> g_iop_use_mutex{false};
bool g_iop_track = false;  // guarded by g_iop_mutex
std::vector<LogEntry> g_iop_writes;
std::vector<u8> g_iop_bytes;
u8* g_snap_before = nullptr;
u8* g_snap_after = nullptr;
u64 g_full_every = 8;

u64 verify_call_full(NativeEntry* e, u64* args_in) {
  if (!g_full_every || (e->seen++ % g_full_every) != 0) {
    return run_native(e->impl, args_in);  // not checked this time
  }
  u64 args[8];
  memcpy(args, args_in, sizeof(args));
  uintptr_t host_sp;
  GOALC_READ_HOST_SP(host_sp);
  const u32 sp = (u32)(host_sp - (uintptr_t)g_ee_main_mem);
  const u32 lo = EE_MAIN_MEM_LOW_PROTECT;
  const u32 scratch_lo = scratch_low(sp);
  const u32 scratch_hi = sp + 4096;  // the frames of this function and the adapter
  const u32 end = EE_MAIN_MEM_SIZE;
  e->calls++;
  g_total_calls++;
  if (!g_snap_before) {
    g_snap_before = (u8*)malloc(EE_MAIN_MEM_SIZE);
    g_snap_after = (u8*)malloc(EE_MAIN_MEM_SIZE);
  }

  {
    std::lock_guard<std::mutex> lock(g_iop_mutex);
    g_iop_track = true;
    g_iop_writes.clear();
    g_iop_bytes.clear();
    memcpy(g_snap_before + lo, g_ee_main_mem + lo, end - lo);
  }
  // random generators in host memory: the VU0 one of mips2c code (sp-launch-particles-var...)
  // and pc-rand (rand-vu)
  // (kept off the stack: this may run on a small stack on the fake scratchpad)
  static Rng* rng_before = new Rng();
  static std::mt19937* pc_rand_before = new std::mt19937();
  *rng_before = gRng;
  *pc_rand_before = extra_random_generator;
  g_state = VerifyState::IN_MIPS2C;
  const u64 v0_mips2c = mips2c_goalc_adapter((void*)e->exec, e->stack_size, args);
  g_state = VerifyState::NONE;
  {
    std::lock_guard<std::mutex> lock(g_iop_mutex);
    memcpy(g_snap_after + lo, g_ee_main_mem + lo, end - lo);
    memcpy(g_ee_main_mem + lo, g_snap_before + lo, scratch_lo - lo);
    memcpy(g_ee_main_mem + scratch_hi, g_snap_before + scratch_hi, end - scratch_hi);
    for (auto& w : g_iop_writes) {
      memcpy(g_ee_main_mem + w.addr, g_iop_bytes.data() + w.old_offset, w.size);
    }
  }
  gRng = *rng_before;
  extra_random_generator = *pc_rand_before;
  g_state = VerifyState::IN_NATIVE;
  const u64 v0_native = second_run(e, args);
  g_state = VerifyState::NONE;

  std::lock_guard<std::mutex> lock(g_iop_mutex);
  g_iop_track = false;
  // IOP writes during the check: equal in the snapshot and in memory
  for (auto& w : g_iop_writes) {
    memcpy(g_snap_after + w.addr, g_ee_main_mem + w.addr, w.size);
  }
  if (!(e->impl->flags & NATIVE_V0_UNDEFINED) && v0_mips2c != v0_native) {
    report(e, args,
           fmt::format("v0: mips2c {:x} ({}) native {:x} ({})", v0_mips2c, u2f((u32)v0_mips2c),
                       v0_native, u2f((u32)v0_native)));
    return v0_native;
  }
  for (auto [a, b] : {std::pair<u32, u32>{lo, scratch_lo}, {scratch_hi, end}}) {
    e->bytes_compared += b - a;
    if (!memcmp(g_ee_main_mem + a, g_snap_after + a, b - a)) {
      continue;
    }
    u32 n = 0;
    std::string where;
    int listed = 0;
    for (u32 i = a; i < b; i++) {
      if (g_ee_main_mem[i] == g_snap_after[i]) {
        continue;
      }
      n++;
      if (listed < 6) {
        const u32 q = i & ~15u;
        where += fmt::format(" [0x{:x}{}: mips2c{} native{}]", i, symbol_at(i),
                             describe_bytes(g_snap_after + q, 16),
                             describe_bytes(g_ee_main_mem + q, 16));
        listed++;
        i = q + 15;
      }
    }
    report(e, args, fmt::format("{} bytes of memory differ:{}", n, where));
    break;
  }
  return v0_native;
}

u64 verify_adapter(void* fn, u64 idx, u64* args) {
  (void)fn;
  auto* e = g_entries[idx];
  switch (g_state) {
    case VerifyState::IN_MIPS2C:
      return mips2c_goalc_adapter((void*)e->exec, e->stack_size, args);
    case VerifyState::IN_NATIVE:
      return second_run(e, args);
    default:
      if (e->impl->flags & NATIVE_CALLS_GOAL) {
        return verify_call_full(e, args);
      }
      return verify_call(e, args);
  }
}
#endif

}  // namespace

#if MIPS2C_WRITE_LOG
bool g_write_log_on = false;
bool g_native_log_on = false;

void write_log_record(u32 addr, u32 size) {
  g_log.push_back({addr, size, (u32)g_log_bytes.size()});
  g_log_bytes.insert(g_log_bytes.end(), g_ee_main_mem + addr, g_ee_main_mem + addr + size);
}

void native_log_record(u32 addr, u32 size) {
  g_native_log.push_back({addr, size, 0});
}
#endif

void iop_write_ee_mem_impl(void* dst, const void* src, u32 size) {
#if MIPS2C_WRITE_LOG
  if (g_iop_use_mutex) {
    std::lock_guard<std::mutex> lock(g_iop_mutex);
    memcpy(dst, src, size);
    if (g_iop_track) {
      g_iop_writes.push_back(
          {(u32)((u8*)dst - g_ee_main_mem), size, (u32)g_iop_bytes.size()});
      g_iop_bytes.insert(g_iop_bytes.end(), (const u8*)src, (const u8*)src + size);
    }
    return;
  }
#endif
  memcpy(dst, src, size);
}

/*!
 * Called by LinkedFunctionTable::reg for a function with a native version, in C mode. Returns the
 * function id GOAL calls (0: use the mips2c version).
 */
u32 native_goalc_fn_id(const std::string& name,
                       u64 (*exec)(void*),
                       u32 stack_size,
                       const NativeImpl* impl) {
  if (natives_disabled()) {
    return 0;
  }
  NativeEntry* e;
  auto it = g_entries_by_name.find(name);
  if (it != g_entries_by_name.end()) {
    e = it->second;
  } else {
    e = new NativeEntry();
    e->name = name;
    e->perf_name = "m2c:" + name;
    g_entries_by_name[name] = e;
    g_entries.push_back(e);
  }
  e->exec = exec;
  e->stack_size = stack_size;
  e->impl = impl;
  u64 idx = 0;
  while (g_entries[idx] != e) {
    idx++;
  }
#if MIPS2C_WRITE_LOG
  if (verify_enabled()) {
    static bool announced = false;
    if (!announced) {
      announced = true;
      g_mips2c_clear_context = true;
      g_iop_use_mutex = true;
      if (const char* v = getenv("OPENGOAL_MIPS2C_VERIFY_SELF")) {
        g_verify_self = v[0] == '1';
      }
      if (const char* f = getenv("OPENGOAL_MIPS2C_VERIFY_FULL_EVERY")) {
        g_full_every = strtoull(f, nullptr, 10);
      }
      atexit([] { print_summary(); });
      lg::warn("mips2c verify mode: native functions are checked against mips2c");
    }
    return goalc_fn_id_for_adapted(verify_adapter, (void*)impl->fn, idx);
  }
#endif
  return goalc_fn_id_for_adapted(native_adapter, (void*)impl->fn, idx);
}

u64 (*native_exec_for_backend(const NativeImpl* impl))(void*) {
  return natives_disabled() ? nullptr : impl->as_exec;
}

void native_set_stub_addr(const std::string& name, u32 addr) {
  g_stub_addrs[name] = addr;
}

const u32* native_stub_slot(const char* name) {
  return &g_stub_addrs[name];
}

u64 native_call_goal(u32 fn, const u64 args[8], const NativeArgs& caller) {
  ExecutionContext ctx;
  const int regs[8] = {a0, a1, a2, a3, t0, t1, t2, t3};
  for (int i = 0; i < 8; i++) {
    ctx.gprs[regs[i]].du64[0] = args[i];
  }
  ctx.gprs[s6].du64[0] = caller.pp;
  ctx.gprs[s7].du64[0] = caller.st;
  ctx.jalr(fn);
  return ctx.gprs[v0].du64[0];
}

void native_print_verify_summary() {
#if MIPS2C_WRITE_LOG
  if (verify_enabled()) {
    print_summary();
  }
#endif
}

}  // namespace Mips2C

void iop_write_ee_mem(void* dst, const void* src, u32 size) {
  Mips2C::iop_write_ee_mem_impl(dst, src, size);
}
