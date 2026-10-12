/*!
 * @file CtrLevel.cpp
 * (AI-assisted)
 * See CtrLevel.h. The camera math follows the PC tfrag renderer
 * (make_new_cam_mat and tfrag3.vert in game/graphics/opengl_renderer).
 */

#include "CtrLevel.h"

#include "game/graphics/ctr/CtrSettings.h"
#include "game/graphics/ctr/CtrImpostor.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>

#include "common/dma/dma.h"
#include "common/dma/gs.h"
#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/util/Timer.h"

#include "game/graphics/ctr/c3l_reader.h"
#include "game/graphics/ctr/ctr_gpu.h"
#include "game/graphics/opengl_renderer/buckets.h"

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
    if (lev.meshes[ci] < 0 && lev.mesh_index_first[ci] != UINT32_MAX &&
        (lev.detail_loaded || far_chunk(lev, ch))) {
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
  for (auto& [name, p] : m_prefetched) {
    unload(*p.lev);
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
  // the level it replaces is still in memory)
  return !job.prefetch || m_levels_count + parked < 2;
}

void CtrLevels::loader_main() {
  std::unique_lock<std::mutex> lk(m_lock);
  while (!m_quit) {
    // the first job that can run
    auto it = m_queue.begin();
    while (it != m_queue.end() && !can_start(*it)) {
      ++it;
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

void CtrLevels::run_job(const Job& job) {
  Done d;
  d.job = job;
  m_room_tried = false;
  // (AI-assisted) an allocation that fails (bad_alloc) fails the job: the loader thread goes on
  // (an exception out of it is std::terminate); the render thread tries again later
  try {
    if (job.parts()) {
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
    if (d.job.name == name && !d.job.parts()) {
      m_promised[name] = ctr_gpu_time_ms();
      return true;  // read (or failed); the render thread takes it at its next frame
    }
  }
  return m_requested.count(name) == 0;  // queued or loading: wait
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
  if (dropped || room) {
    // before the loader goes on (it may be waiting for this memory)
    ctr_gpu_free_pending_now();
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
  if (bytes > 128 * 1024 && jak1::io_heap_free() < (2u << 20)) {
    bytes = 128 * 1024;
  }
#endif
  return bytes;
}

// (AI-assisted) the pieces of a .c3l file, as they are needed (v11: decompressed): c3l_reader.h
struct C3lReader : c3l::Reader {
  C3lReader(const fs::path& path, c3l::Reader::Buffers* buffers)
      : c3l::Reader(path.string().c_str(), c3l_read_window(), buffers) {}
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
    (d.job.detail ? lev->detail_requested : lev->repair_requested) = false;
    bump_set_generation();  // (AI-assisted)
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
  if (d.job.detail) {
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
    if (!far_chunk(lev, lev.chunks[ci]) && lev.meshes[ci] >= 0) {
      ctr_gpu_mesh_delete(lev.meshes[ci]);  // freed after the GPU is done with it
      lev.meshes[ci] = -1;
      freed++;
    }
  }
  lev.detail_loaded = false;
  lev.far_frames = 0;
  lg::info("[ctr] {}: only seen from far away, {} detail meshes freed", lev.name, freed);
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
    if (lev.meshes[ci] < 0 && lev.mesh_index_first[ci] != UINT32_MAX &&
        (!missing_only || lev.detail_loaded || far_chunk(lev, ch))) {
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

CtrLevels::LoadResult CtrLevels::load(const std::string& name, CtrLevelData* out) {
  auto path = file_util::get_jak_project_dir() / "out" / "jak1" / "c3l" / (name + ".c3l");
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
  for (size_t i = 0; i < out->meshes.size(); i++) {
    failed += out->meshes[i] < 0 && out->chunks[i].draw_count > 0;
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
void build_draw_order(CtrLevelData* out) {
  out->draw_states.clear();
  out->draw_chunk.clear();
  out->sorted_draws.clear();
  out->ordered_draws.clear();
  out->early_draws.clear();
  out->draw_states.reserve(out->draws.size());
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
    // blending, or drawn over what's there without writing depth (decals): keep the order
    draw_ordered[di] = st.blend != CTR_BLEND_OFF || !st.zwrite;
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
  for (auto& ch : out->chunks) {
    if (cancelled()) {
      return false;
    }
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
    // (AI-assisted) the vertices are read (decoded) straight into GPU memory
    int mesh = -1;
    void* vdst;
    u16* idst;
    if (end <= index_total) {
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
        make_run_spheres(out, (u32)out->meshes.size(), (const c3l::Vertex*)vdst, idst, first, end);
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
          out->draw_alpha80[d] = ok ? 1 : 0;
        }
      }
      mesh = ctr_gpu_mesh_end(mesh, read_ok);
    }
    out->meshes.push_back(mesh);
    // make the draws relative to the mesh's index buffer
    for (u32 d = ch.first_draw; d < ch.first_draw + ch.draw_count; d++) {
      out->draws[d].first_index -= first;
    }
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
  bool levels_changed = false;
  bool unloaded = false;
  const double now = ctr_gpu_time_ms();
  for (auto& d : done) {
    const std::string& name = d.job.name;
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
        lg::warn("[ctr] no common models (out/jak1/c3l/GAME.c3l): Jak won't be drawn");
      }
      continue;
    }
    switch (d.result) {
      case LoadResult::MISSING:
        lg::warn("[ctr] no background for level {} (out/jak1/c3l/{}.c3l)", name, name);
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

// (AI-assisted) a chunk's draw distance limit (game units, 0: none): the converter's (small and
// medium objects) times detail_scale. With chunk kinds (newer c3l files): shrubs up to
// shrub_distance (GPU-bound levels had 200-300k shrub triangles within the converter's 100 m),
// ties times tie_distance_scale (most ties stopped at 100 m: pop-in); with auto_detail both move
// toward their _max values as far as the GPU time allows.
float chunk_max_dist(const c3l::Chunk& ch, const CtrSettings& s, bool shrub_ps2_dist) {
  if (ch.kind == c3l::CHUNK_KIND_SHRUB && shrub_ps2_dist) {
    // (AI-assisted) c3l v14: the PS2 distance of the chunk's shrubs (their prototypes' class)
    float k = s.shrub_scale;
    if (s.auto_detail) {
      k += (std::max(s.shrub_scale_max, k) - k) * g_detail_t;
    }
    return ch.max_dist * k * s.detail_scale;
  }
  if (ch.kind == c3l::CHUNK_KIND_SHRUB && s.shrub_distance > 0) {
    float d = s.shrub_distance;
    if (s.auto_detail) {
      d += (std::max(s.shrub_distance_max, d) - d) * g_detail_t;
    }
    return d * 4096.f * s.detail_scale;
  }
  if (ch.kind == c3l::CHUNK_KIND_TIE) {
    float k = s.tie_distance_scale;
    if (s.auto_detail) {
      k += (std::max(s.tie_distance_scale_max, k) - k) * g_detail_t;
    }
    return ch.max_dist * k * s.detail_scale;
  }
  return ch.max_dist * s.detail_scale;
}

}  // namespace

void ctr_level_set_detail(float t) {
  g_detail_t = std::clamp(t, 0.f, 1.f);
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
}

void CtrTfragRenderer::draw_level(CtrLevelData& lev,
                                  const CtrBackgroundCamera& cam,
                                  const CtrRenderState& rs) {
  const CtrSettings& settings = ctr_settings();
  auto R = make_cam_mat(cam.rot, cam.perspective, cam.fog.x(), cam.hvdf_off.z());
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
  // (AI-assisted) the game's visibility string for this level (bucket of level 1 or 0). Ties are
  // hidden by it always; tfrag only when the string is the level's own: seen from another
  // level's place the game picks the low resolution tfrag through these bits, which the 3DS
  // picks by distance (far_level) instead, so the detailed tfrag would get holes.
  const int li = (m_id == (int)jak1::BucketId::TFRAG_LEVEL1 ||
                  m_id == (int)jak1::BucketId::TFRAG_TRANS1_AND_SKY_BLEND_LEVEL1 ||
                  m_id == (int)jak1::BucketId::TFRAG_DIRT_LEVEL1 ||
                  m_id == (int)jak1::BucketId::TFRAG_ICE_LEVEL1)
                     ? 1
                     : 0;
  m_vis = settings.vis_culling && rs.vis_valid[li] && !lev.vis_runs.empty() ? rs.vis_bits[li]
                                                                            : nullptr;
  const bool vis_own = m_vis && (rs.vis_flags[li] & 4);
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
  // this level's textures: VRAM first if the camera is in it (see ctr_gpu_pool_create)
  ctr_gpu_pool_set_priority(lev.tex_pool, far_level ? 1 : 2);
  // Jak 1 scissor adjust (tfrag3.vert: y *= 512 / 448)
  constexpr float kYScale = 512.f / 448.f;
  m_visible.clear();
  m_visible_slot.assign(lev.chunks.size(), -1);
  // (AI-assisted) run_culling (needs the spheres made at load)
  const bool run_culling = settings.run_culling && !lev.run_spheres.empty();
  m_cull_frames += run_culling;
  if (m_run_shown.size() < lev.vis_runs.size()) {
    m_run_shown.resize(lev.vis_runs.size());
  }
  if (lev.chunk_state.size() != lev.chunks.size()) {
    lev.chunk_state.assign(lev.chunks.size(), 0);
  }
  // (AI-assisted) a chunk that is to be drawn (mode and distance decided): the view test (the
  // chunk's sphere against the four planes; with run_culling, a chunk that crosses planes has its
  // runs tested against those planes on their own), the visibility bits or (vista tier) the
  // source chunks, then its matrix. vista: drawn in the vista pass.
  auto emit_chunk = [&](size_t ci, bool vista_chunk) {
    const auto& ch = lev.chunks[ci];
    const bool vista_tier = ch.kind == c3l::CHUNK_KIND_VISTA;
      // (AI-assisted) the view test: the chunk's sphere against the four planes; a chunk that crosses
      // planes (run_culling) has its runs tested against those planes on their own
      u32 cross = 0;  // planes the chunk's sphere crosses
      {
        const float* s = ch.bsphere;
        const math::Vector4f acc =
            cam.planes[0] * s[0] + cam.planes[1] * s[1] + cam.planes[2] * s[2] - cam.planes[3];
        if (!(acc.x() > -s[3] && acc.y() > -s[3] && acc.z() > -s[3] && acc.w() > -s[3])) {
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
      bool runs_valid = false;  // (AI-assisted) m_run_shown holds this chunk's runs (impostors)
      // (AI-assisted) ties always; tfrag with the level's own string (not its low resolution
      // version), or every tier with a neighbour's (neighbour_vis)
      const bool vis_test = !vista_tier && m_vis && ch.vis_run_count &&
                            (ch.lod_tier == 0 || (vis_own && ch.lod_tier < 3) ||
                             (vis_nb && ch.lod_tier <= 3));
      if (vis_test || straddles || vista_tier) {
        // (AI-assisted) per run: the visibility bit, then the view test; m_run_shown for the draws
        u32 shown = 0, view_hidden = 0, view_tris = 0;
        const u32 r0 = lev.chunk_first_run[ci];
        const u32 nr =
            vis_test || vista_tier ? ch.vis_run_count : lev.chunk_run_count[ci];
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
        const float scale = ch.scale;
        for (u32 r = r0; r < r0 + nr; r++) {
          bool s = true;
          if (vis_test) {
            const u16 id = lev.vis_runs[r].vis;
            s = id == 0xffff || (m_vis[id >> 3] & (0x80 >> (id & 7)));
          } else if (vista_tier) {
            // (AI-assisted) a vista run stands in for its source chunk where that one isn't drawn
            const u16 src = lev.vis_runs[r].vis;
            s = src < m_vista_src.size() && m_vista_src[src];
          }
          if (s && straddles) {
            const auto& rs = lev.run_spheres[r];
            if (rs.r != 0xffff) {
              const float x = rs.c[0], y = rs.c[1], z = rs.c[2], rad = -(float)rs.r * scale;
              for (int i = 0; i < np && s; i++) {
                s = po[i] + pk[i][0] * x + pk[i][1] * y + pk[i][2] * z > rad;
              }
              if (!s) {
                view_hidden++;
                view_tris += lev.vis_runs[r].index_count / 3;
              }
            }
          }
          m_run_shown[r] = s;
          shown += s;
        }
        m_cull_tris += view_tris;
        runs_valid = true;
        if (!shown) {
          if (view_hidden) {
            m_cull_chunks++;
          } else if (!vista_tier) {
            m_vis_chunks++;
          }
          return;
        }
        partly = shown < nr;
      }
      // clip = -(R * (origin + q * scale - cam_trans)) (tfrag3.vert), as a matrix on (q, 1).
      // The translation is done in double: world coordinates are large.
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
      double d[3];
      for (int i = 0; i < 3; i++) {
        d[i] = (double)ch.origin[i] - (double)cam.trans[i];
      }
      float* m = vc.clip;
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
      ctr_gpu_prepare_mesh_matrix(m, &vc.gpu);
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
  for (size_t ci = 0; ci < lev.chunks.size(); ci++) {
    const auto& ch = lev.chunks[ci];
    if (ch.kind == c3l::CHUNK_KIND_VISTA) {
      continue;  // (AI-assisted) after the others (their decisions pick its runs)
    }
    // (AI-assisted) The detail and distance decisions get 10% hysteresis from last frame's, and
    // are made for every chunk every frame (off screen too): with hard cutoffs, turning the camera
    // around Jak or standing at a threshold made whole cells of objects pop in and out. The two
    // versions of a cell share lod_center and see the same decisions: exactly one is drawn.
    u8& state = lev.chunk_state[ci];
    bool lod_ok = true;
    if (!far_level && (ch.lod_tier == 1 || ch.lod_tier == 2)) {
      // detailed version up close, coarse version further away
      const u8 last = state & 3;
      const float f = last == 1 ? 1.1f : (last == 2 ? 0.9f : 1.f);
      const float dx = ch.lod_center[0] - cam.trans[0], dy = ch.lod_center[1] - cam.trans[1],
                  dz = ch.lod_center[2] - cam.trans[2];
      const float lim = lod_dist * f;
      const bool near = lod_dist <= 0 || dx * dx + dy * dy + dz * dz < lim * lim;
      state = (u8)((state & ~3) | (near ? 1 : 2));
      lod_ok = near == (ch.lod_tier == 1);
    }
    bool dist_ok = true;
    // (AI-assisted) lowres_vista: the low resolution tfrag is far scenery (vista_distance)
    const bool vista_chunk = lowres_vista && ch.lod_tier == 3;
    {
      // draw distance; small objects only up close
      float max_dist = vista_chunk ? vista_dist : draw_dist;
      const float d = chunk_max_dist(ch, settings, lev.shrub_ps2_dist);
      if (d > 0) {
        max_dist = max_dist > 0 ? std::min(max_dist, d) : d;
      }
      if (max_dist > 0) {
        const float dx = ch.bsphere[0] - cam.trans[0], dy = ch.bsphere[1] - cam.trans[1],
                    dz = ch.bsphere[2] - cam.trans[2];
        const float lim = max_dist * ((state & 4) ? 1.1f : 1.f) + ch.bsphere[3];
        dist_ok = dx * dx + dy * dy + dz * dz <= lim * lim;
        state = (u8)(dist_ok ? (state | 4) : (state & ~4));
      }
    }
    if (lev.meshes[ci] < 0 || !dist_ok) {
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
        const float dx = ch.bsphere[0] - cam.trans[0], dy = ch.bsphere[1] - cam.trans[1],
                    dz = ch.bsphere[2] - cam.trans[2];
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
  // (AI-assisted) the vista tier: where its source chunks aren't drawn, up to vista_distance
  if (lev.has_vista && vista_dist > 0) {
    for (size_t ci = 0; ci < lev.chunks.size(); ci++) {
      const auto& ch = lev.chunks[ci];
      if (ch.kind != c3l::CHUNK_KIND_VISTA || lev.meshes[ci] < 0 || !ch.vis_run_count) {
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
        if (!vc.vista) {
          impostors.chunk(vc.chunk, vc.clip, vc.runs_valid, m_run_shown.data(), &vc.partly);
        }
      }
      impostors.finish_chunks();
    }
  }

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
    ctr_gpu_set_mesh_nofog(vc.nofog);  // (AI-assisted) fog_stage_skip
#ifndef __3DS__
    {
      // (AI-assisted) snow-gpu-audit: statistics per chunk kind and LOD tier
      static const char* kKind[4] = {"?", "tfrag", "tie", "shrub"};
      const auto& chs = lev.chunks[vc.chunk];
      ctr_gpu_soft_bucket(fmt::format("{}/{}{}", m_name.substr(0, 2), kKind[chs.kind & 3], chs.lod_tier).c_str());
    }
#endif
    if (!vc.partly || !lev.draw_run_count[di]) {
      ctr_gpu_draw_mesh_prepared(&lev.draw_states[di], &vc.gpu, lev.meshes[vc.chunk],
                                 dr.first_index, dr.index_count);
      vista_indices += vista_pass ? dr.index_count : 0;
      return;
    }
    // (AI-assisted) only the visible runs; neighbouring ones in one draw, with small hidden runs
    // between them drawn along (cheaper than another draw)
    // (not in the vista tier: a hidden vista run is a copy of what the main pass draws)
    const u32 kMaxGap = lev.chunks[vc.chunk].kind == c3l::CHUNK_KIND_VISTA ? 0 : 48;
    u32 idx = dr.first_index, start = 0, len = 0, gap = 0, drawn = 0;
    bool open = false;
    auto emit = [&]() {
      ctr_gpu_draw_mesh_prepared(&lev.draw_states[di], &vc.gpu, lev.meshes[vc.chunk], start, len);
      drawn += len;
      vista_indices += vista_pass ? len : 0;
      open = false;
      gap = 0;
    };
    const u32 r0 = lev.draw_first_run[di];
    for (u32 r = r0; r < r0 + lev.draw_run_count[di]; r++) {
      const auto& run = lev.vis_runs[r];
      const bool shown = m_run_shown[r];  // (AI-assisted) visibility bit and view test
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
        if (gap > kMaxGap) {
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
  if (vista_chunks) {
    // (AI-assisted) one walk over the three lists instead of one per pass: the vista chunks' draws
    // (in list order: early, sorted, ordered, as the vista pass drew them) and the other visible
    // ones per list (in their order: the main pass draws them as it drew the whole lists)
    m_vista_draws.clear();
    for (int li = 0; li < 3; li++) {
      std::vector<u32>& main = m_main_draws[li];
      main.clear();
      for (u32 di : *main_lists[li]) {
        const int slot = m_visible_slot[lev.draw_chunk[di]];
        if (slot >= 0) {
          (m_visible[slot].vista ? m_vista_draws : main).push_back(di);
        }
      }
      main_lists[li] = &main;
    }
    ctr_gpu_set_mesh_fog(fog0, fog1_vista, rs.fog_color[0], rs.fog_color[1], rs.fog_color[2]);
    vista_pass = true;
    for (u32 di : m_vista_draws) {
      draw(di);
    }
    vista_pass = false;
    ctr_gpu_set_mesh_fog(fog0, fog1, rs.fog_color[0], rs.fog_color[1], rs.fog_color[2]);
    m_vista_chunks += vista_chunks;
    m_vista_tris += vista_indices / 3;
  }
  m_vista_frames++;
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
    for (u32 di : *main_lists[0]) {
      draw(di);
    }
  }
  for (u32 di : *main_lists[1]) {
    draw(di);
  }
  impostors.draw_cards();  // (AI-assisted) opaque: before the blended draws
  for (u32 di : *main_lists[2]) {
    draw(di);
  }
  ctr_gpu_set_mesh_nofog(0);
  ctr_gpu_cull_reset();  // (AI-assisted) cull_ties
}
