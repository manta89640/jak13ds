/*!
 * @file CtrSettings.cpp
 * (AI-assisted)
 * See CtrSettings.h.
 */

#include "CtrSettings.h"

#include <algorithm>
#include <cstdlib>
#include <fstream>

#include "common/log/log.h"
#include "common/util/FileUtil.h"

#include "game/graphics/ctr/ctr_gpu.h"

#include "fmt/format.h"

namespace {

std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) {
    return "";
  }
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

bool parse_bool(const std::string& v) {
  return v == "1" || v == "true" || v == "on" || v == "yes";
}

// (AI-assisted) settings that depend on others
void apply_rules(CtrSettings& s) {
  // the shadow volumes need the stencil buffer, which the 16-bit depth buffer doesn't have
  // (kmachine.cpp's choice of what the game builds follows the same rule)
  if (s.shadows == CtrSettings::ShadowMode::Full && s.depth16) {
    s.shadows = CtrSettings::ShadowMode::Blob;
    s.shadows_full_wanted = true;
  }
}

CtrSettings load() {
  CtrSettings s;
  // auto defaults: what the emulator draws correctly / what is fast on the 3DS
  s.emulator = ctr_gpu_is_emulator() != 0;
  s.rgba4_as_rgba8 = s.emulator;
  // (AI-assisted) VRAM in the emulator too: it draws VRAM textures right (only RGBA4 is noise, and
  // that is stored as RGBA8 there). Without it the emulator kept ~4 MB more in linear memory than
  // the 3DS, and a level that fits on the 3DS didn't there (holes, with the pipeline).
  s.vram_textures = true;
  const auto dir = file_util::get_jak_project_dir();
  fs::path path;
  // (AI-assisted) config.ini, the one settings file (the sound and system keys in it are read by
  // ctr_config_get): sdmc:/3ds/jak1/config.ini on the 3DS (the project dir's parent), the project
  // dir on PC
#ifdef __3DS__
  for (const auto& p : {dir.parent_path() / "config.ini"}) {
#else
  for (const auto& p : {dir / "config.ini"}) {
#endif
    if (fs::exists(p)) {
      path = p;
      break;
    }
  }
  if (path.empty()) {
    apply_rules(s);
    lg::info("[ctr] no config.ini, default settings: {}", s.summary());
    return s;
  }
  std::ifstream in(path.string());
  std::string line;
  int n = 0;
  while (std::getline(in, line)) {
    n++;
    line = trim(line.substr(0, line.find_first_of("#;")));
    if (line.empty()) {
      continue;
    }
    auto eq = line.find('=');
    if (eq == std::string::npos) {
      lg::warn("[ctr] {}:{}: expected key = value", path.string(), n);
      continue;
    }
    const std::string key = trim(line.substr(0, eq));
    const std::string v = trim(line.substr(eq + 1));
    const float f = (float)std::atof(v.c_str());
    if (key == "draw_distance") {
      s.draw_distance = f;
    } else if (key == "fog_start") {
      s.fog_start = f;
    } else if (key == "fog") {
      s.fog = parse_bool(v);
    } else if (key == "lod_distance") {
      s.lod_distance = f;
    } else if (key == "far_level_distance") {
      s.far_level_distance = f;
    } else if (key == "detail_scale") {
      s.detail_scale = f;
    } else if (key == "shrub_distance") {
      s.shrub_distance = std::max(0.f, f);
    } else if (key == "tie_distance_scale") {
      s.tie_distance_scale = std::max(0.f, f);
    } else if (key == "auto_detail") {
      s.auto_detail = parse_bool(v);
    } else if (key == "shrub_distance_max") {
      s.shrub_distance_max = std::max(0.f, f);
    } else if (key == "tie_distance_scale_max") {
      s.tie_distance_scale_max = std::max(0.f, f);
    } else if (key == "auto_detail_gpu_ms") {
      s.auto_detail_gpu_ms = std::clamp(f, 10.f, 33.f);
    } else if (key == "merc") {
      s.merc = parse_bool(v);
    } else if (key == "sprites") {
      s.sprites = parse_bool(v);
    } else if (key == "ocean") {
      if (v == "flat" || v == "waves" || v == "full") {
        s.ocean = true;
        s.ocean_mode = v == "full"    ? CtrSettings::OceanMode::Full
                       : v == "waves" ? CtrSettings::OceanMode::Waves
                                      : CtrSettings::OceanMode::Flat;
      } else {
        s.ocean = parse_bool(v);
        s.ocean_mode = CtrSettings::OceanMode::Flat;
      }
    } else if (key == "ocean_texture_rate") {
      s.ocean_texture_rate = std::clamp(std::atoi(v.c_str()), 1, 8);  // (AI-assisted)
    } else if (key == "ocean_near_alpha") {
      s.ocean_near_alpha = std::clamp(f, 0.f, 1.f);  // (AI-assisted)
    } else if (key == "distort") {
      s.distort = parse_bool(v);
    } else if (key == "envmap") {
      s.envmap = parse_bool(v);
    } else if (key == "shadows") {
      // (AI-assisted) full | blob | off (on = full)
      if (v == "full" || parse_bool(v)) {
        s.shadows = CtrSettings::ShadowMode::Full;
      } else if (v == "blob") {
        s.shadows = CtrSettings::ShadowMode::Blob;
      } else if (v == "off" || v == "0" || v == "false" || v == "no") {
        s.shadows = CtrSettings::ShadowMode::Off;
      } else {
        lg::warn("[ctr] {}:{}: shadows = {}: expected full, blob or off", path.string(), n, v);
      }
    } else if (key == "sky") {
      s.sky = parse_bool(v);
    } else if (key == "overlap") {
      s.overlap = parse_bool(v);
    } else if (key == "pipeline") {
      s.pipeline = parse_bool(v);
    } else if (key == "frame_pacing") {
      s.steady_pacing = v != "fast";
    } else if (key == "fps_cap") {
      s.fps_cap = std::atoi(v.c_str()) >= 60 ? 60 : 30;
    } else if (key == "max_sprites") {
      s.max_sprites = std::atoi(v.c_str());
    } else if (key == "rgba4_as_rgba8") {
      if (v != "auto") {
        s.rgba4_as_rgba8 = parse_bool(v);
      }
    } else if (key == "vram_textures") {
      if (v != "auto") {
        s.vram_textures = parse_bool(v);
      }
    } else if (key == "mipmaps") {
      s.mipmaps = v == "off" || v == "0" ? 0 : (v == "trilinear" || v == "2" ? 2 : 1);
    } else if (key == "gpu_profile") {
      s.gpu_profile = parse_bool(v);
    } else if (key == "gpu_counters") {
      s.gpu_counters = parse_bool(v);
    } else if (key == "gpu_perflog") {
      s.gpu_perflog = parse_bool(v);  // (AI-assisted)
    } else if (key == "color16") {
      s.color16 = parse_bool(v);
    } else if (key == "early_depth") {
      s.early_depth_block32 = v == "block32";
      s.early_depth = s.early_depth_block32 || parse_bool(v);
    } else if (key == "depth16") {
      s.depth16 = parse_bool(v);
    } else if (key == "compact_textures") {
      s.compact_textures = parse_bool(v);
    } else if (key == "proctex_glows") {
      s.proctex_glows = parse_bool(v);
    } else if (key == "sprite_max_size") {
      s.sprite_max_size = std::max(0.f, f);
    } else if (key == "vis_culling") {
      s.vis_culling = parse_bool(v);
    } else if (key == "raw_cmds") {
      s.raw_cmds = parse_bool(v);
    } else if (key == "vcache_reuse") {
      s.vcache_reuse = parse_bool(v);
    } else if (key == "new_shaders") {
      s.new_shaders = parse_bool(v);
    } else if (key == "asm_tex") {
      s.asm_tex = parse_bool(v);
    } else if (key == "defer_flush") {
      s.defer_flush = parse_bool(v);
    } else if (key == "shader_basic") {
      s.shader_basic = v == "new" ? 1 : (v == "old" ? 0 : -1);
    } else if (key == "shader_mesh") {
      s.shader_mesh = v == "new" ? 1 : (v == "dph" ? 2 : (v == "old" ? 0 : -1));
    } else if (key == "asm_mem") {
      s.asm_mem = parse_bool(v);
    } else if (key == "fast_clock") {
      s.fast_clock = parse_bool(v);
    } else if (key == "sound" || key == "sound_core" || key == "io_on_system_core" ||
               key == "mips2c_native_off" || key == "perf_sections" || key == "args" ||
               key == "listener" || key == "screenshots" || key == "pad_script" ||
               key == "debug_log" || key == "particle_density" || key == "merc_lod_scale" ||
               key == "io_checksum" || key == "anim_lod" || key == "anim_native" ||
               key == "io_cache" || key == "log_async") {
      // not the renderer's (platform/3ds/port/ctr_port.c, mips2c_native.cpp, kperf.cpp,
      // overlord/jak1/fake_iso.cpp, kernel/jak1/kmachine.cpp, common/log/log.cpp)
    } else {
      lg::warn("[ctr] {}:{}: unknown setting {}", path.string(), n, key);
    }
  }
  apply_rules(s);
  lg::info("[ctr] settings from {}: {}", path.string(), s.summary());
  return s;
}
}  // namespace

std::string CtrSettings::summary() const {
  return fmt::format(
      "dist {:.0f}m lod {:.0f}m far-level {:.0f}m detail x{:.1f} shrub {} tie x{} fog {} "
      "merc {} sprites {} ({}) "
      "ocean {} sky {} distort {} envmap {} shadows {} rgba4_as_rgba8 {} vram_textures {} mipmaps {} "
      "color16 {} early_depth {} compact_textures {} proctex_glows {} sprite_max_size {:.2f} "
      "vis_culling {} raw_cmds {} vcache_reuse {} new_shaders {} asm_tex {} asm_mem {} "
      "fast_clock {}{}{}{}{}{}",
      draw_distance, lod_distance, far_level_distance, detail_scale,
      // (AI-assisted) with auto_detail: the range the distances move in
      auto_detail ? fmt::format("{:.0f}-{:.0f}m (auto, GPU under {:.0f} ms)", shrub_distance,
                                std::max(shrub_distance, shrub_distance_max), auto_detail_gpu_ms)
                  : fmt::format("{:.0f}m", shrub_distance),
      auto_detail ? fmt::format("{:.1f}-{:.1f}", tie_distance_scale,
                                std::max(tie_distance_scale, tie_distance_scale_max))
                  : fmt::format("{:.1f}", tie_distance_scale),
      fog ? "on" : "off",
      merc ? "on" : "off", sprites ? "on" : "off", max_sprites, !ocean ? "off" : (ocean_mode == OceanMode::Full ? "full" : (ocean_mode == OceanMode::Waves ? "waves" : "flat")),
      sky ? "on" : "off", distort ? "on" : "off", envmap ? "on" : "off",
      // (AI-assisted) with depth16, full falls back to blob (no stencil)
      shadows == ShadowMode::Full   ? "full"
      : shadows == ShadowMode::Blob ? (shadows_full_wanted ? "blob (full needs the stencil: depth16)"
                                                           : "blob")
                                    : "off",
      rgba4_as_rgba8 ? "on" : "off", vram_textures ? "on" : "off",
      mipmaps == 0 ? "off" : (mipmaps == 2 ? "trilinear" : "on"), color16 ? "on" : "off",
      early_depth ? (early_depth_block32 ? "block32" : "on") : "off", compact_textures ? "on" : "off", proctex_glows ? "on" : "off",
      sprite_max_size, vis_culling ? "on" : "off", raw_cmds ? "on" : "off",
      vcache_reuse ? "on" : "off", new_shaders ? "on" : "off", asm_tex ? "on" : "off",
      asm_mem ? "on" : "off", fast_clock ? "on" : "off", gpu_profile ? " gpu_profile" : "",
      gpu_counters ? "" : " gpu_counters off",
      gpu_perflog ? " gpu_perflog" : "", depth16 ? " depth16" : "",
      emulator ? " (emulator)" : "");
}

const CtrSettings& ctr_settings() {
  static const CtrSettings s = load();
  return s;
}

namespace ctr_gfx {
std::string settings_summary() {
  return ctr_settings().summary();
}
}  // namespace ctr_gfx
