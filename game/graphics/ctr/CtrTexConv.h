#pragma once

/*!
 * @file CtrTexConv.h
 * (AI-assisted)
 * Linear RGBA texels (top row first, R G B A bytes) to the PICA's tiled texture layout: 8x8 tiles
 * in rows, Morton (Z) order inside a tile, the image's rows bottom-up (the GPU samples t = 0 from
 * the last row in memory). Also built on the Mac for the host test (same results from plain C).
 *
 * Morton order puts a 2x2 block's texels next to each other and two blocks side by side (4x2
 * texels) in 8 consecutive texels, so the loops convert 4x2 groups: two source rows, 16 bytes
 * each, to one run of output words (a whole 32 byte cache line for RGBA8), with no per texel
 * address math.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__ARM_FEATURE_SIMD32)
#include <arm_acle.h>
#endif

#if defined(__ARM_FEATURE_SIMD32) && defined(__arm__) && !defined(__thumb__)
#define CTR_TEXCONV_ARM 1
#endif

enum {
  CTR_CONV_RGBA8 = 0, /* stored A, B, G, R */
  CTR_CONV_L8,        /* G */
  CTR_CONV_LA8,       /* stored A, L; alpha doubled (GS 0x80 = 1.0) */
  CTR_CONV_RGB565,
  CTR_CONV_RGBA5551, /* alpha bit: alpha != 0 */
  CTR_CONV_RGBA4,    /* alpha doubled */
  CTR_CONV_RGBA8_W,  /* (internal) RGBA8 from a 4 byte aligned source */
};

/* a texel as R | G << 8 | B << 16 | A << 24 (any alignment: one ldr on the 3DS) */
static inline uint32_t ctr_ld32(const uint8_t* p) {
  uint32_t v;
  memcpy(&v, p, 4);
  return v;
}

/* (a & 0xffff) | b << 16 and (b & 0xffff0000) | a >> 16 (pkhbt / pkhtb) */
static inline uint32_t ctr_pk_lo(uint32_t a, uint32_t b) {
#if defined(CTR_TEXCONV_ARM)
  uint32_t r;
  __asm__("pkhbt %0, %1, %2, lsl #16" : "=r"(r) : "r"(a), "r"(b));
  return r;
#else
  return (a & 0xffffu) | (b << 16);
#endif
}

static inline uint32_t ctr_pk_hi(uint32_t a, uint32_t b) {
#if defined(CTR_TEXCONV_ARM)
  uint32_t r;
  __asm__("pkhtb %0, %1, %2, asr #16" : "=r"(r) : "r"(b), "r"(a));
  return r;
#else
  return (b & 0xffff0000u) | (a >> 16);
#endif
}

/* bytes 1 and 3 to 0 and 2 (uxtb16 ror #8) */
static inline uint32_t ctr_odd_bytes(uint32_t v) {
#if defined(CTR_TEXCONV_ARM)
  uint32_t r;
  __asm__("uxtb16 %0, %1, ror #8" : "=r"(r) : "r"(v));
  return r;
#else
  return (v >> 8) & 0x00ff00ffu;
#endif
}

/* per byte min(255, 2 * b) */
static inline uint32_t ctr_double_sat_u8x4(uint32_t v) {
#if defined(__ARM_FEATURE_SIMD32)
  return __uqadd8(v, v);
#else
  const uint32_t hi = v & 0x80808080u; /* the bytes that saturate */
  return ((v & 0x7f7f7f7fu) << 1) | ((hi >> 7) * 0xffu);
#endif
}

/* the masks of a format, in registers for a whole texture (gcc would rebuild some of them from
 * immediates for every use) */
typedef struct {
  uint32_t m0, m1, m2;
} CtrConvMasks;

static inline __attribute__((always_inline)) CtrConvMasks ctr_conv_masks(int fmt) {
  CtrConvMasks k;
  switch (fmt) {
    case CTR_CONV_LA8:
      k.m0 = 0xff00ff00u, k.m1 = 0, k.m2 = 0;
      break;
    case CTR_CONV_RGB565:
      k.m0 = 0x00f800f8u, k.m1 = 0xfc00fc00u, k.m2 = 0;
      break;
    case CTR_CONV_RGBA5551:
      k.m0 = 0x00f800f8u, k.m1 = 0x00ff00ffu, k.m2 = 0x00010001u;
      break;
    default:
      k.m0 = 0x00f000f0u, k.m1 = 0xf000f000u, k.m2 = 0;
      break;
  }
  if (fmt != CTR_CONV_RGBA8 && fmt != CTR_CONV_RGBA8_W && fmt != CTR_CONV_L8) {
    __asm__("" : "+r"(k.m0), "+r"(k.m1), "+r"(k.m2));
  }
  return k;
}

/* two adjacent texels (v0, v1) to their 16 bit texels, v0 in the low half. With p = R0 G0 R1 G1
 * and q = B0 A0 B1 A1 (bytes from the low end), each field of both texels is one mask and shift. */
static inline __attribute__((always_inline)) uint32_t ctr_conv16x2(uint32_t v0,
                                                                   uint32_t v1,
                                                                   int fmt,
                                                                   CtrConvMasks k) {
  const uint32_t p = ctr_pk_lo(v0, v1), q = ctr_pk_hi(v0, v1);
  switch (fmt) {
    case CTR_CONV_LA8: /* A (doubled, saturated), L */
      return (p & k.m0) | ctr_odd_bytes(ctr_double_sat_u8x4(q));
    case CTR_CONV_RGB565:
      return ((p & k.m0) << 8) | ((p & k.m1) >> 5) | ((q & k.m0) >> 3);
    case CTR_CONV_RGBA5551:
      /* G: (p >> 8) & m0 is G's top 5 bits at 3..7; the alpha bit: (A + 255) >> 8 */
      return ((p & k.m0) << 8) | (((p >> 8) & k.m0) << 3) | ((q & k.m0) >> 2) |
             (((ctr_odd_bytes(q) + k.m1) >> 8) & k.m2);
    default: /* CTR_CONV_RGBA4: alpha doubled, its top nibble */
      return ((p & k.m0) << 8) | ((p & k.m1) >> 4) | (q & k.m0) |
             ((ctr_double_sat_u8x4(q) & k.m1) >> 12);
  }
}

/* 4x2 texels (a: 4 texels of the tile row with even y, b: the row after it in the tile) to their
 * 8 consecutive texels: a0 a1 b0 b1 a2 a3 b2 b3 */
static inline __attribute__((always_inline)) void ctr_tile_group(uint32_t* d,
                                                                 const uint8_t* a,
                                                                 const uint8_t* b,
                                                                 int fmt,
                                                                 CtrConvMasks k) {
  if (fmt == CTR_CONV_RGBA8 || fmt == CTR_CONV_RGBA8_W) {
    /* R G B A -> A B G R: a byte swap (rev) */
    d[0] = __builtin_bswap32(ctr_ld32(a));
    d[1] = __builtin_bswap32(ctr_ld32(a + 4));
    d[2] = __builtin_bswap32(ctr_ld32(b));
    d[3] = __builtin_bswap32(ctr_ld32(b + 4));
    d[4] = __builtin_bswap32(ctr_ld32(a + 8));
    d[5] = __builtin_bswap32(ctr_ld32(a + 12));
    d[6] = __builtin_bswap32(ctr_ld32(b + 8));
    d[7] = __builtin_bswap32(ctr_ld32(b + 12));
  } else if (fmt == CTR_CONV_L8) {
    /* G of each */
    d[0] = ((ctr_ld32(a) >> 8) & 0xffu) | (ctr_ld32(a + 4) & 0xff00u) |
           ((ctr_ld32(b) & 0xff00u) << 8) | ((ctr_ld32(b + 4) >> 8) << 24);
    d[1] = ((ctr_ld32(a + 8) >> 8) & 0xffu) | (ctr_ld32(a + 12) & 0xff00u) |
           ((ctr_ld32(b + 8) & 0xff00u) << 8) | ((ctr_ld32(b + 12) >> 8) << 24);
  } else {
    d[0] = ctr_conv16x2(ctr_ld32(a), ctr_ld32(a + 4), fmt, k);
    d[1] = ctr_conv16x2(ctr_ld32(b), ctr_ld32(b + 4), fmt, k);
    d[2] = ctr_conv16x2(ctr_ld32(a + 8), ctr_ld32(a + 12), fmt, k);
    d[3] = ctr_conv16x2(ctr_ld32(b + 8), ctr_ld32(b + 12), fmt, k);
  }
}

#if defined(CTR_TEXCONV_ARM)
/* RGBA8 from a 4 byte aligned source: per tile 2 x (4 + 4 texels with LDM, a whole 32 byte line
 * with one STM, registers in output order a0 a1 b0 b1 a2 a3 b2 b3) */
static inline void ctr_tile_row_rgba8_arm(uint32_t* d, const uint8_t* a, const uint8_t* b,
                                          int tiles_x) {
  const uint8_t* const end = a + 32 * tiles_x;
  __asm__ volatile(
      "1:\n\t"
      "ldm %1!, {r4, r5, r8, r9}\n\t"
      "ldm %2!, {r6, r7, r10, ip}\n\t"
      "rev r4, r4\n\t"
      "rev r5, r5\n\t"
      "rev r8, r8\n\t"
      "rev r9, r9\n\t"
      "rev r6, r6\n\t"
      "rev r7, r7\n\t"
      "rev r10, r10\n\t"
      "rev ip, ip\n\t"
      "stm %0, {r4-r10, ip}\n\t"
      "ldm %1!, {r4, r5, r8, r9}\n\t"
      "ldm %2!, {r6, r7, r10, ip}\n\t"
      "rev r4, r4\n\t"
      "rev r5, r5\n\t"
      "rev r8, r8\n\t"
      "rev r9, r9\n\t"
      "rev r6, r6\n\t"
      "rev r7, r7\n\t"
      "rev r10, r10\n\t"
      "rev ip, ip\n\t"
      "add %0, %0, #64\n\t"
      "stm %0, {r4-r10, ip}\n\t"
      "add %0, %0, #192\n\t"
      "cmp %1, %3\n\t"
      "bne 1b"
      : "+r"(d), "+r"(a), "+r"(b)
      : "r"(end)
      : "r4", "r5", "r6", "r7", "r8", "r9", "r10", "ip", "cc", "memory");
}
#endif

/* w, h multiples of 8; dst 4 byte aligned */
static inline __attribute__((always_inline)) void ctr_tile_walk(uint32_t* dst,
                                                                int w,
                                                                int h,
                                                                const uint8_t* rgba,
                                                                int fmt) {
  /* output words per 4x2 group (8 texels) */
  const int gw =
      fmt == CTR_CONV_RGBA8 || fmt == CTR_CONV_RGBA8_W ? 8 : (fmt == CTR_CONV_L8 ? 2 : 4);
  const size_t row = 4 * (size_t)w;
  const int tiles_x = w / 8;
  const CtrConvMasks k = ctr_conv_masks(fmt);
  for (int tr = 0; tr < h / 8; tr++, dst += tiles_x * 8 * gw) {
    /* tile row y = 0: the image row h - 1 - 8 * tr (bottom row first) */
    const uint8_t* y0 = rgba + (size_t)(h - 1 - 8 * tr) * row;
    for (int by = 0; by < 4; by++) {
      /* tile rows 2 * by and 2 * by + 1: groups (by & 1) | (by & 2) << 1 (x 0..3) and that + 2
       * (x 4..7), the Morton index bits being x0 y0 x1 y1 x2 y2 */
      const uint8_t* a = y0 - 2 * (size_t)by * row;
      const uint8_t* b = a - row;
      uint32_t* d = dst + gw * ((by & 1) | ((by & 2) << 1));
#if defined(CTR_TEXCONV_ARM)
      if (fmt == CTR_CONV_RGBA8_W) {
        ctr_tile_row_rgba8_arm(d, a, b, tiles_x);
        continue;
      }
#endif
      for (int tx = 0; tx < tiles_x; tx++, a += 32, b += 32, d += 8 * gw) {
        ctr_tile_group(d, a, b, fmt, k);
        ctr_tile_group(d + 2 * gw, a + 16, b + 16, fmt, k);
      }
    }
  }
}

/* linear RGBA8 -> tiled (fmt CTR_CONV_*) at dst; w, h multiples of 8, dst 4 byte aligned */
static inline void ctr_tile_texels(void* dst, int w, int h, const uint8_t* rgba, int fmt) {
  uint32_t* d = (uint32_t*)dst;
  switch (fmt) {
    case CTR_CONV_RGBA8:
      if (((uintptr_t)rgba & 3) == 0) {
        ctr_tile_walk(d, w, h, rgba, CTR_CONV_RGBA8_W);
      } else {
        ctr_tile_walk(d, w, h, rgba, CTR_CONV_RGBA8);
      }
      break;
    case CTR_CONV_L8:
      ctr_tile_walk(d, w, h, rgba, CTR_CONV_L8);
      break;
    case CTR_CONV_LA8:
      ctr_tile_walk(d, w, h, rgba, CTR_CONV_LA8);
      break;
    case CTR_CONV_RGB565:
      ctr_tile_walk(d, w, h, rgba, CTR_CONV_RGB565);
      break;
    case CTR_CONV_RGBA5551:
      ctr_tile_walk(d, w, h, rgba, CTR_CONV_RGBA5551);
      break;
    default:
      ctr_tile_walk(d, w, h, rgba, CTR_CONV_RGBA4);
      break;
  }
}

static inline uint32_t ctr_nibbles_to_bytes(uint32_t t) {
  /* t: bytes [n1n0, 0, n3n2, 0] -> each nibble in its own byte, times 17 */
  const uint32_t s = (t | (t << 4)) & 0x0f0f0f0fu;
  return s | (s << 4);
}

/* n RGBA4 texels (16 bits, nibble k = n_k) to RGBA8 words with byte k = n_k * 17, in the same
 * (already tiled) order; dst 4 byte aligned */
static inline void ctr_rgba4_to_rgba8(uint32_t* dst, const uint8_t* src, int n) {
  int i = 0;
  for (; i + 2 <= n; i += 2, src += 4, dst += 2) {
    const uint32_t v = ctr_ld32(src); /* two texels */
#if defined(__ARM_FEATURE_SIMD32)
    const uint32_t lo = __uxtb16(v); /* the low bytes: n1n0 of #0 in byte 0, of #1 in byte 2 */
#else
    const uint32_t lo = v & 0x00ff00ffu;
#endif
    const uint32_t hi = ctr_odd_bytes(v); /* the high bytes, n3n2 */
    dst[0] = ctr_nibbles_to_bytes(ctr_pk_lo(lo, hi));
    dst[1] = ctr_nibbles_to_bytes(ctr_pk_hi(lo, hi));
  }
  if (i < n) {
    const uint32_t v = (uint32_t)src[0] | ((uint32_t)src[1] << 8);
    dst[0] = ctr_nibbles_to_bytes((v & 0xffu) | ((v & 0xff00u) << 8));
  }
}
