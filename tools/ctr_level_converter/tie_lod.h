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
#include "common/custom_data/TieMorph.h"

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
// a static instance on its own (the PS2's geomorph: each instance switches by its own distance),
// by its matrix in the tree's packed_vertices
inline u64 instance_key(u32 tree, u32 matrix) {
  return (1ull << 62) | ((u64)tree << 40) | matrix;
}
inline u32 key_tree(u64 key) {
  return (u32)((key >> 40) & 0x3fffff);
}

/*! The groups' triangles of an unpacked tie tree (slot 0: near geometry, 1: far); by_instance: the
 * static instances' each a group of their own (instance_key). */
void collect(const tfrag3::TieTree& tree,
             u32 tree_idx,
             int slot,
             std::unordered_map<u64, Group>* out,
             bool by_instance = false);

/*!
 * How much the far geometry's shape differs from the near one's: the larger of the two one-sided
 * Hausdorff distances (game units), sampled at the corners and centers of the triangles. With no
 * far triangles: the near triangles' bounding radius (the whole thing disappears).
 */
float shape_error(const Group& g);

/*!
 * (AI-assisted, lod-light) The point of triangle t nearest to p, as barycentric weights w (of t's
 * corners 0, 1, 2); returns its squared distance.
 */
float closest_point_bary(const float* p, const Tri& t, float* w);

/*! Bounding sphere (x, y, z, r) of a group's triangles (both versions). */
std::array<float, 4> bounds(const Group& g);

/*! The PS2 distances of a tie prototype (tie_ps2_dists.inc), meters; false: not in the table. */
bool ps2_dists(const std::string& level, const std::string& proto, float* mid, float* far);

/*! A level's PS2 tie geomorph data (common/custom_data/TieMorph.h), by geometry and tie tree. */
struct MorphTree {
  std::vector<tie_morph::Vertex> verts;
  std::vector<tie_morph::Proto> protos;
  std::vector<tie_morph::Inst> insts, wind;
};
struct Morph {
  std::vector<MorphTree> trees[4];
};
/*! false: no such file, or not a TieMorph.h file */
bool read_morph(const std::string& path, Morph* out);
/*! The tree's data has the TieTree's vertices, prototypes and instances. */
bool morph_matches(const MorphTree& m, const tfrag3::TieTree& tree);
/*! For each unpacked vertex of the tree (TieTree::unpack's order): its packed vertex, its matrix
 * (-1: none). */
void unpacked_sources(const tfrag3::TieTree& tree, std::vector<std::pair<u32, s32>>* out);

}  // namespace tie_lod
