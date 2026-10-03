/*!
 * @file c3l_pack.cpp
 * (AI-assisted)
 * See c3l_pack.h.
 */

#include "c3l_pack.h"

#include <algorithm>
#include <cstring>

#include "common/log/log.h"

#include "game/graphics/ctr/c3l_format.h"
#include "game/graphics/ctr/c3l_reader.h"
#include "third-party/BS_thread_pool.hpp"
#include "third-party/lz4/lz4.h"
#include "third-party/lz4/lz4hc.h"
#include "third-party/zstd/lib/zstd.h"

namespace c3l_pack {

namespace {

struct PackedBlock {
  uint32_t raw_offset = 0, raw_size = 0;
  int section = -1;
  std::vector<uint8_t> lz4, zstd;  // empty: not smaller than raw
  uint32_t codec = c3l::CODEC_STORED;
};

double load_ms(uint32_t codec, size_t raw, size_t packed, const Options& o) {
  // MB/s = bytes/us / 1.048576 ~ bytes per ms * 1e-3
  const double sd = o.sd_mbps * 1048.576, lz4 = o.lz4_mbps * 1048.576,
               zstd = o.zstd_mbps * 1048.576;
  switch (codec) {
    case c3l::CODEC_LZ4:
      return packed / sd + raw / lz4;
    case c3l::CODEC_ZSTD:
      return packed / sd + raw / zstd;
    default:
      return raw / sd;
  }
}

}  // namespace

std::vector<uint8_t> pack(const std::vector<uint8_t>& raw,
                          const std::vector<Section>& sections,
                          std::vector<uint32_t> cuts,
                          const Options& opt,
                          std::vector<SectionStats>* stats) {
  const uint32_t start = sizeof(c3l::Header);
  const uint32_t raw_size = (uint32_t)raw.size();
  // hard cuts: section starts (a block never spans two sections)
  std::vector<uint32_t> hard;
  for (const auto& s : sections) {
    hard.push_back(s.start);
    hard.push_back(s.end);
  }
  cuts.insert(cuts.end(), hard.begin(), hard.end());
  cuts.push_back(start);
  cuts.push_back(raw_size);
  std::sort(cuts.begin(), cuts.end());
  cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
  std::sort(hard.begin(), hard.end());
  auto is_hard = [&](uint32_t o) { return std::binary_search(hard.begin(), hard.end(), o); };
  auto section_of = [&](uint32_t o) {
    for (size_t i = 0; i < sections.size(); i++) {
      if (o >= sections[i].start && o < sections[i].end) {
        return (int)i;
      }
    }
    return -1;
  };

  // ---- blocks ----
  std::vector<PackedBlock> blocks;
  uint32_t cur = start;  // start of the open block
  for (size_t i = 0; i + 1 < cuts.size(); i++) {
    const uint32_t a = cuts[i], b = cuts[i + 1];
    if (a < start || a >= raw_size) {
      continue;
    }
    // close the open block before this piece?
    if (a > cur && (is_hard(a) || a - cur >= opt.min_block || b - cur > opt.max_block)) {
      blocks.push_back({cur, a - cur, section_of(cur)});
      cur = a;
    }
    // a piece bigger than a block: whole blocks of it
    while (b - cur > opt.max_block) {
      blocks.push_back({cur, opt.max_block, section_of(cur)});
      cur += opt.max_block;
    }
  }
  if (cur < raw_size) {
    blocks.push_back({cur, raw_size - cur, section_of(cur)});
  }

  // ---- compress (both codecs, in parallel) ----
  {
    BS::thread_pool pool(opt.threads > 0 ? opt.threads : std::thread::hardware_concurrency());
    pool.push_loop(blocks.size(), [&](size_t lo, size_t hi) {
      for (size_t i = lo; i < hi; i++) {
        auto& b = blocks[i];
        const char* src = (const char*)raw.data() + b.raw_offset;
        if (opt.mode == Mode::LZ4 || opt.mode == Mode::AUTO) {
          std::vector<uint8_t> out(LZ4_compressBound((int)b.raw_size));
          const int n = LZ4_compress_HC(src, (char*)out.data(), (int)b.raw_size, (int)out.size(),
                                        opt.lz4_level);
          if (n > 0 && (uint32_t)n < b.raw_size) {
            out.resize(n);
            b.lz4 = std::move(out);
          }
        }
        if (opt.mode == Mode::ZSTD || opt.mode == Mode::AUTO) {
          std::vector<uint8_t> out(ZSTD_compressBound(b.raw_size));
          const size_t n = ZSTD_compress(out.data(), out.size(), src, b.raw_size, opt.zstd_level);
          if (!ZSTD_isError(n) && n < b.raw_size) {
            out.resize(n);
            b.zstd = std::move(out);
          }
        }
        // the fastest to load
        double best = load_ms(c3l::CODEC_STORED, b.raw_size, b.raw_size, opt);
        b.codec = c3l::CODEC_STORED;
        if (!b.lz4.empty()) {
          const double t = load_ms(c3l::CODEC_LZ4, b.raw_size, b.lz4.size(), opt);
          if (t < best) {
            best = t;
            b.codec = c3l::CODEC_LZ4;
          }
        }
        if (!b.zstd.empty()) {
          const double t = load_ms(c3l::CODEC_ZSTD, b.raw_size, b.zstd.size(), opt);
          if (t < best) {
            best = t;
            b.codec = c3l::CODEC_ZSTD;
          }
        }
      }
    });
    pool.wait_for_tasks();
  }

  // ---- write ----
  c3l::BlockDir dir{};
  memcpy(dir.magic, c3l::kBlockMagic, 4);
  dir.raw_size = raw_size;
  dir.num_blocks = (uint32_t)blocks.size();
  std::vector<c3l::Block> table(blocks.size());
  uint32_t off = sizeof(c3l::Header) + sizeof(c3l::BlockDir) + sizeof(c3l::Block) * blocks.size();
  std::vector<uint8_t> out(raw.begin(), raw.begin() + start);
  out.resize(off);
  for (size_t i = 0; i < blocks.size(); i++) {
    const auto& b = blocks[i];
    dir.max_block = std::max(dir.max_block, b.raw_size);
    const uint8_t* data = b.codec == c3l::CODEC_LZ4    ? b.lz4.data()
                          : b.codec == c3l::CODEC_ZSTD ? b.zstd.data()
                                                       : raw.data() + b.raw_offset;
    const uint32_t size = b.codec == c3l::CODEC_LZ4    ? (uint32_t)b.lz4.size()
                          : b.codec == c3l::CODEC_ZSTD ? (uint32_t)b.zstd.size()
                                                       : b.raw_size;
    table[i] = {b.raw_offset, b.raw_size, (uint32_t)out.size(), size | (b.codec << 28)};
    out.insert(out.end(), data, data + size);
  }
  memcpy(out.data() + start, &dir, sizeof(dir));
  memcpy(out.data() + start + sizeof(dir), table.data(), table.size() * sizeof(c3l::Block));

  if (stats) {
    stats->clear();
    for (const auto& s : sections) {
      stats->push_back({s.name});
    }
    stats->push_back({"other"});
    for (size_t i = 0; i < blocks.size(); i++) {
      const auto& b = blocks[i];
      auto& st = (*stats)[b.section >= 0 ? b.section : (int)sections.size()];
      st.raw += b.raw_size;
      st.lz4 += b.lz4.empty() ? b.raw_size : b.lz4.size();
      st.zstd += b.zstd.empty() ? b.raw_size : b.zstd.size();
      st.packed += table[i].packed_size();
      st.blocks[b.codec]++;
      st.load_ms += load_ms(b.codec, b.raw_size, table[i].packed_size(), opt);
    }
  }
  return out;
}

bool verify(const std::string& path,
            const std::vector<uint8_t>& raw,
            const std::vector<uint32_t>& cuts_in) {
  // whole
  {
    c3l::Reader r(path.c_str());
    std::vector<uint8_t> back(raw.size());
    if (!r.ok() || r.raw_size() != raw.size() || !r.read(0, back.data(), back.size()) ||
        back != raw) {
      lg::error("{}: read back (whole) differs from what was written", path);
      return false;
    }
  }
  // piece by piece, backwards (every block decoded again, partial blocks through the cache)
  std::vector<uint32_t> cuts = cuts_in;
  cuts.push_back(0);
  cuts.push_back((uint32_t)raw.size());
  std::sort(cuts.begin(), cuts.end());
  cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
  c3l::Reader r(path.c_str());
  std::vector<uint8_t> piece;
  for (size_t i = cuts.size() - 1; i-- > 0;) {
    const uint32_t a = cuts[i], b = std::min<uint32_t>(cuts[i + 1], (uint32_t)raw.size());
    if (a >= b) {
      continue;
    }
    piece.resize(b - a);
    if (!r.read(a, piece.data(), piece.size()) ||
        memcmp(piece.data(), raw.data() + a, piece.size())) {
      lg::error("{}: read back of bytes {}..{} differs from what was written", path, a, b);
      return false;
    }
  }
  return true;
}

}  // namespace c3l_pack
