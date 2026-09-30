#pragma once

/*!
 * @file kperf.h
 * (AI-assisted)
 * Frame timing for the 3DS port: where the time of a game frame goes, reported once a second
 * (log, and the bottom screen on the 3DS).
 *   logic  = kernel dispatch (GOAL code + C kernel) minus render and vsync. With the 3DS render
 *            thread this includes the game waiting for the render thread (sync-path).
 *   render = the renderer's send_chain (drawing the DMA chain)
 *   vsync  = waiting in syncv for the vertical blank
 *   idle   = time outside the kernel dispatch (listener, sleeps)
 * A second line ("perf threads") shows how much time other threads on the game's core took
 * (IOP / overlord, sound ticks): that time is included in the game's numbers, since those threads
 * preempt the game thread.
 */

#include "common/common_types.h"

namespace kperf {

enum class Cat { DISPATCH = 0, RENDER = 1, VSYNC = 2, COUNT = 3 };

void add(Cat cat, u64 us);
//! Call once per kernel dispatch (the game's frame). Prints the stats once a second.
void frame_done();
u64 now_us();

//! A cheap clock (the 3DS system tick, 268 MHz; steady_clock ns elsewhere).
u64 ticks();
double ticks_to_ms(u64 t);

//! Time used by other threads that share the game's core, added from those threads.
enum class Thread { IOP = 0, SOUND = 1, COUNT = 2 };
void add_thread(Thread t, u64 ticks, u32 wakeups);
//! Values reported as they are (last value set, from any thread).
enum class Gauge { SOUND_HANDLERS = 0, COUNT = 1 };
void set_gauge(Gauge g, u32 value);

//! Event counters, reported per frame (game thread only).
enum class Counter { SUSPEND = 0, SUSPEND_BYTES = 1, COUNT = 2 };
extern u64 g_counters[(int)Counter::COUNT];
inline void count(Counter c, u64 n) {
  g_counters[(int)c] += n;
}

//! Named sections from the game's with-profiler blocks (pc-prof). Inclusive time per name is
//! reported once a second with the frame stats (top sections by ms per frame, with the number of
//! times they ran per frame).
//! Off by default (the timing costs time too, see docs/3ds-port/performance.md): enabled by the
//! flag file sdmc:/3ds/jak1/perf_sections on the 3DS, OPENGOAL_PERF_SECTIONS=1 on PC.
extern bool g_sections_enabled;
void init_sections();
void section_begin(const char* name);
void section_end();

struct SectionScope {
  explicit SectionScope(const char* name) : on(g_sections_enabled) {
    if (on) {
      section_begin(name);
    }
  }
  ~SectionScope() {
    if (on) {
      section_end();
    }
  }
  bool on;
};

struct Scope {
  explicit Scope(Cat c) : cat(c), start(now_us()) {}
  ~Scope() { add(cat, now_us() - start); }
  Cat cat;
  u64 start;
};

}  // namespace kperf
