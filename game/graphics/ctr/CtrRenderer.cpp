/*!
 * @file CtrRenderer.cpp
 * (AI-assisted)
 * See CtrRenderer.h.
 */

#include "CtrRenderer.h"

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <thread>

#include "common/dma/dma.h"
#include "common/log/log.h"
#include "common/util/Assert.h"

#include "fmt/format.h"

#include "game/graphics/ctr/CtrDirect.h"

#ifdef __3DS__
extern "C" void ctr_boot_mark(const char* step);  // platform/3ds/port/ctr_port.c
extern "C" void ctr_thread_sleep_us(unsigned int us);  // platform/3ds/port/ctr_port.c
#include "game/graphics/ctr/ctr_gpu_bench.h"  // (AI-assisted) gpu_perflog, pica_bench
#include "platform/3ds/port/ctr_port.h"
#endif
#include "game/graphics/ctr/CtrEye.h"
#include "game/graphics/ctr/CtrLevel.h"
#include "game/graphics/ctr/CtrRendererJak2.h"
#include "game/graphics/ctr/CtrSky.h"
#include "game/graphics/ctr/CtrMerc.h"
#include "game/graphics/ctr/CtrOcean.h"
#include "game/graphics/ctr/CtrOceanFull.h"
#include "game/graphics/ctr/CtrShadow.h"
#include "game/graphics/ctr/CtrSprite.h"
#include "game/graphics/ctr/CtrVram.h"
#include "game/graphics/ctr/CtrSettings.h"
#include "game/graphics/ctr/ctr_gpu.h"
#include "game/kernel/common/kperf.h"
#include "game/graphics/opengl_renderer/buckets.h"
#include "game/kernel/common/kboot.h"
#include "game/kernel/common/klink.h"
#include "game/kernel/common/kmachine.h"
#include "game/kernel/common/kscheme.h"
#include "game/kernel/jak1/kscheme.h"  // (AI-assisted, title3) find_symbol_from_c
#if !defined(__3DS__) || defined(OG3DS_GAME_JAK2)
#include "game/kernel/jak2/kscheme.h"
#endif
#include "game/runtime.h"
#include "game/sce/pad_script.h"

// (AI-assisted) PMODE alpha from the game thread (ctr_set_pmode_alp), 1 = no fade
static std::atomic<float> g_pmode_alp{1.f};
// what is missing in ring 0 in view at the last frame, and the frames counted
// (ctr_arrival_ready, game thread); Jak II: nothing of it can come now (CtrLevels::coming)
static std::atomic<u32> g_arrival_late{0};
static std::atomic<u64> g_arrival_frame{0};
static std::atomic<bool> g_arrival_idle{false};
static std::atomic<double> g_arrival_done_ms{-1e9};  // the game's wait (ctr_arrival_ready) ended

// (AI-assisted) see CtrRenderer.h; set by the CtrRenderer constructor
static float g_gs_y_scale = 512.f / 448.f;
static bool g_is_jak2 = false;
float ctr_gs_y_scale() {
  return g_gs_y_scale;
}
bool ctr_is_jak2() {
  return g_is_jak2;
}

namespace {
std::string g_shot_dir;
bool g_hud_dump_pending = false;  // (AI-assisted, fable1) host: CTR_HUD_DUMP with a pad "shot"
int g_shot_every = 0;

// config.ini gpu_profile: what each mode leaves out (bucket renderer names)
struct ProfileMode {
  const char* label;
  const char* skip[3];
};
const ProfileMode kProfileModes[] = {
    {"all", {nullptr, nullptr, nullptr}},
    {"level", {"l0-tfrag", "l1-tfrag", nullptr}},
    {"merc", {"merc", "eyes", nullptr}},
    {"sprites", {"sprite", nullptr, nullptr}},
    {"ocean", {"ocean", nullptr, nullptr}},
    {"direct", {"sky", "debug", "subtitle"}},
    {"shadow", {"shadow", nullptr, nullptr}},  // (AI-assisted) shadows = full
};
constexpr int kProfileModeCount = sizeof(kProfileModes) / sizeof(kProfileModes[0]);

constexpr double kProfileModeMs = 2500.0;  // per mode
constexpr int kProfileSettleFrames = 3;    // GPU times lag the frame: skip the first ones
// (AI-assisted) the render thread's statistics: one burst of lines this often (config.ini
// stats_period, seconds)
// (AI-assisted) config.ini gpu_counters: the counters cover every stats window; the hotspot
// sampler's 16-bit counts restart after ~16 s at 268 MHz, so its stage samples stop after this
// long into the window (the traffic counters cover all of it)
constexpr double kHwCountersMs = 10000.0;

// (AI-assisted) gpu_profile: the GPU's own counters over the first "all" window of a level's
// profile (ctr_gpu_hwprof_begin / end), one log line. Azahar likely reads zeros.
bool g_hw_measuring = false;
bool g_hw_refusal_logged = false;

void hw_profile_begin() {
  const int r = ctr_gpu_hwprof_begin();
  g_hw_measuring = r == 0;
  if (r != 0 && !g_hw_refusal_logged) {
    g_hw_refusal_logged = true;
    lg::warn("[ctr] gpu profile: GPU counters unavailable (GSP result {:#x}), not measured",
             (u32)r);
  }
}

void hw_profile_end(const std::string& level) {
  if (!g_hw_measuring) {
    return;
  }
  g_hw_measuring = false;
  ctr_gpu_hwprof hw;
  const int r = ctr_gpu_hwprof_end(&hw);
  if (r != 0) {
    if (!g_hw_refusal_logged) {
      g_hw_refusal_logged = true;
      lg::warn("[ctr] gpu profile: GPU counters unavailable (GSP result {:#x})", (u32)r);
    }
    return;
  }
  static const char* const kStages[8] = {"arrays", "vsh",   "setup", "raster",
                                         "tex",    "light", "comb",  "fb"};
  u32 samples = 0;
  for (u32 n : hw.stage) {
    samples += n;
  }
  std::string line = fmt::format(
      "[ctr] gpu profile {}: hw busy % of {} samples ({} expected, {} frames):", level, samples,
      (int)(hw.stage_ms * 268111.856 / 65536.0), hw.frames);
  for (int i = 0; i < 8; i++) {
    line += fmt::format(" {} {:.1f}", kStages[i], samples ? 100.0 * hw.stage[i] / samples : 0.0);
  }
  // 0x10400070 + 4 * i (GBATEK), assumed bytes: memcopy-r should be ~0.37 MB/frame (the display
  // transfer reads the 240x400 RGBA8 buffer) if so (pica_bench B1.cal logs the real unit)
  static const char* const kTraffic[CTR_GPU_HW_TRAFFIC] = {
      "main-r",  "main-w",   "vramA-r",   "vramA-w",   "vramB-r", "vramB-w", "arrays",
      "tex",     "depth-r",  "depth-w",   "color-r",   "color-w", "lcd-top", "lcd-bot",
      "memcopy-r", "memcopy-w", "fill0-w", "fill1-w", "cpu-vram-r", "cpu-vram-w"};
  line += "; MB/frame:";
  const double div = 1048576.0 * std::max(1, hw.frames);
  for (int i = 0; i < CTR_GPU_HW_TRAFFIC; i++) {
    line += fmt::format(" {} {:.2f}", kTraffic[i], hw.traffic[i] / div);
  }
  lg::info("{}", line);
}

// (AI-assisted) pad script "set KEY VALUE": diagnostics only (the settings don't change while running)
bool pad_set(const char* key, const char* value) {
  (void)value;
#ifndef __3DS__
  if (!strcmp(key, "cam_path")) {
    return ctr_level_host_cam_path(value);
  }
#endif
  if (!strcmp(key, "gpu_micro_mask")) {
    // (AI-assisted, micro-mg) a test: gpu_micro's changes as bits (old / new in one run)
    ctr_gpu_set_micro_mask((unsigned int)strtoul(value, nullptr, 0));
    return true;
  }
  return false;
}

// (AI-assisted) pad script "shot NAME": taken by the render thread at its next frame
std::mutex g_pad_shot_mutex;
std::string g_pad_shot;
void pad_shot(const char* name) {
  std::lock_guard<std::mutex> lock(g_pad_shot_mutex);
  g_pad_shot = name;
}
}  // namespace

namespace ctr_gfx {
void request_named_shot(const char* name) {
  pad_shot(name);
}
bool take_hud_dump() {
  const bool p = g_hud_dump_pending;
  g_hud_dump_pending = false;
  return p;
}
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
    // the eyes of the level's characters (like TextureUploadHandler): their GS setup is 8 qw
    if (m_eyes && dma.current_tag().qwc == 8) {
      const u32 before = dma.current_tag_offset();
      m_eyes->handle_eye_dma(dma, rs);
      if (dma.current_tag_offset() != before) {
        continue;
      }
    }
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
    if (m_sky && data.vifcode1().kind == VifCode::Kind::PC_PORT &&
        data.size_bytes == CtrSky::kPacketBytes) {
      if (ctr_settings().sky_last) {
        // (AI-assisted) sky_last: drawn after the opaque level and models, before the first draw
        // that needs what's behind it (ctr_gpu_set_sky_hook), depth tested against them
        static_assert(sizeof(m_deferred) == CtrSky::kPacketBytes);
        memcpy(m_deferred, data.data, sizeof(m_deferred));
        m_deferred_rs = &rs;
        ctr_gpu_set_sky_hook(&CtrDirectBucketRenderer::deferred_sky, this);
      } else {
        m_sky->draw(data.data, rs);
      }
    } else if (data.size_bytes) {
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

void CtrDirectBucketRenderer::deferred_sky(void* arg) {
  auto* self = static_cast<CtrDirectBucketRenderer*>(arg);
#ifndef __3DS__
  const std::string keep = ctr_gpu_soft_bucket_name();
  ctr_gpu_soft_bucket("sky");
#endif
  self->m_sky->set_depth_tested(true);
  ctr_gpu_set_depth_far(1);
  self->m_sky->draw(self->m_deferred, *self->m_deferred_rs);
  ctr_gpu_set_depth_far(0);
  self->m_sky->set_depth_tested(false);
#ifndef __3DS__
  ctr_gpu_soft_bucket(keep.c_str());
#endif
}

// ---------------------------------------------------------------------------
// Renderer
// ---------------------------------------------------------------------------

CtrRenderer::CtrRenderer()
    // the VRAM copy only where something is written (sparse); Jak 1 prewarms its boot pages
    : m_vram(std::make_unique<CtrVram>(true, g_game_version != GameVersion::Jak2)),
      m_levels(std::make_unique<CtrLevels>()) {
  // texture page data in game memory only changes with a link (both games)
  m_vram->set_epoch(&g_link_generation);
#if !defined(__3DS__) || defined(OG3DS_GAME_JAK2)
  // (AI-assisted) Jak II: its own bucket table (CtrRendererJak2.cpp)
  if (g_game_version == GameVersion::Jak2) {
    m_jak2 = true;
    g_is_jak2 = true;
    g_gs_y_scale = 512.f / 416.f * 0.5f;
    ctr_gpu_set_gs_half_height(208.f);  // the PS2 ocean's GS positions (Jak 1: 112)
    m_vram->set_jak2_remap(true);
    init_buckets_jak2();
    return;
  }
#endif
  using jak1::BucketId;
  m_sprite_bucket = (int)BucketId::SPRITE;
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
  // (AI-assisted) a level without a normal tfrag tree sends its camera only with its dirt, ice or
  // trans tfrag (jungleb: trans only); the level is drawn once per frame, by the first of them
  for (auto [id, name] : {std::pair{BucketId::TFRAG_DIRT_LEVEL0, "l0-tfrag-dirt"},
                          std::pair{BucketId::TFRAG_DIRT_LEVEL1, "l1-tfrag-dirt"},
                          std::pair{BucketId::TFRAG_ICE_LEVEL0, "l0-tfrag-ice"},
                          std::pair{BucketId::TFRAG_ICE_LEVEL1, "l1-tfrag-ice"}}) {
    set(id, std::make_unique<CtrTfragRenderer>(name, (int)id, m_levels.get()));
  }
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
    for (auto id : {BucketId::PRIS_TEX_LEVEL0, BucketId::PRIS_TEX_LEVEL1}) {
      static_cast<CtrTextureUploadRenderer*>(m_buckets[(int)id].get())->set_eye_renderer(eyes.get());
    }
    set(BucketId::MERC_EYES_AFTER_PRIS, std::move(eyes));
  }
  // (AI-assisted) config.ini shadows = full: the game's shadow volumes (CtrShadow: stencil passes,
  // after the level and most merc buckets, before the ocean whose stencil marks it leaves at 0).
  // Else the bucket is skipped: with blob the game draws Jak's fake shadow instead.
  if (ctr_settings().shadows == CtrSettings::ShadowMode::Full) {
    if (ctr_gpu_has_stencil()) {
      set(BucketId::SHADOW, std::make_unique<CtrShadowRenderer>("shadow", (int)BucketId::SHADOW));
    } else {
      lg::warn("[ctr] shadows = full needs the stencil buffer: not drawn");
    }
  }
  // the ocean (after the level and merc, like the PS2's ocean-near)
  if (ctr_settings().ocean && ctr_settings().ocean_mode == CtrSettings::OceanMode::Full) {
    // (AI-assisted) config.ini ocean = full: the PS2 ocean's DMA (draw-ocean), both buckets
    auto full = std::make_shared<CtrOceanFull>();
    set(BucketId::OCEAN_MID_AND_FAR, std::make_unique<CtrOceanFullBucket>(
                                         "ocean", (int)BucketId::OCEAN_MID_AND_FAR, full, false));
    set(BucketId::OCEAN_NEAR,
        std::make_unique<CtrOceanFullBucket>("ocean", (int)BucketId::OCEAN_NEAR, full, true));
  } else {
    set(BucketId::OCEAN_NEAR,
        std::make_unique<CtrOceanRenderer>("ocean", (int)BucketId::OCEAN_NEAR));
  }
  // the sky (CtrSky: render-sky-3ds's packet), or the background when there is no sky (the ND
  // logo, interiors): a full screen gradient in the time of day's erase color. The sky textures
  // are blended in the tfrag-trans/sky-blend buckets.
  {
    auto sky = std::make_unique<CtrDirectBucketRenderer>("sky", (int)BucketId::SKY_DRAW,
                                                         m_vram.get(), false);
    m_sky = sky.get();
    m_sky_draw = std::make_unique<CtrSky>();
    sky->set_sky(m_sky_draw.get());
    set(BucketId::SKY_DRAW, std::move(sky));
    for (auto id : {BucketId::TFRAG_TRANS0_AND_SKY_BLEND_LEVEL0,
                    BucketId::TFRAG_TRANS1_AND_SKY_BLEND_LEVEL1}) {
      set(id, std::make_unique<CtrSkyBlendRenderer>(
                  "sky-blend", (int)id, m_sky_draw.get(),
                  std::make_unique<CtrTfragRenderer>("tfrag-trans", (int)id, m_levels.get())));
    }
  }
  {
    auto sprite =
        std::make_unique<CtrSpriteRenderer>("sprite", (int)BucketId::SPRITE, m_vram.get());
    m_sprite = sprite.get();
    set(BucketId::SPRITE, std::move(sprite));
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

// config.ini hitch_ms below 100: the render thread's frames this long are logged with their parts
constexpr double kRenderLongMs = 26.0;
void CtrRenderer::render_frame(const void* ee_mem, u32 chain_offset) {
#ifndef __3DS__
  std::lock_guard<std::mutex> host_gpu(g_host_gpu_mutex);
#endif
  m_rs.ee_mem = (const u8*)ee_mem;
  m_rs.offset_of_s7 = s7.offset;
  m_rs.vram = m_vram.get();
  if (m_rs.frame_idx == 3 && ctr_settings().shadows == CtrSettings::ShadowMode::Full &&
      ctr_gpu_has_stencil()) {
#ifdef __3DS__
    ctr_boot_mark("9a shadow self-test");
#endif
    ctr_shadow_selftest();
#ifdef __3DS__
    ctr_boot_mark("9b shadow self-test done");
#endif
  }
  // No sky renderer yet: clear to the fog color from the default GS registers (the same place
  // OpenGLRenderer reads it), which is close to the sky color at the horizon.
  // (AI-assisted) The last frame's when they can't be read: black would fog everything to black.
  u8 clear[4];
  memcpy(clear, m_rs.fog_color, 4);
  if (m_jak2) {
    // (AI-assisted) Jak II: *fog-color* in the vif0 of the vis packets (bucket 2, like
    // VisDataHandler)
    DmaFollower peek(ee_mem, bucket_tag(chain_offset, (int)jak2::BucketId::BUCKET_2));
    const u32 end = bucket_tag(chain_offset, (int)jak2::BucketId::BUCKET_2 + 1);
    if (peek.current_tag_offset() != end) {
      peek.read_and_advance();
      if (peek.current_tag_offset() != end) {
        auto vis = peek.read_and_advance();
        if (vis.vifcode1().kind == VifCode::Kind::PC_PORT) {
          const u32 v = vis.vif0();
          memcpy(clear, &v, 4);
        }
      }
    }
  } else {
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
  // (AI-assisted) one burst every 15 s: it was every 300 frames or 5 s, two conditions that
  // fired close together (double bursts, which stalled the render thread while logging was
  // synchronous)
  m_rs.log_now = m_rs.last_log_ms == 0 || t0 - m_rs.last_log_ms >= ctr_settings().stats_period * 1000.0;
  if (m_rs.log_now) {
    m_rs.last_log_ms = t0;
    ctr_gpu_want_detailed_stats();  // texture slots and free memory: only for these lines
  }
  memcpy(m_rs.fog_color, clear, 4);
  // (AI-assisted) Always clear to the fog color, also when the sky draws: where nothing covers
  // the screen near the horizon (past the ocean's far edge), black showed on hardware.
#ifdef __3DS__
  if (m_rs.frame_idx == 1 || m_rs.frame_idx == 300) {
    ctr_boot_mark(m_rs.frame_idx == 1 ? "9 first game frame" : "10 300 game frames");
  }
#endif
  double load_ms = 0;
  {
    const double tl = ctr_gpu_time_ms();
    ctr_gpu_diag_stage("level loads (process_pending_loads)");
    m_levels->process_pending_loads(m_rs.frame_idx);
    load_ms = ctr_gpu_time_ms() - tl;
    if (load_ms >= 4.0) {
      m_auto.load_frame = m_rs.frame_idx;  // (AI-assisted, levellod) auto_detail: a load frame
    }
    if (load_ms >= 20.0) {
      lg::info("[ctr] render thread frame {}: {:.0f} ms in level loads", m_rs.frame_idx, load_ms);
    }
  }
  // (AI-assisted) skip_clear: only the depth is cleared when this frame's opaque sky roof covers
  // the whole screen (looking up): every pixel's color is drawn over
  {
    bool covered = false;
    if (ctr_settings().skip_clear && m_sky_draw && ctr_settings().sky && !m_jak2) {
      const u32 base = chain_offset + 16;  // see dispatch_buckets_jak1
      const u32 id = (u32)jak1::BucketId::SKY_DRAW;
      DmaFollower dma(ee_mem, base + 16 * id);
      for (int k = 0; k < 64 && dma.current_tag_offset() != base + 16 * (id + 1); k++) {
        auto data = dma.read_and_advance();
        if (data.vifcode1().kind == VifCode::Kind::PC_PORT &&
            data.size_bytes == CtrSky::kPacketBytes) {
          covered = m_sky_draw->roof_covers_screen(data.data, m_rs);
          break;
        }
      }
    }
    // (AI-assisted, j2gaps) Jak II: a menu opens; bucket 3 copies the last frame for its
    // background before anything is drawn (CtrJak2BlitRenderer)
    if (m_jak2 && ctr_jak2_frame_copies_screen(
                      ee_mem, bucket_tag(chain_offset, (int)jak2::BucketId::BUCKET_3),
                      bucket_tag(chain_offset, (int)jak2::BucketId::BUCKET_3 + 1))) {
      covered = true;
    }
    ctr_gpu_set_skip_color_clear(covered ? 1 : 0);
  }
  ctr_gpu_frame_begin(clear[0], clear[1], clear[2]);
  ctr_gpu_set_cull(ctr_settings().cull_ties ? 1 : 0, ctr_settings().cull_flip ? 1 : 0);
  ctr_gpu_set_frame_overlap2(ctr_settings().frame_overlap2 ? 1 : 0);
  ctr_gpu_set_skin_rows_f24(ctr_settings().skin_rows_f24 ? 1 : 0);         // (AI-assisted)
  ctr_gpu_set_skin_clip_f24(ctr_settings().skin_clip_f24 ? 1 : 0);         // (AI-assisted)
  ctr_gpu_set_fog_alpha(ctr_settings().fog_alpha ? 1 : 0);
  ctr_gpu_set_tex1_gating(ctr_settings().tex1_gating ? 1 : 0);
  ctr_gpu_set_cmd_dedup(ctr_settings().cmd_dedup ? 1 : 0,           // (AI-assisted)
                        ctr_settings().cmd_dedup_draw ? 1 : 0);
  ctr_gpu_set_raw_cmds(ctr_settings().raw_cmds ? 1 : 0);          // (AI-assisted, hud3)
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
  {
    // (AI-assisted) a named screenshot asked for by the pad script (the screenshot folder: config
    // screenshots = N on the 3DS, --ctr-dump on PC)
    std::lock_guard<std::mutex> lock(g_pad_shot_mutex);
    if (!g_pad_shot.empty() && !g_shot_dir.empty()) {
#ifdef __3DS__
      const char* ext = "bmp";
#else
      const char* ext = "png";
#endif
      ctr_gpu_request_screenshot(fmt::format("{}/{}.{}", g_shot_dir, g_pad_shot, ext).c_str());
#ifndef __3DS__
      g_hud_dump_pending = getenv("CTR_HUD_DUMP") != nullptr;  // (AI-assisted, fable1)
#endif
    }
    g_pad_shot.clear();
  }
  // (AI-assisted, j2intro / title3) the screen is black this frame (the game's blackout since the
  // last frame, or held black by draw_pmode_fade): the level draws keep nothing hidden or far for
  // late data
  m_levels->set_game_black(g_pmode_alp.load(std::memory_order_relaxed) < 0.01f, m_hold_black);
  ctr_level_set_screen_black(
      m_hold_black ||
      (m_shown_alp < 0.01f && g_pmode_alp.load(std::memory_order_relaxed) < 0.01f));
  const bool long_frame_log = kperf::g_hitch_diag.load(std::memory_order_relaxed);
  static std::vector<double> bucket_ms_before;
  if (long_frame_log) {
    bucket_ms_before = m_bucket_ms;
  }
  if (m_jak2) {
    dispatch_buckets_jak2(DmaFollower(ee_mem, chain_offset));
    ctr_jak2_menu_hold_draw();
  } else {
    dispatch_buckets_jak1(DmaFollower(ee_mem, chain_offset));
  }
  ctr_gpu_diag_stage("pmode fade");
#ifndef __3DS__
  ctr_gpu_soft_bucket("pmode-fade");
#endif
  ctr_gpu_phase(CTR_PH_HUD);  // (AI-assisted, gpu-rt) gpu_breakdown: the fade with the HUD
  draw_pmode_fade();
  const double t2 = ctr_gpu_time_ms();
  ctr_gpu_frame_end();
  ctr_gpu_diag_stage("after frame_end: stats, logs");
  const double t3 = ctr_gpu_time_ms();
  ctr_gpu_stats gs;
  ctr_gpu_get_stats(&gs);
  m_timing.begin_ms += t1 - t0;
  m_timing.build_ms += t2 - t1;
  m_timing.end_ms += t3 - t2;
  m_timing.gpu_ms += gs.gpu_ms;
  m_timing.gpu_draw_ms += gs.draw_ms;
  m_timing.splits += gs.cmd_splits;
  m_timing.cmd_kb += gs.cmd_bytes / 1024.0;
  m_timing.cmd_kb_peak = std::max(m_timing.cmd_kb_peak, gs.cmd_bytes / 1024.0);
  m_timing.dedup_kb += gs.cmd_dedup_words * 4 / 1024.0;
  m_timing.dropped += gs.dropped_draws;
  // (AI-assisted) a pipelined frame that ran out of command buffer lost its last draws: the
  // normal mode has the whole buffer for one frame
  if (gs.dropped_draws && ctr_settings().pipeline && !m_pipeline_off) {
    ctr_gpu_set_pipeline(0);
    m_pipeline_off = true;
    m_pipeline_calm = 0;
    m_pipeline_retry_frames = std::min(m_pipeline_retry_frames * 2, 30 * 600);
    lg::warn("[ctr] pipeline: {} draws didn't fit in the command buffer, pipeline off for now",
             gs.dropped_draws);
  } else if (m_pipeline_off && ctr_settings().pipeline) {
    // (AI-assisted, j2gaps) one spike frame used to cost the CPU / GPU overlap for the rest of
    // the session: back on after a run of frames whose commands fit well in a pipeline half
    // (60%), waiting twice as long after each overflow; stays off without the linear memory
    const u32 half_bytes = (u32)ctr_gpu_cmd_buffer_kb() * 1024 / 2;
    m_pipeline_calm = gs.cmd_bytes < half_bytes * 6 / 10 ? m_pipeline_calm + 1 : 0;
    if (m_pipeline_calm >= m_pipeline_retry_frames) {
      m_pipeline_calm = 0;
      if (ctr_gpu_set_pipeline(1)) {
        m_pipeline_off = false;
        lg::info("[ctr] pipeline: on again ({} frames fit in half the command buffer)",
                 m_pipeline_retry_frames);
      }
    }
  }
  m_timing.tex_binds += gs.tex_binds;
  m_timing.draws += gs.draws;
  m_timing.gpu_wait_ms += gs.gpu_wait_ms;
  m_timing.queue_drains += gs.queue_drains;  // (AI-assisted, gpu-home)
  m_timing.gpu_busy_ms += gs.gpu_busy_ms;
  m_timing.gpu_busy_frames += gs.gpu_busy_frames;
  m_timing.gpu_active_ms += gs.gpu_active_ms;
  m_timing.color_clears_skipped += gs.color_clear_skipped;
  m_timing.triangles += gs.triangles;
  m_timing.vbuf_peak = std::max(m_timing.vbuf_peak, gs.vbuf_used);
  m_timing.vbuf_size = gs.vbuf_size;
  m_timing.vbuf_dropped += gs.vbuf_dropped;
  m_timing.dead_draws += gs.dead_draws;
  m_timing.frames++;
  // (AI-assisted) the time of day work isn't GPU load: auto_detail counts it only for stepping
  // down (levellod: see auto_detail_frame)
  const double tod_ms = ctr_level_tod_take_ms();
  m_levels->note_frame_ms(t3 - t0);
  if (long_frame_log && t3 - t0 >= kRenderLongMs) {
    std::string by;
    for (size_t i = 0; i < m_buckets.size() && i < bucket_ms_before.size(); i++) {
      const double d = m_bucket_ms[i] - bucket_ms_before[i];
      if (d >= 1.0) {
        by += fmt::format(" {} {:.1f}", m_buckets[i]->name(), d);
      }
    }
    lg::warn("[rhitch] render frame {}: {:.0f} ms = level loads {:.1f} + begin {:.1f} + build "
             "{:.1f} + end {:.1f}; time of day {:.1f}; buckets:{}",
             m_rs.frame_idx, t3 - t0, load_ms, t1 - t0 - load_ms, t2 - t1, t3 - t2, tod_ms, by);
  }
  m_timing.tod_ms += tod_ms;
  m_timing.tod_max_ms = std::max(m_timing.tod_max_ms, tod_ms);
  m_timing.tod_over += tod_ms > 2.0;
  if (ctr_settings().auto_detail) {
#ifndef __3DS__
    // (AI-assisted, title-popin) host test: env CTR_AUTO_DETAIL_MODEL gives auto_detail the GPU time
    // of the hardware fit (hw-nov01e: 16.1 ms + 0.164 ms per 1000 triangles) of this frame's
    // triangles, plus the env's ms (a forced overload)
    static const bool model = getenv("CTR_AUTO_DETAIL_MODEL") != nullptr;
    if (model) {
      static const float extra = (float)atof(getenv("CTR_AUTO_DETAIL_MODEL"));
      const float ms = 16.1f + 0.164f * gs.triangles / 1000.f + extra;
      auto_detail_frame(ms, ms, 1, 0.0, 0.0);
    } else
#endif
    auto_detail_frame(gs.gpu_busy_ms, gs.gpu_active_ms, gs.gpu_busy_frames, t2 - t1 - tod_ms,
                      t2 - t1);
  }
  if (!ctr_settings().gpu_profile && ctr_settings().gpu_counters) {
    hw_counters_frame();
  }
  if (ctr_settings().gpu_profile) {
    if (!m_profile_active && !m_profile_level.empty() && ctr_gpu_time_ms() >= m_profile_arm_ms &&
        std::find(m_profiled.begin(), m_profiled.end(), m_profile_level) == m_profiled.end()) {
      m_profile_active = true;
      m_profile_ms.clear();
      m_profile_mode = 0;
      m_profile_frames = 0;
      m_profile_sum = 0;
      m_profile_count = 0;
      m_profile_cycles = 0;
      lg::info("[ctr] gpu profile: measuring {} for {:.0f} s (parts of the picture vanish in turns)",
               m_profile_level, 2 * kProfileModeCount * kProfileModeMs / 1000.0);
    }
    if (m_profile_active) {
      // (AI-assisted) the fence's GPU time when there is one (citro3d's misses most of a
      // pipelined frame)
      profile_frame(gs.gpu_busy_frames ? gs.gpu_busy_ms / gs.gpu_busy_frames : gs.draw_ms);
    }
  }
  if (m_rs.log_now) {
    const auto& vs = m_vram->stats();
    lg::debug("[ctr] frame {}: {} draws {} tris, vram uploads {} (written {}, chunks {} written, {} "
              "already there), relocates skipped {}, textures decoded {} (reused {}), gpu "
              "textures {} ({} KB), linear free {} KB, vram free {} KB ({} vram textures, {} "
              "failed copies)",
              m_rs.frame_idx, gs.draws, gs.triangles, vs.uploads, vs.uploads_changed,
              vs.chunks_written, vs.chunks_skipped, vs.relocates_skipped, vs.decoded, vs.revived,
              gs.textures, gs.tex_bytes / 1024, gs.linear_free / 1024, gs.vram_free / 1024,
              gs.vram_textures, gs.vram_copy_failures);
    if (m_timing.frames) {
      const double n = m_timing.frames;
      // (AI-assisted) gpu-wait: in C3D_FrameBegin (part of wait-gpu, or of submit with the
      // pipeline); busy: the GPU's time per frame from the steady pacing fence (the "draw" time
      // misses what was handed over in the middle of a frame)
      const std::string busy =
          m_timing.gpu_busy_frames
              ? fmt::format("{:.2f} ({} frames; active {:.2f})",
                            m_timing.gpu_busy_ms / m_timing.gpu_busy_frames,
                            m_timing.gpu_busy_frames,
                            m_timing.gpu_active_ms / m_timing.gpu_busy_frames)
              : std::string("n/a (needs frame_pacing = steady)");
      lg::info(
          "[ctr] render ms/frame over {} frames: wait-gpu {:.2f}, build {:.2f}, submit {:.2f} "
          "(cpu total {:.2f}; gpu-wait {:.2f}); gpu {:.2f} (draw {:.2f}, busy {}); {:.0f} "
          "triangles, {:.0f} draws, {:.0f} texture binds, {:.0f} KB commands (peak {:.0f} of {} KB; {:.1f} KB left "
          "out by cmd_dedup); vertex buffer peak {} of {} KB{}{}{}{}",
          m_timing.frames, m_timing.begin_ms / n, m_timing.build_ms / n, m_timing.end_ms / n,
          (m_timing.begin_ms + m_timing.build_ms + m_timing.end_ms) / n,
          m_timing.gpu_wait_ms / n, m_timing.gpu_ms / n, m_timing.gpu_draw_ms / n, busy,
          m_timing.triangles / n, m_timing.draws / n, m_timing.tex_binds / n,
          m_timing.cmd_kb / n, m_timing.cmd_kb_peak, ctr_gpu_cmd_buffer_kb(), m_timing.dedup_kb / n,
          m_timing.vbuf_peak / 1024,
          m_timing.vbuf_size / 1024,
          m_timing.vbuf_dropped
              ? fmt::format(", {} draws DROPPED (vertex buffer full)", m_timing.vbuf_dropped)
              : std::string(),
          (m_timing.dropped ? fmt::format(", {} draws DROPPED (command buffer full)",
                                          m_timing.dropped)
                            : std::string()) +
              (m_timing.dead_draws
                   ? fmt::format(", {} draws SKIPPED (mesh not there)", m_timing.dead_draws)
                   : std::string()),
          m_timing.color_clears_skipped
              ? fmt::format(", {} frames without the color clear (skip_clear)",
                            m_timing.color_clears_skipped)
              : std::string(),
          // (AI-assisted, gpu-home) frame_overlap2 = on: drains of a full GX queue (gpu-wait)
          m_timing.queue_drains
              ? fmt::format(", {} GX queue drains (queue full)", m_timing.queue_drains)
              : std::string());
      lg::info("[ctr] textures: {} level pools {} KB, {} in VRAM ({} KB); linear free {} KB "
               "(largest block {} KB), VRAM free {} KB; {} compact textures {} KB ({} in reserved "
               "VRAM); last frame: {} level draws without the fog stage, {} with the fog in the vertex alpha ({} texture unit 1 switches); {} program switches, {} with a shader upload, {} uniform vectors at switches",
               gs.pools, gs.pool_bytes / 1024, gs.pools_in_vram, gs.pool_vram_bytes / 1024,
               gs.linear_free / 1024, gs.linear_largest / 1024, gs.vram_free / 1024,
               gs.compact_textures, gs.compact_bytes / 1024, gs.compact_in_vram,
               gs.nofog_draws,
               gs.fog_alpha_draws, gs.unit1_toggles, gs.prog_switches, gs.prog_uploads,
               gs.prog_unif_vectors);
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
      // (AI-assisted) of that, the screen copies' (the distorter's: the command list sent so far,
      // then the copy)
      const double copy_ms = ctr_gpu_take_screen_copy_ms();
      if (copy_ms / n >= 0.005) {
        by_name += fmt::format(" (screen copies {:.2f})", copy_ms / n);
      }
      lg::info("[ctr] build ms/frame by renderer:{}", by_name);
      if (m_timing.tod_max_ms > 0) {
        // (AI-assisted) of the level renderers' time: the time of day's
        lg::info("[ctr] time of day ms/frame {:.2f}, most {:.2f} in one frame, {} frames over 2 ms",
                 m_timing.tod_ms / n, m_timing.tod_max_ms, m_timing.tod_over);
      }
      if (const auto sw = m_levels->take_tod_sweep_stats(); sw.frames) {
        lg::info("[ctr] time of day catch-up of what isn't drawn: {} vertices in {} frames, "
                 "{:.2f} ms in each, most {:.2f} (before the frame, not in its build)",
                 sw.verts, sw.frames, sw.ms / sw.frames, sw.max_ms);
      }
      {
        // (AI-assisted, gpu-rt) gpu_breakdown, gpu_micro's statistics
        char line[2048];
        if (ctr_gpu_breakdown_line(line, sizeof(line),
                                    m_profile_level.empty() ? "-" : m_profile_level.c_str()) > 0) {
          lg::info("{}", line);
        }
        if (ctr_gpu_micro_line(line, sizeof(line)) > 0) {
          lg::info("{}", line);
        }
      }
#ifdef __3DS__
      if (ctr_settings().gpu_perflog) {
        char pl[512];
        if (ctr_gpu_perflog_line(pl, sizeof(pl), m_timing.frames, m_timing.gpu_draw_ms,
                                 m_timing.gpu_busy_ms, m_timing.gpu_busy_frames) > 0) {
          lg::info("{}", pl);
        }
      }
#endif
      if (ctr_settings().auto_detail && m_auto.init) {
        const auto& s = ctr_settings();
        const auto& c = m_auto.ctl;
        const float t = std::max(c.x, 0.f);
        // the far ring's distances at x (above the config: x > 0)
        const float tie_t = s.tie_distance_scale > 0
                                ? s.tie_distance_scale + (std::max(s.tie_distance_scale_max,
                                                                   s.tie_distance_scale) -
                                                          s.tie_distance_scale) * t
                                : 0.f;
        const auto& rl = m_auto.relief;
        const float r = c.r();
        const auto kn = rl.at(r);
        const auto& tm = m_auto.time.ms;
        auto secs = [&](int i) {
          return fmt::format("{:.1f}/{:.1f}/{:.1f} s", tm[i][0] / 1000.0, tm[i][1] / 1000.0,
                             tm[i][2] / 1000.0);
        };
        const float ties = r > 0 ? kn.tie_distance_scale : tie_t;
        lg::info("[ctr] auto detail x {:.2f} (ceiling {:.2f}, mean {:.1f} ms, band {:.0f} +- {:.0f}; "
                 "{:.1f} s above the config): ties {}, shrubs x{:.2f}; relief {:.2f} of {}: "
                 "tie_lod_px {:.1f} ({}), lod_distance {} ({}), tie limit ({}){} at the config / "
                 "between / old value; {} steps down ({} for the render thread), {} up",
                 c.x, c.ceil, c.mean, s.auto_detail_gpu_ms, ctr_detail::Controller::kBand,
                 m_auto.above_ms / 1000.0,
                 ties > 0 ? fmt::format("x{:.2f}", ties) : std::string("none"),
                 s.shrub_scale + (std::max(s.shrub_scale_max, s.shrub_scale) - s.shrub_scale) * t,
                 r, rl.n, kn.tie_lod_px, secs(ctr_detail::kTieLodPx),
                 kn.lod_distance > 0 ? fmt::format("{:.0f} m", kn.lod_distance) : std::string("off"),
                 secs(ctr_detail::kLodDistance), secs(ctr_detail::kTieDistance),
                 rl.stage[ctr_detail::kShrubFar] < 0
                     ? std::string()
                     : fmt::format(", shrubs x{:.2f} ({}), ties by their vista copies from {} ({})",
                                   kn.shrub_far, secs(ctr_detail::kShrubFar),
                                   kn.tie_far > 0 ? fmt::format("{:.0f} m", kn.tie_far) : "-",
                                   secs(ctr_detail::kTieFar) + " / " + secs(ctr_detail::kTieFar2)),
                 c.sheds, c.sheds_cpu, c.restores);
        m_auto.ctl.sheds = m_auto.ctl.sheds_cpu = m_auto.ctl.restores = 0;
        m_auto.time = ctr_detail::KnobTime();
        m_auto.above_ms = 0;
      }
    }
    m_timing = Timing();
  }
  m_rs.frame_idx++;
}

void CtrRenderer::prepare_frame(const void* ee_mem, u32 chain_offset) {
  // game thread, before handing the frame to the render thread: copy what the frame needs from
  // game memory that is not double buffered (merc bone matrices, (AI-assisted) the sprite arrays)
  CtrRenderState rs;
  rs.ee_mem = (const u8*)ee_mem;
  rs.offset_of_s7 = s7.offset;
  rs.frame_idx = m_rs.frame_idx;  // (AI-assisted, keira3) the frame drawn next (Keira's pose)
  const u32 base = chain_offset + 16;  // see dispatch_buckets_jak1
  for (auto& [id, merc] : m_merc) {
    rs.next_bucket = bucket_tag(chain_offset, id + 1);
    DmaFollower dma(ee_mem, bucket_tag(chain_offset, id));
    merc->snapshot_bones(dma, rs);
  }
  prepare_sprites(ee_mem, chain_offset);
  prepare_textures(ee_mem, chain_offset);
}

void CtrRenderer::prepare_sprites(const void* ee_mem, u32 chain_offset) {
  // (AI-assisted) see CtrSpriteRenderer::snapshot
  if (!m_sprite) {
    return;
  }
  CtrRenderState rs;
  rs.ee_mem = (const u8*)ee_mem;
  rs.offset_of_s7 = s7.offset;
  const int id = m_sprite_bucket;
  rs.next_bucket = bucket_tag(chain_offset, id + 1);
  DmaFollower dma(ee_mem, bucket_tag(chain_offset, id));
  m_sprite->snapshot(dma, rs);
}

#if defined(__3DS__) && !defined(OG3DS_GAME_JAK2)
// (AI-assisted) the Jak 1 3DS build has no Jak II tables (CtrRendererJak2.cpp)
void CtrRenderer::prepare_textures(const void*, u32) {}
void CtrRenderer::tex_frame_done_jak2() {}
bool ctr_jak2_frame_copies_screen(const void*, u32, u32) {
  return false;
}
void ctr_jak2_menu_hold_draw() {}
#endif

u32 CtrRenderer::bucket_tag(u32 chain_offset, int id) const {
  // Jak 1: a call to the default registers first (see dispatch_buckets_jak1); Jak II: none
  return chain_offset + (m_jak2 ? 0 : 16) + 16 * id;
}

void CtrRenderer::hw_counters_frame() {
  // (AI-assisted) The GPU's own counters (ctr_gpu_hwprof_*: busy % of each pipeline stage, memory
  // traffic; read and written through the GSP service) over every stats window: one line per
  // window, logged with the window's other lines (m_rs.log_now), labelled with the level. The
  // stage samples stop kHwCountersMs into the window (16-bit counts); nothing is left out of the
  // picture. A refusal by the GSP is logged once and ends it.
  if (g_hw_refusal_logged) {
    return;
  }
  const double now = ctr_gpu_time_ms();
  if (m_hw_active && !m_rs.log_now && now - m_hw_start_ms >= kHwCountersMs && !m_hw_stages_done) {
    m_hw_stages_done = true;
    if (ctr_gpu_hwprof_stop_stages() != 0) {
      g_hw_refusal_logged = true;
      g_hw_measuring = m_hw_active = false;
      lg::warn("[ctr] gpu counters: the GSP refused the hotspot read, counters off");
    }
    return;
  }
  if (!m_rs.log_now) {
    return;
  }
  if (m_hw_active) {
    hw_profile_end(fmt::format("window {}", m_profile_level.empty() ? "-" : m_profile_level));
  }
  hw_profile_begin();
  m_hw_active = g_hw_measuring;
  m_hw_start_ms = now;
  m_hw_stages_done = false;
}

bool CtrRenderer::profile_skips(const std::string& name) const {
  for (const char* n : kProfileModes[m_profile_mode].skip) {
    if (n && name == n) {
      return true;
    }
  }
  return false;
}

void CtrRenderer::note_levels(const std::vector<std::string>& levels) {
  // (AI-assisted, levellod) auto_detail: the title screen's fast day (auto_detail_frame)
  m_title_loaded = std::find(levels.begin(), levels.end(), "title") != levels.end();
  // the first level that isn't the title screen is the one being played
  std::string lev;
  for (const auto& l : levels) {
    if (!l.empty() && l != "title" && l != "none" && l != "#f") {
      lev = l;
      break;
    }
  }
  if (lev != m_profile_level) {
    m_auto.area = true;
    if (m_profile_active) {
      // a level change in the middle: stop (draw everything), measure the new one later
      m_profile_active = false;
      m_profile_mode = 0;
    }
    m_profile_level = lev;
    m_profile_arm_ms = ctr_gpu_time_ms() + 10000.0;  // after the load and the level start
  }
}

// config.ini auto_detail: the controller (CtrAutoDetail.h) follows the GPU's time from the frame's
// command list on (active) and the render thread's build with the time of day, frame by frame;
// a level load's frames and a level's first ~3 s of draws (their time of day rewrites) don't count
void CtrRenderer::auto_detail_frame(float /*gpu_busy_ms*/, float gpu_active_ms,
                                    int gpu_busy_frames, double /*build_ms*/, double build_tod_ms) {
  const auto& cs = ctr_settings();
  const double now = ctr_gpu_time_ms();
  if (!m_auto.init) {
    m_auto.init = true;
    const bool t_range =
        cs.shrub_scale_max > cs.shrub_scale ||
        (cs.tie_distance_scale > 0 && cs.tie_distance_scale_max > cs.tie_distance_scale);
    m_auto.relief.init(cs.tie_lod_px, cs.lod_distance, cs.tie_distance_scale, cs.draw_distance,
                       g_game_version == GameVersion::Jak1, g_game_version == GameVersion::Jak2);
    // fixed: the relief's floor is the position, the controller has no range to move in
    const float relief_floor = -(float)m_auto.relief.n;
    m_auto.ctl.range(relief_floor, cs.auto_detail_fixed ? relief_floor : (t_range ? 1.f : 0.f));
    if (g_game_version == GameVersion::Jak1) {
      m_auto.ctl.unit_costs(m_auto.relief, ctr_detail::kJak1LeverMs);
    }
    ctr_level_set_detail(m_auto.ctl.x, cs.auto_detail_fixed);
    m_auto.last_ms = now;
  }
  m_auto.time.add(m_auto.relief, m_auto.ctl.r(), now - m_auto.last_ms);
  m_auto.above_ms += m_auto.ctl.x > 0 ? now - m_auto.last_ms : 0.0;
  m_auto.last_ms = now;
#ifndef __3DS__
  // host, env CTR_DETAIL_FORCE=X: the position pinned at X (lever measurements)
  if (const char* fx = getenv("CTR_DETAIL_FORCE")) {
    const float x = std::clamp((float)atof(fx), m_auto.ctl.lo, m_auto.ctl.hi);
    if (m_auto.ctl.x != x || m_rs.frame_idx % 30 == 0) {
      m_auto.ctl.x = x;
      ctr_level_set_detail(x, true);
    }
    return;
  }
#endif
  // Jak II, linear memory short (CtrLevels::need_update): the position stays under its cap. A
  // ceiling already under the cap (a shed's, the relief's) rises by its own rule, up to the cap
  if (const float cap = ctr_level_mem_cap(); cap < 1.f) {
    if (m_auto.ctl.ceil >= cap) {
      m_auto.ctl.ceil = cap;
      m_auto.ctl.ceil_frame = m_auto.ctl.frame;
    }
    if (m_auto.ctl.x > cap) {
      m_auto.ctl.x = cap;
      ctr_level_set_detail(cap);
    }
  }
  if (!gpu_busy_frames) {
    return;  // no GPU time this frame (frame_pacing = fast): the detail stays
  }
  // (AI-assisted, citadel) the target and margins are for fps_cap = 30's 33 ms frame
  const double k = cs.fps_cap >= 60 ? 0.5 : 1.0;
  const double target = cs.auto_detail_gpu_ms * k;
  const bool load = m_auto.load_frame && m_rs.frame_idx <= m_auto.load_frame + 2;
  bool moved = false, snap = false;
  // the title's relief (its late flight) isn't carried into play: the level starts at the config
  if (m_auto.title && !m_title_loaded && m_auto.ctl.x != 0) {
    m_auto.ctl.reset();
    moved = snap = true;
  }
  m_auto.title = m_title_loaded;
  if (m_auto.area) {
    m_auto.area = false;
    m_auto.ctl.new_area();
  }
  const bool settled = !m_auto.load_frame || m_rs.frame_idx > m_auto.load_frame + 90;
  moved = m_auto.ctl.step((double)gpu_active_ms / gpu_busy_frames, build_tod_ms, !load, target, k,
                          settled, snap ? 1e9f : ctr_level_detail()) ||
          moved;

  if (moved) {
    ctr_level_set_detail(m_auto.ctl.x, snap);
  }
  if (m_jak2 && m_auto.relief.n > 0 && !cs.auto_detail_fixed) {
    // Jak II's relief (the far levers, under the config's detail): its begin, each lever as it
    // starts to move, its end
    const float r = m_auto.ctl.r();
    const int n = m_auto.relief.n;
    if (r > 0 && !m_auto.relief_on) {
      m_auto.relief_on = true;
      m_auto.relief_t0 = now;
      m_auto.relief_deep = 0;
      m_auto.relief_logged = 0;
      lg::info("[ctr] detail relief on: load {:.1f} ms a frame against {:.0f} + {:.0f} with the "
               "config's detail",
               m_auto.ctl.mean, target, ctr_detail::Controller::kBand * k);
    }
    if (m_auto.relief_on) {
      m_auto.relief_deep = std::max(m_auto.relief_deep, r);
      if (r > (float)m_auto.relief_logged && m_auto.relief_logged < n) {
        m_auto.relief_logged++;
        const auto kn = m_auto.relief.at((float)m_auto.relief_logged);
        lg::info("[ctr] detail relief level {} of {} begun (at its end: shrub fade bands x{:.2f} "
                 "beyond {:.0f} m, ties by their vista copies from {})",
                 m_auto.relief_logged, n, kn.shrub_far, ctr_detail::kNearRingMeters,
                 kn.tie_far > 0 ? fmt::format("{:.0f} m", kn.tie_far)
                                : std::string("the draw distance"));
      }
      if (r <= 0) {
        m_auto.relief_on = false;
        lg::info("[ctr] detail relief off after {:.0f} s (deepest level {:.2f} of {})",
                 (now - m_auto.relief_t0) / 1000.0, m_auto.relief_deep, n);
      }
    }
  }
#ifndef __3DS__
  // host: env CTR_DETAIL_LOG, the controller every 30 frames
  static const bool dlog = getenv("CTR_DETAIL_LOG") != nullptr;
  if (dlog && m_rs.frame_idx % 30 == 0) {
    lg::info("[detail] f={} x={:.2f} ceil={:.2f} mean={:.1f} busy={:.1f} build={:.1f}",
             m_rs.frame_idx, m_auto.ctl.x, m_auto.ctl.ceil, m_auto.ctl.mean,
             (double)gpu_active_ms / gpu_busy_frames, build_tod_ms);
  }
#endif
}

void CtrRenderer::profile_frame(double gpu_draw_ms) {
  const double now = ctr_gpu_time_ms();
  if (m_profile_ms.empty()) {
    m_profile_ms.assign(kProfileModeCount, -1.0);
    m_profile_start = now;
    hw_profile_begin();
  }
  // the GPU time of the frame before this one: not the first frames of a mode
  if (++m_profile_frames > kProfileSettleFrames) {
    m_profile_sum += gpu_draw_ms;
    m_profile_count++;
  }
  if (now - m_profile_start < kProfileModeMs) {
    return;
  }
  m_profile_ms[m_profile_mode] = m_profile_count ? m_profile_sum / m_profile_count : -1.0;
  if (m_profile_mode == 0 && m_profile_cycles == 0) {
    hw_profile_end(m_profile_level);  // the first window with everything drawn
  }
  m_profile_mode = (m_profile_mode + 1) % kProfileModeCount;
  m_profile_start = now;
  m_profile_frames = 0;
  m_profile_sum = 0;
  m_profile_count = 0;
  if (m_profile_mode == 0) {
    // a whole cycle: how much GPU time each part takes (all - without it)
    std::string line =
        fmt::format("[ctr] gpu profile {}: all {:.1f} ms", m_profile_level, m_profile_ms[0]);
    for (int i = 1; i < kProfileModeCount; i++) {
      if (m_profile_ms[i] >= 0 && m_profile_ms[0] >= 0) {
        line += fmt::format(", {} {:.1f}", kProfileModes[i].label, m_profile_ms[0] - m_profile_ms[i]);
      }
    }
    lg::info("{}", line);
    // (AI-assisted) two whole cycles (35 s) measure it: then stop leaving renderers out. Left on
    // in a config.ini by mistake, it made the level, models, ocean or sky vanish every 2.5 s for
    // the whole game ("polygons loading and unloading").
    if (++m_profile_cycles >= 2) {
      m_profile_active = false;
      m_profiled.push_back(m_profile_level);
      lg::info("[ctr] gpu profile {}: done, drawing everything again", m_profile_level);
    }
  }
}

// (AI-assisted) The PS2 blends the frame with black by PMODE alpha (the PC renderer multiplies
// the frame by it): the game's fades and blackouts, e.g. at boot before the logo and while a level
// loads. Drawn last, a black screen-sized quad with alpha 1 - alp.
void CtrRenderer::draw_pmode_fade() {
  float alp = std::clamp(g_pmode_alp.load(std::memory_order_relaxed), 0.f, 1.f);
  {
    alp = hold_black(alp);  // (AI-assisted, j2intro; Jak 1: title3)
  }
  if (alp >= 0.999f) {
    return;
  }
  ctr_draw_state st;
  memset(&st, 0, sizeof(st));
  st.tex = -1;
  st.blend = CTR_BLEND_ALPHA;
  st.atest = CTR_TEST_ALWAYS;
  st.ztest = CTR_TEST_ALWAYS;
  st.zwrite = 0;
  // untextured alpha: 0x80 = 1 (the texture stage doubles it)
  const u8 a = (u8)std::min(128.f, (1.f - alp) * 128.f + 0.5f);
  ctr_vertex v[6];
  const float xy[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
  for (int i = 0; i < 6; i++) {
    memset(&v[i], 0, sizeof(v[i]));
    v[i].x = xy[i][0];
    v[i].y = xy[i][1];
    v[i].z = 1.f;
    v[i].a = a;
  }
  ctr_gpu_draw(&st, v, 6);
}

// A screen coming out of black (the game's blackout before the title intro, a continue, a level
// start) stays black while, within 3 s of a level shown or of a camera cut, ring 0 in view is
// missing (CtrLevels::note_late_in_view: tie LOD groups within 40 m drawn far for want of their
// near chunks, chunks not there at all), at most kMaxHoldMs and while it gets fewer within
// kStallMs. The game runs on: only the picture waits. A continue's own wait for the same
// (ctr_arrival_ready) comes first; then this one stays out.
// Jak II: what is missing comes through the loader. With nothing queued, being read or waiting to
// be taken for kIdleFrames frames in a row the rest can't come now (the stream arena given back
// or full, no linear memory for a chunk): neither wait goes on for it.
// Jak 1: a black of kLongBlackMs or more (an arrival) fades in over kFadeInMs.
float CtrRenderer::hold_black(float alp) {
  const bool old = !m_jak2 && ctr_arrival_off();  // (host A/B: Jak 1 as before)
  const double kMaxHoldMs = old ? 1500.0 : 4000.0;
  const double kStallMs = old ? 1500.0 : 2500.0;
  constexpr double kLongBlackMs = 400.0, kFadeInMs = 500.0;
  constexpr u32 kIdleFrames = 12;
  bool shown_or_cut = false;
  u32 ring0 = 0;
  const u32 all = m_levels->late_in_view(m_rs.frame_idx, &shown_or_cut, &ring0);
  const u32 late = old ? all : ring0;
  // (frames in a row with no less missing and nothing coming)
  m_hold_idle =
      m_jak2 && late && late >= m_hold_idle_late && !m_levels->coming() ? m_hold_idle + 1 : 0;
  m_hold_idle_late = late;
  const bool idle = m_hold_idle >= kIdleFrames;
  g_arrival_late.store(late, std::memory_order_relaxed);
  g_arrival_idle.store(idle, std::memory_order_relaxed);
  g_arrival_frame.fetch_add(1, std::memory_order_relaxed);
  const double now = ctr_gpu_time_ms();
  // (not again while fading in, nor right after the game's own wait for the same)
  const bool game_waited = !old && now - g_arrival_done_ms.load() < 1000.0;
  if (!m_hold_black && !m_hold_ramp && !game_waited && m_shown_alp < 0.01f && alp >= 0.01f &&
      late > 0 && shown_or_cut) {
    m_hold_black = true;
    m_hold_t0 = now;
    m_hold_late = 0;
    m_hold_frames = 0;
    m_hold_least = late;
    m_hold_progress_t = now;
  }
  if (m_hold_black) {
    m_hold_late = std::max(m_hold_late, late);
    if (late < m_hold_least) {
      m_hold_least = late;
      m_hold_progress_t = now;
    }
    if (late == 0 || now - m_hold_t0 >= kMaxHoldMs || now - m_hold_progress_t >= kStallMs ||
        idle) {
      m_hold_black = false;
      m_hold_ramp = true;
      m_fade_t0 = now;
      lg::info("[ctr] screen kept black {:.0f} ms ({} frames) for what is missing in view{} (at most "
               "{} tie LOD groups within ring 0 drawn far / chunks missing, {} left)",
               now - m_hold_t0, m_hold_frames,
               !late ? "" : idle ? ", given up: nothing more can come now" : ", given up",
               m_hold_late, late);
    } else {
      m_hold_frames++;
      alp = 0.f;
    }
  }
  if (!m_jak2 && !old) {
    if (m_shown_alp < 0.01f && m_black_t0 <= 0) {
      m_black_t0 = now;
    }
    if (!m_hold_ramp && !m_hold_black && alp >= 0.01f && m_shown_alp < 0.01f &&
        now - m_black_t0 >= kLongBlackMs) {
      m_hold_ramp = true;
      m_fade_t0 = now;
    }
  }
  if (m_hold_ramp && !m_hold_black) {
    const float game_alp = alp;
    alp = m_jak2 || old
              ? std::min(alp, m_shown_alp + 0.125f)
              : std::min(alp, std::max(m_shown_alp, (float)((now - m_fade_t0) / kFadeInMs)));
    m_hold_ramp = alp < game_alp;
  }
  if (alp >= 0.01f) {
    m_black_t0 = 0;
  }
  m_shown_alp = alp;
  return alp;
}

// (find_menu_page)
std::atomic<u32> g_menu_page{0};

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
  // (AI-assisted) the visibility strings come with bucket tfrag-0 every frame
  m_rs.vis_valid[0] = m_rs.vis_valid[1] = false;
  m_rs.vis_packets = 0;
  run_buckets(dma, (size_t)jak1::BucketId::ALPHA_TEX_LEVEL0, 0);
  m_vram->prewarm(2.0);  // the boot sprite and HUD textures, before their first draw
  if (!m_menu_page_done) {
    const u32 page = g_menu_page.load(std::memory_order_relaxed);
    m_menu_page_done = page && m_vram->prewarm_page((const u8*)g_ee_main_mem + page, -2,
                                                    (const u8*)g_ee_main_mem, m_rs.offset_of_s7, 2.0);
  }
}

void CtrRenderer::dispatch_buckets_jak2(DmaFollower dma) {
  // (AI-assisted) as OpenGLRenderer::dispatch_buckets_jak2: bucket 0 is the start of the chain
  m_rs.buckets_base = dma.current_tag_offset();
  m_rs.next_bucket = m_rs.buckets_base + 16;
  m_rs.default_regs_buffer = 0xffffffff;  // none
  if (m_bucket_ms.size() != m_buckets.size()) {
    m_bucket_ms.assign(m_buckets.size(), 0.0);
  }
  m_rs.vis_valid[0] = m_rs.vis_valid[1] = false;
  m_rs.vis_packets = 0;
  const bool ok = run_buckets(dma, (size_t)jak2::BucketId::TEX_L0_ALPHA, 1);
  tex_frame_done_jak2();
  if (!ok) {
    return;
  }
  if (m_rs.call_vif_callback) {
    vif_interrupt_callback(m_buckets.size());
  }
}

bool CtrRenderer::run_buckets(DmaFollower& dma, size_t sky_hook_bucket, int callback_offset) {
  const bool profiling = m_profile_mode > 0;
  if (m_bucket_phase.size() != m_buckets.size()) {
    // (AI-assisted, gpu-rt) gpu_breakdown: the GPU phase of each bucket renderer (-1: keeps the
    // phase before it, no draws of its own)
    m_bucket_phase.assign(m_buckets.size(), -1);
    for (size_t i = 0; i < m_buckets.size(); i++) {
      const std::string& n = m_buckets[i]->name();
      m_bucket_phase[i] =
          n == "sky"                                                     ? CTR_PH_SKY
          : n.find("tfrag") != std::string::npos && n != "tfrag-trans"   ? CTR_PH_LEVEL
          : n == "merc" || n == "eyes"                                   ? CTR_PH_MERC
          : n == "sky-blend" || n == "tfrag-trans"                       ? CTR_PH_TRANS
          : n == "shadow"                                                ? CTR_PH_SHADOW
          : n == "ocean"                                                 ? CTR_PH_OCEAN
          : n == "sprite"                                                ? CTR_PH_SPRITE
          : n == "debug" || n == "debug-no-zbuf" || n == "subtitle"      ? CTR_PH_HUD
                                                                         : -1;
    }
  }
  for (size_t bucket_id = 0; bucket_id < m_buckets.size(); bucket_id++) {
    if (m_bucket_phase[bucket_id] >= 0) {
      ctr_gpu_phase(m_bucket_phase[bucket_id]);  // (AI-assisted, gpu-rt) gpu_breakdown
    }
    if (bucket_id >= sky_hook_bucket) {
      // (AI-assisted) sky_last: at the latest before the translucent buckets (and the sky blend
      // in tfrag-trans, which starts this frame's sky textures)
      ctr_gpu_run_sky_hook();
    }
    // (AI-assisted, j2gpu) the clock read only around buckets with data (Jak II has 327 buckets,
    // most of them empty every frame: two system calls and two 64-bit to double conversions each)
    const bool timed = dma.current_tag_offset() != m_rs.next_bucket;
    const double tb = timed ? ctr_gpu_time_ms() : 0.0;
    // (AI-assisted) shadows not full: the shadow bucket left out
    const bool ab_skip = ctr_settings().shadows != CtrSettings::ShadowMode::Full &&
                         m_buckets[bucket_id]->name() == "shadow";
#ifndef __3DS__
    // (AI-assisted) host diagnosis: env CTR_SKIP_BUCKETS=",id,id," leaves those buckets out
    static const char* skip_ids = getenv("CTR_SKIP_BUCKETS");
    const bool env_skip =
        skip_ids && strstr(skip_ids, fmt::format(",{},", bucket_id).c_str()) != nullptr;
#else
    const bool env_skip = false;
#endif
    if ((profiling && profile_skips(m_buckets[bucket_id]->name())) || ab_skip || env_skip) {
      while (dma.current_tag_offset() != m_rs.next_bucket) {
        dma.read_and_advance();
      }
    } else {
#ifndef __3DS__
      ctr_gpu_soft_bucket(m_buckets[bucket_id]->name().c_str());
#endif
      m_buckets[bucket_id]->render(dma, m_rs);
    }
    if (timed) {
      m_bucket_ms[bucket_id] += ctr_gpu_time_ms() - tb;
    }
    ctr_gpu_submit_partial();  // config.ini overlap: the GPU starts on what's built so far
    if (dma.current_tag_offset() != m_rs.next_bucket) {
      lg::error("[ctr] bucket {} ({}) did not end at the next bucket", bucket_id,
                m_buckets[bucket_id]->name());
      return false;
    }
    m_rs.next_bucket += 16;
    if (m_rs.call_vif_callback) {
      vif_interrupt_callback(bucket_id + callback_offset);
    }
  }
  return true;
}

// ---------------------------------------------------------------------------
// GfxRendererModule
// ---------------------------------------------------------------------------

namespace {
std::unique_ptr<CtrRenderer> g_ctr;
u32 g_frame_idx = 0;
std::atomic<bool> g_ctr_ready{false};  // g_ctr can take prefetches (ctr_level_dgo_started)

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
  double max_frame_ms = 0;   // longest frame in the log window
} g_ee;
// frames this long are logged one by one (hangs: level loads, SD card reads)
constexpr double kLongFrameMs = 200.0;
double g_wait_render_total = 0;  // never reset: the wait in one frame is the difference
double g_wait_render_mark = 0;
double g_upload_total = 0, g_upload_mark = 0;  // same for texture uploads / relocates
int g_upload_count_total = 0, g_upload_count_mark = 0;

// game thread: the level list of ctr_set_levels until the frame's hand-over
std::vector<std::string> g_levels_pending;
bool g_levels_pending_set = false;

void apply_levels(const std::vector<std::string>& levels) {
  kperf::SectionScope section("set-levels");
  g_ctr->levels().set_wanted(levels);
  g_ctr->note_levels(levels);
  CtrOceanFull::note_levels(levels);  // (AI-assisted) its log: once per level
}

void render_job(void*) {
  g_ctr->render_frame(g_job_mem, g_job_offset);
}

// before touching renderer state from the game thread
void wait_render_idle() {
  if (g_async) {
    kperf::SectionScope section("wait-render-idle");
    const double w = ctr_gpu_async_wait();
    g_ee.wait_render_ms += w;
    g_wait_render_total += w;
  }
}

int ctr_init(GfxGlobalSettings& /*settings*/) {
  ctr_gpu_set_steady_pacing(ctr_settings().steady_pacing ? 1 : 0);
  // (AI-assisted) hardware hang bisect switches (config.ini, CtrSettings.h)
  ctr_gpu_set_asm_tex(ctr_settings().asm_tex ? 1 : 0);
  ctr_gpu_set_defer_flush(ctr_settings().defer_flush ? 1 : 0);
  lg::info("[ctr] diag: defer_flush {}", ctr_settings().defer_flush ? "on" : "off");
  if (ctr_gpu_init() != 0) {
    lg::error("[ctr] GPU init failed");
    return 1;
  }
  {
    char target[256];
    if (ctr_gpu_describe_target(target, sizeof(target)) > 0) {
      lg::info("[ctr] top render target: {}", target);
    }
  }
#ifdef __3DS__
  ctr_boot_mark("8 GPU ready (first top screen frame drawn)");
  if (ctr_settings().pica_bench) {
    // (AI-assisted) the GPU microbenchmark, before anything else uses the GPU
    lg::info("[ctr] pica_bench: GPU microbenchmark before the game starts");
    ctr_boot_mark("8b pica_bench (a hang here: see data/log/bench.txt)");
    const int stopped = ctr_gpu_bench_run();
    lg::info("[ctr] pica_bench: {}", stopped ? "STOPPED (the GPU didn't finish a test)" : "done");
  }
#endif
  ctr_gpu_set_rgba4_as_rgba8(ctr_settings().rgba4_as_rgba8 ? 1 : 0);
  // (AI-assisted) before vram_textures: the reserved VRAM for compact textures is taken there
  ctr_gpu_set_compact_textures(ctr_settings().compact_textures ? 1 : 0);
  ctr_gpu_set_vram_textures(ctr_settings().vram_textures ? 1 : 0);
  ctr_gpu_set_mip_mode(ctr_settings().mipmaps);
  ctr_gpu_set_overlap(ctr_settings().overlap ? 1 : 0);
  ctr_gpu_set_raw_cmds(ctr_settings().raw_cmds ? 1 : 0);
  ctr_gpu_set_gpu_breakdown(ctr_settings().gpu_breakdown ? 1 : 0);  // (AI-assisted, gpu-rt)
#ifdef __3DS__
  if (ctr_settings().gpu_perflog) {
    // (AI-assisted) the GSP's performance log, read once per stats window (render_frame)
    const int r = ctr_gpu_perflog_start();
    lg::info("[ctr] gpu perflog: {}", r == 0 ? "on" : fmt::format("refused ({:#x})", (u32)r));
  }
#endif
  if (ctr_settings().pipeline) {
    lg::info("[ctr] pipeline (CPU builds the next frame while the GPU draws): {}",
             ctr_gpu_set_pipeline(1) ? "on" : "FAILED (no linear memory), off");
  }
  g_ctr = std::make_unique<CtrRenderer>();
  // (AI-assisted) pad script "set" / "shot NAME" (tests: A/B pictures of one view)
  pad_script::set_set_hook(pad_set);
  pad_script::set_named_shot_hook(pad_shot);
  // Both load on the loader thread while the game boots. The title level (the Naughty Dog logo,
  // the title screen) is the first one the game wants; loaded only when the game has it, the logo
  // started after a ~1.5 s stall on hardware.
  g_ctr->levels().load_common();
  g_ctr->levels().prefetch("title");
  g_async = ctr_gpu_async_start(render_job, nullptr) != 0;
  // the game thread watchdog always; the render thread's lines with config.ini render_watchdog
  ctr_gpu_diag_start(ctr_settings().render_watchdog ? 1 : 0);
  g_ctr_ready = true;
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
  g_ctr_ready = false;
  ctr_gpu_async_stop();
  g_async = false;
  g_ctr.reset();
  ctr_gpu_exit();
}

// (AI-assisted) 60 Hz vblanks between the last two ctr_vsync returns, 1 to 4: how long the frame
// the game just finished was on screen. Jak II's pc-maybe-vsync (TARGET_3DS) gives it to the game
// as its dog-ratio, the PS2's lag ratio, so a 30 fps frame (2 vblanks) moves its clocks by 1/30 s.
// Counted on the vblank counter the frames are released by, so the game's time is the screen's.
static u32 g_frame_vblanks = 1;
static double g_last_vsync_ms = 0;
static u32 g_last_vsync_vblank = 0;
static double g_frame_work_ms = 0;  // from the last vsync's return to this one's start

u32 ctr_vsync() {
  if (MasterExit == RuntimeExitStatus::RUNNING) {
    const double t0 = ctr_gpu_time_ms();
    if (g_last_vsync_ms != 0) {
      g_frame_work_ms = t0 - g_last_vsync_ms;
    }
    // config.ini fps_cap: 30 = two vblanks per frame at least, 60 = one
    ctr_gpu_wait_vblank(ctr_settings().fps_cap <= 30 ? 2 : 1);
    const double t1 = ctr_gpu_time_ms();
    g_ee.vsync_ms += t1 - t0;
    const u32 vblank = ctr_gpu_release_now();
    if (g_last_vsync_ms != 0) {
      g_frame_vblanks = (u32)std::clamp<s32>((s32)(vblank - g_last_vsync_vblank), 1, 4);
    }
    g_last_vsync_vblank = vblank;
    g_last_vsync_ms = t1;
  }
  g_frame_idx++;
  return g_frame_idx & 1;
}

u32 ctr_sync_path() {
  wait_render_idle();
  return 0;
}

// (AI-assisted, title3) the game's camera position (*math-camera* trans, game units) for this frame
// (game thread): false before the game made its camera
bool game_camera_trans(float* out) {
  static u32 sym = 0;
  static u32 tries = 0;
  if (!sym) {
    if (tries++ % 64) {
      return false;
    }
    sym = jak1::find_symbol_from_c("*math-camera*").offset;  // (game thread: kernel lookup)
    if (!sym) {
      return false;
    }
  }
  const u32 mc = Ptr<jak1::Symbol>(sym)->value;
  if ((mc & OFFSET_MASK) != BASIC_OFFSET || mc < 0x100000 || mc + 1024 >= (u32)EE_MAIN_MEM_SIZE) {
    return false;
  }
  // math-camera trans: layout offset 848 (a basic's fields from its address - 4)
  const float* t = Ptr<float>(mc + 848 - 4).c();
  for (int i = 0; i < 3; i++) {
    if (!std::isfinite(t[i])) {
      return false;
    }
    out[i] = t[i];
  }
  return true;
}

// the camera position of *game-info*'s current continue point (game units), Jak 1
bool continue_camera_trans(float* out) {
  static u32 sym = 0;
  static u32 tries = 0;
  if (!sym) {
    if (tries++ % 64) {
      return false;
    }
    sym = jak1::find_symbol_from_c("*game-info*").offset;
    if (!sym) {
      return false;
    }
  }
  auto basic_ok = [](u32 a) {
    return (a & OFFSET_MASK) == BASIC_OFFSET && a >= 0x100000 && a + 256 < (u32)EE_MAIN_MEM_SIZE;
  };
  const u32 gi = Ptr<jak1::Symbol>(sym)->value;
  if (!basic_ok(gi)) {
    return false;
  }
  // game-info current-continue: offset 108; continue-point camera-trans: offset 48
  const u32 cp = *Ptr<u32>(gi + 108 - 4).c();
  if (!basic_ok(cp)) {
    return false;
  }
  const float* t = Ptr<float>(cp + 48 - 4).c();
  for (int i = 0; i < 3; i++) {
    if (!std::isfinite(t[i])) {
      return false;
    }
    out[i] = t[i];
  }
  return t[0] != 0.f || t[1] != 0.f || t[2] != 0.f;
}

// Jak II, a continue in progress (*spawn-actors* is #f from target-continue's start until its
// levels are ready): where *game-info*'s current continue point puts Jak and the camera (game
// units), and how many levels it wants; returns the positions written (game thread)
int continue_place_jak2(float out[2][3], int* want_levels) {
  *want_levels = 0;
#if !defined(__3DS__) || defined(OG3DS_GAME_JAK2)
  static u32 sym = 0, spawn_sym = 0;
  static u32 tries = 0;
  if (!sym || !spawn_sym) {
    if (tries++ % 64) {
      return 0;
    }
    sym = jak2::find_symbol_from_c("*game-info*").offset;
    spawn_sym = jak2::find_symbol_from_c("*spawn-actors*").offset;
    if (!sym || !spawn_sym) {
      return 0;
    }
  }
  if (Ptr<Symbol4<u32>>(spawn_sym)->value() != s7.offset) {
    return 0;
  }
  auto basic_ok = [](u32 a) {
    return (a & OFFSET_MASK) == BASIC_OFFSET && a >= 0x100000 && a + 256 < (u32)EE_MAIN_MEM_SIZE;
  };
  const u32 gi = Ptr<Symbol4<u32>>(sym)->value();
  if (!basic_ok(gi)) {
    return 0;
  }
  // game-info current-continue: offset 240; continue-point trans: 16, camera-trans: 48
  const u32 cp = *Ptr<u32>(gi + 240 - 4).c();
  if (!basic_ok(cp)) {
    return 0;
  }
  int n = 0;
  for (const u32 off : {16u, 48u}) {
    const float* t = Ptr<float>(cp + off - 4).c();
    if (std::isfinite(t[0]) && std::isfinite(t[1]) && std::isfinite(t[2]) &&
        (t[0] != 0.f || t[1] != 0.f || t[2] != 0.f)) {
      for (int i = 0; i < 3; i++) {
        out[n][i] = t[i];
      }
      n++;
    }
  }
  // continue-point want: 6 level-buffer-state of 16 bytes at offset 108, each with its name first
  for (int i = 0; i < 6; i++) {
    *want_levels += *Ptr<u32>(cp + 108 - 4 + 16 * i).c() != s7.offset;
  }
  return n;
#else
  (void)out;
  return 0;
#endif
}

// the start menu's boot common page (texture-pool common-page 0, uploaded only while the menu
// draws), found by the game thread for CtrVram::prewarm_page
void find_menu_page() {
  static u32 sym = 0;
  static u32 tries = 0;
  if (g_menu_page.load(std::memory_order_relaxed) || tries++ % 64) {
    return;
  }
  if (!sym) {
    sym = jak1::find_symbol_from_c("*texture-pool*").offset;
    if (!sym) {
      return;
    }
  }
  const u32 pool = Ptr<jak1::Symbol>(sym)->value;
  if ((pool & OFFSET_MASK) != BASIC_OFFSET || pool < 0x100000 || pool + 0x2b0 >= (u32)EE_MAIN_MEM_SIZE) {
    return;
  }
  const u32 page = Ptr<u32>(pool + 52 - 4).c()[0];  // common-page 0 (a basic's fields from - 4)
  if ((page & OFFSET_MASK) == BASIC_OFFSET && page >= 0x100000 && page + 128 < (u32)EE_MAIN_MEM_SIZE) {
    g_menu_page = page;
  }
}

void ctr_send_chain(const void* data, u32 offset) {
  if (!g_ctr) {
    return;
  }
  const double now = ctr_gpu_time_ms();
  if (g_ee.last_send != 0) {
    const double dt = now - g_ee.last_send;
    g_ee.frame_ms += dt;
    g_ee.frames++;
    g_ee.max_frame_ms = std::max(g_ee.max_frame_ms, dt);
    if (dt >= kLongFrameMs) {
      lg::info("[ctr] long frame {}: {:.0f} ms, of which {:.0f} ms waiting for the render thread, "
               "{:.0f} ms in {} texture uploads",
               g_frame_idx, dt, g_wait_render_total - g_wait_render_mark,
               g_upload_total - g_upload_mark, g_upload_count_total - g_upload_count_mark);
    }
  }
  g_wait_render_mark = g_wait_render_total;
  g_upload_mark = g_upload_total;
  g_upload_count_mark = g_upload_count_total;
  g_ee.last_send = now;
  // (AI-assisted, gpu-home) the release vblank now, noted once the render thread is idle
  const unsigned int release = ctr_gpu_release_now();
  float game_cam[3];
  // (title3, Jak 1 only: *math-camera* through the Jak 1 kernel's symbol table)
  const bool have_game_cam = g_game_version == GameVersion::Jak1 && game_camera_trans(game_cam);
  float cont_cam[3];
  const bool have_cont_cam =
      g_game_version == GameVersion::Jak1 && continue_camera_trans(cont_cam);
  if (g_game_version == GameVersion::Jak1) {
    find_menu_page();
  }
  // Jak II, the render thread idle: while the game is black for a continue, where it goes (the
  // levels that load now hold what is in range there); the decoded textures nothing drew lately
  // go when linear memory was short
  float cont_place[2][3] = {};
  int cont_want = 0;
  const int cont_places =
      g_game_version == GameVersion::Jak2 && g_pmode_alp.load(std::memory_order_relaxed) < 0.01f
          ? continue_place_jak2(cont_place, &cont_want)
          : 0;
  auto note_jak2 = [&]() {
    if (g_game_version == GameVersion::Jak2) {
      g_ctr->levels().note_continue_place(cont_place, cont_places, cont_places ? cont_want : 0);
      if (g_ctr->levels().take_tex_trim()) {
        g_ctr->vram().trim();
      }
    }
  };
  if (g_async) {
    wait_render_idle();
    if (g_levels_pending_set) {
      g_levels_pending_set = false;
      apply_levels(g_levels_pending);
    }
    if (have_game_cam) {
      g_ctr->levels().note_game_camera(game_cam);  // (AI-assisted, title3; render thread idle)
    }
    g_ctr->levels().note_continue_camera(cont_cam, have_cont_cam);
    note_jak2();
    const double tp = ctr_gpu_time_ms();
    g_ctr->prepare_frame(data, offset);
    g_ee.prepare_ms += ctr_gpu_time_ms() - tp;
    g_job_mem = data;
    g_job_offset = offset;
    ctr_gpu_note_release(release);
    ctr_gpu_async_submit();
  } else {
    note_jak2();
#ifndef __3DS__
    // (AI-assisted) host (synchronous): the sprites from the snapshot too, so the host runs check
    // it (CtrSpriteRenderer::snap_data compares it with game memory)
    g_ctr->prepare_sprites(data, offset);
    g_ctr->prepare_textures(data, offset);  // (AI-assisted) Jak II
#endif
    if (have_game_cam) {
      g_ctr->levels().note_game_camera(game_cam);  // (AI-assisted, title3)
    }
    g_ctr->levels().note_continue_camera(cont_cam, have_cont_cam);
    ctr_gpu_note_release(release);
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
        "({:.1f} uploads, {:.1f} relocates per frame) + bone snapshot {:.2f}; longest frame "
        "{:.0f} ms",
        g_ee.frames, g_ee.frame_ms / n, 1000.0 * n / g_ee.frame_ms, logic, g_ee.render_ms / n,
        g_ee.wait_render_ms / n, g_ee.vsync_ms / n, g_ee.upload_ms / n, g_ee.uploads / n,
        g_ee.relocates / n, g_ee.prepare_ms / n, g_ee.max_frame_ms);
    // (AI-assisted) how long each frame stayed on screen (steady pacing: config.ini frame_pacing)
    unsigned int hist[5];
    int latency;
    unsigned int dropped;
    double buf_wait;
    unsigned int just_in_time;
    int floor;
    ctr_gpu_pacing_stats(hist, &latency, &dropped, &buf_wait, &just_in_time, &floor);
    if (latency >= 0) {
      lg::info("[ctr] pacing: frames on screen for 1/2/3/4/5+ vblanks: {}/{}/{}/{}/{}, latency {} "
               "vblanks (not below {} for now), {} frames done just in time, {} skipped, {:.0f} "
               "ms waiting for a screen buffer",
               hist[0], hist[1], hist[2], hist[3], hist[4], latency, floor, just_in_time, dropped,
               buf_wait);
    }
    const double last = g_ee.last_send;
    g_ee = EeTiming();
    g_ee.last_send = last;
    g_ee.last_log = now;
  }
}

void ctr_texture_upload_now(const u8* tpage, int mode, u32 s7_ptr) {
  if (g_ctr) {
    kperf::Stage stage("texture upload");
    kperf::SectionScope section("tex-upload");
    wait_render_idle();
    const double t0 = ctr_gpu_time_ms();
    g_ctr->vram().upload_texture_page_now(tpage, mode, g_ee_main_mem, s7_ptr);
    const double dt = ctr_gpu_time_ms() - t0;
    g_ee.upload_ms += dt;
    g_ee.uploads++;
    g_upload_total += dt;
    g_upload_count_total++;
  }
}

void ctr_texture_relocate(u32 destination, u32 source, u32 format) {
  if (g_ctr) {
    wait_render_idle();
    const double t0 = ctr_gpu_time_ms();
    g_ctr->vram().relocate(destination, source, format);
    const double dt = ctr_gpu_time_ms() - t0;
    g_ee.upload_ms += dt;
    g_ee.relocates++;
    g_upload_total += dt;
    g_upload_count_total++;
  }
}

// With a render thread the list is taken at the frame's hand-over (ctr_send_chain), where the
// render thread is idle anyway: the frames drawn see the same list as when the game waited for
// the render thread here, in the middle of its frame.
void ctr_set_levels(const std::vector<std::string>& levels) {
  if (!g_ctr) {
    return;
  }
  if (g_async && !g_is_jak2) {
    g_levels_pending = levels;
    g_levels_pending_set = true;
    return;
  }
  kperf::Stage stage("set-levels (the renderer's level list)");
  wait_render_idle();
  apply_levels(levels);
}
void ctr_set_active_levels(const std::vector<std::string>&) {}
void ctr_force_reload_all() {}
void ctr_force_reload_level(const std::string&) {}
void ctr_force_reload_common() {}
// (AI-assisted) the PS2's PMODE alpha (screen fades and blackouts at boot and level loads): the
// render thread darkens the frame with it (see CtrRenderer::render_frame)
void ctr_set_pmode_alp(float alp) {
  g_pmode_alp.store(alp, std::memory_order_relaxed);
}
}  // namespace

// (AI-assisted) The game waits for this before a level starts (level-update-after-load, TARGET_3DS):
// the level's .c3l is loaded, so the player doesn't start in a level with nothing drawn yet. Gives
// up after kMaxWaitMs per level (then the level appears when its load is done, as before).
// (AI-assisted) the 60 Hz vblanks the last frame took (g_frame_vblanks; Jak II's kmachine.cpp)
// Game thread: the time left until the frame's flip (the last one plus the frame cap's vblanks);
// negative when the game is behind or draws no frames, large before the first frame.
double ctr_frame_ms_left() {
  if (!g_ctr_ready || g_last_vsync_ms == 0) {
    return 1e9;
  }
  const double period = (ctr_settings().fps_cap <= 30 ? 2 : 1) * (1000.0 / 59.8261);
  return g_last_vsync_ms + period - ctr_gpu_time_ms();
}

u32 ctr_frame_vblanks() {
  return g_frame_vblanks;
}

double ctr_frame_work_ms() {
  return g_frame_work_ms;
}

// the 3DS renderer draws the frames (not the null or the OpenGL renderer)
bool ctr_renderer_draws() {
  return g_ctr_ready.load(std::memory_order_relaxed);
}

void ctr_game_watchdog_tick(void) {
  kperf::watchdog_tick();
}

bool ctr_level_ready(const char* name) {
  constexpr double kMaxWaitMs = 20000.0;
  if (!name || !g_ctr_ready) {
    return true;
  }
  kperf::note_level_poll(name);
  kperf::Stage stage("level ready (the renderer's .c3l)");
  // (AI-assisted) when the wait started and the last poll: the game drops levels that are still
  // loading when its level list changes, which left the entry behind; a later load of that level
  // then found an old start time and started without its background. A wait nobody polled for 2 s
  // starts again.
  struct Wait {
    double since, last;
  };
  static std::map<std::string, Wait> waiting_since;
  const std::string n(name);
  // (AI-assisted) a parked prefetch the game gave up on holds memory (the loader waits for it to
  // go), or the loader ran out of memory: the render thread sees to it at its next frame, but the
  // game may wait here without drawing frames (both level slots empty: it loads in a loop)
  if (g_levels_pending_set) {
    // (no frame since ctr_set_levels: the game loops for a level)
    g_levels_pending_set = false;
    wait_render_idle();
    apply_levels(g_levels_pending);
  }
  if (g_ctr->levels().needs_service()) {
    wait_render_idle();
    g_ctr->levels().service();
  }
#ifndef __3DS__
  if (CtrLevels::load_sim_on()) {
    // (AI-assisted, j2intro) host load model: no loader thread, and the game may wait here without
    // drawing frames
    wait_render_idle();
    g_ctr->levels().host_service();
  }
#endif
  if (g_ctr->levels().ready(n)) {
    // (AI-assisted) how long the game waited for the .c3l after its DGO was in
    auto w = waiting_since.find(n);
    if (w != waiting_since.end()) {
      lg::info("[ctr] {}: the game waited {:.0f} ms for its .c3l", n,
               ctr_gpu_time_ms() - w->second.since);
      waiting_since.erase(w);
    }
    return true;
  }
#ifdef __3DS__
  // the game polls this in a loop at boot: let the loader thread run if it shares the core
  ctr_thread_sleep_us(1000);
#endif
  const double now = ctr_gpu_time_ms();
  auto [it, inserted] = waiting_since.emplace(n, Wait{now, now});
  if (!inserted && now - it->second.last > 2000.0) {
    it->second.since = now;
  }
  it->second.last = now;
  if (now - it->second.since > kMaxWaitMs) {
    lg::warn("[ctr] {}: .c3l still not loaded after {:.0f} s, starting the level anyway", n,
             kMaxWaitMs / 1000.0);
    waiting_since.erase(it);
    return true;
  }
  return false;
}

// game thread (pc-3ds-arrival-ready?, target-continue): a continue's blackout lasts until ring 0
// in view is there at a frame drawn since the wait began, at most kCapMs; Jak II: or until
// nothing of it can come now (hold_black)
bool ctr_arrival_ready() {
  constexpr double kCapMs = 4000.0;
  if (!g_ctr_ready || ctr_arrival_off()) {
    return true;
  }
  static double since = 0, last = 0;
  static u64 f0 = 0;
  static u32 most = 0;
  const double now = ctr_gpu_time_ms();
  const u64 f = g_arrival_frame.load(std::memory_order_relaxed);
  if (since <= 0 || now - last > 1000.0) {
    since = now;
    f0 = f;
    most = 0;
  }
  last = now;
  const u32 late = g_arrival_late.load(std::memory_order_relaxed);
  most = std::max(most, late);
  const bool idle = late && g_arrival_idle.load(std::memory_order_relaxed);
  const bool ready = f >= f0 + 2 && (late == 0 || idle);
  if (!ready && now - since < kCapMs) {
    return false;
  }
  lg::info("[ctr] arrival: the game's blackout held {:.0f} ms for ring 0 in view{} (at most {} "
           "tie LOD groups drawn far / chunks missing, {} left)",
           now - since,
           !ready ? ", given up" : idle ? ", given up: nothing more can come now" : "", most, late);
  since = 0;
  // (Jak II: a wait that saw nothing missing leaves the renderer's hold in place)
  if (!g_is_jak2 || most > 0) {
    g_arrival_done_ms = now;
  }
  return true;
}

// (AI-assisted) The overlord (IOP thread) started loading a level's DGO: start reading the
// level's .c3l at the same time. The game only reports a level (set_levels) once its DGO is in,
// and the .c3l took as long again after that (on hardware several seconds without a background).
void ctr_level_dgo_started(const char* dgo_name) {
  // DGO file names are the levels' nicknames (engine/level/level-info.gc)
  static const struct {
    const char* dgo;
    const char* level;
  } kLevels[] = {
      {"TRA", "training"},   {"VI1", "village1"}, {"BEA", "beach"},      {"JUN", "jungle"},
      {"JUB", "jungleb"},    {"MIS", "misty"},    {"FIC", "firecanyon"}, {"VI2", "village2"},
      {"SUN", "sunken"},     {"SUB", "sunkenb"},  {"SWA", "swamp"},      {"ROL", "rolling"},
      {"OGR", "ogre"},       {"VI3", "village3"}, {"SNO", "snow"},       {"MAI", "maincave"},
      {"DAR", "darkcave"},   {"ROB", "robocave"}, {"LAV", "lavatube"},   {"CIT", "citadel"},
      {"FIN", "finalboss"},  {"INT", "intro"},    {"DEM", "demo"},       {"TIT", "title"},
      {"TSZ", "test-zone"},
  };
  if (!dgo_name || !g_ctr_ready) {
    return;
  }
  if (g_is_jak2) {
    // (AI-assisted, j2intro) Jak II's DGO names (CtrLevel.cpp's table). Its level-update-after-load
    // waits for the .c3l (TARGET_3DS) and reports a level to set_levels only once it's loaded:
    // without this the .c3l of a level would only be asked for by the wait's timeout.
    if (const char* level = ctr_jak2_level_for_dgo(dgo_name)) {
      g_ctr->levels().prefetch(level);
    }
    return;
  }
  // the game asks for "vi1.DGO"
  std::string n(dgo_name);
  const auto dot = n.find('.');
  if (dot != std::string::npos) {
    n.resize(dot);
  }
  for (auto& c : n) {
    c = (char)toupper((unsigned char)c);
  }
  for (const auto& l : kLevels) {
    if (n == l.dgo) {
      g_ctr->levels().prefetch(l.level);
      return;
    }
  }
}

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
