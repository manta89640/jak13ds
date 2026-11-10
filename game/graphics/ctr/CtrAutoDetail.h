#pragma once

// config.ini auto_detail: one slow controller for the level detail, by rings around the camera.
// Ring 0 (kNearRingMeters) never changes. Over the GPU budget the far ring loses detail first
// (Jak 1: the shrub fade bands beyond ring 0, then the distance from which a tie chunk is drawn by
// its vista copy; else the tie distance limit), then the middle ring (lod_distance, tie_lod_px);
// with time to spare the far ring gains detail above the config (the shrub / tie distances toward
// their _max values). A lever only moves what has something drawn in its place or fades: none
// leaves a hole. CPU only (test/ctr_render/auto_detail_test.cpp).
//
// One position x: x in [0, 1] is the near-to-far range of the shrub / tie distances, x < 0 the
// relief r = -x (one unit per knob that has something to move, in kOrder). The level renderer
// follows x at kSlew per frame, so every distance it sets moves a fade band's width in seconds.

#include <algorithm>
#include <cmath>

namespace ctr_detail {

enum Knob { kTieLodPx, kLodDistance, kTieDistance, kShrubFar, kTieFar, kTieFar2, kKnobs };
// far ring first: the tie limit, Jak 1's levers (shrubs, the ties' vista copies from 250 m, then
// from 150 m), the coarse tfrag (200+ m), the tie LOD error
constexpr Knob kOrder[kKnobs] = {kTieDistance, kShrubFar,    kTieFar,
                                 kTieFar2,     kLodDistance, kTieLodPx};
// nothing nearer than this to the camera changes with the relief (meters)
constexpr float kNearRingMeters = 40.f;
constexpr float kOldTieLodPx = 4.f;
constexpr float kOldLodDistance = 200.f;  // meters
constexpr float kOldTieScale = 1.5f;
constexpr float kSmallTieMeters = 100.f;  // the converter's limit of small ties (x the scale)
// Jak 1's levers at their floor (Jak II's too): shrub distances x this (not under ring 0), tie
// chunks drawn by their vista copies from these distances (meters)
constexpr float kShrubFarK = 0.5f;
constexpr float kTieFarMeters = 250.f;
constexpr float kTieFar2Meters = 150.f;
constexpr float kReliefMax = (float)kKnobs;
// a distance limit's fade band: from the limit to this times it
constexpr float kFadeBand = 1.1f;
// the drawn position follows the controller's by this per frame; the controller waits for it
constexpr float kSlew = 0.01f;
constexpr float kSlewNear = 0.05f;
// GPU ms each Jak 1 lever saves over its unit (host title flight, triangles x 0.164 ms per 1000;
// the others: Controller::kMsPerUnit)
constexpr double kJak1LeverMs[kKnobs] = {
    4.0,   // kTieLodPx (does not move)
    4.0,   // kLodDistance (does not move)
    0.25,  // kTieDistance (does not move)
    0.25,  // kShrubFar
    0.2,   // kTieFar
    4.5};  // kTieFar2

// a distance limit at the relief (0: none): never under the config's one within ring 0
inline float ring_limit(float relief_lim, float config_lim, float near_ring) {
  if (!(relief_lim > 0)) {
    return relief_lim;
  }
  return std::max(relief_lim, config_lim > 0 ? std::min(config_lim, near_ring) : near_ring);
}
// a limit d (0: none) cut to `cap` (0: none) by a far lever, never under ring 0
inline float far_cap(float d, float cap, float near_ring) {
  if (!(cap > 0)) {
    return d;
  }
  return d > 0 ? std::max(std::min(d, cap), std::min(d, near_ring)) : std::max(cap, near_ring);
}
// how much of a chunk at `dn` is drawn with the limit `lim` (0: none)
inline float band_alpha(float dn, float lim) {
  if (!(lim > 0) || dn <= lim) {
    return 1.f;
  }
  return std::max(0.f, (lim * kFadeBand - dn) / (lim * (kFadeBand - 1.f)));
}
// the renderer's position, a frame nearer to the controller's
inline float slew(float now, float target) {
  return now + std::clamp(target - now, -kSlew, kSlew);
}
// a shrub's fade band [x, w] (full detail to x, gone at w) at the shrub lever's factor k: a band
// that starts beyond ring 0 moves toward it (the same shape), one that starts inside stays
inline void shrub_band(float* x, float* w, float k, float near_ring) {
  if (!(k < 1.f) || !(*x > 0) || *x < near_ring) {
    return;
  }
  const float nx = std::max(*x * k, near_ring);
  *w = std::max(*w * nx / *x, nx);
  *x = nx;
}

struct Knobs {
  float tie_lod_px;
  float lod_distance;        // 0: none
  float tie_distance_scale;  // 0: none
  float shrub_far = 1.f;     // shrub fade bands x this (shrub_band)
  float tie_far = 0.f;       // tie chunks by their vista copies from this (meters, 0: never)
};

struct Relief {
  Knobs base{1.f, 0.f, 0.f};
  float from[kKnobs] = {}, to[kKnobs] = {};
  int stage[kKnobs] = {-1, -1, -1, -1, -1, -1};  // the knob's unit of r (-1: none)
  int n = 0;

  // a range starts where the knob begins to cut (none -> the draw distance's equal). Jak 1: no
  // lod_distance (its coarse tfrag is the geomorph's far version) and no tie limit (a small tie
  // beyond it has nothing in its place where a level has no vista tier), its own levers instead.
  // jak2: Jak 1's far levers after Jak II's own knobs (which don't move at its default config)
  void init(float px, float lod, float tie, float draw_distance, bool jak1 = false,
            bool jak2 = false) {
    const bool far_levers = jak1 || jak2;
    base = {px, lod, tie};
    const float draw = draw_distance > 0 ? draw_distance : 2000.f;
    from[kTieLodPx] = px;
    to[kTieLodPx] = kOldTieLodPx;
    from[kLodDistance] = lod > 0 ? lod : draw;
    to[kLodDistance] = jak1 ? from[kLodDistance] : kOldLodDistance;
    from[kTieDistance] = tie > 0 ? tie : draw / kSmallTieMeters;
    to[kTieDistance] = jak1 ? from[kTieDistance] : kOldTieScale;
    from[kShrubFar] = 1.f;
    to[kShrubFar] = far_levers ? kShrubFarK : 1.f;
    from[kTieFar] = draw;
    to[kTieFar] = far_levers ? std::min(draw, kTieFarMeters) : draw;
    from[kTieFar2] = to[kTieFar];
    to[kTieFar2] = far_levers ? std::min(to[kTieFar], kTieFar2Meters) : to[kTieFar];
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
    const float b = k == kTieLodPx      ? base.tie_lod_px
                    : k == kLodDistance ? base.lod_distance
                    : k == kTieDistance ? base.tie_distance_scale
                    : k == kShrubFar    ? 1.f
                                        : 0.f;
    return p > 0 ? from[k] * std::pow(to[k] / from[k], p) : b;
  }
  Knobs at(float r) const {
    const float tie2 = value(kTieFar2, r);
    return {value(kTieLodPx, r), value(kLodDistance, r), value(kTieDistance, r),
            value(kShrubFar, r), tie2 > 0 ? tie2 : value(kTieFar, r)};
  }
};

// The controller, one call per frame. Its load is the GPU's time when the GPU is the limit (it
// lasts kGpuLead past the render thread's build), else at most the target, and the render
// thread's mean build. A slow mean of it (kTau frames) against a dead band around the target:
// above target + kBand it sheds a little every kShedGap frames; below target - kBand for
// kRoomFrames in a row it gives back kRestore, up to the ceiling. A shed puts the ceiling where it
// went (more didn't fit here); the ceiling rises a step per kCeilFrames without a shed, and is
// lifted at a new area. At its floor with the load still over the band it stays there.
struct Controller {
  static constexpr int kBuildFrames = 8;  // the build's mean (a camera cut's rewrite is 1/8 of it)
  static constexpr double kGpuLead = 3.0;
  static constexpr double kTau = 45.0;
  static constexpr double kBand = 3.0;
  static constexpr double kMsPerUnit = 4.0;  // GPU ms one unit of x is taken to save
  static constexpr int kShedGap = 45, kRoomFrames = 240, kCeilFrames = 1800;
  static constexpr float kShedMin = 0.05f, kShedMax = 0.25f, kRestore = 0.1f;

  float x = 0, lo = 0, hi = 0, ceil = 0;
  // GPU ms one unit of the relief saves, by unit (Jak 1: its levers' measured savings)
  double unit_ms[kKnobs] = {kMsPerUnit, kMsPerUnit, kMsPerUnit,
                            kMsPerUnit, kMsPerUnit, kMsPerUnit};
  double mean = 0;
  bool primed = false;
  double lead = 0;  // busy - build, averaged
  long frame = 0, last_shed = -1000000, ceil_frame = 0;
  int room = 0;
  int sheds = 0, restores = 0;  // since the last log
  int sheds_cpu = 0;            // of them for the render thread
  double builds[kBuildFrames] = {};

  float r() const { return std::max(-x, 0.f); }
  // each relief unit at its knob's saving (Jak 1)
  void unit_costs(const Relief& rl, const double* ms) {
    for (int k = 0; k < kKnobs; k++) {
      if (rl.stage[k] >= 0) {
        unit_ms[rl.stage[k]] = ms[k];
      }
    }
  }
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
    primed = false;
    room = 0;
    std::fill(builds, builds + kBuildFrames, 0.0);
  }
  // a new area: what didn't fit before may fit here
  void new_area() {
    ceil = hi;
    ceil_frame = frame;
  }

  // x after shedding `ms` of GPU time, unit by unit at their unit_ms
  float shed_to(double ms, double k) const {
    float nx = x;
    while (ms > 1e-6 && nx > lo + 1e-6f) {
      const int u = nx > 0 ? -1 : std::min((int)std::floor(-nx + 1e-4f), kKnobs - 1);
      const float bottom = std::max(u < 0 ? 0.f : -(float)(u + 1), lo);
      const double per = std::max((u < 0 ? kMsPerUnit : unit_ms[u]) * k, 0.05);
      const float take = std::min(nx - bottom, (float)(ms / per));
      nx -= take;
      ms -= take * per;
      if (take <= 0) {
        break;
      }
    }
    return nx;
  }
  // busy: this frame's GPU ms (active), build: the render thread's (counted false: no
  // measurement, or a level load's frame; build_counts false: a level's first draws); k: 1 at
  // fps_cap 30, 0.5 at 60; drawn_x: the position the levels were drawn with. Returns true when x
  // moved.
  bool step(double busy, double build, bool counted, double target, double k = 1.0,
            bool build_counts = true, float drawn_x = 1e9f) {
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
    bool moved = false;
    const bool caught_up = drawn_x > 1e8f || std::fabs(drawn_x - x) <= kSlewNear;
    if (!caught_up) {
      room = 0;
    } else if (mean > (target + kBand * k) && x > lo && frame - last_shed >= kShedGap) {
      const float s = std::clamp(
          (float)(kShedMin + 0.5 * (mean - target - kBand * k) / (kMsPerUnit * k)), kShedMin,
          kShedMax);
      x = std::max(lo, shed_to(s * kMsPerUnit * k, k));
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
