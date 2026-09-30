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

#include "game/graphics/ctr/CtrRenderer.h"
#include "game/graphics/ctr/c3l_format.h"

struct CtrLevelData {
  std::string name;
  std::vector<c3l::Chunk> chunks;
  std::vector<c3l::Draw> draws;
  std::vector<int> textures;  // ctr_gpu handles
  std::vector<int> meshes;    // one per chunk
  u64 last_used_frame = 0;
};

class CtrLevels {
 public:
  ~CtrLevels();
  /*! Get a level, loading it if needed. nullptr if there is no .c3l for it. */
  CtrLevelData* get(const std::string& name, u64 frame);
  /*! Levels the game wants (set_levels): others are unloaded. */
  void set_wanted(const std::vector<std::string>& names);

 private:
  bool load(const std::string& name, CtrLevelData* out);
  void unload(CtrLevelData& lev);
  std::map<std::string, std::unique_ptr<CtrLevelData>> m_levels;
  std::map<std::string, bool> m_missing;  // no file: don't retry every frame
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
  void draw_level(CtrLevelData& lev, const CtrBackgroundCamera& cam);
  CtrLevels* m_levels;
};
