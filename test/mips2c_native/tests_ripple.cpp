/*!
 * @file tests_ripple.cpp
 * (AI-assisted)
 * Tests for ripple.cpp: ripple-create-wave-table (the water ripples' height and normal table) and
 * ripple-execute-init (the VU0 sine table it reads).
 */

#include "fakes.h"

namespace Mips2C::jak1 {
void* ripple_vu0_table();
namespace ripple_execute_init {
void ripple_execute_init_invalidate();
u64 native_impl(const NativeArgs&);
}  // namespace ripple_execute_init
}  // namespace Mips2C::jak1

namespace tests {

using namespace harness;

namespace {

void setup_ripple() {
  // *cos-poly-vec* (trigonometry.gc), then ripple-execute-init fills the VU0 sine table that
  // ripple-create-wave-table reads
  const u32 poly = alloc_static(16, 16);
  st<u32>(poly + 0, 0xbefffd62);
  st<u32>(poly + 4, 0x3d2a7a28);
  st<u32>(poly + 8, 0xbab2bc31);
  st<u32>(poly + 12, 0x37a933eb);
  set_sym("*cos-poly-vec*", poly);
  const u64 none[8] = {};
  call(mips2c_stub("ripple-execute-init"), none);
  // angles: 65536 per turn, like the game's
  constexpr float kUnit = 65536.f / 6.2831855f;
  set_sym("atan", add_goal_fn("atan", 2, [](const u64* a) -> u64 {
            return f_bits(std::atan2(bits_f(a[0]), bits_f(a[1])) * kUnit);
          }));
  set_sym("cos", add_goal_fn("cos", 1, [](const u64* a) -> u64 {
            return f_bits(std::cos(bits_f(a[0]) / kUnit));
          }));
  set_sym("sin", add_goal_fn("sin", 1, [](const u64* a) -> u64 {
            return f_bits(std::sin(bits_f(a[0]) / kUnit));
          }));
  // moves every wave's offset by its speed (both versions call it the same way)
  set_sym("ripple-update-waveform-offs",
          add_goal_fn("ripple-update-waveform-offs", 1, [](const u64* a) -> u64 {
            const u32 set = (u32)a[0];
            const s32 n = ld<s32>(set);
            for (s32 i = 0; i < n && i < 4; i++) {
              const u32 w = set + 16 + 28 * (u32)i;
              st<float>(w + 4, ld<float>(w + 4) + ld<float>(w + 12) * 37.f);
            }
            return 0;
          }));
}

// (ripple-create-wave-table wave-set)
void gen_create_wave_table(Case& c) {
  auto& g = c.g;
  const u32 set = alloc_basic(0, 16 + 28 * 4);
  const s32 count = g.range(0, 4);
  st<s32>(set, count);
  // converted: #f (the first call: direction and speed from xdiv / zdiv) or #t
  st<u32>(set + 4, g.chance(0.5f) ? st() : true_sym());
  st<u32>(set + 8, g.u32_());
  st<float>(set + 12, g.f_edge(-2.f, 6.f));
  for (int i = 0; i < 4; i++) {
    const u32 w = set + 16 + 28 * (u32)i;
    st<float>(w + 0, g.f_edge(-40.f, 40.f));            // scale
    st<float>(w + 4, g.f_edge(-200000.f, 200000.f));    // offs
    st<s16>(w + 8, (s16)g.range(-48, 48));              // xdiv
    st<s16>(w + 10, (s16)g.range(-48, 48));             // zdiv
    st<float>(w + 12, g.f_edge(-8.f, 8.f));             // speed
    st<float>(w + 16, g.f_edge(-120000.f, 120000.f));   // xmul
    st<float>(w + 20, g.f_edge(-120000.f, 120000.f));   // zmul
    st<float>(w + 24, g.f_edge(-100.f, 100.f));         // delta
  }
  // *setting-control*'s video mode (offset 80): ntsc, pal or something else
  const u32 settings = alloc(128);
  const int mode = g.range(0, 2);
  st<u32>(settings + 80, mode == 0 ? sym("ntsc") : (mode == 1 ? sym("pal") : sym("custom")));
  set_sym("*setting-control*", settings);
  c.args[0] = set;
  for (int i = 1; i < 8; i++) {
    c.args[i] = g.u32_();
  }
}

// (AI-assisted) (ripple-execute-init): the table from *cos-poly-vec*. The native fills it only when
// the polynomial changed. Every case starts from the test's first table, so the native's memo is
// reset here; half of the cases first run the native on the same polynomial (the previous frame),
// so that the call checked is the one that keeps the table.
void gen_execute_init(Case& c) {
  auto& g = c.g;
  const u32 poly = alloc(16);
  for (int i = 0; i < 4; i++) {
    st<float>(poly + 4 * i, g.f_edge(-2.f, 2.f));
  }
  if (g.chance(0.3f)) {  // the game's
    st<u32>(poly + 0, 0xbefffd62);
    st<u32>(poly + 4, 0x3d2a7a28);
    st<u32>(poly + 8, 0xbab2bc31);
    st<u32>(poly + 12, 0x37a933eb);
  }
  set_sym("*cos-poly-vec*", poly);
  Mips2C::jak1::ripple_execute_init::ripple_execute_init_invalidate();
  if (g.chance(0.5f)) {
    const u64 none[8] = {};
    Mips2C::jak1::ripple_execute_init::native_impl(Mips2C::NativeArgs{none, 0, 0, 0});
  }
  for (int i = 0; i < 8; i++) {
    c.args[i] = g.u32_();
  }
}

}  // namespace

void register_ripple_tests() {
  add_test({"ripple-create-wave-table", "ripple-create-wave-table", 1500, setup_ripple,
            gen_create_wave_table, {}});
  add_test({"ripple-execute-init",
            "ripple-execute-init",
            300,
            [] {},
            gen_execute_init,
            {{Mips2C::jak1::ripple_vu0_table(), 256 * 16}}});
}

}  // namespace tests
