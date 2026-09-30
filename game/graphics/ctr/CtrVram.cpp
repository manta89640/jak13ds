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
#include "common/log/log.h"
#include "common/texture/texture_conversion.h"
#include "common/util/Assert.h"

#include "game/graphics/ctr/ctr_gpu.h"

namespace {
constexpr u32 kVramBytes = 4 * 1024 * 1024;
constexpr u32 kVramMask = kVramBytes - 1;
constexpr u32 kBlockBytes = 256;

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

// 16-bit GS color to our RGBA8 (alpha: TEXA with TA0 = TA1 = 0x80, the game's setting)
u32 ct16_to_rgba(u32 c) {
  return pack_rgba((c & 0x1f) << 3, ((c >> 5) & 0x1f) << 3, ((c >> 10) & 0x1f) << 3, 0x80);
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
  m_vram.resize(kVramBytes);
  // a PSMCT32 page is 64 x 32 pixels (8 KB)
  m_ct32_page_table.resize(64 * 32);
  for (u32 y = 0; y < 32; y++) {
    for (u32 x = 0; x < 64; x++) {
      m_ct32_page_table[x + 64 * y] = psmct32_addr(x, y, 64);
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
  m_stats.uploads_changed++;
  m_last_relocate.clear();  // a relocated texture may have been overwritten
  const u32 pages_per_row = std::max(1u, width / 64);
  const u32 base = dest_block * kBlockBytes;
  for (u32 y = 0; y < height; y++) {
    const u32 page_row = (y / 32) * pages_per_row;
    const u32* ytab = &m_ct32_page_table[64 * (y % 32)];
    const u8* src_row = data + 4 * y * width;
    for (u32 x = 0; x < width; x++) {
      u32 addr = (base + (page_row + x / 64) * 8192 + ytab[x % 64]) & kVramMask;
      memcpy(m_vram.data() + addr, src_row + 4 * x, 4);
    }
  }
  u32 first = dest_block;
  u32 end = dest_block + (width * height * 4 + kBlockBytes - 1) / kBlockBytes;
  invalidate_blocks(first, end);
}

void CtrVram::upload_texture_page(const u8* tpage, int mode, const u8* ee_mem, u32 s7_ptr) {
  GoalTexturePageHeader page;
  memcpy(&page, tpage, sizeof(page));
  // remember the textures' formats by address, for relocate()
  for (int i = 0; i < page.length && i < 1024; i++) {
    u32 ptr;
    memcpy(&ptr, tpage + sizeof(GoalTexturePageHeader) + 4 * i, 4);
    if (!ptr || ptr == s7_ptr) {
      continue;
    }
    GoalTexture tex;
    memcpy(&tex, ee_mem + ptr, sizeof(tex));
    m_tex_info[tex.dest[0]] = TexInfo{(u16)tex.w, (u16)tex.h, tex.psm, tex.width[0], tex.clutpsm,
                                      tex.clut_dest};
  }
  bool segs[3] = {true, true, true};
  switch (mode) {
    case -1:
      break;
    case 2:
      segs[0] = segs[1] = false;
      break;
    case -2:
      segs[2] = false;
      break;
    case 0:
      segs[1] = segs[2] = false;
      break;
    default:
      lg::warn("[ctr vram] unsupported upload mode {}", mode);
      return;
  }
  for (int i = 0; i < 3; i++) {
    const auto& seg = page.segment[i];
    if (!segs[i] || !seg.size || !seg.block_data_ptr) {
      continue;
    }
    // The game uploads 128 pixel wide PSMCT32 images (upload-vram-data in texture.gc).
    u32 rows = (seg.size + 127) / 128;
    u32 dest_block = seg.dest / 64;
    // skip the upload if this exact data is already there (sampled hash, pages are static)
    const u8* src = ee_mem + seg.block_data_ptr;
    u64 hash = seg.size;
    for (u32 k = 0; k < 64; k++) {
      u32 w;
      memcpy(&w, src + 4 * ((u64)k * seg.size / 64), 4);
      hash = hash * 1099511628211ull ^ w;
    }
    auto last = m_last_upload.find(dest_block);
    if (last != m_last_upload.end() && last->second.src == src && last->second.words == seg.size &&
        last->second.hash == hash) {
      m_stats.uploads++;
      continue;
    }
    m_last_upload[dest_block] = UploadRecord{src, seg.size, hash};
    // record it: drop pending uploads that this one completely overwrites
    const u32 end_block = dest_block + (rows * 128 * 4 + kBlockBytes - 1) / kBlockBytes;
    m_pending.erase(std::remove_if(m_pending.begin(), m_pending.end(),
                                   [&](const PendingUpload& p) {
                                     return p.dest_block >= dest_block && p.end_block <= end_block;
                                   }),
                    m_pending.end());
    m_pending.push_back(PendingUpload{src, dest_block, end_block, seg.size});
    m_stats.uploads++;
  }
}

void CtrVram::write_upload(const u8* src, u32 dest_block, u32 words) {
  // The game uploads 128 pixel wide PSMCT32 images (upload-vram-data in texture.gc).
  const u32 rows = (words + 127) / 128;
  if (words % 128) {
    // partial last row: copy through a padded buffer
    std::vector<u32> tmp(rows * 128, 0);
    memcpy(tmp.data(), src, words * 4);
    upload_ct32((const u8*)tmp.data(), dest_block, 128, rows);
  } else {
    upload_ct32(src, dest_block, 128, rows);
  }
}

void CtrVram::flush_pending() {
  // in order: later uploads may overlap earlier ones
  std::vector<PendingUpload> pending;
  pending.swap(m_pending);
  for (const auto& p : pending) {
    write_upload(p.src, p.dest_block, p.words);
  }
}

void CtrVram::flush_pending(u32 first_block, u32 end_block) {
  // write the uploads that overlap the range, in order; keep the others pending
  if (m_pending.empty()) {
    return;
  }
  std::vector<PendingUpload> keep;
  std::vector<PendingUpload> write;
  for (const auto& p : m_pending) {
    if (p.dest_block < end_block && first_block < p.end_block) {
      write.push_back(p);
    } else {
      keep.push_back(p);
    }
  }
  if (write.empty()) {
    return;
  }
  m_pending.swap(keep);
  for (const auto& p : write) {
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
  auto it = m_tex_info.find(src_block);
  if (it == m_tex_info.end()) {
    lg::warn("[ctr vram] relocate from unknown texture at {}", src_block);
    return;
  }
  const TexInfo info = it->second;
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
  // the game relocates the font every frame: skip if the source and CLUT didn't change
  u64 sig = ((u64)src_block << 40) ^ ((u64)dest_psm << 32) ^ info.clutdest;
  {
    const u32 base = (src_block * kBlockBytes) & kVramMask;
    const u32 bytes = std::min(src_blocks * kBlockBytes, kVramBytes - base);
    for (u32 o = 0; o + 8 <= bytes; o += 8) {
      u64 v;
      memcpy(&v, m_vram.data() + base + o, 8);
      sig = (sig ^ v) * 1099511628211ull;
    }
    const u32 cbase = (info.clutdest * kBlockBytes) & kVramMask;
    for (u32 o = 0; o + 8 <= 4 * kBlockBytes && cbase + o + 8 <= kVramBytes; o += 8) {
      u64 v;
      memcpy(&v, m_vram.data() + cbase + o, 8);
      sig = (sig ^ v) * 1099511628211ull;
    }
  }
  const u64 reloc_key = ((u64)dest_block << 8) | (dest_psm & 0xff);
  auto last = m_last_relocate.find(reloc_key);
  if (last != m_last_relocate.end() && last->second == sig) {
    m_stats.relocates_skipped++;
    return;
  }
  m_last_relocate[reloc_key] = sig;
  const u32 width = std::max<u32>(1, info.width) * 64;
  Range range;
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
          b = (a & 1) ? ((b & 0x0f) | (idx << 4)) : ((b & 0xf0) | (idx & 0xf));
          range.add(a / 2);
        } break;
        case GsTex0::PSM::PSMT8: {
          u32 a = (psmt8_addr(x, y, width) + dest_block * kBlockBytes) & kVramMask;
          m_vram[a] = idx;
          range.add(a);
        } break;
        default: {
          u32 a = (psmct32_addr(x, y, width) + dest_block * kBlockBytes) & kVramMask & ~3u;
          u32 v;
          memcpy(&v, m_vram.data() + a, 4);
          if (dest_psm == (u32)GsTex0::PSM::PSMT8H) {
            v = (v & 0x00ffffff) | (idx << 24);
          } else if (dest_psm == (u32)GsTex0::PSM::PSMT4HH) {
            v = (v & 0x0fffffff) | ((idx & 0xf) << 28);
          } else {
            v = (v & 0xf0ffffff) | ((idx & 0xf) << 24);
          }
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
  list.erase(std::remove_if(list.begin(), list.end(),
                            [&](const Relocation& r) { return r.psm == dest_psm; }),
             list.end());
  list.push_back(std::move(reloc));
  if (range.hi >= range.lo) {
    invalidate_blocks(range.lo / kBlockBytes, range.hi / kBlockBytes + 1);
  }
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
bool decode_impl(const CtrVram* self,
                 const std::vector<u8>& vram,
                 u64 tex0,
                 std::vector<u32>* out,
                 int* w_out,
                 int* h_out,
                 Range* tex_range,
                 std::function<u32(u32, u32, u32)> clut) {
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
          (*out)[x + y * w] = clut(cbp, cpsm, vram[a]);
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
          (*out)[x + y * w] = clut(cbp, cpsm, csa * 16 + idx);
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
            idx = csa * 16 + (v >> 28);
          } else {
            idx = csa * 16 + ((v >> 24) & 0xf);
          }
          (*out)[x + y * w] = clut(cbp, cpsm, idx);
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

bool CtrVram::decode(u64 tex0, std::vector<u32>* out, int* w, int* h) const {
  const_cast<CtrVram*>(this)->flush_pending();
  Range r;
  return decode_impl(this, m_vram, tex0, out, w, h, &r,
                     [this](u32 cbp, u32 cpsm, u32 e) { return clut_color(cbp, cpsm, e); });
}

const CtrTexture* CtrVram::get_texture(u64 tex0) {
  // the parts of TEX0 that select the texture data (not TCC / TFX / CLUT load control)
  constexpr u64 kKeyMask = ~((0b111ull << 34) | (0b111ull << 61));
  u64 key = tex0 & kKeyMask;
  auto it = m_cache.find(key);
  if (it != m_cache.end()) {
    const auto& e = it->second;
    if (m_pending.empty() || (!pending_overlaps(e.first_block, e.end_block) &&
                              !pending_overlaps(e.clut_first, e.clut_end))) {
      m_stats.cached++;
      return &it->second.tex;
    }
  }
  if (!m_pending.empty()) {
    // write the pending uploads this texture (or its cached version) may read
    GsTex0 t(tex0);
    const u32 texels = (1u << t.tw()) * (1u << t.th());
    const u32 blocks = texels * 4 / kBlockBytes + 64;  // as if 32 bit, plus slack
    flush_pending(t.tbp0(), t.tbp0() + blocks);
    flush_pending(t.cbp(), t.cbp() + 4);
    it = m_cache.find(key);
    if (it != m_cache.end()) {
      m_stats.cached++;
      return &it->second.tex;
    }
  }

  // same VRAM contents as a texture decoded before (the game re-uploads pages every frame,
  // alternating pages in the same VRAM): reuse it instead of decoding again
  auto stale = m_stale.find(key);
  if (stale != m_stale.end()) {
    Entry e = stale->second;
    m_stale.erase(stale);
    if (content_hash(e, find_relocation(GsTex0(tex0).tbp0(), (u32)GsTex0(tex0).psm())) ==
        e.hash) {
      m_stats.revived++;
      auto res = m_cache.emplace(key, e);
      return &res.first->second.tex;
    }
    if (e.tex.handle >= 0) {
      ctr_gpu_tex_delete(e.tex.handle);
    }
  }

  std::vector<u32> data;
  int w = 0, h = 0;
  Range tex_range;
  GsTex0 t(tex0);
  const bool has_clut = t.psm() == GsTex0::PSM::PSMT8 || t.psm() == GsTex0::PSM::PSMT4 ||
                        t.psm() == GsTex0::PSM::PSMT8H || t.psm() == GsTex0::PSM::PSMT4HH ||
                        t.psm() == GsTex0::PSM::PSMT4HL;
  // textures moved by texture-relocate use the CLUT they had when they were moved
  const Relocation* reloc = find_relocation(t.tbp0(), (u32)t.psm());
  std::function<u32(u32, u32, u32)> clut;
  if (reloc) {
    clut = [reloc](u32, u32, u32 e) { return reloc->clut[e % reloc->clut.size()]; };
  } else {
    clut = [this](u32 cbp, u32 cpsm, u32 e) { return clut_color(cbp, cpsm, e); };
  }
  if (!decode_impl(this, m_vram, key, &data, &w, &h, &tex_range, clut)) {
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
  e.tex.handle = ctr_gpu_tex_create(gw, gh, (const uint8_t*)data.data());
  e.first_block = tex_range.lo / kBlockBytes;
  e.end_block = tex_range.hi / kBlockBytes + 1;
  if (has_clut && !reloc) {
    e.clut_first = t.cbp();
    e.clut_end = t.cbp() + 4;  // 256 x 32-bit entries = 4 blocks
  } else {
    e.clut_first = e.clut_end = 0;
  }
  e.hash = content_hash(e, reloc);
  m_stats.decoded++;
  auto res = m_cache.emplace(key, e);
  return &res.first->second.tex;
}

void CtrVram::invalidate_blocks(u32 first_block, u32 end_block) {
  if (m_stale.size() > 256) {
    clear_stale();  // bound the GPU memory of kept textures
  }
  for (auto it = m_cache.begin(); it != m_cache.end();) {
    const auto& e = it->second;
    bool hit = (e.first_block < end_block && first_block < e.end_block) ||
               (e.clut_first < end_block && first_block < e.clut_end);
    if (hit) {
      // keep it: the same data often comes back (see get_texture)
      auto old = m_stale.find(it->first);
      if (old != m_stale.end() && old->second.tex.handle >= 0) {
        ctr_gpu_tex_delete(old->second.tex.handle);
      }
      m_stale[it->first] = e;
      it = m_cache.erase(it);
    } else {
      ++it;
    }
  }
}

void CtrVram::clear_cache() {
  for (auto& [key, e] : m_cache) {
    if (e.tex.handle >= 0) {
      ctr_gpu_tex_delete(e.tex.handle);
    }
  }
  m_cache.clear();
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

u64 CtrVram::content_hash(const Entry& e, const Relocation* reloc) const {
  u64 h = 1469598103934665603ull;
  auto add_range = [&](u32 first, u32 end) {
    const u32 lo = std::min(first * kBlockBytes, kVramBytes);
    const u32 hi = std::min(end * kBlockBytes, kVramBytes);
    for (u32 o = lo; o + 8 <= hi; o += 8) {
      u64 v;
      memcpy(&v, m_vram.data() + o, 8);
      h = (h ^ v) * 1099511628211ull;
    }
  };
  add_range(e.first_block, e.end_block);
  add_range(e.clut_first, e.clut_end);
  if (reloc) {
    for (u32 c : reloc->clut) {
      h = (h ^ c) * 1099511628211ull;
    }
  }
  return h;
}
