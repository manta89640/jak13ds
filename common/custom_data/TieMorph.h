#pragma once

/*!
 * @file TieMorph.h
 * The PS2's tie geomorph data of a level, which the .fr3 leaves out: written by the level extractor
 * next to the level's .fr3 as <level>.tiemorph, read by tools/ctr_level_converter.
 *
 * The PS2 draws a tie instance with one of its prototype's four geometries, by the view depth of
 * the instance's sphere minus its radius (d) against the prototype's dists (near-plane x, mid-plane
 * z, far-plane w; draw-inline-array-instance-tie): geometry 0 within 8 m or where it needs
 * clipping, geometry 1 up to the mid-plane, geometry 2 up to the far-plane, geometry 3 beyond.
 * Geometry 1 is geometry 0, and the "interpolated" vertices of geometries 0-2 move with the
 * instance's morph factor (the tie VU program: position = xyofs + morph.x * vtx): geometry 1 by
 * max(0, (d - near-plane) * rlength-near), reaching geometry 2's surface at the mid-plane,
 * geometry 2 by max(0, (d - mid-plane) * rlength-mid), reaching geometry 3's surface at the
 * far-plane. The .fr3 has each vertex at morph 0 (xyofs); this file has the rest.
 *
 * Little endian: FileHeader, then per (geometry, tie tree) in extraction order a TreeHeader
 * followed by vertex_count Vertex (parallel to that TieTree's packed_vertices.vertices),
 * proto_count Proto (its proto_names order), inst_count Inst (its packed_vertices.matrices order)
 * and wind_count Inst (its wind_instance_info order).
 */

#include <cstdint>

namespace tie_morph {

constexpr char kMagic[4] = {'T', 'M', 'R', 'F'};
constexpr uint32_t kVersion = 1;

struct FileHeader {
  char magic[4];
  uint32_t version;
  uint32_t tree_count;
  uint32_t pad;
};
static_assert(sizeof(FileHeader) == 16);

struct TreeHeader {
  uint32_t geo;   // 0..3
  uint32_t tree;  // index in tfrag3::Level::tie_trees[geo]
  uint32_t vertex_count;
  uint32_t proto_count;
  uint32_t inst_count;
  uint32_t wind_count;
  uint32_t pad[2];
};
static_assert(sizeof(TreeHeader) == 32);

// a prototype vertex: at morph factor f its position is the .fr3's + f * morph (prototype space)
struct Vertex {
  float morph[3];
  uint32_t moves;  // 1: an interpolated vertex (morph may still be zero), 0: a base point
};
static_assert(sizeof(Vertex) == 16);

struct Proto {
  float dists[4];   // near-plane, near-stiff, mid-plane, far-plane (game units)
  float rdists[4];  // rlength-near, rlength-stiff, rlength-mid (1 / game units), stiffness
};
static_assert(sizeof(Proto) == 32);

struct Inst {
  float bsphere[4];  // world: x, y, z, radius (game units)
  uint32_t proto;    // Proto index
  uint32_t pad[3];
};
static_assert(sizeof(Inst) == 32);

}  // namespace tie_morph
