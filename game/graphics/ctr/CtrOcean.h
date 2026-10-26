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
 *
 * (AI-assisted) config ocean = waves adds (flat stays as above):
 *  - a near grid around the camera displaced by the wave heights (CtrOceanNear.h), drawn first and
 *    marking the stencil buffer; the far mesh and ring then skip the marked pixels (so the troughs
 *    don't show the flat water), with the waves faded out at the grid's edge (no seam). Without a
 *    stencil buffer (depth16) the far mesh is lowered below the deepest trough instead.
 *  - a shimmer: a second, static wave texture scrolling in another direction (texture unit 2),
 *    and crests brighter / troughs darker (ctr_ocean.v.pica).
 */

#include <memory>
#include <string>
#include <vector>

#include "common/common_types.h"

#include "game/graphics/ctr/CtrRenderer.h"

class CtrDirect;
class CtrVram;

/*!
 * (AI-assisted, j2water) Jak II's ocean sky reflection, part 1: the env map ocean-method-89 draws
 * (ocean-3ds-envmap, goal_src/jak2/engine/gfx/ocean/ocean.gc: the sky color, the sun and moon
 * glows, the haze and the lit clouds, seen from above), its GS packets in the ocean-mid-far bucket
 * every second frame. CtrDirect draws them into a 64 x 64 render target, up to the packets that
 * set the next frame buffer (ocean-method-83's warp, done by CtrOceanRenderer). The other frames
 * keep the last one.
 */
class CtrOceanEnvJak2 : public CtrBucketRenderer {
 public:
  CtrOceanEnvJak2(std::string name, int id, CtrVram* vram);
  ~CtrOceanEnvJak2() override;
  void render(DmaFollower& dma, CtrRenderState& rs) override;

 private:
  std::unique_ptr<CtrDirect> m_direct;
};

class CtrOceanRenderer : public CtrBucketRenderer {
 public:
  CtrOceanRenderer(std::string name, int id);
  ~CtrOceanRenderer() override;
  void render(DmaFollower& dma, CtrRenderState& rs) override;

  struct Packet;

 private:
  bool build_mesh(const u8* ee, const Packet& p);
  void update_texture(const u8* ee, u32 heights, bool shade);
  void draw(const Packet& p, const CtrRenderState& rs);
  // (AI-assisted) ocean = waves
  bool waves() const;
  void build_near(const u8* ee, const Packet& p);
  void make_shimmer(const float* h);

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
  float m_heights[32 * 32];  // this frame's wave heights
  float m_tint[3] = {1.f, 1.f, 1.f};  // (AI-assisted) Jak II: the sky's light on the water
  // statistics
  int m_frames = 0, m_tiles_drawn = 0, m_triangles = 0;

  // (AI-assisted) ocean = waves: water per 24 m sub-cell (1 = water), the near grid of the frame
  std::vector<u8> m_water;
  u32 m_fade[256];
  bool m_have_fade = false;
  int m_shimmer = -1;
  const void* m_near_verts = nullptr;
  const u16* m_near_indices = nullptr;
  int m_near_index_count = 0;
  int m_near_ox = 0, m_near_oz = 0;
  float m_trough = 0;          // deepest trough this level (game units, >= 0)
  float m_time = 0;            // shimmer scroll time (ms), stops while the waves do
  double m_last_ms = 0;
  float m_last_h[2] = {0, 0};  // two wave heights of the last frame (paused when the same)
  // (AI-assisted, j2water) Jak II's sky reflection: the env map (64 x 64 render target: the warp
  // of CtrOceanEnvJak2's, or the environment-ocean texture without the sky) times env-color
  bool update_env_jak2(const Packet& p, const CtrRenderState& rs);
  void update_alphamod(u64 tex0, const CtrRenderState& rs);
  void update_texture_jak2(const u8* ee, u32 heights, bool shade);
  float m_lights[7][4];  // the packet's vu-lights (Jak II)
  bool m_have_lights = false;
  std::vector<u8> m_alphamod;  // environment-ocean-alphamod's alpha
  int m_alphamod_w = 0, m_alphamod_h = 0;
  u64 m_alphamod_tex0 = 0, m_alphamod_frame = 0;
  int m_env_rt = -1;
  float m_env_uv[6] = {0, 0, 0, 0, 0, 0};  // s, t at (0, 0); per unit of u; per unit of v
  u64 m_env_made_from = ~0ull;  // CtrOceanEnvJak2's frame the env map was warped from
  u64 m_env_static = 0;         // the environment-ocean tex0 it was copied from (no sky)
  float m_env_color[3] = {-1, -1, -1};
  bool m_env_ok = false;        // this frame: draw with the reflection
  int m_env_tiles = 0;          // statistics: tiles drawn with it
  // the near grid's CPU time, logged once per level
  int m_near_frames = 0;
  double m_near_ms = 0;
  bool m_near_logged = false;
};
