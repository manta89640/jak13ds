#pragma once

#include <memory>
#include <unordered_map>

/*!
 * @file CtrSky.h
 * (AI-assisted)
 * The sky for the 3DS renderer, like the PC port's SkyBlendCPU + SkyRenderer, without the GS
 * emulation:
 * - make-sky-textures blends the level's 8 time of day sky textures and its cloud texture into a
 *   32x32 sky and a 64x64 cloud texture (sprites into VRAM, in the tfrag-trans/sky-blend buckets).
 *   CtrSkyBlendRenderer does that blend on the CPU (SkyBlendCPU's math) into two textures.
 * - On the 3DS the game doesn't run render-sky-tng (its polygon clipping cost ~1.5 ms per frame):
 *   render-sky-3ds sends the camera, the cloud scroll offsets and where render-sky-tng's polygons
 *   are, and CtrSky::draw draws the same polygons, clipped by the GPU (ctr_gpu_draw_clip).
 */

#include <map>
#include <vector>

#include "common/common_types.h"

#include "game/graphics/ctr/CtrRenderer.h"
#include "game/graphics/ctr/ctr_gpu.h"

class CtrVram;

class CtrSky {
 public:
  ~CtrSky();
  /*! The sky copies of make-sky-textures (pairs of 6 qw transfers). */
  // salt: tells the blends of the two levels apart (their textures can share VRAM addresses)
  void blend(DmaFollower& dma, CtrRenderState& rs, u64 salt);
  /*! render-sky-3ds's packet (8 qw, see there). */
  void draw(const u8* packet, CtrRenderState& rs);
  static constexpr int kPacketBytes = 8 * 16;
  /*! (AI-assisted) sky_last: the sky's draws depth tested (GEQUAL; the renderer sets
   * ctr_gpu_set_depth_far around them), drawn after the opaque level and models */
  void set_depth_tested(bool on) { m_depth_tested = on; }
  /*! (AI-assisted) skip_clear: this packet's opaque roof (drawn with the last complete sky
   * texture) covers the whole screen, so no pixel keeps the frame's clear color */
  bool roof_covers_screen(const u8* packet, const CtrRenderState& rs) const;

 private:
  static constexpr int kSize[2] = {32, 64};  // sky, clouds
  std::vector<u8> m_rgba[2];
  // (AI-assisted) this frame's blend is built here and replaces m_rgba only when it completed
  // (every source read): a frame with a source that couldn't be read used to leave a black or
  // partial texture for a frame (flickering clouds)
  std::vector<u8> m_work[2];
  bool m_dirty[2] = {false, false};
  bool m_valid[2] = {false, false};
  bool m_depth_tested = false;  // (AI-assisted) sky_last
  int m_tex[2] = {-1, -1};
  std::vector<u32> m_decode;
  std::vector<u8> m_premul;              // (AI-assisted) the clouds, rgb * alpha
  std::vector<ctr_clip_vertex2> m_tris2;  // both cloud layers' vertices
  // decoded sources (TEXA fix applied), by tex0 ^ salt: decoding from the VRAM copy writes the
  // level's pending texture page upload first (the two levels alternate pages there every frame),
  // which cost 10-30 ms per frame on hardware. The sky textures don't change while a level is
  // loaded: decode again only every kRedecodeFrames.
  struct Source {
    std::vector<u32> rgba;
    int w = 0, h = 0;
    bool ok = false;
    u32 frame = 0;
  };
  std::unordered_map<u64, Source> m_sources;
  std::vector<u64> m_logged;  // source textures already logged
  struct Stats {
    int blends = 0, draws = 0;
    int fast_blends = 0;  // (AI-assisted, title-popin) textures blended between the 4th frames
    int frames = 0;
    double blend_ms = 0;  // (AI-assisted, title-popin) render thread: blends and uploads
  } m_stats;
  // (AI-assisted, title-popin) this frame's copies, and per texture the copies of its last blend.
  // (AI-assisted, sky-flicker) The copies of both levels (make-sky-textures runs per level: level
  // 0's in bucket 32, level 1's added in bucket 39) are collected over the frame and blended once
  // all of them are in (finish_blend, at the next sky draw): deciding per level whether to blend
  // made frames from one level's part alone whenever the other level's weights stood still (Fire
  // Canyon: village2's or village3's few percent and firecanyon's rest step at different frames as
  // Jak moves; the sky and clouds flickered between the whole blend and one level's share).
  struct Copy {
    u64 key;  // m_sources key (the source texture, as decoded in the copy's bucket)
    u32 intensity;
    int idx;
    bool first;
  };
  std::vector<Copy> m_copies;
  u64 m_copies_frame = ~0ull;  // the frame the copies are from
  std::vector<std::pair<u64, u32>> m_last_weights[2];
  void finish_blend();
};

class CtrTfragRenderer;

/*! The tfrag-trans + sky blend buckets: the sky blend, then the level's trans tfrag: the camera
 * for the level background (drawn from the .c3l files) when the level has no normal tfrag tree. */
class CtrSkyBlendRenderer : public CtrBucketRenderer {
 public:
  CtrSkyBlendRenderer(std::string name, int id, CtrSky* sky, std::unique_ptr<CtrTfragRenderer> tfrag);
  ~CtrSkyBlendRenderer() override;
  void render(DmaFollower& dma, CtrRenderState& rs) override;

 private:
  CtrSky* m_sky;
  std::unique_ptr<CtrTfragRenderer> m_tfrag;
};
