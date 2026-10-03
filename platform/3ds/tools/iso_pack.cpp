// (AI-assisted) Host tool: compress a game file of out/jak1/iso into the 3DS's block-compressed
// .ogz format (game/overlord/common/compressed_iso_file.h), and check that the game's decoder
// gives back the same bytes. Built and run by stage_sd.sh --compress.
//
//   iso_pack [--codec lz4|zstd|zstd-adpcm] [--level N] [--block-size N] IN OUT.ogz
//       compress IN, then decode OUT.ogz with the game's reader (whole pages, sector-sized and
//       odd reads) and compare with IN. Exit code 1 on any difference. zstd-adpcm is for the
//       PS2 ADPCM voice streams (VAGWAD.*): headers and nibbles of the 16-byte frames apart.
//   iso_pack --verify IN OUT.ogz       only the check
//   iso_pack --dump OUT.ogz DECODED    decode a whole .ogz to a file
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "game/overlord/common/compressed_iso_file.h"
#include "third-party/lz4/lz4hc.h"
#include "third-party/zstd/lib/zstd.h"

using namespace iso_cz;

static bool read_file(const char* path, std::vector<uint8_t>& out) {
  FILE* fp = fopen(path, "rb");
  if (!fp) {
    return false;
  }
  fseek(fp, 0, SEEK_END);
  out.resize(ftell(fp));
  fseek(fp, 0, SEEK_SET);
  const bool ok = out.empty() || fread(out.data(), out.size(), 1, fp) == 1;
  fclose(fp);
  return ok;
}

static bool compress(const std::vector<uint8_t>& in, const char* out_path, Codec codec, int level,
                     uint32_t bs) {
  Header h{};
  memcpy(h.magic, kMagic, 4);
  h.version = kVersion;
  h.codec = codec;
  h.block_size = bs;
  h.size = (uint32_t)in.size();
  h.block_count = (uint32_t)((in.size() + bs - 1) / bs);
  h.crc32 = crc32_update(0, in.data(), in.size());
  std::vector<uint32_t> offsets(h.block_count + 1);
  std::vector<uint8_t> blocks;
  const size_t cap = std::max<size_t>(LZ4_compressBound(bs), 2 * ZSTD_compressBound(bs) + 4);
  std::vector<uint8_t> tmp(cap);
  ZSTD_CCtx* cctx = codec != CODEC_LZ4 ? ZSTD_createCCtx() : nullptr;
  uint32_t pos = kHeaderSize + (h.block_count + 1) * 4;
  for (uint32_t b = 0; b < h.block_count; b++) {
    const uint8_t* src = in.data() + (size_t)b * bs;
    const uint32_t n = std::min<uint32_t>(bs, h.size - b * bs);
    size_t c;
    if (codec == CODEC_LZ4) {
      c = LZ4_compress_HC((const char*)src, (char*)tmp.data(), (int)n, (int)cap, level);
    } else if (codec == CODEC_ZSTD_ADPCM) {
      // u32 header stream size, zstd(headers), zstd(nibbles)
      const uint32_t hn = 2 * (n / 16);
      std::vector<uint8_t> split(n);
      adpcm_split(src, n, split.data(), split.data() + hn);
      const size_t h = ZSTD_compressCCtx(cctx, tmp.data() + 4, cap - 4, split.data(), hn, level);
      size_t r = ZSTD_isError(h) ? h
                                 : ZSTD_compressCCtx(cctx, tmp.data() + 4 + h, cap - 4 - h,
                                                     split.data() + hn, n - hn, level);
      if (ZSTD_isError(h) || ZSTD_isError(r)) {
        c = 0;
      } else {
        const uint32_t h32 = (uint32_t)h;
        memcpy(tmp.data(), &h32, 4);
        c = 4 + h + r;
      }
    } else {
      c = ZSTD_compressCCtx(cctx, tmp.data(), cap, src, n, level);
      if (ZSTD_isError(c)) {
        c = 0;
      }
    }
    offsets[b] = pos;
    if (c == 0 || c >= n) {
      blocks.insert(blocks.end(), src, src + n);  // stored
      pos += n;
    } else {
      blocks.insert(blocks.end(), tmp.data(), tmp.data() + c);
      pos += (uint32_t)c;
    }
  }
  offsets[h.block_count] = pos;
  if (cctx) {
    ZSTD_freeCCtx(cctx);
  }
  FILE* fp = fopen(out_path, "wb");
  if (!fp) {
    return false;
  }
  bool ok = fwrite(&h, sizeof(h), 1, fp) == 1 &&
            fwrite(offsets.data(), offsets.size() * 4, 1, fp) == 1 &&
            (blocks.empty() || fwrite(blocks.data(), blocks.size(), 1, fp) == 1);
  return fclose(fp) == 0 && ok;
}

// the game's reader against the original bytes, with the read patterns the fake ISO uses
static bool verify(const std::vector<uint8_t>& in, const char* ogz_path) {
  auto open_reader = [&]() -> std::unique_ptr<Reader> {
    FILE* fp = fopen(ogz_path, "rb");
    auto r = std::make_unique<Reader>();
    if (!fp || !r->open(fp)) {
      fprintf(stderr, "%s: not a valid .ogz\n", ogz_path);
      return nullptr;
    }
    return r;
  };
  auto r = open_reader();
  if (!r) {
    return false;
  }
  if (r->size() != in.size() || r->expected_crc32() != crc32_update(0, in.data(), in.size())) {
    fprintf(stderr, "%s: size / crc in the header don't match\n", ogz_path);
    return false;
  }
  const uint32_t size = r->size();
  std::vector<uint8_t> buf(0x10000 + 64);
  auto check = [&](Reader& rd, uint32_t off, uint32_t len) {
    const uint32_t want = off >= size ? 0 : std::min(len, size - off);
    memset(buf.data(), 0xcd, buf.size());
    const int64_t got = rd.read(off, buf.data(), len);
    if (got != (int64_t)want || memcmp(buf.data(), in.data() + std::min(off, size), want) != 0 ||
        (want < buf.size() && buf[want] != 0xcd)) {
      fprintf(stderr, "%s: read of %u bytes at %u differs\n", ogz_path, len, off);
      return false;
    }
    return true;
  };
  // 1. whole file in 48 KB pages (DGO / CGO loading), then 24 KB pages (half blocks)
  for (uint32_t page : {0xc000u, 0x6000u}) {
    auto rd = open_reader();
    for (uint32_t off = 0; off < size + page; off += page) {
      if (!check(*rd, off, page)) {
        return false;
      }
    }
  }
  // 2. sector-aligned and odd reads at random places, both directions
  std::mt19937 rng(size);
  for (int i = 0; i < 300 && size; i++) {
    uint32_t off = rng() % size;
    if (i & 1) {
      off &= ~0x7ffu;
    }
    const uint32_t len = 1 + rng() % 0x10000;
    if (!check(*r, off, len)) {
      return false;
    }
  }
  return true;
}

int main(int argc, char** argv) {
  Codec codec = CODEC_LZ4;
  int level = -1;
  uint32_t bs = 0xc000;
  bool verify_only = false, dump = false;
  std::vector<const char*> pos;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--codec" && i + 1 < argc) {
      std::string c = argv[++i];
      codec = c == "zstd" ? CODEC_ZSTD : c == "zstd-adpcm" ? CODEC_ZSTD_ADPCM : CODEC_LZ4;
    } else if (a == "--level" && i + 1 < argc) {
      level = atoi(argv[++i]);
    } else if (a == "--block-size" && i + 1 < argc) {
      bs = (uint32_t)strtoul(argv[++i], nullptr, 0);
    } else if (a == "--verify") {
      verify_only = true;
    } else if (a == "--dump") {
      dump = true;
    } else {
      pos.push_back(argv[i]);
    }
  }
  if (pos.size() != 2) {
    fprintf(stderr, "usage: iso_pack [--codec lz4|zstd|zstd-adpcm] [--level N] [--block-size N] IN OUT.ogz\n");
    return 2;
  }
  if (dump) {
    FILE* fp = fopen(pos[0], "rb");
    Reader r;
    if (!fp || !r.open(fp)) {
      return 1;
    }
    std::vector<uint8_t> out(r.size());
    if (r.read(0, out.data(), r.size()) != r.size()) {
      return 1;
    }
    FILE* o = fopen(pos[1], "wb");
    return o && (out.empty() || fwrite(out.data(), out.size(), 1, o) == 1) && fclose(o) == 0 ? 0
                                                                                            : 1;
  }
  if (level < 0) {
    level = codec == CODEC_LZ4 ? LZ4HC_CLEVEL_MAX : 19;
  }
  std::vector<uint8_t> in;
  if (!read_file(pos[0], in)) {
    fprintf(stderr, "can't read %s\n", pos[0]);
    return 1;
  }
  if (!verify_only && !compress(in, pos[1], codec, level, bs)) {
    fprintf(stderr, "can't write %s\n", pos[1]);
    return 1;
  }
  if (!verify(in, pos[1])) {
    return 1;
  }
  return 0;
}
