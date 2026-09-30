#pragma once

/*!
 * @file CtrSettings.h
 * (AI-assisted)
 * Quality / performance settings of the 3DS renderer, read once at startup from render.ini:
 * sdmc:/3ds/jak1/render.ini on the 3DS (next to gk.3dsx), <project>/render.ini on PC.
 * Format: "key = value" lines, '#' or ';' comments. See docs/3ds-port/3ds_build.md.
 */

#include <string>

struct CtrSettings {
  // level geometry: chunks further than this are not drawn, with extra fog towards it so the
  // cutoff blends into the sky (meters, 0 = no limit)
  float draw_distance = 300.f;
  // the extra fog starts at this fraction of draw_distance
  float fog_start = 0.6f;
  // the game's own distance fog (like the PC renderer)
  bool fog = true;
  // tfrag: the detailed version up to this distance, the coarse one beyond (meters, 0 = always
  // detailed). Needs .c3l files with both versions (ctr_level_converter --far-lod).
  float lod_distance = 120.f;
  // a level whose bounds are further than this from the camera (seen from a neighbouring level)
  // is drawn with only its low resolution tfrag, no tie (meters, 0 = off)
  float far_level_distance = 40.f;
  // multiplies the draw distance of small and medium objects (set by the converter)
  float detail_scale = 1.f;
  // renderers
  bool merc = true;
  bool sprites = true;
  // world space sprites (particles) per frame; the HUD is always drawn
  int max_sprites = 1000;

  std::string summary() const;
};

/*! The settings (loaded on first use). */
const CtrSettings& ctr_settings();

namespace ctr_gfx {
/*! One line describing the renderer settings, for status displays. */
std::string settings_summary();
}  // namespace ctr_gfx
