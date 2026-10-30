// auto_detail's relief (game/graphics/ctr/CtrAutoDetail.h): the knobs' ranges, and the controller
// against a model of the GPU's busy time per frame (a view's time at the PS2 values minus what
// each knob saves at its position, reached with a lag, plus noise).
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
  rl.init(1.f, 0.f, 0.f, 500.f);  // the PS2 values, draw_distance 500
  check(rl.n == 3, "PS2 values: three knobs move");
  check(rl.stage[kTieLodPx] == 0 && rl.stage[kLodDistance] == 1 && rl.stage[kTieDistance] == 2,
        "order: tie_lod_px, lod_distance, tie distance");
  Knobs k = rl.at(0.f);
  check(k.tie_lod_px == 1.f && k.lod_distance == 0.f && k.tie_distance_scale == 0.f,
        "r 0: the config values exactly");
  k = rl.at(0.5f);
  check(near(k.tie_lod_px, 2.f) && k.lod_distance == 0.f && k.tie_distance_scale == 0.f,
        "r 0.5: tie_lod_px halfway (geometric), the others untouched");
  k = rl.at(1.f);
  check(near(k.tie_lod_px, 4.f) && k.lod_distance == 0.f, "r 1: tie_lod_px at 4");
  k = rl.at(1.0001f);
  check(near(k.lod_distance, 500.f, 0.1f), "lod_distance starts at the draw distance (no change)");
  k = rl.at(2.f);
  check(near(k.lod_distance, 200.f) && k.tie_distance_scale == 0.f, "r 2: lod_distance 200");
  k = rl.at(2.0001f);
  check(near(k.tie_distance_scale, 5.f, 0.01f), "the tie limit starts at the draw distance's equal");
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
  Relief j2;
  j2.init(4.f, 200.f, 1.5f, 500.f);  // Jak II's defaults: already the old values
  check(j2.n == 0, "Jak II's defaults: nothing to move");
  k = j2.at(3.f);
  check(k.tie_lod_px == 4.f && k.lod_distance == 200.f && k.tie_distance_scale == 1.5f,
        "Jak II: the config values at any r");
  Relief part;
  part.init(2.f, 0.f, 1.5f, 500.f);
  check(part.n == 2 && part.stage[kTieDistance] < 0 && part.stage[kLodDistance] == 1,
        "a knob at its old value is left out of the order");
}

// a view: its GPU ms per frame at the PS2 values, and what each knob saves at its old value;
// build: the render thread's ms (0: busy - 10, a GPU-bound frame as on hardware; < 0: busy + this,
// Azahar's busy that follows the build)
struct View {
  const char* name;
  double busy0;
  double save[kKnobs];
  int frames;
  double build = 0;
};

struct Run {
  int over33 = 0, frames = 0, reversals = 0, first_step = -1;
  double r_max = 0, r_end = 0, at_ps2 = 0;
  double sec_part[kKnobs][3] = {};
};

// lag: a step's saving comes in with a time constant (frames): the GPU times are one frame late,
// and the chunks and tie groups in view keep their decisions until out of view (urgent: at once)
Run simulate(const std::vector<View>& views, unsigned seed, bool print) {
  Relief rl;
  rl.init(1.f, 0.f, 0.f, 500.f);
  Controller c;
  c.n = rl.n;
  KnobTime kt;
  std::mt19937 rng(seed);
  std::normal_distribution<double> noise(0.0, 1.5);
  double eff_r = 0;  // the relief the picture has reached
  std::vector<double> pending(2, 0.0);
  Run out;
  int last_dir = 0;
  int frame = 0;
  for (const auto& v : views) {
    Run vr;
    const float r_start = c.r;
    for (int f = 0; f < v.frames; f++, frame++) {
      double save = 0;
      for (int k = 0; k < kKnobs; k++) {
        save += v.save[k] * rl.part(k, (float)eff_r);
      }
      const double busy = std::max(5.0, v.busy0 - save + noise(rng));
      vr.over33 += busy > 33.4;
      vr.frames++;
      pending.push_back(busy);
      const double measured = pending.front();  // one frame late
      pending.erase(pending.begin());
      const double build = v.build < 0 ? busy + v.build + noise(rng) : busy - 10.0 + noise(rng);
      const float r0 = c.r;
      c.step(measured, build, true, 27.0);
      if (c.r != r0) {
        const int dir = c.r > r0 ? 1 : -1;
        out.reversals += last_dir && dir != last_dir;
        last_dir = dir;
        if (dir > 0 && vr.first_step < 0) {
          vr.first_step = f;
        }
      }
      const double tau = c.urgent ? 2.0 : 15.0;
      eff_r += (c.r - eff_r) / tau;
      kt.add(rl, c.r, 1000.0 / 30.0);
      vr.r_max = std::max(vr.r_max, (double)c.r);
      vr.at_ps2 += c.r <= 0;
    }
    vr.r_end = c.r;
    if (print) {
      const Knobs k = rl.at(c.r);
      std::printf("  %-28s %5.1f ms at the PS2 values: first step after %3d frames, r max %.2f, "
                  "end %.2f (px %.1f, lod %s, ties %s), frames over 33 ms %4.1f%%, at the PS2 "
                  "values %5.1f%%\n",
                  v.name, v.busy0, vr.first_step, vr.r_max, vr.r_end, k.tie_lod_px,
                  k.lod_distance > 0 ? std::to_string((int)k.lod_distance).c_str() : "off",
                  k.tie_distance_scale > 0 ? std::to_string(k.tie_distance_scale).substr(0, 4).c_str()
                                           : "none",
                  100.0 * vr.over33 / vr.frames, 100.0 * vr.at_ps2 / vr.frames);
    }
    if (std::strncmp(v.name, "light", 5) == 0) {
      check(vr.r_max <= r_start, "a light view never moves toward the old values");
      check(r_start > 0 || vr.at_ps2 == vr.frames, "a light view stays at the PS2 values");
    }
    if (v.busy0 > 33.0 && r_start < (float)c.n) {
      check(vr.first_step >= 0 && vr.first_step <= 8, "an overload is answered within 8 frames");
    }
    out.r_max = std::max(out.r_max, vr.r_max);
    out.over33 += vr.over33;
    out.frames += vr.frames;
    out.first_step = out.first_step < 0 ? vr.first_step : out.first_step;
    out.r_end = vr.r_end;
  }
  std::memcpy(out.sec_part, kt.ms, sizeof(kt.ms));
  return out;
}

void test_controller() {
  // hw-oct30 (New 3DS, PS2 values): GPU busy per 15 s window; busy ~ 14.9 ms + 0.136 ms per 1000
  // triangles over the log. The savings per knob are estimates (see the report): tie_lod_px 1 -> 4
  // ~30-70k triangles in open views, lod_distance and the tie limit a few ms at most.
  const std::vector<View> session = {
      {"light (village1 hut)", 14.6, {1.0, 0.3, 0.2}, 600},
      {"title flight", 35.3, {6.0, 2.5, 1.5}, 900},
      {"light (lava tube start)", 19.9, {1.5, 0.5, 0.3}, 900},
      {"fire canyon", 38.4, {7.0, 2.0, 1.0}, 900},
      {"lava tube", 33.6, {5.0, 1.0, 1.0}, 900},
      {"rolling / ogre (edge)", 28.5, {3.0, 0.8, 0.5}, 1800},
      {"light (after)", 16.9, {1.0, 0.3, 0.2}, 1800},
  };
  if (verbose) {
    std::printf("model run (seed 1):\n");
  }
  const Run r = simulate(session, 1, verbose);
  check(r.r_end == 0, "back at the PS2 values after a light stretch");
  // the edge of the budget: no back and forth every second
  check(r.reversals < 40, "few direction changes over the session");
  for (unsigned seed = 2; seed < 40; seed++) {
    simulate(session, seed, false);
  }
  // each hardware view on its own, from the PS2 values (60 s)
  if (verbose) {
    std::printf("each view from the PS2 values:\n");
  }
  for (const View& v : session) {
    View one = v;
    one.frames = 1800;
    simulate({one}, 11, verbose);
  }
  // a view right at the target: stays put or creeps, no oscillation
  std::vector<View> edge = {{"edge 27.5 ms", 27.5, {3.0, 1.0, 0.5}, 3000}};
  const Run e = simulate(edge, 7, verbose);
  // Azahar: busy ~ the build + 0.9 ms (title flight 19-27 ms): no GPU limit, no relief
  std::vector<View> az = {{"azahar title (busy ~ build)", 27.5, {3.0, 1.0, 0.5}, 3000, -0.9}};
  const Run a = simulate(az, 3, verbose);
  check(a.r_max == 0, "Azahar's busy time (the build) never moves the knobs");
  check(e.reversals < 12, "at the edge of the budget: few direction changes");
}
}  // namespace

int main(int argc, char** argv) {
  verbose = argc > 1 && std::strcmp(argv[1], "-v") == 0;
  test_ranges();
  test_controller();
  std::printf("auto_detail_test: ok\n");
  return 0;
}
