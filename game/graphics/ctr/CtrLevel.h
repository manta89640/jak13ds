#pragma once

/*!
 * @file CtrLevel.h
 * (AI-assisted)
 * Level backgrounds for the 3DS renderer: loads .c3l files (docs/3ds-port/c3l_format.md) from
 * <project>/out/jak1/c3l and draws them from the tfrag buckets with the camera the game sends.
 */

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "common/math/Vector.h"

#include "common/dma/gs.h"

#include "game/graphics/ctr/CtrRenderer.h"
#include "game/graphics/ctr/c3l_format.h"
#include "game/graphics/ctr/ctr_gpu.h"

struct CtrMercModelData {
  std::string name;
  int mesh = -1;
  float scale = 1.f;
  std::vector<c3l::MercDraw> draws;
};

struct CtrLevelData {
  std::string name;
  std::vector<CtrMercModelData> merc_models;
  std::vector<c3l::Chunk> chunks;
  std::vector<c3l::Draw> draws;
  std::vector<ctr_draw_state> draw_states;  // per draw
  std::vector<int> textures;  // ctr_gpu handles
  std::vector<int> meshes;    // one per chunk
  u64 last_used_frame = 0;
  float bbox_min[3] = {0, 0, 0}, bbox_max[3] = {0, 0, 0};  // tfrag + tie, game units
  bool has_lowres = false;  // chunks with lod_tier 3
};

/*! DrawMode (tfrag3 draw settings) to ctr_gpu state. */
ctr_draw_state ctr_state_from_draw_mode(DrawMode mode, int tex);

class CtrLevels {
 public:
  ~CtrLevels();
  /*! Load the common file (GAME.c3l: Jak and other shared models), kept loaded. */
  void load_common();
  /*! Find a merc model in the loaded levels. Returns the level too (for its textures). */
  const CtrMercModelData* find_merc_model(const std::string& name, const CtrLevelData** lev);
  /*! Get a level, loading it if needed. nullptr if there is no .c3l for it. */
  CtrLevelData* get(const std::string& name, u64 frame);
  /*! Load the levels get() was asked for (call outside of a GPU frame). */
  void process_pending_loads(u64 frame);
  /*! Levels the game wants (set_levels): others are unloaded. */
  void set_wanted(const std::vector<std::string>& names);

 private:
  bool load(const std::string& name, CtrLevelData* out);
  void unload(CtrLevelData& lev);
  std::map<std::string, std::unique_ptr<CtrLevelData>> m_levels;
  std::map<std::string, bool> m_missing;
  std::vector<std::string> m_pending_loads;  // no file: don't retry every frame
  std::unique_ptr<CtrLevelData> m_common;
  // merc model name -> (level, model index); rebuilt when levels change
  std::map<std::string, std::pair<CtrLevelData*, int>> m_merc_index;
  void rebuild_merc_index();
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
};
