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
  float draw_distance = 500.f;
  // the extra fog starts at this fraction of draw_distance
  float fog_start = 0.6f;
  // the game's own distance fog (like the PC renderer)
  bool fog = true;
  // tfrag: the detailed version up to this distance, the coarse one beyond (meters, 0 = always
  // detailed). Needs .c3l files with both versions (ctr_level_converter --far-lod).
  float lod_distance = 200.f;
  // a level whose bounds are further than this from the camera (seen from a neighbouring level)
  // is drawn with only its low resolution tfrag, no tie (meters, 0 = off)
  float far_level_distance = 40.f;
  // multiplies the draw distance of small and medium objects (set by the converter)
  float detail_scale = 1.f;
  // renderers
  bool merc = true;
  bool sprites = true;
  bool ocean = true;
  // the sprite distorter: particles that warp what's behind them (portals, heat haze); each frame
  // with such particles costs one copy of the screen
  bool distort = false;  // (AI-assisted) off: square sprites reported on hardware
  // the envmap shine of merc models (power cells, precursor metal): a second pass over their draws
  bool envmap = true;
  // the sky (time of day sky texture, clouds); off: the screen is cleared to the fog color
  bool sky = true;
  // GPU starts on each part of a frame while the CPU builds the rest (off: at the frame end)
  bool overlap = false;
  // CPU builds frame N+1 while the GPU draws frame N
  bool pipeline = false;
  // world space sprites (particles) per frame; the HUD is always drawn
  int max_sprites = 1000;
  // RGBA4 level/model textures stored as RGBA8 (twice the memory): Azahar renders RGBA4 as noise.
  // Default (auto): on in the emulator, off on the 3DS.
  bool rgba4_as_rgba8 = false;
  // Level textures in VRAM while there is room (the GPU reads VRAM much faster than the main
  // memory). Default (auto): on on the 3DS, off in the emulator (Azahar draws VRAM textures as
  // noise).
  bool vram_textures = true;
  // Mip levels of the level textures (c3l v8): 0 off, 1 nearest level (default), 2 trilinear
  // (blends two levels: smoother, slower).
  int mipmaps = 1;
  // Measure what each renderer costs the GPU: every few seconds one of them is left out for a
  // moment and the GPU time without it is logged ([ctr] gpu profile). The picture flickers.
  bool gpu_profile = false;
  // running in an emulator (Azahar / Citra), for the auto defaults above
  bool emulator = false;

  std::string summary() const;
};

/*! The settings (loaded on first use). */
const CtrSettings& ctr_settings();

namespace ctr_gfx {
/*! One line describing the renderer settings, for status displays. */
std::string settings_summary();
}  // namespace ctr_gfx
