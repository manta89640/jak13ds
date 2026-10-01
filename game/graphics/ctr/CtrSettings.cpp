/*!
 * @file CtrSettings.cpp
 * (AI-assisted)
 * See CtrSettings.h.
 */

#include "CtrSettings.h"

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

CtrSettings load() {
  CtrSettings s;
  // auto defaults: what the emulator draws correctly / what is fast on the 3DS
  s.emulator = ctr_gpu_is_emulator() != 0;
  s.rgba4_as_rgba8 = s.emulator;
  s.vram_textures = !s.emulator;
  const auto dir = file_util::get_jak_project_dir();
  fs::path path;
  for (const auto& p : {dir / "render.ini", dir.parent_path() / "render.ini"}) {
    if (fs::exists(p)) {
      path = p;
      break;
    }
  }
  if (path.empty()) {
    lg::info("[ctr] no render.ini, default settings: {}", s.summary());
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
    } else if (key == "merc") {
      s.merc = parse_bool(v);
    } else if (key == "sprites") {
      s.sprites = parse_bool(v);
    } else if (key == "ocean") {
      s.ocean = parse_bool(v);
    } else if (key == "distort") {
      s.distort = parse_bool(v);
    } else if (key == "envmap") {
      s.envmap = parse_bool(v);
    } else if (key == "sky") {
      s.sky = parse_bool(v);
    } else if (key == "overlap") {
      s.overlap = parse_bool(v);
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
    } else {
      lg::warn("[ctr] {}:{}: unknown setting {}", path.string(), n, key);
    }
  }
  lg::info("[ctr] settings from {}: {}", path.string(), s.summary());
  return s;
}
}  // namespace

std::string CtrSettings::summary() const {
  return fmt::format(
      "dist {:.0f}m lod {:.0f}m far-level {:.0f}m detail x{:.1f} fog {} merc {} sprites {} ({}) "
      "ocean {} sky {} distort {} envmap {} rgba4_as_rgba8 {} vram_textures {} mipmaps {}{}{}",
      draw_distance, lod_distance, far_level_distance, detail_scale, fog ? "on" : "off",
      merc ? "on" : "off", sprites ? "on" : "off", max_sprites, ocean ? "on" : "off",
      sky ? "on" : "off", distort ? "on" : "off", envmap ? "on" : "off", rgba4_as_rgba8 ? "on" : "off", vram_textures ? "on" : "off",
      mipmaps == 0 ? "off" : (mipmaps == 2 ? "trilinear" : "on"), gpu_profile ? " gpu_profile" : "",
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
