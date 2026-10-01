/*!
 * @file tests_ocean.cpp
 * (AI-assisted)
 * Tests for ocean_vu0.cpp: ocean-interp-wave.
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

}  // namespace

void register_ocean_tests() {
  add_test({"ocean-interp-wave", "ocean-interp-wave", 400, [] {}, gen_interp_wave, {}});
}

}  // namespace tests
