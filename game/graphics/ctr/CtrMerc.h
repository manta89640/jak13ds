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

#include <vector>

#include "common/math/Vector.h"

#include "game/graphics/ctr/CtrRenderer.h"

class CtrLevels;

struct CtrMercMat {
  math::Vector4f tmat[4];
  math::Vector4f nmat[3];
};

class CtrMercRenderer : public CtrBucketRenderer {
 public:
  CtrMercRenderer(std::string name, int id, CtrLevels* levels);
  void render(DmaFollower& dma, CtrRenderState& rs) override;
  /*! Game thread, before the frame is handed to the render thread: copy the bone matrices the
   * bucket's models use (see handle_model). */
  void snapshot_bones(DmaFollower& dma, CtrRenderState& rs);

  struct Stats {
    int models = 0;
    int missing = 0;
    int draws = 0;
    int bad_bones = 0;
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
  std::vector<CtrMercMat> m_snap;
  size_t m_snap_pos = 0, m_snap_first = 0;
  bool m_snap_valid = false;
  bool m_snapshot_mode = false;
};
