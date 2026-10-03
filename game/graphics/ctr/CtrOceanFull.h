#pragma once

/*!
 * @file CtrOceanFull.h
 * (AI-assisted)
 * The PS2 ocean on the 3DS (config.ini ocean = full; the default, flat, is CtrOcean.h).
 *
 * The game runs draw-ocean (drawable.gc: *pc-3ds-ocean-full*, set by the kernel from config.ini)
 * instead of draw-ocean-3ds, so the two ocean buckets get the PS2's DMA, which this renders like
 * the PC renderer (game/graphics/opengl_renderer/ocean):
 *  - the wave texture (OceanTexture): the ocean-texture VU program's math on the CPU for the 33x33
 *    vertices of *ocean-verts* (color, and the env map lookup from the wave normals), then 64x64
 *    texels (the PS2's 128x128 of 4x4 texel quads at half the resolution), each the env map
 *    texel at the interpolated lookup times the interpolated color (GS modulate). One texture with
 *    mip levels for both buckets, computed once per frame.
 *  - ocean-mid-and-far bucket (before the level, depth test off, depth written like the PC's
 *    flush_mid): the far ocean (GS packets, OceanMidAndFar::handle_ocean_far), then the mid ocean
 *    and the transition to the near ocean: the ocean-mid VU program, emulated by the PC's code
 *    (OceanMid_PS2.cpp, see CtrOceanVu.h).
 *  - ocean-near bucket (after the level, depth tested, alpha blended): the near ocean's waves, the
 *    ocean-near VU program (OceanNear_PS2.cpp).
 * The VU programs' GS packets become one indexed triangle list per bucket, written straight to the
 * frame's vertex memory (ctr_gpu_ocean_full_reserve / draw, ctr_ocean_full.v.pica: the GS's S, T, Q
 * perspective, the GS fog value). Only the textured pass is drawn: the PS2's env map pass (sky
 * reflection added through the destination alpha) is left out (the wave texture already has the
 * env map's shading).
 *
 * Log: "[ctr] ocean full:" once per level (set of levels), the render thread's ms per frame.
 */

#include <memory>
#include <string>
#include <vector>

#include "common/common_types.h"

#include "game/graphics/ctr/CtrRenderer.h"

class CtrOceanMidVu;
class CtrOceanNearVu;
class CtrOceanBatch;

class CtrOceanFull {
 public:
  CtrOceanFull();
  ~CtrOceanFull();
  void render_mid_and_far(DmaFollower& dma, CtrRenderState& rs);
  void render_near(DmaFollower& dma, CtrRenderState& rs);
  //! the game's levels changed (ctr_set_levels, render thread idle): log again for these
  static void note_levels(const std::vector<std::string>& levels);

 private:
  struct TextureDma;
  bool read_texture_dma(DmaFollower& dma, TextureDma* out);
  void update_texture(const TextureDma& t, CtrRenderState& rs);
  bool update_env(u64 tex0, CtrRenderState& rs);
  void read_far(DmaFollower& dma);
  void frame_stats(const CtrRenderState& rs);

  std::unique_ptr<CtrOceanMidVu> m_mid;
  std::unique_ptr<CtrOceanNearVu> m_near;
  std::unique_ptr<CtrOceanBatch> m_batch;

  // the wave texture
  int m_tex = -1;
  u64 m_tex_frame = ~0ull;
  std::vector<u32> m_texels;
  // the env map texture it is made from (decoded from CtrVram)
  u64 m_env_key = 0;
  u64 m_env_frame = 0;
  bool m_env_ok = false;
  int m_env_w = 0, m_env_h = 0;
  std::vector<u32> m_env;
  std::vector<u64> m_env_logged;

  // statistics: ms on the render thread, per frame
  u64 m_stat_frame = ~0ull;
  int m_frames = 0;
  double m_tex_ms = 0, m_mid_ms = 0, m_near_ms = 0, m_max_frame_ms = 0, m_cur_frame_ms = 0;
  int m_mid_runs = 0, m_near_runs = 0, m_failed = 0;
  u32 m_window_gen = 0, m_logged_gen = ~0u;
};

/*! The two ocean buckets (ocean-mid-and-far, ocean-near), sharing one CtrOceanFull. */
class CtrOceanFullBucket : public CtrBucketRenderer {
 public:
  CtrOceanFullBucket(std::string name, int id, std::shared_ptr<CtrOceanFull> ocean, bool near)
      : CtrBucketRenderer(std::move(name), id), m_ocean(std::move(ocean)), m_is_near(near) {}
  void render(DmaFollower& dma, CtrRenderState& rs) override {
    if (m_is_near) {
      m_ocean->render_near(dma, rs);
    } else {
      m_ocean->render_mid_and_far(dma, rs);
    }
  }

 private:
  std::shared_ptr<CtrOceanFull> m_ocean;
  bool m_is_near;
};
