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
  failed -= std::min<int>(failed, (int)out->tex_empty);  // (AI-assisted) v15: none to make
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

// (AI-assisted) the weights each level was last blended with (by name: a level loaded again starts
// with them, CtrLevels::load_file), and the last blend's of any level with its day palettes
// (Tod::palettes without the lights: a level loaded for the first time with the same ones, Sandover
// and its neighbours, starts with those)
struct TodSeen {
  std::mutex lock;
  std::map<std::string, std::array<u8, 24>> by_level;
  std::array<u8, 24> last{};
  u32 last_day_mask = 0;
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

// Blends the level's colors with the game's weights when they moved: all colors when a day palette's
// weight moved by 2/64 or more and kTodMinFrames went by since the last such blend (the day moves
// 1/64 every ~40 frames; the title screen's attract mode runs the day ~20x as fast), or at once by
// kTodJump or more (the clock set, a teleport); else only the light colors when a light palette's
// weight moved (the lamps flicker every frame). Chunks are recolored as they are drawn
// (tod_recolor): kTodBudget vertices per level draw after a blend of all colors (a chunk in pieces
// over frames), the light vertices after a blend of the light colors.
constexpr u64 kTodMinFrames = 30;
constexpr int kTodJump = 8;
constexpr u32 kTodBudget = 4000;

void tod_blend(CtrLevelData::Tod& t, u32 color) {
  const u8* src = t.colors.data() + (size_t)color * t.palette_count * 3;
  u32 r = 0, g = 0, b = 0;
  for (u32 k = 0; k < t.palette_count; k++, src += 3) {
    const u8* w = t.weights[t.palettes[k]];
    r += w[0] * src[0];
    g += w[1] * src[1];
    b += w[2] * src[2];
  }
  t.blended[color] = std::min<u32>(255, r >> 6) | std::min<u32>(255, g >> 6) << 8 |
                     std::min<u32>(255, b >> 6) << 16;
}

// returns 2: all colors blended, 1: the light colors, 0: nothing
int tod_update(CtrLevelData::Tod& t, const std::string& level, const CtrBackgroundCamera& cam,
               u64 frame) {
  u8 w[8][3];
  tod_weights(cam, w);
  int day_diff = 0, light_diff = 0;
  for (int p = 0; p < 8; p++) {
    for (int c = 0; c < 3; c++) {
      const int d = std::abs((int)w[p][c] - (int)t.weights[p][c]);
      if (t.light_mask >> p & 1) {
        light_diff = std::max(light_diff, d);
      } else {
        day_diff = std::max(day_diff, d);
      }
    }
  }
  const bool all = !t.blended_valid || day_diff >= kTodJump ||
                   (day_diff >= 2 && frame >= t.blend_frame + kTodMinFrames);
  if (!all && !light_diff) {
    return 0;
  }
  for (int p = 0; p < 8; p++) {
    if (all || (t.light_mask >> p & 1)) {
      memcpy(t.weights[p], w[p], 3);
    }
  }
  t.light_gen = t.light_gen + 1 == CtrLevelData::Tod::kChunkNone ? 1 : t.light_gen + 1;
  if (!all) {
    for (u16 c : t.light_colors) {
      tod_blend(t, c);
    }
    return 1;
  }
  t.blended_valid = true;
  t.blend_frame = frame;
  t.gen = t.gen + 1 == CtrLevelData::Tod::kChunkNone ? 1 : t.gen + 1;
  std::fill(t.chunk_pos.begin(), t.chunk_pos.end(), 0u);  // (chunks part done start again)
  for (u32 c = 0; c < t.color_count; c++) {
    tod_blend(t, c);
  }
  {
    auto& seen = tod_seen();
    std::lock_guard<std::mutex> lk(seen.lock);
    auto& wl = seen.by_level[level];
    memcpy(wl.data(), t.weights, 24);
    seen.last = wl;
    seen.last_day_mask = tod_day_mask(t);
  }
  return 2;
}

// a vertex's color from the blend (false: it has none)
inline bool tod_vertex_rgb(const CtrLevelData::Tod& t, u32 idx, u32* out) {
  const u32 nc = t.color_count;
  if (idx < nc) {
    *out = t.blended[idx];
    return true;
  }
  if (idx - nc < t.muls.size()) {
    const auto& m = t.muls[idx - nc];
    const u32 c = t.blended[m.color];
    *out = std::min<u32>(255, ((c & 0xff) * m.mul[0] + 64) >> 7) |
           std::min<u32>(255, (((c >> 8) & 0xff) * m.mul[1] + 64) >> 7) << 8 |
           std::min<u32>(255, (((c >> 16) & 0xff) * m.mul[2] + 64) >> 7) << 16;
    return true;
  }
  return false;
}

inline void tod_set_rgb(c3l::Vertex& v, u32 rgb) {
  u32 old;
  memcpy(&old, v.rgba, 4);
  rgb |= old & 0xff000000u;
  memcpy(v.rgba, &rgb, 4);
}

// Vertices [first, end)'s colors from the blend. Returns the vertices with a time of day color.
u32 tod_color_vertices(const CtrLevelData::Tod& t, c3l::Vertex* v, int first, int end) {
  u32 colored = 0;
  for (int i = first; i < end; i++) {
    u32 rgb;
    if (tod_vertex_rgb(t, (u16)v[i].pad, &rgb)) {
      tod_set_rgb(v[i], rgb);
      colored++;
    }
  }
  return colored;
}

// The vertices with a light color (the chunk's list for the light blends), and whether any vertex
// has a time of day color.
bool tod_scan(const CtrLevelData::Tod& t, const c3l::Vertex* v, int n, std::vector<u16>* lights) {
  bool any = false;
  const u32 nc = t.color_count, nm = (u32)t.muls.size();
  for (int i = 0; i < n; i++) {
    const u32 idx = (u16)v[i].pad;
    if (idx >= nc + nm) {
      continue;
    }
    any = true;
    const u32 c = idx < nc ? idx : t.muls[idx - nc].color;
    if (t.light_mask && std::binary_search(t.light_colors.begin(), t.light_colors.end(), (u16)c)) {
      lights->push_back((u16)i);
    }
  }
  lights->shrink_to_fit();
  return any;
}


// The chunk's vertex colors from the blends, rewritten in its mesh (the alpha stays; a frame the GPU
// still draws may see part of it): after a blend of all colors (or for a mesh made again, with the
// file's colors) all of them, kTodBudget vertices per level draw (a big chunk over frames, from
// where it stopped); else its light vertices after a blend of the light colors. Returns the
// vertices written; *ms the time.
u32 tod_recolor(CtrLevelData& lev, size_t ci, u32* budget, double* ms) {
  auto& t = *lev.tod;
  const int mesh = ci < lev.meshes.size() ? lev.meshes[ci] : -1;
  if (mesh < 0 || ci >= t.chunk_gen.size() || !t.blended_valid ||
      t.chunk_gen[ci] == CtrLevelData::Tod::kChunkNone) {
    return 0;
  }
  if (t.chunk_mesh[ci] != mesh) {
    t.chunk_mesh[ci] = mesh;
    t.chunk_gen[ci] = CtrLevelData::Tod::kChunkUnset;
    t.chunk_pos[ci] = 0;
  }
  const bool all = t.chunk_gen[ci] != t.gen;
  if (!all && t.chunk_light_gen[ci] == t.light_gen) {
    return 0;
  }
  if (all && !*budget) {
    return 0;
  }
  int n = 0;
  auto* v = (c3l::Vertex*)ctr_gpu_mesh_vertices_inplace(mesh, &n);
  if (!v || n <= 0) {
    return 0;
  }
  const double t0 = ctr_gpu_time_ms();
  u32 written = 0;
  if (!all) {
    // the light vertices only
    auto it = t.chunk_lights.find((u32)ci);
    if (it != t.chunk_lights.end() && !it->second.empty()) {
      const auto& lv = it->second;
      for (u16 i : lv) {
        u32 rgb;
        if (i < n && tod_vertex_rgb(t, (u16)v[i].pad, &rgb)) {
          tod_set_rgb(v[i], rgb);
        }
      }
      ctr_gpu_mesh_flush(mesh, lv.front() * (int)sizeof(c3l::Vertex),
                         (lv.back() + 1 - lv.front()) * (int)sizeof(c3l::Vertex));
      written = (u32)lv.size();
    }
    t.chunk_light_gen[ci] = t.light_gen;
  } else {
    const int first = (int)std::min<u32>(t.chunk_pos[ci], (u32)n);
    const int end = (int)std::min<u32>((u32)n, first + *budget);
    tod_color_vertices(t, v, first, end);
    ctr_gpu_mesh_flush(mesh, first * (int)sizeof(c3l::Vertex),
                       (end - first) * (int)sizeof(c3l::Vertex));
    written = (u32)(end - first);
    *budget -= std::min(*budget, written);
    if (end >= n) {
      t.chunk_gen[ci] = t.gen;
      t.chunk_light_gen[ci] = t.light_gen;
      t.chunk_pos[ci] = 0;
    } else {
      t.chunk_pos[ci] = (u32)end;
    }
  }
  *ms += ctr_gpu_time_ms() - t0;
  return written;
}

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
  u32 light_colors = 0;
  for (int p = 0; p < 8; p++) {
    if (mask & (1u << p)) {
      t->palettes[t->palette_count++] = (u8)p;
      // a light palette: a quarter of the colors or fewer
      if (used[p] * 4 <= nc) {
        t->light_mask |= 1u << p;
        light_colors += used[p];
      }
    }
  }
  [[maybe_unused]] const u32 need = nc * t->palette_count * 3 + nm * (u32)sizeof(c3l::TodMul) +
                                     nc * 4 + light_colors * 2 + (u32)lev->chunks.size() * 12;
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
  for (u32 i = 0; i < nc; i += kPiece) {
    const u32 n = std::min(kPiece, nc - i);
    if (!f.read_array(hdr.tod_offset + i * (u32)sizeof(c3l::TodColor), n, &piece)) {
      return;
    }
    u8* dst = t->colors.data() + (size_t)i * t->palette_count * 3;
    for (u32 j = 0; j < n; j++) {
      const auto& c = piece[j];
      bool light = false;
      for (u32 k = 0; k < t->palette_count; k++) {
        const u8* rgb = c.rgb[t->palettes[k]];
        memcpy(dst, rgb, 3);
        dst += 3;
        light |= (t->light_mask >> t->palettes[k] & 1) && (rgb[0] | rgb[1] | rgb[2]);
      }
      if (light) {
        t->light_colors.push_back((u16)(i + j));
      }
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
  t->blended.assign(nc, 0);
  t->chunk_gen.assign(lev->chunks.size(), CtrLevelData::Tod::kChunkUnset);
  t->chunk_light_gen.assign(lev->chunks.size(), CtrLevelData::Tod::kChunkUnset);
  t->chunk_pos.assign(lev->chunks.size(), 0);
  t->chunk_mesh.assign(lev->chunks.size(), -1);
  // the weights the level was last drawn with, else the last level's with the same day palettes:
  // the meshes are colored as they are read (CtrLevels::load_file), not all at the level's first
  // draw. Else they keep the file's colors until drawn (tod_recolor).
  {
    auto& seen = tod_seen();
    std::array<u8, 24> w{};
    bool have = false;
    {
      std::lock_guard<std::mutex> lk(seen.lock);
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
      for (u32 c = 0; c < nc; c++) {
        tod_blend(*t, c);
      }
      t->blended_valid = true;
      t->gen = t->light_gen = 1;
    }
  }
  lg::info("[ctr] {}: time of day: {} colors in {} palettes (lights {:02x}: {} colors), {} "
           "multiplied ({} KB)",
           lev->name, nc, t->palette_count, t->light_mask, t->light_colors.size(), nm,
           t->bytes() / 1024);
  lev->tod = std::move(t);
}

// (AI-assisted) fine_vis: <level>.c3v (c3v_format.h) into lev->fine when it was made from the
// level's .c3l as loaded (its chunk, draw and run tables: c3v::fine_vis_level_hash). The level's
// (AI-assisted) c3l v17 tie levels of detail (c3l::ExtHeader): the groups and each file run's
// version, mapped onto vis_runs (whose first file_runs come from the file, from offset runs_lo).
// Anything wrong: no levels of detail, and the far versions of the groups are never drawn (a
// level drawn with every group's near version rather than both on top of each other).
void load_tie_lod(c3l::Reader& f,
                  const c3l::Header& hdr,
                  u32 runs_lo,
                  size_t file_runs,
                  CtrLevelData* lev) {
  lev->lod_groups.clear();
  lev->run_lod.clear();
  lev->chunk_lod.clear();
  lev->lod_state.clear();
  if (hdr.version < c3l::kTieLodVersion || !hdr.ext_offset) {
    return;
  }
  c3l::ExtHeader eh{};
  if (!f.read(hdr.ext_offset, &eh, sizeof(eh)) || memcmp(eh.magic, c3l::kExtMagic, 4) ||
      eh.size < sizeof(eh)) {
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
  lg::info("[ctr] {}: tie levels of detail: {} groups, {} near runs{}", lev->name,
           lev->lod_groups.size(), near_runs, bad ? fmt::format(" ({} bad runs)", bad) : "");
}

// visibility runs (and run_culling's pieces) are made already. Anything wrong: no fine visibility.
void load_fine_vis(const fs::path& c3l_path, CtrLevelData* lev) {
  fs::path p = c3l_path;
  p.replace_extension(".c3v");
  FILE* fp = fopen(p.string().c_str(), "rb");
  if (!fp) {
    return;  // (none: the level draws as without it)
  }
  auto fv = std::make_unique<CtrLevelData::FineVis>();
  auto& h = fv->hdr;
  const char* why = nullptr;
  const u32 covered = c3v::covered_chunks(lev->chunks.data(), (u32)lev->chunks.size());
  if (fread(&h, sizeof(h), 1, fp) != 1 || memcmp(h.magic, c3v::kMagic, 4) ||
      h.version != c3v::kVersion) {
    why = "not a C3V v1 file";
  } else if (h.num_chunks != covered ||
             h.level_hash != c3v::fine_vis_level_hash(lev->chunks.data(), covered,
                                                      lev->draws.data(), (u32)lev->draws.size(),
                                                      lev->vis_runs.data(),
                                                      lev->chunk_first_run.data())) {
    why = "made from another conversion of the level (run ctr_vis_builder on this .c3l)";
  } else if (h.set_words != (h.num_units + 31) / 32 || h.num_sets == 0 || h.num_sets > 0xffff ||
             h.cell <= 0 || h.nx == 0 || h.ny == 0 || h.nz == 0 ||
             (u64)h.nx * h.ny * h.nz > 0xffffffffull) {
    why = "bad header";
  } else {
    fv->cells.resize(h.num_cell_runs);
    fv->sets.resize((size_t)h.num_sets * h.set_words);
    if (fread(fv->cells.data(), sizeof(c3v::CellRun), fv->cells.size(), fp) != fv->cells.size() ||
        fread(fv->sets.data(), 4, fv->sets.size(), fp) != fv->sets.size()) {
      why = "truncated";
    }
  }
  fclose(fp);
  if (!why) {
    // the units: each chunk's, in order (c3v_format.h); a chunk's runs are its units when there
    // are as many (the file's runs, or run_culling's pieces)
    fv->chunk_first_unit.resize(covered + 1);
    fv->chunk_runs.resize(covered);
    u32 u = 0;
    for (u32 ci = 0; ci < covered; ci++) {
      const auto& ch = lev->chunks[ci];
      fv->chunk_first_unit[ci] = u;
      const u32 n = c3v::chunk_units(ch, lev->draws.data(), (u32)lev->draws.size());
      const u32 runs = ch.vis_run_count ? ch.vis_run_count
                                        : (ci < lev->chunk_run_count.size() ? lev->chunk_run_count[ci] : 0);
      fv->chunk_runs[ci] = n > 0 && runs == n;
      u += n;
    }
    fv->chunk_first_unit[covered] = u;
    if (u != h.num_units) {
      why = "unit count differs";
    }
    u32 last_end = 0;
    for (const auto& c : fv->cells) {
      if (c.set >= h.num_sets || c.count == 0 || c.key < last_end) {
        why = "bad cells";
        break;
      }
      last_end = c.key + c.count;
    }
  }
  if (why) {
    lg::warn("[ctr] {}: {} not used: {}", lev->name, p.filename().string(), why);
    return;
  }
  fv->chunk_hidden.assign(covered, 0);
  u32 ncells = 0;
  for (const auto& c : fv->cells) {
    ncells += c.count;
  }
  lev->fine = std::move(fv);
  lg::info("[ctr] {}: fine visibility {}: {} cells of {:.0f} m in {} runs, {} sets of {} units, {} KB",
           lev->name, p.filename().string(), ncells, h.cell / 4096.f, h.num_cell_runs, h.num_sets,
           h.num_units, (lev->fine->bytes() + 1023) / 1024);
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
      // (AI-assisted) c3l v16: the chunk's light vertices, and its time of day colors (load_tod's
      // start weights) when there are some
      int tod_state = -1;  // 0: no time of day colors, 1: colored, 2: not yet
      if (read_ok && out->tod) {
        auto& t = *out->tod;
        std::vector<u16> lights;
        const auto* vv = (c3l::Vertex*)vdst;
        tod_state = tod_scan(t, vv, (int)ch.vertex_count, &lights) ? 2 : 0;
        if (!lights.empty()) {
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
        const size_t ci = out->meshes.size();
        t.chunk_mesh[ci] = mesh;
        t.chunk_gen[ci] = tod_state == 0   ? CtrLevelData::Tod::kChunkNone
                          : tod_state == 1 ? t.gen
                                           : CtrLevelData::Tod::kChunkUnset;
        t.chunk_light_gen[ci] = tod_state == 1 ? t.light_gen : CtrLevelData::Tod::kChunkUnset;
      }
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
// toward their _max values as far as the GPU time allows. (micro-level: worked out in draw_level's
// chunk loop, its factors once per level draw)

// (AI-assisted) fine_vis: the set of the cell holding the camera position t (-1: none)
int fine_vis_cell_set(const CtrLevelData::FineVis& fv, const math::Vector4f& t) {
  const auto& h = fv.hdr;
  const float fx = (t.x() - h.origin[0]) / h.cell, fy = (t.y() - h.origin[1]) / h.cell,
              fz = (t.z() - h.origin[2]) / h.cell;
  if (!(fx >= 0 && fy >= 0 && fz >= 0 && fx < (float)h.nx && fy < (float)h.ny &&
        fz < (float)h.nz)) {
    return -1;  // (outside the grid, or not a number)
  }
  const u32 x = std::min<u32>((u32)fx, h.nx - 1), y = std::min<u32>((u32)fy, h.ny - 1),
            z = std::min<u32>((u32)fz, h.nz - 1);
  const u32 key = (z * h.ny + y) * h.nx + x;
  auto it = std::upper_bound(fv.cells.begin(), fv.cells.end(), key,
                             [](u32 k, const c3v::CellRun& c) { return k < c.key; });
  if (it == fv.cells.begin()) {
    return -1;
  }
  --it;
  return key < it->key + it->count ? it->set : -1;
}

// (AI-assisted) fine_vis: what set `set` hides of each covered chunk (FineVis::chunk_hidden)
void fine_vis_select(CtrLevelData::FineVis& fv, int set) {
  fv.set = set;
  if (set < 0) {
    return;
  }
  const u32* bits = fv.sets.data() + (size_t)set * fv.hdr.set_words;
  for (size_t ci = 0; ci < fv.chunk_hidden.size(); ci++) {
    const u32 a = fv.chunk_first_unit[ci], b = fv.chunk_first_unit[ci + 1];
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
    fv.chunk_hidden[ci] = seen == b - a ? 0 : (seen == 0 ? 1 : 2);
  }
}

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
          lg::warn("[ctr] fine_vis check: frame {}: {} chunk {} run {}: {} pixels would be seen",
                   frame, d.level, d.chunk, d.run == ~0u ? -1 : (int)d.run, n);
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
             "(most {} vertices, {:.2f} ms; {} level draws)",
             m_name, m_tod_blends, (double)m_tod_verts / m_tod_frames, m_tod_ms / m_tod_frames,
             m_tod_max, m_tod_max_ms, m_tod_frames);
    m_tod_frames = m_tod_blends = m_tod_verts = m_tod_max = 0;
    m_tod_ms = m_tod_max_ms = 0;
  }
  if (rs.log_now && m_fine_levels) {
    lg::info("[ctr] {}: fine vis hid {:.1f} chunks, {:.0f} tris per level draw with a cell ({} of "
             "{} level draws)",
             m_name, (double)m_fine_chunks / std::max<u64>(1, m_fine_frames),
             (double)m_fine_tris / std::max<u64>(1, m_fine_frames), m_fine_frames, m_fine_levels);
    m_fine_chunks = m_fine_tris = m_fine_frames = m_fine_levels = 0;
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
  const CtrSettings& settings = ctr_settings();
  auto R = make_cam_mat(cam.rot, cam.perspective, cam.fog.x(), cam.hvdf_off.z());
  // (AI-assisted) c3l v16 time of day: blend the level's colors with the game's weights when they
  // moved (the chunks are recolored as they are drawn, emit_chunk)
  u32 tod_budget = kTodBudget;
  u64 tod_verts = 0;
  double tod_ms = 0;
  if (lev.tod) {
    const double t0 = ctr_gpu_time_ms();
    m_tod_frames++;
    m_tod_blends += tod_update(*lev.tod, lev.name, cam, rs.frame_idx) == 2;
    tod_ms += ctr_gpu_time_ms() - t0;
  }
#ifndef __3DS__
  // (AI-assisted) host diagnostic: the game's time of day weights every 30 frames
  if (getenv("CTR_TOD_LOG") && rs.frame_idx % 30 == 0) {
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
  // (AI-assisted) fine_vis: the set of the camera's cell (the camera's own position), while the
  // level is drawn as the sets assume: near mode with all of its meshes (an occluder that isn't
  // drawn hides nothing), everything within the occluder range drawn (draw distance), and the LOD
  // tiers by the distances the tool assumed. Elsewhere (no cell, cutscene cameras outside the
  // cells, far mode...) nothing more is hidden.
  m_fine = nullptr;
  m_fine_hidden.clear();
  if (lev.fine && settings.fine_vis) {
    auto& fv = *lev.fine;
    m_fine_levels++;
    const float occ_dist = fv.hdr.occluder_range + 2 * fv.hdr.eps;
    const bool ok = !far_level && !lev.incomplete && (draw_dist <= 0 || draw_dist >= occ_dist) &&
                    lod_dist > 0 && lod_dist * 0.9f >= fv.hdr.lod_near &&
                    lod_dist * 1.1f <= fv.hdr.lod_far;
    const int set = ok ? fine_vis_cell_set(fv, cam.trans) : -1;
    if (set != fv.set) {
      fine_vis_select(fv, set);
    }
    if (set >= 0) {
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
  // (AI-assisted) c3l v17 tie levels of detail: each group's near version while the camera is
  // within its distance (the prototype's PS2 far-plane, times tie_lod_scale) of its sphere, with 10%
  // hysteresis, decided for every group every frame (turning the camera doesn't switch any).
  // auto_detail pulls the distances in to half while the GPU is busy. tie_lod = off: the far
  // versions only (as before v17).
  const bool tie_lod = !lev.lod_groups.empty() && lev.run_lod.size() >= lev.vis_runs.size();
  if (tie_lod) {
    float k = settings.tie_lod ? settings.detail_scale : 0.f;
    if (settings.auto_detail) {
      k *= 0.5f + 0.5f * g_detail_t;
    }
    const float cx = cam.trans[0], cy = cam.trans[1], cz = cam.trans[2];
    u32 near = 0;
    for (size_t g = 0; g < lev.lod_groups.size(); g++) {
      const auto& lg = lev.lod_groups[g];
      u8& st = lev.lod_state[g];
      const float dx = lg.bsphere[0] - cx, dy = lg.bsphere[1] - cy, dz = lg.bsphere[2] - cz;
      const float lim = lg.dist * k * ((st & 1) ? 1.1f : 1.f) + lg.bsphere[3];
      const bool n = k > 0 && dx * dx + dy * dy + dz * dz < lim * lim;
      st = (u8)(2 | (n ? 1 : 0));
      near += n;
    }
    m_lod_groups_near += near;
    m_lod_frames++;
  }
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
  const u32* fine_bits =
      m_fine ? m_fine->sets.data() + (size_t)m_fine->set * m_fine->hdr.set_words : nullptr;
  const bool fine_check = fine_vis_check_enabled();
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
      if (vis_test || straddles || vista_tier || fine_runs || lod_chunk) {
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
        kRunLoops[vis_test ? 1 : 0][straddles ? np : 0][lod_chunk ? 1 : 0](in, r0, nr, &o);
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
      if (lev.tod) {
        tod_verts += tod_recolor(lev, ci, &tod_budget, &tod_ms);  // (AI-assisted) c3l v16
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
  // (AI-assisted, micro-level) chunk_max_dist's factors once per level draw (per chunk the same
  // operations on them as before), and the arrays and the camera in locals: the u8 stores to the
  // chunk states had every member and vector loaded again after each of them
  const float det = settings.detail_scale;
  float shrub_k = settings.shrub_scale;
  float shrub_d = settings.shrub_distance;
  float tie_k = settings.tie_distance_scale;
  if (settings.auto_detail) {
    shrub_k += (std::max(settings.shrub_scale_max, shrub_k) - shrub_k) * g_detail_t;
    shrub_d += (std::max(settings.shrub_distance_max, shrub_d) - shrub_d) * g_detail_t;
    tie_k += (std::max(settings.tie_distance_scale_max, tie_k) - tie_k) * g_detail_t;
  }
  const bool shrub_ps2 = lev.shrub_ps2_dist;
  const bool shrub_fixed = settings.shrub_distance > 0;
  const float shrub_fixed_dist = shrub_d * 4096.f * det;
  const c3l::Chunk* const chunks = lev.chunks.data();
  u8* const chunk_state = lev.chunk_state.data();
  const int* const meshes = lev.meshes.data();
  const float cx = cam.trans[0], cy = cam.trans[1], cz = cam.trans[2];
  const size_t nchunks = lev.chunks.size();
  for (size_t ci = 0; ci < nchunks; ci++) {
    const auto& ch = chunks[ci];
    if (ch.kind == c3l::CHUNK_KIND_VISTA) {
      continue;  // (AI-assisted) after the others (their decisions pick its runs)
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
      if (d > 0) {
        max_dist = max_dist > 0 ? std::min(max_dist, d) : d;
      }
      if (max_dist > 0) {
        const float dx = ch.bsphere[0] - cx, dy = ch.bsphere[1] - cy, dz = ch.bsphere[2] - cz;
        const float lim = max_dist * ((state & 4) ? 1.1f : 1.f) + ch.bsphere[3];
        dist_ok = dx * dx + dy * dy + dz * dz <= lim * lim;
        state = (u8)(dist_ok ? (state | 4) : (state & ~4));
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
    const u32 kMaxGap = lev.chunks[vc.chunk].kind == c3l::CHUNK_KIND_VISTA ? 0 : 48;
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
    m_calls.clear();
    for (u32 di : m_vista_draws) {
      draw(di);
    }
    flush_calls(1);  // (AI-assisted, gpu-rt) static_cmdlists: the vista pass's
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
  impostors.draw_cards();  // (AI-assisted) opaque: before the blended draws
  walk(*main_lists[2]);
  flush_calls(0);  // (AI-assisted, gpu-rt) static_cmdlists: the main pass's
  ctr_gpu_set_mesh_nofog(0);
  ctr_gpu_cull_reset();  // (AI-assisted) cull_ties
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
    for (const auto& [ci, r] : m_fine_hidden) {
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
                                ci, r});
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
    m_tod_verts += tod_verts;
    m_tod_max = std::max<u64>(m_tod_max, tod_verts);
    m_tod_ms += tod_ms;
    m_tod_max_ms = std::max(m_tod_max_ms, tod_ms);
  }
}
