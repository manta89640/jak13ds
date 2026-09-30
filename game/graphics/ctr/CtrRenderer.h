#pragma once

/*!
 * @file CtrRenderer.h
 * (AI-assisted)
 * The 3DS renderer ("ctr" = the 3DS's internal name). Walks the game's DMA chain bucket by bucket
 * like OpenGLRenderer::dispatch_buckets_jak1, with 3DS bucket renderers (default: skip).
 *
 * On the New 3DS it runs on a render thread on core 2, one frame behind the game like the PS2
 * (send_chain() hands the chain over, sync_path() waits for it; the DMA buffers are double
 * buffered by the game, so the chain stays valid while the next frame is computed). On the Old 3DS
 * and PC it runs synchronously in send_chain(). vsync() waits for the next vertical blank. The GPU side is ctr_gpu.h (citro3d on the 3DS, a software
 * rasterizer on PC for testing: gk --ctr-gfx).
 */

#include <memory>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "common/dma/dma_chain_read.h"

#include "game/graphics/gfx.h"

class CtrVram;
class CtrDirect;
class CtrLevels;

struct CtrRenderState {
  const u8* ee_mem = nullptr;
  u32 offset_of_s7 = 0;
  u32 buckets_base = 0;
  u32 next_bucket = 0;
  u32 default_regs_buffer = 0;
  CtrVram* vram = nullptr;
  u64 frame_idx = 0;
  bool log_now = false;  // print statistics this frame (every 300 frames or 5 seconds)
  double last_log_ms = 0;
  u8 fog_color[4] = {0, 0, 0, 0};  // from the default GS registers (also the clear color)
  bool call_vif_callback = true;  // not from the render thread: it runs GOAL code
};

class CtrBucketRenderer {
 public:
  CtrBucketRenderer(std::string name, int id) : m_name(std::move(name)), m_id(id) {}
  virtual ~CtrBucketRenderer() = default;
  virtual void render(DmaFollower& dma, CtrRenderState& rs) = 0;
  const std::string& name() const { return m_name; }

 protected:
  std::string m_name;
  int m_id;
};

/*! Ignores everything in the bucket. */
class CtrSkipRenderer : public CtrBucketRenderer {
 public:
  using CtrBucketRenderer::CtrBucketRenderer;
  void render(DmaFollower& dma, CtrRenderState& rs) override;
};

/*! Texture uploads (PC_PORT upload tags) into the emulated VRAM. */
class CtrTextureUploadRenderer : public CtrBucketRenderer {
 public:
  using CtrBucketRenderer::CtrBucketRenderer;
  void render(DmaFollower& dma, CtrRenderState& rs) override;
};

/*! GIF packets drawn directly (debug text, menus, debug draws). */
class CtrDirectBucketRenderer : public CtrBucketRenderer {
 public:
  CtrDirectBucketRenderer(std::string name, int id, CtrVram* vram, bool depth);
  ~CtrDirectBucketRenderer();
  void render(DmaFollower& dma, CtrRenderState& rs) override;

 private:
  std::unique_ptr<CtrDirect> m_direct;
};

class CtrRenderer {
 public:
  CtrRenderer();
  ~CtrRenderer();
  void render_frame(const void* ee_mem, u32 chain_offset);
  /*! Game thread, before render_frame runs on the render thread (see prepare_frame). */
  void prepare_frame(const void* ee_mem, u32 chain_offset);
  void set_async(bool async) { m_rs.call_vif_callback = !async; }
  // timing, averaged over the frames between two log lines
  struct Timing {
    double begin_ms = 0;  // frame begin: waiting for the GPU to finish the previous frame
    double build_ms = 0;  // walking the DMA chain, building GPU commands
    double end_ms = 0;    // frame end: submitting the command list
    double gpu_ms = 0;    // GPU processing time (from citro3d)
    double gpu_draw_ms = 0;
    int splits = 0;
    int frames = 0;
  };
  CtrVram& vram() { return *m_vram; }
  CtrLevels& levels() { return *m_levels; }

 private:
  void dispatch_buckets_jak1(DmaFollower dma);
  std::unique_ptr<CtrVram> m_vram;
  std::unique_ptr<CtrLevels> m_levels;
  std::vector<std::unique_ptr<CtrBucketRenderer>> m_buckets;
  CtrRenderState m_rs;
  Timing m_timing;
  std::vector<double> m_bucket_ms;
  std::vector<std::pair<int, class CtrMercRenderer*>> m_merc;
};

extern const GfxRendererModule gRendererCtr;

namespace ctr_gfx {
/*! PC only: write every nth frame of the software rasterizer to <dir>/frame_<n>.png. */
void set_soft_frame_dump(const std::string& dir, int every_nth);
/*! Save a screenshot every nth frame to <dir>/shot_<frame>.(png|bmp) (3DS: BMP from the GPU). */
void set_screenshots(const std::string& dir, int every_nth);
}  // namespace ctr_gfx
