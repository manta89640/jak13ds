/*!
 * @file CtrVram.cpp
 * (AI-assisted)
 * See CtrVram.h.
 */

#include "CtrVram.h"

#include <algorithm>
#include <cstring>
#include <functional>

#include "common/dma/gs.h"
#include "common/goal_constants.h"
#include "common/log/log.h"
#include "common/texture/texture_conversion.h"
#include "common/util/Assert.h"

#include "game/graphics/ctr/ctr_gpu.h"

namespace {
constexpr u32 kVramBytes = 4 * 1024 * 1024;
constexpr u32 kVramMask = kVramBytes - 1;
constexpr u32 kBlockBytes = 256;
constexpr u32 kBlocks = kVramBytes / kBlockBytes;
// (AI-assisted) the game's unit of texture uploads (upload-vram-pages in texture.gc): 4096 words,
// 32 rows of a 128 pixel wide PSMCT32 image, 64 blocks
constexpr u32 kChunkWords = 4096;
constexpr u32 kChunkBlocks = kChunkWords * 4 / kBlockBytes;
constexpr u32 kChunks = kBlocks / kChunkBlocks;

// the blocks a 128 pixel wide PSMCT32 upload of `words` writes: whole chunks (the rows of a partial
// chunk still spread over all of its pages' blocks)
u32 upload_end_block(u32 dest_block, u32 words) {
  return dest_block + (words + kChunkWords - 1) / kChunkWords * kChunkBlocks;
}

// (AI-assisted) the id of a whole chunk of a page upload (odd; see CtrVram.h): where its data comes
// from in EE memory and a sampled hash of it (32 words, one per 512 bytes), taken when it is
// written: a pending upload copies the bytes there at that time, not when it was recorded
u64 page_chunk_id(const u8* chunk_src) {
  u64 h = (u64)(uintptr_t)chunk_src * 0x9E3779B97F4A7C15ull;
  for (u32 k = 0; k < 32; k++) {
    u32 w;
    memcpy(&w, chunk_src + 512 * k + 4 * (k & 7), 4);
    h = (h ^ w) * 1099511628211ull;
  }
  h ^= h >> 29;
  h *= 0xBF58476D1CE4E5B9ull;
  h ^= h >> 32;
  return h | 1;
}

// GOAL texture-page layout (see game/graphics/texture/TexturePool.h, kept GL-free here).
struct GoalTexturePageSeg {
  u32 block_data_ptr;
  u32 size;  // in 32-bit words
  u32 dest;  // in 32-bit words
};
struct GoalTexturePageHeader {
  u32 file_info_ptr;
  u32 name_ptr;
  u32 id;
  s32 length;
  u32 mip0_size;
  u32 size;
  GoalTexturePageSeg segment[3];
  u32 pad[16];
};

u32 pack_rgba(u32 r, u32 g, u32 b, u32 a) {
  return r | (g << 8) | (b << 16) | (a << 24);
}

// 16-bit GS color to our RGBA8. Alpha from the A bit and TEXA: the game's usual TEXA has TA0 = 0,
// TA1 = 0x80 (like rgba16_to_rgba32 on PC), so A = 0 is transparent. With 0x80 for both, particles
// with 16-bit CLUTs were drawn as squares and black quads.
u32 ct16_to_rgba(u32 c) {
  return pack_rgba((c & 0x1f) << 3, ((c >> 5) & 0x1f) << 3, ((c >> 10) & 0x1f) << 3,
                   (c & 0x8000) ? 0x80 : 0);
}

// in-game texture (see GoalTexture in game/graphics/texture/TexturePool.h)
struct GoalTexture {
  s16 w;
  s16 h;
  u8 num_mips;
  u8 tex1_control;
  u8 psm;
  u8 mip_shift;
  u16 clutpsm;
  u16 dest[7];
  u16 clut_dest;
  u8 width[7];
  u32 name_ptr;
  u32 size;
  float uv_dist;
  u32 masks[3];
};
static_assert(sizeof(GoalTexture) == 60);

struct Range {
  u32 lo = UINT32_MAX, hi = 0;
  // addresses past the end of VRAM wrap around on the GS; textures bigger than their data (the
  // font is declared 256x512) read garbage there. Don't let that make the range cover all VRAM.
  void add(u32 a) {
    if (a >= kVramBytes) {
      return;
    }
    lo = std::min(lo, a);
    hi = std::max(hi, a);
  }
};
}  // namespace

CtrVram::CtrVram() {
  static_assert(kChunks == kChunkCount);
  m_vram.resize(kVramBytes);
  m_tex_info.resize(kBlocks);
  // a PSMCT32 page is 64 x 32 pixels (8 KB)
  m_ct32_page_table.resize(64 * 32);
  for (u32 y = 0; y < 32; y++) {
    for (u32 x = 0; x < 64; x++) {
      m_ct32_page_table[x + 64 * y] = psmct32_addr(x, y, 64);
    }
  }
  // (AI-assisted) in PSMCT32 the texels 2k and 2k+1 of a row are next to each other: uploads
  // copy pairs (checked here rather than assumed)
  m_ct32_pairs = true;
  for (u32 i = 0; i < 64 * 32; i += 2) {
    if (m_ct32_page_table[i + 1] != m_ct32_page_table[i] + 4 || (m_ct32_page_table[i] & 7)) {
      m_ct32_pairs = false;
    }
  }
  // (AI-assisted) and the texels x..x+3 (x % 4 = 0) of rows y and y + 1 (y even) are one 32 byte
  // line: pairs of row y and y + 1 alternate. Uploads write whole lines from two rows.
  m_ct32_lines = m_ct32_pairs;
  for (u32 y = 0; y < 32; y += 2) {
    for (u32 x = 0; x < 64; x += 4) {
      const u32* t0 = &m_ct32_page_table[x + 64 * y];
      const u32* t1 = t0 + 64;
      if ((t0[0] & 31) || t1[0] != t0[0] + 8 || t0[2] != t0[0] + 16 || t1[2] != t0[0] + 24) {
        m_ct32_lines = false;
      }
    }
  }
}

CtrVram::~CtrVram() {
  clear_cache();
}

u32 CtrVram::read32(u32 byte_addr) const {
  u32 v;
  memcpy(&v, m_vram.data() + (byte_addr & kVramMask & ~3u), 4);
  return v;
}

void CtrVram::upload_ct32(const u8* data, u32 dest_block, u32 width, u32 height) {
  write_ct32(data, dest_block, width, height);
  // (AI-assisted) whole pages (a page row's blocks are spread over the page)
  const u32 end = dest_block + (height + 31) / 32 * std::max(1u, width / 64) * 32;
  mark_unique(dest_block, end);
  note_write(dest_block, end);
}

void CtrVram::mark_unique(u32 first_block, u32 end_block) {
  if (end_block <= first_block) {
    return;
  }
  const u32 c0 = first_block / kChunkBlocks, c1 = (end_block - 1) / kChunkBlocks;
  for (u32 c = c0; c <= c1 && c < c0 + kChunks; c++) {
    m_chunk_id[c % kChunks] = m_next_unique_id;
    m_next_unique_id += 2;
  }
}

void CtrVram::note_write(u32 first_block, u32 end_block) {
  m_stats.uploads_changed++;
  // a texture relocated there has to be relocated again (relocate() skips unchanged ones)
  for (auto it = m_last_relocate.begin(); it != m_last_relocate.end();) {
    if (it->second.first_block < end_block && first_block < it->second.end_block) {
      it = m_last_relocate.erase(it);
    } else {
      ++it;
    }
  }
}

u64 CtrVram::mix_chunk_ids(u64 h, u32 first_block, u32 end_block) const {
  if (end_block <= first_block) {
    return h;
  }
  const u32 c0 = first_block / kChunkBlocks, c1 = (end_block - 1) / kChunkBlocks;
  for (u32 c = c0; c <= c1 && c < c0 + kChunks; c++) {
    h = (h ^ m_chunk_id[c % kChunks]) * 1099511628211ull;
  }
  return h;
}

void CtrVram::write_ct32(const u8* data, u32 dest_block, u32 width, u32 height) {
  const u32 pages_per_row = std::max(1u, width / 64);
  const u32 base = dest_block * kBlockBytes;
  u32 y = 0;
  const u32 page_rows = (height + 31) / 32;
  // (AI-assisted) asm_tex = off (hardware hang bisect): the per pair loop only, no 64-bit stores
  if (ctr_gpu_asm_tex() && m_ct32_lines && width % 64 == 0 &&
      base + page_rows * pages_per_row * 8192 <= kVramBytes) {
    // (AI-assisted) two rows at a time, one 32 byte VRAM line per 4 texels of each (see the
    // constructor), as 64-bit pairs; the upload doesn't wrap around the end of VRAM
    for (; y + 1 < height; y += 2) {
      u8* row_dst = m_vram.data() + base + (y / 32) * pages_per_row * 8192;
      const u32* ytab = &m_ct32_page_table[64 * (y % 32)];
      const u64* s0 = (const u64*)(data + 4 * y * width);
      const u64* s1 = s0 + width / 2;
      for (u32 page = 0; page < width / 64; page++, row_dst += 8192) {
        for (u32 x = 0; x < 64; x += 4, s0 += 2, s1 += 2) {
          u64* d = (u64*)(row_dst + ytab[x]);
          const u64 a01 = s0[0], a23 = s0[1], b01 = s1[0], b23 = s1[1];
          d[0] = a01;
          d[1] = b01;
          d[2] = a23;
          d[3] = b23;
        }
      }
    }
  }
  for (; y < height; y++) {
    const u32 page_row = (y / 32) * pages_per_row;
    const u32* ytab = &m_ct32_page_table[64 * (y % 32)];
    const u8* src_row = data + 4 * y * width;
    if (m_ct32_pairs && width % 2 == 0) {
      for (u32 x = 0; x < width; x += 2) {
        u32 addr = (base + (page_row + x / 64) * 8192 + ytab[x % 64]) & kVramMask;
        memcpy(m_vram.data() + addr, src_row + 4 * x, 8);
      }
      continue;
    }
    for (u32 x = 0; x < width; x++) {
      u32 addr = (base + (page_row + x / 64) * 8192 + ytab[x % 64]) & kVramMask;
      memcpy(m_vram.data() + addr, src_row + 4 * x, 4);
    }
  }
}

void CtrVram::note_tex_info(const u8* tpage, s32 count, const u8* ee_mem, u32 s7_ptr) {
  for (s32 i = 0; i < count && i < 1024; i++) {
    u32 ptr;
    memcpy(&ptr, tpage + sizeof(GoalTexturePageHeader) + 4 * i, 4);
    if (!ptr || ptr == s7_ptr) {
      continue;
    }
    GoalTexture tex;
    memcpy(&tex, ee_mem + ptr, sizeof(tex));
    if (tex.dest[0] < kBlocks) {
      m_tex_info[tex.dest[0]] = TexInfo{(u16)tex.w, (u16)tex.h, tex.psm, tex.width[0],
                                        tex.clutpsm, tex.clut_dest};
    }
  }
}

void CtrVram::upload_texture_page(const u8* tpage, int mode, const u8* ee_mem, u32 s7_ptr) {
  GoalTexturePageHeader page;
  memcpy(&page, tpage, sizeof(page));
  // (AI-assisted) Like add-to-dma-buffer / upload-vram-pages (texture.gc): ONE 128 pixel wide
  // PSMCT32 image from the first uploaded segment's data to its dest, in whole 4096 word chunks
  // (32 rows), not one image per segment of exactly its size. The page layouts depend on that: the
  // last rows of a segment share VRAM pages with CLUTs that the next segment's first words fill
  // (village1-vis-alpha: the sky and cloud CLUT entries >= 160 stayed stale, a rainbow sky).
  int first_seg = 0;
  u32 words = 0;
  switch (mode) {
    case -1:
      words = page.size;
      break;
    case -2:
      words = page.segment[0].size + page.segment[1].size;
      break;
    case 0:
      words = page.segment[0].size;
      break;
    case 2:
      first_seg = 2;
      words = page.segment[2].size;
      break;
    default:
      lg::warn("[ctr vram] unsupported upload mode {}", mode);
      return;
  }
  const auto& seg = page.segment[first_seg];
  if (!words || !seg.block_data_ptr) {
    return;
  }
  words = (words + 4095) & ~4095u;
  // the PS2 reads past the page's data to fill the last chunk: stay inside EE memory
  if ((u64)seg.block_data_ptr + (u64)words * 4 > (u64)EE_MAIN_MEM_SIZE) {
    if (seg.block_data_ptr >= (u32)EE_MAIN_MEM_SIZE) {
      return;
    }
    words = ((u32)EE_MAIN_MEM_SIZE - seg.block_data_ptr) / 4 / 128 * 128;
  }
  const u32 dest_block = seg.dest / 64;
  // skip the upload if this exact data is already there (sampled hash, pages are static)
  const u8* src = ee_mem + seg.block_data_ptr;
  u64 hash = words;
  for (u32 k = 0; k < 64; k++) {
    u32 w;
    memcpy(&w, src + 4 * ((u64)k * words / 64), 4);
    hash = hash * 1099511628211ull ^ w;
  }
  auto last = m_last_upload.find(dest_block);
  if (last != m_last_upload.end() && last->second.src == src && last->second.words == words &&
      last->second.hash == hash) {
    m_stats.uploads++;
    return;
  }
  // remember the textures' formats by address, for relocate(). (AI-assisted) After the same-data
  // check: a page uploaded again unchanged has them there already (another page uploaded over it
  // in between fails the check)
  note_tex_info(tpage, page.length, ee_mem, s7_ptr);
  // whole 32 row chunks of a 128 pixel wide image: a contiguous block range
  const u32 end_block = upload_end_block(dest_block, words);
  // Other uploads this one overwrites are no longer "already there": without this, a page
  // that is uploaded again after another page covered its area (the common textures, e.g.
  // the eye textures, after a level's pris page) was skipped and read back as garbage.
  forget_uploads(dest_block, end_block);
  m_last_upload[dest_block] = UploadRecord{src, words, hash};
  // drop the pending uploads (or chunks of them) that this one completely overwrites
  trim_pending(dest_block, end_block);
  m_pending.push_back(PendingUpload{src, dest_block, end_block, words});
  m_stats.uploads++;
}

const void* CtrVram::upload_source(u32 block) const {
  for (const auto& [b0, r] : m_last_upload) {
    if (block >= b0 && block < upload_end_block(b0, r.words)) {
      return r.src;
    }
  }
  return nullptr;
}

void CtrVram::forget_uploads(u32 first_block, u32 end_block) {
  for (auto it = m_last_upload.begin(); it != m_last_upload.end();) {
    const u32 b0 = it->first, b1 = upload_end_block(b0, it->second.words);
    if (b0 < end_block && first_block < b1) {
      it = m_last_upload.erase(it);
    } else {
      ++it;
    }
  }
}

void CtrVram::write_upload(const u8* src, u32 dest_block, u32 words) {
  // (AI-assisted) chunk by chunk: a whole chunk whose data is in the VRAM copy already (the same
  // source; the game uploads its pages to the same VRAM every frame) isn't written again
  for (u32 done = 0; done < words; done += kChunkWords) {
    const u32 n = std::min(kChunkWords, words - done);
    const u32 block = dest_block + done / (kBlockBytes / 4);
    const u8* s = src + (size_t)done * 4;
    const u32 end = upload_end_block(block, n);
    if (n == kChunkWords && block % kChunkBlocks == 0 && block < kBlocks) {
      const u64 id = page_chunk_id(s);
      u64& cur = m_chunk_id[block / kChunkBlocks];
      if (cur == id) {
        m_stats.chunks_skipped++;
        continue;
      }
      write_rows(s, block, n);
      cur = id;
    } else {
      write_rows(s, block, n);
      mark_unique(block, end);
    }
    m_stats.chunks_written++;
    note_write(block, end);
  }
}

void CtrVram::write_rows(const u8* src, u32 dest_block, u32 words) {
  // The game uploads 128 pixel wide PSMCT32 images (upload-vram-data in texture.gc).
  const u32 rows = (words + 127) / 128;
  if (words % 128) {
    // partial last row: copy through a padded buffer
    std::vector<u32> tmp(rows * 128, 0);
    memcpy(tmp.data(), src, words * 4);
    write_ct32((const u8*)tmp.data(), dest_block, 128, rows);
  } else {
    write_ct32(src, dest_block, 128, rows);
  }
}

CtrVram::PendingUpload CtrVram::piece(const PendingUpload& p, u32 c0, u32 c1) {
  PendingUpload q;
  q.src = p.src + (size_t)c0 * kChunkWords * 4;
  q.dest_block = p.dest_block + c0 * kChunkBlocks;
  q.words = std::min(p.words, c1 * kChunkWords) - c0 * kChunkWords;
  q.end_block = upload_end_block(q.dest_block, q.words);
  return q;
}

void CtrVram::trim_pending(u32 first_block, u32 end_block) {
  // (AI-assisted) Not only whole pending uploads that this one covers: their chunks it covers too
  // (pieces of whole chunks stay). Every frame the game uploads the levels' tfrag, pris, shrub,
  // alpha and water pages and a common page to the same VRAM, and only the last one is read: the
  // chunks the others had there were written for nothing when a texture read them.
  bool any = false;
  for (const auto& p : m_pending) {
    any |= p.dest_block < end_block && first_block < p.end_block;
  }
  if (!any) {
    return;
  }
  m_keep.clear();
  for (const auto& p : m_pending) {
    if (!(p.dest_block < end_block && first_block < p.end_block)) {
      m_keep.push_back(p);
      continue;
    }
    // chunk k of p: blocks [dest + 64k, min(dest + 64(k + 1), end)). Covered: [ca, cb)
    const u32 chunks = (p.words + kChunkWords - 1) / kChunkWords;
    const u32 ca = first_block > p.dest_block
                       ? (first_block - p.dest_block + kChunkBlocks - 1) / kChunkBlocks
                       : 0;
    const u32 cb = end_block >= p.end_block ? chunks
                                            : std::min(chunks, (end_block - p.dest_block) /
                                                                   kChunkBlocks);
    if (ca >= cb) {
      m_keep.push_back(p);
      continue;
    }
    if (ca > 0) {
      m_keep.push_back(piece(p, 0, ca));
    }
    if (cb < chunks) {
      m_keep.push_back(piece(p, cb, chunks));
    }
  }
  m_pending.swap(m_keep);
}

void CtrVram::flush_pending() {
  // in order: later uploads may overlap earlier ones
  m_write.clear();
  m_write.swap(m_pending);
  for (const auto& p : m_write) {
    write_upload(p.src, p.dest_block, p.words);
  }
}

void CtrVram::flush_pending(u32 first_block, u32 end_block) {
  // write the uploads that overlap the range, in order; keep the others pending.
  // (AI-assisted) Only the 32 row chunks of an upload that overlap the range are written (a 128
  // pixel wide PSMCT32 image: each chunk is 4096 words at 64 consecutive blocks); its other chunks
  // stay pending. A sprite needing one small texture used to write the whole page, every frame
  // where two levels alternate their pages in the same VRAM (Misty: ~12% of the render thread).
  if (!pending_overlaps(first_block, end_block)) {
    return;
  }
  m_keep.clear();
  m_write.clear();
  for (const auto& p : m_pending) {
    if (p.dest_block < end_block && first_block < p.end_block) {
      const u32 chunks = (p.words + kChunkWords - 1) / kChunkWords;
      const u32 c0 = first_block > p.dest_block ? (first_block - p.dest_block) / kChunkBlocks : 0;
      const u32 c1 =
          std::min(chunks, (end_block - p.dest_block + kChunkBlocks - 1) / kChunkBlocks);
      if (c0 >= c1) {
        m_keep.push_back(p);
        continue;
      }
      if (c0 > 0) {
        m_keep.push_back(piece(p, 0, c0));
      }
      m_write.push_back(piece(p, c0, c1));
      if (c1 < chunks) {
        m_keep.push_back(piece(p, c1, chunks));
      }
    } else {
      m_keep.push_back(p);
    }
  }
  if (m_write.empty()) {
    return;
  }
  m_pending.swap(m_keep);
  for (const auto& p : m_write) {
    write_upload(p.src, p.dest_block, p.words);
  }
}

bool CtrVram::pending_overlaps(u32 first_block, u32 end_block) const {
  for (const auto& p : m_pending) {
    if (p.dest_block < end_block && first_block < p.end_block) {
      return true;
    }
  }
  return false;
}

namespace {
bool is_indexed(u32 psm) {
  return psm == (u32)GsTex0::PSM::PSMT8 || psm == (u32)GsTex0::PSM::PSMT4 ||
         psm == (u32)GsTex0::PSM::PSMT8H || psm == (u32)GsTex0::PSM::PSMT4HH ||
         psm == (u32)GsTex0::PSM::PSMT4HL;
}
}  // namespace

void CtrVram::relocate(u32 dest_block, u32 src_block, u32 dest_psm) {
  const TexInfo info = src_block < kBlocks ? m_tex_info[src_block] : TexInfo{};
  if (!info.w) {
    lg::warn("[ctr vram] relocate from unknown texture at {}", src_block);
    return;
  }
  if (!is_indexed(info.psm) || !is_indexed(dest_psm)) {
    lg::warn("[ctr vram] relocate {} -> {} not supported", info.psm, dest_psm);
    return;
  }
  // source texture and CLUT, in blocks (generous: whole pages)
  const u32 bits = (info.psm == (u32)GsTex0::PSM::PSMT4) ? 4
                   : (info.psm == (u32)GsTex0::PSM::PSMT8) ? 8
                                                            : 32;
  const u32 src_blocks = ((u32)info.w * info.h * bits / 8 + 8191) / 8192 * 32;
  flush_pending(src_block, src_block + src_blocks);
  flush_pending(info.clutdest, info.clutdest + 4);
  // the game relocates the font every frame: skip if the source and CLUT didn't change (and
  // nothing was written over the result: note_write). (AI-assisted) By the ids of their chunks,
  // not a hash of their bytes (~64 KB, every frame)
  u64 sig = ((u64)src_block << 40) ^ ((u64)dest_psm << 32) ^ info.clutdest;
  sig = mix_chunk_ids(sig, src_block, src_block + src_blocks);
  sig = mix_chunk_ids(sig, info.clutdest, info.clutdest + 4);
  const u64 reloc_key = ((u64)dest_block << 8) | (dest_psm & 0xff);
  auto last = m_last_relocate.find(reloc_key);
  if (last != m_last_relocate.end() && last->second.sig == sig) {
    m_stats.relocates_skipped++;
    return;
  }
  const u32 width = std::max<u32>(1, info.width) * 64;
  Range range;
  // (AI-assisted) whether any byte changed: relocating the same again leaves the chunks' ids (and
  // the textures decoded from them) as they are
  bool changed = false;
  for (u32 y = 0; y < info.h; y++) {
    for (u32 x = 0; x < info.w; x++) {
      // read the index in the source format
      u32 idx = 0;
      switch ((GsTex0::PSM)info.psm) {
        case GsTex0::PSM::PSMT4: {
          u32 a = (psmt4_addr_half_byte(x, y, width) + src_block * kBlockBytes * 2) &
                  (kVramMask * 2 + 1);
          u8 b = m_vram[a / 2];
          idx = (a & 1) ? (b >> 4) : (b & 0xf);
        } break;
        case GsTex0::PSM::PSMT8:
          idx = m_vram[(psmt8_addr(x, y, width) + src_block * kBlockBytes) & kVramMask];
          break;
        default: {
          u32 v = read32(psmct32_addr(x, y, width) + src_block * kBlockBytes);
          idx = info.psm == (u32)GsTex0::PSM::PSMT8H    ? (v >> 24)
                : info.psm == (u32)GsTex0::PSM::PSMT4HH ? (v >> 28)
                                                        : ((v >> 24) & 0xf);
        } break;
      }
      // write it in the destination format
      switch ((GsTex0::PSM)dest_psm) {
        case GsTex0::PSM::PSMT4: {
          u32 a = (psmt4_addr_half_byte(x, y, width) + dest_block * kBlockBytes * 2) &
                  (kVramMask * 2 + 1);
          u8& b = m_vram[a / 2];
          const u8 nb = (a & 1) ? ((b & 0x0f) | (idx << 4)) : ((b & 0xf0) | (idx & 0xf));
          changed |= nb != b;
          b = nb;
          range.add(a / 2);
        } break;
        case GsTex0::PSM::PSMT8: {
          u32 a = (psmt8_addr(x, y, width) + dest_block * kBlockBytes) & kVramMask;
          changed |= m_vram[a] != (u8)idx;
          m_vram[a] = idx;
          range.add(a);
        } break;
        default: {
          u32 a = (psmct32_addr(x, y, width) + dest_block * kBlockBytes) & kVramMask & ~3u;
          u32 v;
          memcpy(&v, m_vram.data() + a, 4);
          const u32 old = v;
          if (dest_psm == (u32)GsTex0::PSM::PSMT8H) {
            v = (v & 0x00ffffff) | (idx << 24);
          } else if (dest_psm == (u32)GsTex0::PSM::PSMT4HH) {
            v = (v & 0x0fffffff) | ((idx & 0xf) << 28);
          } else {
            v = (v & 0xf0ffffff) | ((idx & 0xf) << 24);
          }
          changed |= v != old;
          memcpy(m_vram.data() + a, &v, 4);
          range.add(a);
        } break;
      }
    }
  }
  // snapshot the CLUT
  Relocation reloc;
  reloc.psm = dest_psm;
  u32 entries = (info.psm == (u32)GsTex0::PSM::PSMT8 || info.psm == (u32)GsTex0::PSM::PSMT8H)
                    ? 256
                    : 16;
  for (u32 e = 0; e < entries; e++) {
    reloc.clut.push_back(clut_color(info.clutdest, info.clutpsm, e));
  }
  auto& list = m_relocations[dest_block];
  const Relocation* before = nullptr;
  for (const auto& r : list) {
    if (r.psm == dest_psm) {
      before = &r;
    }
  }
  // (AI-assisted) a new CLUT changes the textures there as much as new indices
  changed |= !before || before->clut != reloc.clut;
  list.erase(std::remove_if(list.begin(), list.end(),
                            [&](const Relocation& r) { return r.psm == dest_psm; }),
             list.end());
  list.push_back(std::move(reloc));
  RelocateRecord rec{sig, 0, 0};
  if (range.hi >= range.lo) {
    rec.first_block = range.lo / kBlockBytes;
    rec.end_block = range.hi / kBlockBytes + 1;
    if (changed) {
      mark_unique(rec.first_block, rec.end_block);
      note_write(rec.first_block, rec.end_block);  // (before this relocation's record is kept)
    }
  }
  m_last_relocate[reloc_key] = rec;
}

const CtrVram::Relocation* CtrVram::find_relocation(u32 block, u32 psm) const {
  auto it = m_relocations.find(block);
  if (it == m_relocations.end()) {
    return nullptr;
  }
  for (const auto& r : it->second) {
    if (r.psm == psm) {
      return &r;
    }
  }
  return nullptr;
}

u32 CtrVram::clut_color(u32 cbp, u32 cpsm, u32 entry) const {
  // CSM1 layout: 16 entry groups of 8x2 pixels, two groups per 16 pixel row.
  u32 chunk = entry / 16;
  u32 off = entry % 16;
  u32 x = (chunk & 1) * 8 + (off % 8);
  u32 y = (chunk >> 1) * 2 + (off / 8);
  if (cpsm == 0) {  // PSMCT32
    return read32(psmct32_addr(x, y, 64) + cbp * kBlockBytes);
  } else {  // PSMCT16 / 16S
    u32 addr = (psmct16_addr(x, y, 64) + cbp * kBlockBytes) & kVramMask;
    u16 c;
    memcpy(&c, m_vram.data() + (addr & ~1u), 2);
    return ct16_to_rgba(c);
  }
}

namespace {
// clut(cbp, cpsm, index) -> RGBA8: a template (inlined per texel), not a std::function
template <typename Clut>
bool decode_impl(const CtrVram* self,
                 const std::vector<u8>& vram,
                 u64 tex0,
                 std::vector<u32>* out,
                 int* w_out,
                 int* h_out,
                 Range* tex_range,
                 const Clut& clut) {
  (void)self;
  GsTex0 t(tex0);
  u32 tw = std::min(t.tw(), 10u);
  u32 th = std::min(t.th(), 10u);
  int w = 1 << tw;
  int h = 1 << th;
  u32 tbp = t.tbp0();
  u32 width = std::max(1u, t.tbw()) * 64;
  u32 cbp = t.cbp();
  u32 cpsm = t.cpsm();
  u32 csa = (tex0 >> 56) & 0x1f;
  out->resize(w * h);
  // (AI-assisted) the CLUT entries the texture can use, read once instead of per texel: 256, or
  // the 16 from CSA for 4 bit indices
  u32 clut_tab[256];
  {
    const auto psm = t.psm();
    const bool idx8 = psm == GsTex0::PSM::PSMT8 || psm == GsTex0::PSM::PSMT8H;
    const bool idx4 = psm == GsTex0::PSM::PSMT4 || psm == GsTex0::PSM::PSMT4HH ||
                      psm == GsTex0::PSM::PSMT4HL;
    if (idx8 || idx4) {
      const u32 first = idx4 ? csa * 16 : 0;
      for (u32 e = 0; e < (idx4 ? 16u : 256u); e++) {
        clut_tab[e] = clut(cbp, cpsm, first + e);
      }
    }
  }
  auto rd32 = [&](u32 a) {
    tex_range->add(a);
    a &= kVramMask & ~3u;
    u32 v;
    memcpy(&v, vram.data() + a, 4);
    return v;
  };

  switch (t.psm()) {
    case GsTex0::PSM::PSMCT32:
    case GsTex0::PSM::PSMCT24:
      for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
          u32 v = rd32(psmct32_addr(x, y, width) + tbp * kBlockBytes);
          if (t.psm() == GsTex0::PSM::PSMCT24) {
            v = (v & 0xffffff) | (0x80u << 24);
          }
          (*out)[x + y * w] = v;
        }
      }
      break;
    case GsTex0::PSM::PSMCT16:
    case GsTex0::PSM::PSMCT16S:
      for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
          u32 a = psmct16_addr(x, y, width) + tbp * kBlockBytes;
          tex_range->add(a);
          a &= kVramMask;
          u16 c;
          memcpy(&c, vram.data() + (a & ~1u), 2);
          (*out)[x + y * w] = ct16_to_rgba(c);
        }
      }
      break;
    case GsTex0::PSM::PSMT8:
      for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
          u32 a = psmt8_addr(x, y, width) + tbp * kBlockBytes;
          tex_range->add(a);
          a &= kVramMask;
          (*out)[x + y * w] = clut_tab[vram[a]];
        }
      }
      break;
    case GsTex0::PSM::PSMT4:
      for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
          u32 a = psmt4_addr_half_byte(x, y, width) + tbp * kBlockBytes * 2;
          tex_range->add(a / 2);
          a &= kVramMask * 2 + 1;
          u8 b = vram[a / 2];
          u32 idx = (a & 1) ? (b >> 4) : (b & 0xf);
          (*out)[x + y * w] = clut_tab[idx];
        }
      }
      break;
    case GsTex0::PSM::PSMT8H:
    case GsTex0::PSM::PSMT4HH:
    case GsTex0::PSM::PSMT4HL:
      for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
          u32 v = rd32(psmct32_addr(x, y, width) + tbp * kBlockBytes);
          u32 idx;
          if (t.psm() == GsTex0::PSM::PSMT8H) {
            idx = v >> 24;
          } else if (t.psm() == GsTex0::PSM::PSMT4HH) {
            idx = v >> 28;
          } else {
            idx = (v >> 24) & 0xf;
          }
          (*out)[x + y * w] = clut_tab[idx];
        }
      }
      break;
    default:
      return false;
  }
  *w_out = w;
  *h_out = h;
  return true;
}
}  // namespace

namespace {
// (AI-assisted) The blocks from TBP0 that a texture may read. The textures sit in a buffer TBW x 64
// pixels wide, in pages (8 x 4 blocks): a texture narrower than its buffer spans page rows that are
// a whole buffer width apart, more than its own size. flush_pending writes only the upload chunks
// in this range, so an estimate from the texture's size alone left parts of wide-buffer textures
// unwritten (the zoomer HUD as a grey square, flickering water ripples).
u32 texture_block_span(const GsTex0& t) {
  const u32 w = 1u << t.tw(), h = 1u << t.th();
  // page size as shifts (the divides were library calls on the ARM11, on every texture lookup
  // with uploads pending)
  u32 page_w_log2 = 6, page_h_log2 = 5;  // PSMCT32 / 24 and those stored like it (8H, 4HH, 4HL)
  switch ((u32)t.psm()) {
    case (u32)GsTex0::PSM::PSMCT16:
    case (u32)GsTex0::PSM::PSMCT16S:
      page_h_log2 = 6;
      break;
    case (u32)GsTex0::PSM::PSMT8:
      page_w_log2 = 7;
      page_h_log2 = 6;
      break;
    case (u32)GsTex0::PSM::PSMT4:
      page_w_log2 = 7;
      page_h_log2 = 7;
      break;
    default:
      break;
  }
  const u32 page_w = 1u << page_w_log2, page_h = 1u << page_h_log2;
  const u32 row_pages = (w + page_w - 1) >> page_w_log2;
  const u32 stride_pages = std::max(row_pages, (t.tbw() * 64 + page_w - 1) >> page_w_log2);
  const u32 page_rows = (h + page_h - 1) >> page_h_log2;
  const u32 pages = (page_rows - 1) * stride_pages + row_pages;
  const u32 by_size = w * h * 4 / kBlockBytes + 64;  // as if 32 bit, plus slack
  return std::max(by_size, pages * 32 + 32);
}
}  // namespace

bool CtrVram::decode(u64 tex0, std::vector<u32>* out, int* w, int* h) const {
  const_cast<CtrVram*>(this)->flush_pending();
  Range r;
  return decode_impl(this, m_vram, tex0, out, w, h, &r,
                     [this](u32 cbp, u32 cpsm, u32 e) { return clut_color(cbp, cpsm, e); });
}

bool CtrVram::decode_for_cpu(u64 tex0, std::vector<u32>* out, int* w, int* h) {
  GsTex0 t(tex0);
  if (!m_pending.empty()) {
    // only the pending uploads this texture reads (as get_texture)
    const u32 blocks = texture_block_span(t);
    flush_pending(t.tbp0(), t.tbp0() + blocks);
    flush_pending(t.cbp(), t.cbp() + 4);
  }
  const Relocation* reloc = find_relocation(t.tbp0(), (u32)t.psm());
  Range r;
  if (reloc) {
    return decode_impl(this, m_vram, tex0, out, w, h, &r,
                       [reloc](u32, u32, u32 e) { return reloc->clut[e % reloc->clut.size()]; });
  }
  return decode_impl(this, m_vram, tex0, out, w, h, &r,
                     [this](u32 cbp, u32 cpsm, u32 e) { return clut_color(cbp, cpsm, e); });
}

CtrVram::Entry* CtrVram::find_cached(u64 key) {
  // (AI-assisted) multiplicative hash of the folded key: the top bits pick the slot
  const u32 folded = (u32)key ^ (u32)(key >> 32);
  FrontSlot& slot = m_front[(folded * 0x9e3779b1u) >> (32 - kFrontSlotsLog2)];
  if (slot.gen == m_cache_gen && slot.key == key) {
    return slot.entry;
  }
  auto it = m_cache.find(key);
  if (it == m_cache.end()) {
    return nullptr;
  }
  // (unordered_map nodes don't move on insert: the pointer is good until the entry is erased)
  slot.key = key;
  slot.entry = &it->second;
  slot.gen = m_cache_gen;
  return &it->second;
}

u64 CtrVram::entry_sig(const Entry& e) const {
  return mix_chunk_ids(mix_chunk_ids(1469598103934665603ull, e.first_block, e.end_block),
                       e.clut_first, e.clut_end);
}

const CtrTexture* CtrVram::get_texture(u64 tex0) {
  // the parts of TEX0 that select the texture data (not TCC / TFX / CLUT load control)
  constexpr u64 kKeyMask = ~((0b111ull << 34) | (0b111ull << 61));
  u64 key = tex0 & kKeyMask;
  Entry* cached = find_cached(key);
  // (AI-assisted) valid while nothing pending is over it and its chunks hold what it was decoded
  // from (entry_sig: a few chunk ids, not the texture's bytes)
  if (cached &&
      (m_pending.empty() || (!pending_overlaps(cached->first_block, cached->end_block) &&
                             !pending_overlaps(cached->clut_first, cached->clut_end))) &&
      entry_sig(*cached) == cached->sig) {
    m_stats.cached++;
    return &cached->tex;
  }
  GsTex0 t(tex0);
  if (!m_pending.empty()) {
    // write the pending uploads this texture (or its cached version) may read
    const u32 blocks = texture_block_span(t);
    flush_pending(t.tbp0(), t.tbp0() + blocks);
    flush_pending(t.cbp(), t.cbp() + 4);
  }
  if (cached && entry_sig(*cached) == cached->sig) {
    m_stats.cached++;  // the uploads written were the data already there
    return &cached->tex;
  }

  // same VRAM contents as a texture decoded before (the game re-uploads pages every frame,
  // alternating pages in the same VRAM): reuse it instead of decoding again
  auto stale = m_stale.find(key);
  if (stale != m_stale.end()) {
    if (entry_sig(stale->second) == stale->second.sig) {
      m_stats.revived++;
      if (cached) {
        std::swap(*cached, stale->second);  // the outdated one waits there in turn
        return &cached->tex;
      }
      Entry* e = &m_cache.emplace(key, stale->second).first->second;
      m_stale.erase(stale);
      return &e->tex;
    }
    if (!cached) {
      if (stale->second.tex.handle >= 0) {
        ctr_gpu_tex_delete(stale->second.tex.handle);
      }
      m_stale.erase(stale);
    }
  }

  std::vector<u32> data;
  int w = 0, h = 0;
  Range tex_range;
  const bool has_clut = t.psm() == GsTex0::PSM::PSMT8 || t.psm() == GsTex0::PSM::PSMT4 ||
                        t.psm() == GsTex0::PSM::PSMT8H || t.psm() == GsTex0::PSM::PSMT4HH ||
                        t.psm() == GsTex0::PSM::PSMT4HL;
  // textures moved by texture-relocate use the CLUT they had when they were moved
  const Relocation* reloc = find_relocation(t.tbp0(), (u32)t.psm());
  const bool decoded =
      reloc ? decode_impl(this, m_vram, key, &data, &w, &h, &tex_range,
                          [reloc](u32, u32, u32 e) { return reloc->clut[e % reloc->clut.size()]; })
            : decode_impl(this, m_vram, key, &data, &w, &h, &tex_range,
                          [this](u32 cbp, u32 cpsm, u32 e) { return clut_color(cbp, cpsm, e); });
  if (!decoded) {
    lg::warn("[ctr vram] unsupported texture format {} (tex0 {:x})", (int)t.psm(), tex0);
    return nullptr;
  }

  // the 3DS needs at least 8x8: repeat pixels (normalized coordinates stay the same)
  int gw = std::max(w, 8), gh = std::max(h, 8);
  if (gw != w || gh != h) {
    std::vector<u32> big(gw * gh);
    for (int y = 0; y < gh; y++) {
      for (int x = 0; x < gw; x++) {
        big[x + y * gw] = data[(x * w / gw) + (y * h / gh) * w];
      }
    }
    data.swap(big);
  }

  Entry e;
  e.tex.w = gw;
  e.tex.h = gh;
  // (AI-assisted) the smallest 3DS format that holds it (sprites, HUD, font), checked for being a
  // radial glow (CtrSpriteRenderer draws those with the procedural texture unit)
  e.tex.handle = ctr_gpu_tex_create_compact(gw, gh, (const uint8_t*)data.data(), CTR_TEX_CHECK_RADIAL);
  e.first_block = tex_range.lo / kBlockBytes;
  e.end_block = tex_range.hi / kBlockBytes + 1;
  if (has_clut && !reloc) {
    e.clut_first = t.cbp();
    e.clut_end = t.cbp() + 4;  // 256 x 32-bit entries = 4 blocks
  } else {
    e.clut_first = e.clut_end = 0;
  }
  // (AI-assisted) a texture relocated with a new CLUT: relocate() gives its chunks new ids
  e.sig = entry_sig(e);
  m_stats.decoded++;
  if (cached) {
    keep_stale(key, *cached);  // the outdated version: the same data often comes back
    *cached = e;
    return &cached->tex;
  }
  if (m_cache.size() >= m_sweep_at) {
    sweep_outdated();
  }
  auto res = m_cache.emplace(key, e);
  return &res.first->second.tex;
}

void CtrVram::keep_stale(u64 key, const Entry& e) {
  if (m_stale.size() > 256) {
    clear_stale();  // bound the GPU memory of kept textures
  }
  auto it = m_stale.find(key);
  if (it == m_stale.end()) {
    m_stale.emplace(key, e);
    return;
  }
  if (it->second.tex.handle >= 0) {
    ctr_gpu_tex_delete(it->second.tex.handle);
  }
  it->second = e;
}

void CtrVram::sweep_outdated() {
  for (auto it = m_cache.begin(); it != m_cache.end();) {
    if (entry_sig(it->second) != it->second.sig) {
      keep_stale(it->first, it->second);
      it = m_cache.erase(it);
    } else {
      ++it;
    }
  }
  m_cache_gen++;  // (AI-assisted) the front cache may point at erased entries
  m_sweep_at = std::max<size_t>(256, m_cache.size() + 128);
}

void CtrVram::clear_cache() {
  for (auto& [key, e] : m_cache) {
    if (e.tex.handle >= 0) {
      ctr_gpu_tex_delete(e.tex.handle);
    }
  }
  m_cache.clear();
  m_cache_gen++;
  clear_stale();
}

void CtrVram::clear_stale() {
  for (auto& [key, e] : m_stale) {
    if (e.tex.handle >= 0) {
      ctr_gpu_tex_delete(e.tex.handle);
    }
  }
  m_stale.clear();
}
