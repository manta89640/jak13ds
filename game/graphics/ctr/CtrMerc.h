#pragma once

/*!
 * @file CtrMerc.h
 * (AI-assisted)
 * Merc (skinned characters and objects) for the 3DS renderer. Reads the same PC-port model
 * records as Merc2 (model name, lights, bone matrices, effect flags), finds the model in the
 * loaded .c3l files and draws it with GPU skinning (draws are pre-split by the converter so each
 * uses at most 24 bones). Not done: lighting (a per-model tint from the ambient + first light),
 * blend shapes, envmap, eyes, vertex modification (ripple), fog.
 */

#include "common/math/Vector.h"

#include "game/graphics/ctr/CtrRenderer.h"

class CtrLevels;

class CtrMercRenderer : public CtrBucketRenderer {
 public:
  CtrMercRenderer(std::string name, int id, CtrLevels* levels);
  void render(DmaFollower& dma, CtrRenderState& rs) override;

  struct Stats {
    int models = 0;
    int missing = 0;
    int draws = 0;
  };

 private:
  void handle_setup(const DmaTransfer& setup);
  void handle_model(const DmaTransfer& init, CtrRenderState& rs);

  CtrLevels* m_levels;
  // camera from the merc setup packet (low memory of the merc VU program)
  math::Vector4f m_hvdf_offset;
  math::Vector4f m_perspective[4];
  math::Vector4f m_fog;
  float m_clip[16];
  bool m_have_camera = false;
  Stats m_stats;
};
