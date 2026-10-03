#pragma once

/*!
 * @file compressed_iso_file.h
 * (AI-assisted) Block-compressed game files for the 3DS SD card (stage_sd.sh --compress).
 *
 * A file X of out/jak1/iso may be stored as X.ogz instead: the same bytes, cut into blocks of
 * block_size (0xc000, the overlord's read page) that are compressed one by one (LZ4 or zstd), so a
 * read at any sector decodes only the blocks it touches, and a whole-block read (every DGO / CGO
 * page) decodes straight into the caller's buffer. The fake ISO reads X.ogz when it exists, else X.
 *
 *   header (32 bytes, little endian)
 *     char magic[4] = "OGCZ"; u8 version = 1; u8 codec (1 LZ4, 2 zstd); u16 0
 *     u32 block_size; u32 size (of X); u32 block_count; u32 crc32 (of X, zlib's); u32 0; u32 0
 *   u32 offsets[block_count + 1]: file offset of each block; block i is offsets[i + 1] - offsets[i]
 *     bytes, stored as is when that is its full (uncompressed) length
 *   blocks
 *
 * The decoder is shared by the game (fake_iso) and the host tool (platform/3ds/tools/iso_pack.cpp).
 */

#include <cstdint>
#include <cstdio>
#include <vector>

namespace iso_cz {

constexpr char kMagic[4] = {'O', 'G', 'C', 'Z'};
constexpr const char* kSuffix = ".ogz";
constexpr uint8_t kVersion = 1;
constexpr uint32_t kHeaderSize = 32;

enum Codec : uint8_t { CODEC_LZ4 = 1, CODEC_ZSTD = 2 };

struct Header {
  char magic[4];
  uint8_t version;
  uint8_t codec;
  uint16_t pad0;
  uint32_t block_size;
  uint32_t size;
  uint32_t block_count;
  uint32_t crc32;
  uint32_t pad1[2];
};
static_assert(sizeof(Header) == kHeaderSize);

/*!
 * zlib's CRC-32 (python zlib.crc32), incremental: crc = crc32_update(crc, data, n), starting at 0.
 */
uint32_t crc32_update(uint32_t crc, const void* data, size_t size);

/*!
 * An open .ogz file. Not thread safe: one reader at a time (the fake ISO reads one page of a
 * file at a time).
 */
class Reader {
 public:
  ~Reader();
  // takes ownership of fp (positioned anywhere). False (and fp closed) if it isn't a valid .ogz.
  bool open(FILE* fp);
  uint32_t size() const { return m_hdr.size; }
  uint32_t expected_crc32() const { return m_hdr.crc32; }
  const Header& header() const { return m_hdr; }
  // copy bytes [offset, offset + len) of the original file to dst (clamped to the file size).
  // Returns the number of bytes copied, or -1 on a read / decode error.
  int64_t read(uint32_t offset, void* dst, uint32_t len);

  // totals since open, for the load-time log
  uint64_t sd_bytes = 0;    // compressed bytes read from the card
  uint64_t out_bytes = 0;   // decoded bytes given out
  uint64_t sd_us = 0;       // time in fread
  uint64_t decode_us = 0;   // time in the decoder

 private:
  bool load_block(uint32_t block, uint8_t* dst);
  bool read_at(uint32_t pos, void* dst, uint32_t len);
  FILE* m_fp = nullptr;
  Header m_hdr{};
  std::vector<uint32_t> m_offsets;
  uint32_t m_pos = 0;          // file position of m_fp
  uint8_t* m_cbuf = nullptr;   // one compressed block
  uint8_t* m_cache = nullptr;  // one decoded block, for reads that don't cover whole blocks
  int64_t m_cache_block = -1;
  void* m_dctx = nullptr;      // zstd
};

}  // namespace iso_cz
