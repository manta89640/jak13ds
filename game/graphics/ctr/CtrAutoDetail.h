#pragma once

// config.ini auto_detail: one slow controller for the level detail, by rings around the camera.
// Ring 0 (kNearRingMeters) never changes. Over the GPU budget the far ring loses detail first
// (the tie distance limit), then the middle ring (lod_distance, tie_lod_px); with time to spare
// the far ring gains detail above the config (the shrub / tie distances toward their _max values).
// CPU only (test/ctr_render/auto_detail_test.cpp).
//
// One position x: x in [0, 1] is the near-to-far range of the shrub / tie distances, x < 0 the
// relief r = -x (one unit per knob that has something to move, in kOrder).

#include <algorithm>
#include <cmath>

namespace ctr_detail {

enum Knob { kTieLodPx, kLodDistance, kTieDistance, kKnobs };
// far ring first: the tie limit (small ties 150+ m), the coarse tfrag (200+ m), the tie LOD error
constexpr Knob kOrder[kKnobs] = {kTieDistance, kLodDistance, kTieLodPx};
// nothing nearer than this to the camera changes with the relief (meters)
constexpr float kNearRingMeters = 40.f;
constexpr float kOldTieLodPx = 4.f;
constexpr float kOldLodDistance = 200.f;  // meters
constexpr float kOldTieScale = 1.5f;
// Jak 1's relief takes the tie limit to the converter's own distances (small ties 100 m)
constexpr float kJak1ReliefTieScale = 1.f;
constexpr float kSmallTieMeters = 100.f;  // the converter's limit of small ties (x the scale)
constexpr float kReliefMax = (float)kKnobs;

// a distance limit at the relief (0: none): never under the config's one within ring 0
inline float ring_limit(float relief_lim, float config_lim, float near_ring) {
  if (!(relief_lim > 0)) {
    return relief_lim;
  }
  return std::max(relief_lim, config_lim > 0 ? std::min(config_lim, near_ring) : near_ring);
}

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

  // a range starts where the knob begins to cut (none -> the draw distance's equal). Jak 1: no
  // lod_distance (its coarse tfrag is the geomorph's far version), the tie limit down to x1
  void init(float px, float lod, float tie, float draw_distance, bool jak1 = false) {
    base = {px, lod, tie};
    from[kTieLodPx] = px;
    to[kTieLodPx] = kOldTieLodPx;
    from[kLodDistance] = lod > 0 ? lod : (draw_distance > 0 ? draw_distance : 2000.f);
    to[kLodDistance] = jak1 ? from[kLodDistance] : kOldLodDistance;
    from[kTieDistance] =
        tie > 0 ? tie : (draw_distance > 0 ? draw_distance : 2000.f) / kSmallTieMeters;
    to[kTieDistance] = jak1 ? kJak1ReliefTieScale : kOldTieScale;
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

// The controller, one call per frame. Its load is the GPU's time when the GPU is the limit (it
// lasts kGpuLead past the render thread's build), else at most the target, and the render
// thread's mean build. A slow mean of it (kTau frames) against a dead band around the target:
// above target + kBand it sheds a little every kShedGap frames; below target - kBand for
// kRoomFrames in a row it gives back kRestore, up to the ceiling. A shed puts the ceiling where it
// went (more didn't fit here); the ceiling rises a step per kCeilFrames without a shed, and is
// lifted at a new area. Urgent: the mean misses the frame (changes may land in view).
struct Controller {
  static constexpr int kBuildFrames = 8;  // the build's mean (a camera cut's rewrite is 1/8 of it)
  static constexpr double kGpuLead = 3.0;
  static constexpr double kTau = 45.0;
  static constexpr double kBand = 3.0;
  static constexpr double kUrgent = 34.0;
  static constexpr double kMsPerUnit = 4.0;  // GPU ms one unit of x is taken to save
  static constexpr int kShedGap = 45, kRoomFrames = 240, kCeilFrames = 1800;
  static constexpr float kShedMin = 0.05f, kShedMax = 0.25f, kRestore = 0.1f;

  float x = 0, lo = 0, hi = 0, ceil = 0;
  double mean = 0;
  bool primed = false;
  bool urgent = false;
  double lead = 0;  // busy - build, averaged
  long frame = 0, last_shed = -1000000, ceil_frame = 0;
  int room = 0;
  int sheds = 0, restores = 0;  // since the last log
  int sheds_cpu = 0;            // of them for the render thread
  double builds[kBuildFrames] = {};

  float r() const { return std::max(-x, 0.f); }
  void range(float lo_, float hi_) {
    lo = lo_;
    hi = hi_;
    x = std::clamp(0.f, lo, hi);
    ceil = hi;
  }
  // back at the config values, the means started again
  void reset() {
    x = std::clamp(0.f, lo, hi);
    ceil = hi;
    mean = lead = 0;
    primed = urgent = false;
    room = 0;
    std::fill(builds, builds + kBuildFrames, 0.0);
  }
  // a new area: what didn't fit before may fit here
  void new_area() {
    ceil = hi;
    ceil_frame = frame;
  }

  // busy: this frame's GPU ms (active), build: the render thread's (counted false: no
  // measurement, or a level load's frame; build_counts false: a level's first draws); k: 1 at
  // fps_cap 30, 0.5 at 60. Returns true when x moved.
  bool step(double busy, double build, bool counted, double target, double k = 1.0,
            bool build_counts = true) {
    frame++;
    if (!counted) {
      return false;
    }
    lead += (busy - build - lead) * 0.25;
    const bool gpu_limit = lead >= kGpuLead * k;
    if (!gpu_limit) {
      busy = std::min(busy, target);
    }
    builds[frame % kBuildFrames] = build_counts ? build : 0.0;
    double cpu = 0;
    for (double b : builds) {
      cpu += b;
    }
    cpu /= kBuildFrames;
    const double load = std::max(busy, cpu);
    if (!primed) {
      mean = std::min(load, target);
      primed = true;
    }
    mean += (load - mean) / kTau;
    urgent = x < 0 && mean > kUrgent * k;
    bool moved = false;
    if (mean > (target + kBand * k) && x > lo && frame - last_shed >= kShedGap) {
      const float s = std::clamp(
          (float)(kShedMin + 0.5 * (mean - target - kBand * k) / (kMsPerUnit * k)), kShedMin,
          kShedMax);
      x = std::max(lo, x - s);
      ceil = std::min(ceil, x);
      last_shed = ceil_frame = frame;
      sheds++;
      sheds_cpu += cpu > busy;
      room = 0;
      moved = true;
    } else if (mean < target - kBand * k) {
      if (++room >= kRoomFrames) {
        room = 0;
        const float top = std::min(hi, ceil);
        if (x < top) {
          x = std::min(top, x + kRestore);
          restores++;
          moved = true;
        }
      }
    } else {
      room = 0;
    }
    if (ceil < hi && frame - ceil_frame >= kCeilFrames) {
      ceil = std::min(hi, ceil + kRestore);
      ceil_frame = frame;
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
