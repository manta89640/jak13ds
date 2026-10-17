#pragma once

/*!
 * @file CtrStream.h
 * (AI-assisted, streaming)
 * Geometry streaming of c3l v19 levels (docs/3ds-port/streaming.md): the stream arena, the linear
 * memory the near chunks' meshes are made in (CtrLevels reads them from the file around the camera
 * and frees the far ones), so their comings and goings never cut the linear heap into pieces the
 * level loads need whole (texture pools, meshes).
 *
 * Its first segment (config stream_kb) is taken at the first level that streams and kept. Growth
 * segments (at least kSegment) are taken while the units needed don't fit and the linear heap keeps
 * its reserve (config stream_reserve_kb) free after them, and given back when empty, or emptied
 * when a level load needs the memory (CtrLevels::housekeeping).
 */

#include <cstdint>
#include <map>
#include <mutex>
#include <vector>

#include "common/common_types.h"

class CtrStreamArena {
 public:
  static CtrStreamArena& get();
  // the first segment (bytes, once; later calls keep it). false: no linear memory for it
  bool init(u32 bytes);
  bool ready() const { return m_ready; }
  // a piece of `bytes` (kAlign aligned) in any segment, nullptr when none has a free piece that
  // big. Any thread.
  void* alloc(u32 bytes);
  // a piece back (any thread): ctr_gpu calls it (ctr_gpu_set_ext_free) once the GPU is done with
  // a deleted mesh's memory
  void free(void* p);
  static void free_cb(void* mem);
  // another segment of at least `bytes` (and kSegment) when the linear heap keeps `reserve` bytes
  // free after it (render thread)
  bool grow(u32 bytes, u32 reserve);
  // the empty growth segments back to the linear heap (render thread, or the thread of
  // CtrLevels::service; no frame being built). Returns their bytes.
  u32 release_empty();
  // in a growth segment (those are emptied first); any growth segment
  bool in_growth(const void* p);
  bool has_growth() {
    std::lock_guard<std::mutex> lk(m_lock);
    return m_segs.size() > 1;
  }
  // the bytes of a mesh's piece: its vertices, then its indices (ctr_gpu_mesh_begin_ext)
  static u32 mesh_bytes(u32 vertex_count, u32 index_count);
  struct Stats {
    u32 capacity = 0, growth = 0, used = 0, peak = 0, largest_free = 0, pieces = 0, segments = 0;
    u32 peak_capacity = 0;
    u64 allocs = 0, failed = 0, grown = 0, released = 0;
  };
  Stats stats();
  static constexpr u32 kAlign = 128;
  static constexpr u32 kSegment = 1024 * 1024;

 private:
  struct Seg {
    u8* base = nullptr;
    u32 size = 0;
    std::map<u32, u32> free;  // offset -> size (coalesced)
    std::map<u32, u32> used;  // offset -> size
    u32 used_bytes = 0;
  };
  Seg* seg_of(const void* p);
  std::mutex m_lock;
  std::vector<Seg> m_segs;  // [0]: the first segment
  bool m_ready = false, m_tried = false;
  u32 m_used_bytes = 0, m_peak = 0, m_capacity = 0, m_peak_capacity = 0;
  u64 m_allocs = 0, m_failed = 0, m_grown = 0, m_released = 0;
};
