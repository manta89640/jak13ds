#pragma once

/*!
 * @file CtrOcean.h
 * Jak II's ocean sky reflection, shared by CtrOceanFull (the PS2 ocean, CtrOceanFull.h).
 */

#include "common/common_types.h"

#include "game/graphics/ctr/CtrRenderer.h"

class CtrDirect;

/*!
 * Jak II's ocean env map, part 1: ocean-method-89's first pass (the sky color, the sun and moon
 * glows, the haze and the lit clouds, seen from above), drawn into a 64 x 64 render target.
 */
class CtrOceanEnvJak2 {
 public:
  /*!
   * The first pass of ocean-method-89 from dma into its render target, as long as the packets
   * belong to it: up to the packet that sets the next 64-wide frame (the PS2's warp) or a frame of
   * another width (the wave texture's set-display-gs-state, left unread).
   */
  static void draw_first_pass(CtrDirect* direct, DmaFollower& dma, CtrRenderState& rs);
};

/*!
 * Jak II's ocean env map, part 2 (ocean-method-83's warp, see update): a 64 x 64 render target,
 * remade when its source or env-color changes.
 */
class CtrOceanEnvWarpJak2 {
 public:
  ~CtrOceanEnvWarpJak2();
  //! ec: env-color (GS units); sky: the first pass is the source, else the texture static_tex0
  //! (environment-ocean). false: no env map this frame.
  bool update(const float ec[3], bool sky, u64 static_tex0, const CtrRenderState& rs);
  //! a first pass was drawn in the last few frames
  static bool first_pass_fresh(const CtrRenderState& rs);
  int rt() const { return m_rt; }
  //! s, t at logical (0, 0); per unit of u; per unit of v (u, v: the PS2's env map coordinates)
  const float* uv() const { return m_uv; }

 private:
  int m_rt = -1;
  float m_uv[6] = {0, 0, 0, 0, 0, 0};
  u64 m_made_from = ~0ull;  // the first pass's frame it was warped from
  u64 m_static = 0;         // the environment-ocean tex0 it was copied from (no sky)
  float m_color[3] = {-1, -1, -1};
};
