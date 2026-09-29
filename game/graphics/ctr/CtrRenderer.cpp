/*!
 * @file CtrRenderer.cpp
 * (AI-assisted)
 * See CtrRenderer.h.
 */

#include "CtrRenderer.h"

#include <chrono>
#include <cstring>
#include <thread>

#include "common/dma/dma.h"
#include "common/log/log.h"
#include "common/util/Assert.h"

#include "fmt/format.h"

#include "game/graphics/ctr/CtrDirect.h"
#include "game/graphics/ctr/CtrVram.h"
#include "game/graphics/ctr/ctr_gpu.h"
#include "game/graphics/opengl_renderer/buckets.h"
#include "game/kernel/common/kboot.h"
#include "game/kernel/common/kmachine.h"
#include "game/kernel/common/kscheme.h"
#include "game/runtime.h"

namespace {
std::string g_shot_dir;
int g_shot_every = 0;
}  // namespace

namespace ctr_gfx {
void set_screenshots(const std::string& dir, int every_nth) {
  g_shot_dir = dir;
  g_shot_every = every_nth;
}
}  // namespace ctr_gfx

// ---------------------------------------------------------------------------
// Bucket renderers
// ---------------------------------------------------------------------------

void CtrSkipRenderer::render(DmaFollower& dma, CtrRenderState& rs) {
  while (dma.current_tag_offset() != rs.next_bucket) {
    dma.read_and_advance();
  }
}

void CtrTextureUploadRenderer::render(DmaFollower& dma, CtrRenderState& rs) {
  // same data as TextureUploadHandler: 16-byte PC_PORT tags with {u64 page, s64 mode}
  while (dma.current_tag_offset() != rs.next_bucket) {
    auto data = dma.read_and_advance();
    if (data.size_bytes == 16 && data.vifcode0().kind == VifCode::Kind::PC_PORT &&
        data.vif1() == 3) {
      u64 page;
      s64 mode;
      memcpy(&page, data.data, 8);
      memcpy(&mode, data.data + 8, 8);
      rs.vram->upload_texture_page(rs.ee_mem + page, (int)mode, rs.ee_mem, rs.offset_of_s7);
    }
  }
}

CtrDirectBucketRenderer::CtrDirectBucketRenderer(std::string name, int id, CtrVram* vram, bool depth)
    : CtrBucketRenderer(std::move(name), id), m_direct(std::make_unique<CtrDirect>(vram)) {
  m_direct->set_allow_depth(depth);
}

CtrDirectBucketRenderer::~CtrDirectBucketRenderer() = default;

void CtrDirectBucketRenderer::render(DmaFollower& dma, CtrRenderState& rs) {
  m_direct->reset_state();
  while (dma.current_tag_offset() != rs.next_bucket) {
    auto data = dma.read_and_advance();
    if (data.size_bytes) {
      m_direct->render_vif(data.vif0(), data.vif1(), data.data, data.size_bytes);
    }
    if (dma.current_tag_offset() == rs.default_regs_buffer) {
      dma.read_and_advance();  // cnt
      dma.read_and_advance();  // ret
    }
  }
  m_direct->flush();
  if (rs.frame_idx % 300 == 0) {
    const auto& st = m_direct->stats();
    lg::debug("[ctr] {}: {} packets, {} triangles, {} draws, {} skipped prims", m_name, st.packets,
             st.triangles, st.flushes, st.skipped_prims);
    m_direct->clear_stats();
  }
}

// ---------------------------------------------------------------------------
// Renderer
// ---------------------------------------------------------------------------

CtrRenderer::CtrRenderer() : m_vram(std::make_unique<CtrVram>()) {
  using jak1::BucketId;
  m_buckets.resize((int)BucketId::MAX_BUCKETS);
  auto set = [&](BucketId id, std::unique_ptr<CtrBucketRenderer> r) {
    m_buckets[(int)id] = std::move(r);
  };
  for (auto id : {BucketId::TFRAG_TEX_LEVEL0, BucketId::TFRAG_TEX_LEVEL1,
                  BucketId::SHRUB_TEX_LEVEL0, BucketId::SHRUB_TEX_LEVEL1,
                  BucketId::ALPHA_TEX_LEVEL0, BucketId::ALPHA_TEX_LEVEL1, BucketId::PRIS_TEX_LEVEL0,
                  BucketId::PRIS_TEX_LEVEL1, BucketId::WATER_TEX_LEVEL0, BucketId::WATER_TEX_LEVEL1,
                  BucketId::PRE_SPRITE_TEX}) {
    set(id, std::make_unique<CtrTextureUploadRenderer>("tex", (int)id));
  }
  set(BucketId::DEBUG,
      std::make_unique<CtrDirectBucketRenderer>("debug", (int)BucketId::DEBUG, m_vram.get(), true));
  set(BucketId::DEBUG_NO_ZBUF,
      std::make_unique<CtrDirectBucketRenderer>("debug-no-zbuf", (int)BucketId::DEBUG_NO_ZBUF,
                                                m_vram.get(), false));
  set(BucketId::SUBTITLE, std::make_unique<CtrDirectBucketRenderer>(
                              "subtitle", (int)BucketId::SUBTITLE, m_vram.get(), false));
  for (size_t i = 0; i < m_buckets.size(); i++) {
    if (!m_buckets[i]) {
      m_buckets[i] = std::make_unique<CtrSkipRenderer>("skip", (int)i);
    }
  }
}

CtrRenderer::~CtrRenderer() = default;

void CtrRenderer::render_frame(const void* ee_mem, u32 chain_offset) {
  m_rs.ee_mem = (const u8*)ee_mem;
  m_rs.offset_of_s7 = s7.offset;
  m_rs.vram = m_vram.get();
  ctr_gpu_frame_begin(0, 0, 0);
  if (g_shot_every > 0 && m_rs.frame_idx > 0 && m_rs.frame_idx % g_shot_every == 0) {
#ifdef __3DS__
    const char* ext = "bmp";
#else
    const char* ext = "png";
#endif
    std::string path = fmt::format("{}/shot_{:06d}.{}", g_shot_dir, m_rs.frame_idx, ext);
    ctr_gpu_request_screenshot(path.c_str());
  }
  dispatch_buckets_jak1(DmaFollower(ee_mem, chain_offset));
  ctr_gpu_frame_end();
  if (m_rs.frame_idx % 300 == 0) {
    const auto& vs = m_vram->stats();
    ctr_gpu_stats gs;
    ctr_gpu_get_stats(&gs);
    lg::debug("[ctr] frame {}: {} draws {} tris, vram uploads {} (changed {}), textures decoded {}, "
             "gpu textures {} ({} KB)",
             m_rs.frame_idx, gs.draws, gs.triangles, vs.uploads, vs.uploads_changed, vs.decoded,
             gs.textures, gs.tex_bytes / 1024);
  }
  m_rs.frame_idx++;
}

void CtrRenderer::dispatch_buckets_jak1(DmaFollower dma) {
  // same structure as OpenGLRenderer::dispatch_buckets_jak1
  m_rs.buckets_base = dma.current_tag_offset() + 16;  // offset by 1 qw for the initial call
  m_rs.next_bucket = m_rs.buckets_base;

  // the chain starts with a call to the default registers chain
  auto initial_call_tag = dma.current_tag();
  if (initial_call_tag.kind != DmaTag::Kind::CALL) {
    lg::error("[ctr] unexpected start of the DMA chain");
    return;
  }
  dma.read_and_advance();
  m_rs.default_regs_buffer = dma.current_tag_offset();
  dma.read_and_advance();  // default regs (cnt)
  dma.read_and_advance();  // ret
  if (dma.current_tag_offset() != m_rs.next_bucket) {
    lg::error("[ctr] DMA chain: buckets not where expected");
    return;
  }
  m_rs.next_bucket += 16;

  for (size_t bucket_id = 0; bucket_id < m_buckets.size(); bucket_id++) {
    m_buckets[bucket_id]->render(dma, m_rs);
    if (dma.current_tag_offset() != m_rs.next_bucket) {
      lg::error("[ctr] bucket {} ({}) did not end at the next bucket", bucket_id,
                m_buckets[bucket_id]->name());
      return;
    }
    m_rs.next_bucket += 16;
    vif_interrupt_callback(bucket_id);
  }
}

// ---------------------------------------------------------------------------
// GfxRendererModule
// ---------------------------------------------------------------------------

namespace {
std::unique_ptr<CtrRenderer> g_ctr;
u32 g_frame_idx = 0;

int ctr_init(GfxGlobalSettings& /*settings*/) {
  if (ctr_gpu_init() != 0) {
    lg::error("[ctr] GPU init failed");
    return 1;
  }
  g_ctr = std::make_unique<CtrRenderer>();
  lg::info("[ctr] renderer ready");
  return 0;
}

std::shared_ptr<GfxDisplay> ctr_make_display(int, int, const char*, GfxGlobalSettings&, GameVersion,
                                             bool) {
  return nullptr;
}

void ctr_exit() {
  g_ctr.reset();
  ctr_gpu_exit();
}

u32 ctr_vsync() {
  if (MasterExit == RuntimeExitStatus::RUNNING) {
    ctr_gpu_wait_vblank();
  }
  g_frame_idx++;
  return g_frame_idx & 1;
}

u32 ctr_sync_path() {
  return 0;
}

void ctr_send_chain(const void* data, u32 offset) {
  if (g_ctr) {
    g_ctr->render_frame(data, offset);
  }
}

void ctr_texture_upload_now(const u8* tpage, int mode, u32 s7_ptr) {
  if (g_ctr) {
    g_ctr->vram().upload_texture_page(tpage, mode, g_ee_main_mem, s7_ptr);
  }
}

void ctr_texture_relocate(u32 destination, u32 source, u32 format) {
  if (g_ctr) {
    g_ctr->vram().relocate(destination, source, format);
  }
}

void ctr_set_levels(const std::vector<std::string>&) {}
void ctr_set_active_levels(const std::vector<std::string>&) {}
void ctr_force_reload_all() {}
void ctr_force_reload_level(const std::string&) {}
void ctr_force_reload_common() {}
void ctr_set_pmode_alp(float) {}
}  // namespace

const GfxRendererModule gRendererCtr = {
    ctr_init,                 // init
    ctr_make_display,         // make_display
    ctr_exit,                 // exit
    ctr_vsync,                // vsync
    ctr_sync_path,            // sync_path
    ctr_send_chain,           // send_chain
    ctr_texture_upload_now,   // texture_upload_now
    ctr_texture_relocate,     // texture_relocate
    ctr_set_levels,           // set_levels
    ctr_set_active_levels,    // set_active_levels
    ctr_force_reload_all,     // force_reload_all
    ctr_force_reload_level,   // force_reload_level
    ctr_force_reload_common,  // force_reload_common
    ctr_set_pmode_alp,        // set_pmode_alp
    GfxPipeline::Ctr,         // pipeline
    "3DS (citro3d)"           // name
};
