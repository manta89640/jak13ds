/*!
 * @file fakes.cpp
 * (AI-assisted)
 * Fake GOAL functions shared by the tests.
 */

#include "fakes.h"

#include <cstdlib>

namespace tests {

using namespace harness;

void bind_collide_functions(bool compiled_goal) {
  for (const char* name : {"ray-sphere-intersect", "ray-cylinder-intersect",
                           "moving-sphere-sphere-intersect", "closest-pt-in-triangle"}) {
    if (compiled_goal) {
      bind_goalc_reference(name);
    } else {
      bind_mips2c_symbol(name);
    }
  }
}

}  // namespace tests

// (AI-assisted) game/kernel/common/klink.cpp (pc-collide-frag-may-hit?'s vertex box cache)
u32 g_link_generation = 0;

// (AI-assisted) game/mips2c/mips2c_native.cpp (not linked into the harness): the micro fast paths
// of the natives (add_mesh_tris_micro, render-ocean-quad, ripple), on unless OPENGOAL_LOGIC_MICRO=0
// on the host, as in the game
namespace Mips2C {
bool logic_micro_on() {
  static int on = -1;
  if (on < 0) {
    on = 1;
#ifndef __3DS__
    const char* e = getenv("OPENGOAL_LOGIC_MICRO");
    if (e && e[0] == '0') {
      on = 0;
    }
#endif
  }
  return on == 1;
}
}  // namespace Mips2C
