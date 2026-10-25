#pragma once

/*!
 * @file CtrDirect.h
 * (AI-assisted)
 * 3DS version of the DirectRenderer: interprets GIF packets (the GS register writes that
 * debug text, menus and debug draws send directly to the GS) and draws them with ctr_gpu.
 *
 * Supported: PACKED / REGLIST GIF tags, PRIM, RGBAQ, ST, UV, XYZ2, XYZF2, TEX0, TEX1, CLAMP, TEST,
 * ALPHA, ZBUF, SCISSOR (ignored), triangles / strips / fans / sprites. Lines and points are
 * skipped. Texture data comes from CtrVram.
 */

#include <vector>

#include "common/common_types.h"
#include "common/dma/gs.h"

#include "game/graphics/ctr/ctr_gpu.h"

class CtrVram;

/*!
 * (AI-assisted, j2gaps) Jak II's menu background (BlitDisplays on the PC): the copy of the game's
 * last frame (ctr_gpu_copy_screen) that stands in for VRAM at tbp 0x3300, where the sky bucket's
 * menu draws read it. Draws of it sample the copy at their own screen position (the game's
 * sprites cover the screen with the whole image). -1: none.
 */
void ctr_direct_set_menu_screen(int handle);
int ctr_direct_menu_screen();
/*! drawn since the last call (the copy is freed once the menu no longer draws it) */
bool ctr_direct_take_menu_screen_drawn();

#ifndef __3DS__
// (AI-assisted) host diagnosis: the next n vertices pushed by any CtrDirect are logged
extern int g_ctr_direct_dump;
#endif

/*! GS ALPHA register -> enum ctr_blend (unknown equations: normal alpha blending). */
u8 ctr_blend_from_gs_alpha(u64 alpha);

class CtrDirect {
 public:
  explicit CtrDirect(CtrVram* vram);

  /*! Reset the GS state (start of a bucket). */
  void reset_state();

  /*! Process VIF data containing DIRECT transfers. */
  void render_vif(u32 vif0, u32 vif1, const u8* data, u32 size);

  /*! Process GIF data. */
  void render_gif(const u8* data, u32 size);

  /*! Draw everything queued. */
  void flush();

  /*! Draw with depth test (debug bucket) or without (debug-no-zbuf, subtitles). */
  void set_allow_depth(bool allow) { m_allow_depth = allow; }

  /*!
   * (AI-assisted) Jak II's progress bucket: draws to the minimap's frame buffer (FRAME fbp 126,
   * 128 x 128) go to a render to texture, which stands in for VRAM at tbp 4032 (where the game
   * reads it back, like the PC's ProgressRenderer). end_offscreen at the bucket's end.
   */
  void enable_minimap() { m_minimap = true; }
  void end_offscreen();

  /*!
   * (AI-assisted, gpuopt) Record mode: flush() keeps the batches (state and vertices) instead of
   * drawing them; replay_recorded() draws them later in the same frame (Jak II's sky with
   * sky_last). depth_tested: as the deferred sky, only where nothing opaque wrote depth (GEQUAL
   * against the cleared depth, no depth writes; the caller sets ctr_gpu_set_depth_far).
   */
  void set_record(bool on);
  bool has_recorded() const { return !m_rec.empty(); }
  void replay_recorded(bool depth_tested);

  struct Stats {
    int packets = 0;
    int triangles = 0;
    int flushes = 0;
    int skipped_prims = 0;
  };
  const Stats& stats() const { return m_stats; }
  void clear_stats() { m_stats = Stats(); }

 private:
  void handle_ad(const u8* data);
  void handle_prim(u64 val);
  void handle_rgbaq(u64 val);
  void handle_st(float s, float t, float q_or_nan);
  void handle_uv(u32 u, u32 v);
  void handle_xyz(u32 x, u32 y, u32 z, bool advance);
  void set_tex0(u64 val);
  void set_state_dirty() { m_state_dirty = true; }
  void update_draw_state();
  void push_vertex(int idx);

  CtrVram* m_vram;
  bool m_allow_depth = true;
  // (AI-assisted) Jak II minimap (enable_minimap)
  bool m_minimap = false;
  bool m_offscreen = false;
  u32 m_fbp = 408;
  bool m_alpha_only = false;  // (AI-assisted) FBMSK masks rgb
  float m_xyoff[2] = {0, 0};  // (AI-assisted) XYOFFSET - the screen's, GS pixels
  void handle_frame(u64 val);
  // (AI-assisted) GS y to clip y: Jak 1 draws a 224 line field (1 / 112), Jak II a 416 line
  // frame (direct_basic.vert: HEIGHT_SCALE 0.5, SCISSOR_ADJUST 512 / 416: 1 / 208)
  float m_y_scale = 1.f / 112.f;

  // GS registers
  GsPrim m_prim;
  u64 m_tex0 = 0;
  u64 m_tex1 = 0;
  u64 m_clamp = 0b101;
  GsTest m_test;
  GsAlpha m_alpha;
  bool m_zbuf_mask = false;  // true = no z writes
  u8 m_rgba[4] = {0x80, 0x80, 0x80, 0x80};
  float m_s = 0, m_t = 0, m_q = 1;
  u32 m_u = 0, m_v = 0;

  // primitive assembly
  struct BuildVert {
    float x, y, z;
    float s, t;
    u8 rgba[4];
  };
  BuildVert m_build[3];
  int m_build_idx = 0;
  int m_strip_count = 0;

  // batching
  bool m_state_dirty = true;
  ctr_draw_state m_draw_state;
  int m_tex_w = 1, m_tex_h = 1;
  float m_uv_scale_s = 1.f / 16.f, m_uv_scale_t = 1.f / 16.f;  // GS UV (12.4) -> s, t
  std::vector<ctr_vertex> m_verts;
  bool m_quads = false;  // m_verts holds quads (sprites, ctr_gpu_draw_quads), not triangles
  void batch_mode(bool quads);
  // (AI-assisted, gpuopt) set_record: the batches flush() kept, their vertices in m_rec_verts
  struct RecBatch {
    ctr_draw_state st;
    bool quads;
    u32 first, count;
  };
  bool m_record = false;
  std::vector<RecBatch> m_rec;
  std::vector<ctr_vertex> m_rec_verts;

  Stats m_stats;
};
