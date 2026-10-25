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
  // (AI-assisted, j2gpu) a face's corner: vertex idx of an upload (top or bottom)
  struct Corner {
    const u8* verts;
    int idx;
    bool bottom;
  };
  // the 3 (cap) or 4 (wall) corners of one face, as a strip; away: the stencil decrement pass
  void add_face(const Corner* c, int n, bool away);
  // (AI-assisted, j2gpu) the vertex's index in the frame's vertex memory (copied at first use)
  int vertex(const u8* verts, int idx, bool bottom);
  void add_caps(const u8* idx, const u8* verts, bool flip, bool flippable, bool bottom);
  void add_walls(const u8* idx, const u8* top, const u8* bottom);
  bool reserve();
  void draw();

  float m_clip[16];  // camera space -> clip space, row major (applied by the GPU)
  // (AI-assisted, j2gpu) per upload (0 top, 1 bottom) and vertex: its index in m_verts when its
  // stamp is the upload's generation
  u16 m_map[2][256] = {};
  u32 m_stamp[2][256] = {};
  u32 m_gen[2] = {1, 1};
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
  std::vector<u16> m_front, m_back;
  u8 m_color[4] = {0x80, 0x80, 0x80, 0x80};
  int m_no_room = 0;
  struct Stats {
    int frames = 0, faces = 0, dropped = 0;
  } m_stats;
};
