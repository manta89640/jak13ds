/* (AI-assisted, micro-gpu) Host test for ctr_gpu_citro3d.c raw_f24 (float32 -> PICA float24, the
 * mantissa rounded to nearest): the branchless form against the branching one it replaced, for all
 * 2^32 inputs (zeros, denormals, infinities and NaNs too). ~6 s. Build from the repo root:
 *   cc -O2 platform/3ds/tests/f24_test.c -o /tmp/f24_test && /tmp/f24_test
 * Keep new_f24 the same as raw_f24 in game/graphics/ctr/ctr_gpu_citro3d.c. */
#include <stdint.h>
#include <stdio.h>
typedef uint32_t u32;
typedef int32_t s32;

/* the form before micro-gpu */
static u32 old_f24(u32 i) {
  const u32 m23 = (i & 0x7fffff) + 0x40;
  const u32 sign = i >> 31, mant = (m23 >> 7) & 0xffff;
  const s32 e = (s32)((i >> 23) & 0xff) - 127 + 63 + (s32)(m23 >> 23);
  if (e < 0 || !((i >> 23) & 0xff)) {
    return sign << 23;
  }
  if (e > 0x7f) {
    return (sign << 23) | (0x7fu << 16);
  }
  return (sign << 23) | ((u32)e << 16) | mant;
}

/* raw_f24 now */
static u32 new_f24(u32 i) {
  s32 t = (s32)(((i & 0x7fffffffu) + 0x40u) >> 7) - (s32)(64u << 16);
  t = t < 0 ? 0 : t;
  t = t >= (s32)(0x80u << 16) ? (s32)(0x7fu << 16) : t;
  return ((i >> 8) & 0x800000u) | (u32)t;
}

int main(void) {
  u32 bad = 0, i = 0;
  do {
    const u32 a = old_f24(i), b = new_f24(i);
    if (a != b) {
      if (bad < 10) {
        printf("%08x: %06x / %06x\n", i, a, b);
      }
      bad++;
    }
    i++;
  } while (i != 0);
  printf("f24_test: %u of 2^32 inputs differ\n", bad);
  return bad != 0;
}
