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

  Stats m_stats;
};
