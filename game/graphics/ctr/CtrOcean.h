#pragma once

/*!
 * @file CtrOcean.h
 * (AI-assisted)
 * The ocean (Jak 1's "infinite water") for the 3DS renderer.
 *
 * The PS2 ocean (draw-ocean: far / mid / transition / near VU1 programs plus a wave texture
 * rendered every frame) is too slow on the 3DS, so with SKIP_OCEAN_DRAW the game runs
 * draw-ocean-3ds instead (goal_src/jak1/engine/gfx/ocean/ocean.gc). It puts one packet in the
 * ocean-near bucket with the ocean map (height, colors, masks of where there is water), this
 * frame's wave heights and the camera.
 *
 * Here:
 *  - the ocean map becomes one static mesh, built when the map changes: a flat grid at the
 *    PS2 "mid" ocean's resolution (6x6 tiles of 8x8 cells of 96 m) without the cells the game
 *    masks out, with the shore cells cut to the 24 m sub-cells of the transition masks. Vertex
 *    colors from the map's color grid. One draw per visible tile.
 *  - the texture (one repeat per 96 m, like the far ocean) is shaded from the 32x32 wave heights
 *    of the frame (the ones ocean-get-height uses), so the waves move like on the PS2.
 *  - fog like the level background (the game's fog + fog towards the draw distance).
 */

#include <string>
#include <vector>

#include "common/common_types.h"

#include "game/graphics/ctr/CtrRenderer.h"

class CtrOceanRenderer : public CtrBucketRenderer {
 public:
  CtrOceanRenderer(std::string name, int id);
  ~CtrOceanRenderer() override;
  void render(DmaFollower& dma, CtrRenderState& rs) override;

  struct Packet;

 private:
  bool build_mesh(const u8* ee, const Packet& p);
  void update_texture(const u8* ee, u32 heights);
  void draw(const Packet& p, const CtrRenderState& rs);

  struct Tile {
    u32 first_index = 0;
    u32 index_count = 0;
    float center[3] = {0, 0, 0};  // relative to the map's start corner (y = 0)
    float radius = 0;
  };
  // the map the mesh was built for
  u32 m_key[6] = {0, 0, 0, 0, 0, 0};
  bool m_have_mesh = false;
  int m_mesh = -1;
  std::vector<Tile> m_tiles;
  int m_tex = -1;
  std::vector<u8> m_texels;
  // statistics
  int m_frames = 0, m_tiles_drawn = 0, m_triangles = 0;
};
