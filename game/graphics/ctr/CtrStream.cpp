/*!
 * @file CtrStream.cpp
 * (AI-assisted, streaming)
 * See CtrStream.h.
 */

#include "CtrStream.h"

#include <algorithm>

#include "common/log/log.h"

#include "game/graphics/ctr/ctr_gpu.h"

CtrStreamArena& CtrStreamArena::get() {
  static CtrStreamArena a;
  return a;
}

bool CtrStreamArena::init(u32 bytes) {
  std::lock_guard<std::mutex> lk(m_lock);
  if (m_ready || m_tried) {
    return m_ready;
  }
  m_tried = true;
  bytes = (bytes + kAlign - 1) & ~(kAlign - 1);
  u8* base = (u8*)ctr_gpu_linear_block(bytes);
  if (!base) {
    lg::error("[ctr] stream arena: no {} KB of linear memory: the near tie versions are not "
              "streamed",
              bytes / 1024);
    return false;
  }
  Seg s;
  s.base = base;
  s.size = bytes;
  s.free[0] = bytes;
  m_segs.push_back(std::move(s));
  m_capacity = m_peak_capacity = bytes;
  m_ready = true;
  ctr_gpu_set_ext_free(&CtrStreamArena::free_cb);
  lg::info("[ctr] stream arena: {} KB of linear memory for the near tie versions", bytes / 1024);
  return true;
}

u32 CtrStreamArena::mesh_bytes(u32 vertex_count, u32 index_count) {
  return (ctr_gpu_ext_index_offset((int)vertex_count) + index_count * 2u + kAlign - 1) &
         ~(kAlign - 1);
}

void* CtrStreamArena::alloc(u32 bytes) {
  bytes = (std::max<u32>(bytes, kAlign) + kAlign - 1) & ~(kAlign - 1);
  std::lock_guard<std::mutex> lk(m_lock);
  m_allocs++;
  // first fit, the first segment first (the pieces are 10-300 KB)
  for (auto& s : m_segs) {
    for (auto it = s.free.begin(); it != s.free.end(); ++it) {
      if (it->second < bytes) {
        continue;
      }
      const u32 off = it->first, size = it->second;
      s.free.erase(it);
      if (size > bytes) {
        s.free[off + bytes] = size - bytes;
      }
      s.used[off] = bytes;
      s.used_bytes += bytes;
      m_used_bytes += bytes;
      m_peak = std::max(m_peak, m_used_bytes);
      return s.base + off;
    }
  }
  m_failed++;
  return nullptr;
}

CtrStreamArena::Seg* CtrStreamArena::seg_of(const void* p) {
  for (auto& s : m_segs) {
    if ((const u8*)p >= s.base && (const u8*)p < s.base + s.size) {
      return &s;
    }
  }
  return nullptr;
}

void CtrStreamArena::free(void* p) {
  if (!p) {
    return;
  }
  std::lock_guard<std::mutex> lk(m_lock);
  Seg* s = seg_of(p);
  if (!s) {
    lg::error("[ctr] stream arena: freeing a piece it doesn't hold");
    return;
  }
  const u32 off = (u32)((u8*)p - s->base);
  auto u = s->used.find(off);
  if (u == s->used.end()) {
    lg::error("[ctr] stream arena: freeing a piece twice");
    return;
  }
  u32 start = off, size = u->second;
  s->used.erase(u);
  s->pending.erase(off);
  s->used_bytes -= size;
  m_used_bytes -= size;
  // coalesce with the free neighbours
  auto next = s->free.lower_bound(start);
  if (next != s->free.end() && next->first == start + size) {
    size += next->second;
    next = s->free.erase(next);
  }
  if (next != s->free.begin()) {
    auto prev = std::prev(next);
    if (prev->first + prev->second == start) {
      start = prev->first;
      size += prev->second;
      s->free.erase(prev);
    }
  }
  s->free[start] = size;
}

void CtrStreamArena::mark_pending(void* p) {
  std::lock_guard<std::mutex> lk(m_lock);
  Seg* s = p ? seg_of(p) : nullptr;
  if (!s) {
    return;
  }
  const u32 off = (u32)((u8*)p - s->base);
  auto u = s->used.find(off);
  if (u != s->used.end()) {
    s->pending[off] = u->second;
  }
}

u32 CtrStreamArena::run_if_freed(void* p) {
  std::lock_guard<std::mutex> lk(m_lock);
  Seg* s = p ? seg_of(p) : nullptr;
  if (!s) {
    return 0;
  }
  const u32 off = (u32)((u8*)p - s->base);
  auto u = s->used.find(off);
  if (u == s->used.end()) {
    return 0;
  }
  // the pieces before and after it, free or pending, one after the other
  auto size_at = [&](u32 o, u32* sz) {
    auto f = s->free.find(o);
    if (f != s->free.end()) {
      *sz = f->second;
      return true;
    }
    auto q = s->pending.find(o);
    if (q != s->pending.end()) {
      *sz = q->second;
      return true;
    }
    return false;
  };
  u32 run = u->second, end = off + u->second, sz = 0;
  while (end < s->size && size_at(end, &sz)) {
    run += sz;
    end += sz;
  }
  u32 start = off;
  while (start > 0) {
    // the piece ending at start: the free map's or the used (pending) one's
    bool found = false;
    auto f = s->free.lower_bound(start);
    if (f != s->free.begin()) {
      auto pf = std::prev(f);
      if (pf->first + pf->second == start) {
        run += pf->second;
        start = pf->first;
        found = true;
      }
    }
    if (!found) {
      auto q = s->pending.lower_bound(start);
      if (q != s->pending.begin()) {
        auto pq = std::prev(q);
        if (pq->first + pq->second == start) {
          run += pq->second;
          start = pq->first;
          found = true;
        }
      }
    }
    if (!found) {
      break;
    }
  }
  return run;
}

void CtrStreamArena::free_cb(void* mem) {
  get().free(mem);
}

bool CtrStreamArena::grow(u32 bytes, u32 reserve) {
  bytes = std::max<u32>((bytes + kAlign - 1) & ~(kAlign - 1), kSegment);
  if (!m_ready || ctr_gpu_linear_free() < (u64)bytes + reserve) {
    return false;
  }
  u8* base = (u8*)ctr_gpu_linear_block(bytes);
  if (!base) {
    return false;
  }
  std::lock_guard<std::mutex> lk(m_lock);
  Seg s;
  s.base = base;
  s.size = bytes;
  s.free[0] = bytes;
  m_segs.push_back(std::move(s));
  m_capacity += bytes;
  m_peak_capacity = std::max(m_peak_capacity, m_capacity);
  m_grown++;
  return true;
}

u32 CtrStreamArena::release_empty() {
  std::vector<u8*> gone;
  u32 bytes = 0;
  {
    std::lock_guard<std::mutex> lk(m_lock);
    for (size_t i = 1; i < m_segs.size();) {
      if (m_segs[i].used.empty()) {
        gone.push_back(m_segs[i].base);
        bytes += m_segs[i].size;
        m_capacity -= m_segs[i].size;
        m_segs.erase(m_segs.begin() + (long)i);
        m_released++;
      } else {
        i++;
      }
    }
  }
  for (u8* p : gone) {
    ctr_gpu_linear_block_free(p);
  }
  return bytes;
}

bool CtrStreamArena::in_growth(const void* p) {
  std::lock_guard<std::mutex> lk(m_lock);
  for (size_t i = 1; i < m_segs.size(); i++) {
    if ((const u8*)p >= m_segs[i].base && (const u8*)p < m_segs[i].base + m_segs[i].size) {
      return true;
    }
  }
  return false;
}

CtrStreamArena::Stats CtrStreamArena::stats() {
  std::lock_guard<std::mutex> lk(m_lock);
  Stats st;
  st.capacity = m_capacity;
  st.growth = m_capacity - (m_segs.empty() ? 0 : m_segs[0].size);
  st.peak_capacity = m_peak_capacity;
  st.used = m_used_bytes;
  st.peak = m_peak;
  st.segments = (u32)m_segs.size();
  st.allocs = m_allocs;
  st.failed = m_failed;
  st.grown = m_grown;
  st.released = m_released;
  for (const auto& s : m_segs) {
    st.pieces += (u32)s.used.size();
    for (const auto& [o, n] : s.free) {
      st.largest_free = std::max(st.largest_free, n);
    }
  }
  return st;
}
