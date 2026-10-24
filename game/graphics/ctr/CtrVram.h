#pragma once

/*!
 * @file CtrVram.h
 * (AI-assisted)
 * GS VRAM emulation for the 3DS renderer.
 *
 * The PC renderer maps VRAM addresses to textures that were converted ahead of time and stored in
 * the .fr3 level files. The 3DS renderer instead keeps a copy of the 4 MB GS VRAM: texture page
 * uploads from the game are written to it exactly like the PS2 DMA transfer would, and textures
 * are decoded from it the first time they are drawn, using the TEX0 register the game sets
 * (address, format, CLUT). That handles the font (PSMT4HH/HL stored on top of other data), CLUT
 * formats and anything else the direct renderer draws, without per-texture metadata.
 *
 * Decoded textures are cached per TEX0 value and dropped when the VRAM they were read from
 * changes.
 *
 * Texture page uploads are lazy: the game uploads the tfrag, shrub, tie, ... pages to the same
 * VRAM area every frame, but those renderers take their textures from the .c3l files. An upload
 * is only recorded (and replaces a pending upload to the same area); it is written to the VRAM
 * copy when something reads VRAM that it overlaps.
 *
 * (AI-assisted) What VRAM holds is tracked per 16 KB chunk (the game's upload unit): a chunk
 * written whole by a page upload has an id from its source (address in EE memory and a sampled
 * hash of the bytes copied), anything else a new number. A chunk upload whose id is already there isn't
 * written again, and a cached texture is valid while the ids of the chunks it was decoded from
 * are unchanged: no VRAM re-hashing, no cache scans when VRAM is written.
 */

#include <cstdint>
#include <cstring>
#include <memory>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"

/*! (AI-assisted, j2-mem2) the KB of GS VRAM copy the renderer's CtrVram holds (memory log) */
unsigned ctr_vram_copy_kb();

struct CtrTexture {
  int handle = -1;  // ctr_gpu texture
  int w = 0, h = 0;  // size of the GPU texture (1 << tw, 1 << th, clamped to 8..1024)
};

class CtrVram {
 public:
  /*!
   * (AI-assisted, j2-mem2) sparse: the VRAM copy is kept per 16 KB chunk, made when something is
   * first written there (unwritten chunks read as zeros, as the whole copy did); Jak II only
   * writes the areas something reads (uploads are lazy), ~a third of the 4 MB. Else (Jak 1) one
   * 4 MB block, as before.
   */
  explicit CtrVram(bool sparse = false);
  ~CtrVram();

  /*!
   * Emulate the upload of a GOAL texture-page (texture-upload-now / PC_PORT upload tags).
   * mode: -1 = whole page, 0 = segment 0, -2 = segments 0 and 1, 2 = segment 2.
   */
  void upload_texture_page(const u8* tpage, int mode, const u8* ee_mem, u32 s7_ptr);
  /*!
   * The same, written to the VRAM copy right away (texture-upload-now): the game may reuse the
   * source memory afterwards (the common textures are uploaded once, at boot).
   */
  void upload_texture_page_now(const u8* tpage, int mode, const u8* ee_mem, u32 s7_ptr) {
    upload_texture_page(tpage, mode, ee_mem, s7_ptr);
    flush_pending();
  }

  /*!
   * Emulate the VRAM to VRAM copy of texture-relocate (the font is moved into the upper bits of
   * the depth buffer with a format change). dest / src are block addresses, src must be the
   * mip 0 address of a texture from an uploaded page. Also snapshots the texture's CLUT: the game
   * copies it to a location that __pc-texture-relocate doesn't tell us.
   */
  void relocate(u32 dest_block, u32 src_block, u32 dest_psm);

  /*!
   * Emulate a PSMCT32 image transfer (BITBLTBUF with DBW = width / 64) at block address dest
   * (written right away).
   */
  void upload_ct32(const u8* data, u32 dest_block, u32 width, u32 height);

  /*!
   * Get the texture for a TEX0 register value, decoding it from VRAM if needed.
   * Returns nullptr if the format is not supported.
   */
  const CtrTexture* get_texture(u64 tex0);

  /*!
   * Decode a texture to RGBA8 (row-major, top row first) without creating a GPU texture.
   * Used by tests. Returns false for unsupported formats.
   */
  bool decode(u64 tex0, std::vector<u32>* out, int* w, int* h) const;
  /*!
   * The same for the renderer (eyes): only writes the pending uploads the texture reads, and uses
   * the CLUT a relocated texture was moved with (like get_texture).
   */
  bool decode_for_cpu(u64 tex0, std::vector<u32>* out, int* w, int* h);
  /*!
   * (AI-assisted) The source data of the texture page upload that last wrote this block (nullptr if
   * none): tells apart two textures at the same VRAM address (two levels' pages, CtrSky's cache).
   */
  const void* upload_source(u32 block) const;

  /*!
   * (AI-assisted, Jak II texture animation) A texture made on the CPU (CtrTexAnimJak2) that
   * replaces whatever VRAM holds at block tbp, for every TEX0 there (the PC's texture pool gives
   * its texture by address too): rgba is w x h RGBA8 in the GS's scale (0x80 = 1). Updated in
   * place when the size is the same.
   */
  void set_override(u32 tbp, int w, int h, const u32* rgba);
  bool has_override(u32 tbp) const { return m_overrides.count(tbp) != 0; }
  /*! (AI-assisted) The same with a texture made elsewhere (the minimap's render to texture),
   * not deleted here */
  void set_override_handle(u32 tbp, int handle, int w, int h) {
    auto& t = m_overrides[tbp];
    t.handle = handle;
    t.w = w;
    t.h = h;
    m_borrowed[tbp] = true;
  }
  /*! (AI-assisted) Jak II: textures whose dest the PC port moved (lookup_jak2_texture_dest_offset)
   * are read from where their page put them */
  void set_jak2_remap(bool on) {
    m_jak2_remap = on;
    m_remap.assign(on ? 16384 : 0, 0);
    m_tex_ptr.assign(on ? 16384 : 0, 0);
  }

  void clear_cache();
  // (AI-assisted) drop the cached textures ctr_button_art::patch may change
  void drop_art_textures();

  struct Stats {
    int uploads = 0;          // texture page uploads
    int uploads_changed = 0;  // writes to the VRAM copy
    int decoded = 0;
    int cached = 0;
    int relocates_skipped = 0;
    int revived = 0;
    int chunks_written = 0;  // (AI-assisted) 16 KB chunks of page uploads written to the VRAM copy
    int chunks_skipped = 0;  // (AI-assisted) and not written: the same data was there already
  };
  /*! (AI-assisted, j2-mem2) the bytes the VRAM copy holds (sparse: the chunks written so far) */
  u32 copy_bytes() const { return m_full_chunks * kChunkBytes + m_top_chunks * (kChunkBytes / 4); }
  const Stats& stats() const { return m_stats; }

 private:
  struct PendingUpload {
    const u8* src;
    u32 dest_block;
    u32 end_block;
    u32 words;
  };
  static PendingUpload piece(const PendingUpload& p, u32 c0, u32 c1);
  void write_upload(const u8* src, u32 dest_block, u32 words);
  void write_rows(const u8* src, u32 dest_block, u32 words);
  void write_ct32(const u8* data, u32 dest_block, u32 width, u32 height);
  bool ct32_same(const u8* data, u32 dest_block, u32 width, u32 height) const;  // (AI-assisted)
  // write the pending uploads (all, or only those overlapping the given block ranges)
  void flush_pending();
  void flush_pending(u32 first_block, u32 end_block);
  // (AI-assisted) drop the chunks of pending uploads that [first_block, end_block) overwrites
  void trim_pending(u32 first_block, u32 end_block);
  struct RelocateRecord {
    u64 sig;                     // source, CLUT and destination format
    u32 first_block, end_block;  // what it wrote
  };
  std::unordered_map<u64, RelocateRecord> m_last_relocate;  // by (dest, psm)
  bool pending_overlaps(u32 first_block, u32 end_block) const;
  std::vector<PendingUpload> m_pending;
  std::vector<PendingUpload> m_keep, m_write;  // (AI-assisted) scratch: no allocations per flush
  bool m_ct32_pairs = false;  // (AI-assisted) see the constructor
  bool m_ct32_lines = false;
  u32 read32(u32 byte_addr) const;
  u32 clut_color(u32 cbp, u32 cpsm, u32 entry) const;

  // (AI-assisted, j2-mem2) The VRAM copy, by 16 KB chunk (kChunkBytes; a chunk's bytes are the
  // GS addresses [c * 16 KB, (c + 1) * 16 KB): 64 blocks, two PSMCT32 pages). Every access is
  // within one chunk (aligned to its size). A chunk is
  //  - full: m_wr / m_rd point to its 16 KB (m_rd to a shared chunk of zeros while it doesn't
  //    exist: m_wr nullptr, wr() makes it);
  //  - top bytes only (m_top set, sparse only): written only by relocate() into PSMT8H / 4HH /
  //    4HL, which change the top byte of each word (the font: 110 chunks in Jak II), so the
  //    other three bytes are zero: 4 KB, byte 3 of each word. Made full (wr()) when anything
  //    else writes there.
  static constexpr u32 kChunkBytes = 16384;
  const u8* m_rd[256];
  u8* m_wr[256];
  u8* m_top[256];
  u8 m_is_top[256];
  bool m_sparse = false;
  std::unique_ptr<u8[]> m_mem;  // not sparse (Jak 1): all chunks
  std::unique_ptr<u8[]> m_own[256];  // sparse: the chunk's memory
  u32 m_full_chunks = 0, m_top_chunks = 0;
  u8* wr(u32 addr) {
    u8* c = m_wr[addr >> 14];
    if (!c) {
      c = make_full(addr >> 14);
    }
    return c + (addr & (kChunkBytes - 1));
  }
  // the top byte (byte 3) of the word at addr (4 aligned)
  u8* wr_top(u32 addr) {
    const u32 c = addr >> 14;
    if (u8* f = m_wr[c]) {
      return f + (addr & (kChunkBytes - 1)) + 3;
    }
    u8* t = m_top[c] ? m_top[c] : make_top(c);
    return t + ((addr & (kChunkBytes - 1)) >> 2);
  }
  u8* make_full(u32 chunk);
  u8* make_top(u32 chunk);
  void note_chunks() const;

 public:
  // (AI-assisted, j2-mem2) read access (CtrVram.cpp decode_impl); a, 4 / 2 aligned for rd32 /
  // rd16, in [0, 4 MB)
  struct View {
    const u8* const* chunks;
    const u8* is_top;
    u32 rd32(u32 a) const {
      const u32 c = a >> 14;
      if (is_top[c]) {
        return (u32)chunks[c][(a & 16383u) >> 2] << 24;
      }
      u32 v;
      memcpy(&v, chunks[c] + (a & 16383u), 4);
      return v;
    }
    u16 rd16(u32 a) const {
      const u32 c = a >> 14;
      if (is_top[c]) {
        return (a & 2) ? (u16)(chunks[c][(a & 16383u) >> 2] << 8) : 0;
      }
      u16 v;
      memcpy(&v, chunks[c] + (a & 16383u), 2);
      return v;
    }
    u8 rd8(u32 a) const {
      const u32 c = a >> 14;
      if (is_top[c]) {
        return (a & 3) == 3 ? chunks[c][(a & 16383u) >> 2] : 0;
      }
      return chunks[c][a & 16383u];
    }
  };
  View view() const { return View{m_rd, m_is_top}; }

 private:
  std::unordered_map<u32, CtrTexture> m_overrides;  // (AI-assisted) see set_override
  std::unordered_map<u32, bool> m_borrowed;  // (AI-assisted) set_override_handle's
  bool m_jak2_remap = false;
  std::vector<u8> m_remap;  // (AI-assisted) by TEX0 tbp: blocks the PC port moved the texture by
  std::vector<u32> m_tex_ptr;  // (AI-assisted, Jak II) by dest: the texture record's EE address
  const u8* m_ee_mem = nullptr;
  u64 data_tex0(u64 tex0) const {
    if (!m_jak2_remap) {
      return tex0;
    }
    const u32 off = m_remap[tex0 & 0x3fff];
    return off <= (tex0 & 0x3fff) ? tex0 - off : tex0;
  }
  u32 m_art_gen = 0;  // (AI-assisted) ctr_button_art::generation() of the cached textures
  // (AI-assisted) per 16 KB chunk: what it holds (see the top of this file). Page chunks have odd
  // ids, all others even ones from m_next_unique_id.
  static constexpr u32 kChunkCount = 256;
  u64 m_chunk_id[kChunkCount] = {};
  u64 m_next_unique_id = 2;
  void mark_unique(u32 first_block, u32 end_block);
  // after a write: the counters, and relocations over it must be redone
  void note_write(u32 first_block, u32 end_block);
  u64 mix_chunk_ids(u64 h, u32 first_block, u32 end_block) const;
  struct Entry {
    CtrTexture tex;
    u32 first_block, end_block;  // VRAM range the texture (and its CLUT) was read from
    u32 clut_first, clut_end;
    u64 sig = 0;  // (AI-assisted) the ids of the chunks it was decoded from (entry_sig)
  };
  u64 entry_sig(const Entry& e) const;
  std::unordered_map<u64, Entry> m_cache;
  // (AI-assisted) A direct-mapped cache in front of m_cache, for the textures drawn every frame:
  // the hash table's lookup takes the bucket modulo (a library divide on the ARM11, ~tens of
  // cycles) per lookup and per node it passes. Slots are valid while gen == m_cache_gen, which
  // changes whenever an m_cache entry is erased.
  struct FrontSlot {
    u64 key = 0;
    Entry* entry = nullptr;
    u32 gen = 0;
  };
  static constexpr int kFrontSlotsLog2 = 6;
  FrontSlot m_front[1 << kFrontSlotsLog2];
  u32 m_cache_gen = 1;
  Entry* find_cached(u64 key);
  // invalidated textures, kept (with their GPU texture) in case the same data comes back
  std::unordered_map<u64, Entry> m_stale;
  void clear_stale();
  void keep_stale(u64 key, const Entry& e);
  // (AI-assisted) entries are checked when they are looked up: once the cache has grown this far,
  // the ones whose VRAM changed move to m_stale (bounded), so they don't keep GPU memory
  size_t m_sweep_at = 256;
  void sweep_outdated();

  // last upload per destination block: the game re-uploads the same pages every frame
  struct UploadRecord {
    const u8* src;
    u32 words;
    u64 hash;
  };
  std::unordered_map<u32, UploadRecord> m_last_upload;
  // drop the upload records overlapping these blocks (their data is being overwritten)
  void forget_uploads(u32 first_block, u32 end_block);
  std::vector<u32> m_ct32_page_table;  // byte offset in a PSMCT32 page, by x + 64 * y

  struct TexInfo {
    u16 w = 0, h = 0;  // 0: no texture starts at this block
    u8 psm = 0;
    u8 width = 0;  // in 64 pixel units
    u16 clutpsm = 0;
    u16 clutdest = 0;
  };
  // by mip 0 block address. (AI-assisted) An array (160 KB): every changed page upload writes the
  // entries of all its textures, which a hash map made a large part of the "tex" buckets' time
  std::vector<TexInfo> m_tex_info;
  void note_tex_info(const u8* tpage, s32 count, const u8* ee_mem, u32 s7_ptr);

  struct Relocation {
    u32 psm;
    std::vector<u32> clut;  // RGBA8
  };
  std::unordered_map<u32, std::vector<Relocation>> m_relocations;  // by dest block
  const Relocation* find_relocation(u32 block, u32 psm) const;
  Stats m_stats;
};
