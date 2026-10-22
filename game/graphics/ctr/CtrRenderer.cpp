/*!
 * @file CtrRenderer.cpp
 * (AI-assisted)
 * See CtrRenderer.h.
 */

#include "CtrRenderer.h"

#include <algorithm>
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
#endif
#include "game/graphics/ctr/CtrEye.h"
#include "game/graphics/ctr/CtrLevel.h"
#include "game/graphics/ctr/CtrSky.h"
#include "game/graphics/ctr/CtrMerc.h"
#include "game/graphics/ctr/CtrOcean.h"
#include "game/graphics/ctr/CtrOceanFull.h"
#include "game/graphics/ctr/CtrShadow.h"
#include "game/graphics/ctr/CtrSprite.h"
#include "game/graphics/ctr/CtrVram.h"
#include "game/graphics/ctr/CtrImpostor.h"
#include "game/graphics/ctr/CtrSettings.h"
#include "game/graphics/ctr/ctr_gpu.h"
#include "game/graphics/opengl_renderer/buckets.h"
#include "game/kernel/common/kboot.h"
#include "game/kernel/common/kmachine.h"
#include "game/kernel/common/kscheme.h"
#include "game/kernel/jak1/kscheme.h"  // (AI-assisted, title3) find_symbol_from_c
#include "game/runtime.h"
#include "game/sce/pad_script.h"

// (AI-assisted) PMODE alpha from the game thread (ctr_set_pmode_alp), 1 = no fade
static std::atomic<float> g_pmode_alp{1.f};

namespace {
std::string g_shot_dir;
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

// (AI-assisted) config.ini gpu_profile = ab: the switches that are turned on and off in turns.
// Add a row to price another setting: its config.ini key (the log's label), how to set it, and
// how to read it (its config.ini value is the starting point and is restored at the end). The
// setting must be read by the renderers each frame (not only at load or init).
struct AbSwitch {
  const char* key;
  void (*set)(CtrSettings& s, bool on);
  bool (*get)(const CtrSettings& s);
};
const AbSwitch kAbSwitches[] = {
    {"run_culling", [](CtrSettings& s, bool on) { s.run_culling = on; },
     [](const CtrSettings& s) { return s.run_culling; }},
    // (AI-assisted) the finer precomputed visibility (.c3v files; loaded with fine_vis on)
    {"fine_vis", [](CtrSettings& s, bool on) { s.fine_vis = on; },
     [](const CtrSettings& s) { return s.fine_vis; }},
    // (AI-assisted) software occlusion culling of the level background (CtrOccCull.h)
    {"occ_cull", [](CtrSettings& s, bool on) { s.occ_cull = on; },
     [](const CtrSettings& s) { return s.occ_cull; }},
    {"cull_ties", [](CtrSettings& s, bool on) { s.cull_ties = on; },
     [](const CtrSettings& s) { return s.cull_ties; }},
    // (AI-assisted) c3l v17 tie levels of detail (off: the far versions only)
    {"tie_lod", [](CtrSettings& s, bool on) { s.tie_lod = on; },
     [](const CtrSettings& s) { return s.tie_lod; }},
    // the far scenery: lowres_vista and the vista tier (vista_distance 0: none)
    {"vista",
     [](CtrSettings& s, bool on) {
       static const float kConfigDistance = s.vista_distance > 0 ? s.vista_distance : 3000.f;
       s.lowres_vista = on;
       s.vista_distance = on ? kConfigDistance : 0.f;
     },
     [](const CtrSettings& s) { return s.lowres_vista && s.vista_distance > 0; }},
    {"fog_stage_skip", [](CtrSettings& s, bool on) { s.fog_stage_skip = on; },
     [](const CtrSettings& s) { return s.fog_stage_skip; }},
    // the shadow volumes' draws (shadows = full: the shadow bucket; off: left out, the game still
    // computes them)
    {"shadows",
     [](CtrSettings& s, bool on) {
       s.shadows = on ? CtrSettings::ShadowMode::Full : CtrSettings::ShadowMode::Blob;
     },
     [](const CtrSettings& s) { return s.shadows == CtrSettings::ShadowMode::Full; }},
    {"envmap", [](CtrSettings& s, bool on) { s.envmap = on; },
     [](const CtrSettings& s) { return s.envmap; }},
    {"distort", [](CtrSettings& s, bool on) { s.distort = on; },
     [](const CtrSettings& s) { return s.distort; }},
    // (AI-assisted) snow-gpu-impl's keys (merged Oct 11). off = the old behaviour of each one.
    // distort_copy: rows (on) or the whole screen (off)
    {"distort_copy", [](CtrSettings& s, bool on) { s.distort_copy_rows = on; },
     [](const CtrSettings& s) { return s.distort_copy_rows; }},
    // shadow_casters: the config's cap (on; 6 when the config says all) or every caster (off)
    {"shadow_casters",
     [](CtrSettings& s, bool on) {
       static const int kConfigCap = s.shadow_casters > 0 ? s.shadow_casters : 6;
       s.shadow_casters = on ? kConfigCap : 0;
     },
     [](const CtrSettings& s) { return s.shadow_casters > 0; }},
    // sprite_min_alpha: the config's value for big sprites (on; 6 when it is 2) or 2 (off)
    {"sprite_min_alpha",
     [](CtrSettings& s, bool on) {
       static const int kConfigAlpha = s.sprite_min_alpha > 2 ? s.sprite_min_alpha : 6;
       s.sprite_min_alpha = on ? kConfigAlpha : 2;
     },
     [](const CtrSettings& s) { return s.sprite_min_alpha > 2; }},
    {"sprite_alpha_discard", [](CtrSettings& s, bool on) { s.sprite_alpha_discard = on; },
     [](const CtrSettings& s) { return s.sprite_alpha_discard; }},
    {"envmap_batch", [](CtrSettings& s, bool on) { s.envmap_batch = on; },
     [](const CtrSettings& s) { return s.envmap_batch; }},
    {"merc_tex_sort", [](CtrSettings& s, bool on) { s.merc_tex_sort = on; },
     [](const CtrSettings& s) { return s.merc_tex_sort; }},
    {"merc_gen_lod", [](CtrSettings& s, bool on) { s.merc_gen_lod = on; },
     [](const CtrSettings& s) { return s.merc_gen_lod; }},
    // (AI-assisted) gpu-imp: far tie groups as cards (CtrImpostor)
    {"impostors", [](CtrSettings& s, bool on) { s.impostors = on; },
     [](const CtrSettings& s) { return s.impostors; }},
    {"clouds", [](CtrSettings& s, bool on) { s.clouds = on; },
     [](const CtrSettings& s) { return s.clouds; }},
    // (AI-assisted) gpu-b's keys (Oct 11). frame_overlap2: the config's mode (on; clean when the
    // config says off) or off (the old frame end)
    {"frame_overlap2",
     [](CtrSettings& s, bool on) {
       static const int kConfigMode = s.frame_overlap2 > 0 ? s.frame_overlap2 : 1;
       s.frame_overlap2 = on ? kConfigMode : 0;
     },
     [](const CtrSettings& s) { return s.frame_overlap2 > 0; }},
    {"sky_last", [](CtrSettings& s, bool on) { s.sky_last = on; },
     [](const CtrSettings& s) { return s.sky_last; }},
    {"skip_clear", [](CtrSettings& s, bool on) { s.skip_clear = on; },
     [](const CtrSettings& s) { return s.skip_clear; }},
    {"tex_cache_clear", [](CtrSettings& s, bool on) { s.tex_cache_uploads = on; },
     [](const CtrSettings& s) { return s.tex_cache_uploads; }},
    // mip_bias: the config's bias (on; +1 when the config says 0) or 0 (off)
    {"mip_bias",
     [](CtrSettings& s, bool on) {
       static const float kConfigBias = s.mip_bias != 0.f ? s.mip_bias : 1.f;
       s.mip_bias = on ? kConfigBias : 0.f;
     },
     [](const CtrSettings& s) { return s.mip_bias != 0.f; }},
    {"skin_rows_f24", [](CtrSettings& s, bool on) { s.skin_rows_f24 = on; },
     [](const CtrSettings& s) { return s.skin_rows_f24; }},
    // early_depth: the config's mode (on, block32, sky, sky32) or off; nothing when the config
    // says off (it left squares where the level never drew on a New 3DS: not tried uninvited)
    {"early_depth",
     [](CtrSettings& s, bool on) {
       static const bool kOpaque = s.early_depth, kSky = s.early_depth_sky;
       s.early_depth = on && kOpaque;
       s.early_depth_sky = on && kSky;
     },
     [](const CtrSettings& s) { return s.early_depth || s.early_depth_sky; }},
    // (AI-assisted) gpu-a's keys (Oct 11): level fog in the vertex alpha, texture unit 1 gating
    {"fog_alpha", [](CtrSettings& s, bool on) { s.fog_alpha = on; },
     [](const CtrSettings& s) { return s.fog_alpha; }},
    {"tex1_gating", [](CtrSettings& s, bool on) { s.tex1_gating = on; },
     [](const CtrSettings& s) { return s.tex1_gating; }},
    {"cull_merc", [](CtrSettings& s, bool on) { s.cull_merc = on; },
     [](const CtrSettings& s) { return s.cull_merc; }},
    // (AI-assisted, far-fog) the game's fog on merc models
    {"merc_fog", [](CtrSettings& s, bool on) { s.merc_fog = on; },
     [](const CtrSettings& s) { return s.merc_fog; }},
    // (AI-assisted) shader-opt's keys (Oct 12): the shorter vertex programs
    {"shader_opt", [](CtrSettings& s, bool on) { s.shader_opt = on; },
     [](const CtrSettings& s) { return s.shader_opt; }},
    {"cmd_dedup", [](CtrSettings& s, bool on) { s.cmd_dedup = on; },
     [](const CtrSettings& s) { return s.cmd_dedup; }},
    {"cmd_dedup_draw", [](CtrSettings& s, bool on) { s.cmd_dedup_draw = on; },
     [](const CtrSettings& s) { return s.cmd_dedup_draw; }},
    {"skin_clip_f24", [](CtrSettings& s, bool on) { s.skin_clip_f24 = on; },
     [](const CtrSettings& s) { return s.skin_clip_f24; }},
    // (AI-assisted, gpu-rt) static_cmdlists: the config's mode (on; on when the config says off)
    // or off; ocean_tex_half
    {"static_cmdlists",
     [](CtrSettings& s, bool on) {
       static const int kConfigMode = s.static_cmdlists > 0 ? s.static_cmdlists : 1;
       s.static_cmdlists = on ? kConfigMode : 0;
     },
     [](const CtrSettings& s) { return s.static_cmdlists > 0; }},
    {"ocean_tex_half", [](CtrSettings& s, bool on) { s.ocean_tex_half = on; },
     [](const CtrSettings& s) { return s.ocean_tex_half; }},
    // (AI-assisted) gpu-conv (Oct 12): c3l v15 alpha tests / blending that can't change a pixel
    {"opaque_alpha", [](CtrSettings& s, bool on) { s.opaque_alpha = on; },
     [](const CtrSettings& s) { return s.opaque_alpha; }},
};
constexpr size_t kAbSwitchCount = sizeof(kAbSwitches) / sizeof(kAbSwitches[0]);
constexpr double kAbWindowMs = 1500.0;
constexpr int kAbSettleFrames = 4;  // the GPU busy times lag the frame (pipeline)
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

// (AI-assisted) pad script "set KEY VALUE": a kAbSwitches key (or fog) from the game thread, read by
// the renderers from the next frame on (like gpu_profile = ab's switching)
bool pad_set(const char* key, const char* value) {
  const std::string v = value;
  const bool on = v == "on" || v == "1" || v == "true" || v == "yes";
  CtrSettings& s = ctr_settings_ab();
  if (!strcmp(key, "fog")) {
    s.fog = on;
    return true;
  }
  if (!strcmp(key, "cmd_dump")) {
    ctr_gpu_cmd_dump_next();  // (AI-assisted) "set cmd_dump on": the next frame's command list
    return true;
  }
  if (!strcmp(key, "gpu_micro_mask")) {
    // (AI-assisted, micro-mg) a test: gpu_micro's changes as bits (old / new in one run)
    ctr_gpu_set_micro_mask((unsigned int)strtoul(value, nullptr, 0));
    return true;
  }
  for (const auto& sw : kAbSwitches) {
    if (!strcmp(key, sw.key)) {
      sw.set(s, on);
      return true;
    }
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
      lg::warn("[ctr] shadows = full needs the stencil buffer (not with depth16): not drawn");
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

void CtrRenderer::render_frame(const void* ee_mem, u32 chain_offset) {
  m_rs.ee_mem = (const u8*)ee_mem;
  m_rs.offset_of_s7 = s7.offset;
  m_rs.vram = m_vram.get();
  // No sky renderer yet: clear to the fog color from the default GS registers (the same place
  // OpenGLRenderer reads it), which is close to the sky color at the horizon.
  // (AI-assisted) The last frame's when they can't be read: black would fog everything to black.
  u8 clear[4];
  memcpy(clear, m_rs.fog_color, 4);
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
  {
    const double tl = ctr_gpu_time_ms();
    ctr_gpu_diag_stage("level loads (process_pending_loads)");
    m_levels->process_pending_loads(m_rs.frame_idx);
    const double load_ms = ctr_gpu_time_ms() - tl;
    if (load_ms >= 20.0) {
      lg::info("[ctr] render thread frame {}: {:.0f} ms in level loads", m_rs.frame_idx, load_ms);
    }
  }
  // (AI-assisted) skip_clear: only the depth is cleared when this frame's opaque sky roof covers
  // the whole screen (looking up): every pixel's color is drawn over
  {
    bool covered = false;
    if (ctr_settings().skip_clear && m_sky_draw && ctr_settings().sky) {
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
    ctr_gpu_set_skip_color_clear(covered ? 1 : 0);
  }
  ctr_gpu_frame_begin(clear[0], clear[1], clear[2]);
  // (AI-assisted) cull_ties (gpu_profile = ab may switch it between frames)
  ctr_gpu_set_cull(ctr_settings().cull_ties ? 1 : 0, ctr_settings().cull_flip ? 1 : 0);
  // (AI-assisted) frame_overlap2 (gpu_profile = ab may switch it between frames)
  ctr_gpu_set_frame_overlap2(ctr_settings().frame_overlap2);
  // (AI-assisted) early_depth (ab: the config's mode or off)
  ctr_gpu_set_early_depth(ctr_settings().early_depth ? 1 : 0);
  ctr_gpu_set_tex_cache_clear(ctr_settings().tex_cache_uploads ? 1 : 0);  // (AI-assisted)
  ctr_gpu_set_mip_bias(ctr_settings().mip_bias);                          // (AI-assisted)
  ctr_gpu_set_skin_rows_f24(ctr_settings().skin_rows_f24 ? 1 : 0);         // (AI-assisted)
  ctr_gpu_set_skin_clip_f24(ctr_settings().skin_clip_f24 ? 1 : 0);         // (AI-assisted)
  ctr_gpu_set_early_depth_params(
      ctr_settings().early_depth_sky || (ctr_settings().early_depth && ctr_settings().sky_last),
      ctr_settings().early_depth_func, ctr_settings().early_depth_clear);
  // (AI-assisted) fog_alpha, tex1_gating (gpu_profile = ab may switch them between frames too)
  ctr_gpu_set_fog_alpha(ctr_settings().fog_alpha ? 1 : 0);
  ctr_gpu_set_tex1_gating(ctr_settings().tex1_gating ? 1 : 0);
  ctr_gpu_set_shader_opt(ctr_settings().shader_opt ? 1 : 0);        // (AI-assisted)
  ctr_gpu_set_cmd_dedup(ctr_settings().cmd_dedup ? 1 : 0,           // (AI-assisted)
                        ctr_settings().cmd_dedup_draw ? 1 : 0);
  ctr_gpu_set_static_cmdlists(ctr_settings().static_cmdlists);  // (AI-assisted, gpu-rt)
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
    }
    g_pad_shot.clear();
  }
  // (AI-assisted, title3) the screen is black this frame (the game's fade at 0 since the last
  // frame, or held black below): the level draws keep no group far for late data
  ctr_level_set_screen_black(m_hold_black ||
                             (m_shown_alp < 0.01f && g_pmode_alp.load(std::memory_order_relaxed) <
                                                         0.01f));
  dispatch_buckets_jak1(DmaFollower(ee_mem, chain_offset));
  ctr_gpu_diag_stage("pmode fade");
#ifndef __3DS__
  ctr_gpu_soft_bucket("pmode-fade");
#endif
  ctr_gpu_phase(CTR_PH_HUD);  // (AI-assisted, gpu-rt) gpu_breakdown: the fade with the HUD
  draw_pmode_fade();
  ctr_level_fine_vis_check(m_rs.frame_idx, m_rs.log_now);  // (AI-assisted) host check only
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
  m_timing.dedup_kb += gs.cmd_dedup_words * 4 / 1024.0;
  m_timing.dropped += gs.dropped_draws;
  // (AI-assisted) a pipelined frame that ran out of command buffer lost its last draws: the
  // normal mode has the whole buffer for one frame
  if (gs.dropped_draws && ctr_settings().pipeline && !m_pipeline_off) {
    ctr_gpu_set_pipeline(0);
    m_pipeline_off = true;
    lg::warn("[ctr] pipeline: {} draws didn't fit in the command buffer, pipeline off from now on",
             gs.dropped_draws);
  }
  m_timing.tex_binds += gs.tex_binds;
  m_timing.draws += gs.draws;
  m_timing.gpu_wait_ms += gs.gpu_wait_ms;
  m_timing.gpu_busy_ms += gs.gpu_busy_ms;
  m_timing.gpu_busy_frames += gs.gpu_busy_frames;
  m_timing.gpu_active_ms += gs.gpu_active_ms;
  m_timing.color_clears_skipped += gs.color_clear_skipped;
  m_timing.triangles += gs.triangles;
  m_timing.vbuf_peak = std::max(m_timing.vbuf_peak, gs.vbuf_used);
  m_timing.vbuf_size = gs.vbuf_size;
  m_timing.vbuf_dropped += gs.vbuf_dropped;
  m_timing.frames++;
  // (AI-assisted) the time of day work isn't GPU load: not counted for auto_detail
  const double tod_ms = ctr_level_tod_take_ms();
  m_timing.tod_ms += tod_ms;
  m_timing.tod_max_ms = std::max(m_timing.tod_max_ms, tod_ms);
  m_timing.tod_over += tod_ms > 2.0;
  if (ctr_settings().auto_detail) {
#ifndef __3DS__
    // (AI-assisted, title-popin) host test: env CTR_AUTO_DETAIL_MODEL gives auto_detail the GPU time
    // of the hardware fit (busy ~ 10.6 ms + 0.13 ms per 1000 triangles) of this frame's triangles
    static const bool model = getenv("CTR_AUTO_DETAIL_MODEL") != nullptr;
    if (model) {
      auto_detail_frame(10.6f + 0.13f * gs.triangles / 1000.f, 1, 0.0);
    } else
#endif
    auto_detail_frame(gs.gpu_busy_ms, gs.gpu_busy_frames, t2 - t1 - tod_ms);
  }
  if (!ctr_settings().gpu_profile && ctr_settings().gpu_counters) {
    hw_counters_frame();
  }
  if (ctr_settings().gpu_profile_ab) {
    ab_frame(gs.gpu_busy_ms, gs.gpu_busy_frames, gs.triangles, gs.draws);
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
          "triangles, {:.0f} draws, {:.0f} texture binds, {:.0f} KB commands ({:.1f} KB left out by "
          "cmd_dedup); vertex buffer peak {} of {} KB{}{}{}",
          m_timing.frames, m_timing.begin_ms / n, m_timing.build_ms / n, m_timing.end_ms / n,
          (m_timing.begin_ms + m_timing.build_ms + m_timing.end_ms) / n,
          m_timing.gpu_wait_ms / n, m_timing.gpu_ms / n, m_timing.gpu_draw_ms / n, busy,
          m_timing.triangles / n, m_timing.draws / n, m_timing.tex_binds / n,
          m_timing.cmd_kb / n, m_timing.dedup_kb / n, m_timing.vbuf_peak / 1024,
          m_timing.vbuf_size / 1024,
          m_timing.vbuf_dropped
              ? fmt::format(", {} draws DROPPED (vertex buffer full)", m_timing.vbuf_dropped)
              : std::string(),
          m_timing.dropped ? fmt::format(", {} draws DROPPED (command buffer full)",
                                         m_timing.dropped)
                           : std::string(),
          m_timing.color_clears_skipped
              ? fmt::format(", {} frames without the color clear (skip_clear)",
                            m_timing.color_clears_skipped)
              : std::string());
      lg::info("[ctr] textures: {} level pools {} KB, {} in VRAM ({} KB); linear free {} KB "
               "(largest block {} KB), VRAM free {} KB; {} compact textures {} KB ({} in reserved "
               "VRAM, {} radial); last frame: {} early depth draws, {} proctex draws, {} level draws without the fog stage, {} with the fog in the vertex alpha ({} texture unit 1 switches); {} program switches, {} with a shader upload, {} uniform vectors at switches; {} texture binds without the cache clear",
               gs.pools, gs.pool_bytes / 1024, gs.pools_in_vram, gs.pool_vram_bytes / 1024,
               gs.linear_free / 1024, gs.linear_largest / 1024, gs.vram_free / 1024,
               gs.compact_textures, gs.compact_bytes / 1024, gs.compact_in_vram,
               gs.radial_textures, gs.early_depth_draws, gs.proctex_draws, gs.nofog_draws,
               gs.fog_alpha_draws, gs.unit1_toggles, gs.prog_switches, gs.prog_uploads,
               gs.prog_unif_vectors, gs.tex_clears_skipped);
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
      {
        // (AI-assisted, gpu-rt) gpu_breakdown, static_cmdlists
        char line[1024];
        if (ctr_gpu_breakdown_line(line, sizeof(line),
                                    m_profile_level.empty() ? "-" : m_profile_level.c_str()) > 0) {
          lg::info("{}", line);
        }
        if (ctr_gpu_cmdlist_line(line, sizeof(line)) > 0) {
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
      if (ctr_settings().auto_detail) {
        const auto& s = ctr_settings();
        const float t = m_auto.t;
        lg::info("[ctr] auto detail {:.2f}: shrubs {:.0f} m (v14 files: x{:.2f}), ties x{:.2f} ({} steps up, {} down)", t,
                 s.shrub_distance + (std::max(s.shrub_distance_max, s.shrub_distance) - s.shrub_distance) * t,
                 s.shrub_scale + (std::max(s.shrub_scale_max, s.shrub_scale) - s.shrub_scale) * t,
                 s.tie_distance_scale +
                     (std::max(s.tie_distance_scale_max, s.tie_distance_scale) - s.tie_distance_scale) * t,
                 m_auto.ups, m_auto.downs);
        m_auto.ups = m_auto.downs = 0;
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
    rs.next_bucket = base + 16 * (id + 1);
    DmaFollower dma(ee_mem, base + 16 * id);
    merc->snapshot_bones(dma, rs);
  }
  prepare_sprites(ee_mem, chain_offset);
}

void CtrRenderer::prepare_sprites(const void* ee_mem, u32 chain_offset) {
  // (AI-assisted) see CtrSpriteRenderer::snapshot
  if (!m_sprite) {
    return;
  }
  CtrRenderState rs;
  rs.ee_mem = (const u8*)ee_mem;
  rs.offset_of_s7 = s7.offset;
  const u32 base = chain_offset + 16;
  const int id = (int)jak1::BucketId::SPRITE;
  rs.next_bucket = base + 16 * (id + 1);
  DmaFollower dma(ee_mem, base + 16 * id);
  m_sprite->snapshot(dma, rs);
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
  // the first level that isn't the title screen is the one being played
  std::string lev;
  for (const auto& l : levels) {
    if (!l.empty() && l != "title" && l != "none" && l != "#f") {
      lev = l;
      break;
    }
  }
  if (lev != m_profile_level) {
    if (m_profile_active) {
      // a level change in the middle: stop (draw everything), measure the new one later
      m_profile_active = false;
      m_profile_mode = 0;
    }
    m_profile_level = lev;
    m_profile_arm_ms = ctr_gpu_time_ms() + 10000.0;  // after the load and the level start
  }
}

// (AI-assisted) config.ini auto_detail. Every 15 frames: a GPU over the target time on average,
// or one frame near the 33 ms budget (target + 4 ms), pulls the shrub / tie distances in by a
// tenth of their range; time to spare (average 4 ms under the target, no frame over it) pushes
// them out by 4% of it. Out slowly, in fast: a frame that misses its vblank costs more than a
// shrub that appears a little later. The render thread's own build time counts too.
void CtrRenderer::auto_detail_frame(float gpu_busy_ms, int gpu_busy_frames, double build_ms) {
  if (!gpu_busy_frames) {
    return;  // no GPU time this frame (frame_pacing = fast): distances stay where they are
  }
  m_auto.busy_ms += gpu_busy_ms;
  m_auto.busy_frames += gpu_busy_frames;
  m_auto.busy_max = std::max(m_auto.busy_max, (double)gpu_busy_ms / gpu_busy_frames);
  m_auto.build_max = std::max(m_auto.build_max, build_ms);
  if (++m_auto.frames < 15) {
    return;
  }
  // (AI-assisted, citadel) the target and margins are for fps_cap = 30's 33 ms frame: at fps_cap =
  // 60 half of them (a 27 ms target kept the distances at their far end in a 16.7 ms frame:
  // Azahar citadel-generator-start 92-99k triangles, 50 fps)
  const double k = ctr_settings().fps_cap >= 60 ? 0.5 : 1.0;
  const double target = ctr_settings().auto_detail_gpu_ms * k;
  const double avg = m_auto.busy_ms / m_auto.busy_frames;
  float t = m_auto.t;
  if (avg > target || m_auto.busy_max > target + 4.0 * k || m_auto.build_max > target) {
    t -= 0.1f;
    m_auto.downs++;
  } else if (avg < target - 4.0 * k && m_auto.busy_max < target &&
             m_auto.build_max < target - 8.0 * k) {
    t += 0.04f;
    m_auto.ups++;
  }
  t = std::clamp(t, 0.f, 1.f);
  // (AI-assisted, title-popin) a GPU close to missing frames lowers the distances of what is on
  // screen too (else those keep theirs until they are out of view; the build time's spikes, the
  // time of day recolor, don't count here)
  ctr_level_set_detail_urgent(avg > target + 3.0 * k || m_auto.busy_max > 33.0 * k);
  if (t != m_auto.t) {
    m_auto.t = t;
    ctr_level_set_detail(t);
  }
  m_auto.busy_ms = m_auto.busy_max = m_auto.build_max = 0;
  m_auto.busy_frames = m_auto.frames = 0;
}

// (AI-assisted) config.ini gpu_profile = ab. A cycle has two windows per switch of kAbSwitches
// (~1.5 s each): the switch on, then off (every other cycle off, then on, so a slow change of the
// view doesn't favour one side); the other switches keep their config.ini values. The first
// frames of a window aren't counted (the GPU times lag). After every cycle one line per switch:
// the mean GPU busy time per frame with it on and off since the level started, and triangles /
// draws. Per level (the sums restart at a level change); runs as long as the setting is there.
void CtrRenderer::ab_frame(float gpu_busy_ms, int gpu_busy_frames, int triangles, int draws) {
  CtrSettings& s = ctr_settings_ab();
  const double now = ctr_gpu_time_ms();
  if (!m_ab.init) {
    m_ab.init = true;
    for (const auto& sw : kAbSwitches) {
      m_ab.config.push_back(sw.get(s));
    }
    m_ab.stats.assign(kAbSwitchCount, AbStats());
    m_ab.window = 0;
    m_ab.window_start = now;
    lg::info("[ctr] gpu ab: {} switches, {:.1f} s windows (gpu_profile = ab)", kAbSwitchCount,
             kAbWindowMs / 1000.0);
  }
  if (m_profile_level != m_ab.level) {
    // a new level: its own sums; config values while it loads
    m_ab.level = m_profile_level;
    m_ab.stats.assign(kAbSwitchCount, AbStats());
    m_ab.cycle = 0;
    m_ab.window = 0;
    m_ab.window_frames = 0;
    m_ab.window_start = m_profile_arm_ms;  // ~10 s after the level started
  }
  const size_t sw = m_ab.window / 2;
  const bool first_half = m_ab.window % 2 == 0;
  const bool on = (m_ab.cycle % 2 == 0) == first_half;
  if (now < m_ab.window_start) {
    for (size_t i = 0; i < kAbSwitchCount; i++) {
      kAbSwitches[i].set(s, m_ab.config[i]);
    }
    return;
  }
  // this window's settings (for the next frame)
  for (size_t i = 0; i < kAbSwitchCount; i++) {
    kAbSwitches[i].set(s, i == sw ? on : m_ab.config[i]);
  }
  if (gpu_busy_frames && ++m_ab.window_frames > kAbSettleFrames) {
    auto& st = m_ab.stats[sw];
    st.busy[on] += gpu_busy_ms / gpu_busy_frames;
    st.tris[on] += triangles;
    st.draws[on] += draws;
    st.frames[on]++;
  }
  if (now - m_ab.window_start < kAbWindowMs) {
    return;
  }
  m_ab.stats[sw].windows[on]++;
  m_ab.window_start = now;
  m_ab.window_frames = 0;
  if (++m_ab.window < 2 * kAbSwitchCount) {
    return;
  }
  m_ab.window = 0;
  m_ab.cycle++;
  for (size_t i = 0; i < kAbSwitchCount; i++) {
    const auto& st = m_ab.stats[i];
    if (!st.frames[0] || !st.frames[1]) {
      continue;
    }
    const double b1 = st.busy[1] / st.frames[1], b0 = st.busy[0] / st.frames[0];
    lg::info("[ctr] gpu ab {} cycle {}: {} on {:.2f} ms, off {:.2f} ms (on - off {:+.2f} ms; "
             "{:.0f} / {:.0f} triangles, {:.0f} / {:.0f} draws; {}+{} windows, config {})",
             m_ab.level, m_ab.cycle, kAbSwitches[i].key, b1, b0, b1 - b0,
             st.tris[1] / st.frames[1], st.tris[0] / st.frames[0], st.draws[1] / st.frames[1],
             st.draws[0] / st.frames[0], st.windows[1], st.windows[0],
             m_ab.config[i] ? "on" : "off");
  }
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
  // (AI-assisted, title3) a screen coming out of black (a level start, a continue, loading a save:
  // the game fades in from its blackout) stays black while a level shown within the last 3 s, or
  // after a camera cut, still has tie LOD groups in view drawn with their far version because
  // their near chunks are being read (CtrLevels::note_late_in_view): its first visible frame is
  // final, not the far versions switching to the near ones on screen. At most kMaxHoldMs.
  {
    constexpr double kMaxHoldMs = 1500.0;
    bool shown_or_cut = false;
    const u32 late = m_levels->late_in_view(m_rs.frame_idx, &shown_or_cut);
    const double now = ctr_gpu_time_ms();
    if (!m_hold_black && m_shown_alp < 0.01f && alp >= 0.01f && late > 0 && shown_or_cut) {
      m_hold_black = true;
      m_hold_t0 = now;
      m_hold_groups = 0;
      m_hold_frames = 0;
    }
    if (m_hold_black) {
      m_hold_groups = std::max(m_hold_groups, late);
      if (late == 0 || now - m_hold_t0 >= kMaxHoldMs) {
        m_hold_black = false;
        m_hold_ramp = true;
        lg::info("[ctr] screen kept black {:.0f} ms ({} frames) for near chunks in view{} (at most {} "
                 "tie LOD groups drawn far)",
                 now - m_hold_t0, m_hold_frames, late ? ", given up" : "", m_hold_groups);
      } else {
        m_hold_frames++;
        alp = 0.f;
      }
    }
    if (m_hold_ramp && !m_hold_black) {
      const float game_alp = alp;
      alp = std::min(alp, m_shown_alp + 0.125f);
      m_hold_ramp = alp < game_alp;
    }
    m_shown_alp = alp;
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
    if (bucket_id >= (size_t)jak1::BucketId::ALPHA_TEX_LEVEL0) {
      // (AI-assisted) sky_last: at the latest before the translucent buckets (and the sky blend
      // in tfrag-trans, which starts this frame's sky textures)
      ctr_gpu_run_sky_hook();
    }
    const double tb = ctr_gpu_time_ms();
    // (AI-assisted) shadows switched off by gpu_profile = ab: the shadow bucket left out
    const bool ab_skip = ctr_settings().shadows != CtrSettings::ShadowMode::Full &&
                         m_buckets[bucket_id]->name() == "shadow";
    if ((profiling && profile_skips(m_buckets[bucket_id]->name())) || ab_skip) {
      while (dma.current_tag_offset() != m_rs.next_bucket) {
        dma.read_and_advance();
      }
    } else {
#ifndef __3DS__
      ctr_gpu_soft_bucket(m_buckets[bucket_id]->name().c_str());
#endif
      m_buckets[bucket_id]->render(dma, m_rs);
    }
    m_bucket_ms[bucket_id] += ctr_gpu_time_ms() - tb;
    ctr_gpu_submit_partial();  // config.ini overlap: the GPU starts on what's built so far
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

void render_job(void*) {
  g_ctr->render_frame(g_job_mem, g_job_offset);
}

// before touching renderer state from the game thread
void wait_render_idle() {
  if (g_async) {
    const double w = ctr_gpu_async_wait();
    g_ee.wait_render_ms += w;
    g_wait_render_total += w;
  }
}

int ctr_init(GfxGlobalSettings& /*settings*/) {
  ctr_gpu_set_color16(ctr_settings().color16 ? 1 : 0);  // the render target's format
  ctr_gpu_set_steady_pacing(ctr_settings().steady_pacing ? 1 : 0);
  ctr_gpu_set_framebuffer(ctr_settings().early_depth_block32 ? 1 : 0,
                          ctr_settings().depth16 ? 1 : 0);
  // (AI-assisted) hardware hang bisect switches (config.ini, CtrSettings.h)
  ctr_gpu_set_shader_basic(ctr_settings().shader_basic);
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
  ctr_gpu_set_early_depth(ctr_settings().early_depth ? 1 : 0);
  ctr_gpu_set_proctex(ctr_settings().proctex_glows ? 1 : 0);
  // (AI-assisted) impostors: their texture in VRAM before the level texture pools fill it
  if (ctr_settings().impostors) {
    ctr_impostors().reserve();
  }
  ctr_gpu_set_vram_textures(ctr_settings().vram_textures ? 1 : 0);
  ctr_gpu_set_mip_mode(ctr_settings().mipmaps);
  ctr_gpu_set_overlap(ctr_settings().overlap ? 1 : 0);
  ctr_gpu_set_raw_cmds(ctr_settings().raw_cmds ? 1 : 0);
  ctr_gpu_set_vcache_reuse(ctr_settings().vcache_reuse ? 1 : 0);
  ctr_gpu_set_cmd_dump(ctr_settings().cmd_dump ? 1 : 0);  // (AI-assisted)
  ctr_gpu_set_gpu_breakdown(ctr_settings().gpu_breakdown ? 1 : 0);  // (AI-assisted, gpu-rt)
  // (AI-assisted, gpu-rt) its linear memory block now, before levels load
  ctr_gpu_set_static_cmdlists(ctr_settings().static_cmdlists);
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
  if (ctr_settings().render_watchdog) {
    ctr_gpu_diag_start();  // (AI-assisted) config.ini render_watchdog
  }
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

u32 ctr_vsync() {
  if (MasterExit == RuntimeExitStatus::RUNNING) {
    const double t0 = ctr_gpu_time_ms();
    // config.ini fps_cap: 30 = two vblanks per frame at least, 60 = one
    ctr_gpu_wait_vblank(ctr_settings().fps_cap <= 30 ? 2 : 1);
    g_ee.vsync_ms += ctr_gpu_time_ms() - t0;
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
  ctr_gpu_note_release();
  float game_cam[3];
  const bool have_game_cam = game_camera_trans(game_cam);
  if (g_async) {
    wait_render_idle();
    if (have_game_cam) {
      g_ctr->levels().note_game_camera(game_cam);  // (AI-assisted, title3; render thread idle)
    }
    const double tp = ctr_gpu_time_ms();
    g_ctr->prepare_frame(data, offset);
    g_ee.prepare_ms += ctr_gpu_time_ms() - tp;
    g_job_mem = data;
    g_job_offset = offset;
    ctr_gpu_async_submit();
  } else {
#ifndef __3DS__
    // (AI-assisted) host (synchronous): the sprites from the snapshot too, so the host runs check
    // it (CtrSpriteRenderer::snap_data compares it with game memory)
    g_ctr->prepare_sprites(data, offset);
#endif
    if (have_game_cam) {
      g_ctr->levels().note_game_camera(game_cam);  // (AI-assisted, title3)
    }
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

void ctr_set_levels(const std::vector<std::string>& levels) {
  if (g_ctr) {
    wait_render_idle();
    g_ctr->levels().set_wanted(levels);
    g_ctr->note_levels(levels);
    CtrOceanFull::note_levels(levels);  // (AI-assisted) its log: once per level
  }
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
bool ctr_level_ready(const char* name) {
  constexpr double kMaxWaitMs = 20000.0;
  if (!name || !g_ctr_ready) {
    return true;
  }
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
  if (g_ctr->levels().needs_service()) {
    wait_render_idle();
    g_ctr->levels().service();
  }
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
