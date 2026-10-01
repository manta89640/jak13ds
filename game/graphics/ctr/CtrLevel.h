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

struct CtrMercModelData {
  std::string name;
  int mesh = -1;
  float scale = 1.f;
  std::vector<c3l::MercDraw> draws;
  // blend shapes (faces): CtrLevelData::blerc_verts[blerc_first, + blerc_count)
  u32 blerc_first = 0, blerc_count = 0;
  mutable bool blerc_moved = false;  // vertices not in the base pose (CtrMercRenderer)
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
  std::vector<u32> draw_chunk;
  std::vector<int> textures;  // ctr_gpu handles
  int tex_pool = -1;          // ctr_gpu texture pool holding all of them (VRAM when there's room)
  std::vector<int> meshes;    // one per chunk
  std::vector<c3l::MercBlercVertex> blerc_verts;
  std::vector<c3l::MercBlercTarget> blerc_targets;
  std::vector<u16> blerc_dests;
  u64 last_used_frame = 0;
  float bbox_min[3] = {0, 0, 0}, bbox_max[3] = {0, 0, 0};  // tfrag + tie, game units
  bool has_lowres = false;  // chunks with lod_tier 3
};

/*! DrawMode (tfrag3 draw settings) to ctr_gpu state. */
ctr_draw_state ctr_state_from_draw_mode(DrawMode mode, int tex);

/*!
 * The .c3l files of the levels the game has loaded. On the 3DS they are read on a loader thread
 * (core 2, below the render thread and the sound mixer): a level file takes a few hundred ms to
 * seconds to read from the SD card and build, and loading it on the render thread froze the game
 * for that long (the game waits for the render thread). A level is drawn from the frame after its
 * load finished. Elsewhere (PC tools) the loads run on the render thread, as before.
 * Threads: get/find_merc_model/process_pending_loads run on the render thread, set_wanted on the
 * game thread while the render thread is idle, prefetch on any thread.
 */
class CtrLevels {
 public:
  CtrLevels();
  ~CtrLevels();
  /*! Load the common file (GAME.c3l: Jak and other shared models), kept loaded. */
  void load_common();
  /*! Start loading a level before the game asks for it (any thread). Kept aside, not drawn, until
   * set_wanted or get asks for it; dropped if nothing does for a minute. */
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

 private:
  enum class LoadResult { LOADED, MISSING, FAILED, CANCELLED };
  LoadResult load(const std::string& name, CtrLevelData* out);
  bool load_file(const fs::path& path, const std::string& name, CtrLevelData* out, long* file_size);
  void unload(CtrLevelData& lev);
  bool cancelled() const { return m_cancel.load(std::memory_order_relaxed); }
  std::map<std::string, std::unique_ptr<CtrLevelData>> m_levels;
  std::map<std::string, bool> m_missing;
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
  struct Job {
    std::string name;
    bool common = false;
    bool prefetch = false;
  };
  struct Done {
    Job job;
    std::unique_ptr<CtrLevelData> lev;
    LoadResult result = LoadResult::FAILED;
  };
  void request(const Job& job);  // render thread
  void run_job(const Job& job);  // loader thread (or inline without one)
  void loader_main();
  static void* loader_entry(void* self);
  std::mutex m_lock;  // m_queue, m_done, m_loading, m_quit, m_prefetch_requests, m_levels_loaded
  std::condition_variable m_cv;
  std::deque<Job> m_queue;
  std::vector<Done> m_done;
  std::string m_loading;  // the level the loader works on ("" if none)
  bool m_loading_prefetch = false;  // ... for a prefetch (never cancelled: the level is coming)
  std::atomic<bool> m_cancel{false};  // m_loading is no longer wanted: stop early
  bool m_quit = false;
  std::vector<std::string> m_prefetch_requests;
  int m_levels_loaded = 0;  // m_levels + m_prefetched (a prefetch waits while it's 2 or more)
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
class CtrTfragRenderer : public CtrBucketRenderer {
 public:
  CtrTfragRenderer(std::string name, int id, CtrLevels* levels);
  void render(DmaFollower& dma, CtrRenderState& rs) override;

 private:
  void draw_level(CtrLevelData& lev, const CtrBackgroundCamera& cam, const CtrRenderState& rs);
  CtrLevels* m_levels;
  int m_far_levels = 0;  // level draws in the "seen from another level" mode (statistics)
  int m_level_draws = 0;
  // this frame's visible chunks and draws (kept to avoid allocations)
  struct VisibleChunk {
    u32 chunk;
    float clip[16];
    ctr_mesh_matrix gpu;  // clip, converted for the GPU once per frame
  };
  std::vector<VisibleChunk> m_visible;
  std::vector<int> m_visible_slot;  // per chunk: index in m_visible, -1: not drawn
};
