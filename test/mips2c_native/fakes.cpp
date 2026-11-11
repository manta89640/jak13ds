/*!
 * @file fakes.cpp
 * (AI-assisted)
 * Fake GOAL functions shared by the tests.
 */

#include "fakes.h"

#include <cstdlib>
#include <functional>
#include <random>
#include <string>

#include "common/log/log.h"

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

// (AI-assisted, cpusound) more of what the natives use from the runtime (not linked into the
// harness), so it links again: pc-rand's generator (game/kernel/common/kmachine.cpp), the log
// (common/log), and mips2c_native.cpp's switches (no native turned off, no inline verify: the
// harness compares the versions itself)
std::mt19937 extra_random_generator;
u32 pc_rand() {
  return (u32)extra_random_generator();
}
namespace lg::internal {
void log_message(level, LogTime&, const char*) {}
// (AI-assisted, sched) the log's level check (log.h, newer than these fakes): nothing is logged
bool enabled(level) {
  return false;
}
}  // namespace lg::internal
// (AI-assisted, sched) config.ini (platform/3ds/port/ctr_port.c; the ARM config builds with
// -D__3DS__): no key set, every native reads its default
extern "C" int ctr_config_get(const char*, char*, int) {
  return 0;
}
extern "C" int ctr_config_bool(const char*, int def) {
  return def;
}
namespace Mips2C {
bool native_turned_off(const std::string&) {
  return false;
}
#if MIPS2C_WRITE_LOG
bool native_verify_inline_on() {
  return false;
}
u64 native_verify_inline(const char*,
                         std::initializer_list<NativeVerifyRegion>,
                         const std::function<u64()>& native,
                         const std::function<u64()>&) {
  return native();
}
#endif
}  // namespace Mips2C
