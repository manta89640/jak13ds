#pragma once

/*!
 * @file c3l_opt.h
 * (AI-assisted)
 * ctr_level_converter's draw optimisations that must not change a pixel (c3l v15, see
 * docs/3ds-port/c3l_format.md "Draw optimisations"):
 *  - coplanar overlaps: triangles whose drawing order decides which one a pixel shows (equal
 *    depth: the GPU's GEQUAL depth test lets the later one win), so the order between them stays;
 *  - the post-vertex cache order of a draw's triangles, keeping those orders (--vcache-fifo);
 *  - texture atlases (--atlas): small textures of opaque draws packed into one texture with
 *    gutters, so draws of a chunk with the same state merge and share one bind.
 */

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

namespace c3l_opt {

using Tri = std::array<std::array<float, 3>, 3>;  // corners, world positions

struct CoplanarOptions {
  float dist = 4.f;      // planes this close (game units, 4096 = 1 m) count as one
  float cos = 0.9998f;   // normals within ~1.1 degrees (either facing)
  float overlap = 0.5f;  // in-plane overlap deeper than this (units): touching edges don't count
};

/*!
 * Pairs (i < j) of the triangles that lie in one plane (within opt.dist) and overlap there by more
 * than opt.overlap: drawn with an equal depth, the later one wins. groups[i] (if not empty): only
 * pairs of triangles with the same group number are looked for (a draw's runs, a chunk...).
 * Degenerate triangles have no pairs.
 */
std::vector<std::pair<uint32_t, uint32_t>> coplanar_pairs(const std::vector<Tri>& tris,
                                                          const std::vector<uint32_t>& groups,
                                                          const CoplanarOptions& opt);

/*!
 * Triangle order for a post-vertex cache of `fifo` entries (first in, first out; a hit doesn't
 * move an entry), Tom Forsyth's linear-speed greedy method with the score of the simulated FIFO:
 * the next triangle is the best scored one among the triangles of the vertices in the cache (else
 * the earliest unused one). before[t] (may be empty): triangles that must come before triangle t
 * (the original order of coplanar overlaps): t waits until all of them are out.
 * indices: 3 per triangle (each triangle's corner order is kept). Returns the new triangle order.
 */
std::vector<uint32_t> vcache_order(const std::vector<uint32_t>& indices,
                                   uint32_t vertex_count,
                                   int fifo,
                                   const std::vector<std::vector<uint32_t>>& before);

/*!
 * A texture in an atlas: its encoded levels (the 3DS layout, ctr_texture.h), size, and where its
 * first level goes (texels; multiples of 4 << (levels - 1), so every level's blocks line up), and
 * how its draws address it (clamped or repeating, per axis: what its gutters hold).
 */
struct AtlasCell {
  const uint8_t* data = nullptr;
  int w = 0, h = 0;
  int x = 0, y = 0;
  bool clamp_s = false, clamp_t = false;
};
struct AtlasStats {
  // gutter texels against what the texture's own addressing would read there (decoded, rgba)
  uint64_t gutter_texels = 0;
  double gutter_sq = 0;
  int gutter_max = 0;
  bool cells_exact = true;  // every cell's texels decoded from the atlas = the texture's own
};
/*!
 * The atlas texture (aw x ah, `levels` levels, format: ctr_tex::Format) with the cells: each cell's
 * blocks copied as they are (the same texels at every level), the gutters around them (half the
 * gap between cells: 2 << (levels - 1 - level) texels each side) filled with what the texture's
 * own addressing reads beyond its edge (repeat: the other side; clamp: the edge texel), encoded
 * (ETC1: approximately; the blocks a gutter shares with another cell's).
 */
std::vector<uint8_t> build_atlas(int aw,
                                 int ah,
                                 int format,
                                 int levels,
                                 const std::vector<AtlasCell>& cells,
                                 int etc1_quality,
                                 AtlasStats* stats);

/*! Vertices shaded for these indices with a FIFO cache of `fifo` entries cleared at the start. */
uint64_t fifo_misses(const uint32_t* indices, size_t count, int fifo);

}  // namespace c3l_opt
