#pragma once

/*!
 * @file CtrOceanNear.h
 * (AI-assisted)
 * The near ocean of config ocean = waves (CtrOcean.cpp): a grid of 3 m quads around the camera,
 * displaced by this frame's wave heights. Plain integer code without the GPU, so the host test
 * (test/test_ctr_ocean_waves.cpp) checks it against ocean-get-height.
 *
 * The wave heights are 32 x 32, 3 m apart, repeating every 96 m from the ocean map's start corner
 * (ocean-get-height, goal_src/jak1/engine/gfx/ocean/ocean.gc). The grid's origin is a multiple of
 * 24 m from the start corner, so every vertex sits on one of those points: ocean-get-height's
 * bilinear interpolation is the table value itself there, and the GPU interpolates in between
 * (things float on the same waves). Grid: 5 x 5 sub-cells of 24 m (the resolution of the ocean
 * map's water masks), the camera's sub-cell in the middle: 41 x 41 vertices over 120 m, quads only
 * in the sub-cells with water. The waves fade out between kFadeStart and kFadeEnd from the camera
 * (the grid moves in 24 m steps, the fade moves with the camera), so the grid's edge is flat and
 * meets the flat far ocean without a seam.
 *
 * Per vertex: no float, no divide, no compare of floats (the VFP11's 8-cycle latencies and the vmrs
 * stalls stay out of the loop): squared distance from per-row / per-column tables, fade weight
 * from a table, height times weight in one multiply, the color stepped along the row as two
 * packed 16-bit lanes per word.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "common/common_types.h"

namespace ctr_ocean {

constexpr int kWaves = 32;             // wave heights per side
constexpr float kWaveStep = 12288.f;   // 3 m (game units) between wave heights
constexpr int kMapCells = 48;          // 96 m cells per map side
constexpr int kColorRow = 52;          // colors per row of the map's color grid
constexpr int kMapSub = 192;           // 24 m sub-cells per map side (water mask)
constexpr int kBlocks = 5;             // 24 m sub-cells per grid side
constexpr int kQuads = 8 * kBlocks;    // 3 m quads per grid side
constexpr int kVerts = kQuads + 1;     // vertices per grid side
constexpr int kVertexCount = kVerts * kVerts;
constexpr int kBlockIndices = 8 * 8 * 6;
constexpr int kMaxIndices = kBlocks * kBlocks * kBlockIndices;
// positions: x and z in steps of 48 game units (3 m = 256 steps), y in steps of 12 (3 mm)
constexpr float kXZUnit = 48.f;
constexpr float kYUnit = 12.f;
constexpr int kStep = 256;
constexpr float kFadeStart = 24.f * 4096.f;
constexpr float kFadeEnd = 45.f * 4096.f;  // < 48 m: the grid reaches 2 sub-cells past the camera's

// c3l::Vertex's layout, written as words: x | y << 16, z (pad 0), st (unused, not written), rgba
struct Vertex {
  u32 xy;
  u32 z;
  u32 st;
  u32 rgba;
};
static_assert(sizeof(Vertex) == 16);

// ocean-get-height (ocean.gc), without the start corner's y
inline float ocean_get_height_ref(const float* heights, float start_x, float start_z, float x,
                                  float z) {
  const float f2 = 0.00008138021f * (x - start_x);
  const float f3 = 0.00008138021f * (z - start_z);
  const float fx = f2 - (float)(int)f2;
  const float fz = f3 - (float)(int)f3;
  const int a0 = (int)f2 & 31, a2 = (int)f3 & 31;
  const int v1 = (a0 + 1) & 31, a1 = (a2 + 1) & 31;
  const float h00 = heights[a2 * 32 + a0], h01 = heights[a2 * 32 + v1];
  const float h10 = heights[a1 * 32 + a0], h11 = heights[a1 * 32 + v1];
  const float top = h01 * fx + h00 * (1.f - fx);
  return (h11 * fx + h10 * (1.f - fx)) * fz + top * (1.f - fz);
}

// fade weight (8192 = full waves) by the squared distance from the camera in position steps >> 16
// (entry 255 and up: past kFadeEnd). Words: one load with a scaled index.
inline void make_fade(u32* lut) {
  for (int i = 0; i < 256; i++) {
    const float r = std::sqrt((i + 0.5f) * 65536.f) * kXZUnit;
    const float t = std::clamp((kFadeEnd - r) / (kFadeEnd - kFadeStart), 0.f, 1.f);
    lut[i] = (u32)std::lround(t * t * (3.f - 2.f * t) * 8192.f);
  }
}

// ARMv6 media instructions on the 3DS (one cycle each), the same in C elsewhere (the host test)
#if defined(__arm__) && __ARM_ARCH >= 6
// min(v >> 16, 255) for v >= 0
inline u32 sat_index(s32 v) {
  u32 r;
  asm("usat %0, #8, %1, asr #16" : "=r"(r) : "r"(v));
  return r;
}
// bytes 1 and 3 to bytes 0 and 2: (v >> 8) & 0x00ff00ff
inline u32 odd_bytes(u32 v) {
  u32 r;
  asm("uxtb16 %0, %1, ror #8" : "=r"(r) : "r"(v));
  return r;
}
// low 16 bits times low 16 bits, signed
inline s32 mul16(s32 a, u32 b) {
  s32 r;
  asm("smulbb %0, %1, %2" : "=r"(r) : "r"(a), "r"(b));
  return r;
}
// lo's low half, hi's high half
inline u32 pack_halves(u32 lo, u32 hi) {
  u32 r;
  asm("pkhbt %0, %1, %2" : "=r"(r) : "r"(lo), "r"(hi));
  return r;
}
#else
inline u32 sat_index(s32 v) {
  return std::min<u32>((u32)v >> 16, 255);
}
inline u32 odd_bytes(u32 v) {
  return (v >> 8) & 0x00ff00ffu;
}
inline s32 mul16(s32 a, u32 b) {
  return (s32)(s16)a * (s32)(s16)b;
}
inline u32 pack_halves(u32 lo, u32 hi) {
  return (lo & 0xffffu) | (hi & 0xffff0000u);
}
#endif

// the wave heights as fixed point: 1/8 of a y step (biased truncation: no float compare)
inline void quantize_heights(const float* h, s16* hq) {
  constexpr float k = 8.f / kYUnit;
  for (int i = 0; i < kWaves * kWaves; i++) {
    hq[i] = (s16)((s32)(h[i] * k + 16384.5f) - 16384);
  }
}

// the vertices. ox, oz: the grid origin in wave steps (3 m) from the start corner, multiples of 8.
// cam_x, cam_z: the camera in position steps from the grid origin. colors: the map's color grid
// (49 rows of 52 rgba, one per 96 m point).
inline void build_vertices(const s16* hq,
                           const u32* fade,
                           const u32* colors,
                           int ox,
                           int oz,
                           int cam_x,
                           int cam_z,
                           Vertex* out) {
  s32 dx2[kVerts];
  for (int c = 0; c < kVerts; c++) {
    const s32 d = c * kStep - cam_x;
    dx2[c] = d * d;
  }
  constexpr int kEnd = kMapCells * 32;  // the map's last color point, in wave steps
  for (int r = 0; r < kVerts; r++) {
    const s32 dz = r * kStep - cam_z;
    const s32 dz2 = dz * dz;
    const s16* hrow = hq + ((oz + r) & 31) * kWaves;
    const u32 zword = (u32)(r * kStep);
    // the color grid rows around this row (clamped to the map)
    const int gz = std::clamp(oz + r, 0, kEnd);
    const int cz = std::min(gz >> 5, kMapCells - 1), fz = gz - cz * 32;
    const u32* c0 = colors + cz * kColorRow;
    const u32* c1 = c0 + kColorRow;
    auto lerp_z = [&](int cx, s32* ch) {  // channels * 32
      const u32 a = c0[cx], b = c1[cx];
      for (int k = 0; k < 4; k++) {
        const s32 va = (a >> (8 * k)) & 0xff, vb = (b >> (8 * k)) & 0xff;
        ch[k] = va * (32 - fz) + vb * fz;
      }
    };
    Vertex* v = out + r * kVerts;
    int c = 0;
    while (c < kVerts) {
      // a run of vertices between two color points (outside the map: the edge's color)
      const int gx = ox + c;
      int cx, fx, n;
      bool flat = false;
      // (and never across a repeat of the wave heights: one pointer walks them)
      const int hx = gx & 31;
      if (gx < 0) {
        cx = 0, fx = 0, n = std::min({kVerts - c, -gx, 32 - hx}), flat = true;
      } else if (gx >= kEnd) {
        cx = kMapCells - 1, fx = 32, n = std::min(kVerts - c, 32 - hx), flat = true;
      } else {
        cx = gx >> 5, fx = hx, n = std::min(kVerts - c, 32 - fx);
      }
      s32 L[4], R[4], s[4], d[4];
      lerp_z(cx, L);
      lerp_z(cx + 1, R);
      for (int k = 0; k < 4; k++) {
        // 8.8 (+ 0.5: rounded), and the step per vertex (toward 0: stays between L and R)
        s[k] = ((L[k] * (32 - fx) + R[k] * fx) >> 2) + 0x80;
        d[k] = flat ? 0 : (R[k] - L[k]) / 4;
      }
      // two channels per word (r, b and g, a: 8.8 each); plain adds keep the lanes apart because
      // every lane stays in 0..0xffff
      u32 rb = (u32)s[0] + ((u32)s[2] << 16), ga = (u32)s[1] + ((u32)s[3] << 16);
      const u32 drb = (u32)d[0] + ((u32)d[2] << 16), dga = (u32)d[1] + ((u32)d[3] << 16);
      // per vertex (~20 instructions): weight = fade[min(dist^2 >> 16, 255)], y = the height
      // times the weight (rounded, in the word's high half next to x), the color's two words
      const s32* dp = dx2 + c;
      const s16* hp = hrow + hx;
      Vertex* o = v + c;
      Vertex* const o_end = o + n;
      u32 x = (u32)(c * kStep);
      do {
        const u32 w = fade[sat_index(*dp++ + dz2)];
        const s32 t = mul16(*hp++, w) + 0x8000;
        o->xy = pack_halves(x, (u32)t);
        o->z = zword;
        o->rgba = odd_bytes(rb) | (odd_bytes(ga) << 8);
        rb += drb;
        ga += dga;
        x += kStep;
      } while (++o != o_end);
      c += n;
    }
  }
}

// whether the 24 m sub-cell (sx, sz) of the map has water. water: kMapSub x kMapSub, 1 = water
inline bool sub_has_water(const u8* water, int sx, int sz) {
  return sx >= 0 && sz >= 0 && sx < kMapSub && sz < kMapSub && water[sz * kMapSub + sx];
}

// the triangles of the grid's sub-cells with water, as u16 pairs; returns the index count
inline int build_indices(const u8* water, int ox, int oz, u32* out) {
  static u32 tmpl[kBlockIndices / 2];
  static bool have_tmpl = false;
  if (!have_tmpl) {
    u16 t[kBlockIndices];
    int n = 0;
    for (int qz = 0; qz < 8; qz++) {
      for (int qx = 0; qx < 8; qx++) {
        const u16 a = (u16)(qz * kVerts + qx), b = (u16)(a + 1), c = (u16)(a + kVerts + 1),
                  d = (u16)(a + kVerts);
        const u16 q[6] = {a, b, c, a, c, d};  // like the far mesh's quads
        for (u16 k : q) {
          t[n++] = k;
        }
      }
    }
    for (int i = 0; i < kBlockIndices / 2; i++) {
      tmpl[i] = (u32)t[2 * i] | ((u32)t[2 * i + 1] << 16);
    }
    have_tmpl = true;
  }
  int n = 0;
  for (int bz = 0; bz < kBlocks; bz++) {
    for (int bx = 0; bx < kBlocks; bx++) {
      if (!sub_has_water(water, (ox >> 3) + bx, (oz >> 3) + bz)) {
        continue;
      }
      const u32 base = (u32)(bz * 8 * kVerts + bx * 8) * 0x10001u;  // both halves
      u32* o = out + n / 2;
      for (int i = 0; i < kBlockIndices / 2; i++) {
        o[i] = tmpl[i] + base;
      }
      n += kBlockIndices;
    }
  }
  return n;
}

inline int count_water_blocks(const u8* water, int ox, int oz) {
  int n = 0;
  for (int bz = 0; bz < kBlocks; bz++) {
    for (int bx = 0; bx < kBlocks; bx++) {
      n += sub_has_water(water, (ox >> 3) + bx, (oz >> 3) + bz);
    }
  }
  return n;
}

}  // namespace ctr_ocean
