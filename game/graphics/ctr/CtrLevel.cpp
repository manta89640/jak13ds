/*!
 * @file CtrLevel.cpp
 * (AI-assisted)
 * See CtrLevel.h. The camera math follows the PC tfrag renderer
 * (make_new_cam_mat and tfrag3.vert in game/graphics/opengl_renderer).
 */

#include "CtrLevel.h"

#include "game/graphics/ctr/CtrSettings.h"
#include "game/graphics/ctr/CtrImpostor.h"
#include "game/graphics/ctr/CtrOccCull.h"
#include "game/graphics/ctr/CtrStream.h"
#include "game/graphics/ctr/CtrVram.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <new>
#include <stdexcept>
#include <tuple>

#include "common/dma/dma.h"
#include "common/dma/gs.h"
#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/util/Timer.h"

#include "game/graphics/ctr/c3l_reader.h"
#include "game/graphics/ctr/ctr_gpu.h"
#include "game/graphics/opengl_renderer/buckets.h"
#include "game/runtime.h"  // (AI-assisted) g_game_version

#ifdef __3DS__
#include "game/overlord/jak1/fake_iso.h"  // (AI-assisted) io_heap_free
#include "platform/3ds/port/ctr_heap.h"
#include "platform/3ds/port/ctr_port.h"
extern "C" u32 linearSpaceFree(void);  // (AI-assisted) libctru
#endif

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

namespace {
// below the render thread (0x2F) and the sound mixer (0x2E), which share core 2 with the loader
constexpr int kLoaderPrio = 0x3C;
// a prefetched level nothing asked for is dropped after this long
constexpr double kPrefetchKeepMs = 60000.0;
// (AI-assisted) a job that ran out of memory waits at most this long for the render thread to make
// room (CtrLevels::make_room)
constexpr int kRoomWaitMs = 1000;
// (AI-assisted) how long a level ready() said yes to stays parked for the game to report it
constexpr double kPromiseMs = 3000.0;
// (AI-assisted) incomplete levels: checked every kRepairCheckFrames frames, made again when the
// last frame's free linear memory covers the missing parts plus kRepairMargin; after a repair that
// left parts out not before kRepairRetryFrames, twice that after each one in a row that made
// nothing (up to 16x)
constexpr u64 kRepairCheckFrames = 30, kRepairRetryFrames = 300;
constexpr u32 kRepairMargin = 256 * 1024;
// (AI-assisted) a failed load (out of heap memory) of a level the game wants: tried again after
// kFailedRetryMs, or after kFailedRetryMinMs once the heap's largest free block is
// kFailedRetryGrowth bigger than when it failed (checked every kRepairCheckFrames frames). It
// waited for the level to be drawn (Lava Tube's background came ~40 s late on hardware).
constexpr double kFailedRetryMs = 10000.0, kFailedRetryMinMs = 1000.0;
constexpr u32 kFailedRetryGrowth = 256 * 1024;

// (AI-assisted) what far mode draws (a level seen from a neighbouring one): its low resolution
// tfrag (or the coarse one) and, in c3l files with chunk kinds, its ties without a distance limit
// (landmarks: houses, towers, rocks), which popped in at far_level_distance
bool far_chunk(const CtrLevelData& lev, const c3l::Chunk& ch) {
  return ch.lod_tier == (lev.has_lowres ? 3u : 2u) ||
         (ch.lod_tier == 0 && ch.kind == c3l::CHUNK_KIND_TIE && ch.max_dist <= 0.f) ||
         ch.kind == c3l::CHUNK_KIND_VISTA;  // (AI-assisted) v13 vista tier: far scenery
}

// (AI-assisted) the parts of a loaded level that should be there and aren't (no memory when it
// loaded): chunk meshes (only the ones far mode draws while its detail is unloaded), merc model
// meshes, the texture pool. Returns how many, and the linear memory they take.
int missing_parts(const CtrLevelData& lev, u32* bytes) {
  int n = 0;
  u32 b = 0;
  for (size_t ci = 0;
       ci < lev.chunks.size() && ci < lev.meshes.size() && ci < lev.mesh_index_first.size();
       ci++) {
    const auto& ch = lev.chunks[ci];
    // (AI-assisted, streaming) near chunks: streamed (or never there), not missing
    if (lev.meshes[ci] < 0 && lev.mesh_index_first[ci] != UINT32_MAX && !lev.is_unit(ci) &&
        !lev.dormant_freed(ci) && (lev.detail_loaded || far_chunk(lev, ch))) {
      n++;
      b += ch.vertex_count * (u32)sizeof(c3l::Vertex) +
           (lev.mesh_index_end[ci] - lev.mesh_index_first[ci]) * (u32)sizeof(u16);
    }
  }
  for (const auto& m : lev.merc_models) {
    if (m.mesh < 0 && m.index_end > m.index_first) {
      n++;
      b += m.vertex_count * (u32)sizeof(c3l::MercVertex) +
           (m.index_end - m.index_first) * (u32)sizeof(u16);
    }
  }
  if (lev.tex_pool < 0 && lev.tex_pool_bytes) {
    n++;
    b += lev.tex_pool_bytes;
  }
  *bytes = b;
  return n;
}

// (AI-assisted, j2-mem) what a level holds: linear memory (its resident chunk meshes by kind and
// tier, near chunks, merc meshes, the texture pool) and the heap (its tables), for the log
template <typename T>
u64 vec_bytes(const std::vector<T>& v) {
  return (u64)v.capacity() * sizeof(T);
}
struct LevelMem {
  u64 tfrag[4] = {0, 0, 0, 0};  // by lod_tier 0..3
  u64 tie_far = 0, tie_dist = 0, shrub = 0, vista = 0, units = 0, merc = 0, tex = 0;
  u64 heap = 0;
  // chunks the visibility string can't hide (no runs, or a run with no id): drawn while it is zero
  u64 unhideable = 0;
  u32 unhideable_chunks = 0;
  u64 linear() const {
    return tfrag[0] + tfrag[1] + tfrag[2] + tfrag[3] + tie_far + tie_dist + shrub + vista + units +
           merc + tex;
  }
};
LevelMem level_mem(const CtrLevelData& lev) {
  LevelMem m;
  for (size_t ci = 0; ci < lev.chunks.size() && ci < lev.meshes.size() &&
                      ci < lev.mesh_index_first.size();
       ci++) {
    if (lev.meshes[ci] < 0) {
      continue;
    }
    const auto& ch = lev.chunks[ci];
    const u64 b = CtrStreamArena::mesh_bytes(ch.vertex_count,
                                             lev.mesh_index_end[ci] - lev.mesh_index_first[ci]);
    bool hideable = ch.vis_run_count > 0 && !lev.vis_runs.empty();
    for (u32 r = 0; hideable && r < ch.vis_run_count; r++) {
      hideable = lev.vis_runs[lev.chunk_first_run[ci] + r].vis != 0xffff;
    }
    if (!hideable && ch.kind != c3l::CHUNK_KIND_VISTA) {
      m.unhideable += b;
      m.unhideable_chunks++;
    }
    if (lev.is_unit(ci)) {
      m.units += b;
    } else if (ch.kind == c3l::CHUNK_KIND_VISTA) {
      m.vista += b;
    } else if (ch.kind == c3l::CHUNK_KIND_SHRUB) {
      m.shrub += b;
    } else if (ch.kind == c3l::CHUNK_KIND_TIE && ch.lod_tier == 0) {
      (ch.max_dist <= 0.f ? m.tie_far : m.tie_dist) += b;
    } else {
      m.tfrag[std::min<u32>(ch.lod_tier, 3)] += b;
    }
  }
  for (const auto& mm : lev.merc_models) {
    if (mm.mesh >= 0) {
      m.merc += (u64)mm.vertex_count * sizeof(c3l::MercVertex) +
                (u64)(mm.index_end - mm.index_first) * sizeof(u16);
    }
    m.heap += vec_bytes(mm.draws) + vec_bytes(mm.gen_draws) + sizeof(mm);
    for (const auto& e : mm.mod) {
      m.heap += vec_bytes(e.mask) + vec_bytes(e.dests) + sizeof(e);
    }
  }
  m.tex = lev.tex_pool >= 0 ? lev.tex_pool_bytes : 0;
  m.heap += vec_bytes(lev.chunks) + vec_bytes(lev.draws) + vec_bytes(lev.draw_states) +
            vec_bytes(lev.draw_alpha80) + vec_bytes(lev.sorted_draws) +
            vec_bytes(lev.ordered_draws) + vec_bytes(lev.early_draws) + vec_bytes(lev.draw_chunk) +
            vec_bytes(lev.vis_runs) + vec_bytes(lev.chunk_first_run) +
            vec_bytes(lev.draw_first_run) + vec_bytes(lev.draw_run_count) +
            vec_bytes(lev.run_spheres) + vec_bytes(lev.chunk_run_count) +
            vec_bytes(lev.lod_groups) + vec_bytes(lev.run_lod) + vec_bytes(lev.chunk_lod) +
            vec_bytes(lev.lod_state) + vec_bytes(lev.lod_error) + vec_bytes(lev.textures) +
            vec_bytes(lev.tex_min_alpha) + vec_bytes(lev.meshes) + vec_bytes(lev.blerc_verts) +
            vec_bytes(lev.blerc_targets) + vec_bytes(lev.blerc_dests) +
            vec_bytes(lev.outside_cache) + vec_bytes(lev.mesh_index_first) +
            vec_bytes(lev.mesh_index_end) + vec_bytes(lev.chunk_state) +
            vec_bytes(lev.chunk_detail_q) + vec_bytes(lev.lod_detail_q) +
            vec_bytes(lev.lod_cell_sphere) + vec_bytes(lev.occ_hidden) +
            vec_bytes(lev.stream_units) + vec_bytes(lev.chunk_unit) +
            vec_bytes(lev.group_unit_first) + vec_bytes(lev.group_unit_count) +
            vec_bytes(lev.group_units) + vec_bytes(lev.unit_group_first) +
            vec_bytes(lev.unit_groups) + vec_bytes(lev.unit_ok) + vec_bytes(lev.lod_hold_since) +
            sizeof(CtrLevelData);
  if (lev.tod) {
    const auto& t = *lev.tod;
    m.heap += t.bytes() + vec_bytes(t.stamp) + vec_bytes(t.chunk_min_ver) * 3 +
              vec_bytes(t.chunk_mask) * 2 + vec_bytes(t.chunk_mesh) + vec_bytes(t.chunk_group_count);
  }
  if (lev.fine) {
    m.heap += lev.fine->bytes();
  }
  if (lev.file_dir) {
    m.heap += vec_bytes(lev.file_dir->blocks);
  }
  return m;
}
std::string level_mem_text(const LevelMem& m) {
  return fmt::format(
      "linear {} KB (tfrag {}/{}/{}/{} by tier, ties {} far + {} near-limited, shrubs {}, vista {}, "
      "near chunks {}, merc {}, textures {}; {} KB in {} chunks the visibility string can't hide), "
      "heap ~{} KB",
      m.linear() / 1024, m.tfrag[0] / 1024, m.tfrag[1] / 1024, m.tfrag[2] / 1024, m.tfrag[3] / 1024,
      m.tie_far / 1024, m.tie_dist / 1024, m.shrub / 1024, m.vista / 1024, m.units / 1024,
      m.merc / 1024, m.tex / 1024, m.unhideable / 1024, m.unhideable_chunks, m.heap / 1024);
}

// (AI-assisted, j2-mem) the stream arena's first segment (KB): config stream_kb, else Jak II's
// own (its near chunks needed at once in Haven City: docs/3ds-port/streaming.md "Jak II")
constexpr u32 kJak2StreamKb = 1536;
u32 ctr_stream_kb() {
  const auto& s = ctr_settings();
  return s.stream_kb_set || !ctr_is_jak2() ? (u32)s.stream_kb : kJak2StreamKb;
}

void build_draw_order(CtrLevelData* out);  // (AI-assisted) below
size_t c3l_read_window();                  // (AI-assisted) below
// (AI-assisted) the converter's largest block (c3l v11+ BlockDir::max_block); a file with bigger
// ones gets bigger caches when it's read
constexpr size_t kC3lMaxBlock = 64 * 1024;
}  // namespace

CtrLevels::CtrLevels() {
  // (AI-assisted) the reader's memory for all loads, while the heap is still in one piece
  m_read_bufs.reserve(c3l_read_window(), kC3lMaxBlock);
#ifdef __3DS__
  // core 2 on New 3DS (ctr_thread_create falls back to core 0 without it)
  if (ctr_thread_create(&CtrLevels::loader_entry, this, 64 * 1024, kLoaderPrio, 2, &m_thread) !=
      0) {
    m_thread = nullptr;
    lg::warn("[ctr] no level loader thread: levels load on the render thread");
  }
#endif
}

CtrLevels::~CtrLevels() {
  if (m_thread) {
    {
      std::lock_guard<std::mutex> lk(m_lock);
      m_quit = true;
      m_cancel = true;
    }
    m_cv.notify_all();
#ifdef __3DS__
    ctr_thread_join(m_thread);
#endif
    m_thread = nullptr;
  }
  for (auto& d : m_done) {
    if (d.lev) {
      unload(*d.lev);
    }
    delete_parts(d);
  }
  for (auto& d : m_load_delayed) {  // (AI-assisted, j2intro) host, CTR_LOAD_DELAY_MS
    if (d.lev) {
      unload(*d.lev);
    }
  }
  for (auto& [name, p] : m_prefetched) {
    unload(*p.lev);
  }
  for (auto& tk : m_take) {
    ctr_gpu_mesh_delete(tk.out.mesh);  // (AI-assisted, streaming)
  }
  for (auto& [name, lev] : m_levels) {
    unload(*lev);
  }
  if (m_common) {
    unload(*m_common);
  }
}

void* CtrLevels::loader_entry(void* self) {
#ifdef __3DS__
  ctr_thread_install_crash_handler();
#endif
  static_cast<CtrLevels*>(self)->loader_main();
  return nullptr;
}

// (AI-assisted) under m_lock: a parked level is kept while it's the one whose DGO the game loads,
// or ready() just told the game it's there (it reports it at the end of that frame)
bool CtrLevels::parked_matters(const std::string& name, double now) const {
  if (name == m_dgo_level) {
    return true;
  }
  auto it = m_promised.find(name);
  return it != m_promised.end() && now - it->second < kPromiseMs;
}

// (AI-assisted) under m_lock
bool CtrLevels::can_start(const Job& job) const {
  if (job.common) {
    return true;
  }
  // a parked prefetch the game gave up on (it loads another level's DGO now) is unloaded at the
  // render thread's next frame (housekeeping): its memory first
  const double now = ctr_gpu_time_ms();
  int parked = 0;
  for (const auto& n : m_parked) {
    if (!parked_matters(n, now)) {
      return false;
    }
    parked++;
  }
  // a prefetch waits while two levels that matter are loaded (the game has two level slots, so
  // the level it replaces is still in memory). (AI-assisted, j2intro) Jak II has six: its title
  // loads title, ctywide, lwidea and ctysluma one after the other, each prefetched with its DGO
  // and waited for (pc-3ds-level-ready?); lwidea's waited 20 s behind title + ctywide.
  const int slots = g_game_version == GameVersion::Jak2 ? CtrJak2Vis::kSlots : 2;
  return !job.prefetch || m_levels_count + parked < slots;
}

void CtrLevels::loader_main() {
  std::unique_lock<std::mutex> lk(m_lock);
  while (!m_quit) {
    // the first job that can run. (AI-assisted, streaming) In this order: the loads the game
    // waits for (not prefetches), stream jobs with near chunks needed now (the game doesn't wait
    // for a prefetch: Sandover's hut waited ~3 s behind the beach's for its near chunks), the
    // prefetches, the other stream jobs
    auto pick = [&](auto&& want) {
      auto i = m_queue.begin();
      while (i != m_queue.end() && !want(*i)) {
        ++i;
      }
      return i;
    };
    // While levels are drawn (the game plays: the level it loads is the neighbour, it doesn't
    // wait) the stream jobs with near chunks needed now go first: they are short (at most ~0.1 s
    // on hardware), and Sandover's hut waited 3.6 s behind the beach's load for its near chunks.
    // With nothing drawn (a load the game waits for, black screen) the level loads go first. (A
    // rule by the game's ready() calls was tried: it polls them for the neighbour too.)
    const u64 rf = m_render_frame.load(), df = m_drawn_frame.load();
    const bool playing = df && df + 3 >= rf;
    auto it = m_queue.end();
    if (playing) {
      it = pick([](const Job& j) { return j.stream && j.urgent; });
    }
    if (it == m_queue.end()) {
      it = pick([&](const Job& j) { return !j.stream && !j.prefetch && can_start(j); });
    }
    if (it == m_queue.end()) {
      it = pick([](const Job& j) { return j.stream && j.urgent; });
    }
    if (it == m_queue.end()) {
      it = pick([&](const Job& j) { return !j.stream && can_start(j); });
    }
    if (it == m_queue.end()) {
      it = pick([](const Job& j) { return j.stream; });
    }

    if (it == m_queue.end()) {
      if (m_queue.empty()) {
        m_cv.wait(lk);
      } else {
        m_cv.wait_for(lk, std::chrono::milliseconds(100));
      }
      continue;
    }
    Job job = *it;
    m_queue.erase(it);
    m_loading = job.name;
    m_loading_prefetch = job.prefetch;
    m_cancel = false;
    lk.unlock();
    run_job(job);
    lk.lock();
    m_loading.clear();
  }
}

namespace {
double load_sim_delay_ms(const std::string& name);  // (AI-assisted, j2intro) below
}  // namespace

void CtrLevels::run_job(const Job& job) {
  Done d;
  d.job = job;
  m_room_tried = false;
  // (AI-assisted) an allocation that fails (bad_alloc) fails the job: the loader thread goes on
  // (an exception out of it is std::terminate); the render thread tries again later
  try {
    if (job.stream) {
      run_stream_job(job, &d);  // (AI-assisted, streaming)
    } else if (job.parts()) {
      run_parts_job(job, &d);
    } else {
      d.lev = std::make_unique<CtrLevelData>();
      d.lev->generation = ++m_generation;
      d.result = load(job.name, d.lev.get());
    }
  } catch (std::exception& e) {
    lg::error("[ctr] {}: {} failed: {}", job.name,
              job.parts() ? "reading meshes again" : "loading", e.what());
    if (d.lev) {
      unload(*d.lev);
    }
    d.result = LoadResult::FAILED;
  }
  if (!job.parts() && d.result != LoadResult::LOADED) {
    d.lev.reset();
  }
  if (!job.stream && !job.parts() && !job.common && d.result == LoadResult::LOADED) {
    // (AI-assisted, j2intro) host, CTR_LOAD_DELAY_MS: handed over this much later
    if (const double ms = load_sim_delay_ms(job.name); ms > 0) {
      d.sim_ready_ms = ctr_gpu_time_ms() + ms;
    }
  }
#ifdef __3DS__
  if (d.result == LoadResult::FAILED) {
    CtrHeapStats st;
    ctr_heap_stats(&st);
    d.heap_largest = st.largest;
  }
#endif
  std::lock_guard<std::mutex> lk(m_lock);
  m_done.push_back(std::move(d));
}

void CtrLevels::request(const Job& job) {
  {
    std::lock_guard<std::mutex> lk(m_lock);
    if (m_requested.count(job.key()) || (job.prefetch && m_resident.count(job.name))) {
      // (AI-assisted) the game wants a level whose prefetch is still queued: the prefetch would
      // wait while two levels are loaded (one of them maybe a prefetch nobody wants any more)
      if (!job.prefetch && !job.parts()) {
        for (auto& q : m_queue) {
          if (q.name == job.name && q.prefetch) {
            q.prefetch = false;
          }
        }
        if (m_loading == job.name) {
          m_loading_prefetch = false;
        }
      }
      return;
    }
    if (job.prefetch) {
      lg::info("[ctr] prefetching {}", job.name);
    }
    m_requested.insert(job.key());
    m_queue.push_back(job);
  }
  m_cv.notify_all();
}

bool CtrLevels::ready(const std::string& name) {
  std::lock_guard<std::mutex> lk(m_lock);
  if (m_resident.count(name)) {
    m_promised[name] = ctr_gpu_time_ms();  // (AI-assisted) kept for the game (parked_matters)
    return true;
  }
  for (const auto& d : m_done) {
    // (AI-assisted, j2intro: sim_ready_ms, host only, CTR_LOAD_DELAY_MS)
    if (d.job.name == name && !d.job.parts() && d.sim_ready_ms <= ctr_gpu_time_ms()) {
      m_promised[name] = ctr_gpu_time_ms();
      return true;  // read (or failed); the render thread takes it at its next frame
    }
  }
  return m_requested.count(name) == 0;  // queued or loading: wait
}

// (AI-assisted, j2intro) Host model of slow level loads (the 3DS reads a .c3l from the SD card in
// seconds; the host in milliseconds, in the frame it's wanted): env CTR_LOAD_DELAY_MS =
// "level:ms,level:ms" or "*:ms" (all levels). A level load is handed over (ready(), the render
// thread's take) that long after it ran. Reproduces a level shown before its background is in
// (Jak II's title intro, j2intro) and the game's wait for it (pc-3ds-level-ready? on the host
// follows ready() while this is set).
namespace {
double load_sim_delay_ms(const std::string& name) {
#ifdef __3DS__
  (void)name;
  return 0;
#else
  static const char* env = getenv("CTR_LOAD_DELAY_MS");
  if (!env) {
    return 0;
  }
  std::string e(env);
  size_t p = 0;
  while (p < e.size()) {
    size_t q = e.find(',', p);
    if (q == std::string::npos) {
      q = e.size();
    }
    const std::string item = e.substr(p, q - p);
    const size_t c = item.find(':');
    if (c != std::string::npos && (item.substr(0, c) == name || item.substr(0, c) == "*")) {
      return atof(item.c_str() + c + 1);
    }
    p = q + 1;
  }
  return 0;
#endif
}
}  // namespace

bool CtrLevels::load_sim_on() {
#ifdef __3DS__
  return false;
#else
  static const bool on = getenv("CTR_LOAD_DELAY_MS") != nullptr;
  return on;
#endif
}

void CtrLevels::host_service() {
  if (m_thread || !load_sim_on()) {
    return;
  }
  for (;;) {
    Job job;
    {
      std::lock_guard<std::mutex> lk(m_lock);
      if (m_queue.empty()) {
        break;
      }
      job = m_queue.front();
      m_queue.pop_front();
    }
    run_job(job);
  }
  std::lock_guard<std::mutex> lk(m_lock);
  const double now = ctr_gpu_time_ms();
  for (auto it = m_load_delayed.begin(); it != m_load_delayed.end();) {
    if (it->sim_ready_ms <= now) {
      m_done.push_back(std::move(*it));  // (its key stays requested until the frame takes it)
      it = m_load_delayed.erase(it);
    } else {
      ++it;
    }
  }
}

void CtrLevels::prefetch(const std::string& name) {
  {
    std::lock_guard<std::mutex> lk(m_lock);
    // (AI-assisted) The game loads one level at a time: a prefetch of another level that is still
    // queued or loading is one it gave up on (Jak died in Fire Canyon while village2 loaded, and
    // village1 waited behind it). A parked one is unloaded by the render thread (housekeeping).
    if (m_dgo_level != name) {
      for (auto it = m_queue.begin(); it != m_queue.end();) {
        if (it->prefetch && it->name != name) {
          lg::info("[ctr] prefetch of {} cancelled (the game loads {} now)", it->name, name);
          m_requested.erase(it->key());
          it = m_queue.erase(it);
        } else {
          ++it;
        }
      }
      if (!m_loading.empty() && m_loading_prefetch && m_loading != name) {
        m_cancel = true;
      }
    }
    m_dgo_level = name;
  }
  request({name, false, true});
}

void CtrLevels::update_level_count() {
  {
    std::lock_guard<std::mutex> lk(m_lock);
    m_levels_count = (int)m_levels.size();
    m_parked.clear();
    m_resident.clear();
    for (auto& [name, lev] : m_levels) {
      m_resident.insert(name);
    }
    for (auto& [name, p] : m_prefetched) {
      m_resident.insert(name);
      m_parked.insert(name);
    }
  }
  m_cv.notify_all();
}

bool CtrLevels::housekeeping(bool room) {
  // parked prefetches are never wanted (set_wanted takes them): kept while they matter
  std::vector<std::string> drop;
  std::string dgo;
  {
    std::lock_guard<std::mutex> lk(m_lock);
    dgo = m_dgo_level;
    room |= m_room_wanted;
    m_room_wanted = false;
    const double now = ctr_gpu_time_ms();
    for (const auto& [name, p] : m_prefetched) {
      if (!parked_matters(name, now)) {
        drop.push_back(name);
      }
    }
  }
  for (const auto& name : drop) {
    lg::info("[ctr] dropping prefetched {} (the game loads {} now)", name, dgo);
    unload(*m_prefetched[name].lev);
    m_prefetched.erase(name);
  }
  const bool dropped = !drop.empty();
  if (room) {
    stream_shrink(true);  // (AI-assisted, streaming) the arena's growth back for the load
  }
  if (dropped || room) {
    // before the loader goes on (it may be waiting for this memory)
    ctr_gpu_free_pending_now();
  }
  if (room) {
    CtrStreamArena::get().release_empty();  // (AI-assisted, streaming)
  }
  if (dropped) {
    update_level_count();
  }
  if (room) {
    {
      std::lock_guard<std::mutex> lk(m_lock);
      m_room_serial++;
    }
    m_cv.notify_all();
  }
  return dropped;
}

bool CtrLevels::needs_service() {
  std::lock_guard<std::mutex> lk(m_lock);
  if (m_room_wanted) {
    return true;
  }
  const double now = ctr_gpu_time_ms();
  for (const auto& n : m_parked) {
    if (!parked_matters(n, now)) {
      return true;
    }
  }
  return false;
}

void CtrLevels::service() {
  housekeeping(false);
}

bool CtrLevels::make_room(const std::string& name, const char* what) {
  if (m_room_tried || cancelled()) {
    return false;
  }
  m_room_tried = true;
  lg::warn("[ctr] {}: no memory for {}, making room and trying again", name, what);
  if (!m_thread) {
    housekeeping(true);  // the loads run on the render thread (outside of a frame)
    return true;
  }
  std::unique_lock<std::mutex> lk(m_lock);
  const u32 serial = m_room_serial;
  m_room_wanted = true;
  // at the render thread's next frame (or the game thread's service)
  m_cv.wait_for(lk, std::chrono::milliseconds(kRoomWaitMs),
                [&] { return m_room_serial != serial || m_quit || cancelled(); });
  return !m_quit && !cancelled();
}

void CtrLevels::publish(const std::string& name, std::unique_ptr<CtrLevelData> lev, u64 frame) {
  lev->last_used_frame = frame;
  m_levels[name] = std::move(lev);
  rebuild_merc_index();
  bump_set_generation();  // (AI-assisted)
}

namespace {
// (AI-assisted) Each read from a .c3l file reads this much (one SD card request, unbuffered, into
// the reader's window): config.ini c3l_read_buffer (KB, 16..1024; default 256; 0: the old reads,
// through a 64 KB stdio buffer that started again after every seek). The DGO's reads take 256 KB.
size_t c3l_read_window() {
  static long kb = -1;
  if (kb < 0) {
    kb = 256;
#ifdef __3DS__
    char v[16];
    if (ctr_config_get("c3l_read_buffer", v, sizeof(v))) {
      const long n = strtol(v, nullptr, 10);
      kb = n <= 0 ? 0 : std::clamp<long>(n, 16, 1024);
    }
#endif
    if (kb) {
      lg::info("[ctr] .c3l files read {} KB at a time (config.ini c3l_read_buffer)", kb);
    } else {
      lg::info("[ctr] .c3l files read through a 64 KB stdio buffer (config.ini c3l_read_buffer = 0)");
    }
  }
  size_t bytes = (size_t)kb * 1024;
#ifdef __3DS__
  // the heap's level-load peak leaves ~3 MB (the DGO's read-ahead window is there too): at most
  // the old reads' 128 KB window when it's low
  // (AI-assisted) Jak 1's overlord heap (Jak II's overlord doesn't use it)
  if (bytes > 128 * 1024 && g_game_version == GameVersion::Jak1 &&
      jak1::io_heap_free() < (2u << 20)) {
    bytes = 128 * 1024;
  }
#endif
  return bytes;
}

// (AI-assisted) the pieces of a .c3l file, as they are needed (v11: decompressed): c3l_reader.h
struct C3lReader : c3l::Reader {
  C3lReader(const fs::path& path, c3l::Reader::Buffers* buffers)
      : c3l::Reader(path.string().c_str(), c3l_read_window(), buffers) {}
  // (AI-assisted, streaming) a window of its own size (at most c3l_read_window: the buffers'), the
  // file's directory as read before
  C3lReader(const fs::path& path,
            c3l::Reader::Buffers* buffers,
            size_t window,
            const c3l::Reader::Dir& dir)
      : c3l::Reader(path.string().c_str(),
                    c3l_read_window() ? std::min(window, c3l_read_window()) : 0, buffers, dir) {}
  long size() const { return file_size(); }
};
}  // namespace

// (AI-assisted) Partial unload / repair: make meshes of a loaded level again (loader thread): chunk
// meshes, and for a repair merc model meshes and the texture pool. The job has its own copy of
// what it needs: the level may be unloaded meanwhile. The indices of all its chunks are read at
// once, then the vertices in file order: chunk by chunk, every switch between the two threw the
// reader's window away (beach: 36 MB read from the SD card for 4.7 MB of meshes, 11 s on
// hardware; this way ~3 MB). Stops at the first mesh there's no memory for.
void CtrLevels::run_parts_job(const Job& job, Done* d) {
  d->result = LoadResult::FAILED;
  C3lReader f(job.path, &m_read_bufs);
  bool ok = f.ok();
  if (ok && !job.chunks.empty()) {
    u32 lo = UINT32_MAX, hi = 0;
    for (const auto& c : job.chunks) {
      lo = std::min(lo, c.index_first);
      hi = std::max(hi, c.index_end);
    }
    // (AI-assisted) window mode (the default): each chunk's indices straight into its index
    // buffer, like load_file (the reader keeps a window for the indices and one for the vertices),
    // not all of them in one piece of the heap first (up to ~2 MB)
    const bool chunk_indices = f.window_mode();
    std::vector<u16> indices;
    if (!chunk_indices) {
      ok = f.read_array(job.index_data_offset + lo * (u32)sizeof(u16), hi - lo, &indices);
    }
    for (const auto& c : job.chunks) {
      if (!ok || cancelled()) {
        break;
      }
      // the vertices are read (decoded) straight into GPU memory
      void* vdst;
      u16* idst;
      const u32 count = c.index_end - c.index_first;
      int mesh = ctr_gpu_mesh_begin(c.vertex_count, count, &vdst, &idst);
      if (mesh < 0 && make_room(job.name, "meshes")) {
        mesh = ctr_gpu_mesh_begin(c.vertex_count, count, &vdst, &idst);
      }
      if (mesh >= 0) {
        bool read_ok = true;
        if (chunk_indices) {
          read_ok = f.read(job.index_data_offset + c.index_first * (u32)sizeof(u16), idst,
                           count * sizeof(u16));
        } else {
          memcpy(idst, indices.data() + (c.index_first - lo), count * sizeof(u16));
        }
        mesh = ctr_gpu_mesh_end(
            mesh, read_ok && f.read(job.vertex_data_offset +
                                        c.first_vertex * (u32)sizeof(c3l::Vertex),
                                    vdst, c.vertex_count * sizeof(c3l::Vertex)));
      }
      ok = mesh >= 0;
      if (ok) {
        d->meshes.emplace_back(c.chunk, mesh);
      }
    }
  }
  if (ok && !job.merc.empty()) {
    // (AI-assisted) each model's indices with its vertices (as load_file)
    std::vector<u16> indices;
    std::vector<c3l::MercVertex> verts;
    for (const auto& m : job.merc) {
      if (!ok || cancelled()) {
        break;
      }
      ok = f.read_array(job.merc_vertex_offset + m.file_vertex * (u32)sizeof(c3l::MercVertex),
                        m.vertex_count, &verts) &&
           f.read_array(job.merc_index_offset + m.index_first * (u32)sizeof(u16),
                        m.index_end - m.index_first, &indices);
      int mesh = -1;
      if (ok) {
        const u16* ix = indices.data();
        const int count = (int)(m.index_end - m.index_first);
        mesh = ctr_gpu_skinned_mesh_create(verts.data(), m.vertex_count, ix, count);
        if (mesh < 0 && make_room(job.name, "models")) {
          mesh = ctr_gpu_skinned_mesh_create(verts.data(), m.vertex_count, ix, count);
        }
      }
      ok = mesh >= 0;
      if (ok) {
        d->merc_meshes.emplace_back(m.model, mesh);
      }
    }
  }
  if (ok && job.textures && !cancelled()) {
    c3l::Header hdr;
    std::vector<c3l::Texture> texs;
    std::vector<c3l::Draw> draws;
    if (f.read(0, &hdr, sizeof(hdr)) && f.read_array(hdr.textures_offset, hdr.num_textures, &texs) &&
        f.read_array(hdr.draw_data_offset, hdr.draw_data_size / sizeof(c3l::Draw), &draws) &&
        !load_textures(f, hdr, texs, draws, job.name, &d->tex)) {
      delete_textures(&d->tex);  // cancelled: not all texels are there
    }
  }
  if (d->meshes.size() == job.chunks.size() && d->merc_meshes.size() == job.merc.size() &&
      (!job.textures || d->tex.pool >= 0)) {
    d->result = LoadResult::LOADED;
  }
}

void CtrLevels::delete_textures(TexLoad* t) {
  for (int h : t->handles) {
    ctr_gpu_tex_delete(h);
  }
  if (t->pool >= 0) {
    ctr_gpu_pool_delete(t->pool);  // after its textures
  }
  *t = TexLoad();
}

void CtrLevels::delete_parts(Done& d) {
  for (auto& [ci, mesh] : d.meshes) {
    ctr_gpu_mesh_delete(mesh);
  }
  for (auto& [mi, mesh] : d.merc_meshes) {
    ctr_gpu_mesh_delete(mesh);
  }
  d.meshes.clear();
  d.merc_meshes.clear();
  delete_textures(&d.tex);
}

// (AI-assisted) render thread: what a detail reload / repair made, for the same load of the level
// (else deleted). A detail reload counts only if it made everything (the level is drawn in far
// mode until then), a repair keeps what it made.
void CtrLevels::take_parts(Done& d, u64 frame) {
  const std::string& name = d.job.name;
  auto it = m_levels.find(name);
  CtrLevelData* lev = it != m_levels.end() && it->second->generation == d.job.generation
                          ? it->second.get()
                          : nullptr;
  if (lev) {
    (d.job.dormant  ? lev->dormant_requested
     : d.job.detail ? lev->detail_requested
                    : lev->repair_requested) = false;
    bump_set_generation();  // (AI-assisted)
  }
  // (AI-assisted, j2-mem) dormant levels: the chunks of the job that didn't come are freed ones
  // again (another read when needed)
  if (lev && d.job.dormant) {
    for (const auto& c : d.job.chunks) {
      if (c.chunk < lev->chunk_dormant.size() &&
          lev->chunk_dormant[c.chunk] == CtrLevelData::kDormReading) {
        lev->chunk_dormant[c.chunk] = CtrLevelData::kDormFreed;
      }
    }
  }
  if (lev && d.job.dormant && d.result != LoadResult::LOADED) {
    delete_parts(d);
    lev->dormant_retry_frame = frame + 60;
    lg::warn("[ctr] {}: dormant chunks could not be read back, retrying later", name);
    return;
  }
  if (!lev || (d.job.detail && d.result != LoadResult::LOADED)) {
    delete_parts(d);
    if (lev) {
      // not every frame while the memory isn't there
      lev->detail_retry_frame = frame + 300;
      lg::warn("[ctr] {}: detail meshes could not be read again, retrying later", name);
    }
    return;
  }
  int made = 0;
  for (auto& [ci, mesh] : d.meshes) {
    if (ci < lev->meshes.size() && lev->meshes[ci] < 0) {
      lev->meshes[ci] = mesh;
      if (ci < lev->chunk_dormant.size()) {
        lev->chunk_dormant[ci] = 0;  // (AI-assisted, j2-mem)
      }
      if (lev->tod && ci < lev->tod->chunk_mesh.size()) {
        lev->tod->chunk_mesh[ci] = -1;  // (AI-assisted) the file's colors: recolored when drawn
      }
      made++;
    } else {
      ctr_gpu_mesh_delete(mesh);
    }
  }
  d.meshes.clear();
  for (auto& [mi, mesh] : d.merc_meshes) {
    if (mi < lev->merc_models.size() && lev->merc_models[mi].mesh < 0) {
      auto& md = lev->merc_models[mi];
      md.mesh = mesh;
      md.blerc_moved = false;  // a new mesh: the file's vertices
      md.mod_checked = false;
      made++;
    } else {
      ctr_gpu_mesh_delete(mesh);
    }
  }
  d.merc_meshes.clear();
  if (d.tex.pool >= 0 && lev->tex_pool < 0) {
    lev->tex_pool = d.tex.pool;
    lev->textures = std::move(d.tex.handles);
    lev->tex_min_alpha = std::move(d.tex.min_alpha);
    d.tex = TexLoad();
    build_draw_order(lev);  // the draws' GPU states hold the texture handles
    made++;
  }
  delete_parts(d);
  if (d.job.dormant) {
    lg::info("[ctr] {}: {} dormant chunks back", name, made);  // (AI-assisted, j2-mem)
  } else if (d.job.detail) {
    lev->detail_loaded = true;
    lg::info("[ctr] {}: detail meshes back ({})", name, made);
  }
  if (lev->incomplete) {
    u32 bytes = 0;
    const int left = missing_parts(*lev, &bytes);
    lev->incomplete = left > 0;
    if (d.job.repair) {
      lg::info("[ctr] {}: {} missing parts made again{}", name, made,
               left ? fmt::format(", {} still missing ({} KB)", left, bytes / 1024) : "");
      lev->repair_failures = made ? 0 : (u8)std::min(lev->repair_failures + 1, 4);
      if (left) {
        lev->repair_retry_frame = frame + (kRepairRetryFrames << lev->repair_failures);
      }
    }
  }
}

void CtrLevels::unload_detail(CtrLevelData& lev) {
  bump_set_generation();  // (AI-assisted)
  int freed = 0;
  for (size_t ci = 0; ci < lev.chunks.size() && ci < lev.meshes.size(); ci++) {
    // (AI-assisted) what far mode draws stays (far_chunk)
    if (!far_chunk(lev, lev.chunks[ci]) && lev.meshes[ci] >= 0 && !lev.is_unit(ci)) {
      ctr_gpu_mesh_delete(lev.meshes[ci]);  // freed after the GPU is done with it
      lev.meshes[ci] = -1;
      if (lev.tod && ci < lev.tod->chunk_mesh.size()) {
        lev.tod->chunk_mesh[ci] = -1;  // (AI-assisted) (a mesh made later may get this one's id)
      }
      freed++;
    }
  }
  lev.detail_loaded = false;
  lev.far_frames = 0;
  lg::info("[ctr] {}: only seen from far away, {} detail meshes freed", lev.name, freed);
}

// ---- (AI-assisted, j2-mem) Dormant levels (Jak II, config dormant_levels) ----
// Haven City keeps a neighbouring district loaded that can't be seen from where Jak is: the game's
// visibility string for it is all zero (ctyindb from the slums: 7.4 MB of linear memory, nothing
// of it drawn). After kDormantFrames level draws so, the level is dormant: it wants no near chunks
// (the stream scheduler frees its ones first), and the meshes of its ties with a distance limit
// (the small and medium objects: 4.4 MB of ctyindb's 7.4) are freed where they are more than
// kDormFreeMeters from the camera (its sphere's surface; from the slums 4.3 MB of ctyindb's are
// beyond 100 m, 3.2 MB beyond 150 m). Kept: everything near the camera, and what can be drawn from anywhere the string
// may show the level again: the tfrag (all tiers: small in Jak II), the ties without a distance
// limit (landmarks, the far versions of the LOD groups), the vista tier, the chunks the string
// can't hide (shrubs: no visibility runs), textures and merc models. Freed chunks are read back
// (a detail job of those chunks) as the camera comes within kDormReadMeters of them (now, 1.5 s
// ahead at its speed, or Jak), and all of them once the string shows the level again: what is
// nearer than that is always there when the level appears. A farther one not back yet isn't drawn,
// and once its mesh comes it stays hidden while in view, at most kDormantHoldFrames from when that
// began (as a tie LOD group whose near chunk came late, streaming.md): it appears out of view.
namespace {
constexpr u32 kDormantFrames = 90;  // 3 s at 30 fps
constexpr u32 kDormantHoldFrames = 120;
constexpr float kDormFreeMeters = 100.f, kDormReadMeters = 75.f;
constexpr float kDormLeadFrames = 45.f;  // 1.5 s at 30 fps (the camera's velocity is per frame)
// a chunk the visibility string hides whole when it is zero: visibility runs, each with an id
bool chunk_hideable(const CtrLevelData& lev, size_t ci) {
  const auto& ch = lev.chunks[ci];
  if (!ch.vis_run_count || lev.vis_runs.empty() || ci >= lev.chunk_first_run.size()) {
    return false;
  }
  const u32 r0 = lev.chunk_first_run[ci];
  for (u32 r = r0; r < r0 + ch.vis_run_count && r < lev.vis_runs.size(); r++) {
    if (lev.vis_runs[r].vis == 0xffff) {
      return false;
    }
  }
  return true;
}
// freed while dormant and far: a tie chunk with a distance limit the visibility string hides
bool dormant_frees(const CtrLevelData& lev, size_t ci) {
  const auto& ch = lev.chunks[ci];
  return !lev.is_unit(ci) && ch.lod_tier == 0 && ch.kind == c3l::CHUNK_KIND_TIE &&
         ch.max_dist > 0.f && ci < lev.mesh_index_first.size() &&
         lev.mesh_index_first[ci] != UINT32_MAX && chunk_hideable(lev, ci);
}
}  // namespace

void CtrLevels::dormant_update(u64 frame) {
  if (!m_cam_valid) {
    return;
  }
  constexpr float kM = 4096.f;
  float pos[3][3];
  int np = 2;
  for (int c = 0; c < 3; c++) {
    pos[0][c] = m_cam[c];
    pos[1][c] = m_cam[c] + m_cam_vel[c] * kDormLeadFrames;
  }
  if (m_target_frame + 3 >= frame) {
    for (int c = 0; c < 3; c++) {
      pos[2][c] = m_target[c];
    }
    np = 3;
  }
  for (auto& [name, lp] : m_levels) {
    CtrLevelData& lev = *lp;
    if (lev.chunk_dormant.empty()) {
      if (!lev.dormant) {
        continue;
      }
      lev.chunk_dormant.assign(lev.chunks.size(), 0);
      lev.chunk_hold_since.assign(lev.chunks.size(), 0);
    }
    if (!lev.detail_loaded || lev.repair_requested) {
      continue;  // (far mode's partial unload, a repair in flight: theirs first)
    }
    std::vector<DetailChunk> want;
    u32 freed = 0, left_out = 0;
    u64 freed_bytes = 0;
    for (size_t ci = 0; ci < lev.chunks.size() && ci < lev.meshes.size(); ci++) {
      const u8 fl = lev.chunk_dormant[ci];
      if (fl == CtrLevelData::kDormReading || (fl == 0 && !lev.dormant) ||
          !dormant_frees(lev, ci)) {
        continue;
      }
      const auto& ch = lev.chunks[ci];
      float d2 = 1e30f;
      for (int i = 0; i < np; i++) {
        const float dx = pos[i][0] - ch.bsphere[0], dy = pos[i][1] - ch.bsphere[1],
                    dz = pos[i][2] - ch.bsphere[2];
        d2 = std::min(d2, dx * dx + dy * dy + dz * dz);
      }
      const float d = std::sqrt(d2) - ch.bsphere[3];
      if (fl == CtrLevelData::kDormFreed) {
        if (!lev.dormant || d < kDormReadMeters * kM) {
          want.push_back({(u32)ci, ch.first_vertex, ch.vertex_count, lev.mesh_index_first[ci],
                          lev.mesh_index_end[ci]});
        }
      } else if (d > kDormFreeMeters * kM && lev.meshes[ci] < 0) {
        // left out by an incomplete load (no memory): a freed one now, not one to repair
        lev.chunk_dormant[ci] = CtrLevelData::kDormFreed;
        left_out++;
      } else if (d > kDormFreeMeters * kM) {
        freed_bytes += CtrStreamArena::mesh_bytes(ch.vertex_count,
                                                  lev.mesh_index_end[ci] - lev.mesh_index_first[ci]);
        ctr_gpu_mesh_delete(lev.meshes[ci]);  // freed after the GPU is done with it
        lev.meshes[ci] = -1;
        if (lev.tod && ci < lev.tod->chunk_mesh.size()) {
          lev.tod->chunk_mesh[ci] = -1;
        }
        lev.chunk_dormant[ci] = CtrLevelData::kDormFreed;
        freed++;
      }
    }
    if (freed || left_out) {
      bump_set_generation();
      lg::info("[ctr] {}: dormant: {} tie chunks freed ({} KB){}", name, freed, freed_bytes / 1024,
               left_out ? fmt::format(", {} left out by its load not repaired", left_out) : "");
      if (lev.incomplete) {
        u32 bytes = 0;
        lev.incomplete = missing_parts(lev, &bytes) > 0;
      }
    }
    if (!want.empty() && !lev.dormant_requested && !lev.detail_requested &&
        !lev.repair_requested && frame >= lev.dormant_retry_frame) {
      Job job;
      job.name = lev.name;
      job.detail = true;
      job.dormant = true;
      job.generation = lev.generation;
      job.path = lev.path;
      job.vertex_data_offset = lev.vertex_data_offset;
      job.index_data_offset = lev.index_data_offset;
      std::sort(want.begin(), want.end(), [](const DetailChunk& a, const DetailChunk& b) {
        return a.first_vertex < b.first_vertex;
      });
      u64 bytes = 0;
      for (const auto& c : want) {
        lev.chunk_dormant[c.chunk] = CtrLevelData::kDormReading;
        bytes += CtrStreamArena::mesh_bytes(c.vertex_count, c.index_end - c.index_first);
      }
      job.chunks = std::move(want);
      lev.dormant_requested = true;
      lg::info("[ctr] {}: camera near its freed ties, reading {} chunks back ({} KB)", name,
               job.chunks.size(), bytes / 1024);
      request(job);
    }
  }
}

// (AI-assisted) the chunks without a mesh (missing_only: the ones missing_parts counts), in file
// order (one pass over the vertex data)
void CtrLevels::collect_chunks(const CtrLevelData& lev,
                               bool missing_only,
                               std::vector<DetailChunk>* out) {
  for (size_t ci = 0; ci < lev.chunks.size() && ci < lev.meshes.size() &&
                      ci < lev.mesh_index_first.size();
       ci++) {
    const auto& ch = lev.chunks[ci];
    if (lev.meshes[ci] < 0 && lev.mesh_index_first[ci] != UINT32_MAX && !lev.is_unit(ci) &&
        (!missing_only || ((lev.detail_loaded || far_chunk(lev, ch)) && !lev.dormant_freed(ci)))) {
      out->push_back({(u32)ci, ch.first_vertex, ch.vertex_count, lev.mesh_index_first[ci],
                      lev.mesh_index_end[ci]});
    }
  }
  std::sort(out->begin(), out->end(), [](const DetailChunk& a, const DetailChunk& b) {
    return a.first_vertex < b.first_vertex;
  });
}

void CtrLevels::request_detail(CtrLevelData& lev) {
  if (lev.repair_requested) {
    return;  // (AI-assisted) after it
  }
  Job job;
  job.name = lev.name;
  job.detail = true;
  job.generation = lev.generation;
  job.path = lev.path;
  job.vertex_data_offset = lev.vertex_data_offset;
  job.index_data_offset = lev.index_data_offset;
  collect_chunks(lev, false, &job.chunks);
  if (job.chunks.empty()) {
    lev.detail_loaded = true;
    return;
  }
  lev.detail_requested = true;
  lg::info("[ctr] {}: camera coming back, reading {} detail meshes", lev.name, job.chunks.size());
  request(job);
}

void CtrLevels::request_repair(CtrLevelData& lev) {
  Job job;
  job.name = lev.name;
  job.repair = true;
  job.generation = lev.generation;
  job.path = lev.path;
  job.vertex_data_offset = lev.vertex_data_offset;
  job.index_data_offset = lev.index_data_offset;
  job.merc_vertex_offset = lev.merc_vertex_offset;
  job.merc_index_offset = lev.merc_index_offset;
  collect_chunks(lev, true, &job.chunks);
  for (size_t mi = 0; mi < lev.merc_models.size(); mi++) {
    const auto& m = lev.merc_models[mi];
    if (m.mesh < 0 && m.index_end > m.index_first) {
      job.merc.push_back({(u32)mi, m.file_vertex, m.vertex_count, m.index_first, m.index_end});
    }
  }
  std::sort(job.merc.begin(), job.merc.end(), [](const RepairMerc& a, const RepairMerc& b) {
    return a.file_vertex < b.file_vertex;
  });
  job.textures = lev.tex_pool < 0 && lev.tex_pool_bytes > 0;
  lev.repair_requested = true;
  request(job);
}

fs::path ctr_level_c3l_dir() {
  // (AI-assisted) the running game's: out/jak1/c3l, out/jak2/c3l
  return file_util::get_jak_project_dir() / "out" / version_to_game_name(g_game_version) / "c3l";
}

// (AI-assisted) Jak II: the level of a DGO file (its nickname, as the game asks for it: "CTA.DGO"
// is ctysluma), from goal_src/jak2/engine/level/level-info.gc's level-load-info :nickname / :name.
// nullptr: not a level's DGO.
const char* ctr_jak2_level_for_dgo(const char* dgo_name) {
  static const struct {
    const char* dgo;
    const char* level;
  } kLevels[] = {
      {"INT", "intro"}, {"DEMO", "demo"}, {"TITLE", "title"}, {"VIN", "vinroom"}, {"DMI", "drillmid"},
      {"DRI", "drill"}, {"DRB", "drillb"}, {"DRILLMTN", "drillmtn"}, {"SEW", "sewer"},
      {"SEB", "sewerb"}, {"SWE", "sewesc"}, {"SWB", "sewescb"}, {"TOA", "tomba"}, {"TOB", "tombb"},
      {"TOC", "tombc"}, {"TOD", "tombd"}, {"TOE", "tombe"}, {"TOMBEXT", "tombext"},
      {"TBO", "tombboss"}, {"UND", "under"}, {"UNB", "underb"}, {"PAC", "palcab"},
      {"PAS", "palshaft"}, {"PALBOSS", "palboss"}, {"PAR", "palroof"}, {"PALOUT", "palout"},
      {"THR", "throne"}, {"LBRNERMK", "lbrnermk"}, {"LASHTHRN", "lashthrn"}, {"LTHRNOUT", "lthrnout"},
      {"PAE", "palent"}, {"PRI", "prison"}, {"LDJAKBRN", "ldjakbrn"}, {"LPRSNCST", "lprsncst"},
      {"FEA", "forexita"}, {"FEB", "forexitb"}, {"FRA", "forresca"}, {"FRB", "forrescb"},
      {"FDA", "fordumpa"}, {"FDB", "fordumpb"}, {"FORDUMPC", "fordumpc"}, {"FORDUMPD", "fordumpd"},
      {"STR", "strip"}, {"RUI", "ruins"}, {"SAG", "sagehut"}, {"ATO", "atoll"}, {"ATE", "atollext"},
      {"MTN", "mountain"}, {"MTX", "mtnext"}, {"FOR", "forest"}, {"FOB", "forestb"},
      {"MCN", "mincan"}, {"CWI", "ctywide"}, {"LWIDEA", "lwidea"}, {"LWIDEB", "lwideb"},
      {"LWIDEC", "lwidec"}, {"CTYKORA", "ctykora"}, {"CTYASHA", "ctyasha"}, {"CGA", "ctygena"},
      {"CGB", "ctygenb"}, {"CGC", "ctygenc"}, {"CTA", "ctysluma"}, {"CTB", "ctyslumb"},
      {"CTC", "ctyslumc"}, {"CPO", "ctyport"}, {"LJKDXASH", "ljkdxash"}, {"CFA", "ctyfarma"},
      {"CFB", "ctyfarmb"}, {"CIA", "ctyinda"}, {"COA", "consite"}, {"COB", "consiteb"},
      {"CIB", "ctyindb"}, {"CMA", "ctymarka"}, {"CMB", "ctymarkb"}, {"CPA", "ctypal"},
      {"STA", "stadium"}, {"STB", "stadiumb"}, {"STC", "stadiumc"}, {"STD", "stadiumd"},
      {"SKA", "skatea"}, {"GARAGE", "garage"}, {"STADBLMP", "stadblmp"}, {"LWIDESTA", "lwidesta"},
      {"LERROL", "lerrol"}, {"LKEIRIFT", "lkeirift"}, {"LRACELIT", "lracelit"},
      {"LRACEBB", "lracebb"}, {"LRACEBF", "lracebf"}, {"LRACECB", "lracecb"}, {"LRACECF", "lracecf"},
      {"LRACEDB", "lracedb"}, {"LRACEDF", "lracedf"}, {"LGARCSTA", "lgarcsta"},
      {"LSAMERGD", "lsamergd"}, {"LERBRNGD", "lerbrngd"}, {"LSMYSBRT", "lsmysbrt"},
      {"LASHGRD", "lashgrd"}, {"ONINTENT", "onintent"}, {"LTENTOUT", "ltentout"},
      {"LTENTOB", "ltentob"}, {"KIOSK", "kiosk"}, {"ORACLE", "oracle"}, {"HIDEOUT", "hideout"},
      {"LTRNTESS", "ltrntess"}, {"LTRNKRKD", "ltrnkrkd"}, {"LTRNYSAM", "ltrnysam"},
      {"LYSAMSAM", "lysamsam"}, {"LYSKDCD", "lyskdcd"}, {"LKIDDOGE", "lkiddoge"},
      {"LHELLDOG", "lhelldog"}, {"LPACKAGE", "lpackage"}, {"LSACK", "lsack"},
      {"LPORTRUN", "lportrun"}, {"LSHUTTLE", "lshuttle"}, {"LMEETBRT", "lmeetbrt"},
      {"LPOWER", "lpower"}, {"LERLCHAL", "lerlchal"}, {"LPRTRACE", "lprtrace"},
      {"LBOMBBOT", "lbombbot"}, {"LBBUSH", "lbbush"}, {"LPROTECT", "lprotect"}, {"HIPHOG", "hiphog"},
      {"LTESS", "ltess"}, {"LHIPOUT", "lhipout"}, {"LWHACK", "lwhack"}, {"LGUARD", "lguard"},
      {"LCGUARD", "lcguard"}, {"LERLTESS", "lerltess"}, {"GGA", "gungame"}, {"DG1", "dig1"},
      {"D3A", "dig3a"}, {"D3B", "dig3b"}, {"CAP", "caspad"}, {"CAS", "castle"}, {"CAB", "casboss"},
      {"CASEXT", "casext"}, {"CASCITY", "cascity"}, {"VI1", "village1"}, {"INTROCST", "introcst"},
      {"LCITYLOW", "lcitylow"}, {"LINTCSTB", "lintcstb"}, {"LJAKDAX", "ljakdax"}, {"NES", "nest"},
      {"NEB", "nestb"}, {"OUTROCST", "outrocst"}, {"PORTWALL", "portwall"}, {"LOUTCSTB", "loutcstb"},
      {"ISLAND1", "island1"}, {"SKATEPARK", "skatepark"}, {"HALFPIPE", "halfpipe"},
      {"VIS", "vistest"}, {"WOODSTEST", "woodstest"}, {"TOBYTEST", "tobytest"},
      {"CHARTEST", "chartest"}, {"DPT", "dptest"}, {"CTYFENCE", "ctyfence"}, {"4AARON", "4aaron"},
      {"4PAL01", "4pal01"}, {"ANDREW01", "andrew01"}, {"BSBS", "bsbs"}, {"EITEST", "eitest"},
      {"MIKETEST", "miketest"}, {"TATETEST", "tatetest"}, {"TESTSTDC", "teststdc"},
      {"TESTSTDD", "teststdd"}, {"WASALL", "wasall"}, {"STADOCC", "stadocc"}, {"TSZ", "test-zone"},
  };
  if (!dgo_name) {
    return nullptr;
  }
  std::string n(dgo_name);
  const auto dot = n.find('.');
  if (dot != std::string::npos) {
    n.resize(dot);
  }
  for (auto& c : n) {
    c = (char)toupper((unsigned char)c);
  }
  for (const auto& l : kLevels) {
    if (n == l.dgo) {
      return l.level;
    }
  }
  return nullptr;
}

CtrLevels::LoadResult CtrLevels::load(const std::string& name, CtrLevelData* out) {
  // (AI-assisted) out/jak1/c3l or out/jak2/c3l (the running game's)
  auto path = ctr_level_c3l_dir() / (name + ".c3l");
  if (!fs::exists(path)) {
    return LoadResult::MISSING;
  }
  Timer timer;
  bool ok = false;
  long file_size = 0;
  double decode_ms = 0;
#ifdef __3DS__
  ctr_heap_trace_dump(("before " + name).c_str());  // (AI-assisted) trace builds only
#endif
  try {
#ifndef __3DS__
    // (AI-assisted) test hook (host): OPENGOAL_CTR_FAIL_LOAD=NAME[:S] fails NAME's loads for S
    // seconds (default 0: only the first) from its first load on, as if the heap ran out (the
    // retry path)
    if (const char* fail = getenv("OPENGOAL_CTR_FAIL_LOAD")) {
      static double s_first_ms = -1;
      const char* colon = strchr(fail, ':');
      const std::string fail_name = colon ? std::string(fail, colon - fail) : std::string(fail);
      if (name == fail_name) {
        const double now = ctr_gpu_time_ms();
        if (s_first_ms < 0) {
          s_first_ms = now;
          throw std::bad_alloc();
        }
        if (colon && now - s_first_ms < atof(colon + 1) * 1000.0) {
          throw std::bad_alloc();
        }
      }
    }
#endif
    ok = load_file(path, name, out, &file_size, &decode_ms);
  } catch (std::exception& e) {
    std::string heap;
#ifdef __3DS__
    CtrHeapStats st;
    ctr_heap_stats(&st);
    heap = fmt::format(" (heap: {} KB free, largest free block {} KB)", st.free / 1024,
                       st.largest / 1024);
#endif
    lg::error("[ctr] failed to load {}: {}{}; trying again when memory frees", path.string(),
              e.what(), heap);
  }
#ifdef __3DS__
  ctr_heap_trace_dump(((ok ? "after " : "failed ") + name).c_str());
#endif
  if (!ok) {
    unload(*out);
    if (cancelled()) {
      lg::info("[ctr] load of {} cancelled (no longer wanted)", name);
      return LoadResult::CANCELLED;
    }
    return LoadResult::FAILED;
  }
  int failed = 0;
  for (int t : out->textures) {
    failed += t < 0;
  }
  failed -= std::min<int>(failed, (int)out->tex_empty);  // (AI-assisted) v15: none to make
  for (size_t i = 0; i < out->meshes.size(); i++) {
    failed += out->meshes[i] < 0 && out->chunks[i].draw_count > 0 && !out->is_unit(i);
  }
  for (auto& m : out->merc_models) {
    failed += m.mesh < 0;
  }
  // (AI-assisted) where the time went (file reads, decompressing, the rest: building meshes and
  // textures, waiting for the core) and the linear memory left with this level in
  std::string mem;
#ifdef __3DS__
  mem = fmt::format("; linear free {} KB", linearSpaceFree() / 1024);
#endif
  lg::info(
      "[ctr] loaded {} ({} chunks, {} merc models, {} textures: {} KB, background {} KB first; {} "
      "KB) in {:.0f} ms{}; reading {:.0f} ms in {} reads of {} KB{}",
      name, out->chunks.size(), out->merc_models.size(), out->textures.size(),
      out->tex_pool_bytes / 1024, out->tex_background_bytes / 1024, file_size / 1024,
      timer.getMs(), decode_ms > 0 ? fmt::format(" (decompressing {:.0f} ms)", decode_ms) : "",
      m_last_read_ms, m_last_reads, m_last_read_bytes / 1024, mem);
  lg::info("[ctr] {}: {}", name, level_mem_text(level_mem(*out)));  // (AI-assisted, j2-mem)
#ifndef __3DS__
  if (getenv("CTR_MEM_FIELDS")) {
    // (AI-assisted, j2-mem) host: the level's biggest tables
    const auto& L = *out;
    std::vector<std::pair<u64, const char*>> f = {
        {vec_bytes(L.chunks), "chunks"}, {vec_bytes(L.draws), "draws"},
        {vec_bytes(L.draw_states), "draw_states"}, {vec_bytes(L.vis_runs), "vis_runs"},
        {vec_bytes(L.run_spheres), "run_spheres"}, {vec_bytes(L.run_lod), "run_lod"},
        {vec_bytes(L.draw_first_run) + vec_bytes(L.draw_run_count), "draw runs"},
        {vec_bytes(L.sorted_draws) + vec_bytes(L.ordered_draws) + vec_bytes(L.early_draws) +
             vec_bytes(L.draw_chunk) + vec_bytes(L.draw_alpha80),
         "draw order"},
        {vec_bytes(L.lod_groups) + vec_bytes(L.lod_state) + vec_bytes(L.lod_error), "lod groups"},
        {vec_bytes(L.stream_units) + vec_bytes(L.group_units) + vec_bytes(L.unit_groups) +
             vec_bytes(L.group_unit_first) + vec_bytes(L.unit_group_first),
         "stream units"},
        {L.tod ? (u64)L.tod->bytes() : 0, "tod"},
        {L.tod ? vec_bytes(L.tod->stamp) + vec_bytes(L.tod->table) : 0, "tod table+stamp"},
        {L.tod ? vec_bytes(L.tod->groups) + vec_bytes(L.tod->run_group) : 0, "tod groups"},
        {L.fine ? (u64)L.fine->bytes() : 0, "fine_vis"},
        {L.file_dir ? vec_bytes(L.file_dir->blocks) : 0, "file dir"},
        {vec_bytes(L.blerc_verts) + vec_bytes(L.blerc_targets) + vec_bytes(L.blerc_dests), "blerc"},
    };
    std::string t;
    for (auto& [b, n] : f) {
      t += fmt::format(" {} {}", n, b / 1024);
    }
    lg::info("[ctr] {} heap KB:{}; {} draws, {} runs, {} groups", name, t, L.draws.size(),
             L.vis_runs.size(), L.lod_groups.size());
  }
#endif
  if (failed) {
    lg::error("[ctr] {}: {} textures/meshes could not be created (out of GPU memory?)", name,
              failed);
  }
  // (AI-assisted) made again when the memory is there (process_pending_loads)
  u32 missing_bytes = 0;
  const int missing = missing_parts(*out, &missing_bytes);
  out->incomplete = missing > 0;
  if (missing) {
    lg::warn("[ctr] {}: {} parts ({} KB) left out, made again when memory frees", name, missing,
             missing_bytes / 1024);
  }
  return LoadResult::LOADED;
}

namespace {
// (AI-assisted) the smallest texel alpha of a texture's first mip level (0xff = 1.0), in the GPU
// layout as stored in the pool: for the early depth test (an alpha test that never fails)
int texture_min_alpha(const u8* data, int w, int h, int format) {
  const int n = w * h;
  int m = 255;
  switch (format) {
    case CTR_TEX_RGB565:
    case CTR_TEX_ETC1:
      return 255;
    case CTR_TEX_RGBA4:
      for (int i = 0; i < n; i++) {
        m = std::min(m, (data[2 * i] & 0xf) * 17);  // u16, alpha in the low 4 bits
      }
      return m;
    case CTR_TEX_ETC1A4:
      // 4x4 blocks: 8 bytes of 4-bit alpha, then the ETC1 block
      for (int b = 0; b < n / 16; b++) {
        for (int k = 0; k < 8; k++) {
          const u8 v = data[16 * b + k];
          m = std::min({m, (v & 0xf) * 17, (v >> 4) * 17});
        }
      }
      return m;
    case CTR_TEX_RGBA8:
      for (int i = 0; i < n; i++) {
        m = std::min<int>(m, data[4 * i]);  // stored A, B, G, R
      }
      return m;
    default:
      return 0;
  }
}

// (AI-assisted) the weights each level was last blended with (by name: a level loaded again starts
// with them, CtrLevels::load_file), and the last blend's of any level with its day palettes
// (Tod::palettes without the lights: a level loaded for the first time with the same ones, Sandover
// and its neighbours, starts with those)
struct TodSeen {
  std::mutex lock;
  std::map<std::string, std::array<u8, 24>> by_level;
  std::array<u8, 24> last{};
  u32 last_day_mask = 0;
  std::map<std::string, u8> pulse;  // (AI-assisted, citadel) per level its pulsing palettes
};
TodSeen& tod_seen() {
  static TodSeen s;
  return s;
}
u32 tod_day_mask(const CtrLevelData::Tod& t) {
  u32 m = 0;
  for (u32 k = 0; k < t.palette_count; k++) {
    m |= 1u << t.palettes[k];
  }
  return m & ~t.light_mask;
}

// (AI-assisted) c3l v16 time of day. The game's weight (64 = 1.0) of palette p for channel c (r, g,
// b), from the camera's itimes as interp_time_of_day_slow (background_common.cpp) reads them.
void tod_weights(const CtrBackgroundCamera& cam, u8 w[8][3]) {
  for (int p = 0; p < 8; p++) {
    for (int c = 0; c < 3; c++) {
      const u32 word = (u32)cam.itimes[p / 2][(p % 2) * 2 + c / 2];
      w[p][c] = (u8)((c % 2 ? word >> 16 : word) & 0xff);
    }
  }
}

// (AI-assisted) render thread: the time of day work of this frame (blends and rewrites), taken out
// of auto_detail's build time (ctr_level_tod_take_ms): it is not GPU load, and counting it pulled
// the tie / shrub distances in while the title screen's day ran
double g_tod_frame_ms = 0;

// a blended color times a TodMul (shrubs' base color, the untextured vista's texture average)
inline u32 tod_mul_rgb(u32 c, const c3l::TodMul& m) {
  return std::min<u32>(255, ((c & 0xff) * m.mul[0] + 64) >> 7) |
         std::min<u32>(255, (((c >> 8) & 0xff) * m.mul[1] + 64) >> 7) << 8 |
         std::min<u32>(255, (((c >> 16) & 0xff) * m.mul[2] + 64) >> 7) << 16;
}

// The palettes a blend with weights w adds (weight not 0).
void tod_make_act(const CtrLevelData::Tod& t, const u8 (*w)[3], CtrLevelData::Tod::Act* a) {
  a->n = 0;
  for (u32 k = 0; k < t.palette_count; k++) {
    const u8* x = w[t.palettes[k]];
    if (x[0] | x[1] | x[2]) {
      a->slot3[a->n] = (u8)(k * 3);
      memcpy(a->w[a->n], x, 3);
      a->n++;
    }
  }
}

// One color blended: per channel min(255, sum(w * c) >> 6), as the PC (interp_time_of_day_slow).
inline u32 tod_blend_rgb(const CtrLevelData::Tod& t, u32 c, const CtrLevelData::Tod::Act& a) {
  const u8* src = t.colors.data() + (size_t)c * t.palette_count * 3;
  u32 r = 0, g = 0, b = 0;
  for (u32 i = 0; i < a.n; i++) {
    const u8* x = src + a.slot3[i];
    r += a.w[i][0] * x[0];
    g += a.w[i][1] * x[1];
    b += a.w[i][2] * x[2];
  }
  return std::min<u32>(255, r >> 6) | std::min<u32>(255, g >> 6) << 8 |
         std::min<u32>(255, b >> 6) << 16;
}

// Table entry idx blended again (it was older than its palettes' last move).
__attribute__((noinline)) u32 tod_entry_slow(CtrLevelData::Tod& t, u32 idx) {
  const u32 nc = t.color_count;
  u32 rgb;
  if (idx < nc) {
    rgb = tod_blend_rgb(t, idx, t.act);
  } else {
    const auto& m = t.muls[idx - nc];
    rgb = tod_mul_rgb(tod_blend_rgb(t, m.color, t.act), m);
  }
  t.table[idx] = rgb;
  t.stamp[idx] = t.ver;
  return rgb;
}
inline u32 tod_entry(CtrLevelData::Tod& t, u32 idx) {
  return t.stamp[idx] >= t.lut[t.color_mask[idx]] ? t.table[idx] : tod_entry_slow(t, idx);
}

// Every level draw: the game's weights into target_w (the table's entries follow lazily, exact as
// the PC). Day palettes taken in a frame with config tod_busy_half every second frame only while
// they jumped (moved by more than kTodJump in a frame) in each of the last kTodBusyFrames level
// draws (sunken's cycling lights). Returns 2: day palettes taken, 1: only light palettes, 0:
// nothing.
constexpr int kTodBusyFrames = 8;
constexpr int kTodJump = 4;
constexpr u64 kTodPulseFrames = 256, kTodPulseSettle = 8;  // (AI-assisted, citadel)
constexpr int kTodTakeJump = 8;  // (AI-assisted, citadel) tod_max_ms: a day move this big doesn't wait
int tod_update(CtrLevelData::Tod& t, const std::string& level, const CtrBackgroundCamera& cam,
               u64 frame) {
  u8 w[8][3];
  tod_weights(cam, w);
  // (palettes no color uses don't matter)
  u32 used = 0;
  for (u32 k = 0; k < t.palette_count; k++) {
    used |= 1u << t.palettes[k];
  }
  // (AI-assisted, citadel) config tod_pulse_step: a day palette whose weight turned around twice
  // within kTodPulseFrames (a mood light's pulse: citadel's palette 1, 0.875 +- 0.25 every 128
  // frames, 1/64 every ~2 frames, 71% of its colors) is taken only when it moved tod_pulse_step
  // 64ths from the weight last taken, or once it stood still for kTodPulseSettle frames: each move
  // rewrites the vertices drawn (citadel: 4-8 ms per frame, all of them every ~6 frames). The
  // day's own moves (1/64 every ~40 frames, one way for hours) and the lights as before. A palette
  // found pulsing is remembered per level (TodSeen::pulse): a reload counts it as pulsing at once.
  const int pulse_step = ctr_settings().tod_pulse_step;
  if (!t.first_frame) {
    t.first_frame = frame;
  }
  auto pulsing = [&](int p) {
    return (t.pulse_rev[p][1] && frame - t.pulse_rev[p][1] < kTodPulseFrames) ||
           ((t.pulse_known >> p & 1) &&
            frame - std::max(t.pulse_rev[p][0], t.first_frame) < kTodPulseFrames);
  };
  if (pulse_step > 0 && t.blended_valid) {
    for (u32 k = 0; k < t.palette_count; k++) {
      const int p = t.palettes[k];
      if ((t.light_mask >> p & 1) || memcmp(t.weights[p], w[p], 3) == 0) {
        continue;
      }
      int d = 0;
      for (int c = 0; c < 3; c++) {
        d += (int)w[p][c] - (int)t.weights[p][c];
      }
      const u8 dir = d > 0 ? 1 : d < 0 ? 2 : 0;
      if (dir && t.pulse_dir[p] && dir != t.pulse_dir[p]) {
        t.pulse_rev[p][1] = t.pulse_rev[p][0];
        t.pulse_rev[p][0] = frame;
        if (t.pulse_rev[p][1] && frame - t.pulse_rev[p][1] < kTodPulseFrames &&
            !(t.pulse_known >> p & 1)) {
          t.pulse_known |= (u8)(1u << p);
          auto& seen = tod_seen();
          std::lock_guard<std::mutex> lk(seen.lock);
          seen.pulse[level] |= (u8)(1u << p);
        }
      }
      if (dir) {
        t.pulse_dir[p] = dir;
      }
      t.pulse_move[p] = frame;
    }
  }
  u32 changed = 0, day = 0;
  int jump = 0;
  // (AI-assisted, citadel) config tod_max_ms: a day move waits for min_move_frames after the last
  const bool wait_day = ctr_settings().tod_max_ms > 0 && t.blended_valid &&
                        (float)(frame - t.move_frame) < t.min_move_frames;
  bool waited = false;
  for (u32 k = 0; k < t.palette_count; k++) {
    const int p = t.palettes[k];
    if (pulse_step > 0 && t.blended_valid && !(t.light_mask >> p & 1) && pulsing(p) &&
        frame - t.pulse_move[p] < kTodPulseSettle && memcmp(t.target_w[p], w[p], 3) != 0) {
      int diff = 0;
      for (int c = 0; c < 3; c++) {
        diff = std::max(diff, std::abs((int)w[p][c] - (int)t.target_w[p][c]));
      }
      if (diff < pulse_step) {
        t.pulse_held++;
        continue;  // (held: target_w stays)
      }
    }
    if (wait_day && !(t.light_mask >> p & 1) && memcmp(t.target_w[p], w[p], 3) != 0) {
      // (tod_max_ms: the last day move's rewrites aren't spread yet; a jump is taken at once)
      int diff = 0;
      for (int c = 0; c < 3; c++) {
        diff = std::max(diff, std::abs((int)w[p][c] - (int)t.target_w[p][c]));
      }
      if (diff < kTodTakeJump) {
        waited = true;
        continue;
      }
    }
    if (!t.blended_valid || memcmp(t.target_w[p], w[p], 3) != 0) {
      changed |= 1u << p;
      if (!(t.light_mask >> p & 1)) {
        day |= 1u << p;
        for (int c = 0; c < 3; c++) {
          jump = std::max(jump, std::abs((int)w[p][c] - (int)t.weights[p][c]));
        }
      }
    }
  }
  memcpy(t.weights, w, sizeof(w));
  t.day_waited = waited;
  if (t.blended_valid && day) {
    t.busy_run = jump > kTodJump ? (u8)std::min(255, t.busy_run + 1) : 0;
    if (ctr_settings().tod_busy_half && (frame & 1) && t.busy_run >= kTodBusyFrames) {
      changed &= ~day;  // (next frame)
      day = 0;
    }
  } else if (!day) {
    t.busy_run = 0;
  }
  t.blended_valid = true;
  if (!changed) {
    return 0;
  }
  for (u32 c = changed; c; c &= c - 1) {
    const int p = __builtin_ctz(c);
    memcpy(t.target_w[p], w[p], 3);
  }
  t.ver++;
  for (u32 c = changed; c; c &= c - 1) {
    t.pal_ver[__builtin_ctz(c)] = t.ver;
  }
  t.lut[0] = 0;
  for (u32 m = 1; m < 256; m++) {
    t.lut[m] = std::max(t.lut[m & (m - 1)], t.pal_ver[__builtin_ctz(m)]);
  }
  tod_make_act(t, t.target_w, &t.act);
  if (day) {
    // frames per day move (the group pass's budget)
    const float since = (float)std::min<u64>(64, frame - t.move_frame);
    t.move_frames = 0.75f * t.move_frames + 0.25f * std::max(1.f, since);
    t.move_frame = frame;
    t.dver++;
    auto& seen = tod_seen();
    std::lock_guard<std::mutex> lk(seen.lock);
    auto& wl = seen.by_level[level];
    memcpy(wl.data(), t.target_w, 24);
    seen.last = wl;
    seen.last_day_mask = tod_day_mask(t);
    return 2;
  }
  return 1;
}

// may_alias words of a vertex: [1] >> 16 is Vertex::pad, [3] Vertex::rgba (little endian)
typedef u32 __attribute__((may_alias)) tod_word;
static_assert(offsetof(c3l::Vertex, pad) == 6 && offsetof(c3l::Vertex, rgba) == 12);
static_assert(sizeof(c3l::Vertex) == 16);

// Vertices [first, end) from the table (the alpha stays; plain: every one has a time of day color).
inline void tod_write_range(CtrLevelData::Tod& t, c3l::Vertex* v, int first, int end, bool plain) {
  const u32* const tab = t.table.data();
  const u32* const stamp = t.stamp.data();
  const u8* const cm = t.color_mask.data();
  const u32* const lut = t.lut;
  tod_word* w = (tod_word*)(v + first);
  tod_word* const e = (tod_word*)(v + end);
  if (plain) {
    for (; w < e; w += 4) {
      const u32 idx = w[1] >> 16;
      const u32 rgb = stamp[idx] >= lut[cm[idx]] ? tab[idx] : tod_entry_slow(t, idx);
      w[3] = (w[3] & 0xff000000u) | rgb;
    }
    return;
  }
  const u32 ntab = (u32)t.table.size();
  for (; w < e; w += 4) {
    const u32 idx = w[1] >> 16;
    if (idx < ntab) {
      const u32 rgb = stamp[idx] >= lut[cm[idx]] ? tab[idx] : tod_entry_slow(t, idx);
      w[3] = (w[3] & 0xff000000u) | rgb;
    }
  }
}

void tod_color_vertices(CtrLevelData::Tod& t, c3l::Vertex* v, int first, int end) {
  tod_write_range(t, v, first, end, false);
}

// The palettes the chunk's vertices use (0: no time of day color) and its vertices with a light
// color, by the light palettes they use (the light rewrites).
u8 tod_scan(const CtrLevelData::Tod& t, const c3l::Vertex* v, int n, CtrLevelData::Tod::LightList* lights) {
  u32 mask = 0;
  bool any = false;
  const u32 nc = t.color_count, nm = (u32)t.muls.size();
  std::vector<std::pair<u8, u16>> lv;
  for (int i = 0; i < n; i++) {
    const u32 idx = (u16)v[i].pad;
    if (idx >= nc + nm) {
      continue;
    }
    const u32 m = t.color_mask[idx];
    mask |= m;
    any = true;
    if (m & t.light_mask) {
      lv.emplace_back((u8)(m & t.light_mask), (u16)i);
    }
  }
  std::stable_sort(lv.begin(), lv.end(),
                   [](const auto& a, const auto& b) { return a.first < b.first; });
  lights->verts.clear();
  lights->groups.clear();
  for (size_t k = 0; k < lv.size(); k++) {
    lights->verts.push_back(lv[k].second);
    if (k + 1 == lv.size() || lv[k + 1].first != lv[k].first) {
      lights->groups.emplace_back(lv[k].first, (u32)k + 1);
    }
  }
  // (all black colors: palette 0 stands for them, so the chunk counts as colored)
  return any ? (u8)(mask ? mask : 1) : 0;
}

// The chunk's vertex groups: one per visibility id of its runs (c3l v18 files keep each id's
// vertices together and apart from the others' vertices), when their vertex ranges don't overlap;
// else one for all of its vertices. idst: the chunk's indices [idx_first, idx_end).
void tod_make_groups(CtrLevelData* out, u32 ci, const u16* idst, u32 idx_first, u32 idx_end) {
  auto& t = *out->tod;
  const auto& ch = out->chunks[ci];
  using G = CtrLevelData::Tod::VisGroup;
  t.chunk_group_first[ci] = (u32)t.groups.size();
  const auto one = [&]() {
    t.groups.resize(t.chunk_group_first[ci]);
    t.groups.push_back(G{0, (u16)(std::max<u32>(ch.vertex_count, 1) - 1), 0});
    t.chunk_group_count[ci] = 1;
    const u32 r0 = out->chunk_first_run[ci];
    for (u32 r = r0; r < r0 + ch.vis_run_count && r < t.run_group.size(); r++) {
      t.run_group[r] = 0;
    }
  };
  if (!idst || !ch.vis_run_count || ch.vertex_count < 2) {
    one();
    return;
  }
  std::vector<std::pair<u16, u16>> ids;  // vis id, group
  std::vector<G> gs;
  for (u32 k = 0; k < ch.draw_count && ch.first_draw + k < out->draws.size(); k++) {
    const u32 di = ch.first_draw + k;
    u32 idx = out->draws[di].first_index;
    for (u32 r = out->draw_first_run[di]; r < out->draw_first_run[di] + out->draw_run_count[di];
         r++) {
      const u32 n = out->vis_runs[r].index_count;
      if (idx < idx_first || idx + n > idx_end || r >= t.run_group.size()) {
        one();
        return;
      }
      const u16 vis = out->vis_runs[r].vis;
      u16 g = 0xffff;
      for (const auto& [id, gi] : ids) {
        if (id == vis) {
          g = gi;
          break;
        }
      }
      if (g == 0xffff) {
        g = (u16)gs.size();
        ids.emplace_back(vis, g);
        gs.push_back(G{0xffff, 0, 0});
      }
      t.run_group[r] = g;
      for (u32 i = idx; i < idx + n; i++) {
        const u16 v = idst[i - idx_first];
        gs[g].lo = std::min(gs[g].lo, v);
        gs[g].hi = std::max(gs[g].hi, v);
      }
      idx += n;
    }
  }
  if (gs.size() < 2 || gs.size() > 0xfffe) {
    one();
    return;
  }
  std::vector<std::pair<u16, u16>> by_lo;
  for (size_t g = 0; g < gs.size(); g++) {
    by_lo.emplace_back(gs[g].lo, gs[g].hi);
  }
  std::sort(by_lo.begin(), by_lo.end());
  for (size_t k = 1; k < by_lo.size(); k++) {
    if (by_lo[k].first <= by_lo[k - 1].second) {
      one();  // overlapping (older files: vertices in first use order)
      return;
    }
  }
  // in vertex order (the light rewrites walk the groups with the vertices)
  std::vector<u16> order(gs.size()), slot(gs.size());
  for (size_t g = 0; g < gs.size(); g++) {
    order[g] = (u16)g;
  }
  std::sort(order.begin(), order.end(), [&](u16 a, u16 b) { return gs[a].lo < gs[b].lo; });
  for (size_t k = 0; k < order.size(); k++) {
    slot[order[k]] = (u16)k;
    t.groups.push_back(gs[order[k]]);
  }
  const u32 r0 = out->chunk_first_run[ci];
  for (u32 r = r0; r < r0 + ch.vis_run_count && r < t.run_group.size(); r++) {
    t.run_group[r] = slot[t.run_group[r]];
  }
  t.chunk_group_count[ci] = (u16)gs.size();
}

// (AI-assisted, streaming) a near chunk not read at load: room in Tod::groups for its groups (one
// per visibility id of its runs: the most tod_make_groups makes), filled when it is first read
// (CtrLevels::take_stream); one group for all of it until then.
void tod_reserve_unit(CtrLevelData* out, u32 ci) {
  auto& t = *out->tod;
  const auto& ch = out->chunks[ci];
  std::vector<u16> ids;
  const u32 r0 = out->chunk_first_run[ci];
  for (u32 r = r0; r < r0 + ch.vis_run_count && r < out->vis_runs.size(); r++) {
    if (std::find(ids.begin(), ids.end(), out->vis_runs[r].vis) == ids.end()) {
      ids.push_back(out->vis_runs[r].vis);
    }
  }
  const u32 n = std::max<u32>(1, std::min<u32>((u32)ids.size(), 0xfffe));
  using G = CtrLevelData::Tod::VisGroup;
  t.chunk_group_first[ci] = (u32)t.groups.size();
  t.groups.resize(t.groups.size() + n, G{0, (u16)(std::max<u32>(ch.vertex_count, 1) - 1), 0});
  t.chunk_group_count[ci] = 1;
  for (u32 r = r0; r < r0 + ch.vis_run_count && r < t.run_group.size(); r++) {
    t.run_group[r] = 0;
  }
  out->stream_units[out->chunk_unit[ci]].tod_slots = (u16)n;
}

// The chunk's groups that are plain (every vertex with a time of day color: tod_write_range's fast
// loop).
void tod_mark_plain(CtrLevelData::Tod& t, u32 ci, const c3l::Vertex* v, int n) {
  const u32 ntab = (u32)t.table.size();
  for (u32 g = t.chunk_group_first[ci]; g < t.chunk_group_first[ci] + t.chunk_group_count[ci]; g++) {
    auto& gr = t.groups[g];
    bool plain = true;
    for (int i = gr.lo; i <= std::min<int>(gr.hi, n - 1) && plain; i++) {
      plain = (u16)v[i].pad < ntab;
    }
    gr.plain = plain ? 1 : 0;
  }
}

// One level draw's rewrites: tod_recolor per chunk drawn in two passes (the light vertices, then
// the groups), then tod_finish.
// kTodBudgetMs: the frame's time of day work (all levels) the rewrites may take (Azahar ms; the
//   light pass up to kTodLightShare of it); the group pass gets more while the day moves fast
//   (kTodSpreadMoves). Groups kTodMaxLag day moves behind or with the file's colors don't wait.
// Oct 17, Azahar: a group of vertices costs ~80 ns per vertex, a clock read as much as ~20 vertices.
constexpr double kTodBudgetMs = 1.2;
constexpr double kTodLightShare = 0.7;
constexpr float kTodSpreadMoves = 2;
constexpr u32 kTodMaxLag = 3;
constexpr u32 kTodCheckWork = 1000, kTodChunkWork = 40;
constexpr int kTodLights = 0, kTodRound = 1;
inline double tod_budget_ms() {
#ifndef __3DS__
  // (AI-assisted) host: env CTR_TOD_BUDGET_MS (a smaller budget: more chunks drawn behind)
  static const double ms =
      getenv("CTR_TOD_BUDGET_MS") ? atof(getenv("CTR_TOD_BUDGET_MS")) : kTodBudgetMs;
  return ms;
#else
  return kTodBudgetMs;
#endif
}
struct TodPass {
  double t0 = 0;         // ctr_gpu_time_ms at the start
  double before_ms = 0;  // the frame's time of day ms before this pass
  // the pass: kTodLights (light vertices, up to kTodLightShare of the budget), kTodRound (groups)
  int pass = 0;
  bool over = false;     // the budget's used up
  bool capped = false;   // (AI-assisted, citadel) tod_max_ms reached: nothing more this frame
  u32 waiting = 0;       // groups too far behind left for tod_max_ms
  u32 work = 0, next_check = 0;  // vertices written (+ kTodChunkWork per chunk looked at)
  u32 stop_chunk = UINT32_MAX;  // the first chunk the budget didn't reach (the next round's start)
  u32 rewritten = 0, behind = 0;
  double day_budget_ms = 0;  // the budget of the group pass
  bool measure = false;  // (statistics, every 8th frame) the day moves the groups drawn are behind
  u32 max_lag = 0;
};
std::vector<std::pair<uintptr_t, uintptr_t>>& tod_dirty() {
  static std::vector<std::pair<uintptr_t, uintptr_t>> d;  // render thread
  return d;
}

// The chunk's vertex colors brought up to date in its mesh (in place: a frame the GPU still draws
// may see the change) before it is drawn (shown: the chunk's runs drawn this frame, nullptr: all of
// them). A vertex group (one visibility id's vertices) is always rewritten whole, when a day palette
// the chunk uses moved since (or it has the file's colors); its light vertices, all at once, when a
// light palette moved. Within the frame's budget: the light pass (from where the last frame's
// stopped, up to kTodLightShare of tod_budget_ms), then the groups (from where the last frame's
// budget ran out, TodPass::day_budget_ms); what the budget doesn't reach is drawn as it is, a day
// move or two behind: groups with the file's colors or kTodMaxLag moves behind are rewritten at
// once. Hidden groups wait until they are drawn. Returns the vertices written.
u32 tod_recolor(CtrLevelData& lev, size_t ci, const u8* shown, TodPass& ps) {
  auto& t = *lev.tod;
  const int mesh = ci < lev.meshes.size() ? lev.meshes[ci] : -1;
  if (mesh < 0 || ci >= t.chunk_mask.size() || !t.blended_valid || t.chunk_scanned[ci] == 1) {
    return 0;
  }
  const u32 g0 = t.chunk_group_first[ci], ng = t.chunk_group_count[ci];
  if (t.chunk_mesh[ci] != mesh) {
    // a mesh made after the load (its vertices as in the file)
    t.chunk_mesh[ci] = mesh;
    for (u32 g = g0; g < g0 + ng; g++) {
      t.groups[g].ver = 0;
      t.groups[g].dver = 0;
    }
    t.chunk_min_ver[ci] = t.chunk_min_dver[ci] = t.chunk_light_ver[ci] = 0;
  }
  // the last move of the chunk's day / light palettes
  const u32 m = t.chunk_mask[ci];
  const u32 dm = m & ~t.light_mask, lm = m & t.light_mask;
  const u32 day_last = std::max<u32>(1, t.lut[dm]), light_last = t.lut[lm];
  // quick out: every group up to date
  const bool day_ok = t.chunk_min_ver[ci] >= day_last;
  const bool light_ok = t.chunk_light_ver[ci] >= light_last;
  if (ps.capped && ps.pass != kTodLights && !ps.measure) {
    return 0;
  }
  if (t.chunk_scanned[ci] == 2 &&
      ((ps.pass == kTodLights && (light_ok || ps.over)) ||
       (ps.pass != kTodLights &&
        (day_ok ||
         // (the budget used up: only groups too far behind are rewritten)
         (ps.over && t.chunk_min_ver[ci] && t.dver - t.chunk_min_dver[ci] < kTodMaxLag &&
          !ps.measure))))) {
    return 0;
  }
  ps.work += kTodChunkWork;
  int n = 0;
  auto* v = (c3l::Vertex*)ctr_gpu_mesh_vertices_inplace(mesh, &n);
  if (!v || n <= 0) {
    return 0;
  }
  if (!t.chunk_scanned[ci]) {
    // no mesh at load: the palettes and light vertices now (render thread, once)
    CtrLevelData::Tod::LightList lights;
    const u8 mask = tod_scan(t, v, n, &lights);
    t.chunk_scanned[ci] = mask ? 2 : 1;
    t.chunk_mask[ci] = mask;
    if (!lights.verts.empty()) {
      t.chunk_lights[(u32)ci] = std::move(lights);
    }
    if (!mask) {
      return 0;
    }
    tod_mark_plain(t, (u32)ci, v, n);
    return tod_recolor(lev, ci, shown, ps);  // (the groups: one for all, made at load)
  }
  // (the clock read every kTodCheckWork vertices of work: a read costs about as much as 20
  // vertices in Azahar)
  auto budget_ok = [&]() {
    if (ps.over) {
      return false;
    }
    if (ps.work < ps.next_check) {
      return true;
    }
    if (ps.before_ms + (ctr_gpu_time_ms() - ps.t0) <
        (ps.pass == kTodLights ? tod_budget_ms() * kTodLightShare : ps.day_budget_ms)) {
      ps.next_check = ps.work + kTodCheckWork;
      return true;
    }
    ps.stop_chunk = (u32)ci;
    ps.over = true;
    return false;
  };
  // (AI-assisted, citadel) the frame's limit (tod_max_ms) for the groups that don't wait for the
  // budget
  auto cap_ok = [&]() {
    const float cap = ctr_settings().tod_max_ms;
    if (cap <= 0) {
      return true;
    }
    if (ps.before_ms + (ctr_gpu_time_ms() - ps.t0) < cap) {
      return true;
    }
    ps.capped = true;
    if (ps.stop_chunk == UINT32_MAX) {
      ps.stop_chunk = (u32)ci;  // (else the budget's stop chunk stays the next frame's start)
    }
    return false;
  };
  const u32 r0 = lev.chunk_first_run[ci];
  const u32 nr = lev.chunks[ci].vis_run_count;
  const bool by_runs = shown && ng > 1;
  u32 looked = 0;
  int flo = n, fhi = -1;
  // the light pass: its light vertices (the lamps move every frame)
  if (ps.pass == kTodLights && !light_ok && budget_ok()) {
    u32 moved = 0;
    for (u32 mm = lm; mm; mm &= mm - 1) {
      const int p = __builtin_ctz(mm);
      moved |= t.pal_ver[p] > t.chunk_light_ver[ci] ? 1u << p : 0u;
    }
    t.chunk_light_ver[ci] = t.ver;
    auto it = moved ? t.chunk_lights.find((u32)ci) : t.chunk_lights.end();
    if (it != t.chunk_lights.end()) {
      const auto& lights = it->second;
      tod_word* const w = (tod_word*)v;
      const u16* const vl = lights.verts.data();
      u32 begin = 0;
      for (const auto& [lmask, lend] : lights.groups) {
        if ((lmask & moved) && lend > begin) {
          for (u32 k = begin; k < lend; k++) {
            tod_word* x = w + 4 * vl[k];
            x[3] = (x[3] & 0xff000000u) | tod_entry(t, x[1] >> 16);
          }
          flo = std::min<int>(flo, vl[begin]);
          fhi = std::max<int>(fhi, vl[lend - 1]);
          looked += lend - begin;
          ps.work += lend - begin;
        }
        begin = lend;
      }
    }
  }
  if (ps.pass != kTodLights && !day_ok) {
    bool any = false, left = false;
    auto day_group = [&](u32 g) {
      auto& gr = t.groups[g];
      if (gr.ver >= day_last) {
        return;
      }
      // (the file's colors: now)
      // (the file's colors, or kTodMaxLag day moves behind: now)
      if (gr.ver && t.dver - gr.dver < kTodMaxLag && !budget_ok()) {
        left = true;
        ps.max_lag = std::max(ps.max_lag, t.dver - gr.dver);
        return;
      }
      // (AI-assisted, citadel) tod_max_ms: those too far behind wait beyond it (the next frame's
      // pass starts from them)
      if (ps.capped || !cap_ok()) {
        left = true;
        ps.waiting++;
        ps.max_lag = std::max(ps.max_lag, gr.ver ? t.dver - gr.dver : 99u);
        return;
      }
      const int first = gr.lo, end = std::min<int>(gr.hi + 1, n);
      if (end > first) {
        tod_write_range(t, v, first, end, gr.plain);
        looked += (u32)(end - first);
        ps.work += (u32)(end - first);
        flo = std::min(flo, first);
        fhi = std::max(fhi, end - 1);
      }
      gr.ver = t.ver;
      gr.dver = t.dver;
      any = true;
      ps.rewritten++;
    };
    if (by_runs) {
      for (u32 r = 0; r < nr; r++) {
        if (shown[r] && r0 + r < t.run_group.size()) {
          day_group(g0 + t.run_group[r0 + r]);
        }
      }
    } else {
      for (u32 g = g0; g < g0 + ng; g++) {
        day_group(g);
      }
    }
    if (any) {
      u32 mv = t.ver, md = t.dver;
      for (u32 g = g0; g < g0 + ng; g++) {
        mv = std::min<u32>(mv, t.groups[g].ver);
        md = std::min<u32>(md, t.groups[g].dver);
      }
      t.chunk_min_ver[ci] = mv;
      t.chunk_min_dver[ci] = md;
    }
    ps.behind += left;
  }
  if (fhi >= flo) {
    tod_dirty().emplace_back((uintptr_t)(v + flo), (uintptr_t)(v + fhi + 1));
  }
  return looked;
}

// After a level draw's tod_recolor calls: the rewritten vertices' cache flushes (the ranges sorted
// and merged where less than 64 KB apart: a few ctr_gpu_flush_range per frame rather than one per
// chunk, see ctr_gpu_citro3d.c DEFER_GAP2).
void tod_finish() {
  auto& d = tod_dirty();
  if (d.empty()) {
    return;
  }
  std::sort(d.begin(), d.end());
  constexpr uintptr_t kGap = 64 * 1024;
  uintptr_t a = d[0].first, e = d[0].second;
  for (size_t i = 1; i < d.size(); i++) {
    if (d[i].first <= e + kGap) {
      e = std::max(e, d[i].second);
    } else {
      ctr_gpu_flush_range((const void*)a, (u32)(e - a));
      a = d[i].first;
      e = d[i].second;
    }
  }
  ctr_gpu_flush_range((const void*)a, (u32)(e - a));
  d.clear();
}

#if !defined(__3DS__) || defined(CTR_TOD_CHECK_BUILD)
// (AI-assisted) host check, env CTR_TOD_CHECK (2: a line per frame): every vertex of the chunks
// drawn this frame against the PC's blend (interp_time_of_day_slow, all 8 palettes) with this
// frame's weights: "stale" vertices show another color; "step" is the largest change of a drawn
// vertex's PC color since the level's previous frame (what the PC shows changing per frame).
void tod_check(CtrLevelData& lev, const CtrBackgroundCamera& cam, const CtrRenderState& rs,
               const std::vector<std::pair<u32, const u8*>>& drawn) {
#ifdef __3DS__
  // (a check build, -DCTR_TOD_CHECK_BUILD=2: every 16th frame)
  static const int mode = CTR_TOD_CHECK_BUILD;
  if (rs.frame_idx % 16) {
    return;
  }
#else
  static const int mode = getenv("CTR_TOD_CHECK") ? atoi(getenv("CTR_TOD_CHECK")) : 0;
#endif
  if (!mode || !lev.tod) {
    return;
  }
  const auto& t = *lev.tod;
  struct St {
    u64 frames = 0, verts = 0, stale = 0, stale_frames = 0, steps = 0;
    int max_err = 0, max_step = 0;
    std::array<u8, 24> prev{};
    bool have_prev = false;
  };
  static std::map<std::string, St> stats;
  auto& st = stats[lev.name];
  u8 w[8][3];
  tod_weights(cam, w);
  auto exact = [&](const u8 (*wt)[3], std::vector<u32>& e) {
    const u32 nc = t.color_count, nm = (u32)t.muls.size();
    e.resize(nc + nm);
    for (u32 c = 0; c < nc; c++) {
      u32 r[3] = {};
      for (u32 k = 0; k < t.palette_count; k++) {
        const u8* src = &t.colors[((size_t)c * t.palette_count + k) * 3];
        for (int ch = 0; ch < 3; ch++) {
          r[ch] += wt[t.palettes[k]][ch] * src[ch];
        }
      }
      e[c] = std::min<u32>(255, r[0] >> 6) | std::min<u32>(255, r[1] >> 6) << 8 |
             std::min<u32>(255, r[2] >> 6) << 16;
    }
    for (u32 j = 0; j < nm; j++) {
      e[nc + j] = tod_mul_rgb(e[t.muls[j].color], t.muls[j]);
    }
  };
  std::vector<u32> now, before;
  exact(w, now);
  const bool step_ok = st.have_prev;
  if (step_ok) {
    u8 pw[8][3];
    memcpy(pw, st.prev.data(), 24);
    exact(pw, before);
  }
  memcpy(st.prev.data(), w, 24);
  st.have_prev = true;
  // (AI-assisted) "off": vertices that aren't the table's weights' color (drawn behind)
  std::vector<u32> tgt;
  exact(t.target_w, tgt);
  u64 verts = 0, stale = 0, off = 0, far = 0;
  int err = 0, step = 0;
  std::vector<u8> used;
  for (const auto& [ci, shown] : drawn) {
    if (ci >= lev.meshes.size() || lev.meshes[ci] < 0) {
      continue;
    }
    int n = 0;
    const auto* v = (const c3l::Vertex*)ctr_gpu_mesh_vertices_inplace(lev.meshes[ci], &n);
    // the vertices of the runs drawn (all of the chunk's when it is drawn whole)
    const void* pv = nullptr;
    const u16* ix = nullptr;
    int pn = 0, ni = 0;
    used.assign(std::max(n, 0), shown ? 0 : 1);
    if (shown && ctr_gpu_mesh_peek(lev.meshes[ci], &pv, &pn, &ix, &ni) && ix) {
      const auto& ch = lev.chunks[ci];
      const u32 r0 = lev.chunk_first_run[ci];
      for (u32 k = 0; k < ch.draw_count; k++) {
        const u32 di = ch.first_draw + k;
        u32 idx = lev.draws[di].first_index;
        for (u32 r = lev.draw_first_run[di]; r < lev.draw_first_run[di] + lev.draw_run_count[di];
             r++) {
          const u32 cnt = lev.vis_runs[r].index_count;
          if (shown[r - r0]) {
            for (u32 i = idx; i < idx + cnt && i < (u32)ni; i++) {
              if (ix[i] < n) {
                used[ix[i]] = 1;
              }
            }
          }
          idx += cnt;
        }
      }
    } else if (shown) {
      used.assign(std::max(n, 0), 1);
    }
    for (int i = 0; v && i < n; i++) {
      if (!used[i]) {
        continue;
      }
      const u32 idx = (u16)v[i].pad;
      if (idx >= now.size()) {
        continue;
      }
      verts++;
      u32 rgb;
      memcpy(&rgb, v[i].rgba, 4);
      int e = 0, s = 0;
      for (int ch = 0; ch < 3; ch++) {
        const int a = (int)((rgb >> (8 * ch)) & 0xff), b = (int)((now[idx] >> (8 * ch)) & 0xff);
        e = std::max(e, std::abs(a - b));
        if (step_ok) {
          s = std::max(s, std::abs(b - (int)((before[idx] >> (8 * ch)) & 0xff)));
        }
      }
      stale += e > 0;
      far += e > 4;
      {
        const u32 c = rgb & 0xffffff;
        off += c != tgt[idx];
      }
      err = std::max(err, e);
      step = std::max(step, s);
    }
  }
  st.frames++;
  st.verts += verts;
  st.stale += stale;
  st.stale_frames += stale > 0;
  st.max_err = std::max(st.max_err, err);
  st.max_step = std::max(st.max_step, step);
  st.steps += step > 0;
  if (mode >= 2) {
    lg::info("[ctr] tod check {} frame {}: {} vertices drawn, {} stale (max error {}), step {}, "
             "{} off, {} more than 4 off",
             lev.name, rs.frame_idx, verts, stale, err, step, off, far);
  }
  if (rs.log_now) {
    lg::info("[ctr] tod check {}: {} frames, {:.0f} vertices drawn per frame, {} stale vertices in "
             "{} frames (max error {}), {} frames with a color step (max step {})",
             lev.name, st.frames, (double)st.verts / std::max<u64>(1, st.frames), st.stale,
             st.stale_frames, st.max_err, st.steps, st.max_step);
    const auto keep = st.prev;
    st = St();
    st.prev = keep;
    st.have_prev = true;
  }
}
#endif

// (AI-assisted) c3l v16 time of day: the file's table into lev->tod, the palettes no color uses
// left out (Volcanic Crater: 4 of 8). Read in pieces (no second copy of the table). On the 3DS
// only when the heap keeps kTodHeapReserve free after it (the level then keeps its baked palette:
// the colors of an older file). After the chunks are final (vista tier picked).
void load_tod(c3l::Reader& f, const c3l::Header& hdr, CtrLevelData* lev) {
  if (hdr.version < c3l::kTodVersion || !hdr.tod_offset || !hdr.tod_color_count ||
      hdr.tod_color_count + hdr.tod_mul_count >= 0xffff) {
    return;
  }
  const u32 nc = hdr.tod_color_count, nm = hdr.tod_mul_count;
  constexpr u32 kPiece = 2048;
  std::vector<c3l::TodColor> piece;
  u32 mask = 0, used[8] = {};
  for (u32 i = 0; i < nc; i += kPiece) {
    const u32 n = std::min(kPiece, nc - i);
    if (!f.read_array(hdr.tod_offset + i * (u32)sizeof(c3l::TodColor), n, &piece)) {
      lg::warn("[ctr] {}: bad time of day table, not used", lev->name);
      return;
    }
    for (const auto& c : piece) {
      for (int p = 0; p < 8; p++) {
        if (c.rgb[p][0] | c.rgb[p][1] | c.rgb[p][2]) {
          mask |= 1u << p;
          used[p]++;
        }
      }
    }
  }
  auto t = std::make_unique<CtrLevelData::Tod>();
  for (int p = 0; p < 8; p++) {
    if (mask & (1u << p)) {
      t->palettes[t->palette_count++] = (u8)p;
      // a light palette: a quarter of the colors or fewer
      if (used[p] * 4 <= nc) {
        t->light_mask |= 1u << p;
      }
    }
  }
  // (colors, TodMuls, per entry its mask, rgb and stamp, per chunk 28 bytes with its groups, per
  // run 2)
  [[maybe_unused]] const u32 need = nc * t->palette_count * 3 + nm * (u32)sizeof(c3l::TodMul) +
                                     (nc + nm) * 9 +
                                     (u32)lev->chunks.size() * 28 + (u32)lev->vis_runs.size() * 2;
#ifdef __3DS__
  {
    constexpr u32 kTodHeapReserve = 3u << 20;
    CtrHeapStats st;
    ctr_heap_stats(&st);
    if (st.free < need + kTodHeapReserve || st.largest < need) {
      lg::warn("[ctr] {}: no heap for the time of day colors ({} KB; {} KB free, largest block {} "
               "KB): the baked palette",
               lev->name, need / 1024, st.free / 1024, st.largest / 1024);
      return;
    }
  }
#endif
  t->color_count = nc;
  t->colors.resize((size_t)nc * t->palette_count * 3);
  t->color_mask.assign(nc + nm, 0);
  for (u32 i = 0; i < nc; i += kPiece) {
    const u32 n = std::min(kPiece, nc - i);
    if (!f.read_array(hdr.tod_offset + i * (u32)sizeof(c3l::TodColor), n, &piece)) {
      return;
    }
    u8* dst = t->colors.data() + (size_t)i * t->palette_count * 3;
    for (u32 j = 0; j < n; j++) {
      const auto& c = piece[j];
      u8 m = 0;
      for (u32 k = 0; k < t->palette_count; k++) {
        const u8* rgb = c.rgb[t->palettes[k]];
        memcpy(dst, rgb, 3);
        dst += 3;
        if (rgb[0] | rgb[1] | rgb[2]) {
          m |= (u8)(1u << t->palettes[k]);
        }
      }
      t->color_mask[i + j] = m;
      t->light_color_count += (m & t->light_mask) != 0;
    }
  }
  if (nm && !f.read_array(hdr.tod_offset + nc * (u32)sizeof(c3l::TodColor), nm, &t->muls)) {
    lg::warn("[ctr] {}: bad time of day table, not used", lev->name);
    return;
  }
  for (const auto& m : t->muls) {
    if (m.color >= nc) {
      lg::warn("[ctr] {}: bad time of day table, not used", lev->name);
      return;
    }
  }
  for (u32 j = 0; j < nm; j++) {
    t->color_mask[nc + j] = t->color_mask[t->muls[j].color];
  }
  t->table.assign(nc + nm, 0);
  t->stamp.assign(nc + nm, 0);
  const size_t nch = lev->chunks.size();
  t->chunk_group_first.assign(nch, 0);
  t->chunk_group_count.assign(nch, 0);
  t->run_group.assign(lev->vis_runs.size(), 0);
  t->chunk_min_ver.assign(nch, 0);
  t->chunk_min_dver.assign(nch, 0);
  t->chunk_light_ver.assign(nch, 0);
  t->chunk_mask.assign(nch, 0);
  t->chunk_scanned.assign(nch, 0);
  t->chunk_mesh.assign(nch, -1);
  // the weights the level was last drawn with, else the last level's with the same day palettes:
  // the meshes are colored as they are read (CtrLevels::load_file), not all at the level's first
  // draw. Else they keep the file's colors until drawn (tod_recolor).
  {
    auto& seen = tod_seen();
    std::array<u8, 24> w{};
    bool have = false;
    {
      std::lock_guard<std::mutex> lk(seen.lock);
      if (auto pit = seen.pulse.find(lev->name); pit != seen.pulse.end()) {
        t->pulse_known = pit->second;  // (AI-assisted, citadel)
      }
      auto it = seen.by_level.find(lev->name);
      if (it != seen.by_level.end()) {
        w = it->second;
        have = true;
      } else if (seen.last_day_mask && seen.last_day_mask == tod_day_mask(*t)) {
        w = seen.last;
        have = true;
      }
    }
    if (have) {
      memcpy(t->weights, w.data(), 24);
      memcpy(t->target_w, w.data(), 24);
      t->blended_valid = true;
      // version 1: the colors the meshes get as they are read
      t->ver = 1;
      for (u32 k = 0; k < t->palette_count; k++) {
        t->pal_ver[t->palettes[k]] = 1;
      }
      for (u32 m = 1; m < 256; m++) {
        t->lut[m] = std::max(t->lut[m & (m - 1)], t->pal_ver[__builtin_ctz(m)]);
      }
      tod_make_act(*t, t->target_w, &t->act);
    }
  }
  lg::info("[ctr] {}: time of day: {} colors in {} palettes (lights {:02x}: {} colors), {} "
           "multiplied ({} KB)",
           lev->name, nc, t->palette_count, t->light_mask, t->light_color_count, nm,
           t->bytes() / 1024);
  lev->tod = std::move(t);
}

// (AI-assisted) fine_vis: <level>.c3v (c3v_format.h) into lev->fine when it was made from the
// level's .c3l as loaded (its chunk, draw and run tables: c3v::fine_vis_level_hash). The level's
// (AI-assisted) c3l v17 tie levels of detail (c3l::ExtHeader): the groups and each file run's
// version, mapped onto vis_runs (whose first file_runs come from the file, from offset runs_lo).
// Anything wrong: no levels of detail, and the far versions of the groups are never drawn (a
// level drawn with every group's near version rather than both on top of each other).
// (AI-assisted, streaming) the extension header: v17/v18's 32 bytes, v19's 48 (the fields it
// doesn't have stay 0)
bool read_ext_header(c3l::Reader& f, const c3l::Header& hdr, c3l::ExtHeader* eh) {
  *eh = c3l::ExtHeader{};
  if (!hdr.ext_offset || !f.read(hdr.ext_offset, eh, c3l::kExtHeaderV17Size) ||
      memcmp(eh->magic, c3l::kExtMagic, 4) || eh->size < c3l::kExtHeaderV17Size) {
    *eh = c3l::ExtHeader{};
    return false;
  }
  if (eh->size >= sizeof(c3l::ExtHeader) && hdr.version >= c3l::kStreamVersion &&
      !f.read(hdr.ext_offset, eh, sizeof(c3l::ExtHeader))) {
    *eh = c3l::ExtHeader{};
    return false;
  }
  if (eh->size < sizeof(c3l::ExtHeader) || hdr.version < c3l::kStreamVersion) {
    eh->run_sphere_offset = eh->stream_count = eh->stream_offset = eh->stream_flags = 0;
  }
  return true;
}

void load_tie_lod(c3l::Reader& f,
                  const c3l::Header& hdr,
                  u32 runs_lo,
                  size_t file_runs,
                  CtrLevelData* lev) {
  lev->lod_groups.clear();
  lev->run_lod.clear();
  lev->chunk_lod.clear();
  lev->lod_state.clear();
  lev->lod_error.clear();
  if (hdr.version < c3l::kTieLodVersion || !hdr.ext_offset) {
    return;
  }
  c3l::ExtHeader eh{};
  if (!read_ext_header(f, hdr, &eh)) {
    lg::warn("[ctr] {}: bad extension header, no tie levels of detail", lev->name);
    return;
  }
  if (!eh.lod_group_count || !eh.run_lod_count || runs_lo == UINT32_MAX || !file_runs) {
    return;
  }
  std::vector<c3l::RunLod> all;
  const bool ok = runs_lo >= eh.runs_offset && (runs_lo - eh.runs_offset) % sizeof(c3l::VisRun) == 0 &&
                  (runs_lo - eh.runs_offset) / sizeof(c3l::VisRun) + file_runs <= eh.run_lod_count &&
                  f.read_array(eh.lod_groups_offset, eh.lod_group_count, &lev->lod_groups) &&
                  f.read_array(eh.run_lod_offset, eh.run_lod_count, &all);
  if (!ok) {
    lg::warn("[ctr] {}: bad tie level of detail data, not used", lev->name);
    lev->lod_groups.clear();
    return;
  }
  const size_t first = (runs_lo - eh.runs_offset) / sizeof(c3l::VisRun);
  lev->run_lod.assign(lev->vis_runs.size(), c3l::RunLod{0xffff, c3l::kRunLodAlways, 0});
  lev->chunk_lod.assign(lev->chunks.size(), 0);
  size_t near_runs = 0, bad = 0;
  for (size_t ci = 0; ci < lev->chunks.size(); ci++) {
    const auto& ch = lev->chunks[ci];
    if (!ch.vis_run_count) {
      continue;  // (no runs, or runs that didn't check out at load: drawn whole)
    }
    for (u32 r = lev->chunk_first_run[ci]; r < lev->chunk_first_run[ci] + ch.vis_run_count; r++) {
      if (r >= file_runs) {
        break;
      }
      auto rl = all[first + r];
      if (rl.lod != c3l::kRunLodAlways && (rl.group >= lev->lod_groups.size() || rl.lod > 2)) {
        bad++;
        rl = c3l::RunLod{0xffff, c3l::kRunLodAlways, 0};
      }
      lev->run_lod[r] = rl;
      if (rl.lod != c3l::kRunLodAlways) {
        lev->chunk_lod[ci] = 1;
        near_runs += rl.lod == c3l::kRunLodNear;
      }
    }
  }
  lev->lod_state.assign(lev->lod_groups.size(), 0);
  // (AI-assisted, title-popin) the groups' shape errors: the switch by screen-space error
  if (eh.lod_error_offset &&
      !f.read_array(eh.lod_error_offset, eh.lod_group_count, &lev->lod_error)) {
    lev->lod_error.clear();
  }
  lg::info("[ctr] {}: tie levels of detail: {} groups, {} near runs{}{}", lev->name,
           lev->lod_groups.size(), near_runs, bad ? fmt::format(" ({} bad runs)", bad) : "",
           lev->lod_error.empty() ? ", switched at the PS2 distances"
                                  : ", switched by screen-space error");
}

// (AI-assisted, j2vis) c3l v20 prototype visibility (c3l::ExtProtoVis): the prototypes the game
// hides by task state, each with its runs (mapped onto vis_runs like load_tie_lod) and their chunks.
// Anything wrong: none (drawn always, as before v20).
void load_proto_vis(c3l::Reader& f,
                    const c3l::Header& hdr,
                    u32 runs_lo,
                    size_t file_runs,
                    CtrLevelData* lev) {
  lev->protos.clear();
  lev->proto_hidden_runs.clear();
  lev->chunk_proto_hidden.clear();
  for (int k = 0; k < 2; k++) {
    lev->proto_mask[k].clear();
    lev->proto_mask_seen[k] = false;
  }
  if (hdr.version < c3l::kProtoVisVersion || !hdr.ext_offset) {
    return;
  }
  c3l::ExtHeader eh;
  if (!read_ext_header(f, hdr, &eh) || eh.size < c3l::kExtHeaderV20Size) {
    return;
  }
  c3l::ExtProtoVis ep{};
  if (!f.read(hdr.ext_offset + (u32)sizeof(c3l::ExtHeader), &ep, sizeof(ep)) || !ep.proto_count) {
    return;
  }
  std::vector<c3l::ProtoVis> pv;
  std::vector<u32> pr;
  const bool ok = runs_lo != UINT32_MAX && file_runs && runs_lo >= eh.runs_offset &&
                  (runs_lo - eh.runs_offset) % sizeof(c3l::VisRun) == 0 && ep.proto_count < 4096 &&
                  f.read_array(ep.protos_offset, ep.proto_count, &pv) &&
                  f.read_array(ep.proto_runs_offset, ep.proto_run_count, &pr);
  if (!ok) {
    lg::warn("[ctr] {}: bad prototype visibility data, not used", lev->name);
    return;
  }
  const size_t first = (runs_lo - eh.runs_offset) / sizeof(c3l::VisRun);
  // the listed runs (vis_runs indices) -> their chunks: one walk over the chunks' run ranges
  std::vector<std::pair<u32, u32>> wanted;  // (run, proto)
  size_t bad = 0;
  for (size_t p = 0; p < pv.size(); p++) {
    for (u32 j = pv[p].first_run; j < pv[p].first_run + pv[p].run_count; j++) {
      if (j >= pr.size() || pr[j] < first || pr[j] - first >= file_runs) {
        bad++;
        continue;
      }
      wanted.push_back({(u32)(pr[j] - first), (u32)p});
    }
  }
  std::sort(wanted.begin(), wanted.end());
  lev->protos.resize(pv.size());
  for (size_t p = 0; p < pv.size(); p++) {
    auto& o = lev->protos[p];
    o.name.assign(pv[p].name, strnlen(pv[p].name, sizeof(pv[p].name)));
    o.kind = pv[p].kind;
  }
  size_t found = 0;
  for (size_t ci = 0; ci < lev->chunks.size(); ci++) {
    const auto& ch = lev->chunks[ci];
    if (!ch.vis_run_count) {
      continue;  // (runs that didn't check out at load: drawn whole)
    }
    const u32 r0 = lev->chunk_first_run[ci], r1 = r0 + ch.vis_run_count;
    for (auto it = std::lower_bound(wanted.begin(), wanted.end(), std::make_pair(r0, 0u));
         it != wanted.end() && it->first < r1; ++it) {
      auto& o = lev->protos[it->second];
      o.runs.push_back(it->first);
      if (o.chunks.empty() || o.chunks.back() != ci) {
        o.chunks.push_back((u32)ci);
      }
      found++;
    }
  }
  bad += wanted.size() - found;
  for (auto& o : lev->protos) {
    std::sort(o.runs.begin(), o.runs.end());
  }
  lg::info("[ctr] {}: prototype visibility: {} prototypes the game may hide, {} runs{}", lev->name,
           lev->protos.size(), found, bad ? fmt::format(" ({} bad runs)", bad) : "");
}

// (AI-assisted, streaming) host check, env CTR_STREAM_CHECK (CtrLevels::stream_check)
bool stream_check_enabled() {
#ifdef __3DS__
  return false;
#else
  static const bool on = getenv("CTR_STREAM_CHECK") != nullptr;
  return on;
#endif
}
// (AI-assisted, streaming) the file's values against the ones worked out at load (host check)
u64 g_stream_check_spheres = 0, g_stream_check_sphere_diff = 0, g_stream_check_alpha = 0,
    g_stream_check_alpha_diff = 0;

// (AI-assisted, streaming) c3l v19: each file run's sphere (run_culling: made at load from the
// vertices before v19; a near chunk has none then) and the near chunks (c3l::StreamChunk) as units,
// with the tie LOD groups whose near version they hold. After load_tie_lod. Anything wrong with the
// near chunks: no tie levels of detail for the level (every group's far version), and the near
// chunks never drawn.
void load_stream_info(c3l::Reader& f,
                      const c3l::Header& hdr,
                      u32 runs_lo,
                      size_t file_runs,
                      CtrLevelData* lev) {
  lev->stream_units.clear();
  lev->chunk_unit.clear();
  lev->group_unit_first.clear();
  lev->group_unit_count.clear();
  lev->group_units.clear();
  if (hdr.version < c3l::kStreamVersion) {
    return;
  }
  c3l::ExtHeader eh;
  if (!read_ext_header(f, hdr, &eh)) {
    return;
  }
  const bool runs_ok = runs_lo != UINT32_MAX && file_runs && runs_lo >= eh.runs_offset &&
                       (runs_lo - eh.runs_offset) % sizeof(c3l::VisRun) == 0 &&
                       (runs_lo - eh.runs_offset) / sizeof(c3l::VisRun) + file_runs <=
                           eh.run_lod_count;
  const size_t first = runs_ok ? (runs_lo - eh.runs_offset) / sizeof(c3l::VisRun) : 0;
  if (eh.run_sphere_offset && runs_ok && lev->run_spheres.size() >= file_runs) {
    std::vector<c3l::RunSphere> sp;
    if (f.read_array(eh.run_sphere_offset + (u32)(first * sizeof(c3l::RunSphere)), file_runs,
                     &sp)) {
      static_assert(sizeof(c3l::RunSphere) == sizeof(CtrLevelData::RunSphere));
      memcpy(lev->run_spheres.data(), sp.data(), file_runs * sizeof(c3l::RunSphere));
    }
  }
  if (!eh.stream_count || !eh.stream_offset) {
    return;
  }
  std::vector<c3l::StreamChunk> sc;
  const char* why = nullptr;
  if (!f.read_array(eh.stream_offset, eh.stream_count, &sc)) {
    why = "truncated";
  }
  lev->chunk_unit.assign(lev->chunks.size(), UINT32_MAX);
  for (size_t i = 0; !why && i < sc.size(); i++) {
    const auto& c = sc[i];
    if (c.chunk >= lev->chunks.size() || lev->chunk_unit[c.chunk] != UINT32_MAX ||
        lev->chunks[c.chunk].kind != c3l::CHUNK_KIND_TIE || !lev->chunks[c.chunk].vis_run_count ||
        (c.cls != c3l::kStreamCore && c.cls != c3l::kStreamExtra) || lev->run_lod.empty()) {
      why = "bad near chunk";
      break;
    }
    lev->chunk_unit[c.chunk] = (u32)lev->stream_units.size();
    auto& u = lev->stream_units.emplace_back();
    u.chunk = c.chunk;
    u.cls = c.cls;
    u.need_a = c.need_a;
    u.need_e = c.need_e;
  }
  // the groups' units (a chunk's runs one after the other: a group's last unit is this one)
  if (!why) {
    const size_t ng = lev->lod_groups.size();
    std::vector<u32> last(ng, UINT32_MAX);
    lev->group_unit_count.assign(ng, 0);
    for (int pass = 0; pass < 2 && !why; pass++) {
      if (pass == 1) {
        lev->group_unit_first.assign(ng, 0);
        u32 n = 0;
        for (size_t g = 0; g < ng; g++) {
          lev->group_unit_first[g] = n;
          n += lev->group_unit_count[g];
          lev->group_unit_count[g] = 0;
        }
        lev->group_units.assign(n, 0);
        last.assign(ng, UINT32_MAX);
      }
      for (u32 u = 0; u < lev->stream_units.size() && !why; u++) {
        const u32 ci = lev->stream_units[u].chunk;
        const auto& ch = lev->chunks[ci];
        for (u32 r = lev->chunk_first_run[ci]; r < lev->chunk_first_run[ci] + ch.vis_run_count;
             r++) {
          const auto& rl = lev->run_lod[r];
          if (rl.lod != c3l::kRunLodNear || rl.group >= ng) {
            why = "a near chunk run that isn't a near version";
            break;
          }
          if (last[rl.group] == u) {
            continue;
          }
          last[rl.group] = u;
          if (pass == 1) {
            lev->group_units[lev->group_unit_first[rl.group] + lev->group_unit_count[rl.group]] = u;
          }
          if (lev->group_unit_count[rl.group] == 0xff) {
            why = "a group in too many near chunks";
            break;
          }
          lev->group_unit_count[rl.group]++;
        }
      }
    }
  }
  if (!why) {
    // the other way round: each unit's groups
    lev->unit_group_first.assign(lev->stream_units.size() + 1, 0);
    for (size_t g = 0; g < lev->group_unit_count.size(); g++) {
      for (u32 j = 0; j < lev->group_unit_count[g]; j++) {
        lev->unit_group_first[lev->group_units[lev->group_unit_first[g] + j] + 1]++;
      }
    }
    for (size_t u = 0; u < lev->stream_units.size(); u++) {
      lev->unit_group_first[u + 1] += lev->unit_group_first[u];
    }
    lev->unit_groups.assign(lev->unit_group_first.back(), 0);
    std::vector<u32> fill(lev->unit_group_first.begin(), lev->unit_group_first.end() - 1);
    for (size_t g = 0; g < lev->group_unit_count.size(); g++) {
      for (u32 j = 0; j < lev->group_unit_count[g]; j++) {
        lev->unit_groups[fill[lev->group_units[lev->group_unit_first[g] + j]]++] = (u32)g;
      }
    }
  }
  if (why) {
    lev->unit_group_first.clear();
    lev->unit_groups.clear();
    lg::warn("[ctr] {}: {} in the near chunk table: no tie levels of detail", lev->name, why);
    // every group far, never a near chunk (they stay units: never loaded, never drawn)
    for (auto& u : lev->stream_units) {
      u.state = CtrLevelData::kUnitNever;
    }
    lev->lod_groups.clear();
    lev->run_lod.clear();
    lev->chunk_lod.clear();
    lev->lod_state.clear();
    lev->lod_error.clear();
    lev->group_unit_first.clear();
    lev->group_unit_count.clear();
    lev->group_units.clear();
    if (lev->stream_units.empty()) {
      lev->chunk_unit.clear();
    }
    return;
  }
}

// visibility runs (and run_culling's pieces) are made already. Anything wrong: no fine visibility.
void load_fine_vis(const fs::path& c3l_path, CtrLevelData* lev) {
  fs::path p = c3l_path;
  p.replace_extension(".c3v");
  lev->fine = CtrFineVis::open(p.string(), lev->name, lev->chunks.data(), (u32)lev->chunks.size(),
                               lev->draws.data(), (u32)lev->draws.size(), lev->vis_runs.data(),
                               lev->chunk_first_run.data(),
                               lev->chunk_run_count.size() >= lev->chunks.size()
                                   ? lev->chunk_run_count.data()
                                   : std::vector<u32>(lev->chunks.size(), 0).data());
}

// (AI-assisted) run_culling: the bounding spheres of chunk ci's runs from its vertices (as read
// from the file) and the file's indices (the draws' first_index still into all of them). The
// center of the run's bounding box and half its diagonal, in the chunk's quantized units.
// (AI-assisted) indices: the file's indices [idx_first, idx_end) (the chunk's), indices[0] is index
// idx_first
void make_run_spheres(CtrLevelData* out,
                      u32 ci,
                      const c3l::Vertex* verts,
                      const u16* indices,
                      u32 idx_first,
                      u32 idx_end) {
  const auto& ch = out->chunks[ci];
  for (u32 k = 0; k < ch.draw_count && ch.first_draw + k < out->draws.size(); k++) {
    const u32 di = ch.first_draw + k;
    u32 idx = out->draws[di].first_index;
    for (u32 r = out->draw_first_run[di]; r < out->draw_first_run[di] + out->draw_run_count[di];
         r++) {
      const u32 n = out->vis_runs[r].index_count;
      int mn[3] = {32767, 32767, 32767}, mx[3] = {-32768, -32768, -32768};
      bool ok = n > 0 && idx >= idx_first && idx + n <= idx_end;
      for (u32 i = idx; ok && i < idx + n; i++) {
        const u16 v = indices[i - idx_first];
        ok = v < ch.vertex_count;
        for (int c = 0; ok && c < 3; c++) {
          mn[c] = std::min<int>(mn[c], verts[v].pos[c]);
          mx[c] = std::max<int>(mx[c], verts[v].pos[c]);
        }
      }
      idx += n;
      if (!ok) {
        continue;  // stays unknown: never hidden
      }
      auto& s = out->run_spheres[r];
      float d2 = 0;
      for (int c = 0; c < 3; c++) {
        s.c[c] = (s16)((mn[c] + mx[c]) >> 1);
        // the larger half of the extent (the center is rounded down)
        const float h = (float)std::max(s.c[c] - mn[c], mx[c] - s.c[c]);
        d2 += h * h;
      }
      s.r = (u16)std::min(65534.f, std::ceil(std::sqrt(d2)) + 1.f);
    }
  }
}

// GPU state per draw, so drawing doesn't decode the draw modes every frame, and the drawing order.
// (AI-assisted) At load, and again when the textures are made later (a repair: the states hold the
// texture handles).
// (AI-assisted) c3l v15 kDrawAlphaTestDropped / kDrawBlendDropped draws (alpha 1.0 in every texel
// and vertex: the alpha test / blending can't change a pixel): on, as the converter left them (no
// test, no blending); off, the test / blending back as the draw mode had them (GEQUAL with the
// mode's aref; Cs * As + Cd * (1 - As)). Other draws: unchanged.
void set_opaque_alpha(ctr_draw_state* st, const c3l::Draw& d, bool on, int min_alpha) {
  if (!(d.kind & (c3l::kDrawAlphaTestDropped | c3l::kDrawBlendDropped))) {
    return;
  }
  DrawMode mode;
  mode.as_int() = d.mode;
  if (d.kind & c3l::kDrawAlphaTestDropped) {
    st->atest = on ? CTR_TEST_ALWAYS : CTR_TEST_GEQUAL;
    st->aref = on ? 0 : mode.get_aref();
  }
  if (d.kind & c3l::kDrawBlendDropped) {
    st->blend = on ? CTR_BLEND_OFF : CTR_BLEND_ALPHA;
  }
  // the early depth test as build_draw_order decides it (the texture's alpha passes any test)
  st->flags &= ~CTR_STATE_EARLY_DEPTH;
  if (st->blend == CTR_BLEND_OFF && st->zwrite && st->ztest == CTR_TEST_GEQUAL &&
      (st->atest == CTR_TEST_ALWAYS || min_alpha >= std::min(255, st->aref * 2))) {
    st->flags |= CTR_STATE_EARLY_DEPTH;
  }
}

void build_draw_order(CtrLevelData* out) {
  out->draw_states.clear();
  out->draw_chunk.clear();
  out->sorted_draws.clear();
  out->ordered_draws.clear();
  out->early_draws.clear();
  out->draw_states.reserve(out->draws.size());
  out->opaque_alpha_applied = ctr_settings().opaque_alpha;
  for (auto& d : out->draws) {
    DrawMode mode;
    mode.as_int() = d.mode;
    const bool textured = d.texture < out->textures.size();
    ctr_draw_state st =
        ctr_state_from_draw_mode(mode, textured ? out->textures[d.texture] : -1);
    // (AI-assisted) cull_ties: c3l v13 draws of closed, consistently wound tie surfaces
    // (whether they are culled is decided per frame: ctr_gpu_set_cull)
    if (d.kind & c3l::kDrawCullBack) {
      st.tcc |= CTR_TCC_CULL_BACK;
    }
    // (AI-assisted) c3l v15: an atlas's texture coordinates (1/32768)
    if (d.kind & c3l::kDrawAtlasUV) {
      st.tcc |= CTR_TCC_ST_ATLAS;
    }
    // (AI-assisted) fog_alpha: the vertex alpha is 0x80 everywhere in this draw
    const size_t di = out->draw_states.size();
    if (di < out->draw_alpha80.size() && out->draw_alpha80[di]) {
      st.tcc |= CTR_TCC_ALPHA80;
    }
    // (AI-assisted) opaque, depth written and tested like the early depth test, and the alpha
    // test never fails (the texture's alpha is above its reference everywhere; vertex alpha 0x80):
    // the early depth test may drop what's behind it before texturing
    const int min_alpha = textured ? out->tex_min_alpha[d.texture] : 0;
    if (st.blend == CTR_BLEND_OFF && st.zwrite && st.ztest == CTR_TEST_GEQUAL &&
        (st.atest == CTR_TEST_ALWAYS || (textured && min_alpha >= std::min(255, st.aref * 2)))) {
      st.flags |= CTR_STATE_EARLY_DEPTH;
    }
    // (AI-assisted) c3l v15: the alpha test / blending the converter took out, back with
    // config opaque_alpha = off
    set_opaque_alpha(&st, d, ctr_settings().opaque_alpha, min_alpha);
    out->draw_states.push_back(st);
  }
  // sort keys: texture first (a texture change clears the GPU's texture cache), then the rest of
  // the state (numbered per level)
  std::vector<ctr_draw_state> states;
  // (AI-assisted) only needed here: not kept in the level (10-35 KB of the heap per level)
  std::vector<u32> draw_sort_keys(out->draws.size());
  std::vector<u8> draw_ordered(out->draws.size());
  for (size_t di = 0; di < out->draws.size(); di++) {
    ctr_draw_state st = out->draw_states[di];
    const int tex = st.tex;
    st.tex = 0;
    u32 id = 0;
    while (id < states.size() && memcmp(&states[id], &st, sizeof(st))) {
      id++;
    }
    if (id == states.size()) {
      states.push_back(st);
    }
    draw_sort_keys[di] = ((u32)(tex + 1) << 16) | (id & 0xffff);
    // blending, or drawn over what's there without writing depth (decals): keep the order.
    // (AI-assisted) v15 kDrawBlendDropped draws keep their place too (moving them before the
    // blended draws drawn ahead of them could change what those pixels show)
    draw_ordered[di] = st.blend != CTR_BLEND_OFF || !st.zwrite ||
                       (out->draws[di].kind & c3l::kDrawBlendDropped);
  }
  // drawing order, made once (see sorted_draws)
  out->draw_chunk.assign(out->draws.size(), 0);
  for (u32 ci = 0; ci < out->chunks.size(); ci++) {
    const auto& ch = out->chunks[ci];
    for (u32 k = 0; k < ch.draw_count && ch.first_draw + k < out->draws.size(); k++) {
      out->draw_chunk[ch.first_draw + k] = ci;
    }
  }
  std::vector<u64> sorted;
  for (u32 ci = 0; ci < out->chunks.size(); ci++) {
    const auto& ch = out->chunks[ci];
    for (u32 k = 0; k < ch.draw_count && ch.first_draw + k < out->draws.size(); k++) {
      const u32 di = ch.first_draw + k;
      if (draw_ordered[di]) {
        out->ordered_draws.push_back(di);
      } else {
        // (draws are numbered in chunk order: di sorts like (chunk, draw))
        sorted.push_back(((u64)draw_sort_keys[di] << 32) | di);
      }
    }
  }
  std::sort(sorted.begin(), sorted.end());
  out->sorted_draws.reserve(sorted.size());
  for (u64 e : sorted) {
    const u32 di = (u32)e;
    if (out->draw_states[di].flags & CTR_STATE_EARLY_DEPTH) {
      out->early_draws.push_back(di);
    } else {
      out->sorted_draws.push_back(di);
    }
  }
  // (AI-assisted, j2cull) each draw's place in its list
  out->draw_list_pos.assign(out->draws.size(), UINT32_MAX);
  const std::vector<u32>* lists[3] = {&out->early_draws, &out->sorted_draws, &out->ordered_draws};
  for (u32 li = 0; li < 3; li++) {
    for (u32 p = 0; p < lists[li]->size(); p++) {
      out->draw_list_pos[(*lists[li])[p]] = (li << 30) | p;
    }
  }
}

// (AI-assisted, j2cull) draw_level's cull clusters (CtrLevelData::CullCluster): runs of
// consecutive chunks (and tie LOD groups) whose spheres lie together, at most kClusterMax of them
// within kClusterR. Chunks the chunk loop doesn't decide by distance alone (the vista tier, the
// low resolution tfrag, near chunks) are in plain clusters: always looked at one by one.
constexpr u32 kClusterMax = 32;
constexpr float kClusterR = 48.f * 4096.f;
// a cluster's bound: center and the largest |c - center| + r (made a little larger: float)
template <typename SphereOf>
void cluster_bound(CtrLevelData::CullCluster& cl, SphereOf sphere_of) {
  float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
  for (u32 i = cl.first; i < cl.first + cl.count; i++) {
    const float* s = sphere_of(i);
    for (int c = 0; c < 3; c++) {
      mn[c] = std::min(mn[c], s[c]);
      mx[c] = std::max(mx[c], s[c]);
    }
  }
  for (int c = 0; c < 3; c++) {
    cl.c[c] = 0.5f * (mn[c] + mx[c]);
  }
  float a = 0;
  for (u32 i = cl.first; i < cl.first + cl.count; i++) {
    const float* s = sphere_of(i);
    const double dx = s[0] - cl.c[0], dy = s[1] - cl.c[1], dz = s[2] - cl.c[2];
    a = std::max(a, (float)(std::sqrt(dx * dx + dy * dy + dz * dz) + s[3]));
  }
  cl.a = a * 1.0001f + 16.f;
}
// greedy: the next item joins while the cluster stays within kClusterR (centers' box half
// diagonal + the largest radius)
template <typename Ok, typename SphereOf>
void make_clusters(std::vector<CtrLevelData::CullCluster>& out, u32 n, Ok ok, SphereOf sphere_of) {
  out.clear();
  u32 i = 0;
  while (i < n) {
    CtrLevelData::CullCluster cl{};
    cl.first = i;
    if (!ok(i)) {
      u32 j = i + 1;
      while (j < n && !ok(j) && j - i < 0xffffff) {
        j++;
      }
      cl.count = j - i;
      cl.flags = CtrLevelData::kClPlain;
      out.push_back(cl);
      i = j;
      continue;
    }
    const float* s0 = sphere_of(i);
    float mn[3] = {s0[0], s0[1], s0[2]}, mx[3] = {s0[0], s0[1], s0[2]}, rmax = s0[3];
    u32 j = i + 1;
    while (j < n && j - i < kClusterMax && ok(j)) {
      const float* s = sphere_of(j);
      float nmn[3], nmx[3];
      float h2 = 0;
      for (int c = 0; c < 3; c++) {
        nmn[c] = std::min(mn[c], s[c]);
        nmx[c] = std::max(mx[c], s[c]);
        h2 += 0.25f * (nmx[c] - nmn[c]) * (nmx[c] - nmn[c]);
      }
      if (std::sqrt(h2) + std::max(rmax, s[3]) > kClusterR) {
        break;
      }
      memcpy(mn, nmn, sizeof(mn));
      memcpy(mx, nmx, sizeof(mx));
      rmax = std::max(rmax, s[3]);
      j++;
    }
    cl.count = j - i;
    cluster_bound(cl, sphere_of);
    out.push_back(cl);
    i = j;
  }
}
void make_cull_clusters(CtrLevelData& lev) {
  lev.cull_made = true;
  lev.cull_sizes[0] = lev.chunks.size();
  lev.cull_sizes[1] = lev.lod_groups.size();
  lev.cull_sizes[2] = lev.chunk_unit.size();
  const u32 n = (u32)lev.chunks.size();
  auto chunk_ok = [&](u32 ci) {
    const auto& ch = lev.chunks[ci];
    return ch.kind != c3l::CHUNK_KIND_VISTA && ch.lod_tier != 3 &&
           (lev.chunk_unit.empty() || lev.chunk_unit[ci] == UINT32_MAX);
  };
  make_clusters(lev.chunk_clusters, n, chunk_ok,
                [&](u32 ci) { return (const float*)lev.chunks[ci].bsphere; });
  for (auto& cl : lev.chunk_clusters) {
    if (cl.flags & CtrLevelData::kClPlain) {
      continue;
    }
    cl.lod_a = 0;
    for (u32 ci = cl.first; ci < cl.first + cl.count; ci++) {
      const auto& ch = lev.chunks[ci];
      const int k = ch.kind == c3l::CHUNK_KIND_TIE ? 0 : (ch.kind == c3l::CHUNK_KIND_SHRUB ? 1 : 2);
      cl.flags |= CtrLevelData::kClHas << k;
      if (ch.max_dist > 0) {
        cl.raw[k] = std::max(cl.raw[k], ch.max_dist);
      } else {
        cl.flags |= CtrLevelData::kClNoOwn << k;
      }
      if (ch.lod_tier == 1 || ch.lod_tier == 2) {
        cl.flags |= CtrLevelData::kClTier12;
        const double dx = ch.lod_center[0] - cl.c[0], dy = ch.lod_center[1] - cl.c[1],
                     dz = ch.lod_center[2] - cl.c[2];
        cl.lod_a = std::max(cl.lod_a, (float)std::sqrt(dx * dx + dy * dy + dz * dz));
      }
    }
    cl.lod_a = cl.lod_a * 1.0001f + 16.f;
  }
  lev.vista_chunks.clear();
  for (u32 ci = 0; ci < n; ci++) {
    if (lev.chunks[ci].kind == c3l::CHUNK_KIND_VISTA) {
      lev.vista_chunks.push_back(ci);
    }
  }
  const u32 ng = (u32)lev.lod_groups.size();
  make_clusters(lev.lod_clusters, ng, [](u32) { return true; },
                [&](u32 g) { return (const float*)lev.lod_groups[g].bsphere; });
  const bool err = lev.lod_error.size() == ng;
  for (auto& cl : lev.lod_clusters) {
    for (u32 g = cl.first; g < cl.first + cl.count; g++) {
      cl.raw[0] = std::max(cl.raw[0], err ? lev.lod_error[g] : 0.f);
      cl.raw[1] = std::max(cl.raw[1], lev.lod_groups[g].dist);
      if ((err && !(lev.lod_error[g] >= 0)) || !(lev.lod_groups[g].dist >= 0)) {
        cl.flags |= CtrLevelData::kClPlain;  // (a negative switch distance: one by one)
      }
    }
  }
  size_t plain = 0;
  for (const auto& cl : lev.chunk_clusters) {
    plain += (cl.flags & CtrLevelData::kClPlain) ? cl.count : 0;
  }
  lg::debug("[ctr] {}: cull clusters: {} chunks in {} ({} looked at one by one), {} tie LOD groups "
            "in {}",
            lev.name, n, lev.chunk_clusters.size(), plain, ng, lev.lod_clusters.size());
}
}  // namespace

// All textures (with their mip levels) in one pool: the renderer moves it to VRAM when there's
// room, with one GPU copy. The texels are read straight into it. (AI-assisted) When VRAM is short
// only the pool's start goes there (~2.6 MB): the textures of the background (tfrag, tie, shrub)
// come first, then the ones only merc models (characters) use, which were 75-95% of a level's
// pool and came first in the file. They are read in file order all the same.
bool CtrLevels::load_textures(c3l::Reader& f,
                              const c3l::Header& hdr,
                              const std::vector<c3l::Texture>& texs,
                              const std::vector<c3l::Draw>& draws,
                              const std::string& name,
                              TexLoad* out) {
  struct Plan {
    bool ok = false;
    bool expand = false;  // RGBA4 stored as RGBA8 (ctr_gpu_set_rgba4_as_rgba8)
    int format = 0;       // ctr_tex_format
    int levels = 1;
    u32 file_bytes = 0;   // of the levels used, as stored in the file
    u32 offset = 0;       // in the pool
  };
  std::vector<Plan> plan(texs.size());
  const bool rgba4_as_rgba8 = ctr_gpu_rgba4_as_rgba8() != 0;
  for (size_t i = 0; i < texs.size(); i++) {
    const auto& t = texs[i];
    auto& p = plan[i];
    const bool pow2 = t.w >= 8 && t.h >= 8 && t.w <= 1024 && t.h <= 1024 &&
                      !(t.w & (t.w - 1)) && !(t.h & (t.h - 1));
    if (!pow2 || t.format > c3l::TEX_RGBA8 || (hdr.version < 8 && t.format > c3l::TEX_RGBA4)) {
      continue;
    }
    if (t.data_size == 0 && hdr.version >= c3l::kAtlasVersion) {
      out->empty++;  // (AI-assisted) v15: moved into an atlas, nothing draws it
      continue;
    }
    // the levels the data holds (and the GPU allows: at least 8 texels on the short side)
    const int want = hdr.version >= 8 ? std::max<int>(1, t.levels) : 1;
    int levels = 0;
    u32 bytes = 0;
    for (int l = 0; l < want; l++) {
      const u32 w = t.w >> l, h = t.h >> l;
      const u32 b = c3l::texture_level_bytes(w, h, t.format);
      if (w < 8 || h < 8 || bytes + b > t.data_size) {
        break;
      }
      bytes += b;
      levels++;
    }
    if (!levels) {
      continue;
    }
    p.ok = true;
    p.levels = levels;
    p.file_bytes = bytes;
    p.expand = t.format == c3l::TEX_RGBA4 && rgba4_as_rgba8;
    p.format = p.expand ? CTR_TEX_RGBA8 : (int)t.format;
  }
  // (AI-assisted) the places in the pool: the background's textures first
  std::vector<u8> background(texs.size(), 0);
  for (const auto& d : draws) {
    if (d.texture < texs.size()) {
      background[d.texture] = 1;
    }
  }
  u32 pool_bytes = 0;
  for (int pass = 0; pass < 2; pass++) {
    for (size_t i = 0; i < texs.size(); i++) {
      auto& p = plan[i];
      if (p.ok && background[i] == (pass == 0)) {
        p.offset = pool_bytes;
        pool_bytes += (ctr_gpu_tex_bytes(texs[i].w, texs[i].h, p.format, p.levels) + 127) & ~127u;
      }
    }
    if (pass == 0) {
      out->background_bytes = pool_bytes;
    }
  }
  out->pool_bytes = pool_bytes;
  out->handles.assign(texs.size(), -1);
  out->min_alpha.assign(texs.size(), 0);
  int pool = pool_bytes ? ctr_gpu_pool_create(pool_bytes) : -1;
  if (pool_bytes && pool < 0 && make_room(name, "the textures")) {
    pool = ctr_gpu_pool_create(pool_bytes);
  }
  if (pool_bytes && pool < 0) {
    lg::error("[ctr] {}: no memory for the textures ({} KB)", name, pool_bytes / 1024);
  }
  out->pool = pool;
  u8* base = pool >= 0 ? (u8*)ctr_gpu_pool_data(pool) : nullptr;
  std::vector<u8> texels;
  for (size_t i = 0; i < texs.size(); i++) {
    if (cancelled()) {
      return false;
    }
    const auto& t = texs[i];
    const auto& p = plan[i];
    if (!p.ok || !base) {
      continue;
    }
    u8* dst = base + p.offset;
    bool read_ok;
    if (!p.expand) {
      read_ok = f.read(t.data_offset, dst, p.file_bytes);
    } else {
      // RGBA4 -> RGBA8 (the same tiled order, all levels; GPU_RGBA8 is stored A, B, G, R)
      texels.resize(p.file_bytes);
      read_ok = f.read(t.data_offset, texels.data(), p.file_bytes);
      const u32 n = p.file_bytes / 2;
      for (u32 k = 0; read_ok && k < n; k++) {
        const u16 v = (u16)(texels[2 * k] | (texels[2 * k + 1] << 8));
        dst[4 * k + 0] = (u8)((v & 0xf) * 17);
        dst[4 * k + 1] = (u8)(((v >> 4) & 0xf) * 17);
        dst[4 * k + 2] = (u8)(((v >> 8) & 0xf) * 17);
        dst[4 * k + 3] = (u8)(((v >> 12) & 0xf) * 17);
      }
    }
    if (read_ok) {
      out->handles[i] = ctr_gpu_pool_tex(pool, p.offset, t.w, t.h, p.format, p.levels);
      // (AI-assisted, Jak II) an animated slot's base texture: drawn as the slot's animation
      if (const u16 anim_slot = c3l::texture_anim_slot(t)) {
        ctr_gpu_tex_set_anim_slot(out->handles[i], anim_slot - 1);
      }
      out->min_alpha[i] = (u8)texture_min_alpha(dst, t.w, t.h, p.format);
    }
  }
  if (pool >= 0) {
    ctr_gpu_pool_ready(pool);
  }
  return true;
}

bool CtrLevels::load_file(const fs::path& path,
                          const std::string& name,
                          CtrLevelData* out,
                          long* file_size,
                          double* decode_ms) {
  C3lReader f(path, &m_read_bufs);
  // (AI-assisted) the decode time, however the load ends
  struct DecodeTime {
    const C3lReader& f;
    double* ms;
    double* read_ms;
    u32* reads;
    u64* read_bytes;
    ~DecodeTime() {
      *ms = f.decode_ms;
      *read_ms = f.read_ms;  // (AI-assisted)
      *reads = f.file_reads;
      *read_bytes = f.file_bytes_read;
    }
  } decode_time{f, decode_ms, &m_last_read_ms, &m_last_reads, &m_last_read_bytes};
  c3l::Header hdr;
  if (!f.read(0, &hdr, sizeof(hdr))) {
    lg::error("[ctr] failed to read {}", path.string());
    return false;
  }
  *file_size = f.size();
  if (memcmp(hdr.magic, c3l::kMagic, 4) || hdr.version < c3l::kMinVersion ||
      hdr.version > c3l::kVersion) {
    lg::error("[ctr] {}: not a C3L v{}..{} file (version {})", path.string(), c3l::kMinVersion,
              c3l::kVersion, hdr.version);
    return false;
  }
  // (AI-assisted) v10 files are only uncompressed (slower to read): no warning
  if (hdr.version < 10) {
    lg::warn("[ctr] {}: old C3L v{} file ({}): convert the levels again (ctr_level_converter --all)",
             path.string(), hdr.version,
             hdr.version < 8   ? "no mip levels, 16-bit textures: slow on the 3DS"
             : hdr.version < 9 ? "no envmap shine on models"
                               : "no visibility culling");
  }
  out->name = name;
  out->shrub_ps2_dist = hdr.version >= c3l::kShrubPs2DistVersion;  // (AI-assisted)
  if (!f.read_array(hdr.chunks_offset, hdr.num_chunks, &out->chunks) ||
      !f.read_array(hdr.draw_data_offset, hdr.draw_data_size / sizeof(c3l::Draw), &out->draws)) {
    lg::error("[ctr] {}: truncated file", path.string());
    return false;
  }
  {
    // (AI-assisted) config vista_untextured: a file with both vista tiers (textured CHUNK_KIND_VISTA,
    // then untextured CHUNK_KIND_VISTA_FLAT, the last chunks: nothing refers to them by index)
    // keeps the configured one as CHUNK_KIND_VISTA; the other's meshes are never made
    size_t first_vista = out->chunks.size(), flat = 0;
    for (size_t ci = 0; ci < out->chunks.size(); ci++) {
      const u32 k = out->chunks[ci].kind;
      if ((k == c3l::CHUNK_KIND_VISTA || k == c3l::CHUNK_KIND_VISTA_FLAT) && first_vista > ci) {
        first_vista = ci;
      }
      flat += k == c3l::CHUNK_KIND_VISTA_FLAT;
    }
    bool tail = flat > 0;
    for (size_t ci = first_vista; ci < out->chunks.size() && tail; ci++) {
      tail = out->chunks[ci].kind == c3l::CHUNK_KIND_VISTA ||
             out->chunks[ci].kind == c3l::CHUNK_KIND_VISTA_FLAT;
    }
    if (tail) {
      const bool want_flat = ctr_settings().vista_untextured;
      std::vector<c3l::Chunk> kept;
      kept.reserve(out->chunks.size());  // (AI-assisted) one piece of the heap, not a few
      kept.assign(out->chunks.begin(), out->chunks.begin() + first_vista);
      for (size_t ci = first_vista; ci < out->chunks.size(); ci++) {
        c3l::Chunk ch = out->chunks[ci];
        if ((ch.kind == c3l::CHUNK_KIND_VISTA_FLAT) == want_flat) {
          ch.kind = c3l::CHUNK_KIND_VISTA;
          kept.push_back(ch);
        }
      }
      lg::info("[ctr] {}: {} vista tier ({} of {} vista chunks)", name,
               want_flat ? "untextured" : "textured", kept.size() - first_vista,
               out->chunks.size() - first_vista);
      out->chunks = std::move(kept);
    } else {
      for (auto& ch : out->chunks) {
        if (ch.kind == c3l::CHUNK_KIND_VISTA_FLAT) {
          ch.kind = c3l::CHUNK_KIND_UNKNOWN;  // (not where expected: never drawn)
          ch.draw_count = 0;
          ch.vis_run_count = 0;
        }
      }
    }
  }

  // (AI-assisted) v10: the visibility runs, all chunks' in one block
  out->chunk_first_run.assign(out->chunks.size(), 0);
  out->draw_first_run.assign(out->draws.size(), 0);
  out->draw_run_count.assign(out->draws.size(), 0);
  u32 runs_lo = UINT32_MAX;  // (AI-assisted) the file offset of vis_runs[0] (v17 tie LOD)
  size_t file_runs = 0;
  if (hdr.version < 10) {
    for (auto& ch : out->chunks) {
      ch.vis_run_count = 0;
    }
  } else {
    u32 lo = UINT32_MAX, hi = 0;
    for (const auto& ch : out->chunks) {
      if (ch.vis_run_count) {
        lo = std::min<u32>(lo, ch.vis_runs_offset);
        hi = std::max<u32>(hi, ch.vis_runs_offset + ch.vis_run_count * sizeof(c3l::VisRun));
      }
    }
    if (lo < hi && !f.read_array(lo, (hi - lo) / sizeof(c3l::VisRun), &out->vis_runs)) {
      lg::warn("[ctr] {}: bad visibility data, not used", path.string());
      out->vis_runs.clear();
    }
    runs_lo = lo;
    file_runs = out->vis_runs.size();
    for (size_t ci = 0; ci < out->chunks.size(); ci++) {
      auto& ch = out->chunks[ci];
      if (!ch.vis_run_count) {
        continue;
      }
      const u32 first = (ch.vis_runs_offset - lo) / sizeof(c3l::VisRun);
      bool ok = !out->vis_runs.empty() && first + ch.vis_run_count <= out->vis_runs.size();
      // each draw's runs follow each other and cover the draw
      std::vector<u32> covered(ch.draw_count, 0);
      for (u32 r = first; ok && r < first + ch.vis_run_count; r++) {
        const auto& run = out->vis_runs[r];
        ok = run.draw < ch.draw_count;
        if (ok) {
          const u32 di = ch.first_draw + run.draw;
          if (!out->draw_run_count[di]) {
            out->draw_first_run[di] = r;
          }
          ok = out->draw_first_run[di] + out->draw_run_count[di] == r;
          out->draw_run_count[di]++;
          covered[run.draw] += run.index_count;
        }
      }
      for (u32 k = 0; ok && k < ch.draw_count; k++) {
        ok = !out->draw_run_count[ch.first_draw + k] ||
             covered[k] == out->draws[ch.first_draw + k].index_count;
      }
      if (!ok) {
        for (u32 k = 0; k < ch.draw_count; k++) {
          out->draw_run_count[ch.first_draw + k] = 0;
        }
        ch.vis_run_count = 0;
        continue;
      }
      out->chunk_first_run[ci] = first;
    }
  }
  // (AI-assisted) run_culling: runs for the chunks without visibility runs (their draws in pieces
  // of kCullRunIndices, never hidden by the visibility data), so the view test can drop the parts
  // of a chunk that are off screen; then every chunk's runs and their spheres (made with the
  // meshes below)
  out->chunk_run_count.assign(out->chunks.size(), 0);
  if (ctr_settings().run_culling) {
    constexpr u32 kCullRunIndices = 96 * 3;
    static_assert(kCullRunIndices == c3v::kRunIndices, "fine_vis units are these pieces");
    {
      // (AI-assisted) the runs added below, so vis_runs is made once (not grown by doubling)
      size_t runs = out->vis_runs.size();
      for (const auto& ch : out->chunks) {
        for (u32 k = 0; !ch.vis_run_count && k < ch.draw_count &&
                        ch.first_draw + k < out->draws.size();
             k++) {
          runs += (out->draws[ch.first_draw + k].index_count + kCullRunIndices - 1) /
                  kCullRunIndices;
        }
      }
      out->vis_runs.reserve(runs);
    }
    for (size_t ci = 0; ci < out->chunks.size(); ci++) {
      const auto& ch = out->chunks[ci];
      if (ch.vis_run_count) {
        out->chunk_run_count[ci] = ch.vis_run_count;
        continue;
      }
      const u32 first = (u32)out->vis_runs.size();
      for (u32 k = 0; k < ch.draw_count && ch.first_draw + k < out->draws.size(); k++) {
        const u32 di = ch.first_draw + k;
        // every draw is covered (a chunk whose runs are all hidden isn't drawn)
        const u32 n = out->draws[di].index_count;
        out->draw_first_run[di] = (u32)out->vis_runs.size();
        for (u32 i = 0; i < n; i += kCullRunIndices) {
          out->vis_runs.push_back(c3l::VisRun{(u16)k, 0xffff, std::min(kCullRunIndices, n - i)});
          out->draw_run_count[di]++;
        }
      }
      out->chunk_first_run[ci] = first;
      out->chunk_run_count[ci] = (u32)out->vis_runs.size() - first;
    }
    out->run_spheres.assign(out->vis_runs.size(), CtrLevelData::RunSphere{{0, 0, 0}, 0xffff});
  }
  load_tie_lod(f, hdr, runs_lo, file_runs, out);  // (AI-assisted) c3l v17
  // (AI-assisted, streaming) c3l v19: run spheres, near chunks
  load_stream_info(f, hdr, runs_lo, file_runs, out);
  load_proto_vis(f, hdr, runs_lo, file_runs, out);  // (AI-assisted, j2vis) c3l v20
  out->streaming = !out->stream_units.empty() && out->stream_units[0].state != CtrLevelData::kUnitNever &&
                   ctr_settings().streaming &&
                   CtrStreamArena::get().init(ctr_stream_kb() * 1024u);
  // (AI-assisted) fine_vis: the level's .c3v next to it, if it was made from this .c3l
  if (ctr_settings().fine_vis) {
    load_fine_vis(path, out);
  }

  load_tod(f, hdr, out);  // (AI-assisted) c3l v16

  std::vector<c3l::Texture> texs;
  if (!f.read_array(hdr.textures_offset, hdr.num_textures, &texs)) {
    lg::error("[ctr] {}: truncated file", path.string());
    return false;
  }
  {
    TexLoad tl;
    const bool ok = load_textures(f, hdr, texs, out->draws, name, &tl);
    // in the level also when cancelled: unloaded with it
    out->tex_pool = tl.pool;
    out->tex_pool_bytes = tl.pool_bytes;
    out->tex_background_bytes = tl.background_bytes;
    out->tex_empty = tl.empty;
    out->textures = std::move(tl.handles);
    out->tex_min_alpha = std::move(tl.min_alpha);
    if (!ok) {
      return false;
    }
  }

  out->path = path.string();
  out->vertex_data_offset = hdr.vertex_data_offset;
  out->index_data_offset = hdr.index_data_offset;
  out->merc_vertex_offset = hdr.merc_vertex_offset;
  out->merc_index_offset = hdr.merc_index_offset;
  // all indices at once (1-3 MB): reading them chunk by chunk between the vertex reads would seek
  // back and forth, and every seek throws away the read buffer. (AI-assisted) The reader's window
  // mode keeps a window for each (c3l_read_buffer): then every chunk's indices are read straight
  // into its index buffer, without the 1-3 MB piece of the heap (at the level-load peak the heap
  // has ~3 MB: maincave's 2.5 MB once failed with bad_alloc and the level had no background).
  const bool chunk_indices = f.window_mode();
  const u32 index_total = hdr.index_data_size / sizeof(u16);
  std::vector<u16> indices;
  if (!chunk_indices && !f.read_array(hdr.index_data_offset, index_total, &indices)) {
    lg::error("[ctr] {}: truncated file", path.string());
    return false;
  }
  out->meshes.reserve(out->chunks.size());  // (AI-assisted)
  out->mesh_index_first.reserve(out->chunks.size());
  out->mesh_index_end.reserve(out->chunks.size());
  // (AI-assisted, streaming) c3l v19: the draws' kDrawAlpha80 (worked out again below for the
  // chunks read now: the same), and the file bytes of a raw range (a near chunk's blocks)
  const bool v19 = hdr.version >= c3l::kStreamVersion;
  if (v19) {
    out->draw_alpha80.assign(out->draws.size(), 0);
    for (size_t d = 0; d < out->draws.size(); d++) {
      out->draw_alpha80[d] = (out->draws[d].kind & c3l::kDrawAlpha80) ? 1 : 0;
    }
  }
  auto packed_of = [&](u32 off, u32 bytes) {
    const auto& bl = f.blocks();
    if (bl.empty() || !bytes) {
      return bytes;
    }
    auto it = std::upper_bound(bl.begin(), bl.end(), off,
                               [](u32 o, const c3l::Block& b) { return o < b.raw_offset; });
    if (it != bl.begin()) {
      --it;
    }
    u32 n = 0;
    for (; it != bl.end() && it->raw_offset < off + bytes; ++it) {
      n += it->packed_size();
    }
    return n;
  };
  u32 stream_skipped = 0, stream_core_kb = 0;
  for (auto& ch : out->chunks) {
    if (cancelled()) {
      return false;
    }
    const size_t ci = out->meshes.size();
    // the chunk's draws cover a contiguous range of the index data
    u32 first = UINT32_MAX, end = 0;
    for (u32 d = ch.first_draw; d < ch.first_draw + ch.draw_count; d++) {
      first = std::min<u32>(first, out->draws[d].first_index);
      end = std::max<u32>(end, out->draws[d].first_index + out->draws[d].index_count);
    }
    if (first == UINT32_MAX) {
      out->meshes.push_back(-1);
      out->mesh_index_first.push_back(UINT32_MAX);
      out->mesh_index_end.push_back(0);
      continue;
    }
    out->mesh_index_first.push_back(first);
    out->mesh_index_end.push_back(end);
    // (AI-assisted, streaming) a near chunk: read later (streaming) or never (the ones the v18
    // budget left out, without streaming); the core ones without streaming now
    bool skip = false;
    if (out->is_unit(ci)) {
      auto& su = out->stream_units[out->chunk_unit[ci]];
      su.bytes = CtrStreamArena::mesh_bytes(ch.vertex_count, end - first);
      su.packed = packed_of(hdr.vertex_data_offset + ch.first_vertex * (u32)sizeof(c3l::Vertex),
                            ch.vertex_count * (u32)sizeof(c3l::Vertex)) +
                  packed_of(hdr.index_data_offset + first * (u32)sizeof(u16),
                            (end - first) * (u32)sizeof(u16));
      if (su.state == CtrLevelData::kUnitNever) {
        skip = true;
      } else if (out->streaming) {
        su.state = CtrLevelData::kUnitAbsent;
        skip = true;
      } else if (su.cls != c3l::kStreamCore || !ctr_settings().tie_lod) {
        // (tie_lod off: the near versions are never drawn)
        su.state = CtrLevelData::kUnitNever;
        skip = true;
      } else {
        su.state = CtrLevelData::kUnitResident;
        stream_core_kb += su.bytes / 1024;
      }
      stream_skipped += skip;
    }
    // (AI-assisted) the vertices are read (decoded) straight into GPU memory
    int mesh = -1;
    void* vdst;
    u16* idst;
    if (end <= index_total && !skip) {
      mesh = ctr_gpu_mesh_begin(ch.vertex_count, end - first, &vdst, &idst);
      if (mesh < 0 && make_room(name, "meshes")) {
        mesh = ctr_gpu_mesh_begin(ch.vertex_count, end - first, &vdst, &idst);
      }
    }
    if (mesh >= 0) {
      bool read_ok = true;
      if (chunk_indices) {
        read_ok = f.read(hdr.index_data_offset + first * (u32)sizeof(u16), idst,
                         (end - first) * sizeof(u16));
      } else {
        memcpy(idst, indices.data() + first, (end - first) * sizeof(u16));
      }
      read_ok = read_ok && f.read(hdr.vertex_data_offset + ch.first_vertex * sizeof(c3l::Vertex),
                                  vdst, ch.vertex_count * sizeof(c3l::Vertex));
      if (read_ok && !out->run_spheres.empty()) {
        // (AI-assisted, streaming) host check: the v19 file's spheres are these
        std::vector<CtrLevelData::RunSphere> file_sp;
        const u32 r0 = out->chunk_first_run[ci], nr = out->chunk_run_count[ci];
        if (v19 && stream_check_enabled() && ch.vis_run_count) {
          file_sp.assign(out->run_spheres.begin() + r0, out->run_spheres.begin() + r0 + nr);
        }
        make_run_spheres(out, (u32)out->meshes.size(), (const c3l::Vertex*)vdst, idst, first, end);
        for (u32 k = 0; k < file_sp.size(); k++) {
          const auto& a = file_sp[k];
          const auto& b = out->run_spheres[r0 + k];
          g_stream_check_spheres++;
          g_stream_check_sphere_diff += memcmp(&a, &b, sizeof(a)) != 0;
        }
      }
      if (read_ok) {
        // (AI-assisted) fog_alpha (CTR_TCC_ALPHA80): which draws have vertex alpha 0x80 everywhere.
        // The chunk's vertices first (nearly always all 0x80), the draws' indices only if not
        const c3l::Vertex* vv = (const c3l::Vertex*)vdst;
        bool all = true;
        for (u32 v = 0; v < ch.vertex_count && all; v++) {
          all = vv[v].rgba[3] == 0x80;
        }
        out->draw_alpha80.resize(out->draws.size(), 0);
        for (u32 d = ch.first_draw; d < ch.first_draw + ch.draw_count; d++) {
          bool ok = all;
          if (!ok) {
            ok = true;
            const auto& dr = out->draws[d];
            // (AI-assisted) the chunk's own copy (idst holds [first, end)): `indices` is empty in
            // the reader's window mode (chunk_indices), the default
            for (u32 i = dr.first_index; i < dr.first_index + dr.index_count && ok; i++) {
              const u16 ix = idst[i - first];
              ok = ix < ch.vertex_count && vv[ix].rgba[3] == 0x80;
            }
          }
          if (v19 && stream_check_enabled()) {
            g_stream_check_alpha++;
            g_stream_check_alpha_diff += out->draw_alpha80[d] != (ok ? 1 : 0);
          }
          out->draw_alpha80[d] = ok ? 1 : 0;
        }
      }
      // (AI-assisted) c3l v16: the chunk's light vertices, and its time of day colors (load_tod's
      // start weights) when there are some
      int tod_state = -1;  // 0: no time of day colors, 1: colored, 2: not yet
      u8 tod_mask = 0;
      if (out->tod) {
        tod_make_groups(out, (u32)out->meshes.size(), read_ok ? idst : nullptr, first, end);
      }
      if (read_ok && out->tod) {
        auto& t = *out->tod;
        CtrLevelData::Tod::LightList lights;
        const auto* vv = (c3l::Vertex*)vdst;
        tod_mask = tod_scan(t, vv, (int)ch.vertex_count, &lights);
        tod_state = tod_mask ? 2 : 0;
        if (tod_mask) {
          tod_mark_plain(t, (u32)out->meshes.size(), vv, (int)ch.vertex_count);
        }
        if (!lights.verts.empty()) {
          t.chunk_lights[(u32)out->meshes.size()] = std::move(lights);
        }
        if (tod_state == 2 && t.blended_valid) {
          tod_color_vertices(t, (c3l::Vertex*)vdst, 0, (int)ch.vertex_count);
          tod_state = 1;
        }
      }
      mesh = ctr_gpu_mesh_end(mesh, read_ok);
      if (mesh >= 0 && tod_state >= 0) {
        auto& t = *out->tod;
        t.chunk_mesh[ci] = mesh;
        t.chunk_mask[ci] = tod_mask;
        t.chunk_scanned[ci] = tod_state == 0 ? 1 : 2;
        if (tod_state == 1) {
          // colored with the start weights: every group at version 1
          for (u32 g = t.chunk_group_first[ci]; g < t.chunk_group_first[ci] + t.chunk_group_count[ci];
               g++) {
            t.groups[g].ver = t.ver;
            t.groups[g].dver = t.dver;
          }
          t.chunk_min_ver[ci] = t.ver;
          t.chunk_min_dver[ci] = t.dver;
          t.chunk_light_ver[ci] = t.ver;
        }
      }
    } else if (out->tod && skip) {
      tod_reserve_unit(out, (u32)ci);  // (AI-assisted, streaming)
    } else if (out->tod) {
      tod_make_groups(out, (u32)out->meshes.size(), nullptr, 0, 0);
    }
    out->meshes.push_back(mesh);
    // make the draws relative to the mesh's index buffer
    for (u32 d = ch.first_draw; d < ch.first_draw + ch.draw_count; d++) {
      out->draws[d].first_index -= first;
    }
  }
  // (AI-assisted, streaming) c3l v19 near chunks
  if (out->streaming) {
    out->file_dir = std::make_shared<const c3l::Reader::Dir>(f.dir());
  }
  if (!out->stream_units.empty()) {
    u64 all = 0, packed = 0;
    for (const auto& u : out->stream_units) {
      all += u.bytes;
      packed += u.packed;
    }
    u64 rest = 0;
    for (size_t ci = 0; ci < out->chunks.size() && ci < out->mesh_index_first.size(); ci++) {
      if (!out->is_unit(ci) && out->mesh_index_first[ci] != UINT32_MAX) {
        rest += CtrStreamArena::mesh_bytes(out->chunks[ci].vertex_count,
                                           out->mesh_index_end[ci] - out->mesh_index_first[ci]);
      }
    }
    lg::info("[ctr] {}: near chunks (v19): {} with {} KB ({} KB in the file), {}; the other chunks "
             "{} KB",
             name, out->stream_units.size(), all / 1024, packed / 1024,
             out->streaming ? std::string("streamed")
                            : fmt::format("{} KB of them loaded now (streaming off), {} never",
                                          stream_core_kb, stream_skipped),
             rest / 1024);
  }
  // bounds of the level (for the "seen from another level" mode)
  {
    bool first = true;
    for (auto& ch : out->chunks) {
      if (ch.lod_tier == 3) {
        out->has_lowres = true;
      }
      if (ch.kind == c3l::CHUNK_KIND_VISTA) {
        out->has_vista = true;  // (AI-assisted)
        continue;               // not in the level's bounds (far scenery)
      }
      if (ch.lod_tier > 1) {
        continue;
      }
      for (int c = 0; c < 3; c++) {
        const float lo = ch.bsphere[c] - ch.bsphere[3], hi = ch.bsphere[c] + ch.bsphere[3];
        out->bbox_min[c] = first ? lo : std::min(out->bbox_min[c], lo);
        out->bbox_max[c] = first ? hi : std::max(out->bbox_max[c], hi);
      }
      first = false;
    }
    // (AI-assisted) the detailed tfrag's bounds (lowres_vista): tier 1, or the tier 0 tfrag of
    // files with one tier
    bool any_tier1 = false;
    for (auto& ch : out->chunks) {
      any_tier1 |= ch.lod_tier == 1;
    }
    for (auto& ch : out->chunks) {
      const bool tfrag = ch.kind == c3l::CHUNK_KIND_TFRAG || ch.kind == c3l::CHUNK_KIND_UNKNOWN;
      if (!(any_tier1 ? ch.lod_tier == 1 : (ch.lod_tier == 0 && tfrag))) {
        continue;
      }
      for (int c = 0; c < 3; c++) {
        const float lo = ch.bsphere[c] - ch.bsphere[3], hi = ch.bsphere[c] + ch.bsphere[3];
        out->detail_min[c] = out->has_detail ? std::min(out->detail_min[c], lo) : lo;
        out->detail_max[c] = out->has_detail ? std::max(out->detail_max[c], hi) : hi;
      }
      out->has_detail = true;
    }
  }
  build_draw_order(out);
  // merc models: one skinned mesh per model
  if (hdr.num_merc_models) {
    std::vector<c3l::MercModel> models;
    std::vector<c3l::MercDraw> mdraws;
    // (AI-assisted) each model's indices are read with its vertices (the reader keeps a window for
    // each), not all of the level's at once (up to 250 KB of the heap in one piece)
    const u32 mindex_total = hdr.merc_index_size / sizeof(u16);
    std::vector<u16> mindices;
    if (!f.read_array(hdr.merc_models_offset, hdr.num_merc_models, &models) ||
        !f.read_array(hdr.merc_draw_offset, hdr.merc_draw_size / sizeof(c3l::MercDraw),
                      &mdraws)) {
      lg::error("[ctr] {}: truncated file", path.string());
      return false;
    }
    // blend shapes
    c3l::MercModHeader mh{};
    std::vector<c3l::MercModEffect> mod_effects;
    std::vector<c3l::MercModDest> mod_dests;
    std::vector<u8> mod_masks;
    if (hdr.merc_blerc_offset && hdr.merc_blerc_size >= sizeof(c3l::MercBlercHeader)) {
      c3l::MercBlercHeader bh{};
      u32 off = hdr.merc_blerc_offset;
      if (f.read(off, &bh, sizeof(bh))) {
        off += sizeof(bh);
        if (f.read_array(off, bh.num_vertices, &out->blerc_verts)) {
          off += bh.num_vertices * sizeof(c3l::MercBlercVertex);
          if (f.read_array(off, bh.num_targets, &out->blerc_targets)) {
            off += bh.num_targets * sizeof(c3l::MercBlercTarget);
            f.read_array(off, bh.num_dests, &out->blerc_dests);
          }
        }
      }
      if (out->blerc_dests.size() != bh.num_dests) {
        lg::warn("[ctr] {}: bad blend shape data", path.string());
        out->blerc_verts.clear();
        out->blerc_targets.clear();
        out->blerc_dests.clear();
      }
      // (AI-assisted) v12: vertices the game rewrites (ripple water, texture scroll)
      if (hdr.version >= 12 && bh.mod_offset) {
        u32 off = bh.mod_offset;
        bool ok = f.read(off, &mh, sizeof(mh));
        off += sizeof(mh);
        ok = ok && f.read_array(off, mh.num_effects, &mod_effects);
        off += mh.num_effects * sizeof(c3l::MercModEffect);
        ok = ok && f.read_array(off, mh.num_dests, &mod_dests);
        off += mh.num_dests * sizeof(c3l::MercModDest);
        ok = ok && f.read_array(off, mh.num_mask_bytes, &mod_masks);
        if (!ok) {
          lg::warn("[ctr] {}: bad merc vertex rewrite data", path.string());
          mod_effects.clear();
        }
      }
    }
    std::vector<c3l::MercVertex> mverts;
    // (AI-assisted) generated low LODs (c3l::kMercGenLodSuffix): the model they belong to (same
    // vertices) gets their draws as gen_draws, in one mesh with its own
    std::unordered_map<std::string, u32> gen_of;
    const size_t gen_suffix_len = strlen(c3l::kMercGenLodSuffix);
    for (u32 mi = 0; mi < models.size(); mi++) {
      const std::string n(models[mi].name, strnlen(models[mi].name, sizeof(models[mi].name)));
      if (n.size() > gen_suffix_len &&
          n.compare(n.size() - gen_suffix_len, gen_suffix_len, c3l::kMercGenLodSuffix) == 0) {
        gen_of[n.substr(0, n.size() - gen_suffix_len)] = mi;
      }
    }
    out->merc_models.reserve(models.size());  // (AI-assisted) no copies while it grows
    for (u32 mi = 0; mi < models.size(); mi++) {
      const auto& m = models[mi];
      if (cancelled()) {
        return false;
      }
      CtrMercModelData md;
      md.name = std::string(m.name, strnlen(m.name, sizeof(m.name)));
      md.scale = m.scale;
      if (!gen_of.empty() && md.name.find(c3l::kMercGenLodSuffix) != std::string::npos) {
        continue;  // (drawn as its model's gen_draws)
      }
      for (const auto& me : mod_effects) {
        if (me.model != mi || (u64)me.first_mask + me.frag_count > mod_masks.size() ||
            (u64)me.first_dest + me.dest_count > mod_dests.size() || m.scale <= 0.f) {
          continue;
        }
        auto& e = md.mod.emplace_back();
        e.effect = me.effect;
        e.mask.assign(mod_masks.begin() + me.first_mask,
                      mod_masks.begin() + me.first_mask + me.frag_count);
        e.vidx_end = me.vidx_end;
        for (u32 k = 0; k < me.dest_count; k++) {
          const auto& d = mod_dests[me.first_dest + k];
          if (d.vertex < m.vertex_count && d.src < me.vidx_end) {
            e.dests.push_back(d);
          }
        }
        e.pos_k = me.xyz_scale / m.scale;
        // Merc2::model_mod_draws: st = float bits (st_vif_add + byte) + st_magic
        for (u32 b = 0; b < 256; b++) {
          const u32 bits = me.st_vif_add + b;
          float st;
          memcpy(&st, &bits, 4);
          st += me.st_magic;
          e.st_lut[b] = (s16)std::clamp((int)std::lround(st * 1024.f), -32768, 32767);
        }
      }
      if (m.blerc_first + m.blerc_count <= out->blerc_verts.size()) {
        md.blerc_first = m.blerc_first;
        md.blerc_count = m.blerc_count;
      }
      if (m.first_draw + m.draw_count > mdraws.size()) {
        continue;
      }
      md.draws.assign(mdraws.begin() + m.first_draw,
                      mdraws.begin() + m.first_draw + m.draw_count);
      if (md.draws.empty()) {
        continue;
      }
      {
        auto git = gen_of.find(md.name);
        if (git != gen_of.end()) {
          const auto& g = models[git->second];
          if (g.first_vertex == m.first_vertex && g.vertex_count == m.vertex_count &&
              g.first_draw + g.draw_count <= mdraws.size()) {
            md.gen_draws.assign(mdraws.begin() + g.first_draw,
                                mdraws.begin() + g.first_draw + g.draw_count);
          }
        }
      }
      u32 first = UINT32_MAX, end = 0;
      for (const auto* ds : {&md.draws, &md.gen_draws}) {
        for (const auto& d : *ds) {
          first = std::min<u32>(first, d.first_index);
          end = std::max<u32>(end, d.first_index + d.index_count);
        }
      }
      for (auto* ds : {&md.draws, &md.gen_draws}) {
        for (auto& d : *ds) {
          d.first_index -= first;
        }
      }
      if (end <= mindex_total) {
        md.file_vertex = m.first_vertex;
        md.vertex_count = m.vertex_count;
        md.index_first = first;
        md.index_end = end;
        if (f.read_array(hdr.merc_vertex_offset + m.first_vertex * sizeof(c3l::MercVertex),
                         m.vertex_count, &mverts) &&
            f.read_array(hdr.merc_index_offset + first * (u32)sizeof(u16), end - first,
                         &mindices)) {
          md.mesh = ctr_gpu_skinned_mesh_create(mverts.data(), m.vertex_count, mindices.data(),
                                                end - first);
          if (md.mesh < 0 && make_room(name, "the models")) {
            md.mesh = ctr_gpu_skinned_mesh_create(mverts.data(), m.vertex_count, mindices.data(),
                                                  end - first);
          }
        }
      }
      out->merc_models.push_back(std::move(md));
    }
  }
  return true;
}

void CtrLevels::process_pending_loads(u64 frame) {
  m_render_frame.store(frame);
#ifndef __3DS__
  // (AI-assisted, j2-mem) host: env CTR_MEM_LOG_EVERY=N, the memory line every N frames
  static const u64 s_mem_every = getenv("CTR_MEM_LOG_EVERY") ? atoi(getenv("CTR_MEM_LOG_EVERY")) : 0;
  if (s_mem_every && frame % s_mem_every == 0) {
    log_memory();
  }
#endif
  // (AI-assisted) parked prefetches the game gave up on, room for the loader
  housekeeping(false);
  retry_failed_loads(frame);
  for (const auto& name : m_pending_loads) {
    if (!m_levels.count(name) && !m_prefetched.count(name) && !m_missing.count(name)) {
      request({name, false, false});
    }
  }
  m_pending_loads.clear();
  if (!m_thread) {
    // no loader thread: run the queued loads here
    for (;;) {
      Job job;
      {
        std::lock_guard<std::mutex> lk(m_lock);
        if (m_queue.empty()) {
          break;
        }
        job = m_queue.front();
        m_queue.pop_front();
      }
      run_job(job);
    }
  }
  // finished loads
  std::vector<Done> done;
  {
    std::lock_guard<std::mutex> lk(m_lock);
    done.swap(m_done);
    for (auto& d : done) {
      m_requested.erase(d.job.key());
    }
  }
#ifndef __3DS__
  // (AI-assisted, streaming) host, env CTR_STREAM_SIM=MBPS: a stream job is taken when the New 3DS's
  // SD card (MBPS MB/s, 8 ms per request) and decoder (100 MB/s) would have finished it, in frames of
  // the fps cap (game time: the host's frames run as fast as they can)
  static const double sim_mbps =
      getenv("CTR_STREAM_SIM") ? std::max(0.5, atof(getenv("CTR_STREAM_SIM"))) : 0.0;
  if (sim_mbps > 0) {
    const double frame_ms = 1000.0 / std::max(1, ctr_settings().fps_cap);
    for (auto it = done.begin(); it != done.end();) {
      if (!it->job.stream) {
        ++it;
        continue;
      }
      if (!it->ready_frame) {
        u64 raw = 0;
        for (const auto& p : it->job.stream_parts) {
          raw += (u64)p.vertex_count * 16 + (u64)(p.index_end - p.index_first) * 2;
        }
        const double ms = it->reads * 8.0 + it->read_bytes / (sim_mbps * 1048.576) +
                          raw / (100.0 * 1048.576);
        it->ready_frame = frame + (u64)std::ceil(ms / frame_ms);
      }
      if (frame < it->ready_frame) {
        m_stream_delayed.push_back(std::move(*it));
        it = done.erase(it);
      } else {
        ++it;
      }
    }
    for (auto it = m_stream_delayed.begin(); it != m_stream_delayed.end();) {
      if (frame >= it->ready_frame) {
        done.push_back(std::move(*it));
        it = m_stream_delayed.erase(it);
      } else {
        ++it;
      }
    }
  }
#endif
#ifndef __3DS__
  if (load_sim_on()) {
    // (AI-assisted, j2intro) host, CTR_LOAD_DELAY_MS: level loads whose time hasn't come wait (still
    // requested: ready() says no), the others are taken now
    const double tnow = ctr_gpu_time_ms();
    std::lock_guard<std::mutex> lk(m_lock);
    for (auto it = done.begin(); it != done.end();) {
      if (it->sim_ready_ms > tnow) {
        m_requested.insert(it->job.key());
        m_load_delayed.push_back(std::move(*it));
        it = done.erase(it);
      } else {
        ++it;
      }
    }
    for (auto it = m_load_delayed.begin(); it != m_load_delayed.end();) {
      if (it->sim_ready_ms <= tnow) {
        m_requested.erase(it->job.key());
        done.push_back(std::move(*it));
        it = m_load_delayed.erase(it);
      } else {
        ++it;
      }
    }
  }
#endif
  bool levels_changed = false;
  bool unloaded = false;
  const double now = ctr_gpu_time_ms();
  for (auto& d : done) {
    const std::string& name = d.job.name;
    if (d.job.stream) {
      take_stream(d, frame);  // (AI-assisted, streaming)
      continue;
    }
    if (d.job.parts()) {
      // (AI-assisted) partial unload / repair: the meshes made again
      take_parts(d, frame);
      continue;
    }
    if (d.job.common) {
      if (d.result == LoadResult::LOADED) {
        m_common = std::move(d.lev);
        // Jak and the shared models: on screen all the time, and small
        ctr_gpu_pool_set_priority(m_common->tex_pool, 3);
        rebuild_merc_index();
      } else {
        lg::warn("[ctr] no common models ({}/GAME.c3l): Jak won't be drawn",
                 ctr_level_c3l_dir().string());
      }
      continue;
    }
    switch (d.result) {
      case LoadResult::MISSING:
        lg::warn("[ctr] no background for level {} ({}/{}.c3l)", name, ctr_level_c3l_dir().string(),
                 name);
        m_missing[name] = true;
        break;
      case LoadResult::FAILED:
        // (AI-assisted) not for good (a failed allocation or SD read): again when the heap's
        // largest free block grew, or in 10 s (retry_failed_loads, get)
        m_failed[name] = FailedLoad{now, d.heap_largest};
        break;
      case LoadResult::CANCELLED:
        break;
      case LoadResult::LOADED: {
        bool keep;
        {
          std::lock_guard<std::mutex> lk(m_lock);
          keep = parked_matters(name, now);
        }
        if (m_levels.count(name) || m_prefetched.count(name)) {
          unload(*d.lev);  // loaded twice (asked for again while it loaded)
        } else if (std::find(m_wanted.begin(), m_wanted.end(), name) != m_wanted.end() ||
                   (!d.job.prefetch && m_wanted.empty())) {
          publish(name, std::move(d.lev), frame);
          levels_changed = true;
        } else if (keep) {
          // (AI-assisted) a prefetch, or a load the game no longer wants (it changed its level list
          // while this one loaded), of the level whose DGO the game loads: kept aside, dropped if
          // nothing asks for it
          m_prefetched[name] = Prefetched{std::move(d.lev), now};
          levels_changed = true;
        } else {
          // (AI-assisted) the game gave up on it (it loads another level now): its memory is for
          // that one
          lg::info("[ctr] {} loaded but no longer wanted, unloading", name);
          unload(*d.lev);
          unloaded = true;
        }
        break;
      }
    }
  }
  // prefetched levels nothing asked for
  for (auto it = m_prefetched.begin(); it != m_prefetched.end();) {
    if (now - it->second.since_ms > kPrefetchKeepMs) {
      lg::info("[ctr] dropping prefetched {} (not used)", it->first);
      unload(*it->second.lev);
      it = m_prefetched.erase(it);
      levels_changed = true;
      unloaded = true;
    } else {
      ++it;
    }
  }
  if (unloaded) {
    ctr_gpu_free_pending_now();  // (AI-assisted) before the loader goes on
  }
  if (levels_changed) {
    update_level_count();
  }
  stream_take_pending(frame);  // (AI-assisted, streaming) the near chunks read, colored
  stream_update(frame);        // (AI-assisted, streaming) the near chunks around the camera
  // (AI-assisted) levels that loaded with parts left out (no memory then): made again once the last
  // frame's free linear memory covers them (another level was unloaded, or this one's textures
  // moved to VRAM), but not while a level loads (the game waits for that one: its memory first)
  if (frame % kRepairCheckFrames == 0) {
    bool level_loading = false;
    {
      std::lock_guard<std::mutex> lk(m_lock);
      for (const auto& key : m_requested) {
        level_loading |= key.find('#') == std::string::npos;  // not a detail reload / repair
      }
    }
    ctr_gpu_stats st;
    ctr_gpu_get_stats(&st);
    m_stream_repair_room = 0;
    for (auto& [name, lev] : m_levels) {
      if (level_loading || !lev->incomplete || lev->repair_requested || lev->detail_requested ||
          frame < lev->repair_retry_frame) {
        continue;
      }
      u32 bytes = 0;
      const int n = missing_parts(*lev, &bytes);
      if (!n) {
        lev->incomplete = false;  // unloaded meanwhile (partial unload)
      } else if (st.linear_free >= bytes + kRepairMargin) {
        lg::info("[ctr] {}: making {} missing parts again ({} KB, {} KB free)", name, n,
                 bytes / 1024, st.linear_free / 1024);
        request_repair(*lev);
      } else {
        // (AI-assisted, streaming) the stream arena's growth makes way for it (stream_update)
        m_stream_repair_room += bytes + kRepairMargin;
      }
    }
  }
}

void CtrLevels::retry_failed_loads(u64 frame) {
  if (m_failed.empty() || frame % kRepairCheckFrames != 0) {
    return;
  }
  const double now = ctr_gpu_time_ms();
  u32 largest = 0;
  bool have_largest = false;
  for (auto it = m_failed.begin(); it != m_failed.end();) {
    const std::string& name = it->first;
    const double since = now - it->second.ms;
    const bool wanted = it->second.drawn ||
                        std::find(m_wanted.begin(), m_wanted.end(), name) != m_wanted.end();
    if (!wanted || m_levels.count(name) || m_prefetched.count(name) || since < kFailedRetryMinMs) {
      ++it;
      continue;
    }
    bool retry = since >= kFailedRetryMs;
#ifdef __3DS__
    if (!retry) {
      if (!have_largest) {
        CtrHeapStats st;
        ctr_heap_stats(&st);
        largest = st.largest;
        have_largest = true;
      }
      retry = largest >= it->second.largest + kFailedRetryGrowth;
    }
#endif
    if (!retry) {
      ++it;
      continue;
    }
    lg::info("[ctr] {}: loading again ({:.1f} s after it failed{})", name, since / 1000.0,
             have_largest ? fmt::format("; largest free heap block {} KB, was {} KB",
                                        largest / 1024, it->second.largest / 1024)
                          : "");
    if (std::find(m_pending_loads.begin(), m_pending_loads.end(), name) == m_pending_loads.end()) {
      m_pending_loads.push_back(name);
    }
    it = m_failed.erase(it);
  }
  (void)largest;
}

void CtrLevels::unload(CtrLevelData& lev) {
  bump_set_generation();  // (AI-assisted)
  if (lev.fine) {
    lev.fine->cancel();  // (AI-assisted) fine_vis: its queued block reads are dropped
    lev.fine.reset();
  }
  for (int t : lev.textures) {
    ctr_gpu_tex_delete(t);
  }
  if (lev.tex_pool >= 0) {
    ctr_gpu_pool_delete(lev.tex_pool);  // after its textures (freed together, after the frame)
    lev.tex_pool = -1;
  }
  for (int m : lev.meshes) {
    ctr_gpu_mesh_delete(m);
  }
  for (auto& m : lev.merc_models) {
    ctr_gpu_mesh_delete(m.mesh);
  }
  lev.merc_models.clear();
  lev.textures.clear();
  lev.meshes.clear();
}

void CtrLevels::load_common() {
  // on the loader thread (from the first frame without one): level textures always go to linear
  // memory, so creating them away from the render thread doesn't touch the GX queue
  m_common_wanted = true;
  request({"GAME", true, false});
}

void CtrLevels::rebuild_merc_index() {
  m_merc_index.clear();
  if (m_common) {
    for (size_t i = 0; i < m_common->merc_models.size(); i++) {
      m_merc_index[m_common->merc_models[i].name] = {m_common.get(), (int)i};
    }
  }
  for (auto& [name, lev] : m_levels) {
    for (size_t i = 0; i < lev->merc_models.size(); i++) {
      m_merc_index[lev->merc_models[i].name] = {lev.get(), (int)i};
    }
  }
}

const CtrMercModelData* CtrLevels::find_merc_model(std::string_view name,
                                                   const CtrLevelData** lev) {
  auto it = m_merc_index.find(name);
  if (it == m_merc_index.end()) {
    return nullptr;
  }
  *lev = it->second.first;
  return &it->second.first->merc_models[it->second.second];
}

CtrLevelData* CtrLevels::get(const std::string& name, u64 frame) {
  auto it = m_levels.find(name);
  if (it == m_levels.end()) {
    // prefetched: drawn from now on
    auto pf = m_prefetched.find(name);
    if (pf != m_prefetched.end()) {
      auto lev = std::move(pf->second.lev);
      m_prefetched.erase(pf);
      publish(name, std::move(lev), frame);
      update_level_count();
      return m_levels[name].get();
    }
    // loading starts before the next frame (process_pending_loads); not right after a failed load
    auto failed = m_failed.find(name);
    if (failed != m_failed.end()) {
      failed->second.drawn = true;  // (AI-assisted) loaded again by retry_failed_loads
      return nullptr;
    }
    if (!m_missing.count(name) &&
        std::find(m_pending_loads.begin(), m_pending_loads.end(), name) == m_pending_loads.end()) {
      m_pending_loads.push_back(name);
    }
    return nullptr;
  }
  it->second->last_used_frame = frame;
  return it->second.get();
}

void CtrLevels::set_wanted(const std::vector<std::string>& names) {
  m_wanted = names;
  // stop loads of levels that are no longer wanted (prefetches are for levels to come: kept; the
  // ones the game gave up on are dropped by prefetch / housekeeping)
  {
    std::lock_guard<std::mutex> lk(m_lock);
    auto wanted = [&](const std::string& n) {
      return std::find(names.begin(), names.end(), n) != names.end();
    };
    for (auto it = m_queue.begin(); it != m_queue.end();) {
      if (!it->common && !it->prefetch && !wanted(it->name)) {
        m_requested.erase(it->key());
        if (it->stream) {
          free_stream_parts(*it);  // (AI-assisted, streaming) its arena pieces back
          m_stream_inflight -= std::min<u32>(m_stream_inflight, 1);
        }
        it = m_queue.erase(it);
      } else {
        ++it;
      }
    }
    if (!m_loading.empty() && !m_loading_prefetch && m_loading != "GAME" && !wanted(m_loading)) {
      m_cancel = true;
    }
    // (AI-assisted) the levels ready() said yes to are in the list now, or the game dropped them
    m_promised.clear();
  }
  m_cv.notify_all();  // (AI-assisted) a job waiting for room (make_room) sees the cancel
  bool changed = false;
  // prefetched levels the game now wants
  for (auto& n : names) {
    auto pf = m_prefetched.find(n);
    if (pf != m_prefetched.end() && !m_levels.count(n)) {
      auto lev = std::move(pf->second.lev);
      m_prefetched.erase(pf);
      publish(n, std::move(lev), 0);
      changed = true;
    }
  }
  for (auto it = m_levels.begin(); it != m_levels.end();) {
    bool wanted = false;
    for (auto& n : names) {
      wanted |= n == it->first;
    }
    if (!wanted) {
      lg::info("[ctr] unloading {}", it->first);
      unload(*it->second);
      it = m_levels.erase(it);
      rebuild_merc_index();
      bump_set_generation();  // (AI-assisted)
      changed = true;
    } else {
      ++it;
    }
  }
  if (changed) {
    // (AI-assisted) give the unloaded levels' memory back now: the next level's load starts on
    // the loader thread right away, and while the game waits for it no frame is drawn, so the
    // deferred deletes waited (a level without textures or chunks for good, or a 20 s wait)
    ctr_gpu_free_pending_now();
    update_level_count();
  }
  housekeeping(false);  // (AI-assisted) the render thread is idle: as at its next frame
  // Load the wanted levels before the next frame, not only when their background is drawn: a level
  // can be loaded just for its models. The intro loads the "intro" level (Gol and Maia) next to
  // misty with display mode special, so it never draws a background and was never loaded.
  // (AI-assisted) A level whose load failed is loaded again by retry_failed_loads (when the heap
  // has room for it, at most a second after the levels unloaded here made it).
  for (auto& n : names) {
    if (n.empty() || m_levels.count(n) || m_missing.count(n) || m_failed.count(n) ||
        std::find(m_pending_loads.begin(), m_pending_loads.end(), n) != m_pending_loads.end()) {
      continue;
    }
    m_pending_loads.push_back(n);
  }
}

// ---------------------------------------------------------------------------
// (AI-assisted, streaming) Streaming of the c3l v19 near chunks (docs/3ds-port/streaming.md)
// ---------------------------------------------------------------------------

namespace {
// a unit is wanted within its need (CtrLevelData::StreamUnit) plus this of the camera, now or where
// it will be in kLeadFrames at its speed (and halfway there); kept (not freed for another) until
// kKeepMeters beyond it
constexpr float kWantMeters = 20.f, kKeepMeters = 45.f;
constexpr float kLeadFrames = 60.f;  // 2 s at 30 fps
constexpr int kLeadSteps = 4;        // positions: now, 1/3, 2/3, all of the lead
constexpr u32 kJobsInFlight = 2;     // a second job queued behind the one being read
// while nothing is wanted: the units within this of their bound read into the arena's free room
// (no freeing, no growth), one small job every kIdleFrames: more is there when the camera cuts
constexpr float kIdleMeters = 250.f;
constexpr u64 kIdleFrames = 8;
constexpr u32 kIdleBytes = 128 * 1024;
// a unit needed now and in view goes this much ahead of the others (after a cut: what is on
// screen first)
constexpr float kInViewMeters = 300.f;
// a jump of the camera of more than this in a frame (a warp, a cut): no speed
constexpr float kCutMeters = 40.f;
// a stream job: at most this many file bytes, units (one level load waits for it at most this)
constexpr u32 kBatchBytes = 256 * 1024, kBatchUnits = 24;
// the arena's growth (stream_update; the floor: config stream_reserve_kb): taken while the linear
// heap keeps the floor + kGrowHeadroom free after it, by what is needed now and 2 s ahead +
// kGrowMargin (or 1/8 of it); given back under the floor + kPressure (the units not needed now),
// under the floor (all), while a level loads under the floor + kLoadRoom
constexpr u32 kGrowHeadroom = 256 * 1024, kPressure = 128 * 1024;
constexpr u32 kGrowMargin = 1024 * 1024;
constexpr u32 kLoadRoom = 12u * 1024 * 1024;

// The time of day groups of a near chunk read by a stream job (loader thread): as tod_make_groups
// (one per visibility id of its runs when their vertex ranges don't overlap, in vertex order;
// else one), from the job's copy of its draws and runs; plain: every vertex has a table entry
// (ntab). Returns false for one group of all of it.
void stream_tod_layout(const CtrLevels* /*unused*/,
                       u32 vertex_count,
                       u32 index_count,
                       const std::vector<c3l::Draw>& draws,
                       const std::vector<c3l::VisRun>& runs,
                       const u16* idst,
                       const c3l::Vertex* v,
                       u32 ntab,
                       std::vector<CtrLevelData::Tod::VisGroup>* groups,
                       std::vector<u16>* run_group) {
  using G = CtrLevelData::Tod::VisGroup;
  groups->clear();
  run_group->assign(runs.size(), 0);
  auto one = [&]() {
    groups->clear();
    groups->push_back(G{0, (u16)(std::max<u32>(vertex_count, 1) - 1), 0});
    run_group->assign(runs.size(), 0);
  };
  bool ok = vertex_count >= 2 && !runs.empty();
  std::vector<std::pair<u16, u16>> ids;
  std::vector<G> gs;
  // per draw its runs in order (the file keeps a draw's runs together)
  for (u32 k = 0; ok && k < draws.size(); k++) {
    u32 idx = draws[k].first_index;
    for (size_t r = 0; ok && r < runs.size(); r++) {
      if (runs[r].draw != k) {
        continue;
      }
      const u32 n = runs[r].index_count;
      if (idx + n > index_count) {
        ok = false;
        break;
      }
      const u16 vis = runs[r].vis;
      u16 g = 0xffff;
      for (const auto& [id, gi] : ids) {
        if (id == vis) {
          g = gi;
          break;
        }
      }
      if (g == 0xffff) {
        g = (u16)gs.size();
        ids.emplace_back(vis, g);
        gs.push_back(G{0xffff, 0, 0});
      }
      (*run_group)[r] = g;
      for (u32 i = idx; i < idx + n; i++) {
        gs[g].lo = std::min(gs[g].lo, idst[i]);
        gs[g].hi = std::max(gs[g].hi, idst[i]);
      }
      idx += n;
    }
  }
  if (!ok || gs.size() < 2 || gs.size() > 0xfffe) {
    one();
  } else {
    std::vector<std::pair<u16, u16>> by_lo;
    for (const auto& g : gs) {
      by_lo.emplace_back(g.lo, g.hi);
    }
    std::sort(by_lo.begin(), by_lo.end());
    for (size_t k = 1; k < by_lo.size(); k++) {
      if (by_lo[k].first <= by_lo[k - 1].second) {
        ok = false;
        break;
      }
    }
    if (!ok) {
      one();
    } else {
      std::vector<u16> order(gs.size()), slot(gs.size());
      for (size_t g = 0; g < gs.size(); g++) {
        order[g] = (u16)g;
      }
      std::sort(order.begin(), order.end(), [&](u16 a, u16 b) { return gs[a].lo < gs[b].lo; });
      for (size_t k = 0; k < order.size(); k++) {
        slot[order[k]] = (u16)k;
        groups->push_back(gs[order[k]]);
      }
      for (auto& rg : *run_group) {
        rg = slot[rg];
      }
    }
  }
  for (auto& g : *groups) {
    bool plain = ntab > 0;
    for (u32 i = g.lo; i <= std::min<u32>(g.hi, vertex_count - 1) && plain; i++) {
      plain = (u16)v[i].pad < ntab;
    }
    g.plain = plain ? 1 : 0;
    g.ver = 0;
    g.dver = 0;
  }
}
}  // namespace

void CtrLevels::free_stream_parts(const Job& job) {
  for (const auto& p : job.stream_parts) {
    CtrStreamArena::get().free(p.mem);
  }
}

void CtrLevels::stream_note_camera(const math::Vector4f& t,
                                   const math::Vector4f planes[4],
                                   const math::Vector4f rot[4],
                                   u64 frame) {
  if (m_cam_valid && frame == m_cam_frame) {
    return;
  }
  // Jak's world place: camera position + rot (rows) times his first bone's tmat[3]
  if (m_target_cam_frame + 2 >= frame) {
    for (int i = 0; i < 3; i++) {
      m_target[i] = t[i] + rot[i][0] * m_target_cam[0] + rot[i][1] * m_target_cam[1] +
                    rot[i][2] * m_target_cam[2];
    }
    m_target_frame = frame;
  }
  for (int i = 0; i < 4; i++) {
    m_planes[i] = planes[i];
  }
  if (m_cam_valid && frame > m_cam_frame) {
    const float n = (float)std::min<u64>(frame - m_cam_frame, 8);
    float v[3], d2 = 0;
    for (int c = 0; c < 3; c++) {
      v[c] = (t[c] - m_cam[c]) / n;
      d2 += v[c] * v[c];
    }
    if (d2 > kCutMeters * kCutMeters * 4096.f * 4096.f) {
      for (float& c : m_cam_vel) {
        c = 0;
      }
      m_cut_frame = frame;
    } else {
      for (int c = 0; c < 3; c++) {
        m_cam_vel[c] = 0.75f * m_cam_vel[c] + 0.25f * v[c];
      }
    }
  }
  for (int c = 0; c < 3; c++) {
    m_cam[c] = t[c];
  }
  m_cam_frame = frame;
  m_cam_valid = true;
  m_drawn_frame.store(frame);
}

void CtrLevels::stream_late(CtrLevelData& lev, u32 unit, u64 frame) {
  auto& su = lev.stream_units[unit];
  const double now = ctr_gpu_time_ms();
  if (su.late_ms > 0 && frame > su.late_last + 2) {
    // the last time ended without it (the camera went away): that time's length
    stream_late_end(su, su.late_last_ms, su.late_last + 1);
  }
  if (su.late_ms <= 0) {
    su.late_ms = now;
    su.late_frame = frame;
    su.late_cut = m_cut_frame && frame <= m_cut_frame + 60;
    m_sst.late_events++;
    // (being read already, or not asked for yet: the reads too slow, or the need not foreseen /
    // no room)
    (su.state == CtrLevelData::kUnitLoading ? m_sst.late_loading : m_sst.late_absent)++;
    if (!su.want_frame) {
      m_sst.late_unwanted++;
    } else {
      m_sst.late_wanted_frames += frame - std::min(frame, su.want_frame);
    }
    {
#ifndef __3DS__
      static const bool dbg_env = getenv("CTR_STREAM_DEBUG") != nullptr;
#else
      constexpr bool dbg_env = false;
#endif
      if (dbg_env || ctr_settings().stream_debug) {
      lg::info("[ctr] stream late: {} unit {} ({} KB) {} frame {}, wanted since {}, last score {:.1f} "
               "m, speed {:.1f} m/s, jobs in flight {}",
               lev.name, unit, su.bytes / 1024,
               su.state == CtrLevelData::kUnitLoading ? "loading" : "absent", frame, su.want_frame,
               su.last_score / 4096.f,
               std::sqrt(m_cam_vel[0] * m_cam_vel[0] + m_cam_vel[1] * m_cam_vel[1] +
                         m_cam_vel[2] * m_cam_vel[2]) * 30.f / 4096.f,
               m_stream_inflight);
      }
    }
  }
  su.late_last_ms = now;
  su.late_last = frame;
  if (stream_check_enabled() && su.state == CtrLevelData::kUnitAbsent &&
      frame > su.late_frame + 2) {
    m_scheck.late_unrequested++;
  }
}

void CtrLevels::stream_late_end(CtrLevelData::StreamUnit& su, double end_ms, u64 end_frame) {
  const double late = std::max(0.0, end_ms - su.late_ms);
  const u64 frames = end_frame - std::min(end_frame, su.late_frame);
  m_sst.late_periods++;
  m_sst.late_ms_total += late;
  m_sst.late_ms_max = std::max(m_sst.late_ms_max, late);
  m_sst.late_frames_total += frames;
  m_sst.late_frames_max = std::max<u64>(m_sst.late_frames_max, frames);
  if (su.room_frame && su.room_frame + 2 >= su.late_frame) {
    // (the arena had no room for it during this time, or it was freed under memory pressure)
    m_sst.late_room++;
    m_sst.late_room_ms += late;
    m_sst.late_room_max = std::max(m_sst.late_room_max, late);
  }
  if (su.late_cut) {
    m_sst.late_cut++;
    m_sst.late_cut_ms += late;
    m_sst.late_cut_max = std::max(m_sst.late_cut_max, late);
  } else {
    m_sst.late_other++;
    m_sst.late_other_ms += late;
    m_sst.late_other_max = std::max(m_sst.late_other_max, late);
  }
  su.late_ms = 0;
}

void CtrLevels::stream_note_level(CtrLevelData& lev, u32 late_groups, u32 held_groups,
                                  u64 frame) {
  (void)lev;
  if (held_groups) {
    m_sst.held_group_frames += held_groups;
    if (m_sst.last_held_frame != frame) {
      m_sst.last_held_frame = frame;
      m_sst.held_frames++;
    }
  }
  if (late_groups) {
    m_sst.late_group_frames += late_groups;
    if (m_sst.last_late_frame != frame) {
      m_sst.last_late_frame = frame;
      m_sst.late_frames++;
    }
  }
}

void CtrLevels::stream_evict(CtrLevelData& lev, u32 unit) {
  auto& su = lev.stream_units[unit];
  const u32 ci = su.chunk;
  if (lev.meshes[ci] >= 0) {
    ctr_gpu_mesh_delete(lev.meshes[ci]);  // the arena piece back after the GPU is done with it
    lev.meshes[ci] = -1;
    CtrStreamArena::get().mark_pending(su.mem);
  }
  if (lev.tod && ci < lev.tod->chunk_mesh.size()) {
    lev.tod->chunk_mesh[ci] = -1;
  }
  su.state = CtrLevelData::kUnitAbsent;
  su.mem = nullptr;
  lev.stream_resident -= std::min(lev.stream_resident, su.bytes);
  m_sst.evictions++;
}

u32 CtrLevels::stream_shrink(bool all) {
  auto& arena = CtrStreamArena::get();
  if (!arena.ready() || !arena.has_growth()) {
    return 0;
  }
  u32 n = 0;
  const float kM = 4096.f;
  for (auto& [name, lp] : m_levels) {
    CtrLevelData& lev = *lp;
    for (u32 u = 0; u < lev.stream_units.size(); u++) {
      auto& su = lev.stream_units[u];
      if (su.state != CtrLevelData::kUnitResident || !su.mem || !arena.in_growth(su.mem)) {
        continue;
      }
      if (!all) {
        // not needed now (the camera beyond its groups' near distances by the keep margin)
        const float* c = lev.chunks[su.chunk].bsphere;
        const float dx = m_cam[0] - c[0], dy = m_cam[1] - c[1], dz = m_cam[2] - c[2];
        const float k = lev.stream_k;
        if (k > 0 && std::sqrt(dx * dx + dy * dy + dz * dz) - (su.need_a + k * su.need_e) <
                         kKeepMeters * kM) {
          continue;
        }
      }
      su.room_frame = m_cam_frame;  // (statistics: freed for the linear heap)
      stream_evict(lev, u);
      n++;
    }
  }
  return n;
}

// Render thread: nothing wanted, the loader idle: the nearest units within kIdleMeters of their
// bound (m_idle) into the arena's free room, one small job (non-urgent: behind level loads).
void CtrLevels::stream_idle_fill(u64 frame) {
  auto& arena = CtrStreamArena::get();
  std::sort(m_idle.begin(), m_idle.end(),
            [](const StreamCand& a, const StreamCand& b) { return a.score < b.score; });
  CtrLevelData& lev = *m_idle[0].lev;
  Job job;
  job.name = lev.name;
  job.stream = true;
  job.generation = lev.generation;
  job.path = lev.path;
  job.vertex_data_offset = lev.vertex_data_offset;
  job.index_data_offset = lev.index_data_offset;
  job.tod_ntab = lev.tod ? (u32)lev.tod->table.size() : 0;
  job.dir = lev.file_dir;
  u32 packed = 0;
  for (const auto& w : m_idle) {
    if (w.lev != &lev) {
      continue;
    }
    auto& su = lev.stream_units[w.unit];
    void* mem = arena.alloc(su.bytes);
    if (!mem) {
      continue;  // (no room: none freed for it)
    }
    const u32 ci = su.chunk;
    const auto& ch = lev.chunks[ci];
    Job::StreamPart part;
    part.unit = w.unit;
    part.chunk = ci;
    part.first_vertex = ch.first_vertex;
    part.vertex_count = ch.vertex_count;
    part.index_first = lev.mesh_index_first[ci];
    part.index_end = lev.mesh_index_end[ci];
    part.mem = mem;
    part.layout = lev.tod && !su.tod_layout;
    if (part.layout) {
      part.draws.assign(lev.draws.begin() + ch.first_draw,
                        lev.draws.begin() + ch.first_draw + ch.draw_count);
      const u32 r0 = lev.chunk_first_run[ci];
      part.runs.assign(lev.vis_runs.begin() + r0, lev.vis_runs.begin() + r0 + ch.vis_run_count);
    }
    su.state = CtrLevelData::kUnitLoading;
    su.mem = mem;
    job.stream_parts.push_back(std::move(part));
    packed += su.packed;
    m_sst.idle_units++;
    if (packed >= kIdleBytes || job.stream_parts.size() >= kBatchUnits) {
      break;
    }
  }
  m_idle_frame = frame;
  if (job.stream_parts.empty()) {
    return;
  }
  std::sort(job.stream_parts.begin(), job.stream_parts.end(),
            [](const Job::StreamPart& a, const Job::StreamPart& b) {
              return a.first_vertex < b.first_vertex;
            });
  job.window = c3l_read_window();
  m_stream_inflight++;
  m_sst.idle_jobs++;
  job.serial = ++m_stream_serial;
  request(job);
}

// Render thread, each frame (process_pending_loads): the units each level needs around the camera
// (now and where its speed takes it), the most urgent first, into a stream job for one level (one
// job at a time); the arena's room made by freeing the units furthest beyond their need.
void CtrLevels::stream_update(u64 frame) {
  const double t0 = ctr_gpu_time_ms();
  struct Done_ {
    CtrLevels* l;
    double t0;
    ~Done_() {
      const double ms = ctr_gpu_time_ms() - t0;
      l->m_sst.sched_ms += ms;
      l->m_sst.sched_max_ms = std::max(l->m_sst.sched_max_ms, ms);
    }
  } timing{this, t0};
  m_sst.frames++;
  auto& arena = CtrStreamArena::get();
  bool level_loading = false;
  {
    std::lock_guard<std::mutex> lk(m_lock);
    for (const auto& key : m_requested) {
      level_loading |= key.find('#') == std::string::npos;
    }
  }
  // (statistics) the linear heap's free memory at each frame's start
  const u32 free_now = ctr_gpu_linear_free();
  m_sst.linear_min = std::min<u64>(m_sst.linear_min, free_now);
  if (level_loading) {
    m_sst.linear_min_load = std::min<u64>(m_sst.linear_min_load, free_now);
  }
  dormant_update(frame);  // (AI-assisted, j2-mem)
  if (!m_cam_valid || !arena.ready()) {
    return;
  }
  // the arena's growth: given back where the linear heap has less than its floor (+ what the
  // repair of an incomplete level waits for) or a level loads (its memory first: the floor plus
  // kLoadRoom); under the floor + kPressure the units not needed now first, under the floor all
  // of them. The empty segments back to the heap (their meshes' memory came back once the GPU was
  // done with it).
  const u32 reserve = (u32)ctr_settings().stream_reserve_kb * 1024u + m_stream_repair_room;
  if (arena.has_growth()) {
    if (level_loading) {
      const u32 need = reserve + kLoadRoom;
      if (free_now < need) {
        m_sst.shrunk += stream_shrink(free_now + 512 * 1024 < need);
      }
    } else if (free_now < reserve + kPressure) {
      m_sst.shrunk += stream_shrink(free_now < reserve);
    }
    arena.release_empty();
  }
  m_want.clear();
  m_have.clear();
  m_idle.clear();
  auto in_view = [&](const float* c) {
    for (int i = 0; i < 4; i++) {
      const float a = m_planes[0][i] * c[0] + m_planes[1][i] * c[1] + m_planes[2][i] * c[2] -
                      m_planes[3][i];
      if (!(a > -c[3])) {
        return false;
      }
    }
    return true;
  };
  float p[kLeadSteps + 1][3];
  for (int i = 0; i < kLeadSteps; i++) {
    for (int c = 0; c < 3; c++) {
      p[i][c] = m_cam[c] + m_cam_vel[c] * kLeadFrames * (float)i / (kLeadSteps - 1);
    }
  }
  // (a camera standing still: one position)
  const float v2 = m_cam_vel[0] * m_cam_vel[0] + m_cam_vel[1] * m_cam_vel[1] +
                   m_cam_vel[2] * m_cam_vel[2];
  int np = v2 * kLeadFrames * kLeadFrames < 4096.f * 4096.f ? 1 : kLeadSteps;
  // the wanted margin: kWantMeters, + half a second of the camera's travel (a fast camera turning
  // sees what its lead missed)
  const float want_m = kWantMeters * 4096.f + std::sqrt(v2) * 15.f;
  // Jak's place too (drawn within the last frames): a cut to another camera (a cutscene's, the
  // race's, a zoom) lands near him, and his near chunks are there before it
  if (m_target_frame + 3 >= frame) {
    for (int c = 0; c < 3; c++) {
      p[np][c] = m_target[c];
    }
    np++;
  }
  constexpr float kM = 4096.f;
  u64 need_now = 0;
  for (auto& [name, lp] : m_levels) {
    CtrLevelData& lev = *lp;
    if (!lev.streaming) {
      continue;
    }
    // a level not drawn lately (far mode never asks: its k is 0 there) wants nothing
    // (15% more, or the recent largest: a zooming camera's focal length moves the factor)
    const float k = lev.stream_frame + 3 >= frame
                        ? std::max(lev.stream_k * 1.15f, lev.stream_k_hi)
                        : 0.f;
    for (u32 u = 0; u < lev.stream_units.size(); u++) {
      auto& su = lev.stream_units[u];
      if (su.state != CtrLevelData::kUnitAbsent && su.state != CtrLevelData::kUnitResident) {
        if (su.state == CtrLevelData::kUnitLoading && k > 0 && su.last_score < 0) {
          need_now += su.bytes;  // (being read: needed when it was asked for)
        }
        continue;
      }
      // the unit's bound first (its sphere's center: need_a + k x need_e), then its groups': how
      // far the camera (now or where it goes) is beyond the nearest group's near distance
      const bool resident = su.state == CtrLevelData::kUnitResident;
      if (k <= 0) {
        if (resident) {
          m_have.push_back({&lev, u, 1e30f});
        }
        continue;
      }
      const float* c = lev.chunks[su.chunk].bsphere;
      float dmin = 1e30f;
      for (int i = 0; i < np; i++) {
        const float dx = p[i][0] - c[0], dy = p[i][1] - c[1], dz = p[i][2] - c[2];
        dmin = std::min(dmin, dx * dx + dy * dy + dz * dz);
      }
      // (squared first: most units are far beyond their bound, no root for them)
      const float bound = su.need_a + k * su.need_e;
      const float far_lim = bound + (resident ? kKeepMeters * kM : want_m);
      if (dmin > far_lim * far_lim) {
        if (resident) {
          m_have.push_back({&lev, u, std::sqrt(dmin) - bound});
        } else {
          const float idle_lim = bound + kIdleMeters * kM;
          if (dmin < idle_lim * idle_lim) {
            m_idle.push_back({&lev, u, std::sqrt(dmin) - bound});
          }
        }
        continue;
      }
      float score = std::sqrt(dmin) - bound;
      if (!lev.unit_group_first.empty()) {
        float best = 1e30f;
        for (u32 j = lev.unit_group_first[u]; j < lev.unit_group_first[u + 1]; j++) {
          const u32 g = lev.unit_groups[j];
          const auto& lg = lev.lod_groups[g];
          const float sw = g < lev.lod_error.size() ? lev.lod_error[g] : lg.dist;
          const float lim = sw * k + lg.bsphere[3];
          float g2 = 1e30f;
          for (int i = 0; i < np; i++) {
            const float dx = p[i][0] - lg.bsphere[0], dy = p[i][1] - lg.bsphere[1],
                        dz = p[i][2] - lg.bsphere[2];
            g2 = std::min(g2, dx * dx + dy * dy + dz * dz);
          }
          // (beyond best already: no root)
          const float bl = best + lim;
          if (best < 1e29f && bl > 0 && g2 >= bl * bl) {
            continue;
          }
          best = std::min(best, std::sqrt(g2) - lim);
        }
        score = best;
      }
      if (score < 0) {
        need_now += su.bytes;  // (statistics: needed now or soon)
      }
      su.last_score = score;
      if (resident) {
        m_have.push_back({&lev, u, score});
      } else if (score < want_m) {
        // (needed now and on screen: first)
        const float prio = score < 0 && in_view(c) ? score - kInViewMeters * kM : score;
        m_want.push_back({&lev, u, prio});
        if (!su.want_frame) {
          su.want_frame = frame;
        }
      } else {
        m_idle.push_back({&lev, u, score});
      }
    }
  }
  m_sst.need_max = std::max<u64>(m_sst.need_max, need_now);
  m_sst.need_sum += need_now;
  if (m_want.empty() && !m_stream_inflight && !level_loading && !m_idle.empty() &&
      frame >= m_idle_frame + kIdleFrames) {
    stream_idle_fill(frame);
    return;
  }
  if (m_want.empty() || m_stream_inflight >= kJobsInFlight) {
    return;
  }
  std::sort(m_want.begin(), m_want.end(),
            [](const StreamCand& a, const StreamCand& b) { return a.score < b.score; });
  bool have_sorted = false;
  u64 evicted = 0;  // bytes freed now (back after the GPU is done with them: no growth for those)
  CtrLevelData& lev = *m_want[0].lev;
  Job job;
  job.name = lev.name;
  job.stream = true;
  job.generation = lev.generation;
  job.path = lev.path;
  job.vertex_data_offset = lev.vertex_data_offset;
  job.index_data_offset = lev.index_data_offset;
  job.tod_ntab = lev.tod ? (u32)lev.tod->table.size() : 0;
  job.dir = lev.file_dir;
  size_t hi = 0;
  u32 packed = 0;
  bool no_room = false;
  for (const auto& w : m_want) {
    if (w.lev != &lev) {
      continue;
    }
    auto& su = lev.stream_units[w.unit];
    void* mem = arena.alloc(su.bytes);
    if (!mem && !have_sorted) {
      std::sort(m_have.begin(), m_have.end(),
                [](const StreamCand& a, const StreamCand& b) { return a.score > b.score; });
      have_sorted = true;
    }
    // (on the 3DS the freed memory comes back at the next frame: not more than this unit's worth
    // freed for it, else one cut freed the whole arena)
    // The victim: of the units beyond the keep margin (the furthest first), the first whose
    // freeing makes a free run this big (with the free and pending pieces next to it: the arena's
    // pieces are of all sizes, its free room in many small pieces after a while); else the
    // furthest. Not more than this unit's worth (pending: their memory comes back next frame).
    // (`beyond`: the victims' least score; m_have from hi on is sorted, the furthest first)
    auto free_for = [&](float beyond) {
      u64 freed_for = 0;
      while (!mem && hi < m_have.size() && m_have[hi].score > beyond && freed_for < su.bytes) {
        size_t pick_i = hi;
        for (size_t j = hi; j < m_have.size() && j < hi + 64 && m_have[j].score > beyond; j++) {
          const auto& cu = m_have[j].lev->stream_units[m_have[j].unit];
          if (cu.state == CtrLevelData::kUnitResident && arena.run_if_freed(cu.mem) >= su.bytes) {
            pick_i = j;
            break;
          }
        }
        std::swap(m_have[hi], m_have[pick_i]);
        const auto& victim = m_have[hi].lev->stream_units[m_have[hi].unit];
        if (victim.state != CtrLevelData::kUnitResident) {
          hi++;
          continue;
        }
        const bool makes_room = arena.run_if_freed(victim.mem) >= su.bytes;
        freed_for += makes_room ? su.bytes : victim.bytes;
        evicted += victim.bytes;
        stream_evict(*m_have[hi].lev, m_have[hi].unit);
        hi++;
        mem = arena.alloc(su.bytes);
      }
    };
    free_for(kKeepMeters * kM);
    // Every unit there is needed (none beyond the keep margin to free): more room, while the
    // linear heap keeps its floor + kGrowHeadroom free after it. One segment for what the
    // camera needs now and 2 s ahead (need_now) + kGrowMargin beyond the arena's capacity (at
    // least this unit's and kSegment; at most kMaxSegment, or the largest block the heap has):
    // few, large segments. While a level loads only when the heap keeps kLoadRoom more (the
    // load's memory first; a load that runs short makes room, freeing the growth).
    if (!mem && evicted < su.bytes) {
      const u32 grow_reserve = reserve + (level_loading ? kLoadRoom : kGrowHeadroom);
      const u64 cap = arena.capacity();
      const u64 demand = need_now + std::max<u64>(kGrowMargin, need_now / 8);
      const u32 want = demand > cap ? (u32)std::min<u64>(demand - cap, 1u << 30) : 0;
      if (arena.grow(su.bytes, want, grow_reserve)) {
        mem = arena.alloc(su.bytes);
      }
    }
    // The arena at its most (the heap at its floor) and this unit needed now: the units kept
    // within the keep margin that aren't wanted (beyond the wanted margin: not read now either)
    // make room for it
    if (!mem && evicted < su.bytes && w.score < 0) {
      free_for(want_m);
    }
    if (!mem) {
      m_sst.no_room++;
      no_room = true;
      su.room_frame = frame;
      break;
    }
    const u32 ci = su.chunk;
    const auto& ch = lev.chunks[ci];
    Job::StreamPart part;
    part.unit = w.unit;
    part.chunk = ci;
    part.first_vertex = ch.first_vertex;
    part.vertex_count = ch.vertex_count;
    part.index_first = lev.mesh_index_first[ci];
    part.index_end = lev.mesh_index_end[ci];
    part.mem = mem;
    part.layout = lev.tod && !su.tod_layout;
    if (part.layout) {
      part.draws.assign(lev.draws.begin() + ch.first_draw,
                        lev.draws.begin() + ch.first_draw + ch.draw_count);
      const u32 r0 = lev.chunk_first_run[ci];
      part.runs.assign(lev.vis_runs.begin() + r0, lev.vis_runs.begin() + r0 + ch.vis_run_count);
    }
    su.state = CtrLevelData::kUnitLoading;
    su.mem = mem;
    job.stream_parts.push_back(std::move(part));
    packed += su.packed;
    job.urgent = job.urgent || w.score < 0;
    // (a backlog of needed units, after a cut: bigger jobs, fewer requests per byte)
    const u32 limit = need_now > 1024 * 1024 ? 2 * kBatchBytes : kBatchBytes;
    if (packed >= limit || job.stream_parts.size() >= 2 * kBatchUnits ||
        (job.stream_parts.size() >= kBatchUnits && limit == kBatchBytes)) {
      break;
    }
  }
  if (no_room) {
    // (statistics) the other units needed now that are not there wait for room too
    for (const auto& w : m_want) {
      auto& su = w.lev->stream_units[w.unit];
      if (w.score < 0 && su.state == CtrLevelData::kUnitAbsent) {
        su.room_frame = frame;
      }
    }
  }
  if (job.stream_parts.empty()) {
    return;
  }
  // the units next to these in the file (their neighbours in space) that will be needed soon,
  // while there's room without freeing anything: read with them for little more than their bytes
  // (one request for all), instead of a request of their own later
  for (size_t k = 0; k < job.stream_parts.size() && packed < kBatchBytes &&
                     job.stream_parts.size() < kBatchUnits;
       k++) {
    for (int dir = -1; dir <= 1; dir += 2) {
      const u32 u = (u32)((int)job.stream_parts[k].unit + dir);
      if (u >= lev.stream_units.size() || packed >= kBatchBytes ||
          job.stream_parts.size() >= kBatchUnits) {
        continue;
      }
      auto& su = lev.stream_units[u];
      if (su.state != CtrLevelData::kUnitAbsent) {
        continue;
      }
      float score = 1e30f;
      for (const auto& h : m_want) {
        if (h.lev == &lev && h.unit == u) {
          score = h.score;
          break;
        }
      }
      if (score == 1e30f) {
        // not wanted yet: its score now (the unit's bound)
        const float* c = lev.chunks[su.chunk].bsphere;
        const float dx = m_cam[0] - c[0], dy = m_cam[1] - c[1], dz = m_cam[2] - c[2];
        const float k2 = lev.stream_frame + 3 >= frame ? lev.stream_k : 0.f;
        score = k2 > 0 ? std::sqrt(dx * dx + dy * dy + dz * dz) - (su.need_a + k2 * su.need_e)
                       : 1e30f;
      }
      if (score > kKeepMeters * kM) {
        continue;
      }
      void* mem = arena.alloc(su.bytes);
      if (!mem) {
        continue;
      }
      const u32 ci = su.chunk;
      const auto& ch = lev.chunks[ci];
      Job::StreamPart part;
      part.unit = u;
      part.chunk = ci;
      part.first_vertex = ch.first_vertex;
      part.vertex_count = ch.vertex_count;
      part.index_first = lev.mesh_index_first[ci];
      part.index_end = lev.mesh_index_end[ci];
      part.mem = mem;
      part.layout = lev.tod && !su.tod_layout;
      if (part.layout) {
        part.draws.assign(lev.draws.begin() + ch.first_draw,
                          lev.draws.begin() + ch.first_draw + ch.draw_count);
        const u32 r0 = lev.chunk_first_run[ci];
        part.runs.assign(lev.vis_runs.begin() + r0, lev.vis_runs.begin() + r0 + ch.vis_run_count);
      }
      su.state = CtrLevelData::kUnitLoading;
      su.mem = mem;
      job.stream_parts.push_back(std::move(part));
      packed += su.packed;
      m_sst.neighbours++;
    }
  }
  // in file order (the reader's windows go forward through the vertex and index data)
  std::sort(job.stream_parts.begin(), job.stream_parts.end(),
            [](const Job::StreamPart& a, const Job::StreamPart& b) {
              return a.first_vertex < b.first_vertex;
            });
  job.window = c3l_read_window();
  m_stream_inflight++;
  job.serial = ++m_stream_serial;
  request(job);
}

// Loader thread: a stream job's near chunks, each read (indices, then vertices) straight into its
// arena piece; their time of day groups made from the indices where asked. Stops at the first that
// fails (the rest's pieces go back to the arena; the render thread asks again).
void CtrLevels::run_stream_job(const Job& job, Done* d) {
  Timer timer;
  d->result = LoadResult::FAILED;
  size_t i = 0;
  try {
    if (!job.dir) {
      throw std::runtime_error("no file directory");
    }
    C3lReader f(job.path, &m_read_bufs, job.window, *job.dir);
    size_t run_end = 0;
    for (; i < job.stream_parts.size(); i++) {
      const auto& p = job.stream_parts[i];
      if (!f.ok() || cancelled()) {
        break;
      }
      if (i >= run_end) {
        // the parts lying one after the other in the file from here: their index and vertex
        // bytes in one request each (the converter put the near chunks together, in spatial order)
        // (and across small gaps: reading a few KB more costs less than another request)
        constexpr u32 kGapVerts = 48 * 1024 / 16, kGapIndices = 16 * 1024 / 2;
        constexpr u32 kSpanBytes = 384 * 1024;
        run_end = i + 1;
        while (run_end < job.stream_parts.size()) {
          const auto& a = job.stream_parts[run_end - 1];
          const auto& b = job.stream_parts[run_end];
          const u32 a_vend = a.first_vertex + a.vertex_count;
          if (b.first_vertex < a_vend || b.index_first < a.index_end ||
              b.first_vertex - a_vend > kGapVerts || b.index_first - a.index_end > kGapIndices ||
              (b.first_vertex + b.vertex_count - p.first_vertex) * 16u +
                      (b.index_end - p.index_first) * 2u >
                  kSpanBytes) {
            break;
          }
          run_end++;
        }
        const auto& last = job.stream_parts[run_end - 1];
        for (const bool ok :
             {f.preload(job.index_data_offset + p.index_first * (u32)sizeof(u16),
                        (last.index_end - p.index_first) * (u32)sizeof(u16)),
              f.preload(job.vertex_data_offset + p.first_vertex * (u32)sizeof(c3l::Vertex),
                        (last.first_vertex + last.vertex_count - p.first_vertex) *
                            (u32)sizeof(c3l::Vertex))}) {
          (ok ? d->preloads : d->preload_misses)++;
        }
      }
      void* vdst;
      u16* idst;
      const u32 icount = p.index_end - p.index_first;
      int mesh = ctr_gpu_mesh_begin_ext(p.mem, (int)p.vertex_count, (int)icount, &vdst, &idst);
      if (mesh < 0) {
        break;  // (its piece back below)
      }
      const bool ok =
          f.read(job.index_data_offset + p.index_first * (u32)sizeof(u16), idst,
                 icount * sizeof(u16)) &&
          f.read(job.vertex_data_offset + p.first_vertex * (u32)sizeof(c3l::Vertex), vdst,
                 p.vertex_count * sizeof(c3l::Vertex));
      Done::StreamOut o;
      o.unit = p.unit;
      if (ok && p.layout) {
        stream_tod_layout(this, p.vertex_count, icount, p.draws, p.runs, idst,
                          (const c3l::Vertex*)vdst, job.tod_ntab, &o.groups, &o.run_group);
        o.layout = true;
      }
      mesh = ctr_gpu_mesh_end(mesh, ok);  // (!ok: its piece back through ctr_gpu_set_ext_free)
      if (mesh < 0) {
        i++;
        break;
      }
      o.mesh = mesh;
      d->stream_out.push_back(std::move(o));
    }
    d->read_ms = f.read_ms;
    d->decode_ms = f.decode_ms;
    d->reads = f.file_reads;
    d->read_bytes = f.file_bytes_read;
  } catch (std::exception& e) {
    lg::error("[ctr] {}: reading near chunks failed: {}", job.name, e.what());
  }
  for (; i < job.stream_parts.size(); i++) {
    CtrStreamArena::get().free(job.stream_parts[i].mem);
  }
  d->ms = timer.getMs();
  d->result = d->stream_out.size() == job.stream_parts.size() ? LoadResult::LOADED
                                                               : LoadResult::FAILED;
}

// Render thread: a stream job's meshes into their level (the same load of it), or deleted.
void CtrLevels::take_stream(Done& d, u64 frame) {
  (void)frame;
  m_stream_inflight -= std::min<u32>(m_stream_inflight, 1);
  auto it = m_levels.find(d.job.name);
  CtrLevelData* lev = it != m_levels.end() && it->second->generation == d.job.generation
                          ? it->second.get()
                          : nullptr;
  m_sst.jobs++;
  m_sst.preloads += d.preloads;
  m_sst.preload_misses += d.preload_misses;
  m_sst.read_bytes += d.read_bytes;
  m_sst.reads += d.reads;
  m_sst.read_ms += d.read_ms;
  m_sst.decode_ms += d.decode_ms;
  m_sst.job_ms += d.ms;
  m_sst.job_max_ms = std::max(m_sst.job_max_ms, d.ms);
  if (!lev) {
    for (auto& o : d.stream_out) {
      ctr_gpu_mesh_delete(o.mesh);
    }
    return;
  }
  // the parts the job didn't make: not there (asked for again); the others are taken by
  // stream_take_pending (colored first, within a budget per frame), loading until then
  std::vector<u8> made(lev->stream_units.size(), 0);
  for (const auto& o : d.stream_out) {
    if (o.unit < made.size()) {
      made[o.unit] = 1;
    }
  }
  for (const auto& p : d.job.stream_parts) {
    if (p.unit < lev->stream_units.size() && !made[p.unit] &&
        lev->stream_units[p.unit].state == CtrLevelData::kUnitLoading) {
      lev->stream_units[p.unit].state = CtrLevelData::kUnitAbsent;
    }
  }
  for (auto& o : d.stream_out) {
    m_take.push_back({d.job.name, d.job.generation, std::move(o)});
  }
}

// Render thread, each frame (outside of a GPU frame): the near chunks read, into their levels.
// Each one's time of day colors are written first with the current blend (exact: a near chunk is
// never drawn with the file's baked colors, and its rewrite doesn't wait for tod_max_ms); at most
// kTakeVertices vertices per frame (a chunk at least), the rest the next frame (drawn far until
// then).
constexpr u32 kTakeVertices = 16384;
void CtrLevels::stream_take_pending(u64 frame) {
  u32 verts = 0;
  size_t i = 0;
  const double now = ctr_gpu_time_ms();
  for (; i < m_take.size(); i++) {
    auto& tk = m_take[i];
    auto it = m_levels.find(tk.name);
    CtrLevelData* lev = it != m_levels.end() && it->second->generation == tk.generation
                            ? it->second.get()
                            : nullptr;
    auto& o = tk.out;
    if (!lev || o.unit >= lev->stream_units.size()) {
      ctr_gpu_mesh_delete(o.mesh);
      continue;
    }
    auto& su = lev->stream_units[o.unit];
    const u32 ci = su.chunk;
    if (su.state != CtrLevelData::kUnitLoading || lev->meshes[ci] >= 0) {
      ctr_gpu_mesh_delete(o.mesh);  // (freed meanwhile: evicted while loading never happens)
      continue;
    }
    const u32 nv = lev->chunks[ci].vertex_count;
    if (verts && verts + nv > kTakeVertices) {
      break;
    }
    verts += nv;
    lev->meshes[ci] = o.mesh;
    su.state = CtrLevelData::kUnitResident;
    su.want_frame = 0;
    su.loads++;
    lev->stream_resident += su.bytes;
    m_sst.units++;
    m_sst.bytes += su.bytes;
    m_sst.packed += su.packed;
    if (lev->tod) {
      auto& t = *lev->tod;
      t.chunk_mesh[ci] = -1;  // (the file's colors, unless colored below)
      if (o.layout && !su.tod_layout && ci < t.chunk_group_first.size()) {
        // its groups, into the room kept at load
        const u32 g0 = t.chunk_group_first[ci];
        const bool fits = o.groups.size() <= su.tod_slots && g0 + o.groups.size() <= t.groups.size();
        const u32 n = fits ? (u32)o.groups.size() : 1;
        for (u32 g = 0; g < n; g++) {
          t.groups[g0 + g] = fits ? o.groups[g]
                                  : CtrLevelData::Tod::VisGroup{
                                        0, (u16)(std::max<u32>(nv, 1) - 1), 0};
        }
        t.chunk_group_count[ci] = (u16)n;
        const u32 r0 = lev->chunk_first_run[ci];
        for (u32 k = 0; k < o.run_group.size() && r0 + k < t.run_group.size(); k++) {
          t.run_group[r0 + k] = fits ? o.run_group[k] : 0;
        }
        t.chunk_min_ver[ci] = t.chunk_min_dver[ci] = t.chunk_light_ver[ci] = 0;
        su.tod_layout = 1;
      }
      int n = 0;
      auto* v = (c3l::Vertex*)ctr_gpu_mesh_vertices_inplace(o.mesh, &n);
      if (v && n > 0 && t.blended_valid && ci < t.chunk_mask.size()) {
        if (!t.chunk_scanned[ci]) {
          // its palettes and light vertices (once per level load)
          CtrLevelData::Tod::LightList lights;
          const u8 mask = tod_scan(t, v, n, &lights);
          t.chunk_scanned[ci] = mask ? 2 : 1;
          t.chunk_mask[ci] = mask;
          if (!lights.verts.empty()) {
            t.chunk_lights[ci] = std::move(lights);
          }
          if (mask) {
            tod_mark_plain(t, ci, v, n);
          }
        }
        if (t.chunk_scanned[ci] == 2) {
          // every group with the current blend (the table's entries follow lazily, exact)
          const u32 g0 = t.chunk_group_first[ci], ng = t.chunk_group_count[ci];
          for (u32 g = g0; g < g0 + ng; g++) {
            auto& gr = t.groups[g];
            const int first = gr.lo, end = std::min<int>(gr.hi + 1, n);
            if (end > first) {
              tod_write_range(t, v, first, end, gr.plain);
            }
            gr.ver = t.ver;
            gr.dver = t.dver;
          }
          t.chunk_min_ver[ci] = t.ver;
          t.chunk_min_dver[ci] = t.dver;
          t.chunk_light_ver[ci] = t.ver;
          ctr_gpu_flush_range(v, (u32)n * (u32)sizeof(c3l::Vertex));
          m_sst.colored++;
        }
        t.chunk_mesh[ci] = o.mesh;  // (colored: tod_recolor keeps it)
      }
    }
    if (su.late_ms > 0) {
      // (a time that ended before: up to its last frame)
      const bool open = frame <= su.late_last + 2;
      stream_late_end(su, open ? now : su.late_last_ms, open ? frame : su.late_last + 1);
    }
  }
  m_take.erase(m_take.begin(), m_take.begin() + (long)i);
}

// (AI-assisted, j2-mem) what the levels hold, the heaps' free memory (every stats period; host env
// CTR_MEM_LOG_EVERY=N: every N frames too)
void CtrLevels::log_memory() {
  LevelMem all;
  std::string per;
  auto add = [&](const std::string& name, const CtrLevelData& lev) {
    const LevelMem m = level_mem(lev);
    all.heap += m.heap;
    per += fmt::format("{}{}{} {}/{} KB", per.empty() ? "" : ", ", name,
                       lev.dormant ? " (dormant)" : "", m.linear() / 1024, m.heap / 1024);
    all.unhideable += m.unhideable;
    all.unhideable_chunks += m.unhideable_chunks;
    for (int t = 0; t < 4; t++) {
      all.tfrag[t] += m.tfrag[t];
    }
    all.tie_far += m.tie_far;
    all.tie_dist += m.tie_dist;
    all.shrub += m.shrub;
    all.vista += m.vista;
    all.units += m.units;
    all.merc += m.merc;
    all.tex += m.tex;
  };
  if (m_common) {
    add("GAME", *m_common);
  }
  for (const auto& [name, lev] : m_levels) {
    add(name, *lev);
  }
  const auto as = CtrStreamArena::get().stats();
  std::string free;
#ifdef __3DS__
  CtrHeapStats st;
  ctr_heap_stats(&st);
  free = fmt::format("; free: linear {} KB, heap {} KB (largest {} KB)", linearSpaceFree() / 1024,
                     st.free / 1024, st.largest / 1024);
#else
  free = fmt::format("; free: linear model {} KB", ctr_gpu_linear_free() / 1024);
#endif
  // (AI-assisted, j2-mem2) + the GS VRAM copy (CtrVram: sparse for Jak II)
  lg::info("[ctr] memory (frame {}): levels {}; stream arena {} KB; VRAM copy {} KB; {} (linear / "
           "heap KB){}",
           m_render_frame.load(), level_mem_text(all), as.capacity / 1024, ctr_vram_copy_kb(), per,
           free);
}

void CtrLevels::stream_log() {
  log_memory();
  const auto as = CtrStreamArena::get().stats();
  const double now = ctr_gpu_time_ms();
  const double secs = m_sst.since_ms > 0 ? (now - m_sst.since_ms) / 1000.0 : 0.0;
  m_sst.since_ms = now;
  if (!as.capacity && !m_sst.jobs) {
    return;
  }
  // the levels' resident linear memory: meshes (near chunks apart), textures
  std::string levels;
  for (const auto& [name, lev] : m_levels) {
    u64 meshes = 0;
    for (size_t ci = 0; ci < lev->chunks.size() && ci < lev->meshes.size() &&
                        ci < lev->mesh_index_first.size();
         ci++) {
      if (lev->meshes[ci] >= 0 && !(lev->is_unit(ci) && lev->streaming)) {
        meshes += CtrStreamArena::mesh_bytes(lev->chunks[ci].vertex_count,
                                             lev->mesh_index_end[ci] - lev->mesh_index_first[ci]);
      }
    }
    u32 resident = 0, units = 0;
    for (const auto& u : lev->stream_units) {
      resident += u.state == CtrLevelData::kUnitResident;
      units += u.state != CtrLevelData::kUnitNever;
    }
    levels += fmt::format("{}{} meshes {} KB + textures {} KB, near chunks {} of {} ({} KB)",
                          levels.empty() ? "" : "; ", name, meshes / 1024,
                          lev->tex_pool_bytes / 1024, resident, units,
                          lev->streaming ? lev->stream_resident / 1024 : 0);
  }
  const double n = (double)std::max<u64>(m_sst.frames, 1);
  if (m_sst.late_events || m_sst.late_periods || m_sst.held_frames || m_sst.idle_jobs) {
    lg::info("[ctr] streaming: late near chunks: {} being read already, {} not asked for yet ({} "
             "never wanted before; the others {:.1f} frames after they were wanted); late times "
             "after a camera cut {} ({:.0f} ms on average, at most {:.0f}), else {} ({:.0f} ms, at "
             "most {:.0f}); for lack of room in the arena {} ({:.0f} ms on average, at most "
             "{:.0f}); {} frames with groups kept far in view after their data came ({} "
             "group frames); idle fill {} jobs, {} near chunks",
             m_sst.late_loading, m_sst.late_absent, m_sst.late_unwanted,
             (double)m_sst.late_wanted_frames /
                 std::max<u64>(1, m_sst.late_events - m_sst.late_unwanted),
             m_sst.late_cut, m_sst.late_cut ? m_sst.late_cut_ms / m_sst.late_cut : 0.0,
             m_sst.late_cut_max, m_sst.late_other,
             m_sst.late_other ? m_sst.late_other_ms / m_sst.late_other : 0.0,
             m_sst.late_other_max, m_sst.late_room,
             m_sst.late_room ? m_sst.late_room_ms / m_sst.late_room : 0.0, m_sst.late_room_max,
             m_sst.held_frames, m_sst.held_group_frames, m_sst.idle_jobs, m_sst.idle_units);
  }
  lg::info("[ctr] streaming: arena {} of {} KB used (peak {}, largest free {} KB, {} pieces; "
           "growth {} KB in {} segments, at most {} KB in all; {} taken, {} given back, {} units "
           "freed for it); {} "
           "jobs, {} near chunks read ({} KB, {} KB of their blocks; file {} KB in {} reads, {} of "
           "{} ranges in one request, {:.0f} ms reading, {:.0f} ms decoding, {:.0f} "
           "ms in jobs, longest {:.0f} ms; {:.0f} KB/s), {} freed, {} times no room; late: {} "
           "times, {} frames with groups drawn far for it ({} group frames), {:.0f} ms on "
           "average, at most {:.0f} ms ({} frames); needed now {:.0f} KB on average, at most "
           "{} KB; {} read with their file neighbours; linear free least {} KB ({} while a "
           "level loaded); scheduling {:.3f} ms/frame (most {:.2f}); {}",
           as.used / 1024, as.capacity / 1024, as.peak / 1024, as.largest_free / 1024, as.pieces,
           as.growth / 1024, as.segments ? as.segments - 1 : 0, as.peak_capacity / 1024, as.grown,
           as.released, m_sst.shrunk,
           m_sst.jobs, m_sst.units, m_sst.bytes / 1024, m_sst.packed / 1024,
           m_sst.read_bytes / 1024, m_sst.reads, m_sst.preloads,
           m_sst.preloads + m_sst.preload_misses,
           m_sst.read_ms, m_sst.decode_ms, m_sst.job_ms, m_sst.job_max_ms,
           secs > 0 ? m_sst.read_bytes / 1024.0 / secs : 0.0, m_sst.evictions, m_sst.no_room,
           m_sst.late_events, m_sst.late_frames, m_sst.late_group_frames,
           m_sst.late_periods ? m_sst.late_ms_total / m_sst.late_periods : 0.0,
           m_sst.late_ms_max, m_sst.late_frames_max, m_sst.need_sum / n / 1024.0,
           m_sst.need_max / 1024, m_sst.neighbours,
           m_sst.linear_min == ~0ull ? 0 : m_sst.linear_min / 1024,
           m_sst.linear_min_load == ~0ull ? std::string("-")
                                          : std::to_string(m_sst.linear_min_load / 1024),
           m_sst.sched_ms / n, m_sst.sched_max_ms,
           levels.empty() ? std::string("no levels") : levels);
  const double since = m_sst.since_ms;
  CtrStreamArena::get().new_period();
  m_sst = StreamStats();
  m_sst.since_ms = since;
  if (stream_check_enabled()) {
    lg::info("[ctr] stream check: {} level draws, {} group decisions; {} groups in {} draws drawn "
             "far because their near chunks weren't there yet ({} of them not being read after 2 "
             "frames); {} near versions drawn without their data; near chunks drawn {}, with a "
             "group in the file's baked colors {}; file vs load: run spheres {} "
             "of {} differ, alpha 0x80 flags {} of {} differ",
             m_scheck.frames, m_scheck.groups, m_scheck.late_groups, m_scheck.late_frames,
             m_scheck.late_unrequested, m_scheck.holes, m_scheck.units_drawn,
             m_scheck.units_baked, g_stream_check_sphere_diff,
             g_stream_check_spheres, g_stream_check_alpha_diff, g_stream_check_alpha);
    lg::info("[ctr] dormant check: {} chunk frames of freed ties within their distance in view "
             "(not drawn), {} shown in view after a hold",
             m_scheck.dorm_missing, m_scheck.dorm_switch_in_view);
  }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

namespace {

struct TfragPcPortData {
  CtrBackgroundCamera camera;
  char level_name[32];
};
static_assert(sizeof(TfragPcPortData) == 16 * 25);

bool looks_like_tfragment_dma(const DmaFollower& follow) {
  return follow.current_tag_vifcode0().kind == VifCode::Kind::STCYCL;
}

bool looks_like_tfrag_init(const DmaFollower& follow) {
  return follow.current_tag_vifcode0().kind == VifCode::Kind::NOP &&
         follow.current_tag_vifcode1().kind == VifCode::Kind::DIRECT &&
         follow.current_tag_vifcode1().immediate == 2;
}

// same as make_new_cam_mat (background_common.cpp)
std::array<math::Vector4f, 4> make_cam_mat(const math::Vector4f cam_T_w[4],
                                           const math::Vector4f persp[4],
                                           float fog_constant,
                                           float hvdf_z) {
  const float pc_pxx = fog_constant * persp[0][0] / 256.f;
  const float pc_pyy = -fog_constant * persp[1][1] / 128.f;
  const float depth_scale = fog_constant * persp[2][2] / 8388608;
  const float game_pzw = persp[2][3];
  const float game_depth_offset = persp[3][2];
  math::Vector3f persp_scale(pc_pxx, pc_pyy, depth_scale);
  persp_scale.z() += (hvdf_z / 8388608.f - 1.f) * game_pzw;
  std::array<math::Vector4f, 4> result;
  for (auto& x : result) {
    x.set_zero();
  }
  for (int row = 0; row < 3; row++) {
    for (int col = 0; col < 3; col++) {
      result[row][col] = cam_T_w[row][col] * persp_scale[col];
    }
  }
  for (int row = 0; row < 3; row++) {
    result[row][3] = cam_T_w[row][2] * game_pzw;
  }
  result[3][2] = fog_constant * game_depth_offset / 8388608;
  return result;
}

// (AI-assisted) lowres_vista: a sphere entirely outside a level's detailed area grown by margin
bool outside_detail(const CtrLevelData& l, const float* s, float margin) {
  if (!l.has_detail) {
    return true;
  }
  float d2 = 0;
  for (int c = 0; c < 3; c++) {
    const float lo = l.detail_min[c] - margin, hi = l.detail_max[c] + margin;
    const float o = s[c] < lo ? lo - s[c] : (s[c] > hi ? s[c] - hi : 0.f);
    d2 += o * o;
  }
  return d2 > s[3] * s[3];
}

// (AI-assisted) auto_detail: how far between the near (0) and the far (1) shrub / tie distances
// (render thread: set by CtrRenderer from the GPU time, read here)
float g_detail_t = 0.25f;
// (AI-assisted, title-popin) auto_detail: the GPU is about to miss frames (ctr_level_set_detail_urgent)
bool g_detail_urgent = false;
// (AI-assisted, j2intro) the screen is black this frame (ctr_level_set_screen_black; Jak II only)
bool g_screen_black = false;
// (AI-assisted, title-popin) the auto_detail position as stored per chunk / tie LOD group
// (CtrLevelData::chunk_detail_q): 0..kDetailQ, 0xff none yet
constexpr float kDetailQ = 250.f;
u8 detail_q(float t) {
  return (u8)std::lround(std::clamp(t, 0.f, 1.f) * kDetailQ);
}
// (AI-assisted, title-popin) what is in view and moving away keeps its nearer version / stays drawn
// up to this many times its distance (with the 10% hysteresis out of view): it goes when it is out
// of view or small on screen, instead of where the camera happens to cross the distance. On the
// title camera's flight (host, Oct 17) most switches of tie versions, tfrag tiers and distance
// limits in view were of things moving away.
constexpr float kHoldInView = 1.5f;
// (AI-assisted) occ_cull: a level whose occlusion buffer hid fewer triangles than this per level
// draw lately (average) runs it every 8th frame only; runs of fewer triangles aren't tested
constexpr float kOccMinTris = 1000.f;
// (AI-assisted) CTR_FINE_VIS_CHECK: the chunk of a piece occ_cull hid has this bit
constexpr u32 kOccMark = 0x80000000u;
constexpr u32 kOccMinRunTris = 16;
// (AI-assisted, title-popin) a sphere against the view's four planes (the chunk test of emit_chunk)
bool sphere_in_view(const CtrBackgroundCamera& cam, const float* s) {
  for (int i = 0; i < 4; i++) {
    const float a = cam.planes[0][i] * s[0] + cam.planes[1][i] * s[1] + cam.planes[2][i] * s[2] -
                    cam.planes[3][i];
    if (!(a > -s[3])) {
      return false;
    }
  }
  return true;
}

// (AI-assisted) a chunk's draw distance limit (game units, 0: none): the converter's (small and
// medium objects) times detail_scale. With chunk kinds (newer c3l files): shrubs up to
// shrub_distance (GPU-bound levels had 200-300k shrub triangles within the converter's 100 m),
// ties times tie_distance_scale (most ties stopped at 100 m: pop-in); with auto_detail both move
// toward their _max values as far as the GPU time allows. (micro-level: worked out in draw_level's
// chunk loop, its factors once per level draw)

// (AI-assisted, micro-level) a chunk's runs, one by one (CtrTfragRenderer::draw_level, emit_chunk):
// the visibility bit (VIS) or the vista source, the other version of a tie LOD group (LOD), the
// view test against the crossed planes (STR), the fine_vis bit; writes m_run_shown /
// m_run_nomerge. A copy per combination of VIS, STR and LOD (no tests of them per run), the
// arrays in locals (the u8 stores may alias anything: through members every vector's data
// pointer was loaded again after each store).
struct RunLoopIn {
  const c3l::VisRun* runs;
  const CtrLevelData::RunSphere* spheres;
  const c3l::RunLod* run_lod;
  const u8* lod_state;
  const u8* vis;
  const u8* vista_src;
  size_t vista_src_n;
  const u32* fbits;
  u32 fine_u0;
  bool vista_tier, fine_runs;
  std::vector<std::pair<u32, u32>>* fine_hidden;  // CTR_FINE_VIS_CHECK (else nullptr)
  u32 ci;
  const float* po;
  const float (*pk)[3];
  int np;
  float scale;
  u8* shown_out;
  u8* nomerge_out;
};
struct RunLoopOut {
  u32 shown = 0, view_hidden = 0, view_tris = 0, fine_hid = 0, lod_hid = 0;
  u64 fine_tris = 0, lod_tris[2] = {0, 0};
};
// (AI-assisted, micro-level) NP: the number of crossed planes (0: the chunk crosses none, no view
// test), their coefficients in locals (registers: the stores don't make them be loaded again)
template <bool VIS, int NP, bool LOD>
void run_loop(const RunLoopIn& in, u32 r0, u32 nr, RunLoopOut* out) {
  constexpr bool STR = NP > 0;
  constexpr int kP = NP > 0 ? NP : 1;
  float po[kP], pk0[kP], pk1[kP], pk2[kP];
  for (int i = 0; i < NP; i++) {
    po[i] = in.po[i];
    pk0[i] = in.pk[i][0];
    pk1[i] = in.pk[i][1];
    pk2[i] = in.pk[i][2];
  }
  const float scale = in.scale;
  u32 shown = 0, view_hidden = 0, view_tris = 0, fine_hid = 0, lod_hid = 0;
  u64 fine_tris = 0, lod_tris1 = 0, lod_tris2 = 0;
  const c3l::VisRun* const runs = in.runs;
  const CtrLevelData::RunSphere* const spheres = in.spheres;
  const c3l::RunLod* const run_lod = in.run_lod;
  const u8* const lod_state = in.lod_state;
  const u8* const vis = in.vis;
  u8* const shown_out = in.shown_out;
  u8* const nomerge_out = in.nomerge_out;
  const bool vista_tier = in.vista_tier, fine_runs = in.fine_runs;
  for (u32 r = r0; r < r0 + nr; r++) {
    const c3l::VisRun& run = runs[r];
    bool s = true;
    if (VIS) {
      const u16 id = run.vis;
      s = id == 0xffff || (vis[id >> 3] & (0x80 >> (id & 7)));
    } else if (vista_tier) {
      // (AI-assisted) a vista run stands in for its source chunk where that one isn't drawn
      const u16 src = run.vis;
      s = src < in.vista_src_n && in.vista_src[src];
    }
    // (AI-assisted) c3l v17: the other version of a LOD group: hidden, and never drawn along
    // with its neighbours (draw: both versions would be on top of each other)
    u8 nomerge = 0, lod_version = 0;
    if (LOD) {
      const auto& rl = run_lod[r];
      if (rl.lod != c3l::kRunLodAlways &&
          (rl.lod == c3l::kRunLodNear) != (bool)(lod_state[rl.group] & 1)) {
        nomerge = 1;
        if (s) {
          s = false;
          lod_hid++;
        }
      } else {
        lod_version = rl.lod;
      }
    }
    nomerge_out[r] = nomerge;
    if (STR && s) {
      const auto& rs = spheres[r];
      if (rs.r != 0xffff) {
        const float x = rs.c[0], y = rs.c[1], z = rs.c[2], rad = -(float)rs.r * scale;
        for (int i = 0; i < NP; i++) {
          if (!(po[i] + pk0[i] * x + pk1[i] * y + pk2[i] * z > rad)) {
            s = false;
            break;
          }
        }
        if (!s) {
          view_hidden++;
          view_tris += run.index_count / 3;
        }
      }
    }
    if (fine_runs && s) {
      // (AI-assisted) fine_vis: the unit's bit in the camera cell's set
      const u32 u = in.fine_u0 + (r - r0);
      if (!((in.fbits[u >> 5] >> (u & 31)) & 1)) {
        s = false;
        fine_hid++;
        fine_tris += run.index_count / 3;
        if (in.fine_hidden) {
          in.fine_hidden->push_back({in.ci, r});
        }
      }
    }
    shown_out[r] = s;
    shown += s;
    if (LOD && s && lod_version) {
      (lod_version == 1 ? lod_tris1 : lod_tris2) += run.index_count / 3;  // (AI-assisted)
    }
  }
  out->shown = shown;
  out->view_hidden = view_hidden;
  out->view_tris = view_tris;
  out->fine_hid = fine_hid;
  out->lod_hid = lod_hid;
  out->fine_tris = fine_tris;
  out->lod_tris[0] = lod_tris1;
  out->lod_tris[1] = lod_tris2;
}
using RunLoopFn = void (*)(const RunLoopIn&, u32, u32, RunLoopOut*);
// [VIS][NP][LOD]
constexpr RunLoopFn kRunLoops[2][5][2] = {
    {{run_loop<false, 0, false>, run_loop<false, 0, true>},
     {run_loop<false, 1, false>, run_loop<false, 1, true>},
     {run_loop<false, 2, false>, run_loop<false, 2, true>},
     {run_loop<false, 3, false>, run_loop<false, 3, true>},
     {run_loop<false, 4, false>, run_loop<false, 4, true>}},
    {{run_loop<true, 0, false>, run_loop<true, 0, true>},
     {run_loop<true, 1, false>, run_loop<true, 1, true>},
     {run_loop<true, 2, false>, run_loop<true, 2, true>},
     {run_loop<true, 3, false>, run_loop<true, 3, true>},
     {run_loop<true, 4, false>, run_loop<true, 4, true>}}};

// (AI-assisted, j2cull) host --ctr-gfx: env CTR_CULL_CHECK: every cluster that stood for its
// members (chunks, tie LOD groups) is decided member by member as well, and must have changed
// nothing; the visible draws' lists are checked against the walk over the whole lists. Mismatches
// are logged (the first few) and counted ("[ctr] cull check" line at exit and every 15 s).
bool cull_check_enabled() {
#ifdef __3DS__
  return false;
#else
  static const bool on = getenv("CTR_CULL_CHECK") != nullptr;
  return on;
#endif
}
struct {
  u64 checks = 0, bad = 0;
} g_cull_check;
void cull_check_count(bool ok, const std::string& level, const char* what, u32 first) {
  g_cull_check.checks++;
  if (!ok) {
    if (g_cull_check.bad++ < 20) {
      lg::error("[ctr] cull check: {} {} at {} differs", level, what, first);
    }
  }
}
// a cluster's members' decision state before deciding them one by one
struct CullCheckSnap {
  std::vector<u8> state, q, vsrc;
  std::vector<u32> hold;
  size_t visible = 0;
  u64 emits = 0;
  void snap(const CtrLevelData& lev, u32 first, u32 n, const std::vector<u8>& vista_src,
            const u32* hold_since) {
    state.assign(lev.chunk_state.begin() + first, lev.chunk_state.begin() + first + n);
    q.assign(lev.chunk_detail_q.begin() + first, lev.chunk_detail_q.begin() + first + n);
    vsrc.assign(n, 0);
    if (!vista_src.empty()) {
      memcpy(vsrc.data(), &vista_src[first], n);
    }
    hold.assign(n, 0);
    if (hold_since) {
      memcpy(hold.data(), hold_since + first, n * 4);
    }
  }
  bool same(const CtrLevelData& lev, u32 first, u32 n, const std::vector<u8>& vista_src,
            const u32* hold_since) const {
    bool ok = memcmp(state.data(), &lev.chunk_state[first], n) == 0 &&
              memcmp(q.data(), &lev.chunk_detail_q[first], n) == 0;
    if (!vista_src.empty()) {
      ok = ok && memcmp(vsrc.data(), &vista_src[first], n) == 0;
    }
    if (hold_since) {
      ok = ok && memcmp(hold.data(), hold_since + first, n * 4) == 0;
    }
    return ok;
  }
};

// (AI-assisted) host --ctr-gfx: env CTR_FINE_VIS_CHECK (ctr_level_fine_vis_check)
bool fine_vis_check_enabled() {
#ifdef __3DS__
  return false;
#else
  static const bool on = getenv("CTR_FINE_VIS_CHECK") != nullptr;
  return on;
#endif
}

#ifndef __3DS__
// (AI-assisted) the draws of what fine_vis hid this frame, checked at its end
struct FineCheckDraw {
  ctr_draw_state state;
  ctr_mesh_matrix matrix;
  int mesh;
  u32 first, count;
  std::string level;
  u32 chunk, run;
  bool by_occ;  // hidden by occ_cull (else fine_vis)
};
std::vector<FineCheckDraw> g_fine_check;
// (AI-assisted) env CTR_FINE_VIS_ORACLE: every drawn run (kind: chunk kind), queried at the frame end
struct OracleDraw {
  ctr_draw_state state;
  ctr_mesh_matrix matrix;
  int mesh;
  u32 first, count, kind;
};
std::vector<OracleDraw> g_oracle;
struct {
  u64 frames = 0, tris[4] = {}, run_hidden[4] = {}, piece_hidden[4] = {}, offscreen[4] = {};
} g_oracle_stats;
bool oracle_enabled() {
  static const bool on = getenv("CTR_FINE_VIS_ORACLE") != nullptr;
  return on;
}
struct {
  u64 frames = 0, draws = 0, bad_frames = 0, pixels = 0, worst = 0;
} g_fine_check_stats;
#endif

}  // namespace

void ctr_level_fine_vis_check(u64 frame, bool log_now) {
#ifndef __3DS__
  if (!fine_vis_check_enabled() && !oracle_enabled()) {
    return;
  }
  auto& st = g_fine_check_stats;
  st.frames++;
  u64 px = 0;
  if (!g_fine_check.empty()) {
    ctr_gpu_soft_query_begin();
    int logged = 0;
    for (const auto& d : g_fine_check) {
      const long before = ctr_gpu_soft_query_count();
      ctr_gpu_draw_mesh_prepared(&d.state, &d.matrix, d.mesh, (int)d.first, (int)d.count);
      const long n = ctr_gpu_soft_query_count() - before;
      if (n > 0) {
        px += n;
        if (logged++ < 4) {
          lg::warn("[ctr] fine_vis check: frame {}: {} chunk {} run {}: {} pixels would be seen "
                   "(hidden by {})",
                   frame, d.level, d.chunk, d.run == ~0u ? -1 : (int)d.run, n,
                   d.by_occ ? "occ_cull" : "fine_vis");
        }
      }
    }
    ctr_gpu_soft_query_end();
    st.draws += g_fine_check.size();
  }
  if (oracle_enabled()) {
    auto& o = g_oracle_stats;
    o.frames++;
    ctr_gpu_soft_query_begin();
    for (const auto& d : g_oracle) {
      const u32 k = d.kind & 3;
      o.tris[k] += d.count / 3;
      long before = ctr_gpu_soft_query_count();
      ctr_draw_state any = d.state;
      any.ztest = CTR_TEST_ALWAYS;
      ctr_gpu_draw_mesh_prepared(&any, &d.matrix, d.mesh, (int)d.first, (int)d.count);
      if (ctr_gpu_soft_query_count() == before) {
        o.offscreen[k] += d.count / 3;  // (no pixel on screen at all: the view, not occlusion)
        continue;
      }
      before = ctr_gpu_soft_query_count();
      ctr_gpu_draw_mesh_prepared(&d.state, &d.matrix, d.mesh, (int)d.first, (int)d.count);
      if (ctr_gpu_soft_query_count() == before) {
        o.run_hidden[k] += d.count / 3;
        o.piece_hidden[k] += d.count / 3;
        continue;
      }
      for (u32 i = 0; i < d.count; i += 288) {
        const u32 n = std::min<u32>(288, d.count - i);
        before = ctr_gpu_soft_query_count();
        ctr_gpu_draw_mesh_prepared(&d.state, &d.matrix, d.mesh, (int)(d.first + i), (int)n);
        if (ctr_gpu_soft_query_count() == before) {
          o.piece_hidden[k] += n / 3;
        }
      }
    }
    ctr_gpu_soft_query_end();
    g_oracle.clear();
    if (log_now) {
      static const char* kn[4] = {"?", "tfrag", "tie", "shrub"};
      for (int k = 0; k < 4; k++) {
        if (o.tris[k]) {
          lg::info("[ctr] fine_vis oracle {}: {:.0f} tris drawn per frame: {:.1f}% in runs off "
                   "screen, {:.1f}% in runs on screen with no pixel seen (occluded), {:.1f}% "
                   "occluded counting 96-triangle pieces",
                   kn[k], (double)o.tris[k] / o.frames, 100.0 * o.offscreen[k] / o.tris[k],
                   100.0 * o.run_hidden[k] / o.tris[k], 100.0 * o.piece_hidden[k] / o.tris[k]);
        }
      }
      o = {};
    }
  }
  st.pixels += px;
  st.bad_frames += px > 0;
  st.worst = std::max<u64>(st.worst, px);
  g_fine_check.clear();
  if (log_now) {
    lg::info("[ctr] fine_vis check: {} frames, {} hidden draws checked, {} frames with pixels of "
             "them that would be seen ({} pixels, at most {} in a frame)",
             st.frames, st.draws, st.bad_frames, st.pixels, st.worst);
  }
#else
  (void)frame;
  (void)log_now;
#endif
}

double ctr_level_tod_take_ms() {
  const double ms = g_tod_frame_ms;
  g_tod_frame_ms = 0;
  return ms;
}

void ctr_level_set_detail(float t) {
  g_detail_t = std::clamp(t, 0.f, 1.f);
}

void ctr_level_set_detail_urgent(bool urgent) {
  g_detail_urgent = urgent;
}

void ctr_level_set_screen_black(bool black) {
  g_screen_black = black;
}

float ctr_level_detail() {
  return g_detail_t;
}

ctr_draw_state ctr_state_from_draw_mode(DrawMode mode, int tex) {
  ctr_draw_state st;
  memset(&st, 0, sizeof(st));
  st.tex = tex;
  st.tcc = 1;
  // the texture color alone (tfrag3.vert, shrub.vert, merc2.frag: TEX0 decal bit)
  st.decal = mode.get_decal();
  st.filter = mode.get_filt_enable();
  st.clamp_s = mode.get_clamp_s_enable();
  st.clamp_t = mode.get_clamp_t_enable();
  st.blend = CTR_BLEND_OFF;
  if (mode.get_ab_enable()) {
    switch (mode.get_alpha_blend()) {
      case DrawMode::AlphaBlend::SRC_DST_SRC_DST:
        st.blend = CTR_BLEND_ALPHA;
        break;
      case DrawMode::AlphaBlend::SRC_0_SRC_DST:
        st.blend = CTR_BLEND_ADD;
        break;
      case DrawMode::AlphaBlend::SRC_0_FIX_DST:
        st.blend = CTR_BLEND_ONE_ONE;
        break;
      case DrawMode::AlphaBlend::SRC_DST_FIX_DST:
        st.blend = CTR_BLEND_FIX;
        st.fix = 0x40;
        break;
      case DrawMode::AlphaBlend::ZERO_SRC_SRC_DST:
        st.blend = CTR_BLEND_SUB;
        break;
      case DrawMode::AlphaBlend::SRC_0_DST_DST:
        st.blend = CTR_BLEND_ADD_DST_A;
        break;
      default:
        break;
    }
  }
  if (mode.get_at_enable() && mode.get_alpha_test() == DrawMode::AlphaTest::GEQUAL) {
    st.atest = CTR_TEST_GEQUAL;
    st.aref = mode.get_aref();
  } else if (mode.get_at_enable() && mode.get_alpha_test() == DrawMode::AlphaTest::NEVER) {
    st.atest = CTR_TEST_ALWAYS;  // afail tricks: just draw
  } else {
    st.atest = CTR_TEST_ALWAYS;
  }
  if (mode.get_zt_enable()) {
    switch (mode.get_depth_test()) {
      case GsTest::ZTest::NEVER:
        st.ztest = CTR_TEST_NEVER;
        break;
      case GsTest::ZTest::ALWAYS:
        st.ztest = CTR_TEST_ALWAYS;
        break;
      case GsTest::ZTest::GEQUAL:
        st.ztest = CTR_TEST_GEQUAL;
        break;
      case GsTest::ZTest::GREATER:
        st.ztest = CTR_TEST_GREATER;
        break;
    }
  } else {
    st.ztest = CTR_TEST_ALWAYS;
  }
  st.zwrite = mode.get_depth_write_enable();
  return st;
}

CtrTfragRenderer::CtrTfragRenderer(std::string name, int id, CtrLevels* levels)
    : CtrBucketRenderer(std::move(name), id), m_levels(levels) {}

void CtrTfragRenderer::render(DmaFollower& dma, CtrRenderState& rs) {
  {
    // (AI-assisted, streaming) the streaming statistics, once per log period (from the first
    // background bucket of the frame: a level slot may be empty)
    static u64 s_logged = ~0ull;
    if (rs.log_now && s_logged != rs.frame_idx) {
      s_logged = rs.frame_idx;
      m_levels->stream_log();
    }
  }
  TfragPcPortData pc_data;
  bool have_data = false;
  while (dma.current_tag_offset() != rs.next_bucket) {
    if (looks_like_tfrag_init(dma)) {
      // setup test, matrix 0, matrix 1, data, mscal (see TFragment::handle_initialization)
      for (int i = 0; i < 5; i++) {
        dma.read_and_advance();
      }
      auto pc = dma.read_and_advance();
      if (pc.size_bytes == sizeof(TfragPcPortData)) {
        memcpy(&pc_data, pc.data, sizeof(pc_data));
        pc_data.level_name[11] = '\0';
        have_data = true;
      }
      dma.read_and_advance();  // double buffer setup
      while (looks_like_tfragment_dma(dma) && dma.current_tag_offset() != rs.next_bucket) {
        dma.read_and_advance();
      }
    } else {
      auto t = dma.read_and_advance();
      // (AI-assisted) the visibility strings of level 0 and 1, first in bucket tfrag-0
      // (add-pc-port-background-data): 2048 bytes for an active level, else 16. vif0: flags from
      // the 3DS game code (1 valid, 2 all visible: no real data, 4 the level's own data only)
      if (m_id == (int)jak1::BucketId::TFRAG_LEVEL0 && rs.vis_packets < 2 &&
          t.vifcode1().kind == VifCode::Kind::PC_PORT &&
          (t.size_bytes == 2048 || t.size_bytes == 16)) {
        const int li = rs.vis_packets++;
        rs.vis_flags[li] = (u8)t.vif0();
        bool any = false;
        if (t.size_bytes == 2048) {
          memcpy(rs.vis_bits[li], t.data, 2048);
          for (int i = 0; i < 2048 && !any; i++) {
            any = rs.vis_bits[li][i] != 0;
          }
        }
        // all zero: just cleared by the game (while it loads the next string), not "nothing"
        rs.vis_valid[li] = any && (rs.vis_flags[li] & 1) && !(rs.vis_flags[li] & 2);
      }
    }
  }
  if (!have_data) {
    return;
  }
  CtrLevelData* lev = m_levels->get(pc_data.level_name, rs.frame_idx);
  // (AI-assisted) once per frame, from whichever of the level's tfrag buckets comes first: a level
  // may have no normal tfrag tree (jungleb only has a trans one), and the .c3l holds all of its
  // background (tfrag of every kind, tie, shrub)
  if (lev && lev->drawn_frame != rs.frame_idx) {
    lev->drawn_frame = rs.frame_idx;
    draw_level(*lev, pc_data.camera, rs);
  }
  log_stats(rs, lev);
}

// (AI-assisted) Jak II: tfrag (tfrag.gc), tie (tie-methods.gc) and shrub (shrubbery.gc) all put
// add-pc-tfrag3-data's packet (a PC_PORT transfer of TfragPcPortData) in their buckets; the rest of
// their DMA is for the PS2's VU programs. The .c3l has all of the level's background, so the first
// bucket of the slot with the camera this frame draws it, the others only skip their DMA.
void CtrTfragRenderer::render_jak2(DmaFollower& dma, CtrRenderState& rs, int slot) {
  {
    static u64 s_logged = ~0ull;  // the streaming statistics, as render()
    if (rs.log_now && s_logged != rs.frame_idx) {
      s_logged = rs.frame_idx;
      m_levels->stream_log();
    }
  }
  TfragPcPortData pc_data;
  bool have_data = false;
  while (dma.current_tag_offset() != rs.next_bucket) {
    auto t = dma.read_and_advance();
    if (!have_data && t.size_bytes == sizeof(TfragPcPortData) &&
        t.vifcode1().kind == VifCode::Kind::PC_PORT) {
      memcpy(&pc_data, t.data, sizeof(pc_data));
      pc_data.level_name[11] = '\0';
      have_data = true;
    }
  }
  if (!have_data) {
    return;
  }
  CtrLevelData* lev = m_levels->get(pc_data.level_name, rs.frame_idx);
  if (lev && lev->drawn_frame != rs.frame_idx) {
    lev->drawn_frame = rs.frame_idx;
    m_jak2_slot = slot;
    if (!lev->protos.empty()) {
      proto_vis_scan(*lev, rs);  // (AI-assisted, j2vis) c3l v20
    }
    draw_level(*lev, pc_data.camera, rs);
  }
  log_stats(rs, lev);
}

// (AI-assisted, j2vis) c3l v20 prototype visibility. The game sends the names of the prototypes it
// hides (prototypes-game-visible-set!: the prototype-bucket's `visible` flag) every frame it draws
// the level's ties / shrubs: pc-add-tie-vis-mask (tie-methods.gc) in the slot's tie-lN-tfrag
// bucket, pc-add-shrub-vis-mask (shrubbery.gc) in shrub-lN-shrub, each the PC_PORT transfer after
// add-pc-tfrag3-data's (names NUL ended, zero padded to a quadword; none: an empty transfer). Those
// buckets come after the slot's first background bucket, which draws the whole level, so their DMA
// (all of the frame's chain is there) is read ahead here: a few tags. A packet that didn't come
// (the game drew no ties this frame) keeps the last one. Only when a packet changed are the
// level's hidden prototypes worked out again (Tie3 / Shrub on the PC: TieProtoVisibility).
void CtrTfragRenderer::proto_vis_scan(CtrLevelData& lev, const CtrRenderState& rs) {
  bool changed = false;
  for (int k = 0; k < 2; k++) {
    const int id = m_proto_bucket[k];
    if (id < 0 || !rs.ee_mem) {
      continue;
    }
    const u32 end = rs.buckets_base + 16 * (u32)(id + 1);
    DmaFollower dma(rs.ee_mem, rs.buckets_base + 16 * (u32)id);
    bool after_pc = false, mine = false;
    for (int n = 0; n < 4096 && dma.current_tag_offset() != end; n++) {
      auto t = dma.read_and_advance();
      if (t.vifcode1().kind != VifCode::Kind::PC_PORT) {
        after_pc = false;
        continue;
      }
      if (after_pc) {
        if (mine) {
          if (!lev.proto_mask_seen[k] || lev.proto_mask[k].size() != t.size_bytes ||
              (t.size_bytes && memcmp(lev.proto_mask[k].data(), t.data, t.size_bytes) != 0)) {
            lev.proto_mask[k].assign((const char*)t.data, t.size_bytes);
            changed = true;
          }
          lev.proto_mask_seen[k] = true;
        }
        break;
      }
      if (t.size_bytes == sizeof(TfragPcPortData)) {
        // the level the packet is about (the slot's: the one being drawn)
        const char* name = (const char*)t.data + offsetof(TfragPcPortData, level_name);
        mine = strncmp(name, lev.name.c_str(), 11) == 0;
        after_pc = true;
      }
    }
  }
  if (!changed) {
    return;
  }
  // the names in a packet (Tie3's TieProtoVisibility::update)
  auto named = [&](int k, const std::string& name) {
    const std::string& m = lev.proto_mask[k];
    size_t i = 0;
    while (i < m.size()) {
      const size_t len = strnlen(m.data() + i, m.size() - i);
      if (len && len == name.size() && memcmp(m.data() + i, name.data(), len) == 0) {
        return true;
      }
      i += len + 1;
    }
    return false;
  };
  lev.proto_hidden_runs.clear();
  lev.chunk_proto_hidden.assign(lev.chunks.size(), 0);
  std::string names;
  u32 hidden = 0;
  for (auto& p : lev.protos) {
    const int k = p.kind == c3l::kProtoShrub ? 1 : 0;
    p.hidden = lev.proto_mask_seen[k] && named(k, p.name);
    if (!p.hidden) {
      continue;
    }
    hidden++;
    lev.proto_hidden_runs.insert(lev.proto_hidden_runs.end(), p.runs.begin(), p.runs.end());
    for (u32 ci : p.chunks) {
      lev.chunk_proto_hidden[ci] = 1;
    }
    names += (names.empty() ? "" : ", ") + p.name;
  }
  std::sort(lev.proto_hidden_runs.begin(), lev.proto_hidden_runs.end());
  if (!hidden) {
    lev.chunk_proto_hidden.clear();
  }
  lg::info("[ctr] {}: the game hides {} of its {} toggled prototypes ({} runs){}{}", lev.name,
           hidden, lev.protos.size(), lev.proto_hidden_runs.size(), hidden ? ": " : "", names);
}

void CtrTfragRenderer::log_stats(const CtrRenderState& rs, CtrLevelData* lev) {
  if (rs.log_now && m_level_draws) {
    lg::debug("[ctr] {}: {} of {} level draws seen from another level", m_name, m_far_levels,
              m_level_draws);
    m_far_levels = m_level_draws = 0;
  }
  if (rs.log_now && m_vis_frames) {
    lg::info("[ctr] {}: visibility hid {:.0f} chunks, {:.0f} tris of partly drawn chunks hidden (visibility and view) per frame ({} frames with data, {} of them the level's own)",
             m_name, (double)m_vis_chunks / m_vis_frames, (double)m_vis_tris / m_vis_frames,
             m_vis_frames, m_vis_own_frames);
    m_vis_chunks = m_vis_tris = m_vis_frames = m_vis_own_frames = 0;
  }
  if (rs.log_now && m_cull_frames) {
    lg::info("[ctr] {}: run culling tested {:.0f} runs, hid {:.0f} chunks and {:.0f} tris per level "
             "draw ({} level draws)",
             m_name, (double)m_cull_runs / m_cull_frames, (double)m_cull_chunks / m_cull_frames,
             (double)m_cull_tris / m_cull_frames, m_cull_frames);
    m_cull_runs = m_cull_tris = m_cull_chunks = m_cull_frames = 0;
  }
  if (rs.log_now) {
    ctr_impostors().log_stats();  // (AI-assisted)
  }
  if (rs.log_now && m_vista_frames) {
    lg::info("[ctr] {}: vista {:.1f} chunks {:.0f} tris, low resolution chunks in near mode {:.1f} "
             "per level draw ({} level draws)",
             m_name, (double)m_vista_chunks / m_vista_frames,
             (double)m_vista_tris / m_vista_frames, (double)m_lowres_near / m_vista_frames,
             m_vista_frames);
    m_vista_chunks = m_vista_tris = m_lowres_near = m_vista_frames = 0;
  }
  if (rs.log_now && m_proto_frames) {
    // (AI-assisted, j2vis) c3l v20
    lg::info("[ctr] {}: prototypes hidden by the game: {:.0f} tris not drawn per level draw ({} "
             "level draws with hidden prototypes)",
             m_name, (double)m_proto_tris / m_proto_frames, m_proto_frames);
    m_proto_tris = m_proto_frames = 0;
  }
  {
    static u64 s_check_logged = ~0ull;  // (AI-assisted, j2cull) CTR_CULL_CHECK, once per log period
    if (rs.log_now && cull_check_enabled() && s_check_logged != rs.frame_idx) {
      s_check_logged = rs.frame_idx;
      lg::info("[ctr] cull check: {} checks, {} differ", g_cull_check.checks, g_cull_check.bad);
    }
  }
  if (rs.log_now && m_ph_draws) {
    // (AI-assisted, j2cull) draw_level's phases
    const double n = (double)m_ph_draws;
    double tot = 0;
    double ms[kPhN];
    for (int k = 0; k < kPhN; k++) {
      ms[k] = ctr_gpu_ticks_to_ms(m_ph[k]) / n;
      tot += ms[k];
      m_ph[k] = 0;
    }
    lg::info("[ctr] {}: draw_level ms per level draw {:.3f}: setup {:.3f} lod {:.3f} chunks {:.3f} "
             "occ {:.3f} tod {:.3f} impostors {:.3f} lists {:.3f} cards {:.3f} flush {:.3f}; "
             "{:.0f} chunks emitted, {:.0f} runs tested, {:.0f} draws walked, {:.0f} calls; "
             "clusters stood for {:.0f} chunks, {:.0f} tie LOD groups ({} level draws)",
             m_name, tot, ms[kPhSetup], ms[kPhLod], ms[kPhChunks], ms[kPhOcc], ms[kPhTod],
             ms[kPhImp], ms[kPhLists], ms[kPhCards], ms[kPhFlush], m_ph_emits / n, m_ph_runs / n,
             m_ph_walked / n, m_ph_calls / n, m_ph_skipped / n, m_ph_lod_skipped / n, m_ph_draws);
    m_ph_draws = m_ph_emits = m_ph_runs = m_ph_walked = m_ph_calls = 0;
    m_ph_skipped = m_ph_lod_skipped = 0;
  }
  if (rs.log_now && m_lod_frames) {
    // (AI-assisted) c3l v17 tie levels of detail
    const double n = (double)m_lod_frames;
    lg::info("[ctr] {}: tie levels of detail: {:.0f} groups near, {:.0f} near + {:.0f} far version "
             "tris drawn per level draw ({} level draws)",
             m_name, m_lod_groups_near / n, m_lod_tris[0] / n, m_lod_tris[1] / n, m_lod_frames);
    m_lod_groups_near = m_lod_frames = m_lod_tris[0] = m_lod_tris[1] = 0;
  }
  if (rs.log_now && m_tod_frames) {
    lg::info("[ctr] {}: time of day {} blends, {:.0f} vertices recolored {:.2f} ms per level draw "
             "(most {} vertices, {:.2f} ms; {} level draws); {} groups rewritten, {} chunks drawn "
             "behind; groups drawn behind by at most {} day moves ({} of {} sampled level draws "
             "2 or more); a pulsing palette held in {} level draws, a day move waited in {}, {} "
             "groups too far behind waited for tod_max_ms",
             m_name, m_tod_blends, (double)m_tod_verts / m_tod_frames, m_tod_ms / m_tod_frames,
             m_tod_max, m_tod_max_ms, m_tod_frames, m_tod_spread, m_tod_deferred, m_tod_lag_max,
             m_tod_lag2, m_tod_lag_frames, m_tod_held, m_tod_waited, m_tod_capped);
    m_tod_frames = m_tod_blends = m_tod_verts = m_tod_max = m_tod_held = 0;
    m_tod_waited = m_tod_capped = 0;
    m_tod_spread = m_tod_deferred = m_tod_lag_frames = m_tod_lag_max = m_tod_lag2 = 0;
    m_tod_ms = m_tod_max_ms = 0;
  }
  if (rs.log_now && m_occ_frames) {
    lg::info("[ctr] {}: occ cull hid {:.1f} chunks, {:.1f} runs, {:.0f} tris per level draw; "
             "occluders {:.0f} tris taken, {:.0f} rasterized; {:.3f} ms per level draw ({:.3f} at "
             "most; {} level draws; gather {:.3f}, raster {:.3f} (transform {:.3f}, sort {:.3f}), pyramid "
             "{:.3f}, tests {:.3f})",
             m_name, (double)m_occ_chunks / m_occ_frames, (double)m_occ_runs / m_occ_frames,
             (double)m_occ_tris / m_occ_frames, (double)m_occ_tris_in / m_occ_frames,
             (double)m_occ_tris_drawn / m_occ_frames, m_occ_ms / m_occ_frames, m_occ_max_ms,
             m_occ_frames, m_occ_phase[0] / m_occ_frames, m_occ_phase[1] / m_occ_frames,
             m_occ_phase[4] / m_occ_frames, m_occ_phase[5] / m_occ_frames,
             m_occ_phase[2] / m_occ_frames, m_occ_phase[3] / m_occ_frames);
    for (auto& ph : m_occ_phase) {
      ph = 0;
    }
    m_occ_frames = m_occ_tris_in = m_occ_tris_drawn = m_occ_chunks = m_occ_runs = m_occ_tris = 0;
    m_occ_ms = m_occ_max_ms = 0;
  }
  if (rs.log_now && m_fine_levels) {
    lg::info("[ctr] {}: fine vis hid {:.1f} chunks, {:.0f} tris per level draw with a set ({} of "
             "{} level draws){}",
             m_name, (double)m_fine_chunks / std::max<u64>(1, m_fine_frames),
             (double)m_fine_tris / std::max<u64>(1, m_fine_frames), m_fine_frames, m_fine_levels,
             m_fine_k_hi > 0 ? fmt::format("; tie LOD factor {:.3f}..{:.3f}", m_fine_k_lo,
                                           m_fine_k_hi)
                             : "");
    m_fine_chunks = m_fine_tris = m_fine_frames = m_fine_levels = 0;
    m_fine_k_lo = 1e30f;
    m_fine_k_hi = 0;
    if (lev && lev->fine) {
      lev->fine->log_stats(lev->name);  // (AI-assisted) its blocks, cells, decodes
    }
  }
}

// (AI-assisted, gpu-rt) static_cmdlists: the pass's draws collected by draw_level, to the backend
// with the pass's command cache (0: the main pass, 1: the vista pass)
void CtrTfragRenderer::flush_calls(int pass) {
  if (!m_calls.empty()) {
    ctr_gpu_draw_mesh_calls(m_calls.data(), (int)m_calls.size(), &m_cmd_cache[pass]);
  }
  m_calls.clear();
}

CtrTfragRenderer::~CtrTfragRenderer() {
  for (auto*& c : m_cmd_cache) {
    ctr_gpu_cmd_cache_free(&c);
  }
}

void CtrTfragRenderer::draw_level(CtrLevelData& lev,
                                  const CtrBackgroundCamera& cam,
                                  const CtrRenderState& rs) {
  // (AI-assisted, j2cull) the phases' ticks (m_ph)
  u64 ph_t = ctr_gpu_ticks();
  auto phase = [&](int k) {
    const u64 now = ctr_gpu_ticks();
    m_ph[k] += now - ph_t;
    ph_t = now;
  };
  m_ph_draws++;
  if (!lev.cull_made || lev.cull_sizes[0] != lev.chunks.size() ||
      lev.cull_sizes[1] != lev.lod_groups.size() || lev.cull_sizes[2] != lev.chunk_unit.size()) {
    make_cull_clusters(lev);  // (AI-assisted, j2cull) once per load
  }
  const CtrSettings& settings = ctr_settings();
  auto R = make_cam_mat(cam.rot, cam.perspective, cam.fog.x(), cam.hvdf_off.z());
  m_late_in_view = 0;  // (AI-assisted, j2intro) counted below, reported after the chunk loop
  // (AI-assisted) c3l v16 time of day: blend the level's colors with the game's weights when they
  // moved (the chunks are recolored as they are drawn, emit_chunk)
  u64 tod_verts = 0;
  double tod_ms = 0;
  if (lev.tod) {
    const double t0 = ctr_gpu_time_ms();
    m_tod_frames++;
    const u32 held0 = lev.tod->pulse_held;
    m_tod_blends += tod_update(*lev.tod, lev.name, cam, rs.frame_idx) == 2;
    m_tod_held += lev.tod->pulse_held != held0;  // (AI-assisted, citadel) tod_pulse_step
    tod_ms += ctr_gpu_time_ms() - t0;
  }
#ifndef __3DS__
  // (AI-assisted) host diagnostic: the game's time of day weights every 30 frames
  if (const char* e = getenv("CTR_TOD_LOG"); e && rs.frame_idx % (e[0] == '2' ? 1 : 30) == 0) {
    std::string w;
    for (int p = 0; p < 8; p++) {
      w += " [";
      for (int ch = 0; ch < 4; ch++) {
        const u32 word = (u32)cam.itimes[p / 2][(p % 2) * 2 + ch / 2];
        w += fmt::format("{}{}", ch ? "," : "", ((ch % 2) ? (word >> 16) : word) & 0xff);
      }
      w += "]";
    }
    lg::info("[ctr] tod {} frame {}:{}", lev.name, rs.frame_idx, w);
  }
#endif
  // fog (ctr_gpu_set_mesh_fog): the game's (tfrag3.vert), and towards the draw distance
  const float draw_dist = settings.draw_distance * 4096.f;
  const float lod_dist = settings.lod_distance * 4096.f;
  // camera outside the level's bounds by more than far_level_distance: the level is only seen
  // from a neighbouring one
  bool far_level = false;
  if (settings.far_level_distance > 0) {
    float d2 = 0;
    for (int c = 0; c < 3; c++) {
      const float v = cam.trans[c];
      const float o = v < lev.bbox_min[c] ? lev.bbox_min[c] - v
                                         : (v > lev.bbox_max[c] ? v - lev.bbox_max[c] : 0.f);
      d2 += o * o;
    }
    const float m = settings.far_level_distance * 4096.f;
    // (AI-assisted) +-10% hysteresis: a camera at the edge doesn't switch the level's ties and
    // shrubs on and off every few frames
    const float mf = m * (lev.was_far ? 0.9f : 1.1f);
    far_level = d2 > mf * mf;
    lev.was_far = far_level;
    // (AI-assisted) partial unload (the user's idea): a level seen only from far away for a few
    // seconds gives back the memory of the chunks far mode doesn't draw; read again when the camera
    // is within 30 m of the far distance (freed beyond 60 m: no back and forth at one edge)
    constexpr float kUnloadMeters = 60.f, kReloadMeters = 30.f;
    constexpr u32 kUnloadFrames = 300;
    const float unload_d = m + kUnloadMeters * 4096.f, reload_d = m + kReloadMeters * 4096.f;
    if (lev.detail_loaded) {
      lev.far_frames = d2 > unload_d * unload_d ? lev.far_frames + 1 : 0;
      if (lev.far_frames > kUnloadFrames) {
        m_levels->unload_detail(lev);
      }
    } else if (!lev.detail_requested && rs.frame_idx >= lev.detail_retry_frame &&
               d2 < reload_d * reload_d) {
      m_levels->request_detail(lev);
    }
    if (!lev.detail_loaded) {
      far_level = true;  // until the meshes are back
    }
  }
  m_far_levels += far_level;
  m_level_draws++;
  bool j2_hidden = false;  // (AI-assisted, j2-mem) Jak II: its string is all zero this frame
  // (AI-assisted) the game's visibility string for this level (bucket of level 1 or 0). Ties are
  // hidden by it always; tfrag only when the string is the level's own: seen from another
  // level's place the game picks the low resolution tfrag through these bits, which the 3DS
  // picks by distance (far_level) instead, so the detailed tfrag would get holes.
  bool vis_own = false;
  if (m_jak2_slot >= 0) {
    // (AI-assisted) Jak II: the slot's string (bucket 2). It has no 3DS flags; the PC hides the
    // level's tfrag and tie by it alike, so it counts as the level's own.
    const auto& v = m_levels->jak2_vis();
    const bool ok = m_jak2_slot < CtrJak2Vis::kSlots && v.valid[m_jak2_slot];
    m_vis = settings.vis_culling && ok && !lev.vis_runs.empty() ? v.bits[m_jak2_slot] : nullptr;
    vis_own = m_vis != nullptr;
    // (AI-assisted, j2-mem) dormant levels: the string all zero (the bytes of the level's ids)
    if (m_vis && settings.dormant_levels) {
      if (lev.vis_scan_bytes == UINT32_MAX) {
        u32 hi = 0;
        bool any = false;
        for (const auto& r : lev.vis_runs) {
          if (r.vis != 0xffff) {
            hi = std::max<u32>(hi, r.vis);
            any = true;
          }
        }
        lev.vis_scan_bytes = any ? std::min<u32>(hi / 8 + 1, 2048) : 0;
      }
      bool zero = lev.vis_scan_bytes > 0;
      for (u32 b = 0; zero && b < lev.vis_scan_bytes; b++) {
        zero = m_vis[b] == 0;
      }
      j2_hidden = zero;
    }
    if (settings.dormant_levels) {
      lev.hidden_frames = j2_hidden ? lev.hidden_frames + 1 : 0;
      if (j2_hidden && !lev.dormant && lev.hidden_frames >= kDormantFrames) {
        lev.dormant = true;  // (its far ties freed: CtrLevels::dormant_update)
        lg::info("[ctr] {}: hidden by the visibility string for {} frames: dormant", lev.name,
                 kDormantFrames);
      } else if (!j2_hidden && lev.dormant) {
        lev.dormant = false;
        lev.shown_frame = std::max<u64>(rs.frame_idx, 1);  // (AI-assisted, j2intro) a reveal
        lg::info("[ctr] {}: shown by the visibility string again (camera {:.0f} {:.0f} {:.0f} m)",
                 lev.name, cam.trans.x() / 4096.f, cam.trans.y() / 4096.f, cam.trans.z() / 4096.f);
      }
    } else {
      lev.dormant = false;  // (its freed chunks still come back by distance)
    }
  } else {
  const int li = (m_id == (int)jak1::BucketId::TFRAG_LEVEL1 ||
                  m_id == (int)jak1::BucketId::TFRAG_TRANS1_AND_SKY_BLEND_LEVEL1 ||
                  m_id == (int)jak1::BucketId::TFRAG_DIRT_LEVEL1 ||
                  m_id == (int)jak1::BucketId::TFRAG_ICE_LEVEL1)
                     ? 1
                     : 0;
  m_vis = settings.vis_culling && rs.vis_valid[li] && !lev.vis_runs.empty() ? rs.vis_bits[li]
                                                                            : nullptr;
  vis_own = m_vis && (rs.vis_flags[li] & 4);
  }
  // (AI-assisted) neighbour_vis: the string of a level the camera isn't in (computed from the
  // other level's data): the PS2 and PC draw its tfrag, low resolution tfrag included, by the bits
  const bool vis_nb = settings.neighbour_vis && m_vis && !vis_own;
  m_vis_frames += m_vis != nullptr;
  m_vis_own_frames += vis_own;
  // (AI-assisted) fine_vis: the set of the camera's cell (the camera's own position), while the
  // level is drawn as the sets assume: near mode with all of its meshes (an occluder that isn't
  // drawn hides nothing), everything within the occluder range drawn (draw distance), the LOD
  // tiers by the distances the tool assumed (the coarse tier certain only beyond the 1.5x hold of
  // a detailed cell in view), the occluders' own distance limits at factors no smaller than the
  // tool's, ties drawn as geometry (impostors) and closed tie surfaces culled the right way round
  // (cull_ties not flip), and the tie LOD switch within the tool's range. Elsewhere (no cell,
  // cutscene cameras outside the cells, a block not read yet, far mode...) nothing more is hidden.
#ifndef __3DS__
  // (AI-assisted) host: env CTR_FINE_VIS_CAMLOG=FILE, the camera positions (level x y z, meters)
  // every 4th level draw (ctr_vis_builder --cam-stats: where the cells should be)
  static FILE* camlog = getenv("CTR_FINE_VIS_CAMLOG") ? fopen(getenv("CTR_FINE_VIS_CAMLOG"), "a")
                                                      : nullptr;
  if (camlog && (getenv("CTR_FINE_VIS_CAMLOG_ALL") || (rs.frame_idx & 3) == 0)) {
    fprintf(camlog, "%s %.3f %.3f %.3f %llu\n", lev.name.c_str(), cam.trans.x() / 4096.f,
            cam.trans.y() / 4096.f, cam.trans.z() / 4096.f, (unsigned long long)rs.frame_idx);
    fflush(camlog);
  }
#endif
  m_fine = nullptr;
  m_fine_bits = nullptr;
  m_fine_hidden.clear();
  if (lev.fine && settings.fine_vis) {
    auto& fv = *lev.fine;
    const auto& fh = fv.hdr;
    m_fine_levels++;
    const float occ_dist = fh.occluder_range + 2 * std::max(fh.eps, fh.eps_coarse);
    // (not while another level's visibility string hides this level's tfrag: neighbour_vis)
    // why not (the first reason; statistics)
    const char* why = nullptr;
    if (far_level) {
      why = "far mode";
    } else if (lev.incomplete) {
      why = "incomplete load";
    } else if (lev.dormant_requested) {
      why = "dormant chunks being read";  // (AI-assisted, j2-mem)
    } else if (!(draw_dist <= 0 || draw_dist >= occ_dist)) {
      why = "draw_distance";
    } else if (!(lod_dist > 0 && lod_dist * 0.9f >= fh.lod_near &&
                 lod_dist * std::max(1.1f, kHoldInView) <= fh.lod_far)) {
      why = "lod_distance";
    } else if (settings.detail_scale < fh.detail_min) {
      why = "detail_scale";
    } else if ((fh.occluders & c3v::kOccTie) &&
               (settings.tie_distance_scale < fh.tie_scale_min ||
                (settings.impostors && settings.impostor_distance * 4096.f < fh.tie_occ_max))) {
      why = "tie distances";
    } else if ((fh.occluders & c3v::kOccShrub) &&
               (!lev.shrub_ps2_dist || settings.shrub_scale < fh.shrub_scale_min)) {
      why = "shrub distances";
    } else if ((fh.occluders & c3v::kOccClosed) && settings.cull_flip) {
      why = "cull_flip";
    }
    if (!lev.lod_groups.empty()) {
      // the tie LOD switch factor (as below: detail_scale x focal / tie_lod_px, switched by the
      // groups' shape errors)
      const float pzw = std::abs(cam.perspective[2][3]);
      const float focal =
          pzw > 0 ? 200.f * std::abs(cam.fog.x() * cam.perspective[0][0] / 256.f) / pzw : 0.f;
      const float k = settings.tie_lod ? settings.detail_scale * focal /
                                             std::max(settings.tie_lod_px, 0.25f)
                                       : 0.f;
      m_fine_k_lo = std::min(m_fine_k_lo, k);
      m_fine_k_hi = std::max(m_fine_k_hi, k);
      // the switch as draw_level makes it; without it (or tie_lod off) only the far versions are
      // drawn: fine for far version occluders, not for near ones
      const bool switching = settings.tie_lod && lev.run_lod.size() >= lev.vis_runs.size();
      if (!why && (fh.occluders & c3v::kOccTieLod)) {
        if (switching ? !(lev.lod_error.size() == lev.lod_groups.size() && k >= fh.tie_k_lo &&
                          k <= fh.tie_k_hi)
                      : (fh.occluders & c3v::kOccTieNear) != 0) {
          why = "tie LOD switch";
        }
      }
    } else if (!why && (fh.occluders & c3v::kOccTieLod)) {
      why = "no tie LOD";
    }
    const bool ok = !why;
    if (why) {
      fv.not_usable_why = why;
    }
    m_fine_bits = fv.update(cam.trans.x(), cam.trans.y(), cam.trans.z(), ok, rs.frame_idx);
    if (m_fine_bits) {
      m_fine = &fv;
      m_fine_frames++;
    }
  }
  // (AI-assisted) lowres_vista: the low resolution tfrag as far scenery (vista fog, out to
  // vista_distance); in near mode where no loaded level's detailed tfrag is (+ kDetailMargin)
  const bool lowres_vista = settings.lowres_vista;
  const float vista_dist = settings.vista_distance * 4096.f;
  constexpr float kDetailMargin = 50.f * 4096.f;
  m_levels->loaded(&m_loaded);
  // (AI-assisted) lowres_vista: outside_detail's answers, per chunk, for this set of loaded levels
  if (lev.outside_gen != m_levels->set_generation() ||
      lev.outside_cache.size() != lev.chunks.size()) {
    lev.outside_gen = m_levels->set_generation();
    lev.outside_cache.assign(lev.chunks.size(), 0);
  }
  // (AI-assisted) fog_stage_skip
  const bool fog_skip = settings.fog_stage_skip;
  const bool early_depth = settings.early_depth;  // (AI-assisted) the band order below
  const float pzw = std::abs(cam.perspective[2][3]);
  constexpr float kNoFog = 0.5f / 255.f;
  float fog0[4] = {0.f, 255.f, 255.f, -1.f / 255.f};
  float fog1[4] = {0.f, 1.f, 0.f, 0.f};
  float fog1_vista[4] = {0.f, 1.f, 0.f, 0.f};
  {
    if (settings.fog) {
      fog0[0] = cam.hvdf_off.w();
      fog0[1] = cam.fog.y();
      fog0[2] = cam.fog.z();
    }
    if (draw_dist > 0 && pzw > 0) {
      const float start = draw_dist * settings.fog_start;
      fog1[0] = 1.f / pzw;
      fog1[1] = start;
      fog1[2] = 1.f / std::max(draw_dist - start, 1.f);
    }
    // (AI-assisted) the vista's haze: from draw_distance (or a third of vista_distance) to
    // vista_distance
    if (vista_dist > 0 && pzw > 0) {
      const float start = draw_dist > 0 && draw_dist < vista_dist ? draw_dist : vista_dist / 3.f;
      fog1_vista[0] = 1.f / pzw;
      fog1_vista[1] = start;
      fog1_vista[2] = 1.f / std::max(vista_dist - start, 1.f);
      // (AI-assisted) a level with a vista tier continues past the draw distance: one haze for
      // all of it, from fog_start * draw_distance to vista_distance (else a ring of solid fog
      // color at the draw distance, then the vista unfogged)
      if (lev.has_vista && draw_dist > 0 && draw_dist < vista_dist) {
        fog1_vista[1] = draw_dist * settings.fog_start;
        fog1_vista[2] = 1.f / std::max(vista_dist - fog1_vista[1], 1.f);
        memcpy(fog1, fog1_vista, sizeof(fog1));
      }
    }
    ctr_gpu_set_mesh_fog(fog0, fog1, rs.fog_color[0], rs.fog_color[1], rs.fog_color[2]);
  }
  // (AI-assisted) config opaque_alpha (gpu_profile = ab switches it): the v15 draws' alpha tests /
  // blending back, or out again
  if (lev.opaque_alpha_applied != settings.opaque_alpha) {
    lev.opaque_alpha_applied = settings.opaque_alpha;
    for (size_t di = 0; di < lev.draws.size() && di < lev.draw_states.size(); di++) {
      const auto& d = lev.draws[di];
      set_opaque_alpha(&lev.draw_states[di], d, settings.opaque_alpha,
                       d.texture < lev.tex_min_alpha.size() ? lev.tex_min_alpha[d.texture] : 255);
    }
  }
  // this level's textures: VRAM first if the camera is in it (see ctr_gpu_pool_create)
  ctr_gpu_pool_set_priority(lev.tex_pool, far_level ? 1 : 2);
  // Jak 1 scissor adjust (tfrag3.vert: y *= 512 / 448); (AI-assisted) Jak II's 512 / 416 * 0.5
  const float kYScale = ctr_gs_y_scale();
  m_visible.clear();
  m_visible_slot.assign(lev.chunks.size(), -1);
  // (AI-assisted) run_culling (needs the spheres made at load)
  const bool run_culling = settings.run_culling && !lev.run_spheres.empty();
  // (AI-assisted, j2vis) c3l v20: prototypes the game hides (proto_vis_scan)
  const bool proto_hide = !lev.chunk_proto_hidden.empty() && m_jak2_slot >= 0;
  m_proto_frames += proto_hide;
  m_cull_frames += run_culling;
  if (m_run_shown.size() < lev.vis_runs.size()) {
    m_run_shown.resize(lev.vis_runs.size());
  }
  if (lev.chunk_state.size() != lev.chunks.size()) {
    lev.chunk_state.assign(lev.chunks.size(), 0);
  }
  // (AI-assisted, micro-level) chunk_max_dist's factors once per level draw (per chunk the same
  // operations on them as before), and the arrays and the camera in locals: the u8 stores to the
  // chunk states had every member and vector loaded again after each of them
  const float det = settings.detail_scale;
  // (AI-assisted, title-popin) the auto_detail factors at position t (t = g_detail_t: this frame's)
  const bool auto_detail = settings.auto_detail;
  const float shrub_k0 = settings.shrub_scale, shrub_d0 = settings.shrub_distance,
              tie_k0 = settings.tie_distance_scale;
  const float shrub_k_span = std::max(settings.shrub_scale_max, shrub_k0) - shrub_k0;
  const float shrub_d_span = std::max(settings.shrub_distance_max, shrub_d0) - shrub_d0;
  const float tie_k_span = std::max(settings.tie_distance_scale_max, tie_k0) - tie_k0;
  const float t_now = auto_detail ? g_detail_t : 0.f;
  const float shrub_k = shrub_k0 + shrub_k_span * t_now;
  const float shrub_d = shrub_d0 + shrub_d_span * t_now;
  const float tie_k = tie_k0 + tie_k_span * t_now;
  const bool shrub_ps2 = lev.shrub_ps2_dist;
  const bool shrub_fixed = settings.shrub_distance > 0;
  const float shrub_fixed_dist = shrub_d * 4096.f * det;
  // (AI-assisted, title-popin) a chunk's own distance limit (0: none) at auto_detail position t
  auto chunk_limit = [&](const c3l::Chunk& ch, float t) {
    if (ch.kind == c3l::CHUNK_KIND_SHRUB && shrub_ps2) {
      return ch.max_dist * (shrub_k0 + shrub_k_span * t) * det;
    } else if (ch.kind == c3l::CHUNK_KIND_SHRUB && shrub_fixed) {
      return (shrub_d0 + shrub_d_span * t) * 4096.f * det;
    } else if (ch.kind == c3l::CHUNK_KIND_TIE) {
      return ch.max_dist * (tie_k0 + tie_k_span * t) * det;
    }
    return ch.max_dist * det;
  };
  const u8 q_now = detail_q(g_detail_t);
  if (lev.chunk_detail_q.size() != lev.chunks.size()) {
    lev.chunk_detail_q.assign(lev.chunks.size(), 0xff);
  }
  u8* const chunk_q = lev.chunk_detail_q.data();
  const float cx = cam.trans[0], cy = cam.trans[1], cz = cam.trans[2];
  // (AI-assisted, streaming) a chunk's distance decision (draw distance, its own limit, 10%
  // hysteresis, auto_detail's held distances, kept while in view and moving away): the chunk loop's,
  // and the near chunks' (before the tie LOD groups, which need them). Updates the chunk's state.
  auto decide_dist = [&](size_t ci, const c3l::Chunk& ch, u8& state,
                         bool vista_chunk) __attribute__((always_inline)) {
    bool dist_ok = true;
    {
      // draw distance; small objects only up close
      const float base_dist = vista_chunk ? vista_dist : draw_dist;
      float max_dist = base_dist;
      float d;
      if (ch.kind == c3l::CHUNK_KIND_SHRUB && shrub_ps2) {
        d = ch.max_dist * shrub_k * det;  // (AI-assisted) c3l v14: the shrubs' PS2 distance
      } else if (ch.kind == c3l::CHUNK_KIND_SHRUB && shrub_fixed) {
        d = shrub_fixed_dist;
      } else if (ch.kind == c3l::CHUNK_KIND_TIE) {
        d = ch.max_dist * tie_k * det;
      } else {
        d = ch.max_dist * det;
      }
      // (AI-assisted, title-popin) the limit with own distance d (the draw distance's fog hides
      // what goes at base_dist: not held)
      auto limit_with = [&](float dd) {
        return dd > 0 ? (base_dist > 0 ? std::min(base_dist, dd) : dd) : base_dist;
      };
      max_dist = limit_with(d);
      if (max_dist > 0) {
        const float dx = ch.bsphere[0] - cx, dy = ch.bsphere[1] - cy, dz = ch.bsphere[2] - cz;
        const float d2 = dx * dx + dy * dy + dz * dz;
        const bool was_in = state & 4;
        auto within = [&](float m, float f) {
          const float lim = m * f + ch.bsphere[3];
          return d2 <= lim * lim;
        };
        const float f = was_in ? 1.1f : 1.f;
        dist_ok = within(max_dist, f);
        // (AI-assisted, title-popin) no chunk in view appears or goes because auto_detail moved:
        // it keeps the distances it was decided with while they decide otherwise (except a GPU
        // about to miss frames, for going)
        int in_view = -1;
        float dd = d;
        const u8 cq = chunk_q[ci];
        if (auto_detail && cq != q_now && cq != 0xff && d > 0 &&
            (ch.kind == c3l::CHUNK_KIND_SHRUB || ch.kind == c3l::CHUNK_KIND_TIE)) {
          const float d_old = chunk_limit(ch, cq / kDetailQ);
          const bool ok_old = within(limit_with(d_old), f);
          if (ok_old != dist_ok && !(g_detail_urgent && !dist_ok)) {
            in_view = sphere_in_view(cam, ch.bsphere);
            if (in_view) {
              dist_ok = ok_old;
              dd = d_old;
            }
          }
        }
        if (dd == d) {
          chunk_q[ci] = q_now;
        }
        // (AI-assisted, title-popin) in view and moving away: drawn up to kHoldInView x its own
        // distance (not beyond the draw distance)
        if (was_in && !dist_ok && dd > 0 && (base_dist <= 0 || dd < base_dist)) {
          if (in_view < 0) {
            in_view = sphere_in_view(cam, ch.bsphere);
          }
          if (in_view) {
            dist_ok = within(limit_with(dd * kHoldInView), 1.f);
          }
        }
        state = (u8)(dist_ok ? (state | 4) : (state & ~4));
      }
    }
    return dist_ok;
  };
  // (AI-assisted, streaming) c3l v19 near chunks: there (or not) and within their distance, this
  // frame (never in far mode: the far versions)
  phase(kPhSetup);
  const bool has_units = !lev.stream_units.empty();
  if (has_units) {
    lev.unit_ok.assign(lev.stream_units.size(), 0);
    // 0: no (far mode, never loaded), 1: yes, 2: beyond its distance, 3: within it but not there
    // (its distance state follows the camera all the same: no hysteresis jump when it comes)
    // (a unit whose groups are all beyond their near distance, by its bound with the last frame's
    // factor x 1.25: not decided, 2; its groups are far anyway)
    const float kb = lev.stream_k * 1.25f;
    for (size_t u = 0; u < lev.stream_units.size(); u++) {
      const auto& su = lev.stream_units[u];
      const u32 ci = su.chunk;
      if (far_level || su.state == CtrLevelData::kUnitNever) {
        continue;
      }
      {
        const float* c = lev.chunks[ci].bsphere;
        const float dx = c[0] - cx, dy = c[1] - cy, dz = c[2] - cz;
        const float lim = su.need_a + kb * su.need_e;
        if (kb > 0 && dx * dx + dy * dy + dz * dz > lim * lim) {
          lev.unit_ok[u] = 2;
          continue;
        }
      }
      const bool there = su.state == CtrLevelData::kUnitResident && lev.meshes[ci] >= 0;
      lev.unit_ok[u] = !decide_dist(ci, lev.chunks[ci], lev.chunk_state[ci], false) ? 2
                       : there                                                     ? 1
                                                                                   : 3;
    }
  }
  // (AI-assisted) c3l v17 tie levels of detail: each group's near version while the camera is
  // within its distance (the prototype's PS2 far-plane, times tie_lod_scale) of its sphere, with 10%
  // hysteresis, decided for every group every frame (turning the camera doesn't switch any).
  // auto_detail pulls the distances in to half while the GPU is busy. tie_lod = off: the far
  // versions only (as before v17).
  const bool tie_lod = !lev.lod_groups.empty() && lev.run_lod.size() >= lev.vis_runs.size();
  u32 late_groups = 0;  // (AI-assisted, streaming) drawn far: their near chunks not there yet
  u32 held_groups = 0;  // (AI-assisted, streaming) ... there now, but kept far while in view
  if (tie_lod) {
    u32 near = 0;
    // (AI-assisted, title-popin) files with shape errors: the near version while the far one's
    // error would show as more than tie_lod_px pixels (focal: this frame's projection, pixels of
    // the 400-wide top screen per unit of x / depth; make_cam_mat). The PS2 morphs geometry 2 into
    // 3 up to the far-plane; switching there between 0 and 3 made pops of 3 px (median) to 19 px
    // in village1, and auto_detail pulling the distances in made them larger. Older files: the
    // PS2 distance, auto_detail pulling it in to half while the GPU is busy.
    const bool by_error = lev.lod_error.size() == lev.lod_groups.size();
    const float pzw = std::abs(cam.perspective[2][3]);
    const float focal =
        pzw > 0 ? 200.f * std::abs(cam.fog.x() * cam.perspective[0][0] / 256.f) / pzw : 0.f;
    // the distance factor at auto_detail position t: with shape errors tie_lod_px pixels (auto_detail
    // doesn't move it: pulling the switch in made the pops larger); older files: the PS2 distance,
    // pulled in to half while the GPU is busy
    const float k0 = settings.tie_lod ? settings.detail_scale : 0.f;
    // (AI-assisted, streaming: lod_auto, was auto_detail: the chunk loop's auto_detail is above now)
    const bool lod_auto = settings.auto_detail && !by_error;
    const float px = std::max(settings.tie_lod_px, 0.25f);
    auto k_at = [&](float t) {
      if (by_error) {
        return k0 * focal / px;
      }
      return lod_auto ? k0 * (0.5f + 0.5f * t) : k0;
    };
    const float k = k_at(g_detail_t);
    if (lev.lod_detail_q.size() != lev.lod_groups.size()) {
      lev.lod_detail_q.assign(lev.lod_groups.size(), 0xff);
    }
    u8* const lod_q = lev.lod_detail_q.data();
    if (has_units && lev.lod_hold_since.size() != lev.lod_groups.size()) {
      lev.lod_hold_since.assign(lev.lod_groups.size(), 0);
    }
    // (AI-assisted, streaming) the longest a group drawn far for want of its data is kept far in
    // view once the data came (4 s at 30 fps)
    constexpr u32 kHoldFrames = 120;
    // (AI-assisted, j2cull) cluster by cluster (CtrLevelData::CullCluster): a cluster beyond every
    // member's near distance (largest switch distance x k, with margins for float rounding) whose
    // members all were far and not held last frame (state 2) stands for their decisions: each
    // would come out far again, state 2, its lod_detail_q this frame's (only so without lod_auto:
    // its in-view hold reads them)
    const u8* const lod_st = lev.lod_state.data();
    const bool lod_skip = !lod_auto && lev.lod_state.size() == lev.lod_groups.size();
    const bool cull_check = cull_check_enabled();
    bool lod_check = false;
    std::vector<u8> chk_lod_q;
    u32 chk_near = 0;
    for (const auto& gcl : lev.lod_clusters) {
      if (lod_skip && !(gcl.flags & CtrLevelData::kClPlain)) {
        const float sw = by_error ? gcl.raw[0] : gcl.raw[1];
        bool far = !(k > 0);
        if (!far) {
          const float dx = gcl.c[0] - cx, dy = gcl.c[1] - cy, dz = gcl.c[2] - cz;
          const float reach = (gcl.a + std::max(sw, 0.f) * k) * 1.0002f + 256.f;
          far = dx * dx + dy * dy + dz * dz > reach * reach;
        }
        if (far) {
          u32 g = gcl.first;
          const u32 end = gcl.first + gcl.count;
          while (g < end && lod_st[g] == 2) {
            g++;
          }
          if (g == end) {
            memset(lod_q + gcl.first, q_now, gcl.count);
            m_ph_lod_skipped += gcl.count;
            if (!cull_check) {
              continue;
            }
            lod_check = true;
            chk_lod_q.assign(lod_q + gcl.first, lod_q + end);
            chk_near = near;
          }
        }
      }
    for (size_t g = gcl.first, g_end = gcl.first + gcl.count; g < g_end; g++) {
      const auto& lg = lev.lod_groups[g];
      u8& st = lev.lod_state[g];
      const float dx = lg.bsphere[0] - cx, dy = lg.bsphere[1] - cy, dz = lg.bsphere[2] - cz;
      const float d2 = dx * dx + dy * dy + dz * dz;
      const float sw = by_error ? lev.lod_error[g] : lg.dist;
      const float f = (st & 1) ? 1.1f : 1.f;
      auto near_at = [&](float kk) {
        const float lim = sw * kk * f + lg.bsphere[3];
        return kk > 0 && d2 < lim * lim;
      };
      bool n = near_at(k);
      // (AI-assisted, title-popin) no switch in view because auto_detail moved: a group in view
      // keeps the factor it was decided with while the two would choose differently (except a
      // GPU about to miss frames, for the far version)
      bool held = false;
      if (lod_auto && (st & 2) && lod_q[g] != q_now && lod_q[g] != 0xff) {
        const bool n_old = near_at(k_at(lod_q[g] / kDetailQ));
        if (n_old != n && !(g_detail_urgent && !n) && sphere_in_view(cam, lg.bsphere)) {
          n = n_old;
          held = true;
        }
      }
      if (!held) {
        lod_q[g] = q_now;
      }
      // (AI-assisted, streaming) c3l v19: the near version only while every near chunk holding it
      // is there and within its distance; else the far version (data late: statistics). A group
      // drawn far in view for want of its data (bit 4: held) keeps its far version while in view
      // once the data is there: switching then would be a pop on screen; it switches out of view
      bool keep_far = (st & 4) != 0;
      if (n && has_units) {
        if (g >= lev.group_unit_count.size() || !lev.group_unit_count[g]) {
          n = false;  // (a v19 group whose near version is in no near chunk: none to draw)
        } else {
          const u32* gu = &lev.group_units[lev.group_unit_first[g]];
          bool ok = true, late = false;
          for (u32 j = 0; j < lev.group_unit_count[g]; j++) {
            const u8 uo = lev.unit_ok[gu[j]];
            ok = ok && uo == 1;
            late = late || uo == 3;
          }
          if (!ok) {
            n = false;
            if (late) {
              late_groups++;
              for (u32 j = 0; j < lev.group_unit_count[g]; j++) {
                if (lev.unit_ok[gu[j]] == 3) {
                  m_levels->stream_late(lev, gu[j], rs.frame_idx);
                }
              }
              const bool seen = sphere_in_view(cam, lg.bsphere);
              m_late_in_view += seen;  // (AI-assisted, j2intro)
              // (AI-assisted, j2intro) not while the screen is black (Jak II): it switches as soon
              // as the data is there
              if (!keep_far && !g_screen_black && seen) {
                keep_far = true;
                lev.lod_hold_since[g] = (u32)rs.frame_idx;
              }
            }
          } else if (keep_far && g_screen_black) {
            keep_far = false;  // (AI-assisted, j2intro) black screen: near at once
          } else if (keep_far) {
            // kept far while in view, at most kHoldFrames, and while the switch would show: the
            // far version's error at least 1.5 x tie_lod_px (the camera nearer than 2/3 of the
            // group's near distance; near it the error is about tie_lod_px)
            const float lim = sw * k / 1.5f + lg.bsphere[3];
            if (sphere_in_view(cam, lg.bsphere) && d2 < lim * lim &&
                (u32)rs.frame_idx - lev.lod_hold_since[g] < kHoldFrames) {
              n = false;
              held_groups++;
            } else {
              keep_far = false;
            }
          }
        }
      } else if (!n) {
        keep_far = false;  // (beyond its distance: far anyway)
      }
      st = (u8)((keep_far ? 4 : 0) | 2 | (n ? 1 : 0));
      near += n;
    }
      if (lod_check) {
        lod_check = false;
        bool ok = chk_near == near &&
                  memcmp(chk_lod_q.data(), lod_q + gcl.first, gcl.count) == 0;
        for (u32 g = gcl.first; g < gcl.first + gcl.count; g++) {
          ok = ok && lod_st[g] == 2;
        }
        cull_check_count(ok, lev.name, "tie LOD cluster", gcl.first);
      }
    }
    m_lod_groups_near += near;
    m_lod_frames++;
    // (AI-assisted, streaming) where the near chunks are needed (CtrLevels::stream_update)
    if (lev.stream_k > 0 && (k * 1.1f > lev.stream_k * 1.3f || k * 1.1f * 1.3f < lev.stream_k)) {
      m_levels->stream_note_cut(rs.frame_idx);  // (another camera, or a zoom)
    }
    lev.stream_k = k * 1.1f;
    lev.stream_k_hi = std::max(lev.stream_k, lev.stream_k_hi * 0.99f);
    if (j2_hidden && settings.dormant_levels) {
      // (AI-assisted, j2-mem) nothing of it can be seen: no near chunks wanted (theirs go first)
      lev.stream_k = lev.stream_k_hi = 0.f;
    }
    if (has_units && stream_check_enabled()) {
      // (AI-assisted, streaming) host check: a near version is only ever drawn with all of its
      // near chunks there and within their distance (else the far one: never a hole)
      auto& sc = m_levels->stream_check();
      sc.frames++;
      sc.groups += lev.lod_groups.size();
      sc.late_groups += late_groups;
      sc.late_frames += late_groups > 0;
      for (size_t g = 0; g < lev.lod_groups.size(); g++) {
        if (!(lev.lod_state[g] & 1)) {
          continue;
        }
        const u32 cnt = g < lev.group_unit_count.size() ? lev.group_unit_count[g] : 0;
        bool ok = cnt > 0;
        for (u32 j = 0; j < cnt; j++) {
          const u32 u = lev.group_units[lev.group_unit_first[g] + j];
          ok = ok && lev.unit_ok[u] == 1 && lev.meshes[lev.stream_units[u].chunk] >= 0;
        }
        sc.holes += !ok;
      }
    }
  } else {
    lev.stream_k = 0.f;
  }
  if (has_units) {
    lev.stream_frame = rs.frame_idx;
    m_levels->stream_note_level(lev, late_groups, held_groups, rs.frame_idx);
  }
  m_levels->stream_note_camera(cam.trans, cam.planes, cam.rot, rs.frame_idx);
  phase(kPhLod);
  if (m_run_nomerge.size() < lev.vis_runs.size()) {
    m_run_nomerge.resize(lev.vis_runs.size());
  }
  // (AI-assisted) a chunk that is to be drawn (mode and distance decided): the view test (the
  // chunk's sphere against the four planes; with run_culling, a chunk that crosses planes has its
  // runs tested against those planes on their own), the visibility bits or (vista tier) the
  // source chunks, then its matrix. vista: drawn in the vista pass.
  // clip = -(R * (origin + q * scale - cam_trans)) (tfrag3.vert), as a matrix on (q, 1).
  // The translation is done in double: world coordinates are large.
  auto make_clip = [&](const c3l::Chunk& ch, float* m) {
    double d[3];
    for (int i = 0; i < 3; i++) {
      d[i] = (double)ch.origin[i] - (double)cam.trans[i];
    }
    for (int c = 0; c < 4; c++) {
      double t = 0;
      for (int i = 0; i < 3; i++) {
        m[4 * c + i] = -R[i][c] * ch.scale;
        t += (double)R[i][c] * d[i];
      }
      if (c < 3) {
        t += R[3][c];
      }
      m[4 * c + 3] = (float)-t;
    }
    for (int i = 0; i < 4; i++) {
      m[4 + i] *= kYScale;
    }
  };
  // (AI-assisted) fine_vis: the set's bits (nullptr: no set this frame)
  const u32* fine_bits = m_fine ? m_fine_bits : nullptr;
  const bool fine_check = fine_vis_check_enabled();
  // (AI-assisted) a chunk that is to be drawn (mode and distance decided): the view test (the
  // chunk's sphere against the four planes; with run_culling, a chunk that crosses planes has its
  // runs tested against those planes on their own), the visibility bits or (vista tier) the
  // source chunks, then its matrix. vista: drawn in the vista pass.
  auto emit_chunk = [&](size_t ci, bool vista_chunk) {
    m_ph_emits++;
    const auto& ch = lev.chunks[ci];
    const bool vista_tier = ch.kind == c3l::CHUNK_KIND_VISTA;
      // (AI-assisted) the view test: the chunk's sphere against the four planes; a chunk that crosses
      // planes (run_culling) has its runs tested against those planes on their own
      u32 cross = 0;  // planes the chunk's sphere crosses
      {
        // (AI-assisted, micro-level) lane by lane, the operations of planes[0] * s[0] + planes[1] *
        // s[1] + planes[2] * s[2] - planes[3] in their order (each product rounded, as the vector
        // temporaries had it; separate statements: no fused multiply-add on hosts that have one)
        // without the math::Vector4f temporaries (copied through the stack lane by lane)
        const float* s = ch.bsphere;
        float acc[4];
        for (int i = 0; i < 4; i++) {
          float a = cam.planes[0][i] * s[0];
          float b = cam.planes[1][i] * s[1];
          a = a + b;
          b = cam.planes[2][i] * s[2];
          a = a + b;
          acc[i] = a - cam.planes[3][i];
        }
        if (!(acc[0] > -s[3] && acc[1] > -s[3] && acc[2] > -s[3] && acc[3] > -s[3])) {
          return;
        }
        if (run_culling && lev.chunk_run_count[ci]) {
          for (int i = 0; i < 4; i++) {
            cross |= acc[i] < s[3] ? 1u << i : 0u;
          }
        }
      }
      const bool straddles = cross != 0;
      // (AI-assisted) fine_vis: what the camera cell's set hides of this chunk (0 nothing, 1 all,
      // 2 some units); unit by unit when its runs are its units, else all of it or nothing
      const u8 fine_hidden =
          fine_bits && ci < m_fine->hdr.num_chunks ? m_fine->chunk_hidden[ci] : 0;
      const bool fine_runs = fine_hidden && m_fine->chunk_runs[ci];
      if (fine_hidden == 1 && !fine_runs) {
        m_fine_chunks++;
        for (u32 k = 0; k < ch.draw_count; k++) {
          m_fine_tris += lev.draws[ch.first_draw + k].index_count / 3;
        }
        if (fine_check) {
          m_fine_hidden.push_back({(u32)ci, ~0u});
        }
        return;
      }
      bool partly = false;
      bool runs_valid = false;  // (AI-assisted) m_run_shown holds this chunk's runs (impostors)
      // (AI-assisted) ties always; tfrag with the level's own string (not its low resolution
      // version), or every tier with a neighbour's (neighbour_vis)
      const bool vis_test = !vista_tier && m_vis && ch.vis_run_count &&
                            (ch.lod_tier == 0 || (vis_own && ch.lod_tier < 3) ||
                             (vis_nb && ch.lod_tier <= 3));
      // (AI-assisted) c3l v17: a chunk with tie levels of detail shows one version of each group
      const bool lod_chunk = tie_lod && lev.chunk_lod[ci];
      // (AI-assisted, j2vis) c3l v20: runs of prototypes the game hides
      const bool proto_chunk = proto_hide && lev.chunk_proto_hidden[ci];
      if (vis_test || straddles || vista_tier || fine_runs || lod_chunk || proto_chunk) {
        // (AI-assisted) per run: the visibility bit, then the view test; m_run_shown for the draws
        u32 shown = 0, view_hidden = 0, view_tris = 0, fine_hid = 0, lod_hid = 0;
        const u32 r0 = lev.chunk_first_run[ci];
        // (fine_vis without run_culling: the file's runs)
        const u32 nr = vis_test || vista_tier
                           ? ch.vis_run_count
                           : (lev.chunk_run_count[ci] ? lev.chunk_run_count[ci] : ch.vis_run_count);
        const u32 fine_u0 = fine_runs ? m_fine->chunk_first_unit[ci] : 0;
        // the crossed planes: distance of the chunk's origin, and its change per quantized unit
        float po[4], pk[4][3];
        int np = 0;
        if (straddles) {
          for (int i = 0; i < 4; i++) {
            if (cross & (1u << i)) {
              po[np] = cam.planes[0][i] * ch.origin[0] + cam.planes[1][i] * ch.origin[1] +
                       cam.planes[2][i] * ch.origin[2] - cam.planes[3][i];
              for (int c = 0; c < 3; c++) {
                pk[np][c] = cam.planes[c][i] * ch.scale;
              }
              np++;
            }
          }
          m_cull_runs += nr;
        }
        // (AI-assisted, micro-level) the loop is run_loop: one copy per combination of the tests
        RunLoopIn in;
        in.runs = lev.vis_runs.data();
        in.spheres = lev.run_spheres.data();
        in.run_lod = lev.run_lod.data();
        in.lod_state = lev.lod_state.data();
        in.vis = m_vis;
        in.vista_src = m_vista_src.data();
        in.vista_src_n = m_vista_src.size();
        in.fbits = fine_bits;
        in.fine_u0 = fine_u0;
        in.vista_tier = vista_tier;
        in.fine_runs = fine_runs;
        in.fine_hidden = fine_check ? &m_fine_hidden : nullptr;
        in.ci = (u32)ci;
        in.po = po;
        in.pk = pk;
        in.np = np;
        in.scale = ch.scale;
        in.shown_out = m_run_shown.data();
        in.nomerge_out = m_run_nomerge.data();
        RunLoopOut o;
        m_ph_runs += nr;
        kRunLoops[vis_test ? 1 : 0][straddles ? np : 0][lod_chunk ? 1 : 0](in, r0, nr, &o);
        if (proto_chunk) {
          // (AI-assisted, j2vis) the hidden prototypes' runs: not drawn, and never drawn along
          // with their neighbours (the small-gap merge)
          const auto& hr = lev.proto_hidden_runs;
          for (auto it = std::lower_bound(hr.begin(), hr.end(), r0); it != hr.end() && *it < r0 + nr;
               ++it) {
            o.shown -= m_run_shown[*it];
            m_proto_tris += m_run_shown[*it] ? lev.vis_runs[*it].index_count / 3 : 0;
            m_run_shown[*it] = 0;
            m_run_nomerge[*it] = 1;
          }
        }
        shown = o.shown;
        view_hidden = o.view_hidden;
        view_tris = o.view_tris;
        fine_hid = o.fine_hid;
        lod_hid = o.lod_hid;
        m_fine_tris += o.fine_tris;
        m_lod_tris[0] += o.lod_tris[0];
        m_lod_tris[1] += o.lod_tris[1];
        m_cull_tris += view_tris;
        runs_valid = true;
        if (!shown) {
          if (fine_hid) {
            m_fine_chunks++;
          } else if (lod_hid && !view_hidden) {
            // (AI-assisted) only the other versions: not a visibility statistic
          } else if (view_hidden) {
            m_cull_chunks++;
          } else if (proto_chunk) {
            // (AI-assisted, j2vis) hidden prototypes: counted in m_proto_tris
          } else if (!vista_tier) {
            m_vis_chunks++;
          }
          return;
        }
        partly = shown < nr;
      }
      m_visible_slot[ci] = (int)m_visible.size();
      VisibleChunk& vc = m_visible.emplace_back();
      vc.chunk = (u32)ci;
      vc.partly = partly;
      vc.runs_valid = runs_valid;
      vc.vista = vista_chunk;
      vc.band = 0;
      vc.nofog = false;
      // (AI-assisted) only read with early_depth (band) or fog_stage_skip (nofog)
      if (early_depth || (fog_skip && !vista_chunk)) {
        // (AI-assisted) distance band of the chunk's nearest point, for the early depth order
        const float dx = ch.bsphere[0] - cam.trans[0], dy = ch.bsphere[1] - cam.trans[1],
                    dz = ch.bsphere[2] - cam.trans[2];
        const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        const float near = dist - ch.bsphere[3];
        vc.band = near < 30.f * 4096.f ? 0 : (near < 100.f * 4096.f ? 1 : 2);
        // (AI-assisted) fog_stage_skip: both fogs ~0 at the chunk's farthest point (its depth is at
        // most its distance; w = depth * pzw), the same formulas as the mesh shader
        if (fog_skip && !vista_chunk) {
          const float far_d = dist + ch.bsphere[3];
          const float g = 1.f + fog0[3] * std::clamp(fog0[0] - far_d * pzw, fog0[1], fog0[2]);
          const float dd = (far_d - fog1[1]) * fog1[2];
          vc.nofog = g <= kNoFog && dd <= kNoFog;
        }
      }
      make_clip(ch, vc.clip);
      ctr_gpu_prepare_mesh_matrix(vc.clip, &vc.gpu);
  };
  m_vista_src.assign(lev.has_vista ? lev.chunks.size() : 0, 0);
  // (AI-assisted) vista tier: a chunk not drawn because of its distance, within vista_distance
  auto vista_source = [&](size_t ci) {
    if (!lev.has_vista) {
      return;
    }
    const auto& ch = lev.chunks[ci];
    const float dx = ch.bsphere[0] - cam.trans[0], dy = ch.bsphere[1] - cam.trans[1],
                dz = ch.bsphere[2] - cam.trans[2];
    const float lim = vista_dist + ch.bsphere[3];
    m_vista_src[ci] = vista_dist <= 0 || dx * dx + dy * dy + dz * dz < lim * lim;
  };
  // (AI-assisted, title-popin) the cell spheres of tiers 1 / 2 (lod_cell_sphere), once per load
  if (lev.lod_cell_sphere.size() != lev.chunks.size() * 4) {
    lev.lod_cell_sphere.assign(lev.chunks.size() * 4, 0.f);
    std::map<std::tuple<float, float, float>, float> radius;
    for (const auto& ch : lev.chunks) {
      if (ch.lod_tier == 1 || ch.lod_tier == 2) {
        float d2 = 0;
        for (int c = 0; c < 3; c++) {
          d2 += (ch.bsphere[c] - ch.lod_center[c]) * (ch.bsphere[c] - ch.lod_center[c]);
        }
        float& r = radius[{ch.lod_center[0], ch.lod_center[1], ch.lod_center[2]}];
        r = std::max(r, std::sqrt(d2) + ch.bsphere[3]);
      }
    }
    for (size_t ci = 0; ci < lev.chunks.size(); ci++) {
      const auto& ch = lev.chunks[ci];
      if (ch.lod_tier == 1 || ch.lod_tier == 2) {
        float* cs = &lev.lod_cell_sphere[ci * 4];
        cs[0] = ch.lod_center[0];
        cs[1] = ch.lod_center[1];
        cs[2] = ch.lod_center[2];
        cs[3] = radius[{ch.lod_center[0], ch.lod_center[1], ch.lod_center[2]}];
      }
    }
  }
  const c3l::Chunk* const chunks = lev.chunks.data();
  u8* const chunk_state = lev.chunk_state.data();
  const int* const meshes = lev.meshes.data();
  const u32* const chunk_unit = has_units ? lev.chunk_unit.data() : nullptr;  // (AI-assisted)
  // (AI-assisted, j2-mem) dormant levels: a level that was dormant once (its flags made)
  const bool dorm = !lev.chunk_dormant.empty();
  const u32 frame32 = std::max<u32>((u32)rs.frame_idx, 1u);
  // (AI-assisted, j2cull) the chunks cluster by cluster (CtrLevelData::CullCluster). A cluster
  // beyond the distance limit of each of its members (their largest limit this frame, by kind: the
  // limits decide_dist works out), whose members were all beyond theirs last frame (state bit 2
  // clear, decided with this auto_detail position, no dormant hold; tiers 1 / 2: the coarse version
  // chosen and the cell beyond 0.9 x lod_distance) needs none of their decisions: each would come
  // out "beyond, as before" (not drawn, nothing changed) and only a vista source to mark. Its test
  // has margins for float rounding: where it can't tell, the members are decided one by one.
  auto lim_d = [&](float d) { return d > 0 ? (draw_dist > 0 ? std::min(draw_dist, d) : d) : draw_dist; };
  const u32* const hold_since = dorm ? lev.chunk_hold_since.data() : nullptr;
  u64 cl_skipped = 0;
  const bool cull_check = cull_check_enabled();
  bool cl_check = false;
  CullCheckSnap chk;
  auto cluster_beyond = [&](const CtrLevelData::CullCluster& cl, float* d2_out) {
    // the members' largest distance limit this frame
    float D = 0;
    bool none = false;  // a member without a limit: always within
    auto add = [&](float d) {
      const float m = lim_d(d);
      none = none || !(m > 0);
      D = std::max(D, m);
    };
    const u32 f = cl.flags;
    if (f & (CtrLevelData::kClHas << 0)) {
      add(cl.raw[0] * tie_k * det);
      if (f & (CtrLevelData::kClNoOwn << 0)) {
        add(0.f);
      }
    }
    if (f & (CtrLevelData::kClHas << 1)) {
      if (shrub_ps2) {
        add(cl.raw[1] * shrub_k * det);
      } else if (shrub_fixed) {
        add(shrub_fixed_dist);
      } else {
        add(cl.raw[1] * det);
      }
      if ((f & (CtrLevelData::kClNoOwn << 1)) && !(!shrub_ps2 && shrub_fixed)) {
        add(0.f);
      }
    }
    if (f & (CtrLevelData::kClHas << 2)) {
      add(cl.raw[2] * det);
      if (f & (CtrLevelData::kClNoOwn << 2)) {
        add(0.f);
      }
    }
    if (none) {
      return false;
    }
    const float dx = cl.c[0] - cx, dy = cl.c[1] - cy, dz = cl.c[2] - cz;
    const float d2 = dx * dx + dy * dy + dz * dz;
    *d2_out = d2;
    const float reach = (cl.a + D) * 1.0002f + 256.f;
    if (!(d2 > reach * reach)) {
      return false;
    }
    if ((f & CtrLevelData::kClTier12) && !far_level) {
      if (!(lod_dist > 0)) {
        return false;
      }
      const float lr = (cl.lod_a + 0.9f * lod_dist) * 1.0002f + 256.f;
      if (!(d2 > lr * lr)) {
        return false;
      }
    }
    // last frame's decisions: all beyond
    for (u32 ci = cl.first; ci < cl.first + cl.count; ci++) {
      const u8 st = chunk_state[ci];
      if ((st & 4) || chunk_q[ci] != q_now || (hold_since && hold_since[ci])) {
        return false;
      }
      if (!far_level && (chunks[ci].lod_tier == 1 || chunks[ci].lod_tier == 2) && (st & 3) != 2) {
        return false;
      }
    }
    return true;
  };
  for (const auto& cl : lev.chunk_clusters) {
    float cl_d2 = 0;
    if (!(cl.flags & CtrLevelData::kClPlain) && cluster_beyond(cl, &cl_d2)) {
      cl_skipped += cl.count;
      if (lev.has_vista) {
        // their vista sources (vista_source: within vista_distance)
        const float in_r = vista_dist * 0.9998f - 256.f - cl.a;
        const float out_r = (vista_dist + cl.a) * 1.0002f + 256.f;
        if (vista_dist <= 0 || (in_r > 0 && cl_d2 < in_r * in_r)) {
          memset(&m_vista_src[cl.first], 1, cl.count);
        } else if (!(cl_d2 > out_r * out_r)) {
          for (u32 ci = cl.first; ci < cl.first + cl.count; ci++) {
            vista_source(ci);
          }
        }
      }
      if (!cull_check) {
        continue;
      }
      // (host check: the members decided one by one all the same, nothing may change)
      cl_check = true;
      chk.snap(lev, cl.first, cl.count, m_vista_src, hold_since);
      chk.visible = m_visible.size();
      chk.emits = m_ph_emits;
    } else {
      cl_check = false;
    }
  for (size_t ci = cl.first, ci_end = cl.first + cl.count; ci < ci_end; ci++) {
    const auto& ch = chunks[ci];
    if (ch.kind == c3l::CHUNK_KIND_VISTA) {
      continue;  // (AI-assisted) after the others (their decisions pick its runs)
    }
    if (chunk_unit && chunk_unit[ci] != UINT32_MAX) {
      // (AI-assisted, streaming) a near chunk: decided before the tie LOD groups (unit_ok)
      if (lev.unit_ok[chunk_unit[ci]] == 1) {
        emit_chunk(ci, false);
      }
      continue;
    }
    // (AI-assisted) The detail and distance decisions get 10% hysteresis from last frame's, and
    // are made for every chunk every frame (off screen too): with hard cutoffs, turning the camera
    // around Jak or standing at a threshold made whole cells of objects pop in and out. The two
    // versions of a cell share lod_center and see the same decisions: exactly one is drawn.
    u8& state = chunk_state[ci];
    bool lod_ok = true;
    if (!far_level && (ch.lod_tier == 1 || ch.lod_tier == 2)) {
      // detailed version up close, coarse version further away
      const u8 last = state & 3;
      const float f = last == 1 ? 1.1f : (last == 2 ? 0.9f : 1.f);
      const float dx = ch.lod_center[0] - cx, dy = ch.lod_center[1] - cy,
                  dz = ch.lod_center[2] - cz;
      const float lim = lod_dist * f;
      const float d2 = dx * dx + dy * dy + dz * dz;
      bool near = lod_dist <= 0 || d2 < lim * lim;
      // (AI-assisted, title-popin) a cell in view moving away keeps its detailed version up to
      // kHoldInView x lod_distance (both versions test the same cell sphere: one is drawn)
      if (!near && last == 1 && sphere_in_view(cam, &lev.lod_cell_sphere[ci * 4])) {
        const float hl = lod_dist * kHoldInView;
        near = d2 < hl * hl;
      }
      state = (u8)((state & ~3) | (near ? 1 : 2));
      lod_ok = near == (ch.lod_tier == 1);
    }
    // (AI-assisted) lowres_vista: the low resolution tfrag is far scenery (vista_distance)
    const bool vista_chunk = lowres_vista && ch.lod_tier == 3;
    const bool dist_ok = decide_dist(ci, ch, state, vista_chunk);
    // (AI-assisted, j2-mem) dormant levels: a tie chunk without its mesh within its distance in
    // view stays hidden while in view once it came (at most kDormantHoldFrames from then)
    if (dorm && ch.lod_tier == 0 && !far_level) {
      u32& hold = lev.chunk_hold_since[ci];
      if (!dist_ok) {
        hold = 0;
      } else if (meshes[ci] < 0 ? lev.dormant_freed(ci) && !lev.dormant : hold != 0) {
        // (while dormant the string hides it anyway: no hold)
        const bool seen = sphere_in_view(cam, ch.bsphere);
        if (meshes[ci] < 0) {
          hold = seen ? (hold ? hold : frame32) : 0;
          m_levels->stream_check().dorm_missing += seen;
          m_late_in_view += seen;  // (AI-assisted, j2intro)
        } else if (seen && !g_screen_black && frame32 - hold < kDormantHoldFrames) {
          // (AI-assisted, j2intro: not while the screen is black: nobody saw it missing)
          continue;
        } else {
          m_levels->stream_check().dorm_switch_in_view += seen;
          hold = 0;
        }
      }
    }
    if (meshes[ci] < 0 || !dist_ok) {
      if (!dist_ok && ch.lod_tier != 3) {
        vista_source(ci);
      }
      continue;
    }
    if (far_level) {
      // only the level's low resolution version (or the coarse one), no tie but (AI-assisted) the
      // landmarks (far_chunk)
      if (!far_chunk(lev, ch)) {
        continue;
      }
      // (AI-assisted) landmarks only within the LOD distance: a neighbouring level has no
      // visibility data here, and its landmark ties are 57k-152k triangles (village2, lavatube)
      if (ch.lod_tier == 0 && lod_dist > 0) {
        const float dx = ch.bsphere[0] - cx, dy = ch.bsphere[1] - cy, dz = ch.bsphere[2] - cz;
        const float lim = lod_dist + ch.bsphere[3];
        if (dx * dx + dy * dy + dz * dz > lim * lim) {
          vista_source(ci);
          continue;
        }
      }
    } else if (ch.lod_tier == 3) {
      // (AI-assisted) the low resolution tfrag in near mode: where a neighbour's string says
      // (neighbour_vis, chunks with visibility runs), else as vista outside the detailed areas
      bool want = vis_nb && ch.vis_run_count;
      if (!want && lowres_vista) {
        u8& oc = lev.outside_cache[ci];
        if (!(oc & 1)) {
          bool all = true;
          for (const CtrLevelData* l : m_loaded) {
            all = all && outside_detail(*l, ch.bsphere, kDetailMargin);
          }
          oc |= 1 | (all ? 2 : 0);
        }
        want = oc & 2;
      }
      if (!want) {
        continue;
      }
      m_lowres_near++;
    } else if (!lod_ok) {
      continue;
    }
    // (AI-assisted) lowres_vista: in far mode too, not over another loaded level's detailed tfrag
    if (vista_chunk && far_level) {
      u8& oc = lev.outside_cache[ci];
      if (!(oc & 4)) {
        bool over = false;
        for (const CtrLevelData* l : m_loaded) {
          over = over || (l != &lev && !outside_detail(*l, ch.bsphere, kDetailMargin));
        }
        oc |= 4 | (over ? 8 : 0);
      }
      if (oc & 8) {
        continue;
      }
    }
    if (lev.has_vista && ch.lod_tier == 3) {
      // (AI-assisted) drawn through its copies in the vista tier (merged into large cells)
      m_vista_src[ci] = 1;
      continue;
    }
    emit_chunk(ci, vista_chunk);
  }
    if (cl_check) {
      cull_check_count(chk.same(lev, cl.first, cl.count, m_vista_src, hold_since) &&
                           chk.visible == m_visible.size() && chk.emits == m_ph_emits,
                       lev.name, "chunk cluster", cl.first);
    }
  }
  m_ph_skipped += cl_skipped;
  if (m_jak2_slot >= 0) {
    // (AI-assisted, j2intro) Jak II: what is missing in view (late tie LOD groups, dormant chunks
    // not back yet), for the renderer's black hold after a reveal or a cut
    m_levels->note_late_in_view(rs.frame_idx, m_late_in_view,
                                lev.shown_frame && rs.frame_idx < lev.shown_frame + 90);
  }
  // (AI-assisted) the vista tier: where its source chunks aren't drawn, up to vista_distance
  if (lev.has_vista && vista_dist > 0) {
    // (AI-assisted, j2cull) the vista chunks' list (in index order, as the walk over all chunks)
    for (const u32 ci : lev.vista_chunks) {
      const auto& ch = lev.chunks[ci];
      if (lev.meshes[ci] < 0 || !ch.vis_run_count) {
        continue;
      }
      const float dx = ch.bsphere[0] - cam.trans[0], dy = ch.bsphere[1] - cam.trans[1],
                  dz = ch.bsphere[2] - cam.trans[2];
      const float lim = vista_dist + ch.bsphere[3];
      if (dx * dx + dy * dy + dz * dz < lim * lim) {
        emit_chunk(ci, true);
      }
    }
  }
  phase(kPhChunks);
  // (AI-assisted) occ_cull: software occlusion culling (CtrOccCull.h). The opaque pieces drawn
  // this frame nearest the camera into the CPU occlusion buffer (the biggest on screen first, up to
  // occ_cull_tris triangles within occ_cull_dist), then every drawn piece (chunk, then run) behind
  // it isn't drawn. Not the vista pass's chunks (far scenery with its own fog: never hidden, never
  // an occluder).
  // (a level where it hid little lately: only every 8th frame, to see whether that changed)
  if (settings.occ_cull && settings.occ_cull_tris > 0 && !m_visible.empty() &&
      (lev.occ_ema >= kOccMinTris || (rs.frame_idx & 7) == 0)) {
    const u64 occ_tris0 = m_occ_tris;
    const double t0 = ctr_gpu_time_ms();
    static CtrOccCull s_occ;  // (render thread; one buffer per frame for all the levels drawn)
    CtrOccCull& oc = s_occ;
    if (oc.frame() != rs.frame_idx) {
      oc.begin_frame(rs.frame_idx);
    }
    oc.tris_in = oc.tris_drawn = oc.cells_written = 0;
    // the cells a level draw's occluders may write (a full screen: 6000)
    constexpr u64 kOccCellBudget = 4000;  // (a screen: 1500 cells)
    const float maxd = settings.occ_cull_dist * 4096.f;
    // an occluder draw: drawn opaque and depth written (GEQUAL), every pixel of it: no blending,
    // an alpha test that its texture's smallest alpha (the GPU's 2 x aref) and vertex alpha 0x80
    // always pass; back face culled ones (closed tie surfaces) only with their front faces, and
    // not with cull_ties = flip (then the GPU keeps the other side)
    auto occluder = [&](u32 di, bool* culled) {
      const auto& st = lev.draw_states[di];
      if (st.blend != CTR_BLEND_OFF || !st.zwrite || st.ztest != CTR_TEST_GEQUAL) {
        return false;
      }
      if (st.atest != CTR_TEST_ALWAYS) {
        const auto& d = lev.draws[di];
        const int ma = d.texture < lev.tex_min_alpha.size() ? lev.tex_min_alpha[d.texture] : 0;
        if (st.atest != CTR_TEST_GEQUAL || ma < std::min(255, 2 * (int)st.aref) ||
            di >= lev.draw_alpha80.size() || !lev.draw_alpha80[di]) {
          return false;
        }
      }
      *culled = (st.tcc & CTR_TCC_CULL_BACK) &&
                (settings.cull_ties || (st.tcc & CTR_TCC_CULL_OWN));
      return !(*culled && settings.cull_flip);
    };
    // a draw that may be hidden: depth tested
    auto hideable = [&](u32 di) {
      const auto& st = lev.draw_states[di];
      return st.ztest == CTR_TEST_GEQUAL || st.ztest == CTR_TEST_GREATER;
    };
    auto run_world = [&](const c3l::Chunk& ch, u32 r, float* c, float* rad) {
      const auto& rsph = lev.run_spheres[r];
      for (int a = 0; a < 3; a++) {
        c[a] = ch.origin[a] + rsph.c[a] * ch.scale;
      }
      *rad = rsph.r * ch.scale;
    };
    // ---- occluders ----
    m_occ_cands.clear();
    for (u32 slot = 0; slot < m_visible.size(); slot++) {
      const auto& vc = m_visible[slot];
      if (vc.vista) {
        continue;
      }
      const auto& ch = lev.chunks[vc.chunk];
      // impostors: a tie group beyond impostor_distance may be drawn as a card (an occludee,
      // never an occluder): ties occlude only nearer than that
      const float chunk_maxd = ch.kind == c3l::CHUNK_KIND_TIE && settings.impostors
                                   ? std::min(maxd, settings.impostor_distance * 4096.f)
                                   : maxd;
      const float* M = vc.clip;
      const float len0 = std::sqrt(M[0] * M[0] + M[1] * M[1] + M[2] * M[2]);
      const float len1 = std::sqrt(M[4] * M[4] + M[5] * M[5] + M[6] * M[6]);
      const float len3 = std::sqrt(M[12] * M[12] + M[13] * M[13] + M[14] * M[14]);
      // a run's sphere on screen (its quantized center and radius)
      auto run_on_screen = [&](const CtrLevelData::RunSphere& rsph) {
        float q[4];
        for (int r = 0; r < 4; r++) {
          q[r] = M[4 * r] * rsph.c[0] + M[4 * r + 1] * rsph.c[1] + M[4 * r + 2] * rsph.c[2] +
                 M[4 * r + 3];
        }
        return CtrOccCull::on_screen(q, rsph.r * len0, rsph.r * len1, rsph.r * len3);
      };
      {
        const float dx = ch.bsphere[0] - cam.trans[0], dy = ch.bsphere[1] - cam.trans[1],
                    dz = ch.bsphere[2] - cam.trans[2];
        if (std::sqrt(dx * dx + dy * dy + dz * dz) - ch.bsphere[3] > maxd) {
          continue;
        }
      }
      for (u32 k = 0; k < ch.draw_count; k++) {
        const u32 di = ch.first_draw + k;
        bool culled;
        if (!occluder(di, &culled)) {
          continue;
        }
        const auto& dr = lev.draws[di];
        const u32 nr = lev.draw_run_count[di];
        if (!nr || lev.run_spheres.empty()) {
          if (ch.kind == c3l::CHUNK_KIND_TIE) {
            continue;  // (impostors: per group)
          }
          const float dx = ch.bsphere[0] - cam.trans[0], dy = ch.bsphere[1] - cam.trans[1],
                      dz = ch.bsphere[2] - cam.trans[2];
          const float d2 = std::max(dx * dx + dy * dy + dz * dz, 1.f);
          m_occ_cands.push_back(
              {ch.bsphere[3] * ch.bsphere[3] / d2, slot, di, dr.first_index, dr.index_count});
          continue;
        }
        u32 idx = dr.first_index;
        for (u32 r = lev.draw_first_run[di]; r < lev.draw_first_run[di] + nr; r++) {
          const u32 n = lev.vis_runs[r].index_count;
          const bool shown = !vc.runs_valid || m_run_shown[r];
          if (shown && lev.run_spheres[r].r != 0xffff && run_on_screen(lev.run_spheres[r])) {
            float c[3], rad;
            run_world(ch, r, c, &rad);
            const float dx = c[0] - cam.trans[0], dy = c[1] - cam.trans[1],
                        dz = c[2] - cam.trans[2];
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (ch.kind == c3l::CHUNK_KIND_TIE ? d + rad <= chunk_maxd : d - rad <= maxd) {
              m_occ_cands.push_back({rad * rad / std::max(d * d, 1.f), slot, di, idx, n});
            }
          }
          idx += n;
        }
      }
    }
    std::sort(m_occ_cands.begin(), m_occ_cands.end(),
              [](const OccCand& a, const OccCand& b) { return a.pri > b.pri; });
    const double t_gather = ctr_gpu_time_ms();
    const u64 budget = (u64)settings.occ_cull_tris;
    int peeked_mesh = -1;
    const c3l::Vertex* verts = nullptr;
    const u16* inds = nullptr;
    int nv = 0, ni = 0;
    // the triangles of the candidates (nearest, biggest runs first) transformed, up to
    // kOccTransform of them; then the biggest on screen rasterized, up to the budget
    constexpr u32 kOccTransform = 1000;
    m_occ_tris_buf.clear();
    u32 transformed = 0;
    for (const auto& oc_c : m_occ_cands) {
      if (transformed >= kOccTransform) {
        break;
      }
      const auto& vc = m_visible[oc_c.slot];
      const auto& ch = lev.chunks[vc.chunk];
      const int mesh = lev.meshes[vc.chunk];
      if (mesh != peeked_mesh) {
        const void* vp = nullptr;
        if (!ctr_gpu_mesh_peek(mesh, &vp, &nv, &inds, &ni)) {
          continue;
        }
        verts = (const c3l::Vertex*)vp;
        peeked_mesh = mesh;
      }
      if (oc_c.first + oc_c.count > (u32)ni) {
        continue;
      }
      bool culled = false;
      occluder(oc_c.di, &culled);
      // the camera in the chunk's quantized units (front faces: counter-clockwise from outside,
      // c3l kDrawCullBack)
      const float cl[3] = {(cam.trans[0] - ch.origin[0]) / ch.scale,
                           (cam.trans[1] - ch.origin[1]) / ch.scale,
                           (cam.trans[2] - ch.origin[2]) / ch.scale};
      const float* M = vc.clip;
      for (u32 i = oc_c.first; i + 3 <= oc_c.first + oc_c.count; i += 3) {
        const u16 i0 = inds[i], i1 = inds[i + 1], i2 = inds[i + 2];
        if (i0 >= (u32)nv || i1 >= (u32)nv || i2 >= (u32)nv || transformed >= kOccTransform) {
          continue;
        }
        transformed++;
        const s16* p[3] = {verts[i0].pos, verts[i1].pos, verts[i2].pos};
        if (culled) {
          const float ax = p[1][0] - p[0][0], ay = p[1][1] - p[0][1], az = p[1][2] - p[0][2];
          const float bx = p[2][0] - p[0][0], by = p[2][1] - p[0][1], bz = p[2][2] - p[0][2];
          const float nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
          if (!(nx * (cl[0] - p[0][0]) + ny * (cl[1] - p[0][1]) + nz * (cl[2] - p[0][2]) > 0)) {
            continue;  // a back face: maybe culled
          }
        }
        float q[3][4];
        for (int k = 0; k < 3; k++) {
          const float x = p[k][0], y = p[k][1], z = p[k][2];
          for (int r = 0; r < 4; r++) {
            q[k][r] = M[4 * r] * x + M[4 * r + 1] * y + M[4 * r + 2] * z + M[4 * r + 3];
          }
        }
        // its size on screen (clip x / w, y / w; a corner behind the camera: the biggest)
        float sz = 1e30f;
        if (q[0][3] > 1e-3f && q[1][3] > 1e-3f && q[2][3] > 1e-3f) {
          const float x0 = q[0][0] / q[0][3], y0 = q[0][1] / q[0][3];
          const float x1 = q[1][0] / q[1][3], y1 = q[1][1] / q[1][3];
          const float x2 = q[2][0] / q[2][3], y2 = q[2][1] / q[2][3];
          sz = std::abs((x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0));
          if (sz < 4e-4f || std::max({x0, x1, x2}) < -1.f || std::min({x0, x1, x2}) > 1.f ||
              std::max({y0, y1, y2}) < -1.f || std::min({y0, y1, y2}) > 1.f) {
            continue;  // (smaller than ~2 cells, or off screen)
          }
        } else if (q[0][3] <= 0 && q[1][3] <= 0 && q[2][3] <= 0) {
          continue;
        }
        OccTri t;
        memcpy(t.q, q, sizeof(q));
        t.size = sz;
        m_occ_tris_buf.push_back(t);
      }
    }
    const double t_xf = ctr_gpu_time_ms();
    m_occ_phase[4] += t_xf - t_gather;
    {
      const size_t n = std::min<size_t>(budget, m_occ_tris_buf.size());
      if (n < m_occ_tris_buf.size()) {
        std::nth_element(m_occ_tris_buf.begin(), m_occ_tris_buf.begin() + n,
                         m_occ_tris_buf.end(),
                         [](const OccTri& a, const OccTri& b) { return a.size > b.size; });
      }
      m_occ_phase[5] += ctr_gpu_time_ms() - t_xf;
      for (size_t i = 0; i < n && oc.cells_written < kOccCellBudget; i++) {
        oc.add_triangle(m_occ_tris_buf[i].q[0], m_occ_tris_buf[i].q[1], m_occ_tris_buf[i].q[2]);
      }
      oc.tris_in = transformed;
    }
    const double t_raster = ctr_gpu_time_ms();
    oc.finish_occluders();
    const double t_finish = ctr_gpu_time_ms();
    m_occ_phase[0] += t_gather - t0;
    m_occ_phase[1] += t_raster - t_gather;
    m_occ_phase[2] += t_finish - t_raster;
    m_occ_tris_in += oc.tris_in;
    m_occ_tris_drawn += oc.tris_drawn;
    // ---- tests ----
    bool removed = false;
    if (lev.occ_hidden.size() != lev.vis_runs.size()) {
      lev.occ_hidden.assign(lev.vis_runs.size(), 0);
    }
    for (auto& vc : m_visible) {
      if (vc.vista) {
        continue;
      }
      const u32 ci = vc.chunk;
      const auto& ch = lev.chunks[ci];
      const float* M = vc.clip;
      const float len0 = std::sqrt(M[0] * M[0] + M[1] * M[1] + M[2] * M[2]);
      const float len1 = std::sqrt(M[4] * M[4] + M[5] * M[5] + M[6] * M[6]);
      const float len3 = std::sqrt(M[12] * M[12] + M[13] * M[13] + M[14] * M[14]);
      auto test = [&](const float* cq, float rq) {
        float q[4];
        for (int r = 0; r < 4; r++) {
          q[r] = M[4 * r] * cq[0] + M[4 * r + 1] * cq[1] + M[4 * r + 2] * cq[2] + M[4 * r + 3];
        }
        return oc.hidden(q, rq * len0, rq * len1, rq * len3);
      };
      bool all_hideable = true;
      for (u32 k = 0; k < ch.draw_count && all_hideable; k++) {
        all_hideable = hideable(ch.first_draw + k);
      }
      {
        const float cq[3] = {(ch.bsphere[0] - ch.origin[0]) / ch.scale,
                             (ch.bsphere[1] - ch.origin[1]) / ch.scale,
                             (ch.bsphere[2] - ch.origin[2]) / ch.scale};
        float q[4];
        for (int r = 0; r < 4; r++) {
          q[r] = M[4 * r] * cq[0] + M[4 * r + 1] * cq[1] + M[4 * r + 2] * cq[2] + M[4 * r + 3];
        }
        const float rq = ch.bsphere[3] / ch.scale;
        if (!oc.may_hide(q, rq * len0, rq * len1, rq * len3)) {
          continue;  // nothing of it can be hidden: no run tests
        }
        if (all_hideable && test(cq, rq)) {
          m_occ_chunks++;
          for (u32 k = 0; k < ch.draw_count; k++) {
            const u32 di = ch.first_draw + k;
            const u32 nr = lev.draw_run_count[di];
            if (vc.partly && nr) {
              for (u32 r = lev.draw_first_run[di]; r < lev.draw_first_run[di] + nr; r++) {
                m_occ_tris += m_run_shown[r] ? lev.vis_runs[r].index_count / 3 : 0;
              }
            } else {
              m_occ_tris += lev.draws[di].index_count / 3;
            }
          }
          if (fine_check) {
            if (vc.partly && vc.runs_valid) {
              // (only the runs that were to be drawn)
              const u32 r0 = lev.chunk_first_run[ci];
              const u32 rn = lev.chunk_run_count.size() > ci && lev.chunk_run_count[ci]
                                 ? lev.chunk_run_count[ci]
                                 : ch.vis_run_count;
              for (u32 q = r0; q < r0 + rn; q++) {
                if (m_run_shown[q]) {
                  m_fine_hidden.push_back({ci | kOccMark, q});
                }
              }
            } else {
              m_fine_hidden.push_back({ci | kOccMark, ~0u});
            }
          }
          m_visible_slot[ci] = -1;
          removed = true;
          continue;
        }
      }
      if (lev.run_spheres.empty()) {
        continue;
      }
      for (u32 k = 0; k < ch.draw_count; k++) {
        const u32 di = ch.first_draw + k;
        const u32 nr = lev.draw_run_count[di];
        if (!nr || !hideable(di)) {
          continue;
        }
        for (u32 r = lev.draw_first_run[di]; r < lev.draw_first_run[di] + nr; r++) {
          if ((vc.runs_valid && !m_run_shown[r]) || lev.run_spheres[r].r == 0xffff) {
            continue;
          }
          // a run seen last frame is tested every 4th frame only (staggered): one that was hidden
          // every frame (a run not tested stays drawn)
          if (lev.vis_runs[r].index_count < 3 * kOccMinRunTris) {
            continue;  // (hiding it would save less than the test costs)
          }
          u8& was = lev.occ_hidden[r];
          if (!was && ((r + rs.frame_idx) & 3) != 0) {
            continue;
          }
          was = 0;
          const auto& rsph = lev.run_spheres[r];
          const float cq[3] = {(float)rsph.c[0], (float)rsph.c[1], (float)rsph.c[2]};
          if (!test(cq, rsph.r)) {
            continue;
          }
          if (!vc.runs_valid) {
            // all of the chunk's runs were shown: their flags from here on
            const u32 r0 = lev.chunk_first_run[ci];
            const u32 rn = lev.chunk_run_count.size() > ci && lev.chunk_run_count[ci]
                               ? lev.chunk_run_count[ci]
                               : ch.vis_run_count;
            for (u32 q = r0; q < r0 + rn; q++) {
              m_run_shown[q] = 1;
            }
            vc.runs_valid = true;
          }
          m_run_shown[r] = 0;
          was = 1;
          vc.partly = true;
          m_occ_runs++;
          m_occ_tris += lev.vis_runs[r].index_count / 3;
          if (fine_check) {
            m_fine_hidden.push_back({ci | kOccMark, r});
          }
        }
      }
    }
    if (removed) {
      size_t o = 0;
      for (size_t i = 0; i < m_visible.size(); i++) {
        if (m_visible_slot[m_visible[i].chunk] >= 0) {
          if (o != i) {
            m_visible[o] = m_visible[i];
          }
          m_visible_slot[m_visible[o].chunk] = (int)o;
          o++;
        }
      }
      m_visible.resize(o);
    }
    m_occ_frames++;
    lev.occ_ema = 0.75f * lev.occ_ema + 0.25f * (float)(m_occ_tris - occ_tris0);
    const double dt = ctr_gpu_time_ms() - t0;
    m_occ_phase[3] += ctr_gpu_time_ms() - t_finish;
    m_occ_ms += dt;
    m_occ_max_ms = std::max(m_occ_max_ms, dt);
  }
  phase(kPhOcc);
  // (AI-assisted) c3l v16 time of day: the chunks to be drawn brought up to this frame's blend
  // (the groups of the runs drawn: m_run_shown when runs_valid), before any of their draws
  if (lev.tod) {
    const double t0 = ctr_gpu_time_ms();
    TodPass ps;
    ps.t0 = t0;
    ps.before_ms = g_tod_frame_ms + tod_ms;
    auto shown_of = [&](const VisibleChunk& vc) {
      return vc.runs_valid ? &m_run_shown[lev.chunk_first_run[vc.chunk]] : nullptr;
    };
    // the light pass from where the last frame's stopped, then the groups: the chunks the most
    // day moves behind first (one the camera just revealed), else from where the last frame's
    // stopped
    ps.pass = kTodLights;
    ps.over = false;
    ps.stop_chunk = UINT32_MAX;
    {
      const u32 from = lev.tod->light_round_chunk;
      for (int half = 0; half < 2; half++) {
        for (const auto& vc : m_visible) {
          if ((vc.chunk >= from) == (half == 0)) {
            tod_verts += tod_recolor(lev, vc.chunk, shown_of(vc), ps);
          }
        }
      }
      lev.tod->light_round_chunk = ps.stop_chunk == UINT32_MAX ? 0 : ps.stop_chunk;
    }
    ps.pass = kTodRound;
    ps.over = false;
    ps.stop_chunk = UINT32_MAX;
    ps.measure = rs.frame_idx % 8 == 0;
    {
      // the groups from where the last frame's budget ran out. The budget: tod_budget_ms, or more
      // while the day moves fast (the title screen's ~20x: a move nearly every frame): the chunks
      // drawn (their vertices, at the last frames' ms per vertex) once per kTodSpreadMoves day
      // moves, so that they keep within that of the game (kTodMaxLag behind: rewritten at once)
      auto& t = *lev.tod;
      u64 drawn = 0;
      for (const auto& vc : m_visible) {
        drawn += vc.chunk < t.chunk_mask.size() && t.chunk_scanned[vc.chunk] == 2
                     ? lev.chunks[vc.chunk].vertex_count
                     : 0;
      }
      ps.day_budget_ms = std::max(
          tod_budget_ms(),
          (double)drawn * t.ms_per_vertex / (kTodSpreadMoves * std::max(1.f, t.move_frames)));
      // (AI-assisted, citadel) tod_max_ms: the budget under it, and the next day move waits until
      // these vertices' rewrites fit once per move
      if (const float cap = settings.tod_max_ms; cap > 0) {
        // (the frame's group work up to 0.8 of the limit, a day move per full round at 0.7 of it:
        // the light pass's share comes out of the same ms)
        ps.day_budget_ms = std::min<double>(std::max<double>(ps.day_budget_ms, 0.8 * cap), cap);
        t.min_move_frames = std::min<float>(
            32.f, (float)((double)drawn * t.ms_per_vertex / (0.7 * cap)));
      } else {
        t.min_move_frames = 0;
      }
      const double d0 = ctr_gpu_time_ms();
      const u32 w0 = ps.work;
      const u32 from = t.round_chunk;
      for (int half = 0; half < 2; half++) {
        for (const auto& vc : m_visible) {
          if ((vc.chunk >= from) == (half == 0)) {
            tod_verts += tod_recolor(lev, vc.chunk, shown_of(vc), ps);
          }
        }
      }
      t.round_chunk = ps.stop_chunk == UINT32_MAX ? 0 : ps.stop_chunk;
      // ms per vertex of work (for the budget above)
      if (ps.work - w0 >= 2000) {
        const double mpv = (ctr_gpu_time_ms() - d0) / (ps.work - w0);
        t.ms_per_vertex = 0.875 * t.ms_per_vertex + 0.125 * std::clamp(mpv, 1e-6, 1e-3);
      }
    }
    tod_finish();
    tod_ms += ctr_gpu_time_ms() - t0;
    if (has_units && stream_check_enabled() && lev.tod->blended_valid) {
      // (AI-assisted, streaming) host check: a near chunk drawn with a group still in the file's
      // baked colors (version 0)
      auto& t = *lev.tod;
      auto& sc = m_levels->stream_check();
      for (const auto& vc : m_visible) {
        if (!lev.is_unit(vc.chunk) || t.chunk_scanned[vc.chunk] != 2) {
          continue;
        }
        sc.units_drawn++;
        const u32 g0 = t.chunk_group_first[vc.chunk], ng = t.chunk_group_count[vc.chunk];
        for (u32 g = g0; g < g0 + ng; g++) {
          if (!t.groups[g].ver || t.chunk_mesh[vc.chunk] != lev.meshes[vc.chunk]) {
            sc.units_baked++;
            break;
          }
        }
      }
    }
    m_tod_spread += ps.rewritten;
    m_tod_deferred += ps.behind;
    m_tod_capped += ps.waiting;                  // (AI-assisted, citadel) tod_max_ms
    m_tod_waited += lev.tod->day_waited ? 1 : 0;
    if (ps.measure) {
      m_tod_lag_frames++;
      m_tod_lag_max = std::max(m_tod_lag_max, ps.max_lag);
      m_tod_lag2 += ps.max_lag >= 2;
    }
  }

  phase(kPhTod);
  // (AI-assisted) impostors: far tie groups shown as cards (their runs hidden here), the new
  // pictures of them drawn now (CtrImpostor.h)
  CtrImpostors& impostors = ctr_impostors();
  {
    CtrImpostors::Camera ic;
    ic.R = R;
    ic.trans = cam.trans;
    ic.y_scale = kYScale;
    ic.fog0 = fog0;
    ic.fog1 = fog1;
    for (int c = 0; c < 3; c++) {
      ic.fog_rgb[c] = rs.fog_color[c];
    }
    impostors.begin_level(lev, ic, rs.frame_idx);
    if (impostors.active()) {
      for (auto& vc : m_visible) {
        // (AI-assisted, streaming) not the near chunks (cards show far versions; their meshes come
        // and go)
        if (!vc.vista && !lev.is_unit(vc.chunk)) {
          impostors.chunk(vc.chunk, vc.clip, vc.runs_valid, m_run_shown.data(), &vc.partly);
        }
      }
      impostors.finish_chunks();
    }
  }
  phase(kPhImp);

  // The draws whose order doesn't matter (opaque, depth written) sorted by texture and state,
  // across chunks: every texture change clears the GPU's texture cache, and the chunks share most
  // of their textures. Then the others (blending, decals) in their order, over the opaque ones.
  // The order is made at load (sorted_draws, ordered_draws): only the visible chunks' draws.
  bool vista_pass = false;  // (AI-assisted) the pass drawing: the vista chunks or the others
  u64 vista_indices = 0;    // (AI-assisted) drawn by the vista pass (statistics)
  auto draw = [&](u32 di) {
    const int slot = m_visible_slot[lev.draw_chunk[di]];
    if (slot < 0 || m_visible[slot].vista != vista_pass) {
      return;
    }
    const VisibleChunk& vc = m_visible[slot];
    const auto& dr = lev.draws[di];
#ifdef __3DS__
    // (AI-assisted, gpu-rt) static_cmdlists: the pass's draws go to the backend in one list
    auto submit = [&](u32 first, u32 count) {
      m_calls.push_back({&lev.draw_states[di], &vc.gpu, lev.meshes[vc.chunk], (int)first,
                         (int)count, vc.nofog ? 1 : 0});
    };
#else
    ctr_gpu_set_mesh_nofog(vc.nofog);  // (AI-assisted) fog_stage_skip
    auto submit = [&](u32 first, u32 count) {
      ctr_gpu_draw_mesh_prepared(&lev.draw_states[di], &vc.gpu, lev.meshes[vc.chunk], first,
                                 count);
    };
#endif
#ifndef __3DS__
    {
      // (AI-assisted) snow-gpu-audit: statistics per chunk kind and LOD tier
      static const char* kKind[4] = {"?", "tfrag", "tie", "shrub"};
      const auto& chs = lev.chunks[vc.chunk];
      ctr_gpu_soft_bucket(fmt::format("{}/{}{}", m_name.substr(0, 2), kKind[chs.kind & 3], chs.lod_tier).c_str());
    }
#endif
    if (!vc.partly || !lev.draw_run_count[di]) {
      submit(dr.first_index, dr.index_count);
      vista_indices += vista_pass ? dr.index_count : 0;
      return;
    }
    // (AI-assisted) only the visible runs; neighbouring ones in one draw, with small hidden runs
    // between them drawn along (cheaper than another draw)
    // (not in the vista tier: a hidden vista run is a copy of what the main pass draws)
    // (AI-assisted, gpuopt -> j2cull) a hidden gap of up to 300 indices (100 triangles) is drawn
    // along: on hardware a draw costs ~15-24 us (reports/logs-oct12 fit), more than 100 triangles
    // (<= 0.14 us each), so fewer, longer draws are cheaper (Haven City slums: -81 of ~890 draws
    // per frame for +4.1k of ~80k triangles; reports/gpuopt/run_gap.md). Was 48.
    const u32 kMaxGap = lev.chunks[vc.chunk].kind == c3l::CHUNK_KIND_VISTA ? 0 : 300;
    u32 idx = dr.first_index, start = 0, len = 0, gap = 0, drawn = 0;
    bool open = false;
    auto emit = [&]() {
      submit(start, len);
      drawn += len;
      vista_indices += vista_pass ? len : 0;
      open = false;
      gap = 0;
    };
    const u32 r0 = lev.draw_first_run[di];
    // (AI-assisted, micro-level) in locals: submit's stores would have them loaded again per run
    const u32 r_end = r0 + lev.draw_run_count[di];
    const c3l::VisRun* const runs = lev.vis_runs.data();
    const u8* const run_shown = m_run_shown.data();
    const u8* const run_nomerge = m_run_nomerge.data();
    for (u32 r = r0; r < r_end; r++) {
      const auto& run = runs[r];
      const bool shown = run_shown[r];  // (AI-assisted) visibility bit and view test
      if (shown) {
        if (!open) {
          open = true;
          start = idx;
          len = 0;
        } else {
          len += gap;
        }
        gap = 0;
        len += run.index_count;
      } else if (open) {
        gap += run.index_count;
        // (AI-assisted) c3l v17: the other version of a tie LOD group is never drawn along
        if (gap > kMaxGap || run_nomerge[r]) {
          emit();
        }
      }
      idx += run.index_count;
    }
    if (open) {
      emit();
    }
    m_vis_tris += (dr.index_count - drawn) / 3;
  };
  // (AI-assisted) The opaque draws that may use the early depth test first, nearest chunks first
  // (three distance bands, each sorted by texture): what's drawn near the camera covers the
  // screen early, and the GPU drops the fragments behind it before texturing them. The alpha
  // tested ones (foliage) after them: they can't use the early depth test, but behind the near
  // ones their fragments still fail the depth test before blending and writing.
  // (AI-assisted) the vista first (its own fog: a haze instead of the draw distance fog), then the
  // level's fog again for the rest (and for the renderers after it: the ocean reads it)
  u32 vista_chunks = 0;
  for (const auto& vc : m_visible) {
    vista_chunks += vc.vista;
  }
  // the main pass's draw lists: the level's, or (vista chunks) what of them the split left
  const std::vector<u32>* main_lists[3] = {&lev.early_draws, &lev.sorted_draws, &lev.ordered_draws};
  // (AI-assisted, j2cull) only the visible chunks' draws, in list order: their places marked in a
  // bit set per list (draw_list_pos), then the set bits in order (the walks below then see the
  // same draws in the same order, without the level's hidden ones)
  if (lev.draw_list_pos.size() == lev.draws.size()) {
    const u32* const lpos = lev.draw_list_pos.data();
    for (int li = 0; li < 3; li++) {
      m_list_bits[li].assign((main_lists[li]->size() + 31) / 32, 0u);
    }
    u32* const bits[3] = {m_list_bits[0].data(), m_list_bits[1].data(), m_list_bits[2].data()};
    for (const auto& vc : m_visible) {
      const auto& ch = lev.chunks[vc.chunk];
      const u32 end = std::min<u32>(ch.first_draw + ch.draw_count, (u32)lev.draws.size());
      for (u32 di = ch.first_draw; di < end; di++) {
        const u32 p = lpos[di];
        if (p != UINT32_MAX) {
          const u32 pos = p & 0x3fffffffu;
          bits[p >> 30][pos >> 5] |= 1u << (pos & 31);
        }
      }
    }
    for (int li = 0; li < 3; li++) {
      std::vector<u32>& out = m_list_vis[li];
      out.clear();
      const u32* const list = main_lists[li]->data();
      const size_t nw = m_list_bits[li].size();
      for (size_t w = 0; w < nw; w++) {
        for (u32 b = bits[li][w]; b; b &= b - 1) {
          out.push_back(list[32 * w + __builtin_ctz(b)]);
        }
      }
      if (cull_check_enabled()) {
        // (host check: the walk over the whole list keeps the same draws)
        std::vector<u32> full;
        for (u32 di : *main_lists[li]) {
          if (m_visible_slot[lev.draw_chunk[di]] >= 0) {
            full.push_back(di);
          }
        }
        cull_check_count(full == out, lev.name, "draw list", (u32)li);
      }
      main_lists[li] = &out;
    }
  }
  if (vista_chunks) {
    // (AI-assisted) one walk over the three lists instead of one per pass: the vista chunks' draws
    // (in list order: early, sorted, ordered, as the vista pass drew them) and the other visible
    // ones per list (in their order: the main pass draws them as it drew the whole lists)
    m_vista_draws.clear();
    // (AI-assisted, micro-level) in locals: push_back's stores had them loaded again per draw
    const u32* const draw_chunk = lev.draw_chunk.data();
    const int* const vslot = m_visible_slot.data();
    const VisibleChunk* const vchunks = m_visible.data();
    for (int li = 0; li < 3; li++) {
      std::vector<u32>& main = m_main_draws[li];
      main.clear();
      for (u32 di : *main_lists[li]) {
        const int slot = vslot[draw_chunk[di]];
        if (slot >= 0) {
          (vchunks[slot].vista ? m_vista_draws : main).push_back(di);
        }
      }
      main_lists[li] = &main;
    }
    ctr_gpu_set_mesh_fog(fog0, fog1_vista, rs.fog_color[0], rs.fog_color[1], rs.fog_color[2]);
    vista_pass = true;
    m_calls.clear();
    for (u32 di : m_vista_draws) {
      draw(di);
    }
    phase(kPhLists);
    m_ph_calls += m_calls.size();
    flush_calls(1);  // (AI-assisted, gpu-rt) static_cmdlists: the vista pass's
    phase(kPhFlush);
    vista_pass = false;
    ctr_gpu_set_mesh_fog(fog0, fog1, rs.fog_color[0], rs.fog_color[1], rs.fog_color[2]);
    m_vista_chunks += vista_chunks;
    m_vista_tris += vista_indices / 3;
  }
  m_vista_frames++;
  m_calls.clear();
  // (AI-assisted, micro-level) the lists hold every draw of the level: the main pass's test of a
  // draw's chunk here, inline (a call of draw for each hidden draw cost ~20 instructions)
  const u32* const draw_chunk = lev.draw_chunk.data();
  const int* const vslot = m_visible_slot.data();
  const VisibleChunk* const vchunks = m_visible.data();
  auto walk = [&](const std::vector<u32>& list) {
    m_ph_walked += list.size();
    for (u32 di : list) {
      const int slot = vslot[draw_chunk[di]];
      if (slot >= 0 && !vchunks[slot].vista) {
        draw(di);
      }
    }
  };
  if (early_depth) {
    for (u8 band = 0; band < 3; band++) {
      for (u32 di : *main_lists[0]) {
        const int slot = m_visible_slot[lev.draw_chunk[di]];
        if (slot >= 0 && m_visible[slot].band == band) {
          draw(di);
        }
      }
    }
  } else {
    walk(*main_lists[0]);
  }
  walk(*main_lists[1]);
  phase(kPhLists);
  impostors.draw_cards();  // (AI-assisted) opaque: before the blended draws
  phase(kPhCards);
  walk(*main_lists[2]);
  phase(kPhLists);
  m_ph_calls += m_calls.size();
  flush_calls(0);  // (AI-assisted, gpu-rt) static_cmdlists: the main pass's
  ctr_gpu_set_mesh_nofog(0);
  ctr_gpu_cull_reset();  // (AI-assisted) cull_ties
  phase(kPhFlush);
#ifndef __3DS__
  if (oracle_enabled()) {
    for (const auto& vc : m_visible) {
      const auto& ch = lev.chunks[vc.chunk];
      if (vc.vista) {
        continue;
      }
      for (u32 k = 0; k < ch.draw_count; k++) {
        const u32 di = ch.first_draw + k;
        const auto& dr = lev.draws[di];
        if (!vc.partly || !lev.draw_run_count[di]) {
          // the draw in runs (or pieces)
          if (lev.draw_run_count[di]) {
            u32 idx = dr.first_index;
            for (u32 r = lev.draw_first_run[di]; r < lev.draw_first_run[di] + lev.draw_run_count[di]; r++) {
              g_oracle.push_back({lev.draw_states[di], vc.gpu, lev.meshes[vc.chunk], idx,
                                  lev.vis_runs[r].index_count, ch.kind});
              idx += lev.vis_runs[r].index_count;
            }
          } else {
            g_oracle.push_back({lev.draw_states[di], vc.gpu, lev.meshes[vc.chunk], dr.first_index,
                                dr.index_count, ch.kind});
          }
          continue;
        }
        u32 idx = dr.first_index;
        for (u32 r = lev.draw_first_run[di]; r < lev.draw_first_run[di] + lev.draw_run_count[di]; r++) {
          if (m_run_shown[r]) {
            g_oracle.push_back({lev.draw_states[di], vc.gpu, lev.meshes[vc.chunk], idx,
                                lev.vis_runs[r].index_count, ch.kind});
          }
          idx += lev.vis_runs[r].index_count;
        }
      }
    }
  }
  // (AI-assisted) CTR_FINE_VIS_CHECK: what fine_vis hid, for ctr_level_fine_vis_check
  if (fine_check && !m_fine_hidden.empty()) {
    u32 last = ~0u;
    ctr_mesh_matrix mat;
    for (const auto& [ci_marked, r] : m_fine_hidden) {
      const u32 ci = ci_marked & ~kOccMark;
      const bool by_occ = ci_marked & kOccMark;
      const auto& ch = lev.chunks[ci];
      if (lev.meshes[ci] < 0) {
        continue;
      }
      if (ci != last) {
        float m[16];
        make_clip(ch, m);
        ctr_gpu_prepare_mesh_matrix(m, &mat);
        last = ci;
      }
      auto add = [&](u32 di, u32 first, u32 count) {
        g_fine_check.push_back({lev.draw_states[di], mat, lev.meshes[ci], first, count, lev.name,
                                ci, r, by_occ});
      };
      if (r == ~0u) {
        for (u32 k = 0; k < ch.draw_count; k++) {
          const auto& dr = lev.draws[ch.first_draw + k];
          add(ch.first_draw + k, dr.first_index, dr.index_count);
        }
        continue;
      }
      const u32 di = ch.first_draw + lev.vis_runs[r].draw;
      u32 first = lev.draws[di].first_index;
      for (u32 q = lev.draw_first_run[di]; q < r; q++) {
        first += lev.vis_runs[q].index_count;
      }
      add(di, first, lev.vis_runs[r].index_count);
    }
  }
#endif
  if (lev.tod) {
    g_tod_frame_ms += tod_ms;
#if !defined(__3DS__) || defined(CTR_TOD_CHECK_BUILD)
    {
      std::vector<std::pair<u32, const u8*>> drawn;
      for (const auto& vc : m_visible) {
        drawn.emplace_back(vc.chunk,
                           vc.runs_valid ? &m_run_shown[lev.chunk_first_run[vc.chunk]] : nullptr);
      }
      tod_check(lev, cam, rs, drawn);  // (AI-assisted) host: env CTR_TOD_CHECK
    }
#endif
    m_tod_verts += tod_verts;
    m_tod_max = std::max<u64>(m_tod_max, tod_verts);
    m_tod_ms += tod_ms;
    m_tod_max_ms = std::max(m_tod_max_ms, tod_ms);
  }
  phase(kPhSetup);
}
