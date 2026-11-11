#pragma once

/*!
 * @file horizon.h
 * (AI-assisted, horizon) The terrain grid for horizon culling (c3l::ExtHorizon,
 * docs/3ds-port/culling.md "2."): per cell, whether the level's opaque tfrag covers all of it in one
 * layer in every version that reaches it, and the lowest and highest point of what reaches it.
 */

#include <cstdint>
#include <vector>

namespace horizon {

struct TerrainTri {
  float p[3][3];     // corners (game units)
  float ymin, ymax;  // the heights its corners take, morph offsets included
  uint32_t cls;      // the version (tier, geomorph version): each one present must cover a cell
};

struct Grid {
  float x0 = 0, z0 = 0, cell = 0, base_y = 0;
  int nx = 0, nz = 0;
  std::vector<uint16_t> minh;  // ctr_cull::kHorizonUncovered, or quarter meters above base_y
  std::vector<uint8_t> span;   // maxh - minh in half meters, rounded up (255: or more)
  int covered = 0;
};

struct Params {
  float cell_m = 8.f;    // cell size (meters), larger if the level needs more than max_cells
  int max_cells = 192;   // per side
  int samples = 5;       // coverage samples per side of a cell
  float layer_m = 2.f;   // two non-steep surfaces of one version further apart: not one layer
  float steep = 0.3f;    // |normal y| below this: a wall (not checked for layers)
};

Grid build(const std::vector<TerrainTri>& tris, const Params& p = {});

}  // namespace horizon
