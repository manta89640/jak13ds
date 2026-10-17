/*!
 * @file micro_test.cpp
 * (AI-assisted)
 * Host bit tests of the 3DS renderers' CPU shortcuts: each one against a copy of what it
 * replaces, all inputs where that's possible (2^32 float bit patterns), else many random ones.
 *
 *   test/ctr_micro/build.sh && /tmp/ctr_micro_test
 */

#include <bit>
#include <cmath>
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

// CtrMerc.cpp handle_model's bad bone test before (AI-assisted, micro-cpu)
static bool ref_bad_bone(const float* m) {
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 3; c++) {
      const float v = m[4 * r + c];
      const float lim = r < 3 ? 64.f : 1e8f;
      if (!(std::abs(v) < lim)) {
        return true;
      }
    }
  }
  return false;
}

static int test_bad_bones() {
  // every float bit pattern in every position, then random matrices of values near the limits
  static_assert(std::bit_cast<u32>(64.f) == 0x42800000u && std::bit_cast<u32>(1e8f) == 0x4cbebc20u);
  u32 bad = 0;
  float m[16] = {};
  for (int pos = 0; pos < 16; pos++) {
    for (u32 i = 0;; i += 7) {  // (every 7th pattern: 600 M per position would take long)
      m[pos] = from_u32(i);
      bad += ref_bad_bone(m) != (bool)(ctr_micro::bad_bone_bits(m) & 0x80000000u);
      if (i > 0xffffffffu - 7) {
        break;
      }
    }
    m[pos] = 0;
  }
  const float near[] = {0.f, -0.f, 63.99999f, 64.f, -64.f, 64.00001f, 1e8f, -1e8f, 99999992.f,
                        100000008.f, INFINITY, -INFINITY, NAN, -NAN, 1e-45f, 1.f, -1.f};
  std::mt19937 rng(2);
  for (int t = 0; t < 2000000; t++) {
    for (int k = 0; k < 16; k++) {
      m[k] = (rng() & 3) ? near[rng() % (sizeof(near) / 4)] : from_u32(rng());
    }
    bad += ref_bad_bone(m) != (bool)(ctr_micro::bad_bone_bits(m) & 0x80000000u);
  }
  printf("bad bones: 16 x 2^32 / 7 single values + 2M random matrices, %u differ\n", bad);
  return bad;
}

// CtrMerc.cpp handle_model's palette rows and mirror test before (AI-assisted, micro-cpu)
static void ref_rows(const float* tm, const float* scale_p, float* rows, bool* mirrors) {
  const float(*t)[4] = (const float(*)[4])tm;
  const float det = t[0][0] * (t[1][1] * t[2][2] - t[1][2] * t[2][1]) -
                    t[0][1] * (t[1][0] * t[2][2] - t[1][2] * t[2][0]) +
                    t[0][2] * (t[1][0] * t[2][1] - t[1][1] * t[2][0]);
  *mirrors = *mirrors || !(det > 0.f);
  for (int r = 0; r < 3; r++) {
    rows[4 * r + 0] = -t[0][r] * *scale_p;
    rows[4 * r + 1] = -t[1][r] * *scale_p;
    rows[4 * r + 2] = -t[2][r] * *scale_p;
    rows[4 * r + 3] = -t[3][r];
  }
}

static int test_bone_rows() {
  std::mt19937 rng(3);
  const float special[] = {0.f, -0.f, 1.f, -1.f, 1e-40f, -1e-40f, 1e-30f, INFINITY, -INFINITY, NAN,
                           1e30f, 0.5f};
  u32 bad = 0;
  for (int t = 0; t < 4000000; t++) {
    float m[16], scale;
    for (int k = 0; k < 16; k++) {
      const u32 r = rng();
      m[k] = (t % 3 == 0) ? from_u32(r)
                          : (t % 3 == 1 ? special[r % 12] : (float)(s32)r * (1.f / 1073741824.f));
    }
    scale = (t & 4) ? from_u32(rng()) : 1e-4f * (float)(rng() % 1000);
    float a[12], b[12];
    bool ma = false;
    ref_rows(m, &scale, a, &ma);
    ctr_micro::merc_bone_rows(m, scale, b);
    const bool mb = ctr_micro::merc_bone_mirrors(m);
    bad += memcmp(a, b, sizeof(a)) != 0 || ma != mb;
  }
  printf("bone rows + mirror test: 4M random bones, %u differ\n", bad);
  return bad;
}

int main() {
  int errors = 0;
  errors += test_f24();
  errors += test_bad_bones();
  errors += test_bone_rows();
  printf("%s\n", errors ? "FAIL" : "OK");
  return errors ? 1 : 0;
}
