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

#include "game/graphics/ctr/ctr_config_presets.h"
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

// (AI-assisted) settings that depend on others
void apply_rules(CtrSettings& s) {
  // the shadow volumes need the stencil buffer, which the 16-bit depth buffer doesn't have
  // (kmachine.cpp's choice of what the game builds follows the same rule)
  if (s.shadows == CtrSettings::ShadowMode::Full && s.depth16) {
    s.shadows = CtrSettings::ShadowMode::Blob;
    s.shadows_full_wanted = true;
  }
  // (AI-assisted) early_depth = sky / sky32: the early test is for the sky drawn last
  if (s.early_depth_sky) {
    s.sky_last = true;
  }
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
      s.detail_scale = f;
    } else if (key == "shrub_distance") {
      s.shrub_distance = std::max(0.f, f);
    } else if (key == "tie_distance_scale") {
      s.tie_distance_scale = std::max(0.f, f);
    } else if (key == "auto_detail") {
      s.auto_detail = parse_bool(v);
    } else if (key == "shrub_scale") {
      s.shrub_scale = std::max(0.f, f);  // (AI-assisted)
    } else if (key == "shrub_scale_max") {
      s.shrub_scale_max = std::max(0.f, f);  // (AI-assisted)
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
    } else if (key == "impostors") {
      s.impostors = parse_bool(v);  // (AI-assisted)
    } else if (key == "impostor_distance") {
      s.impostor_distance = std::clamp(f, 30.f, 3000.f);  // (AI-assisted)
    } else if (key == "impostor_error") {
      s.impostor_error = std::clamp(f, 0.25f, 8.f);  // (AI-assisted)
    } else if (key == "impostor_budget") {
      s.impostor_budget = std::clamp(std::atoi(v.c_str()), 0, 100000);  // (AI-assisted)
    } else if (key == "impostor_atlas") {
      const int kb = std::atoi(v.c_str());  // (AI-assisted)
      s.impostor_atlas = kb >= 1024 ? 1024 : (kb >= 512 ? 512 : 256);
    } else if (key == "merc_gen_lod") {
      s.merc_gen_lod = parse_bool(v);  // (AI-assisted)
    } else if (key == "merc_lod_scale") {
      s.merc_lod_scale = std::clamp(f, 0.05f, 1.f);  // (AI-assisted) (kmachine.cpp reads it too)
    } else if (key == "merc_tex_sort") {
      s.merc_tex_sort = parse_bool(v);  // (AI-assisted)
    } else if (key == "envmap_batch") {
      s.envmap_batch = parse_bool(v);  // (AI-assisted)
    } else if (key == "distort_copy") {
      s.distort_copy_rows = v != "full";  // (AI-assisted) rows | full
    } else if (key == "shadows") {
      // (AI-assisted) full | blob | off (on = full)
      if (v == "full" || parse_bool(v)) {
        s.shadows = CtrSettings::ShadowMode::Full;
      } else if (v == "blob") {
        s.shadows = CtrSettings::ShadowMode::Blob;
      } else if (v == "off" || v == "0" || v == "false" || v == "no") {
        s.shadows = CtrSettings::ShadowMode::Off;
      } else {
        if (!s_quiet) {
          lg::warn("[ctr] {}:{}: shadows = {}: expected full, blob or off", path.string(), n, v);
        }
      }
    } else if (key == "shadow_casters") {
      s.shadow_casters = std::clamp(std::atoi(v.c_str()), 0, 64);  // (AI-assisted)
    } else if (key == "sky") {
      s.sky = parse_bool(v);
    } else if (key == "clouds") {
      s.clouds = parse_bool(v);  // (AI-assisted)
    } else if (key == "sky_last") {
      s.sky_last = parse_bool(v);  // (AI-assisted)
    } else if (key == "skip_clear") {
      s.skip_clear = parse_bool(v);  // (AI-assisted)
    } else if (key == "overlap") {
      s.overlap = parse_bool(v);
    } else if (key == "pipeline") {
      s.pipeline = parse_bool(v);
    } else if (key == "frame_overlap2") {
      // (AI-assisted) off | clean | on
      s.frame_overlap2 = v == "on" || v == "2" ? 2 : (v == "clean" || v == "1" ? 1 : 0);
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
    } else if (key == "mip_bias") {
      s.mip_bias = std::clamp(f, -4.f, 4.f);  // (AI-assisted)
    } else if (key == "skin_rows_f24") {
      s.skin_rows_f24 = parse_bool(v);  // (AI-assisted)
    } else if (key == "mipmaps") {
      s.mipmaps = v == "off" || v == "0" ? 0 : (v == "trilinear" || v == "2" ? 2 : 1);
    } else if (key == "gpu_profile") {
      s.gpu_profile_ab = v == "ab";  // (AI-assisted)
      s.gpu_profile = !s.gpu_profile_ab && parse_bool(v);
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
    } else if (key == "color16") {
      s.color16 = parse_bool(v);
    } else if (key == "early_depth") {
      // (AI-assisted) off | on | block32 | sky | sky32
      s.early_depth_block32 = v == "block32" || v == "sky32";
      s.early_depth_sky = v == "sky" || v == "sky32";
      s.early_depth = v == "block32" || (!s.early_depth_sky && parse_bool(v));
    } else if (key == "early_depth_func") {
      // (AI-assisted) the early test's function: ge (default) | gt | le | lt
      s.early_depth_func = v == "gt" ? 1 : (v == "le" ? 2 : (v == "lt" ? 3 : 0));
    } else if (key == "early_depth_clear") {
      // (AI-assisted) the early depth data's clear value: 0 (default) | max | a number (24 bits)
      s.early_depth_clear =
          v == "max" ? 0xFFFFFFu : (unsigned int)(std::strtoul(v.c_str(), nullptr, 0) & 0xFFFFFFu);
    } else if (key == "depth16") {
      s.depth16 = parse_bool(v);
    } else if (key == "compact_textures") {
      s.compact_textures = parse_bool(v);
    } else if (key == "proctex_glows") {
      s.proctex_glows = parse_bool(v);
    } else if (key == "sprite_max_size") {
      s.sprite_max_size = std::max(0.f, f);
    } else if (key == "sprite_min_alpha") {
      s.sprite_min_alpha = std::clamp(std::atoi(v.c_str()), 2, 64);  // (AI-assisted)
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
    } else if (key == "stream_reserve_kb") {
      s.stream_reserve_kb = std::clamp((int)f, 1024, 32768);  // (AI-assisted, streaming)
    } else if (key == "tod_busy_half") {
      s.tod_busy_half = parse_bool(v);  // (AI-assisted)
    } else if (key == "tie_lod_px") {
      s.tie_lod_px = std::clamp(f, 0.25f, 16.f);  // (AI-assisted, title-popin)
    } else if (key == "neighbour_vis") {
      s.neighbour_vis = parse_bool(v);  // (AI-assisted)
    } else if (key == "fine_vis") {
      s.fine_vis = parse_bool(v);  // (AI-assisted)
    } else if (key == "lowres_vista") {
      s.lowres_vista = parse_bool(v);  // (AI-assisted)
    } else if (key == "vista_distance") {
      s.vista_distance = std::max(0.f, f);  // (AI-assisted)
    } else if (key == "fog_stage_skip") {
      s.fog_stage_skip = parse_bool(v);  // (AI-assisted)
    } else if (key == "vista_untextured") {
      s.vista_untextured = parse_bool(v);  // (AI-assisted)
    } else if (key == "fog_alpha") {
      s.fog_alpha = parse_bool(v);  // (AI-assisted)
    } else if (key == "tex1_gating") {
      s.tex1_gating = parse_bool(v);  // (AI-assisted)
    } else if (key == "shader_opt") {
      s.shader_opt = parse_bool(v);  // (AI-assisted)
    } else if (key == "cmd_dedup") {
      s.cmd_dedup = parse_bool(v);  // (AI-assisted)
    } else if (key == "cmd_dedup_draw") {
      s.cmd_dedup_draw = parse_bool(v);  // (AI-assisted)
    } else if (key == "skin_clip_f24") {
      s.skin_clip_f24 = parse_bool(v);  // (AI-assisted)
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
    } else if (key == "vcache_reuse") {
      s.vcache_reuse = parse_bool(v);
    } else if (key == "static_cmdlists") {
      // (AI-assisted, gpu-rt) on | off | compare
      s.static_cmdlists = v == "compare" ? 2 : (parse_bool(v) ? 1 : 0);
    } else if (key == "gpu_breakdown") {
      s.gpu_breakdown = parse_bool(v);  // (AI-assisted, gpu-rt)
    } else if (key == "ocean_tex_half") {
      s.ocean_tex_half = parse_bool(v);  // (AI-assisted, gpu-rt)
    } else if (key == "opaque_alpha") {
      s.opaque_alpha = parse_bool(v);  // (AI-assisted)
    } else if (key == "cmd_dump") {
      s.cmd_dump = parse_bool(v);  // (AI-assisted)
    } else if (key == "tex_cache_clear") {
      s.tex_cache_uploads = v == "uploads";  // (AI-assisted) always | uploads
    } else if (key == "asm_tex") {
      s.asm_tex = parse_bool(v);
    } else if (key == "defer_flush") {
      s.defer_flush = parse_bool(v);
    } else if (key == "shader_basic") {
      s.shader_basic = v == "old" ? 0 : 1;  // (AI-assisted) new (default) | old
    } else if (key == "asm_mem") {
      s.asm_mem = parse_bool(v);
    } else if (key == "fast_clock") {
      s.fast_clock = parse_bool(v);
    } else if (key == "sound" || key == "sound_core" ||
               key == "mips2c_native_off" || key == "perf_sections" || key == "args" ||
               key == "listener" || key == "screenshots" || key == "pad_script" ||
               key == "debug_log" || key == "particle_density" ||
               key == "io_checksum" || key == "anim_lod" || key == "anim_native" ||
               key == "io_cache" || key == "log_async" || key == "collide_native" ||
               key == "collide_frag_cache" || key == "bones_native" ||
               key == "c3l_read_buffer" || key == "text_scale" || key == "text_min_height" ||
               key == "button_art" || key == "preset" || key == "gpu_micro" ||
               key == "shader_resident" || key == "gpu_micro_ident" ||
               key == "logic_micro" ||
               ctr_cfg_is_user_alias(key.c_str())) {
      // not the renderer's (preset, particles, text_size: ctr_config_presets.h, applied as the
      // keys they stand for) (platform/3ds/port/ctr_port.c, mips2c_native.cpp, kperf.cpp,
      // overlord/jak1/fake_iso.cpp, kernel/jak1/kmachine.cpp, common/log/log.cpp,
      // (AI-assisted) native_collide_cache.cpp, native_bones.cpp, CtrLevel.cpp)
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
  // (AI-assisted) VRAM in the emulator too: it draws VRAM textures right (only RGBA4 is noise, and
  // that is stored as RGBA8 there). Without it the emulator kept ~4 MB more in linear memory than
  // the 3DS, and a level that fits on the 3DS didn't there (holes, with the pipeline).
  s.vram_textures = true;
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
    lg::warn("[ctr] config.ini: preset = {}: expected normal, safe, bench or breakdown; normal",
             preset_name);
  }
  for (const auto& k : fl.keys) {
    const char* v = file_get(&fl, k.c_str());
    if (ctr_cfg_is_user_alias(k.c_str()) && !ctr_cfg_alias_value_ok(k.c_str(), v)) {
      lg::warn("[ctr] config.ini: {} = {}: not a known value, ignored", k, v);
    }
    if (ctr_cfg_is_removed(k.c_str())) {
      lg::info("[ctr] config.ini: {} was removed (Oct 16 config cleanup), ignored", k);
    }
  }
  const CtrSettings base = resolve_settings(s, fl, path, false);
  s = resolve_settings(s, fl, path, true);
  apply_rules(s);
  // (AI-assisted) the lines whose value differs from what the preset (and the defaults) give
  std::string diffs;
  CtrSettings ruled_base = base;
  apply_rules(ruled_base);
  const std::string base_summary = ruled_base.summary();
  auto differs = [&](const std::string& k, const char* v) {
    const char* pv = ctr_cfg_resolve(k.c_str(), file_get, &fl, 0);
    if (pv) {
      return !ctr_cfg_same(v, pv);
    }
    CtrSettings t = base;
    s_quiet = true;
    apply_setting(t, k, v, path, 0);
    s_quiet = false;
    apply_rules(t);
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
  return fmt::format(
      "dist {:.0f}m lod {:.0f}m far-level {:.0f}m detail x{:.1f} shrub {} (v14 files: x{}) tie x{} fog {} "
      "merc {} sprites {} ({}) "
      "pipeline {} frame_overlap2 {} ocean {} sky {} clouds {} sky_last {} skip_clear {} distort {} distort_copy {} envmap {} envmap_batch {} merc_tex_sort {} merc_gen_lod {} (x{:.2f}) shadows {} shadow_casters {} rgba4_as_rgba8 {} vram_textures {} mipmaps {} mip_bias {:.2f} skin_rows_f24 {} skin_clip_f24 {} "
      "color16 {} early_depth {}{} compact_textures {} proctex_glows {} sprite_max_size {:.2f} sprite_min_alpha {} sprite_alpha_discard {} "
      "vis_culling {} run_culling {} tie_lod {} streaming {} tod_busy_half {} neighbour_vis {} fine_vis {} lowres_vista {} vista {:.0f}m fog_stage_skip {} vista_untextured {} fog_alpha {} tex1_gating {} shader_opt {} cmd_dedup {} cull_ties {} cull_merc {} merc_fog {} impostors {} raw_cmds {} vcache_reuse {} opaque_alpha {} tex_cache_clear {} asm_tex {} asm_mem {} "
      "static_cmdlists {} ocean_tex_half {} fast_clock {} "
      // (AI-assisted) Oct 16: the rest of the keys, so the line holds every setting
      "fog_start {:.2f} overlap {} frame_pacing {} fps_cap {} ocean_texture_rate {} ocean_near_alpha {:.2f} "
      "shader_basic {} defer_flush {} cmd_dump {} render_watchdog {} stats_period {:.0f}s"
      "{}{}{}{}{}{}{}",
      draw_distance, lod_distance, far_level_distance, detail_scale,
      // (AI-assisted) with auto_detail: the range the distances move in
      auto_detail ? fmt::format("{:.0f}-{:.0f}m (auto, GPU under {:.0f} ms)", shrub_distance,
                                std::max(shrub_distance, shrub_distance_max), auto_detail_gpu_ms)
                  : fmt::format("{:.0f}m", shrub_distance),
      auto_detail && shrub_scale_max > shrub_scale
          ? fmt::format("{:.2f}-{:.2f}", shrub_scale, shrub_scale_max)
          : fmt::format("{:.2f}", shrub_scale),
      auto_detail ? fmt::format("{:.1f}-{:.1f}", tie_distance_scale,
                                std::max(tie_distance_scale, tie_distance_scale_max))
                  : fmt::format("{:.1f}", tie_distance_scale),
      fog ? "on" : "off",
      merc ? "on" : "off", sprites ? "on" : "off", max_sprites, pipeline ? "on" : "off",
      frame_overlap2 == 2 ? "on" : (frame_overlap2 == 1 ? "clean" : "off"), !ocean ? "off" : (ocean_mode == OceanMode::Full ? "full" : (ocean_mode == OceanMode::Waves ? "waves" : "flat")),
      sky ? "on" : "off", clouds ? "on" : "off", sky_last ? "on" : "off",
      skip_clear ? "on" : "off", distort ? "on" : "off", distort_copy_rows ? "rows" : "full",
      envmap ? "on" : "off", envmap_batch ? "on" : "off", merc_tex_sort ? "on" : "off",
      merc_gen_lod ? "on" : "off", merc_lod_scale,
      // (AI-assisted) with depth16, full falls back to blob (no stencil)
      shadows == ShadowMode::Full   ? "full"
      : shadows == ShadowMode::Blob ? (shadows_full_wanted ? "blob (full needs the stencil: depth16)"
                                                           : "blob")
                                    : "off",
      shadow_casters,
      rgba4_as_rgba8 ? "on" : "off", vram_textures ? "on" : "off",
      mipmaps == 0 ? "off" : (mipmaps == 2 ? "trilinear" : "on"), mip_bias,
      skin_rows_f24 ? "on" : "off", skin_clip_f24 ? "on" : "off", color16 ? "on" : "off",
      early_depth ? (early_depth_block32 ? "block32" : "on")
                  : (early_depth_sky ? (early_depth_block32 ? "sky32" : "sky") : "off"),
      early_depth_func || early_depth_clear
          ? fmt::format(" (func {} clear {:#x})",
                        early_depth_func == 1 ? "gt"
                                              : (early_depth_func == 2 ? "le"
                                                                       : (early_depth_func == 3 ? "lt" : "ge")),
                        early_depth_clear)
          : std::string(),
      compact_textures ? "on" : "off", proctex_glows ? "on" : "off",
      sprite_max_size, sprite_min_alpha, sprite_alpha_discard ? "on" : "off",
      vis_culling ? "on" : "off", run_culling ? "on" : "off", tie_lod ? "on" : "off",
      streaming ? fmt::format("on ({} KB, reserve {} KB)", stream_kb, stream_reserve_kb)
                : std::string("off"),
      tod_busy_half ? "on" : "off", neighbour_vis ? "on" : "off", fine_vis ? "on" : "off", lowres_vista ? "on" : "off", vista_distance, fog_stage_skip ? "on" : "off", vista_untextured ? "on" : "off", fog_alpha ? "on" : "off", tex1_gating ? "on" : "off", shader_opt ? "on" : "off", cmd_dedup ? (cmd_dedup_draw ? "on+draw" : "on") : "off", cull_ties ? (cull_flip ? "flip" : "on") : "off", cull_merc ? "on" : "off",
      merc_fog ? "on" : "off",
      impostors ? fmt::format("on ({:.0f}m, {:.2f}px, {} tris, {} KB)", impostor_distance,
                              impostor_error, impostor_budget, impostor_atlas)
                : std::string("off"),
      raw_cmds ? "on" : "off",
      vcache_reuse ? "on" : "off", opaque_alpha ? "on" : "off",
      tex_cache_uploads ? "uploads" : "always",
      asm_tex ? "on" : "off",
      asm_mem ? "on" : "off",
      static_cmdlists == 2 ? "compare" : (static_cmdlists ? "on" : "off"), ocean_tex_half ? "on" : "off",
      fast_clock ? "on" : "off", fog_start, overlap ? "on" : "off", steady_pacing ? "steady" : "fast",
      fps_cap, ocean_texture_rate, ocean_near_alpha, shader_basic ? "new" : "old",
      defer_flush ? "on" : "off", cmd_dump ? "on" : "off", render_watchdog ? "on" : "off",
      stats_period, gpu_profile ? " gpu_profile" : (gpu_profile_ab ? " gpu_profile ab" : ""),
      gpu_counters ? " gpu_counters" : "",
      gpu_perflog ? " gpu_perflog" : "", pica_bench ? " pica_bench" : "",
      gpu_breakdown ? " gpu_breakdown" : "",
      depth16 ? " depth16" : "",
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

CtrSettings& ctr_settings_ab() {
  return settings_instance();
}

namespace ctr_gfx {
std::string settings_summary() {
  return ctr_settings().summary();
}
}  // namespace ctr_gfx
