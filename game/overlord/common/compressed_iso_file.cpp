// (AI-assisted) Block-compressed game files (.ogz): see compressed_iso_file.h
#include "compressed_iso_file.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>

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

Reader::~Reader() {
  if (m_fp) {
    fclose(m_fp);
  }
  free(m_cbuf);
  free(m_cache);
  if (m_dctx) {
    ZSTD_freeDCtx((ZSTD_DCtx*)m_dctx);
  }
}

bool Reader::open(FILE* fp) {
  m_fp = fp;
  // one fread per block, straight from the card (newlib's 1 KB buffer would split each one)
  setvbuf(m_fp, nullptr, _IONBF, 0);
  m_pos = ~0u;
  if (!read_at(0, &m_hdr, sizeof(m_hdr)) || memcmp(m_hdr.magic, kMagic, 4) != 0 ||
      m_hdr.version != kVersion || (m_hdr.codec != CODEC_LZ4 && m_hdr.codec != CODEC_ZSTD) ||
      m_hdr.block_size == 0 || m_hdr.block_size > (1u << 20) ||
      m_hdr.block_count != (uint32_t)(((uint64_t)m_hdr.size + m_hdr.block_size - 1) /
                                      m_hdr.block_size)) {
    return false;
  }
  m_offsets.resize(m_hdr.block_count + 1);
  if (!read_at(kHeaderSize, m_offsets.data(), (m_hdr.block_count + 1) * 4)) {
    return false;
  }
  for (uint32_t i = 0; i < m_hdr.block_count; i++) {
    const uint32_t n = m_offsets[i + 1] - m_offsets[i];
    if (m_offsets[i + 1] < m_offsets[i] || n > m_hdr.block_size) {
      return false;
    }
  }
  m_cbuf = (uint8_t*)malloc(m_hdr.block_size);
  return m_cbuf != nullptr;
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
  return ok;
}

// decode a whole block into dst (room for its full length)
bool Reader::load_block(uint32_t block, uint8_t* dst) {
  const uint32_t start = block * m_hdr.block_size;
  const uint32_t out_len = std::min(m_hdr.block_size, m_hdr.size - start);
  const uint32_t in_len = m_offsets[block + 1] - m_offsets[block];
  if (in_len == out_len) {
    // stored
    return read_at(m_offsets[block], dst, out_len);
  }
  if (!read_at(m_offsets[block], m_cbuf, in_len)) {
    return false;
  }
  const uint64_t t0 = now_us();
  bool ok;
  if (m_hdr.codec == CODEC_LZ4) {
    ok = LZ4_decompress_safe((const char*)m_cbuf, (char*)dst, (int)in_len, (int)out_len) ==
         (int)out_len;
  } else {
    if (!m_dctx) {
      m_dctx = ZSTD_createDCtx();
    }
    ok = m_dctx && ZSTD_decompressDCtx((ZSTD_DCtx*)m_dctx, dst, out_len, m_cbuf, in_len) == out_len;
  }
  decode_us += now_us() - t0;
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
