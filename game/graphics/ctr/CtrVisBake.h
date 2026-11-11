#pragma once

/*!
 * @file CtrVisBake.h
 * (AI-assisted, visbake) Baked visibility of a level's runs (docs/3ds-port/culling.md "3." and
 * "5."): a sidecar `<level>.c3b` next to the level's .c3l, made on the host by the soft renderer
 * (env CTR_VIS_BAKE), read by the 3DS renderer while config.ini `title_vis` (kind 1) or `cell_vis`
 * (kind 2) is on. No 3DS dependencies (test/ctr_render uses it too).
 *
 * Layout (little endian, offsets from the start of the file):
 *   Header
 *   Sample[sample_count]        kind 1: a camera (position, its four view planes' normals)
 *                                kind 2: a cell of the camera's space and a view direction
 *   u32 run_first[run_count + 1] each run's intervals: [run_first[r], run_first[r + 1])
 *   Interval[...]               sample index ranges [lo, hi] in which the run was seen
 * A run with no interval was never seen from any sample: hidden while a sample is used.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace ctr_vis_bake {

constexpr char kMagic[4] = {'C', '3', 'V', 'B'};
constexpr uint32_t kVersion = 1;
constexpr uint32_t kKindPath = 1, kKindCells = 2;

struct Header {
  char magic[4];
  uint32_t version;
  uint32_t kind;
  // the level file the runs are of: its run, chunk and draw counts (a mismatch: not used)
  uint32_t run_count, chunk_count, draw_count;
  uint32_t sample_count;
  uint32_t interval_count;
  // kind 2: the cells' grid (x0, y0, z0, cell size: game units; nx, ny, nz cells)
  float grid[4];
  uint32_t grid_n[3];
  uint32_t pad;
};
static_assert(sizeof(Header) == 64);

struct Sample {
  float pos[3];      // kind 1: the camera's position (game units); kind 2: unused
  float n[4][3];     // kind 1: the view's four planes' unit normals; kind 2: n[0] the direction
  uint32_t cell;     // kind 2: the cell (x + nx * (y + ny * z)); kind 1: unused
};
static_assert(sizeof(Sample) == 64);

struct Interval {
  uint16_t lo, hi;
};

//! Every sample index s in the sorted list `hits` grown by `grow` on each side, as intervals.
inline void to_intervals(const std::vector<uint16_t>& hits, int grow, uint32_t max_index,
                         std::vector<Interval>* out) {
  out->clear();
  for (uint16_t h : hits) {
    const uint32_t lo = h > grow ? h - grow : 0;
    const uint32_t hi = std::min<uint32_t>(h + grow, max_index);
    if (!out->empty() && lo <= (uint32_t)out->back().hi + 1) {
      out->back().hi = (uint16_t)std::max<uint32_t>(out->back().hi, hi);
    } else {
      out->push_back({(uint16_t)lo, (uint16_t)hi});
    }
  }
}

//! Is sample s in run r's intervals (run_first, iv: the file's tables)?
inline bool seen(const uint32_t* run_first, const Interval* iv, uint32_t r, uint32_t s) {
  for (uint32_t i = run_first[r]; i < run_first[r + 1]; i++) {
    if (s >= iv[i].lo && s <= iv[i].hi) {
      return true;
    }
  }
  return false;
}

//! The unit normals of a view's four planes from CtrBackgroundCamera::planes (rows x, y, z).
inline void view_normals(const float (*planes)[4], float n[4][3]) {
  for (int i = 0; i < 4; i++) {
    float l = 0;
    for (int c = 0; c < 3; c++) {
      n[i][c] = planes[c][i];
      l += n[i][c] * n[i][c];
    }
    l = l > 0 ? 1.f / std::sqrt(l) : 0.f;
    for (int c = 0; c < 3; c++) {
      n[i][c] *= l;
    }
  }
}

//! The smallest cosine between two views' matching plane normals.
inline float view_match(const float a[4][3], const float b[4][3]) {
  float m = 1.f;
  for (int i = 0; i < 4; i++) {
    m = std::min(m, a[i][0] * b[i][0] + a[i][1] * b[i][1] + a[i][2] * b[i][2]);
  }
  return m;
}

/*
 * (AI-assisted, cellvis) kind 2 (`<level>.c3c`, config.ini `cell_vis`): a sample is a cell of the
 * camera's space (kCellSize cubes, Header::grid) and a view direction bin (kYawBins x kPitchBins:
 * Sample::n[0] = yaw bin, pitch bin, frames recorded), holding every run seen in the frames the bake
 * recorded with the camera in that cell looking that way. Samples are sorted by cell, yaw, pitch.
 */
constexpr float kCellSize = 8.f * 4096.f;
constexpr int kYawBins = 12, kPitchBins = 3;
constexpr float kPitchSplit = 0.3490659f;  // 20 degrees: below -20 bin 0, above +20 bin 2
constexpr float kBinPi = 3.14159265358979323846f;

//! The view's direction: its four (inward) plane normals' sum, unit length.
inline bool view_dir(const float n[4][3], float* d) {
  float l = 0;
  for (int c = 0; c < 3; c++) {
    d[c] = n[0][c] + n[1][c] + n[2][c] + n[3][c];
    l += d[c] * d[c];
  }
  if (!(l > 1e-12f)) {
    return false;
  }
  l = 1.f / std::sqrt(l);
  for (int c = 0; c < 3; c++) {
    d[c] *= l;
  }
  return true;
}
//! A half angle (radians) around the view's direction that holds all of the view, its corners too:
//! a plane's half angle is asin(n . d); the corners are within atan(sqrt(2) tan(the largest)).
inline float view_half_angle(const float n[4][3], const float* d) {
  float m = 0;
  for (int i = 0; i < 4; i++) {
    const float s = n[i][0] * d[0] + n[i][1] * d[1] + n[i][2] * d[2];
    m = std::max(m, std::asin(std::min(1.f, std::max(-1.f, s))));
  }
  return std::atan(1.41421356f * std::tan(std::min(m, 1.5f)));
}
inline float yaw_of(const float* d) {
  return std::atan2(d[0], d[2]);
}
inline float pitch_of(const float* d) {
  return std::asin(std::min(1.f, std::max(-1.f, d[1])));
}
inline int yaw_bin(float yaw) {
  int b = (int)std::floor((yaw + kBinPi) * (kYawBins / (2.f * kBinPi)));
  b %= kYawBins;
  return b < 0 ? b + kYawBins : b;
}
inline int pitch_bin(float pitch) {
  return pitch < -kPitchSplit ? 0 : (pitch > kPitchSplit ? 2 : 1);
}
//! The bins a view of half angle h around (yaw, pitch) reaches into: yaw (bit per bin), pitch.
inline void view_bins(float yaw, float pitch, float h, uint32_t* yaw_mask, uint32_t* pitch_mask) {
  *pitch_mask = 0;
  for (int b = 0; b < kPitchBins; b++) {
    const float lo = b == 0 ? -kBinPi : (b == 1 ? -kPitchSplit : kPitchSplit);
    const float hi = b == 0 ? -kPitchSplit : (b == 1 ? kPitchSplit : kBinPi);
    if (pitch + h >= lo && pitch - h <= hi) {
      *pitch_mask |= 1u << b;
    }
  }
  // looking steeply up or down the view reaches round in yaw
  const float reach = std::fabs(pitch) + h;
  if (reach >= 1.0472f) {  // 60 degrees
    *yaw_mask = (1u << kYawBins) - 1;
    return;
  }
  const float yh = h / std::max(0.2f, std::cos(reach)) + 0.0873f;  // + 5 degrees
  *yaw_mask = 0;
  const float w = 2.f * kBinPi / kYawBins;
  const int b0 = (int)std::floor((yaw - yh + kBinPi) / w), b1 = (int)std::floor((yaw + yh + kBinPi) / w);
  for (int b = b0; b <= b1 && b < b0 + kYawBins; b++) {
    int k = b % kYawBins;
    k = k < 0 ? k + kYawBins : k;
    *yaw_mask |= 1u << k;
  }
}

//! (AI-assisted, cellvis) cell_vis's cache key: the own cell, the camera's side of it per axis, the
//! bins; fields that don't overlap (own: bits 32+, sides 20-25, yaw bins 3-14, pitch bins 0-2)
inline uint64_t cell_select_key(uint32_t own, const int side[3], uint32_t ym, uint32_t pm) {
  const uint32_t sides = (uint32_t)(side[0] + 1) | ((uint32_t)(side[1] + 1) << 2) |
                         ((uint32_t)(side[2] + 1) << 4);
  return ((uint64_t)own << 32) | ((uint64_t)sides << 20) | ((uint64_t)(ym & 0xfff) << 3) | (pm & 7);
}

//! Was run r seen in any of the samples `use` (ascending)? Each interval once, by binary search.
inline bool seen_any(const uint32_t* run_first, const Interval* iv, uint32_t r, const uint32_t* use,
                     size_t n) {
  for (uint32_t i = run_first[r]; i < run_first[r + 1]; i++) {
    const uint32_t* at = std::lower_bound(use, use + n, (uint32_t)iv[i].lo);
    if (at != use + n && *at <= iv[i].hi) {
      return true;
    }
  }
  return false;
}

}  // namespace ctr_vis_bake
