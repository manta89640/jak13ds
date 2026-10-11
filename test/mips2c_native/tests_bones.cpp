/*!
 * @file tests_bones.cpp
 * (AI-assisted)
 * Test of native_bones.cpp: pc-new-bones-mtx-calc against the GOAL function it replaces,
 * new-bones-mtx-calc-asm, as compiled by goalc (goalc_ref/bones.c).
 */

#include "fakes.h"

namespace tests {

using namespace harness;

namespace {

//! the FPU gives the default NaN for every NaN result (the 3DS's mode, run.sh vfp / arm): which
//! input NaN an operation passes on otherwise depends on how the compiler ordered its operands
bool default_nan_mode() {
  volatile float a = Mips2C::u2f(0x7fc12345);
  volatile float b = 1.f;
  return Mips2C::f2u(a + b) == 0x7fc00000;
}

//! a matrix element: usually a rotation-sized or translation-sized value, sometimes special (any
//! bits: NaN, inf, denormal) in the 3DS's FPU mode
float mat_float(Gen& g, int k) {
  static const bool specials = default_nan_mode();
  if (g.chance(0.01f) && specials) {
    return Mips2C::u2f(g.u32_());  // any bits (NaN, inf, denormal)
  }
  if (k >= 12 && k < 15) {
    return g.f_edge(-1e5f, 1e5f);  // translation
  }
  if (k % 4 == 3) {
    return g.pick(std::vector<float>{0.f, 0.f, 1.f, g.f(-1.f, 1.f)});  // w column
  }
  return g.f_edge(-2.f, 2.f);
}

void gen_matrix(Gen& g, u32 addr) {
  // sometimes a singular one (the inverse transpose divides by its determinant)
  const bool singular = g.chance(0.03f);
  for (int k = 0; k < 16; k++) {
    st<float>(addr + 4 * k, mat_float(g, k));
  }
  if (singular) {
    for (int k = 0; k < 4; k++) {
      st<float>(addr + 16 + 4 * k, ld<float>(addr + 4 * k));
    }
  }
}

// (new-bones-mtx-calc-asm output joints bones cam count)
void gen_bones(Case& c) {
  auto& g = c.g;
  const int count = g.chance(0.05f) ? g.range(0, 1) : g.range(2, 40);
  const int n = count > 0 ? count : 1;
  const u32 output = alloc(128 * n + 16);
  const u32 joints = alloc(80 * n + 16) + 4;  // basics: the bind pose at +12 is 16-byte aligned
  const u32 bones = alloc(96 * n + 16);
  const u32 cam = alloc(64);
  for (u32 i = 0; i < 128 * (u32)n; i += 4) {
    st<u32>(output + i, g.u32_());  // the parts it doesn't write must stay
  }
  for (u32 i = 0; i < 80 * (u32)n; i += 4) {
    st<u32>(joints + i, g.u32_());
  }
  for (u32 i = 0; i < 96 * (u32)n; i += 4) {
    st<u32>(bones + i, g.u32_());
  }
  for (int i = 0; i < n; i++) {
    gen_matrix(g, joints + 80 * i + 12);
    gen_matrix(g, bones + 96 * i);
  }
  if (g.chance(0.2f)) {
    for (int k = 0; k < 16; k++) {  // *identity-matrix* (bone-calc-flags bncfl01)
      st<float>(cam + 4 * k, k % 5 == 0 ? 1.f : 0.f);
    }
  } else {
    gen_matrix(g, cam);
  }
  c.args[0] = output;
  c.args[1] = joints;
  c.args[2] = bones;
  c.args[3] = cam;
  c.args[4] = (u64)(s64)count;
}

}  // namespace

void register_bones_tests() {
  add_test({"pc-new-bones-mtx-calc", "pc-new-bones-mtx-calc", 4000, nullptr, gen_bones, {}});
}

}  // namespace tests
