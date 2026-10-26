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
  // (AI-assisted, j2gpu) the index of the first vertex of the current top (k 0) or bottom (k 1)
  // upload in the frame's vertex memory (all of it copied at the first use), -1: no room
  int upload_base(int k);
  bool index_room(int count);
  // the caps of upload k (0 top, 1 bottom); faces turned to the camera to the front list (the
  // stencil increment pass), the others to the back list (decrement)
  void add_caps(const u8* idx, bool flip, bool flippable, int k);
  void add_walls(const u8* idx);
  bool reserve();
  void draw();

  float m_clip[16];  // camera space -> clip space, row major (applied by the GPU)
  // (AI-assisted, j2gpu) per upload (0 top, 1 bottom): its data, vertex count and first vertex
  // in m_verts (-1: not copied yet)
  const u8* m_src[2] = {nullptr, nullptr};
  int m_src_count[2] = {0, 0};
  int m_base[2] = {-1, -1};
  int m_nf = 0, m_nb = 0;  // front indices (in m_indices), back indices (in m_back)
  // (AI-assisted, j2gpu + gpuopt) per volume (from one top vertex upload to the next) its camera
  // space box: the darkening rectangles cover only their projections
  struct Box {
    float lo[3] = {1e30f, 1e30f, 1e30f};
    float hi[3] = {-1e30f, -1e30f, -1e30f};
  };
  std::vector<Box> m_boxes;
  struct Rect {
    float x0, y0, x1, y1;  // clip space x, y / w
  };
  std::vector<Rect> m_rects;
  ctr_clip_vertex* m_verts = nullptr;
  u16* m_indices = nullptr;
  bool m_reserved = false, m_full = false;
  int m_nv = 0;
  int m_cap_verts = 0, m_cap_indices = 0;  // the reservation taken this frame
  std::vector<u16> m_back;
  u8 m_color[4] = {0x80, 0x80, 0x80, 0x80};
  int m_no_room = 0;
  struct Stats {
    int frames = 0, faces = 0, dropped = 0;
  } m_stats;
};
