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
#include <unistd.h>
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

u64 unlinked_function() {
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
// Host functions that don't have the goalc_fnx signature (all-u64 kernel functions, typed kernel
// functions, arg3_is_pp kernel functions, stack-argument kernel functions, mips2c functions) need a
// distinct goalc_fnx entry point that knows which function to call and which arguments are wide.
// There is no runtime code generation, so we use a fixed pool of thunks.

constexpr u32 kMaxThunks = 2048;

struct ThunkSlot {
  goalc_adapter adapter = nullptr;
  void* fn = nullptr;
  u64 extra = 0;
  u32 wide_mask = 0;
  u32 id = 0;
};

ThunkSlot g_thunk_slots[kMaxThunks];
u32 g_thunk_count = 0;

// OPENGOAL_C_ABI_CHECK=1 (with modules built with -DGOALC_ABI_CHECK, see goal_c_ops.h): report
// wide arguments of host functions that the caller passed without their upper half
bool g_abi_check = false;
constexpr u32 kAbiPoison = 0x7fbadbad;  // GC_ABI_POISON in goal_c_ops.h

void check_thunk_arg(float hi, u32 lo, int i, const ThunkSlot& slot);

inline u64 thunk_arg(u32 lo, float hi, u32 mask, int i, const ThunkSlot& slot) {
  if (!((mask >> i) & 1)) {
    return lo;
  }
  if (g_abi_check) {
    check_thunk_arg(hi, lo, i, slot);
  }
  return goalc_arg64(lo, hi);
}

// (AI-assisted) uint32_t, not u32: goalc_fnx's type. On the 3DS u32 is unsigned int and uint32_t
// unsigned long (newlib): the same registers, but different C++ types.
template <u32 I>
u64 thunk(uint32_t a0,
          uint32_t a1,
          uint32_t a2,
          uint32_t a3,
          uint32_t a4,
          uint32_t a5,
          uint32_t a6,
          uint32_t a7,
          float h0,
          float h1,
          float h2,
          float h3,
          float h4,
          float h5,
          float h6,
          float h7) {
  const ThunkSlot& slot = g_thunk_slots[I];
  const u32 m = slot.wide_mask;
  u64 args[8] = {thunk_arg(a0, h0, m, 0, slot), thunk_arg(a1, h1, m, 1, slot),
                 thunk_arg(a2, h2, m, 2, slot), thunk_arg(a3, h3, m, 3, slot),
                 thunk_arg(a4, h4, m, 4, slot), thunk_arg(a5, h5, m, 5, slot),
                 thunk_arg(a6, h6, m, 6, slot), thunk_arg(a7, h7, m, 7, slot)};
  return slot.adapter(slot.fn, slot.extra, args);
}

template <u32... Is>
constexpr std::array<goalc_fnx, sizeof...(Is)> make_thunks(std::integer_sequence<u32, Is...>) {
  return {&thunk<Is>...};
}

void check_thunk_arg(float hi, u32 lo, int i, const ThunkSlot& slot) {
  u32 bits;
  memcpy(&bits, &hi, 4);
  if (bits == kAbiPoison) {
    char what[192];
    const char* fn_name = "?";
#if GOALC_USE_DLOPEN
    // (AI-assisted) name the host function when the dynamic symbol table has it
    Dl_info info;
    if (dladdr(slot.fn, &info) && info.dli_sname) {
      fn_name = info.dli_sname;
    }
#endif
    snprintf(what, sizeof(what), "host function %p %s (adapter %p) wide argument %d", slot.fn,
             fn_name, (void*)slot.adapter, i);
    goalc_abi_check_failed("thunk", 0, what, lo);
  }
}

u64 adapter_fn8(void* fn, u64, u64* args) {
  return ((goalc_fn8)fn)(args[0], args[1], args[2], args[3], args[4], args[5], args[6], args[7]);
}

const auto g_thunks = make_thunks(std::make_integer_sequence<u32, kMaxThunks>{});

// ---------------------------------------------------------------------------
// Modules
// ---------------------------------------------------------------------------

// (AI-assisted) hash -> the instances of the module: separately compiled copies of the same C
// source, each with its own static data (seg_base, sym_offsets). Several instances let several
// loaded copies of one object file (the same object in two co-loaded level DGOs) each have their
// own segment bases. Static builds register extra instances (gen_c_registry.py, the .c file
// compiled again under another name); dlopen hosts load copies of the .so on demand.
std::unordered_map<u64, std::vector<const GoalCModule*>>& static_registry() {
  static std::unordered_map<u64, std::vector<const GoalCModule*>> registry;
  return registry;
}

//! A level heap that objects are linked into, to tell when it was emptied (a level unloaded).
struct HeapTrack {
  u32 base = 0;
  u32 last_addr = 0;
  u32 generation = 0;
};
//! by the GOAL address of the kheapinfo
std::unordered_map<u32, HeapTrack> g_heaps;

struct ModuleInstance {
  const GoalCModule* mod = nullptr;
  bool has_ids = false;
  u32 base_id = 0;
  //! the object file copy linked to this instance last (0: none since the last init)
  u32 seg_bases[3] = {0, 0, 0};
  bool linked = false;
  //! that copy is alive while the heap it is in wasn't emptied and its first stub (outside the
  //! top-level segment) still holds this instance's function
  u32 heap_key = 0;
  u32 heap_generation = 0;
  u32 probe_addr = 0;
  void* probe_fn = nullptr;
  u64 link_seq = 0;
};

struct LoadedModule {
  std::vector<ModuleInstance> instances;
  bool warned = false;
};

// modules that have been linked at least once since the last goalc_runtime_init
std::unordered_map<u64, LoadedModule> g_linked_modules;
u64 g_link_seq = 0;

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
  return goalc_call8(call->f, call->args);
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

void goalc_abi_check_failed(const char* file, int line, const char* what, uint64_t value) {
  // (AI-assisted) see goal_c_ops.h. Report each place once.
  static std::mutex mutex;
  static std::unordered_map<std::string, int> seen;
  std::lock_guard<std::mutex> lock(mutex);
  auto key = fmt::format("{}:{}: {}", file, line, what);
  if (seen[key]++ == 0) {
    lg::warn("[goalc abi check] {} (value #x{:x})", key, value);
  }
}

void goalc_register_module(const GoalCModule* mod) {
  // (AI-assisted) a module registered again under the same hash is another instance of it
  auto& list = static_registry()[mod->hash];
  for (auto* m : list) {
    if (m == mod) {
      return;
    }
    if (m->name && mod->name && strcmp(m->name, mod->name) != 0) {
      // can't use lg here, this may run in a static constructor.
      fprintf(stderr, "[goalc] module %s has the same hash as %s (%016llx)\n", mod->name,
              m->name, (unsigned long long)mod->hash);
    }
  }
  list.push_back(mod);
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
  g_heaps.clear();
  if (const char* check = getenv("OPENGOAL_C_ABI_CHECK")) {
    g_abi_check = check[0] == '1';
  }
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

bool goalc_abi_check_enabled() {
  return g_abi_check;
}

u32 goalc_fn_id_for_adapted(goalc_adapter adapter, void* fn, u64 extra, u32 wide_mask) {
  wide_mask &= GOALC_ALL_WIDE;
  for (u32 i = 0; i < g_thunk_count; i++) {
    auto& slot = g_thunk_slots[i];
    if (slot.adapter == adapter && slot.fn == fn && slot.extra == extra &&
        slot.wide_mask == wide_mask) {
      return slot.id;
    }
  }
  ASSERT_MSG(g_thunk_count < kMaxThunks, "goalc: out of adapter thunks, increase kMaxThunks");
  auto& slot = g_thunk_slots[g_thunk_count];
  slot.adapter = adapter;
  slot.fn = fn;
  slot.extra = extra;
  slot.wide_mask = wide_mask;
  slot.id = g_fn_table.push((void*)g_thunks[g_thunk_count]);
  g_thunk_count++;
  return slot.id;
}

namespace {
std::vector<GoalcKernelFunctionInfo> g_kernel_functions;
}

void goalc_note_kernel_function(const char* name, u32 wide_mask) {
  for (auto& f : g_kernel_functions) {
    if (!strcmp(f.name, name)) {
      f.wide_mask = wide_mask;
      return;
    }
  }
  g_kernel_functions.push_back({name, wide_mask});
}

const std::vector<GoalcKernelFunctionInfo>& goalc_kernel_functions() {
  return g_kernel_functions;
}

void goalc_dump_kernel_functions_if_requested() {
  const char* path = getenv("OPENGOAL_C_ABI_KERNEL_DUMP");
  if (!path || !path[0]) {
    return;
  }
  FILE* f = fopen(path, "w");
  if (!f) {
    lg::error("goalc: can't write the kernel function list to {}", path);
    return;
  }
  for (const auto& kf : g_kernel_functions) {
    fprintf(f, "%s %u\n", kf.name, kf.wide_mask);
  }
  fclose(f);
  lg::info("goalc: wrote {} kernel functions to {}", g_kernel_functions.size(), path);
}

u32 goalc_fn_id_for_fn8(void* fn, u32 wide_mask) {
  return goalc_fn_id_for_adapted(adapter_fn8, fn, 0, wide_mask);
}

u32 goalc_fn_id_remask(u32 id, u32 wide_mask) {
  for (u32 i = 0; i < g_thunk_count; i++) {
    const auto& slot = g_thunk_slots[i];
    if (slot.id == id) {
      return goalc_fn_id_for_adapted(slot.adapter, slot.fn, slot.extra, wide_mask);
    }
  }
  return id;
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
  // fn is a goalc_fn8
  // the native stub moves pp into the fourth argument register and leaves the rest alone.
  return ((goalc_fn8)fn)(args[0], args[1], args[2], goalc_pp, args[4], args[5], args[6], args[7]);
}

u64 goalc_adapter_stack_args(void* fn, u64, u64* args) {
  // like the native _stack_call trampolines: a pointer to the 8 arguments, on the stack.
  return ((u64(*)(u64*))fn)(args);
}

u64 goalc_nothing_fn(u32 a0) {
  // the native `nothing` is just a return, which leaves the first argument in the return register
  // on arm64 (only its low half here: the upper half is only passed for wide arguments).
  return a0;
}

u64 goalc_zero_fn() {
  return 0;
}

// ---------------------------------------------------------------------------
// Modules
// ---------------------------------------------------------------------------

namespace {
#if GOALC_USE_DLOPEN
const GoalCModule* dlopen_module(const fs::path& path, u64 hash) {
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
  // keep the library loaded forever
  return mod;
}

fs::path module_so_path(u64 hash) {
  return file_util::get_jak_project_dir() / "out" / game_version_names[g_game_version] / "cmod" /
         fmt::format("{:016x}.so", hash);
}
#endif

/*!
 * (AI-assisted) Instance k of a module, or null if there is none. Static builds have the instances
 * that were compiled in. dlopen hosts make any number: dlopen of the same path returns the same
 * library, so instance k > 0 is a copy of the .so under another name (its own static data).
 */
u32 max_instances() {
  // OPENGOAL_C_MODULE_INSTANCES=1: one instance per module, the old behaviour (tests). The default
  // bounds the dlopen copies, in case copies are wrongly thought alive.
  static const u32 max = [] {
    const char* v = getenv("OPENGOAL_C_MODULE_INSTANCES");
    const int n = v ? atoi(v) : 0;
    return n > 0 ? (u32)n : 8u;
  }();
  return max;
}

const GoalCModule* find_module_instance(u64 hash, u32 k) {
  if (k >= max_instances()) {
    return nullptr;
  }
  auto& list = static_registry()[hash];
  if (k < list.size()) {
    return list[k];
  }
#if GOALC_USE_DLOPEN
  if (k != list.size()) {
    return nullptr;
  }
  const GoalCModule* mod = nullptr;
  if (k == 0) {
    mod = dlopen_module(module_so_path(hash), hash);
  } else {
    std::error_code ec;
    auto dir = fs::temp_directory_path(ec) / fmt::format("opengoal-cmod-{}", (int)getpid());
    fs::create_directories(dir, ec);
    auto copy = dir / fmt::format("{:016x}.{}.so", hash, k);
    fs::copy_file(module_so_path(hash), copy, fs::copy_options::overwrite_existing, ec);
    if (ec) {
      lg::error("[goalc] could not copy module {:016x} for instance {}: {}", hash, k, ec.message());
      return nullptr;
    }
    mod = dlopen_module(copy, hash);
    fs::remove(copy, ec);  // stays mapped
  }
  if (mod) {
    list.push_back(mod);
  }
  return mod;
#else
  return nullptr;
#endif
}

bool instance_alive(const ModuleInstance& inst) {
  if (!inst.linked || !inst.probe_addr) {
    return false;
  }
  if (inst.heap_key) {
    auto h = g_heaps.find(inst.heap_key);
    if (h == g_heaps.end() || h->second.generation != inst.heap_generation) {
      return false;
    }
    // the heap now (kheapinfo: base, top, current, top-base): a copy above its current end was
    // freed (a level unloads by emptying its heap, before the next level fills it)
    u32 heap_words[4];
    memcpy(heap_words, goalc_mem + inst.heap_key, sizeof(heap_words));
    if (heap_words[0] != h->second.base || inst.probe_addr >= heap_words[2]) {
      return false;
    }
  }
  void* fn = nullptr;
  memcpy(&fn, goalc_mem + inst.probe_addr, sizeof(void*));
  return fn == inst.probe_fn;
}

/*!
 * Note an object linked at addr into a heap. Heaps allocate upwards until they are emptied, so an
 * object at or below the previous one (or a heap with a new base) means the heap was emptied: the
 * object files linked there before are gone. Returns the heap's generation.
 */
u32 note_heap_link(const GoalcLinkHeap& heap, u32 addr) {
  if (!heap.key) {
    return 0;
  }
  auto& h = g_heaps[heap.key];
  if (h.base != heap.base || addr <= h.last_addr) {
    h.generation++;
    h.base = heap.base;
  }
  h.last_addr = addr;
  return h.generation;
}

/*!
 * The instance to link this copy of the object file to: the one this copy already uses (each
 * segment has its own LINK_C_MODULE entry), else one whose copy is gone, else a new one (dlopen
 * hosts), else the least recently linked one (its copy then uses this copy's static data: the old
 * hazard, reported once per module).
 */
ModuleInstance& pick_instance(u64 hash,
                              const u32 seg_bases[3],
                              const GoalcLinkHeap& heap,
                              const char* object_name) {
  auto& lm = g_linked_modules[hash];
  for (auto& inst : lm.instances) {
    if (inst.linked && !memcmp(inst.seg_bases, seg_bases, sizeof(inst.seg_bases))) {
      return inst;
    }
  }
  // a new copy of the object file
  const u32 main_addr = seg_bases[MAIN_SEGMENT];
  const u32 generation = main_addr ? note_heap_link(heap, main_addr) : 0;
  ModuleInstance* pick = nullptr;
  for (auto& inst : lm.instances) {
    if (!instance_alive(inst)) {
      pick = &inst;
      break;
    }
  }
  if (!pick) {
    if (auto* mod = find_module_instance(hash, (u32)lm.instances.size())) {
      lm.instances.emplace_back();
      pick = &lm.instances.back();
      pick->mod = mod;
      if (lm.instances.size() > 1) {
        lg::info("[goalc] {}: {} copies loaded at once, linked to C module instance {}",
                 object_name, lm.instances.size(), lm.instances.size() - 1);
      }
    }
  }
  if (!pick) {
    if (lm.instances.empty()) {
      // (AI-assisted) the usual cause: the game files (out/<game>/iso) and the runtime come from
      // different goalc runs (the C modules are linked into the runtime, e.g. gk.3dsx on the 3DS)
      ASSERT_MSG(false, fmt::format("goalc: could not find C module {:016x} for object file {}. "
                                    "The game files (out/<game>/iso: CGO/DGO) and this runtime "
                                    "are from different builds: copy both from the same build.",
                                    hash, object_name));
    }
    pick = &lm.instances[0];
    for (auto& inst : lm.instances) {
      if (inst.link_seq < pick->link_seq) {
        pick = &inst;
      }
    }
    if (!lm.warned) {
      lm.warned = true;
      lg::error("[goalc] {}: {} copies loaded at once but only {} C module instance(s): the older "
                "copy now uses this copy's static data (add instances for this module)",
                object_name, lm.instances.size() + 1, lm.instances.size());
    }
  }
  memcpy(pick->seg_bases, seg_bases, sizeof(pick->seg_bases));
  pick->linked = true;
  pick->heap_key = main_addr ? heap.key : 0;
  pick->heap_generation = generation;
  pick->probe_addr = 0;
  pick->probe_fn = nullptr;
  pick->link_seq = ++g_link_seq;
  return *pick;
}
}  // namespace

const GoalCModule* goalc_find_module(u64 hash) {
  return find_module_instance(hash, 0);
}

u32 goalc_module_instance_count(u64 hash) {
  auto it = g_linked_modules.find(hash);
  return it == g_linked_modules.end() ? 0 : (u32)it->second.instances.size();
}

u32 goalc_link_module_entry(const u8* link,
                            const u32 seg_bases[3],
                            u8* seg_data,
                            goalc_sym_resolver resolver,
                            const char* object_name,
                            const GoalcLinkHeap& heap) {
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

  ModuleInstance& inst = pick_instance(hash, seg_bases, heap, object_name);
  const GoalCModule* mod = inst.mod;

  for (int i = 0; i < 3; i++) {
    mod->seg_base[i] = seg_bases[i];
  }

  for (u32 i = 0; i < mod->n_syms; i++) {
    mod->sym_offsets[i] = resolver(mod->sym_names[i]);
  }
  if (mod->after_link) {
    mod->after_link();
  }

  if (!inst.has_ids) {
    inst.base_id = goalc_alloc_fn_ids(mod->funcs, mod->n_funcs);
    inst.has_ids = true;
  }
  u32 base_id = inst.base_id;

  const u32 seg_addr = (u32)(seg_data - goalc_mem);
  const bool top_level = seg_bases[TOP_LEVEL_SEGMENT] && seg_addr == seg_bases[TOP_LEVEL_SEGMENT];
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
    if (!inst.probe_addr && !top_level) {
      inst.probe_addr = seg_addr + stub_offset;
      inst.probe_fn = fn;
    }
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
    result = goalc_call8(f, args);
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
  u64 result = goalc_call8(f, args);
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
