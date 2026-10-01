/*!
 * @file fakes.cpp
 * (AI-assisted)
 * Fake GOAL functions shared by the tests.
 */

#include "fakes.h"

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
