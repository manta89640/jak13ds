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
  // (AI-assisted) The reader's buffers (its windows and decoded blocks: ~640 KB in window mode).
  // A Reader made with one uses it instead of its own and leaves the memory in it: the 3DS level
  // loader keeps one for all its loads, made at startup, so a load needs no 256 KB pieces of a
  // heap that is fragmented by then (they failed with bad_alloc). One Reader at a time per Buffers.
  struct Buffers {
    std::vector<uint8_t> window[2];
    std::vector<uint8_t> cache[2];
    void* zstd = nullptr;  // the zstd decoder (~100 KB, made at the first zstd block)
    Buffers() = default;
    Buffers(const Buffers&) = delete;
    Buffers& operator=(const Buffers&) = delete;
    ~Buffers();
    // (the memory for a reader of this window size and files with blocks of up to max_block)
    void reserve(size_t window_size, size_t max_block);
  };

  // (AI-assisted) window: 0, reads through a 64 KB stdio buffer and windows of kWindow packed
  // bytes. Otherwise every read from the file is one unbuffered read of `window` bytes (or one
  // exact read of a bigger piece) into the reader's window, which all pieces are copied from,
  // stored (uncompressed) blocks and uncompressed files too: on the 3DS every SD card request
  // costs several ms of its own, and a stdio buffer refills itself from scratch after each seek.
  explicit Reader(const char* path, size_t window = 0, Buffers* buffers = nullptr);
  ~Reader();
  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;

  bool ok() const { return m_f && !m_error; }
  long file_size() const { return m_file_size; }
  // the size of the raw layout (the file size for files without compression)
  uint32_t raw_size() const { return m_raw_size; }
  bool compressed() const { return !m_blocks.empty(); }
  // (AI-assisted) unbuffered reads through two windows (constructed with a window size)
  bool window_mode() const { return m_window_cap != 0; }
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
  // (AI-assisted) time spent in the file reads (seek + read, ms) and how many there were
  double read_ms = 0;
  uint32_t file_reads = 0;

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
  // (AI-assisted) bytes [offset, offset + bytes) of the file: through the window in window mode
  bool raw_read(long offset, void* dst, size_t bytes);
  size_t m_window_cap = 0;  // (AI-assisted) window mode: its size (0: stdio buffer, kWindow)
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
  // (AI-assisted) file bytes [offset, offset + size): one window (stdio mode) or two in window
  // mode, the one used longest ago replaced (a level's index and vertex data are read in turns)
  Buffers m_own;
  Buffers* m_bufs = &m_own;
  struct Window {
    std::vector<uint8_t>* buf = nullptr;  // m_bufs->window[i]
    long offset = 0;
    size_t size = 0;
    uint64_t used = 0;
  };
  Window m_windows[2];
  uint64_t m_window_uses = 0;
  // the bytes [offset, offset + bytes) if a window holds them (nullptr: none)
  const uint8_t* in_window(long offset, size_t bytes);
  // a window of `want` file bytes from offset on (nullptr on a read error)
  const uint8_t* load_window(long offset, size_t want);
  // block m_cache_block, decoded (m_bufs->cache[0], sized max_block)
  long m_cache_block = -1;
  // (AI-assisted) window mode: a second decoded block (index and vertex reads in turns share
  // blocks with the next chunk's); the one used longest ago is replaced (m_bufs->cache[1])
  long m_cache2_block = -1;
  uint32_t m_max_block = 0;
  bool m_cache2_newer = false;
};

}  // namespace c3l
