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
  // (AI-assisted) The defaults are the settled play values (Oct 16 config cleanup: the user's
  // release config.ini); config.ini's preset = safe | bench | breakdown changes some of them
  // (ctr_config_presets.h), its own lines win over both.
  // the extra fog starts at this fraction of draw_distance
  float fog_start = 0.6f;
  // the game's own distance fog (like the PC renderer)
  bool fog = true;
  // tfrag: the detailed version up to this distance, the coarse one beyond (meters, 0 = always
  // detailed, as the PS2: its tfrag has no distance LOD). Needs .c3l files with both versions
  // (ctr_level_converter --far-lod).
  float lod_distance = 0.f;
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
  // 0 (config "none"): ties without a distance limit, as the PS2 (geometry 3 beyond the far-plane).
  // 1.5: small ties to 150 m, medium to 450 m (the baseline auto_detail rarely needs to leave)
  float tie_distance_scale = 1.5f;
  // (AI-assisted) auto_detail: those two grow toward shrub_distance_max / tie_distance_scale_max
  // while the GPU has time to spare (its time per frame under auto_detail_gpu_ms) and go back
  // when it hasn't, so the distances are as far as each place allows (Precursor Basin has room,
  // Rock Village hardly any). Needs frame_pacing = steady (the GPU time comes from its fence).
  bool auto_detail = true;
  // auto_detail = fixed (default): the far ring always at the relief's floor (CtrAutoDetail.h),
  // nothing moves with the load; on: the GPU-time controller; off: the config's distances
  bool auto_detail_fixed = true;
  float shrub_distance_max = 120.f;
  float tie_distance_scale_max = 3.f;
  // (AI-assisted) .c3l v14 (c3l::kShrubPs2DistVersion): each shrub is drawn up to its prototype's
  // PS2 distance (the middle of its PS2 fade: the fort wall ropes 155 m, grass 60-75 m) times
  // shrub_scale (growing toward shrub_scale_max with auto_detail), times detail_scale.
  // shrub_distance / shrub_distance_max are for older files.
  float shrub_scale = 1.f;
  float shrub_scale_max = 1.f;
  float auto_detail_gpu_ms = 27.f;
  // renderers
  bool merc = true;
  bool sprites = true;
  bool ocean = true;
  // (AI-assisted) config ocean = off | flat | waves | full (on = flat). flat: CtrOcean's static grid;
  // waves: the grid plus real waves near the camera; full: the PS2 ocean (draw-ocean)
  enum class OceanMode { Flat, Waves, Full };
  OceanMode ocean_mode = OceanMode::Full;  // (AI-assisted) full since the Oct 16 config cleanup
  // (AI-assisted) ocean = full: the wave texture is made every nth frame (1 = every frame; the
  // render thread's 1-1.7 ms for it, half as often by default)
  int ocean_texture_rate = 2;
  // (AI-assisted) ocean = full: the near ocean's alpha is at least this (0..1; the PS2's is 0.5
  // within 12 m of the camera, 1 from 24 m on). (j1ocean) 0 = the PS2's own alpha: the sky
  // reflection is drawn now (ctr_gpu_ocean_full_draw_env), the old 0.6 stood in for it.
  float ocean_near_alpha = 0.f;
  // the sprite distorter: particles that warp what's behind them (portals, heat haze); each frame
  // with such particles costs one copy of the screen
  bool distort = true;  // (AI-assisted) on (Oct 16 config cleanup; was off: square sprites on hardware, fixed)
  // the envmap shine of merc models (power cells, precursor metal): a second pass over their draws
  bool envmap = true;
  // (AI-assisted, fidelity2) envmap_min_px: no shine pass for models smaller than this on the top
  // screen (their bounding sphere's diameter in pixels; 0 = every model). HUD models always keep it
  float envmap_min_px = 0.f;
  // (AI-assisted) the envmap draws of a merc bucket queued and drawn together at its end (or before
  // a blended / no depth write draw): one skin -> env shader switch pair instead of one per draw
  bool envmap_batch = true;
  // (AI-assisted) a merc bucket's opaque draws of models drawn more than once (crates, yetis, ...)
  // drawn draw by draw across the instances: one texture bind per draw of the model instead of
  // per instance, but the bones are uploaded for each draw then
  bool merc_tex_sort = true;  // (AI-assisted) on: Snowy host stats -7..-45% merc texture binds
  // (AI-assisted) models with only lod0 (the yeti, ...) switch to the converter's generated low LOD
  // (c3l::kMercGenLodSuffix: same vertices, ~40-60% of the triangles) beyond max(25 m, 12.5 x the
  // model's size) x merc_lod_scale. Needs .c3l files from a converter with --gen-lod
  bool merc_gen_lod = true;
  // config merc_lod_scale (also read by kmachine.cpp for the game's own LOD distances).
  // (AI-assisted) default 1 (the PS2's distances; was 0.5)
  float merc_lod_scale = 1.f;
  // (AI-assisted) config shadows = full | blob | off (on = full). full: the game's shadow volumes
  // (Jak, enemies, objects; the game runs shadow-execute-all, CtrShadow draws them with the stencil
  // buffer); blob: a dark spot under Jak only (the game's fake shadow, no volumes); off: none.
  enum class ShadowMode { Off, Blob, Full };
  ShadowMode shadows = ShadowMode::Full;
  // (AI-assisted) shadows = full: at most this many objects' shadows per frame, the ones that look
  // biggest from the camera (bounding sphere over distance: Jak's normally is one); 0 = all
  // (AI-assisted) default 0 since Oct 12 (all casters, like the PS2); 6 was the 30 fps era cut
  int shadow_casters = 0;
  // the sky (time of day sky texture, clouds); off: the screen is cleared to the fog color
  bool sky = true;
  // (AI-assisted) the clouds over the sky (one added pass over the sky's part of the screen)
  bool clouds = true;
  // GPU starts on each part of a frame while the CPU builds the rest (off: at the frame end)
  bool overlap = true;
  // CPU builds frame N+1 while the GPU draws frame N
  bool pipeline = true;
  // (AI-assisted) config frame_overlap2 = off | on (with the pipeline; ctr_gpu.h
  // ctr_gpu_set_frame_overlap2): off the old frame end (preset safe); on (default) the list queued
  // behind the frame the GPU is drawing, no wait at the frame end
  bool frame_overlap2 = true;
  // (AI-assisted) frame rate cap: 30 (default: the target, steady frame times) or 60
  int fps_cap = 30;
  // (AI-assisted) frame_pacing: steady (default) shows each frame a fixed time after the game
  // made it, so motion is even; fast shows it as soon as the GPU is done (lower latency, uneven)
  bool steady_pacing = true;
  // world space sprites (particles) per frame; the HUD is always drawn
  // (AI-assisted) default 2176 since Oct 12: the game's own particle capacity (1920 2d + 256 3d,
  // sparticle.gc), so nothing the PS2 draws is left out; 1000 was the 30 fps era cut
  int max_sprites = 2176;
  // RGBA4 level/model textures stored as RGBA8 (twice the memory): Azahar renders RGBA4 as noise.
  // Default (auto): on in the emulator, off on the 3DS.
  bool rgba4_as_rgba8 = false;
  // Copies of the level textures in VRAM while there is room (the GPU reads them faster).
  bool vram_textures = true;
  // config.ini texshare = off: no shared texture groups, 1 MB level pools (as before sharing)
  bool texshare = true;
  // Mip levels of the level textures (c3l v8): 0 off, 1 nearest level (default: one texel fetch),
  // 2 trilinear (blends two levels, as the PC).
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
  // (AI-assisted) render_watchdog: a thread (core 0, 16 KB stack) that logs once per second for the
  // first 60 s whether the render thread still finishes frames and its last stage (for hangs at
  // boot). Off by default: ~60 log lines and the stack for nothing in a normal session
  bool render_watchdog = false;
  // (AI-assisted) seconds between the render thread's statistics bursts (frame times, triangles,
  // draws, culling); shorter for measuring fixed views (more SD card writes)
  float stats_period = 15.f;
  // (AI-assisted) the GSP's performance log: how long the GPU's command lists (P3D), display
  // transfers (PPF) and memory fills (PSC) took per frame, against the fence's busy time
  // ([ctr] gpu perflog, once per stats window). One GSP request per window; hardware only.
  bool gpu_perflog = false;
  // (AI-assisted) a GPU microbenchmark at boot, before the game starts (~20-60 s; `[bench]` lines
  // in gk.log and data/log/bench.txt, platform/3ds/tools/bench_report.py). Its last tests use
  // shaders never run on hardware: a hang there names the test in bench.txt. Hardware only.
  bool pica_bench = false;
  // (AI-assisted) 3DS hardware features (docs/3ds-port/config_ini.md):
  // sprite and HUD textures in the smallest 3DS format that holds them (8 / 16 bits per texel),
  // and in reserved VRAM (with vram_textures)
  bool compact_textures = true;
  // world sprites (particles, glows) at most this fraction of the screen height (0 = no limit)
  float sprite_max_size = 0.f;  // (AI-assisted) 0 since Oct 12, as the PS2 (0.5 was the 30 fps cut)
  // (AI-assisted) world sprites with a half size over ~20 pixels are left out below this alpha
  // (the game's 0..128); 2: none (only sprites drawn with alpha 0 are skipped, where that changes
  // no pixel)
  int sprite_min_alpha = 2;
  // (AI-assisted) blended world sprites drop their alpha 0 fragments (alpha test, where alpha 0
  // adds nothing): no framebuffer traffic for them; the picture is the same. off: no alpha test
  bool sprite_alpha_discard = true;
  // (AI-assisted, perfv2) particle_fill = off | on | strong. strong: big faint overlapping 2D
  // particles thinned by alike slot pairs (CtrSprite draw_chunk_mode; an approximation); off and
  // on draw every particle (each one over its texture's alpha box only, always: the same picture)
  int particle_fill = 1;
  // the game's own visibility data (which parts of a level can be seen from the camera's spot)
  // hides level parts behind walls and hills (needs .c3l v10 files)
  bool vis_culling = true;
  // (AI-assisted) a level chunk crossing the edge of the view has its pieces (visibility runs, or
  // pieces of ~96 triangles made at load) tested against the view on their own: only the ones on
  // screen are drawn (bounding spheres made at load, 8 bytes per piece)
  bool run_culling = true;
  // (AI-assisted, occluders) level geometry hidden behind the occluder_count biggest occluders near
  // the camera (boxes inside closed tie surfaces, c3l::ExtOcc: levels converted with the current
  // ctr_level_converter) is not drawn (docs/3ds-port/culling.md). Off: not yet seen on a console
  bool occluders = false;
  int occluder_count = 6;
  // (AI-assisted, horizon) level geometry entirely beyond horizon_radius meters and below the
  // terrain's horizon (c3l::ExtHorizon: the hills of the level the camera is over) is not drawn
  // (docs/3ds-port/culling.md). Off: not yet seen on a console
  bool horizon = false;
  float horizon_radius = 64.f;
  // (AI-assisted, visbake) the title screen's flight: the level runs a bake (<level>.c3b next to the
  // .c3l, made on the host with CTR_VIS_BAKE=dir:title) never saw from the nearest cameras of its
  // path are not drawn (docs/3ds-port/culling.md). Off: not yet seen on a console
  bool title_vis = false;
  // (AI-assisted, cellvis) level runs that a bake (<level>.c3c, CTR_VIS_BAKE=dir:cells on the host)
  // never saw from the camera's cell (8 m) looking the ways the view reaches are not drawn
  // (docs/3ds-port/culling.md). Off: not yet seen on a console
  bool cell_vis = false;
  // (AI-assisted, ps2lodscale) the PS2's tie and tfrag level of detail distances (its switches and
  // geomorph bands, not the screen-space error rule) times this: auto = the PS2's lines over the
  // 3DS's (240 / 448 Jak 1, 240 / 416 Jak II), where a shape error covers as many pixels of the
  // 240-line top screen as it did of the PS2's picture (docs/3ds-port/culling.md). 1: the PS2's
  float ps2_lod_scale = 1.f;
  bool ps2_lod_scale_auto = false;
  // (AI-assisted, sizecull) level chunks and runs whose bounding sphere is smaller than this many
  // pixels across on the top screen are not drawn; one hidden that way comes back above 1.25x it
  // (docs/3ds-port/culling.md). 0: off (default)
  float min_run_px = 0.f;
  // (AI-assisted, farcopy) levels converted with --far-copies: a cell's plain tie geometry is drawn
  // as its simplified copy while the camera is far_copy_distance meters or more from the cell's
  // center (docs/3ds-port/culling.md). Off: the sources always (the copies never)
  bool far_copy = false;
  float far_copy_distance = 150.f;
  // (AI-assisted, terraincull) back faces of the terrain the converter marked (c3l::kDrawCullTerrain,
  // --terrain-cull: ground whose underside the camera can't get to) are culled, with cull_ties'
  // winding (needs cull_ties on). Read at level load. Off: both sides (docs/3ds-port/culling.md)
  bool cull_terrain = true;
  // (AI-assisted, facecull) levels converted with --face-runs: a run of a back face culled draw
  // whose every triangle faces away from the camera (its normal cone and sphere) is not sent to the
  // GPU at all (it would cull all of it: the same picture, less vertex work). Needs cull_ties.
  // Off (docs/3ds-port/culling.md)
  bool cone_cull = false;
  // (AI-assisted, depthreuse) level chunks and runs that the depth of a recent frame (the GPU's copy
  // after the camera's level is drawn) hid in two copies in a row are not drawn, while the camera
  // stands nearly still (0.25 m, 1 degree) and nothing nearer than depth_reuse_min meters covers
  // them (docs/3ds-port/culling.md). Off: not yet seen on a console
  bool depth_reuse = false;
  float depth_reuse_min = 20.f;
  // (AI-assisted) c3l v17 tie levels of detail: the instance groups that have a near version
  // (geometry 0) are drawn with it within their PS2 distance, else with the far one (geometry 3).
  // off: the far versions only (as before v17)
  bool tie_lod = true;
  // (AI-assisted, streaming) c3l v19 levels: the near versions of the tie LOD groups (near chunks)
  // are read from the file around the camera on the loader thread, into a stream arena of
  // stream_kb KB of linear memory (taken at the first such level, kept), and freed far away; a group
  // whose near chunk isn't there yet is drawn with its far version. off: the near chunks of the
  // groups the level's v18 budget took are loaded with the level, the others never (v18's ties)
  bool streaming = true;
  int stream_kb = 4096;
  // (AI-assisted, j2-mem) stream_kb was in config.ini: else Jak II takes its own size
  // (CtrLevel.cpp ctr_stream_kb)
  bool stream_kb_set = false;
  // (AI-assisted, j2-mem) Jak II: a level the game's visibility string hides whole for 3 s (ctyindb
  // seen from the slums) is dormant: no near chunks wanted, and its ties with a distance limit
  // farther than 100 m from the camera freed; read back within 75 m and all once the string shows
  // it again (CtrLevel.cpp "Dormant levels", streaming.md). off: all resident.
  bool dormant_levels = true;
  // (AI-assisted, streaming) the linear heap's floor: the arena grows past stream_kb (segments of
  // 1-4 MB, sized to the near chunks needed now and 2 s ahead) while they don't fit and the heap
  // keeps this much (+ 256 KB) free after it; given back when the heap has less, or a level load
  // needs the memory
  int stream_reserve_kb = 3072;
  // (AI-assisted, streaming) diagnostic: a log line for each near chunk that is late (and the
  // camera's speed, its score before)
  bool stream_debug = false;
  // (AI-assisted, citadel) time of day, an approximation: a day palette that pulses (CtrTodPulse.h:
  // up and down around a steady mean within ~6 s; Gol and Maia's Citadel's update-mood-light on
  // palette 1, which 71% of its colors use, moves 1/64 every ~2 frames) is taken only when it
  // moved this many 64ths from the weight last taken, or once it stood still for 8 frames
  // (CtrLevel.cpp tod_update; never on the title screen). 0 (default): every move, exact as the
  // PC (a rewrite writes only the vertices whose color changed: cheaper than the hold was)
  int tod_pulse_step = 0;
  // (AI-assisted, title-popin) files with shape errors (current converter): a group switches to its
  // far version where the far version's shape error is at most this many pixels on screen (this
  // frame's projection), not at the PS2 distance (3 px median, up to 19 px pops in village1).
  // 1 px: the switch can't be seen, as the PS2's morph to geometry 3 (~+30-70k triangles on the
  // title and in Snowy against 4 px)
  // 4: the PS2 far-plane floor keeps the PS2's look; 1 cost the title ~2.5 ms of wind ties
  float tie_lod_px = 4.f;
  // (AI-assisted) a neighbouring level's visibility string (the camera is in the other level): its
  // tfrag is hidden by the bits too, and its low resolution tfrag drawn where they say, as the
  // PS2 and PC do (else only its ties used the bits)
  bool neighbour_vis = true;
  // (AI-assisted) the levels' low resolution tfrag (the game's far scenery) as vista: also in near
  // mode, outside the loaded levels' detailed areas (+50 m), out to vista_distance, with a haze
  // towards the fog color from draw_distance on instead of the draw distance fog (that one is
  // solid fog color from draw_distance on). Off: only in far mode, within draw_distance.
  bool lowres_vista = true;
  // (AI-assisted) meters: far scenery (lowres_vista, the vista tier) is drawn up to here (the
  // PS2's far clip plane, math-camera f: 10 km)
  float vista_distance = 10000.f;
  // (AI-assisted) skin_clip_f24: each merc model's clip matrix and lights as float24 uniforms like
  // the bone rows (skin_rows_f24): 11 words less per model. Not bit-identical (rounding): off until
  // priced (pica_bench B2.unif_f24 vs B2.unif_f32, the same line as skin_rows_f24)
  bool skin_clip_f24 = true;
  // (AI-assisted) back faces of closed tie surfaces (c3l v13 draws marked by the converter) are
  // culled. cull_flip: the other screen winding (if the PICA's turns out mirrored). On: no face
  // went missing in Azahar at Sandover, Snowy's fort and start, Fire Canyon (2026-10-11)
  bool cull_ties = true;
  bool cull_flip = false;
  // (AI-assisted) back faces of merc draws the converter marked (c3l kMercDrawCullBack: wound like
  // their vertex normals, at most 2% of their triangles at an open edge, opaque) are culled, with
  // cull_ties' winding (flip). Not HUD models, not draws whose palette has a mirroring bone. Needs
  // levels converted with the current ctr_level_converter (older files: no marks)
  bool cull_merc = true;
  // (AI-assisted, far-fog) the game's fog on merc models (characters, objects, the far views of
  // other levels: med-res-level, e.g. Sandover from Misty), per vertex like merc2.vert. Needs
  // shader_opt (ctr_skin_opt); the Oct 2 skin program has no fog. Read per model (gpu_profile = ab)
  bool merc_fog = true;
  // (AI-assisted) draws write their GPU commands themselves instead of through citro3d (less CPU
  // per draw on the render thread, fewer command words)
  bool raw_cmds = true;
  // (AI-assisted, gpu-rt) gpu_breakdown: every 4th frame the GPU runs each part of the frame alone
  // (clear, sky, level, merc, translucent, shadows, ocean, sprites, distort copy, HUD, transfer)
  // and its time is logged per stats window ([ctr] gpu breakdown). Serialises the GPU: slower
  // while on. Profiling only, off; needs pipeline = on
  bool gpu_breakdown = false;
  // (AI-assisted, gpu-rt) ocean_tex_half: the flat / waves ocean's shading texture is made every
  // second frame (the heights still every frame for the near grid); ocean = full has
  // ocean_texture_rate
  bool ocean_tex_half = true;
  // (AI-assisted) hardware hang bisect switches (Oct 3 optimisations; off = the Oct 2 behaviour):
  // asm_tex: the 4x2 group / LDM-STM texture tiling, 64-bit PSMCT32 uploads and SIMD sky loops
  // (off: the old per texel loops); asm_mem: ctr_mem.S memcpy/memmove/memset (off: byte/word C
  // loops; read by platform/3ds/port at boot); fast_clock: steady_clock without divides (off:
  // clock_gettime)
  bool asm_tex = true;
  // (AI-assisted) defer_flush: the render thread's cache flushes held until the frame is submitted
  // (e165bc719); off: each one right away, as on Oct 2
  bool defer_flush = true;
  bool asm_mem = true;
  bool fast_clock = true;
  // running in an emulator (Azahar / Citra), for the auto defaults above
  bool emulator = false;

  std::string summary() const;
};

/*! The settings (loaded on first use). (AI-assisted, micro-level) Inline: after the first call a
 * read of a plain pointer (published with a release store once the settings are made; the reads
 * through it depend on its address, which ARM orders), not an out-of-line call with a static's
 * guard and barrier. */
namespace ctr_settings_detail {
extern const CtrSettings* g_settings;
const CtrSettings& first_use();
}  // namespace ctr_settings_detail
inline const CtrSettings& ctr_settings() {
  const CtrSettings* s = ctr_settings_detail::g_settings;
  return __builtin_expect(s != nullptr, 1) ? *s : ctr_settings_detail::first_use();
}

namespace ctr_gfx {
/*! One line describing the renderer settings, for status displays. */
std::string settings_summary();
}  // namespace ctr_gfx
