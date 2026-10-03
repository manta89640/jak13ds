#pragma once

/*!
 * @file c3l_reader.h
 * (AI-assisted)
 * Reads pieces of a .c3l file by their offset in the raw (uncompressed) layout: v7..v10 files are
 * read as they are, v11 files (c3l::BlockDir) are decompressed block by block (LZ4 or zstd). Used
 * by CtrLevels (the 3DS level loader) and by ctr_level_converter to check what it wrote.
 *
 * A level file is ~10 MB: it is read piece by piece, as the loader needs it, never in one block
 * (the 3DS heap rarely has that much in one piece). Packed bytes are read from the file in windows
 * of up to kWindow bytes and decoded block by block as they come in. A block that the read covers
 * completely is decoded straight into the destination (for textures and vertices: linear memory),
 * one that it covers in part is decoded into a one-block cache first (small pieces sharing a
 * block, or reads that don't follow the block cuts).
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "game/graphics/ctr/c3l_format.h"

namespace c3l {

class Reader {
 public:
  explicit Reader(const char* path);
  ~Reader();
  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;

  bool ok() const { return m_f && !m_error; }
  long file_size() const { return m_file_size; }
  // the size of the raw layout (the file size for files without compression)
  uint32_t raw_size() const { return m_raw_size; }
  bool compressed() const { return !m_blocks.empty(); }
  const std::vector<Block>& blocks() const { return m_blocks; }

  // bytes [offset, offset + bytes) of the raw layout into dst
  bool read(uint32_t offset, void* dst, size_t bytes);
  template <typename T>
  bool read_array(uint32_t offset, size_t count, std::vector<T>* out) {
    out->resize(count);
    return read(offset, out->data(), count * sizeof(T));
  }

  // statistics: time spent decoding (ms), bytes read from the file
  double decode_ms = 0;
  uint64_t file_bytes_read = 0;

  // decode one packed block (exactly raw_size bytes out); false on corrupt data
  static bool decode_block(uint32_t codec,
                           const void* src,
                           uint32_t packed_size,
                           void* dst,
                           uint32_t raw_size,
                           void** zstd_ctx);
  static void free_zstd_ctx(void* ctx);

 private:
  static constexpr uint32_t kWindow = 128 * 1024;
  bool file_read(long offset, void* dst, size_t bytes);
  // the packed bytes of block b (read through the window)
  const uint8_t* packed_bytes(size_t b);
  bool read_block(size_t b, uint32_t offset, uint8_t* dst, size_t bytes);

  FILE* m_f = nullptr;
  long m_file_size = 0;
  long m_pos = 0;
  bool m_error = false;
  uint32_t m_raw_size = 0;
  Header m_header{};
  std::vector<Block> m_blocks;
  std::vector<uint8_t> m_window;  // packed bytes [m_window_offset, + m_window_size)
  long m_window_offset = 0;
  size_t m_window_size = 0;
  std::vector<uint8_t> m_cache;  // block m_cache_block, decoded
  long m_cache_block = -1;
  void* m_zstd = nullptr;
};

}  // namespace c3l
