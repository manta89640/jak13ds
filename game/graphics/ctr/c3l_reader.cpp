/*!
 * @file c3l_reader.cpp
 * (AI-assisted)
 * See c3l_reader.h.
 */

#include "c3l_reader.h"

#include <algorithm>
#include <cstring>

#include "common/util/Timer.h"

#include "third-party/lz4/lz4.h"
#include "third-party/zstd/lib/zstd.h"

namespace c3l {

Reader::Reader(const char* path) : m_f(fopen(path, "rb")) {
  if (!m_f) {
    return;
  }
  setvbuf(m_f, nullptr, _IOFBF, 64 * 1024);
  fseek(m_f, 0, SEEK_END);
  m_file_size = ftell(m_f);
  fseek(m_f, 0, SEEK_SET);
  m_raw_size = (uint32_t)m_file_size;
  if (!file_read(0, &m_header, sizeof(m_header))) {
    return;
  }
  if (memcmp(m_header.magic, kMagic, 4) || m_header.version < 11) {
    return;  // no compression: the file is the raw layout
  }
  BlockDir dir;
  if (!file_read(sizeof(Header), &dir, sizeof(dir)) || memcmp(dir.magic, kBlockMagic, 4) ||
      (uint64_t)dir.num_blocks * sizeof(Block) > (uint64_t)m_file_size) {
    m_error = true;
    return;
  }
  m_blocks.resize(dir.num_blocks);
  if (!file_read(sizeof(Header) + sizeof(BlockDir), m_blocks.data(),
                 m_blocks.size() * sizeof(Block))) {
    m_blocks.clear();
    return;
  }
  // the blocks must cover [128, raw_size) in order and be inside the file
  uint32_t expect = sizeof(Header), max_block = 0;
  for (const auto& b : m_blocks) {
    if (b.raw_offset != expect || b.codec() > CODEC_ZSTD ||
        (uint64_t)b.file_offset + b.packed_size() > (uint64_t)m_file_size ||
        (b.codec() == CODEC_STORED && b.packed_size() != b.raw_size)) {
      m_error = true;
      break;
    }
    expect += b.raw_size;
    max_block = std::max(max_block, b.raw_size);
  }
  if (expect != dir.raw_size || max_block > dir.max_block) {
    m_error = true;
  }
  m_raw_size = dir.raw_size;
  m_cache.resize(dir.max_block);
}

Reader::~Reader() {
  if (m_f) {
    fclose(m_f);
  }
  free_zstd_ctx(m_zstd);
}

bool Reader::file_read(long offset, void* dst, size_t bytes) {
  if (!m_f || m_error || offset < 0 || offset + (long)bytes > m_file_size) {
    m_error = true;
    return false;
  }
  if (bytes == 0) {
    return true;
  }
  if (m_pos != offset && fseek(m_f, offset, SEEK_SET) != 0) {
    m_error = true;
    return false;
  }
  if (fread(dst, 1, bytes, m_f) != bytes) {
    m_error = true;
    return false;
  }
  m_pos = offset + (long)bytes;
  file_bytes_read += bytes;
  return true;
}

void Reader::free_zstd_ctx(void* ctx) {
  if (ctx) {
    ZSTD_freeDCtx((ZSTD_DCtx*)ctx);
  }
}

bool Reader::decode_block(uint32_t codec,
                          const void* src,
                          uint32_t packed_size,
                          void* dst,
                          uint32_t raw_size,
                          void** zstd_ctx) {
  switch (codec) {
    case CODEC_STORED:
      if (packed_size != raw_size) {
        return false;
      }
      memcpy(dst, src, raw_size);
      return true;
    case CODEC_LZ4:
      return LZ4_decompress_safe((const char*)src, (char*)dst, (int)packed_size, (int)raw_size) ==
             (int)raw_size;
    case CODEC_ZSTD: {
      if (!*zstd_ctx) {
        *zstd_ctx = ZSTD_createDCtx();
        if (!*zstd_ctx) {
          return false;
        }
      }
      const size_t r =
          ZSTD_decompressDCtx((ZSTD_DCtx*)*zstd_ctx, dst, raw_size, src, packed_size);
      return !ZSTD_isError(r) && r == raw_size;
    }
    default:
      return false;
  }
}

const uint8_t* Reader::packed_bytes(size_t bi) {
  const Block& b = m_blocks[bi];
  const long start = (long)b.file_offset;
  const size_t size = b.packed_size();
  if (start >= m_window_offset && start + (long)size <= m_window_offset + (long)m_window_size) {
    return m_window.data() + (start - m_window_offset);
  }
  // a new window from this block on: as many of the following blocks as fit
  size_t want = std::max<size_t>(size, kWindow);
  want = std::min<size_t>(want, (size_t)(m_file_size - start));
  if (m_window.size() < want) {
    m_window.resize(want);
  }
  m_window_size = 0;
  if (!file_read(start, m_window.data(), want)) {
    return nullptr;
  }
  m_window_offset = start;
  m_window_size = want;
  return m_window.data();
}

bool Reader::read_block(size_t bi, uint32_t offset, uint8_t* dst, size_t bytes) {
  const Block& b = m_blocks[bi];
  const bool whole = offset == b.raw_offset && bytes == b.raw_size;
  if (b.codec() == CODEC_STORED) {
    // straight from the file (or the window, if it's there already)
    const long start = (long)b.file_offset + (long)(offset - b.raw_offset);
    if (start >= m_window_offset && start + (long)bytes <= m_window_offset + (long)m_window_size) {
      memcpy(dst, m_window.data() + (start - m_window_offset), bytes);
      return true;
    }
    return file_read(start, dst, bytes);
  }
  if (!whole && m_cache_block == (long)bi) {
    memcpy(dst, m_cache.data() + (offset - b.raw_offset), bytes);
    return true;
  }
  const uint8_t* src = packed_bytes(bi);
  if (!src) {
    return false;
  }
  uint8_t* out = whole ? dst : m_cache.data();
  Timer t;
  const bool ok = decode_block(b.codec(), src, b.packed_size(), out, b.raw_size, &m_zstd);
  decode_ms += t.getMs();
  if (!ok) {
    m_error = true;
    m_cache_block = -1;
    return false;
  }
  if (!whole) {
    m_cache_block = (long)bi;
    memcpy(dst, m_cache.data() + (offset - b.raw_offset), bytes);
  }
  return true;
}

bool Reader::read(uint32_t offset, void* dst_v, size_t bytes) {
  if (!ok() || (uint64_t)offset + bytes > m_raw_size) {
    m_error = true;
    return false;
  }
  if (bytes == 0) {
    return true;
  }
  if (m_blocks.empty()) {
    return file_read((long)offset, dst_v, bytes);
  }
  uint8_t* dst = (uint8_t*)dst_v;
  const uint32_t end = offset + (uint32_t)bytes;
  if (offset < sizeof(Header)) {
    const uint32_t n = std::min<uint32_t>(end, sizeof(Header)) - offset;
    memcpy(dst, (const uint8_t*)&m_header + offset, n);
    dst += n;
    offset += n;
  }
  if (offset >= end) {
    return true;
  }
  // the first block holding offset
  size_t bi = std::upper_bound(m_blocks.begin(), m_blocks.end(), offset,
                               [](uint32_t o, const Block& b) { return o < b.raw_offset; }) -
              m_blocks.begin() - 1;
  while (offset < end) {
    const Block& b = m_blocks[bi];
    const uint32_t n = std::min(end, b.raw_offset + b.raw_size) - offset;
    if (!read_block(bi, offset, dst, n)) {
      return false;
    }
    dst += n;
    offset += n;
    bi++;
  }
  return true;
}

}  // namespace c3l
