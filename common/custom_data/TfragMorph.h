#pragma once

/*!
 * @file TfragMorph.h
 * The PS2's tfrag geomorph data of a level, which the .fr3 leaves out: written by the level extractor
 * next to the level's .fr3 as <level>.tfragmorph, read by tools/ctr_level_converter.
 *
 * A tfragment has three geometries: base (.fr3 geometry 2), level0 (geometry 1: base plus
 * "interpolated" level0 points) and level1 (geometry 0: level0 plus level1 points). With the
 * level's subdivide distances close and far (bsp-header unk-data-4 / 5, 0: 30 m / 70 m;
 * update-subdivide-settings!) the meters m4 = close, m3 = close + 0.5 s, m2 = m3 + s, m1 = m2 + 2 s,
 * m0 = far, s = (far - close) / 7. draw-inline-array-tfrag picks a fragment's geometry by the view
 * depth of its sphere's nearest point: base beyond m0, level0 beyond m1, else level1. The VU program
 * moves each interpolated point by its own view depth w (tfrag-dists k0s / k1s): a level0 point
 * from where it is (w <= m1) to the midpoint of its two parents (w >= m0), a level1 point the same
 * from m2 to m1, its parents being moved first; the point's color (its second quadword) goes along.
 * So the surface at any depth is the one geometry 0 would show, and a switch shows nothing.
 *
 * Little endian: FileHeader, then per (geometry 0 / 1, tfrag tree) in extraction order a TreeHeader
 * followed by vertex_count Vertex, parallel to that TfragTree's packed_vertices.vertices.
 */

#include <cstdint>

namespace tfrag_morph {

constexpr char kMagic[4] = {'T', 'F', 'M', 'R'};
constexpr uint32_t kVersion = 1;

struct FileHeader {
  char magic[4];
  uint32_t version;
  uint32_t tree_count;
  uint32_t pad;
  float close;  // the level's subdivide distances (game units)
  float far;
  uint32_t pad2[2];
};
static_assert(sizeof(FileHeader) == 32);

struct TreeHeader {
  uint32_t geo;   // 0 or 1
  uint32_t tree;  // index in tfrag3::Level::tfrag_trees[geo]
  uint32_t vertex_count;
  uint32_t pad;
};
static_assert(sizeof(TreeHeader) == 16);

enum Kind : uint16_t { kBase = 0, kLevel0 = 1, kLevel1 = 2 };

// a packed vertex: at view depth w its position is the .fr3's + f0(w) * band0 + f1(w) * band1,
// f0 going 0 -> 1 from m1 to m0, f1 from m2 to m1 (game units, world space); its color goes from
// its own to the mean of the colors color_m at m1, then to the mean of color_t at m0 (palette
// color indices, as PreloadedVertex::color_index)
struct Vertex {
  float band0[3];
  float band1[3];
  uint16_t kind;
  uint16_t color_m[2];
  uint16_t color_t[4];
  uint16_t pad;
};
static_assert(sizeof(Vertex) == 40);

}  // namespace tfrag_morph
