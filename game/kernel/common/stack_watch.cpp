/*!
 * @file stack_watch.cpp
 * See stack_watch.h.
 */

#include "stack_watch.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "common/log/log.h"

#include "game/kernel/common/goalc_runtime.h"

#ifdef __3DS__
extern "C" int ctr_config_get(const char* key, char* out, int size);  // platform/3ds/port
#endif

namespace stack_watch {

namespace {

constexpr u32 kPattern = 0x5afe57ac;
//! a run's frames end at the first gap of this many untouched words (big local arrays that are
//! never written can hide deeper frames: the depth is then too low)
constexpr u32 kGapWords = 512;
//! the temporary cpu-thread at stack_top - temp_size: never filled (it is live during a hook)
constexpr u32 kThreadBytes = 256;

struct State {
  int on = -1;
  bool filled = false;
  Config cfg;
  struct Record {
    u32 depth = 0;
    std::string name;  // read when the record is set (GOAL memory is gone at exit)
  };
  std::unordered_map<u64, Record> max_depth;
  u64 runs = 0;
  bool changed = false;
  std::chrono::steady_clock::time_point last_log;
} g;

u32* words(u32 addr) {
  return (u32*)(goalc_mem + addr);
}

std::string key_name(u64 key) {
  return g.cfg.name ? g.cfg.name((u32)(key >> 32), (u32)key) : "?";
}

void fill(u32 lo, u32 hi) {
  const u32 win_lo = g.cfg.stack_top - g.cfg.temp_size;
  const u32 win_hi = win_lo + kThreadBytes;
  for (u32 a = lo; a < hi; a += 4) {
    if (a < win_lo || a >= win_hi) {
      *words(a) = kPattern;
    }
  }
}

std::string summary() {
  std::vector<const State::Record*> v;
  for (const auto& [k, r] : g.max_depth) {
    v.push_back(&r);
  }
  std::sort(v.begin(), v.end(), [](const auto* a, const auto* b) { return a->depth > b->depth; });
  std::string line;
  for (size_t i = 0; i < v.size() && i < 12; i++) {
    line += fmt::format(" {} {:.1f}", v[i]->name, v[i]->depth / 1024.f);
  }
  return fmt::format(
      "[stack] deepest runs (KB; temporary threads have {} KB, main threads {} KB), {} runs:{}",
      g.cfg.temp_size / 1024, g.cfg.stack_size / 1024, g.runs, line);
}

}  // namespace

bool on() {
  if (g.on < 0) {
    g.on = 0;
#ifdef __3DS__
    char v[16];
    if (ctr_config_get("stack_watch", v, sizeof(v))) {
      g.on = !strcmp(v, "on") || !strcmp(v, "1") || !strcmp(v, "yes") || !strcmp(v, "true");
    }
#else
    const char* env = getenv("OPENGOAL_STACK_WATCH");
    g.on = env && env[0] == '1';
#endif
    if (g.on) {
      lg::warn("[stack] stack watch on: process stack depths are measured (slower)");
      atexit([] { printf("%s\n", summary().c_str()); });
    }
  }
  return g.on == 1;
}

void configure(const Config& config) {
  g.cfg = config;
  g.last_log = std::chrono::steady_clock::now();
}

bool configured() {
  return g.cfg.stack_top != 0;
}

void after_run(u32 stack_top, u32 type, u32 thread_name) {
  const u32 top = g.cfg.stack_top;
  if (stack_top != top) {
    return;
  }
  const u32 bottom = top - g.cfg.stack_size;
  if (!g.filled) {
    g.filled = true;
    fill(bottom, top);
    return;
  }
  g.runs++;
  const u32 win_lo = top - g.cfg.temp_size;
  const u32 win_hi = win_lo + kThreadBytes;
  u32 lowest = top;
  u32 gap = 0;
  for (u32 a = top - 4; a >= bottom && gap < kGapWords; a -= 4) {
    if (a >= win_lo && a < win_hi) {
      continue;
    }
    if (*words(a) != kPattern) {
      lowest = a;
      gap = 0;
    } else {
      gap++;
    }
    if (a == bottom) {
      break;
    }
  }
  fill(lowest, top);
  const u32 depth = top - lowest;
  const u64 key = ((u64)type << 32) | thread_name;
  auto& rec = g.max_depth[key];
  if (depth > rec.depth) {
    rec.depth = depth;
    if (rec.name.empty()) {
      rec.name = key_name(key);
    }
    g.changed = true;
    // a hook that reaches its cpu-thread (or comes within 1 KB of it)
    if (lowest < win_hi + 1024 && depth <= g.cfg.temp_size + kThreadBytes &&
        depth + 1024 + kThreadBytes > g.cfg.temp_size) {
      lg::warn("[stack] {} used {} bytes, near the temporary thread at {} KB", rec.name, depth,
               g.cfg.temp_size / 1024);
    }
  }
  if ((g.runs & 1023) == 0) {
    const auto now = std::chrono::steady_clock::now();
    if (g.changed && now - g.last_log > std::chrono::seconds(30)) {
      g.last_log = now;
      g.changed = false;
      lg::info("{}", summary());
    }
  }
}

}  // namespace stack_watch
