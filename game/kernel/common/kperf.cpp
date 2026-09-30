/*!
 * @file kperf.cpp
 * (AI-assisted)
 * See kperf.h.
 */

#include "kperf.h"

#include <chrono>
#include <cstdio>

#include "common/log/log.h"

#ifdef __3DS__
#include <malloc.h>

#include "platform/3ds/port/ctr_port.h"
#endif

namespace kperf {
namespace {
u64 g_totals[(int)Cat::COUNT] = {};
u64 g_frames = 0;
u64 g_window_start = 0;
}  // namespace

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
  lg::info("perf: {}", line);
  for (auto& t : g_totals) {
    t = 0;
  }
  g_frames = 0;
  g_window_start = now;
}

}  // namespace kperf
