#pragma once

/*!
 * @file CtrShadowJak2.h
 * (AI-assisted)
 * Jak II's shadow volumes for the 3DS renderer (buckets shadow and shadow2), like the PC's
 * Shadow2: the game's shadow-cpu sends camera space volume vertices (top and bottom) and byte
 * index lists for the caps and walls; the faces turned to the camera add 1 to the stencil, the
 * others take 1 away (depth test, no color or depth writes), then a screen rectangle darkens
 * where the stencil isn't 0, by the shadow color (multiplied, like Jak 1's CtrShadow; the PC
 * subtracts). The same GPU passes as CtrShadow (ctr_gpu_clip_reserve / clip_draw_ranges).
 */

#include <vector>

#include "game/graphics/ctr/CtrRenderer.h"
#include "game/graphics/ctr/ctr_gpu.h"

class CtrShadowJak2Renderer : public CtrBucketRenderer {
 public:
  CtrShadowJak2Renderer(std::string name, int id);
  void render(DmaFollower& dma, CtrRenderState& rs) override;

 private:
  // the 3 (cap) or 4 (wall) camera space positions of one face, as a strip; away: the stencil
  // decrement pass
  void add_face(const float* const* p, int n, bool away);
  void add_caps(const u8* idx, const u8* verts, bool flip, bool flippable);
  void add_walls(const u8* idx, const u8* top, const u8* bottom);
  bool reserve();
  void draw();

  float m_clip[16];  // camera space -> clip space, row major
  ctr_clip_vertex* m_verts = nullptr;
  u16* m_indices = nullptr;
  bool m_reserved = false, m_full = false;
  int m_nv = 0;
  std::vector<u16> m_front, m_back;
  u8 m_color[4] = {0x80, 0x80, 0x80, 0x80};
  int m_no_room = 0;
  struct Stats {
    int frames = 0, faces = 0, dropped = 0;
  } m_stats;
};
