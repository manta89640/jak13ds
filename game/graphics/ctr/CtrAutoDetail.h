#pragma once

// config.ini auto_detail's relief: while the GPU is over its budget, the tie LOD switch, the tfrag
// distance LOD and the tie distance limit move from their config values (the PS2's) toward the
// older 3DS values, one knob after the other, the least visible first, and go back once there is
// room again. CPU only (test/ctr_render/auto_detail_test.cpp).
//
// One position s for all of auto_detail: s in [0, 1] is the near-to-far range of the shrub / tie
// distances (configs whose _max values are above them), s < 0 is the relief r = -s (one unit per
// knob that has something to move, in kOrder).

#include <algorithm>
#include <cmath>

namespace ctr_detail {

enum Knob { kTieLodPx, kLodDistance, kTieDistance, kKnobs };
// tie_lod_px stays at or above the PS2's detail (the PS2 far-plane floor keeps geometry 0 inside
// it); the coarse tfrag beyond lod_distance changes far shapes; the tie limit removes small ties
constexpr Knob kOrder[kKnobs] = {kTieLodPx, kLodDistance, kTieDistance};
constexpr float kOldTieLodPx = 4.f;
constexpr float kOldLodDistance = 200.f;  // meters
constexpr float kOldTieScale = 1.5f;
constexpr float kSmallTieMeters = 100.f;  // the converter's limit of small ties (x the scale)
constexpr float kReliefMax = (float)kKnobs;

struct Knobs {
  float tie_lod_px;
  float lod_distance;        // 0: none
  float tie_distance_scale;  // 0: none
};

struct Relief {
  Knobs base{1.f, 0.f, 0.f};
  float from[kKnobs] = {}, to[kKnobs] = {};
  int stage[kKnobs] = {-1, -1, -1};  // the knob's unit of r (-1: nothing to move)
  int n = 0;

  // the start of a range is where the knob begins to cut (none -> the draw distance's equal)
  void init(float px, float lod, float tie, float draw_distance) {
    base = {px, lod, tie};
    from[kTieLodPx] = px;
    to[kTieLodPx] = kOldTieLodPx;
    from[kLodDistance] = lod > 0 ? lod : (draw_distance > 0 ? draw_distance : 2000.f);
    to[kLodDistance] = kOldLodDistance;
    from[kTieDistance] =
        tie > 0 ? tie : (draw_distance > 0 ? draw_distance : 2000.f) / kSmallTieMeters;
    to[kTieDistance] = kOldTieScale;
    n = 0;
    for (Knob k : kOrder) {
      const bool moves = k == kTieLodPx ? from[k] < to[k] : from[k] > to[k];
      stage[k] = moves ? n++ : -1;
    }
  }
  // how far knob k has moved at relief r: 0 its config value, 1 the old value
  float part(int k, float r) const {
    return stage[k] < 0 ? 0.f : std::clamp(r - (float)stage[k], 0.f, 1.f);
  }
  float value(int k, float r) const {
    const float p = part(k, r);
    const float b = k == kTieLodPx ? base.tie_lod_px
                    : k == kLodDistance ? base.lod_distance
                                        : base.tie_distance_scale;
    return p > 0 ? from[k] * std::pow(to[k] / from[k], p) : b;
  }
  Knobs at(float r) const {
    return {value(kTieLodPx, r), value(kLodDistance, r), value(kTieDistance, r)};
  }
};

// The relief's controller, one call per frame with the GPU's busy time. Toward the old values
// within a few frames of an overload (kFastFrames average over the target; over it by less than
// kUrgentMargin, two such windows in a row and kSettleFrames apart, so a step's effect shows
// before the next), back after
// kRoomFrames with room, as far as the room is worth at kMsPerUnit; a step toward the old values
// soon after a step back doubles the wait before the next step back (no back and forth at the
// edge of the budget). Only a GPU that is the limit counts as over: its time lasts kGpuLead past
// the render thread's build on average; else the render thread is the limit (or, in Azahar, the
// commands run as they are handed over and the time follows the build) and a frame counts at most
// as the target: fewer triangles would hardly help, so it never steps toward the old values.
// The render thread's own build counts as well, averaged over kBuildFrames (the title's time of day
// lands every fourth frame; a camera cut rewrites what comes in view once): over the target it
// steps the same way, never urgent (fewer vertices and draws shorten the time of day rewrites and
// the list walk; the in-view holds stay).
struct Controller {
  static constexpr int kFastFrames = 4;
  static constexpr int kSettleFrames = 24;
  static constexpr int kBuildFrames = 8;  // the build's mean (a camera cut's rewrite is 1/8 of it)
  static constexpr int kRoomFrames = 30;
  static constexpr double kMsPerUnit = 4.0;  // GPU ms one unit of r is taken to save
  static constexpr double kRoomMargin = 2.0;   // ms under the target, scaled like it
  static constexpr double kUrgentMargin = 3.0;  // ms over it: changes apply in view too
  static constexpr double kGpuLead = 3.0;
  static constexpr float kStepMin = 0.04f, kStepMax = 0.1f, kStepBack = 0.05f, kBackMax = 0.5f;
  static constexpr int kHoldMin = 60, kHoldMax = 480, kBackoffFrames = 90, kCalmFrames = 300;

  float r = 0;
  int n = 0;
  double lead = 0;  // busy - build, averaged
  bool urgent = false;
  long frame = 0;
  int hold = kHoldMin;
  long last_toward = -1000000, last_back = -1000000;
  double fast_sum = 0, fast_gpu = 0, fast_cpu = 0;
  int fast_n = 0, fast_over = 0;
  bool last_over = false;  // the window before was over the target
  double room_sum = 0;
  int room_n = 0, room_over = 0;
  int towards = 0, backs = 0;  // steps since the last log
  int towards_cpu = 0;         // of them for the render thread
  double builds[kBuildFrames] = {};

  // back at the config values, the windows started again
  void reset() {
    r = 0;
    urgent = last_over = false;
    fast_sum = fast_gpu = fast_cpu = room_sum = lead = 0;
    fast_n = fast_over = room_n = room_over = 0;
    std::fill(builds, builds + kBuildFrames, 0.0);
    last_back = frame;
  }

  // busy: this frame's GPU ms (active), build: the render thread's, its time of day included
  // (counted false: no measurement, or a level load's frame; build_counts false: a level's first
  // draws, whose rewrites of everything don't count for the render thread); k: 1 at fps_cap 30,
  // 0.5 at 60. Returns true when r moved.
  bool step(double busy, double build, bool counted, double target, double k = 1.0,
            bool build_counts = true) {
    frame++;
    if (!counted) {
      return false;
    }
    lead += (busy - build - lead) * 0.25;
    if (lead < kGpuLead * k) {
      busy = std::min(busy, target);
    }
    // (the mean of the last kBuildFrames, the missing ones 0: a start doesn't count one frame alone)
    builds[frame % kBuildFrames] = build_counts ? build : 0.0;
    double cpu = 0;
    for (double b : builds) {
      cpu += b;
    }
    cpu /= kBuildFrames;
    const double load = std::max(busy, cpu);
    fast_sum += load;
    fast_gpu += busy;
    fast_cpu += cpu;
    fast_n++;
    fast_over += busy > 33.0 * k;
    room_sum += load;
    room_n++;
    room_over += load > target;
    bool moved = false;
    if (fast_n >= kFastFrames) {
      const double avg = fast_sum / fast_n;
      urgent = fast_gpu / fast_n > target + kUrgentMargin * k || fast_over >= 2;
      const bool over = avg > target;
      if (over && r < (float)n &&
          (urgent || (last_over && frame - last_toward >= kSettleFrames))) {
        const float s = std::clamp((float)(kStepMin + 0.02 * (avg - target) / k), kStepMin,
                                   kStepMax);
        r = std::min((float)n, r + s);
        if (frame - last_back < kBackoffFrames) {
          hold = std::min(hold * 2, kHoldMax);
        }
        last_toward = frame;
        towards++;
        towards_cpu += fast_cpu > fast_gpu;
        moved = true;
        room_sum = 0;
        room_n = room_over = 0;
      }
      last_over = over;
      fast_sum = fast_gpu = fast_cpu = 0;
      fast_n = fast_over = 0;
    }
    if (room_n >= kRoomFrames) {
      const double room = target - kRoomMargin * k - room_sum / room_n;
      if (r > 0 && room > 0 && room_over <= 1 && frame - last_toward >= hold) {
        r = std::max(0.f, r - std::clamp((float)(room / (kMsPerUnit * k)), kStepBack, kBackMax));
        last_back = frame;
        backs++;
        moved = true;
      }
      if (frame - last_toward > kCalmFrames) {
        hold = kHoldMin;
      }
      room_sum = 0;
      room_n = room_over = 0;
    }
    if (r <= 0) {
      urgent = false;
    }
    return moved;
  }
};

// time per knob at its config value, between, at the old value (ms, since the last log)
struct KnobTime {
  double ms[kKnobs][3] = {};
  void add(const Relief& rl, float r, double dt) {
    for (int k = 0; k < kKnobs; k++) {
      const float p = rl.part(k, r);
      ms[k][p <= 0 ? 0 : (p >= 1 ? 2 : 1)] += dt;
    }
  }
};

}  // namespace ctr_detail
