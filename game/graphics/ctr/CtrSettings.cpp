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
    } else if (key == "max_sprites") {
      s.max_sprites = std::atoi(v.c_str());
    } else {
      lg::warn("[ctr] {}:{}: unknown setting {}", path.string(), n, key);
    }
  }
  lg::info("[ctr] settings from {}: {}", path.string(), s.summary());
  return s;
}
}  // namespace

std::string CtrSettings::summary() const {
  return fmt::format("dist {:.0f}m lod {:.0f}m far-level {:.0f}m detail x{:.1f} fog {} merc {} sprites {} ({})",
                     draw_distance, lod_distance, far_level_distance, detail_scale, fog ? "on" : "off",
                     merc ? "on" : "off", sprites ? "on" : "off", max_sprites);
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
