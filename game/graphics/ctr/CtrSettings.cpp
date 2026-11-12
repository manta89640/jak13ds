/*!
 * @file CtrSettings.cpp
 * (AI-assisted)
 * See CtrSettings.h.
 */

#include "CtrSettings.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <vector>

#include "common/log/log.h"
#include "common/util/FileUtil.h"

#include "game/graphics/ctr/CtrDetail.h"
#include "game/graphics/ctr/ctr_config_presets.h"
#include "game/runtime.h"
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

// (AI-assisted) no warnings while comparing settings for the log (load's differs)
bool s_quiet = false;

bool parse_bool(const std::string& v) {
  return v == "1" || v == "true" || v == "on" || v == "yes";
}

// (AI-assisted) config.ini's lines: each key's last value and its line number, the keys in file order
struct FileLines {
  std::vector<std::string> keys;
  std::map<std::string, std::pair<std::string, int>> last;
};

const char* file_get(void* ctx, const char* key) {
  const auto* fl = (const FileLines*)ctx;
  const auto it = fl->last.find(key);
  return it == fl->last.end() ? nullptr : it->second.first.c_str();
}

// (AI-assisted) one key's value into the settings (path, n: where it came from, for the warnings;
// n = 0: the preset or a default)
// (AI-assisted, levellod) the distance scale keys' floor: 0 gave "no limit" (limit_with(0) in
// CtrLevel.cpp draw_level: every shrub / tie out to draw_distance), the opposite of "lowest"
constexpr float kMinScale = 0.05f;
void apply_setting(CtrSettings& s, const std::string& key, const std::string& v, const fs::path& path,
                   int n) {
  const float f = (float)std::atof(v.c_str());
  {
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
      s.detail_scale = std::clamp(f, 0.1f, 4.f);  // (AI-assisted, levellod: 0 drew everything far)
    } else if (key == "shrub_distance") {
      s.shrub_distance = std::max(0.f, f);
    } else if (key == "tie_distance_scale") {
      s.tie_distance_scale = v == "none" ? 0.f : std::max(kMinScale, f);
    } else if (key == "shrub_scale") {
      s.shrub_scale = std::max(kMinScale, f);  // (AI-assisted)
    } else if (key == "merc") {
      s.merc = parse_bool(v);
    } else if (key == "sprites") {
      s.sprites = parse_bool(v);
    } else if (key == "ocean") {
      s.ocean = !(v == "off" || v == "0" || v == "false" || v == "no");
    } else if (key == "ocean_texture_rate") {
      s.ocean_texture_rate = std::clamp(std::atoi(v.c_str()), 1, 8);  // (AI-assisted)
    } else if (key == "distort") {
      s.distort = parse_bool(v);
    } else if (key == "envmap") {
      s.envmap = parse_bool(v);
    } else if (key == "merc_gen_lod") {
      s.merc_gen_lod = parse_bool(v);  // (AI-assisted)
    } else if (key == "merc_lod_scale") {
      // the game's own LOD distances: kmachine.cpp reads it
    } else if (key == "shadows") {
      // full | off (on = full; the removed all = full, blob = off)
      if (v == "full" || v == "all" || parse_bool(v)) {
        s.shadows = CtrSettings::ShadowMode::Full;
      } else if (v == "blob" || v == "off" || v == "0" || v == "false" || v == "no") {
        s.shadows = CtrSettings::ShadowMode::Off;
      } else {
        if (!s_quiet) {
          lg::warn("[ctr] {}:{}: shadows = {}: expected full or off", path.string(), n, v);
        }
      }
    } else if (key == "sky") {
      s.sky = parse_bool(v);
    } else if (key == "clouds") {
      s.clouds = parse_bool(v);  // (AI-assisted)
    } else if (key == "overlap") {
      s.overlap = parse_bool(v);
    } else if (key == "pipeline") {
      s.pipeline = parse_bool(v);
    } else if (key == "frame_pacing") {
      s.steady_pacing = v != "fast";
    } else if (key == "fps_cap") {
      s.fps_cap = std::atoi(v.c_str()) >= 60 ? 60 : 30;
    } else if (key == "rgba4_as_rgba8") {
      if (v != "auto") {
        s.rgba4_as_rgba8 = parse_bool(v);
      }
    } else if (key == "texshare") {
      s.texshare = parse_bool(v);
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
    } else if (key == "render_watchdog") {
      s.render_watchdog = parse_bool(v);  // (AI-assisted)
    } else if (key == "stats_period") {
      s.stats_period = std::clamp(f, 1.f, 600.f);  // (AI-assisted)
    } else if (key == "gpu_perflog") {
      s.gpu_perflog = parse_bool(v);  // (AI-assisted)
    } else if (key == "pica_bench") {
      s.pica_bench = parse_bool(v);  // (AI-assisted)
    } else if (key == "compact_textures") {
      s.compact_textures = parse_bool(v);
    } else if (key == "sprite_alpha_discard") {
      s.sprite_alpha_discard = parse_bool(v);  // (AI-assisted)
    } else if (key == "vis_culling") {
      s.vis_culling = parse_bool(v);
    } else if (key == "run_culling") {
      s.run_culling = parse_bool(v);  // (AI-assisted)
    } else if (key == "tie_lod") {
      s.tie_lod = parse_bool(v);  // (AI-assisted) c3l v17
    } else if (key == "streaming") {
      s.streaming = parse_bool(v);  // (AI-assisted, streaming) c3l v19
    } else if (key == "stream_kb") {
      s.stream_kb = std::clamp((int)f, 512, 16384);  // (AI-assisted, streaming)
      s.stream_kb_set = true;                         // (AI-assisted, j2-mem)
    } else if (key == "dormant_levels") {
      s.dormant_levels = parse_bool(v);  // (AI-assisted, j2-mem) Jak II
    } else if (key == "stream_debug") {
      s.stream_debug = parse_bool(v);  // (AI-assisted, streaming)
    } else if (key == "stream_reserve_kb") {
      s.stream_reserve_kb = std::clamp((int)f, 1024, 32768);  // (AI-assisted, streaming)
    } else if (key == "tie_lod_px") {
      s.tie_lod_px = std::clamp(f, 0.25f, 16.f);  // (AI-assisted, title-popin)
    } else if (key == "neighbour_vis") {
      s.neighbour_vis = parse_bool(v);  // (AI-assisted)
    } else if (key == "lowres_vista") {
      s.lowres_vista = parse_bool(v);  // (AI-assisted)
    } else if (key == "vista_distance") {
      s.vista_distance = std::max(0.f, f);  // (AI-assisted)
    } else if (key == "cull_ties") {
      // (AI-assisted) on | off | flip (on, with the other winding)
      s.cull_flip = v == "flip";
      s.cull_ties = s.cull_flip || parse_bool(v);
    } else if (key == "cull_merc") {
      s.cull_merc = parse_bool(v);  // (AI-assisted)
    } else if (key == "merc_fog") {
      s.merc_fog = parse_bool(v);  // (AI-assisted, far-fog)
    } else if (key == "raw_cmds") {
      s.raw_cmds = parse_bool(v);
    } else if (key == "gpu_breakdown") {
      s.gpu_breakdown = parse_bool(v);  // (AI-assisted, gpu-rt)
    } else if (key == "ocean_tex_half") {
      s.ocean_tex_half = parse_bool(v);  // (AI-assisted, gpu-rt)
    } else if (key == "asm_tex") {
      s.asm_tex = parse_bool(v);
    } else if (key == "defer_flush") {
      s.defer_flush = parse_bool(v);
    } else if (key == "asm_mem") {
      s.asm_mem = parse_bool(v);
    } else if (key == "fast_clock") {
      s.fast_clock = parse_bool(v);
    } else if (key == "sound" || key == "sound_core" || key == "kernel_flush" ||
               key == "mips2c_native_off" || key == "perf_sections" || key == "hitch_ms" ||
               key == "args" ||
               key == "listener" || key == "screenshots" || key == "pad_script" ||
               key == "debug_log" || key == "particle_density" || key == "traffic_density" ||
               key == "shield_density" ||
               key == "io_checksum" || key == "anim_lod" || key == "io_cache" ||
               key == "anim_lod_far" || key == "shadow_count" ||  // (AI-assisted, j2cuts)
               key == "vfp_vector" ||  // (AI-assisted, vfpvec) native_bones_vfp.h
               key == "log_async" || key == "c3l_read_buffer" || key == "text_scale" ||
               key == "text_min_height" ||
                 key == "preset" ||
               ctr_cfg_is_user_alias(key.c_str())) {
      // not the renderer's (preset, particles, text_size: ctr_config_presets.h, applied as the
      // keys they stand for) (platform/3ds/port/ctr_port.c, mips2c_native.cpp, kperf.cpp,
      // overlord/jak1/fake_iso.cpp, kernel/jak1/kmachine.cpp, common/log/log.cpp,
      // (AI-assisted) mips2c native code, CtrLevel.cpp)
    } else if (!s_quiet) {
      lg::warn("[ctr] {}:{}: unknown setting {}", path.string(), n, key);
    }
  }
}

// (AI-assisted) the settings from the defaults, the preset and (from_file) config.ini's lines,
// in ctr_config_presets.h's order of precedence. Each key is set once, with the value that wins.
CtrSettings resolve_settings(const CtrSettings& defaults, FileLines& fl, const fs::path& path,
                             bool from_file) {
  CtrSettings s = defaults;
  std::vector<std::string> keys;
  std::set<std::string> seen;
  auto add = [&](const std::string& k) {
    if (seen.insert(k).second) {
      keys.push_back(k);
    }
  };
  for (const auto& kv : ctr_cfg_defaults) {
    add(kv.key);
  }
  const ctr_cfg_preset* preset = ctr_cfg_preset_of(file_get, &fl);
  for (int i = 0; i < preset->n; i++) {
    add(preset->kv[i].key);
  }
  if (from_file) {
    for (const auto& a : ctr_cfg_aliases) {
      if (file_get(&fl, a.user_key)) {
        add(a.key);
      }
    }
    for (const auto& k : fl.keys) {
      add(k);
    }
  }
  for (const auto& k : keys) {
    if (ctr_cfg_is_removed(k.c_str())) {
      continue;
    }
    const char* v = ctr_cfg_resolve(k.c_str(), file_get, &fl, from_file ? 1 : 0);
    if (v) {
      const auto it = fl.last.find(k);
      apply_setting(s, k, v, path, from_file && it != fl.last.end() ? it->second.second : 0);
    }
  }
  return s;
}

CtrSettings load() {
  CtrSettings s;
  // auto defaults: what the emulator draws correctly / what is fast on the 3DS
  s.emulator = ctr_gpu_is_emulator() != 0;
  s.rgba4_as_rgba8 = s.emulator;
  if (g_game_version == GameVersion::Jak2) {
    // Jak II: the distances its city held 30 fps with, until its renderer can afford the PS2's
    s.lod_distance = 200.f;
    s.tie_distance_scale = 1.5f;
    s.tie_lod_px = 4.f;
    s.vista_distance = 3000.f;
    s.shrub_scale = 0.8f;
    s.mipmaps = 1;
  }
  const auto dir = file_util::get_jak_project_dir();
  // (AI-assisted) config.ini, the one settings file (the sound and system keys in it are read by
  // ctr_config_get): sdmc:/3ds/jak1/config.ini on the 3DS (the project dir's parent), the project
  // dir on PC
#ifdef __3DS__
  const fs::path cfg = dir.parent_path() / "config.ini";
#else
  const fs::path cfg = dir / "config.ini";
#endif
  fs::path path;
  if (fs::exists(cfg)) {
    path = cfg;
  }
  FileLines fl;
  if (!path.empty()) {
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
      if (!fl.last.count(key)) {
        fl.keys.push_back(key);
      }
      fl.last[key] = {trim(line.substr(eq + 1)), n};
    }
  }
  // (AI-assisted) the preset and the user keys' values
  const char* preset_name = file_get(&fl, "preset");
  if (preset_name && preset_name[0] && !ctr_cfg_preset_named(preset_name)) {
    lg::warn("[ctr] config.ini: preset = {}: expected normal, full, safe, bench or breakdown; normal",
             preset_name);
  }
  for (const auto& k : fl.keys) {
    const char* v = file_get(&fl, k.c_str());
    if (ctr_cfg_is_user_alias(k.c_str()) && !ctr_cfg_alias_value_ok(k.c_str(), v)) {
      lg::warn("[ctr] config.ini: {} = {}: not a known value, ignored", k, v);
    }
    if (ctr_cfg_is_removed(k.c_str())) {
      lg::info("[ctr] config.ini: {} was removed (config cleanups of Oct 16 / Oct 23), ignored", k);
    }
  }
  const CtrSettings base = resolve_settings(s, fl, path, false);
  s = resolve_settings(s, fl, path, true);
  // (AI-assisted) the lines whose value differs from what the preset (and the defaults) give
  std::string diffs;
  const std::string base_summary = base.summary();
  auto differs = [&](const std::string& k, const char* v) {
    const char* pv = ctr_cfg_resolve(k.c_str(), file_get, &fl, 0);
    if (pv) {
      return !ctr_cfg_same(v, pv);
    }
    CtrSettings t = base;
    s_quiet = true;
    apply_setting(t, k, v, path, 0);
    s_quiet = false;
    return t.summary() != base_summary;
  };
  for (const auto& k : fl.keys) {
    const char* v = file_get(&fl, k.c_str());
    bool d = false;
    if (k == "preset" || ctr_cfg_is_removed(k.c_str())) {
      continue;
    } else if (ctr_cfg_is_user_alias(k.c_str())) {
      for (const auto& a : ctr_cfg_aliases) {
        if (k == a.user_key && (!a.user_value || !strcmp(a.user_value, v))) {
          d = d || differs(a.key, a.value ? a.value : v);
        }
      }
    } else {
      d = differs(k, v);
    }
    if (d) {
      diffs += fmt::format("{}{} = {}", diffs.empty() ? "" : ", ", k, v);
    }
  }
  // (AI-assisted) the keys ctr_config_get serves (sound, I/O, native code, text, ...)
  std::string other;
  for (const auto& kv : ctr_cfg_defaults) {
    const char* v = ctr_cfg_resolve(kv.key, file_get, &fl, 1);
    other += fmt::format("{}{} {}", other.empty() ? "" : " ", kv.key, v ? v : "");
  }
  lg::info("[ctr] settings from {} (preset {}; {}): {} | {}",
           path.empty() ? std::string("defaults, no config.ini") : path.string(),
           ctr_cfg_preset_of(file_get, &fl)->name,
           diffs.empty() ? std::string("no lines differ from it")
                         : "lines that differ from it: " + diffs,
           s.summary(), other);
  return s;
}
}  // namespace

std::string CtrSettings::summary() const {
  // the fixed detail floor (CtrDetail.h): what it changes from the config
  auto floor_text = [&]() {
    const ctr_detail::Floor fl = ctr_detail::floor_of(
        tie_lod_px, lod_distance, tie_distance_scale, draw_distance,
        g_game_version == GameVersion::Jak1, g_game_version == GameVersion::Jak2);
    if (!fl.on) {
      return std::string("no floor");
    }
    std::string r = fmt::format("fixed floor: shrub bands x{:.1f} beyond {:.0f}m", fl.shrub_far,
                                ctr_detail::kNearRingMeters);
    if (fl.tie_far > 0) {
      r += fmt::format(", tie vista copies from {:.0f}m", fl.tie_far);
    }
    if (fl.tie_lod_px != tie_lod_px) {
      r += fmt::format(", tie_lod_px {:.0f}", fl.tie_lod_px);
    }
    if (fl.lod_distance != lod_distance) {
      r += fmt::format(", lod {:.0f}m", fl.lod_distance);
    }
    if (fl.tie_distance_scale != tie_distance_scale) {
      r += fmt::format(", ties x{:.1f}", fl.tie_distance_scale);
    }
    return r;
  };
  return fmt::format(
      "dist {:.0f}m lod {:.0f}m far-level {:.0f}m detail x{:.1f} shrub {} (v14 files: x{}) tie x{} fog {} "
      "merc {} sprites {} "
      "pipeline {} ocean {} sky {} clouds {} distort {} envmap {} merc_gen_lod {} shadows {} rgba4_as_rgba8 {} vram_textures {} mipmaps {} "
      "compact_textures {} sprite_alpha_discard {} "
      "vis_culling {} run_culling {} tie_lod {} streaming {} neighbour_vis {} lowres_vista {} vista {:.0f}m cull_ties {} cull_merc {} merc_fog {} raw_cmds {} asm_tex {} asm_mem {} "
      "ocean_tex_half {} fast_clock {} "
      // (AI-assisted) Oct 16: the rest of the keys, so the line holds every setting
      "fog_start {:.2f} overlap {} frame_pacing {} fps_cap {} ocean_texture_rate {} "
      "defer_flush {} render_watchdog {} stats_period {:.0f}s"
      "{}{}{}{}{}{}",
      draw_distance, lod_distance, far_level_distance, detail_scale,
      fmt::format("{:.0f}m ({})", shrub_distance, floor_text()),
      fmt::format("{:.2f}", shrub_scale),
      !(tie_distance_scale > 0) ? std::string("none") : fmt::format("{:.1f}", tie_distance_scale),
      fog ? "on" : "off",
      merc ? "on" : "off", sprites ? "on" : "off", pipeline ? "on" : "off", ocean ? "on" : "off",
      sky ? "on" : "off", clouds ? "on" : "off", distort ? "on" : "off",
      envmap ? "on" : "off",
      merc_gen_lod ? "on" : "off",
      shadows == ShadowMode::Full ? "full" : "off",
      rgba4_as_rgba8 ? "on" : "off", vram_textures ? "on" : "off",
      mipmaps == 0 ? "off" : (mipmaps == 2 ? "trilinear" : "on"),
      compact_textures ? "on" : "off",
      sprite_alpha_discard ? "on" : "off",
      vis_culling ? "on" : "off", run_culling ? "on" : "off", tie_lod ? "on" : "off",
      streaming ? fmt::format("on ({} KB, reserve {} KB)", stream_kb, stream_reserve_kb)
                : std::string("off"),
      neighbour_vis ? "on" : "off", lowres_vista ? "on" : "off", vista_distance, cull_ties ? (cull_flip ? "flip" : "on") : "off", cull_merc ? "on" : "off",
      merc_fog ? "on" : "off",
      raw_cmds ? "on" : "off",
      asm_tex ? "on" : "off",
      asm_mem ? "on" : "off",
      ocean_tex_half ? "on" : "off",
      fast_clock ? "on" : "off", fog_start, overlap ? "on" : "off", steady_pacing ? "steady" : "fast",
      fps_cap, ocean_texture_rate,
      defer_flush ? "on" : "off", render_watchdog ? "on" : "off",
      stats_period,
      gpu_profile ? " gpu_profile" : "",
      gpu_counters ? " gpu_counters" : "",
      gpu_perflog ? " gpu_perflog" : "",
      pica_bench ? " pica_bench" : "",
      gpu_breakdown ? " gpu_breakdown" : "",
      emulator ? " (emulator)" : "");
}

namespace {
CtrSettings& settings_instance() {
  static CtrSettings s = load();
  return s;
}
}  // namespace

namespace ctr_settings_detail {
const CtrSettings* g_settings = nullptr;
const CtrSettings& first_use() {
  const CtrSettings& s = settings_instance();
  __atomic_store_n(&g_settings, &s, __ATOMIC_RELEASE);
  return s;
}
}  // namespace ctr_settings_detail

namespace ctr_gfx {
std::string settings_summary() {
  return ctr_settings().summary();
}
}  // namespace ctr_gfx
