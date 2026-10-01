/*!
 * @file CtrLevel.cpp
 * (AI-assisted)
 * See CtrLevel.h. The camera math follows the PC tfrag renderer
 * (make_new_cam_mat and tfrag3.vert in game/graphics/opengl_renderer).
 */

#include "CtrLevel.h"

#include "game/graphics/ctr/CtrSettings.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include "common/dma/dma.h"
#include "common/dma/gs.h"
#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/util/Timer.h"

#include "game/graphics/ctr/ctr_gpu.h"
#include "game/graphics/opengl_renderer/buckets.h"

#ifdef __3DS__
#include "platform/3ds/port/ctr_port.h"
#endif

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

namespace {
// below the render thread (0x2F) and the sound mixer (0x2E), which share core 2 with the loader
constexpr int kLoaderPrio = 0x3C;
// a prefetched level nothing asked for is dropped after this long
constexpr double kPrefetchKeepMs = 60000.0;
}  // namespace

CtrLevels::CtrLevels() {
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

void CtrLevels::loader_main() {
  std::unique_lock<std::mutex> lk(m_lock);
  while (!m_quit) {
    // the first job that can run: a prefetch waits while two levels are loaded (the game has two
    // level slots, so the level it replaces is still in memory)
    auto it = m_queue.begin();
    while (it != m_queue.end() && it->prefetch && m_levels_loaded >= 2) {
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
  if (job.detail) {
    run_detail_job(job, &d);
    std::lock_guard<std::mutex> lk(m_lock);
    m_done.push_back(std::move(d));
    return;
  }
  d.lev = std::make_unique<CtrLevelData>();
  d.lev->generation = ++m_generation;
  d.result = load(job.name, d.lev.get());
  if (d.result != LoadResult::LOADED) {
    d.lev.reset();
  }
  std::lock_guard<std::mutex> lk(m_lock);
  m_done.push_back(std::move(d));
}

void CtrLevels::request(const Job& job) {
  {
    std::lock_guard<std::mutex> lk(m_lock);
    if (m_requested.count(job.key()) || (job.prefetch && m_resident.count(job.name))) {
      // (AI-assisted) the game wants a level whose prefetch is still queued: the prefetch would
      // wait while two levels are loaded (one of them maybe a prefetch nobody wants any more)
      if (!job.prefetch && !job.detail) {
        for (auto& q : m_queue) {
          if (q.name == job.name && q.prefetch && !q.detail) {
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
    return true;
  }
  for (const auto& d : m_done) {
    if (d.job.name == name) {
      return true;  // read (or failed); the render thread takes it at its next frame
    }
  }
  return m_requested.count(name) == 0;  // queued or loading: wait
}

void CtrLevels::prefetch(const std::string& name) {
  request({name, false, true});
}

void CtrLevels::update_level_count() {
  {
    std::lock_guard<std::mutex> lk(m_lock);
    m_levels_loaded = (int)(m_levels.size() + m_prefetched.size());
    m_resident.clear();
    for (auto& [name, lev] : m_levels) {
      m_resident.insert(name);
    }
    for (auto& [name, p] : m_prefetched) {
      m_resident.insert(name);
    }
  }
  m_cv.notify_all();
}

void CtrLevels::publish(const std::string& name, std::unique_ptr<CtrLevelData> lev, u64 frame) {
  lev->last_used_frame = frame;
  m_levels[name] = std::move(lev);
  rebuild_merc_index();
}

namespace {
/*!
 * Reads the pieces of a .c3l file as they are needed. A level file is ~10 MB: reading it in one
 * piece needs a free block of memory that big, which the 3DS heap often doesn't have by the time
 * a level loads (std::bad_alloc, and the level has no background).
 */
class C3lReader {
 public:
  explicit C3lReader(const fs::path& path) : m_f(fopen(path.string().c_str(), "rb")) {
    if (m_f) {
      setvbuf(m_f, nullptr, _IOFBF, 64 * 1024);
      fseek(m_f, 0, SEEK_END);
      m_size = ftell(m_f);
      fseek(m_f, 0, SEEK_SET);
    }
  }
  ~C3lReader() {
    if (m_f) {
      fclose(m_f);
    }
  }
  bool ok() const { return m_f && !m_error; }
  long size() const { return m_size; }
  bool read(u32 offset, void* dst, size_t bytes) {
    if (!ok() || (long)offset + (long)bytes > m_size) {
      m_error = true;
      return false;
    }
    if (bytes == 0) {
      return true;
    }
    if (m_pos != (long)offset && fseek(m_f, offset, SEEK_SET) != 0) {
      m_error = true;
      return false;
    }
    if (fread(dst, 1, bytes, m_f) != bytes) {
      m_error = true;
      return false;
    }
    m_pos = (long)offset + (long)bytes;
    return true;
  }
  template <typename T>
  bool read_array(u32 offset, size_t count, std::vector<T>* out) {
    out->resize(count);
    return read(offset, out->data(), count * sizeof(T));
  }

 private:
  FILE* m_f;
  long m_size = 0;
  long m_pos = 0;
  bool m_error = false;
};
}  // namespace

// (AI-assisted) partial unload: read the chunk meshes of a loaded level again (loader thread). The
// job has its own copy of what it needs: the level may be unloaded meanwhile.
void CtrLevels::run_detail_job(const Job& job, Done* d) {
  d->result = LoadResult::FAILED;
  C3lReader f(job.path);
  std::vector<u16> indices;
  std::vector<c3l::Vertex> verts;
  for (const auto& c : job.chunks) {
    if (cancelled() || !f.ok()) {
      break;
    }
    if (!f.read_array(job.index_data_offset + c.index_first * (u32)sizeof(u16),
                      c.index_end - c.index_first, &indices) ||
        !f.read_array(job.vertex_data_offset + c.first_vertex * (u32)sizeof(c3l::Vertex),
                      c.vertex_count, &verts)) {
      break;
    }
    const int mesh = ctr_gpu_mesh_create(verts.data(), c.vertex_count, indices.data(),
                                         c.index_end - c.index_first);
    if (mesh < 0) {
      break;
    }
    d->meshes.emplace_back(c.chunk, mesh);
  }
  if (d->meshes.size() == job.chunks.size()) {
    d->result = LoadResult::LOADED;
  }
}

void CtrLevels::unload_detail(CtrLevelData& lev) {
  const u32 keep = lev.has_lowres ? 3u : 2u;  // what far mode draws (CtrTfragRenderer)
  int freed = 0;
  for (size_t ci = 0; ci < lev.chunks.size() && ci < lev.meshes.size(); ci++) {
    if (lev.chunks[ci].lod_tier != keep && lev.meshes[ci] >= 0) {
      ctr_gpu_mesh_delete(lev.meshes[ci]);  // freed after the GPU is done with it
      lev.meshes[ci] = -1;
      freed++;
    }
  }
  lev.detail_loaded = false;
  lev.far_frames = 0;
  lg::info("[ctr] {}: only seen from far away, {} detail meshes freed", lev.name, freed);
}

void CtrLevels::request_detail(CtrLevelData& lev) {
  Job job;
  job.name = lev.name;
  job.detail = true;
  job.generation = lev.generation;
  job.path = lev.path;
  job.vertex_data_offset = lev.vertex_data_offset;
  job.index_data_offset = lev.index_data_offset;
  for (size_t ci = 0; ci < lev.chunks.size() && ci < lev.meshes.size() &&
                      ci < lev.mesh_index_first.size();
       ci++) {
    if (lev.meshes[ci] < 0 && lev.mesh_index_first[ci] != UINT32_MAX) {
      const auto& ch = lev.chunks[ci];
      job.chunks.push_back({(u32)ci, ch.first_vertex, ch.vertex_count, lev.mesh_index_first[ci],
                            lev.mesh_index_end[ci]});
    }
  }
  if (job.chunks.empty()) {
    lev.detail_loaded = true;
    return;
  }
  lev.detail_requested = true;
  lg::info("[ctr] {}: camera coming back, reading {} detail meshes", lev.name, job.chunks.size());
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
  try {
    ok = load_file(path, name, out, &file_size);
  } catch (std::exception& e) {
    lg::error("[ctr] failed to load {}: {}", path.string(), e.what());
  }
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
  lg::info("[ctr] loaded {} ({} chunks, {} merc models, {} textures, {} KB) in {:.0f} ms", name,
           out->chunks.size(), out->merc_models.size(), out->textures.size(), file_size / 1024,
           timer.getMs());
  if (failed) {
    lg::error("[ctr] {}: {} textures/meshes could not be created (out of GPU memory?)", name,
              failed);
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
}  // namespace

bool CtrLevels::load_file(const fs::path& path,
                          const std::string& name,
                          CtrLevelData* out,
                          long* file_size) {
  C3lReader f(path);
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
  if (hdr.version < c3l::kVersion) {
    lg::warn("[ctr] {}: old C3L v{} file ({}): convert the levels again (ctr_level_converter --all)",
             path.string(), hdr.version,
             hdr.version < 8   ? "no mip levels, 16-bit textures: slow on the 3DS"
             : hdr.version < 9 ? "no envmap shine on models"
                               : "no visibility culling");
  }
  out->name = name;
  if (!f.read_array(hdr.chunks_offset, hdr.num_chunks, &out->chunks) ||
      !f.read_array(hdr.draw_data_offset, hdr.draw_data_size / sizeof(c3l::Draw), &out->draws)) {
    lg::error("[ctr] {}: truncated file", path.string());
    return false;
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

  std::vector<c3l::Texture> texs;
  if (!f.read_array(hdr.textures_offset, hdr.num_textures, &texs)) {
    lg::error("[ctr] {}: truncated file", path.string());
    return false;
  }
  // All textures (with their mip levels) in one pool: the renderer moves it to VRAM when there's
  // room, with one GPU copy. The texels are read straight into it.
  {
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
    u32 pool_bytes = 0;
    for (size_t i = 0; i < texs.size(); i++) {
      const auto& t = texs[i];
      auto& p = plan[i];
      const bool pow2 = t.w >= 8 && t.h >= 8 && t.w <= 1024 && t.h <= 1024 &&
                        !(t.w & (t.w - 1)) && !(t.h & (t.h - 1));
      if (!pow2 || t.format > c3l::TEX_RGBA8 ||
          (hdr.version < 8 && t.format > c3l::TEX_RGBA4)) {
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
      p.offset = pool_bytes;
      pool_bytes += (ctr_gpu_tex_bytes(t.w, t.h, p.format, p.levels) + 127) & ~127u;
    }
    out->tex_min_alpha.assign(texs.size(), 0);
    const int pool = pool_bytes ? ctr_gpu_pool_create(pool_bytes) : -1;
    if (pool_bytes && pool < 0) {
      lg::error("[ctr] {}: no memory for the textures ({} KB)", name, pool_bytes / 1024);
    }
    out->tex_pool = pool;
    u8* base = pool >= 0 ? (u8*)ctr_gpu_pool_data(pool) : nullptr;
    std::vector<u8> texels;
    for (size_t i = 0; i < texs.size(); i++) {
      if (cancelled()) {
        return false;
      }
      const auto& t = texs[i];
      const auto& p = plan[i];
      int handle = -1;
      if (p.ok && base) {
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
          handle = ctr_gpu_pool_tex(pool, p.offset, t.w, t.h, p.format, p.levels);
          out->tex_min_alpha[i] = texture_min_alpha(dst, t.w, t.h, p.format);
        }
      }
      out->textures.push_back(handle);
    }
    if (pool >= 0) {
      ctr_gpu_pool_ready(pool);
    }
  }

  out->path = path.string();
  out->vertex_data_offset = hdr.vertex_data_offset;
  out->index_data_offset = hdr.index_data_offset;
  // all indices at once (~1 MB): reading them chunk by chunk between the vertex reads would seek
  // back and forth, and every seek throws away the read buffer
  std::vector<u16> indices;
  if (!f.read_array(hdr.index_data_offset, hdr.index_data_size / sizeof(u16), &indices)) {
    lg::error("[ctr] {}: truncated file", path.string());
    return false;
  }
  std::vector<c3l::Vertex> verts;
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
    int mesh = -1;
    if (end <= indices.size() &&
        f.read_array(hdr.vertex_data_offset + ch.first_vertex * sizeof(c3l::Vertex),
                     ch.vertex_count, &verts)) {
      mesh = ctr_gpu_mesh_create(verts.data(), ch.vertex_count, indices.data() + first,
                                 end - first);
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
  }
  // GPU state per draw, so drawing doesn't decode the draw modes every frame
  out->draw_states.reserve(out->draws.size());
  for (auto& d : out->draws) {
    DrawMode mode;
    mode.as_int() = d.mode;
    const bool textured = d.texture < out->textures.size();
    ctr_draw_state st =
        ctr_state_from_draw_mode(mode, textured ? out->textures[d.texture] : -1);
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
  {
    std::vector<ctr_draw_state> states;
    out->draw_sort_keys.resize(out->draws.size());
    out->draw_ordered.resize(out->draws.size());
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
      out->draw_sort_keys[di] = ((u32)(tex + 1) << 16) | (id & 0xffff);
      // blending, or drawn over what's there without writing depth (decals): keep the order
      out->draw_ordered[di] = st.blend != CTR_BLEND_OFF || !st.zwrite;
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
        if (out->draw_ordered[di]) {
          out->ordered_draws.push_back(di);
        } else {
          // (draws are numbered in chunk order: di sorts like (chunk, draw))
          sorted.push_back(((u64)out->draw_sort_keys[di] << 32) | di);
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
  // merc models: one skinned mesh per model
  if (hdr.num_merc_models) {
    std::vector<c3l::MercModel> models;
    std::vector<c3l::MercDraw> mdraws;
    std::vector<u16> mindices;
    if (!f.read_array(hdr.merc_models_offset, hdr.num_merc_models, &models) ||
        !f.read_array(hdr.merc_draw_offset, hdr.merc_draw_size / sizeof(c3l::MercDraw),
                      &mdraws) ||
        !f.read_array(hdr.merc_index_offset, hdr.merc_index_size / sizeof(u16), &mindices)) {
      lg::error("[ctr] {}: truncated file", path.string());
      return false;
    }
    // blend shapes
    if (hdr.merc_blerc_offset && hdr.merc_blerc_size >= sizeof(c3l::MercBlercHeader)) {
      c3l::MercBlercHeader bh;
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
    }
    std::vector<c3l::MercVertex> mverts;
    for (auto& m : models) {
      if (cancelled()) {
        return false;
      }
      CtrMercModelData md;
      md.name = std::string(m.name, strnlen(m.name, sizeof(m.name)));
      md.scale = m.scale;
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
      u32 first = UINT32_MAX, end = 0;
      for (auto& d : md.draws) {
        first = std::min<u32>(first, d.first_index);
        end = std::max<u32>(end, d.first_index + d.index_count);
      }
      for (auto& d : md.draws) {
        d.first_index -= first;
      }
      if (end <= mindices.size() &&
          f.read_array(hdr.merc_vertex_offset + m.first_vertex * sizeof(c3l::MercVertex),
                       m.vertex_count, &mverts)) {
        md.mesh = ctr_gpu_skinned_mesh_create(mverts.data(), m.vertex_count,
                                              mindices.data() + first, end - first);
      }
      out->merc_models.push_back(std::move(md));
    }
  }
  return true;
}

void CtrLevels::process_pending_loads(u64 frame) {
  m_render_frame.store(frame);
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
  const double now = ctr_gpu_time_ms();
  for (auto& d : done) {
    const std::string& name = d.job.name;
    if (d.job.detail) {
      // (AI-assisted) partial unload: the meshes read again, for the same load of the level
      auto it = m_levels.find(name);
      CtrLevelData* lev = it != m_levels.end() ? it->second.get() : nullptr;
      if (lev && lev->generation != d.job.generation) {
        lev = nullptr;
      }
      if (lev && d.result == LoadResult::LOADED) {
        for (auto& [ci, mesh] : d.meshes) {
          if (ci < lev->meshes.size() && lev->meshes[ci] < 0) {
            lev->meshes[ci] = mesh;
          } else {
            ctr_gpu_mesh_delete(mesh);
          }
        }
        lev->detail_loaded = true;
        lg::info("[ctr] {}: detail meshes back ({})", name, d.meshes.size());
      } else {
        for (auto& [ci, mesh] : d.meshes) {
          ctr_gpu_mesh_delete(mesh);
        }
        if (lev) {
          // not every frame while the memory isn't there
          lev->detail_retry_frame = frame + 300;
          lg::warn("[ctr] {}: detail meshes could not be read again, retrying later", name);
        }
      }
      if (lev) {
        lev->detail_requested = false;
      }
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
        // (AI-assisted) not for good (a failed allocation or SD read): again in 10 s
        m_failed[name] = now;
        break;
      case LoadResult::CANCELLED:
        break;
      case LoadResult::LOADED:
        if (m_levels.count(name) || m_prefetched.count(name)) {
          unload(*d.lev);  // loaded twice (asked for again while it loaded)
        } else if (std::find(m_wanted.begin(), m_wanted.end(), name) != m_wanted.end() ||
                   (!d.job.prefetch && m_wanted.empty())) {
          publish(name, std::move(d.lev), frame);
          levels_changed = true;
        } else {
          // (AI-assisted) a prefetch, or a load the game no longer wants (it changed its level list
          // while this one loaded): kept aside, dropped if nothing asks for it
          m_prefetched[name] = Prefetched{std::move(d.lev), now};
          levels_changed = true;
        }
        break;
    }
  }
  // prefetched levels nothing asked for
  for (auto it = m_prefetched.begin(); it != m_prefetched.end();) {
    if (now - it->second.since_ms > kPrefetchKeepMs) {
      lg::info("[ctr] dropping prefetched {} (not used)", it->first);
      unload(*it->second.lev);
      it = m_prefetched.erase(it);
      levels_changed = true;
    } else {
      ++it;
    }
  }
  if (levels_changed) {
    update_level_count();
  }
}

void CtrLevels::unload(CtrLevelData& lev) {
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

const CtrMercModelData* CtrLevels::find_merc_model(const std::string& name,
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
      if (ctr_gpu_time_ms() - failed->second < 10000.0) {
        return nullptr;
      }
      m_failed.erase(failed);
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
  // stop loads of levels that are no longer wanted (prefetches are for levels to come: kept)
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
  }
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
  // Load the wanted levels before the next frame, not only when their background is drawn: a level
  // can be loaded just for its models. The intro loads the "intro" level (Gol and Maia) next to
  // misty with display mode special, so it never draws a background and was never loaded.
  for (auto& n : names) {
    if (n.empty() || m_levels.count(n) || m_missing.count(n) ||
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

bool sphere_in_view(const float* s, const math::Vector4f* planes) {
  math::Vector4f acc = planes[0] * s[0] + planes[1] * s[1] + planes[2] * s[2] - planes[3];
  return acc.x() > -s[3] && acc.y() > -s[3] && acc.z() > -s[3] && acc.w() > -s[3];
}

}  // namespace

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
    lg::info("[ctr] {}: visibility hid {:.0f} chunks and {:.0f} tris per frame ({} frames with data)",
             m_name, (double)m_vis_chunks / m_vis_frames, (double)m_vis_tris / m_vis_frames,
             m_vis_frames);
    m_vis_chunks = m_vis_tris = m_vis_frames = 0;
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
  m_vis_frames += m_vis != nullptr;
  {
    float fog0[4] = {0.f, 255.f, 255.f, -1.f / 255.f};
    if (settings.fog) {
      fog0[0] = cam.hvdf_off.w();
      fog0[1] = cam.fog.y();
      fog0[2] = cam.fog.z();
    }
    float fog1[4] = {0.f, 1.f, 0.f, 0.f};
    const float pzw = std::abs(cam.perspective[2][3]);
    if (draw_dist > 0 && pzw > 0) {
      const float start = draw_dist * settings.fog_start;
      fog1[0] = 1.f / pzw;
      fog1[1] = start;
      fog1[2] = 1.f / std::max(draw_dist - start, 1.f);
    }
    ctr_gpu_set_mesh_fog(fog0, fog1, rs.fog_color[0], rs.fog_color[1], rs.fog_color[2]);
  }
  // this level's textures: VRAM first if the camera is in it (see ctr_gpu_pool_create)
  ctr_gpu_pool_set_priority(lev.tex_pool, far_level ? 1 : 2);
  // Jak 1 scissor adjust (tfrag3.vert: y *= 512 / 448)
  constexpr float kYScale = 512.f / 448.f;
  m_visible.clear();
  m_visible_slot.assign(lev.chunks.size(), -1);
  if (lev.chunk_state.size() != lev.chunks.size()) {
    lev.chunk_state.assign(lev.chunks.size(), 0);
  }
  for (size_t ci = 0; ci < lev.chunks.size(); ci++) {
    const auto& ch = lev.chunks[ci];
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
    {
      // draw distance; small objects only up close
      float max_dist = draw_dist;
      if (ch.max_dist > 0) {
        const float d = ch.max_dist * settings.detail_scale;
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
      continue;
    }
    if (far_level) {
      // only the level's low resolution version (or the coarse one), no tie
      if (ch.lod_tier != (lev.has_lowres ? 3u : 2u)) {
        continue;
      }
    } else if (ch.lod_tier == 3 || !lod_ok) {
      continue;
    }
    if (!sphere_in_view(ch.bsphere, cam.planes)) {
      continue;
    }
    bool partly = false;
    if (m_vis && ch.vis_run_count && (ch.lod_tier == 0 || (vis_own && ch.lod_tier < 3))) {
      u32 shown = 0;
      const u32 r0 = lev.chunk_first_run[ci];
      for (u32 r = r0; r < r0 + ch.vis_run_count; r++) {
        const u16 id = lev.vis_runs[r].vis;
        shown += id == 0xffff || (m_vis[id >> 3] & (0x80 >> (id & 7)));
      }
      if (!shown) {
        m_vis_chunks++;
        continue;
      }
      partly = shown < ch.vis_run_count;
    }
    // clip = -(R * (origin + q * scale - cam_trans)) (tfrag3.vert), as a matrix on (q, 1).
    // The translation is done in double: world coordinates are large.
    m_visible_slot[ci] = (int)m_visible.size();
    VisibleChunk& vc = m_visible.emplace_back();
    vc.chunk = (u32)ci;
    vc.partly = partly;
    {
      // (AI-assisted) distance band of the chunk's nearest point, for the early depth order
      const float dx = ch.bsphere[0] - cam.trans[0], dy = ch.bsphere[1] - cam.trans[1],
                  dz = ch.bsphere[2] - cam.trans[2];
      const float near = std::sqrt(dx * dx + dy * dy + dz * dz) - ch.bsphere[3];
      vc.band = near < 30.f * 4096.f ? 0 : (near < 100.f * 4096.f ? 1 : 2);
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
  }

  // The draws whose order doesn't matter (opaque, depth written) sorted by texture and state,
  // across chunks: every texture change clears the GPU's texture cache, and the chunks share most
  // of their textures. Then the others (blending, decals) in their order, over the opaque ones.
  // The order is made at load (sorted_draws, ordered_draws): only the visible chunks' draws.
  auto draw = [&](u32 di) {
    const int slot = m_visible_slot[lev.draw_chunk[di]];
    if (slot < 0) {
      return;
    }
    const VisibleChunk& vc = m_visible[slot];
    const auto& dr = lev.draws[di];
    if (!vc.partly || !lev.draw_run_count[di]) {
      ctr_gpu_draw_mesh_prepared(&lev.draw_states[di], &vc.gpu, lev.meshes[vc.chunk],
                                 dr.first_index, dr.index_count);
      return;
    }
    // (AI-assisted) only the visible runs; neighbouring ones in one draw, with small hidden runs
    // between them drawn along (cheaper than another draw)
    constexpr u32 kMaxGap = 48;
    u32 idx = dr.first_index, start = 0, len = 0, gap = 0, drawn = 0;
    bool open = false;
    auto emit = [&]() {
      ctr_gpu_draw_mesh_prepared(&lev.draw_states[di], &vc.gpu, lev.meshes[vc.chunk], start, len);
      drawn += len;
      open = false;
      gap = 0;
    };
    const u32 r0 = lev.draw_first_run[di];
    for (u32 r = r0; r < r0 + lev.draw_run_count[di]; r++) {
      const auto& run = lev.vis_runs[r];
      const bool shown = run.vis == 0xffff || (m_vis[run.vis >> 3] & (0x80 >> (run.vis & 7)));
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
  if (settings.early_depth) {
    for (u8 band = 0; band < 3; band++) {
      for (u32 di : lev.early_draws) {
        const int slot = m_visible_slot[lev.draw_chunk[di]];
        if (slot >= 0 && m_visible[slot].band == band) {
          draw(di);
        }
      }
    }
  } else {
    for (u32 di : lev.early_draws) {
      draw(di);
    }
  }
  for (u32 di : lev.sorted_draws) {
    draw(di);
  }
  for (u32 di : lev.ordered_draws) {
    draw(di);
  }
}
