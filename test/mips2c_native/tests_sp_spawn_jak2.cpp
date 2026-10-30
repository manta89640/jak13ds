/*!
 * @file tests_sp_spawn_jak2.cpp
 * Test for Jak 2's pc-sp-spawn (jak2_functions/native_sp_spawn.cpp) against the Jak 2 GOAL method
 * spawn of sparticle-launch-control as compiled by goalc (goalc_ref/jak2_sparticle_launcher.c).
 * Jak 2 symbols: #t is s7 + 4; the fixture's symbol values are where the harness keeps them.
 */

#include "fakes.h"

namespace tests {

using namespace harness;

namespace {

u32 jak2_true() {
  return st() + 4;
}

//! the fakes' log (GOAL memory, compared): 0 count, 12 the visibility flag, then 8 words per call
u32 fake_log() {
  return sym_value("*spawn2-test-log*");
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

u32 type_of(const char* name, int methods) {
  const u32 t = sym_value(name);
  return t ? t : make_type(name, methods);
}

void setup_spawn() {
  set_sym("*spawn2-test-log*", alloc_static(16 + 32 * 60, 16));
  const u32 lc_type = type_of("sparticle-launch-control", 16);
  set_method(lc_type, 10, add_goal_fn("is-visible?", 0, [](const u64* a) -> u64 {
               log_call(1, a, 2);
               return ld<u32>(fake_log() + 12) ? jak2_true() : st();
             }));
  set_sym("math-camera-pos", add_goal_fn("math-camera-pos", 0, [](const u64* a) -> u64 {
            log_call(2, a, 0);
            return sym_value("*spawn2-test-cam*");
          }));
  set_sym("vector-vector-distance",
          add_goal_fn("vector-vector-distance", 0, [](const u64* a) -> u64 {
            log_call(3, a, 2);
            const float x = ld<float>((u32)a[0]) - ld<float>((u32)a[1]);
            const float z = ld<float>((u32)a[0] + 8) - ld<float>((u32)a[1] + 8);
            const float d = std::fabs(x) + std::fabs(z);
            return f_bits(d);
          }));
  // logs the launch matrix's trans too (written just before the call)
  set_sym("sp-launch-particles-var",
          add_goal_fn("sp-launch-particles-var", 0, [](const u64* a) -> u64 {
            log_call(4, a, 6);
            const u32 log = fake_log();
            const u32 k = ld<u32>(log) - 1;
            if (k < 60) {
              st<u32>(log + 16 + 32 * k + 28, ld<u32>((u32)a[2] + 48));
            }
            return 0;
          }));
  // the GOAL code prints the error with format, the native through pc-sp-spawn-error
  const u32 error_fn = add_goal_fn("spawn-error", 0, [](const u64* a) -> u64 {
    log_call(5, a, 0);
    return 0;
  });
  set_sym("format", error_fn);
  set_sym("pc-sp-spawn-error", error_fn);
  type_of("sparticle-launcher", 9);
  set_sym("*sp-particle-system-3d*", alloc_static(16, 16));
  set_sym("*sp-particle-system-2d*", alloc_static(16, 16));
}

}  // namespace

void register_sp_spawn_jak2_tests() {
  add_test({"pc-sp-spawn-jak2",
            "jak2:pc-sp-spawn",
            8000,
            setup_spawn,
            [](Case& c) {
              auto& g = c.g;
              const u32 log = fake_log();
              st<u32>(log, 0);
              st<u32>(log + 12, g.chance(0.85f) ? 1 : 0);
              // the process's clock
              const u32 clock = alloc(128);
              const u64 now = g.chance(0.1f) ? ((u64)g.u32_() << 32 | g.u32_())
                                              : (u64)g.range(0, 2000000);
              st<u64>(clock + 20, now);
              st<float>(clock + 60, g.chance(0.7f) ? 5.f : g.f(0.f, 300.f));
              st<u32>(kProcess + 8, clock);
              const u32 display = alloc(128);
              const u32 real_clock = alloc(64);
              st<u32>(display + 56, real_clock);
              st<u64>(real_clock + 36, (u64)g.range(0, 100000));
              set_sym("*display*", display);
              const u32 cam = alloc_vec(g.f(-1e5f, 1e5f), g.f(-1e5f, 1e5f), g.f(-1e5f, 1e5f), 1.f);
              set_sym("*spawn2-test-cam*", cam);
              if (g.chance(0.8f)) {
                const u32 proc = alloc(256);
                st<s32>(proc + 124, g.chance(0.1f) ? g.range(-70, 70) : g.range(0, 23));
                const u32 tod = alloc(16);
                st<u32>(tod, proc);
                set_sym("*time-of-day*", tod);
              } else {
                set_sym("*time-of-day*", st());
              }
              if (g.chance(0.9f)) {
                const u32 pcs = alloc(512);
                st<u32>(pcs + 276, g.chance(0.85f) ? jak2_true() : st());
                set_sym("*pc-settings*", pcs);
              } else {
                set_sym("*pc-settings*", st());
              }
              const u32 launch_matrix = alloc(64);
              st_vec(launch_matrix + 48, 9.f, 9.f, 9.f, 1.f);
              set_sym("*launch-matrix*", launch_matrix);
              // launchers: the part id table (a boxed array, data at 12)
              const int nl = 8;
              const u32 table = alloc(16 + 4 * nl);
              for (int i = 0; i < nl; i++) {
                u32 l;
                if (g.chance(0.08f)) {
                  l = g.chance(0.5f) ? 0 : st();
                } else {
                  // sometimes not a sparticle-launcher: the error branch
                  const u32 type = g.chance(0.03f) ? sym_value("*spawn2-test-cam*")
                                                   : sym_value("sparticle-launcher");
                  l = alloc_basic(type, 64);
                }
                st<u32>(table + 12 + 4 * i, l);
              }
              set_sym("*part-id-table*", table);
              // the group and the launch control
              const int n = g.range(0, 6);
              const u32 group = alloc(64);
              st<u16>(group + 6, (u16)g.range(0, 7));
              const u32 lc = alloc_basic(sym_value("sparticle-launch-control"), 108 + 48 * n);
              st<s32>(lc + 0, n);
              st<u32>(lc + 12, group);
              st<s32>(lc + 20, g.chance(0.05f) ? (s32)g.u32_() : g.range(0, 100000));
              st<s8>(lc + 28, g.chance(0.1f) ? (s8)g.range(-128, 127) : 0);
              const u64 frame = ld<u64>(real_clock + 36);
              st<s32>(lc + 36, g.chance(0.7f) ? (s32)(frame - 1) : g.range(0, 100000));
              st<s32>(lc + 40, (s32)now - g.range(0, 300));
              for (int i = 0; i < n; i++) {
                const u32 state = lc + 108 + 48 * i;
                const u32 gi = alloc(32);
                st<u32>(gi, g.range(0, nl - 1));
                st<float>(gi + 4, g.chance(0.5f) ? 0.f : g.f_edge(0.f, 3e5f));
                st<float>(gi + 8, g.chance(0.5f) ? 0.f : g.f_edge(-1e5f, 3e5f));
                u16 flags = (u16)g.range(0, 0xff);
                if (g.chance(0.6f)) {
                  flags &= ~0x08;
                }
                st<u16>(gi + 12, flags);
                const u16 period = g.chance(0.3f) ? 0 : (u16)g.range(1, 3000);
                st<u16>(gi + 14, period);
                st<u16>(gi + 16, (u16)(period ? g.range(0, period + 10) : g.range(0, 100)));
                st<u32>(gi + 20, g.chance(0.7f) ? 0 : g.u32_() & 0xffffff);
                st<u32>(state, gi);
                st<u16>(state + 4, (u16)g.range(0, 3));
                st<u32>(state + 8, alloc_vec(g.f(-10.f, 10.f), 2.f, 3.f, 1.f));
                st<u32>(state + 20, g.chance(0.5f) ? 0 : g.u32_() % 5000);
                st<u32>(state + 28, (u32)now - g.range(0, 5000));
              }
              c.args[0] = lc;
              c.args[1] = alloc_vec(g.f(-1e5f, 1e5f), g.f(-1e5f, 1e5f), g.f(-1e5f, 1e5f), 1.f);
            },
            {}});
}

}  // namespace tests
