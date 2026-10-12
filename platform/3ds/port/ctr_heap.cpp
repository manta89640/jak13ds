/*
 * (AI-assisted)
 * The malloc heap on the 3DS: a log line for every failed allocation, and (OG3DS_HEAP_TRACE
 * builds) who holds the heap.
 *
 * gk links with -Wl,--wrap=malloc,... (platform/3ds/CMakeLists.txt): every call to malloc,
 * calloc, realloc, memalign and free from the game, libstdc++ (operator new) and libctru comes
 * here first. newlib's own internal allocations (_malloc_r: stdio buffers) don't.
 *
 * A failed allocation logs, at most once a second:
 *   [heap] 262144 bytes (malloc) failed: free 5873 KB (2210 KB in the arena, 3663 KB never used),
 *          largest free block 1640 KB, in use 66935 KB of 72808 KB; called from 0x... 0x...
 * The addresses are the caller's frames (arm-none-eabi-addr2line -e gk.elf).
 *
 * Trace builds (cmake -DOG3DS_HEAP_TRACE=ON) also keep every live allocation of 2 KB or more with
 * its call stack; ctr_heap_trace_dump(tag) logs the sites holding the most.
 */

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <unistd.h>
#include <unwind.h>

#include <3ds.h>

#include "common/log/log.h"

#include "platform/3ds/port/ctr_heap.h"

extern "C" {
void* __real_malloc(size_t n);
void* __real_calloc(size_t n, size_t s);
void* __real_realloc(void* p, size_t n);
void* __real_memalign(size_t a, size_t n);
void __real_free(void* p);
void* __wrap_malloc(size_t n);
void* __wrap_calloc(size_t n, size_t s);
void* __wrap_realloc(void* p, size_t n);
void* __wrap_memalign(size_t a, size_t n);
void __wrap_free(void* p);
void __malloc_lock(struct _reent*);
void __malloc_unlock(struct _reent*);
extern u32 __ctru_heap_size;
extern char* fake_heap_end;  // the end of the heap sbrk hands out (ctr_port.c)
extern void* __malloc_av_[];  // newlib malloc's bins
}

namespace {

constexpr int kFrames = 8;

struct Trace {
  void* pc[kFrames];
  int n;
  int skip;
};

_Unwind_Reason_Code trace_frame(_Unwind_Context* ctx, void* arg) {
  Trace* t = (Trace*)arg;
  if (t->skip > 0) {
    t->skip--;
    return _URC_NO_REASON;
  }
  if (t->n >= kFrames) {
    return _URC_END_OF_STACK;
  }
  t->pc[t->n++] = (void*)(_Unwind_GetIP(ctx) & ~1u);
  return _URC_NO_REASON;
}

void backtrace_here(Trace* t, int skip) {
  t->n = 0;
  t->skip = skip;
  _Unwind_Backtrace(trace_frame, t);
}

// (AI-assisted) The largest block malloc can hand out now, from newlib's free lists (its malloc is
// Doug Lea's 2.6: 128 bins of free chunks in __malloc_av_, bin 0's fd is the top chunk, which can
// grow up to the end of the heap). Not by trying allocations: those would take the memory from
// the other threads for a moment. Returns 0 if the walk doesn't add up to mallinfo's free bytes
// (another malloc layout).
struct MallocChunk {
  size_t prev_size;
  size_t size;
  MallocChunk* fd;
  MallocChunk* bk;
};
size_t chunk_size(const MallocChunk* c) {
  return c->size & ~(size_t)3;  // PREV_INUSE, IS_MMAPPED
}

size_t largest_free_block(size_t* walked_free) {
  constexpr int kBins = 128;
  __malloc_lock(_REENT);
  MallocChunk** av = (MallocChunk**)__malloc_av_;
  auto bin_at = [&](int i) {
    return (MallocChunk*)((char*)&av[2 * i + 2] - 2 * sizeof(size_t));
  };
  const MallocChunk* top = av[2];
  size_t sum = top ? chunk_size(top) : 0, largest = 0;
  bool sane = top != nullptr;
  for (int i = 1; i < kBins && sane; i++) {
    MallocChunk* b = bin_at(i);
    int n = 0;
    for (MallocChunk* c = b->fd; c != b; c = c->fd) {
      if (!c || ++n > 1000000) {
        sane = false;
        break;
      }
      const size_t sz = chunk_size(c);
      sum += sz;
      largest = std::max(largest, sz);
    }
  }
  // the top chunk and what sbrk can still add to it
  const char* brk = (const char*)sbrk(0);
  const size_t beyond = brk && brk < fake_heap_end ? (size_t)(fake_heap_end - brk) : 0;
  if (top) {
    largest = std::max(largest, chunk_size(top) + beyond);
  }
  __malloc_unlock(_REENT);
  *walked_free = sum;
  return sane ? largest : 0;
}

std::atomic<int> g_reporting{0};
std::atomic<u64> g_last_report_ms{0};
std::atomic<u32> g_failures{0};

void report_failure(size_t n, const char* what) {
  g_failures++;
  const u64 now = osGetTime();
  const u64 last = g_last_report_ms.load();
  if (last && now - last < 1000) {
    return;
  }
  int expected = 0;
  if (!g_reporting.compare_exchange_strong(expected, 1)) {
    return;  // another thread reports (or this one, from the log's own allocations)
  }
  g_last_report_ms.store(now ? now : 1);
  Trace t;
  backtrace_here(&t, 2);  // past report_failure and the wrapper
  char where[kFrames * 12 + 1];
  int len = 0;
  for (int i = 0; i < t.n; i++) {
    len += snprintf(where + len, sizeof(where) - len, " %p", t.pc[i]);
  }
  where[len] = 0;
  CtrHeapStats st;
  ctr_heap_stats(&st);
  lg::error(
      "[heap] {} bytes ({}) failed: free {} KB ({} KB in the arena, {} KB never used), largest free "
      "block {} KB, in use {} KB of {} KB; failures so far {}; called from{}",
      n, what, st.free / 1024, st.arena_free / 1024, st.never_used / 1024, st.largest / 1024,
      st.in_use / 1024, st.heap / 1024, g_failures.load(), where);
  g_reporting.store(0);
}

#ifdef OG3DS_HEAP_TRACE
// ---- trace builds ----
constexpr size_t kTraceMin = 2048;
constexpr u32 kSlots = 1u << 15;  // live allocations of kTraceMin or more
constexpr u32 kSites = 1u << 12;
struct Site {
  void* pc[kFrames];
  u32 hash;
  u32 count;
  u64 bytes;
  u64 peak;
};
struct Slot {
  void* p;
  u32 size;
  u32 site;  // + 1, 0: empty slot
};
Slot* g_slots = nullptr;
Site* g_sites = nullptr;
u64 g_traced = 0;
u32 g_dropped = 0;
bool g_in_trace = false;

u32 hash_ptr(const void* p) {
  return ((u32)(uintptr_t)p >> 3) * 2654435761u;
}

bool trace_init() {
  if (!g_slots) {
    // linear memory: not the heap that's measured (and not counted in its use)
    g_slots = (Slot*)linearAlloc(sizeof(Slot) * kSlots);
    g_sites = (Site*)linearAlloc(sizeof(Site) * kSites);
    if (!g_slots || !g_sites) {
      return false;
    }
    memset(g_slots, 0, sizeof(Slot) * kSlots);
    memset(g_sites, 0, sizeof(Site) * kSites);
  }
  return true;
}

// under the malloc lock
void trace_add(void* p, size_t n, void* caller) {
  if (!p || n < kTraceMin || g_in_trace || !trace_init()) {
    return;
  }
  g_in_trace = true;
  Trace t;
  backtrace_here(&t, 2);
  if (t.n == 0 || t.pc[0] != caller) {
    // the wrapper's caller first (C code has no unwind tables: the unwinder stops there)
    if (t.n == kFrames) {
      t.n--;
    }
    memmove(&t.pc[1], &t.pc[0], t.n * sizeof(void*));
    t.pc[0] = caller;
    t.n++;
  }
  u32 h = 2166136261u;
  for (int i = 0; i < t.n; i++) {
    h = (h ^ (u32)(uintptr_t)t.pc[i]) * 16777619u;
  }
  u32 s = h & (kSites - 1);
  for (u32 k = 0; k < kSites; k++, s = (s + 1) & (kSites - 1)) {
    Site& site = g_sites[s];
    if (!site.count && !site.bytes && !site.hash) {
      memcpy(site.pc, t.pc, sizeof(t.pc));
      for (int i = t.n; i < kFrames; i++) {
        site.pc[i] = nullptr;
      }
      site.hash = h | 1;
      break;
    }
    if (site.hash == (h | 1) && !memcmp(site.pc, t.pc, t.n * sizeof(void*))) {
      break;
    }
  }
  u32 i = hash_ptr(p) & (kSlots - 1);
  for (u32 k = 0; k < kSlots; k++, i = (i + 1) & (kSlots - 1)) {
    if (!g_slots[i].site) {
      g_slots[i] = {p, (u32)n, s + 1};
      Site& site = g_sites[s];
      site.count++;
      site.bytes += n;
      if (site.bytes > site.peak) {
        site.peak = site.bytes;
      }
      g_traced += n;
      g_in_trace = false;
      return;
    }
  }
  g_dropped++;
  g_in_trace = false;
}

void trace_remove(void* p) {
  if (!p || !g_slots) {
    return;
  }
  u32 i = hash_ptr(p) & (kSlots - 1);
  for (u32 k = 0; k < kSlots; k++, i = (i + 1) & (kSlots - 1)) {
    if (!g_slots[i].site) {
      return;
    }
    if (g_slots[i].p == p) {
      Site& site = g_sites[g_slots[i].site - 1];
      site.count--;
      site.bytes -= g_slots[i].size;
      g_traced -= g_slots[i].size;
      // backward-shift deletion (linear probing)
      u32 j = i;
      for (;;) {
        g_slots[i].site = 0;
        for (;;) {
          j = (j + 1) & (kSlots - 1);
          if (!g_slots[j].site) {
            return;
          }
          const u32 home = hash_ptr(g_slots[j].p) & (kSlots - 1);
          if (i <= j ? (i < home && home <= j) : (i < home || home <= j)) {
            continue;
          }
          break;
        }
        g_slots[i] = g_slots[j];
        i = j;
      }
    }
  }
}
#endif

}  // namespace

extern "C" {

void* __wrap_malloc(size_t n) {
#ifdef OG3DS_HEAP_TRACE
  __malloc_lock(_REENT);
  void* p = __real_malloc(n);
  trace_add(p, n, __builtin_return_address(0));
  __malloc_unlock(_REENT);
#else
  void* p = __real_malloc(n);
#endif
  if (!p && n) {
    report_failure(n, "malloc");
  }
  return p;
}

void* __wrap_calloc(size_t n, size_t s) {
#ifdef OG3DS_HEAP_TRACE
  __malloc_lock(_REENT);
  void* p = __real_calloc(n, s);
  trace_add(p, n * s, __builtin_return_address(0));
  __malloc_unlock(_REENT);
#else
  void* p = __real_calloc(n, s);
#endif
  if (!p && n && s) {
    report_failure(n * s, "calloc");
  }
  return p;
}

void* __wrap_realloc(void* old, size_t n) {
#ifdef OG3DS_HEAP_TRACE
  __malloc_lock(_REENT);
  void* p = __real_realloc(old, n);
  if (p || !n) {
    trace_remove(old);
    trace_add(p, n, __builtin_return_address(0));
  }
  __malloc_unlock(_REENT);
#else
  void* p = __real_realloc(old, n);
#endif
  if (!p && n) {
    report_failure(n, "realloc");
  }
  return p;
}

void* __wrap_memalign(size_t a, size_t n) {
#ifdef OG3DS_HEAP_TRACE
  __malloc_lock(_REENT);
  void* p = __real_memalign(a, n);
  trace_add(p, n, __builtin_return_address(0));
  __malloc_unlock(_REENT);
#else
  void* p = __real_memalign(a, n);
#endif
  if (!p && n) {
    report_failure(n, "memalign");
  }
  return p;
}

void __wrap_free(void* p) {
#ifdef OG3DS_HEAP_TRACE
  __malloc_lock(_REENT);
  trace_remove(p);
  __real_free(p);
  __malloc_unlock(_REENT);
#else
  __real_free(p);
#endif
}

}  // extern "C"

void ctr_heap_stats(CtrHeapStats* st) {
  struct mallinfo mi = mallinfo();
  st->heap = __ctru_heap_size;
  st->in_use = (u32)mi.uordblks;
  st->arena_free = (u32)mi.fordblks;
  st->never_used = st->heap > (u32)mi.arena ? st->heap - (u32)mi.arena : 0;
  st->free = st->arena_free + st->never_used;
  size_t walked = 0;
  st->largest = (u32)largest_free_block(&walked);
  // (mallinfo is a moment earlier: other threads may have allocated in between)
  static bool warned = false;
  const size_t diff = walked > mi.fordblks ? walked - mi.fordblks : mi.fordblks - walked;
  if (!st->largest || diff > (1u << 20)) {
    if (!warned) {
      warned = true;
      lg::warn("[heap] the free lists ({} KB) don't match mallinfo ({} KB): largest block unknown",
               walked / 1024, (size_t)mi.fordblks / 1024);
    }
    st->largest = 0;
  }
}

u32 ctr_heap_failures() {
  return g_failures.load();
}

void ctr_heap_trace_dump(const char* tag) {
#ifdef OG3DS_HEAP_TRACE
  CtrHeapStats st;
  ctr_heap_stats(&st);
  // copy the sites out under the lock, then log (which allocates)
  constexpr int kTop = 40;
  static Site top[kTop];
  int n = 0;
  u64 traced;
  u32 dropped;
  __malloc_lock(_REENT);
  traced = g_traced;
  dropped = g_dropped;
  if (g_sites) {
    for (u32 s = 0; s < kSites; s++) {
      const Site& site = g_sites[s];
      if (site.bytes < 32 * 1024) {
        continue;
      }
      int at = n < kTop ? n++ : kTop;
      while (at > 0 && top[at - 1].bytes < site.bytes) {
        if (at < kTop) {
          top[at] = top[at - 1];
        }
        at--;
      }
      if (at < kTop) {
        top[at] = site;
      }
    }
  }
  __malloc_unlock(_REENT);
  lg::info(
      "[heaptrace] {}: in use {} KB (traced >=2 KB: {} KB, smaller: {} KB), free {} KB, largest "
      "{} KB, dropped {}",
      tag, st.in_use / 1024, traced / 1024, (st.in_use - (u32)traced) / 1024, st.free / 1024,
      st.largest / 1024, dropped);
  for (int i = 0; i < n; i++) {
    char where[kFrames * 12 + 1];
    int len = 0;
    for (int k = 0; k < kFrames && top[i].pc[k]; k++) {
      len += snprintf(where + len, sizeof(where) - len, " %p", top[i].pc[k]);
    }
    where[len] = 0;
    lg::info("[heaptrace]   {} KB in {} (peak {} KB):{}", top[i].bytes / 1024, top[i].count,
             top[i].peak / 1024, where);
  }
#else
  (void)tag;
#endif
}
