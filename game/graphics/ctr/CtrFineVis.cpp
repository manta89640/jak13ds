/*!
 * @file CtrFineVis.cpp
 * (AI-assisted)
 * config.ini fine_vis: the .c3v sidecars, their blocks read on demand (see CtrFineVis.h).
 */

#include "CtrFineVis.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <new>

#include "common/log/log.h"

#include "game/graphics/ctr/CtrSettings.h"
#include "game/graphics/ctr/ctr_gpu.h"

#ifdef __3DS__
#include "platform/3ds/port/ctr_port.h"
#else
#include <thread>
#endif

namespace {

// the blocks within this distance of the camera are read ahead of time (game units: 4 m; at a
// fast 20 m/s the camera crosses it in 0.2 s, a block read takes ~6 ms in Azahar)
constexpr float kPrefetch = 4.f * 4096.f;
// below the render thread (0x2F) and the sound mixer (0x2E); above the level loader (0x3C), whose
// jobs take seconds while a block takes a few ms of SD card time
constexpr int kLoaderPrio = 0x3B;

struct Loader {
  std::mutex lock;
  std::condition_variable cv;
  std::deque<std::pair<std::shared_ptr<CtrFineVis>, int>> queue;
  bool started = false;
  bool failed = false;
};

Loader& loader() {
  static Loader* l = new Loader();  // (never destroyed: the thread may outlive static destructors)
  return *l;
}

void* loader_main(void*) {
#ifdef __3DS__
  ctr_thread_install_crash_handler();
#endif
  Loader& L = loader();
  for (;;) {
    std::shared_ptr<CtrFineVis> fv;
    int slot = -1;
    {
      std::unique_lock<std::mutex> lk(L.lock);
      L.cv.wait(lk, [&] { return !L.queue.empty(); });
      fv = std::move(L.queue.front().first);
      slot = L.queue.front().second;
      L.queue.pop_front();
    }
    fv->load_slot(slot);
  }
  return nullptr;
}

// the loader thread, at the first block asked for (false: none, blocks are never read)
bool start_loader() {
  Loader& L = loader();
  std::lock_guard<std::mutex> lk(L.lock);
  if (L.started || L.failed) {
    return L.started;
  }
#ifdef __3DS__
  void* handle = nullptr;
  // core 2 on New 3DS, with the level loader (ctr_thread_create falls back to core 0)
  if (ctr_thread_create(&loader_main, nullptr, 32 * 1024, kLoaderPrio, 2, &handle) != 0) {
    L.failed = true;
    lg::warn("[ctr] fine vis: no loader thread: no blocks are read");
    return false;
  }
#else
  std::thread(&loader_main, nullptr).detach();
#endif
  L.started = true;
  return true;
}

}  // namespace

std::shared_ptr<CtrFineVis> CtrFineVis::open(const std::string& path,
                                             const std::string& level,
                                             const c3l::Chunk* chunks,
                                             u32 num_chunks,
                                             const c3l::Draw* draws,
                                             u32 num_draws,
                                             const c3l::VisRun* runs,
                                             const u32* chunk_first_run,
                                             const u32* chunk_run_count) {
  FILE* fp = fopen(path.c_str(), "rb");
  if (!fp) {
    return nullptr;  // (none: the level draws as without it)
  }
  std::shared_ptr<CtrFineVis> fv;
  const char* why = nullptr;
  try {
    fv.reset(new CtrFineVis());
    auto& h = fv->hdr;
    const u32 covered = c3v::covered_chunks(chunks, num_chunks);
    long file_size = -1;
    if (fseek(fp, 0, SEEK_END) == 0) {
      file_size = ftell(fp);
    }
    if (fseek(fp, 0, SEEK_SET) != 0 || fread(&h, sizeof(h), 1, fp) != 1 ||
        memcmp(h.magic, c3v::kMagic, 4)) {
      why = "not a C3V file";
    } else if (h.version != c3v::kVersion) {
      why = "another C3V version (run ctr_vis_builder on this .c3l)";
    } else if (h.num_chunks != covered ||
               h.level_hash != c3v::fine_vis_level_hash(chunks, covered, draws, num_draws, runs,
                                                        chunk_first_run)) {
      why = "made from another conversion of the level (run ctr_vis_builder on this .c3l)";
    } else if (h.set_words != (h.num_units + 31) / 32 || h.cell <= 0 || h.block < 2 ||
               h.block > 32 || h.bnx == 0 || h.bny == 0 || h.bnz == 0 ||
               h.nx != h.bnx * h.block || h.ny != h.bny * h.block || h.nz != h.bnz * h.block ||
               (u64)h.bnx * h.bny * h.bnz >= 0xffffffffull || h.num_blocks == 0 ||
               h.max_block > 0xffff || h.max_sets == 0 || h.max_sets > 0xffff) {
      why = "bad header";
    } else {
      fv->m_index.resize(h.num_blocks + 1);
      if (fseek(fp, (long)h.index_offset, SEEK_SET) != 0 ||
          fread(fv->m_index.data(), sizeof(c3v::BlockEntry), fv->m_index.size(), fp) !=
              fv->m_index.size()) {
        why = "truncated";
      } else {
        for (size_t i = 0; i < h.num_blocks && !why; i++) {
          const auto& a = fv->m_index[i];
          const auto& b = fv->m_index[i + 1];
          if ((i + 1 < h.num_blocks && b.key <= a.key) || b.offset < a.offset ||
              b.offset - a.offset > h.max_block || (u64)a.key >= (u64)h.bnx * h.bny * h.bnz) {
            why = "bad block index";
          }
        }
        if (!why && file_size >= 0 && fv->m_index.back().offset > (u64)file_size) {
          why = "truncated";
        }
      }
    }
    if (!why) {
      // the units: each chunk's, in order (c3v_format.h); a chunk's runs are its units when there
      // are as many (the file's runs, or run_culling's pieces)
      fv->chunk_first_unit.resize(covered + 1);
      fv->chunk_runs.resize(covered);
      u32 u = 0;
      for (u32 ci = 0; ci < covered; ci++) {
        const auto& ch = chunks[ci];
        fv->chunk_first_unit[ci] = u;
        const u32 n = c3v::chunk_units(ch, draws, num_draws);
        const u32 nr = ch.vis_run_count ? ch.vis_run_count : chunk_run_count[ci];
        fv->chunk_runs[ci] = n > 0 && nr == n;
        u += n;
      }
      fv->chunk_first_unit[covered] = u;
      if (u != h.num_units) {
        why = "unit count differs";
      }
    }
    if (!why) {
      const u32 kb = (u32)std::clamp(ctr_settings().fine_vis_kb, 16, 1024);
      fv->m_pool.resize(std::max<u32>(kb * 1024, h.max_block));
      fv->m_bits.assign(h.set_words, 0);
      fv->m_path_scratch.assign(h.max_sets, 0);
      fv->chunk_hidden.assign(covered, 0);
    }
  } catch (std::bad_alloc&) {
    why = "no memory";
  }
  if (why) {
    lg::warn("[ctr] {}: {} not used: {}", level, path, why);
    fclose(fp);
    return nullptr;
  }
#ifdef __3DS__
  setvbuf(fp, nullptr, _IONBF, 0);  // one SD card request per block
#endif
  fv->m_fp = fp;
  fv->m_path = path;
  const auto& h = fv->hdr;
  lg::info("[ctr] {}: fine visibility: {} blocks of {:.0f} m ({:.0f} m cells), {} units, largest "
           "block {:.1f} KB, {} KB in memory (pool {} KB)",
           level, h.num_blocks, h.block * h.cell / 4096.f, h.cell / 4096.f, h.num_units,
           h.max_block / 1024.f, (fv->bytes() + 1023) / 1024, fv->m_pool.size() / 1024);
  return fv;
}

CtrFineVis::~CtrFineVis() {
  if (m_fp) {
    fclose(m_fp);
  }
}

u32 CtrFineVis::bytes() const {
  return (u32)(sizeof(*this) + m_index.size() * sizeof(c3v::BlockEntry) + m_pool.size() +
               m_bits.size() * 4 + m_path_scratch.size() * 2 + chunk_first_unit.size() * 4 +
               chunk_runs.size() + chunk_hidden.size());
}

void CtrFineVis::cancel() {
  cancelled = true;
}

int CtrFineVis::find_block(u32 key) const {
  const size_t n = hdr.num_blocks;
  size_t lo = 0, hi = n;
  while (lo < hi) {
    const size_t m = (lo + hi) / 2;
    if (m_index[m].key < key) {
      lo = m + 1;
    } else {
      hi = m;
    }
  }
  return lo < n && m_index[lo].key == key ? (int)lo : -1;
}

int CtrFineVis::slot_of(u32 key) const {
  for (int s = 0; s < kSlots; s++) {
    if (m_slot[s].state.load(std::memory_order_acquire) != kFree && m_slot[s].key == key) {
      return s;
    }
  }
  return -1;
}

int CtrFineVis::request(u32 key, u64 frame) {
  int s = slot_of(key);
  if (s >= 0) {
    m_slot[s].used = frame;
    return s;
  }
  const int bi = find_block(key);
  if (bi < 0) {
    return -1;
  }
  const u32 size = m_index[bi + 1].offset - m_index[bi].offset;
  if (size == 0 || size > m_pool.size()) {
    return -1;
  }
  // a place in the pool (first fit), the least recently wanted blocks (not wanted this frame,
  // not being read) making room
  for (;;) {
    int free_slot = -1;
    std::pair<u32, u32> used[kSlots];  // (off, end) (no heap on the render thread)
    int nused = 0;
    for (int i = 0; i < kSlots; i++) {
      if (m_slot[i].state.load(std::memory_order_acquire) == kFree) {
        if (free_slot < 0) {
          free_slot = i;
        }
      } else {
        used[nused++] = {m_slot[i].off, m_slot[i].off + m_slot[i].size};
      }
    }
    std::sort(used, used + nused);
    u32 at = 0;
    bool fits = false;
    for (int i = 0; i < nused; i++) {
      if (used[i].first >= at + size) {
        fits = true;
        break;
      }
      at = std::max(at, used[i].second);
    }
    fits = fits || at + size <= m_pool.size();
    if (fits && free_slot >= 0) {
      Slot& sl = m_slot[free_slot];
      sl.key = key;
      sl.off = at;
      sl.size = size;
      sl.gen++;
      sl.used = frame;
      sl.state.store(kLoading, std::memory_order_release);
      if (!start_loader()) {
        sl.state.store(kFailed, std::memory_order_release);
        return free_slot;
      }
      Loader& L = loader();
      {
        std::lock_guard<std::mutex> lk(L.lock);
        L.queue.push_back({shared_from_this(), free_slot});
      }
      L.cv.notify_one();
      return free_slot;
    }
    // evict the least recently wanted block that isn't being read
    int victim = -1;
    for (int i = 0; i < kSlots; i++) {
      const u8 st = m_slot[i].state.load(std::memory_order_acquire);
      if ((st == kReady || st == kFailed) && m_slot[i].used < frame &&
          (victim < 0 || m_slot[i].used < m_slot[victim].used)) {
        victim = i;
      }
    }
    if (victim < 0) {
      m_no_room++;
      return -1;
    }
    if (victim == m_have_slot) {
      m_have = false;
    }
    m_slot[victim].state.store(kFree, std::memory_order_release);
    m_evicted++;
  }
}

void CtrFineVis::load_slot(int s) {
  Slot& sl = m_slot[s];
  if (cancelled.load()) {
    sl.state.store(kFailed, std::memory_order_release);
    return;
  }
  const int bi = find_block(sl.key);
  bool ok = bi >= 0 && sl.off + sl.size <= m_pool.size();
  const double t0 = ctr_gpu_time_ms();
  if (ok) {
    ok = fseek(m_fp, (long)m_index[bi].offset, SEEK_SET) == 0 &&
         fread(m_pool.data() + sl.off, 1, sl.size, m_fp) == sl.size &&
         c3v::block_sets(m_pool.data() + sl.off, sl.size) != 0;
  }
  const u64 us = (u64)std::max(0.0, (ctr_gpu_time_ms() - t0) * 1000.0);
  st_reads++;
  st_bytes += sl.size;
  st_read_us += us;
  u64 m = st_read_max_us.load();
  while (us > m && !st_read_max_us.compare_exchange_weak(m, us)) {
  }
  if (!ok) {
    st_failed++;
  }
  sl.state.store(ok ? kReady : kFailed, std::memory_order_release);
}

void CtrFineVis::select() {
  const u32* bits = m_bits.data();
  for (size_t ci = 0; ci < chunk_hidden.size(); ci++) {
    const u32 a = chunk_first_unit[ci], b = chunk_first_unit[ci + 1];
    u32 seen = 0;
    for (u32 u = a; u < b;) {
      if ((u & 31) == 0 && u + 32 <= b) {
        seen += (u32)__builtin_popcount(bits[u >> 5]);
        u += 32;
      } else {
        seen += (bits[u >> 5] >> (u & 31)) & 1;
        u++;
      }
    }
    chunk_hidden[ci] = seen == b - a ? 0 : (seen == 0 ? 1 : 2);
  }
}

const u32* CtrFineVis::update(float x, float y, float z, bool usable, u64 frame) {
  const double t0 = ctr_gpu_time_ms();
  m_frames++;
  const u32* out = nullptr;
  const auto& h = hdr;
  const float fx = (x - h.origin[0]) / h.cell, fy = (y - h.origin[1]) / h.cell,
              fz = (z - h.origin[2]) / h.cell;
  if (!usable) {
    m_unusable++;
  } else if (!(fx >= 0 && fy >= 0 && fz >= 0 && fx < (float)h.nx && fy < (float)h.ny &&
               fz < (float)h.nz)) {
    m_no_cell++;  // (outside the grid, or not a number)
  } else {
    const u32 B = h.block;
    const u32 cx = std::min<u32>((u32)fx, h.nx - 1), cy = std::min<u32>((u32)fy, h.ny - 1),
              cz = std::min<u32>((u32)fz, h.nz - 1);
    const u32 key = ((cz / B) * h.bny + cy / B) * h.bnx + cx / B;
    const u32 cell_key = (cz * h.ny + cy) * h.nx + cx;
    if (cell_key != m_last_cell) {
      m_cells_entered++;
      m_last_cell = cell_key;
    }
    // the camera's block first, then the others within kPrefetch
    const int s = request(key, frame);
    {
      const float p = kPrefetch / h.cell;
      auto range = [&](float f, u32 n, int* lo, int* hi) {
        *lo = std::max(0, (int)std::floor((f - p) / B));
        *hi = std::min((int)(n / B) - 1, (int)std::floor((f + p) / B));
      };
      int x0, x1, y0, y1, z0, z1;
      range(fx, h.nx, &x0, &x1);
      range(fy, h.ny, &y0, &y1);
      range(fz, h.nz, &z0, &z1);
      for (int bz = z0; bz <= z1; bz++) {
        for (int by = y0; by <= y1; by++) {
          for (int bx = x0; bx <= x1; bx++) {
            const u32 k = ((u32)bz * h.bny + by) * h.bnx + bx;
            if (k != key) {
              request(k, frame);
            }
          }
        }
      }
    }
    if (s < 0 && find_block(key) < 0) {
      m_no_cell++;  // (no cells in this block)
    } else if (s < 0 || m_slot[s].state.load(std::memory_order_acquire) != kReady) {
      m_waiting++;
    } else {
      const Slot& sl = m_slot[s];
      const u8* blk = m_pool.data() + sl.off;
      const u32 cell = ((cz % B) * B + cy % B) * B + cx % B;
      const int set = c3v::block_cell_set(blk, sl.size, cell);
      if (set < 0) {
        m_no_cell++;
      } else if (m_have && m_have_slot == s && m_have_gen == sl.gen && m_have_cell == (u32)set) {
        out = m_bits.data();
      } else {
        const double td = ctr_gpu_time_ms();
        m_have = c3v::block_decode_set(blk, sl.size, (u32)set, m_bits.data(), h.set_words,
                                       m_path_scratch.data());
        if (m_have) {
          select();
          m_have_slot = s;
          m_have_gen = sl.gen;
          m_have_cell = (u32)set;
          out = m_bits.data();
        }
        const double dt = ctr_gpu_time_ms() - td;
        m_decodes++;
        m_decode_ms += dt;
        m_decode_max = std::max(m_decode_max, dt);
      }
    }
  }
  m_with_set += out != nullptr;
  m_update_ms += ctr_gpu_time_ms() - t0;
  return out;
}

void CtrFineVis::log_stats(const std::string& level) {
  if (!m_frames) {
    return;
  }
  const u64 reads = st_reads.exchange(0), bytes = st_bytes.exchange(0), failed = st_failed.exchange(0);
  const u64 read_us = st_read_us.exchange(0), read_max = st_read_max_us.exchange(0);
  lg::info("[ctr] {}: fine vis: {} of {} level draws with a set ({} waiting for a block, {} "
           "outside the cells, {} not usable); {} cells entered, {} sets decoded ({:.3f} ms on "
           "average, {:.3f} at most); {} blocks read ({:.1f} KB, {:.2f} ms each on average, {:.2f} "
           "at most{}), {} dropped, {} without room; {:.3f} ms per level draw",
           level, m_with_set, m_frames, m_waiting, m_no_cell, m_unusable, m_cells_entered,
           m_decodes, m_decodes ? m_decode_ms / m_decodes : 0.0, m_decode_max, reads,
           bytes / 1024.0, reads ? read_us / 1000.0 / reads : 0.0, read_max / 1000.0,
           failed ? fmt::format(", {} failed", failed) : "", m_evicted, m_no_room,
           m_update_ms / m_frames);
  m_frames = m_with_set = m_no_cell = m_waiting = m_unusable = 0;
  m_decodes = m_cells_entered = m_evicted = m_no_room = 0;
  m_decode_ms = m_decode_max = m_update_ms = 0;
}
