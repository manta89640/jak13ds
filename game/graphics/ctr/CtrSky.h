#pragma once

/*!
 * @file CtrSky.h
 * (AI-assisted)
 * The sky for the 3DS renderer, like the PC port's SkyBlendCPU + SkyRenderer, without the GS
 * emulation:
 * - make-sky-textures blends the level's 8 time of day sky textures and its cloud texture into a
 *   32x32 sky and a 64x64 cloud texture (sprites into VRAM, in the tfrag-trans/sky-blend buckets).
 *   CtrSkyBlendRenderer does that blend on the CPU (SkyBlendCPU's math) into two textures.
 * - On the 3DS the game doesn't run render-sky-tng (its polygon clipping cost ~1.5 ms per frame):
 *   render-sky-3ds sends the camera, the cloud scroll offsets and where render-sky-tng's polygons
 *   are, and CtrSky::draw draws the same polygons, clipped by the GPU (ctr_gpu_draw_clip).
 */

#include <vector>

#include "common/common_types.h"

#include "game/graphics/ctr/CtrRenderer.h"

class CtrVram;

class CtrSky {
 public:
  ~CtrSky();
  /*! The sky copies of make-sky-textures (pairs of 6 qw transfers). */
  void blend(DmaFollower& dma, CtrRenderState& rs);
  /*! render-sky-3ds's packet (8 qw, see there). */
  void draw(const u8* packet, CtrRenderState& rs);
  static constexpr int kPacketBytes = 8 * 16;

 private:
  static constexpr int kSize[2] = {32, 64};  // sky, clouds
  std::vector<u8> m_rgba[2];
  bool m_dirty[2] = {false, false};
  bool m_valid[2] = {false, false};
  int m_tex[2] = {-1, -1};
  std::vector<u32> m_decode;
  std::vector<u64> m_logged;  // source textures already logged
  struct Stats {
    int blends = 0, draws = 0;
  } m_stats;
};

/*! The tfrag-trans + sky blend buckets: the sky blend (tfrag trans comes from the .c3l files). */
class CtrSkyBlendRenderer : public CtrBucketRenderer {
 public:
  CtrSkyBlendRenderer(std::string name, int id, CtrSky* sky)
      : CtrBucketRenderer(std::move(name), id), m_sky(sky) {}
  void render(DmaFollower& dma, CtrRenderState& rs) override;

 private:
  CtrSky* m_sky;
};
