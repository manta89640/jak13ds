#pragma once

/*!
 * @file CtrSettings.h
 * (AI-assisted)
 * Quality / performance settings of the 3DS renderer, read once at startup from config.ini:
 * sdmc:/3ds/jak1/config.ini on the 3DS (next to gk.3dsx), <project>/config.ini on PC (the one
 * settings file; docs/3ds-port/config_ini.md).
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
  // is drawn with only its low resolution tfrag, no tie (meters, 0 = off). (AI-assisted) 80 (was
  // 40): ties popped in at 40 m; c3l files with chunk kinds also keep the ties without a distance
  // limit (landmarks) in that mode
  float far_level_distance = 80.f;
  // multiplies the draw distance of small and medium objects (set by the converter)
  float detail_scale = 1.f;
  // (AI-assisted) c3l files with chunk kinds (c3l::Chunk::kind; older files: detail_scale only):
  // shrubs up to this distance (meters, times detail_scale; 0 = the converter's, 100 m), and the
  // converter's distances of small and medium ties (100 / 300 m) times this
  float shrub_distance = 50.f;
  float tie_distance_scale = 1.5f;
  // (AI-assisted) auto_detail: those two grow toward shrub_distance_max / tie_distance_scale_max
  // while the GPU has time to spare (its time per frame under auto_detail_gpu_ms) and go back
  // when it hasn't, so the distances are as far as each place allows (Precursor Basin has room,
  // Rock Village hardly any). Needs frame_pacing = steady (the GPU time comes from its fence).
  bool auto_detail = true;
  float shrub_distance_max = 120.f;
  float tie_distance_scale_max = 3.f;
  float auto_detail_gpu_ms = 27.f;
  // renderers
  bool merc = true;
  bool sprites = true;
  bool ocean = true;
  // (AI-assisted) config ocean = off | flat | waves | full (on = flat). flat: CtrOcean's static grid;
  // waves: the grid plus real waves near the camera; full: the PS2 ocean (draw-ocean)
  enum class OceanMode { Flat, Waves, Full };
  OceanMode ocean_mode = OceanMode::Flat;
  // (AI-assisted) ocean = full: the wave texture is made every nth frame (1 = every frame; the
  // render thread's 1-1.7 ms for it, half as often by default)
  int ocean_texture_rate = 2;
  // (AI-assisted) ocean = full: the near ocean's alpha is at least this (0..1; the PS2's is 0.5
  // within 12 m of the camera, 1 from 24 m on). It stands in for the sky reflection pass the 3DS
  // leaves out; 0.5 = the textured pass as on the PS2 and PC.
  float ocean_near_alpha = 0.6f;
  // the sprite distorter: particles that warp what's behind them (portals, heat haze); each frame
  // with such particles costs one copy of the screen
  bool distort = false;  // (AI-assisted) off: square sprites reported on hardware
  // the envmap shine of merc models (power cells, precursor metal): a second pass over their draws
  bool envmap = true;
  // (AI-assisted) config shadows = full | blob | off (on = full). full: the game's shadow volumes
  // (Jak, enemies, objects; the game runs shadow-execute-all, CtrShadow draws them with the stencil
  // buffer); blob: a dark spot under Jak only (the game's fake shadow, no volumes); off: none.
  // full needs the stencil buffer: with depth16 it is blob (the game applies the same rule).
  enum class ShadowMode { Off, Blob, Full };
  ShadowMode shadows = ShadowMode::Full;
  bool shadows_full_wanted = false;  // full in config.ini, blob because of depth16 (for the log)
  // the sky (time of day sky texture, clouds); off: the screen is cleared to the fog color
  bool sky = true;
  // GPU starts on each part of a frame while the CPU builds the rest (off: at the frame end)
  bool overlap = false;
  // CPU builds frame N+1 while the GPU draws frame N
  bool pipeline = false;
  // (AI-assisted) frame rate cap: 30 (default: the target, steady frame times) or 60
  int fps_cap = 30;
  // (AI-assisted) frame_pacing: steady (default) shows each frame a fixed time after the game
  // made it, so motion is even; fast shows it as soon as the GPU is done (lower latency, uneven)
  bool steady_pacing = true;
  // world space sprites (particles) per frame; the HUD is always drawn
  int max_sprites = 1000;
  // RGBA4 level/model textures stored as RGBA8 (twice the memory): Azahar renders RGBA4 as noise.
  // Default (auto): on in the emulator, off on the 3DS.
  bool rgba4_as_rgba8 = false;
  // Level textures in VRAM while there is room (the GPU reads VRAM much faster than the main
  // memory). Default (auto): on, in the emulator too (it draws VRAM textures right; only RGBA4 is
  // noise there, see rgba4_as_rgba8).
  bool vram_textures = true;
  // Mip levels of the level textures (c3l v8): 0 off, 1 nearest level (default), 2 trilinear
  // (blends two levels: smoother, slower).
  int mipmaps = 1;
  // Measure what each renderer costs the GPU: every few seconds one of them is left out for a
  // moment and the GPU time without it is logged ([ctr] gpu profile). The picture flickers.
  bool gpu_profile = false;
  // (AI-assisted) the GPU's own counters (busy % of each pipeline stage, memory traffic) over 10 s
  // of each level, logged once per level ([ctr] gpu profile <level>: hw busy ...); nothing is left
  // out of the picture. Read through the GSP service (no extra system calls). (AI-assisted) Off by
  // default: the hotspot profiler registers have never run on hardware (a GPU that stalls on its
  // result FIFO would hang the game); turn on for a diagnostic run.
  bool gpu_counters = false;
  // (AI-assisted) the GSP's performance log: how long the GPU's command lists (P3D), display
  // transfers (PPF) and memory fills (PSC) took per frame, against the fence's busy time
  // ([ctr] gpu perflog, once per stats window). One GSP request per window; hardware only.
  bool gpu_perflog = false;
  // (AI-assisted) a GPU microbenchmark at boot, before the game starts (~20-60 s; `[bench]` lines
  // in gk.log and data/log/bench.txt, platform/3ds/tools/bench_report.py). Its last tests use
  // shaders never run on hardware: a hang there names the test in bench.txt. Hardware only.
  bool pica_bench = false;
  // (AI-assisted) 3DS hardware features (docs/3ds-port/config_ini.md):
  // 16-bit (RGB565) color buffer and top screen: half the memory traffic of every pixel drawn,
  // blended and cleared. Off by default: without dithering, faint blended layers (waterfall mist,
  // smoke) add less than one color step each and vanish, and gradients band.
  bool color16 = false;
  // the GPU's early depth test for opaque level draws, drawn front to back in distance bands.
  // (AI-assisted) Off by default: on a New 3DS it left large screen blocks where the level never
  // drew (Azahar ignores the early depth test, so only hardware shows it).
  bool early_depth = false;
  // (AI-assisted) early_depth = block32: with the render target in 32x32 block mode (256x416,
  // cropped to the screen), which the early depth test seems to need. Hardware only (off).
  bool early_depth_block32 = false;
  // (AI-assisted) 16-bit depth buffer instead of 24-bit + stencil: half the depth traffic, but
  // 256x coarser depth (z-fighting far away) and no stencil (shadows = full becomes blob, the
  // ocean = waves marks are left out). Off by default.
  bool depth16 = false;
  // sprite and HUD textures in the smallest 3DS format that holds them (8 / 16 bits per texel),
  // and in reserved VRAM (with vram_textures)
  bool compact_textures = true;
  // glows (radial gradient textures) from the procedural texture unit: no texels read
  bool proctex_glows = true;
  // world sprites (particles, glows) at most this fraction of the screen height (0 = no limit)
  float sprite_max_size = 0.5f;
  // the game's own visibility data (which parts of a level can be seen from the camera's spot)
  // hides level parts behind walls and hills (needs .c3l v10 files)
  bool vis_culling = true;
  // (AI-assisted) draws write their GPU commands themselves instead of through citro3d (less CPU
  // per draw on the render thread, fewer command words)
  bool raw_cmds = true;
  // (AI-assisted) with raw_cmds: draws of the same vertices and matrices one after the other keep
  // the GPU's post-vertex cache (fewer command words; off until checked on hardware)
  bool vcache_reuse = false;
  // (AI-assisted) hardware hang bisect switches (Oct 3 optimisations; off = the Oct 2 behaviour):
  // new_shaders: the shorter vertex shaders (off: the Oct 2 ctr_basic/mesh/skin/skin_env);
  // asm_tex: the 4x2 group / LDM-STM texture tiling, 64-bit PSMCT32 uploads and SIMD sky loops
  // (off: the old per texel loops); asm_mem: ctr_mem.S memcpy/memmove/memset (off: byte/word C
  // loops; read by platform/3ds/port at boot); fast_clock: steady_clock without divides (off:
  // clock_gettime)
  // (AI-assisted) off by default: the new ctr_mesh hung the real GPU (hardware bisect 2026-10-03);
  // the new ctr_basic passed on hardware (shader_basic = new below), the skin ones are untested
  bool new_shaders = false;
  bool asm_tex = true;
  // (AI-assisted) defer_flush: the render thread's cache flushes held until the frame is submitted
  // (e165bc719); off: each one right away, as on Oct 2
  bool defer_flush = true;
  // (AI-assisted) hang bisect: shader_basic old|new, shader_mesh old|new|dph (-1: new_shaders)
  int shader_basic = 1, shader_mesh = -1;
  bool asm_mem = true;
  bool fast_clock = true;
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
