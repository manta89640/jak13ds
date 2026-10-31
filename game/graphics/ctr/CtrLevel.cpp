/*!
 * @file CtrLevel.cpp
 * (AI-assisted)
 * See CtrLevel.h. The camera math follows the PC tfrag renderer
 * (make_new_cam_mat and tfrag3.vert in game/graphics/opengl_renderer).
 */

#include "CtrLevel.h"

#include "game/graphics/ctr/CtrAutoDetail.h"
#include "game/graphics/ctr/CtrHaze.h"
#include "game/graphics/ctr/CtrKeiraRig.h"
#include "game/graphics/ctr/CtrSettings.h"
#include "game/graphics/ctr/CtrStream.h"
#include "game/graphics/ctr/CtrVram.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <new>
#include <stdexcept>
#include <tuple>
#include <unordered_set>

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
#endif

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

#ifndef __3DS__
// (AI-assisted, perfv2) host diagnostic (ctr_gpu_soft.cpp): mesh handle -> what it is
std::map<int, std::string>& ctr_soft_mesh_desc();
#endif

namespace {
void tod_drop_entries(CtrLevelData::Tod& t, size_t ci);
// (AI-assisted, title3) host diagnostic, env CTR_POP_LOG: per level draw, what changed on screen
// (chunks appearing / going by distance in view, tie LOD switches in view, groups late or held
// far, time of day rewrites, units taken): which mechanism a visible change is
#ifndef __3DS__
bool pop_log_on() {
  static const bool on = getenv("CTR_POP_LOG") != nullptr;
  return on;
}
#elif defined(OG3DS_POP_LOG)
// (AI-assisted, titlemove) a 3DS measurement build (cmake -DOG3DS_POP_LOG=ON): the same lines every
// frame in gk.log
constexpr bool pop_log_on() {
  return true;
}
#else
constexpr bool pop_log_on() {
  return false;
}
#endif
struct PopLog {
  u32 chunk_in[4] = {}, chunk_out[4] = {};  // by chunk kind
  u32 lod_up = 0, lod_down = 0;
  u32 taken = 0;
};
PopLog g_pop;
// (AI-assisted, titlemove) the title screen's least tie LOD switch distance, meters
// (CtrTfragRenderer::draw_level). Host, the flight's first ~8 s: switches in view 80 -> 19 (all at
// 102-113 m; the island's groups are near from the reveal on), the flight's later parts +3-10k
// triangles; 130 m: 7 switches, but +14-34k triangles later (Azahar: 201k triangles, 28 ms render
// thread build where the gameplay distances drew 138k / 19.5 ms)
constexpr float kTitleLodMin = 100.f;
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
// (AI-assisted, streammem) the loader's aging while levels are drawn: a job that isn't a stream job
// goes before the urgent stream jobs once it has waited kAgeMs (a prefetch kPrefetchAgeMs), a
// detail reload / repair / read back also after kStreamStreak stream jobs in a row. In fast motion
// nearly every stream job is urgent: those jobs waited until the camera stopped (far mode for good
// on the zoomer).
constexpr double kAgeMs = 250.0, kPrefetchAgeMs = 1500.0;
constexpr u32 kStreamStreak = 2;
// (AI-assisted, streammem) a detail reload reads at most this much per job (the tfrag first, then
// the nearest chunks): what it read is kept and the next job asked for at once. Lava tube's
// non-near chunks are 10.9 MB: one job of all of them failed for want of room and read it all again
// 10 s later, far mode meanwhile.
constexpr u64 kDetailJobBytes = 2u * 1024 * 1024;

// (AI-assisted) what far mode draws (a level seen from a neighbouring one): its low resolution
// tfrag (or the coarse one) and, in c3l files with chunk kinds, its ties without a distance limit
// (landmarks: houses, towers, rocks), which popped in at far_level_distance
bool far_chunk(const CtrLevelData& lev, const c3l::Chunk& ch) {
  return ch.lod_tier == (lev.has_lowres ? 3u : 2u) ||
         (ch.lod_tier == 0 && ch.kind == c3l::CHUNK_KIND_TIE && ch.max_dist <= 0.f) ||
         ch.kind == c3l::CHUNK_KIND_VISTA;  // (AI-assisted) v13 vista tier: far scenery
}

// what a chunk is, for the lines about parts not made
std::string chunk_section(const c3l::Chunk& ch) {
  switch (ch.kind) {
    case c3l::CHUNK_KIND_TIE:
      return ch.max_dist > 0.f ? "distance-limited ties" : "ties";
    case c3l::CHUNK_KIND_SHRUB:
      return "shrubs";
    case c3l::CHUNK_KIND_VISTA:
      return "vista";
    default:
      return fmt::format("tfrag tier {}", ch.lod_tier);
  }
}

// the linear heap's room, for those lines
std::string linear_room_text() {
  return fmt::format("linear free {} KB, largest block {} KB", ctr_gpu_linear_free() / 1024,
                     ctr_gpu_linear_largest() / 1024);
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
  if (lev.tex_missing) {
    n++;
    b += lev.tex_pool_bytes;
  }
  *bytes = b;
  return n;
}

// (AI-assisted, streammem) the tfrag chunks of tiers 1 / 2 a partial unload freed that aren't back
// yet (draw_level draws the level's tfrag by far mode's rules while there are some: a cell's tier 1
// or 2 version missing would be a hole, the far tier is always there)
u32 tfrag_missing(const CtrLevelData& lev) {
  u32 n = 0;
  for (size_t ci = 0;
       ci < lev.chunks.size() && ci < lev.meshes.size() && ci < lev.mesh_index_first.size(); ci++) {
    const auto& ch = lev.chunks[ci];
    n += (ch.lod_tier == 1 || ch.lod_tier == 2) && lev.meshes[ci] < 0 &&
         lev.mesh_index_first[ci] != UINT32_MAX && !lev.is_unit(ci) && !far_chunk(lev, ch);
  }
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
                                             lev.mesh_index_end[ci] - lev.mesh_index_first[ci]) +
                  (lev.tf_morph(ci) ? lev.normals_tail_bytes(ci) : 0);
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
  m.tex = (lev.tex_missing ? 0 : lev.tex_pool_bytes) + lev.tex_shared_bytes;
  m.heap += vec_bytes(lev.chunks) + vec_bytes(lev.draws) + vec_bytes(lev.draw_states) +
            vec_bytes(lev.draw_alpha80) + vec_bytes(lev.sorted_draws) +
            vec_bytes(lev.ordered_draws) + vec_bytes(lev.early_draws) + vec_bytes(lev.draw_chunk) +
            vec_bytes(lev.vis_runs) + vec_bytes(lev.chunk_first_run) +
            vec_bytes(lev.draw_first_run) + vec_bytes(lev.draw_run_count) +
            vec_bytes(lev.run_spheres) + vec_bytes(lev.chunk_run_count) +
            vec_bytes(lev.lod_groups) + vec_bytes(lev.run_lod) + vec_bytes(lev.chunk_lod) +
            vec_bytes(lev.lod_state) + vec_bytes(lev.lod_error) + vec_bytes(lev.tf_groups) +
            vec_bytes(lev.tf_state) + vec_bytes(lev.tf_chunks) + vec_bytes(lev.chunk_tf) +
            vec_bytes(lev.tf_colors) + vec_bytes(lev.tf_color_ver) + vec_bytes(lev.tf_color_mask) +
            vec_bytes(lev.textures) +
            vec_bytes(lev.tex_min_alpha) + vec_bytes(lev.meshes) + vec_bytes(lev.blerc_verts) +
            vec_bytes(lev.blerc_targets) + vec_bytes(lev.blerc_dests) +
            vec_bytes(lev.outside_cache) + vec_bytes(lev.mesh_index_first) +
            vec_bytes(lev.mesh_index_end) + vec_bytes(lev.chunk_state) +
            vec_bytes(lev.chunk_detail_q) + vec_bytes(lev.lod_detail_q) +
            vec_bytes(lev.lod_cell_sphere) +
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
  for (auto& [name, lev] : m_extras) {
    unload(*lev);
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
      // (AI-assisted, streammem) aging (kAgeMs): the other jobs don't wait behind the urgent stream
      // jobs for good
      const double now = ctr_gpu_time_ms();
      it = pick([&](const Job& j) {
        if (j.stream || !can_start(j)) {
          return false;
        }
        const double age = now - j.queued_ms;
        return j.parts() ? (m_stream_streak >= kStreamStreak || age >= kAgeMs)
                         : age >= (j.prefetch ? kPrefetchAgeMs : kAgeMs);
      });
    }
    if (playing && it == m_queue.end()) {
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
    m_stream_streak = job.stream ? m_stream_streak + 1 : 0;  // (AI-assisted, streammem)
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
  m_room_tries = 0;
  m_job_level_load = !job.stream && !job.parts() && !job.common;  // (AI-assisted, streammem)
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
      m_load_stream_tried = m_load_stream_ok = false;
      m_load_streams_served = 0;
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
  if (m_load_stream_ok) {
    m_load_stream_bufs.release();
    m_load_stream_ok = false;
  }
  if (m_load_streams_served) {
    lg::info("[ctr] {}: {} near chunk jobs read during its load", job.name, m_load_streams_served);
    m_load_streams_served = 0;
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

namespace {
// a stream job read during a level load: windows this big (two) and the block caches
constexpr size_t kLoadStreamWindow = 128 * 1024;
constexpr size_t kLoadStreamHeapMargin = 2u * 1024 * 1024;
}  // namespace

void CtrLevels::serve_streams_in_load() {
  if (!m_stream_urgent_queued.load(std::memory_order_relaxed) || !m_job_level_load ||
      cancelled() || !c3l_read_window() || (m_load_stream_tried && !m_load_stream_ok)) {
    return;
  }
  auto pick = [&]() {
    const u64 rf = m_render_frame.load(), df = m_drawn_frame.load();
    if (!(df && df + 3 >= rf)) {
      return m_queue.end();  // nothing drawn: the game waits for this load
    }
    auto it = std::find_if(m_queue.begin(), m_queue.end(),
                           [](const Job& j) { return j.stream && j.urgent; });
    if (it == m_queue.end()) {
      m_stream_urgent_queued = false;
    }
    return it;
  };
  if (!m_load_stream_tried) {
    {
      std::lock_guard<std::mutex> lk(m_lock);
      if (pick() == m_queue.end()) {
        return;
      }
    }
    m_load_stream_tried = true;
    size_t largest = SIZE_MAX;
#ifdef __3DS__
    CtrHeapStats st;
    ctr_heap_stats(&st);
    largest = st.largest;
#endif
    if (largest >= 2 * kLoadStreamWindow + 2 * kC3lMaxBlock + kLoadStreamHeapMargin) {
      try {
        m_load_stream_bufs.reserve(kLoadStreamWindow, kC3lMaxBlock);
        m_load_stream_ok = true;
      } catch (std::bad_alloc&) {
        m_load_stream_bufs.release();
      }
    }
    if (!m_load_stream_ok) {
      return;
    }
  }
  Job job;
  {
    std::lock_guard<std::mutex> lk(m_lock);
    auto it = pick();
    if (it == m_queue.end()) {
      return;
    }
    job = *it;
    m_queue.erase(it);
  }
  job.window = std::min(job.window, kLoadStreamWindow);
  Done d;
  d.job = job;
  m_load_stream_bufs.zstd = m_read_bufs.zstd;  // (one decoder: the load waits meanwhile)
  try {
    run_stream_job(job, &d, &m_load_stream_bufs);
  } catch (std::exception& e) {
    lg::error("[ctr] {}: loading failed: {}", job.name, e.what());
    d.result = LoadResult::FAILED;
  }
  m_read_bufs.zstd = m_load_stream_bufs.zstd;
  m_load_stream_bufs.zstd = nullptr;
  m_load_streams_served++;
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
    m_queue.back().queued_ms = ctr_gpu_time_ms();  // (AI-assisted, streammem) aging
    if (job.stream && job.urgent) {
      m_stream_urgent_queued = true;
    }
    if (job.parts() && !job.stream) {
      m_parts_room[job.key()] = job.room_bytes;  // (AI-assisted, streammem) stream_update
    }
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
  bool whole = false;
  if (room) {
    {
      std::lock_guard<std::mutex> lk(m_lock);
      whole = m_room_whole;
      m_room_whole = false;
    }
    // (AI-assisted, streammem) the queued stream jobs' and the not yet taken meshes' pieces too:
    // they kept growth segments from emptying (SM-1)
    stream_drop_pending(whole);
    stream_shrink(true, whole);  // (AI-assisted, streaming) the arena's growth back for the load
    // (AI-assisted, j2strip) Jak II: the loaded levels' chunks far beyond their draw distance
    // too (freed by distance from now on while the heap is short: dist_update)
    if (g_game_version == GameVersion::Jak2) {
      u64 want;
      {
        std::lock_guard<std::mutex> lk(m_lock);
        want = m_room_bytes;
      }
      const u64 bytes = dist_make_room(want);
      if (bytes) {
        lg::info("[ctr] making room: {} KB of chunks far beyond their draw distance freed (the "
                 "farthest first; {} KB wanted)",
                 bytes / 1024, want / 1024);
      }
    }
  }
  if (dropped || room) {
    // before the loader goes on (it may be waiting for this memory)
    ctr_gpu_free_pending_now();
  }
  if (room) {
    const u32 back = CtrStreamArena::get().release_empty(whole);  // (AI-assisted, streaming)
    if (whole) {
      lg::warn("[ctr] making room: the stream arena given back ({} KB; made again when there is "
               "room)",
               back / 1024);
    }
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

bool CtrLevels::make_room(const std::string& name, const char* what, u64 bytes) {
  // (AI-assisted, j2pop) twice per job: the first frees about what the allocation wants (Jak II's
  // distance freeing: the farthest chunks first), the second all it can (a heap in pieces)
  if (m_room_tries >= 2 || cancelled()) {
    return false;
  }
  m_room_tries++;
  const u64 want = m_room_tries == 1 ? bytes : 0;
  // (AI-assisted, streammem) a level load's second try: the stream arena's first segment too (its
  // near chunks drawn far until the arena is made again), else the level has no background
  const bool whole = m_room_tries == 2 && m_job_level_load;
  lg::warn("[ctr] {}: no memory for {}{}, making room and trying again", name, what,
           m_room_tries == 1 ? "" : (whole ? " (again, the whole stream arena)" : " (again)"));
  if (!m_thread) {
    m_room_bytes = want;
    m_room_whole = whole;
    housekeeping(true);  // the loads run on the render thread (outside of a frame)
    return true;
  }
  std::unique_lock<std::mutex> lk(m_lock);
  const u32 serial = m_room_serial;
  m_room_wanted = true;
  m_room_bytes = want;
  m_room_whole = whole;
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

// (AI-assisted, streammem) A mesh begun (ctr_gpu_mesh_begin) and not ended yet: ended as failed
// (its memory and slot back) when an exception leaves the scope first. A bad_alloc between begin
// and end (the time of day groups, the run spheres) leaked up to ~1 MB of linear memory and a slot
// per failed load (SM-6).
struct MeshBuild {
  int mesh;
  explicit MeshBuild(int m) : mesh(m) {}
  MeshBuild(const MeshBuild&) = delete;
  MeshBuild& operator=(const MeshBuild&) = delete;
  ~MeshBuild() {
    if (mesh >= 0) {
      ctr_gpu_mesh_end(mesh, 0);
    }
  }
  int end(bool ok) {
    const int m = mesh;
    mesh = -1;
    return ctr_gpu_mesh_end(m, ok ? 1 : 0);
  }
};

// A chunk's n vertices (c3l::Vertex in the file at offset) into its mesh's memory as
// ctr_gpu_mesh_split_colors keeps them: read whole as before (whole blocks decode straight into
// it, the reader's caches keep the indices' blocks), then split in place.
bool read_split_vertices(c3l::Reader& f, u32 offset, u32 n, void* dst) {
  u8* const rec = (u8*)dst;
  u8* const col = rec + ctr_gpu_mesh_color_offset((int)n);
  std::unique_ptr<u32[]> colors(new (std::nothrow) u32[std::max<u32>(n, 1)]);
  if (!colors) {
    // (no heap for the colors: a block at a time)
    constexpr u32 kBlock = 256;
    c3l::Vertex buf[kBlock];
    for (u32 i = 0; i < n; i += kBlock) {
      const u32 k = std::min(kBlock, n - i);
      if (!f.read(offset + i * (u32)sizeof(c3l::Vertex), buf, k * sizeof(c3l::Vertex))) {
        return false;
      }
      for (u32 j = 0; j < k; j++) {
        memcpy(rec + 12 * (i + j), &buf[j], 12);
        memcpy(col + 4 * (i + j), buf[j].rgba, 4);
      }
    }
    return true;
  }
  if (!f.read(offset, dst, n * sizeof(c3l::Vertex))) {
    return false;
  }
  for (u32 i = 0; i < n; i++) {
    memcpy(&colors[i], rec + 16 * i + 12, 4);
  }
  for (u32 i = 0; i < n; i++) {
    u32 w[3];
    memcpy(w, rec + 16 * i, 12);  // (all of it read before the write: the first ones overlap)
    memcpy(rec + 12 * i, w, 12);
  }
  memcpy(col, colors.get(), 4 * (size_t)n);
  return true;
}
}  // namespace

// (AI-assisted) Partial unload / repair: make meshes of a loaded level again (loader thread): chunk
// meshes, and for a repair merc model meshes and the texture pool. The job has its own copy of
// what it needs: the level may be unloaded meanwhile. The indices of all its chunks are read at
// once, then the vertices in file order: chunk by chunk, every switch between the two threw the
// reader's window away (beach: 36 MB read from the SD card for 4.7 MB of meshes, 11 s on
// hardware; this way ~3 MB). Stops at the first mesh there's no memory for.
void CtrLevels::run_parts_job(const Job& job, Done* d) {
  d->result = LoadResult::FAILED;
  // (AI-assisted, streammem) no allocation after a mesh is made: one that throws would leave it
  // in no list
  d->meshes.reserve(job.chunks.size());
  d->merc_meshes.reserve(job.merc.size());
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
    // (AI-assisted, j2pop) make_room: the meshes of the job from c on
    auto parts_left_bytes = [](const Job& j, const DetailChunk* c) {
      u64 b = 0;
      for (const DetailChunk* k = c; k != j.chunks.data() + j.chunks.size(); k++) {
        b += CtrStreamArena::mesh_bytes(k->vertex_count, k->index_end - k->index_first) +
             k->vertex_count * k->tail_k;
      }
      return b;
    };
    for (const auto& c : job.chunks) {
      if (!ok || cancelled()) {
        break;
      }
      // the vertices are read (decoded) straight into GPU memory
      void* vdst;
      u16* idst;
      const u32 count = c.index_end - c.index_first;
      const int tail = c.normals_offset ? (int)(c.vertex_count * c.tail_k) : 0;
      int mesh = ctr_gpu_mesh_begin_tail(c.vertex_count, tail, count, &vdst, &idst);
      while (mesh < 0 && make_room(job.name, "meshes", parts_left_bytes(job, &c))) {
        mesh = ctr_gpu_mesh_begin_tail(c.vertex_count, tail, count, &vdst, &idst);
      }
      if (mesh < 0 && !cancelled()) {
        lg::error("[ctr] {}: {}: {} of {} meshes not made, no linear memory ({} KB needed; {})",
                  job.name,
                  job.dist      ? "read back by distance"
                  : job.dormant ? "dormant read back"
                  : job.detail  ? "detail reload"
                                : "repair",
                  job.chunks.size() - d->meshes.size(), job.chunks.size(),
                  parts_left_bytes(job, &c) / 1024, linear_room_text());
      }
      if (mesh >= 0) {
        MeshBuild mb(mesh);  // (AI-assisted, streammem)
        ctr_gpu_mesh_split_colors(mesh);
        bool read_ok = true;
        if (chunk_indices) {
          read_ok = f.read(job.index_data_offset + c.index_first * (u32)sizeof(u16), idst,
                           count * sizeof(u16));
        } else {
          memcpy(idst, indices.data() + (c.index_first - lo), count * sizeof(u16));
        }
        read_ok = read_ok &&
                  read_split_vertices(f,
                                      job.vertex_data_offset +
                                          c.first_vertex * (u32)sizeof(c3l::Vertex),
                                      c.vertex_count, vdst);
        // its envmapped tie normals, freed and read back with it
        mesh = mb.end(read_ok && (!tail || f.read(c.normals_offset,
                                                  (u8*)vdst + c.vertex_count * 16, tail)));
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
        while (mesh < 0 && make_room(job.name, "models",
                                     (u64)m.vertex_count * sizeof(c3l::MercVertex) + count * 2u)) {
          mesh = ctr_gpu_skinned_mesh_create(verts.data(), m.vertex_count, ix, count);
        }
        if (mesh < 0 && !cancelled()) {
          lg::error("[ctr] {}: repair: merc model mesh not made, no linear memory ({} KB needed; "
                    "{})",
                    job.name, ((u64)m.vertex_count * sizeof(c3l::MercVertex) + count * 2u) / 1024,
                    linear_room_text());
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
      (!job.textures || d->tex.loaded)) {
    d->result = LoadResult::LOADED;
  }
}

void CtrLevels::delete_textures(TexLoad* t) {
  for (size_t i = 0; i < t->handles.size(); i++) {
    if (!(i < t->shared.size() && t->shared[i])) {
      ctr_gpu_tex_delete(t->handles[i]);
    }
  }
  for (int pool : t->pools) {
    ctr_gpu_pool_delete(pool);  // after its textures
  }
  release_share_groups(t->share_groups);
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
// (else deleted). (AI-assisted, streammem) Every kind keeps what it made; a detail reload's level
// counts as loaded (detail_loaded) once its last job made everything.
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
      if (c.chunk < lev->chunk_dormant.size()) {
        u8& fl = lev->chunk_dormant[c.chunk];
        if (fl == CtrLevelData::kDormReading) {
          fl = CtrLevelData::kDormFreed;
        } else if (fl == CtrLevelData::kDistReading) {
          fl = CtrLevelData::kDistFreed;  // (AI-assisted, j2strip)
        } else if (fl == CtrLevelData::kTitleReading) {
          fl = CtrLevelData::kTitleFreed;
        }
      }
    }
  }
  // (AI-assisted, j2strip) a distance read back that ran short: what it made is taken (the
  // nearest first next time), the rest read again a second later
  if (lev && d.job.dist && d.result != LoadResult::LOADED) {
    lev->dormant_retry_frame = frame + 30;
    lg::warn("[ctr] {}: {} of {} chunks read back by distance (no memory for the others yet)",
             name, d.meshes.size(), d.job.chunks.size());
  }
  // (AI-assisted, streammem) a dormant read back or a detail reload that ran short keeps what it
  // made too (it threw all of it away and read it all again: SM-1, SM-7); the rest is read by the
  // next one (collect_chunks: the chunks without a mesh), soon when this one made some, else after
  // a wait (not every frame while the memory isn't there)
  if (lev && d.job.dormant && !d.job.dist && d.result != LoadResult::LOADED) {
    lev->dormant_retry_frame = frame + (d.meshes.empty() ? 120 : 30);
    lg::warn("[ctr] {}: {} of {} dormant chunks read back (no memory for the others yet)", name,
             d.meshes.size(), d.job.chunks.size());
  }
  if (lev && d.job.detail && !d.job.dormant && d.result != LoadResult::LOADED) {
    lev->detail_retry_frame = frame + (d.meshes.empty() ? 300 : 30);
    lg::warn("[ctr] {}: {} of {} detail meshes read again (no memory for the others yet), the "
             "rest later",
             name, d.meshes.size(), d.job.chunks.size());
  }
  if (!lev) {
    delete_parts(d);
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
  if (d.job.textures && d.tex.loaded && lev->tex_missing) {
    lev->tex_pools = std::move(d.tex.pools);
    lev->tex_pool_bytes = d.tex.pool_bytes;
    lev->tex_background_bytes = d.tex.background_bytes;
    lev->tex_shared_bytes = d.tex.shared_bytes;
    lev->tex_missing = false;
    lev->textures = std::move(d.tex.handles);  // the failed load's were only borrowed ones
    lev->tex_min_alpha = std::move(d.tex.min_alpha);
    lev->tex_shared = std::move(d.tex.shared);
    lev->tex_share_groups.insert(lev->tex_share_groups.end(), d.tex.share_groups.begin(),
                                 d.tex.share_groups.end());
    d.tex = TexLoad();
    build_draw_order(lev);  // the draws' GPU states hold the texture handles
    made++;
  }
  delete_parts(d);
  if (d.job.dist) {
    // (AI-assisted, j2strip) (logged when it ran short)
  } else if (d.job.dormant) {
    lg::info("[ctr] {}: {} dormant chunks back", name, made);  // (AI-assisted, j2-mem)
  } else if (d.job.detail) {
    // (AI-assisted, streammem) jobs of kDetailJobBytes: the next one at the next draw
    if (d.result == LoadResult::LOADED && !d.job.detail_more) {
      lev->detail_loaded = true;
      lg::info("[ctr] {}: detail meshes back ({})", name, made);
    } else if (d.result == LoadResult::LOADED) {
      lev->detail_retry_frame = 0;
    }
    lev->detail_tfrag_missing = lev->detail_loaded ? 0 : tfrag_missing(*lev);
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
  const bool was_loaded = lev.detail_loaded;  // (AI-assisted, streammem) or partly read back
  int freed = 0;
  for (size_t ci = 0; ci < lev.chunks.size() && ci < lev.meshes.size(); ci++) {
    // (AI-assisted) what far mode draws stays (far_chunk)
    if (!far_chunk(lev, lev.chunks[ci]) && lev.meshes[ci] >= 0 && !lev.is_unit(ci)) {
      ctr_gpu_mesh_delete(lev.meshes[ci]);  // freed after the GPU is done with it
      lev.meshes[ci] = -1;
      if (lev.tod && ci < lev.tod->chunk_mesh.size()) {
        lev.tod->chunk_mesh[ci] = -1;  // (AI-assisted) (a mesh made later may get this one's id)
        tod_drop_entries(*lev.tod, ci);
      }
      freed++;
    }
  }
  lev.detail_loaded = false;
  lev.far_frames = 0;
  lev.detail_tfrag_missing = tfrag_missing(lev);  // (AI-assisted, streammem)
  if (was_loaded || freed) {
    bump_set_generation();  // (AI-assisted)
    lg::info("[ctr] {}: only seen from far away, {} detail meshes freed", lev.name, freed);
  }
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
// (AI-assisted, streammem) one read back: at most this much, the nearest first (the rest by the
// next one): ctyindb's 4.3 MB in one job failed part way under memory pressure and was read again
// whole every 2 s
constexpr u64 kDormJobBytes = 2u * 1024 * 1024;
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
    std::vector<std::pair<float, DetailChunk>> want;  // (AI-assisted, streammem) with its distance
    u32 freed = 0, left_out = 0;
    u64 freed_bytes = 0;
    for (size_t ci = 0; ci < lev.chunks.size() && ci < lev.meshes.size(); ci++) {
      const u8 fl = lev.chunk_dormant[ci];
      if (fl == CtrLevelData::kDormReading || fl >= CtrLevelData::kDistFreed ||
          (fl == 0 && !lev.dormant) || !dormant_frees(lev, ci)) {  // (j2strip: dist_update's)
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
          want.emplace_back(d, DetailChunk{(u32)ci, ch.first_vertex, ch.vertex_count,
                                           lev.mesh_index_first[ci], lev.mesh_index_end[ci],
                                           lev.tail_file(ci), lev.tail_k(ci)});
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
          tod_drop_entries(*lev.tod, ci);
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
      // (AI-assisted, streammem) the nearest first, at most kDormJobBytes (then in file order)
      std::sort(want.begin(), want.end(),
                [](const auto& a, const auto& b) { return a.first < b.first; });
      u64 bytes = 0;
      for (const auto& [d, c] : want) {
        const u32 b = CtrStreamArena::mesh_bytes(c.vertex_count, c.index_end - c.index_first);
        if (!job.chunks.empty() && bytes + b > kDormJobBytes) {
          break;
        }
        lev.chunk_dormant[c.chunk] = CtrLevelData::kDormReading;
        bytes += b;
        job.chunks.push_back(c);
      }
      std::sort(job.chunks.begin(), job.chunks.end(), [](const DetailChunk& a, const DetailChunk& b) {
        return a.first_vertex < b.first_vertex;
      });
      job.room_bytes = bytes;
      lev.dormant_requested = true;
      lg::info("[ctr] {}: camera near its freed ties, reading {} of {} chunks back ({} KB)", name,
               job.chunks.size(), want.size(), bytes / 1024);
      request(job);
    }
  }
}

// ---- (AI-assisted, j2strip) Distance freeing (Jak II: a level short of linear memory) ----
// Jak II loads levels in pairs that don't fit the 3DS's linear memory: the strip mine (its 670k
// shrub triangles are 13 MB of meshes, 19 MB in all) with Vin's room beside it (7 MB) left 128 KB
// free and 79 parts out, and the slums with the industrial district or the mountain had the same.
// Most of those meshes are shrubs and small ties drawn only within their distance (shrubs 35-160 m,
// ties their limit x tie_distance_scale_max), so nothing far beyond it needs them. A load whose
// meshes don't fit the free linear memory (dist_load_tight; dist_free on) leaves out the chunks
// of such kinds that are farther than kDistFreeMeters beyond that distance (with its 10%
// hysteresis, at the largest auto_detail distance) from the camera now, 2 s ahead at its speed and
// Jak; a load that runs short and makes room (housekeeping: dist_make_room) frees those of the
// loaded levels, the farthest first and about as much as it wants (all of them at its second try),
// and turns dist_free on for them. While the loaded levels leave less than kDistPlayRoom above the
// stream arena's floor (its growth counted as free), dist_update frees the chunks more than
// kDistPlayFreeMeters beyond their distance, the farthest first. A freed chunk is
// read back (a dormant job of its level, the nearest first) as soon as the camera is within
// kDistReadMeters beyond its distance. A chunk not back yet within its distance is not drawn: one
// in view stays hidden while in view once it came, as a dormant level's (draw_level, the screen
// kept black after a cut). With enough linear memory none of this happens: the same output.
namespace {
// (AI-assisted, title-popin) what is in view and moving away keeps its nearer version / stays drawn
// up to this many times its distance (with the 10% hysteresis out of view): it goes when it is out
// of view or small on screen, instead of where the camera happens to cross the distance. On the
// title camera's flight (host, Oct 17) most switches of tie versions, tfrag tiers and distance
// limits in view were of things moving away.
constexpr float kHoldInView = 1.5f;
// (AI-assisted, fable1) c3l v14 shrubs: a chunk's max_dist is its PS2 distance class, the middle of
// the prototype's fade (x + w) / 2 (shrub_ps2_dists.inc: full detail up to x, the fading version
// up to w). The PS2 shows a shrub out to w and the PC without a limit; the 3DS drew a run up to
// max_dist x shrub_scale (0.8 by default): a wall of shrubs coming in at ~0.6 of the PS2's far
// plane. Now a chunk's shrubs reach kShrubFarK x max_dist (~w: w / ((x + w) / 2) is 1.2-1.5 over
// the table) and fade out from kShrubNearK of that (~x) on, as the PS2's (emit_chunk).
#ifdef __3DS__
constexpr float kShrubFarK = 1.3f;
constexpr float kShrubNearK = 0.55f;
#else
// host tests: env CTR_SHRUB_FAR_K / CTR_SHRUB_NEAR_K (1 and 1: the Oct 24 titlemove behaviour)
static const float kShrubFarK = getenv("CTR_SHRUB_FAR_K") ? atof(getenv("CTR_SHRUB_FAR_K")) : 1.3f;
static const float kShrubNearK =
    getenv("CTR_SHRUB_NEAR_K") ? atof(getenv("CTR_SHRUB_NEAR_K")) : 0.55f;
#endif
// a shrub run's fade (CtrTfragRenderer::m_run_fade): blend FIX's 0x80 = drawn as it is
constexpr u8 kRunOpaque = 0x80;
constexpr float kDistFreeMeters = 60.f, kDistReadMeters = 30.f;
// in play (dist_update): while the loaded levels leave less than kDistPlayRoom above the stream
// arena's floor (its growth counted as free), the chunks this far beyond their distance go, the
// farthest first; checked every kDistPlayEvery frames, not within kDistPlayCutFrames of a cut
constexpr float kDistPlayFreeMeters = 90.f;
constexpr u64 kDistPlayRoom = 12u * 1024 * 1024;
constexpr u64 kDistPlayEvery = 15, kDistPlayCutFrames = 60;
constexpr float kDistLeadFrames = 60.f;  // 2 s at 30 fps (the camera's velocity is per frame)
constexpr u32 kDistJobBytes = 2u * 1024 * 1024;  // one read back: at most this much (the nearest)
constexpr u64 kDistLoadMargin = 1536 * 1024;     // a load is tight with less left than this
constexpr u64 kDistReadRoom = 512 * 1024;  // (AI-assisted, j2pop) a read back with less left frees
// a chunk's largest draw distance (game units) at any auto_detail position, with the 10%
// hysteresis (draw_level's decide_dist); 0: none (drawn at any distance: kept)
float dist_limit(const CtrLevelData& lev, const c3l::Chunk& ch) {
  const auto& s = ctr_settings();
  float d = 0;
  // (AI-assisted, levellod) as decide_dist draws them: v14 shrubs out to kShrubFarK x their class
  // (fable1), ties held in view up to kHoldInView x their distance (more than the 10% hysteresis):
  // without those, chunks still drawn were left out at load or freed by make_room
  float k = 1.1f;
  if (ch.kind == c3l::CHUNK_KIND_SHRUB) {
    if (lev.shrub_ps2_dist) {
      d = ch.max_dist * kShrubFarK * std::max(s.shrub_scale_max, s.shrub_scale) * s.detail_scale;
    } else if (s.shrub_distance > 0) {
      d = std::max(s.shrub_distance_max, s.shrub_distance) * 4096.f * s.detail_scale;
    } else {
      d = ch.max_dist * s.detail_scale;
    }
  } else if (ch.kind == c3l::CHUNK_KIND_TIE) {
    // without a draw limit (tie_distance_scale none) the farthest auto_detail one still decides
    // what a level short of memory may free
    d = ch.max_dist * std::max(s.tie_distance_scale_max, s.tie_distance_scale) * s.detail_scale;
    k = std::max(k, kHoldInView);
  }
  return d > 0 ? d * k : 0.f;
}
// a chunk distance freeing may free: a shrub or tie (not a near chunk) with a draw distance
bool dist_frees(const CtrLevelData& lev, size_t ci) {
  const auto& ch = lev.chunks[ci];
  return !lev.is_unit(ci) && ch.lod_tier == 0 &&
         (ch.kind == c3l::CHUNK_KIND_SHRUB || ch.kind == c3l::CHUNK_KIND_TIE) && ch.draw_count &&
         dist_limit(lev, ch) > 0;
}
// the squared distance from the nearest position to the chunk's sphere center
float dist_d2(const c3l::Chunk& ch, const float pos[4][3], int np) {
  float d2 = 1e30f;
  for (int i = 0; i < np; i++) {
    const float dx = pos[i][0] - ch.bsphere[0], dy = pos[i][1] - ch.bsphere[1],
                dz = pos[i][2] - ch.bsphere[2];
    d2 = std::min(d2, dx * dx + dy * dy + dz * dz);
  }
  return d2;
}
// beyond `meters` past its draw distance from every position
bool dist_beyond(const CtrLevelData& lev, const c3l::Chunk& ch, const float pos[4][3], int np,
                 float meters) {
  const float lim = dist_limit(lev, ch) + meters * 4096.f + ch.bsphere[3];
  return dist_d2(ch, pos, np) > lim * lim;
}
// (AI-assisted, j2pop) how far (game units) it is beyond `meters` past its draw distance from the
// nearest position (<= 0: not beyond)
float dist_excess(const CtrLevelData& lev, const c3l::Chunk& ch, const float pos[4][3], int np,
                  float meters) {
  const float lim = dist_limit(lev, ch) + meters * 4096.f + ch.bsphere[3];
  return std::sqrt(dist_d2(ch, pos, np)) - lim;
}
}  // namespace

int CtrLevels::dist_positions(float pos[4][3]) const {
  if (!m_cam_valid) {
    return 0;
  }
  for (int c = 0; c < 3; c++) {
    pos[0][c] = m_cam[c];
    pos[1][c] = m_cam[c] + m_cam_vel[c] * kDistLeadFrames * 0.5f;
    pos[2][c] = m_cam[c] + m_cam_vel[c] * kDistLeadFrames;
    pos[3][c] = m_target[c];
  }
  return m_target_frame + 3 >= m_cam_frame ? 4 : 3;
}

// the chunks of lev distance freeing may free that are far beyond their distance: freed (or, left
// out by an incomplete load, not repaired); returns the bytes freed
u64 CtrLevels::dist_free_far(CtrLevelData& lev,
                             const float pos[4][3],
                             int np,
                             float min_excess,
                             float meters,
                             bool log) {
  if (lev.chunk_dormant.size() != lev.chunks.size()) {
    lev.chunk_dormant.assign(lev.chunks.size(), 0);
    lev.chunk_hold_since.assign(lev.chunks.size(), 0);
  }
  if (!lev.detail_loaded || lev.repair_requested) {
    return 0;  // (far mode's partial unload, a repair in flight: theirs first)
  }
  u64 bytes = 0;
  u32 freed = 0, left_out = 0;
  for (size_t ci = 0; ci < lev.chunks.size() && ci < lev.meshes.size() &&
                      ci < lev.mesh_index_first.size();
       ci++) {
    if (lev.chunk_dormant[ci] != 0 || lev.mesh_index_first[ci] == UINT32_MAX ||
        !dist_frees(lev, ci) || !dist_beyond(lev, lev.chunks[ci], pos, np, meters) ||
        (min_excess > 0 && dist_excess(lev, lev.chunks[ci], pos, np, meters) <= min_excess)) {
      continue;
    }
    const auto& ch = lev.chunks[ci];
    if (lev.meshes[ci] >= 0) {
      bytes += CtrStreamArena::mesh_bytes(ch.vertex_count,
                                          lev.mesh_index_end[ci] - lev.mesh_index_first[ci]) +
               lev.normals_tail_bytes(ci);
      ctr_gpu_mesh_delete(lev.meshes[ci]);  // freed after the GPU is done with it
      lev.meshes[ci] = -1;
      if (lev.tod && ci < lev.tod->chunk_mesh.size()) {
        lev.tod->chunk_mesh[ci] = -1;
        tod_drop_entries(*lev.tod, ci);
      }
      freed++;
    } else {
      left_out++;
    }
    lev.chunk_dormant[ci] = CtrLevelData::kDistFreed;
  }
  if (freed || left_out) {
    bump_set_generation();
    if (lev.incomplete) {
      u32 b = 0;
      lev.incomplete = missing_parts(lev, &b) > 0;
    }
    if (log) {
      lg::info("[ctr] {}: linear memory short: {} chunks far beyond their draw distance freed ({} "
               "KB){}; linear free {} KB",
               lev.name, freed, bytes / 1024,
               left_out ? fmt::format(", {} left out by its load not repaired", left_out) : "",
               ctr_gpu_linear_free() / 1024);
    }
    m_dst.freed_chunks += freed;
    m_dst.freed_bytes += bytes;
  }
  return bytes;
}

// (AI-assisted, j2pop) a load short of memory (make_room): the loaded levels' chunks far beyond
// their distance, the farthest first, until about want bytes (+ kDistLoadMargin) are freed; want
// 0: all of them (make_room's second try in a job). The HW log of Oct 25 (a continue to the
// slums): one texture pool of 2.7 MB freed 7.7 MB, 2 MB of it around where the camera went next,
// read back over seconds in view.
u64 CtrLevels::dist_make_room(u64 want, float meters, bool log) {
  if (meters <= 0.f) {
    meters = kDistFreeMeters;
  }
  float pos[4][3];
  const int np = dist_positions(pos);
  std::vector<std::pair<float, u32>> cand;  // excess, bytes
  if (want && np) {
    for (auto& [n, lp] : m_levels) {
      const CtrLevelData& lev = *lp;
      if (!lev.detail_loaded || lev.repair_requested) {
        continue;
      }
      for (size_t ci = 0; ci < lev.chunks.size() && ci < lev.meshes.size() &&
                          ci < lev.mesh_index_first.size();
           ci++) {
        if ((ci < lev.chunk_dormant.size() && lev.chunk_dormant[ci] != 0) || lev.meshes[ci] < 0 ||
            lev.mesh_index_first[ci] == UINT32_MAX || !dist_frees(lev, ci)) {
          continue;
        }
        const auto& ch = lev.chunks[ci];
        const float e = dist_excess(lev, ch, pos, np, meters);
        if (e > 0) {
          cand.emplace_back(e, CtrStreamArena::mesh_bytes(
                                   ch.vertex_count,
                                   lev.mesh_index_end[ci] - lev.mesh_index_first[ci]));
        }
      }
    }
  }
  // the excess the freeing stops at (0: all)
  float min_excess = 0.f;
  if (want && np) {
    std::sort(cand.begin(), cand.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    u64 sum = 0;
    for (size_t k = 0; k < cand.size(); k++) {
      sum += cand[k].second;
      if (sum >= want + kDistLoadMargin) {
        // (dist_free_far frees those beyond the next one's excess)
        min_excess = k + 1 < cand.size() ? cand[k + 1].first : 0.f;
        break;
      }
    }
  }
  u64 bytes = 0;
  for (auto& [n, lp] : m_levels) {
    lp->dist_free = true;
    bytes += dist_free_far(*lp, pos, np, min_excess, meters, log);
  }
  return bytes;
}

void CtrLevels::dist_update(u64 frame, u32 free_now) {
  if (g_game_version != GameVersion::Jak2) {
    return;
  }
  float pos[4][3];
  const int np = dist_positions(pos);
  {
    std::lock_guard<std::mutex> lk(m_lock);  // (for the loader: load_file)
    memcpy(m_dist_snap, pos, sizeof(pos));
    m_dist_snap_n = np;
  }
  if (!np) {
    return;
  }
  // short of a level load's room: free by position before a load has to (read back 60 m nearer)
  if (frame >= m_dist_play_frame + kDistPlayEvery && frame > m_cut_frame + kDistPlayCutFrames) {
    m_dist_play_frame = frame;
    const u32 cap = CtrStreamArena::get().capacity(), first = ctr_stream_kb() * 1024u;
    const u64 avail = (u64)free_now + (cap > first ? cap - first : 0);
    const u64 room = (u64)ctr_settings().stream_reserve_kb * 1024u + kDistPlayRoom;
    if (avail < room) {
      m_dst.play_frees += dist_make_room(room - avail, kDistPlayFreeMeters, false) > 0;
    }
  }
  for (auto& [name, lp] : m_levels) {
    CtrLevelData& lev = *lp;
    if (!lev.dist_free) {
      continue;
    }
    if (lev.chunk_dormant.size() != lev.chunks.size() || !lev.detail_loaded ||
        lev.dormant_requested || lev.detail_requested || lev.repair_requested ||
        frame < lev.dormant_retry_frame) {
      continue;
    }
    // the freed chunks within kDistReadMeters beyond their distance, the nearest first
    std::vector<std::pair<float, size_t>> near;
    for (size_t ci = 0; ci < lev.chunks.size() && ci < lev.meshes.size(); ci++) {
      if (lev.chunk_dormant[ci] != CtrLevelData::kDistFreed) {
        continue;
      }
      const auto& ch = lev.chunks[ci];
      if (!dist_beyond(lev, ch, pos, np, kDistReadMeters)) {
        const float r = dist_limit(lev, ch) + ch.bsphere[3];
        near.emplace_back(dist_d2(ch, pos, np) - r * r, ci);
      }
    }
    if (near.empty()) {
      continue;
    }
    std::sort(near.begin(), near.end());
    Job job;
    job.name = lev.name;
    job.detail = true;
    job.dormant = true;
    job.dist = true;
    job.generation = lev.generation;
    job.path = lev.path;
    job.vertex_data_offset = lev.vertex_data_offset;
    job.index_data_offset = lev.index_data_offset;
    u64 bytes = 0;
    for (const auto& [d, ci] : near) {
      const auto& ch = lev.chunks[ci];
      const u32 b = CtrStreamArena::mesh_bytes(ch.vertex_count,
                                               lev.mesh_index_end[ci] - lev.mesh_index_first[ci]) +
                    lev.normals_tail_bytes(ci);
      if (!job.chunks.empty() && bytes + b > kDistJobBytes) {
        break;
      }
      bytes += b;
      job.chunks.push_back({(u32)ci, ch.first_vertex, ch.vertex_count, lev.mesh_index_first[ci],
                            lev.mesh_index_end[ci], lev.tail_file(ci), lev.tail_k(ci)});
      lev.chunk_dormant[ci] = CtrLevelData::kDistReading;
    }
    std::sort(job.chunks.begin(), job.chunks.end(), [](const DetailChunk& a, const DetailChunk& b) {
      return a.first_vertex < b.first_vertex;
    });
    lev.dormant_requested = true;
    job.room_bytes = bytes;  // (AI-assisted, streammem)
    // (AI-assisted, j2pop) a read back that would not fit: the farthest chunks freed first, as a
    // load makes room (not the stream arena's growth, which make_room would take back too)
    if ((u64)free_now < bytes + kDistReadRoom) {
      dist_make_room(bytes);
    }
    m_dst.read_jobs++;
    m_dst.read_chunks += job.chunks.size();
    m_dst.read_bytes += bytes;
    request(job);
  }
}

// ---- Title screen (Jak 1) ----
// The title's attract camera (the logo-cam animation, title-obs.gc) flies one fixed path over
// Sandover: a glimpse under the logo, then the end of logo-intro-2 and logo-loop, which repeats
// without a cut. kTitlePath is that path to within 1.3 m (meters, the host's CTR_TITLE_VIS_LOG).
// While the title level is wanted, a chunk no point of the path can draw is left out of the load
// (kTitleFreed), or freed if the level was there before: one beyond its largest draw distance at
// any auto_detail position, a vista copy whose sources are drawn from all of the path. The view
// isn't used: the loop turns all the way round, it would leave out < 0.1 MB more. If the camera
// leaves the path, or the title ends, every such chunk is read back, the nearest first.
namespace {
constexpr s16 kTitlePath[][3] = {
    {-133, 47, 224}, {-161, 59, 165}, {-127, 52, 295}, {-134, 52, 292}, {-142, 53, 280},
    {-163, 55, 256}, {-199, 57, 225}, {-208, 58, 209}, {-211, 58, 194}, {-209, 58, 185},
    {-205, 57, 178}, {-199, 57, 172}, {-191, 56, 168}, {-163, 50, 163}, {-146, 41, 158},
    {-135, 33, 153}, {-126, 23, 146}, {-120, 16, 139}, {-116, 11, 131}, {-115, 9, 119},
    {-121, 12, 102}, {-135, 16, 81}, {-144, 21, 70}, {-152, 32, 56}, {-154, 37, 43},
    {-151, 37, 26}, {-143, 35, 14}, {-129, 33, 7}, {-114, 30, 7}, {-103, 26, 12}, {-94, 22, 20},
    {-89, 19, 29}, {-86, 17, 42}, {-86, 16, 55}, {-90, 16, 70}, {-111, 14, 122}, {-116, 13, 136},
    {-117, 11, 146}, {-115, 10, 161}, {-99, 10, 211}, {-97, 11, 224}, {-96, 15, 236},
    {-96, 26, 253}, {-99, 39, 271}, {-106, 47, 281}, {-110, 49, 283}, {-114, 52, 282},
    {-134, 57, 273}, {-153, 59, 263}, {-188, 70, 235}, {-201, 70, 223}, {-210, 65, 206},
    {-211, 62, 196}, {-210, 58, 186}, {-203, 52, 173}, {-195, 48, 165}, {-189, 46, 160},
    {-181, 45, 158}, {-157, 43, 156}, {-139, 36, 153}, {-131, 30, 150}, {-124, 22, 144},
    {-119, 16, 137}, {-116, 12, 129}, {-115, 10, 123}, {-116, 9, 115}, {-123, 12, 97},
    {-133, 20, 78}, {-150, 33, 57}, {-154, 37, 44}, {-153, 38, 31}, {-147, 37, 19}, {-139, 36, 11},
    {-127, 34, 7}, {-113, 31, 7}, {-100, 26, 14}, {-91, 20, 25}, {-86, 17, 39}, {-86, 16, 53},
    {-90, 16, 69}, {-111, 14, 122}, {-116, 13, 135}, {-117, 11, 146}, {-115, 10, 161},
    {-99, 10, 210}, {-97, 11, 222}, {-96, 14, 232}, {-95, 22, 247}, {-97, 34, 264},
    {-102, 43, 275}, {-108, 48, 282}, {-113, 52, 282}, {-120, 54, 280}, {-135, 58, 273},
    {-149, 59, 265}, {-160, 61, 258},
};
// its polylines: [kTitlePathParts[k], kTitlePathParts[k + 1])
constexpr u32 kTitlePathParts[] = {0, 2, std::size(kTitlePath)};
// meters: farther from the path the camera has left it; what any point within kTitleMargin of
// the path can draw is kept
constexpr float kTitleOffPath = 8.f, kTitleMargin = 10.f;

// the squared distance from p (game units) to the path
float title_path_d2(const float* p) {
  constexpr float kM = 4096.f;
  float best = 1e30f;
  for (size_t k = 0; k + 1 < std::size(kTitlePathParts); k++) {
    for (u32 i = kTitlePathParts[k]; i + 1 < kTitlePathParts[k + 1]; i++) {
      float ab[3], ap[3];
      float len2 = 0, dot = 0;
      for (int c = 0; c < 3; c++) {
        ab[c] = (float)(kTitlePath[i + 1][c] - kTitlePath[i][c]) * kM;
        ap[c] = p[c] - (float)kTitlePath[i][c] * kM;
        len2 += ab[c] * ab[c];
        dot += ab[c] * ap[c];
      }
      const float t = len2 > 0 ? std::clamp(dot / len2, 0.f, 1.f) : 0.f;
      float d2 = 0;
      for (int c = 0; c < 3; c++) {
        const float d = ap[c] - t * ab[c];
        d2 += d * d;
      }
      best = std::min(best, d2);
    }
  }
  return best;
}

// the largest distance from p (game units) to a point of the path
float title_path_far(const float* p) {
  float best = 0;
  for (const auto& q : kTitlePath) {
    float d2 = 0;
    for (int c = 0; c < 3; c++) {
      const float d = p[c] - (float)q[c] * 4096.f;
      d2 += d * d;
    }
    best = std::max(best, d2);
  }
  return std::sqrt(best);
}

// whether some point of the path may draw chunk ci (draw_level's rules, the settings now, any
// auto_detail position)
bool title_needed(const CtrLevelData& lev, size_t ci) {
  constexpr float kM = 4096.f;
  const auto& s = ctr_settings();
  const auto& ch = lev.chunks[ci];
  const float margin = kTitleMargin * kM;
  const float draw = s.draw_distance * kM;
  auto within = [&](float reach) {
    if (!(reach > 0)) {
      return true;
    }
    const float lim = reach + ch.bsphere[3] + margin;
    return title_path_d2(ch.bsphere) <= lim * lim;
  };
  if (ch.kind == c3l::CHUNK_KIND_VISTA) {
    if (!within(s.vista_distance * kM * 1.1f)) {
      return false;
    }
    // a run shows where its source chunk is beyond its distance: one with a limit of its own
    // (auto_detail moves it), or one the path leaves the draw distance (far mode: the LOD
    // distance) of
    float lim = draw;
    if (s.lod_distance > 0) {
      lim = lim > 0 ? std::min(lim, s.lod_distance * kM) : s.lod_distance * kM;
    }
    const u32 r0 = lev.chunk_first_run[ci];
    for (u32 r = r0; r < r0 + ch.vis_run_count && r < lev.vis_runs.size(); r++) {
      const u32 src = lev.vis_runs[r].vis;
      if (src >= lev.chunks.size()) {
        return true;
      }
      const auto& sc = lev.chunks[src];
      if (sc.lod_tier == 3 || sc.max_dist > 0 || sc.kind == c3l::CHUNK_KIND_SHRUB ||
          (lim > 0 && title_path_far(sc.bsphere) > lim + sc.bsphere[3] - margin)) {
        return true;
      }
    }
    return false;
  }
  if (ch.lod_tier == 3) {
    return true;
  }
  // decide_dist's largest: its own limit (held in view), at most the draw distance's with 10%
  const float own = ch.kind == c3l::CHUNK_KIND_SHRUB || ch.kind == c3l::CHUNK_KIND_TIE
                        ? dist_limit(lev, ch)
                        : ch.max_dist * s.detail_scale * kHoldInView;
  float reach = draw > 0 ? draw * 1.1f : 0.f;
  if (own > 0) {
    reach = reach > 0 ? std::min(reach, own) : own;
  }
  return within(reach);
}
}  // namespace

void CtrLevels::title_update(u64 frame) {
  if (g_game_version != GameVersion::Jak1) {
    return;
  }
  const bool title = m_title_on.load(std::memory_order_relaxed);
  if (!title) {
    m_title_off_path = false;
  }
  for (auto& [name, lp] : m_levels) {
    CtrLevelData& lev = *lp;
    if (name == "title" || lev.mesh_index_first.size() != lev.chunks.size() ||
        lev.meshes.size() != lev.chunks.size()) {
      continue;
    }
    const bool drawn = m_cam_valid && lev.last_draw_frame + 2 >= frame;
    if (title && drawn && !m_title_off_path) {
      const float off = kTitleOffPath * 4096.f;
      if (title_path_d2(m_cam) > off * off) {
        m_title_off_path = true;
        lg::warn("[ctr] {}: the title's camera is off its path ({:.0f} {:.0f} {:.0f} m): what the "
                 "title left out is read back",
                 name, m_cam[0] / 4096.f, m_cam[1] / 4096.f, m_cam[2] / 4096.f);
      }
    }
    const bool keep_out = title && !m_title_off_path;
    if (!keep_out) {
      lev.title_pass = false;
    }
    // the title came with the level there: what the path can't draw freed, once
    if (keep_out && drawn && !lev.title_pass && lev.detail_loaded && !lev.repair_requested) {
      lev.title_pass = true;
      if (lev.chunk_dormant.size() != lev.chunks.size()) {
        lev.chunk_dormant.assign(lev.chunks.size(), 0);
        lev.chunk_hold_since.assign(lev.chunks.size(), 0);
      }
      u32 freed = 0;
      u64 bytes = 0;
      for (size_t ci = 0; ci < lev.chunks.size(); ci++) {
        if (lev.chunk_dormant[ci] != 0 || lev.meshes[ci] < 0 || lev.is_unit(ci) ||
            lev.mesh_index_first[ci] == UINT32_MAX || title_needed(lev, ci)) {
          continue;
        }
        const auto& ch = lev.chunks[ci];
        bytes += CtrStreamArena::mesh_bytes(ch.vertex_count,
                                            lev.mesh_index_end[ci] - lev.mesh_index_first[ci]) +
                 lev.normals_tail_bytes(ci);
        ctr_gpu_mesh_delete(lev.meshes[ci]);  // freed after the GPU is done with it
        lev.meshes[ci] = -1;
        if (lev.tod && ci < lev.tod->chunk_mesh.size()) {
          lev.tod->chunk_mesh[ci] = -1;
          tod_drop_entries(*lev.tod, ci);
        }
        lev.chunk_dormant[ci] = CtrLevelData::kTitleFreed;
        freed++;
      }
      if (freed) {
        lev.title_left += freed;
        bump_set_generation();
        lg::info("[ctr] {}: title screen: {} chunks its camera can't show freed ({} KB)", name,
                 freed, bytes / 1024);
      }
    }
    if (!lev.title_left) {
      continue;
    }
    // what is left out; read back once the title is over or off its path, the nearest first
    std::vector<std::pair<float, size_t>> back;
    u32 left = 0;
    for (size_t ci = 0; ci < lev.chunk_dormant.size(); ci++) {
      const u8 fl = lev.chunk_dormant[ci];
      left += fl == CtrLevelData::kTitleFreed || fl == CtrLevelData::kTitleReading;
      if (fl == CtrLevelData::kTitleFreed && !keep_out) {
        float d2 = 0;
        for (int c = 0; c < 3; c++) {
          const float d = lev.chunks[ci].bsphere[c] - m_cam[c];
          d2 += d * d;
        }
        back.emplace_back(d2, ci);
      }
    }
    lev.title_left = left;
    if (!left) {
      if (!keep_out) {
        // nothing of the title's left: as a level that never had it
        lg::info("[ctr] {}: the chunks the title left out are back", name);
        lev.chunk_dormant.clear();
        lev.chunk_hold_since.clear();
      }
      continue;
    }
    if (back.empty() || lev.dormant_requested || lev.detail_requested || lev.repair_requested ||
        frame < lev.dormant_retry_frame) {
      continue;
    }
    std::sort(back.begin(), back.end());
    Job job;
    job.name = lev.name;
    job.detail = true;
    job.dormant = true;
    job.dist = true;
    job.generation = lev.generation;
    job.path = lev.path;
    job.vertex_data_offset = lev.vertex_data_offset;
    job.index_data_offset = lev.index_data_offset;
    u64 bytes = 0;
    for (const auto& [d2, ci] : back) {
      const auto& ch = lev.chunks[ci];
      const u32 b = CtrStreamArena::mesh_bytes(ch.vertex_count,
                                               lev.mesh_index_end[ci] - lev.mesh_index_first[ci]) +
                    lev.normals_tail_bytes(ci);
      if (!job.chunks.empty() && bytes + b > kDetailJobBytes) {
        break;
      }
      bytes += b;
      job.chunks.push_back({(u32)ci, ch.first_vertex, ch.vertex_count, lev.mesh_index_first[ci],
                            lev.mesh_index_end[ci], lev.tail_file(ci), lev.tail_k(ci)});
      lev.chunk_dormant[ci] = CtrLevelData::kTitleReading;
    }
    std::sort(job.chunks.begin(), job.chunks.end(), [](const DetailChunk& a, const DetailChunk& b) {
      return a.first_vertex < b.first_vertex;
    });
    lev.dormant_requested = true;
    job.room_bytes = bytes;
    lg::info("[ctr] {}: {}: reading {} of the {} chunks it left out ({} KB)", name,
             title ? "the title's camera is off its path" : "the title is over", job.chunks.size(),
             left, bytes / 1024);
    request(job);
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
    // (AI-assisted, j2strip) the chunks freed by distance come back by distance (dist_update)
    if (lev.meshes[ci] < 0 && lev.mesh_index_first[ci] != UINT32_MAX && !lev.is_unit(ci) &&
        !(ci < lev.chunk_dormant.size() && lev.chunk_dormant[ci] >= CtrLevelData::kDistFreed) &&
        (!missing_only || ((lev.detail_loaded || far_chunk(lev, ch)) && !lev.dormant_freed(ci)))) {
      out->push_back({(u32)ci, ch.first_vertex, ch.vertex_count, lev.mesh_index_first[ci],
                      lev.mesh_index_end[ci], lev.tail_file(ci), lev.tail_k(ci)});
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
  std::vector<DetailChunk> all;
  collect_chunks(lev, false, &all);
  if (all.empty()) {
    lev.detail_loaded = true;
    lev.detail_tfrag_missing = 0;
    return;
  }
  // (AI-assisted, streammem) at most kDetailJobBytes per job: the tfrag of tiers 1 / 2 first (the
  // level's tfrag is drawn by far mode's rules until it is all back), then the nearest chunks
  float cam[3] = {0, 0, 0};
  camera_lead(0.f, cam);
  // (not tfrag, squared distance), index in all
  std::vector<std::pair<std::pair<int, float>, u32>> order;
  order.reserve(all.size());
  for (u32 k = 0; k < all.size(); k++) {
    const auto& ch = lev.chunks[all[k].chunk];
    const float dx = ch.bsphere[0] - cam[0], dy = ch.bsphere[1] - cam[1],
                dz = ch.bsphere[2] - cam[2];
    const int cls = (ch.lod_tier == 1 || ch.lod_tier == 2) ? 0 : 1;
    order.push_back({{cls, dx * dx + dy * dy + dz * dz}, k});
  }
  std::sort(order.begin(), order.end());
  u64 bytes = 0;
  for (const auto& [key, k] : order) {
    const auto& c = all[k];
    const u32 b = CtrStreamArena::mesh_bytes(c.vertex_count, c.index_end - c.index_first);
    if (!job.chunks.empty() && bytes + b > kDetailJobBytes) {
      break;
    }
    bytes += b;
    job.chunks.push_back(c);
  }
  job.detail_more = job.chunks.size() < all.size();
  std::sort(job.chunks.begin(), job.chunks.end(), [](const DetailChunk& a, const DetailChunk& b) {
    return a.first_vertex < b.first_vertex;
  });
  job.room_bytes = bytes;
  lev.detail_requested = true;
  lg::info("[ctr] {}: camera coming back, reading {} of {} detail meshes ({} KB)", lev.name,
           job.chunks.size(), all.size(), bytes / 1024);
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
  job.textures = lev.tex_missing;
  // (AI-assisted, streammem) its room kept free of the stream arena's growth until it is done (it
  // was dropped as soon as the repair was asked for: SM-1)
  for (const auto& c : job.chunks) {
    job.room_bytes += CtrStreamArena::mesh_bytes(c.vertex_count, c.index_end - c.index_first);
  }
  for (const auto& m : job.merc) {
    job.room_bytes += (u64)m.vertex_count * sizeof(c3l::MercVertex) +
                      (u64)(m.index_end - m.index_first) * sizeof(u16);
  }
  if (job.textures) {
    job.room_bytes += lev.tex_pool_bytes;
  }
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
    // (AI-assisted, j2strip) not the ones left for later by distance (load_file)
    failed += out->meshes[i] < 0 && out->chunks[i].draw_count > 0 && !out->is_unit(i) &&
              !out->dormant_freed(i) &&
              !(i < out->mesh_index_first.size() && out->mesh_index_first[i] == UINT32_MAX);
  }
  for (auto& m : out->merc_models) {
    failed += m.mesh < 0;
  }
  // (AI-assisted) where the time went (file reads, decompressing, the rest: building meshes and
  // textures, waiting for the core) and the linear memory left with this level in
  std::string mem;
#ifdef __3DS__
  mem = fmt::format("; linear free {} KB", ctr_gpu_linear_free() / 1024);
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
    lg::error("[ctr] {}: {} textures/meshes could not be created ({})", name, failed,
              linear_room_text());
  }
  // (AI-assisted) made again when the memory is there (process_pending_loads)
  u32 missing_bytes = 0;
  const int missing = missing_parts(*out, &missing_bytes);
  out->incomplete = missing > 0;
  if (missing) {
    lg::error("[ctr] {}: {} parts ({} KB) left out, made again when memory frees ({})", name,
              missing, missing_bytes / 1024, linear_room_text());
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
    case CTR_TEX_A8:
      for (int i = 0; i < n; i++) {
        m = std::min<int>(m, data[i]);
      }
      return m;
    case CTR_TEX_A4:
      for (int i = 0; i < n / 2; i++) {
        m = std::min({m, (data[i] & 0xf) * 17, (data[i] >> 4) * 17});
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
  std::map<std::string, std::array<u8, 32>> by_level;  // rgb weights, then the alpha weights
  std::array<u8, 32> last{};
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

// c3l v22: the game's alpha weight of palette p (64 = 1.0), as interp_time_of_day_slow reads it
void tod_alpha_weights(const CtrBackgroundCamera& cam, u8 wa[8]) {
  for (int p = 0; p < 8; p++) {
    wa[p] = (u8)(((u32)cam.itimes[p / 2][(p % 2) * 2 + 1] >> 16) & 0xff);
  }
}

// an alpha kind blended: min(128, sum(w * a) >> 6), the table's alpha byte
inline u32 tod_blend_alpha(const std::array<u8, 8>& a, const u8 wa[8]) {
  u32 s = 0;
  for (int p = 0; p < 8; p++) {
    s += (u32)wa[p] * a[p];
  }
  return std::min<u32>(128, s >> 6) << 24;
}

inline u32 tod_alpha_of(const CtrLevelData::Tod& t, u32 c) {
  return t.akind.empty() ? (t.aword.empty() ? 0 : t.aword[0]) : t.aword[t.akind[c]];
}

// (AI-assisted) render thread: the time of day work of this frame (blends and rewrites), taken out
// of auto_detail's build time (ctr_level_tod_take_ms): it is not GPU load, and counting it pulled
// the tie / shrub distances in while the title screen's day ran
double g_tod_frame_ms = 0;

// min(255, x) for x < 2^31 (USAT on ARM)
inline u32 tod_sat8(u32 x) {
#if defined(__arm__) && defined(__ARM_FEATURE_SAT)
  return (u32)__builtin_arm_usat((s32)x, 8);
#else
  return std::min<u32>(255, x);
#endif
}

// a blended color times a TodMul (shrubs' base color, the untextured vista's texture average);
// the color's alpha as it is
inline u32 tod_mul_rgb(u32 c, const c3l::TodMul& m) {
  return tod_sat8(((c & 0xff) * m.mul[0] + 64) >> 7) |
         tod_sat8((((c >> 8) & 0xff) * m.mul[1] + 64) >> 7) << 8 |
         tod_sat8((((c >> 16) & 0xff) * m.mul[2] + 64) >> 7) << 16 | (c & 0xff000000u);
}

// The palettes a blend with weights w adds (weight not 0).
void tod_make_act(const CtrLevelData::Tod& t, const u8 (*w)[3], CtrLevelData::Tod::Act* a) {
  a->n = 0;
  a->mask = 0;
  for (u32 k = 0; k < t.palette_count; k++) {
    const u8* x = w[t.palettes[k]];
    if (x[0] | x[1] | x[2]) {
      a->slot3[a->n] = (u8)(k * 3);
      memcpy(a->w[a->n], x, 3);
      const int p = t.palettes[k];
      a->mask |= 1u << p;
      a->off[p] = (u8)(k * 3);
      a->wrb[p] = x[0] | (u32)x[2] << 16;
      a->wg[p] = x[1];
      a->n++;
    }
  }
}

// One color blended: per channel min(255, sum(w * c) >> 6), as the PC (interp_time_of_day_slow);
// only the palettes it isn't black in (the others add 0)
inline u32 tod_blend_rgb(const CtrLevelData::Tod& t, u32 c, const CtrLevelData::Tod::Act& a) {
  const u8* src = t.colors.data() + (size_t)c * t.palette_count * 3;
  u32 r = 0, g = 0, b = 0;
  for (u32 m = t.color_mask[c] & a.mask; m;) {
    const u32 p = 31 - __builtin_clz(m);
    m ^= 1u << p;
    const u8* x = src + a.off[p];
    const u32 wrb = a.wrb[p];
    r += (wrb & 0xffff) * x[0];
    g += a.wg[p] * x[1];
    b += (wrb >> 16) * x[2];
  }
  return std::min<u32>(255, r >> 6) | std::min<u32>(255, g >> 6) << 8 |
         std::min<u32>(255, b >> 6) << 16;
}

// Table entry idx blended again (it was older than its palettes' last move). (AI-assisted,
// todlag) a TodMul from its color's entry (blended once for all of its TodMuls).
u32 tod_entry_slow(CtrLevelData::Tod& t, u32 idx);
inline u32 tod_entry(CtrLevelData::Tod& t, u32 idx) {
  return t.stamp[idx] >= t.lut[t.color_mask[idx]] ? t.table[idx] : tod_entry_slow(t, idx);
}
__attribute__((noinline)) u32 tod_entry_slow(CtrLevelData::Tod& t, u32 idx) {
  const u32 nc = t.color_count;
  u32 rgb;
  if (idx < nc) {
    rgb = tod_blend_rgb(t, idx, t.act) | tod_alpha_of(t, idx);
    if (rgb != t.table[idx]) {
      t.cver[idx] = t.ver;
    }
  } else {
    const auto& m = t.muls[idx - nc];
    rgb = tod_mul_rgb(tod_entry(t, m.color), m);
  }
  t.table[idx] = rgb;
  t.stamp[idx] = t.ver;
  return rgb;
}

// (AI-assisted, todlag) the colors [0, nc) not up to date blended with the N palettes of t.act,
// whose weights are the same for r, g and b and add up to at most 256: r and b in the two halves
// of one word (each sum <= 255 * 256 < 65536), g in another, the palettes' offsets and weights in
// registers: ~6 instructions per palette instead of ~14 (tod_blend_rgb), the same sums.
template <int N, bool kChange, bool kKinds>
void tod_refresh_colors(CtrLevelData::Tod& t) {
  const auto& a = t.act;
  u32 off[N], w[N];
  for (int k = 0; k < N; k++) {
    off[k] = a.slot3[k];
    w[k] = a.w[k][0];
  }
  const u32 nc = t.color_count, stride = t.palette_count * 3, ver = t.ver;
  const u8* src = t.colors.data();
  u32* const table = t.table.data();
  u32* const stamp = t.stamp.data();
  const u8* const cm = t.color_mask.data();
  const u32* const lut = t.lut;
  const u8* const ak = t.akind.data();
  const u32* const aw = t.aword.data();
  const u32 a0 = t.aword.empty() ? 0 : t.aword[0];
  u32* const cver = t.cver.data();
  for (u32 c = 0; c < nc; c++, src += stride) {
    // (the fast day: every color blended, the current ones to what they hold)
    if (kChange && stamp[c] >= lut[cm[c]]) {
      continue;
    }
    u32 rb = 0, g = 0;
#pragma GCC unroll 8
    for (int k = 0; k < N; k++) {
      const u8* x = src + off[k];
      rb += ((u32)x[0] | (u32)x[2] << 16) * w[k];
      g += (u32)x[1] * w[k];
    }
    const u32 rgba = tod_sat8((rb & 0xffff) >> 6) | tod_sat8(g >> 6) << 8 |
                     tod_sat8(rb >> 22) << 16 | (kKinds ? aw[ak[c]] : a0);
    if (kChange) {
      if (rgba != table[c]) {
        cver[c] = ver;
      }
      stamp[c] = ver;
    }
    table[c] = rgba;
  }
  if (!kChange) {
    std::fill(stamp, stamp + nc, ver);
  }
}
template <int N, bool kChange>
void tod_refresh_colors(CtrLevelData::Tod& t) {
  if (t.akind.empty()) {
    tod_refresh_colors<N, kChange, false>(t);
  } else {
    tod_refresh_colors<N, kChange, true>(t);
  }
}

// (AI-assisted, todlag) every entry brought up to date (colors first: the TodMuls use them). For a
// frame whose group rewrites touch many more vertices than the table has entries (the title's fast
// day, citadel's pulse): the rewrites then read the table without the version checks.
void tod_refresh_all(CtrLevelData::Tod& t) {
  if (t.fresh_ver == t.ver) {
    return;
  }
  t.fresh_ver = t.ver;
  const auto& a = t.act;
  bool packed = a.n >= 1 && a.n <= 8;
  u32 sum = 0;
  for (u32 k = 0; k < a.n && packed; k++) {
    packed = a.w[k][0] == a.w[k][1] && a.w[k][0] == a.w[k][2];
    sum += a.w[k][0];
  }
  packed = packed && sum <= 256;
  if (packed && t.fast_day) {
    switch (a.n) {
      case 1: tod_refresh_colors<1, false>(t); break;
      case 2: tod_refresh_colors<2, false>(t); break;
      case 3: tod_refresh_colors<3, false>(t); break;
      case 4: tod_refresh_colors<4, false>(t); break;
      case 5: tod_refresh_colors<5, false>(t); break;
      case 6: tod_refresh_colors<6, false>(t); break;
      case 7: tod_refresh_colors<7, false>(t); break;
      default: tod_refresh_colors<8, false>(t); break;
    }
  } else if (packed) {
    switch (a.n) {
      case 1: tod_refresh_colors<1, true>(t); break;
      case 2: tod_refresh_colors<2, true>(t); break;
      case 3: tod_refresh_colors<3, true>(t); break;
      case 4: tod_refresh_colors<4, true>(t); break;
      case 5: tod_refresh_colors<5, true>(t); break;
      case 6: tod_refresh_colors<6, true>(t); break;
      case 7: tod_refresh_colors<7, true>(t); break;
      default: tod_refresh_colors<8, true>(t); break;
    }
  }
  const u32 n = (u32)t.table.size(), nc = t.color_count, ver = t.ver;
  u32* const table = t.table.data();
  u32* const stamp = t.stamp.data();
  const u8* const cm = t.color_mask.data();
  const u32* const lut = t.lut;
  if (!packed) {
    for (u32 idx = 0; idx < nc; idx++) {
      if (stamp[idx] < lut[cm[idx]]) {
        tod_entry_slow(t, idx);
      }
    }
  }
  // the TodMuls from their (now current) colors, as tod_entry_slow
  const c3l::TodMul* const muls = t.muls.data();
  if (packed && t.fast_day) {
    for (u32 idx = nc; idx < n; idx++) {
      table[idx] = tod_mul_rgb(table[muls[idx - nc].color], muls[idx - nc]);
    }
    std::fill(stamp + nc, stamp + n, ver);
    return;
  }
  for (u32 idx = nc; idx < n; idx++) {
    if (stamp[idx] < lut[cm[idx]]) {
      const auto& m = muls[idx - nc];
      table[idx] = tod_mul_rgb(table[m.color], m);
      stamp[idx] = ver;
    }
  }
}

// The title's entry copies (Tod::chunk_entries): u32 count, then the entries
void tod_drop_entries(CtrLevelData::Tod& t, size_t ci) {
  if (ci < t.chunk_entries.size() && t.chunk_entries[ci]) {
    u32 n;
    memcpy(&n, t.chunk_entries[ci].get(), 4);
    t.entries_bytes -= 4 + 2 * n;
    t.chunk_entries[ci].reset();
  }
}
void tod_drop_all_entries(CtrLevelData::Tod& t) {
  t.chunk_entries.clear();
  t.chunk_entries.shrink_to_fit();
  t.entries_bytes = 0;
}

// Every level draw: the game's weights into target_w (the table's entries follow lazily, exact as
// the PC). (AI-assisted, levellod) config tod_pulse_step (default 4): a pulsing day palette's moves
// are taken by the groups bright in it at once and by the others only every tod_pulse_step 64ths
// (or once it stood still for kTodPulseSettle frames: pal_hver / hlut); a group none of whose
// colors is bright in it is then at most a color unit off. no_hold (the title screen): every
// group every move. Returns 2: day palettes taken, 1: only light or held palettes, 0: nothing.
constexpr u64 kTodPulseSettle = 8;  // (AI-assisted, citadel)
// palette p's new weights move by more than one 64th from the table's (the day steps by one)
inline bool tod_weight_jump(const CtrLevelData::Tod& t, int p, const u8 w[3], u8 wa) {
  bool jump = (t.apal_mask >> p & 1) && std::abs((int)wa - (int)t.wa[p]) > 1;
  for (int c = 0; c < 3; c++) {
    jump = jump || std::abs((int)w[c] - (int)t.target_w[p][c]) > 1;
  }
  return jump;
}
// Whether the game's weights jump in a day palette since the table's last blend (a lightning
// flash: draw_level's heavy frames in Jak II)
bool tod_day_jump(const CtrLevelData::Tod& t, const CtrBackgroundCamera& cam) {
  if (!t.blended_valid) {
    return true;
  }
  u8 w[8][3];
  tod_weights(cam, w);
  u8 wa[8];
  tod_alpha_weights(cam, wa);
  for (u32 m = t.track_mask & ~t.light_mask; m; m &= m - 1) {
    const int p = __builtin_ctz(m);
    if (tod_weight_jump(t, p, w[p], wa[p])) {
      return true;
    }
  }
  return false;
}
int tod_update(CtrLevelData::Tod& t, const std::string& level, const CtrBackgroundCamera& cam,
               u64 frame, bool no_hold) {
  u8 w[8][3];
  tod_weights(cam, w);
  u8 wa[8];
  tod_alpha_weights(cam, wa);
  // (AI-assisted, citadel) config tod_pulse_step: a day palette that pulses (CtrTodPulse.h: a mood
  // light's weight going up and down around a steady mean within ~6 s; citadel's palette 1, 1/64
  // every ~2 frames, 71% of its colors). A palette found pulsing is remembered per level
  // (TodSeen::pulse): a reload holds it at once, until it stops turning for CtrTodPulse::kKeep
  // frames.
  const int pulse_step = no_hold ? 0 : ctr_settings().tod_pulse_step;
  if (t.fast_day && !no_hold) {
    // (the colors' changes weren't kept: every group's colors may have changed)
    std::fill(t.cver.begin(), t.cver.end(), t.ver);
    tod_drop_all_entries(t);
  }
  t.fast_day = no_hold;
  if (!t.first_frame) {
    t.first_frame = frame;
  }
  auto pulsing = [&](int p) {
    return t.pulse[p].pulsing(frame) ||
           ((t.pulse_known >> p & 1) &&
            frame - std::max(t.pulse[p].last_turn(), t.first_frame) < CtrTodPulse::kKeep);
  };
  if (t.blended_valid) {
    for (u32 k = 0; k < t.palette_count; k++) {
      const int p = t.palettes[k];
      if ((t.light_mask >> p & 1) || memcmp(t.weights[p], w[p], 3) == 0) {
        continue;
      }
      // (the title's weights are watched too, for the log line, but not remembered)
      if (t.pulse[p].move(w[p], frame) && !(t.pulse_logged >> p & 1)) {
        t.pulse_logged |= (u8)(1u << p);
        lg::info("[ctr] {}: time of day palette {} pulses (frame {}{})", level, p, frame,
                 no_hold ? ", title: not held" : "");
        if (!no_hold) {
          t.pulse_known |= (u8)(1u << p);
          auto& seen = tod_seen();
          std::lock_guard<std::mutex> lk(seen.lock);
          seen.pulse[level] |= (u8)(1u << p);
        }
      }
      t.pulse_move[p] = frame;
    }
  }
  u32 changed = 0, taken = 0, day = 0;
  bool day_jump = !t.blended_valid;
  for (u32 m = t.track_mask; m; m &= m - 1) {
    const int p = __builtin_ctz(m);
    const bool a_moved = (t.apal_mask >> p & 1) && t.wa[p] != wa[p];
    if (t.blended_valid && !a_moved && memcmp(t.target_w[p], w[p], 3) == 0) {
      continue;
    }
    changed |= 1u << p;
    if (!(t.light_mask >> p & 1) && tod_weight_jump(t, p, w[p], wa[p])) {
      day_jump = true;
    }
    if (pulse_step > 0 && t.blended_valid && !(t.light_mask >> p & 1) && pulsing(p) &&
        frame - t.pulse_move[p] < kTodPulseSettle) {
      int diff = (t.apal_mask >> p & 1) ? std::abs((int)wa[p] - (int)t.held_wa[p]) : 0;
      for (int c = 0; c < 3; c++) {
        diff = std::max(diff, std::abs((int)w[p][c] - (int)t.held_w[p][c]));
      }
      if (diff < pulse_step) {
        t.pulse_held++;
        continue;  // (held: only the groups bright in it follow)
      }
    }
    taken |= 1u << p;
    memcpy(t.held_w[p], w[p], 3);
    t.held_wa[p] = wa[p];
    if (!(t.light_mask >> p & 1)) {
      day |= 1u << p;
    }
  }
  memcpy(t.weights, w, sizeof(w));
  // the alpha kinds: a kind outside its colors' masks that blends to another alpha makes every
  // entry blend again
  if (!t.akinds.empty() && (!t.blended_valid || (changed & t.apal_mask))) {
    bool all = false;
    for (size_t k = 0; k < t.akinds.size(); k++) {
      const u32 a = tod_blend_alpha(t.akinds[k], wa);
      all = all || (t.akind_full[k] && a != t.aword[k]);
      t.aword[k] = a;
    }
    if (all && t.blended_valid) {
      changed |= t.track_mask;
      taken |= t.track_mask;
      day |= t.track_mask & ~t.light_mask;
      for (u32 m = t.track_mask; m; m &= m - 1) {
        const int p = __builtin_ctz(m);
        memcpy(t.held_w[p], w[p], 3);
        t.held_wa[p] = wa[p];
      }
    }
  }
  memcpy(t.wa, wa, sizeof(wa));
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
  for (u32 c = taken; c; c &= c - 1) {
    t.pal_hver[__builtin_ctz(c)] = t.ver;
  }
  t.lut[0] = 0;
  t.hlut[0] = 0;
  for (u32 m = 1; m < 256; m++) {
    t.lut[m] = std::max(t.lut[m & (m - 1)], t.pal_ver[__builtin_ctz(m)]);
    t.hlut[m] = std::max(t.hlut[m & (m - 1)], t.pal_hver[__builtin_ctz(m)]);
  }
  tod_make_act(t, t.target_w, &t.act);
  if (day) {
    t.dver = (t.dver + 1) & 0xffffffu;
    // (AI-assisted, perfv2) the spread of its rewrites (TodPass): a step of the day only; a jump
    // (lightning) is drawn at once
    t.day_move_frame = day_jump ? 0 : frame;
    auto& seen = tod_seen();
    std::lock_guard<std::mutex> lk(seen.lock);
    auto& wl = seen.by_level[level];
    memcpy(wl.data(), t.target_w, 24);
    memcpy(wl.data() + 24, t.wa, 8);
    seen.last = wl;
    seen.last_day_mask = tod_day_mask(t);
    return 2;
  }
  return 1;
}

// may_alias words of a vertex color (u8 rgba[4], little endian)
typedef u32 __attribute__((may_alias)) tod_word;
typedef u16 __attribute__((may_alias)) tod_half;
static_assert(offsetof(c3l::Vertex, pad) == 6 && offsetof(c3l::Vertex, rgba) == 12);
static_assert(sizeof(c3l::Vertex) == 16);

// A level mesh's vertices as ctr_gpu_mesh_split_colors keeps them: per vertex a 12-byte record
// (c3l::Vertex without its color: the time of day entry in pad), then the colors on their own, so
// a color rewrite touches 4 bytes per vertex in FCRAM instead of the vertex's 16.
constexpr int kTodRec = 12;
struct TodVerts {
  u8* rec = nullptr;
  tod_word* rgba = nullptr;
  u32 entry(int i) const { return *(const tod_half*)(rec + kTodRec * i + 6); }
  explicit operator bool() const { return rec != nullptr; }
};
// a chunk mesh's vertices in place (none when it has no colors of its own)
inline TodVerts tod_verts(int mesh, int* n) {
  TodVerts v;
  void* rec = ctr_gpu_mesh_vertices_inplace(mesh, n);
  void* col = rec ? ctr_gpu_mesh_colors_inplace(mesh) : nullptr;
  if (col && *n > 0) {
    v.rec = (u8*)rec;
    v.rgba = (tod_word*)col;
  }
  return v;
}

// (AI-assisted, perfv2) the vertex rewrites run through level meshes in FCRAM (~200 ns a missed
// cache line on hardware: the loops wait on memory, Azahar can't show it): the lines this many
// bytes ahead are asked for early (PLD, as ctr_mem.S's copies)
constexpr int kTodPrefetch = 128;
// f(entry, color) for vertices [first, end), 8 at a time: three lines of records and one of colors
template <typename F>
inline void tod_each(const TodVerts& v, int first, int end, F&& f) {
  const u8* p = v.rec + kTodRec * first;
  tod_word* c = v.rgba + first;
  int n = end - first;
  for (; n >= 8; n -= 8, p += 8 * kTodRec, c += 8) {
    __builtin_prefetch(p + kTodPrefetch, 0);
    __builtin_prefetch(p + kTodPrefetch + 32, 0);
    __builtin_prefetch(p + kTodPrefetch + 64, 0);
    __builtin_prefetch(c + kTodPrefetch / 4, 1);
#pragma GCC unroll 8
    for (int j = 0; j < 8; j++) {
      f(*(const tod_half*)(p + kTodRec * j + 6), c + j);
    }
  }
  for (int j = 0; j < n; j++) {
    f(*(const tod_half*)(p + kTodRec * j + 6), c + j);
  }
}
// A color is stored only where it differs: a line whose vertices keep their colors stays clean (no
// write-back to FCRAM; a lightning flash changes about half of the vertices it touches).
inline void tod_store(tod_word* x, u32 c) {
  if (*x != c) {
    *x = c;
  }
}
inline void tod_store_rgb(u8* x, u32 c) {
  if ((*(const tod_word*)x & 0xffffffu) != (c & 0xffffffu)) {
    *(tod_half*)x = (u16)c;
    x[2] = (u8)(c >> 16);
  }
}
// (AI-assisted, todlag) vertices [first, end) of a plain group from a table that is all up to date
// (tod_refresh_all): no version checks; the rgb written as a halfword and a byte (the vertex keeps
// its alpha), or the whole word with the blended alpha (c3l v22)
inline void tod_write_fresh(const CtrLevelData::Tod& t, const TodVerts& v, int first, int end) {
  const u32* const tab = t.table.data();
  if (!t.keep_alpha) {
    tod_each(v, first, end, [tab](u32 idx, tod_word* c) { tod_store(c, tab[idx]); });
    return;
  }
  tod_each(v, first, end, [tab](u32 idx, tod_word* c) { tod_store_rgb((u8*)c, tab[idx]); });
}

// The heap the title's entry copies may still take (checked at most every kTodEntryCheck frames);
// they leave kTodEntryKeep free
struct TodEntryRoom {
  s64 left = 0;
  u64 frame = ~0ull;
};
TodEntryRoom g_tod_entry_room;
constexpr u64 kTodEntryCheck = 30;
constexpr s64 kTodEntryKeep = 4 << 20;
// Chunk ci's vertices' table entries (the title's fast day: made at its first whole rewrite), or
// nullptr when the heap has no room for them
const u16* tod_entries(CtrLevelData::Tod& t, size_t ci, const TodVerts& v, int n, u64 frame) {
  if (ci < t.chunk_entries.size() && t.chunk_entries[ci]) {
    u32 had;
    memcpy(&had, t.chunk_entries[ci].get(), 4);
    if (had == (u32)n) {
      return t.chunk_entries[ci].get() + 2;
    }
    tod_drop_entries(t, ci);  // (made for another mesh of the chunk)
  }
  auto& room = g_tod_entry_room;
  if (room.frame == ~0ull || frame - room.frame >= kTodEntryCheck) {
    room.frame = frame;
#ifdef __3DS__
    CtrHeapStats st;
    ctr_heap_stats(&st);
    room.left = std::min<s64>((s64)st.free - kTodEntryKeep, (s64)st.largest / 2);
#else
    room.left = 64 << 20;
#endif
  }
  const s64 bytes = 4 + 2 * (s64)n;
  if (n <= 0 || ci >= t.chunk_mesh.size() || bytes > room.left) {
    return nullptr;
  }
  u16* const p = new (std::nothrow) u16[2 + n];
  if (!p) {
    room.left = 0;
    return nullptr;
  }
  room.left -= bytes;
  const u32 nv = (u32)n;
  memcpy(p, &nv, 4);
  for (int i = 0; i < n; i++) {
    p[2 + i] = (u16)v.entry(i);
  }
  if (t.chunk_entries.size() < t.chunk_mesh.size()) {
    t.chunk_entries.resize(t.chunk_mesh.size());
  }
  t.chunk_entries[ci].reset(p);
  t.entries_bytes += (u32)bytes;
  return p + 2;
}
// Vertices [first, end) of a plain group from a table that is all up to date, through the chunk's
// entries (e, 4-byte aligned): every color stored, 8 vertices from 4 entry words
inline void tod_write_entries(const u32* tab, const u16* e, tod_word* c, int first, int end) {
  int i = first;
  if ((i & 1) && i < end) {
    c[i] = tab[e[i]];
    i++;
  }
  const tod_word* w = (const tod_word*)(e + i);
  tod_word* d = c + i;
  int n = end - i;
#if defined(__arm__)
  // (23 instructions per 8 vertices: one LDM of entries, two STMs of colors)
  if (n >= 8) {
    int blocks = n >> 3;
    n &= 7;
    asm volatile(
        "1:\n\t"
        "pld [%[w], #64]\n\t"
        "pld [%[d], #128]\n\t"
        "ldmia %[w]!, {r4, r5, r6, r7}\n\t"
        "uxth r8, r4\n\t"
        "lsr r9, r4, #16\n\t"
        "uxth r10, r5\n\t"
        "lsr ip, r5, #16\n\t"
        "ldr r8, [%[tab], r8, lsl #2]\n\t"
        "ldr r9, [%[tab], r9, lsl #2]\n\t"
        "ldr r10, [%[tab], r10, lsl #2]\n\t"
        "ldr ip, [%[tab], ip, lsl #2]\n\t"
        "stmia %[d]!, {r8, r9, r10, ip}\n\t"
        "uxth r8, r6\n\t"
        "lsr r9, r6, #16\n\t"
        "uxth r10, r7\n\t"
        "lsr ip, r7, #16\n\t"
        "ldr r8, [%[tab], r8, lsl #2]\n\t"
        "ldr r9, [%[tab], r9, lsl #2]\n\t"
        "ldr r10, [%[tab], r10, lsl #2]\n\t"
        "ldr ip, [%[tab], ip, lsl #2]\n\t"
        "stmia %[d]!, {r8, r9, r10, ip}\n\t"
        "subs %[k], %[k], #1\n\t"
        "bne 1b\n\t"
        : [w] "+r"(w), [d] "+r"(d), [k] "+r"(blocks)
        : [tab] "r"(tab)
        : "r4", "r5", "r6", "r7", "r8", "r9", "r10", "ip", "cc", "memory");
  }
#endif
  for (; n >= 8; n -= 8, w += 4, d += 8) {
    __builtin_prefetch(w + 16, 0);
    __builtin_prefetch(d + kTodPrefetch / 4, 1);
    const u32 a = w[0], b = w[1];
    const u32 x0 = tab[a & 0xffff], x1 = tab[a >> 16], x2 = tab[b & 0xffff], x3 = tab[b >> 16];
    d[0] = x0;
    d[1] = x1;
    d[2] = x2;
    d[3] = x3;
    const u32 a2 = w[2], b2 = w[3];
    const u32 y0 = tab[a2 & 0xffff], y1 = tab[a2 >> 16], y2 = tab[b2 & 0xffff], y3 = tab[b2 >> 16];
    d[4] = y0;
    d[5] = y1;
    d[6] = y2;
    d[7] = y3;
  }
  const u16* r = (const u16*)w;
  for (int j = 0; j < n; j++) {
    d[j] = tab[r[j]];
  }
}

// Vertices [first, end) from the table (without c3l v22 alpha the vertex keeps its own; plain: every
// one has a time of day color).
inline void tod_write_range(CtrLevelData::Tod& t, const TodVerts& v, int first, int end,
                            bool plain) {
  const u32* const tab = t.table.data();
  const u32* const stamp = t.stamp.data();
  const u8* const cm = t.color_mask.data();
  const u32* const lut = t.lut;
  const u32 keep = t.keep_alpha;
  if (plain) {
    tod_each(v, first, end, [&](u32 idx, tod_word* c) {
      const u32 rgb = stamp[idx] >= lut[cm[idx]] ? tab[idx] : tod_entry_slow(t, idx);
      tod_store(c, (*c & keep) | rgb);
    });
    return;
  }
  const u32 ntab = (u32)t.table.size();
  tod_each(v, first, end, [&](u32 idx, tod_word* c) {
    if (idx < ntab) {
      const u32 rgb = stamp[idx] >= lut[cm[idx]] ? tab[idx] : tod_entry_slow(t, idx);
      tod_store(c, (*c & keep) | rgb);
    }
  });
}

void tod_color_vertices(CtrLevelData::Tod& t, const TodVerts& v, int first, int end) {
  tod_write_range(t, v, first, end, false);
}

// The palettes the chunk's vertices use (0: no time of day color) and its vertices with a light
// color, by the light palettes they use (the light rewrites).
u8 tod_scan(const CtrLevelData::Tod& t, const TodVerts& v, int n, CtrLevelData::Tod::LightList* lights) {
  u32 mask = 0;
  bool any = false;
  const u32 nc = t.color_count, nm = (u32)t.muls.size();
  std::vector<std::pair<u8, u16>> lv;
  for (int i = 0; i < n; i++) {
    const u32 idx = v.entry(i);
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
  lights->masks.clear();
  lights->runs.clear();
  lights->ranges.clear();
  if (!lv.empty()) {
    lights->verts.reserve(lv.size() + CtrLightLists::kPad);
  }
  for (size_t k = 0; k < lv.size(); k++) {
    lights->verts.push_back(lv[k].second);
    if (k + 1 == lv.size() || lv[k + 1].first != lv[k].first) {
      lights->masks.emplace_back(lv[k].first, (u32)k + 1);
    }
  }
  if (!lv.empty()) {
    lights->verts.resize(lv.size() + CtrLightLists::kPad, 0);
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
void tod_mark_plain(CtrLevelData::Tod& t, u32 ci, const TodVerts& v, int n) {
  const u32 ntab = (u32)t.table.size();
  const u8* const cb = t.color_bright.empty() ? nullptr : t.color_bright.data();
  u8 chunk_bright = 0;
  for (u32 g = t.chunk_group_first[ci]; g < t.chunk_group_first[ci] + t.chunk_group_count[ci]; g++) {
    auto& gr = t.groups[g];
    bool plain = true;
    u8 bright = 0;
    for (int i = gr.lo; i <= std::min<int>(gr.hi, n - 1); i++) {
      const u32 idx = v.entry(i);
      plain = plain && idx < ntab;
      if (cb && idx < ntab) {
        bright |= cb[idx];
      }
    }
    gr.plain = plain ? 1 : 0;
    gr.bright = bright;
    chunk_bright |= bright;
  }
  if (ci < t.chunk_bright.size()) {
    t.chunk_bright[ci] = chunk_bright;
  }
}

// One level draw's rewrites: groups first, then light vertices not already rewritten, followed
// by tod_finish. (AI-assisted, todlag) every group drawn is brought up to
// this frame's weights before its draw (the colors drawn are the frame's, as on the PC). Azahar,
// Oct 23: the title's fast day (a move nearly every frame) 4.6-8.0 ms a frame; c3l_format.md.
constexpr int kTodLights = 0, kTodRound = 1;
// (AI-assisted, todlag) the group pass refreshes the whole table first (tod_refresh_all) when the
// vertices drawn are this many times the table's entries
constexpr u64 kTodFreshRatio = 3;
// (AI-assisted, perfv2) a day move's group rewrites spread over this many frames (budget: the
// vertices drawn / kTodSpreadFrames, at least kTodSpreadMin a frame). A day move is one 64th of
// a palette's weight every ~40 frames in gameplay: one move rewrote every group drawn in one frame
// (village2 on hardware: ~90k vertices, 14-22 ms on the render thread, 13 times per 15 s); a group
// a step behind for a few frames differs by at most ~4 color units. Two moves behind (the title's
// fast day): rewritten at once.
constexpr u64 kTodSpreadFrames = 6;
constexpr s64 kTodSpreadMin = 4000;
// (AI-assisted, todlag) the time of day's clock: ctr_gpu_time_ms; on the host times env
// CTR_TOD_TIME_SCALE (statistics only: the host is ~10x faster than Azahar)
inline double tod_now_ms() {
#ifndef __3DS__
  static const double scale =
      getenv("CTR_TOD_TIME_SCALE") ? atof(getenv("CTR_TOD_TIME_SCALE")) : 1.0;
  return ctr_gpu_time_ms() * scale;
#else
  return ctr_gpu_time_ms();
#endif
}
struct TodPass {
  // the pass: kTodLights (light vertices), kTodRound (groups)
  int pass = 0;
  bool fresh_ok = false;  // (AI-assisted, todlag) many vertices drawn: tod_refresh_all first
  bool fresh = false;     // the table is all up to date (tod_refresh_all done this pass)
  u32 rewritten = 0;
  u32 light_verts = 0;  // (AI-assisted, perfv2) the light pass's vertices written
  // (AI-assisted, perfv2) within kTodSpreadFrames of a day move: a group exactly that one move
  // behind (its colors this level's blend of the move before, not the file's) waits while the
  // frame's vertex budget spread_left is used up (drawn with colors one day step old, at most
  // kTodSpreadFrames frames); everything else is rewritten at once as before
  bool spread = false;
  s64 spread_left = 0;
  u32 deferred = 0;
  u32 unchanged = 0;  // groups behind whose colors all blend as before (not written)
  u64 frame = 0;
  s64 list_left = 8 * 1024;  // vertices whose lists may be made in this pass
  std::vector<u8> light_visible;  // scratch, reused for each chunk's visibility groups
  std::vector<u8> list_visible;
};
std::vector<std::pair<uintptr_t, uintptr_t>>& tod_dirty() {
  static std::vector<std::pair<uintptr_t, uintptr_t>> d;  // render thread
  return d;
}

// The vertex lists of every level's groups (render thread, Tod::group_list): per group a record
// of u16s: runs, wide; per run (its entries' palette mask | 0x100 for TodMuls, entries, bytes of
// their vertices, the version all of them were last written at, low and high half); per entry its table entry (a TodMul's: then its color); per entry its vertex
// count | the escapes below << 8;
// then the entries' vertices in vertex order per entry, a byte each: from the group's first, or
// when wide (over 255 vertices) from the entry's vertex before (its first from the group's; 255:
// the next two bytes hold it). An entry of more than 255 vertices is several. When full the room
// grows once to kRoomMax while the heap keeps kTodListReserve free after it (a lightning flash
// rewrites a group without a list whole), else it is emptied (each list made again as its group
// is written), at most once in kKeep frames.
struct TodLists {
  static constexpr u32 kRoom = 192 * 1024, kRoomMax = 384 * 1024, kKeep = 300, kMaxGroup = 8192;
  u16* room = nullptr;  // (made at the first list, kept)
  u32 cap = kRoom;      // u16s in room
  bool no_room = false;
  u32 used = 0, gen = 1;
  u64 emptied = 0, grow_tried = ~0ull;
  bool full = false;
  struct Entry {
    u32 key;  // palette mask | 0x100 for a TodMul, then the entry
    u32 count, at;  // its vertices, where they go
  };
  std::vector<u32> hash;  // scratch: (entry + 1) << 16 | its Entry
  std::vector<Entry> entries;
  std::vector<u16> order, offs;
};
TodLists g_tod_lists;
constexpr u32 kTodListReserve = 5u << 20;

// The room grown to kRoomMax with its lists (tried at most once in kKeep frames).
bool tod_lists_grow(u64 frame) {
  auto& L = g_tod_lists;
  if (!L.room || L.cap >= TodLists::kRoomMax ||
      (L.grow_tried != ~0ull && frame - L.grow_tried < TodLists::kKeep)) {
    return false;
  }
  L.grow_tried = frame;
#ifdef __3DS__
  CtrHeapStats st;
  ctr_heap_stats(&st);
  if (st.free < kTodListReserve + TodLists::kRoomMax * 2 || st.largest < TodLists::kRoomMax * 2) {
    return false;
  }
#endif
  u16* const r = new (std::nothrow) u16[TodLists::kRoomMax];
  if (!r) {
    return false;
  }
  memcpy(r, L.room, L.used * sizeof(u16));
  delete[] L.room;
  L.room = r;
  L.cap = TodLists::kRoomMax;
  L.full = false;
  lg::info("[ctr] time of day vertex lists: room grown to {} KB", TodLists::kRoomMax * 2 / 1024);
  return true;
}

// Group g's list from its vertices [first, end) (just written: every one shows the table).
void tod_make_list(CtrLevelData::Tod& t, u32 g, const TodVerts& v, int first, int end,
                   u64 frame) {
  auto& L = g_tod_lists;
  const u32 nv = (u32)(end - first);
  if (nv > TodLists::kMaxGroup) {
    t.group_list[g] = CtrLevelData::Tod::kNoList;
    return;
  }
  const bool wide = nv > 255;
  const bool may_empty = frame - L.emptied >= TodLists::kKeep;
  const u32 least = 6 + (nv + 1) / 2;
  if (L.no_room ||
      (!may_empty && (L.full || L.used + least > L.cap) && !tod_lists_grow(frame))) {
    return;  // (full for now: written whole until then)
  }
  const u32 nc = t.color_count, ntab = (u32)t.table.size();
  // the entries and their vertex counts (open addressing on the entry)
  int bits = 4;
  while ((1u << bits) < 2 * nv) {
    bits++;
  }
  auto& h = L.hash;
  h.assign(1u << bits, 0);
  const u32 hmask = (1u << bits) - 1;
  auto& es = L.entries;
  es.clear();
  auto slot = [&](u32 idx) -> u32& {
    u32 i = (idx * 2654435761u) >> (32 - bits);
    while (h[i] && (h[i] >> 16) != idx + 1) {
      i = (i + 1) & hmask;
    }
    return h[i];
  };
  u32 nk = 0;
  for (int i = first; i < end; i++) {
    const u32 idx = v.entry(i);
    if (idx >= ntab) {
      continue;
    }
    u32& x = slot(idx);
    if (!x) {
      if (es.size() * 2 >= hmask) {
        t.group_list[g] = CtrLevelData::Tod::kNoList;
        return;
      }
      x = (idx + 1) << 16 | (u32)es.size();
      es.push_back({(u32)t.color_mask[idx] << 16 | (idx >= nc ? 1u << 24 : 0) | idx, 0, 0});
    }
    es[x & 0xffff].count++;
    nk++;
  }
  // by palette mask (runs), then entry; an entry of more than 255 vertices as several
  auto& order = L.order;
  order.resize(es.size());
  for (u32 i = 0; i < (u32)es.size(); i++) {
    order[i] = (u16)i;
  }
  std::sort(order.begin(), order.end(), [&](u16 a, u16 b) { return es[a].key < es[b].key; });
  u32 runs = 0, ne = 0, nm = 0, at = 0;
  for (u32 i = 0; i < (u32)order.size(); i++) {
    auto& x = es[order[i]];
    runs += i == 0 || x.key >> 16 != es[order[i - 1]].key >> 16;
    ne += (x.count + 254) / 255;
    nm += (x.key >> 24) ? (x.count + 254) / 255 : 0;
    x.at = at;
    at += x.count;
  }
  // the vertices in entry order, and their bytes
  auto& offs = L.offs;
  offs.resize(nk);
  for (int i = first; i < end; i++) {
    const u32 idx = v.entry(i);
    if (idx < ntab) {
      offs[es[slot(idx) & 0xffff].at++] = (u16)(i - first);
    }
  }
  u32 bytes = nk;
  if (wide) {
    for (u32 i = 0, k = 0; i < (u32)order.size(); i++) {
      u32 last = 0;
      for (u32 j = 0; j < es[order[i]].count; j++, k++) {
        last = j % 255 ? last : 0;
        bytes += offs[k] - last >= 255 ? 2 : 0;
        last = offs[k];
      }
    }
  }
  const u32 size = 2 + 5 * runs + 2 * ne + nm + (bytes + 1) / 2;
  if (size > TodLists::kRoom || ne > 0xffff) {
    t.group_list[g] = CtrLevelData::Tod::kNoList;
    return;
  }
  if (L.used + size > L.cap && !tod_lists_grow(frame)) {
    if (!may_empty) {
      L.full = true;
      return;
    }
    L.full = false;
    L.used = 0;
    L.gen++;
    L.emptied = frame;
    std::fill(t.group_list.begin(), t.group_list.end(), 0u);
    t.list_gen = L.gen;
  }
  if (!L.room) {
    L.room = new (std::nothrow) u16[TodLists::kRoom];
    L.emptied = frame;
    if (!L.room) {
      L.no_room = true;
      lg::warn("[ctr] no heap for the time of day vertex lists ({} KB)",
               TodLists::kRoom * 2 / 1024);
      return;
    }
  }
  u16* const r = L.room + L.used;
  r[0] = (u16)runs;
  r[1] = wide ? 1 : 0;
  u16* run = r + 2 - 5;
  u16* e = r + 2 + 5 * runs;
  u16* cnt = e + ne + nm;
  u8* o = (u8*)(e + 2 * ne + nm);
  u32 k = 0;
  for (u32 i = 0; i < (u32)order.size(); i++) {
    const auto& x = es[order[i]];
    if (i == 0 || x.key >> 16 != es[order[i - 1]].key >> 16) {
      run += 5;
      run[0] = (u16)(x.key >> 16);
      run[1] = run[2] = run[3] = run[4] = 0;
    }
    u8* const o0 = o;
    for (u32 left = x.count; left;) {
      const u32 c = std::min<u32>(left, 255);
      run[1]++;
      *e++ = (u16)x.key;
      if (x.key >> 24) {
        *e++ = t.muls[(x.key & 0xffff) - nc].color;
      }
      u32 escapes = 0, last = 0;
      for (u32 j = 0; j < c; j++, k++) {
        const u32 d = wide ? offs[k] - last : offs[k];
        if (d >= 255) {
          *o++ = 255;
          *o++ = (u8)d;
          *o++ = (u8)(d >> 8);
          escapes++;
        } else {
          *o++ = (u8)d;
        }
        last = offs[k];
      }
      *cnt++ = (u16)(c | escapes << 8);
      left -= c;
    }
    run[2] += (u16)(o - o0);
  }
  t.group_list[g] = L.used + 1;
  L.used += size;
}

// Group g's vertices [first, ...) of the table entries whose color changed since version since
// (all: every one) written from the table (r: its list), brought up to date for them; only the
// entries using a palette of only (kTodAllRuns: all of them). A run of entries whose palettes
// didn't move since is passed over whole; a run written since then counts from its own write.
// Returns the vertices written.
constexpr u32 kTodAllRuns = ~0u;
template <bool kWide>
u32 tod_list_write(CtrLevelData::Tod& t, u16* r, const TodVerts& v, int first, u32 group_since,
                   bool all, u32 only) {
  const u32 nr = r[0];
  u16* run = r + 2;
  const u16* e = run + 5 * nr;
  u32 ne = 0, nm = 0;
  for (u32 k = 0; k < nr; k++) {
    ne += run[5 * k + 1];
    nm += run[5 * k] >> 8 ? run[5 * k + 1] : 0;
  }
  const u16* c = e + ne + nm;
  const u8* o = (const u8*)(e + 2 * ne + nm);
  tod_word* const w = v.rgba + first;
  const u32* const stamp = t.stamp.data();
  const u32* const cver = t.cver.data();
  const u32* const tab = t.table.data();
  const u32 keep = t.keep_alpha;
  u32 written = 0;
  const u32 ver = t.ver;
  for (const u16* const run_end = e; run < run_end; run += 5) {
    const u32 last = t.lut[run[0] & 0xff], n = run[1];
    const u32 step = run[0] >> 8 ? 2 : 1;  // (a TodMul: its color after it)
    const u32 since = std::max<u32>(group_since, run[3] | (u32)run[4] << 16);
    if ((!all && last <= since) || (only != kTodAllRuns && !(run[0] & only))) {
      // (none of these entries was blended again since)
      e += n * step;
      c += n;
      o += run[2];
      continue;
    }
    for (const u16* const end = e + n * step; e < end; e += step, c++) {
      const u32 idx = *e, cnt = *c & 0xff;
      if (stamp[idx] < last) {
        tod_entry_slow(t, idx);
      }
      if (all || cver[e[step - 1]] > since) {
        const u32 rgba = tab[idx];
        if (!kWide) {
          if (!keep) {
            for (u32 j = 0; j < cnt; j++) {
              w[o[j]] = rgba;
            }
          } else {
            for (u32 j = 0; j < cnt; j++) {
              tod_word* x = w + o[j];
              *x = (*x & keep) | rgba;
            }
          }
          o += cnt;
        } else {
          u32 at = 0;
          for (u32 j = 0; j < cnt; j++) {
            u32 d = *o++;
            if (d == 255) {
              d = o[0] | (u32)o[1] << 8;
              o += 2;
            }
            at += d;
            tod_word* x = w + at;
            if (keep) {
              *x = (*x & keep) | rgba;
            } else {
              *x = rgba;
            }
          }
        }
        written += cnt;
      } else {
        o += cnt + 2 * (*c >> 8);
      }
    }
    run[3] = (u16)ver;
    run[4] = (u16)(ver >> 16);
  }
  return written;
}

u32 tod_list_write(CtrLevelData::Tod& t, u32 gl, const TodVerts& v, int first, u32 since, bool all,
                   u32 only) {
  u16* r = g_tod_lists.room + gl - 1;
  return r[1] ? tod_list_write<true>(t, r, v, first, since, all, only)
              : tod_list_write<false>(t, r, v, first, since, all, only);
}

// The chunk's vertex colors brought up to date in its mesh (in place: a frame the GPU still draws
// may see the change) before it is drawn (shown: the chunk's runs drawn this frame, nullptr: all of
// them). A vertex group (one visibility id's vertices) is brought up to date when a day palette
// the chunk uses moved since (or it has the file's colors); its light vertices when a light palette
// moved (the light pass, per shown group). A group with a list (tod_make_list) gets only the
// vertices whose color changed, the others are written whole. Hidden groups wait until drawn.
// (AI-assisted, todlag) no budget: everything drawn is current. Returns the vertices written.
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
    tod_drop_entries(t, ci);
    for (u32 g = g0; g < g0 + ng; g++) {
      t.groups[g].ver = 0;
      t.groups[g].dver = 0;
      t.groups[g].lver = 0;
    }
    t.chunk_min_ver[ci] = t.chunk_min_dver[ci] = t.chunk_light_ver[ci] = 0;
  }
  // the last move of the chunk's day / light palettes
  const u32 m = t.chunk_mask[ci];
  const u32 dm = m & ~t.light_mask, lm = m & t.light_mask;
  // (a group behind only by held moves of palettes it isn't bright in waits: hlut)
  const u32 cb = ci < t.chunk_bright.size() ? t.chunk_bright[ci] : 0;
  const u32 day_last = std::max<u32>(1, std::max(t.lut[dm & cb], t.hlut[dm & ~cb]));
  const u32 light_last = t.lut[lm];
  // quick out: every group up to date
  const bool day_ok = t.chunk_min_ver[ci] >= day_last;
  const bool light_ok = t.chunk_light_ver[ci] >= light_last;
  if (t.chunk_scanned[ci] == 2 && (ps.pass == kTodLights ? light_ok : day_ok)) {
    return 0;
  }
  int n = 0;
  const TodVerts v = tod_verts(mesh, &n);
  if (!v) {
    return 0;
  }
  if (ps.frame != t.whole_frame) {
    t.whole_last = t.whole_now;
    t.whole_now = 0;
    t.whole_frame = ps.frame;
  }
  const bool lists = !t.fast_day;
  if (t.list_gen != g_tod_lists.gen || t.group_list.size() != t.groups.size()) {
    t.group_list.assign(t.groups.size(), 0);
    t.list_gen = g_tod_lists.gen;
  }
  if (!t.chunk_scanned[ci]) {
    // no mesh at load: the palettes and light vertices now (render thread, once)
    CtrLevelData::Tod::LightList lights;
    const u8 mask = tod_scan(t, v, n, &lights);
    t.chunk_scanned[ci] = mask ? 2 : 1;
    t.chunk_mask[ci] = mask;
    if (!lights.empty()) {
      t.chunk_lights[(u32)ci] = std::move(lights);
    }
    if (!mask) {
      return 0;
    }
    tod_mark_plain(t, (u32)ci, v, n);
    return tod_recolor(lev, ci, shown, ps);  // (the groups: one for all, made at load)
  }
  const u32 r0 = lev.chunk_first_run[ci];
  const u32 nr = lev.chunks[ci].vis_run_count;
  const bool by_runs = shown && ng > 1 && r0 <= t.run_group.size() &&
                       nr <= t.run_group.size() - r0;
  u32 looked = 0;
  int flo = n, fhi = -1;
  // (AI-assisted) Per-group light versions: hidden runs wait until revealed. The day pass
  // runs first and marks its groups current, avoiding a second write of their light vertices.
  if (ps.pass == kTodLights && !light_ok) {
    auto it = t.chunk_lights.find((u32)ci);
    if (it != t.chunk_lights.end()) {
      // the groups with a list from it (their light entries that changed), the others below
      if (lists) {
        auto& vis = ps.list_visible;
        if (by_runs) {
          vis.assign(ng, 0);
          for (u32 r = 0; r < nr; r++) {
            if (shown[r] && t.run_group[r0 + r] < ng) {
              vis[t.run_group[r0 + r]] = 1;
            }
          }
        }
        for (u32 g = g0; g < g0 + ng; g++) {
          auto& gr = t.groups[g];
          const u32 gl = t.group_list[g];
          if ((by_runs && !vis[g - g0]) || gr.lver >= light_last || !gl ||
              gl == CtrLevelData::Tod::kNoList) {
            continue;
          }
          u32 moved = 0;
          for (u32 m = lm; m; m &= m - 1) {
            const int p = __builtin_ctz(m);
            moved |= t.pal_ver[p] > gr.lver ? 1u << p : 0;
          }
          const int first = gr.lo, end = std::min<int>(gr.hi + 1, n);
          const u32 k = tod_list_write(t, gl, v, first, gr.ver, gr.ver == 0, moved);
          if (k) {
            flo = std::min(flo, first);
            fhi = std::max(fhi, end - 1);
            looked += k;
            ps.light_verts += k;
          }
          gr.lver = t.ver;
        }
      }
      auto& lights = it->second;
      tod_word* const w = v.rgba;
      const u32* const stamp = t.stamp.data();
      const u32* const tab = t.table.data();
      const u32* const cver = t.cver.data();
      const u8* const cm = t.color_mask.data();
      const u32* const lut = t.lut;
      const c3l::TodMul* const muls = t.muls.data();
      const u32 keep = t.keep_alpha, nc = t.color_count;
      // (t.cver isn't kept during the fast day)
      const bool track = !t.fast_day && !t.cver.empty();
      t.chunk_light_ver[ci] = ctr_recolor_light_groups_cached(
          t.groups.data() + g0, ng, by_runs ? shown : nullptr,
          by_runs ? t.run_group.data() + r0 : nullptr, nr, t.ver, light_last, lm, t.pal_ver,
          lights, ps.light_visible, [&](u32 i) { return v.entry((int)i); },
          [&](const CtrLightGroup& gr, u32 first, u32 rf, u32 rl, u32 lowest, u32 highest,
              u32& at) {
            // per entry its vertices; an entry whose color didn't change since they were all
            // last current (the group's day write or this part's last write) is passed over
            const u32 since = track && gr.ver ? std::max<u32>(gr.ver, at) : 0;
            at = t.ver;
            const u16* p = lights.verts.data() + first;
            const u32* const runs = lights.runs.data();
            u32 written = 0;
            for (u32 r = rf; r < rl; r++) {
              const u32 e = runs[r] & 0xffff, c = runs[r] >> 16;
              const u32 rgb = stamp[e] >= lut[cm[e]] ? tab[e] : tod_entry_slow(t, e);
              if (since && cver[e < nc ? e : muls[e - nc].color] <= since) {
                p += c;
                continue;
              }
              written += c;
              if (!keep) {
                for (const u16* const end = p + c; p < end; p++) {
                  __builtin_prefetch(w + p[CtrLightLists::kPad], 1);
                  w[*p] = rgb;
                }
              } else {
                for (const u16* const end = p + c; p < end; p++) {
                  __builtin_prefetch(w + p[CtrLightLists::kPad], 1);
                  tod_word* const x = w + *p;
                  *x = (*x & keep) | rgb;
                }
              }
            }
            if (written) {
              flo = std::min<int>(flo, (int)lowest);
              fhi = std::max<int>(fhi, (int)highest);
              looked += written;
              ps.light_verts += written;
            }
          });
    } else {
      t.chunk_light_ver[ci] = t.ver;
    }
  }
  if (ps.pass != kTodLights && !day_ok) {
    bool any = false;
    auto day_group = [&](u32 g) {
      auto& gr = t.groups[g];
      const u32 gb = gr.bright;
      if (gr.ver >= std::max<u32>(1, std::max(t.lut[dm & gb], t.hlut[dm & ~gb]))) {
        return;
      }
      // behind only by held moves: its entries blended as they are read, not the whole table
      const bool held_only = gr.ver >= std::max<u32>(1, t.hlut[dm]);
      const int first = gr.lo, end = std::min<int>(gr.hi + 1, n);
      if (ps.spread && gr.ver != 0 && gr.dver + 1 == t.dver && ps.spread_left < end - first) {
        ps.deferred++;  // (AI-assisted, perfv2) one day step behind: a later frame of the spread
        return;
      }
      ps.spread_left -= std::max(0, end - first);
      const u32 gl = t.group_list[g];
      if (gl && gl != CtrLevelData::Tod::kNoList && lists) {
        // (only the vertices of the entries whose color changed)
        const u32 n = tod_list_write(t, gl, v, first, gr.ver, gr.ver == 0, kTodAllRuns);
        if (n) {
          looked += n;
          flo = std::min(flo, first);
          fhi = std::max(fhi, end - 1);
          ps.rewritten++;
        } else {
          ps.unchanged++;
        }
        gr.ver = t.ver;
        gr.dver = t.dver;
        gr.lver = t.ver;
        any = true;
        return;
      }
      if (ps.fresh_ok && !ps.fresh && !held_only &&
          (!lists || !g_tod_lists.room || t.whole_last >= t.table.size())) {
        // (many vertices written whole: at the frame's first such group; versions don't move
        // until the next tod_update)
        tod_refresh_all(t);
        ps.fresh = true;
      }
      if (end > first) {
        const u16* ents = ps.fresh && gr.plain && t.fast_day && !t.keep_alpha
                              ? tod_entries(t, ci, v, n, ps.frame)
                              : nullptr;
        if (ents) {
          tod_write_entries(t.table.data(), ents, v.rgba, first, end);
        } else if (ps.fresh && gr.plain) {
          tod_write_fresh(t, v, first, end);
        } else {
          tod_write_range(t, v, first, end, gr.plain);
        }
        if (!gl && ps.list_left > 0 && lists) {
          tod_make_list(t, g, v, first, end, ps.frame);
          ps.list_left -= end - first;
        }
        t.whole_now += (u32)(end - first);
        looked += (u32)(end - first);
        flo = std::min(flo, first);
        fhi = std::max(fhi, end - 1);
      }
      gr.ver = t.ver;
      gr.dver = t.dver;
      gr.lver = t.ver;
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
  }
  if (fhi >= flo) {
    tod_dirty().emplace_back((uintptr_t)(v.rgba + flo), (uintptr_t)(v.rgba + fhi + 1));
  }
  return looked;
}

// The PS2's tfrag geomorph's colors (c3l::ExtTfrag): the morph vertices' m and t (the means of
// their TfragMorphColor entries' blended colors) in chunk ci's mesh tail, for its time of day
// groups with near or mid runs shown, when a palette of those entries moved since they were made.
// Returns the vertices written.
u32 tf_morph_colors(CtrLevelData& lev, size_t ci, const u8* shown) {
  auto& t = *lev.tod;
  const int mesh = ci < lev.meshes.size() ? lev.meshes[ci] : -1;
  if (mesh < 0 || !t.blended_valid || !lev.tf_morph(ci) || ci >= t.chunk_group_first.size()) {
    return 0;
  }
  int n = 0;
  u8* const base = (u8*)ctr_gpu_mesh_vertices_inplace(mesh, &n);
  if (!base || (u32)n != lev.chunks[ci].vertex_count) {
    return 0;
  }
  const auto& tc = lev.tf_chunks[lev.chunk_tf[ci]];
  const u32 g0 = t.chunk_group_first[ci], ng = t.chunk_group_count[ci];
  if (lev.tf_color_ver.size() != t.groups.size()) {
    lev.tf_color_ver.assign(t.groups.size(), 0);
    lev.tf_color_mask.assign(t.groups.size(), 0);
    lev.tf_mesh.assign(lev.chunks.size(), -1);
  }
  if (lev.tf_mesh[ci] != mesh) {
    lev.tf_mesh[ci] = mesh;
    for (u32 g = g0; g < g0 + ng; g++) {
      lev.tf_color_ver[g] = 0;
    }
  }
  static std::vector<u8> need;  // (render thread)
  need.assign(ng, shown ? 0 : 1);
  const u32 r0 = lev.chunk_first_run[ci], nr = lev.chunks[ci].vis_run_count;
  if (shown && r0 + nr <= t.run_group.size()) {
    // (a group entirely nearer than band 1's start: nothing moves, its colors made when it is)
    for (u32 r = 0; r < nr; r++) {
      const auto& rl = lev.run_lod[r0 + r];
      if (shown[r] && (rl.lod == c3l::kRunLodNear || rl.lod == c3l::kRunLodMid) &&
          t.run_group[r0 + r] < ng && rl.group < lev.tf_state.size() &&
          (lev.tf_state[rl.group] & 32)) {
        need[t.run_group[r0 + r]] = 1;
      }
    }
  } else {
    need.assign(ng, 1);
  }
  u8* const tail = base + lev.normals_offset_in_mesh(ci);
  c3l::TfragMorphColor* const mc = lev.tf_colors.data() + tc.color_first;
  const u32 nm = tc.color_count;
  // the mean of two rgba words, byte by byte (rounded down)
  auto avg = [](u32 a, u32 b) { return (a & b) + (((a ^ b) & 0xfefefefeu) >> 1); };
  const u32 nt = (u32)t.table.size();
  const u32* const tab = t.table.data();
  u32 written = 0, lo = UINT32_MAX, hi = 0;
  for (u32 g = 0; g < ng; g++) {
    if (!need[g]) {
      continue;
    }
    const auto& gr = t.groups[g0 + g];
    c3l::TfragMorphColor* it = std::lower_bound(
        mc, mc + nm, gr.lo, [](const c3l::TfragMorphColor& a, u32 v) { return a.vertex < v; });
    c3l::TfragMorphColor* const end = std::upper_bound(
        it, mc + nm, gr.hi, [](u32 v, const c3l::TfragMorphColor& a) { return v < a.vertex; });
    if (it == end) {
      continue;
    }
    u16& mask = lev.tf_color_mask[g0 + g];
    if (!mask) {
      // the palettes its entries use (bit 8: made), each vertex's in its pad
      u32 m = 0x100;
      for (auto* x = it; x < end; x++) {
        u32 vm = 0;
        for (u16 e : {x->m[0], x->m[1], x->t[0], x->t[1], x->t[2], x->t[3]}) {
          vm |= e < t.color_mask.size() ? t.color_mask[e] : 0xff;
        }
        x->pad = (u16)vm;
        m |= vm;
      }
      mask = (u16)m;
    }
    u32& ver = lev.tf_color_ver[g0 + g];
    if (ver && ver >= t.lut[mask & 0xff]) {
      continue;
    }
    // the palettes that moved since its colors were made: only the vertices with a color of
    // theirs (a lamp's flicker moves a few)
    u32 moved = 0xff;
    if (ver) {
      moved = 0;
      for (u32 pm = mask & 0xff; pm; pm &= pm - 1) {
        const int pi = __builtin_ctz(pm);
        moved |= t.pal_ver[pi] > ver ? 1u << pi : 0;
      }
    }
    ver = std::max<u32>(t.ver, 1);
    auto colors = [&](auto fresh) {
      for (const auto* x = it; x < end; x++) {
        if (!(x->pad & moved)) {
          continue;
        }
        auto e = [&](u16 i) { return i < nt ? (fresh ? tab[i] : tod_entry(t, i)) : 0u; };
        // (a level0 point's m is its own color, its t the mean of two)
        const u32 a = e(x->m[0]);
        const u32 m = x->m[1] == x->m[0] ? a : avg(a, e(x->m[1]));
        const u32 t0 = e(x->t[0]);
        const u32 t1 = x->t[1] == x->t[0] ? t0 : avg(t0, e(x->t[1]));
        const u32 t2 = e(x->t[2]);
        const u32 t3 = x->t[3] == x->t[2] ? t2 : avg(t2, e(x->t[3]));
        tod_word* const r = (tod_word*)(tail + 16u * x->vertex);
        r[2] = m;
        r[3] = avg(t1, t3);
        lo = std::min<u32>(lo, x->vertex);
        hi = std::max<u32>(hi, x->vertex);
        written++;
      }
    };
    // (a table all up to date, tod_refresh_all: read without the version checks)
    if (t.fresh_ver == t.ver) {
      colors(std::true_type{});
    } else {
      colors(std::false_type{});
    }
  }
  if (written) {
    tod_dirty().emplace_back((uintptr_t)(tail + 16u * lo), (uintptr_t)(tail + 16u * (hi + 1)));
  }
  return written;
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
    u64 frames = 0, verts = 0, stale = 0, stale_frames = 0, steps = 0, astale = 0;
    int max_err = 0, max_step = 0;
    std::array<u8, 24> prev{};
    bool have_prev = false;
    // (AI-assisted, todlag) the game's weights of the last kAgeFrames frames (newest first) and
    // the age in frames of the colors drawn: the smallest k whose weights' blend a drawn vertex
    // shows (kAgeFrames: none of them, e.g. day and light palettes from different frames)
    std::deque<std::array<u8, 24>> hist;
    u64 age_sum = 0, age_verts = 0, aged_frames = 0;
    u32 age_max = 0;
    // groups drawn with colors 3+ frames old (file: still the file's colors), group draws
    u64 old_groups = 0, file_groups = 0, group_draws = 0;
  };
  constexpr u32 kAgeFrames = 64;
  static std::map<std::string, St> stats;
  auto& st = stats[lev.name];
  u8 w[8][3];
  tod_weights(cam, w);
  // c3l v22: the alpha each entry blends to with this frame's alpha weights
  u8 wa[8];
  tod_alpha_weights(cam, wa);
  std::vector<u8> akind_alpha(t.akinds.size());
  for (size_t k = 0; k < t.akinds.size(); k++) {
    akind_alpha[k] = (u8)(tod_blend_alpha(t.akinds[k], wa) >> 24);
  }
  auto alpha_now = [&](u32 idx) -> int {
    const u32 nc = t.color_count;
    const u32 c = idx < nc ? idx : t.muls[idx - nc].color;
    return akind_alpha.empty() ? -1 : akind_alpha[t.akind.empty() ? 0 : t.akind[c]];
  };
  u64 astale = 0;
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
  st.hist.push_front(st.prev);
  if (st.hist.size() > kAgeFrames) {
    st.hist.pop_back();
  }
  std::vector<std::vector<u32>> aged(st.hist.size());  // exact(hist[k]), made when needed
  auto age_of = [&](u32 idx, u32 c) -> u32 {
    for (u32 k = 1; k < st.hist.size(); k++) {
      if (aged[k].empty()) {
        u8 hw[8][3];
        memcpy(hw, st.hist[k].data(), 24);
        exact(hw, aged[k]);
      }
      if (aged[k][idx] == c) {
        return k;
      }
    }
    return kAgeFrames;
  };
  u32 frame_age = 0;
  u64 frame_age_sum = 0;
  // (AI-assisted) "off": vertices that aren't the table's weights' color (drawn behind)
  std::vector<u32> tgt;
  exact(t.target_w, tgt);
  u64 verts = 0, stale = 0, off = 0, far = 0;
  int err = 0, step = 0;
  std::vector<u8> used;
  u64 old_groups = 0, file_groups = 0, group_draws = 0;
  for (const auto& [ci, shown] : drawn) {
    if (ci >= lev.meshes.size() || lev.meshes[ci] < 0) {
      continue;
    }
    // (AI-assisted, todlag) per group of the chunk: vertices drawn, the oldest color's age
    const u32 cg0 = ci < t.chunk_group_first.size() ? t.chunk_group_first[ci] : 0;
    const u32 cgn = ci < t.chunk_group_count.size() ? t.chunk_group_count[ci] : 0;
    std::vector<u32> g_age(cgn, 0), g_n(cgn, 0);
    auto group_of = [&](int i) -> int {
      for (u32 g = 0; g < cgn; g++) {
        if (i >= t.groups[cg0 + g].lo && i <= t.groups[cg0 + g].hi) {
          return (int)g;
        }
      }
      return -1;
    };
    int n = 0;
    const TodVerts v = tod_verts(lev.meshes[ci], &n);
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
      const u32 idx = v.entry(i);
      if (idx >= now.size()) {
        continue;
      }
      verts++;
      const u32 rgb = v.rgba[i];
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
      if (!t.keep_alpha) {
        astale += alpha_now(idx) != (int)(rgb >> 24);
      }
      const int gi = group_of(i);
      if (gi >= 0) {
        g_n[gi]++;
      }
      if (e > 0) {
        const u32 a = age_of(idx, rgb & 0xffffff);
        frame_age = std::max(frame_age, a);
        frame_age_sum += a;
        if (gi >= 0) {
          g_age[gi] = std::max(g_age[gi], a);
        }
      }
      {
        const u32 c = rgb & 0xffffff;
        off += c != tgt[idx];
      }
      err = std::max(err, e);
      step = std::max(step, s);
    }
    for (u32 g = 0; g < cgn; g++) {
      if (!g_n[g]) {
        continue;
      }
      group_draws++;
      const auto& gr = t.groups[cg0 + g];
      if (g_age[g] >= 3) {
        old_groups++;
        file_groups += gr.ver == 0;
        if (mode >= 3 || (mode >= 2 && gr.ver == 0)) {
          lg::info("[ctr] tod old group {} frame {}: chunk {} (kind {} tier {}{}) group {} ({} "
                   "vertices drawn): colors {} frames old{}, {} day moves behind",
                   lev.name, rs.frame_idx, ci, lev.chunks[ci].kind, lev.chunks[ci].lod_tier,
                   lev.is_unit(ci) ? ", streamed" : "", g, g_n[g], g_age[g],
                   gr.ver == 0 ? " (the file's)" : "", gr.ver ? t.dver - gr.dver : 0);
        }
      }
    }
  }
  st.old_groups += old_groups;
  st.file_groups += file_groups;
  st.group_draws += group_draws;
  st.frames++;
  st.verts += verts;
  st.stale += stale;
  st.astale += astale;
  st.stale_frames += stale > 0;
  st.max_err = std::max(st.max_err, err);
  st.max_step = std::max(st.max_step, step);
  st.steps += step > 0;
  st.age_sum += frame_age_sum;
  st.age_verts += verts;
  st.age_max = std::max(st.age_max, frame_age);
  st.aged_frames += frame_age > 1;
  if (mode >= 2) {
    lg::info("[ctr] tod check {} frame {}: {} vertices drawn, {} stale (max error {}), step {}, "
             "{} off, {} more than 4 off; color age {} frames at most, {:.2f} on average; {} of "
             "{} groups drawn 3+ frames old ({} with the file's colors)",
             lev.name, rs.frame_idx, verts, stale, err, step, off, far, frame_age,
             (double)frame_age_sum / std::max<u64>(1, verts), old_groups, group_draws,
             file_groups);
  }
  if (rs.log_now) {
    lg::info("[ctr] tod check {}: {} frames, {:.0f} vertices drawn per frame, {} stale vertices in "
             "{} frames (max error {}), {} frames with a color step (max step {}); {} vertices "
             "with another alpha than the blend",
             lev.name, st.frames, (double)st.verts / std::max<u64>(1, st.frames), st.stale,
             st.stale_frames, st.max_err, st.steps, st.max_step, st.astale);
    lg::info("[ctr] tod check {}: color age of the vertices drawn {} frames at most ({} = none of "
             "the last {}), {:.3f} on average, {} frames with a color more than 1 frame old; "
             "{} of {} group draws 3+ frames old ({} with the file's colors)",
             lev.name, st.age_max, kAgeFrames, kAgeFrames,
             (double)st.age_sum / std::max<u64>(1, st.age_verts), st.aged_frames, st.old_groups,
             st.group_draws, st.file_groups);
    const auto keep = st.prev;
    auto keep_hist = std::move(st.hist);
    st = St();
    st.prev = keep;
    st.hist = std::move(keep_hist);
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
  // c3l v22: TodColorA (alpha blended); older: TodColor (the vertices keep their alpha)
  const bool alpha = hdr.version >= c3l::kTodAlphaVersion;
  const u32 csize = alpha ? (u32)sizeof(c3l::TodColorA) : (u32)sizeof(c3l::TodColor);
  constexpr u32 kPiece = 2048;
  std::vector<c3l::TodColorA> piece;
  std::vector<c3l::TodColor> piece_rgb;
  auto read_piece = [&](u32 i, u32 n) {
    if (alpha) {
      return f.read_array(hdr.tod_offset + i * csize, n, &piece);
    }
    if (!f.read_array(hdr.tod_offset + i * csize, n, &piece_rgb)) {
      return false;
    }
    piece.assign(n, c3l::TodColorA{});
    for (u32 j = 0; j < n; j++) {
      for (int p = 0; p < 8; p++) {
        memcpy(piece[j].rgba[p], piece_rgb[j].rgb[p], 3);
      }
    }
    return true;
  };
  auto alpha_of = [](const c3l::TodColorA& c) {
    std::array<u8, 8> a;
    for (int p = 0; p < 8; p++) {
      a[p] = c.rgba[p][3];
    }
    return a;
  };
  auto rgb_mask = [](const c3l::TodColorA& c) {
    u32 m = 0;
    for (int p = 0; p < 8; p++) {
      if (c.rgba[p][0] | c.rgba[p][1] | c.rgba[p][2]) {
        m |= 1u << p;
      }
    }
    return m;
  };
  // the alpha kinds: all 0 (never moves); 0x80 in the day palettes (black in no more than 3/4
  // of the colors: the moods' weights for them add up to 1.0 or more) and 0x80 or 0 in the others
  // ("full": it blends to 0x80, outside the masks); the others move with their palettes
  std::map<std::array<u8, 8>, u32> kind_of;
  u32 mask = 0, rgb_used[8] = {};
  for (u32 i = 0; i < nc; i += kPiece) {
    const u32 n = std::min(kPiece, nc - i);
    if (!read_piece(i, n)) {
      lg::warn("[ctr] {}: bad time of day table, not used", lev->name);
      return;
    }
    for (const auto& c : piece) {
      const u32 m = rgb_mask(c);
      mask |= m;
      for (int p = 0; p < 8; p++) {
        rgb_used[p] += m >> p & 1;
      }
      if (alpha && kind_of.size() <= 256) {
        kind_of.emplace(alpha_of(c), 0);
      }
    }
  }
  auto t = std::make_unique<CtrLevelData::Tod>();
  if (alpha && kind_of.size() > 256) {
    lg::warn("[ctr] {}: {} time of day alphas (at most 256): the vertices keep their alpha",
             lev->name, kind_of.size());
  } else if (alpha) {
    t->keep_alpha = 0;
    u32 day = 0;
    for (int p = 0; p < 8; p++) {
      if (rgb_used[p] * 4 > nc) {
        day |= 1u << p;
      }
    }
    for (auto& [a, k] : kind_of) {
      k = (u32)t->akinds.size();
      t->akinds.push_back(a);
      bool full = day != 0, zero = true;
      for (int p = 0; p < 8; p++) {
        full = full && (a[p] == 0x80 || (a[p] == 0 && !(day >> p & 1)));
        zero = zero && a[p] == 0;
        if (a[p]) {
          t->apal_mask |= 1u << p;
        }
      }
      t->akind_full.push_back(full && !zero);
    }
    t->aword.assign(t->akinds.size(), 0);
  }
  auto kind_mask = [&](u32 k) {
    u32 m = 0;
    for (int p = 0; p < 8; p++) {
      if (t->akinds[k][p]) {
        m |= 1u << p;
      }
    }
    return m;
  };
  for (int p = 0; p < 8; p++) {
    if (mask & (1u << p)) {
      t->palettes[t->palette_count++] = (u8)p;
    }
  }
  t->track_mask = mask | t->apal_mask;
  // the pulse hold's error: a color bright in a palette (channel * (tod_pulse_step - 1) >= 64)
  // would be a color unit or more off while the palette is held
  const int step = ctr_settings().tod_pulse_step;
  auto bright_of = [&](u32 v) { return step > 1 && v * (u32)(step - 1) >= 64; };
  // (colors, TodMuls, per entry its mask, rgb and stamp, per color its alpha kind and change
  // version, the vertex lists' room, per chunk 28 bytes with its groups, per run 2). (AI-assisted)
  // Allow for the added light stamp and list of every possible visibility group (at most one per
  // run, plus the whole-chunk fallback).
  [[maybe_unused]] const u32 need = nc * t->palette_count * 3 + nm * (u32)sizeof(c3l::TodMul) +
                                     (nc + nm) * 10 + nc * 5 +
                                     (g_tod_lists.room ? 0 : TodLists::kRoom * 2) +
                                     (u32)lev->chunks.size() * 29 + (u32)lev->vis_runs.size() * 2 +
                                     (u32)(lev->chunks.size() + lev->vis_runs.size()) * 8;
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
  t->color_bright.assign(nc + nm, 0);
  if (t->akinds.size() > 1) {
    t->akind.assign(nc, 0);
  }
  for (u32 i = 0; i < nc; i += kPiece) {
    const u32 n = std::min(kPiece, nc - i);
    if (!read_piece(i, n)) {
      return;
    }
    u8* dst = t->colors.data() + (size_t)i * t->palette_count * 3;
    for (u32 j = 0; j < n; j++) {
      const auto& c = piece[j];
      u32 m = 0;
      u8 bright = 0;
      for (u32 k = 0; k < t->palette_count; k++) {
        const u8* rgb = c.rgba[t->palettes[k]];
        memcpy(dst, rgb, 3);
        dst += 3;
        if (rgb[0] | rgb[1] | rgb[2]) {
          m |= 1u << t->palettes[k];
        }
        if (bright_of(std::max({rgb[0], rgb[1], rgb[2]}))) {
          bright |= (u8)(1u << t->palettes[k]);
        }
      }
      if (!t->akinds.empty()) {
        const u32 k = kind_of[alpha_of(c)];
        if (!t->akind.empty()) {
          t->akind[i + j] = (u8)k;
        }
        const u32 km = kind_mask(k);
        if (!t->akind_full[k] || !m) {
          m |= km;  // (its alpha moves with these palettes; a black color: so it is blended)
        }
        if (!t->akind_full[k]) {
          for (int p = 0; p < 8; p++) {
            if (bright_of(c.rgba[p][3])) {
              bright |= (u8)(1u << p);
            }
          }
        }
      }
      t->color_mask[i + j] = (u8)m;
      t->color_bright[i + j] = bright;
    }
  }
  if (nm && !f.read_array(hdr.tod_offset + nc * csize, nm, &t->muls)) {
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
    const auto& mu = t->muls[j];
    t->color_mask[nc + j] = t->color_mask[mu.color];
    // (bright in the colors' palettes times the largest factor)
    const u8* src = t->colors.data() + (size_t)mu.color * t->palette_count * 3;
    const u32 f = std::max({mu.mul[0], mu.mul[1], mu.mul[2]});
    u8 bright = t->color_bright[mu.color];
    for (u32 k = 0; k < t->palette_count; k++, src += 3) {
      if (bright_of(std::max({src[0], src[1], src[2]}) * f / 128)) {
        bright |= (u8)(1u << t->palettes[k]);
      }
    }
    t->color_bright[nc + j] = bright;
  }
  // light palettes: a quarter of the colors or fewer
  {
    u32 used[8] = {};
    for (u32 i = 0; i < nc; i++) {
      for (u32 m = t->color_mask[i]; m; m &= m - 1) {
        used[__builtin_ctz(m)]++;
      }
    }
    for (int p = 0; p < 8; p++) {
      if ((t->track_mask >> p & 1) && used[p] * 4 <= nc) {
        t->light_mask |= 1u << p;
      }
    }
    for (u32 i = 0; i < nc; i++) {
      t->light_color_count += (t->color_mask[i] & t->light_mask) != 0;
    }
  }
  t->table.assign(nc + nm, 0);
  t->stamp.assign(nc + nm, 0);
  t->fresh_ver = ~0u;
  t->cver.assign(nc, 0);
  const size_t nch = lev->chunks.size();
  t->chunk_group_first.assign(nch, 0);
  t->chunk_group_count.assign(nch, 0);
  t->run_group.assign(lev->vis_runs.size(), 0);
  t->chunk_min_ver.assign(nch, 0);
  t->chunk_min_dver.assign(nch, 0);
  t->chunk_light_ver.assign(nch, 0);
  t->chunk_mask.assign(nch, 0);
  t->chunk_bright.assign(nch, 0);
  t->chunk_scanned.assign(nch, 0);
  t->chunk_mesh.assign(nch, -1);
  // the weights the level was last drawn with, else the last level's with the same day palettes:
  // the meshes are colored as they are read (CtrLevels::load_file), not all at the level's first
  // draw. Else they keep the file's colors until drawn (tod_recolor).
  {
    auto& seen = tod_seen();
    std::array<u8, 32> w{};
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
      memcpy(t->held_w, w.data(), 24);
      memcpy(t->wa, w.data() + 24, 8);
      memcpy(t->held_wa, w.data() + 24, 8);
      for (size_t k = 0; k < t->akinds.size(); k++) {
        t->aword[k] = tod_blend_alpha(t->akinds[k], t->wa);
      }
      t->blended_valid = true;
      // version 1: the colors the meshes get as they are read
      t->ver = 1;
      for (u32 m = t->track_mask; m; m &= m - 1) {
        t->pal_ver[__builtin_ctz(m)] = 1;
        t->pal_hver[__builtin_ctz(m)] = 1;
      }
      for (u32 m = 1; m < 256; m++) {
        t->lut[m] = std::max(t->lut[m & (m - 1)], t->pal_ver[__builtin_ctz(m)]);
        t->hlut[m] = std::max(t->hlut[m & (m - 1)], t->pal_hver[__builtin_ctz(m)]);
      }
      tod_make_act(*t, t->target_w, &t->act);
    }
  }
  size_t bright = 0;
  for (u8 b : t->color_bright) {
    bright += b != 0;
  }
  lg::info("[ctr] {}: time of day: {} colors in {} palettes (lights {:02x}: {} colors), {} "
           "multiplied ({} KB); {} alpha kinds{}, {} entries bright in a palette",
           lev->name, nc, t->palette_count, t->light_mask, t->light_color_count, nm,
           t->bytes() / 1024, t->akinds.size(), t->keep_alpha ? " (the vertices' alpha)" : "",
           bright);
  lev->tod = std::move(t);
}

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
      if (rl.lod != c3l::kRunLodAlways && (rl.group >= lev->lod_groups.size() || rl.lod > 3)) {
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
#ifndef __3DS__
  // (AI-assisted, lodnear) host log, env CTR_LOD_LOG: where the groups switch to their far
  // version by screen-space error (gameplay focal 256, tie_lod_px 4) against the PS2 far-plane
  if (getenv("CTR_LOD_LOG") && lev->lod_error.size() == lev->lod_groups.size() &&
      !lev->lod_groups.empty()) {
    std::vector<float> e_m, ratio;
    u32 closer = 0, closer_half = 0;
    for (size_t g = 0; g < lev->lod_groups.size(); g++) {
      const float de = lev->lod_error[g] * 64.f / 4096.f, dp = lev->lod_groups[g].dist / 4096.f;
      e_m.push_back(de);
      ratio.push_back(dp > 0 ? de / dp : 1e9f);
      closer += de < dp;
      closer_half += de < 0.5f * dp;
    }
    auto pct = [](std::vector<float> v, float q) {
      std::sort(v.begin(), v.end());
      return v[std::min(v.size() - 1, (size_t)(q * v.size()))];
    };
    lg::info("[lodlog] {}: {} groups: screen-error switch (4 px, focal 256) at p10 {:.1f} / p50 "
             "{:.1f} / p90 {:.1f} m (+ radius); / PS2 far-plane p10 {:.2f} p50 {:.2f} p90 {:.2f}; "
             "nearer than the PS2 far-plane {}, nearer than half of it {}",
             lev->name, e_m.size(), pct(e_m, 0.1f), pct(e_m, 0.5f), pct(e_m, 0.9f),
             pct(ratio, 0.1f), pct(ratio, 0.5f), pct(ratio, 0.9f), closer, closer_half);
  }
#endif
  lg::info("[ctr] {}: tie levels of detail: {} groups, {} near runs{}{}", lev->name,
           lev->lod_groups.size(), near_runs, bad ? fmt::format(" ({} bad runs)", bad) : "",
           lev->lod_error.empty() ? ", switched at the PS2 distances"
                                  : ", switched by screen-space error");
}

#ifndef __3DS__
// (AI-assisted, lodnear) host log counters (CTR_LOD_LOG), by unit state
u64 g_lod_far_why[4] = {}, g_lod_far_close[4] = {};
#endif

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

// c3l v21 envmapped ties (c3l::ExtEnvTie): the table of the chunks with
// normals, read before the chunks' meshes are made. Each chunk carries its normals in its own
// mesh's tail (kNormalsInMesh: normals_offset_in_mesh), so they are freed and read back with it
// (distance freeing, dormant levels, streamed near chunks).
void load_env_tie_chunks(c3l::Reader& f, const c3l::Header& hdr, CtrLevelData* lev) {
  lev->chunk_normals.clear();
  lev->chunk_normals_file.clear();
  if (hdr.version < c3l::kEnvTieVersion || !hdr.ext_offset || !ctr_gpu_etie_ready()) {
    return;
  }
  c3l::ExtHeader eh;
  if (!read_ext_header(f, hdr, &eh) || eh.size < c3l::kExtHeaderV21Size) {
    return;
  }
  c3l::ExtEnvTie et{};
  if (!f.read(hdr.ext_offset + c3l::kExtHeaderV20Size, &et, sizeof(et)) || !et.draw_count) {
    return;
  }
  std::vector<c3l::EnvTieChunk> ecs;
  if (et.draw_count > 65535 || !f.read_array(et.chunks_offset, et.chunk_count, &ecs)) {
    lg::warn("[ctr] {}: bad envmapped tie data, not used", lev->name);
    return;
  }
  for (const auto& ec : ecs) {
    if (ec.chunk >= lev->chunks.size() || !ec.normals_offset) {
      lg::warn("[ctr] {}: bad envmapped tie chunk, not used", lev->name);
      return;
    }
  }
  lev->chunk_normals.assign(lev->chunks.size(), -1);
  lev->chunk_normals_file.assign(lev->chunks.size(), 0);
  for (const auto& ec : ecs) {
    lev->chunk_normals_file[ec.chunk] = ec.normals_offset;
    lev->chunk_normals[ec.chunk] = CtrLevelData::kNormalsInMesh;
  }
}

// the listed base draws (after the chunks, textures and draw order)
void load_env_ties(c3l::Reader& f, const c3l::Header& hdr, CtrLevelData* lev) {
  lev->env_draws.clear();
  lev->draw_env.clear();
  lev->normals_mesh = -1;
  if (lev->chunk_normals.size() != lev->chunks.size()) {
    lev->chunk_normals.clear();
    return;
  }
  c3l::ExtEnvTie et{};
  std::vector<c3l::EnvTieDraw> eds;
  if (!f.read(hdr.ext_offset + c3l::kExtHeaderV20Size, &et, sizeof(et)) ||
      !f.read_array(et.draws_offset, et.draw_count, &eds)) {
    lg::warn("[ctr] {}: bad envmapped tie data, not used", lev->name);
    lev->chunk_normals.clear();
    return;
  }
  u32 chunks = 0, bytes = 0;
  for (size_t ci = 0; ci < lev->chunks.size(); ci++) {
    if (lev->chunk_normals[ci] == CtrLevelData::kNormalsInMesh) {
      chunks++;
      bytes += lev->chunks[ci].vertex_count * 4;
    }
  }
  lev->draw_env.assign(lev->draws.size(), -1);
  u32 used = 0;
  for (size_t i = 0; i < eds.size(); i++) {
    const auto& e = eds[i];
    const size_t ci = lev->draw_chunk.size() > e.draw ? lev->draw_chunk[e.draw] : 0;
    // (a morph chunk's normals are in its tail only after its records)
    if (e.draw < lev->draws.size() && e.texture < lev->textures.size() &&
        lev->chunk_normals[ci] == CtrLevelData::kNormalsInMesh &&
        (ci >= lev->chunk_morph.size() || lev->chunk_morph[ci] < 0 || lev->morph_normals(ci))) {
      lev->draw_env[e.draw] = (s16)i;
      used++;
    }
  }
  lev->env_draws = std::move(eds);
  lg::info("[ctr] {}: envmapped ties: {} of {} draws with their shine, {} chunks' normals ({} KB, "
           "in the chunks' meshes)",
           lev->name, used, lev->env_draws.size(), chunks, bytes / 1024);
}

// the PS2's tie geomorph (c3l::ExtMorph), before the meshes: their tails (the MorphVertex
// records, as the envmapped tie normals). Without it the mid versions are never drawn (their
// groups switch between near and far as before)
void load_morph(c3l::Reader& f, const c3l::Header& hdr, CtrLevelData* lev) {
  lev->group_mid.clear();
  lev->morph_insts.clear();
  lev->morph_rows.clear();
  lev->morph_draw_rows.clear();
  lev->morph_chunks.clear();
  lev->chunk_morph.clear();
  lev->draw_morph_rows.clear();
  c3l::ExtHeader eh;
  if (!hdr.ext_offset || lev->lod_groups.empty() || !read_ext_header(f, hdr, &eh) ||
      eh.size < c3l::kExtHeaderMorphSize) {
    return;
  }
  c3l::ExtMorph em{};
  if (!f.read(hdr.ext_offset + c3l::kExtHeaderV24Size, &em, sizeof(em)) || !em.chunk_count) {
    return;  // (a file with the tfrag geomorph only)
  }
  bool ok = f.read_array(em.insts_offset, em.inst_count, &lev->morph_insts) &&
            f.read_array(em.chunks_offset, em.chunk_count, &lev->morph_chunks) &&
            f.read_array(em.rows_offset, em.row_count, &lev->morph_rows) &&
            f.read_array(em.draw_rows_offset, em.draw_rows_size, &lev->morph_draw_rows) &&
            f.read_array(em.group_mid_offset, (u32)lev->lod_groups.size(), &lev->group_mid);
  lev->chunk_morph.assign(lev->chunks.size(), -1);
  lev->draw_morph_rows.assign(lev->draws.size(), UINT32_MAX);
  for (size_t i = 0; ok && i < lev->morph_chunks.size(); i++) {
    const auto& mc = lev->morph_chunks[i];
    ok = mc.chunk < lev->chunks.size() && lev->chunk_morph[mc.chunk] < 0 && mc.records_offset &&
         mc.delta_scale >= 1.f &&
         mc.vertex_count == lev->chunks[mc.chunk].vertex_count &&
         !(mc.chunk < lev->chunk_normals.size() && lev->chunk_normals[mc.chunk] >= 0) &&
         mc.row_count < c3l::kMorphRows && mc.row_first + mc.row_count <= lev->morph_rows.size();
    for (u32 r = 0; ok && r < mc.row_count; r++) {
      const u32 row = lev->morph_rows[mc.row_first + r];
      ok = row < lev->morph_insts.size();
    }
    // its draws' row lists
    const auto& ch = ok ? lev->chunks[mc.chunk] : lev->chunks[0];
    u32 at = mc.draw_rows;
    for (u32 d = ch.first_draw; ok && d < ch.first_draw + ch.draw_count; d++) {
      ok = d < lev->draws.size() && at < lev->morph_draw_rows.size() &&
           at + 1 + lev->morph_draw_rows[at] <= lev->morph_draw_rows.size();
      for (u32 k = 0; ok && k < lev->morph_draw_rows[at]; k++) {
        const u8 r = lev->morph_draw_rows[at + 1 + k];
        ok = r >= 1 && r <= mc.row_count;
      }
      if (ok) {
        lev->draw_morph_rows[d] = at;
        at += 1 + lev->morph_draw_rows[at];
      }
    }
    if (ok) {
      lev->chunk_morph[mc.chunk] = (s32)i;
    }
  }
  if (!ok) {
    lg::warn("[ctr] {}: bad tie geomorph data: the ties switch as before, at rest", lev->name);
    lev->group_mid.clear();
    lev->morph_chunks.clear();
    lev->chunk_morph.clear();
    lev->draw_morph_rows.clear();
    return;
  }
  size_t groups = 0;
  for (const float m : lev->group_mid) {
    groups += m > 0;
  }
  lg::info("[ctr] {}: tie geomorph: {} groups, {} instances, {} near chunks with moving vertices",
           lev->name, groups, lev->morph_insts.size(), lev->morph_chunks.size());
}

// the PS2's tfrag geomorph (c3l::ExtTfrag), before the meshes: their tails (the TfragMorphVertex
// records), the runs' versions. Without it the near tier draws every version of its groups
void load_tfrag_lod(c3l::Reader& f, const c3l::Header& hdr, size_t file_runs, CtrLevelData* lev) {
  lev->tf_groups.clear();
  lev->tf_state.clear();
  lev->tf_chunks.clear();
  lev->chunk_tf.clear();
  lev->tf_colors.clear();
  lev->tf_color_ver.clear();
  lev->tf_color_mask.clear();
  c3l::ExtHeader eh;
  if (!hdr.ext_offset || !read_ext_header(f, hdr, &eh) || eh.size < c3l::kExtHeaderTfragSize) {
    return;
  }
  c3l::ExtTfrag et{};
  std::vector<c3l::RunLod> rl;
  bool ok = f.read(hdr.ext_offset + c3l::kExtHeaderMorphSize, &et, sizeof(et)) && et.chunk_count &&
            f.read_array(et.groups_offset, et.group_count, &lev->tf_groups) &&
            f.read_array(et.chunks_offset, et.chunk_count, &lev->tf_chunks) &&
            f.read_array(et.run_lod_offset, et.run_lod_count, &rl) &&
            f.read_array(et.colors_offset, et.color_count, &lev->tf_colors) &&
            et.dists[0] > 0 && et.dists[1] > et.dists[0] && et.dists[2] > et.dists[1];
  if (!et.chunk_count) {
    return;
  }
  lev->chunk_tf.assign(lev->chunks.size(), -1);
  if (lev->run_lod.size() < lev->vis_runs.size()) {
    lev->run_lod.assign(lev->vis_runs.size(), c3l::RunLod{0xffff, c3l::kRunLodAlways, 0});
  }
  if (lev->chunk_lod.size() != lev->chunks.size()) {
    lev->chunk_lod.assign(lev->chunks.size(), 0);
  }
  for (size_t i = 0; ok && i < lev->tf_chunks.size(); i++) {
    const auto& tc = lev->tf_chunks[i];
    ok = tc.chunk < lev->chunks.size() && lev->chunk_tf[tc.chunk] < 0 &&
         !(tc.chunk < lev->chunk_normals.size() && lev->chunk_normals[tc.chunk] >= 0) &&
         !(tc.chunk < lev->chunk_morph.size() && lev->chunk_morph[tc.chunk] >= 0) &&
         (!tc.records_offset || tc.delta_scale >= 1.f) &&
         tc.color_first + tc.color_count <= lev->tf_colors.size();
    const auto& ch = ok ? lev->chunks[tc.chunk] : lev->chunks[0];
    const u32 r0 = ok ? lev->chunk_first_run[tc.chunk] : 0;
    ok = ok && ch.vis_run_count && tc.run_first + ch.vis_run_count <= rl.size() &&
         r0 + ch.vis_run_count <= file_runs;
    for (u32 c = tc.color_first; ok && c < tc.color_first + tc.color_count; c++) {
      ok = lev->tf_colors[c].vertex < ch.vertex_count &&
           (c == tc.color_first || lev->tf_colors[c].vertex > lev->tf_colors[c - 1].vertex);
    }
    for (u32 r = 0; ok && r < ch.vis_run_count; r++) {
      const auto& x = rl[tc.run_first + r];
      ok = x.lod == c3l::kRunLodAlways || (x.group < lev->tf_groups.size() && x.lod <= 3);
    }
    if (ok) {
      lev->chunk_tf[tc.chunk] = (s32)i;
    }
  }
  if (!ok) {
    lg::warn("[ctr] {}: bad tfrag geomorph data: every version drawn", lev->name);
    lev->tf_groups.clear();
    lev->tf_chunks.clear();
    lev->chunk_tf.clear();
    lev->tf_colors.clear();
    return;
  }
  for (const auto& tc : lev->tf_chunks) {
    const u32 r0 = lev->chunk_first_run[tc.chunk];
    for (u32 r = 0; r < lev->chunks[tc.chunk].vis_run_count; r++) {
      lev->run_lod[r0 + r] = rl[tc.run_first + r];
    }
    lev->chunk_lod[tc.chunk] = 2;
  }
  memcpy(lev->tf_dists, et.dists, sizeof(lev->tf_dists));
  lev->tf_state.assign(lev->tf_groups.size(), 1);
  // each group's near chunks (streaming units holding its near or mid version)
  lev->tf_unit_first.assign(lev->tf_groups.size(), 0);
  lev->tf_unit_count.assign(lev->tf_groups.size(), 0);
  lev->tf_units.clear();
  if (!lev->chunk_unit.empty()) {
    std::vector<std::vector<u32>> gu(lev->tf_groups.size());
    for (const auto& tc : lev->tf_chunks) {
      const u32 u = lev->chunk_unit[tc.chunk];
      if (u == UINT32_MAX) {
        continue;
      }
      const u32 r0 = lev->chunk_first_run[tc.chunk];
      for (u32 r = 0; r < lev->chunks[tc.chunk].vis_run_count; r++) {
        const auto& x = lev->run_lod[r0 + r];
        if ((x.lod == c3l::kRunLodNear || x.lod == c3l::kRunLodMid) &&
            (gu[x.group].empty() || gu[x.group].back() != u)) {
          gu[x.group].push_back(u);
        }
      }
    }
    for (size_t g = 0; g < gu.size(); g++) {
      lev->tf_unit_first[g] = (u32)lev->tf_units.size();
      lev->tf_unit_count[g] = (u32)gu[g].size();
      lev->tf_units.insert(lev->tf_units.end(), gu[g].begin(), gu[g].end());
    }
  }
  lg::info("[ctr] {}: tfrag geomorph: {} groups in {} chunks, {} moving vertices; versions at "
           "{:.1f} / {:.1f} m of view depth",
           lev->name, lev->tf_groups.size(), lev->tf_chunks.size(), lev->tf_colors.size(),
           et.dists[1] / 4096.f, et.dists[2] / 4096.f);
}

// the kDrawCullCond draws' opening planes (c3l::ExtCullCond); without them those
// draws keep both sides
void load_cull_cond(c3l::Reader& f, const c3l::Header& hdr, CtrLevelData* lev) {
  lev->cond_draws.clear();
  lev->cond_planes.clear();
  c3l::ExtHeader eh;
  if (!hdr.ext_offset || !read_ext_header(f, hdr, &eh) || eh.size < c3l::kExtHeaderV24Size) {
    return;
  }
  c3l::ExtCullCond ec{};
  std::vector<c3l::CullCondDraw> cds;
  if (!f.read(hdr.ext_offset + c3l::kExtHeaderTexKeySize, &ec, sizeof(ec)) || !ec.draw_count ||
      ec.draw_count > lev->draws.size() || !f.read_array(ec.draws_offset, ec.draw_count, &cds)) {
    return;
  }
  for (const auto& cd : cds) {
    std::vector<c3l::CullCondPlane> ps;
    if (cd.draw >= lev->draws.size() || !(lev->draws[cd.draw].kind & c3l::kDrawCullCond) ||
        !cd.plane_count || !f.read_array(cd.planes_offset, cd.plane_count, &ps)) {
      lg::warn("[ctr] {}: bad opening planes, those draws keep both sides", lev->name);
      lev->cond_draws.clear();
      lev->cond_planes.clear();
      return;
    }
    lev->cond_draws.push_back({cd.draw, (u32)lev->cond_planes.size(), cd.plane_count});
    lev->cond_planes.insert(lev->cond_planes.end(), ps.begin(), ps.end());
  }
  lg::info("[ctr] {}: culling by opening planes: {} draws, {} planes", lev->name,
           lev->cond_draws.size(), lev->cond_planes.size());
}

// shrub fade bands (c3l::ExtShrubFade): per file run; a shrub chunk with bands reaches its largest
// w (its max_dist is decide_dist's class, so w / kShrubFarK)
void load_shrub_fade(c3l::Reader& f,
                     const c3l::Header& hdr,
                     u32 runs_lo,
                     size_t file_runs,
                     CtrLevelData* lev) {
  lev->run_band.clear();
  lev->chunk_band_x.clear();
  c3l::ExtHeader eh;
  if (!hdr.ext_offset || !lev->shrub_ps2_dist || !file_runs || runs_lo == UINT32_MAX ||
      !read_ext_header(f, hdr, &eh) || eh.size < c3l::kExtHeaderV23Size ||
      runs_lo < eh.runs_offset || (runs_lo - eh.runs_offset) % sizeof(c3l::VisRun)) {
    return;
  }
  c3l::ExtShrubFade ef{};
  const size_t first = (runs_lo - eh.runs_offset) / sizeof(c3l::VisRun);
  std::vector<u32> bands;
  if (!f.read(hdr.ext_offset + c3l::kExtHeaderV22Size, &ef, sizeof(ef)) || !ef.bands_offset ||
      first + file_runs > ef.band_count ||
      !f.read_array(ef.bands_offset + (u32)(first * 4), file_runs, &bands)) {
    return;
  }
  bands.resize(lev->vis_runs.size(), 0);
  lev->chunk_band_x.assign(lev->chunks.size(), 0.f);
  u32 chunks = 0;
  for (size_t ci = 0; ci < lev->chunks.size(); ci++) {
    auto& ch = lev->chunks[ci];
    const u32 r0 = lev->chunk_first_run[ci];
    if (ch.kind != c3l::CHUNK_KIND_SHRUB || !ch.vis_run_count || r0 + ch.vis_run_count > file_runs) {
      continue;
    }
    float w = 0, x = INFINITY;
    for (u32 r = r0; r < r0 + ch.vis_run_count; r++) {
      if (bands[r]) {
        w = std::max(w, (float)(bands[r] & 0xffff));
        x = std::min(x, (float)(bands[r] >> 16));
      }
    }
    if (w > 0) {
      ch.max_dist = w * 256.f / kShrubFarK;
      lev->chunk_band_x[ci] = x * 256.f;
      chunks++;
    }
  }
  lev->run_band = std::move(bands);
  lg::info("[ctr] {}: shrub fade bands: {} chunks", lev->name, chunks);
}

// wind ties (c3l::ExtWind): the instances and the slots of their chunks' vertices (all in one
// mesh's vertex memory, loaded with the level)
void load_wind_ties(c3l::Reader& f, const c3l::Header& hdr, CtrLevelData* lev) {
  lev->wind_insts.clear();
  lev->chunk_wind.clear();
  lev->wind_mesh = -1;
  if (!hdr.ext_offset || !ctr_gpu_wind_ready()) {
    return;
  }
  c3l::ExtHeader eh;
  if (!read_ext_header(f, hdr, &eh) || eh.size < c3l::kExtHeaderV22Size) {
    return;
  }
  c3l::ExtWind ew{};
  if (!f.read(hdr.ext_offset + c3l::kExtHeaderV21Size, &ew, sizeof(ew)) || !ew.inst_count ||
      !ew.chunk_count) {
    return;
  }
  std::vector<c3l::WindInst> insts;
  std::vector<c3l::WindChunk> wcs;
  if (ew.inst_count > 65535 || !f.read_array(ew.insts_offset, ew.inst_count, &insts) ||
      !f.read_array(ew.chunks_offset, ew.chunk_count, &wcs)) {
    lg::warn("[ctr] {}: bad wind tie data, at rest", lev->name);
    return;
  }
  u32 total = 0, slots = 0;
  for (const auto& wc : wcs) {
    if (wc.chunk >= lev->chunks.size() || wc.slot_count > c3l::kWindSlots) {
      lg::warn("[ctr] {}: bad wind tie chunk, at rest", lev->name);
      return;
    }
    total += (lev->chunks[wc.chunk].vertex_count + 15) & ~15u;
    slots += wc.slot_count;
  }
  std::vector<u16> slot_inst(slots);
  std::vector<u8> draw_wind(lev->draws.size(), 0);
  lev->chunk_wind_first.assign(lev->chunks.size(), 0);
  lev->chunk_wind_n.assign(lev->chunks.size(), 0);
  u32 at = 0;
  for (const auto& wc : wcs) {
    const auto& ch = lev->chunks[wc.chunk];
    if (ch.first_draw + ch.draw_count > draw_wind.size() ||
        !f.read(wc.draws_offset, draw_wind.data() + ch.first_draw, ch.draw_count) ||
        !f.read(wc.insts_offset, slot_inst.data() + at, wc.slot_count * 2)) {
      lg::warn("[ctr] {}: bad wind tie slots, at rest", lev->name);
      return;
    }
    for (u32 k = 0; k < wc.slot_count; k++) {
      if (slot_inst[at + k] >= insts.size()) {
        lg::warn("[ctr] {}: bad wind tie slots, at rest", lev->name);
        return;
      }
    }
    lev->chunk_wind_first[wc.chunk] = at;
    lev->chunk_wind_n[wc.chunk] = (u8)wc.slot_count;
    at += wc.slot_count;
  }
  void* nv = nullptr;
  uint16_t* ni = nullptr;
  const int mesh = total ? ctr_gpu_mesh_begin((int)((total + 15) / 16), 2, &nv, &ni) : -1;
  if (mesh < 0) {
    lg::warn("[ctr] {}: no memory for the wind ties' slots ({} KB), at rest", lev->name,
             total / 1024);
    return;
  }
  lev->chunk_wind.assign(lev->chunks.size(), -1);
  u32 off = 0;
  bool ok = true;
  for (const auto& wc : wcs) {
    const u32 n = lev->chunks[wc.chunk].vertex_count;
    ok = ok && f.read(wc.slots_offset, (u8*)nv + off, n);
    lev->chunk_wind[wc.chunk] = (s32)off;
    off += (n + 15) & ~15u;
  }
  ni[0] = ni[1] = 0;
  if (!ok || ctr_gpu_mesh_end(mesh, 1) < 0) {
    if (!ok) {
      ctr_gpu_mesh_end(mesh, 0);
    }
    lg::warn("[ctr] {}: wind tie slots not made, at rest", lev->name);
    lev->chunk_wind.clear();
    return;
  }
  u32 max_idx = 0;
  for (const auto& wi : insts) {
    max_idx = std::max<u32>(max_idx, wi.wind_idx);
  }
  lev->wind_mesh = mesh;
  lev->wind_slot_inst = std::move(slot_inst);
  lev->draw_wind = std::move(draw_wind);
  lev->wind_insts = std::move(insts);
  lev->wind_vectors.assign(4 * (size_t)max_idx + 4, 0.f);
  lev->wind_k.assign(2 * lev->wind_insts.size(), 0.f);
  lev->wind_frame = ~0ull;
  lg::info("[ctr] {}: wind ties: {} instances, {} chunks' slots ({} KB)", lev->name,
           lev->wind_insts.size(), wcs.size(), total / 1024);
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
    // (a tfrag one: the tfrag geomorph's near and mid versions, c3l::ExtTfrag)
    if (c.chunk >= lev->chunks.size() || lev->chunk_unit[c.chunk] != UINT32_MAX ||
        (lev->chunks[c.chunk].kind != c3l::CHUNK_KIND_TIE &&
         lev->chunks[c.chunk].kind != c3l::CHUNK_KIND_TFRAG) ||
        !lev->chunks[c.chunk].vis_run_count ||
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
        if (ch.kind == c3l::CHUNK_KIND_TFRAG) {
          continue;  // (its groups: load_tfrag_lod)
        }
        for (u32 r = lev->chunk_first_run[ci]; r < lev->chunk_first_run[ci] + ch.vis_run_count;
             r++) {
          const auto& rl = lev->run_lod[r];
          if ((rl.lod != c3l::kRunLodNear && rl.lod != c3l::kRunLodMid) || rl.group >= ng) {
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
    // (AI-assisted, lodnear) each unit's largest PS2 far-plane (the switch's floor)
    for (size_t u = 0; u < lev->stream_units.size(); u++) {
      float d = 0;
      for (u32 j = lev->unit_group_first[u]; j < lev->unit_group_first[u + 1]; j++) {
        d = std::max(d, lev->lod_groups[lev->unit_groups[j]].dist);
      }
      lev->stream_units[u].need_d = d;
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

// (AI-assisted) run_culling: the bounding spheres of chunk ci's runs from its vertices (as read
// from the file) and the file's indices (the draws' first_index still into all of them). The
// center of the run's bounding box and half its diagonal, in the chunk's quantized units.
// (AI-assisted) indices: the file's indices [idx_first, idx_end) (the chunk's), indices[0] is index
// idx_first
void make_run_spheres(CtrLevelData* out,
                      u32 ci,
                      const u8* rec,
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
        s16 pos[3];
        if (ok) {
          memcpy(pos, rec + 12 * v, sizeof(pos));
        }
        for (int c = 0; ok && c < 3; c++) {
          mn[c] = std::min<int>(mn[c], pos[c]);
          mx[c] = std::max<int>(mx[c], pos[c]);
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
      cl.raw[0] = std::max(cl.raw[0], err && !lev.morph_group(g) ? lev.lod_error[g] : 0.f);
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

// A level's texture pools: VRAM first by the level's priority, within it the earlier pieces (the
// background's textures) first.
constexpr u32 kTexPoolPiece = 256 * 1024;
void set_tex_priority(const std::vector<int>& pools, int priority) {
  for (size_t i = 0; i < pools.size(); i++) {
    ctr_gpu_pool_set_priority(pools[i], priority * 16 + 15 - (int)std::min<size_t>(i, 15));
  }
}

// All textures (with their mip levels) in pools of at most kTexPoolPiece (a texture bigger than
// that in one of its own): the renderer moves each to VRAM when there's room, with one GPU copy.
// The texels are read straight into them. (AI-assisted) The textures of the background (tfrag,
// tie, shrub) come first, then the ones only merc models (characters) use, which were 75-95% of a
// level's textures and came first in the file. They are read in file order all the same.
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
    u32 offset = 0;       // in its pool
    u32 piece = 0;        // its pool
  };
  std::vector<Plan> plan(texs.size());
  const bool rgba4_as_rgba8 = ctr_gpu_rgba4_as_rgba8() != 0;
  for (size_t i = 0; i < texs.size(); i++) {
    const auto& t = texs[i];
    auto& p = plan[i];
    const bool pow2 = t.w >= 8 && t.h >= 8 && t.w <= 1024 && t.h <= 1024 &&
                      !(t.w & (t.w - 1)) && !(t.h & (t.h - 1));
    const u8 max_format = hdr.version >= c3l::kTexMaskVersion ? c3l::TEX_A4 : c3l::TEX_RGBA8;
    if (!pow2 || t.format > max_format || (hdr.version < 8 && t.format > c3l::TEX_RGBA4)) {
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
  // c3l v23: an alpha mask with its texture
  std::vector<int> mask_idx(texs.size(), -1);
  for (size_t i = 0; i < texs.size(); i++) {
    if (c3l::texture_is_mask(texs[i]) && c3l::texture_mask_of(texs[i]) < texs.size()) {
      background[i] = background[c3l::texture_mask_of(texs[i])];
      mask_idx[c3l::texture_mask_of(texs[i])] = (int)i;
    }
  }
  out->handles.assign(texs.size(), -1);
  out->min_alpha.assign(texs.size(), 0);
  out->shared.assign(texs.size(), 0);
  // c3l v23: the textures other levels have too (texshare.c3t), by their content keys: a group
  // another loaded level holds is used as it is, one nobody holds is loaded here into a pool of its
  // own (group_of), for the levels that come later
  std::vector<int> group_of(texs.size(), -1);
  std::vector<u64> keys;
  {
    c3l::ExtHeader eh;
    c3l::ExtTexKey tk{};
    if (hdr.version >= c3l::kTexMaskVersion && read_ext_header(f, hdr, &eh) &&
        eh.size >= c3l::kExtHeaderTexKeySize &&
        f.read(hdr.ext_offset + c3l::kExtHeaderV23Size, &tk, sizeof(tk)) &&
        tk.count == texs.size() && !f.read_array(tk.offset, tk.count, &keys)) {
      keys.clear();
    }
  }
  std::vector<u32> creating;
  if (keys.size() == texs.size()) {
    std::lock_guard<std::mutex> lk(m_share_lock);
    std::unordered_set<u64> placed;  // a texture twice in the file: the copy stays the level's
    for (size_t i = 0; i < texs.size(); i++) {
      const int g = plan[i].ok && keys[i] && !c3l::texture_is_mask(texs[i])
                        ? share_group_of(keys[i])
                        : -1;
      if (g < 0) {
        continue;
      }
      auto& grp = m_share_groups[(u32)g];
      if (std::find(out->share_groups.begin(), out->share_groups.end(), (u32)g) ==
          out->share_groups.end()) {
        out->share_groups.push_back((u32)g);
        grp.users++;
        if (!grp.loading && grp.pools.empty() && grp.tex.empty()) {
          grp.loading = true;
          creating.push_back((u32)g);
        }
      }
      const int m = mask_idx[i];
      auto it = grp.tex.find(keys[i]);
      if (it != grp.tex.end()) {
        out->handles[i] = it->second.handle;
        out->min_alpha[i] = it->second.min_alpha;
        out->shared[i] = 1;
        out->borrowed_bytes += plan[i].file_bytes;
        plan[i].ok = false;
        if (m >= 0) {
          out->handles[m] = it->second.mask;
          out->shared[m] = 1;
          out->borrowed_bytes += plan[m].file_bytes;
          plan[m].ok = false;
        }
      } else if (std::find(creating.begin(), creating.end(), (u32)g) != creating.end() &&
                 placed.insert(keys[i]).second) {
        group_of[i] = g;
        if (m >= 0) {
          group_of[m] = g;
        }
      }
    }
  }
  u32 pool_bytes = 0;
  std::vector<u32> piece_bytes;
  std::vector<int> piece_group;  // -1: the level's own piece, else a share group's pool
  for (int pass = 0; pass < 2; pass++) {
    for (size_t i = 0; i < texs.size(); i++) {
      auto& p = plan[i];
      if (p.ok && group_of[i] < 0 && background[i] == (pass == 0)) {
        const u32 b = (ctr_gpu_tex_bytes(texs[i].w, texs[i].h, p.format, p.levels) + 127) & ~127u;
        if (piece_bytes.empty() || piece_bytes.back() + b > kTexPoolPiece) {
          piece_bytes.push_back(0);
          piece_group.push_back(-1);
        }
        p.piece = (u32)piece_bytes.size() - 1;
        p.offset = piece_bytes.back();
        piece_bytes.back() += b;
        pool_bytes += b;
      }
    }
    if (pass == 0) {
      out->background_bytes = pool_bytes;
    }
  }
  out->pool_bytes = pool_bytes;
  for (const u32 g : creating) {
    bool any = false;
    for (size_t i = 0; i < texs.size(); i++) {
      auto& p = plan[i];
      if (p.ok && group_of[i] == (int)g) {
        const u32 b = (ctr_gpu_tex_bytes(texs[i].w, texs[i].h, p.format, p.levels) + 127) & ~127u;
        if (!any || piece_bytes.back() + b > kTexPoolPiece) {
          piece_bytes.push_back(0);
          piece_group.push_back((int)g);
          any = true;
        }
        p.piece = (u32)piece_bytes.size() - 1;
        p.offset = piece_bytes.back();
        piece_bytes.back() += b;
        out->shared_bytes += b;
      }
    }
  }
  std::vector<int> all_pools;
  std::vector<u8*> bases;
  // the groups this load makes are given up (failed or cancelled): a later load makes them
  auto drop_creating = [&]() {
    for (size_t i = 0; i < texs.size(); i++) {
      if (group_of[i] >= 0 && out->handles[i] >= 0) {
        ctr_gpu_tex_delete(out->handles[i]);
        out->handles[i] = -1;
      }
    }
    std::lock_guard<std::mutex> lk(m_share_lock);
    for (size_t k = 0; k < all_pools.size(); k++) {
      if (piece_group[k] >= 0) {
        ctr_gpu_pool_delete(all_pools[k]);  // after its textures
      }
    }
    for (const u32 g : creating) {
      m_share_groups[g].loading = false;
      m_share_groups[g].pools.clear();
    }
    creating.clear();
  };
  for (const u32 b : piece_bytes) {
    int pool = ctr_gpu_pool_create(b);
    while (pool < 0 && make_room(name, "the textures", b)) {
      pool = ctr_gpu_pool_create(b);
    }
    if (pool < 0) {
      lg::error("[ctr] {}: textures: pool not made, no linear memory ({} KB, a piece of {} KB; {})",
                name, (pool_bytes + out->shared_bytes) / 1024, b / 1024, linear_room_text());
      for (int q : all_pools) {
        ctr_gpu_pool_delete(q);
      }
      all_pools.clear();
      bases.clear();
      drop_creating();
      break;
    }
    all_pools.push_back(pool);
    bases.push_back((u8*)ctr_gpu_pool_data(pool));
  }
  for (size_t k = 0; k < all_pools.size(); k++) {
    if (piece_group[k] < 0) {
      out->pools.push_back(all_pools[k]);
    } else {
      std::lock_guard<std::mutex> lk(m_share_lock);
      m_share_groups[(u32)piece_group[k]].pools.push_back(all_pools[k]);
      ctr_gpu_pool_set_priority(all_pools[k], 2 * 16 + 15);
    }
  }
  std::vector<u8> texels;
  for (size_t i = 0; i < texs.size(); i++) {
    if (cancelled()) {
      drop_creating();
      return false;
    }
    const auto& t = texs[i];
    const auto& p = plan[i];
    if (!p.ok || bases.empty()) {
      continue;
    }
    const int pool = all_pools[p.piece];
    u8* dst = bases[p.piece] + p.offset;
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
      if (const u16 anim_slot = c3l::texture_is_mask(t) ? 0 : c3l::texture_anim_slot(t)) {
        ctr_gpu_tex_set_anim_slot(out->handles[i], anim_slot - 1);
      }
      out->min_alpha[i] = (u8)texture_min_alpha(dst, t.w, t.h, p.format);
    }
  }
  // c3l v23: each texture's alpha mask, whose alpha its draws take
  for (size_t i = 0; i < texs.size(); i++) {
    const u16 c = c3l::texture_mask_of(texs[i]);
    if (c3l::texture_is_mask(texs[i]) && c < texs.size() && out->handles[i] >= 0 &&
        out->handles[c] >= 0 && !out->shared[c]) {
      ctr_gpu_tex_set_mask(out->handles[c], out->handles[i]);
      out->min_alpha[c] = out->min_alpha[i];
    }
  }
  // the share groups loaded here, for the levels that load later
  if (!creating.empty()) {
    std::lock_guard<std::mutex> lk(m_share_lock);
    for (size_t i = 0; i < texs.size(); i++) {
      if (group_of[i] < 0 || c3l::texture_is_mask(texs[i]) || out->handles[i] < 0) {
        continue;
      }
      const int m = mask_idx[i];
      if (m >= 0 && out->handles[m] < 0) {
        continue;
      }
      auto& st = m_share_groups[(u32)group_of[i]].tex[keys[i]];
      st.handle = out->handles[i];
      st.mask = m >= 0 ? out->handles[m] : -1;
      st.min_alpha = out->min_alpha[i];
      out->shared[i] = 1;
      if (m >= 0) {
        out->shared[m] = 1;
      }
    }
    for (const u32 g : creating) {
      m_share_groups[g].loading = false;
    }
  }
  for (int pool : all_pools) {
    ctr_gpu_pool_ready(pool);
  }
  out->loaded = piece_bytes.empty() || !bases.empty();
  return true;
}

int CtrLevels::share_group_of(u64 key) {
  if (!m_share_read) {
    m_share_read = true;
    const auto path = ctr_level_c3l_dir() / "texshare.c3t";
    if (FILE* fp = fopen(path.string().c_str(), "rb")) {
      c3l::TexShareHeader h{};
      if (fread(&h, sizeof(h), 1, fp) == 1 && !memcmp(h.magic, c3l::kTexShareMagic, 4) &&
          h.version == 1 && h.count < (1u << 20)) {
        m_share_table.resize(h.count);
        if (h.count && fread(m_share_table.data(), sizeof(c3l::TexShareEntry), h.count, fp) !=
                           h.count) {
          m_share_table.clear();
        }
      }
      fclose(fp);
      lg::info("[ctr] texshare.c3t: {} textures in {} levels' share groups", m_share_table.size(),
               m_share_table.empty() ? 0 : 1 + std::max_element(m_share_table.begin(),
                                                                m_share_table.end(),
                                                                [](const auto& a, const auto& b) {
                                                                  return a.group < b.group;
                                                                })->group);
    }
  }
  auto it = std::lower_bound(m_share_table.begin(), m_share_table.end(), key,
                             [](const c3l::TexShareEntry& e, u64 k) { return e.key < k; });
  return it != m_share_table.end() && it->key == key ? (int)it->group : -1;
}

void CtrLevels::release_share_groups(const std::vector<u32>& groups) {
  std::lock_guard<std::mutex> lk(m_share_lock);
  for (const u32 g : groups) {
    auto it = m_share_groups.find(g);
    if (it == m_share_groups.end() || --it->second.users) {
      continue;
    }
    for (const auto& [key, t] : it->second.tex) {
      ctr_gpu_tex_delete(t.handle);
      if (t.mask >= 0) {
        ctr_gpu_tex_delete(t.mask);
      }
    }
    for (int pool : it->second.pools) {
      ctr_gpu_pool_delete(pool);  // after its textures
    }
    m_share_groups.erase(it);
  }
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
  if (hdr.ext_offset) {
    u32 magic_size[2];
    if (f.read(hdr.ext_offset, magic_size, sizeof(magic_size)) &&
        !memcmp(magic_size, c3l::kExtMagic, 4)) {
      f.keep(hdr.ext_offset, magic_size[1]);
    }
  }
  out->name = name;
  out->shrub_ps2_dist = hdr.version >= c3l::kShrubPs2DistVersion;  // (AI-assisted)
  if (!f.read_array(hdr.chunks_offset, hdr.num_chunks, &out->chunks) ||
      !f.read_array(hdr.draw_data_offset, hdr.draw_data_size / sizeof(c3l::Draw), &out->draws)) {
    lg::error("[ctr] {}: truncated file", path.string());
    return false;
  }
  // (AI-assisted, levellod) chunk kind 5 was the untextured copy of the vista tier (the converter's
  // --vista-flat and config vista_untextured, removed Oct 29: no shipped file had one); such chunks
  // of an older file are never drawn
  for (auto& ch : out->chunks) {
    if (ch.kind == 5) {
      ch.kind = c3l::CHUNK_KIND_UNKNOWN;
      ch.draw_count = 0;
      ch.vis_run_count = 0;
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
  // meshes below). (AI-assisted, levellod) made whatever run_culling says (it gates only the view
  // test, draw_level): the shrubs' distances run by run and the v19 spheres need them too, so
  // run_culling = off at boot also changed those (an A/B of it measured more than the view test)
  out->chunk_run_count.assign(out->chunks.size(), 0);
  {
    constexpr u32 kCullRunIndices = 96 * 3;
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
  load_shrub_fade(f, hdr, runs_lo, file_runs, out);
  load_proto_vis(f, hdr, runs_lo, file_runs, out);  // (AI-assisted, j2vis) c3l v20
  out->streaming = !out->stream_units.empty() && out->stream_units[0].state != CtrLevelData::kUnitNever &&
                   ctr_settings().streaming &&
                   CtrStreamArena::get().init(ctr_stream_kb() * 1024u);

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
    out->tex_pools = std::move(tl.pools);
    out->tex_missing = !tl.loaded;
    out->tex_pool_bytes = tl.loaded ? tl.pool_bytes : tl.pool_bytes + tl.shared_bytes;
    out->tex_background_bytes = tl.background_bytes;
    out->tex_empty = tl.empty;
    out->textures = std::move(tl.handles);
    out->tex_min_alpha = std::move(tl.min_alpha);
    out->tex_shared = std::move(tl.shared);
    out->tex_share_groups = std::move(tl.share_groups);
    out->tex_shared_bytes = tl.loaded ? tl.shared_bytes : 0;
    if (tl.shared_bytes || tl.borrowed_bytes) {
      lg::info("[ctr] {}: shared textures: {} KB loaded for the levels that also have them, {} KB "
               "from the loaded ones",
               name, tl.shared_bytes / 1024, tl.borrowed_bytes / 1024);
    }
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
  // (AI-assisted, j2strip) Jak II: meshes that don't fit the free linear memory (with
  // kDistLoadMargin, the merc models' too): the chunks far beyond their draw distance are left for
  // later, read back by distance ("Distance freeing")
  load_env_tie_chunks(f, hdr, out);  // before the meshes: their tails
  load_morph(f, hdr, out);
  load_tfrag_lod(f, hdr, file_runs, out);
  std::vector<u8> dist_skip;
  if (g_game_version == GameVersion::Jak2) {
    auto chunk_range = [&](const c3l::Chunk& ch, u32* first, u32* end) {
      *first = UINT32_MAX;
      *end = 0;
      for (u32 d = ch.first_draw; d < ch.first_draw + ch.draw_count; d++) {
        *first = std::min<u32>(*first, out->draws[d].first_index);
        *end = std::max<u32>(*end, out->draws[d].first_index + out->draws[d].index_count);
      }
      return *first != UINT32_MAX;
    };
    u64 need = (u64)hdr.merc_vertex_size + hdr.merc_index_size;
    for (size_t ci = 0; ci < out->chunks.size(); ci++) {
      u32 first, end;
      if (chunk_range(out->chunks[ci], &first, &end) && !(out->is_unit(ci) && out->streaming)) {
        need += CtrStreamArena::mesh_bytes(out->chunks[ci].vertex_count, end - first) +
                out->normals_tail_bytes(ci);
      }
    }
    const u64 free_now = ctr_gpu_linear_free();
    out->dist_free = true;  // (freed in play by distance too: dist_update)
    out->chunk_dormant.assign(out->chunks.size(), 0);
    out->chunk_hold_since.assign(out->chunks.size(), 0);
    if (need + kDistLoadMargin > free_now) {
      float pos[4][3];
      int np;
      {
        std::lock_guard<std::mutex> lk(m_lock);
        memcpy(pos, m_dist_snap, sizeof(pos));
        np = m_dist_snap_n;
      }
      dist_skip.assign(out->chunks.size(), 0);
      u64 skipped = 0;
      u32 n = 0;
      for (size_t ci = 0; ci < out->chunks.size(); ci++) {
        u32 first, end;
        const auto& ch = out->chunks[ci];
        if (chunk_range(ch, &first, &end) && dist_frees(*out, ci) &&
            dist_beyond(*out, ch, pos, np, kDistFreeMeters)) {
          dist_skip[ci] = 1;
          skipped += CtrStreamArena::mesh_bytes(ch.vertex_count, end - first);
          n++;
        }
      }
      lg::info("[ctr] {}: linear memory short (meshes {} KB, free {} KB): {} chunks ({} KB) far "
               "beyond their draw distance{} left for later, read back by distance",
               name, need / 1024, free_now / 1024, n, skipped / 1024,
               np ? "" : " (no camera yet: all of them)");
    }
  }
  u32 stream_skipped = 0, stream_core_kb = 0;
  std::map<std::string, std::pair<u32, u64>> not_made;  // section: chunks, bytes
  // per chunk: kLeaveLowres the low resolution tfrag of a level with a vista tier (never drawn:
  // its vista copies stand in for it, draw_level), kLeaveTitle what the Jak 1 title's camera can't
  // show ("Title screen")
  constexpr u8 kLeaveLowres = 1, kLeaveTitle = 2;
  std::vector<u8> leave(out->chunks.size(), 0);
  {
    bool vista = false;
    for (const auto& ch : out->chunks) {
      vista = vista || ch.kind == c3l::CHUNK_KIND_VISTA;
    }
    const bool title = g_game_version == GameVersion::Jak1 && name != "title" &&
                       m_title_on.load(std::memory_order_relaxed);
    if (title) {
      out->chunk_dormant.assign(out->chunks.size(), 0);
      out->chunk_hold_since.assign(out->chunks.size(), 0);
      out->title_pass = true;
    }
    for (size_t ci = 0; ci < out->chunks.size(); ci++) {
      const auto& ch = out->chunks[ci];
      if (out->is_unit(ci) || !ch.draw_count) {
        continue;
      }
      if (vista && ch.lod_tier == 3) {
        leave[ci] = kLeaveLowres;
      } else if (title && !title_needed(*out, ci)) {
        leave[ci] = kLeaveTitle;
      }
    }
  }
  u32 title_n = 0;
  u64 title_bytes = 0;
  // (AI-assisted, j2pop) make_room: the meshes still to be made from chunk ci on (the near chunks
  // streamed and those left for later not counted), the merc models'
  auto load_left_bytes = [&](size_t ci) {
    u64 b = (u64)hdr.merc_vertex_size + hdr.merc_index_size;
    for (size_t k = ci; k < out->chunks.size(); k++) {
      const auto& c = out->chunks[k];
      u32 lo = UINT32_MAX, hi = 0;
      for (u32 d = c.first_draw; d < c.first_draw + c.draw_count; d++) {
        lo = std::min<u32>(lo, out->draws[d].first_index);
        hi = std::max<u32>(hi, out->draws[d].first_index + out->draws[d].index_count);
      }
      if (lo != UINT32_MAX && !(out->is_unit(k) && out->streaming) &&
          !(k < dist_skip.size() && dist_skip[k]) && !leave[k]) {
        b += CtrStreamArena::mesh_bytes(c.vertex_count, hi - lo);
      }
    }
    return b;
  };
  for (auto& ch : out->chunks) {
    if (cancelled()) {
      return false;
    }
    serve_streams_in_load();
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
    out->mesh_index_first.push_back(leave[ci] == kLeaveLowres ? UINT32_MAX : first);
    out->mesh_index_end.push_back(leave[ci] == kLeaveLowres ? 0 : end);
    if (leave[ci] == kLeaveTitle) {
      out->chunk_dormant[ci] = CtrLevelData::kTitleFreed;
      title_n++;
      title_bytes += CtrStreamArena::mesh_bytes(ch.vertex_count, end - first) +
                     out->normals_tail_bytes(ci);
    }
    // (AI-assisted, streaming) a near chunk: read later (streaming) or never (the ones the v18
    // budget left out, without streaming); the core ones without streaming now
    bool skip = false;
    if (out->is_unit(ci)) {
      auto& su = out->stream_units[out->chunk_unit[ci]];
      su.bytes = CtrStreamArena::mesh_bytes(ch.vertex_count, end - first) +
                 out->normals_tail_bytes(ci);
      const u32 vfile = hdr.vertex_data_offset + ch.first_vertex * (u32)sizeof(c3l::Vertex);
      const u32 vbytes = ch.vertex_count * (u32)sizeof(c3l::Vertex);
      su.packed = packed_of(vfile, vbytes + (out->tail_file(ci) == vfile + vbytes
                                                 ? out->normals_tail_bytes(ci)
                                                 : 0)) +
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
    const bool dskip = !dist_skip.empty() && dist_skip[ci];  // (AI-assisted, j2strip)
    if (dskip) {
      out->chunk_dormant[ci] = CtrLevelData::kDistFreed;
    }
    const int ntail = (int)out->normals_tail_bytes(ci);
    if (end <= index_total && !skip && !dskip && !leave[ci]) {
      mesh = ctr_gpu_mesh_begin_tail(ch.vertex_count, ntail, end - first, &vdst, &idst);
      while (mesh < 0 && make_room(name, "meshes", load_left_bytes(ci))) {
        mesh = ctr_gpu_mesh_begin_tail(ch.vertex_count, ntail, end - first, &vdst, &idst);
      }
      if (mesh < 0 && !cancelled()) {
        auto& nm = not_made[chunk_section(ch)];
        nm.first++;
        nm.second += CtrStreamArena::mesh_bytes(ch.vertex_count, end - first);
      }
    }
    if (mesh >= 0) {
      MeshBuild mb(mesh);  // (AI-assisted, streammem)
      ctr_gpu_mesh_split_colors(mesh);
      bool read_ok = true;
      if (chunk_indices) {
        read_ok = f.read(hdr.index_data_offset + first * (u32)sizeof(u16), idst,
                         (end - first) * sizeof(u16));
      } else {
        memcpy(idst, indices.data() + first, (end - first) * sizeof(u16));
      }
      read_ok = read_ok &&
                read_split_vertices(f, hdr.vertex_data_offset + ch.first_vertex * sizeof(c3l::Vertex),
                                    ch.vertex_count, vdst);
      read_ok = read_ok && (!ntail || f.read(out->tail_file(ci),
                                             (u8*)vdst + ch.vertex_count * 16, ntail));
      if (read_ok && !out->run_spheres.empty()) {
        // (AI-assisted, streaming) host check: the v19 file's spheres are these
        std::vector<CtrLevelData::RunSphere> file_sp;
        const u32 r0 = out->chunk_first_run[ci], nr = out->chunk_run_count[ci];
        if (v19 && stream_check_enabled() && ch.vis_run_count) {
          file_sp.assign(out->run_spheres.begin() + r0, out->run_spheres.begin() + r0 + nr);
        }
        make_run_spheres(out, (u32)out->meshes.size(), (const u8*)vdst, idst, first, end);
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
        const u8* const va =
            (const u8*)vdst + ctr_gpu_mesh_color_offset((int)ch.vertex_count) + 3;
        bool all = true;
        for (u32 v = 0; v < ch.vertex_count && all; v++) {
          all = va[4 * v] == 0x80;
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
              ok = ix < ch.vertex_count && va[4 * ix] == 0x80;
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
        const TodVerts vv{(u8*)vdst,
                          (tod_word*)((u8*)vdst + ctr_gpu_mesh_color_offset((int)ch.vertex_count))};
        tod_mask = tod_scan(t, vv, (int)ch.vertex_count, &lights);
        tod_state = tod_mask ? 2 : 0;
        if (tod_mask) {
          tod_mark_plain(t, (u32)out->meshes.size(), vv, (int)ch.vertex_count);
        }
        if (!lights.empty()) {
          t.chunk_lights[(u32)out->meshes.size()] = std::move(lights);
        }
        if (tod_state == 2 && t.blended_valid) {
          tod_color_vertices(t, vv, 0, (int)ch.vertex_count);
          tod_state = 1;
        }
      }
      mesh = mb.end(read_ok);
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
            t.groups[g].lver = t.ver;
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
  out->title_left = title_n;
  if (title_n) {
    lg::info("[ctr] {}: title screen: {} chunks its camera can't show left out ({} KB; read when "
             "the title is over)",
             name, title_n, title_bytes / 1024);
  }
  for (const auto& [sec, nm] : not_made) {
    lg::error("[ctr] {}: {}: {} meshes not made, no linear memory ({} KB needed; {})", name, sec,
              nm.first, nm.second / 1024, linear_room_text());
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
      if (tfrag && ch.lod_tier < 3) {
        out->detail_radius = std::max(out->detail_radius, ch.bsphere[3]);
      }
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
  load_env_ties(f, hdr, out);  // (AI-assisted, j2water2) c3l v21 (after the textures, draw order)
  load_wind_ties(f, hdr, out);
  load_cull_cond(f, hdr, out);
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
    // (AI-assisted, j1ocean) none for the pickups (c3l::merc_gen_lod_exempt: always full detail)
    std::unordered_map<std::string, u32> gen_of;
    bool any_gen = false;
    const size_t gen_suffix_len = strlen(c3l::kMercGenLodSuffix);
    for (u32 mi = 0; mi < models.size(); mi++) {
      const std::string n(models[mi].name, strnlen(models[mi].name, sizeof(models[mi].name)));
      if (n.size() > gen_suffix_len &&
          n.compare(n.size() - gen_suffix_len, gen_suffix_len, c3l::kMercGenLodSuffix) == 0) {
        any_gen = true;
        if (!c3l::merc_gen_lod_exempt(n)) {
          gen_of[n.substr(0, n.size() - gen_suffix_len)] = mi;
        }
      }
    }
    out->merc_models.reserve(models.size());  // (AI-assisted) no copies while it grows
    for (u32 mi = 0; mi < models.size(); mi++) {
      const auto& m = models[mi];
      if (cancelled()) {
        return false;
      }
      serve_streams_in_load();
      CtrMercModelData md;
      md.name = std::string(m.name, strnlen(m.name, sizeof(m.name)));
      md.scale = m.scale;
      if (any_gen && md.name.find(c3l::kMercGenLodSuffix) != std::string::npos) {
        continue;  // (drawn as its model's gen_draws, or not at all: an exempt model's)
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
      // (AI-assisted) the "play as Keira" model: bound to her rig, posed from Jak's bones
      md.keira_rig = !md.name.compare(0, ctr_keira::kModelPrefixLen, ctr_keira::kModelPrefix);
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
          // (AI-assisted) her shadow's hulls: model space positions, slots, triangles
          const size_t sfx = strlen(ctr_keira::kShadowSuffix);
          if (md.name.size() > sfx &&
              !md.name.compare(md.name.size() - sfx, sfx, ctr_keira::kShadowSuffix)) {
            std::vector<float> pos(3 * m.vertex_count);
            std::vector<u8> slot(m.vertex_count, 0);
            std::vector<u16> tris;
            for (const auto& d : md.draws) {
              for (u32 k = d.first_index; k + 2 < d.first_index + d.index_count &&
                                          k + 2 < mindices.size();
                   k += 3) {
                for (int i = 0; i < 3; i++) {
                  const u16 v = mindices[k + i];
                  if (v < m.vertex_count) {
                    slot[v] = d.palette[mverts[v].bones[0] % c3l::kMercPaletteSize];
                  }
                  tris.push_back(v);
                }
              }
            }
            for (u32 v = 0; v < m.vertex_count; v++) {
              for (int c = 0; c < 3; c++) {
                pos[3 * v + c] = mverts[v].pos[c] * m.scale;
              }
            }
            auto sm = std::make_shared<ctr_keira::ShadowMeshData>();
            if (ctr_keira::build_shadow_mesh(pos.data(), slot.data(), (int)m.vertex_count,
                                             tris.data(), (int)tris.size() / 3, sm.get())) {
              md.keira_shadow = sm;
              lg::info("[ctr] {}: {} shadow hulls", md.name, sm->view.nhulls);
            } else {
              lg::warn("[ctr] {}: shadow hulls not closed, not used", md.name);
            }
          }
          md.mesh = ctr_gpu_skinned_mesh_create(mverts.data(), m.vertex_count, mindices.data(),
                                                end - first);
          while (md.mesh < 0 &&
                 make_room(name, "the models",
                           (u64)m.vertex_count * sizeof(c3l::MercVertex) + (end - first) * 2u)) {
            md.mesh = ctr_gpu_skinned_mesh_create(mverts.data(), m.vertex_count, mindices.data(),
                                                  end - first);
          }
          if (md.mesh < 0 && !cancelled()) {
            lg::error("[ctr] {}: merc model {}: mesh not made, no linear memory ({} KB needed; {})",
                      name, md.name,
                      ((u64)m.vertex_count * sizeof(c3l::MercVertex) + (end - first) * 2u) / 1024,
                      linear_room_text());
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
      m_parts_room.erase(d.job.key());  // (AI-assisted, streammem)
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
    if (d.job.common && name != "GAME") {
      // (AI-assisted) load_extra
      if (d.result == LoadResult::LOADED) {
        set_tex_priority(d.lev->tex_pools, 3);
        lg::info("[ctr] loaded {}.c3l ({} models)", name, d.lev->merc_models.size());
        m_extras[name] = std::move(d.lev);
        rebuild_merc_index();
      } else {
        lg::warn("[ctr] no {} (out/jak1/c3l/{}.c3l)", name, name);
      }
      continue;
    }
    if (d.job.common) {
      if (d.result == LoadResult::LOADED) {
        m_common = std::move(d.lev);
        // Jak and the shared models: on screen all the time, and small
        set_tex_priority(m_common->tex_pools, 3);
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
  for (size_t i = 0; i < lev.textures.size(); i++) {
    if (!(i < lev.tex_shared.size() && lev.tex_shared[i])) {
      ctr_gpu_tex_delete(lev.textures[i]);
    }
  }
  for (int pool : lev.tex_pools) {
    ctr_gpu_pool_delete(pool);  // after its textures (freed together, after the frame)
  }
  lev.tex_pools.clear();
  release_share_groups(lev.tex_share_groups);  // c3l v23: the shared ones, with the last user
  lev.tex_share_groups.clear();
  lev.tex_shared.clear();
  for (int m : lev.meshes) {
    ctr_gpu_mesh_delete(m);
  }
  if (lev.wind_mesh >= 0) {
    ctr_gpu_mesh_delete(lev.wind_mesh);
    lev.wind_mesh = -1;
    lev.chunk_wind.clear();
  }
  if (lev.normals_mesh >= 0) {
    ctr_gpu_mesh_delete(lev.normals_mesh);  // (AI-assisted, j2water2)
    lev.normals_mesh = -1;
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

void CtrLevels::load_extra(const std::string& name) {
  if (std::find(m_extra_asked.begin(), m_extra_asked.end(), name) != m_extra_asked.end()) {
    return;
  }
  m_extra_asked.push_back(name);
  lg::info("[ctr] loading {}.c3l", name);
  request({name, true, false});
}

void CtrLevels::rebuild_merc_index() {
  m_merc_index.clear();
  if (m_common) {
    for (size_t i = 0; i < m_common->merc_models.size(); i++) {
      m_merc_index[m_common->merc_models[i].name] = {m_common.get(), (int)i};
    }
  }
  for (auto& [name, lev] : m_extras) {
    for (size_t i = 0; i < lev->merc_models.size(); i++) {
      m_merc_index[lev->merc_models[i].name] = {lev.get(), (int)i};
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
  m_title_on = g_game_version == GameVersion::Jak1 &&
               std::find(names.begin(), names.end(), "title") != names.end();
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
        m_parts_room.erase(it->key());  // (AI-assisted, streammem)
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
// (AI-assisted, popin2) at the arena's capacity: a unit needed now frees units there whose priority
// (StreamUnit::prio) is this much smaller than its own; in view a unit's priority counts this much
// more; a unit taken this many frames ago or less is not freed so (no back and forth)
constexpr float kPrioHyst = 1.5f, kInViewPrio = 1.5f;
constexpr u64 kMinResidentFrames = 30;
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
                       const TodVerts& v,
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
      plain = v.entry((int)i) < ntab;
    }
    g.plain = plain ? 1 : 0;
    g.ver = 0;
    g.dver = 0;
    g.lver = 0;
  }
}
}  // namespace

void CtrLevels::free_stream_parts(const Job& job) {
  for (const auto& p : job.stream_parts) {
    CtrStreamArena::get().free(p.mem);
  }
}

// (AI-assisted, streammem) Render thread (housekeeping with room; outside of a frame). make_room
// freed the growth's resident units (stream_shrink), but the units of queued stream jobs (their
// pieces taken at request) and the meshes read and not taken yet (m_take) kept their segments from
// emptying, so release_empty gave the heap less than it looked. Those in growth segments (all:
// every one) are given back here: their units are absent again (asked for again when wanted). The
// job the loader runs now is the one that asked for room: no stream job.
u64 CtrLevels::stream_drop_pending(bool all) {
  auto& arena = CtrStreamArena::get();
  if (!arena.ready() || (!all && !arena.has_growth())) {
    return 0;
  }
  std::vector<Job> jobs;
  {
    std::lock_guard<std::mutex> lk(m_lock);
    for (auto it = m_queue.begin(); it != m_queue.end();) {
      bool held = all && it->stream;
      for (size_t k = 0; it->stream && !held && k < it->stream_parts.size(); k++) {
        held = arena.in_growth(it->stream_parts[k].mem);
      }
      if (held) {
        m_requested.erase(it->key());
        jobs.push_back(std::move(*it));
        it = m_queue.erase(it);
      } else {
        ++it;
      }
    }
  }
  auto unit_of = [&](const std::string& name, u32 gen, u32 unit) -> CtrLevelData::StreamUnit* {
    auto it = m_levels.find(name);
    if (it == m_levels.end() || it->second->generation != gen ||
        unit >= it->second->stream_units.size()) {
      return nullptr;
    }
    return &it->second->stream_units[unit];
  };
  u64 bytes = 0;
  for (const auto& job : jobs) {
    m_stream_inflight -= std::min<u32>(m_stream_inflight, 1);
    for (const auto& p : job.stream_parts) {
      auto* su = unit_of(job.name, job.generation, p.unit);
      if (su && su->state == CtrLevelData::kUnitLoading && su->mem == p.mem) {
        su->state = CtrLevelData::kUnitAbsent;
        su->mem = nullptr;
        bytes += su->bytes;
      }
    }
    free_stream_parts(job);
  }
  size_t kept = 0;
  for (size_t i = 0; i < m_take.size(); i++) {
    auto& tk = m_take[i];
    auto* su = unit_of(tk.name, tk.generation, tk.out.unit);
    const bool mine = su && su->state == CtrLevelData::kUnitLoading && su->mem;
    if (!all && !(mine && arena.in_growth(su->mem))) {
      if (kept != i) {
        m_take[kept] = std::move(tk);
      }
      kept++;
      continue;
    }
    ctr_gpu_mesh_delete(tk.out.mesh);  // (its piece back once the pending deletes are freed)
    if (mine) {
      bytes += su->bytes;
      su->state = CtrLevelData::kUnitAbsent;
      su->mem = nullptr;
    }
  }
  m_take.resize(kept);
  if (!jobs.empty() || bytes) {
    lg::info("[ctr] making room: {} queued stream jobs dropped, {} KB of near chunks given back",
             jobs.size(), bytes / 1024);
  }
  return bytes;
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
  for (int i = 0; i < 3; i++) {
    for (int c = 0; c < 3; c++) {
      m_cam_rot[3 * i + c] = rot[i][c];
    }
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
    tod_drop_entries(*lev.tod, ci);
  }
  su.state = CtrLevelData::kUnitAbsent;
  su.mem = nullptr;
  lev.stream_resident -= std::min(lev.stream_resident, su.bytes);
  m_sst.evictions++;
}

u32 CtrLevels::stream_shrink(bool all, bool whole) {
  auto& arena = CtrStreamArena::get();
  if (!arena.ready() || (!whole && !arena.has_growth())) {
    return 0;
  }
  u32 n = 0;
  const float kM = 4096.f;
  for (auto& [name, lp] : m_levels) {
    CtrLevelData& lev = *lp;
    for (u32 u = 0; u < lev.stream_units.size(); u++) {
      auto& su = lev.stream_units[u];
      if (su.state != CtrLevelData::kUnitResident || !su.mem ||
          (!whole && !arena.in_growth(su.mem))) {
        continue;
      }
      if (!all) {
        // not needed now (the camera beyond its groups' near distances by the keep margin)
        const float* c = lev.chunks[su.chunk].bsphere;
        const float dx = m_cam[0] - c[0], dy = m_cam[1] - c[1], dz = m_cam[2] - c[2];
        const float k = lev.stream_k;
        if (k > 0 && std::sqrt(dx * dx + dy * dy + dz * dz) - su.bound(k, lev.lod_floor_k, lev.lod_min_e) <
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
    part.normals_offset = lev.tail_file(ci);
    part.tail_k = lev.tail_k(ci);
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
  // (AI-assisted, streammem) the linear memory the detail reloads, repairs and read backs queued or
  // running take (their meshes): kept free of the growth like a level load's (SM-1). Their keys
  // have a '#', so they didn't count as a load: the room make_room freed for them was taken back by
  // the growth in the same frame, the job failed and a detail reload threw everything away.
  u64 parts_room = 0;
  {
    std::lock_guard<std::mutex> lk(m_lock);
    for (const auto& key : m_requested) {
      level_loading |= key.find('#') == std::string::npos;
    }
    for (const auto& [key, bytes] : m_parts_room) {
      parts_room += bytes;
    }
  }
  const u32 parts_reserve = (u32)std::min<u64>(parts_room ? parts_room + kGrowHeadroom : 0, kLoadRoom);
  // (statistics) the linear heap's free memory at each frame's start
  const u32 free_now = ctr_gpu_linear_free();
  m_sst.linear_min = std::min<u64>(m_sst.linear_min, free_now);
  if (level_loading) {
    m_sst.linear_min_load = std::min<u64>(m_sst.linear_min_load, free_now);
  }
  const u32 floor_bytes = (u32)ctr_settings().stream_reserve_kb * 1024u;
#ifdef __3DS__
  // (AI-assisted, streammem) the heap's last-resort linear blocks leave 1 MB less than the arena's
  // floor (at least 1 MB): with the arena grown the heap sits at its floor + kGrowHeadroom, and the
  // fallback's fixed 3.5 MB never fitted there (SM-2). The arena gives its growth back when the
  // heap is under its floor (below), which brings the floor back.
  ctr_heap_set_linear_floor(std::max<u32>(1024 * 1024, floor_bytes - std::min(floor_bytes, 1024u * 1024)));
#endif
  dormant_update(frame);  // (AI-assisted, j2-mem)
  dist_update(frame, free_now);  // (AI-assisted, j2strip)
  title_update(frame);
  if (!arena.ready() && !level_loading && !parts_room && frame % kRepairCheckFrames == 0) {
    // (AI-assisted, streammem) the arena given back to a level load out of memory (make_room), or
    // its first segment not there at the first streaming level's load: made again once the heap
    // has it over its floor
    bool any = false;
    for (const auto& [name, lp] : m_levels) {
      any = any || lp->streaming;
    }
    if (any && free_now >= ctr_stream_kb() * 1024u + floor_bytes + kGrowMargin &&
        arena.init(ctr_stream_kb() * 1024u)) {
      lg::info("[ctr] stream arena made again ({} KB linear free)", free_now / 1024);
    }
  }
  if ((!m_cam_valid && !m_game_cam_new) || !arena.ready()) {
    return;
  }
  // the arena's growth: given back where the linear heap has less than its floor (+ what the
  // repair of an incomplete level waits for) or a level loads (its memory first: the floor plus
  // kLoadRoom); under the floor + kPressure the units not needed now first, under the floor all
  // of them. The empty segments back to the heap (their meshes' memory came back once the GPU was
  // done with it).
  const u32 reserve = (u32)ctr_settings().stream_reserve_kb * 1024u + m_stream_repair_room;
  // (AI-assisted, popin2) while the growth is emptied for the heap (all of it freed), no new piece
  // goes there. Before, the units read meanwhile went into the growth segments again and were freed
  // the next frame, so a segment never emptied and the heap never got its memory back: Oct 25
  // hardware log, rolling + village2 at 2.6 MB linear free (floor 3 MB) for minutes, 250-850 near
  // chunks read and freed again per 15 s, needed ones late 0.5-1.7 s on average (up to 15 s)
  bool drain = false;
  if (arena.has_growth()) {
    if (level_loading) {
      const u32 need = reserve + kLoadRoom;
      if (free_now < need) {
        drain = free_now + 512 * 1024 < need;
        m_sst.shrunk += stream_shrink(drain);
      }
    } else if (parts_reserve && free_now < reserve + parts_reserve) {
      // (AI-assisted, streammem) a parts job's room: the growth units not needed now (make_room
      // frees the rest if the job runs short)
      drain = free_now < reserve;
      m_sst.shrunk += stream_shrink(drain);
    } else if (free_now < reserve + kPressure) {
      drain = free_now < reserve;
      m_sst.shrunk += stream_shrink(drain);
    }
    arena.release_empty();
  }
  arena.set_draining(drain);
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
  // (AI-assisted, title3) the game's camera this frame (note_game_camera) and where its speed takes
  // it: a level loaded but not drawn lately (Sandover behind the title's logo, for seconds before
  // the logo reveals it at the attract camera's start; a level the game shows later) reads the near
  // chunks needed there, with the tie LOD factor of the last level drawn. They are there when it is
  // shown: no groups drawn far at its first frames, then switching near on screen.
  float pg[kLeadSteps][3];
  int ngp = 0;
  if (m_game_cam_new) {
    m_game_cam_new = false;
    float v[3], d2 = 0;
    for (int c = 0; c < 3; c++) {
      v[c] = m_game_cam_next[c] - m_game_cam[c];
      d2 += v[c] * v[c];
    }
    const bool cut = !m_game_cam_valid || d2 > kCutMeters * kCutMeters * kM * kM;
    for (int c = 0; c < 3; c++) {
      m_game_vel[c] = cut ? 0.f : 0.75f * m_game_vel[c] + 0.25f * v[c];
      m_game_cam[c] = m_game_cam_next[c];
    }
    m_game_cam_valid = true;
    const float gv2 = m_game_vel[0] * m_game_vel[0] + m_game_vel[1] * m_game_vel[1] +
                      m_game_vel[2] * m_game_vel[2];
    ngp = gv2 * kLeadFrames * kLeadFrames < kM * kM ? 1 : kLeadSteps;
    for (int i = 0; i < ngp; i++) {
      for (int c = 0; c < 3; c++) {
        pg[i][c] = m_game_cam[c] + m_game_vel[c] * kLeadFrames * (float)i / (kLeadSteps - 1);
      }
    }
  }
  for (auto& [name, lp] : m_levels) {
    if (lp->streaming && lp->stream_frame + 3 >= frame && lp->stream_k > 0) {
      m_k_last = std::max(lp->stream_k * 1.15f, lp->stream_k_hi);
    }
  }
  if (m_k_last <= 0) {
    m_k_last = m_k_any * 1.15f;  // (no streaming level drawn yet: the title's logo)
  }
  u64 need_now = 0;
  for (auto& [name, lp] : m_levels) {
    CtrLevelData& lev = *lp;
    if (!lev.streaming) {
      continue;
    }
    // a level not drawn lately (far mode never asks: its k is 0 there) wants nothing
    // (15% more, or the recent largest: a zooming camera's focal length moves the factor)
    // (AI-assisted, title3) except one not drawn at all lately (in no mode) while loaded: the
    // near chunks around the game's camera, with the last level's factor (hidden)
    const bool hidden = lev.stream_frame + 3 < frame && lev.last_draw_frame + 3 < frame &&
                        ngp > 0 && m_k_last > 0;
    const float k = lev.stream_frame + 3 >= frame
                        ? std::max(lev.stream_k * 1.15f, lev.stream_k_hi)
                        : (hidden ? m_k_last : 0.f);
    const float(*const pp)[3] = hidden ? pg : p;
    const int npp = hidden ? ngp : np;
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
      su.urgent = false;
      su.prio = 0.f;
      if (k <= 0) {
        if (resident) {
          m_have.push_back({&lev, u, 1e30f});
        }
        continue;
      }
      const float* c = lev.chunks[su.chunk].bsphere;
      float dmin = 1e30f;
      for (int i = 0; i < npp; i++) {
        const float dx = pp[i][0] - c[0], dy = pp[i][1] - c[1], dz = pp[i][2] - c[2];
        dmin = std::min(dmin, dx * dx + dy * dy + dz * dz);
      }
      // (squared first: most units are far beyond their bound, no root for them)
      const float bound = su.bound(k, lev.lod_floor_k, lev.lod_min_e);
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
      // (AI-assisted, lodnear) with the PS2 floor, a unit needed only by it (its groups' far
      // versions within tie_lod_px there) is less urgent than one whose groups' far versions
      // would show: those first, and they may take the others' room at capacity
      const bool has_err = lev.lod_error.size() == lev.lod_groups.size();
      const bool floor_on = lev.lod_floor_k > 0 && has_err;
      bool urgent = !floor_on;
      float prio = 0.f;  // (AI-assisted, popin2) StreamUnit::prio before the in-view factor
      if (lev.chunks[su.chunk].kind == c3l::CHUNK_KIND_TFRAG) {
        // the tfrag geomorph's near chunk (no tie groups): needed within its bound, its far
        // versions showing less detail meanwhile
        urgent = true;
        prio = 1.f;
      } else if (!lev.unit_group_first.empty()) {
        float best = 1e30f;
        for (u32 j = lev.unit_group_first[u]; j < lev.unit_group_first[u + 1]; j++) {
          const u32 g = lev.unit_groups[j];
          const auto& lg = lev.lod_groups[g];
          const float sw = g < lev.lod_error.size() ? lev.lod_switch_error(g) : lg.dist;
          const float lim = sw * k + lg.bsphere[3];
          float g2 = 1e30f;
          for (int i = 0; i < npp; i++) {
            const float dx = pp[i][0] - lg.bsphere[0], dy = pp[i][1] - lg.bsphere[1],
                        dz = pp[i][2] - lg.bsphere[2];
            g2 = std::min(g2, dx * dx + dy * dy + dz * dz);
          }
          if (!urgent) {
            const float lim_e = lev.lod_error[g] * k + lg.bsphere[3];
            urgent = g2 < lim_e * lim_e;
          }
          if (has_err) {
            // (AI-assisted, popin2) its far version's error from the camera now (not the lead
            // positions: a unit needed only ahead is read when there is room, and frees nothing),
            // in units of the switch's (shape error x k at the distance to the group's surface)
            const float dx = pp[0][0] - lg.bsphere[0], dy = pp[0][1] - lg.bsphere[1],
                        dz = pp[0][2] - lg.bsphere[2];
            const float ds = std::sqrt(dx * dx + dy * dy + dz * dz) - lg.bsphere[3];
            prio = std::max(prio, lev.lod_error[g] * k / std::max(ds, kM));
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
      su.urgent = urgent && score < 0;
      const bool view = (resident || score < 0) && !hidden && in_view(c);
      su.prio = prio * (view ? kInViewPrio : 1.f);
      if (resident) {
        m_have.push_back({&lev, u, score});
      } else if (score < want_m) {
        // (needed now and on screen: first; (AI-assisted, lodnear) then the urgent ones; title3:
        // not for hidden levels). (AI-assisted, popin2) with shape errors: the needed ones by
        // priority, the most first (where the far versions show most), then the others by score
        const float key =
            has_err && score < 0
                ? -kM * (1.f + 100.f * su.prio)
                : (score < 0 && view ? score - kInViewMeters * kM : score) -
                      (su.urgent ? kInViewMeters * kM : 0.f);
        m_want.push_back({&lev, u, key});
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
  if (pop_log_on()) {  // (AI-assisted, titlemove) CTR_POP_LOG: the streaming's view of the frame
    u32 want_now = 0;
    for (const auto& w : m_want) want_now += w.score < 0;
    lg::info("[popS] f={} gcam={:.1f},{:.1f},{:.1f} ngp={} cam={:.1f},{:.1f},{:.1f} np={} want={} want_now={} need_now={}KB inflight={} idle={} arena={}KB",
             frame, m_game_cam[0] / 4096.f, m_game_cam[1] / 4096.f, m_game_cam[2] / 4096.f, ngp,
             m_cam[0] / 4096.f, m_cam[1] / 4096.f, m_cam[2] / 4096.f, np, m_want.size(), want_now,
             need_now / 1024, m_stream_inflight, m_idle.size(), arena.capacity() / 1024);
  }
  if (m_want.empty() && !m_stream_inflight && !level_loading && !m_idle.empty() &&
      frame >= m_idle_frame + kIdleFrames) {
    stream_idle_fill(frame);
    return;
  }
  if (m_want.empty()) {
    return;
  }
  bool have_sorted = false;
  bool prio_sorted = false;  // (AI-assisted, popin2) m_have_prio made, pi: its next unit
  size_t pi = 0;
  size_t hi = 0;
  u64 evicted = 0;  // bytes freed now (back after the GPU is done with them: no growth for those)
  // (AI-assisted, streammem, SM-5) Room in bulk: the units needed now that aren't there, beyond
  // the arena's free and pending room, freed at once from the units far beyond their need (2 x
  // the keep margin; a level not drawn: all of them), the furthest first. Also while 2 jobs are in
  // flight: their memory is back (the GPU done with it) by the time a job slot frees. Before, at
  // capacity one unit's worth was freed per frame and none with 2 jobs queued: entering lava tube
  // from village3 (the arena full of village3's units) the needed 3-6 MB came over seconds, the
  // groups drawn far and switching near on screen.
  {
    u64 needed = 0;
    for (const auto& w : m_want) {
      if (w.score < 0) {
        needed += w.lev->stream_units[w.unit].bytes;
      }
    }
    u32 free_b = 0, pending_b = 0;
    arena.room(&free_b, &pending_b);
    if (needed > (u64)free_b + pending_b) {
      const u64 deficit = needed - free_b - pending_b;
      std::sort(m_have.begin(), m_have.end(),
                [](const StreamCand& a, const StreamCand& b) { return a.score > b.score; });
      have_sorted = true;
      u64 bulk = 0;
      while (bulk < deficit && hi < m_have.size() && m_have[hi].score > 2 * kKeepMeters * kM) {
        auto& victim = m_have[hi].lev->stream_units[m_have[hi].unit];
        if (victim.state == CtrLevelData::kUnitResident) {
          bulk += victim.bytes;
          stream_evict(*m_have[hi].lev, m_have[hi].unit);
          m_sst.bulk_evictions++;
        }
        hi++;
      }
      evicted += bulk;
    }
  }
  if (m_stream_inflight >= kJobsInFlight) {
    return;
  }
  std::sort(m_want.begin(), m_want.end(),
            [](const StreamCand& a, const StreamCand& b) { return a.score < b.score; });
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
      // (AI-assisted, streammem) the room of the parts jobs in flight too (parts_reserve)
      const u32 grow_reserve =
          reserve + (level_loading ? kLoadRoom : std::max(kGrowHeadroom, parts_reserve));
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
    // (AI-assisted, lodnear) still none and this unit urgent: the units there only for the PS2
    // floor (their groups go to the far version, within tie_lod_px), the furthest first
    if (!mem && evicted < su.bytes && su.urgent) {
      if (!have_sorted) {
        std::sort(m_have.begin(), m_have.end(),
                  [](const StreamCand& a, const StreamCand& b) { return a.score > b.score; });
        have_sorted = true;
      }
      u64 freed_for = 0;
      for (size_t j = hi; !mem && j < m_have.size() && freed_for < su.bytes; j++) {
        auto& victim = m_have[j].lev->stream_units[m_have[j].unit];
        if (victim.state != CtrLevelData::kUnitResident || victim.urgent) {
          continue;
        }
        freed_for += arena.run_if_freed(victim.mem) >= su.bytes ? su.bytes : victim.bytes;
        evicted += victim.bytes;
        stream_evict(*m_have[j].lev, m_have[j].unit);
        mem = arena.alloc(su.bytes);
      }
    }
    // (AI-assisted, popin2) still none and needed now: the units there whose groups' far versions
    // would show kPrioHyst x less than this one's (StreamUnit::prio), the least first, not one
    // taken within kMinResidentFrames. The hardware log (Oct 25: rolling + village2, swamp,
    // lavatube) had the needed near data larger than the arena most of the time (needed 4-8 MB, the
    // arena 5-8 MB at the linear heap's floor) and every unit there urgent: a needed unit waited
    // for room until the camera went away from others, on average 0.5-2 s and up to 15 s, its
    // groups drawn far meanwhile, then switched near on screen up close. At capacity the arena
    // now holds what shows most: a group left far for want of room is one whose far version shows
    // little, and it comes when it shows more than what is there, at about the same error
    if (!mem && evicted < su.bytes && w.score < 0 && su.prio > 0) {
      if (!prio_sorted) {
        m_have_prio.clear();
        for (const auto& h : m_have) {
          const auto& hu = h.lev->stream_units[h.unit];
          if (hu.state == CtrLevelData::kUnitResident) {
            m_have_prio.push_back({h.lev, h.unit, hu.prio});
          }
        }
        std::sort(m_have_prio.begin(), m_have_prio.end(),
                  [](const StreamCand& a, const StreamCand& b) { return a.score < b.score; });
        prio_sorted = true;
      }
      u64 freed_for = 0;
      for (; !mem && pi < m_have_prio.size() && freed_for < su.bytes; pi++) {
        const auto& h = m_have_prio[pi];
        auto& victim = h.lev->stream_units[h.unit];
        if (h.score * kPrioHyst >= su.prio) {
          break;  // (the rest have more)
        }
        if (victim.state != CtrLevelData::kUnitResident ||
            frame < victim.resident_frame + kMinResidentFrames) {
          continue;
        }
        freed_for += arena.run_if_freed(victim.mem) >= su.bytes ? su.bytes : victim.bytes;
        evicted += victim.bytes;
        stream_evict(*h.lev, h.unit);
        m_sst.prio_evictions++;
        mem = arena.alloc(su.bytes);
      }
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
    part.normals_offset = lev.tail_file(ci);
    part.tail_k = lev.tail_k(ci);
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
        score = k2 > 0 ? std::sqrt(dx * dx + dy * dy + dz * dz) - su.bound(k2, lev.lod_floor_k, lev.lod_min_e)
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
      part.normals_offset = lev.tail_file(ci);
      part.tail_k = lev.tail_k(ci);
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
void CtrLevels::run_stream_job(const Job& job, Done* d, c3l::Reader::Buffers* bufs) {
  Timer timer;
  d->result = LoadResult::FAILED;
  size_t i = 0;
  int live = -1;  // (AI-assisted, streammem) part i's mesh, begun and not ended
  try {
    if (!job.dir) {
      throw std::runtime_error("no file directory");
    }
    d->stream_out.reserve(job.stream_parts.size());  // (no allocation after a mesh is ended)
    C3lReader f(job.path, bufs ? bufs : &m_read_bufs, job.window, *job.dir);
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
        constexpr u32 kGapVertexBytes = 48 * 1024, kGapIndices = 16 * 1024 / 2;
        constexpr u32 kSpanBytes = 384 * 1024;
        // a part's vertex bytes in the file, with its tail when that follows them
        auto vbegin = [&](const Job::StreamPart& q) {
          return job.vertex_data_offset + q.first_vertex * (u32)sizeof(c3l::Vertex);
        };
        auto vend = [&](const Job::StreamPart& q) {
          const u32 e = vbegin(q) + q.vertex_count * (u32)sizeof(c3l::Vertex);
          return q.normals_offset == e ? e + q.vertex_count * q.tail_k : e;
        };
        run_end = i + 1;
        while (run_end < job.stream_parts.size()) {
          const auto& a = job.stream_parts[run_end - 1];
          const auto& b = job.stream_parts[run_end];
          if (vbegin(b) < vend(a) || b.index_first < a.index_end ||
              vbegin(b) - vend(a) > kGapVertexBytes || b.index_first - a.index_end > kGapIndices ||
              vend(b) - vbegin(p) + (b.index_end - p.index_first) * 2u > kSpanBytes) {
            break;
          }
          run_end++;
        }
        const auto& last = job.stream_parts[run_end - 1];
        for (const bool ok :
             {f.preload(job.index_data_offset + p.index_first * (u32)sizeof(u16),
                        (last.index_end - p.index_first) * (u32)sizeof(u16)),
              f.preload(vbegin(p), vend(last) - vbegin(p))}) {
          (ok ? d->preloads : d->preload_misses)++;
        }
      }
      void* vdst;
      u16* idst;
      const u32 icount = p.index_end - p.index_first;
      // its tail after the indices (CtrLevelData::normals_offset_in_mesh)
      const u32 ntail = p.normals_offset ? p.vertex_count * p.tail_k : 0;
      void* ndst = nullptr;
      int mesh = ctr_gpu_mesh_begin_ext_tail(p.mem, (int)p.vertex_count, (int)icount,
                                             CtrStreamArena::mesh_bytes(p.vertex_count, icount),
                                             ntail, &vdst, &idst, &ndst);
      if (mesh < 0) {
        lg::error("[ctr] {}: near chunks: {} of {} not made, no GPU mesh slot", job.name,
                  job.stream_parts.size() - i, job.stream_parts.size());
        break;  // (its piece back below)
      }
      live = mesh;
      ctr_gpu_mesh_split_colors(mesh);
      const bool ok =
          f.read(job.index_data_offset + p.index_first * (u32)sizeof(u16), idst,
                 icount * sizeof(u16)) &&
          read_split_vertices(f,
                              job.vertex_data_offset + p.first_vertex * (u32)sizeof(c3l::Vertex),
                              p.vertex_count, vdst) &&
          (!ntail || f.read(p.normals_offset, ndst, ntail));
      Done::StreamOut o;
      o.unit = p.unit;
      if (ok && p.layout) {
        const TodVerts sv{
            (u8*)vdst, (tod_word*)((u8*)vdst + ctr_gpu_mesh_color_offset((int)p.vertex_count))};
        stream_tod_layout(this, p.vertex_count, icount, p.draws, p.runs, idst, sv, job.tod_ntab,
                          &o.groups, &o.run_group);
        o.layout = true;
      }
      live = -1;
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
    if (live >= 0) {
      // (AI-assisted, streammem) its piece back through ctr_gpu_set_ext_free: not below
      ctr_gpu_mesh_end(live, 0);
      i++;
    }
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
// never drawn with the file's baked colors); at most
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
    su.resident_frame = frame;  // (AI-assisted, popin2)
    g_pop.taken++;  // (AI-assisted, title3: CTR_POP_LOG)
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
        if (g0 + n <= t.group_list.size()) {
          std::fill(t.group_list.begin() + g0, t.group_list.begin() + g0 + n, 0u);
        }
        su.tod_layout = 1;
      }
      int n = 0;
      const TodVerts v = tod_verts(o.mesh, &n);
      if (v && t.blended_valid && ci < t.chunk_mask.size()) {
        if (!t.chunk_scanned[ci]) {
          // its palettes and light vertices (once per level load)
          CtrLevelData::Tod::LightList lights;
          const u8 mask = tod_scan(t, v, n, &lights);
          t.chunk_scanned[ci] = mask ? 2 : 1;
          t.chunk_mask[ci] = mask;
          if (!lights.empty()) {
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
            gr.lver = t.ver;
          }
          t.chunk_min_ver[ci] = t.ver;
          t.chunk_min_dver[ci] = t.dver;
          t.chunk_light_ver[ci] = t.ver;
          ctr_gpu_flush_range(v.rgba, (u32)n * 4u);
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
  free = fmt::format("; free: linear {} KB, heap {} KB (largest {} KB)", ctr_gpu_linear_free() / 1024,
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
           "ms in jobs, longest {:.0f} ms; {:.0f} KB/s), {} freed ({} for a needed one of more "
           "priority, {} in bulk), {} times no room; late: {} "
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
           secs > 0 ? m_sst.read_bytes / 1024.0 / secs : 0.0, m_sst.evictions,
           m_sst.prio_evictions, m_sst.bulk_evictions, m_sst.no_room,
           m_sst.late_events, m_sst.late_frames, m_sst.late_group_frames,
           m_sst.late_periods ? m_sst.late_ms_total / m_sst.late_periods : 0.0,
           m_sst.late_ms_max, m_sst.late_frames_max, m_sst.need_sum / n / 1024.0,
           m_sst.need_max / 1024, m_sst.neighbours,
           m_sst.linear_min == ~0ull ? 0 : m_sst.linear_min / 1024,
           m_sst.linear_min_load == ~0ull ? std::string("-")
                                          : std::to_string(m_sst.linear_min_load / 1024),
           m_sst.sched_ms / n, m_sst.sched_max_ms,
           levels.empty() ? std::string("no levels") : levels);
  if (m_dst.freed_chunks || m_dst.read_jobs) {
    lg::info("[ctr] distance freeing: {} chunks freed ({} KB; {} times in play), {} read backs of "
             "{} chunks ({} KB)",
             m_dst.freed_chunks, m_dst.freed_bytes / 1024, m_dst.play_frees, m_dst.read_jobs,
             m_dst.read_chunks, m_dst.read_bytes / 1024);
  }
  m_dst = DistStats();
#ifdef __3DS__
  ctr_heap_trace_dump("stats");  // (AI-assisted, lavamem) trace builds only
#endif
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
// (render thread: set by CtrRenderer from the GPU time, read here); below 0 the relief
// (CtrAutoDetail.h: -1 per knob moved toward its old value)
float g_detail_t = 0.25f;
// (AI-assisted, title-popin) auto_detail: the GPU is about to miss frames (ctr_level_set_detail_urgent)
bool g_detail_urgent = false;
// (AI-assisted, j2intro / title3) the screen is black this frame (ctr_level_set_screen_black)
bool g_screen_black = false;
// (AI-assisted, title-popin) the auto_detail position as stored per chunk / tie LOD group
// (CtrLevelData::chunk_detail_q): 0..kDetailQ over -kReliefMax..1, 0xff none yet
constexpr float kDetailQ = 250.f;
constexpr float kDetailLo = -ctr_detail::kReliefMax;
u8 detail_q(float t) {
  return (u8)std::lround((std::clamp(t, kDetailLo, 1.f) - kDetailLo) / (1.f - kDetailLo) * kDetailQ);
}
float detail_at(u8 q) {
  return kDetailLo + q / kDetailQ * (1.f - kDetailLo);
}
// the relief's knobs at position t (no relief at t >= 0); one entry of cache (the held chunks and
// groups of a frame mostly share the position they were decided with)
struct ReliefAt {
  ctr_detail::Relief rl;
  float t = 2.f;
  ctr_detail::Knobs kn{};
  explicit ReliefAt(const CtrSettings& s) {
    rl.init(s.tie_lod_px, s.lod_distance, s.tie_distance_scale, s.draw_distance);
  }
  const ctr_detail::Knobs& at(float tt) {
    if (tt != t) {
      t = tt;
      kn = rl.at(std::max(-tt, 0.f));
    }
    return kn;
  }
};
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
// view test against the crossed planes (STR); writes m_run_shown /
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
  bool vista_tier;
  const float* po;
  const float (*pk)[3];
  int np;
  float scale;
  u8* shown_out;
  u8* nomerge_out;
};
struct RunLoopOut {
  u32 shown = 0, view_hidden = 0, view_tris = 0, lod_hid = 0;
  u64 lod_tris[2] = {0, 0};
};
// (AI-assisted, micro-level) NP: the number of crossed planes (0: the chunk crosses none, no view
// test), their coefficients in locals (registers: the stores don't make them be loaded again)
template <bool VIS, int NP, bool LOD>
void run_loop(const RunLoopIn& in, u32 r0, u32 nr, RunLoopOut* out) {
  constexpr bool STR = NP > 0;
  constexpr int kP = NP > 0 ? NP : 1;
  float po[kP], pk0[kP], pk1[kP], pk2[kP];
  // (AI-assisted, perfv2) unrolled: GCC kept the planes in this stack array and reloaded all four
  // coefficients and the loop pointers for every run (~92 instructions per 3-plane run); unrolled
  // they are scalars in VFP registers
#pragma GCC unroll 4
  for (int i = 0; i < NP; i++) {
    po[i] = in.po[i];
    pk0[i] = in.pk[i][0];
    pk1[i] = in.pk[i][1];
    pk2[i] = in.pk[i][2];
  }
  const float scale = in.scale;
  u32 shown = 0, view_hidden = 0, view_tris = 0, lod_hid = 0;
  // (AI-assisted, perfv2) u32 within one chunk (at most 65536 vertices, runs of u16 index counts):
  // the u64 sums were adds / adc pairs through the stack per run
  u32 lod_tris1 = 0, lod_tris2 = 0;
  const c3l::VisRun* const runs = in.runs;
  const CtrLevelData::RunSphere* const spheres = in.spheres;
  const c3l::RunLod* const run_lod = in.run_lod;
  const u8* const lod_state = in.lod_state;
  const u8* const vis = in.vis;
  u8* const shown_out = in.shown_out;
  u8* const nomerge_out = in.nomerge_out;
  const bool vista_tier = in.vista_tier;
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
      // (bit 3: a morph group's mid version, c3l::ExtMorph)
      const u8 gs = rl.lod != c3l::kRunLodAlways ? lod_state[rl.group] : 0;
      const u8 want = (gs & 1) ? ((gs & 8) ? c3l::kRunLodMid : c3l::kRunLodNear) : c3l::kRunLodFar;
      if (rl.lod != c3l::kRunLodAlways && rl.lod != want) {
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
#pragma GCC unroll 4
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
    shown_out[r] = s;
    shown += s;
    if (LOD && s && lod_version) {
      (lod_version == c3l::kRunLodFar ? lod_tris2 : lod_tris1) += run.index_count / 3;
    }
  }
  out->shown = shown;
  out->view_hidden = view_hidden;
  out->view_tris = view_tris;
  out->lod_hid = lod_hid;
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

}  // namespace

// (AI-assisted, levellod) CtrHaze.h: the last frame a level with a vista tier was drawn near
static unsigned long long g_haze_vista_frame = 0;
void ctr_haze_note_vista(unsigned long long frame) {
  g_haze_vista_frame = frame + 1;  // (+ 1: frame 0 counts too)
}
bool ctr_haze_vista(unsigned long long frame) {
  // this frame's, or the last one's (merc may be drawn before the level's background)
  return g_haze_vista_frame && g_haze_vista_frame >= frame;
}

double ctr_level_tod_take_ms() {
  const double ms = g_tod_frame_ms;
  g_tod_frame_ms = 0;
  return ms;
}

void ctr_level_set_detail(float t) {
  g_detail_t = detail_at(detail_q(t));  // (on the grid of the held positions)
}

void ctr_level_set_screen_black(bool black) {
  g_screen_black = black;
}

void ctr_level_set_detail_urgent(bool urgent) {
  g_detail_urgent = urgent;
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
    if (!lev->env_draws.empty() || !lev->wind_insts.empty()) {
      // (AI-assisted) Jak 1 sends wind data then the reflection tint in the tie bucket;
      // Jak II sends its prototype mask then the tint. Both are two PC_PORT transfers.
      using B = jak1::BucketId;
      const bool slot0 = m_id == (int)B::TFRAG_LEVEL0 || m_id == (int)B::TFRAG_DIRT_LEVEL0 ||
                         m_id == (int)B::TFRAG_ICE_LEVEL0 ||
                         m_id == (int)B::TFRAG_TRANS0_AND_SKY_BLEND_LEVEL0;
      m_proto_bucket[0] = (int)(slot0 ? B::TIE_LEVEL0 : B::TIE_LEVEL1);
      m_wind_seen = false;
      env_color_scan(*lev, rs);
      m_env_frames += !lev->env_draws.empty();
      if (m_wind_seen && !lev->wind_insts.empty()) {
        wind_update(*lev, rs.frame_idx);
      }
    }
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
    if (!lev->env_draws.empty()) {
      env_color_scan(*lev, rs);  // (AI-assisted, j2water2) c3l v21
      m_env_frames++;
    }
    draw_level(*lev, pc_data.camera, rs);
  }
  log_stats(rs, lev);
}

// (AI-assisted, j2water) Jak II: the level's blended and ordered draws held back by draw_level, at
// the slot's water buckets (after the merc buckets), with the level's fog of the frame
void CtrTfragRenderer::draw_jak2_late(const CtrRenderState& rs) {
  if ((m_late_calls.empty() && m_env_late.empty()) || m_late_frame != rs.frame_idx) {
    m_late_calls.clear();
    m_env_late.clear();
    return;
  }
  ctr_gpu_set_mesh_fog(m_late_fog0, m_late_fog1, m_late_fog_rgb[0], m_late_fog_rgb[1],
                       m_late_fog_rgb[2]);
  if (!m_late_calls.empty()) {
    ctr_gpu_draw_mesh_calls(m_late_calls.data(), (int)m_late_calls.size());
  }
  m_late_calls.clear();
  draw_env_calls(m_env_late);  // (AI-assisted, j2water2) the blended envmapped ties
  ctr_gpu_set_mesh_nofog(0);
  ctr_gpu_cull_reset();
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

// (AI-assisted, j2water2) the time of day's env color for the envmapped ties' shine: the transfer
// after the hidden prototypes' names in the slot's tie bucket (tie-methods.gc: add-pc-tfrag3-data,
// pc-add-tie-vis-mask, pc-add-tie-envmap-info: *time-of-day-context* current-env-color, GS units),
// read ahead as proto_vis_scan does. Kept when the bucket has none this frame.
void CtrTfragRenderer::env_color_scan(const CtrLevelData& lev, const CtrRenderState& rs) {
  const int id = m_proto_bucket[0];
  if (id < 0 || !rs.ee_mem) {
    return;
  }
  const u32 end = rs.buckets_base + 16 * (u32)(id + 1);
  DmaFollower dma(rs.ee_mem, rs.buckets_base + 16 * (u32)id);
  int after = -1;  // PC_PORT transfers since this level's TfragPcPortData
  for (int n = 0; n < 4096 && dma.current_tag_offset() != end; n++) {
    auto t = dma.read_and_advance();
    if (t.vifcode1().kind != VifCode::Kind::PC_PORT) {
      after = -1;
      continue;
    }
    if (after < 0) {
      if (t.size_bytes == sizeof(TfragPcPortData) &&
          strncmp((const char*)t.data + offsetof(TfragPcPortData, level_name), lev.name.c_str(),
                  11) == 0) {
        after = 0;
      }
      continue;
    }
    if (++after == 1 && m_jak2_slot < 0 && t.size_bytes == sizeof(WindWork)) {
      memcpy(&m_wind_work, t.data, sizeof(WindWork));
      m_wind_seen = true;
    }
    if (after == 2) {
      if (t.size_bytes == 16) {
        const float* c = (const float*)t.data;
        for (int k = 0; k < 3; k++) {
          m_env_color[k] = std::isfinite(c[k]) ? std::clamp(c[k], 0.f, 255.f) : 128.f;
        }
      }
      return;
    }
  }
}

// Tie3::render_tree_wind's do_wind_math for every wind instance in the PC's order (each step moves
// its wind_idx's state), once per 60 Hz frame as the PS2: the game steps the wind time-ratio times
// per drawn frame (update-wind), so this frame's steps are the wind_time it advanced, each with
// the wind_array of its step (a slot written by a later step still holds last frame's value).
// The shear factors are the last step's (lev.wind_k).
void CtrTfragRenderer::wind_update(CtrLevelData& lev, u64 frame) {
  if (lev.wind_frame == frame) {
    return;
  }
  lev.wind_frame = frame;
  constexpr float cx = 0.5f, cy = 100.f, cz = 0.0166f, cw = -1.f;
  const WindWork& w = m_wind_work;
  const u32 t1 = w.wind_time;
  const u32 adv = lev.wind_have_last ? t1 - lev.wind_last_time : 1;
  const u32 n = std::clamp<u32>(adv, 1, 4);
  const u32 written = lev.wind_have_last ? std::min<u32>(adv, 64) : 0;
  float* const vectors = lev.wind_vectors.data();
  for (u32 j = 0; j < n; j++) {
    const u32 tj = t1 - (n - 1 - j);
    const bool last = j + 1 == n;
    for (size_t i = 0; i < lev.wind_insts.size(); i++) {
      const auto& wi = lev.wind_insts[i];
      float* my = vectors + 4 * (size_t)wi.wind_idx;
      const u32 slot = (tj + wi.wind_idx) & 63;
      // written at this frame's step k = written - 1 - (t1 - slot): later than step j: the old value
      const u32 back = (t1 - slot) & 63;
      const bool later = back < written && written - 1 - back > j + (written - n);
      const float* work = later ? lev.wind_last_array[slot] : w.wind_array[slot];
      const float v16x = work[0] - (cx * my[2] + cy * my[0]);
      const float v16z = work[2] - (cx * my[3] + cy * my[1]);
      const float v18x = my[2] + v16x * cz, v18z = my[3] + v16z * cz;
      const float v17x = std::min(my[0] + v18x * cz, 1.f), v17z = std::min(my[1] + v18z * cz, 1.f);
      const float kx = std::max(v17x, cw) * wi.stiffness, kz = std::max(v17z, cw) * wi.stiffness;
      if (last) {
        lev.wind_k[2 * i] = std::isfinite(kx) ? kx : 0.f;
        lev.wind_k[2 * i + 1] = std::isfinite(kz) ? kz : 0.f;
      }
      if (!w.paused) {
        my[0] = kx;
        my[1] = kz;
        my[2] = v18x;
        my[3] = v18z;
      }
    }
  }
  lev.wind_last_time = t1;
  lev.wind_have_last = true;
  memcpy(lev.wind_last_array, w.wind_array, sizeof(lev.wind_last_array));
}

// one sequence for every level's renderer: the GPU keeps the last rows by this id
static u32 g_wind_rows_id = 0;

// the wind tie draws collected by draw_level (their rows in m_wind_rows)
void CtrTfragRenderer::draw_wind_calls(const CtrLevelData& lev) {
  const u64 t0 = ctr_gpu_ticks();
  // opaque (the early and sorted lists' wind draws): a chunk's draws together, so its matrix,
  // wind rows and attribute buffers are sent once
  std::stable_sort(m_wind_calls.begin(), m_wind_calls.end(),
                   [](const WindCall& a, const WindCall& b) { return a.mesh < b.mesh; });
  for (const auto& c : m_wind_calls) {
    ctr_gpu_set_mesh_nofog(c.nofog);
    ctr_gpu_draw_mesh_wind(c.state, c.matrix, m_wind_rows.data() + 4 * c.row_first,
                           (int)c.row_count, c.rows_id, c.mesh, lev.wind_mesh, c.slots_offset,
                           c.first, c.count);
    m_wind_tris += c.count / 3;
  }
  m_wind_draws += m_wind_calls.size();
  m_wind_calls.clear();
  m_back_pending += ctr_gpu_ticks() - t0;
}

// (AI-assisted, j2water2) c3l v21: the envmapped tie draws collected by draw_level
void CtrTfragRenderer::draw_env_calls(std::vector<EnvCall>& calls) {
  if (calls.empty()) {
    return;
  }
  const u64 t0 = ctr_gpu_ticks();
  // each run of opaque draws by chunk (their uniforms and buffers sent once per chunk), the others
  // in their order
  for (size_t i = 0; i < calls.size();) {
    size_t j = i;
    while (j < calls.size() && calls[j].state->blend == CTR_BLEND_OFF && calls[j].state->zwrite) {
      j++;
    }
    std::stable_sort(calls.begin() + i, calls.begin() + j,
                     [](const EnvCall& a, const EnvCall& b) { return a.mesh < b.mesh; });
    i = j + 1;
  }
  for (const auto& c : calls) {
    ctr_gpu_etie_draw(c.state, c.env_tex, &c.d, c.mesh, c.normals_mesh, c.normals_offset, c.first,
                      c.count);
    m_env_tris += c.count / 3;
  }
  m_env_draws += calls.size();
  ctr_gpu_etie_end();
  calls.clear();
  m_back_pending += ctr_gpu_ticks() - t0;
}

void CtrTfragRenderer::log_stats(const CtrRenderState& rs, CtrLevelData* lev) {
  if (rs.log_now && m_wind_draws && m_level_draws) {
    lg::info("[ctr] {}: wind ties: {:.1f} draws, {:.0f} tris per level draw", m_name,
             (double)m_wind_draws / m_level_draws, (double)m_wind_tris / m_level_draws);
    m_wind_draws = m_wind_tris = 0;
  }
  if (rs.log_now && (m_shrub_run_hid || m_fade_runs) && m_level_draws) {
    // (AI-assisted, titlemove) shrub runs beyond their distance in chunks that were drawn
    lg::info("[ctr] {}: shrub runs hidden by distance {:.1f}, fading draws {:.1f} per level draw",
             m_name, (double)m_shrub_run_hid / m_level_draws, (double)m_fade_runs / m_level_draws);
    m_shrub_run_hid = 0;
    m_fade_runs = 0;
  }
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
  if (rs.log_now && m_cond_draws) {
    lg::info("[ctr] {}: culling by opening planes: {:.1f}% of {} draws culled", m_name,
             100.0 * m_cond_culled / m_cond_draws, m_cond_draws);
    m_cond_draws = m_cond_culled = 0;
  }
  if (rs.log_now && m_cull_frames) {
    lg::info("[ctr] {}: run culling tested {:.0f} runs, hid {:.0f} chunks and {:.0f} tris per level "
             "draw ({} level draws)",
             m_name, (double)m_cull_runs / m_cull_frames, (double)m_cull_chunks / m_cull_frames,
             (double)m_cull_tris / m_cull_frames, m_cull_frames);
    m_cull_runs = m_cull_tris = m_cull_chunks = m_cull_frames = 0;
  }
  if (rs.log_now && m_vista_frames) {
    lg::info("[ctr] {}: vista {:.1f} chunks {:.0f} tris, low resolution chunks in near mode {:.1f} "
             "per level draw ({} level draws)",
             m_name, (double)m_vista_chunks / m_vista_frames,
             (double)m_vista_tris / m_vista_frames, (double)m_lowres_near / m_vista_frames,
             m_vista_frames);
    m_vista_chunks = m_vista_tris = m_lowres_near = m_vista_frames = 0;
  }
  if (rs.log_now && m_env_frames) {
    // (AI-assisted, j2water2) c3l v21
    lg::info("[ctr] {}: envmapped ties: {:.1f} draws, {:.0f} tris per level draw with them ({} "
             "level draws), env color {:.0f} {:.0f} {:.0f}",
             m_name, (double)m_env_draws / m_env_frames, (double)m_env_tris / m_env_frames,
             m_env_frames, m_env_color[0], m_env_color[1], m_env_color[2]);
    m_env_draws = m_env_tris = m_env_frames = 0;
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
             "tod {:.3f} lists {:.3f} flush {:.3f}; "
             "{:.0f} chunks emitted, {:.0f} runs tested, {:.0f} draws walked, {:.0f} calls; "
             "clusters stood for {:.0f} chunks, {:.0f} tie LOD groups ({} level draws)",
             m_name, tot, ms[kPhSetup], ms[kPhLod], ms[kPhChunks], ms[kPhTod], ms[kPhLists],
             ms[kPhFlush], m_ph_emits / n, m_ph_runs / n, m_ph_walked / n, m_ph_calls / n,
             m_ph_skipped / n, m_ph_lod_skipped / n, m_ph_draws);
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
  if (rs.log_now && m_tf_frames) {
    // the PS2's tfrag geomorph (c3l::ExtTfrag)
    const double n = (double)m_tf_frames;
    lg::info("[ctr] {}: tfrag geomorph: groups near / mid / far {:.0f} / {:.0f} / {:.0f}, {:.0f} "
             "near + mid and {:.0f} far version tris drawn, {:.0f} morph colors written per level "
             "draw ({} level draws)",
             m_name, m_tf_groups[0] / n, m_tf_groups[1] / n, m_tf_groups[2] / n, m_tf_tris[0] / n,
             m_tf_tris[1] / n, m_tf_colors_written / n, m_tf_frames);
    m_tf_groups[0] = m_tf_groups[1] = m_tf_groups[2] = m_tf_tris[0] = m_tf_tris[1] = 0;
    m_tf_colors_written = m_tf_frames = 0;
  }
  if (rs.log_now && m_tod_frames) {
    lg::info("[ctr] {}: time of day {} blends, {:.0f} vertices recolored ({:.0f} light) {:.2f} ms "
             "per level draw (most {} vertices, {:.2f} ms; {} level draws); {} groups rewritten, the table "
             "refreshed whole in {} level draws ({} heavy frames took no new weights); a pulsing "
             "palette held in {} level draws; {} group rewrites spread to a later frame; {} groups "
             "behind with no color changed",
             m_name, m_tod_blends, (double)m_tod_verts / m_tod_frames,
             (double)m_tod_light_verts / m_tod_frames, m_tod_ms / m_tod_frames,
             m_tod_max, m_tod_max_ms, m_tod_frames, m_tod_spread, m_tod_fresh, m_tod_skipped,
             m_tod_held, m_tod_deferred, m_tod_unchanged);
    m_tod_frames = m_tod_blends = m_tod_verts = m_tod_max = m_tod_held = m_tod_light_verts = m_tod_deferred = 0;
    m_tod_unchanged = 0;
    m_tod_spread = m_tod_fresh = m_tod_skipped = 0;
    m_tod_ms = m_tod_max_ms = 0;
  }
}

// (AI-assisted, gpu-rt) the pass's draws collected by draw_level, to the backend (0: the main
// pass, 1: the vista pass)
void CtrTfragRenderer::flush_calls(int /*pass*/) {
  if (!m_calls.empty()) {
    const u64 t0 = ctr_gpu_ticks();
    ctr_gpu_draw_mesh_calls(m_calls.data(), (int)m_calls.size());
    m_back_pending += ctr_gpu_ticks() - t0;
    m_ph_calls += m_calls.size();
  }
  m_calls.clear();
}

CtrTfragRenderer::~CtrTfragRenderer() = default;

void CtrTfragRenderer::draw_level(CtrLevelData& lev,
                                  const CtrBackgroundCamera& cam,
                                  const CtrRenderState& rs) {
  // (AI-assisted, j2cull) the phases' ticks (m_ph)
  u64 ph_t = ctr_gpu_ticks();
  // (the backend's calls made in a phase count as flush)
  m_back_pending = 0;
  auto phase = [&](int k) {
    const u64 now = ctr_gpu_ticks();
    m_ph[k] += now - ph_t - m_back_pending;
    m_ph[kPhFlush] += m_back_pending;
    m_back_pending = 0;
    ph_t = now;
  };
  m_ph_draws++;
  if (!lev.cull_made || lev.cull_sizes[0] != lev.chunks.size() ||
      lev.cull_sizes[1] != lev.lod_groups.size() || lev.cull_sizes[2] != lev.chunk_unit.size()) {
    make_cull_clusters(lev);  // (AI-assisted, j2cull) once per load
  }
  const CtrSettings& settings = ctr_settings();
  // (AI-assisted, autodetail) the auto_detail position and the relief's knobs at it
  const float t_pos = settings.auto_detail ? g_detail_t : 0.f;
  ReliefAt relief(settings);
  const ctr_detail::Knobs knobs = relief.at(t_pos);
  {
    const u32 taken = g_pop.taken;  // (AI-assisted, title3: CTR_POP_LOG; taken since the last draw)
    g_pop = PopLog();
    g_pop.taken = taken;
  }
  auto R = make_cam_mat(cam.rot, cam.perspective, cam.fog.x(), cam.hvdf_off.z());
  m_late_in_view = 0;  // (AI-assisted, j2intro) counted below, reported after the chunk loop
  // (AI-assisted, titlemove) the title screen (the title level loaded): its attract camera flies a
  // fixed path over Sandover. No tie group switches LOD (geometry 3 -> 0) nearer than
  // kTitleLodMin there: at the gameplay distances (the PS2 far-plane, 35-75 m for the island) the
  // island's palms, grass, hut and rock switched one after another on screen as the flight came
  // nearer (807 switches in view in 40 s in Azahar; 81 in its first ~8 s on the host, objects up to
  // 137 px across), each a step in shape and in the vertex colors (the far versions' own shading)
  m_levels->loaded(&m_loaded);
  bool title_on = false;
  for (const auto* l : m_loaded) {
    title_on = title_on || (m_jak2_slot < 0 && l->name == "title");  // (Jak 1's title level)
  }
  float title_min = kTitleLodMin;
#ifndef __3DS__
  if (const char* e = getenv("CTR_TITLE_LOD_MIN")) {
    title_min = (float)atof(e);  // (host test: meters; 0 = as in gameplay)
  }
#endif
  // (AI-assisted, title3) shown again after a gap (the title's reveal of Sandover behind the logo,
  // a level's start): reveal_frame (late near chunks in view)
  {
    // (AI-assisted, title3) this frame's tie LOD factor for the hidden levels' near chunks
    // (CtrLevels::stream_update), as the tie LOD block below works it out
    const float pzw = std::abs(cam.perspective[2][3]);
    const float focal =
        pzw > 0 ? 200.f * std::abs(cam.fog.x() * cam.perspective[0][0] / 256.f) / pzw : 0.f;
    if (settings.tie_lod && focal > 0) {
      m_levels->note_k_any(settings.detail_scale * focal / std::max(knobs.tie_lod_px, 0.25f) *
                           1.1f);
    }
  }
  const bool reveal = lev.last_draw_frame + 2 < rs.frame_idx;
  lev.last_draw_frame = rs.frame_idx;
  if (reveal) {
    lev.reveal_frame = rs.frame_idx;
  }
  // (AI-assisted) c3l v16 time of day: blend the level's colors with the game's weights when they
  // moved (the chunks are recolored as they are drawn, emit_chunk)
  u64 tod_verts = 0;
  double tod_ms = 0;
  if (lev.tod) {
    const double t0 = tod_now_ms();
    m_tod_frames++;
    const u32 held0 = lev.tod->pulse_held;
    // (AI-assisted, fable1) a heavy frame (the frame before drew 3x the table's entries in
    // vertices or more: the title's fast day, citadel's pulse, where a move comes nearly every
    // frame and every group drawn is rewritten after it) takes the game's new weights every second
    // frame only: the table and the groups are then rewritten once per two frames, together, and
    // the colors drawn are at most one frame behind the PC's blend (2-8 color units at the title's
    // 20x day; the same for every group). Half the time of day work there; gameplay (a move every
    // few frames, few vertices) is unchanged. Azahar, title: performance.md "Time of day".
    // (AI-assisted, j2pop) Jak II: every third frame (at most two frames behind). Its city levels'
    // light palettes move every frame (mood-funcs.gc update-mood-ctysluma: the neon's random
    // 0.8-1.0 flicker, the flames), so each update rewrites every light vertex in view: HW Oct
    // 25, the title flight over the slums (ctysluma, 0 day moves) 5.1-5.3 ms of time of day per
    // frame, build 22-27 ms, 40-66 of 150 frames at 3 vblanks (the game thread waiting 5-10 ms
    // per frame for the render thread). A flicker at 10 Hz instead of 15 looks the same.
    // Jak 1 leaves frames out on the title only: in play (village2 draws 3x its table every frame)
    // every frame's weights are drawn. Jak II never leaves out a frame where a day palette jumps
    // (a lightning flash: a new weight every frame).
    // Jak 1's title takes new weights every fourth frame (its 20x day moves every palette)
    const u64 tod_period = g_game_version == GameVersion::Jak2 ? 3 : 4;
    const bool tod_skip =
        m_tod_heavy && (rs.frame_idx % tod_period) != 0 &&
        (g_game_version == GameVersion::Jak2 ? !tod_day_jump(*lev.tod, cam) : title_on);
    m_tod_skipped += tod_skip;
    if (!tod_skip) {
      // (AI-assisted, levellod) no pulse hold on the title screen (its fast day: todlag)
      m_tod_blends += tod_update(*lev.tod, lev.name, cam, rs.frame_idx, title_on) == 2;
    }
    m_tod_held += lev.tod->pulse_held != held0;  // (AI-assisted, citadel) tod_pulse_step
    tod_ms += tod_now_ms() - t0;
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
  const float lod_dist = knobs.lod_distance * 4096.f;
  const float lod_dist_cfg = settings.lod_distance * 4096.f;  // (far mode's landmark rule)
  // camera outside the level's bounds by more than far_level_distance: the level is only seen
  // from a neighbouring one
  bool far_level = false;
  // (AI-assisted, streammem) the level's tfrag by far mode's rules (its far tier) while a partial
  // unload's tier 1 / 2 tfrag isn't all back; far_level for everything else
  bool far_tfrag = false;
  bool far_inside = true;  // (AI-assisted, levellod) the camera within the near switch (chunk loop)
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
    far_inside = d2 <= (m * 0.9f) * (m * 0.9f);
    // (AI-assisted) partial unload (the user's idea): a level seen only from far away for a few
    // seconds gives back the memory of the chunks far mode doesn't draw; read again when the camera
    // is within 30 m of the far distance (freed beyond 60 m: no back and forth at one edge)
    // (AI-assisted, streammem, F3) also when the camera will be there in kReloadLeadFrames at its
    // speed (the zoomer, running), and at once after a camera cut within the unload distance (a
    // respawn, a cutscene). Until the meshes are back the level was drawn in far mode, the camera
    // inside it too (coarse tfrag and landmarks only for the 1.5-2 s citadel's read takes, longer
    // when it failed); now near mode draws it with what is there: the tie LOD groups' far versions
    // and the far tier's tfrag are always there (far_tfrag), the rest comes as it is read (in jobs
    // of kDetailJobBytes, each kept). A partly read back level far away again is freed again.
    constexpr float kUnloadMeters = 60.f, kReloadMeters = 30.f;
    constexpr float kReloadLeadFrames = 60.f;  // 2 s at 30 fps (the camera's velocity is per frame)
    constexpr u32 kUnloadFrames = 300;
    const float unload_d = m + kUnloadMeters * 4096.f, reload_d = m + kReloadMeters * 4096.f;
    lev.far_frames = d2 > unload_d * unload_d ? lev.far_frames + 1 : 0;
    if (lev.far_frames > kUnloadFrames && !lev.detail_requested) {
      m_levels->unload_detail(lev);  // (resets far_frames; logs only what it freed)
    } else if (!lev.detail_loaded && !lev.detail_requested &&
               rs.frame_idx >= lev.detail_retry_frame) {
      float lead[3];
      float dl2 = 1e30f;
      if (m_levels->camera_lead(kReloadLeadFrames, lead)) {
        dl2 = 0;
        for (int c = 0; c < 3; c++) {
          const float o = lead[c] < lev.bbox_min[c]
                              ? lev.bbox_min[c] - lead[c]
                              : (lead[c] > lev.bbox_max[c] ? lead[c] - lev.bbox_max[c] : 0.f);
          dl2 += o * o;
        }
      }
      const u64 cut = m_levels->stream_cut_frame();
      const bool after_cut = cut && rs.frame_idx <= cut + 3;
      if (d2 < reload_d * reload_d || dl2 < reload_d * reload_d ||
          (after_cut && d2 < unload_d * unload_d)) {
        m_levels->request_detail(lev);
      }
    }
    far_tfrag = far_level || (!lev.detail_loaded && lev.detail_tfrag_missing > 0);
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
  const bool early_depth = ctr_gpu_early_depth_active() != 0;  // (AI-assisted) the band order
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
    // (AI-assisted, levellod) the port's haze, only where its distances cut geometry (CtrHaze.h):
    // the main pass towards draw_distance, or for a level with a vista tier (its geometry goes on
    // in the vista copies) towards vista_distance like the vista pass. Was: the vista from
    // draw_distance on, and a level with a vista tier from fog_start x draw_distance (300 m):
    // finalboss's far view 26-63% fog color at 1-2 km where the PC has none
    ctr_haze_row(settings, false, lev.has_vista, pzw, fog1);
    ctr_haze_row(settings, true, lev.has_vista, pzw, fog1_vista);
    if (lev.has_vista && !far_level) {
      ctr_haze_note_vista(rs.frame_idx);  // (merc's haze: CtrHaze.h)
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
  set_tex_priority(lev.tex_pools, far_level ? 1 : 2);
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
  // (tie_k0 0: no tie distance limit, at any auto_detail position: d = 0 below)
  const float tie_k_span =
      tie_k0 > 0 ? std::max(settings.tie_distance_scale_max, tie_k0) - tie_k0 : 0.f;
  const float t_now = std::max(t_pos, 0.f);
  const float shrub_k = shrub_k0 + shrub_k_span * t_now;
  const float shrub_d = shrub_d0 + shrub_d_span * t_now;
  // (AI-assisted, autodetail) below 0 the relief's tie distance (0: none)
  auto tie_k_at = [&](float t) {
    return t >= 0 ? tie_k0 + tie_k_span * t : relief.at(t).tie_distance_scale;
  };
  const float tie_k = t_pos >= 0 ? tie_k0 + tie_k_span * t_now : knobs.tie_distance_scale;
  const bool shrub_ps2 = lev.shrub_ps2_dist;
  const bool shrub_fixed = settings.shrub_distance > 0;
  const float shrub_fixed_dist = shrub_d * 4096.f * det;
  // (AI-assisted, title-popin) a chunk's own distance limit (0: none) at auto_detail position t
  auto chunk_limit = [&](const c3l::Chunk& ch, float t) {
    const float tp = std::max(t, 0.f);
    if (ch.kind == c3l::CHUNK_KIND_SHRUB && shrub_ps2) {
      return ch.max_dist * kShrubFarK * (shrub_k0 + shrub_k_span * tp) * det;
    } else if (ch.kind == c3l::CHUNK_KIND_SHRUB && shrub_fixed) {
      return (shrub_d0 + shrub_d_span * tp) * 4096.f * det;
    } else if (ch.kind == c3l::CHUNK_KIND_TIE) {
      return ch.max_dist * tie_k_at(t) * det;
    }
    return ch.max_dist * det;
  };
  const u8 q_now = detail_q(t_pos);
  if (lev.chunk_detail_q.size() != lev.chunks.size()) {
    lev.chunk_detail_q.assign(lev.chunks.size(), 0xff);
  }
  u8* const chunk_q = lev.chunk_detail_q.data();
  const float cx = cam.trans[0], cy = cam.trans[1], cz = cam.trans[2];
  // (AI-assisted, streaming) a chunk's distance decision (draw distance, its own limit, 10%
  // hysteresis, auto_detail's held distances, kept while in view and moving away): the chunk loop's,
  // and the near chunks' (before the tie LOD groups, which need them). Updates the chunk's state.
  if (m_shrub_lim.size() < lev.chunks.size()) {
    m_shrub_lim.resize(lev.chunks.size());
  }
  if (lev.run_dist_in.size() != lev.vis_runs.size()) {
    lev.run_dist_in.assign(lev.vis_runs.size(), 0);
  }
  auto decide_dist = [&](size_t ci, const c3l::Chunk& ch, u8& state,
                         bool vista_chunk) __attribute__((always_inline)) {
    bool dist_ok = true;
    {
      // draw distance; small objects only up close
      const float base_dist = vista_chunk ? vista_dist : draw_dist;
      float max_dist = base_dist;
      float d;
      if (ch.kind == c3l::CHUNK_KIND_SHRUB && shrub_ps2) {
        // (AI-assisted) c3l v14: the shrubs' PS2 far plane (kShrubFarK; the runs thin out before it)
        d = ch.max_dist * kShrubFarK * shrub_k * det;
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
        // (AI-assisted, titlemove) only for going: a chunk the new distances show is shown (held
        // back, a shrub cell culled while the GPU was busy came in only once the camera was within
        // the old, shorter distance: close up)
        if (auto_detail && cq != q_now && cq != 0xff && d > 0 && !dist_ok &&
            (ch.kind == c3l::CHUNK_KIND_SHRUB || ch.kind == c3l::CHUNK_KIND_TIE)) {
          const float d_old = chunk_limit(ch, detail_at(cq));
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
        // distance (not beyond the draw distance). (AI-assisted, titlemove) not shrubs: they
        // follow the PS2's distances (the user: "small things like shrubs pop in when i walk close,
        // but if i walk away they stay"); their runs are culled by distance one by one too
        // (emit_chunk)
        if (was_in && !dist_ok && dd > 0 && (base_dist <= 0 || dd < base_dist) &&
            ch.kind != c3l::CHUNK_KIND_SHRUB) {
          if (in_view < 0) {
            in_view = sphere_in_view(cam, ch.bsphere);
          }
          if (in_view) {
            dist_ok = within(limit_with(dd * kHoldInView), 1.f);
          }
        }
        if (pop_log_on() && was_in != dist_ok && sphere_in_view(cam, ch.bsphere)) {
          (dist_ok ? g_pop.chunk_in : g_pop.chunk_out)[ch.kind & 3]++;
          lg::info("[popC] f={} {} ci={} kind={} {} d={:.1f} lim={:.1f} r={:.1f}", rs.frame_idx,
                   lev.name, ci, ch.kind, dist_ok ? "in" : "out", std::sqrt(d2) / 4096.f,
                   max_dist / 4096.f, ch.bsphere[3] / 4096.f);  // (titlemove: which chunk)
        }
        state = (u8)(dist_ok ? (state | 4) : (state & ~4));
        if (ch.kind == c3l::CHUNK_KIND_SHRUB && ci < m_shrub_lim.size()) {
          m_shrub_lim[ci] = limit_with(dd);  // (AI-assisted, titlemove) for its runs
        }
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
    // (AI-assisted, titlemove) with this frame's factor too (stream_k is the last frame's: after a
    // cut to another focal length units were decided beyond their bound for a frame and their
    // groups drawn far)
    float kb = lev.stream_k * 1.25f;
    if (settings.tie_lod && lev.lod_error.size() == lev.lod_groups.size()) {
      const float pzw = std::abs(cam.perspective[2][3]);
      const float focal =
          pzw > 0 ? 200.f * std::abs(cam.fog.x() * cam.perspective[0][0] / 256.f) / pzw : 0.f;
      kb = std::max(kb, settings.detail_scale * focal / std::max(knobs.tie_lod_px, 0.25f) *
                            1.1f * 1.25f);
    }
    for (size_t u = 0; u < lev.stream_units.size(); u++) {
      const auto& su = lev.stream_units[u];
      const u32 ci = su.chunk;
      if (far_level || su.state == CtrLevelData::kUnitNever) {
        continue;
      }
      {
        const float* c = lev.chunks[ci].bsphere;
        const float dx = c[0] - cx, dy = c[1] - cy, dz = c[2] - cz;
        const float lim = su.bound(kb, lev.lod_floor_k, lev.lod_min_e);
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
#ifndef __3DS__
  if (getenv("CTR_LOD_LOG") && rs.frame_idx % 30 == 0 &&
      (g_lod_far_why[0] + g_lod_far_why[2] + g_lod_far_why[3])) {
    lg::info("[lodlog] {}: group frames in view within their distance but far (30 frames): late {} "
             "(within 20 m: {}), beyond the near chunk's distance {} ({}), no near chunk {} ({})",
             lev.name, g_lod_far_why[3], g_lod_far_close[3], g_lod_far_why[2], g_lod_far_close[2],
             g_lod_far_why[0], g_lod_far_close[0]);
    memset(g_lod_far_why, 0, sizeof(g_lod_far_why));
    memset(g_lod_far_close, 0, sizeof(g_lod_far_close));
  }
#endif
  u32 late_view = 0;    // (AI-assisted, title3) ... late and in view
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
    // (AI-assisted, autodetail) with shape errors: the relief's tie_lod_px at t
    const bool lod_auto = settings.auto_detail && (by_error ? relief.rl.stage[ctr_detail::kTieLodPx] >= 0 : true);
    auto k_at = [&](float t) {
      if (by_error) {
        return k0 * focal / std::max(relief.at(t).tie_lod_px, 0.25f);
      }
      return lod_auto ? k0 * (0.5f + 0.5f * std::max(t, 0.f)) : k0;
    };
    const float k = k_at(t_pos);
    // (AI-assisted, lodnear) the PS2 floor: by screen-space error alone (4 px of the 400-wide top
    // screen) half of the groups switched to geometry 3 nearer than their PS2 far-plane, some at
    // 3-15 m (jungle, beach: "coarse models really close"); the PS2 morphs toward geometry 3 only up
    // to the far-plane. So a group is near within max(error x k, PS2 far-plane x detail_scale):
    // never coarser than the PS2, and where the PS2 distance decides, its far version's error at
    // the switch is under tie_lod_px
    lev.lod_floor_k = by_error && k > 0 ? k0 / k : 0.f;
    // (AI-assisted, titlemove) the title: no group switches nearer than kTitleLodMin meters
    lev.lod_min_e = title_on && by_error && k > 0 ? title_min * 4096.f / k : 0.f;
#ifndef __3DS__
    static const bool no_floor = getenv("CTR_LOD_NO_FLOOR") != nullptr;  // (host A/B: as before)
    if (no_floor) {
      lev.lod_floor_k = 0.f;
    }
#endif
    if (lev.lod_detail_q.size() != lev.lod_groups.size()) {
      lev.lod_detail_q.assign(lev.lod_groups.size(), 0xff);
    }
    u8* const lod_q = lev.lod_detail_q.data();
    if (has_units && lev.lod_hold_since.size() != lev.lod_groups.size()) {
      lev.lod_hold_since.assign(lev.lod_groups.size(), 0);
    }
    // (AI-assisted, streaming) the longest a group drawn far for want of its data is kept far in
    // view once the data came (1 s at 30 fps)
    constexpr u32 kHoldFrames = 30;  // (AI-assisted, lodnear: was 120)
    // the PS2's geomorph (c3l::ExtMorph): its distances x detail_scale, pulled in by auto_detail's
    // relief (tie_lod_px 1 -> 4: x 1 -> 0.5; the morph rows use the same factor, so it shows no
    // switch)
    const bool morph_on = !lev.group_mid.empty();
    float kd = k0;
    if (morph_on && settings.auto_detail) {
      const float px0 = std::max(settings.tie_lod_px, 0.25f);
      kd = k0 * std::sqrt(px0 / std::max(relief.at(t_pos).tie_lod_px, px0));
    }
    auto dx2 = [&](const float* c) {
      const float x = c[0] - cx, y = c[1] - cy, z = c[2] - cz;
      return x * x + y * y + z * z;
    };
    lev.morph_k = kd;
#ifndef __3DS__
    // host A/B, env CTR_MORPH_AB: on odd frames 1 every morph group's near version, 2 no morph
    static const int morph_ab = getenv("CTR_MORPH_AB") ? atoi(getenv("CTR_MORPH_AB")) : 0;
#endif
    // (AI-assisted, j2cull) cluster by cluster (CtrLevelData::CullCluster): a cluster beyond every
    // member's near distance (largest switch distance x k, with margins for float rounding) whose
    // members all were far and not held last frame (state 2) stands for their decisions: each
    // would come out far again, state 2, its lod_detail_q this frame's (only so without lod_auto:
    // its in-view hold reads them)
    const u8* const lod_st = lev.lod_state.data();
    const bool lod_skip = (by_error || !lod_auto) && lev.lod_state.size() == lev.lod_groups.size();
    const bool cull_check = cull_check_enabled();
    bool lod_check = false;
    std::vector<u8> chk_lod_q;
    u32 chk_near = 0;
    for (const auto& gcl : lev.lod_clusters) {
      if (lod_skip && !(gcl.flags & CtrLevelData::kClPlain)) {
        // (lod_switch_error: lodnear's PS2 floor and titlemove's title minimum raise the error)
        const float sw = by_error ? std::max(std::max(gcl.raw[0], gcl.raw[1] * lev.lod_floor_k),
                                             lev.lod_min_e)
                                  : gcl.raw[1];
        bool far = !(k > 0);
        if (!far) {
          const float dx = gcl.c[0] - cx, dy = gcl.c[1] - cy, dz = gcl.c[2] - cz;
          const float reach = (gcl.a + std::max(sw, 0.f) * k) * 1.0002f + 256.f;
          far = dx * dx + dy * dy + dz * dz > reach * reach;
        }
        if (far) {
          u32 g = gcl.first;
          const u32 end = gcl.first + gcl.count;
          while (g < end && lod_st[g] == 2 && (!lod_auto || lod_q[g] == q_now)) {
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
      if (morph_on && lev.group_mid[g] > 0) {
        // the PS2's rule (c3l::ExtMorph) by the group's nearest point (its sphere: no instance is
        // nearer): the near version within the mid-plane, the mid version within the far-plane;
        // each instance morphs by its own distance, and every version draws that surface (geometry
        // 2 with geometry 0's colors): no hysteresis. Geometry 3 has colors of its own (fitted to
        // the near surface): the title's flight (titlemove) has none nearer than kTitleLodMin
        const float dv = std::sqrt(dx2(lg.bsphere)) - lg.bsphere[3];
        const float tmin = lev.lod_min_e * k;
        u8 v = !(kd > 0)                                    ? 2
               : dv < lev.group_mid[g] * kd                 ? 0
               : dv < std::max(lg.dist * kd, tmin)          ? 1
                                                            : 2;
#ifndef __3DS__
        if (morph_ab == 1 && (rs.frame_idx & 1)) {
          v = 0;  // (host A/B: every group's near version, the surface the same)
        }
#endif
        if (v < 2 && has_units) {
          bool ok = g < lev.group_unit_count.size() && lev.group_unit_count[g] > 0;
          bool late = false;
          const u32* gu = ok ? &lev.group_units[lev.group_unit_first[g]] : nullptr;
          for (u32 j = 0; ok && j < lev.group_unit_count[g]; j++) {
            ok = lev.unit_ok[gu[j]] == 1;
            late = late || lev.unit_ok[gu[j]] == 3;
          }
          bool keep_far = (st & 4) != 0 && !g_screen_black;
          if (!ok) {
            v = 2;
            if (late) {
              late_groups++;
              for (u32 j = 0; j < lev.group_unit_count[g]; j++) {
                if (lev.unit_ok[gu[j]] == 3) {
                  m_levels->stream_late(lev, gu[j], rs.frame_idx);
                }
              }
              const bool seen = sphere_in_view(cam, lg.bsphere);
              m_late_in_view += seen;
              late_view += seen;
              if (!keep_far && !g_screen_black && seen) {
                keep_far = true;
                lev.lod_hold_since[g] = (u32)rs.frame_idx;
              }
            }
          } else if (keep_far) {
            // drawn far in view for want of its data: kept so while in view (at most kHoldFrames),
            // a switch at a morph factor short of 1 would show
            if (sphere_in_view(cam, lg.bsphere) &&
                (u32)rs.frame_idx - lev.lod_hold_since[g] < kHoldFrames) {
              v = 2;
              held_groups++;
            } else {
              keep_far = false;
            }
          }
          st = (u8)(st & ~4) | (keep_far ? 4 : 0);
        }
        if (pop_log_on() && (st & 2) && ((st & 1) ? ((st & 8) ? 1 : 0) : 2) != v &&
            sphere_in_view(cam, lg.bsphere)) {
          (v < 2 ? g_pop.lod_up : g_pop.lod_down)++;
          lg::info("[popL] f={} {} g{} morph {} -> {} d={:.1f} mid={:.1f} far={:.1f} r={:.1f}",
                   rs.frame_idx, lev.name, g, (st & 1) ? ((st & 8) ? 1 : 0) : 2, v, dv / 4096.f,
                   lev.group_mid[g] / 4096.f, lg.dist / 4096.f, lg.bsphere[3] / 4096.f);
        }
        lod_q[g] = q_now;
        st = (u8)((st & 4) | 2 | (v < 2 ? 1 : 0) | (v == 1 ? 8 : 0));
        near += v < 2;
        continue;
      }
      const float dx = lg.bsphere[0] - cx, dy = lg.bsphere[1] - cy, dz = lg.bsphere[2] - cz;
      const float d2 = dx * dx + dy * dy + dz * dz;
      const float sw = by_error ? lev.lod_switch_error(g) : lg.dist;
      const float f = (st & 1) ? 1.1f : 1.f;
      auto near_at = [&](float kk) {
        // (AI-assisted, autodetail) at another factor the PS2 floor and the title's least distance
        // stay where they are (lod_switch_error has this frame's factor in them)
        const float swk = by_error && kk != k
                              ? std::max(std::max(lev.lod_error[g] * kk, lg.dist * lev.lod_floor_k * k),
                                         lev.lod_min_e * k)
                              : sw * kk;
        const float lim = swk * f + lg.bsphere[3];
        return kk > 0 && d2 < lim * lim;
      };
      bool n = near_at(k);
      // (AI-assisted, title-popin) no switch in view because auto_detail moved: a group in view
      // keeps the factor it was decided with while the two would choose differently (except a
      // GPU about to miss frames, for the far version)
      bool held = false;
      if (lod_auto && (st & 2) && lod_q[g] != q_now && lod_q[g] != 0xff) {
        const bool n_old = near_at(k_at(detail_at(lod_q[g])));
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
      // (AI-assisted, title3) not while the screen is black: none was seen far
      bool keep_far = (st & 4) != 0 && !g_screen_black;
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
#ifndef __3DS__
            {
              // (AI-assisted, lodnear) host log: groups in view within their distance drawn far,
              // by why (worst unit state: 3 late, 2 beyond the near chunk's draw distance, 0 none)
              static const bool lod_log2 = getenv("CTR_LOD_LOG") != nullptr;
              if (lod_log2 && sphere_in_view(cam, lg.bsphere)) {
                u8 w = 1;
                for (u32 j = 0; j < lev.group_unit_count[g]; j++) {
                  const u8 uo = lev.unit_ok[gu[j]];
                  w = uo == 3 ? 3 : (w == 3 ? 3 : (uo != 1 ? uo : w));
                }
                const float d = std::sqrt(d2) / 4096.f;
                g_lod_far_why[w & 3] += 1;
                if (d < 20.f) {
                  g_lod_far_close[w & 3] += 1;
                }
              }
            }
#endif
            if (late) {
              late_groups++;
              for (u32 j = 0; j < lev.group_unit_count[g]; j++) {
                if (lev.unit_ok[gu[j]] == 3) {
                  m_levels->stream_late(lev, gu[j], rs.frame_idx);
                }
              }
              const bool seen = sphere_in_view(cam, lg.bsphere);
              m_late_in_view += seen;  // (AI-assisted, j2intro) Jak II
              late_view += seen;       // (AI-assisted, title3) Jak 1
              // (AI-assisted, j2intro / title3) not while the screen is black: it switches as
              // soon as the data is there
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
            // group's near distance; near it the error is about tie_lod_px). (AI-assisted,
            // lodnear) but not where the far version is plainly wrong: from 3 x tie_lod_px of
            // error on (the camera within 1/3 of that distance) the near version at once. Held
            // up to 4 s, a continue's late groups stayed coarse 3-15 m from the camera and then
            // switched together
            const float e = by_error ? lev.lod_error[g] : lg.dist;
            const float lim = e * k / 1.5f + lg.bsphere[3], lim_close = e * k / 3.f + lg.bsphere[3];
            if (sphere_in_view(cam, lg.bsphere) && d2 < lim * lim && d2 > lim_close * lim_close &&
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
#ifndef __3DS__
      // (AI-assisted, lodnear) host log, env CTR_LOD_LOG: every switch of a group in view, its
      // distance, the PS2 far-plane and the screen-error distance, and why it is far
      static const bool lod_log = getenv("CTR_LOD_LOG") != nullptr;
      if (lod_log && (st & 2) && ((st & 1) != 0) != n && sphere_in_view(cam, lg.bsphere)) {
        const char* why = n ? "near" : !near_at(k) ? "dist" : keep_far ? "late/held" : "nodata";
        lg::info("[lodlog] {} g{} {} d {:.1f} m (to surface {:.1f}) ps2 far {:.1f} err-dist {:.1f} "
                 "r {:.1f} err {:.3f} m",
                 lev.name, g, why, std::sqrt(d2) / 4096.f, (std::sqrt(d2) - lg.bsphere[3]) / 4096.f,
                 lg.dist / 4096.f, by_error ? lev.lod_error[g] * k / 4096.f : 0.f,
                 lg.bsphere[3] / 4096.f, by_error ? lev.lod_error[g] / 4096.f : 0.f);
      }
#endif
      if (pop_log_on() && (st & 2) && ((st & 1) != 0) != n && sphere_in_view(cam, lg.bsphere)) {
        (n ? g_pop.lod_up : g_pop.lod_down)++;
        lg::info("[popL] f={} {} g{} {} d={:.1f} sw={:.1f} r={:.1f} keep_far={}", rs.frame_idx,
                 lev.name, g, n ? "near" : (!near_at(k) ? "dist" : "nodata/late"),
                 std::sqrt(d2) / 4096.f, (sw * k) / 4096.f, lg.bsphere[3] / 4096.f,
                 keep_far ? 1 : 0);  // (titlemove: which group, why)
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
    lev.lod_floor_k = 0.f;
  }
  // the PS2's tfrag geomorph (c3l::ExtTfrag): each group's version by the view depth of its
  // nearest point (draw-inline-array-tfrag's frag-dists), in clip w as the morph shader's bands.
  // A version in near chunks not there yet: the coarser one (kept while in view for 1 s once
  // the data came, as the tie groups)
  const bool tf_on = !lev.tf_groups.empty() && lev.tf_state.size() == lev.tf_groups.size();
  if (tf_on) {
    const float wv[3] = {-R[0][3], -R[1][3], -R[2][3]};
    const float wl = std::sqrt(wv[0] * wv[0] + wv[1] * wv[1] + wv[2] * wv[2]);
    const float k = settings.detail_scale * wl;
    const float w2 = lev.tf_dists[0] * k, w1 = lev.tf_dists[1] * k, w0 = lev.tf_dists[2] * k;
    m_tf_band[0] = 1.f / (w0 - w1);
    m_tf_band[1] = 1.f / (w1 - w2);
    m_tf_band[2] = -w1 / (w0 - w1);
    m_tf_band[3] = -w2 / (w1 - w2);
    const bool tf_units = has_units && lev.tf_unit_count.size() == lev.tf_groups.size();
    if (lev.tf_hold_since.size() != lev.tf_groups.size()) {
      lev.tf_hold_since.assign(lev.tf_groups.size(), 0);
    }
    constexpr u32 kTfHoldFrames = 30;
    u32 n[3] = {0, 0, 0};
    for (size_t g = 0; g < lev.tf_groups.size(); g++) {
      const auto& tg = lev.tf_groups[g];
      const float* b = tg.bsphere;
      const float w = wv[0] * (b[0] - cx) + wv[1] * (b[1] - cy) + wv[2] * (b[2] - cz) - b[3] * wl;
      u8 v = w < tg.mid * k ? 1 : (w < tg.far * k ? 9 : 0);
#ifndef __3DS__
      // host A/B, env CTR_TFRAG_AB: on odd frames every group's near version
      static const int tf_ab = getenv("CTR_TFRAG_AB") ? atoi(getenv("CTR_TFRAG_AB")) : 0;
      if (tf_ab == 1 && (rs.frame_idx & 1)) {
        v = 1;
      }
#endif
      u8& st = lev.tf_state[g];
      bool hold = (st & 16) != 0;
      const u32 cnt = tf_units ? lev.tf_unit_count[g] : 0;
      if (v && cnt) {
        bool ok = true, late = false;
        const u32* const gu = &lev.tf_units[lev.tf_unit_first[g]];
        for (u32 j = 0; j < cnt; j++) {
          ok = ok && lev.unit_ok[gu[j]] == 1;
          late = late || lev.unit_ok[gu[j]] == 3;
        }
        // (its coarser version in the level's own chunks: the mid one when there is no far one)
        const u8 fallback = tg.far < std::numeric_limits<float>::max() ? 0 : 9;
        if (!ok) {
          if (late) {
            late_groups++;
            for (u32 j = 0; j < cnt; j++) {
              if (lev.unit_ok[gu[j]] == 3) {
                m_levels->stream_late(lev, gu[j], rs.frame_idx);
              }
            }
            const bool seen = sphere_in_view(cam, b);
            m_late_in_view += seen;
            late_view += seen;
            if (!hold && !g_screen_black && seen) {
              hold = true;
              lev.tf_hold_since[g] = (u32)rs.frame_idx;
            }
          }
          v = std::min(v, fallback);
        } else if (hold) {
          if (sphere_in_view(cam, b) && (u32)rs.frame_idx - lev.tf_hold_since[g] < kTfHoldFrames) {
            v = std::min(v, fallback);
            held_groups++;
          } else {
            hold = false;
          }
        }
      } else {
        hold = false;
      }
      // (bit 5: its far point reaches band 1's start, some of its vertices may move)
      const bool moves = w + 2.f * b[3] * wl >= w2;
      st = (u8)(v | (hold ? 16 : 0) | (moves ? 32 : 0));
      n[v == 1 ? 0 : v == 9 ? 1 : 2]++;
    }
    m_tf_groups[0] += n[0];
    m_tf_groups[1] += n[1];
    m_tf_groups[2] += n[2];
    m_tf_frames++;
  }
  if (has_units) {
    lev.stream_frame = rs.frame_idx;
    m_levels->stream_note_level(lev, late_groups, held_groups, rs.frame_idx);
    if (m_jak2_slot < 0) {
      // (AI-assisted, title3) Jak 1 (Jak II reports after the chunk loop: m_late_in_view)
      m_levels->note_late_in_view(rs.frame_idx, late_view, rs.frame_idx < lev.reveal_frame + 90);
    }
  }
  m_levels->stream_note_camera(cam.trans, cam.planes, cam.rot, rs.frame_idx);
  phase(kPhLod);
  if (m_run_nomerge.size() < lev.vis_runs.size()) {
    m_run_nomerge.resize(lev.vis_runs.size());
  }
  if (m_run_fade.size() < lev.vis_runs.size()) {
    m_run_fade.resize(lev.vis_runs.size());
  }
  m_fade_calls.clear();
  m_fade_states.clear();
  // (AI-assisted) a chunk that is to be drawn (mode and distance decided): the view test (the
  // chunk's sphere against the four planes; with run_culling, a chunk that crosses planes has its
  // runs tested against those planes on their own), the visibility bits or (vista tier) the
  // source chunks, then its matrix. vista: drawn in the vista pass.
  // clip = -(R * (origin + q * scale - cam_trans)) (tfrag3.vert), as a matrix on (q, 1).
  // The translation is done in double: world coordinates are large.
  // (the camera's columns converted to double once per level draw, the loops unrolled: the same
  // operations in the same order as per chunk before)
  double rd[4][3], cam_d[3];
  float rn[4][3];
  for (int c = 0; c < 4; c++) {
    for (int i = 0; i < 3; i++) {
      rd[c][i] = (double)R[i][c];
      rn[c][i] = -R[i][c];
    }
  }
  const double r3d[3] = {(double)R[3][0], (double)R[3][1], (double)R[3][2]};
  for (int i = 0; i < 3; i++) {
    cam_d[i] = (double)cam.trans[i];
  }
  auto make_clip = [&](const c3l::Chunk& ch, float* m) {
    const double d0 = (double)ch.origin[0] - cam_d[0], d1 = (double)ch.origin[1] - cam_d[1],
                 d2 = (double)ch.origin[2] - cam_d[2];
    const float s = ch.scale;
#pragma GCC unroll 4
    for (int c = 0; c < 4; c++) {
      double t = 0;
      t += rd[c][0] * d0;
      t += rd[c][1] * d1;
      t += rd[c][2] * d2;
      if (c < 3) {
        t += r3d[c];
      }
      float m0 = rn[c][0] * s, m1 = rn[c][1] * s, m2 = rn[c][2] * s, m3 = (float)-t;
      if (c == 1) {
        m0 *= kYScale;
        m1 *= kYScale;
        m2 *= kYScale;
        m3 *= kYScale;
      }
      m[4 * c + 0] = m0;
      m[4 * c + 1] = m1;
      m[4 * c + 2] = m2;
      m[4 * c + 3] = m3;
    }
  };
  // (AI-assisted) a chunk that is to be drawn (mode and distance decided): the view test (the
  // chunk's sphere against the four planes; with run_culling, a chunk that crosses planes has its
  // runs tested against those planes on their own), the visibility bits or (vista tier) the
  // source chunks, then its matrix. vista: drawn in the vista pass.
  // the run loop's inputs that are the same for every chunk (filled before the chunk loop)
  RunLoopIn run_in;
  float run_po[4], run_pk[4][3];
  run_in.po = run_po;
  run_in.pk = run_pk;
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
      bool partly = false;
      bool tmorph = false;  // the tfrag geomorph: near or mid versions shown
      bool runs_valid = false;  // (AI-assisted) m_run_shown holds this chunk's runs
      // (AI-assisted) ties always; tfrag with the level's own string (not its low resolution
      // version), or every tier with a neighbour's (neighbour_vis)
      const bool vis_test = !vista_tier && m_vis && ch.vis_run_count &&
                            (ch.lod_tier == 0 || (vis_own && ch.lod_tier < 3) ||
                             (vis_nb && ch.lod_tier <= 3));
      // (AI-assisted) c3l v17: a chunk with tie levels of detail shows one version of each group
      // the tfrag geomorph's groups: one version each (c3l::ExtTfrag, their states in tf_state)
      const bool tf_chunk = tf_on && lev.chunk_lod[ci] == 2;
      const bool lod_chunk = tf_chunk || (tie_lod && lev.chunk_lod[ci] == 1);
      // (AI-assisted, j2vis) c3l v20: runs of prototypes the game hides
      const bool proto_chunk = proto_hide && lev.chunk_proto_hidden[ci];
      if (vis_test || straddles || vista_tier || lod_chunk || proto_chunk) {
        // (AI-assisted) per run: the visibility bit, then the view test; m_run_shown for the draws
        u32 shown = 0, view_hidden = 0, view_tris = 0, lod_hid = 0;
        const u32 r0 = lev.chunk_first_run[ci];
        const u32 nr = vis_test || vista_tier
                           ? ch.vis_run_count
                           : (lev.chunk_run_count[ci] ? lev.chunk_run_count[ci] : ch.vis_run_count);
        // the crossed planes: distance of the chunk's origin, and its change per quantized unit
        float* const po = run_po;
        float (*const pk)[3] = run_pk;
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
        RunLoopIn& in = run_in;
        in.vista_tier = vista_tier;
        in.lod_state = tf_chunk ? lev.tf_state.data() : lev.lod_state.data();
        in.np = np;
        in.scale = ch.scale;
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
        lod_hid = o.lod_hid;
        u64* const lt = tf_chunk ? m_tf_tris : m_lod_tris;
        lt[0] += o.lod_tris[0];
        lt[1] += o.lod_tris[1];
        m_cull_tris += view_tris;
        runs_valid = true;
        if (!shown) {
          if (lod_hid && !view_hidden) {
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
        tmorph = tf_chunk && o.lod_tris[0] && lev.tf_morph(ci);
      }
      // Shrubs fade out as on the PS2 (prototype-bucket-shrub dists: full detail up to x, then
      // alpha (w - d) / (w - x) up to w), run by run (a visibility node: up to 8 instances): w is
      // the chunk's limit, x kShrubNearK of it; d is the nearest point of the run's sphere.
      bool faded = false;
      if (ch.kind == c3l::CHUNK_KIND_SHRUB && !vista_tier && ci < m_shrub_lim.size() &&
          m_shrub_lim[ci] > 0 && !lev.run_spheres.empty()) {
        const float lim = m_shrub_lim[ci];
        // per run bands (the PS2's x and w of its prototype), else the chunk's class band
        const bool banded = !lev.run_band.empty() && lev.chunk_band_x[ci] > 0;
        const float band_k = shrub_k * det * (1.f / 16.f) * 4096.f;
        const float near_lim = banded ? std::min(lev.chunk_band_x[ci] * shrub_k * det, lim)
                                      : lim * kShrubNearK;
        const float dcx = ch.bsphere[0] - cam.trans[0], dcy = ch.bsphere[1] - cam.trans[1],
                    dcz = ch.bsphere[2] - cam.trans[2];
        const float dc = std::sqrt(dcx * dcx + dcy * dcy + dcz * dcz);
        const u32 r0 = lev.chunk_first_run[ci];
        // (the runs the run loop above uses)
        const u32 nr = vis_test ? ch.vis_run_count
                                : (lev.chunk_run_count[ci] ? lev.chunk_run_count[ci]
                                                           : ch.vis_run_count);
        u8* const rin = lev.run_dist_in.data();
        if (dc + ch.bsphere[3] <= near_lim) {
          // all of it within full detail: every run in, none faded (no per run test)
          for (u32 r = r0; r < r0 + nr; r++) {
            rin[r] = 1;
          }
        } else {
          if (!runs_valid) {
            for (u32 r = r0; r < r0 + nr; r++) {
              m_run_shown[r] = 1;
              m_run_nomerge[r] = 0;
            }
            runs_valid = true;
          }
          const float chunk_band = 128.f / std::max(lim - near_lim, 1.f);
          u32 shown = 0;
          for (u32 r = r0; r < r0 + nr; r++) {
            const auto& rs_ = lev.run_spheres[r];
            u8 fade = kRunOpaque;
            if (rs_.r != 0xffff) {
              const float x = ch.origin[0] + rs_.c[0] * ch.scale - cam.trans[0];
              const float y = ch.origin[1] + rs_.c[1] * ch.scale - cam.trans[1];
              const float z = ch.origin[2] + rs_.c[2] * ch.scale - cam.trans[2];
              const float d = std::sqrt(x * x + y * y + z * z) - (float)rs_.r * ch.scale;
              const u32 b = banded && r < lev.run_band.size() ? lev.run_band[r] : 0;
              float rl = lim, rn = near_lim, rb = chunk_band;
              if (b) {
                rl = std::min((float)(b & 0xffff) * band_k, lim);
                rn = std::min((float)(b >> 16) * band_k, rl);
                rb = 128.f / std::max(rl - rn, 1.f);
              }
              if (d > rn) {
                // 16 steps (of 8 in 0x80): neighbouring runs share a draw
                const int q = (int)std::lround((rl - d) * rb * (1.f / 8.f)) * 8;
                fade = (u8)std::clamp(q, 0, (int)kRunOpaque);
              }
            }
            rin[r] = fade != 0;
            m_run_fade[r] = fade;
            if (!fade) {
              // never drawn along with its neighbours either (the small-gap merge)
              m_run_nomerge[r] = 1;
              if (m_run_shown[r]) {
                m_run_shown[r] = 0;
                m_shrub_run_hid++;
              }
            } else if (fade < kRunOpaque && m_run_shown[r]) {
              faded = true;
            }
            shown += m_run_shown[r];
          }
          if (!shown) {
            return;
          }
          partly = partly || shown < nr || faded;
        }
      }
      m_visible_slot[ci] = (int)m_visible.size();
      VisibleChunk& vc = m_visible.emplace_back();
      vc.chunk = (u32)ci;
      vc.partly = partly;
      vc.runs_valid = runs_valid;
      vc.faded = faded;
      vc.vista = vista_chunk;
      vc.tmorph = tmorph;
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
  run_in.runs = lev.vis_runs.data();
  run_in.spheres = lev.run_spheres.data();
  run_in.run_lod = lev.run_lod.data();
  run_in.lod_state = lev.lod_state.data();
  run_in.vis = m_vis;
  run_in.vista_src = m_vista_src.data();
  run_in.vista_src_n = m_vista_src.size();
  run_in.shown_out = m_run_shown.data();
  run_in.nomerge_out = m_run_nomerge.data();
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
        add(cl.raw[1] * kShrubFarK * shrub_k * det);  // (as decide_dist: fable1's far plane)
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
    if ((f & CtrLevelData::kClTier12) && !far_tfrag) {
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
      if (!far_tfrag && (chunks[ci].lod_tier == 1 || chunks[ci].lod_tier == 2) && (st & 3) != 2) {
        return false;
      }
    }
    return true;
  };
  const u32 late0 = m_late_in_view;  // (the chunks' share: Jak 1's report below)
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
    int tier_q = -1;  // (AI-assisted, autodetail) the position a tier decision held in view keeps
    if (!far_tfrag && (ch.lod_tier == 1 || ch.lod_tier == 2)) {
      // detailed version up close, coarse version further away
      const u8 last = state & 3;
      const float f = last == 1 ? 1.1f : (last == 2 ? 0.9f : 1.f);
      const float dx = ch.lod_center[0] - cx, dy = ch.lod_center[1] - cy,
                  dz = ch.lod_center[2] - cz;
      const float d2 = dx * dx + dy * dy + dz * dz;
      int in_view = -1;
      auto near_with = [&](float ld) {
        const float lim = ld * f;
        bool nr = ld <= 0 || d2 < lim * lim;
        // (AI-assisted, title-popin) a cell in view moving away keeps its detailed version up to
        // kHoldInView x lod_distance (both versions test the same cell sphere: one is drawn)
        if (!nr && last == 1) {
          if (in_view < 0) {
            in_view = sphere_in_view(cam, &lev.lod_cell_sphere[ci * 4]);
          }
          const float hl = ld * kHoldInView;
          nr = in_view && d2 < hl * hl;
        }
        return nr;
      };
      bool near = near_with(lod_dist);
      // (AI-assisted, autodetail) no tier switch in view because the relief moved lod_distance
      // (except a GPU about to miss frames, for the coarse version)
      const u8 cq = chunk_q[ci];
      if (auto_detail && last && cq != q_now && cq != 0xff) {
        const float ld_old = relief.at(detail_at(cq)).lod_distance * 4096.f;
        if (ld_old != lod_dist) {
          const bool near_old = near_with(ld_old);
          if (near_old != near && !(g_detail_urgent && !near)) {
            if (in_view < 0) {
              in_view = sphere_in_view(cam, &lev.lod_cell_sphere[ci * 4]);
            }
            if (in_view) {
              near = near_old;
              tier_q = cq;
            }
          }
        }
      }
      state = (u8)((state & ~3) | (near ? 1 : 2));
      lod_ok = near == (ch.lod_tier == 1);
    }
    // (AI-assisted) lowres_vista: the low resolution tfrag is far scenery (vista_distance)
    const bool vista_chunk = lowres_vista && ch.lod_tier == 3;
    const bool dist_ok = decide_dist(ci, ch, state, vista_chunk);
    if (tier_q >= 0) {
      chunk_q[ci] = (u8)tier_q;
    }
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
    // (a level with a vista tier draws its low resolution tfrag through the copies: no mesh)
    if ((meshes[ci] < 0 && !(lev.has_vista && ch.lod_tier == 3)) || !dist_ok) {
      if (!dist_ok && ch.lod_tier != 3) {
        vista_source(ci);
      } else if (dorm && ch.lod_tier != 0 && lev.chunk_dormant[ci] >= CtrLevelData::kTitleFreed &&
                 sphere_in_view(cam, ch.bsphere)) {
        m_late_in_view++;  // left out by the title, not back yet
      }
      continue;
    }
    // (AI-assisted, streammem) the tfrag tiers by far_tfrag, the rest by far_level
    if (ch.lod_tier != 0 ? far_tfrag : far_level) {
      // only the level's low resolution version (or the coarse one), no tie but (AI-assisted) the
      // landmarks (far_chunk)
      if (!far_chunk(lev, ch)) {
        continue;
      }
      // (AI-assisted) landmarks only within the LOD distance: a neighbouring level has no
      // visibility data here, and its landmark ties are 57k-152k triangles (village2, lavatube).
      // (AI-assisted, levellod) but out to draw_distance, as near mode draws them, while far mode
      // is forced with the camera where near mode would be (its detail meshes not read back yet:
      // a respawn, a cut) in a level without a vista tier (citadel: 375 of its 411 landmark
      // chunks, nothing stands in for them beyond 200 m). Not when seen from a neighbour (Lava
      // Tube's end from Citadel): the far-to-near pop stays rather than up to 152k more triangles
      if (ch.lod_tier == 0 && lod_dist_cfg > 0) {
        const float dx = ch.bsphere[0] - cx, dy = ch.bsphere[1] - cy, dz = ch.bsphere[2] - cz;
        const float reach =
            !lev.has_vista && far_inside && draw_dist > lod_dist_cfg ? draw_dist : lod_dist_cfg;
        const float lim = reach + ch.bsphere[3];
        if (dx * dx + dy * dy + dz * dz > lim * lim) {
          vista_source(ci);
          continue;
        }
      }
    } else if (ch.lod_tier == 3) {
      // (AI-assisted) the low resolution tfrag in near mode: where a neighbour's string says
      // (neighbour_vis, chunks with visibility runs), else as vista outside the detailed areas.
      // (AI-assisted, levellod) a level with a vista tier draws it through its vista copies,
      // which have no visibility test and lie over the detailed tfrag where the bits don't say
      // otherwise (finalboss: 576 of its 1156 low resolution chunks): there the bits only narrow
      // the detail rule below (lowres_vista; a chunk none of whose runs the string shows: not
      // drawn)
      const bool by_bits = vis_nb && ch.vis_run_count;
      bool want = by_bits && !(lev.has_vista && lowres_vista);
      if (by_bits && lev.has_vista) {
        bool any = false;
        const u32 r0 = lev.chunk_first_run[ci];
        for (u32 r = r0; r < r0 + ch.vis_run_count && !any; r++) {
          const u16 id = lev.vis_runs[r].vis;
          any = id == 0xffff || (m_vis[id >> 3] & (0x80 >> (id & 7)));
        }
        if (!any) {
          continue;
        }
      }
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
        // (AI-assisted) The static bounds say where detail exists in the file, not where it
        // is drawn this frame. Beyond the distance limit, allow the lowres fallback even
        // inside those bounds (finalboss spans kilometers). Include hysteresis and twice
        // the largest detail sphere: its center may be one radius past the cutoff and its
        // geometry another radius past its center. This avoids overlapping nearby detail.
        if (!want && draw_dist > 0) {
          float radius = 0;
          for (const CtrLevelData* l : m_loaded) {
            radius = std::max(radius, l->detail_radius);
          }
          const float dx = ch.bsphere[0] - cx, dy = ch.bsphere[1] - cy, dz = ch.bsphere[2] - cz;
          const float lim = draw_dist * 1.1f + 2 * radius + ch.bsphere[3] + kDetailMargin;
          want = dx * dx + dy * dy + dz * dz > lim * lim;
        }
      }
      if (!want) {
        continue;
      }
      m_lowres_near++;
    } else if (!lod_ok) {
      continue;
    }
    // (AI-assisted) lowres_vista: in far mode too, not over another loaded level's detailed tfrag
    if (vista_chunk && far_tfrag) {
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
      if (!ch.vis_run_count) {
        continue;
      }
      const float dx = ch.bsphere[0] - cam.trans[0], dy = ch.bsphere[1] - cam.trans[1],
                  dz = ch.bsphere[2] - cam.trans[2];
      const float lim = vista_dist + ch.bsphere[3];
      if (dx * dx + dy * dy + dz * dz < lim * lim) {
        if (lev.meshes[ci] >= 0) {
          emit_chunk(ci, true);
        } else if (dorm && lev.chunk_dormant[ci] >= CtrLevelData::kTitleFreed &&
                   sphere_in_view(cam, ch.bsphere)) {
          // left out by the title, not back yet: late if a run of it would show
          const u32 r0 = lev.chunk_first_run[ci];
          for (u32 r = r0; r < r0 + ch.vis_run_count; r++) {
            const u16 src = lev.vis_runs[r].vis;
            if (src < m_vista_src.size() && m_vista_src[src]) {
              m_late_in_view++;
              break;
            }
          }
        }
      }
    }
  }
  if (m_jak2_slot < 0 && m_late_in_view > late0) {
    // chunks the title left out, in view and not back yet (its tie LOD groups: reported above)
    m_levels->note_late_in_view(rs.frame_idx, m_late_in_view - late0,
                                rs.frame_idx < lev.reveal_frame + 90);
  }
  phase(kPhChunks);
#ifndef __3DS__
  // host diagnostic, env CTR_TITLE_VIS_LOG=FILE: per title frame the camera and the chunks drawn
  if (const char* tvl = getenv("CTR_TITLE_VIS_LOG"); tvl && title_on && lev.name != "title") {
    static FILE* tvf = fopen(tvl, "w");
    static bool tv_hdr = false;
    if (tvf && !tv_hdr) {
      tv_hdr = true;
      for (size_t ci = 0; ci < lev.chunks.size(); ci++) {
        const auto& ch = lev.chunks[ci];
        fprintf(tvf, "C %zu %u %u %.1f %.1f %.1f %.1f %.1f %u %u %d\n", ci, ch.kind, ch.lod_tier,
                ch.bsphere[0], ch.bsphere[1], ch.bsphere[2], ch.bsphere[3], ch.max_dist,
                ch.vertex_count,
                ci < lev.mesh_index_end.size() && lev.mesh_index_first[ci] != UINT32_MAX
                    ? lev.mesh_index_end[ci] - lev.mesh_index_first[ci]
                    : 0u,
                lev.is_unit(ci) ? 1 : 0);
      }
    }
    if (tvf) {
      fprintf(tvf, "F %llu %.1f %.1f %.1f", (unsigned long long)rs.frame_idx, cam.trans[0],
              cam.trans[1], cam.trans[2]);
      for (int i = 0; i < 4; i++) {
        fprintf(tvf, " %.6f %.6f %.6f %.1f", cam.planes[0][i], cam.planes[1][i], cam.planes[2][i],
                cam.planes[3][i]);
      }
      fprintf(tvf, " late %u |", m_late_in_view - late0);
      for (const auto& vc : m_visible) {
        fprintf(tvf, " %u", vc.chunk);
      }
      fprintf(tvf, "\n");
      fflush(tvf);
    }
  }
  // (AI-assisted, perfv2) host diagnostic, env CTR_SOFT_DRAW_PX: what each mesh drawn is (the
  // soft renderer's per-draw fragment list names them)
  if (getenv("CTR_SOFT_DRAW_PX")) {
    for (const auto& vc : m_visible) {
      if (vc.chunk < lev.meshes.size() && lev.meshes[vc.chunk] >= 0) {
        const auto& ch = lev.chunks[vc.chunk];
        ctr_soft_mesh_desc()[lev.meshes[vc.chunk]] =
            fmt::format("{} chunk {} kind {} tier {} sphere ({:.0f} {:.0f} {:.0f} r {:.0f}) verts {}",
                        lev.name, vc.chunk, ch.kind, ch.lod_tier, ch.bsphere[0], ch.bsphere[1],
                        ch.bsphere[2], ch.bsphere[3], ch.vertex_count);
      }
    }
  }
#endif
  // (AI-assisted) c3l v16 time of day: the chunks to be drawn brought up to this frame's blend
  // (the groups of the runs drawn: m_run_shown when runs_valid), before any of their draws
  u32 pop_tod = 0;  // (AI-assisted, title3: CTR_POP_LOG) groups rewritten
  if (lev.tod) {
    const double t0 = tod_now_ms();
    TodPass ps;
    ps.frame = rs.frame_idx;
    auto shown_of = [&](const VisibleChunk& vc) {
      return vc.runs_valid ? &m_run_shown[lev.chunk_first_run[vc.chunk]] : nullptr;
    };
    // (AI-assisted) Full groups first: their light vertices need no second rewrite.
    ps.pass = kTodRound;
    {
      auto& t = *lev.tod;
      u64 drawn = 0;
      for (const auto& vc : m_visible) {
        drawn += vc.chunk < t.chunk_mask.size() && t.chunk_scanned[vc.chunk] == 2
                     ? lev.chunks[vc.chunk].vertex_count
                     : 0;
      }
      ps.fresh_ok = drawn >= kTodFreshRatio * t.table.size();
      ps.spread = t.day_move_frame && rs.frame_idx - t.day_move_frame < kTodSpreadFrames;
      ps.spread_left = std::max<s64>(kTodSpreadMin, (s64)(drawn / kTodSpreadFrames));
      for (const auto& vc : m_visible) {
        tod_verts += tod_recolor(lev, vc.chunk, shown_of(vc), ps);
      }
    }
    ps.pass = kTodLights;
    for (const auto& vc : m_visible) {
      tod_verts += tod_recolor(lev, vc.chunk, shown_of(vc), ps);
    }
    for (const auto& vc : m_visible) {
      if (vc.tmorph) {
        m_tf_colors_written += tf_morph_colors(lev, vc.chunk, shown_of(vc));
      }
    }
    tod_finish();
    tod_ms += tod_now_ms() - t0;
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
    pop_tod = ps.rewritten;
    m_tod_spread += ps.rewritten;
    m_tod_fresh += ps.fresh ? 1 : 0;
    m_tod_heavy = ps.fresh_ok;  // (AI-assisted, fable1) the next frame's tod_update, see there
    m_tod_light_verts += ps.light_verts;
    m_tod_deferred += ps.deferred;
    m_tod_unchanged += ps.unchanged;
  }

  phase(kPhTod);
  // The draws whose order doesn't matter (opaque, depth written) sorted by texture and state,
  // across chunks: every texture change clears the GPU's texture cache, and the chunks share most
  // of their textures. Then the others (blending, decals) in their order, over the opaque ones.
  // The order is made at load (sorted_draws, ordered_draws): only the visible chunks' draws.
  if (!lev.cond_draws.empty()) {
    // kDrawCullCond: back faces culled while the camera is inside every opening
    // plane of the draw (they can only be seen through an opening, from outside its plane)
    const float px = cam.trans[0], py = cam.trans[1], pz = cam.trans[2];
    const c3l::CullCondPlane* const planes = lev.cond_planes.data();
    for (const auto& cd : lev.cond_draws) {
      if (m_visible_slot[lev.draw_chunk[cd.draw]] < 0) {
        continue;
      }
      bool in = true;
      for (u32 k = cd.first; k < cd.first + cd.count && in; k++) {
        const auto& p = planes[k];
        in = p.n[0] * px + p.n[1] * py + p.n[2] * pz < p.d;
      }
      auto& st = lev.draw_states[cd.draw];
      st.tcc = (u8)(in ? (st.tcc | CTR_TCC_CULL_BACK) : (st.tcc & ~CTR_TCC_CULL_BACK));
      m_cond_draws++;
      m_cond_culled += in;
    }
  }
  bool vista_pass = false;  // (AI-assisted) the pass drawing: the vista chunks or the others
  u64 vista_indices = 0;    // (AI-assisted) drawn by the vista pass (statistics)
  // (AI-assisted, j2water2) c3l v21: an envmapped tie draw (its shine in the same draw, drawn
  // after the pass: ctr_gpu_etie_draw has its own program and state)
  m_env_calls.clear();
  auto env_submit = [&](u32 di, const VisibleChunk& vc, int env, u32 first, u32 count) {
    const auto& e = lev.env_draws[env];
    const auto& ch = lev.chunks[vc.chunk];
    EnvCall c;
    c.state = &lev.draw_states[di];
    c.env_tex = e.texture < lev.textures.size() ? lev.textures[e.texture] : -1;
    c.mesh = lev.meshes[vc.chunk];
    c.normals_mesh = c.mesh;
    c.normals_offset = (int)lev.normals_offset_in_mesh(vc.chunk);
    if (vc.chunk < lev.chunk_morph.size() && lev.chunk_morph[vc.chunk] >= 0) {
      c.normals_offset += (int)(ch.vertex_count * sizeof(c3l::MorphVertex));  // (after the records)
    }
    c.first = (int)first;
    c.count = (int)count;
    c.d.clip = vc.gpu;
    // etie.vert's cam_no_persp (the camera's rot, columns), the chunk's quantizing folded in
    for (int r = 0; r < 3; r++) {
      for (int k = 0; k < 3; k++) {
        c.d.cam[r][k] = cam.rot[k][r] * ch.scale;
        c.d.nrm[r][k] = cam.rot[k][r] * (1.f / 127.f);
      }
      c.d.cam[r][3] = cam.rot[0][r] * ch.origin[0] + cam.rot[1][r] * ch.origin[1] +
                      cam.rot[2][r] * ch.origin[2] + cam.rot[3][r];
      c.d.nrm[r][3] = 0.f;
    }
    c.d.st_scale = (c.state->tcc & CTR_TCC_ST_ATLAS) ? 1.f / 32768.f : 1.f / 1024.f;
    // the shine: proto tint (0xff = 1) * env color (0x80 = 1), as Tie3 (Jak II: env / 128),
    // halved for the combiner's x2 (not decal)
    u32 shine = 0xff000000u;
    for (int k = 0; k < 3; k++) {
      float v = e.tint[k] * m_env_color[k] / (m_jak2_slot < 0 ? 64.f : 128.f);
      if (!settings.envmap) {
        v = 0;
      }
      if (!c.state->decal) {
        v *= 0.5f;
      }
      shine |= (u32)std::clamp((int)std::lround(v), 0, 255) << (8 * k);
    }
    c.d.shine = shine;
    (m_late_collect ? m_env_late : m_env_calls).push_back(c);
  };
  u32 fade_di = UINT32_MAX;  // the last faded call's draw and fade (fade_submit)
  u8 fade_fd = 0;
  // the tie geomorph (c3l::ExtMorph): each visible chunk's rows, its instances' morph factors as
  // the PS2's (d the distance to the instance's sphere: the PS2 takes its view depth, which turning
  // the camera changes; band 1 from the near-plane, band 2 from the mid-plane, f = clamp((d -
  // start) * rlength, 0, 1)), the distances x morph_k
  const bool morph_draws = !lev.chunk_morph.empty() && !lev.group_mid.empty() &&
                           lev.draw_morph_rows.size() == lev.draws.size();
  // (each chunk's on its first morph draw: most visible chunks have none drawn)
  const float morph_kd = lev.morph_k;
  if (morph_draws) {
    size_t n = 0;
    m_morph_row_of.assign(m_visible.size(), -1);
    m_morph_row_id.assign(m_visible.size(), 0);
    for (size_t slot = 0; slot < m_visible.size(); slot++) {
      const s32 mi = lev.chunk_morph[m_visible[slot].chunk];
      if (mi >= 0) {
        m_morph_row_of[slot] = (s32)n;
        n += 4 * (lev.morph_chunks[mi].row_count + 1);
      }
    }
    m_morph_rows.resize(n);
  }
  auto morph_rows_of = [&](int slot) -> const float* {
    float* row = &m_morph_rows[m_morph_row_of[slot]];
    if (m_morph_row_id[slot]) {
      return row;
    }
    m_morph_row_id[slot] = ++m_morph_rows_id ? m_morph_rows_id : ++m_morph_rows_id;
    const auto& mc = lev.morph_chunks[lev.chunk_morph[m_visible[slot].chunk]];
    const float kd = morph_kd;
    const float px = cam.trans[0], py = cam.trans[1], pz = cam.trans[2];
    row[0] = row[1] = row[2] = row[3] = 0.f;
    const u32* keys = &lev.morph_rows[mc.row_first];
    for (u32 r = 0; r < mc.row_count; r++) {
      const auto& in = lev.morph_insts[keys[r]];
      const float ex = in.c[0] - px, ey = in.c[1] - py, ez = in.c[2] - pz;
      const float d = std::sqrt(ex * ex + ey * ey + ez * ez) - in.r;
      float* rr = row + 4 * (r + 1);
      for (int b = 0; b < 2; b++) {
        const float f = kd > 0 ? (d - in.start[b] * kd) * in.rlen[b] / kd : 0.f;
        rr[b] = std::clamp(f, 0.f, 1.f) * mc.delta_scale;
      }
      rr[2] = rr[3] = 0.f;
#ifndef __3DS__
      static const int morph_ab = getenv("CTR_MORPH_AB") ? atoi(getenv("CTR_MORPH_AB")) : 0;
      if (morph_ab == 2 && (rs.frame_idx & 1)) {
        rr[0] = rr[1] = 0.f;
      }
#endif
    }
    return row;
  };
  // the tfrag geomorph (c3l::ExtTfrag): a visible chunk's morph uniforms (the bands, its scale)
  if (m_tf_unif.size() < m_visible.size() * 8) {
    m_tf_unif.resize(m_visible.size() * 8);
  }
  m_tf_unif_id.assign(m_visible.size(), 0);
  auto tf_unif_of = [&](int slot) -> const float* {
    float* u = &m_tf_unif[(size_t)slot * 8];
    if (!m_tf_unif_id[slot]) {
      m_tf_unif_id[slot] = ++m_tf_unif_ids ? m_tf_unif_ids : ++m_tf_unif_ids;
      memcpy(u, m_tf_band, sizeof(m_tf_band));
      u[4] = lev.tf_chunks[lev.chunk_tf[m_visible[slot].chunk]].delta_scale;
      u[5] = u[6] = u[7] = 0.f;
    }
    return u;
  };
  // wind ties: their draws through ctr_gpu_draw_mesh_wind with the chunk's slot rows
  const bool wind_on = lev.wind_mesh >= 0 && lev.chunk_wind.size() == lev.chunks.size() &&
                       lev.draw_wind.size() == lev.draws.size() &&
                       lev.wind_k.size() == 2 * lev.wind_insts.size();
  bool wind_inline = false;  // the ordered pass: drawn in its place
  m_wind_calls.clear();
  if (wind_on) {
    m_wind_rows.clear();
    m_wind_row_of.assign(m_visible.size(), -1);
    m_wind_row_id.assign(m_visible.size(), 0);
  }
  auto wind_submit = [&](u32 di, int slot, const VisibleChunk& vc, u32 first, u32 count) {
    const auto& ch = lev.chunks[vc.chunk];
    const u32 n = lev.chunk_wind_n[vc.chunk];
    if (m_wind_row_of[slot] < 0) {
      m_wind_row_of[slot] = (s32)(m_wind_rows.size() / 4);
      m_wind_row_id[slot] = ++g_wind_rows_id ? g_wind_rows_id : ++g_wind_rows_id;
      m_wind_rows.insert(m_wind_rows.end(), 4, 0.f);
      // x += kx * (y - y0) in world space is x += kx * y + kx * (origin y - y0) / scale here
      const u16* inst = &lev.wind_slot_inst[lev.chunk_wind_first[vc.chunk]];
      for (u32 k = 0; k < n; k++) {
        const float kx = lev.wind_k[2 * inst[k]], kz = lev.wind_k[2 * inst[k] + 1];
        const float c = (ch.origin[1] - lev.wind_insts[inst[k]].y0) / ch.scale;
        const float row[4] = {kx, kz, kx * c, kz * c};
        m_wind_rows.insert(m_wind_rows.end(), row, row + 4);
      }
    }
    m_wind_calls.push_back({&lev.draw_states[di], &vc.gpu, (u32)m_wind_row_of[slot], n + 1,
                            m_wind_row_id[slot], lev.meshes[vc.chunk], lev.chunk_wind[vc.chunk],
                            (int)first, (int)count, vc.nofog ? 1 : 0});
    if (wind_inline) {
#ifdef __3DS__
      flush_calls(0);
#endif
      draw_wind_calls(lev);
    }
  };
  auto draw = [&](u32 di) {
    const int slot = m_visible_slot[lev.draw_chunk[di]];
    if (slot < 0 || m_visible[slot].vista != vista_pass) {
      return;
    }
    const VisibleChunk& vc = m_visible[slot];
    const auto& dr = lev.draws[di];
    const int env = lev.draw_env.empty() ? -1 : lev.draw_env[di];  // (AI-assisted, j2water2)
    const bool wind = wind_on && !vista_pass && env < 0 && lev.draw_wind[di];
    // the tie geomorph: a draw whose vertices move
    const u32 mlist = morph_draws && !vista_pass && !wind && env < 0 ? lev.draw_morph_rows[di]
                                                                     : UINT32_MAX;
    const bool morph = mlist != UINT32_MAX && lev.morph_draw_rows[mlist] &&
                       m_morph_row_of[slot] >= 0;
    const float* const mrows = morph ? morph_rows_of(slot) : nullptr;
    const int moff = morph ? (int)lev.normals_offset_in_mesh(vc.chunk) : 0;
    // the tfrag geomorph: a chunk with near or mid versions shown
    const bool tmorph = vc.tmorph && !vista_pass && !wind && env < 0;
    const float* const tunif = tmorph ? tf_unif_of(slot) : nullptr;
    const int toff = tmorph ? (int)lev.normals_offset_in_mesh(vc.chunk) : 0;
#ifdef __3DS__
    // (AI-assisted, gpu-rt) the pass's draws go to the backend in one list
    auto submit = [&](u32 first, u32 count) {
      if (wind) {
        wind_submit(di, slot, vc, first, count);
        return;
      }
      if (env >= 0) {
        env_submit(di, vc, env, first, count);
        return;
      }
      if (morph) {
        m_calls.push_back({&lev.draw_states[di], &vc.gpu, lev.meshes[vc.chunk], (int)first,
                           (int)count, vc.nofog ? 1 : 0, mrows, &lev.morph_draw_rows[mlist + 1],
                           lev.morph_draw_rows[mlist], moff, m_morph_row_id[slot], nullptr, 0});
        return;
      }
      if (tmorph) {
        m_calls.push_back({&lev.draw_states[di], &vc.gpu, lev.meshes[vc.chunk], (int)first,
                           (int)count, vc.nofog ? 1 : 0, nullptr, nullptr, 0, 0,
                           m_tf_unif_id[slot], tunif, toff});
        return;
      }
      m_calls.push_back({&lev.draw_states[di], &vc.gpu, lev.meshes[vc.chunk], (int)first,
                         (int)count, vc.nofog ? 1 : 0, nullptr, nullptr, 0, 0, 0, nullptr, 0});
    };
#else
    ctr_gpu_set_mesh_nofog(vc.nofog);  // (AI-assisted) fog_stage_skip
    auto submit = [&](u32 first, u32 count) {
      if (wind) {
        wind_submit(di, slot, vc, first, count);
        return;
      }
      if (env >= 0) {
        env_submit(di, vc, env, first, count);  // (AI-assisted, j2water2)
        return;
      }
      if (m_late_collect) {
        // (AI-assisted, j2water) Jak II's blended draws, drawn later (draw_jak2_late)
        m_calls.push_back({&lev.draw_states[di], &vc.gpu, lev.meshes[vc.chunk], (int)first,
                           (int)count, vc.nofog ? 1 : 0, nullptr, nullptr, 0, 0, 0, nullptr, 0});
        return;
      }
      if (morph) {
        ctr_gpu_draw_mesh_morph(&lev.draw_states[di], &vc.gpu, mrows,
                                &lev.morph_draw_rows[mlist + 1], lev.morph_draw_rows[mlist],
                                m_morph_row_id[slot], lev.meshes[vc.chunk], moff, (int)first,
                                (int)count);
        return;
      }
      if (tmorph) {
        ctr_gpu_draw_mesh_tmorph(&lev.draw_states[di], &vc.gpu, tunif, m_tf_unif_id[slot],
                                 lev.meshes[vc.chunk], toff, (int)first, (int)count);
        return;
      }
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
    // a faded shrub run: its own call with the draw's state blended by the run's fade (FIX: the
    // GS's constant alpha; alpha blended draws keep theirs), next runs of the same fade joined
    auto fade_submit = [&](u32 first, u32 count, u8 fade) {
      const ctr_draw_state& st = lev.draw_states[di];
      if (!m_fade_calls.empty() && fade_di == di && fade_fd == fade) {
        ctr_mesh_call& last = m_fade_calls.back();
        if (last.matrix == &vc.gpu && last.first_index + last.index_count == (int)first) {
          last.index_count += (int)count;
          return;
        }
      }
      fade_di = di;
      fade_fd = fade;
      const ctr_draw_state* use = &st;
      if (st.blend == CTR_BLEND_OFF) {
        ctr_draw_state& f = m_fade_states.emplace_back(st);
        f.blend = CTR_BLEND_FIX;
        f.fix = fade;
        f.flags &= (u8)~CTR_STATE_EARLY_DEPTH;
        use = &f;
      }
      m_fade_calls.push_back(
          {use, &vc.gpu, lev.meshes[vc.chunk], (int)first, (int)count, vc.nofog ? 1 : 0, nullptr,
           nullptr, 0, 0, 0, nullptr, 0});
      m_fade_runs++;
    };
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
    const u8* const run_fade = vc.faded ? m_run_fade.data() : nullptr;
    for (u32 r = r0; r < r_end; r++) {
      const auto& run = runs[r];
      const bool shown = run_shown[r];  // (AI-assisted) visibility bit and view test
      if (shown && run_fade && run_fade[r] < kRunOpaque) {
        if (open) {
          emit();
        }
        fade_submit(idx, run.index_count, run_fade[r]);
        drawn += run.index_count;
      } else if (shown) {
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
    flush_calls(1);  // (AI-assisted, gpu-rt) the vista pass's
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
  // the tie geomorph's draws after the others of their list: each switch between the morph program
  // and the mesh program sends citro3d's whole context again (163 per frame interleaved, ~20 so)
  const u32* const morph_list = morph_draws ? lev.draw_morph_rows.data() : nullptr;
  auto is_morph = [&](u32 di) {
    return morph_list[di] != UINT32_MAX && lev.morph_draw_rows[morph_list[di]] &&
           !(wind_on && lev.draw_wind[di]) && (lev.draw_env.empty() || lev.draw_env[di] < 0);
  };
  // the tfrag geomorph's draws (a chunk with near or mid versions shown) too, before the tie ones
  bool any_tmorph = false;
  for (const auto& vc : m_visible) {
    any_tmorph = any_tmorph || vc.tmorph;
  }
  auto is_tmorph = [&](u32 di, int slot) {
    return vchunks[slot].tmorph && !(wind_on && lev.draw_wind[di]) &&
           (lev.draw_env.empty() || lev.draw_env[di] < 0);
  };
  // 0: in its place, 1: the tfrag geomorph's, 2: the tie geomorph's
  auto morph_pass = [&](u32 di, int slot) {
    return morph_list && is_morph(di) ? 2 : any_tmorph && is_tmorph(di, slot) ? 1 : 0;
  };
  auto walk_split = [&](const std::vector<u32>& list) {
    if (!morph_list && !any_tmorph) {
      walk(list);
      return;
    }
    m_ph_walked += list.size();
    m_morph_later.clear();
    m_tmorph_later.clear();
    for (u32 di : list) {
      const int slot = vslot[draw_chunk[di]];
      if (slot >= 0 && !vchunks[slot].vista) {
        const int mp = morph_pass(di, slot);
        if (mp == 2) {
          m_morph_later.push_back(di);
        } else if (mp == 1) {
          m_tmorph_later.push_back(di);
        } else {
          draw(di);
        }
      }
    }
    for (u32 di : m_tmorph_later) {
      draw(di);
    }
    for (u32 di : m_morph_later) {
      draw(di);
    }
  };
  if (early_depth) {
    // (AI-assisted) one walk: the bands nearest first, each in list order with its morph draws
    // last (the tfrag geomorph's, then the tie geomorph's: opaque and depth tested as the others)
    for (auto& b : m_band_draws) {
      b.clear();
    }
    m_ph_walked += main_lists[0]->size();
    for (u32 di : *main_lists[0]) {
      const int slot = vslot[draw_chunk[di]];
      if (slot >= 0 && !vchunks[slot].vista) {
        m_band_draws[vchunks[slot].band * 3 + morph_pass(di, slot)].push_back(di);
      }
    }
    for (const auto& b : m_band_draws) {
      for (u32 di : b) {
        draw(di);
      }
    }
  } else {
    walk_split(*main_lists[0]);
  }
  walk_split(*main_lists[1]);
  if (!m_wind_calls.empty()) {
#ifdef __3DS__
    flush_calls(0);
#endif
    draw_wind_calls(lev);
  }
  wind_inline = true;
  // the faded shrub runs: blended, so after the opaque draws (before the ordered ones)
#ifdef __3DS__
  m_calls.insert(m_calls.end(), m_fade_calls.begin(), m_fade_calls.end());
#else
  for (const auto& c : m_fade_calls) {
    ctr_gpu_set_mesh_nofog(c.nofog);
    ctr_gpu_draw_mesh_prepared(c.state, c.matrix, c.mesh, c.first_index, c.index_count);
  }
#endif
  if (m_jak2_slot >= 0) {
    // (AI-assisted, j2water) Jak II: the blended and ordered draws (water, glass, decals) wait
    // for the slot's water bucket (draw_jak2_late), after the characters (merc), as the PS2
    // draws its translucent background groups after the opaque merc ones
    flush_calls(0);
    draw_env_calls(m_env_calls);  // (AI-assisted, j2water2) the opaque envmapped ties
    m_env_late.clear();
    m_late_collect = true;
    walk(*main_lists[2]);
    m_late_collect = false;
    m_late_calls.swap(m_calls);
    m_calls.clear();
    m_late_frame = rs.frame_idx;
    memcpy(m_late_fog0, fog0, sizeof(m_late_fog0));
    memcpy(m_late_fog1, fog1, sizeof(m_late_fog1));
    memcpy(m_late_fog_rgb, rs.fog_color, sizeof(m_late_fog_rgb));
  } else {
    walk(*main_lists[2]);
  }
  phase(kPhLists);
  flush_calls(0);  // (AI-assisted, gpu-rt) the main pass's
  draw_env_calls(m_env_calls);  // (AI-assisted, j2water2) (none left in Jak II's case)
  ctr_gpu_set_mesh_nofog(0);
  ctr_gpu_cull_reset();  // (AI-assisted) cull_ties
  phase(kPhFlush);
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
  if (pop_log_on()) {
    // (AI-assisted, title3) CTR_POP_LOG: one line per level draw (in view: appeared / went by
    // distance per kind tfrag, tie, shrub; tie LOD switches near / far; groups drawn far for late
    // data, kept far once it came; time of day groups rewritten; near chunks taken since the last
    // draw)
    lg::info("[pop] f={} {} cam={:.1f},{:.1f},{:.1f} vis={} far={} t={:.2f} in={}/{}/{} out={}/{}/{} "
             "lod+={} lod-={} late={} held={} tod={} taken={}",
             rs.frame_idx, lev.name, cam.trans.x() / 4096.f, cam.trans.y() / 4096.f,
             cam.trans.z() / 4096.f, m_visible.size(), far_level ? 1 : 0, g_detail_t,
             g_pop.chunk_in[1], g_pop.chunk_in[2], g_pop.chunk_in[3], g_pop.chunk_out[1],
             g_pop.chunk_out[2], g_pop.chunk_out[3], g_pop.lod_up, g_pop.lod_down, late_groups,
             held_groups, pop_tod, g_pop.taken);
    g_pop.taken = 0;
  }
  phase(kPhSetup);
}

u32 CtrLevelData::normals_offset_in_mesh(size_t ci) const {
  const auto& ch = chunks[ci];
  return is_unit(ci) && streaming
             ? CtrStreamArena::mesh_bytes(ch.vertex_count, mesh_index_end[ci] - mesh_index_first[ci])
             : ch.vertex_count * 16;
}
