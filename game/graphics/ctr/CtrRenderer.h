#pragma once

/*!
 * @file CtrRenderer.h
 * (AI-assisted)
 * The 3DS renderer ("ctr" = the 3DS's internal name). Walks the game's DMA chain bucket by bucket
 * like OpenGLRenderer::dispatch_buckets_jak1, with 3DS bucket renderers (default: skip).
 *
 * It runs synchronously on the EE thread: send_chain() renders the whole frame and vsync() waits
 * for the next vertical blank. The GPU side is ctr_gpu.h (citro3d on the 3DS, a software
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
  CtrVram& vram() { return *m_vram; }
  CtrLevels& levels() { return *m_levels; }

 private:
  void dispatch_buckets_jak1(DmaFollower dma);
  std::unique_ptr<CtrVram> m_vram;
  std::unique_ptr<CtrLevels> m_levels;
  std::vector<std::unique_ptr<CtrBucketRenderer>> m_buckets;
  CtrRenderState m_rs;
};

extern const GfxRendererModule gRendererCtr;

namespace ctr_gfx {
/*! PC only: write every nth frame of the software rasterizer to <dir>/frame_<n>.png. */
void set_soft_frame_dump(const std::string& dir, int every_nth);
/*! Save a screenshot every nth frame to <dir>/shot_<frame>.(png|bmp) (3DS: BMP from the GPU). */
void set_screenshots(const std::string& dir, int every_nth);
}  // namespace ctr_gfx
