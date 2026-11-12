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
 * written whole by a page upload has an id from its source (address in EE memory and a full
 * hash of the bytes copied), anything else a new number. A chunk upload whose id is already there isn't
 * written again, and a cached texture is valid while the ids of the chunks it was decoded from
 * are unchanged: no VRAM re-hashing, no cache scans when VRAM is written.
 */

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"

/*! (AI-assisted, j2-mem2) the KB of GS VRAM copy the renderer's CtrVram holds (memory log) */
unsigned ctr_vram_copy_kb();

struct CtrTexture {
  int handle = -1;  // ctr_gpu texture
  int w = 0, h = 0;  // size of the GPU texture (1 << tw, 1 << th, clamped to 8..1024)
  int max_alpha = 255;  // the largest texel alpha
};

class CtrVram {
 public:
  /*!
   * (AI-assisted, j2-mem2) sparse: the VRAM copy is kept per 16 KB chunk, made when something is
   * first written there (unwritten chunks read as zeros, as the whole copy did); uploads are
   * lazy, so only the areas something reads are written (Jak II ~1.4 MB, Jak 1 ~2.7 MB of the
   * 4). Else one 4 MB block. prewarm: Jak 1's boot pages and fonts decoded ahead (queue_prewarm);
   * Jak II (set_jak2_remap) queues its large font's first page.
   */
  explicit CtrVram(bool sparse = false, bool prewarm = false);
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
    m_upload_now = true;
    upload_texture_page(tpage, mode, ee_mem, s7_ptr);
    m_upload_now = false;
    flush_pending();
    // also chunks an identical earlier upload left lazy (that upload is skipped)
    materialize(m_page_first, m_page_end);
    queue_prewarm(tpage, ee_mem, s7_ptr);
  }
  /*!
   * Decode queued textures of the boot sprite and HUD pages ahead of their first draw, for up to
   * budget_ms (render thread, in a frame). The textures are the ones their first draw would make.
   */
  void prewarm(double budget_ms);
  /*!
   * A boot common page the game uploads only when it draws it (the start menu's): uploaded as the
   * game does and its textures decoded, for up to budget_ms a frame, once the queue above is done.
   * False: not all yet (call again next frame).
   */
  bool prewarm_page(const u8* tpage, int mode, const u8* ee_mem, u32 s7_ptr, double budget_ms);

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
  const CtrTexture* get_texture(u64 tex0, bool mips = false);
  // get_texture's mips: an entry of its own with mip levels (world sprites: minified without them
  // they miss the GPU's texture cache at nearly every pixel)
  static constexpr u64 kMipKeyBit = 1ull << 61;

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
   * (AI-assisted) hash: the upload's data hash (0 if none): another level's page loaded to the same
   * EE address.
   */
  const void* upload_source(u32 block, u64* hash = nullptr) const;

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
  /*! (AI-assisted, j2gaps) VRAM at tbp again (a borrowed texture is not deleted) */
  void clear_override(u32 tbp);
  /*! (AI-assisted) Jak II: textures whose dest the PC port moved (lookup_jak2_texture_dest_offset)
   * are read from where their page put them */
  // A counter that moves whenever texture page data in game memory can have changed (the
  // kernel's count of linked object files). While it stands, a page or page chunk uploaded again
  // from the same place keeps the hash its data gave: the data is not read again.
  void set_epoch(const u32* epoch) { m_epoch = epoch; }
  void set_jak2_remap(bool on) {
    m_jak2_remap = on;
    m_remap.assign(on ? 16384 : 0, 0);
    m_tex_ptr.assign(on ? 16384 : 0, 0);
  }

  void clear_cache();
  // Frees the decoded textures no lookup asked for since the last call: the ones kept aside go,
  // and every cached one is kept aside until its next lookup takes it back (no decode). The
  // relocated ones (the font pages) stay. Between frames (no texture of a frame in use).
  void trim();

  struct Stats {
    int uploads = 0;          // texture page uploads
    int uploads_changed = 0;  // writes to the VRAM copy
    int decoded = 0;
    int cached = 0;
    int relocates_skipped = 0;
    int revived = 0;
    int chunks_written = 0;  // (AI-assisted) 16 KB chunks of page uploads written to the VRAM copy
    int chunks_skipped = 0;  // (AI-assisted) and not written: the same data was there already
    int notes_cached = 0;    // page uploads whose texture notes came from the note cache
  };
  /*! (AI-assisted, j2-mem2) the bytes the VRAM copy holds (sparse: the chunks written so far) */
  u32 copy_bytes() const { return m_full_chunks * kChunkBytes + m_top_chunks * (kChunkBytes / 4); }
  /*! the chunks of the VRAM copy by what they hold (memory log) */
  std::string copy_text();
  const Stats& stats() const { return m_stats; }

 private:
  struct PendingUpload {
    const u8* src;
    u32 dest_block;
    u32 end_block;
    u32 words;
    u64 page_key;
  };
  static PendingUpload piece(const PendingUpload& p, u32 c0, u32 c1);
  // bytes = false: whole chunks only take their id, their bytes are written when read (lazy)
  void write_upload(const u8* src, u32 dest_block, u32 words, u64 page_key, bool bytes = true);
  // the TEX0 of each texture of a boot sprite / HUD page, as adgif-shader<-texture! makes it
  void queue_prewarm(const u8* tpage, const u8* ee_mem, u32 s7_ptr);
  std::vector<u64> m_prewarm;
  int m_prewarm_hold = 0;
  size_t m_prewarm_at = 0;
  int m_prewarm_decoded = 0;
  std::vector<u64> m_page_keys;  // prewarm_page's
  size_t m_page_at = 0;
  double m_prewarm_ms = 0;
  bool m_prewarming = false;
  void write_rows(const u8* src, u32 dest_block, u32 words);
  void write_ct32(const u8* data, u32 dest_block, u32 width, u32 height);
  bool ct32_same(const u8* data, u32 dest_block, u32 width, u32 height) const;  // (AI-assisted)
  // upload_ct32's images: a texture over one reads the image's rows only
  struct RawImage {
    u32 dest_block, end_block, width, height;
  };
  std::vector<RawImage> m_raw_images;
  int raw_rows(u64 data_tex0) const;
  void drop_raw_images(u32 first_block, u32 end_block);
  // write the pending uploads (all, or only those overlapping the given block ranges)
  void flush_pending();
  void flush_pending(u32 first_block, u32 end_block, bool bytes = true);
  // (AI-assisted) Lazy chunks: a whole chunk of a page upload whose id is current but whose bytes
  // are not written yet (their source and page). Textures are matched by chunk ids, so a page that
  // alternates with another in the same VRAM needs its bytes only when something decodes or reads
  // VRAM there; every byte read or partial write writes them first.
  const u8* m_lazy_src[256] = {};
  u64 m_lazy_key[256] = {};
  u32 m_lazy_n = 0;
  // per chunk: the last per-frame page upload over it (its frame, the chunk's source and page
  // key); frame 0: none, or uploaded "now" (its source may be gone)
  u32 m_frame = 1;
  bool m_upload_now = false;
  u32 m_up_frame[256] = {};
  const u8* m_up_src[256] = {};
  u64 m_up_key[256] = {};
  void note_page_upload(const u8* src, u32 dest_block, u32 words, u64 page_key, bool now);
  // Chunk memory given back (CtrVram.cpp drop_cold). Per chunk: the source and page key of the
  // page's chunk it holds (nullptr: something else), the frame its bytes were last read or
  // written in part, whether it waits for its source again (m_relazy; m_dropped: dropped while
  // its page was uploaded every frame)
  const u8* m_src[256] = {};
  u64 m_key[256] = {};
  u32 m_touch[256] = {};
  u8 m_dropped[256] = {}, m_relazy[256] = {};
  u32 m_drop_at = 0;
  u32 m_drops = 0, m_drop_back = 0, m_drop_lost = 0, m_drop_stopped = 0;
  bool m_page_write = false;  // a page's whole chunk is being written
  // host check (env CTR_VRAM_CHECK): the bytes dropped, compared when the chunk is read again
  std::unique_ptr<u8[]> m_shadow[256];
  u64 m_shadow_id[256] = {};
  u32 m_drop_checked = 0, m_drop_bad = 0;
  static bool drop_check();
  void free_chunk(u32 c, bool same_bytes);
  void drop_cold();
  u32 m_page_first = 0, m_page_end = 0;  // the last upload_texture_page's blocks
  void materialize(u32 first_block, u32 end_block);
  void materialize_all() { materialize(0, kChunkCount * 64); }
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
  bool m_prewarm_on = false;
  std::unique_ptr<u8[]> m_mem;  // not sparse: all chunks
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
    // a texel's place in its page by x + page width * y, for PSMCT32, 16, T8, T4 (half bytes)
    const u16* const* page_tab;
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
  View view() const { return View{m_rd, m_is_top, m_page_tab_ptr}; }

 private:
  std::vector<u16> m_page_tab[4];
  const u16* m_page_tab_ptr[4] = {};
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
  // textures whose GPU texture couldn't be made: the time (ctr_gpu_time_ms) of the next try
  std::unordered_map<u64, double> m_failed;
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
  // a page upload's sampled hash, kept while *m_epoch stands (set_epoch)
  struct HashMemo {
    const u8* src = nullptr;
    u32 words = 0;
    u32 epoch = 0;
    u64 key = 0;
    u64 hash = 0;
  };
  static constexpr u32 kHashMemos = 256;
  HashMemo m_hash_memo[kHashMemos];
  const u32* m_epoch = nullptr;
  // a page chunk's id (page_chunk_id's sampled hash), kept the same way
  struct ChunkMemo {
    const u8* src = nullptr;
    u32 epoch = 0;
    u64 key = 0;
    u64 id = 0;
  };
  static constexpr u32 kChunkMemos = 1024;
  ChunkMemo m_chunk_memo[kChunkMemos];
  u64 chunk_id(const u8* chunk_src, u64 page_key);
  // Jak II: a page's dest offsets (lookup_jak2_texture_dest_offset) by texture index, by page id
  std::unordered_map<u32, std::vector<u8>> m_page_offsets;
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
  // a page's notes (its textures' entries) while *m_epoch stands: a page uploaded again after
  // another one took its blocks (Jak II alternates pages every frame) writes them without
  // reading its texture records from EE memory again
  struct NoteEntry {
    u16 dest0;
    u8 remap;
    TexInfo info;
    u32 ptr;
  };
  struct NoteCache {
    u32 epoch = 0;
    s32 count = -1;
    bool keep = false;  // not a font page (setup-font-texture changes its records)
    std::vector<NoteEntry> e;
  };
  std::unordered_map<const u8*, NoteCache> m_note_cache;

  struct Relocation {
    u32 psm;
    std::vector<u32> clut;  // RGBA8
  };
  std::unordered_map<u32, std::vector<Relocation>> m_relocations;  // by dest block
  const Relocation* find_relocation(u32 block, u32 psm) const;
  Stats m_stats;
};
