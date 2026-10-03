/*
 * (AI-assisted)
 * Unit test + rough microbenchmark for platform/3ds/port/ctr_mem.S (memcpy/memmove/memset).
 * Every result is compared against a byte loop over the whole buffer (guard bytes included):
 *   memcpy  sizes 0..300 + a few big ones, src and dst offsets 0..31
 *   memmove the same sizes and src offsets, dst = src + d for d in -36..36 (overlap both ways)
 *   memset  sizes 0..300 + big ones, dst offsets 0..31, several fill values
 * plus the __aeabi_ entry points. Writes sdmc:/memtest.txt. Build: ./build.sh
 * The timings are emulator ticks (indicative only); newlib's versions are linked in under
 * other names for the comparison.
 */
#include <3ds.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void* newlib_memcpy(void*, const void*, size_t);
void* newlib_memmove(void*, const void*, size_t);
void* newlib_memset(void*, int, size_t);
void __aeabi_memcpy(void*, const void*, size_t);
void __aeabi_memcpy4(void*, const void*, size_t);
void __aeabi_memmove(void*, const void*, size_t);
void __aeabi_memset(void*, size_t, int);
void __aeabi_memclr(void*, size_t);
void __aeabi_memclr8(void*, size_t);

/* called through volatile pointers so gcc cannot inline or fold them */
static void* (*volatile p_memcpy)(void*, const void*, size_t) = memcpy;
static void* (*volatile p_memmove)(void*, const void*, size_t) = memmove;
static void* (*volatile p_memset)(void*, int, size_t) = memset;
static void (*volatile p_aeabi_memcpy)(void*, const void*, size_t) = __aeabi_memcpy;
static void (*volatile p_aeabi_memcpy4)(void*, const void*, size_t) = __aeabi_memcpy4;
static void (*volatile p_aeabi_memmove)(void*, const void*, size_t) = __aeabi_memmove;
static void (*volatile p_aeabi_memset)(void*, size_t, int) = __aeabi_memset;
static void (*volatile p_aeabi_memclr)(void*, size_t) = __aeabi_memclr;
static void (*volatile p_aeabi_memclr8)(void*, size_t) = __aeabi_memclr8;

#define BIG_MAX (65536 + 64)
#define BUF (BIG_MAX + 256)
static uint8_t src_buf[BUF] __attribute__((aligned(64)));
static uint8_t dst_buf[BUF] __attribute__((aligned(64)));
static uint8_t ref_buf[BUF] __attribute__((aligned(64)));

static FILE* out;
static int failures;

static void report(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void report(const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vprintf(fmt, ap);
  va_end(ap);
  if (out) {
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fflush(out);
  }
}

static uint32_t rng = 12345;
static void fill_random(uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    rng = rng * 1103515245u + 12345u;
    p[i] = (uint8_t)(rng >> 16);
  }
}
static void byte_copy(volatile uint8_t* d, const volatile uint8_t* s, size_t n) {
  for (size_t i = 0; i < n; i++)
    d[i] = s[i];
}
static int byte_cmp(const volatile uint8_t* a, const volatile uint8_t* b, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (a[i] != b[i])
      return (int)i + 1;
  return 0;
}

static void fail(const char* what, size_t n, int so, int doff, int at) {
  if (failures++ < 20)
    report("FAIL %s n=%u src_off=%d dst_off=%d first bad byte %d\n", what, (unsigned)n, so, doff,
           at - 1);
}

static const size_t big_sizes[] = {511, 1024, 4096 + 13, 65536 + 7};
static size_t size_at(int i) {
  return i <= 300 ? (size_t)i : big_sizes[i - 301];
}
#define NUM_SIZES (301 + 4)

static void test_memcpy(const char* name, int which) {
  for (int si = 0; si < NUM_SIZES; si++) {
    size_t n = size_at(si);
    size_t span = n + 64 + 64;
    fill_random(src_buf, span);
    for (int so = 0; so < 32; so++) {
      for (int doff = 0; doff < 32; doff++) {
        if (n > 300 && ((so * 32 + doff) % 37) != 0)
          continue; /* big sizes: a spread of 28 offset pairs */
        fill_random(dst_buf, span);
        byte_copy(ref_buf, dst_buf, span);
        byte_copy(ref_buf + 32 + doff, src_buf + so, n);
        void* r = dst_buf + 32 + doff;
        switch (which) {
          case 0: r = p_memcpy(dst_buf + 32 + doff, src_buf + so, n); break;
          case 1: p_aeabi_memcpy(dst_buf + 32 + doff, src_buf + so, n); break;
          case 2:
            if ((so | doff) & 3) continue;
            p_aeabi_memcpy4(dst_buf + 32 + doff, src_buf + so, n);
            break;
        }
        int at = byte_cmp(dst_buf, ref_buf, span);
        if (at || r != dst_buf + 32 + doff)
          fail(name, n, so, doff, at);
      }
    }
  }
}

static void test_memmove(const char* name, int which) {
  for (int si = 0; si < NUM_SIZES; si++) {
    size_t n = size_at(si);
    size_t span = n + 160;
    for (int so = 0; so < 32; so++) {
      for (int d = -36; d <= 36; d++) {
        if (n > 300 && ((so * 73 + d + 36) % 41) != 0)
          continue;
        uint8_t* s = dst_buf + 64 + so;
        uint8_t* dd = s + d;
        fill_random(dst_buf, span);
        byte_copy(ref_buf, dst_buf, span);
        byte_copy(src_buf, s, n); /* reference: through a temporary */
        byte_copy(ref_buf + 64 + so + d, src_buf, n);
        void* r = dd;
        if (which == 0)
          r = p_memmove(dd, s, n);
        else
          p_aeabi_memmove(dd, s, n);
        int at = byte_cmp(dst_buf, ref_buf, span);
        if (at || r != dd)
          fail(name, n, so, so + d, at);
      }
    }
  }
}

static void test_memset(const char* name, int which) {
  static const int vals[] = {0, 0xA5, 0x17F, -1};
  for (int si = 0; si < NUM_SIZES; si++) {
    size_t n = size_at(si);
    size_t span = n + 128;
    for (int doff = 0; doff < 32; doff++) {
      for (int vi = 0; vi < 4; vi++) {
        int v = which == 0 ? vals[vi] : (which == 1 ? vals[vi] : 0);
        if (which == 2 && vi)
          continue;
        fill_random(dst_buf, span);
        byte_copy(ref_buf, dst_buf, span);
        for (size_t i = 0; i < n; i++)
          ((volatile uint8_t*)ref_buf)[32 + doff + i] = (uint8_t)v;
        void* r = dst_buf + 32 + doff;
        switch (which) {
          case 0: r = p_memset(dst_buf + 32 + doff, v, n); break;
          case 1: p_aeabi_memset(dst_buf + 32 + doff, n, v); break;
          case 2:
            if (doff & 7) p_aeabi_memclr(dst_buf + 32 + doff, n);
            else p_aeabi_memclr8(dst_buf + 32 + doff, n);
            break;
        }
        int at = byte_cmp(dst_buf, ref_buf, span);
        if (at || r != dst_buf + 32 + doff)
          fail(name, n, -1, doff, at);
      }
    }
  }
}

/* ---- rough timing (emulator ticks, indicative only) ---- */
static void* (*volatile b_cpy)(void*, const void*, size_t);
static u64 time_copy(void* (*f)(void*, const void*, size_t), size_t n, int so, int doff, int reps) {
  b_cpy = f;
  u64 t0 = svcGetSystemTick();
  for (int i = 0; i < reps; i++)
    b_cpy(dst_buf + doff, src_buf + so, n);
  return svcGetSystemTick() - t0;
}
static void* (*volatile b_set)(void*, int, size_t);
static u64 time_set(void* (*f)(void*, int, size_t), size_t n, int reps) {
  b_set = f;
  u64 t0 = svcGetSystemTick();
  for (int i = 0; i < reps; i++)
    b_set(dst_buf, i, n);
  return svcGetSystemTick() - t0;
}

static void bench(void) {
  static const struct {
    size_t n;
    int so, doff, reps;
  } cases[] = {
      {16, 0, 0, 200000}, {64, 0, 0, 100000},   {64, 1, 0, 100000},  {256, 0, 0, 40000},
      {256, 3, 1, 40000}, {4096, 0, 0, 4000},   {4096, 1, 0, 4000},  {4096, 2, 5, 4000},
      {65536, 0, 0, 200}, {65536, 3, 0, 200},
  };
  report("bench (ticks at 268 MHz, emulator: indicative only)\n");
  for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    u64 a = time_copy(newlib_memcpy, cases[i].n, cases[i].so, cases[i].doff, cases[i].reps);
    u64 b = time_copy(memcpy, cases[i].n, cases[i].so, cases[i].doff, cases[i].reps);
    report("memcpy n=%5u so=%d do=%d  newlib %.2f  new %.2f ticks/B\n", (unsigned)cases[i].n,
           cases[i].so, cases[i].doff, (double)a / ((double)cases[i].n * cases[i].reps),
           (double)b / ((double)cases[i].n * cases[i].reps));
  }
  static const size_t sn[] = {16, 256, 4096, 65536};
  static const int sr[] = {200000, 40000, 4000, 200};
  for (int i = 0; i < 4; i++) {
    u64 a = time_set(newlib_memset, sn[i], sr[i]);
    u64 b = time_set(memset, sn[i], sr[i]);
    report("memset n=%5u  newlib %.2f  new %.2f ticks/B\n", (unsigned)sn[i],
           (double)a / ((double)sn[i] * sr[i]), (double)b / ((double)sn[i] * sr[i]));
  }
}

int main(void) {
  gfxInitDefault();
  consoleInit(GFX_TOP, NULL);
  out = fopen("sdmc:/memtest.txt", "w");
  report("memtest start\n");
  u64 t0 = svcGetSystemTick();
  test_memcpy("memcpy", 0);
  test_memcpy("__aeabi_memcpy", 1);
  test_memcpy("__aeabi_memcpy4", 2);
  report("memcpy done, failures %d\n", failures);
  test_memmove("memmove", 0);
  test_memmove("__aeabi_memmove", 1);
  report("memmove done, failures %d\n", failures);
  test_memset("memset", 0);
  test_memset("__aeabi_memset", 1);
  test_memset("__aeabi_memclr", 2);
  report("memset done, failures %d\n", failures);
  report("tests took %.1f s\n", (double)(svcGetSystemTick() - t0) / SYSCLOCK_ARM11);
  bench();
  report("memtest %s (%d failures)\nDONE\n", failures ? "FAILED" : "PASSED", failures);
  if (out)
    fclose(out);
  while (aptMainLoop()) {
    gspWaitForVBlank();
    hidScanInput();
    if (hidKeysDown() & KEY_START)
      break;
  }
  gfxExit();
  return 0;
}
