#pragma once

/*!
 * @file CtrFineVis.h
 * (AI-assisted)
 * config.ini fine_vis: a level's finer precomputed visibility, its <level>.c3v sidecar
 * (c3v_format.h, made by tools/ctr_level_converter/ctr_vis_builder.cpp). The file's block index
 * is read at the level's load; its blocks of cells are read from the SD card as the camera comes
 * near them, by a loader thread of their own, into a fixed pool (config fine_vis_kb). The render
 * thread never waits for one: until the camera's block is there, nothing more is hidden.
 *
 * Per level draw (CtrTfragRenderer::draw_level, render thread): update() with the camera's
 * position gives the set of the cell holding it (decoded when the camera moves into another
 * cell), and per covered chunk what that set hides (chunk_hidden). See docs/3ds-port/
 * c3l_format.md, "Fine visibility (.c3v)".
 */

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "common/common_types.h"

#include "game/graphics/ctr/c3v_format.h"

class CtrFineVis : public std::enable_shared_from_this<CtrFineVis> {
 public:
  /*! At the level's load (any thread): path's .c3v if it was made from this .c3l (its chunk, draw
   * and run tables: c3v::fine_vis_level_hash; draw_count: draws[] entries, chunk_run_count: the
   * runs CtrLevels made per chunk). nullptr: none, or not usable (logged). */
  static std::shared_ptr<CtrFineVis> open(const std::string& path,
                                          const std::string& level,
                                          const c3l::Chunk* chunks,
                                          u32 num_chunks,
                                          const c3l::Draw* draws,
                                          u32 num_draws,
                                          const c3l::VisRun* runs,
                                          const u32* chunk_first_run,
                                          const u32* chunk_run_count);
  ~CtrFineVis();
  CtrFineVis(const CtrFineVis&) = delete;
  CtrFineVis& operator=(const CtrFineVis&) = delete;

  /*! Render thread, each level draw. usable: the level is drawn as the sets assume (else nothing
   * is asked for, and no set). The camera position in game units. Returns the bits of the camera
   * cell's set (bit u: unit u may be seen), nullptr: none this frame. */
  const u32* update(float x, float y, float z, bool usable, u64 frame);
  /*! The level is unloaded: blocks still queued are dropped (render thread). */
  void cancel();
  /*! Statistics since the last call, one log line (render thread). */
  void log_stats(const std::string& level);
  /*! Memory held (bytes): index, pool, decoded set, tables. */
  u32 bytes() const;

  c3v::Header hdr{};
  std::vector<u32> chunk_first_unit;  // per covered chunk (hdr.num_chunks), and the end
  std::vector<u8> chunk_runs;  // per covered chunk: its runs (chunk_first_run...) are its units
  // what the current set hides of each covered chunk: 0 nothing, 1 all of it, 2 some of its units
  std::vector<u8> chunk_hidden;

  // (the loader thread)
  struct Slot {
    u32 key = 0, off = 0, size = 0;
    u32 gen = 0;              // changes whenever the slot gets another block
    u64 used = 0;             // the last frame it was wanted
    std::atomic<u8> state{0};  // kFree, kLoading, kReady, kFailed
  };
  static constexpr u8 kFree = 0, kLoading = 1, kReady = 2, kFailed = 3;
  static constexpr int kSlots = 16;
  void load_slot(int s);  // loader thread
  std::atomic<bool> cancelled{false};

 private:
  CtrFineVis() = default;
  int find_block(u32 key) const;  // index entry, -1: no cells there
  int slot_of(u32 key) const;
  int request(u32 key, u64 frame);  // the slot holding / reading the block (-1: no room)
  void select();                    // chunk_hidden from bits

  std::string m_path;
  FILE* m_fp = nullptr;  // (read by the loader thread only, after open)
  std::vector<c3v::BlockEntry> m_index;
  std::vector<u8> m_pool;
  Slot m_slot[kSlots];
  std::vector<u32> m_bits;
  std::vector<u16> m_path_scratch;
  bool m_have = false;  // m_bits hold the set of (m_block_gen slot, m_cell)
  int m_have_slot = -1;
  u32 m_have_gen = 0, m_have_cell = ~0u;
  u32 m_last_cell = ~0u;  // the last cell key looked up (statistics)
  // statistics (render thread; reads: loader, atomics)
  u64 m_frames = 0, m_with_set = 0, m_no_cell = 0, m_waiting = 0, m_unusable = 0;
  u64 m_decodes = 0, m_cells_entered = 0, m_evicted = 0, m_no_room = 0;
  double m_decode_ms = 0, m_decode_max = 0, m_update_ms = 0;
 public:
  std::atomic<u64> st_reads{0}, st_bytes{0}, st_failed{0};
  std::atomic<u64> st_read_us{0}, st_read_max_us{0};
};
