/*!
 * @file tests_misc.cpp
 * Test for the native mem-copy! (native_misc.cpp) against the GOAL byte loop.
 */

#include "fakes.h"

namespace tests {

using namespace harness;

void register_misc_tests() {
  add_test({"mem-copy!",
            "mem-copy!",
            4000,
            nullptr,
            [](Case& c) {
              auto& g = c.g;
              const u32 buf = alloc(4096);
              for (u32 i = 0; i < 4096; i += 4) {
                st<u32>(buf + i, g.u32_());
              }
              const s32 n = g.chance(0.05f) ? g.range(-4, 0) : g.range(1, 1500);
              const u32 src = buf + g.range(0, 2000);
              // overlapping both ways, or apart
              const u32 dst = g.chance(0.5f) ? src + g.range(-300, 300) : buf + g.range(0, 2000);
              c.args[0] = dst;
              c.args[1] = src;
              c.args[2] = (u64)(s64)n;
            },
            {}});
}

}  // namespace tests
