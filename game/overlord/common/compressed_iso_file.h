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
 * Codec 3 (zstd + ADPCM split, for the VAGWAD voice streams): the file is PS2 ADPCM, 16-byte
 * frames of 2 header bytes (shift/filter, flags) + 14 bytes of sample nibbles. A block is stored
 * as u32 n, then a zstd frame (n bytes) of all its frames' header bytes, then a zstd frame of all
 * their nibble bytes (+ the block's bytes past its last whole frame): each stream compresses much
 * better alone (~0.67 of the original vs 0.72 for plain zstd).
 *
 * The decoder is shared by the game (fake_iso) and the host tool (platform/3ds/tools/iso_pack.cpp).
 */

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

namespace iso_cz {

constexpr char kMagic[4] = {'O', 'G', 'C', 'Z'};
constexpr const char* kSuffix = ".ogz";
constexpr uint8_t kVersion = 1;
constexpr uint32_t kHeaderSize = 32;

enum Codec : uint8_t { CODEC_LZ4 = 1, CODEC_ZSTD = 2, CODEC_ZSTD_ADPCM = 3 };

// ADPCM frame split (codec 3): frames = n / 16
// split: headers[2 * frames], rest[n - 2 * frames] <- data[n]
void adpcm_split(const uint8_t* data, uint32_t n, uint8_t* headers, uint8_t* rest);
// join in place: data[2 * frames ...] holds rest, headers elsewhere -> data[n]
void adpcm_join_in_place(uint8_t* data, uint32_t n, const uint8_t* headers);

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
  // With a path, the block index is kept for the next open of the same file (the voice WAD is
  // opened for every line of dialogue).
  bool open(FILE* fp, const char* path = nullptr);
  uint32_t size() const { return m_hdr.size; }
  uint32_t expected_crc32() const { return m_hdr.crc32; }
  const Header& header() const { return m_hdr; }
  FILE* file() const { return m_fp; }
  // copy bytes [offset, offset + len) of the original file to dst (clamped to the file size).
  // Returns the number of bytes copied, or -1 on a read / decode error.
  int64_t read(uint32_t offset, void* dst, uint32_t len);

  // Read-ahead: each card read takes up to `bytes` of whole compressed blocks (at least one
  // block) into a window that the next blocks are decoded from. 0 (the default): one block per
  // card read. On the 3DS every SD card request costs several ms of its own, so a DGO read in
  // ~31 KB blocks ran at ~3 MB/s. The window is allocated at the first read (no window when that
  // fails: one block per read, as without).
  void set_read_ahead(uint32_t bytes);
  // Read-ahead doesn't go past the block that holds byte end - 1 of the original file (0: the
  // end of the file): a load of part of a file (a spool chunk) reads no more than that part.
  void set_read_end(uint32_t end) { m_read_end = end; }
  // Free the buffers, keeping the open file, its index and (keep_block) the last decoded block:
  // for a file that stays open between loads (fake ISO). They are allocated again when needed.
  // Also ends the read-ahead and the read end.
  void release_buffers(bool keep_block);
  // zero the totals below (a file kept open for the next load)
  void reset_stats();

  // totals since open, for the load-time log
  uint64_t sd_bytes = 0;    // compressed bytes read from the card
  uint64_t out_bytes = 0;   // decoded bytes given out
  uint64_t sd_us = 0;       // time in fread
  uint64_t decode_us = 0;   // time in the decoder
  uint32_t blocks = 0;        // blocks decoded
  uint64_t max_decode_us = 0;  // slowest block
  uint32_t sd_reads = 0;      // card reads (fread calls)

 private:
  bool load_block(uint32_t block, uint8_t* dst);
  bool window(uint32_t block, const uint8_t** src);
  bool read_at(uint32_t pos, void* dst, uint32_t len);
  FILE* m_fp = nullptr;
  Header m_hdr{};
  std::shared_ptr<const std::vector<uint32_t>> m_index;
  const uint32_t* m_offsets = nullptr;
  uint32_t m_pos = 0;          // file position of m_fp
  uint8_t* m_cbuf = nullptr;   // one compressed block
  uint8_t* m_cache = nullptr;  // one decoded block, for reads that don't cover whole blocks
  int64_t m_cache_block = -1;
  uint8_t* m_hbuf = nullptr;   // codec 3: one block's frame headers
  // read-ahead window: compressed bytes [m_ra_pos, m_ra_pos + m_ra_len) of the file
  uint8_t* m_ra = nullptr;
  uint32_t m_ra_cap = 0;  // 0: no read-ahead
  uint32_t m_ra_pos = 0;
  uint32_t m_ra_len = 0;
  uint32_t m_read_end = 0;
};

}  // namespace iso_cz
