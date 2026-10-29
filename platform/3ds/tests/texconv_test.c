/* (AI-assisted) Host test for game/graphics/ctr/CtrTexConv.h: the tiled texel conversions against
 * the old per texel code, bit exact, every power of two size 8..1024 and format. Build from the
 * repo root: cc -O2 -I. platform/3ds/tests/texconv_test.c -o /tmp/texconv_test && /tmp/texconv_test
 * (on the 3DS: include it from a 3dsx main to test the ARM paths). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game/graphics/ctr/CtrTexConv.h"
typedef uint32_t u32;
static inline u32 morton8(u32 x, u32 y) {
  return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
}
static void old_rgba8(uint8_t* dst, int w, int h, const uint8_t* rgba) {
  const int tiles_x = w / 8;
  for (int y = 0; y < h; y++) {
    const int ty = h - 1 - y;
    const uint8_t* p = rgba + 4 * y * w;
    for (int x = 0; x < w; x++, p += 4) {
      u32 tile = (u32)((ty / 8) * tiles_x + (x / 8));
      u32 off = (tile * 64 + morton8(x & 7, ty & 7)) * 4;
      dst[off + 0] = p[3]; dst[off + 1] = p[2]; dst[off + 2] = p[1]; dst[off + 3] = p[0];
    }
  }
}
static inline u32 tiled_texel(int x, int y, int w, int h) {
  const int ty = h - 1 - y;
  return (u32)((ty / 8) * (w / 8) + (x / 8)) * 64 + morton8((u32)x & 7, (u32)ty & 7);
}
/* (AI-assisted, gpu-home) the rounded quantization, worked out here (not from the header's
 * tables): the x of `bits` bits whose GPU expansion (bits repeated) is nearest to v */
static u32 q(u32 v, int bits) {
  u32 best = 0, best_d = 1000;
  for (u32 x = 0; x < (1u << bits); x++) {
    const u32 e = (x << (8 - bits)) | (x >> (2 * bits - 8));
    const u32 d = e > v ? e - v : v - e;
    if (d < best_d) {
      best_d = d;
      best = x;
    }
  }
  return best;
}
static void old_compact(void* data, int w, int h, const uint8_t* rgba, int fmt) {
  uint8_t* d8 = (uint8_t*)data; uint16_t* d16 = (uint16_t*)data;
  for (int y = 0; y < h; y++) {
    const uint8_t* p = rgba + 4 * y * w;
    for (int x = 0; x < w; x++, p += 4) {
      const u32 o = tiled_texel(x, y, w, h);
      const u32 a2 = p[3] >= 0x80 ? 255u : (u32)p[3] * 2u;
      switch (fmt) {
        case CTR_CONV_L8: d8[o] = p[1]; break;
        case CTR_CONV_LA8: d8[2 * o] = (uint8_t)a2; d8[2 * o + 1] = p[1]; break;
        case CTR_CONV_RGB565: d16[o] = (uint16_t)((q(p[0], 5) << 11) | (q(p[1], 6) << 5) | q(p[2], 5)); break;
        case CTR_CONV_RGBA5551: d16[o] = (uint16_t)((q(p[0], 5) << 11) | (q(p[1], 5) << 6) | (q(p[2], 5) << 1) | (p[3] ? 1 : 0)); break;
        default: d16[o] = (uint16_t)((q(p[0], 4) << 12) | (q(p[1], 4) << 8) | (q(p[2], 4) << 4) | q(a2, 4)); break;
      }
    }
  }
}
static void old_rgba4(uint8_t* dst, const uint16_t* src, int n) {
  for (int i = 0; i < n; i++) {
    const uint16_t v = src[i];
    dst[4 * i + 0] = (uint8_t)((v & 0xf) * 17); dst[4 * i + 1] = (uint8_t)(((v >> 4) & 0xf) * 17);
    dst[4 * i + 2] = (uint8_t)(((v >> 8) & 0xf) * 17); dst[4 * i + 3] = (uint8_t)(((v >> 12) & 0xf) * 17);
  }
}
static const int bpp[] = {4, 1, 2, 2, 2, 2};
int main(void) {
  srand(12345);
  const size_t maxb = 1024 * 1024 * 4;
  uint8_t* src = malloc(maxb + 8); uint8_t* a = malloc(maxb); uint8_t* b = malloc(maxb);
  int fails = 0, cases = 0;
  for (int w = 8; w <= 1024; w *= 2)
    for (int h = 8; h <= 1024; h *= 2)
      for (int fmt = 0; fmt < 6; fmt++) {
        /* random texels; some with alpha 0 / 0x80 / > 0x80 picked often */
        const int off = (w * h) & 4 ? 0 : 2 * (fmt & 1); /* unaligned source too */
        uint8_t* s = src + off;
        for (int i = 0; i < 4 * w * h; i++) s[i] = (uint8_t)rand();
        for (int i = 3; i < 4 * w * h; i += 4) { int r = rand() & 7; if (r == 0) s[i] = 0; else if (r == 1) s[i] = 0x80; else if (r == 2) s[i] = 0x7f; }
        const size_t bytes = (size_t)w * h * bpp[fmt];
        memset(a, 0xcd, bytes + 64 < maxb ? bytes + 64 : maxb); memset(b, 0xcd, bytes + 64 < maxb ? bytes + 64 : maxb);
        if (fmt == 0) old_rgba8(a, w, h, s); else old_compact(a, w, h, s, fmt);
        ctr_tile_texels(b, w, h, s, fmt);
        cases++;
        if (memcmp(a, b, bytes + 64 < maxb ? bytes + 64 : maxb)) { printf("FAIL w=%d h=%d fmt=%d\n", w, h, fmt); fails++; }
      }
  for (int k = 0; k < 200; k++) {
    int n = rand() % 70000; int off = 2 * (k & 1);
    for (int i = 0; i < 2 * n; i++) src[off + i] = (uint8_t)rand();
    memset(a, 0xcd, 4 * n + 16); memset(b, 0xcd, 4 * n + 16);
    uint16_t tmp[1]; (void)tmp;
    uint16_t* s16 = malloc(2 * n + 2); memcpy(s16, src + off, 2 * n);
    old_rgba4(a, s16, n); free(s16);
    ctr_rgba4_to_rgba8((uint32_t*)b, src + off, n);
    cases++;
    if (memcmp(a, b, 4 * n + 16)) { printf("FAIL rgba4 n=%d\n", n); fails++; }
  }
  /* (AI-assisted, gpu-home) the plain loop's ctr_pack16 per texel against the reference */
  for (u32 v = 0; v < 256; v++) {
    cases++;
    if (ctr_q4[v] != q(v, 4) || ctr_q5[v] != q(v, 5) || ctr_q6[v] != q(v, 6)) {
      printf("FAIL table v=%u\n", v);
      fails++;
    }
  }
  printf("%d cases, %d failures\n", cases, fails);
  return fails != 0;
}
