#pragma once

/*!
 * @file CtrSprite.h
 * (AI-assisted)
 * Sprites for the 3DS renderer (Jak 1 sprite bucket): the 2D HUD (orb and cell counters,
 * icons), world space 2D sprites (particles, glows) and 3D sprites. Follows the DMA layout of
 * Sprite3::render_jak1 and the vertex math of sprite3_3d.vert, done on the CPU: each sprite
 * becomes two triangles for ctr_gpu_draw. Not done: the distorter (heat haze), the fake shadow.
 */

#include <memory>
#include <vector>

#include "common/dma/gs.h"
#include "common/math/Vector.h"

#include "game/graphics/ctr/CtrRenderer.h"
#include "game/graphics/ctr/ctr_gpu.h"

class CtrDirect;

class CtrSpriteRenderer : public CtrBucketRenderer {
 public:
  CtrSpriteRenderer(std::string name, int id, CtrVram* vram);
  ~CtrSpriteRenderer();
  void render(DmaFollower& dma, CtrRenderState& rs) override;

  static constexpr int kSpritesPerChunk = 48;

 private:
  enum Mode { MODE_2D = 1, MODE_HUD = 2, MODE_3D = 3 };
  void skip_distorter(DmaFollower& dma, CtrRenderState& rs);
  bool read_chunk(DmaFollower& dma, u32* count, u32* program);
  void draw_chunk(u32 count, Mode mode);
  void flush();

  struct VecData {
    math::Vector4f xyz_sx;
    math::Vector4f flag_rot_sy;
    math::Vector4f rgba;
  };
  struct AdGif {
    u64 tex0_data, tex0_addr, tex1_data, tex1_addr, mip_data, mip_addr, clamp_data, clamp_addr,
        alpha_data, alpha_addr;
  };
  static_assert(sizeof(AdGif) == 80);

  CtrVram* m_vram;
  std::unique_ptr<CtrDirect> m_direct;

  // per frame data (see SpriteFrameDataJak1)
  math::Vector4f m_xy_array[8];
  math::Vector4f m_st_array[4];
  math::Vector4f m_xyz_array[4];
  float m_pfog0 = 1, m_deg_to_rad = 0, m_min_scale = 0, m_inv_area = 0;
  float m_max_scale = 1, m_fog_min = 0, m_fog_max = 0;
  math::Vector4f m_basis_x, m_basis_y;
  // 3D / world 2D camera (Sprite3DMatrixData): column major
  float m_camera[16];
  math::Vector4f m_hvdf_offset;
  // HUD (SpriteHudMatrixData)
  float m_hud_matrix[16];
  math::Vector4f m_hud_hvdf[76];  // [0] = hvdf_offset, [1 + i] = user_hvdf[i]

  VecData m_vec[kSpritesPerChunk];
  AdGif m_adgif[kSpritesPerChunk];

  // sprites are grouped by GS state for the whole group (like Sprite3's buckets), then drawn
  struct Bucket {
    ctr_draw_state state;
    std::vector<ctr_vertex> verts;
  };
  std::vector<Bucket> m_buckets;
  size_t m_bucket_count = 0;
  size_t m_last_bucket = 0;
  std::vector<ctr_vertex>& bucket_for(const ctr_draw_state& st);
  // TEX0 -> texture lookups, per frame
  u64 m_last_tex0 = 0;
  int m_last_tex = -1;
  bool m_last_tex_valid = false;
  // state of the previous sprite (same adgif: same state and bucket)
  AdGif m_last_ad;
  bool m_have_last_ad = false;
  ctr_draw_state m_last_state;
  std::vector<ctr_vertex>* m_last_verts = nullptr;
  float m_corner_reach = 0;
  int m_world_left = 0;  // world sprites left this frame (CtrSettings::max_sprites)
  struct Stats {
    int sprites_2d = 0, sprites_hud = 0, sprites_3d = 0, draws = 0;
  } m_stats;
};
