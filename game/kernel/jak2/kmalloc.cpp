#include "kmalloc.h"

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "game/kernel/common/kscheme.h"

/*
 * Jak 2 adds a few "memopen" functions. I think these were used to categorize memory allocations.
 * GOAL and C++ code will call "memopen" with an appropriate category name before making
 * allocations.
 *
 * These functions do nothing in the release version (and all known versions), but they might be
 * interested for us to track memory usage.
 *
 * (AI-assisted) They now track the bottom allocations of the heap that is open, per category
 * (exclusive of nested categories), for the memory report of the pad script (kmem_report, called
 * from GOAL as pad-script-kmem-report; docs/3ds-port/jak2_status.md). Bookkeeping only.
 */

namespace jak2 {
namespace {
struct MemFrame {
  std::string name;
  Ptr<kheapinfo> heap;
  u32 start;
  u32 child = 0;
};
std::vector<MemFrame> g_mem_stack;
std::map<std::string, s64> g_mem_totals;
}  // namespace

void kmemopen_from_c(Ptr<kheapinfo> heap, const char* name) {
  g_mem_stack.push_back({name, heap, heap->current.offset});
}

void kmemopen(u32 heap, u32 name) {
  Ptr<kheapinfo> h(heap);
  g_mem_stack.push_back({Ptr<String>(name)->data(), h, h->current.offset});
}

void kmemclose() {
  if (g_mem_stack.empty()) {
    return;
  }
  MemFrame f = g_mem_stack.back();
  g_mem_stack.pop_back();
  s64 delta = (s64)f.heap->current.offset - (s64)f.start;
  g_mem_totals[f.name] += delta - (s64)f.child;
  if (!g_mem_stack.empty() && g_mem_stack.back().heap.offset == f.heap.offset) {
    g_mem_stack.back().child += (u32)delta;
  }
}

//! Print the categories (KB), largest first. Categories still open count up to now.
u64 kmem_report() {
  auto totals = g_mem_totals;
  for (auto& f : g_mem_stack) {
    totals[f.name + " (open)"] += (s64)f.heap->current.offset - (s64)f.start - (s64)f.child;
  }
  std::vector<std::pair<s64, std::string>> v;
  for (auto& [k, n] : totals) {
    v.push_back({n, k});
  }
  std::sort(v.rbegin(), v.rend());
  printf("  kmemopen categories (global heap bottom, exclusive KB):\n");
  for (auto& [n, k] : v) {
    if (n >= 16 * 1024) {
      printf("    %-28s %10.1f\n", k.c_str(), n / 1024.0);
    }
  }
  fflush(stdout);
  return 0;
}
}  // namespace jak2
