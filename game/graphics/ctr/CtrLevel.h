#pragma once

/*!
 * @file CtrLevel.h
 * (AI-assisted)
 * Level backgrounds for the 3DS renderer: loads .c3l files (docs/3ds-port/c3l_format.md) from
 * <project>/out/jak1/c3l and draws them from the tfrag buckets with the camera the game sends.
 */

#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "common/math/Vector.h"

#include "common/dma/gs.h"
#include "common/util/FileUtil.h"

#include "game/graphics/ctr/CtrRenderer.h"
#include "game/graphics/ctr/c3l_format.h"
#include "game/graphics/ctr/ctr_gpu.h"

/*! (AI-assisted) c3l v12: an effect whose vertices the game rewrites (c3l::MercModEffect). */
struct CtrMercModEffect {
  u8 effect = 0;
  std::vector<u8> mask;  // per fragment: unpacked or skipped
  u32 vidx_end = 0;
  std::vector<c3l::MercModDest> dests;  // by model vertex
  float pos_k = 1.f;     // xyz_scale / model scale: unpacked position -> MercVertex::pos
  s16 st_lut[256] = {};  // lump byte -> MercVertex::st (* 1024)
};

struct CtrMercModelData {
  std::string name;
  int mesh = -1;
  float scale = 1.f;
  std::vector<c3l::MercDraw> draws;
  // (AI-assisted) the converter's generated low LOD (models with only lod0): the same vertices,
  // fewer triangles; empty: none. CtrMercRenderer draws them far away (config merc_gen_lod)
  std::vector<c3l::MercDraw> gen_draws;
  // blend shapes (faces): CtrLevelData::blerc_verts[blerc_first, + blerc_count)
  u32 blerc_first = 0, blerc_count = 0;
  mutable bool blerc_moved = false;  // vertices not in the base pose (CtrMercRenderer)
  std::vector<CtrMercModEffect> mod;  // (AI-assisted) v12, see CtrMercRenderer::apply_mod
  mutable bool mod_checked = false;   // the first update was compared with the file's vertices
  // (AI-assisted) where the mesh's data is in the file, to make it again after a load that ran out
  // of memory: first MercVertex and count, merc index range (index_end 0: not possible)
  u32 file_vertex = 0, vertex_count = 0, index_first = 0, index_end = 0;
};

struct CtrLevelData {
  std::string name;
  std::vector<CtrMercModelData> merc_models;
  std::vector<c3l::Chunk> chunks;
  std::vector<c3l::Draw> draws;
  std::vector<ctr_draw_state> draw_states;  // per draw
  // per draw: sort key for the draws whose order doesn't matter (texture, then GPU state), and
  // whether the order matters (blending, or no depth write: decals); see CtrTfragRenderer
  std::vector<u32> draw_sort_keys;
  std::vector<u8> draw_ordered;
  // made at load: the draws in drawing order, so a frame needs no sort. sorted_draws: the draws
  // whose order doesn't matter by (sort key, chunk, draw), then ordered_draws in chunk order.
  // draw_chunk: the chunk of each draw.
  std::vector<u32> sorted_draws;
  std::vector<u32> ordered_draws;
  // (AI-assisted) the opaque draws that may use the early depth test (CTR_STATE_EARLY_DEPTH),
  // sorted like sorted_draws; drawn first, nearest chunks first (draw_level)
  std::vector<u32> early_draws;
  std::vector<u32> draw_chunk;
  // (AI-assisted) c3l v10 visibility runs (c3l::VisRun): all of them, the first of each chunk,
  // and the range of each draw
  std::vector<c3l::VisRun> vis_runs;
  std::vector<u32> chunk_first_run;
  std::vector<u32> draw_first_run;
  std::vector<u16> draw_run_count;
  // (AI-assisted) config.ini run_culling: each run's bounding sphere, made at load from the
  // chunk's vertices, in the chunk's quantized units (world = origin + c * scale, radius r *
  // scale); r = 0xffff: unknown (the mesh wasn't made at load), never hidden. Chunks without
  // visibility runs get runs made at load for this (their draws in pieces, vis 0xffff).
  // chunk_run_count: all of a chunk's runs (Chunk::vis_run_count: only the file's).
  struct RunSphere {
    s16 c[3];
    u16 r;
  };
  std::vector<RunSphere> run_spheres;
  std::vector<u32> chunk_run_count;
  std::vector<int> textures;  // ctr_gpu handles
  std::vector<u8> tex_min_alpha;  // (AI-assisted) smallest texel alpha per texture (0xff = 1.0)
  int tex_pool = -1;          // ctr_gpu texture pool holding all of them (VRAM when there's room)
  u32 tex_pool_bytes = 0;     // (AI-assisted) its size (0: no textures)
  u32 tex_background_bytes = 0;  // (AI-assisted) its start: the background's textures (VRAM first)
  std::vector<int> meshes;    // one per chunk
  std::vector<c3l::MercBlercVertex> blerc_verts;
  std::vector<c3l::MercBlercTarget> blerc_targets;
  std::vector<u16> blerc_dests;
  u64 last_used_frame = 0;
  // (AI-assisted) the frame the background was drawn: the game sends a level's camera with each
  // of its tfrag trees (normal, trans, dirt, ice buckets); the first one draws all of it
  u64 drawn_frame = ~0ull;
  float bbox_min[3] = {0, 0, 0}, bbox_max[3] = {0, 0, 0};  // tfrag + tie, game units
  bool has_lowres = false;  // chunks with lod_tier 3
  // (AI-assisted) the bounds of the detailed tfrag (tier 1, or tier 0 tfrag in single tier files):
  // where the level's low resolution tfrag isn't drawn in near mode (config lowres_vista)
  float detail_min[3] = {0, 0, 0}, detail_max[3] = {0, 0, 0};
  bool has_detail = false;
  bool has_vista = false;  // (AI-assisted) c3l v13 vista tier chunks (CHUNK_KIND_VISTA)
  // (AI-assisted) partial unload: a level only seen from a neighbouring one (far mode: only its
  // far chunks are drawn) frees the meshes of its other chunks after a while (linear memory for
  // the level the camera is in), and reads them again from its file when the camera comes back
  // (CtrLevels::unload_detail / request_detail). Until then it is drawn in far mode.
  std::string path;  // the .c3l file
  u32 vertex_data_offset = 0, index_data_offset = 0;
  u32 merc_vertex_offset = 0, merc_index_offset = 0;  // (AI-assisted) for incomplete loads
  std::vector<u32> mesh_index_first, mesh_index_end;  // per chunk: its index range in the file
  bool detail_loaded = true;      // the meshes of all chunks are there
  bool detail_requested = false;  // their reload is queued
  u32 far_frames = 0;             // frames in a row beyond the unload distance
  u64 detail_retry_frame = 0;     // after a failed reload (no memory): not before this frame
  // (AI-assisted) the load ran out of linear memory even after making room: some chunk or merc
  // meshes (or the texture pool) are missing. They are made again from the file once the memory
  // is there (CtrLevels::process_pending_loads, request_repair), while the level is drawn.
  bool incomplete = false;
  bool repair_requested = false;
  u64 repair_retry_frame = 0;  // after a repair that left parts out: not before this frame
  u8 repair_failures = 0;      // repairs in a row that made nothing (longer waits)
  // (AI-assisted) last frame's decisions, for hysteresis (CtrTfragRenderer::draw_level): per chunk
  // bits 0-1 detailed/coarse (1 near, 2 far, 0 not decided yet), bit 2 within its draw distance
  std::vector<u8> chunk_state;
  bool was_far = false;
  u32 generation = 0;             // tells loads of the same level apart
};

/*! DrawMode (tfrag3 draw settings) to ctr_gpu state. */
ctr_draw_state ctr_state_from_draw_mode(DrawMode mode, int tex);

namespace c3l {
class Reader;
}

/*!
 * The .c3l files of the levels the game has loaded. On the 3DS they are read on a loader thread
 * (core 2, below the render thread and the sound mixer): a level file takes a few hundred ms to
 * seconds to read from the SD card and build, and loading it on the render thread froze the game
 * for that long (the game waits for the render thread). A level is drawn from the frame after its
 * load finished. Elsewhere (PC tools) the loads run on the render thread, as before.
 * Threads: get/find_merc_model/process_pending_loads run on the render thread, set_wanted and
 * service on the game thread while the render thread is idle, prefetch on any thread.
 */
class CtrLevels {
 public:
  CtrLevels();
  ~CtrLevels();
  /*! Load the common file (GAME.c3l: Jak and other shared models), kept loaded. */
  void load_common();
  /*! Start loading a level before the game asks for it (any thread): the overlord started its DGO.
   * Kept aside, not drawn, until set_wanted or get asks for it. (AI-assisted) The game loads one
   * level at a time: a prefetch of any other level it doesn't want is dropped (it gave up on that
   * one), the one of this level after a minute if nothing asks for it. */
  void prefetch(const std::string& name);
  /*! Find a merc model in the loaded levels. Returns the level too (for its textures). */
  const CtrMercModelData* find_merc_model(const std::string& name, const CtrLevelData** lev);
  /*! Get a level, starting its load if needed. nullptr while it loads, or if there is no .c3l. */
  CtrLevelData* get(const std::string& name, u64 frame);
  /*! Start the loads get()/set_wanted asked for and take the finished ones (render thread, outside
   * of a GPU frame). Doesn't wait for loads on the 3DS. */
  void process_pending_loads(u64 frame);
  /*! Levels the game wants (set_levels): others are unloaded. */
  void set_wanted(const std::vector<std::string>& names);
  /*! The level's .c3l is loaded (or failed, or there is none, or no load is pending for it):
   * nothing to wait for (any thread). The game waits for this before a level starts. */
  bool ready(const std::string& name);
  /*! (AI-assisted) The levels being drawn (render thread). */
  void loaded(std::vector<const CtrLevelData*>* out) const {
    out->clear();
    for (const auto& [name, lev] : m_levels) {
      out->push_back(lev.get());
    }
  }
  /*! Partial unload (render thread): free the meshes a far level doesn't draw / read them again. */
  void unload_detail(CtrLevelData& lev);
  void request_detail(CtrLevelData& lev);
  /*! (AI-assisted) What the render thread does at the start of each frame for the loader: a
   * parked prefetch the game gave up on to unload, or the loader waiting for memory (make_room).
   * The game can wait for a level without drawing frames (both level slots empty: it loads in a
   * loop), so it does this itself then: service on the game thread while the render thread is
   * idle, when needs_service (any thread). */
  bool needs_service();
  void service();

 private:
  enum class LoadResult { LOADED, MISSING, FAILED, CANCELLED };
  LoadResult load(const std::string& name, CtrLevelData* out);
  // (AI-assisted) decode: time spent decompressing (c3l v11), 0 for older files
  bool load_file(const fs::path& path,
                 const std::string& name,
                 CtrLevelData* out,
                 long* file_size,
                 double* decode_ms);
  void unload(CtrLevelData& lev);
  bool cancelled() const { return m_cancel.load(std::memory_order_relaxed); }
  std::map<std::string, std::unique_ptr<CtrLevelData>> m_levels;
  std::map<std::string, bool> m_missing;
  std::map<std::string, double> m_failed;  // (AI-assisted) when a load failed: retried later
  std::vector<std::string> m_pending_loads;
  std::vector<std::string> m_wanted;
  bool m_common_wanted = false;
  std::unique_ptr<CtrLevelData> m_common;
  // merc model name -> (level, model index); rebuilt when levels change
  std::map<std::string, std::pair<CtrLevelData*, int>> m_merc_index;
  void rebuild_merc_index();
  // a level that loaded (or was prefetched) and is taken into m_levels
  void publish(const std::string& name, std::unique_ptr<CtrLevelData> lev, u64 frame);

  // ---- loader ----
  // (AI-assisted) a chunk mesh to read again (partial unload, repair)
  struct DetailChunk {
    u32 chunk;
    u32 first_vertex, vertex_count;
    u32 index_first, index_end;
  };
  // (AI-assisted) a merc model mesh to make again (repair)
  struct RepairMerc {
    u32 model;
    u32 file_vertex, vertex_count;
    u32 index_first, index_end;
  };
  struct Job {
    std::string name;
    bool common = false;
    bool prefetch = false;
    // detail reload (request_detail) or (AI-assisted) repair of an incomplete load
    // (request_repair): only these parts of the loaded level `generation`
    bool detail = false;
    bool repair = false;
    u32 generation = 0;
    std::string path;
    u32 vertex_data_offset = 0, index_data_offset = 0;
    std::vector<DetailChunk> chunks;  // in file order
    u32 merc_vertex_offset = 0, merc_index_offset = 0;
    std::vector<RepairMerc> merc;  // in file order
    bool textures = false;         // the texture pool
    bool parts() const { return detail || repair; }
    // in m_requested
    std::string key() const {
      return detail ? name + "#detail" : (repair ? name + "#repair" : name);
    }
  };
  // (AI-assisted) a level's texture pool and textures (load_textures)
  struct TexLoad {
    int pool = -1;
    u32 pool_bytes = 0;
    u32 background_bytes = 0;  // the pool's start: the background's textures
    std::vector<int> handles;
    std::vector<u8> min_alpha;
  };
  struct Done {
    Job job;
    std::unique_ptr<CtrLevelData> lev;
    LoadResult result = LoadResult::FAILED;
    std::vector<std::pair<u32, int>> meshes;       // detail reload, repair: (chunk, mesh)
    std::vector<std::pair<u32, int>> merc_meshes;  // repair: (merc model, mesh)
    TexLoad tex;                                   // repair: the texture pool
  };
  void request(const Job& job);  // render thread
  void run_job(const Job& job);  // loader thread (or inline without one)
  void run_parts_job(const Job& job, Done* d);
  bool load_textures(c3l::Reader& f,
                     const c3l::Header& hdr,
                     const std::vector<c3l::Texture>& texs,
                     const std::vector<c3l::Draw>& draws,
                     const std::string& name,
                     TexLoad* out);
  // (AI-assisted) render thread: the results of a detail reload / repair
  void take_parts(Done& d, u64 frame);
  static void delete_parts(Done& d);
  static void delete_textures(TexLoad* t);
  void collect_chunks(const CtrLevelData& lev, bool missing_only, std::vector<DetailChunk>* out);
  void request_repair(CtrLevelData& lev);
  // (AI-assisted) render thread (or service): unload the parked prefetches the game gave up on;
  // room: free the deleted GPU memory now too (make_room). Returns whether a level was unloaded.
  bool housekeeping(bool room);
  // (AI-assisted) loader: an allocation failed. Once per job: has the render thread unload the
  // parked prefetches the game gave up on and free the deleted GPU memory now, waits for it, and
  // returns true (try the allocation again).
  bool make_room(const std::string& name, const char* what);
  bool m_room_tried = false;  // by the loader's job (one at a time)
  // (AI-assisted) the last load_file's file reads (ms, count): for the "loaded" log line
  double m_last_read_ms = 0;
  u32 m_last_reads = 0;
  u64 m_last_read_bytes = 0;
  // under m_lock: the loader asks for room / the number of times the render thread made it
  bool m_room_wanted = false;
  u32 m_room_serial = 0;
  bool can_start(const Job& job) const;  // under m_lock
  std::atomic<u32> m_generation{0};
  void loader_main();
  static void* loader_entry(void* self);
  // m_queue, m_done, m_loading, m_quit, m_levels_count, m_parked, m_resident, m_requested,
  // m_dgo_level, m_room_*
  std::mutex m_lock;
  std::condition_variable m_cv;
  std::deque<Job> m_queue;
  std::vector<Done> m_done;
  std::string m_loading;  // the level the loader works on ("" if none)
  bool m_loading_prefetch = false;  // ... for a prefetch (cancelled when the game loads another)
  std::atomic<bool> m_cancel{false};  // m_loading is no longer wanted: stop early
  bool m_quit = false;
  // (AI-assisted) the level whose DGO the overlord loads (the last prefetch): parked prefetches of
  // other levels are unloaded, the game gave up on them (Jak died while the next level loaded)
  std::string m_dgo_level;
  // (AI-assisted) levels ready() said yes to (when): the game reports them at the end of that
  // frame (set_levels), and may start the next level's DGO before. Kept until set_wanted, at most
  // a few seconds.
  std::map<std::string, double> m_promised;
  bool parked_matters(const std::string& name, double now) const;  // under m_lock
  // for the loader (can_start): m_levels.size(), the names in m_prefetched
  int m_levels_count = 0;
  std::set<std::string> m_parked;
  std::atomic<u64> m_render_frame{0};  // the render thread's frame (process_pending_loads)
  std::set<std::string> m_resident;  // names in m_levels + m_prefetched (a prefetch skips them)
  void* m_thread = nullptr;
  std::set<std::string> m_requested;  // queued, loading or done but not taken (under m_lock)
  struct Prefetched {
    std::unique_ptr<CtrLevelData> lev;
    double since_ms = 0;  // when the load finished (ctr_gpu_time_ms)
  };
  std::map<std::string, Prefetched> m_prefetched;
  void update_level_count();
};

/*! The camera the game sends to the background renderers (GoalBackgroundCameraData). */
struct CtrBackgroundCamera {
  math::Vector4f planes[4];
  math::Vector<s32, 4> itimes[4];
  math::Vector4f camera[4];
  math::Vector4f hvdf_off;
  math::Vector4f fog;
  math::Vector4f trans;
  math::Vector4f rot[4];
  math::Vector4f perspective[4];
};
static_assert(sizeof(CtrBackgroundCamera) == 16 * 23);

/*! TFRAG_LEVEL0/1 buckets: draws the whole level background (tfrag + tie) from the .c3l. */
/*! (AI-assisted) config.ini auto_detail: where the shrub / tie distances are between their near
 * (0) and far (1) values (render thread). */
void ctr_level_set_detail(float t);
float ctr_level_detail();

class CtrTfragRenderer : public CtrBucketRenderer {
 public:
  CtrTfragRenderer(std::string name, int id, CtrLevels* levels);
  void render(DmaFollower& dma, CtrRenderState& rs) override;

 private:
  void draw_level(CtrLevelData& lev, const CtrBackgroundCamera& cam, const CtrRenderState& rs);
  CtrLevels* m_levels;
  int m_far_levels = 0;  // level draws in the "seen from another level" mode (statistics)
  int m_level_draws = 0;
  // (AI-assisted) the game's visibility string for the level being drawn (nullptr: none), and
  // what it hid (statistics)
  const u8* m_vis = nullptr;
  u64 m_vis_chunks = 0, m_vis_tris = 0, m_vis_frames = 0, m_vis_own_frames = 0;
  // (AI-assisted) run_culling: per run of the level being drawn, shown this frame (filled for the
  // chunks tested run by run); statistics: runs tested, triangles and chunks the view test hid
  std::vector<u8> m_run_shown;
  u64 m_cull_runs = 0, m_cull_tris = 0, m_cull_chunks = 0, m_cull_frames = 0;
  // (AI-assisted) lowres_vista / neighbour_vis: low resolution chunks drawn in near mode, and the
  // vista pass's chunks and triangles (statistics); the levels' detailed areas (per level draw)
  u64 m_lowres_near = 0, m_vista_chunks = 0, m_vista_tris = 0, m_vista_frames = 0;
  std::vector<const CtrLevelData*> m_loaded;
  // (AI-assisted) vista tier: per chunk, its vista copies are drawn this frame (it isn't, because
  // of its distance)
  std::vector<u8> m_vista_src;
  // this frame's visible chunks and draws (kept to avoid allocations)
  struct VisibleChunk {
    u32 chunk;
    u8 band;    // (AI-assisted) distance band for the early depth order: 0 near .. 2 far
    bool partly;  // (AI-assisted) some of its visibility runs are hidden: draw run by run
    bool vista;   // (AI-assisted) far scenery: drawn first, with the vista fog (lowres_vista)
    bool nofog;   // (AI-assisted) in front of where both fogs start (fog_stage_skip)
    float clip[16];
    ctr_mesh_matrix gpu;  // clip, converted for the GPU once per frame
  };
  std::vector<VisibleChunk> m_visible;
  std::vector<int> m_visible_slot;  // per chunk: index in m_visible, -1: not drawn
};
