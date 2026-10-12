#pragma once

/*!
 * @file CtrSprite.h
 * (AI-assisted)
 * Sprites for the 3DS renderer (Jak 1 sprite bucket): the 2D HUD (orb and cell counters,
 * icons), world space 2D sprites (particles, glows) and 3D sprites. Follows the DMA layout of
 * Sprite3::render_jak1 and the vertex math of sprite3_3d.vert, done on the CPU: each sprite
 * becomes a quad for ctr_gpu_draw_quads. The distorter (portals, heat haze: fans that warp what
 * is behind them) reads a copy of the screen (ctr_gpu_copy_screen), like Sprite3_Distort. Not
 * done: the fake shadow.
 */

#include <algorithm>
#include <cstring>
#include <memory>
#include <vector>

#include "common/dma/gs.h"
#include "common/math/Vector.h"

#include "game/graphics/ctr/CtrRenderer.h"
#include "game/graphics/ctr/CtrVram.h"
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
  // reads the distorter's DMA (Sprite3::distort_dma; all of it, also when it doesn't look right:
  // then false); draw_distorter draws it (Sprite3::distort_setup / distort_draw)
  bool read_distorter(DmaFollower& dma, CtrRenderState& rs);
  void draw_distorter();
  bool read_chunk(DmaFollower& dma, u32* count, u32* program);
  void draw_chunk(u32 count, Mode mode);
  // draws the buckets. hud: as Sprite3's HUD flush, a sprite that writes depth with the alpha test
  // (FB_ONLY) is drawn a second time for the fragments that fail it, without depth writes
  void flush(bool hud = false);

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

  // distorter (Sprite3.h: SpriteDistorterSineTables, SpriteDistortFrameData)
  struct DistortTables {
    math::Vector4f entry[128];
    math::Vector<u32, 4> ientry[9];
    u64 gif_tag[2];
    math::Vector<u32, 4> color;
  };
  static_assert(sizeof(DistortTables) == 0x8b * 16);
  struct DistortSprite {
    math::Vector3f xyz;  // GS screen position
    float num_255;
    math::Vector2f st;   // the screen point it shows: GS frame texture (512 x 256) coordinates
    float num_1;
    u32 flag;            // slices (3..11)
    math::Vector4f rgba;  // x: inner radius, y: outer radius, z: texture radius
  };
  static_assert(sizeof(DistortSprite) == 48);
  static constexpr int kMaxDistortSprites = 1024;
  DistortTables m_distort_tables;
  std::vector<DistortSprite> m_distort_sprites;
  int m_distort_count = 0;
  std::vector<ctr_vertex> m_distort_verts;  // (AI-assisted) one triangle strip (ctr_gpu_draw_strip)

  VecData m_vec[kSpritesPerChunk];
  AdGif m_adgif[kSpritesPerChunk];

  // (AI-assisted) a vertex array that grows without zeroing the new vertices (std::vector's
  // resize does), which are written right after
  template <typename T>
  struct VertBuf {
    std::unique_ptr<T[]> buf;
    size_t n = 0, cap = 0;
    T* grow(size_t k) {
      if (n + k > cap) {
        cap = std::max<size_t>(2 * cap, n + k + 64);
        std::unique_ptr<T[]> bigger(new T[cap]);
        if (n) {
          memcpy(bigger.get(), buf.get(), n * sizeof(T));
        }
        buf = std::move(bigger);
      }
      T* p = buf.get() + n;
      n += k;
      return p;
    }
    T* data() { return buf.get(); }
    size_t size() const { return n; }
    bool empty() const { return n == 0; }
    void clear() { n = 0; }
  };
  // sprites are grouped by GS state for the whole group (like Sprite3's buckets), then drawn
  struct Bucket {
    ctr_draw_state state;
    VertBuf<ctr_vertex> verts;
    // (AI-assisted) 3D sprites in clip space, 6 vertices each: the GPU clips them (a corner
    // behind the camera mirrored on the CPU: eco beams popping, water rings stretched over the
    // screen)
    VertBuf<ctr_clip_vertex> clip;
  };
  std::vector<Bucket> m_buckets;
  size_t m_bucket_count = 0;
  size_t m_last_bucket = 0;
  Bucket& bucket_for(const ctr_draw_state& st);
  // the draw state and bucket of a sprite's adgif (m_last_state, m_last_bucket_ptr); nothing to
  // do for the same adgif as the last one
  void resolve_state(const AdGif& ad, Mode mode);
  // TEX0 -> texture lookups, per frame
  u64 m_last_tex0 = 0;
  int m_last_tex = -1;
  bool m_last_tex_valid = false;
  // state of the previous sprite (same adgif: same state and bucket)
  AdGif m_last_ad;
  bool m_have_last_ad = false;
  // (AI-assisted) the clamp mode in effect (CLAMP_1 of the last adgif that set it, this chunk)
  bool m_clamp_s = true;
  bool m_clamp_t = true;
  ctr_draw_state m_last_state;
  Bucket* m_last_bucket_ptr = nullptr;
  float m_corner_reach = 0;
  // (AI-assisted) per frame: config.ini sprite_max_size in GS units, and the corners' texture
  // coordinates ([1]: for the procedural texture, centered on 0)
  float m_size_cap = 0;
  // (AI-assisted) config sprite_min_alpha (GS alpha, 128 = opaque) for sprites with a half size
  // over kBigHalfGs GS units: 20 pixels at the screen's 240 / 448 pixels per GS unit
  float m_min_alpha_big = 2.f;
  static constexpr float kBigHalfGs = 20.f * 448.f / 240.f;
  float m_st_out[2][4][2] = {};
  int m_world_left = 0;  // world sprites left this frame (CtrSettings::max_sprites)
  struct Stats {
    int sprites_2d = 0, sprites_hud = 0, sprites_3d = 0, draws = 0, distort = 0, capped = 0;
    int distort_small = 0;  // (AI-assisted) distort_copy = rows: left out, under 8 pixels
    int faint = 0;          // (AI-assisted) sprite_min_alpha: big faint ones left out
  } m_stats;
  // (AI-assisted) a frame that took more than 3 ms logs its counts (render)
  void build_sprites(DmaFollower& dma, CtrRenderState& rs);
  void log_slow_frame(CtrRenderState& rs, double ms, const Stats& s0, const CtrVram::Stats& v0);
  double m_distort_ms = 0;
  double m_last_slow_log = -1e9;
  int m_slow_frames = 0;  // since the last slow frame line
  int m_vram_uploads_mark = 0;  // CtrVram page uploads at the end of the last sprite build
};
