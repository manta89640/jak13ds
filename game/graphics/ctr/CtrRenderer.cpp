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
#include "game/graphics/ctr/CtrEye.h"
#include "game/graphics/ctr/CtrLevel.h"
#include "game/graphics/ctr/CtrMerc.h"
#include "game/graphics/ctr/CtrOcean.h"
#include "game/graphics/ctr/CtrSprite.h"
#include "game/graphics/ctr/CtrVram.h"
#include "game/graphics/ctr/CtrSettings.h"
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
  const int triangles_before = m_direct->stats().triangles;
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
  m_drew = m_direct->stats().triangles != triangles_before;
  if (rs.log_now) {
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
    auto merc = std::make_unique<CtrMercRenderer>("merc", (int)id, m_levels.get());
    m_merc.emplace_back((int)id, merc.get());
    set(id, std::move(merc));
  }
  // eyes: drawn to textures that the merc eye draws use (the draws before this bucket use the
  // previous frame's)
  {
    auto eyes = std::make_unique<CtrEyeRenderer>("eyes", (int)BucketId::MERC_EYES_AFTER_PRIS,
                                                 m_vram.get());
    for (auto& [id, merc] : m_merc) {
      merc->set_eye_renderer(eyes.get());
    }
    set(BucketId::MERC_EYES_AFTER_PRIS, std::move(eyes));
  }
  // the ocean (after the level and merc, like the PS2's ocean-near)
  set(BucketId::OCEAN_NEAR,
      std::make_unique<CtrOceanRenderer>("ocean", (int)BucketId::OCEAN_NEAR));
  // the background when there is no sky (the ND logo, interiors): a full screen gradient in the
  // time of day's erase color. With a sky, the clear color (the fog color) stands in for it.
  {
    auto sky = std::make_unique<CtrDirectBucketRenderer>("sky", (int)BucketId::SKY_DRAW,
                                                         m_vram.get(), false);
    m_sky = sky.get();
    set(BucketId::SKY_DRAW, std::move(sky));
  }
  set(BucketId::SPRITE,
      std::make_unique<CtrSpriteRenderer>("sprite", (int)BucketId::SPRITE, m_vram.get()));
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
  m_rs.log_now = m_rs.frame_idx % 300 == 0 || t0 - m_rs.last_log_ms > 5000.0;
  if (m_rs.log_now) {
    m_rs.last_log_ms = t0;
  }
  memcpy(m_rs.fog_color, clear, 4);
  // No sky (its bucket had the background gradient last frame): black, like the PS2 outside of
  // the 4:3 picture. Else the fog color stands in for the sky.
  if (m_sky && m_sky->drew()) {
    clear[0] = clear[1] = clear[2] = 0;
  }
  m_levels->process_pending_loads(m_rs.frame_idx);
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
  if (m_rs.log_now) {
    const auto& vs = m_vram->stats();
    lg::debug("[ctr] frame {}: {} draws {} tris, vram uploads {} (written {}), relocates skipped "
             "{}, textures decoded {} (reused {}), gpu textures {} ({} KB), linear free {} KB, vram free {} KB ({} vram textures, {} failed copies)",
             m_rs.frame_idx, gs.draws, gs.triangles, vs.uploads, vs.uploads_changed,
             vs.relocates_skipped, vs.decoded, vs.revived, gs.textures, gs.tex_bytes / 1024,
             gs.linear_free / 1024, gs.vram_free / 1024, gs.vram_textures,
             gs.vram_copy_failures);
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

void CtrRenderer::prepare_frame(const void* ee_mem, u32 chain_offset) {
  // game thread, before handing the frame to the render thread: copy what the frame needs from
  // game memory that is not double buffered (merc bone matrices)
  CtrRenderState rs;
  rs.ee_mem = (const u8*)ee_mem;
  rs.offset_of_s7 = s7.offset;
  const u32 base = chain_offset + 16;  // see dispatch_buckets_jak1
  for (auto& [id, merc] : m_merc) {
    rs.next_bucket = base + 16 * (id + 1);
    DmaFollower dma(ee_mem, base + 16 * id);
    merc->snapshot_bones(dma, rs);
  }
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
  double upload_ms = 0;      // texture_upload_now / relocate (VRAM emulation, game thread)
  double prepare_ms = 0;     // prepare_frame (bone matrix snapshot)
  int uploads = 0, relocates = 0;
  double last_log = 0;
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
  ctr_gpu_set_rgba4_as_rgba8(ctr_settings().rgba4_as_rgba8 ? 1 : 0);
  ctr_gpu_set_vram_textures(ctr_settings().vram_textures ? 1 : 0);
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
    const double tp = ctr_gpu_time_ms();
    g_ctr->prepare_frame(data, offset);
    g_ee.prepare_ms += ctr_gpu_time_ms() - tp;
    g_job_mem = data;
    g_job_offset = offset;
    ctr_gpu_async_submit();
  } else {
    g_ctr->render_frame(data, offset);
    g_ee.render_ms += ctr_gpu_time_ms() - now;
  }
  if (g_ee.frames >= 300 || (g_ee.frames > 0 && now - g_ee.last_log > 5000.0)) {
    const double n = g_ee.frames;
    const double logic = (g_ee.frame_ms - g_ee.wait_render_ms - g_ee.vsync_ms - g_ee.render_ms -
                          g_ee.upload_ms - g_ee.prepare_ms) /
                         n;
    lg::info(
        "[ctr] game thread ms/frame over {} frames: frame {:.2f} ({:.1f} fps) = logic {:.2f} + "
        "sync render {:.2f} + wait render thread {:.2f} + vsync {:.2f} + texture uploads {:.2f} "
        "({:.1f} uploads, {:.1f} relocates per frame) + bone snapshot {:.2f}",
        g_ee.frames, g_ee.frame_ms / n, 1000.0 * n / g_ee.frame_ms, logic, g_ee.render_ms / n,
        g_ee.wait_render_ms / n, g_ee.vsync_ms / n, g_ee.upload_ms / n, g_ee.uploads / n,
        g_ee.relocates / n, g_ee.prepare_ms / n);
    const double last = g_ee.last_send;
    g_ee = EeTiming();
    g_ee.last_send = last;
    g_ee.last_log = now;
  }
}

void ctr_texture_upload_now(const u8* tpage, int mode, u32 s7_ptr) {
  if (g_ctr) {
    wait_render_idle();
    const double t0 = ctr_gpu_time_ms();
    g_ctr->vram().upload_texture_page_now(tpage, mode, g_ee_main_mem, s7_ptr);
    g_ee.upload_ms += ctr_gpu_time_ms() - t0;
    g_ee.uploads++;
  }
}

void ctr_texture_relocate(u32 destination, u32 source, u32 format) {
  if (g_ctr) {
    wait_render_idle();
    const double t0 = ctr_gpu_time_ms();
    g_ctr->vram().relocate(destination, source, format);
    g_ee.upload_ms += ctr_gpu_time_ms() - t0;
    g_ee.relocates++;
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
