#pragma once

/*!
 * @file kperf.h
 * (AI-assisted)
 * Frame timing for the 3DS port: where the time of a game frame goes, reported once a second
 * (log, and the bottom screen on the 3DS).
 *   logic  = kernel dispatch (GOAL code + C kernel) minus render and vsync
 *   render = the renderer's send_chain (drawing the DMA chain)
 *   vsync  = waiting in syncv for the vertical blank
 *   idle   = time outside the kernel dispatch (listener, sleeps)
 */

#include "common/common_types.h"

namespace kperf {

enum class Cat { DISPATCH = 0, RENDER = 1, VSYNC = 2, COUNT = 3 };

void add(Cat cat, u64 us);
//! Call once per kernel dispatch (the game's frame). Prints the stats once a second.
void frame_done();
u64 now_us();

struct Scope {
  explicit Scope(Cat c) : cat(c), start(now_us()) {}
  ~Scope() { add(cat, now_us() - start); }
  Cat cat;
  u64 start;
};

}  // namespace kperf
