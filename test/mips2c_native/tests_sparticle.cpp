/*!
 * @file tests_sparticle.cpp
 * (AI-assisted)
 * Tests for sparticle.cpp: sp-process-block-2d and sp-process-block-3d.
 */

#include "fakes.h"

namespace tests {

using namespace harness;

namespace {

// sparticle-cpuinfo (144 bytes): sprite 0, adgif 4, radius 8, omega 12, vel-sxvel 16,
// rot-syvel 32, fade 48, acc 64, rotvel3d 80, friction 96, timer 100, flags 104, user 108,
// func 112, next-time 116, next-launcher 120, cache-alpha 124, valid 128, key 132, binding 136
constexpr u32 kInfoSize = 144;
constexpr u32 kVecSize = 48;

u32 g_callbacks[3];

//! a hash of some words of GOAL memory: the fakes' decisions depend only on their inputs
u32 hash_words(u32 addr, int n) {
  u32 h = 2166136261u;
  for (int i = 0; i < n; i++) {
    h = (h ^ ld<u32>(addr + 4 * i)) * 16777619u;
  }
  return h ^ (h >> 15);
}

//! the system counts the calls made to the fakes and remembers the last ones' arguments
void log_call(u32 system, u32 what, u64 a) {
  const u32 n = ld<u32>(system + 8);
  st<u32>(system + 8, n + 1);
  st<u32>(system + 16 + 8 * (n & 3), what);
  st<u32>(system + 20 + 8 * (n & 3), (u32)a);
}

void setup_sparticle() {
  static bool done = false;
  if (done) {
    return;
  }
  done = true;

  // (sp-free-particle system index info vec)
  set_sym("sp-free-particle", add_goal_fn("sp-free-particle", 4, [](const u64* a) -> u64 {
            log_call((u32)a[0], 1, a[1]);
            st<u32>((u32)a[2] + 128, st());
            st<float>((u32)a[3] + 44, 0.f);
            return 0;
          }));
  // (sp-relaunch-particle-2d/3d system launcher info vec): new timers, velocity, color, sometimes
  // a new callback or no next launcher
  for (const char* name : {"sp-relaunch-particle-2d", "sp-relaunch-particle-3d"}) {
    set_sym(name, add_goal_fn(name, 4, [](const u64* a) -> u64 {
              const u32 info = (u32)a[2];
              const u32 vec = (u32)a[3];
              log_call((u32)a[0], 2, a[1]);
              const u32 h = hash_words(info, 36) ^ (u32)a[1];
              st<u32>(info + 116, h % 23);
              if (h & 0x100) {
                st<u32>(info + 120, 0);
              }
              st<float>(info + 16, ld<float>(info + 16) + 1.5f);
              st<float>(info + 52, (float)(h % 7) - 3.f);
              st<float>(vec + 32, (float)(h % 255));
              if ((h & 0x3000) == 0x3000) {
                st<u32>(info + 112, g_callbacks[h % 3]);
              }
              if ((h & 0xc000) == 0xc000) {
                st<u32>(info + 104, ld<u32>(info + 104) ^ (h >> 20));
              }
              return 0;
            }));
  }
  // (sp-orbiter system info vec): moves the position, like the real one
  set_sym("sp-orbiter", add_goal_fn("sp-orbiter", 4, [](const u64* a) -> u64 {
            const u32 info = (u32)a[1];
            const u32 vec = (u32)a[2];
            log_call((u32)a[0], 3, a[3]);
            const float omega = ld<float>(info + 12) + 0.25f;
            st<float>(info + 12, omega);
            st<float>(vec, ld<float>(vec) + omega * 100.f);
            st<float>(vec + 8, ld<float>(vec + 8) - omega * 50.f);
            return 0;
          }));
  // (quaternion*! dest a b)
  set_sym("quaternion*!", add_goal_fn("quaternion*!", 4, [](const u64* a) -> u64 {
            float p[4], q[4], r[4];
            memcpy(p, hptr((u32)a[1]), 16);
            memcpy(q, hptr((u32)a[2]), 16);
            r[0] = p[3] * q[0] + p[0] * q[3] + p[1] * q[2] - p[2] * q[1];
            r[1] = p[3] * q[1] - p[0] * q[2] + p[1] * q[3] + p[2] * q[0];
            r[2] = p[3] * q[2] + p[0] * q[1] - p[1] * q[0] + p[2] * q[3];
            r[3] = p[3] * q[3] - p[0] * q[0] - p[1] * q[1] - p[2] * q[2];
            // sometimes -0 or +0 lanes, and a w of -0 (not below 0)
            const u32 h = hash_words((u32)a[1], 4) ^ hash_words((u32)a[2], 4);
            if (h % 8 == 0) {
              r[(h >> 3) & 3] = -0.f;
            } else if (h % 8 == 1) {
              r[(h >> 3) & 3] = 0.f;
            }
            if (h % 16 == 2) {
              r[3] = -0.f;
            }
            st_bytes((u32)a[0], r, 16);
            return a[0];
          }));
  // user callbacks (system info vec), with a3 to check that both versions pass the same
  g_callbacks[0] = add_goal_fn("callback-fade", 4, [](const u64* a) -> u64 {
    const u32 info = (u32)a[1];
    const u32 vec = (u32)a[2];
    log_call((u32)a[0], 4, a[3]);
    const u32 h = hash_words(vec, 12);
    if (h & 1) {
      st_vec(vec + 32, 0.f, 0.f, 0.f, ld<float>(vec + 44));  // black
    } else if (h & 2) {
      st<float>(vec + 44, -1.f);
    }
    st<u32>(info + 108, h);
    return 0;
  });
  g_callbacks[1] = add_goal_fn("callback-flags", 4, [](const u64* a) -> u64 {
    const u32 info = (u32)a[1];
    log_call((u32)a[0], 5, a[3]);
    const u32 h = hash_words(info, 36);
    st<u32>(info + 104, ld<u32>(info + 104) ^ (h & 0x2047));
    if (h & 0x10000) {
      st<u32>(info + 112, 0);  // no more callbacks
    }
    if ((h & 0x60000) == 0x60000) {
      st<s32>(info + 120, (s32)(h | 1));  // relaunch soon
      st<s32>(info + 116, -1);
    }
    return 0;
  });
  // changes *sp-frame-time*'s frame count: sp-process-block-3d reads it again for every particle
  g_callbacks[2] = add_goal_fn("callback-frame-time", 4, [](const u64* a) -> u64 {
    log_call((u32)a[0], 6, a[3]);
    const u32 ft = sym_value("*sp-frame-time*");
    const u32 h = hash_words((u32)a[1], 36);
    st<u32>(ft, 0x800000u | (h % 16));
    return 0;
  });
}

float rand_float_bits(Gen& g) {
  return Mips2C::u2f(g.u32_());
}

//! a random particle: its cpuinfo and vecdata
void gen_particle(Gen& g, u32 info, u32 vec, bool is_3d) {
  for (u32 i = 0; i < kInfoSize; i += 4) {
    st<u32>(info + i, g.u32_());
  }
  for (u32 i = 16; i < 96; i += 4) {
    st<float>(info + i, g.f_edge(-200.f, 200.f));
  }
  // rotvel3d: a rotation
  float q[4] = {g.f(-0.2f, 0.2f), g.f(-0.2f, 0.2f), g.f(-0.2f, 0.2f), 0.f};
  q[3] = std::sqrt(std::max(0.f, 1.f - q[0] * q[0] - q[1] * q[1] - q[2] * q[2]));
  st_bytes(info + 80, q, 16);
  st<float>(info + 96, g.pick(std::vector<float>{0.f, 0.f, 0.99f, 0.9f, g.f(0.f, 1.f), -0.f}));
  if (g.chance(0.02f)) {
    st<float>(info + 96, rand_float_bits(g));
  }
  st<s32>(info + 100, g.pick(std::vector<s32>{-1, -1, 0, 1, 5, 10, 300, g.range(-100, 100)}));
  u32 flags = 0;
  for (u32 bit : {1u, 2u, 4u, 64u, 128u, 8192u, 8u, 32u}) {
    if (g.chance(0.3f)) {
      flags |= bit;
    }
  }
  st<u32>(info + 104, flags | (g.chance(0.1f) ? g.u32_() : 0));
  st<u32>(info + 112, g.chance(0.7f) ? 0 : g_callbacks[g.range(0, 2)]);
  st<s32>(info + 116, g.chance(0.9f) ? g.range(-5, 30) : (s32)g.u32_());
  st<u32>(info + 120, g.chance(0.6f) ? 0 : g.chance(0.8f) ? alloc_basic(0, 16) : g.u32_());
  st<float>(info + 124, g.f(-10.f, 128.f));
  st<u32>(info + 128, g.chance(0.15f) ? st() : g.chance(0.9f) ? true_sym() : sym("other"));

  // position, size x
  st_vec(vec, g.f_edge(-1e5f, 1e5f), g.f_edge(-1e5f, 1e5f), g.f_edge(-1e5f, 1e5f),
         g.chance(0.1f) ? g.f(-10.f, 0.f) : g.f_edge(0.f, 5000.f));
  if (is_3d) {
    // quaternion x y z (w from them), size y
    float r[3] = {g.f(-1.f, 1.f), g.f(-1.f, 1.f), g.f(-1.f, 1.f)};
    if (g.chance(0.7f)) {
      const float len = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]) + 1e-6f;
      const float s = g.f(0.f, 1.f) / len;
      for (auto& v : r) {
        v *= s;
      }
    }
    st_vec(vec + 16, r[0], r[1], r[2], g.chance(0.1f) ? g.f(-10.f, 0.f) : g.f_edge(0.f, 5000.f));
  } else {
    // size y, (unused), rotation (wraps at 16 bits), size y
    st_vec(vec + 16, g.f(-10.f, 10.f), g.f(-10.f, 10.f),
           g.pick(std::vector<float>{g.f(-40000.f, 40000.f), g.f(-1e6f, 1e6f), 32767.9f,
                                     -32768.5f, 65535.f}),
           g.chance(0.1f) ? g.f(-10.f, 0.f) : g.f_edge(0.f, 5000.f));
  }
  // color: sometimes black, sometimes transparent or -0
  float c[4];
  for (auto& v : c) {
    v = g.f_edge(-50.f, 255.f);
  }
  if (g.chance(0.15f)) {
    c[0] = c[1] = c[2] = 0.f;
    st<float>(info + 48, 0.f);
    st<float>(info + 52, g.chance(0.5f) ? 0.f : -0.f);
    st<float>(info + 56, 0.f);
  }
  if (g.chance(0.1f)) {
    c[3] = g.pick(std::vector<float>{0.f, -0.f, -1.f});
  }
  st_bytes(vec + 32, c, 16);
}

// (sp-process-block-2d/3d system cpuinfo vecdata index count paused?)
void gen_process_block(Case& c, bool is_3d) {
  auto& g = c.g;
  const u32 system = alloc_basic(0, 64);
  for (u32 i = 0; i < 64; i += 4) {
    st<u32>(system + i, 0);
  }
  // the frame time: x the frame count (bits), y velocity scale, z acceleration, w friction
  const u32 n = (u32)g.pick(std::vector<s32>{5, 6, 10, 12, 3, 15, 20, 9, 11, 0, 255});
  const u32 ft = alloc(16);
  st_vec(ft, Mips2C::u2f(0x800000u | n), (float)n, 0.2f * (float)n, 0.2f * (float)n);
  if (g.chance(0.1f)) {
    st<u32>(ft, g.u32_());
  }
  set_sym("*sp-frame-time*", ft);

  const int count = g.chance(0.1f) ? 1 : g.range(1, 60);
  // like sp-process-block: the cpuinfos, then the vecdata, in the scratchpad
  const u32 info = kSpad + 16;
  const u32 vec = info + kInfoSize * count;
  for (int i = 0; i < count; i++) {
    gen_particle(g, info + kInfoSize * i, vec + kVecSize * i, is_3d);
  }
  c.args[0] = system;
  c.args[1] = info;
  c.args[2] = vec;
  c.args[3] = (u64)g.range(0, 4000);
  c.args[4] = (u64)count;
  c.args[5] = g.chance(0.7f) ? st() : g.chance(0.7f) ? true_sym() : sym("paused");
  for (int i = 6; i < 8; i++) {
    c.args[i] = g.u32_();
  }
}

}  // namespace

void register_sparticle_tests() {
  add_test({"sp-process-block-2d", "sp-process-block-2d", 4000, setup_sparticle,
            [](Case& c) { gen_process_block(c, false); }, {}});
  add_test({"sp-process-block-3d", "sp-process-block-3d", 4000, setup_sparticle,
            [](Case& c) { gen_process_block(c, true); }, {}});
}

}  // namespace tests
