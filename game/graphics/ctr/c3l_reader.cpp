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

Reader::Buffers::~Buffers() {
  free_zstd_ctx(zstd);
}

void Reader::Buffers::reserve(size_t window_size, size_t max_block) {
  const size_t w = window_size ? window_size : kWindow;
  for (auto& b : window) {
    if (b.size() < w) {
      b.resize(w);
    }
    if (!window_size) {
      break;  // (stdio mode: one window)
    }
  }
  for (auto& c : cache) {
    if (c.size() < max_block) {
      c.resize(max_block);
    }
    if (!window_size) {
      break;
    }
  }
}

Reader::Reader(const char* path, size_t window, Buffers* buffers)
    : m_f(fopen(path, "rb")), m_window_cap(window) {
  if (buffers) {
    m_bufs = buffers;
  }
  m_windows[0].buf = &m_bufs->window[0];
  m_windows[1].buf = &m_bufs->window[1];
  if (!m_f) {
    return;
  }
  if (m_window_cap) {
    setvbuf(m_f, nullptr, _IONBF, 0);  // (AI-assisted) window mode: one request per read
  } else {
    setvbuf(m_f, nullptr, _IOFBF, 64 * 1024);
  }
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
  m_own_blocks.resize(dir.num_blocks);
  if (!file_read(sizeof(Header) + sizeof(BlockDir), m_own_blocks.data(),
                 m_own_blocks.size() * sizeof(Block))) {
    m_own_blocks.clear();
    return;
  }
  // the blocks must cover [128, raw_size) in order and be inside the file
  uint32_t expect = sizeof(Header), max_block = 0;
  for (const auto& b : (*m_bl)) {
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
  m_max_block = dir.max_block;
  if (m_bufs->cache[0].size() < dir.max_block) {
    m_bufs->cache[0].resize(dir.max_block);
  }
}

Reader::Reader(const char* path, size_t window, Buffers* buffers, const Dir& dir)
    : m_f(fopen(path, "rb")), m_window_cap(window) {
  if (buffers) {
    m_bufs = buffers;
  }
  m_windows[0].buf = &m_bufs->window[0];
  m_windows[1].buf = &m_bufs->window[1];
  if (!m_f) {
    return;
  }
  setvbuf(m_f, nullptr, m_window_cap ? _IONBF : _IOFBF, m_window_cap ? 0 : 64 * 1024);
  fseek(m_f, 0, SEEK_END);
  m_file_size = ftell(m_f);
  fseek(m_f, 0, SEEK_SET);
  m_header = dir.header;
  m_bl = &dir.blocks;  // (the caller keeps dir while this reader is used)
  m_raw_size = dir.blocks.empty() ? (uint32_t)m_file_size : dir.raw_size;
  m_max_block = dir.max_block;
  if (!m_bl->empty() && m_bufs->cache[0].size() < m_max_block) {
    m_bufs->cache[0].resize(m_max_block);
  }
}

Reader::Dir Reader::dir() const {
  Dir d;
  d.header = m_header;
  d.blocks = *m_bl;
  d.raw_size = m_raw_size;
  d.max_block = m_max_block;
  return d;
}

Reader::~Reader() {
  if (m_f) {
    fclose(m_f);
  }
}

bool Reader::file_read(long offset, void* dst, size_t bytes) {
  if (!m_f || m_error || offset < 0 || offset + (long)bytes > m_file_size) {
    m_error = true;
    return false;
  }
  if (bytes == 0) {
    return true;
  }
  Timer t;  // (AI-assisted) read_ms
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
  read_ms += t.getMs();
  file_reads++;
  return true;
}

const uint8_t* Reader::in_window(long offset, size_t bytes) {
  for (auto& w : m_windows) {
    if (w.size && offset >= w.offset && offset + (long)bytes <= w.offset + (long)w.size) {
      w.used = ++m_window_uses;
      return w.buf->data() + (offset - w.offset);
    }
  }
  return nullptr;
}

const uint8_t* Reader::load_window(long offset, size_t want) {
  Window* w = &m_windows[0];
  if (m_window_cap) {
    // the window this read continues (the nearest one before it: the same stream of reads, now
    // past its end), else the one used longest ago
    Window* behind = nullptr;
    for (auto& c : m_windows) {
      if (c.size && c.offset <= offset && (!behind || c.offset > behind->offset)) {
        behind = &c;
      }
    }
    w = behind ? behind : (m_windows[1].used < m_windows[0].used ? &m_windows[1] : &m_windows[0]);
  }
  if (w->buf->size() < want) {
    w->buf->resize(want);
  }
  w->size = 0;
  if (!file_read(offset, w->buf->data(), want)) {
    return nullptr;
  }
  w->offset = offset;
  w->size = want;
  w->used = ++m_window_uses;
  return w->buf->data();
}

bool Reader::raw_read(long offset, void* dst, size_t bytes) {
  if (!m_window_cap) {
    return file_read(offset, dst, bytes);
  }
  if (const uint8_t* p = in_window(offset, bytes)) {
    memcpy(dst, p, bytes);
    return true;
  }
  if (bytes >= m_window_cap) {
    return file_read(offset, dst, bytes);  // one exact read
  }
  if (offset < 0 || offset + (long)bytes > m_file_size) {
    m_error = true;
    return false;
  }
  const uint8_t* p =
      load_window(offset, std::min<size_t>(m_window_cap, (size_t)(m_file_size - offset)));
  if (!p) {
    return false;
  }
  memcpy(dst, p, bytes);
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
  const Block& b = (*m_bl)[bi];
  const long start = (long)b.file_offset;
  const size_t size = b.packed_size();
  if (const uint8_t* p = in_window(start, size)) {
    return p;
  }
  // a new window from this block on: as many of the following blocks as fit
  size_t want = std::max<size_t>(size, m_window_cap ? m_window_cap : kWindow);
  want = std::min<size_t>(want, (size_t)(m_file_size - start));
  if (m_window_cap) {
    // (AI-assisted) whole blocks only: a block cut at the window's end was read again with the next
    size_t whole = size;
    for (size_t j = bi + 1; j < (*m_bl).size(); j++) {
      const Block& n = (*m_bl)[j];
      if ((long)n.file_offset != start + (long)whole || whole + n.packed_size() > want) {
        break;
      }
      whole += n.packed_size();
    }
    want = whole;
  }
  return load_window(start, want);
}

bool Reader::read_block(size_t bi, uint32_t offset, uint8_t* dst, size_t bytes) {
  const Block& b = (*m_bl)[bi];
  const bool whole = offset == b.raw_offset && bytes == b.raw_size;
  if (b.codec() == CODEC_STORED) {
    // straight from the file (or the window, if it's there already)
    const long start = (long)b.file_offset + (long)(offset - b.raw_offset);
    if (const uint8_t* p = in_window(start, bytes)) {
      memcpy(dst, p, bytes);
      return true;
    }
    return raw_read(start, dst, bytes);  // (AI-assisted) window mode: through the windows
  }
  std::vector<uint8_t>& cache0 = m_bufs->cache[0];
  std::vector<uint8_t>& cache1 = m_bufs->cache[1];
  if (!whole && m_cache_block == (long)bi) {
    memcpy(dst, cache0.data() + (offset - b.raw_offset), bytes);
    m_cache2_newer = false;
    return true;
  }
  if (!whole && m_window_cap && m_cache2_block == (long)bi) {
    memcpy(dst, cache1.data() + (offset - b.raw_offset), bytes);
    m_cache2_newer = true;
    return true;
  }
  const uint8_t* src = packed_bytes(bi);
  if (!src) {
    return false;
  }
  // (AI-assisted) window mode: into the cache used longest ago
  const bool second = !whole && m_window_cap && !m_cache2_newer;
  if (second && cache1.size() < m_max_block) {
    cache1.resize(m_max_block);
  }
  std::vector<uint8_t>& cache = second ? cache1 : cache0;
  long& cache_block = second ? m_cache2_block : m_cache_block;
  uint8_t* out = whole ? dst : cache.data();
  Timer t;
  const bool ok = decode_block(b.codec(), src, b.packed_size(), out, b.raw_size, &m_bufs->zstd);
  decode_ms += t.getMs();
  if (!ok) {
    m_error = true;
    m_cache_block = -1;
    m_cache2_block = -1;
    return false;
  }
  if (!whole) {
    cache_block = (long)bi;
    m_cache2_newer = second;
    memcpy(dst, cache.data() + (offset - b.raw_offset), bytes);
  }
  return true;
}

bool Reader::preload(uint32_t offset, uint32_t bytes) {
  if (!ok() || !m_window_cap || !bytes || (uint64_t)offset + bytes > m_raw_size) {
    return false;
  }
  long start, end;
  if ((*m_bl).empty()) {
    start = (long)offset;
    end = (long)offset + (long)bytes;
  } else {
    if (offset < sizeof(Header)) {
      return false;
    }
    const auto first = std::upper_bound((*m_bl).begin(), (*m_bl).end(), offset,
                                        [](uint32_t o, const Block& b) { return o < b.raw_offset; }) -
                       1;
    auto last = std::upper_bound((*m_bl).begin(), (*m_bl).end(), offset + bytes - 1,
                                 [](uint32_t o, const Block& b) { return o < b.raw_offset; }) -
                1;
    start = (long)first->file_offset;
    end = (long)last->file_offset + (long)last->packed_size();
    for (auto b = first; b != last; ++b) {
      if ((long)(b + 1)->file_offset != (long)b->file_offset + (long)b->packed_size()) {
        return false;  // (not one after the other in the file)
      }
    }
  }
  if (end - start > (long)m_window_cap || end > m_file_size) {
    return false;
  }
  if (in_window(start, (size_t)(end - start))) {
    return true;
  }
  return load_window(start, (size_t)(end - start)) != nullptr;
}

bool Reader::read(uint32_t offset, void* dst_v, size_t bytes) {
  if (!ok() || (uint64_t)offset + bytes > m_raw_size) {
    m_error = true;
    return false;
  }
  if (bytes == 0) {
    return true;
  }
  if ((*m_bl).empty()) {
    return raw_read((long)offset, dst_v, bytes);
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
  size_t bi = std::upper_bound((*m_bl).begin(), (*m_bl).end(), offset,
                               [](uint32_t o, const Block& b) { return o < b.raw_offset; }) -
              (*m_bl).begin() - 1;
  while (offset < end) {
    const Block& b = (*m_bl)[bi];
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
