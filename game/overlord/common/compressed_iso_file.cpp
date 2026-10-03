// (AI-assisted) Block-compressed game files (.ogz): see compressed_iso_file.h
#include "compressed_iso_file.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "third-party/lz4/lz4.h"
#include "third-party/zstd/lib/zstd.h"

namespace iso_cz {

namespace {
struct CrcTable {
  uint32_t t[256];
  constexpr CrcTable() : t() {
    for (uint32_t i = 0; i < 256; i++) {
      uint32_t c = i;
      for (int k = 0; k < 8; k++) {
        c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : (c >> 1);
      }
      t[i] = c;
    }
  }
};
constexpr CrcTable kCrcTable{};

// one zstd decoder for every file (~95 KB, kept: no big malloc / free per voice line)
std::mutex g_zstd_mutex;
ZSTD_DCtx* g_dctx = nullptr;

bool zstd_decode(void* dst, uint32_t dst_len, const void* src, uint32_t src_len) {
  std::lock_guard<std::mutex> lock(g_zstd_mutex);
  if (!g_dctx) {
    g_dctx = ZSTD_createDCtx();
    if (!g_dctx) {
      return false;
    }
  }
  return ZSTD_decompressDCtx(g_dctx, dst, dst_len, src, src_len) == dst_len;
}

// block indexes by path
std::mutex g_index_mutex;
std::map<std::string, std::shared_ptr<const std::vector<uint32_t>>> g_index_cache;

uint64_t now_us() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}
}  // namespace

uint32_t crc32_update(uint32_t crc, const void* data, size_t size) {
  const uint8_t* p = (const uint8_t*)data;
  crc = ~crc;
  while (size--) {
    crc = kCrcTable.t[(crc ^ *p++) & 0xff] ^ (crc >> 8);
  }
  return ~crc;
}

void adpcm_split(const uint8_t* data, uint32_t n, uint8_t* headers, uint8_t* rest) {
  const uint32_t frames = n / 16;
  for (uint32_t i = 0; i < frames; i++) {
    headers[2 * i] = data[16 * i];
    headers[2 * i + 1] = data[16 * i + 1];
    memcpy(rest + 14 * i, data + 16 * i + 2, 14);
  }
  memcpy(rest + 14 * frames, data + 16 * frames, n - 16 * frames);
}

void adpcm_join_in_place(uint8_t* data, uint32_t n, const uint8_t* headers) {
  // frame i's nibbles move down from 2 * frames + 14 * i to 16 * i + 2: never past a later
  // frame's nibbles, which start at or after 16 * (i + 1); the tail is already in place
  const uint32_t frames = n / 16;
  const uint8_t* rest = data + 2 * frames;
  for (uint32_t i = 0; i < frames; i++) {
    memmove(data + 16 * i + 2, rest + 14 * i, 14);
    data[16 * i] = headers[2 * i];
    data[16 * i + 1] = headers[2 * i + 1];
  }
}

Reader::~Reader() {
  if (m_fp) {
    fclose(m_fp);
  }
  free(m_cbuf);
  free(m_cache);
  free(m_hbuf);
  free(m_ra);
}

void Reader::set_read_ahead(uint32_t bytes) {
  // a window holds at least one whole block
  const uint32_t cap = bytes < m_hdr.block_size ? 0 : bytes;
  if (cap != m_ra_cap) {
    free(m_ra);
    m_ra = nullptr;
    m_ra_len = 0;
    m_ra_cap = cap;
  }
}

void Reader::release_buffers(bool keep_block) {
  free(m_cbuf);
  m_cbuf = nullptr;
  free(m_hbuf);
  m_hbuf = nullptr;
  free(m_ra);
  m_ra = nullptr;
  m_ra_len = 0;
  m_ra_cap = 0;
  m_read_end = 0;
  if (!keep_block) {
    free(m_cache);
    m_cache = nullptr;
    m_cache_block = -1;
  }
}

void Reader::reset_stats() {
  sd_bytes = 0;
  out_bytes = 0;
  sd_us = 0;
  decode_us = 0;
  blocks = 0;
  max_decode_us = 0;
  sd_reads = 0;
}

bool Reader::open(FILE* fp, const char* path) {
  m_fp = fp;
  // one fread per block, straight from the card (newlib's 1 KB buffer would split each one)
  setvbuf(m_fp, nullptr, _IONBF, 0);
  m_pos = ~0u;
  if (!read_at(0, &m_hdr, sizeof(m_hdr)) || memcmp(m_hdr.magic, kMagic, 4) != 0 ||
      m_hdr.version != kVersion ||
      (m_hdr.codec != CODEC_LZ4 && m_hdr.codec != CODEC_ZSTD && m_hdr.codec != CODEC_ZSTD_ADPCM) ||
      m_hdr.block_size == 0 || m_hdr.block_size > (1u << 20) ||
      m_hdr.block_count != (uint32_t)(((uint64_t)m_hdr.size + m_hdr.block_size - 1) /
                                      m_hdr.block_size)) {
    return false;
  }
  const std::string key = path ? std::string(path) : std::string();
  if (path) {
    std::lock_guard<std::mutex> lock(g_index_mutex);
    auto it = g_index_cache.find(key);
    if (it != g_index_cache.end() && it->second->size() == m_hdr.block_count + 1) {
      m_index = it->second;
    }
  }
  if (!m_index) {
    auto index = std::make_shared<std::vector<uint32_t>>(m_hdr.block_count + 1);
    if (!read_at(kHeaderSize, index->data(), (m_hdr.block_count + 1) * 4)) {
      return false;
    }
    for (uint32_t i = 0; i < m_hdr.block_count; i++) {
      const uint32_t n = (*index)[i + 1] - (*index)[i];
      if ((*index)[i + 1] < (*index)[i] || n > m_hdr.block_size) {
        return false;
      }
    }
    m_index = index;
    if (path) {
      std::lock_guard<std::mutex> lock(g_index_mutex);
      g_index_cache[key] = m_index;
    }
  }
  m_offsets = m_index->data();
  // the block buffers are allocated when a read needs them (release_buffers)
  return true;
}

bool Reader::read_at(uint32_t pos, void* dst, uint32_t len) {
  const uint64_t t0 = now_us();
  if (pos != m_pos && fseek(m_fp, pos, SEEK_SET) != 0) {
    m_pos = ~0u;
    return false;
  }
  const bool ok = len == 0 || fread(dst, len, 1, m_fp) == 1;
  m_pos = ok ? pos + len : ~0u;
  sd_us += now_us() - t0;
  sd_bytes += len;
  sd_reads++;
  return ok;
}

// The compressed bytes of `block` in the read-ahead window, read from the card (from this block
// on) when the window doesn't hold them. *src = nullptr if there is no window (no memory for it).
// False on a read error.
bool Reader::window(uint32_t block, const uint8_t** src) {
  *src = nullptr;
  const uint32_t pos = m_offsets[block];
  const uint32_t end = m_offsets[block + 1];
  if (m_ra && pos >= m_ra_pos && end <= m_ra_pos + m_ra_len) {
    *src = m_ra + (pos - m_ra_pos);
    return true;
  }
  if (!m_ra) {
    m_ra = (uint8_t*)malloc(m_ra_cap);
    if (!m_ra) {
      m_ra_cap = 0;  // one block per read from now on
      return true;
    }
  }
  // whole blocks from this one on, as many as fit (at least this one: the window holds a block)
  uint32_t last = m_hdr.block_count;
  if (m_read_end) {
    last = std::min(last, (m_read_end - 1) / m_hdr.block_size + 1);
  }
  uint32_t next = block + 1;
  while (next < last && m_offsets[next + 1] - pos <= m_ra_cap) {
    next++;
  }
  m_ra_len = 0;
  if (!read_at(pos, m_ra, m_offsets[next] - pos)) {
    return false;
  }
  m_ra_pos = pos;
  m_ra_len = m_offsets[next] - pos;
  *src = m_ra;
  return true;
}

// decode a whole block into dst (room for its full length)
bool Reader::load_block(uint32_t block, uint8_t* dst) {
  const uint32_t start = block * m_hdr.block_size;
  const uint32_t out_len = std::min(m_hdr.block_size, m_hdr.size - start);
  const uint32_t in_len = m_offsets[block + 1] - m_offsets[block];
  const uint8_t* src = nullptr;
  if (m_ra_cap && !window(block, &src)) {
    return false;
  }
  if (in_len == out_len) {
    // stored
    if (src) {
      memcpy(dst, src, out_len);
      return true;
    }
    return read_at(m_offsets[block], dst, out_len);
  }
  if (!src) {
    if (!m_cbuf && !(m_cbuf = (uint8_t*)malloc(m_hdr.block_size))) {
      return false;
    }
    if (!read_at(m_offsets[block], m_cbuf, in_len)) {
      return false;
    }
    src = m_cbuf;
  }
  if (m_hdr.codec == CODEC_ZSTD_ADPCM && !m_hbuf &&
      !(m_hbuf = (uint8_t*)malloc(m_hdr.block_size / 8 + 2))) {
    return false;
  }
  const uint64_t t0 = now_us();
  bool ok;
  if (m_hdr.codec == CODEC_LZ4) {
    ok = LZ4_decompress_safe((const char*)src, (char*)dst, (int)in_len, (int)out_len) ==
         (int)out_len;
  } else if (m_hdr.codec == CODEC_ZSTD) {
    ok = zstd_decode(dst, out_len, src, in_len);
  } else {
    // ADPCM split: headers to m_hbuf, nibbles to the end of dst, then interleaved in place
    const uint32_t frames = out_len / 16;
    uint32_t hlen;
    memcpy(&hlen, src, 4);
    ok = in_len >= 4 && hlen <= in_len - 4 &&
         zstd_decode(m_hbuf, 2 * frames, src + 4, hlen) &&
         zstd_decode(dst + 2 * frames, out_len - 2 * frames, src + 4 + hlen, in_len - 4 - hlen);
    if (ok) {
      adpcm_join_in_place(dst, out_len, m_hbuf);
    }
  }
  const uint64_t dt = now_us() - t0;
  decode_us += dt;
  max_decode_us = std::max(max_decode_us, dt);
  blocks++;
  return ok;
}

int64_t Reader::read(uint32_t offset, void* dst_v, uint32_t len) {
  if (offset >= m_hdr.size) {
    return 0;
  }
  len = std::min(len, m_hdr.size - offset);
  uint8_t* dst = (uint8_t*)dst_v;
  const uint32_t bs = m_hdr.block_size;
  uint32_t done = 0;
  while (done < len) {
    const uint32_t pos = offset + done;
    const uint32_t block = pos / bs;
    const uint32_t in_block = pos - block * bs;
    const uint32_t block_len = std::min(bs, m_hdr.size - block * bs);
    const uint32_t n = std::min(block_len - in_block, len - done);
    if (in_block == 0 && n == block_len) {
      // the whole block: decode in place (every page of a DGO)
      if (!load_block(block, dst + done)) {
        return -1;
      }
    } else {
      if (m_cache_block != (int64_t)block) {
        if (!m_cache) {
          m_cache = (uint8_t*)malloc(bs);
          if (!m_cache) {
            return -1;
          }
        }
        m_cache_block = -1;
        if (!load_block(block, m_cache)) {
          return -1;
        }
        m_cache_block = block;
      }
      memcpy(dst + done, m_cache + in_block, n);
    }
    done += n;
  }
  out_bytes += len;
  return len;
}

}  // namespace iso_cz
