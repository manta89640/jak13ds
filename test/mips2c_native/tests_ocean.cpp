/*!
 * @file tests_ocean.cpp
 * (AI-assisted)
 * Tests for ocean_vu0.cpp: ocean-interp-wave, ocean-generate-verts.
 */

#include "fakes.h"

namespace tests {

using namespace harness;

namespace {

// (ocean-interp-wave dest frame)
void gen_interp_wave(Case& c) {
  auto& g = c.g;
  // 64 frames of 1024 s8 heights
  const u32 frames = alloc(64 * 1024);
  for (u32 i = 0; i < 64 * 1024; i += 4) {
    st<u32>(frames + i, g.u32_());
  }
  set_sym("*ocean-wave-frames*", frames);
  // ocean-work (a basic): the weights go to 60 and 64 (a quadword)
  const u32 work = alloc_basic(0, 96);
  for (u32 i = 0; i < 96; i += 4) {
    st<u32>(work + i, g.u32_());
  }
  set_sym("*ocean-work*", work);
  c.args[0] = alloc(4096);
  switch (g.range(0, 3)) {
    case 0:
      c.args[1] = (u64)g.range(0, 64 * 32 * 2);
      break;
    case 1:
      c.args[1] = g.u32_();
      break;
    case 2:
      c.args[1] = ((u64)g.u32_() << 32) | g.u32_();  // dsra: an arithmetic shift
      break;
    default:
      c.args[1] = (u64)(64 * 32 - 1 - g.range(0, 40));  // the last frame, then the first
      break;
  }
  for (int i = 2; i < 8; i++) {
    c.args[i] = g.u32_();
  }
}

// (AI-assisted) the GOAL functions ocean-generate-verts calls: the VU0 program upload (nothing),
// the lights (copied from the light group: 7 quadwords) and vector*! (xyz products, w = 1)
void setup_generate_verts() {
  set_sym("upload-vu0-program",
          add_goal_fn("upload-vu0-program", 2, [](const u64*) -> u64 { return 0; }));
  set_sym("vu-lights<-light-group!",
          add_goal_fn("vu-lights<-light-group!", 2, [](const u64* a) -> u64 {
            for (u32 i = 0; i < 112; i += 4) {
              st<u32>((u32)a[0] + i, ld<u32>((u32)a[1] + i));
            }
            return 0;
          }));
  set_sym("vector*!", add_goal_fn("vector*!", 3, [](const u64* a) -> u64 {
            for (u32 i = 0; i < 12; i += 4) {
              st<float>((u32)a[0] + i, ld<float>((u32)a[1] + i) * ld<float>((u32)a[2] + i));
            }
            st<float>((u32)a[0] + 12, 1.f);
            return a[0];
          }));
}

// (ocean-generate-verts dest heights)
void gen_generate_verts(Case& c) {
  auto& g = c.g;
  // *ocean-vu0-work*: scales (the game's, or random), the masks, the lights (written by the
  // calls), wait-to-vu0
  const u32 work = alloc(176);
  for (u32 i = 0; i < 176; i += 4) {
    st<u32>(work + i, g.u32_());
  }
  if (g.chance(0.7f)) {
    st_vec(work, -0.00006975447f, 255.f, -0.00006975447f, g.f(-1.f, 1.f));
  } else {
    st_vec(work, g.f_edge(-0.001f, 0.001f), g.f_edge(0.f, 300.f), g.f_edge(-0.001f, 0.001f),
           g.f(-1.f, 1.f));
  }
  set_sym("*ocean-vu0-work*", work);
  // *time-of-day-context*: the light group at 156 (directions, colors, ambient)
  const u32 tod = alloc(156 + 112 + 12);
  for (u32 i = 0; i < 112; i += 4) {
    const u32 lane = (i / 4) % 4;
    const float v = i < 48 ? g.f_edge(-1.f, 1.f) : (lane == 3 ? g.f(0.f, 1.f) : g.f_edge(0.f, 1.5f));
    st<float>(tod + 156 + i, v);
  }
  set_sym("*time-of-day-context*", tod);
  set_sym("ocean-vu0-block", alloc(16));
  const u32 scale = alloc(16);
  if (g.chance(0.8f)) {
    st_vec(scale, 64.f, 128.f, 128.f, 128.f);  // ocean.gc's
  } else {
    st_vec(scale, g.f(0.f, 200.f), g.f(0.f, 200.f), g.f(0.f, 200.f), g.f(0.f, 200.f));
  }
  set_sym("*ocean-generate-verts-vector*", scale);
  // the 32x32 heights (ocean-interp-wave's: up to about +-20 m), sometimes flat
  const u32 heights = alloc(4096 + 64);  // (the mips2c version reads 16 bytes past the end)
  const float amp = g.chance(0.1f) ? 0.f : g.f(0.f, 80000.f);
  for (u32 i = 0; i < 4096 + 64; i += 4) {
    st<float>(heights + i, g.chance(0.02f) ? g.f_edge(-amp, amp) : g.f(-amp, amp));
  }
  c.args[0] = alloc(0x8000);
  c.args[1] = heights;
  for (int i = 2; i < 8; i++) {
    c.args[i] = g.u32_();
  }
}

}  // namespace

void register_ocean_tests() {
  add_test({"ocean-interp-wave", "ocean-interp-wave", 400, [] {}, gen_interp_wave, {}});
  add_test({"ocean-generate-verts", "ocean-generate-verts", 300, setup_generate_verts,
            gen_generate_verts, {}});
}

}  // namespace tests
