#pragma once

/*!
 * @file c3l_pack.h
 * (AI-assisted)
 * c3l v11 compression (see c3l::BlockDir in game/graphics/ctr/c3l_format.h): cuts the raw layout
 * into blocks and packs each one with LZ4 (HC) or zstd, whichever loads faster on the New 3DS by
 * a cost model (SD read time of the packed bytes + decode time), or stores it.
 */

#include <cstdint>
#include <string>
#include <vector>

namespace c3l_pack {

enum class Mode { NONE, LZ4, ZSTD, AUTO };

struct Options {
  Mode mode = Mode::AUTO;
  int lz4_level = 12;   // LZ4HC level (12: the best ratio, decodes as fast as any)
  int zstd_level = 19;  // zstd level (decode speed hardly depends on it)
  uint32_t max_block = 64 * 1024;  // a block's raw size at most
  uint32_t min_block = 16 * 1024;  // small pieces share a block until it has this many bytes
  // the cost model (MB/s on the New 3DS): see docs/3ds-port/c3l_format.md
  double sd_mbps = 5.5;
  double lz4_mbps = 100.0;
  double zstd_mbps = 16.0;
  int threads = 0;
};

// a section of the raw layout, for the statistics and as a hard block cut
struct Section {
  std::string name;
  uint32_t start = 0, end = 0;
};

struct SectionStats {
  std::string name;
  uint64_t raw = 0;
  uint64_t lz4 = 0;     // all blocks as LZ4 (or stored, if smaller)
  uint64_t zstd = 0;    // all blocks as zstd (or stored)
  uint64_t packed = 0;  // as written
  uint32_t blocks[3] = {0, 0, 0};  // by codec: stored, LZ4, zstd
  double load_ms = 0;   // the cost model's load time of the written blocks
};

/*!
 * raw: the file in the v10 layout (header version 11). cuts: raw offsets where the runtime starts
 * reading a piece (a block starts there, unless the block is still smaller than min_block).
 * Returns the v11 file.
 */
std::vector<uint8_t> pack(const std::vector<uint8_t>& raw,
                          const std::vector<Section>& sections,
                          std::vector<uint32_t> cuts,
                          const Options& opt,
                          std::vector<SectionStats>* stats);

/*! Reads the file back with the runtime's reader (c3l::Reader): whole, and piece by piece. */
bool verify(const std::string& path, const std::vector<uint8_t>& raw,
            const std::vector<uint32_t>& cuts);

}  // namespace c3l_pack
