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
#include "game/graphics/ctr/CtrLevel.h"
#include "game/graphics/ctr/CtrMerc.h"
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

CtrRenderer::CtrRenderer()
    : m_vram(std::make_unique<CtrVram>()), m_levels(std::make_unique<CtrLevels>()) {
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
  // level backgrounds (tfrag + tie from the .c3l files) are drawn from the tfrag buckets
  set(BucketId::TFRAG_LEVEL0, std::make_unique<CtrTfragRenderer>("l0-tfrag", (int)BucketId::TFRAG_LEVEL0,
                                                                 m_levels.get()));
  set(BucketId::TFRAG_LEVEL1, std::make_unique<CtrTfragRenderer>("l1-tfrag", (int)BucketId::TFRAG_LEVEL1,
                                                                 m_levels.get()));
  // merc (characters, objects)
  for (auto id : {BucketId::MERC_TFRAG_TEX_LEVEL0, BucketId::MERC_TFRAG_TEX_LEVEL1,
                  BucketId::MERC_AFTER_ALPHA, BucketId::MERC_PRIS_LEVEL0, BucketId::MERC_PRIS_LEVEL1,
                  BucketId::MERC_AFTER_PRIS, BucketId::MERC_WATER_LEVEL0,
                  BucketId::MERC_WATER_LEVEL1}) {
    set(id, std::make_unique<CtrMercRenderer>("merc", (int)id, m_levels.get()));
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
  // No sky renderer yet: clear to the fog color from the default GS registers (the same place
  // OpenGLRenderer reads it), which is close to the sky color at the horizon.
  u8 clear[4] = {0, 0, 0, 0};
  {
    DmaFollower peek(ee_mem, chain_offset);
    if (peek.current_tag().kind == DmaTag::Kind::CALL) {
      peek.read_and_advance();
      auto regs = peek.read_and_advance();
      if (regs.size_bytes > 148) {
        memcpy(clear, regs.data + 144, 4);
      }
    }
  }
  const double t0 = ctr_gpu_time_ms();
  ctr_gpu_frame_begin(clear[0], clear[1], clear[2]);
  const double t1 = ctr_gpu_time_ms();
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
  const double t2 = ctr_gpu_time_ms();
  ctr_gpu_frame_end();
  const double t3 = ctr_gpu_time_ms();
  ctr_gpu_stats gs;
  ctr_gpu_get_stats(&gs);
  m_timing.begin_ms += t1 - t0;
  m_timing.build_ms += t2 - t1;
  m_timing.end_ms += t3 - t2;
  m_timing.gpu_ms += gs.gpu_ms;
  m_timing.gpu_draw_ms += gs.draw_ms;
  m_timing.splits += gs.cmd_splits;
  m_timing.frames++;
  if (m_rs.frame_idx % 300 == 0) {
    const auto& vs = m_vram->stats();
    lg::debug("[ctr] frame {}: {} draws {} tris, vram uploads {} (changed {}), textures decoded {}, "
             "gpu textures {} ({} KB), linear free {} KB",
             m_rs.frame_idx, gs.draws, gs.triangles, vs.uploads, vs.uploads_changed, vs.decoded,
             gs.textures, gs.tex_bytes / 1024, gs.linear_free / 1024);
    if (m_timing.frames) {
      const double n = m_timing.frames;
      lg::info(
          "[ctr] render ms/frame over {} frames: wait-gpu {:.2f}, build {:.2f}, submit {:.2f} "
          "(cpu total {:.2f}); gpu {:.2f} (draw {:.2f}); cmdbuf splits {:.1f}",
          m_timing.frames, m_timing.begin_ms / n, m_timing.build_ms / n, m_timing.end_ms / n,
          (m_timing.begin_ms + m_timing.build_ms + m_timing.end_ms) / n, m_timing.gpu_ms / n,
          m_timing.gpu_draw_ms / n, m_timing.splits / n);
      // build time by bucket renderer
      std::string by_name;
      std::vector<std::pair<std::string, double>> sums;
      for (size_t i = 0; i < m_buckets.size(); i++) {
        if (m_bucket_ms[i] <= 0) {
          continue;
        }
        bool found = false;
        for (auto& e : sums) {
          if (e.first == m_buckets[i]->name()) {
            e.second += m_bucket_ms[i];
            found = true;
          }
        }
        if (!found) {
          sums.emplace_back(m_buckets[i]->name(), m_bucket_ms[i]);
        }
        m_bucket_ms[i] = 0;
      }
      for (auto& e : sums) {
        if (e.second / n >= 0.05) {
          by_name += fmt::format(" {} {:.2f}", e.first, e.second / n);
        }
      }
      lg::info("[ctr] build ms/frame by renderer:{}", by_name);
    }
    m_timing = Timing();
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

  if (m_bucket_ms.size() != m_buckets.size()) {
    m_bucket_ms.assign(m_buckets.size(), 0.0);
  }
  for (size_t bucket_id = 0; bucket_id < m_buckets.size(); bucket_id++) {
    const double tb = ctr_gpu_time_ms();
    m_buckets[bucket_id]->render(dma, m_rs);
    m_bucket_ms[bucket_id] += ctr_gpu_time_ms() - tb;
    if (dma.current_tag_offset() != m_rs.next_bucket) {
      lg::error("[ctr] bucket {} ({}) did not end at the next bucket", bucket_id,
                m_buckets[bucket_id]->name());
      return;
    }
    m_rs.next_bucket += 16;
    if (m_rs.call_vif_callback) {
      vif_interrupt_callback(bucket_id);
    }
  }
}

// ---------------------------------------------------------------------------
// GfxRendererModule
// ---------------------------------------------------------------------------

namespace {
std::unique_ptr<CtrRenderer> g_ctr;
u32 g_frame_idx = 0;

// render thread (New 3DS): the chain handed over by send_chain
bool g_async = false;
const void* g_job_mem = nullptr;
u32 g_job_offset = 0;

// game thread timing (see ctr_send_chain)
struct EeTiming {
  double last_send = 0;
  double frame_ms = 0;       // between two send_chain calls
  double wait_render_ms = 0; // sync_path/send_chain waiting for the render thread
  double vsync_ms = 0;       // waiting for vblank
  double render_ms = 0;      // synchronous rendering in send_chain
  int frames = 0;
} g_ee;

void render_job(void*) {
  g_ctr->render_frame(g_job_mem, g_job_offset);
}

// before touching renderer state from the game thread
void wait_render_idle() {
  if (g_async) {
    g_ee.wait_render_ms += ctr_gpu_async_wait();
  }
}

int ctr_init(GfxGlobalSettings& /*settings*/) {
  if (ctr_gpu_init() != 0) {
    lg::error("[ctr] GPU init failed");
    return 1;
  }
  g_ctr = std::make_unique<CtrRenderer>();
  g_ctr->levels().load_common();
  g_async = ctr_gpu_async_start(render_job, nullptr) != 0;
  g_ctr->set_async(g_async);
  lg::info("[ctr] renderer ready ({})",
           g_async ? "render thread on core 2" : "synchronous rendering");
  return 0;
}

std::shared_ptr<GfxDisplay> ctr_make_display(int, int, const char*, GfxGlobalSettings&, GameVersion,
                                             bool) {
  return nullptr;
}

void ctr_exit() {
  ctr_gpu_async_stop();
  g_async = false;
  g_ctr.reset();
  ctr_gpu_exit();
}

u32 ctr_vsync() {
  if (MasterExit == RuntimeExitStatus::RUNNING) {
    const double t0 = ctr_gpu_time_ms();
    ctr_gpu_wait_vblank();
    g_ee.vsync_ms += ctr_gpu_time_ms() - t0;
  }
  g_frame_idx++;
  return g_frame_idx & 1;
}

u32 ctr_sync_path() {
  wait_render_idle();
  return 0;
}

void ctr_send_chain(const void* data, u32 offset) {
  if (!g_ctr) {
    return;
  }
  const double now = ctr_gpu_time_ms();
  if (g_ee.last_send != 0) {
    g_ee.frame_ms += now - g_ee.last_send;
    g_ee.frames++;
  }
  g_ee.last_send = now;
  if (g_async) {
    wait_render_idle();
    g_job_mem = data;
    g_job_offset = offset;
    ctr_gpu_async_submit();
  } else {
    g_ctr->render_frame(data, offset);
    g_ee.render_ms += ctr_gpu_time_ms() - now;
  }
  if (g_ee.frames >= 300) {
    const double n = g_ee.frames;
    const double logic = (g_ee.frame_ms - g_ee.wait_render_ms - g_ee.vsync_ms - g_ee.render_ms) / n;
    lg::info(
        "[ctr] game thread ms/frame over {} frames: frame {:.2f} ({:.1f} fps) = logic {:.2f} + "
        "sync render {:.2f} + wait render thread {:.2f} + vsync {:.2f}",
        g_ee.frames, g_ee.frame_ms / n, 1000.0 * n / g_ee.frame_ms, logic, g_ee.render_ms / n,
        g_ee.wait_render_ms / n, g_ee.vsync_ms / n);
    const double last = g_ee.last_send;
    g_ee = EeTiming();
    g_ee.last_send = last;
  }
}

void ctr_texture_upload_now(const u8* tpage, int mode, u32 s7_ptr) {
  if (g_ctr) {
    wait_render_idle();
    g_ctr->vram().upload_texture_page(tpage, mode, g_ee_main_mem, s7_ptr);
  }
}

void ctr_texture_relocate(u32 destination, u32 source, u32 format) {
  if (g_ctr) {
    wait_render_idle();
    g_ctr->vram().relocate(destination, source, format);
  }
}

void ctr_set_levels(const std::vector<std::string>& levels) {
  if (g_ctr) {
    wait_render_idle();
    g_ctr->levels().set_wanted(levels);
  }
}
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
