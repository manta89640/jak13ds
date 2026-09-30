/*!
 * @file goalc_runtime.cpp
 * Runtime state, function ids, module registry and GOAL calls for GOAL code compiled to C.
 * See docs/3ds-port/c_backend.md.
 */

#include "goalc_runtime.h"

#include <array>
#include <csignal>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/goal_constants.h"
#include "common/link_types.h"
#include "common/log/log.h"
#include "common/util/Assert.h"
#include "common/util/FileUtil.h"
#include "common/versions/versions.h"

#include "game/kernel/common/memory_layout.h"
#include "game/runtime.h"

#include "fmt/format.h"

#if defined(__3DS__)
#include "platform/3ds/port/ctr_port.h"
#endif

#if !defined(_WIN32) && !defined(__3DS__) && !defined(GOALC_NO_DLOPEN)
#define GOALC_USE_DLOPEN 1
#include <dlfcn.h>
#else
#define GOALC_USE_DLOPEN 0
#endif

// ---------------------------------------------------------------------------
// State shared with generated code (goal_c_abi.h)
// ---------------------------------------------------------------------------

extern "C" {
uint8_t* goalc_mem = nullptr;
uint64_t goalc_st = 0;
uint64_t goalc_pp = 0;
void** goalc_fn_table = nullptr;
}

bool g_goalc_enabled = false;

namespace {

// ---------------------------------------------------------------------------
// Function table
// ---------------------------------------------------------------------------

u64 unlinked_function(u64, u64, u64, u64, u64, u64, u64, u64) {
  lg::error("[goalc] called a GOAL function whose stub was never linked (function id 0)");
  goalc_break();
  return 0;
}

struct FnTable {
  // The published table. When it grows, old arrays are kept alive (never freed) so a thread that
  // is reading goalc_fn_table concurrently (the renderer's VIF interrupt callback) stays safe.
  std::vector<void**> arrays;
  u32 size = 0;
  u32 capacity = 0;
  std::unordered_map<void*, u32> host_ids;

  void reset() {
    size = 0;
    host_ids.clear();
    if (arrays.empty()) {
      grow(1024);
    }
    push((void*)unlinked_function);
  }

  void grow(u32 min_capacity) {
    u32 new_cap = capacity ? capacity : 1024;
    while (new_cap < min_capacity) {
      new_cap *= 2;
    }
    auto** fresh = new void*[new_cap];
    for (u32 i = 0; i < new_cap; i++) {
      fresh[i] = (void*)unlinked_function;
    }
    if (capacity) {
      memcpy(fresh, goalc_fn_table, sizeof(void*) * size);
    }
    arrays.push_back(fresh);
    capacity = new_cap;
    goalc_fn_table = fresh;
  }

  u32 push(void* fn) {
    if (size == capacity) {
      grow(capacity + 1);
    }
    goalc_fn_table[size] = fn;
    return size++;
  }
};

FnTable g_fn_table;

// ---------------------------------------------------------------------------
// Adapter thunks
// ---------------------------------------------------------------------------
// Host functions that are not callable as goalc_fn8 (arg3_is_pp kernel functions, stack-argument
// kernel functions, mips2c functions) need a distinct goalc_fn8 entry point that knows which
// function to call. There is no runtime code generation, so we use a fixed pool of thunks.

constexpr u32 kMaxThunks = 2048;

struct ThunkSlot {
  goalc_adapter adapter = nullptr;
  void* fn = nullptr;
  u64 extra = 0;
  u32 id = 0;
};

ThunkSlot g_thunk_slots[kMaxThunks];
u32 g_thunk_count = 0;

template <u32 I>
u64 thunk(u64 a0, u64 a1, u64 a2, u64 a3, u64 a4, u64 a5, u64 a6, u64 a7) {
  u64 args[8] = {a0, a1, a2, a3, a4, a5, a6, a7};
  const ThunkSlot& slot = g_thunk_slots[I];
  return slot.adapter(slot.fn, slot.extra, args);
}

template <u32... Is>
constexpr std::array<goalc_fn8, sizeof...(Is)> make_thunks(std::integer_sequence<u32, Is...>) {
  return {&thunk<Is>...};
}

const auto g_thunks = make_thunks(std::make_integer_sequence<u32, kMaxThunks>{});

// ---------------------------------------------------------------------------
// Modules
// ---------------------------------------------------------------------------

std::unordered_map<u64, const GoalCModule*>& static_registry() {
  static std::unordered_map<u64, const GoalCModule*> registry;
  return registry;
}

struct LoadedModule {
  const GoalCModule* mod = nullptr;
  u32 base_id = 0;
};

// modules that have been linked at least once since the last goalc_runtime_init
std::unordered_map<u64, LoadedModule> g_linked_modules;

// ---------------------------------------------------------------------------
// Stacks
// ---------------------------------------------------------------------------

std::thread::id g_ee_thread_id;
u8* g_foreign_stack_bottom = nullptr;
u32 g_foreign_stack_size = 0;
std::mutex g_foreign_mutex;

template <typename T>
T read_unaligned(const u8* p) {
  T result;
  memcpy(&result, p, sizeof(T));
  return result;
}

struct PendingCall {
  u32 f;
  u64 args[8];
};

u64 call_pending_entry(void* arg) {
  auto* call = (PendingCall*)arg;
  auto fn = (goalc_fn8)goalc_fn(call->f);
  return fn(call->args[0], call->args[1], call->args[2], call->args[3], call->args[4],
            call->args[5], call->args[6], call->args[7]);
}

}  // namespace

// ---------------------------------------------------------------------------
// C API from goal_c_abi.h
// ---------------------------------------------------------------------------

extern "C" {

uint64_t goalc_get_sp(void) {
  uintptr_t sp;
  GOALC_READ_HOST_SP(sp);
  return (uint64_t)(sp - (uintptr_t)goalc_mem);
}

void goalc_break(void) {
  lg::error("[goalc] (break) at pp #x{:x}", goalc_pp);
#if defined(__3DS__)
  char detail[64];
  snprintf(detail, sizeof(detail), "GOAL (break), pp #x%x", (unsigned)goalc_pp);
  ctr_crash("The game stopped (GOAL break)", detail);
#endif
#if defined(SIGTRAP)
  raise(SIGTRAP);
#else
  abort();
#endif
}

/*!
 * Registers statically linked modules. The real one is generated by scripts/3ds/gen_c_registry.py
 * and overrides this empty weak definition in static builds.
 */
__attribute__((weak)) void goalc_register_static_modules(void) {}

void goalc_register_module(const GoalCModule* mod) {
  auto& reg = static_registry();
  auto it = reg.find(mod->hash);
  if (it != reg.end() && it->second != mod) {
    // can't use lg here, this may run in a static constructor.
    fprintf(stderr, "[goalc] module %s has the same hash as %s (%016llx)\n", mod->name,
            it->second->name, (unsigned long long)mod->hash);
  }
  reg[mod->hash] = mod;
}
}

// ---------------------------------------------------------------------------
// Mode and init
// ---------------------------------------------------------------------------

void goalc_set_enabled(bool enable) {
  if (!GOALC_HAS_NATIVE_BACKEND && !enable) {
    lg::warn("[goalc] this host has no native GOAL backend, staying in C mode");
  }
  g_goalc_enabled = enable;
}

void goalc_runtime_init(u8* mem, u32 st) {
  goalc_mem = mem;
  goalc_st = st;
  goalc_pp = st;
  g_fn_table.reset();
  for (auto& slot : g_thunk_slots) {
    slot = ThunkSlot();
  }
  g_thunk_count = 0;
  g_linked_modules.clear();
  g_ee_thread_id = std::this_thread::get_id();
  goalc_register_static_modules();
}

void goalc_set_foreign_stack(u8* bottom, u32 size) {
  g_foreign_stack_bottom = bottom;
  g_foreign_stack_size = size;
}

// ---------------------------------------------------------------------------
// Function ids
// ---------------------------------------------------------------------------

u32 goalc_fn_id_for_host(void* fn) {
  auto it = g_fn_table.host_ids.find(fn);
  if (it != g_fn_table.host_ids.end()) {
    return it->second;
  }
  u32 id = g_fn_table.push(fn);
  g_fn_table.host_ids[fn] = id;
  return id;
}

u32 goalc_fn_id_for_adapted(goalc_adapter adapter, void* fn, u64 extra) {
  for (u32 i = 0; i < g_thunk_count; i++) {
    auto& slot = g_thunk_slots[i];
    if (slot.adapter == adapter && slot.fn == fn && slot.extra == extra) {
      return slot.id;
    }
  }
  ASSERT_MSG(g_thunk_count < kMaxThunks, "goalc: out of adapter thunks, increase kMaxThunks");
  auto& slot = g_thunk_slots[g_thunk_count];
  slot.adapter = adapter;
  slot.fn = fn;
  slot.extra = extra;
  slot.id = g_fn_table.push((void*)g_thunks[g_thunk_count]);
  g_thunk_count++;
  return slot.id;
}

u32 goalc_alloc_fn_ids(void* const* fns, u32 n) {
  u32 base = g_fn_table.size;
  for (u32 i = 0; i < n; i++) {
    g_fn_table.push(fns[i]);
  }
  return base;
}

u32 goalc_fn_count() {
  return g_fn_table.size;
}

void goalc_write_stub(u32 goal_addr, u32 id, u32 index) {
  // see goal_c_abi.h: the stub holds the host address of the function
  ASSERT(id < g_fn_table.size);
  memcpy(goalc_mem + goal_addr + 4, &index, 4);
  void* fn = goalc_fn_table[id];
  memcpy(goalc_mem + goal_addr, &fn, sizeof(void*));
}

u64 goalc_adapter_arg3_pp(void* fn, u64, u64* args) {
  // the native stub moves pp into the fourth argument register and leaves the rest alone.
  return ((goalc_fn8)fn)(args[0], args[1], args[2], goalc_pp, args[4], args[5], args[6], args[7]);
}

u64 goalc_adapter_stack_args(void* fn, u64, u64* args) {
  // like the native _stack_call trampolines: a pointer to the 8 arguments, on the stack.
  return ((u64(*)(u64*))fn)(args);
}

u64 goalc_nothing_fn(u64 a0, u64, u64, u64, u64, u64, u64, u64) {
  // the native `nothing` is just a return, which leaves the first argument in the return register
  // on arm64.
  return a0;
}

u64 goalc_zero_fn(u64, u64, u64, u64, u64, u64, u64, u64) {
  return 0;
}

// ---------------------------------------------------------------------------
// Modules
// ---------------------------------------------------------------------------

const GoalCModule* goalc_find_module(u64 hash) {
  auto& reg = static_registry();
  auto it = reg.find(hash);
  if (it != reg.end()) {
    return it->second;
  }

#if GOALC_USE_DLOPEN
  auto path = file_util::get_jak_project_dir() / "out" / game_version_names[g_game_version] /
              "cmod" / fmt::format("{:016x}.so", hash);
  void* handle = dlopen(path.string().c_str(), RTLD_NOW | RTLD_LOCAL);
  if (!handle) {
    lg::error("[goalc] failed to load module {}: {}", path.string(), dlerror());
    return nullptr;
  }
  auto desc = (goalc_module_desc_fn)dlsym(handle, GOALC_MODULE_DESC_FUNC);
  if (!desc) {
    lg::error("[goalc] module {} has no {}", path.string(), GOALC_MODULE_DESC_FUNC);
    dlclose(handle);
    return nullptr;
  }
  const GoalCModule* mod = desc();
  if (!mod || mod->hash != hash) {
    lg::error("[goalc] module {} has the wrong hash", path.string());
    dlclose(handle);
    return nullptr;
  }
  // keep the library loaded forever, and remember it so we don't dlopen again.
  reg[hash] = mod;
  return mod;
#else
  return nullptr;
#endif
}

u32 goalc_link_module_entry(const u8* link,
                            const u32 seg_bases[3],
                            u8* seg_data,
                            goalc_sym_resolver resolver,
                            const char* object_name) {
  const u8* start = link;
  u64 hash = read_unaligned<u64>(link);
  link += 8;
  u32 n = read_unaligned<u32>(link);
  link += 4;

  if (!goalc_enabled()) {
    ASSERT_MSG(false, fmt::format("Object file {} was compiled with the C backend, but the runtime "
                                  "is not in C mode. Run gk with the -cbackend argument.",
                                  object_name));
  }

  const GoalCModule* mod = goalc_find_module(hash);
  if (!mod) {
    ASSERT_MSG(false, fmt::format("goalc: could not find C module {:016x} for object file {}",
                                  hash, object_name));
  }

  for (int i = 0; i < 3; i++) {
    mod->seg_base[i] = seg_bases[i];
  }

  for (u32 i = 0; i < mod->n_syms; i++) {
    mod->sym_offsets[i] = resolver(mod->sym_names[i]);
  }

  auto it = g_linked_modules.find(hash);
  if (it == g_linked_modules.end() || it->second.mod != mod) {
    LoadedModule lm;
    lm.mod = mod;
    lm.base_id = goalc_alloc_fn_ids(mod->funcs, mod->n_funcs);
    it = g_linked_modules.insert_or_assign(hash, lm).first;
  }
  u32 base_id = it->second.base_id;

  for (u32 i = 0; i < n; i++) {
    u32 stub_offset = read_unaligned<u32>(link);
    u32 func_index = read_unaligned<u32>(link + 4);
    link += 8;
    ASSERT_MSG(func_index < mod->n_funcs,
               fmt::format("goalc: object {} references function {} but module {} has {}",
                           object_name, func_index, mod->name ? mod->name : "?", mod->n_funcs));
    // see goal_c_abi.h: the stub holds the host address of the function
    void* fn = goalc_fn_table[base_id + func_index];
    memcpy(seg_data + stub_offset, &fn, sizeof(void*));
  }

  return (u32)(link - start);
}

// ---------------------------------------------------------------------------
// Calling GOAL
// ---------------------------------------------------------------------------

bool goalc_on_goal_stack() {
  uintptr_t sp;
  GOALC_READ_HOST_SP(sp);
  return sp > (uintptr_t)goalc_mem && sp <= (uintptr_t)goalc_mem + EE_MAIN_MEM_SIZE;
}

namespace {
u8* default_goal_stack_top() {
  // same stack as KernelCheckAndDispatch uses for the kernel.
  return goalc_mem + GOAL_KERNEL_STACK_TOP - 16;
}

u64 call_on_foreign_thread(u32 f, const u64* args) {
  // Another host thread (the renderer's fake VIF interrupts) calls GOAL. Use a dedicated stack in
  // GOAL memory and leave goalc_pp alone, since the EE thread owns it.
  std::lock_guard<std::mutex> lock(g_foreign_mutex);
  PendingCall call;
  call.f = f;
  memcpy(call.args, args, sizeof(call.args));
  if (!g_foreign_stack_bottom) {
    lg::warn("[goalc] GOAL called from a foreign thread without a foreign stack");
    return call_pending_entry(&call);
  }
  return goalc_call_on_stack(g_foreign_stack_bottom + g_foreign_stack_size, call_pending_entry,
                             &call);
}
}  // namespace

u64 goalc_call_goal(u32 f, u64 a, u64 b, u64 c, u64 st) {
  u64 args[8] = {a, b, c, 0, 0, 0, 0, 0};
  if (std::this_thread::get_id() != g_ee_thread_id) {
    return call_on_foreign_thread(f, args);
  }

  goalc_st = st;
  // like the native trampoline: pp is set to st (#f) for the call, and restored after.
  u64 saved_pp = goalc_pp;
  goalc_pp = st;
  u64 result;
  if (goalc_on_goal_stack()) {
    result = ((goalc_fn8)goalc_fn(f))(a, b, c, 0, 0, 0, 0, 0);
  } else {
    // called from plain C (for example during boot). Compiled code must run on a stack in GOAL
    // memory, so switch to the kernel stack.
    PendingCall call;
    call.f = f;
    memcpy(call.args, args, sizeof(args));
    result = goalc_call_on_stack(default_goal_stack_top(), call_pending_entry, &call);
  }
  goalc_pp = saved_pp;
  return result;
}

u64 goalc_call_goal_on_stack(u32 f, u8* stack_top, u64 st) {
  ASSERT(stack_top > goalc_mem && stack_top <= goalc_mem + EE_MAIN_MEM_SIZE);
  goalc_st = st;
  u64 saved_pp = goalc_pp;
  goalc_pp = st;
  PendingCall call;
  call.f = f;
  memset(call.args, 0, sizeof(call.args));
  u64 result = goalc_call_on_stack(stack_top, call_pending_entry, &call);
  goalc_pp = saved_pp;
  return result;
}

u64 goalc_call_goal8(u32 f, const u64* args, u64 pp) {
  // mips2c code calling GOAL. mips2c runs on the GOAL stack already.
  u64 saved_pp = goalc_pp;
  goalc_pp = pp;
  u64 result = ((goalc_fn8)goalc_fn(f))(args[0], args[1], args[2], args[3], args[4], args[5],
                                         args[6], args[7]);
  goalc_pp = saved_pp;
  return result;
}

// ---------------------------------------------------------------------------
// Debugging
// ---------------------------------------------------------------------------

void goalc_print_backtrace(const goalc_ctx* ctx) {
#if defined(__aarch64__)
  uintptr_t fp = ctx->regs[10];
  uintptr_t pc = ctx->regs[11];
#elif defined(__x86_64__)
  uintptr_t fp = ctx->regs[1];
  uintptr_t pc = ctx->regs[7];
#else
  // ARM32 frame records aren't standardized, don't try.
  uintptr_t fp = 0;
  uintptr_t pc = 0;
  (void)ctx;
#endif
  for (int depth = 0; depth < 64 && pc; depth++) {
    std::string where = "?";
#if GOALC_USE_DLOPEN
    Dl_info info;
    if (dladdr((void*)pc, &info) && info.dli_fname) {
      where = fmt::format("{} +0x{:x} ({})", info.dli_fname, pc - (uintptr_t)info.dli_fbase,
                          info.dli_sname ? info.dli_sname : "?");
    }
#endif
    lg::error("  [{}] pc 0x{:x} sp/fp #x{:x}: {}", depth, pc, fp - (uintptr_t)goalc_mem, where);
    if (!fp || fp < (uintptr_t)goalc_mem || fp >= (uintptr_t)goalc_mem + EE_MAIN_MEM_SIZE) {
      break;
    }
    uintptr_t next_fp;
    memcpy(&next_fp, (void*)fp, sizeof(uintptr_t));
    memcpy(&pc, (void*)(fp + sizeof(uintptr_t)), sizeof(uintptr_t));
    if (next_fp <= fp) {
      break;
    }
    fp = next_fp;
  }
}
