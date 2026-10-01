/*!
 * @file tests_joint.cpp
 * (AI-assisted)
 * Tests for joint.cpp: cspace<-parented-transformq-joint!, calc-animation-from-spr.
 */

#include "fakes.h"

namespace tests {

using namespace harness;

namespace {

//! a bone: matrix, scale (w: 0 or 1 or random), then the bone cache
u32 gen_bone(Gen& g) {
  const u32 bone = alloc(96);
  for (int i = 0; i < 16; i++) {
    st<float>(bone + 4 * i, i >= 12 ? g.f(-1e5f, 1e5f) : g.f_edge(-2.f, 2.f));
  }
  for (int i = 0; i < 3; i++) {
    const float s = g.chance(0.1f) ? 0.f : (g.chance(0.5f) ? 1.f : g.f(0.1f, 3.f));
    st<float>(bone + 64 + 4 * i, s);
  }
  st<float>(bone + 76, g.pick(std::vector<float>{0.f, 1.f, -1.f, 0.5f}));
  for (int i = 80; i < 96; i += 4) {
    st<u32>(bone + i, g.u32_());
  }
  return bone;
}

}  // namespace

void register_joint_tests() {
  add_test({"cspace<-parented-transformq-joint!",
            "cspace<-parented-transformq-joint!",
            20000,
            nullptr,
            [](Case& c) {
              auto& g = c.g;
              const u32 parent = alloc(32);
              const u32 cs = alloc(32);
              const u32 pbone = gen_bone(g);
              const u32 bone = g.chance(0.05f) ? pbone : gen_bone(g);
              st<u32>(parent + 16, pbone);
              st<u32>(cs + 0, parent);
              st<u32>(cs + 16, bone);
              const u32 tq = alloc(48);
              // trans, quat (usually normalized), scale
              st_vec(tq, g.f(-1e5f, 1e5f), g.f(-1e5f, 1e5f), g.f(-1e5f, 1e5f), 1.f);
              float q[4];
              float len = 0;
              for (int i = 0; i < 4; i++) {
                q[i] = g.f(-1.f, 1.f);
                len += q[i] * q[i];
              }
              for (int i = 0; i < 4; i++) {
                q[i] = g.chance(0.8f) ? q[i] / std::sqrt(len) : q[i];
              }
              st_bytes(tq + 16, q, 16);
              st_vec(tq + 32, g.f_edge(0.f, 2.f), g.f_edge(0.f, 2.f), g.f_edge(0.f, 2.f),
                     g.f(-1.f, 1.f));
              c.args[0] = cs;
              c.args[1] = tq;
            },
            {}});
}

}  // namespace tests
