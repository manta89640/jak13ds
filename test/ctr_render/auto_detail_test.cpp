// auto_detail (game/graphics/ctr/CtrAutoDetail.h): the knobs' ranges, ring 0, and the controller
// against a model of the GPU's busy time per frame (a view's time at the config values plus or
// minus what the far ring's detail costs at the position, reached with a lag, plus noise); the
// hardware log hw-nov01e replayed through the old controller and the new one.
//   c++ -std=c++17 -O2 -I. test/ctr_render/auto_detail_test.cpp -o /tmp/auto_detail_test && /tmp/auto_detail_test [-v]
#include "game/graphics/ctr/CtrAutoDetail.h"
#include "check.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

using namespace ctr_detail;

namespace {
bool verbose = false;

bool near(float a, float b, float e = 1e-3f) {
  return std::fabs(a - b) <= e;
}

void test_ranges() {
  Relief rl;
  rl.init(1.f, 0.f, 0.f, 500.f);  // the PS2 values, draw_distance 500 (Jak II's rules)
  check(rl.n == 3, "PS2 values: three knobs move");
  check(rl.stage[kTieDistance] == 0 && rl.stage[kLodDistance] == 1 && rl.stage[kTieLodPx] == 2,
        "order: the far ring first (tie limit, lod_distance, tie_lod_px)");
  Knobs k = rl.at(0.f);
  check(k.tie_lod_px == 1.f && k.lod_distance == 0.f && k.tie_distance_scale == 0.f,
        "r 0: the config values exactly");
  k = rl.at(0.0001f);
  check(near(k.tie_distance_scale, 5.f, 0.01f), "the tie limit starts at the draw distance's equal");
  k = rl.at(1.f);
  check(near(k.tie_distance_scale, 1.5f) && k.lod_distance == 0.f && k.tie_lod_px == 1.f,
        "r 1: the tie limit at x1.5, the others untouched");
  k = rl.at(2.f);
  check(near(k.lod_distance, 200.f) && k.tie_lod_px == 1.f, "r 2: lod_distance 200");
  k = rl.at(3.f);
  check(near(k.tie_lod_px, 4.f) && near(k.lod_distance, 200.f) && near(k.tie_distance_scale, 1.5f),
        "r 3: the old values");
  for (float r = 0; r < 3.f; r += 0.01f) {
    const Knobs a = rl.at(r), b = rl.at(r + 0.01f);
    check(b.tie_lod_px >= a.tie_lod_px, "tie_lod_px never goes back with r");
    check(!(a.lod_distance > 0) || (b.lod_distance > 0 && b.lod_distance <= a.lod_distance),
          "lod_distance never grows with r");
    check(!(a.tie_distance_scale > 0) ||
              (b.tie_distance_scale > 0 && b.tie_distance_scale <= a.tie_distance_scale),
          "the tie limit never grows with r");
  }
  check(rl.stage[kShrubFar] < 0 && rl.stage[kTieFar] < 0 && rl.stage[kTieFar2] < 0,
        "Jak II's rules: none of Jak 1's levers");
  Relief j1;
  j1.init(4.f, 0.f, 1.5f, 500.f, true);  // Jak 1's defaults
  check(j1.n == 3 && j1.stage[kShrubFar] == 0 && j1.stage[kTieFar] == 1 &&
            j1.stage[kTieFar2] == 2 && j1.stage[kTieDistance] < 0 && j1.stage[kLodDistance] < 0 &&
            j1.stage[kTieLodPx] < 0,
        "Jak 1: shrubs, the ties' vista copies from 250 m, then from 150 m; no tie limit");
  {
    const Knobs a = j1.at(0.f), z = j1.at(3.f);
    check(a.shrub_far == 1.f && a.tie_far == 0.f && a.tie_distance_scale == 1.5f,
          "Jak 1 r 0: the config");
    check(near(z.shrub_far, kShrubFarK) && near(z.tie_far, kTieFar2Meters) &&
              z.tie_distance_scale == 1.5f,
          "Jak 1 r 3: every lever at its floor, the tie limit the config's");
    check(near(j1.at(1.f).shrub_far, kShrubFarK) && j1.at(1.f).tie_far == 0.f,
          "Jak 1 r 1: shrubs before the ties");
    check(near(j1.at(2.f).tie_far, kTieFarMeters), "Jak 1 r 2: ties from 250 m before 150 m");
    check(near(j1.at(1.5f).tie_far, 500.f * std::sqrt(kTieFarMeters / 500.f), 0.5f),
          "ties: halfway in the ratio of the distances");
    for (float r = 0; r < 3.f; r += 0.01f) {
      const Knobs p = j1.at(r), q = j1.at(r + 0.01f);
      check(q.shrub_far <= p.shrub_far, "shrubs never come back with r");
      check(!(p.tie_far > 0) || (q.tie_far > 0 && q.tie_far <= p.tie_far + 1e-3f),
            "the ties' distance never grows with r");
      check(q.tie_distance_scale == 1.5f && q.tie_lod_px == 4.f && q.lod_distance == 0.f,
            "Jak 1: nothing else moves");
    }
  }
  Relief j1old;
  j1old.init(1.f, 0.f, 0.f, 500.f, true);  // Jak 1 with the PS2 values in config.ini
  check(j1old.n == 4 && j1old.stage[kTieDistance] < 0 && j1old.stage[kTieFar2] == 2 &&
            j1old.stage[kTieLodPx] == 3,
        "Jak 1 at the PS2 values: its levers, then tie_lod_px");
  for (float r = 0; r <= 3.f; r += 0.25f) {
    check(j1old.at(r).lod_distance == 0.f && j1old.at(r).tie_distance_scale == 0.f,
          "Jak 1: lod_distance and the tie limit stay the config's");
  }
  Relief j2;
  j2.init(4.f, 200.f, 1.5f, 500.f);  // Jak II's defaults: already the old values
  check(j2.n == 0, "Jak II's defaults: nothing to move");
  k = j2.at(3.f);
  check(k.tie_lod_px == 4.f && k.lod_distance == 200.f && k.tie_distance_scale == 1.5f,
        "Jak II: the config values at any r");
}

// ring 0: what the config draws within kNearRingMeters (its nearest point), the relief draws too,
// with the same version (tie chunk limits, tie LOD switch distances; meters)
void test_near_ring() {
  const float R = kNearRingMeters;
  const float small[] = {5.f, 20.f, 39.f, 40.f, 60.f, 100.f, 300.f};
  for (float cfg : small) {
    for (float rel = 0.5f; rel <= cfg; rel *= 1.3f) {
      const float lim = ring_limit(rel, cfg, R);
      check(lim >= std::min(cfg, R) - 1e-4f, "ring 0: a relief limit never under the config's there");
      check(lim <= std::max(rel, std::min(cfg, R)) + 1e-4f, "ring 0: no more than the config's");
      for (float d = 0; d < R; d += 0.5f) {
        check((d <= cfg) == (d <= lim) || d > cfg, "ring 0: drawn as at the config");
      }
    }
    check(ring_limit(0.f, cfg, R) == 0.f, "no limit stays no limit");
  }
  check(ring_limit(10.f, 0.f, R) == R, "a config without a limit: the relief's from ring 0 on");
  // the far caps: what the config draws within ring 0 stays
  for (float d : {0.f, 5.f, 30.f, 39.f, 40.f, 60.f, 150.f, 300.f, 450.f}) {
    for (float cap : {0.f, 10.f, 40.f, 120.f, 250.f, 1000.f}) {
      const float lim = far_cap(d, cap, R);
      check(!(d > 0) || lim >= std::min(d, R) - 1e-4f, "far cap: never under the config in ring 0");
      check(!(d > 0) || lim <= d + 1e-4f, "far cap: never beyond the config");
      check(d > 0 || !(cap > 0) || lim >= R, "far cap of no limit: at least ring 0");
      check(cap > 0 || lim == d, "no cap: the config");
    }
  }
  // the shrub bands: alpha (full to x, 0 at w) within ring 0 exactly as at the config
  auto alpha = [](float d, float x, float w) {
    return d <= x ? 1.f : std::clamp((w - d) / std::max(w - x, 1e-3f), 0.f, 1.f);
  };
  for (float x = 5.f; x <= 160.f; x += 7.f) {
    for (float wk = 1.f; wk <= 2.f; wk += 0.25f) {
      for (float k = 0.3f; k <= 1.f; k += 0.1f) {
        float nx = x, nw = x * wk;
        shrub_band(&nx, &nw, k, R);
        check(nw <= x * wk + 1e-3f && nx <= x + 1e-3f, "shrub lever: never further");
        for (float d = 0; d < R; d += 0.5f) {
          check(near(alpha(d, x, x * wk), alpha(d, nx, nw), 1e-4f), "shrub lever: ring 0 as before");
        }
      }
    }
  }
  // every relief position of both games' defaults and of the PS2 values: the small ties' limit
  // (100 m x the scale) stays beyond ring 0, tie_lod_px at most the old 4
  const float cfgs[][3] = {{4.f, 0.f, 1.5f}, {1.f, 0.f, 0.f}, {4.f, 200.f, 1.5f}};
  for (const auto& c : cfgs) {
    for (bool jak1 : {true, false}) {
      Relief rl;
      rl.init(c[0], c[1], c[2], 500.f, jak1);
      for (float r = 0; r <= kReliefMax; r += 0.05f) {
        const Knobs kn = rl.at(r);
        check(!(kn.tie_distance_scale > 0) || kn.tie_distance_scale * kSmallTieMeters >= R,
              "relief: small ties drawn beyond ring 0");
        check(kn.tie_lod_px <= std::max(c[0], kOldTieLodPx), "relief: tie_lod_px at most 4");
        check(!(kn.tie_far > 0) || kn.tie_far >= 100.f, "relief: the ties' copies beyond 100 m");
      }
    }
  }
}

// a distance limit's fade band, and how fast the slewed position moves a limit through it
void test_fade_band() {
  for (float lim : {40.f, 150.f, 250.f, 500.f}) {
    check(band_alpha(0.f, lim) == 1.f && band_alpha(lim, lim) == 1.f, "all of it up to the limit");
    check(band_alpha(lim * kFadeBand, lim) < 1e-4f && band_alpha(lim * 3.f, lim) == 0.f,
          "none of it from the band's end");
    float last = 1.f;
    for (float d = lim; d <= lim * kFadeBand; d += lim * 0.001f) {
      const float a = band_alpha(d, lim);
      check(a <= last + 1e-6f && a >= 0.f, "the band only fades out");
      check(last - a <= 0.011f, "the band has no step");
      last = a;
    }
  }
  check(band_alpha(1e9f, 0.f) == 1.f, "no limit: all of it");
  // one frame of the slew moves a Jak 1 tie limit by less than a tenth of its band, the shrub
  // bands by less than 2%
  Relief rl;
  rl.init(4.f, 0.f, 1.5f, 500.f, true);
  for (float r = 1.f; r + kSlew <= 3.f; r += kSlew) {
    const Knobs a = rl.at(r), b = rl.at(r + kSlew);
    if (a.tie_far > 0 && b.tie_far > 0) {
      check(std::fabs(a.tie_far - b.tie_far) <= 0.1f * b.tie_far * (kFadeBand - 1.f),
            "a frame's slew: under a tenth of the tie limit's band");
    }
  }
  for (float r = 0.f; r + kSlew <= 1.f; r += kSlew) {
    check(std::fabs(rl.at(r).shrub_far - rl.at(r + kSlew).shrub_far) <= 0.02f * 0.5f,
          "a frame's slew: the shrub bands by under 2%");
  }
  float x = 0.f;
  for (int f = 0; f < 1000; f++) {
    const float n = slew(x, -3.f);
    check(std::fabs(n - x) <= kSlew + 1e-6f, "the slew's step");
    x = n;
  }
  check(x == -3.f, "the slew arrives");
}

// the controller before (the old relief, 15 frame windows, 60-480 frame holds, fast steps)
struct OldController {
  static constexpr int kFastFrames = 4, kSettleFrames = 24, kBuildFrames = 8, kRoomFrames = 30;
  static constexpr double kMsPerUnit = 4.0, kRoomMargin = 2.0, kUrgentMargin = 3.0, kGpuLead = 3.0;
  static constexpr float kStepMin = 0.04f, kStepMax = 0.1f, kStepBack = 0.05f, kBackMax = 0.5f;
  static constexpr int kHoldMin = 60, kHoldMax = 480, kBackoffFrames = 90, kCalmFrames = 300;
  float r = 0;
  int n = 0;
  double lead = 0;
  bool urgent = false;
  long frame = 0;
  int hold = kHoldMin;
  long last_toward = -1000000, last_back = -1000000;
  double fast_sum = 0, fast_gpu = 0, fast_cpu = 0;
  int fast_n = 0, fast_over = 0;
  bool last_over = false;
  double room_sum = 0;
  int room_n = 0, room_over = 0;
  double builds[kBuildFrames] = {};
  bool step(double busy, double build, double target) {
    frame++;
    lead += (busy - build - lead) * 0.25;
    if (lead < kGpuLead) {
      busy = std::min(busy, target);
    }
    builds[frame % kBuildFrames] = build;
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
    fast_over += busy > 33.0;
    room_sum += load;
    room_n++;
    room_over += load > target;
    bool moved = false;
    if (fast_n >= kFastFrames) {
      const double avg = fast_sum / fast_n;
      urgent = fast_gpu / fast_n > target + kUrgentMargin || fast_over >= 2;
      const bool over = avg > target;
      if (over && r < (float)n && (urgent || (last_over && frame - last_toward >= kSettleFrames))) {
        const float s =
            std::clamp((float)(kStepMin + 0.02 * (avg - target)), kStepMin, kStepMax);
        r = std::min((float)n, r + s);
        if (frame - last_back < kBackoffFrames) {
          hold = std::min(hold * 2, kHoldMax);
        }
        last_toward = frame;
        moved = true;
        room_sum = 0;
        room_n = room_over = 0;
      }
      last_over = over;
      fast_sum = fast_gpu = fast_cpu = 0;
      fast_n = fast_over = 0;
    }
    if (room_n >= kRoomFrames) {
      const double room = target - kRoomMargin - room_sum / room_n;
      if (r > 0 && room > 0 && room_over <= 1 && frame - last_toward >= hold) {
        r = std::max(0.f, r - std::clamp((float)(room / kMsPerUnit), kStepBack, kBackMax));
        last_back = frame;
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

// a view: its GPU ms per frame at Jak 1's defaults (the old values: tie_lod_px 4, ties x1.5),
// frames; build: the render thread's ms (0: busy - 10, GPU-bound as on hardware; < 0: busy +
// this, Azahar); spike: ms more every fourth frame (the title's time of day), in busy and build
struct View {
  const char* name;
  double busy;
  int frames;
  double build = 0;
  double spike = 0;
};

// the GPU model (host title flight, triangles x the hardware's 0.164 ms per 1000): the PS2 values
// (tie_lod_px 1, no tie limit) cost kOldSave ms more than Jak 1's defaults, tie_lod_px alone
// kOldSavePx of it (wind ties); the new x: kUpMs per unit above (ties x1.5 -> x3), below it the far
// levers' kJak1LeverMs (relief_saving)
constexpr double kOldSave = 1.5, kOldSavePx = 1.3;
constexpr double kUpMs = 0.5;

struct Run {
  int over33 = 0, frames = 0, reversals = 0, swings = 0, max_window_steps = 0, steps = 0;
  int first_step = -1;
  double x_min = 0, x_max = 0, x_end = 0, at_config = 0;
  double slew_max = 0;  // the most the drawn position moved in a frame
};

// lag: the GPU times are one frame late; the old relief's step came in with a time constant (what
// was in view kept its decisions until out of view; urgent: at once), the new position is drawn
// kSlew per frame nearer to the controller's, which waits for it
// Jak 1's relief: its levers' GPU ms, unit by unit (kJak1LeverMs in stage order)
Relief jak1_relief() {
  Relief rl;
  rl.init(4.f, 0.f, 1.5f, 500.f, true);
  return rl;
}
double relief_saving(double r) {
  static const Relief rl = jak1_relief();
  double ms = 0;
  for (int k = 0; k < kKnobs; k++) {
    ms += rl.part(k, (float)r) * kJak1LeverMs[k];
  }
  return ms;
}

template <bool OLD>
Run simulate(const std::vector<View>& views, unsigned seed, double target = 27.0) {
  OldController oc;
  oc.n = 2;  // Jak 1 at the PS2 values: tie_lod_px, then the tie limit
  Controller c;
  const Relief rl = jak1_relief();
  c.range(-(float)rl.n, 1.f);  // Jak 1's defaults: the far levers .. ties x3
  c.unit_costs(rl, kJak1LeverMs);
  std::mt19937 rng(seed);
  std::normal_distribution<double> noise(0.0, 1.5);
  double eff = 0;
  float drawn = 0;
  double pending = views.empty() ? 0.0 : views[0].busy;
  Run out;
  int last_dir = 0;
  int frame = 0, window_steps = 0;
  bool low = false;  // at the bottom of the range since the last swing
  for (const auto& v : views) {
    for (int f = 0; f < v.frames; f++, frame++) {
      double cost;
      if (OLD) {
        const double r = eff;
        cost = v.busy + kOldSave - kOldSavePx * std::clamp(r, 0.0, 1.0) -
               (kOldSave - kOldSavePx) * std::clamp(r - 1.0, 0.0, 1.0);
      } else {
        cost = v.busy + (eff > 0 ? kUpMs * eff : -relief_saving(-eff));
      }
      const double busy = std::max(5.0, cost + noise(rng)) + (frame % 4 == 0 ? v.spike : 0.0);
      out.over33 += busy > 33.4;
      out.frames++;
      const double measured = pending;  // one frame late
      pending = busy;
      const double build = (v.build < 0 ? busy + v.build : busy - 10.0) + noise(rng);
      const double pos0 = OLD ? -oc.r : c.x;
      if (OLD) {
        oc.step(measured, build, target);
      } else {
        c.step(measured, build, true, target, 1.0, true, drawn);
        const float d0 = drawn;
        drawn = slew(drawn, c.x);
        out.slew_max = std::max(out.slew_max, (double)std::fabs(drawn - d0));
      }
      const double pos = OLD ? -oc.r : c.x;
      if (pos != pos0) {
        const int dir = pos > pos0 ? 1 : -1;
        out.reversals += last_dir && dir != last_dir;
        last_dir = dir;
        out.steps++;
        window_steps++;
        if (dir < 0 && out.first_step < 0) {
          out.first_step = frame;
        }
      }
      if (frame % 450 == 449) {
        out.max_window_steps = std::max(out.max_window_steps, window_steps);
        window_steps = 0;
      }
      // a full swing: back to the top of the range after its bottom (the old relief 0 <- 2, the
      // new x 0 <- -1)
      const double lo_edge = OLD ? -1.9 : -0.9, hi_edge = -0.1;
      if (pos <= lo_edge) {
        low = true;
      } else if (low && pos >= hi_edge) {
        low = false;
        out.swings++;
      }
      if (OLD) {
        eff += (oc.r - eff) / (oc.urgent ? 2.0 : 15.0);
      } else {
        eff = drawn;
      }
      out.x_min = std::min(out.x_min, pos);
      out.x_max = std::max(out.x_max, pos);
      out.at_config += pos == 0;
    }
  }
  out.x_end = OLD ? -oc.r : c.x;
  return out;
}

void print_run(const char* name, const Run& r) {
  std::printf("  %-34s frames over 33 ms %5.1f%%, steps %4d (most in 15 s %2d), reversals %3d, full "
              "swings %2d, position %.2f .. %.2f (end %.2f)\n",
              name, 100.0 * r.over33 / std::max(r.frames, 1), r.steps, r.max_window_steps,
              r.reversals, r.swings, r.x_min, r.x_max, r.x_end);
}

// hw-nov01e (New 3DS, title -> village1 -> beach -> jungle, 8 min): per 15 s window the GPU's
// active ms and the old relief at its end. Each window becomes three 5 s views (the window's time
// -2, +0, +2 ms in a seeded order); a window that ended below r 2 had more detail on than the
// defaults for part of it: its time at the defaults is lower by that share of kOldSave
constexpr double kHw[][2] = {
    {34.20, 2.00}, {33.24, 2.00}, {30.86, 2.00}, {26.17, 1.76}, {29.09, 2.00}, {30.79, 2.00},
    {33.28, 2.00}, {31.76, 2.00}, {33.56, 2.00}, {33.70, 2.00}, {28.13, 2.00}, {28.51, 2.00},
    {25.98, 0.00}, {25.99, 0.60}, {28.33, 2.00}, {27.75, 1.30}, {27.94, 2.00}, {35.73, 2.00},
    {33.17, 2.00}, {28.55, 2.00}, {25.15, 2.00}, {27.84, 1.91}, {24.26, 0.00}, {23.10, 0.76},
    {27.31, 1.50}, {25.92, 1.27}, {28.62, 2.00}, {18.98, 0.00}, {25.21, 2.00}, {33.09, 2.00},
    {24.53, 0.94}, {21.58, 2.00}, {38.05, 2.00}, {30.19, 2.00}, {21.58, 0.00}, {26.62, 2.00},
    {25.12, 0.30}, {27.51, 2.00}, {32.22, 2.00}, {27.65, 2.00}};

std::vector<View> hw_session(unsigned seed) {
  std::mt19937 rng(seed);
  std::vector<View> v;
  for (const auto& w : kHw) {
    const double at_defaults = w[0] - kOldSave * (1.0 - w[1] / 2.0) * 0.5;
    double d[3] = {-2.0, 0.0, 2.0};
    std::shuffle(d, d + 3, rng);
    for (double e : d) {
      v.push_back({"hw", at_defaults + e, 150});
    }
  }
  return v;
}

void test_hw_replay() {
  if (verbose) {
    std::printf("hw-nov01e replayed (seed 1):\n");
  }
  const auto session = hw_session(1);
  const Run o = simulate<true>(session, 1);
  const Run n = simulate<false>(session, 1);
  if (verbose) {
    print_run("old controller, PS2 values", o);
    print_run("new controller, Jak 1 defaults", n);
  }
  // the hardware pattern: the old relief swings over its whole range again and again
  check(o.swings >= 4, "old: full 2 -> 0 swings, as on hardware");
  check(o.max_window_steps >= 15, "old: 15 or more steps in a 15 s window, as on hardware");
  // the new one settles: no full swings, few steps, fewer frames over budget
  check(n.swings == 0, "new: no full swings");
  check(n.max_window_steps <= 8, "new: at most 8 steps in a 15 s window (6 levers to go through)");
  check(n.reversals <= 6, "new: few direction changes");
  check(n.steps * 4 < o.steps, "new: a quarter of the old steps or fewer");
  check(n.over33 < o.over33, "new: fewer frames over 33 ms than the old");
  for (unsigned seed = 2; seed < 30; seed++) {
    const auto s = hw_session(seed);
    const Run a = simulate<false>(s, seed);
    // (at most one shed per kShedGap: 10 in 15 s)
    check(a.swings == 0 && a.max_window_steps <= 450 / Controller::kShedGap,
          "new: settles for every seed");
    const Run b = simulate<true>(s, seed);
    check(a.over33 <= b.over33, "new: no more frames over 33 ms than the old, every seed");
  }
}

void test_controller() {
  // a light stretch: the far ring gains detail, slowly; a heavy view: it sheds, quickly a little
  const std::vector<View> light = {{"light", 18.0, 3600}};
  const Run l = simulate<false>(light, 3);
  check(l.x_min == 0 && l.x_max > 0.5, "a light view: detail above the config, never below");
  check(l.max_window_steps <= 2, "a light view: at most 2 steps up per 15 s");
  const std::vector<View> heavy = {{"heavy", 34.0, 1800}};
  const Run h = simulate<false>(heavy, 4);
  check(h.first_step >= 0 && h.first_step <= 45, "an overload sheds within 1.5 s");
  check(h.x_end < 0, "a heavy view: below the config");
  // a view right at the budget, inside the dead band: nothing moves
  const std::vector<View> edge = {{"edge", 27.5, 3600}};
  const Run e = simulate<false>(edge, 7);
  check(e.steps <= 2, "inside the dead band: (almost) no steps");
  // light, then heavy, then light again: the light stretch after it doesn't go back above what
  // didn't fit (the ceiling), until it has been calm for a while
  std::vector<View> cyc;
  for (int i = 0; i < 6; i++) {
    cyc.push_back({"light", 21.0, 900});
    cyc.push_back({"heavy", 31.5, 450});
  }
  const Run c = simulate<false>(cyc, 9);
  const Run co = simulate<true>(cyc, 9);
  if (verbose) {
    std::printf("light 30 s / heavy 15 s, six times:\n");
    print_run("old controller, PS2 values", co);
    print_run("new controller, Jak 1 defaults", c);
  }
  check(c.reversals <= 6, "light / heavy cycles: few direction changes");
  check(c.over33 <= co.over33, "light / heavy cycles: no more frames over 33 ms than the old");
  // Azahar: busy ~ the build + 0.9 ms, the render thread the limit: views that hold 30 fps there
  // never move below the config
  for (unsigned seed = 3; seed < 13; seed++) {
    const std::vector<View> play = {{"azahar lava tube", 13.5, 900, -0.9},
                                    {"azahar fire canyon", 22.7, 900, -0.9},
                                    {"azahar citadel", 15.0, 900, -0.9}};
    const Run a = simulate<false>(play, seed);
    check(a.x_min == 0, "Azahar gameplay that holds 30 fps never sheds");
  }
  // a late title flight in Azahar: a 28 ms build plus 12 ms every fourth frame (31 ms mean)
  const std::vector<View> title = {{"azahar late title", 28.0, 1800, -0.9, 12.0}};
  const Run t = simulate<false>(title, 5);
  check(t.x_min < 0, "a render thread over budget sheds");
  // reset (the title's end) and a new area
  Controller ct;
  ct.range(-1.f, 1.f);
  for (int f = 0; f < 600; f++) {
    ct.step(36.0, 26.0, true, 27.0);
  }
  check(ct.x < 0 && ct.ceil < 0, "a long overload: below the config, the ceiling too");
  ct.new_area();
  check(ct.ceil == 1.f, "a new area lifts the ceiling");
  ct.reset();
  check(ct.x == 0, "reset: the config values");
  for (int f = 0; f < 120; f++) {
    check(!ct.step(15.5, 15.0, true, 27.0), "after the reset a light view waits before more");
  }
  // Jak 1's levers: a shed is GPU ms (at most 1), so the cheap far levers go in the first sheds
  {
    Controller cj;
    const Relief rl = jak1_relief();
    cj.range(-(float)rl.n, 1.f);
    cj.unit_costs(rl, kJak1LeverMs);
    int sheds = 0;
    for (int f = 0; f < 900 && cj.x > -2.f; f++) {
      sheds += cj.step(36.0, 26.0, true, 27.0);
    }
    check(cj.x <= -2.f && sheds <= 2, "Jak 1: the cheap levers in 2 sheds");
    Controller cu;
    cu.range(-(float)rl.n, 1.f);
    sheds = 0;
    for (int f = 0; f < 900 && cu.x > -2.f; f++) {
      sheds += cu.step(36.0, 26.0, true, 27.0);
    }
    check(sheds >= 8, "without the costs (4 ms a unit) it would take 8 sheds");
    // it waits for the position the levels are drawn with
    Controller cw;
    cw.range(-(float)rl.n, 1.f);
    cw.unit_costs(rl, kJak1LeverMs);
    float drawn = 0;
    int moves_ahead = 0;
    for (int f = 0; f < 1800; f++) {
      const bool behind = std::fabs(drawn - cw.x) > kSlewNear;
      moves_ahead += cw.step(36.0, 26.0, true, 27.0, 1.0, true, drawn) && behind;
      drawn = slew(drawn, cw.x);
    }
    check(moves_ahead == 0 && cw.x == cw.lo && near(drawn, cw.lo, 1e-3f),
          "no move while the drawn position is behind; a long overload ends at the floor, drawn");
  }
  // Jak II's defaults: nothing below the config, the tie range above (x1.5 .. x3)
  Controller j2;
  j2.range(0.f, 1.f);
  for (int f = 0; f < 900; f++) {
    j2.step(40.0, 30.0, true, 27.0);
  }
  check(j2.x == 0, "Jak II's defaults: nothing to shed below the config");
}
}  // namespace

int main(int argc, char** argv) {
  verbose = argc > 1 && std::strcmp(argv[1], "-v") == 0;
  test_ranges();
  test_near_ring();
  test_fade_band();
  test_controller();
  test_hw_replay();
  std::printf("auto_detail_test: ok\n");
  return 0;
}
