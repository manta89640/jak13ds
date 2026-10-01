/*!
 * @file native_ocean.cpp
 * (AI-assisted)
 * Native versions of the mips2c functions in ocean_vu0.cpp that the game logic needs. See
 * game/mips2c/mips2c_native.h for the rules (same float operations, grouped the same way, as the
 * mips2c code).
 */

#include "game/kernel/jak1/kscheme.h"
#include "game/mips2c/jak1_functions/native_functions.h"

namespace Mips2C::jak1::native {

namespace {

u32 sym_addr(const char* name) {
  return ::jak1::intern_from_c(name).offset;
}

//! vitof15 of each s8 height moved to the top byte of a word (pextlb, pextlh): the height * 512
struct HeightFloats {
  float f[256];
  HeightFloats() {
    for (u32 b = 0; b < 256; b++) {
      f[b] = ((float)(s32)(b << 24)) * (1.f / 32768.f);
    }
  }
};

/*!
 * (ocean-interp-wave dest frame): the ocean's wave heights for this frame (32x32 floats at dest),
 * between two of the 64 frames of *ocean-wave-frames* (1024 s8 heights each): frame / 32 and the
 * next one, weighted by frame % 32. The two weights (times 0.333) are also left in *ocean-work* at
 * 60 and 64. Called every frame: ocean-get-height (boats, floating things, Jak in the water) reads
 * the result.
 */
u64 ocean_interp_wave_impl(const NativeArgs& args) {
  static const u32 frames_sym = sym_addr("*ocean-wave-frames*");
  static const u32 work_sym = sym_addr("*ocean-work*");
  static const HeightFloats heights;

  const u32 dest = (u32)args.a[0];
  const u64 frame = args.a[1];
  const s64 i = (s64)frame >> 5;
  const u8* frame_a = gptr(gload<u32>(frames_sym) + (u32)((i & 63) << 10));
  const u8* frame_b = gptr(gload<u32>(frames_sym) + (u32)(((i + 1) & 63) << 10));

  const u32 work = gload<u32>(work_sym);
  gstore<float>(work + 64, 0.03125f * (float)(s32)(frame & 31));
  gstore<float>(work + 60, 1.f - gload<float>(work + 64));
  gstore<float>(work + 60, (float)0.333 * gload<float>(work + 60));
  gstore<float>(work + 64, (float)0.333 * gload<float>(work + 64));
  const float weight_a = gload<float>(work + 60);
  const float weight_b = gload<float>(work + 64);

  // vmulax: acc = a * weight_a (for each of the 256 heights), vmaddy: acc + b * weight_b
  float acc[256];
  for (u32 b = 0; b < 256; b++) {
    acc[b] = heights.f[b] * weight_a;
  }
  float row[32];
  for (u32 r = 0; r < 1024; r += 32) {
    for (u32 k = 0; k < 32; k++) {
      row[k] = acc[frame_a[r + k]] + heights.f[frame_b[r + k]] * weight_b;
    }
    gstore_bytes(dest + 4 * r, row, sizeof(row));
  }
  return 0;
}

}  // namespace

const NativeImpl ocean_interp_wave = MIPS2C_NATIVE_IMPL(ocean_interp_wave_impl, 0, 0);

}  // namespace Mips2C::jak1::native
