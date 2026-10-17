// (AI-assisted) goal_c_ops.h: the lane-by-lane 128-bit integer ops (gc_pcgtw, gc_pshuflw, ...)
// against the gc_m128 byte/halfword loop versions (gc_*_u), bit for bit, on random and special
// inputs (NaN and infinity bit patterns, sign bits, 0, -1), every shuffle control and shift amount.
//   cc -std=gnu11 -O2 -fno-strict-aliasing -Igoalc/cbackend -Igame/kernel/common \
//      test/goal_c_ops/lanes.c -o /tmp/lanes && /tmp/lanes
// (also -O0, and -arch x86_64 on an Apple silicon Mac). -DLANES_BARE: no stdio (main returns
// the number of mismatches, g_first has the first one), -DLANES_ITERS=N: fewer random cases (for
// running the 3DS compiler's code under an ARM emulator).
#ifndef LANES_BARE
#include <stdio.h>
#endif
#ifndef LANES_ITERS
#define LANES_ITERS 2000000
#endif

#include "goal_c_ops.h"

static u64 g_rng = 0x9e3779b97f4a7c15ull;
static u32 rnd32(void) {
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 7;
  g_rng ^= g_rng << 17;
  return (u32)(g_rng >> 16);
}
static const u32 kSpecial[] = {0,          1,          0xffffffffu, 0x80000000u, 0x7fffffffu,
                               0x7f800000u, 0xff800000u, 0x7fc00000u, 0xffc00000u, 0x7f800001u,
                               0xff800001u, 0x7fbfffffu, 0x00000001u, 0x80000001u, 0x0000ffffu,
                               0xffff0000u, 0x00ff00ffu, 0xff00ff00u, 0x3f800000u, 0x00007fffu};
#define NSPECIAL (sizeof(kSpecial) / sizeof(kSpecial[0]))
static u32 word(void) {
  const u32 r = rnd32();
  switch (r & 3) {
    case 0:
      return kSpecial[(r >> 2) % NSPECIAL];
    case 1:  // a special value with some random halfword/byte
      return kSpecial[(r >> 2) % NSPECIAL] ^ ((rnd32() & 0xff) << (8 * ((r >> 10) & 3)));
    default:
      return rnd32();
  }
}
static v128 vec(void) {
  v128 v = {gc_bitsf(word()), gc_bitsf(word()), gc_bitsf(word()), gc_bitsf(word())};
  return v;
}
static long g_fail, g_checks;
u32 g_first[17];
static void check(const char* name, u32 arg, v128 a, v128 b, v128 got, v128 want) {
  g_checks++;
  if (memcmp(&got, &want, 16) != 0) {
#ifdef LANES_BARE
    if (g_fail++ == 0) {
      memcpy(g_first, &a, 16);
      memcpy(g_first + 4, &b, 16);
      memcpy(g_first + 8, &got, 16);
      memcpy(g_first + 12, &want, 16);
      g_first[16] = arg;
    }
    (void)name;
#else
    if (g_fail++ < 20) {
      const u32* g = (const u32*)&got;
      const u32* w = (const u32*)&want;
      const u32* x = (const u32*)&a;
      const u32* y = (const u32*)&b;
      printf("%s(%u): a %08x %08x %08x %08x b %08x %08x %08x %08x got %08x %08x %08x %08x want "
             "%08x %08x %08x %08x\n",
             name, arg, x[0], x[1], x[2], x[3], y[0], y[1], y[2], y[3], g[0], g[1], g[2], g[3],
             w[0], w[1], w[2], w[3]);
    }
#endif
  }
}
#define CHK2(op)                                                       \
  check(#op, 0, a, b, gc_##op(a, b), gc_um(gc_##op##_u(gc_mu(a), gc_mu(b))))
#define CHK1(op, n) check(#op, n, a, b, gc_##op(a, n), gc_um(gc_##op##_u(gc_mu(a), n)))

int main(void) {
  for (int it = 0; it < LANES_ITERS; it++) {
    const v128 a = vec();
    v128 b = vec();
    if ((it & 7) == 0) {
      b = a;  // equal lanes for the compares
    }
    CHK2(pextlb);
    CHK2(pextub);
    CHK2(pextlh);
    CHK2(pextuh);
    CHK2(pextlw);
    CHK2(pextuw);
    CHK2(pcpyld);
    CHK2(pcpyud);
    CHK2(psubw);
    CHK2(pceqw);
    CHK2(pcgtw);
    CHK2(por);
    CHK2(pxor);
    CHK2(pand);
    const u32 n = rnd32() % 40;
    CHK1(pw_sll, n);
    CHK1(pw_srl, n);
    CHK1(pw_sra, n);
    const u32 bytes = rnd32() % 20;
    CHK1(psrldq, bytes);
    CHK1(pslldq, bytes);
    const u32 c = rnd32() & 255;
    CHK1(pshuflw, c);
    CHK1(pshufhw, c);
  }
  // every constant shift and shuffle control
  for (int it = 0; it < LANES_ITERS / 1000; it++) {
    const v128 a = vec(), b = a;
    for (u32 n = 0; n < 64; n++) {
      CHK1(pw_sll, n);
      CHK1(pw_srl, n);
      CHK1(pw_sra, n);
      CHK1(psrldq, n);
      CHK1(pslldq, n);
    }
    for (u32 c = 0; c < 256; c++) {
      CHK1(pshuflw, c);
      CHK1(pshufhw, c);
    }
  }
#ifdef LANES_BARE
  return (int)g_fail;
#else
  printf("goal_c_ops lanes: %ld checks, %ld mismatches\n", g_checks, g_fail);
  return g_fail != 0;
#endif
}
