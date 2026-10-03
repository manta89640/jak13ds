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

#include "fmt/format.h"

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
u64 g_totals[(int)Cat::COUNT] = {};        // this second (the status window)
u64 g_totals_frame[(int)Cat::COUNT] = {};  // (AI-assisted) this frame only (hitch_check)
u64 g_frames = 0;
u64 g_window_start = 0;
// (AI-assisted) The log window: the three perf lines every kLogWindows status windows, over all
// of them (3DS: 5 s; every line is SD card traffic there), the bottom screen every second.
#ifdef __3DS__
constexpr int kLogWindows = 5;
#else
constexpr int kLogWindows = 1;
#endif
u64 g_log_totals[(int)Cat::COUNT] = {};
u64 g_log_frames = 0;
u64 g_log_us = 0;
int g_log_windows = 0;
bool g_log_sampled = false;  // one of its seconds had sampled section timing (slower)
#ifdef __3DS__
unsigned g_heap_kb = 0;  // mallinfo once per log window (it walks the free lists)
#endif

struct Section {
  std::string name;
  u64 ticks = 0;
  u64 count = 0;
  u64 frame_ticks = 0;  // (AI-assisted) this frame only: the long frame dump (frame_done)
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
u64 g_rpc_wait_frame[kRpcChannels];  // (AI-assisted) this frame only
u64 g_rpc_last_busy[kRpcChannels];
// (AI-assisted) long frames: the frame's own sections, RPC waits and IOP time are logged
// ("[hitch]"), so a freeze names what it was. Section timing must be on (perf_sections = on).
constexpr double kHitchMs = 100.0;
u64 g_last_frame_us = 0;
u64 g_frame_index = 0;
std::atomic<u64> g_iop_ticks_total{0};
u64 g_iop_ticks_mark = 0;
// (AI-assisted) at most one [hitch] line a second: long frames that follow each other (or come
// within the second) are one line, the longest one's, with how many there were
constexpr u64 kHitchLineUs = 1000000;
struct HitchGroup {
  int count = 0;
  double total_ms = 0;
  double worst_ms = 0;
  u64 first_frame = 0;
  u64 since_us = 0;
  std::string worst;  // the longest one's line
};
HitchGroup g_hitch;
u64 g_hitch_line_us = 0;

// sampled per-section timing (3DS, perf_sections off)
constexpr int kSectionSampleEvery = 60;  // (AI-assisted) status windows (about seconds); was 15
void (*g_sections_hook)(bool) = nullptr;
bool g_sections_forced = false;  // config.ini perf_sections / environment: always on
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
  if (t == Thread::IOP) {
    g_iop_ticks_total.fetch_add(dt, std::memory_order_relaxed);
  }
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
    g_rpc_wait_frame[channel] += now - last;
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
  if (ctr_config_bool("perf_sections", 0)) {  // config.ini
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
  s.frame_ticks += now - o.start;
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
  g_totals_frame[(int)cat] += us;
}

// (AI-assisted) log what a long frame spent its time on
void hitch_check(u64 now) {
  const u64 last = g_last_frame_us;
  g_last_frame_us = now;
  g_frame_index++;
  const u64 iop_total = g_iop_ticks_total.load(std::memory_order_relaxed);
  const u64 iop_frame = iop_total - g_iop_ticks_mark;
  g_iop_ticks_mark = iop_total;
  const double ms = last ? (now - last) / 1000.0 : 0.0;
  const bool hitch = ms >= kHitchMs;
  if (hitch) {
    if (!g_hitch.count) {
      g_hitch.first_frame = g_frame_index;
      g_hitch.since_us = now;
    }
    g_hitch.count++;
    g_hitch.total_ms += ms;
  }
  if (hitch && ms > g_hitch.worst_ms) {
    g_hitch.worst_ms = ms;
    std::string line = fmt::format(
        "[hitch] frame {}: {:.0f} ms; game dispatch {:.0f} ms (render {:.0f}, vsync {:.0f}); iop "
        "{:.0f} ms",
        g_frame_index, ms, g_totals_frame[(int)Cat::DISPATCH] / 1000.0,
        g_totals_frame[(int)Cat::RENDER] / 1000.0, g_totals_frame[(int)Cat::VSYNC] / 1000.0,
        ticks_to_ms(iop_frame));
    for (int ch = 0; ch < kRpcChannels; ch++) {
      if (g_rpc_wait_frame[ch]) {
        line += fmt::format("; waiting for rpc #{} {:.0f} ms", ch, ticks_to_ms(g_rpc_wait_frame[ch]));
      }
    }
    if (g_totals_frame[(int)Cat::WAIT] >= 1000) {
      line += fmt::format("; waiting for the render thread {:.0f} ms",
                          g_totals_frame[(int)Cat::WAIT] / 1000.0);
    }
    if (g_sections_enabled) {
      std::vector<const Section*> top;
      for (auto& [k, v] : g_sections) {
        if (v.frame_ticks) {
          top.push_back(&v);
        }
      }
      std::sort(top.begin(), top.end(),
                [](auto* a, auto* b) { return a->frame_ticks > b->frame_ticks; });
      line += "; sections:";
      for (size_t i = 0; i < top.size() && i < 14; i++) {
        line += fmt::format(" {} {:.1f}", top[i]->name, ticks_to_ms(top[i]->frame_ticks));
      }
    } else {
      line += " (perf_sections = on in config.ini for the sections)";
    }
    g_hitch.worst = std::move(line);
  }
  // the long frames since the last line: once they stop, or after a second of them
  if (g_hitch.count && now - g_hitch_line_us >= kHitchLineUs &&
      (!hitch || now - g_hitch.since_us >= kHitchLineUs)) {
    if (g_hitch.count > 1) {
      g_hitch.worst += fmt::format("; the longest of {} long frames since frame {} ({:.0f} ms)",
                                   g_hitch.count, g_hitch.first_frame, g_hitch.total_ms);
    }
    lg::warn("{}", g_hitch.worst);
    g_hitch = HitchGroup();
    g_hitch_line_us = now;
  }
  for (auto& w : g_rpc_wait_frame) {
    w = 0;
  }
  for (auto& t : g_totals_frame) {
    t = 0;
  }
  if (g_sections_enabled) {
    for (auto& [k, v] : g_sections) {
      v.frame_ticks = 0;
    }
  }
}

// the per-section lines, over `frames` frames; clears the sections' counts
void report_sections(double frames) {
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
  // (nothing timed: sampled timing was off)
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

void frame_done() {
  u64 now = now_us();
  hitch_check(now);
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
  // (AI-assisted) this second: on the bottom screen, and into the log window
  const bool sampled = g_sections_enabled && !g_sections_forced;
#ifdef __3DS__
  {
    const double frames = (double)g_frames;
    const double dispatch = g_totals[(int)Cat::DISPATCH] / 1000.0 / frames;
    const double render = g_totals[(int)Cat::RENDER] / 1000.0 / frames;
    const double vsync = g_totals[(int)Cat::VSYNC] / 1000.0 / frames;
    const double wait = g_totals[(int)Cat::WAIT] / 1000.0 / frames;
    if (!g_heap_kb) {
      g_heap_kb = (unsigned)(mallinfo().uordblks / 1024);
    }
    char status[160];
    snprintf(status, sizeof(status),
             "%4.1f fps  logic %5.1f  wait %5.1f\nrender %5.1f  vsync %5.1f  idle %5.1f\n"
             "(ms per frame)  heap %u KB",
             frames * 1e6 / (double)elapsed, dispatch - render - vsync - wait, wait, render,
             vsync, elapsed / 1000.0 / frames - dispatch, g_heap_kb);
    ctr_console_status(status);
  }
#endif
  for (int i = 0; i < (int)Cat::COUNT; i++) {
    g_log_totals[i] += g_totals[i];
    g_totals[i] = 0;
  }
  g_log_frames += g_frames;
  g_log_us += elapsed;
  g_log_windows++;
  g_log_sampled = g_log_sampled || sampled;
  const double window_frames = (double)g_frames;
  g_frames = 0;
  g_window_start = now;

  if (g_log_windows >= kLogWindows) {
    const double frames = (double)g_log_frames;
    const double dispatch = g_log_totals[(int)Cat::DISPATCH] / 1000.0 / frames;
    const double render = g_log_totals[(int)Cat::RENDER] / 1000.0 / frames;
    const double vsync = g_log_totals[(int)Cat::VSYNC] / 1000.0 / frames;
    const double wait = g_log_totals[(int)Cat::WAIT] / 1000.0 / frames;
    const double logic = dispatch - render - vsync - wait;
    const double idle = g_log_us / 1000.0 / frames - dispatch;
    const double fps = frames * 1e6 / (double)g_log_us;
    char line[160];
#ifdef __3DS__
    g_heap_kb = (unsigned)(mallinfo().uordblks / 1024);
    // (AI-assisted) "wait" after "idle": the fields before it are as they were (perf_report.py)
    snprintf(line, sizeof(line),
             "%4.1f fps  logic %5.1f  render %5.1f vsync %5.1f  idle %5.1f ms  wait %5.1f ms  heap "
             "%u KB",
             fps, logic, render, vsync, idle, wait, g_heap_kb);
#else
    snprintf(line, sizeof(line),
             "%4.1f fps  logic %5.1f  render %5.1f  vsync %5.1f  idle %5.1f ms  wait %5.1f ms",
             fps, logic, render, vsync, idle, wait);
#endif
    lg::info("perf: {}{}", line, g_log_sampled ? " (sampled section timing on: slower)" : "");

    // other threads on the game's core
    {
      const double secs = g_log_us / 1e6;
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
    // perf_sections = on: the sections over the whole log window
    if (!sampled && !g_sections.empty()) {
      report_sections(frames);
    }
    for (auto& t : g_log_totals) {
      t = 0;
    }
    g_log_frames = 0;
    g_log_us = 0;
    g_log_windows = 0;
    g_log_sampled = false;
  }
  // a sampled second: its sections now
  if (sampled) {
    report_sections(window_frames);
  }

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
