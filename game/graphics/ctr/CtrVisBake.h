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

}  // namespace ctr_vis_bake
