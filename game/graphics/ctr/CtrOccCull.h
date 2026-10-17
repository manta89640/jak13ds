#pragma once

/*!
 * @file CtrOccCull.h
 * (AI-assisted)
 * config.ini occ_cull: software occlusion culling of the level background, each frame, on the
 * render thread (CtrTfragRenderer::draw_level). The opaque level pieces drawn this frame nearest
 * the camera (the biggest on screen first, up to occ_cull_tris triangles within occ_cull_dist
 * meters) are rasterized on the CPU into a small occlusion buffer, then every piece about to be
 * drawn is tested against it: one that is behind it everywhere on screen isn't drawn.
 *
 * Conservative: the buffer is 100x60 cells of 4x4 screen pixels; a cell holds which of its 16
 * pixel centers occluder triangles cover (a sample counts only if it is at least half a pixel
 * inside a triangle, so the GPU's own pixel centers and edge rules can't differ from it) and the
 * farthest depth (clip w) of those triangles over the cell. Only a cell with all 16 samples
 * covered hides anything, and only what is behind its farthest depth by 2%. A piece is tested
 * with its bounding sphere: its screen rectangle (a pixel wider each way) and its nearest depth.
 * The caller gives only triangles the GPU draws opaque and depth written for certain this frame
 * (see CtrLevel.cpp occ_cull), and hides only depth tested draws. Triangles crossing the near
 * plane are left out.
 */

#include <vector>

#include "common/common_types.h"

class CtrOccCull {
 public:
  static constexpr int kW = 400, kH = 240;  // screen pixels
  static constexpr int kCell = 4;           // pixels per cell side
  static constexpr int kCW = kW / kCell, kCH = kH / kCell;
  static constexpr int kLevels = 8;

  /*! A new frame: an empty buffer (it is shared by the levels drawn in the frame). */
  void begin_frame(u64 frame);
  u64 frame() const { return m_frame; }
  /*! One occluder triangle, its corners in clip space (x, y, z, w: the GPU's matrix applied). */
  void add_triangle(const float* a, const float* b, const float* c);
  /*! After the occluders of a level draw: the pyramid for the tests. */
  void finish_occluders();
  /*! Is a sphere (its center in clip space, its half extents in clip x, y and w) behind the buffer
   * everywhere on screen? (false where unsure) */
  bool hidden(const float* q, float rx, float ry, float rw) const;
  /*! Could any of the sphere be on screen (in front of the camera)? (true where unsure) */
  static bool on_screen(const float* q, float rx, float ry, float rw);
  /*! (testing) the buffer as a picture (PGM): full cells by depth, partial grey, empty black */
  void dump(const char* path) const;

  // statistics (the caller resets them)
  u64 tris_in = 0, tris_drawn = 0;
  u64 full_cells = 0, partial_cells = 0;  // after finish_occluders (statistics)
  u64 near_rejects = 0, area_rejects = 0;

 private:
  u64 m_frame = ~0ull;
  float m_w[kCW * kCH];   // the farthest w of the triangles covering the cell's samples
  u16 m_mask[kCW * kCH];  // which of its 16 samples are covered (bit y * 4 + x)
  // max pyramid of the cells (a full cell: its w, others: infinity); level 0 is the cells
  std::vector<float> m_pyr[kLevels];
  int m_pw[kLevels] = {}, m_ph[kLevels] = {};
  float rect_max(int x0, int y0, int x1, int y1) const;
  void raster(const float* a, const float* b, const float* c);
};
