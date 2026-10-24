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
 *    mip levels for both buckets, computed at most once per frame ((AI-assisted) every
 *    config.ini ocean_texture_rate frames: 2 by default).
 *  - ocean-mid-and-far bucket (before the level, depth test off, depth written like the PC's
 *    flush_mid): the far ocean (GS packets, OceanMidAndFar::handle_ocean_far), then the mid ocean
 *    and the transition to the near ocean: the ocean-mid VU program's drawing calls as native
 *    code (CtrOceanNative.cpp; the PC's emulation, OceanMid_PS2.cpp via CtrOceanVu.h, for the
 *    other calls and the host replay test).
 *  - ocean-near bucket (after the level, depth tested, alpha blended): the near ocean's waves, the
 *    ocean-near VU program's call 39 (CtrOceanNative.cpp; OceanNear_PS2.cpp for the rest).
 * The GS packets become indexed triangle lists written straight to the frame's vertex memory
 * (ctr_gpu_ocean_full_reserve / draw, ctr_ocean_full.v.pica: the GS's S, T, Q perspective, the GS
 * fog value); the native programs' vertices are in clip space, unclipped (draw_clip,
 * ctr_ocean_full_clip.v.pica: the GPU clips).
 * (AI-assisted, j1ocean) The sky reflection: the PS2 draws an env map pass after the textured one
 * where the programs say so (mid tiles of call 73, the transitions, the near patches; not the far
 * ocean or call 46's tiles): environment-ocean times the time of day's env-color, at the view
 * direction's horizontal part, added with the destination alpha as its weight. That alpha is the
 * wave texture's alpha (environment-ocean-alphamod's) times the vertex alpha: written by the mid
 * ocean's opaque textured pass, and by the near ocean's alpha-only pass (CommonOceanRenderer's
 * three buckets). The 3DS draws all of it in the textured pass's draw (ctr_gpu_ocean_full_draw_env):
 * the env map times env-color is made here once per change (ocean_env), the native programs give
 * each vertex the pass's texture coordinates, and the combiner adds env * weight after the fog.
 * Mid: fog(tex * v) + env * tex.a * v.a, as the PS2's two passes; near: alpha blended with v.a,
 * fog(tex * v) + env * tex.a, = the PS2's three passes (v.a * fog(tex * v) + (1 - v.a) * dest, then
 * + env * v.a * tex.a) but for where the sum reaches white (the GS saturates once at the end).
 * config.ini ocean_near_alpha (default 0: the PS2's own alpha) is a floor on the near alpha.
 *
 * Log: "[ctr] ocean full:" once per level (set of levels), the render thread's ms per frame;
 * (AI-assisted) a warning when parts weren't drawn (at most every 300 frames, see frame_stats).
 */

#include <memory>
#include <string>
#include <vector>

#include "common/common_types.h"

#include "game/graphics/ctr/CtrOceanVu.h"
#include "game/graphics/ctr/CtrRenderer.h"

class CtrOceanMidVu;
class CtrOceanNearVu;
class CtrOceanBatch;

class CtrOceanFull : public CtrOceanEnvHook {
 public:
  CtrOceanFull();
  ~CtrOceanFull();
  void render_mid_and_far(DmaFollower& dma, CtrRenderState& rs);
  void render_near(DmaFollower& dma, CtrRenderState& rs);
  //! the game's levels changed (ctr_set_levels, render thread idle): log again for these
  static void note_levels(const std::vector<std::string>& levels);
  //! the native ocean programs (default) or the emulated VU programs (host replay test)
  void set_native(bool on);
  //! (AI-assisted, j1ocean) CtrOceanEnvHook: the sky reflection's texture for this bucket's draws
  void ocean_env(u64 tex0, const float color[4]) override;

 private:
  struct TextureDma;
  bool read_texture_dma(DmaFollower& dma, TextureDma* out);
  void update_texture(const TextureDma& t, CtrRenderState& rs);
  bool update_env(u64 tex0, CtrRenderState& rs);
  void read_far(DmaFollower& dma);
  void frame_stats(const CtrRenderState& rs);
  bool texture_due(const CtrRenderState& rs) const;

  std::unique_ptr<CtrOceanMidVu> m_mid;
  std::unique_ptr<CtrOceanNearVu> m_near;
  std::unique_ptr<CtrOceanBatch> m_batch;

  // the wave texture
  int m_tex = -1;
  u64 m_tex_frame = ~0ull;  // the frame it was last made in
  int m_tex_rate = 1;       // (AI-assisted) config.ini ocean_texture_rate
  std::vector<u32> m_texels;
  // the env map texture it is made from (decoded from CtrVram)
  u64 m_env_key = 0;
  u64 m_env_frame = 0;
  bool m_env_ok = false;
  int m_env_w = 0, m_env_h = 0;
  std::vector<u32> m_env;
  std::vector<u64> m_env_logged;

  // (AI-assisted, j1ocean) the sky reflection: environment-ocean (decoded) times env-color
  CtrRenderState* m_rs = nullptr;  // the bucket being drawn (ocean_env)
  int m_refl = -1, m_refl_w = 0, m_refl_h = 0;
  u64 m_refl_key = 0, m_refl_frame = ~0ull, m_refl_made = ~0ull;
  u32 m_refl_color = ~0u;
  bool m_refl_ok = false;
  std::vector<u32> m_refl_src, m_refl_texels;
  std::vector<u64> m_refl_logged;

  // statistics: ms on the render thread, per frame
  u64 m_stat_frame = ~0ull;
  int m_frames = 0;
  double m_tex_ms = 0, m_mid_ms = 0, m_near_ms = 0, m_max_frame_ms = 0, m_cur_frame_ms = 0;
  int m_mid_runs = 0, m_near_runs = 0, m_failed = 0, m_tex_updates = 0;
  int m_refl_updates = 0;  // (AI-assisted, j1ocean) sky reflection textures made
  u32 m_window_gen = 0, m_logged_gen = ~0u;
  // (AI-assisted) the warning about parts not drawn (frame_stats)
  int m_failed_unlogged = 0;
  u64 m_warn_frame = ~0ull;
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
