/*!
 * @file kperf.cpp
 * (AI-assisted)
 * See kperf.h.
 */

#include "kperf.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/log/log.h"

#ifdef __3DS__
extern "C" int ctr_config_bool(const char* key, int def);  // platform/3ds/port/ctr_port.c
#endif

#ifdef __3DS__
#include <malloc.h>

#include "platform/3ds/port/ctr_port.h"

// libctru (svc 0x28): the ARM11 system tick. Much cheaper than steady_clock, which converts the
// tick to nanoseconds with a 64-bit division.
extern "C" unsigned long long svcGetSystemTick(void);
#endif

namespace kperf {
namespace {
u64 g_totals[(int)Cat::COUNT] = {};
u64 g_frames = 0;
u64 g_window_start = 0;

struct Section {
  std::string name;
  u64 ticks = 0;
  u64 count = 0;
};
// keyed by the name pointer: with-profiler names are static GOAL strings
std::unordered_map<const char*, Section> g_sections;
struct Open {
  const char* name;
  u64 start;
};
constexpr int kMaxOpen = 64;
Open g_open[kMaxOpen];
int g_open_count = 0;
u64 g_section_events = 0;
u64 g_unbalanced = 0;

std::atomic<u64> g_thread_ticks[(int)Thread::COUNT];
std::atomic<u32> g_thread_wakeups[(int)Thread::COUNT];

// RPC waits (game thread only)
constexpr int kRpcChannels = 8;
u64 g_rpc_wait[kRpcChannels];
u64 g_rpc_last_busy[kRpcChannels];

// sampled per-section timing (3DS, no flag file)
constexpr int kSectionSampleEvery = 15;  // report windows (about seconds)
void (*g_sections_hook)(bool) = nullptr;
bool g_sections_forced = false;  // flag file / environment: always on
int g_windows = 0;
}  // namespace

bool g_sections_enabled = false;
u64 g_counters[(int)Counter::COUNT] = {};

u64 ticks() {
#ifdef __3DS__
  return svcGetSystemTick();
#else
  return (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
#endif
}

double ticks_to_ms(u64 t) {
#ifdef __3DS__
  return (double)t / 268111.856;  // SYSCLOCK_ARM11 / 1000
#else
  return (double)t / 1e6;
#endif
}

std::atomic<u32> g_gauges[(int)Gauge::COUNT];

void set_gauge(Gauge g, u32 value) {
  g_gauges[(int)g].store(value, std::memory_order_relaxed);
}

void add_thread(Thread t, u64 dt, u32 wakeups) {
  g_thread_ticks[(int)t].fetch_add(dt, std::memory_order_relaxed);
  g_thread_wakeups[(int)t].fetch_add(wakeups, std::memory_order_relaxed);
}

void set_sections_hook(void (*hook)(bool on)) {
  g_sections_hook = hook;
}

void rpc_poll(int channel, bool busy) {
  if (channel < 0 || channel >= kRpcChannels) {
    return;
  }
  // polls further apart than 2 ms were not a wait loop (the game did other work in between)
#ifdef __3DS__
  constexpr u64 kMaxGap = 2 * 268112;  // ticks
#else
  constexpr u64 kMaxGap = 2 * 1000000;  // ns
#endif
  const u64 now = ticks();
  u64& last = g_rpc_last_busy[channel];
  if (last && now - last < kMaxGap) {
    g_rpc_wait[channel] += now - last;
  }
  last = busy ? now : 0;
}

void init_sections() {
  static bool done = false;
  if (done) {
    return;
  }
  done = true;
#ifdef __3DS__
  FILE* f = fopen("/3ds/jak1/perf_sections", "r");
  if (f) {
    fclose(f);
  }
  if (f || ctr_config_bool("perf_sections", 0)) {  // config.ini or the flag file
    g_sections_enabled = true;
    g_sections_forced = true;
  }
#else
  const char* env = getenv("OPENGOAL_PERF_SECTIONS");
  g_sections_enabled = env && env[0] == '1';
#endif
  if (g_sections_enabled) {
    // what one timed section costs on the C side (the GOAL side adds a call to pc-prof per event)
    static const char* kCal = "calibration";
    const int n = 1000;
    u64 t0 = ticks();
    for (int i = 0; i < n; i++) {
      section_begin(kCal);
      section_end();
    }
    u64 t1 = ticks();
    g_sections.erase(kCal);
    g_section_events = 0;
    lg::info("perf: per-section timing enabled ({:.2f} us per timed section on the C side)",
             ticks_to_ms(t1 - t0) * 1000.0 / n);
  }
}

void section_begin(const char* name) {
  if (g_open_count < kMaxOpen) {
    g_open[g_open_count] = {name, ticks()};
  }
  g_open_count++;
}

void section_end() {
  u64 now = ticks();
  if (g_open_count <= 0) {
    g_unbalanced++;
    return;
  }
  g_open_count--;
  if (g_open_count >= kMaxOpen) {
    return;
  }
  const auto& o = g_open[g_open_count];
  auto& s = g_sections[o.name];
  if (s.name.empty()) {
    s.name = std::string(g_open_count, '.') + o.name;
  }
  s.ticks += now - o.start;
  s.count++;
  g_section_events++;
}

u64 now_us() {
  return (u64)std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void add(Cat cat, u64 us) {
  g_totals[(int)cat] += us;
}

void frame_done() {
  u64 now = now_us();
  if (!g_window_start) {
    init_sections();
    g_window_start = now;
    return;
  }
  g_frames++;
  u64 elapsed = now - g_window_start;
  if (elapsed < 1000000) {
    return;
  }
  double frames = (double)g_frames;
  double dispatch = g_totals[(int)Cat::DISPATCH] / 1000.0 / frames;
  double render = g_totals[(int)Cat::RENDER] / 1000.0 / frames;
  double vsync = g_totals[(int)Cat::VSYNC] / 1000.0 / frames;
  double logic = dispatch - render - vsync;
  double idle = elapsed / 1000.0 / frames - dispatch;
  double fps = frames * 1e6 / (double)elapsed;
  char line[128];
#ifdef __3DS__
  struct mallinfo mi = mallinfo();
  snprintf(line, sizeof(line),
           "%4.1f fps  logic %5.1f  render %5.1f\nvsync %5.1f  idle %5.1f ms  heap %u KB", fps,
           logic, render, vsync, idle, (unsigned)(mi.uordblks / 1024));
  ctr_console_status(line);
#else
  snprintf(line, sizeof(line), "%4.1f fps  logic %5.1f  render %5.1f  vsync %5.1f  idle %5.1f ms",
           fps, logic, render, vsync, idle);
#endif
  for (char* c = line; *c; c++) {
    if (*c == '\n') {
      *c = ' ';
    }
  }
  lg::info("perf: {}{}", line,
           g_sections_enabled && !g_sections_forced ? " (sampled section timing on: slower)" : "");

  // other threads on the game's core
  {
    const double secs = elapsed / 1e6;
    u64 iop_t = g_thread_ticks[(int)Thread::IOP].exchange(0);
    u32 iop_w = g_thread_wakeups[(int)Thread::IOP].exchange(0);
    u64 snd_t = g_thread_ticks[(int)Thread::SOUND].exchange(0);
    u32 snd_w = g_thread_wakeups[(int)Thread::SOUND].exchange(0);
    lg::info(
        "perf threads (ms/frame): iop {:.2f} ({:.0f} dispatches/s), sound {:.2f} ({:.0f}/s, {} "
        "handlers); kernel: {:.0f} thread suspends/frame, {:.1f} KB of stack saved/frame",
        ticks_to_ms(iop_t) / frames, iop_w / secs, ticks_to_ms(snd_t) / frames, snd_w / secs,
        g_gauges[(int)Gauge::SOUND_HANDLERS].load(),
        g_counters[(int)Counter::SUSPEND] / frames,
        g_counters[(int)Counter::SUSPEND_BYTES] / frames / 1024.0);
    for (auto& c : g_counters) {
      c = 0;
    }
    // what the game waited for: the sound player's lock (IOP sound calls blocked by the mixer)
    // and each RPC channel (the game polling a busy channel)
    u64 lock_t = g_thread_ticks[(int)Thread::SOUND_LOCK_WAIT].exchange(0);
    u32 lock_w = g_thread_wakeups[(int)Thread::SOUND_LOCK_WAIT].exchange(0);
    std::string rpc;
    for (int ch = 0; ch < kRpcChannels; ch++) {
      if (g_rpc_wait[ch]) {
        char buf[32];
        snprintf(buf, sizeof(buf), " #%d %.2f", ch, ticks_to_ms(g_rpc_wait[ch]) / frames);
        rpc += buf;
        g_rpc_wait[ch] = 0;
      }
    }
    lg::info("perf waits (ms/frame): sound lock {:.2f} ({:.0f} waits/s); rpc busy:{}",
             ticks_to_ms(lock_t) / frames, lock_w / secs, rpc.empty() ? " none" : rpc);
  }

  if (!g_sections.empty()) {
    std::vector<const Section*> top;
    for (auto& [k, v] : g_sections) {
      if (v.count) {
        top.push_back(&v);
      }
    }
    std::sort(top.begin(), top.end(), [](auto* a, auto* b) { return a->ticks > b->ticks; });
    std::string sec;
    for (size_t i = 0; i < top.size() && i < 48; i++) {
      char buf[128];
      snprintf(buf, sizeof(buf), "%s%s %.2f (%.1f)", i ? ", " : "", top[i]->name.c_str(),
               ticks_to_ms(top[i]->ticks) / frames, top[i]->count / frames);
      sec += buf;
    }
    // (nothing timed in this window: sampled timing was off)
    if (!top.empty()) {
      lg::info("perf sections (ms/frame (calls/frame)): {}", sec);
      lg::info("perf sections: {:.0f} timed sections per frame, {} unbalanced ends",
               g_section_events / frames, g_unbalanced);
    }
    for (auto& [k, v] : g_sections) {
      v.ticks = 0;
      v.count = 0;
    }
    g_section_events = 0;
  }
  for (auto& t : g_totals) {
    t = 0;
  }
  g_frames = 0;
  g_window_start = now;

  // sampled per-section timing: the next window is timed by section every kSectionSampleEvery
  if (g_sections_hook && !g_sections_forced) {
    g_windows++;
    const bool want = g_windows % kSectionSampleEvery == 0;
    if (want != g_sections_enabled) {
      g_sections_enabled = want;
      g_open_count = 0;
      g_sections_hook(want);
    }
  }
}

}  // namespace kperf
