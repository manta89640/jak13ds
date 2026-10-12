#pragma once

/*!
 * @file c3v_format.h
 * (AI-assisted)
 * "C3V": a level's finer precomputed visibility (config.ini fine_vis), a sidecar file next to its
 * .c3l (<level>.c3v), written by tools/ctr_level_converter/ctr_vis_builder.cpp from the finished
 * .c3l and read by CtrLevels (game/graphics/ctr/CtrLevel.cpp). See docs/3ds-port/c3l_format.md,
 * "Fine visibility (.c3v)".
 *
 * The level's camera space is cut into cubic cells; each cell where the camera can be has a set of
 * the level's pieces ("units") that may be seen from anywhere in it. The other units are hidden
 * from there for certain (conservative occlusion by the level's own opaque tfrag, see the tool).
 *
 * Units: the chunks before the first vista chunk (the vista tiers are never hidden by it), in
 * order; a chunk with visibility runs (c3l::VisRun) has one unit per run, in order; one without has
 * its draws cut in pieces of kRunIndices indices (the last one shorter), draw by draw: the same
 * pieces CtrLevels makes at load for run_culling. Unit u of chunk c is then its run
 * chunk_first_run[c] + (u - first unit of c).
 *
 * The file is keyed to the .c3l it was made from by level_hash (fine_vis_level_hash below, from
 * the chunk, draw and run tables the runtime has in memory): a .c3l converted again with other
 * chunks, draws or runs makes the runtime ignore the .c3v (the level renders as without it) until
 * the tool is run again on the new file.
 *
 * Little endian. Layout: Header, CellRun[num_cell_runs] (by start), then num_sets sets of
 * set_words u32 words each (bit u & 31 of word u >> 5: unit u may be seen).
 */

#include <cstdint>

#include "game/graphics/ctr/c3l_format.h"

namespace c3v {

constexpr char kMagic[4] = {'C', '3', 'V', 'S'};
constexpr uint32_t kVersion = 1;
// pieces of a chunk without visibility runs (indices; CtrLevel.cpp run_culling uses the same)
constexpr uint32_t kRunIndices = 96 * 3;

struct Header {
  char magic[4];
  uint32_t version;
  uint32_t level_hash;  // fine_vis_level_hash of the .c3l
  uint32_t num_chunks;  // chunks covered: [0, num_chunks), all before the vista chunks
  uint32_t num_units;
  uint32_t num_sets;
  uint32_t set_words;  // (num_units + 31) / 32
  uint32_t num_cell_runs;
  float origin[3];  // the grid's corner (game units, 4096 = 1 m)
  float cell;       // cell size (game units)
  uint32_t nx, ny, nz;  // cells; cell (x, y, z) has key (z * ny + y) * nx + x
  // occluders were only the level's opaque tfrag within this distance of the camera (game
  // units): the runtime uses the sets only while everything that close is drawn (draw_distance)
  float occluder_range;
  float eps;  // (statistics) the largest distance from a point of a cell to one of its samples
  // the LOD tiers as occluders: detailed tfrag (lod_tier 1) only nearer than lod_near to its
  // lod_center, coarse (2) only further than lod_far (game units): the runtime uses the sets only
  // with an lod_distance that draws them so for certain (0.9 x .. 1.1 x with its hysteresis)
  float lod_near, lod_far;
  uint32_t pad;
};
static_assert(sizeof(Header) == 80);

/*! cells key .. key + count - 1 (along x) use set `set` */
struct CellRun {
  uint32_t key;
  uint16_t count;
  uint16_t set;
};
static_assert(sizeof(CellRun) == 8);

inline uint32_t fnv(uint32_t h, const void* data, size_t n) {
  const uint8_t* p = (const uint8_t*)data;
  for (size_t i = 0; i < n; i++) {
    h = (h ^ p[i]) * 16777619u;
  }
  return h;
}

inline bool is_vista(const c3l::Chunk& ch) {
  return ch.kind == c3l::CHUNK_KIND_VISTA || ch.kind == c3l::CHUNK_KIND_VISTA_FLAT;
}

/*! The chunks the units cover: those before the first vista chunk. */
inline uint32_t covered_chunks(const c3l::Chunk* chunks, uint32_t n) {
  uint32_t i = 0;
  while (i < n && !is_vista(chunks[i])) {
    i++;
  }
  return i;
}

/*! Units of a chunk (see the file comment). */
inline uint32_t chunk_units(const c3l::Chunk& ch, const c3l::Draw* draws, uint32_t num_draws) {
  if (ch.vis_run_count) {
    return ch.vis_run_count;
  }
  uint32_t n = 0;
  for (uint32_t k = 0; k < ch.draw_count && ch.first_draw + k < num_draws; k++) {
    n += (draws[ch.first_draw + k].index_count + kRunIndices - 1) / kRunIndices;
  }
  return n;
}

/*!
 * The .c3l's fingerprint: chunks [0, num_chunks) (not their file offsets), their draws' index
 * counts and kinds, and their visibility runs (runs[chunk_first_run[c] ...], vis_run_count of
 * them). Index order within a run, textures and draw modes are not part of it.
 */
template <typename Index>  // (u32 differs from uint32_t on the 3DS)
inline uint32_t fine_vis_level_hash(const c3l::Chunk* chunks,
                                    uint32_t num_chunks,
                                    const c3l::Draw* draws,
                                    uint32_t num_draws,
                                    const c3l::VisRun* runs,
                                    const Index* chunk_first_run) {
  uint32_t h = fnv(2166136261u, &num_chunks, 4);
  for (uint32_t c = 0; c < num_chunks; c++) {
    const c3l::Chunk& ch = chunks[c];
    h = fnv(h, ch.bsphere, sizeof(ch.bsphere));
    h = fnv(h, ch.origin, sizeof(ch.origin));
    h = fnv(h, &ch.scale, 4);
    h = fnv(h, &ch.vertex_count, 4);
    h = fnv(h, &ch.draw_count, 4);
    h = fnv(h, &ch.max_dist, 4);
    h = fnv(h, &ch.lod_tier, 4);
    h = fnv(h, ch.lod_center, sizeof(ch.lod_center));
    h = fnv(h, &ch.vis_run_count, 4);
    h = fnv(h, &ch.kind, 4);
    for (uint32_t k = 0; k < ch.draw_count && ch.first_draw + k < num_draws; k++) {
      const c3l::Draw& d = draws[ch.first_draw + k];
      h = fnv(h, &d.index_count, 4);
      h = fnv(h, &d.kind, 2);
    }
    for (uint32_t r = 0; r < ch.vis_run_count; r++) {
      const c3l::VisRun& run = runs[chunk_first_run[c] + r];
      h = fnv(h, &run.draw, 2);
      h = fnv(h, &run.vis, 2);
      h = fnv(h, &run.index_count, 4);
    }
  }
  return h;
}

}  // namespace c3v
