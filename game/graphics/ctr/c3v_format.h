#pragma once

/*!
 * @file c3v_format.h
 * (AI-assisted)
 * "C3V": a level's finer precomputed visibility (config.ini fine_vis), a sidecar file next to its
 * .c3l (<level>.c3v), written by tools/ctr_level_converter/ctr_vis_builder.cpp from the finished
 * .c3l and read by CtrFineVis (game/graphics/ctr/CtrFineVis.cpp) for CtrLevels. See
 * docs/3ds-port/c3l_format.md, "Fine visibility (.c3v)".
 *
 * The level's camera space is cut into cubic cells; each cell where the camera can be has a set of
 * the level's pieces ("units") that may be seen from anywhere in it. The other units are hidden
 * from there for certain (conservative occlusion by the level's own opaque static geometry, see
 * the tool). The cells are kept in blocks (block x block x block cells) that the runtime reads
 * from the SD card as the camera comes near them, a few at a time (no block: nothing hidden).
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
 * Little endian. Layout: Header, BlockEntry[num_blocks + 1] (by key; the last one: key
 * 0xffffffff, offset = the end of the block data), then the blocks (BlockEntry::offset, absolute;
 * a block's size is the next entry's offset minus its own). A block (all varints are LEB128, u32):
 *   varint num_sets
 *   u16 set_offset[num_sets]   (from the start of the block)
 *   cell map: (varint run, varint value) pairs over the block's cells in key order (x fastest,
 *             then y, then z), run >= 1 cells; value 0: no set (the camera there: nothing hidden),
 *             else set value - 1
 *   sets: varint parent_delta (0: the root, else this set's parent is set index - parent_delta),
 *         varint count, count varints: the positions where the set differs from its parent (the
 *         root: from the empty set), ascending, as gaps (first position, then position - previous
 *         - 1). Bit u of a set: unit u may be seen.
 */

#include <cstddef>
#include <cstdint>

#include "game/graphics/ctr/c3l_format.h"

namespace c3v {

constexpr char kMagic[4] = {'C', '3', 'V', 'S'};
constexpr uint32_t kVersion = 2;
// pieces of a chunk without visibility runs (indices; CtrLevel.cpp run_culling uses the same)
constexpr uint32_t kRunIndices = 96 * 3;

// Header::occluders: what was used as an occluder (the runtime checks that it draws them so)
constexpr uint32_t kOccTie = 1;        // ties (kRunLodAlways runs, nearer than tie_occ_max)
constexpr uint32_t kOccShrub = 2;      // shrubs (v14+ PS2 distances)
constexpr uint32_t kOccClosed = 4;     // kDrawCullBack draws, front faces only (cull_ties not flip)
constexpr uint32_t kOccTieLod = 8;     // tie LOD far versions (tie_k_lo .. tie_k_hi)
constexpr uint32_t kOccTieNear = 16;   // tie LOD near versions too (c3l v17/v18 files)

struct Header {
  char magic[4];
  uint32_t version;
  uint32_t level_hash;  // fine_vis_level_hash of the .c3l
  uint32_t num_chunks;  // chunks covered: [0, num_chunks), all before the vista chunks
  uint32_t num_units;
  uint32_t set_words;  // (num_units + 31) / 32
  float origin[3];     // the grid's corner (game units, 4096 = 1 m)
  float cell;          // cell size (game units)
  uint32_t nx, ny, nz;  // cells (multiples of block)
  uint32_t block;       // cells per block side
  uint32_t bnx, bny, bnz;  // blocks; block (x, y, z) has key (z * bny + y) * bnx + x
  uint32_t num_blocks;     // blocks with cells (BlockEntry)
  uint32_t index_offset;   // BlockEntry[num_blocks + 1]
  uint32_t max_block;      // the largest block (bytes)
  uint32_t max_sets;       // the most sets in a block
  // occluders were only the level's own static geometry within this distance of the camera (game
  // units): the runtime uses the sets only while everything that close is drawn (draw_distance)
  float occluder_range;
  float eps;         // (statistics) the largest distance from a point of a cell to one of its samples
  float eps_coarse;  // (statistics) the same for the cells computed at twice the size (0: none)
  // the tfrag LOD tiers as occluders: detailed (lod_tier 1) only nearer than lod_near to its
  // lod_center, coarse (2) only further than lod_far (game units): the runtime uses the sets only
  // with an lod_distance that draws them so for certain (0.9 x for the detailed one; for the
  // coarse one the larger of its 1.1 x hysteresis and the 1.5 x hold of a cell in view)
  float lod_near, lod_far;
  uint32_t occluders;  // kOcc*
  // the distance factors the occluders' own distance limits were taken at (the smallest the
  // runtime may use): detail_scale, tie_distance_scale (x detail_scale), shrub_scale (x
  // detail_scale)
  float detail_min, tie_scale_min, shrub_scale_min;
  float tie_occ_max;  // ties occluded only nearer than this (game units; impostor_distance)
  // kOccTieLod: the tie LOD switch factor (CtrLevel.cpp: detail_scale x focal / tie_lod_px) the
  // versions were taken at: a near version occludes only where it's drawn for any factor >=
  // tie_k_lo, a far one for any factor <= tie_k_hi
  float tie_k_lo, tie_k_hi;
  uint32_t pad[3];
};
static_assert(sizeof(Header) == 144);

struct BlockEntry {
  uint32_t key;
  uint32_t offset;
};
static_assert(sizeof(BlockEntry) == 8);

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

// ---- blocks ----

/*! LEB128 u32 at p (< end); false on a truncated or too long one. */
inline bool read_varint(const uint8_t*& p, const uint8_t* end, uint32_t* v) {
  uint32_t r = 0;
  for (int s = 0; s < 35; s += 7) {
    if (p >= end) {
      return false;
    }
    const uint8_t b = *p++;
    r |= (uint32_t)(b & 0x7f) << s;
    if (!(b & 0x80)) {
      *v = r;
      return true;
    }
  }
  return false;
}

/*! A block's set count (0 on bad data). */
inline uint32_t block_sets(const uint8_t* blk, uint32_t size) {
  const uint8_t* p = blk;
  uint32_t n = 0;
  if (!read_varint(p, blk + size, &n) || (size_t)(p - blk) + 2 * (size_t)n > size) {
    return 0;
  }
  return n;
}

/*!
 * The set of cell `cell` (its index in the block, key order) of a block: -1 none (or bad data).
 */
inline int block_cell_set(const uint8_t* blk, uint32_t size, uint32_t cell) {
  const uint8_t* end = blk + size;
  const uint8_t* p = blk;
  uint32_t n = 0;
  if (!read_varint(p, end, &n) || (size_t)(end - p) < 2 * (size_t)n) {
    return -1;
  }
  p += 2 * (size_t)n;
  uint32_t at = 0;
  while (p < end) {
    uint32_t run = 0, value = 0;
    if (!read_varint(p, end, &run) || !read_varint(p, end, &value) || run == 0) {
      return -1;
    }
    if (cell < at + run) {
      return value == 0 || value > n ? -1 : (int)(value - 1);
    }
    at += run;
  }
  return -1;
}

/*!
 * Set `set` of a block into bits (words u32 words, all written): its root's positions, then each
 * set's on the path down to it. path: room for the block's set count (scratch). False on bad data
 * (bits then hold nothing usable).
 */
template <typename Word>
inline bool block_decode_set(const uint8_t* blk,
                             uint32_t size,
                             uint32_t set,
                             Word* bits,
                             uint32_t words,
                             uint16_t* path) {
  const uint8_t* end = blk + size;
  const uint8_t* p = blk;
  uint32_t n = 0;
  if (!read_varint(p, end, &n) || set >= n || (size_t)(end - p) < 2 * (size_t)n) {
    return false;
  }
  const uint8_t* offs = p;
  auto set_at = [&](uint32_t i) {
    return (uint32_t)offs[2 * i] | ((uint32_t)offs[2 * i + 1] << 8);
  };
  // the path up to the root
  uint32_t depth = 0;
  for (uint32_t s = set;;) {
    if (depth >= n) {
      return false;  // (a loop)
    }
    path[depth++] = (uint16_t)s;
    const uint32_t o = set_at(s);
    if (o >= size) {
      return false;
    }
    const uint8_t* q = blk + o;
    uint32_t delta = 0;
    if (!read_varint(q, end, &delta) || delta > s) {
      return false;
    }
    if (delta == 0) {
      break;
    }
    s -= delta;
  }
  for (uint32_t w = 0; w < words; w++) {
    bits[w] = 0;
  }
  const uint32_t nbits = words * 32;
  while (depth) {
    const uint8_t* q = blk + set_at(path[--depth]);
    uint32_t delta = 0, count = 0;
    if (!read_varint(q, end, &delta) || !read_varint(q, end, &count)) {
      return false;
    }
    uint32_t pos = 0;
    for (uint32_t i = 0; i < count; i++) {
      uint32_t g = 0;
      if (!read_varint(q, end, &g)) {
        return false;
      }
      pos = i == 0 ? g : pos + g + 1;
      if (pos >= nbits) {
        return false;
      }
      bits[pos >> 5] ^= (Word)1 << (pos & 31);
    }
  }
  return true;
}

}  // namespace c3v
