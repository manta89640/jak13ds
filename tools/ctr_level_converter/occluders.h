#pragma once

/*!
 * @file occluders.h
 * (AI-assisted) Occluders for the 3DS level renderer (c3l::ExtOcc, docs/3ds-port/c3l_format.md
 * "Occluders"): a box inside a closed tie surface, found on a voxel grid of the surface's inside and
 * then checked exactly (no triangle of the surface touches it, its center is inside).
 */

#include <array>
#include <vector>

#include "game/graphics/ctr/CtrCull.h"

namespace occluders {

using Tri = std::array<std::array<double, 3>, 3>;

struct Params {
  double min_half = 0.5 * 4096.0;          // each half extent at least (0.5 m)
  double min_faces = 6.0 * 4096.0 * 4096.0;  // h0 h1 + h1 h2 + h2 h0 at least (6 m^2: a 1.5 m cube)
  int grid = 24;                           // voxels along the surface's longest side
  double min_cell = 0.2 * 4096.0;          // the smallest voxel
};

/*!
 * The largest box found inside the closed surface `tris` (any winding): a box upright in the
 * world (y up) at one of a few yaw angles. False: none of at least Params' size.
 */
bool inscribed_box(const std::vector<Tri>& tris, const Params& p, ctr_cull::Box* out);

//! Does triangle t touch box b grown by `grow` on every side (separating axis test)?
bool tri_box_overlap(const ctr_cull::Box& b, const Tri& t, double grow);
//! Is p inside the closed surface (crossings of rays in three directions, the majority)?
bool point_inside(const std::vector<Tri>& tris, const double* p);
//! Does the box grown by `grow` fit inside: no triangle touches it and its center is inside?
bool box_fits(const std::vector<Tri>& tris, const ctr_cull::Box& b, double grow);

}  // namespace occluders
