/*!
 * @file micro_test.cpp
 * (AI-assisted)
 * Host bit tests of the 3DS renderers' CPU shortcuts: each one against a copy of what it
 * replaces, all inputs where that's possible (2^32 float bit patterns), else many random ones.
 *
 *   test/ctr_micro/build.sh && /tmp/ctr_micro_test
 */

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>

#include "game/graphics/ctr/CtrMicro.h"

using u32 = uint32_t;
using s32 = int32_t;

namespace ref {
// ctr_gpu_citro3d.c raw_f24 / raw_pack_f24 / ctr_gpu_pack_skin_rows_f24 (copied)
static inline u32 raw_f24(float f) {
  union {
    float f;
    u32 i;
  } u = {f};
  const u32 m23 = (u.i & 0x7fffff) + 0x40;
  const u32 sign = u.i >> 31, mant = (m23 >> 7) & 0xffff;
  const s32 e = (s32)((u.i >> 23) & 0xff) - 127 + 63 + (s32)(m23 >> 23);
  if (e < 0 || !((u.i >> 23) & 0xff)) {
    return sign << 23;
  }
  if (e > 0x7f) {
    return (sign << 23) | (0x7fu << 16);
  }
  return (sign << 23) | ((u32)e << 16) | mant;
}
static void raw_pack_f24(const float* v, u32* out) {
  const u32 w = raw_f24(v[0]), z = raw_f24(v[1]), y = raw_f24(v[2]), x = raw_f24(v[3]);
  out[0] = (w << 8) | (z >> 16);
  out[1] = (z << 16) | (y >> 8);
  out[2] = (y << 24) | x;
}
void pack_skin_rows_f24(const float* bones, int palette_count, uint32_t* out) {
  for (int row = 0; row < 3; row++) {
    const float* src = bones + 4 * row;
    for (int b = 0; b < palette_count; b++, src += 12, out += 3) {
      const float v[4] = {src[3], src[2], src[1], src[0]};
      raw_pack_f24(v, out);
    }
  }
}
}  // namespace ref

static float from_u32(u32 i) {
  float f;
  memcpy(&f, &i, 4);
  return f;
}

static int test_f24() {
  u32 bad = 0;
  u32 i = 0;
  do {
    if (ctr_micro::f24_from_bits(i) != ref::raw_f24(from_u32(i))) {
      if (bad++ < 5) {
        printf("  f24 %08x: %06x vs %06x\n", i, ctr_micro::f24_from_bits(i), ref::raw_f24(from_u32(i)));
      }
    }
  } while (++i != 0);
  printf("f24: 2^32 inputs, %u differ\n", bad);
  // the palette packing, random palettes of random bits and of matrix-like values
  std::mt19937 rng(1);
  u32 bad_pack = 0;
  for (int t = 0; t < 200000; t++) {
    const int n = 1 + (int)(rng() % 24);
    float bones[24 * 12];
    for (int k = 0; k < 12 * n; k++) {
      const u32 r = rng();
      bones[k] = (t & 1) ? from_u32(r) : (float)((s32)r) * 1e-6f;
    }
    u32 a[9 * 24], b[9 * 24];
    memset(a, 0xcd, sizeof(a));
    memset(b, 0xcd, sizeof(b));
    ref::pack_skin_rows_f24(bones, n, a);
    ctr_micro::pack_skin_rows_f24(bones, n, b);
    bad_pack += memcmp(a, b, sizeof(a)) != 0;
  }
  printf("f24 palettes: 200000 random, %u differ\n", bad_pack);
  return bad + bad_pack;
}

int main() {
  int errors = 0;
  errors += test_f24();
  printf("%s\n", errors ? "FAIL" : "OK");
  return errors ? 1 : 0;
}
