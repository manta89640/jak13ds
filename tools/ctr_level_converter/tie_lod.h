#pragma once

/*!
 * @file tie_lod.h
 * (AI-assisted) c3l v17 tie levels of detail (docs/3ds-port/c3l_format.md, "Tie levels of
 * detail"): the PS2 draws each tie instance at one of four geometries (prototype-bucket-tie
 * geometry[4]) picked by its distance; the fr3 has all four (tfrag3::Level::tie_trees[geo]). The
 * converter keeps a near version (geo 0) and a far version (geo 3) of the instance groups whose
 * shape changes most, within a memory budget. This file: the groups and their shape error.
 */

#include <array>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "common/custom_data/Tfrag3Data.h"

namespace tie_lod {

// a triangle's corners, world space (game units)
using Tri = std::array<float, 9>;

/*!
 * An instance group: the triangles of one tie prototype in one visibility node of the tree (up to
 * 8 instances; tfrag3 StripDraw::VisGroup), or one wind instance. The same group exists in every
 * geometry level of the tree (the same BVH).
 */
struct Group {
  std::vector<Tri> tris[2];  // [0] the near geometry, [1] the far one
  // their corners with texture coordinates (x, y, z, s, t): for the extra vertex data
  std::vector<std::array<float, 5>> corners[2];
  u16 proto = 0xffff;        // prototype index in the tree (0xffff: a wind instance)
};

// group keys: tree index, then the visibility node and prototype (static) or the wind instance
inline u64 static_key(u32 tree, u32 vis, u32 proto) {
  return ((u64)tree << 40) | ((u64)vis << 16) | proto;
}
inline u64 wind_key(u32 tree, u32 instance) {
  return (1ull << 63) | ((u64)tree << 40) | instance;
}

/*! The groups' triangles of an unpacked tie tree (slot 0: near geometry, 1: far). */
void collect(const tfrag3::TieTree& tree, u32 tree_idx, int slot, std::unordered_map<u64, Group>* out);

/*!
 * How much the far geometry's shape differs from the near one's: the larger of the two one-sided
 * Hausdorff distances (game units), sampled at the corners and centers of the triangles. With no
 * far triangles: the near triangles' bounding radius (the whole thing disappears).
 */
float shape_error(const Group& g);

/*! Bounding sphere (x, y, z, r) of a group's triangles (both versions). */
std::array<float, 4> bounds(const Group& g);

/*! The PS2 distances of a tie prototype (tie_ps2_dists.inc), meters; false: not in the table. */
bool ps2_dists(const std::string& level, const std::string& proto, float* mid, float* far);

}  // namespace tie_lod
