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
#include <unordered_map>
#include <vector>

#include "common/common_types.h"

struct CtrTexture {
  int handle = -1;  // ctr_gpu texture
  int w = 0, h = 0;  // size of the GPU texture (1 << tw, 1 << th, clamped to 8..1024)
};

class CtrVram {
 public:
  CtrVram();
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

  std::vector<u8> m_vram;
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
