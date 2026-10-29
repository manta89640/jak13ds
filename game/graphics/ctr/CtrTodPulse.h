#pragma once

// (AI-assisted, levellod, Oct 29) config tod_pulse_step's detector: is a time of day palette's
// weight pulsing (a mood light going up and down around a fixed level, Gol and Maia's Citadel's
// update-mood-light on palette 1: 0.875 +- 0.25 every ~128 frames, 71% of its colors) or moving
// with the day (one way for game hours: the title's 20-60x day too)? CPU only, no renderer state
// (test/ctr_render/citadel_test.cpp).
//
// The weight's turning points are found with a dead band (a turn counts once the weight went
// kMinSwing back from its extreme: the rounding of a slow sine to 64ths near its top, 71 72 71 72,
// is no turn). Pulsing: the last three turning points (top, bottom, top or the other way round)
// within kPeriod frames, both swings between kMinPulse and kMaxSwing, and the first and third
// within kMaxDrift of each other (a steady mean). The Oct 17 detector (two direction changes of any size within
// 256 frames) took the title's day palettes for pulsing (one 1/64 step back and forth was enough)
// and held them: the user's "todlag". Weights in 64ths, the sum of r, g and b.

#include "common/common_types.h"

struct CtrTodPulse {
  static constexpr int kMinSwing = 9;    // the dead band: 3/64 on all three channels
  static constexpr int kMinPulse = 24;   // a pulse's swings: at least 8/64 on all three
  static constexpr int kMaxSwing = 120;  // ... at most 40/64 (citadel: 32/64)
  static constexpr int kMaxDrift = 18;   // 6/64 on all three
  static constexpr u64 kPeriod = 320;    // frames for a whole cycle (citadel: ~128, 256 at 60 fps)
  static constexpr u64 kKeep = 512;      // pulsing this long after the last confirmed cycle

  u8 dir = 0;          // the trend: 0 none yet, 1 up, 2 down
  bool started = false;
  int ext = 0;         // the trend's extreme so far (the weight's sum), and its frame
  u64 ext_frame = 0;
  int tp[3] = {0, 0, 0};   // the last turning points, newest first
  u64 tp_frame[3] = {0, 0, 0};
  int tps = 0;             // how many of them there are (up to 3)
  u64 confirmed = 0;       // the frame of the last cycle that looked like a pulse (0: none)

  // the game's weight this frame (only called when it moved: a turn's frame is its extreme's)
  // returns true when this move confirmed a pulse cycle
  bool move(const u8 w[3], u64 frame) {
    const int v = (int)w[0] + (int)w[1] + (int)w[2];
    if (!started) {
      started = true;
      ext = v;
      ext_frame = frame;
      return false;
    }
    if (dir == 0) {
      if (v - ext >= kMinSwing || ext - v >= kMinSwing) {
        dir = v > ext ? 1 : 2;
        ext = v;
        ext_frame = frame;
      }
      return false;
    }
    const bool further = dir == 1 ? v >= ext : v <= ext;
    if (further) {
      ext = v;
      ext_frame = frame;
      return false;
    }
    if ((dir == 1 ? ext - v : v - ext) < kMinSwing) {
      return false;  // (inside the dead band)
    }
    // a turn at the extreme
    tp[2] = tp[1];
    tp_frame[2] = tp_frame[1];
    tp[1] = tp[0];
    tp_frame[1] = tp_frame[0];
    tp[0] = ext;
    tp_frame[0] = ext_frame;
    tps = tps < 3 ? tps + 1 : 3;
    dir = dir == 1 ? 2 : 1;
    ext = v;
    ext_frame = frame;
    if (tps < 3) {
      return false;
    }
    auto absi = [](int x) { return x < 0 ? -x : x; };
    const int s0 = absi(tp[0] - tp[1]), s1 = absi(tp[1] - tp[2]);
    const bool ok = tp_frame[0] - tp_frame[2] <= kPeriod && s0 >= kMinPulse && s1 >= kMinPulse &&
                    s0 <= kMaxSwing && s1 <= kMaxSwing && absi(tp[0] - tp[2]) <= kMaxDrift;
    if (ok) {
      confirmed = frame;
    }
    return ok;
  }
  bool pulsing(u64 frame) const { return confirmed && frame - confirmed < kKeep; }
  // the frame of the newest turning point (0: none)
  u64 last_turn() const { return tps ? tp_frame[0] : 0; }
};
