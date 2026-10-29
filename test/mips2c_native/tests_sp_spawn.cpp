/*!
 * @file tests_sp_spawn.cpp
 * Test for pc-sp-spawn (native_sp_spawn.cpp) against the GOAL method spawn of
 * sparticle-launch-control as compiled by goalc (goalc_ref/sparticle_launcher.c).
 */

#include "fakes.h"

namespace tests {

using namespace harness;

namespace {

//! the fakes' log (GOAL memory, compared): 0 count, then 8 words per call
u32 fake_log() {
  return sym_value("*spawn-test-log*");
}

void log_call(u32 tag, const u64* a, int n) {
  const u32 log = fake_log();
  const u32 k = ld<u32>(log);
  if (k < 60) {
    const u32 e = log + 16 + 32 * k;
    st<u32>(e, tag);
    for (int i = 0; i < n; i++) {
      st<u32>(e + 4 + 4 * i, (u32)a[i]);
    }
  }
  st<u32>(log, k + 1);
}

void setup_spawn() {
  static bool done = false;
  if (done) {
    return;
  }
  done = true;
  set_sym("*spawn-test-log*", alloc_static(16 + 32 * 60, 16));
  const u32 lc_type = make_type("sparticle-launch-control", 16);
  // (is-visible? this pos): from a flag the case sets at 12 of the log
  set_method(lc_type, 10, add_goal_fn("is-visible?", 0, [](const u64* a) -> u64 {
               log_call(1, a, 2);
               return ld<u32>(fake_log() + 12) ? true_sym() : st();
             }));
  set_sym("math-camera-pos", add_goal_fn("math-camera-pos", 0, [](const u64* a) -> u64 {
            log_call(2, a, 0);
            return sym_value("*spawn-test-cam*");
          }));
  // a distance-like float from both vectors
  set_sym("vector-vector-distance",
          add_goal_fn("vector-vector-distance", 0, [](const u64* a) -> u64 {
            log_call(3, a, 2);
            const float x = ld<float>((u32)a[0]) - ld<float>((u32)a[1]);
            const float z = ld<float>((u32)a[0] + 8) - ld<float>((u32)a[1] + 8);
            const float d = std::fabs(x) + std::fabs(z);
            return f_bits(d);
          }));
  set_sym("sp-launch-particles-var",
          add_goal_fn("sp-launch-particles-var", 0, [](const u64* a) -> u64 {
            log_call(4, a, 6);
            return 0;
          }));
  set_sym("format", add_goal_fn("format", 0, [](const u64* a) -> u64 {
            log_call(5, a, 1);
            return 0;
          }));
  make_type("sparticle-launcher", 9);
  set_sym("*sp-particle-system-3d*", alloc_static(16, 16));
  set_sym("*sp-particle-system-2d*", alloc_static(16, 16));
}

}  // namespace

void register_sp_spawn_tests() {
  add_test({"pc-sp-spawn",
            "pc-sp-spawn",
            6000,
            setup_spawn,
            [](Case& c) {
              auto& g = c.g;
              const u32 log = fake_log();
              st<u32>(log, 0);
              st<u32>(log + 12, g.chance(0.85f) ? 1 : 0);
              // globals
              set_sym("*particle-300hz-timer*",
                      g.chance(0.1f) ? g.u32_() : (u32)g.range(0, 2000000));
              const u32 display = alloc(1024);
              set_sym("*display*", display);
              st<u64>(display + 820, (u64)g.range(0, 100000));
              const u32 ft = alloc(16);
              st<float>(ft, g.chance(0.7f) ? 5.f : g.f(0.f, 300.f));
              set_sym("*sp-frame-time*", ft);
              const u32 cam = alloc_vec(g.f(-1e5f, 1e5f), g.f(-1e5f, 1e5f), g.f(-1e5f, 1e5f), 1.f);
              set_sym("*spawn-test-cam*", cam);
              if (g.chance(0.8f)) {
                const u32 proc = alloc(256);
                st<s32>(proc + 124, g.chance(0.05f) ? g.range(-3, 40) : g.range(0, 23));
                const u32 pp = alloc(16);
                st<u32>(pp, proc);
                set_sym("*time-of-day-proc*", pp);
              } else {
                set_sym("*time-of-day-proc*", st());
              }
              if (g.chance(0.9f)) {
                const u32 pcs = alloc(512);
                st<u32>(pcs + 276, g.chance(0.85f) ? true_sym() : st());
                set_sym("*pc-settings*", pcs);
              } else {
                set_sym("*pc-settings*", st());
              }
              // launchers: the part id table (a boxed array, data at 12)
              const int nl = 8;
              const u32 table = alloc(16 + 4 * nl);
              for (int i = 0; i < nl; i++) {
                u32 l;
                if (g.chance(0.08f)) {
                  l = g.chance(0.5f) ? 0 : st();
                } else {
                  // (not the "non-sparticle-launcher" error: the native prints it through a GOAL
                  // function of its own)
                  l = alloc_basic(sym_value("sparticle-launcher"), 64);
                }
                st<u32>(table + 12 + 4 * i, l);
              }
              set_sym("*part-id-table*", table);
              // the group and the launch control
              const int n = g.range(0, 6);
              const u32 group = alloc(64);
              st<u16>(group + 6, (u16)g.range(0, 7));
              const u32 lc = alloc_basic(sym_value("sparticle-launch-control"), 60 + 32 * n);
              st<s32>(lc + 0, n);
              st<u32>(lc + 12, group);
              st<s32>(lc + 20, g.range(0, 100000));
              st<s32>(lc + 28, g.chance(0.1f) ? 1 : 0);
              const u64 frame = ld<u64>(display + 820);
              st<s32>(lc + 32, g.chance(0.7f) ? (s32)(frame - 1) : g.range(0, 100000));
              st<s32>(lc + 36, (s32)sym_value("*particle-300hz-timer*") - g.range(0, 300));
              for (int i = 0; i < n; i++) {
                const u32 state = lc + 60 + 32 * i;
                const u32 gi = alloc(32);
                st<u32>(gi, g.range(0, nl - 1));
                st<float>(gi + 4, g.chance(0.5f) ? 0.f : g.f_edge(0.f, 3e5f));
                st<float>(gi + 8, g.chance(0.5f) ? 0.f : g.f_edge(0.f, 3e5f));
                u16 flags = (u16)g.range(0, 0xff);
                if (g.chance(0.6f)) {
                  flags &= ~0x48;
                }
                st<u16>(gi + 12, flags);
                const u16 period = g.chance(0.3f) ? 0 : (u16)g.range(1, 3000);
                st<u16>(gi + 14, period);
                st<u16>(gi + 16, (u16)(period ? g.range(0, period + 10) : g.range(0, 100)));
                st<u32>(gi + 20, g.chance(0.7f) ? 0 : g.u32_() & 0xffffff);
                st<u32>(state, gi);
                st<u16>(state + 4, (u16)g.range(0, 3));
                st<u32>(state + 8, alloc_vec(1.f, 2.f, 3.f, 1.f));
                st<u32>(state + 20, g.chance(0.5f) ? 0 : g.u32_() % 5000);
                st<u32>(state + 28, (u32)sym_value("*particle-300hz-timer*") - g.range(0, 5000));
              }
              c.args[0] = lc;
              c.args[1] = alloc_vec(g.f(-1e5f, 1e5f), g.f(-1e5f, 1e5f), g.f(-1e5f, 1e5f), 1.f);
            },
            {}});
}

}  // namespace tests
