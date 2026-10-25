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
  // (AI-assisted) .c3l v14 (c3l::kShrubPs2DistVersion): each shrub is drawn up to its prototype's
  // PS2 distance (the middle of its PS2 fade: the fort wall ropes 155 m, grass 60-75 m) times
  // shrub_scale (growing toward shrub_scale_max with auto_detail), times detail_scale.
  // shrub_distance / shrub_distance_max are for older files.
  float shrub_scale = 0.8f;
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
  // (AI-assisted) config distort_copy = rows | full. rows: the distorter's screen copy covers only
  // the 8-pixel columns of tile rows its sprites read, the copy texture goes to VRAM when there is
  // room, and distort sprites under 8 pixels are left out (no copy at all when none is left).
  // full: every frame with distort sprites copies the whole screen into linear memory (as before)
  bool distort_copy_rows = true;
  // (AI-assisted) config shadows = full | blob | off (on = full). full: the game's shadow volumes
  // (Jak, enemies, objects; the game runs shadow-execute-all, CtrShadow draws them with the stencil
  // buffer); blob: a dark spot under Jak only (the game's fake shadow, no volumes); off: none.
  // full needs the stencil buffer: with depth16 it is blob (the game applies the same rule).
  enum class ShadowMode { Off, Blob, Full };
  ShadowMode shadows = ShadowMode::Full;
  bool shadows_full_wanted = false;  // full in config.ini, blob because of depth16 (for the log)
  // (AI-assisted) shadows = full: at most this many objects' shadows per frame, the ones that look
  // biggest from the camera (bounding sphere over distance: Jak's normally is one); 0 = all
  // (AI-assisted) default 0 since Oct 12 (all casters, like the PS2); 6 was the 30 fps era cut
  int shadow_casters = 0;
  // the sky (time of day sky texture, clouds); off: the screen is cleared to the fog color
  bool sky = true;
  // (AI-assisted) the clouds over the sky (one added pass over the sky's part of the screen)
  bool clouds = true;
  // (AI-assisted) the sky drawn after the opaque level and models (before the first draw that
  // needs what's behind it), only where they left the cleared depth: its pixels behind them are
  // dropped by the depth test (before blending and writing; with the early depth test, before
  // texturing). Same picture
  bool sky_last = true;
  // (AI-assisted) frames whose opaque sky roof covers the whole screen (looking up) clear only the
  // depth buffer, not the color. Same picture
  bool skip_clear = true;
  // GPU starts on each part of a frame while the CPU builds the rest (off: at the frame end)
  bool overlap = true;
  // CPU builds frame N+1 while the GPU draws frame N
  bool pipeline = true;
  // (AI-assisted) config frame_overlap2 = off | clean | on (with the pipeline; ctr_gpu.h
  // ctr_gpu_set_frame_overlap2): 0 the old frame end; 1 (clean, default) the frame's command list
  // cleaned from the CPU cache before waiting for the GPU, handed over without the GSP's flush;
  // 2 (on) the list queued behind the frame the GPU is drawing, no wait at the frame end
  int frame_overlap2 = 2;
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
  // Level textures in VRAM while there is room (the GPU reads VRAM much faster than the main
  // memory). Default (auto): on, in the emulator too (it draws VRAM textures right; only RGBA4 is
  // noise there, see rgba4_as_rgba8).
  bool vram_textures = true;
  // Mip levels of the level textures (c3l v8): 0 off, 1 nearest level (default), 2 trilinear
  // (blends two levels: smoother, slower).
  int mipmaps = 1;
  // (AI-assisted) the LOD bias of mipmapped level / model textures (+0.5, +1: the next smaller
  // level sooner, blurrier, fewer texels read); 0 = none
  float mip_bias = 0.f;
  // (AI-assisted) merc bone rows uploaded as float24 instead of float32 (a quarter fewer uniform
  // words; the GPU computes in float24 either way). On: Jak and Daxter identical in Azahar, the
  // float24 uniform path (raw_unif, raw_pack_f24) already draws the level meshes on hardware
  bool skin_rows_f24 = true;
  // Measure what each renderer costs the GPU: every few seconds one of them is left out for a
  // moment and the GPU time without it is logged ([ctr] gpu profile). The picture flickers.
  bool gpu_profile = false;
  // (AI-assisted) gpu_profile = ab: instead, the switches of the A/B table (CtrRenderer.cpp
  // kAbSwitches: run_culling, cull_ties, vista, fog_stage_skip, shadows, envmap, distort, and the
  // snow-gpu-impl keys distort_copy, shadow_casters, sprite_*, envmap_batch, merc_*, clouds) are
  // turned on and off in turns, in windows of ~1.5 s, and the GPU busy time of each setting is
  // logged per level ([ctr] gpu ab). Nothing vanishes for good; the picture changes slightly.
  bool gpu_profile_ab = false;
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
  // (AI-assisted) early_depth = sky (sky32: with the block32 target): the early depth test only
  // for the sky, drawn last (sky_last forced on) at depth 0: its fragments behind what the opaque
  // draws covered are dropped before texturing. With on / block32 and sky_last the sky uses it too
  bool early_depth_sky = false;
  // (AI-assisted) early_depth_func = ge | gt | le | lt: the early test's function (the normal
  // depth test stays >=); early_depth_clear = the early depth data's clear value (0 = far, max =
  // 0xFFFFFF, or a number). pica_bench B8.*.c3 shows which pair passes everything after a clear
  int early_depth_func = 0;
  unsigned int early_depth_clear = 0;
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
  float sprite_max_size = 0.f;  // (AI-assisted) 0 since Oct 12, as the PS2 (0.5 was the 30 fps cut)
  // (AI-assisted) world sprites with a half size over ~20 pixels are left out below this alpha
  // (the game's 0..128; all sprites are left out below 2, the old rule for every size)
  int sprite_min_alpha = 2;  // (AI-assisted) 2 since Oct 12: the old rule only (6 was the 30 fps cut)
  // (AI-assisted) blended world sprites drop their alpha 0 fragments (alpha test, where alpha 0
  // adds nothing): no framebuffer traffic for them; the picture is the same. off: no alpha test
  bool sprite_alpha_discard = true;
  // (AI-assisted, perfv2) particle_fill = off | on | strong: fewer blended particle pixels for the
  // GPU. on: each world particle drawn over its texture's alpha box only (the same picture), and
  // big faint particles thinned: of the ones with a half size of 16+ pixels and alpha <= 0.375,
  // every 2nd particle slot is drawn with the alpha of two (1 - (1 - a)^2, additive 2a), the other
  // left out. strong: every 3rd, 12+ pixels, alpha <= 0.25. off: as before (preset safe)
  int particle_fill = 1;
  // the game's own visibility data (which parts of a level can be seen from the camera's spot)
  // hides level parts behind walls and hills (needs .c3l v10 files)
  bool vis_culling = true;
  // (AI-assisted) a level chunk crossing the edge of the view has its pieces (visibility runs, or
  // pieces of ~96 triangles made at load) tested against the view on their own: only the ones on
  // screen are drawn (bounding spheres made at load, 8 bytes per piece)
  bool run_culling = true;
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
  // (AI-assisted, streaming) the linear heap's floor: the arena grows past stream_kb (segments of
  // 1-4 MB, sized to the near chunks needed now and 2 s ahead) while they don't fit and the heap
  // keeps this much (+ 256 KB) free after it; given back when the heap has less, or a level load
  // needs the memory
  int stream_reserve_kb = 3072;
  // (AI-assisted, streaming) diagnostic: a log line for each near chunk that is late (and the
  // camera's speed, its score before)
  bool stream_debug = false;
  // (AI-assisted, citadel) time of day, an approximation (off by default since Oct 23, todlag): a
  // day palette that pulses (its weight turned around twice within ~4 s: Gol and Maia's Citadel's
  // update-mood-light on palette 1, which 71% of its colors use, moves 1/64 every ~2 frames) is
  // taken only when it moved this many 64ths from the weight last taken, or once it stood still
  // for 8 frames (CtrLevel.cpp tod_update). 0: every move (exact as the PC)
  int tod_pulse_step = 0;
  // (AI-assisted, title-popin) files with shape errors (current converter): a group switches to its
  // far version where the far version's shape error is at most this many pixels on screen (this
  // frame's projection), not at the PS2 distance (3 px median, up to 19 px pops in village1).
  // 1 px costs ~+30-70k triangles on the title and in Snowy; 4 keeps the worst views about where
  // they were (host, Oct 17)
  float tie_lod_px = 4.f;
  // (AI-assisted) a neighbouring level's visibility string (the camera is in the other level): its
  // tfrag is hidden by the bits too, and its low resolution tfrag drawn where they say, as the
  // PS2 and PC do (else only its ties used the bits)
  bool neighbour_vis = true;
  // (AI-assisted) the levels' finer precomputed visibility (<level>.c3v next to the .c3l, made by
  // ctr_vis_builder): per cell of the camera's space, the level pieces that may be seen from there;
  // the others aren't drawn (static tfrag / tie / shrub only; merc as before). A level without the
  // file, or a camera outside the cells, draws as without it
  bool fine_vis = false;  // (off: no .c3v files shipped, see c3l_format.md "Results")
  // (AI-assisted) fine_vis: the memory per level for the .c3v blocks read around the camera (KB)
  int fine_vis_kb = 128;
  // (AI-assisted) software occlusion culling of the level background (CtrOccCull.h): each frame
  // the drawn opaque pieces nearest the camera (up to occ_cull_tris triangles within
  // occ_cull_dist meters, the biggest on screen first) are rasterized on the CPU; the pieces behind
  // them aren't drawn
  bool occ_cull = false;  // (off: costs more CPU than it saves GPU, see config_dev.md)
  int occ_cull_tris = 300;
  float occ_cull_dist = 60.f;
  // (AI-assisted) the levels' low resolution tfrag (the game's far scenery) as vista: also in near
  // mode, outside the loaded levels' detailed areas (+50 m), out to vista_distance, with a haze
  // towards the fog color from draw_distance on instead of the draw distance fog (that one is
  // solid fog color from draw_distance on). Off: only in far mode, within draw_distance.
  bool lowres_vista = true;
  // (AI-assisted) meters: far scenery (lowres_vista, the vista tier) is drawn up to here
  float vista_distance = 3000.f;
  // (AI-assisted) level chunks entirely in front of where both fogs start are drawn without the
  // fog stage (texture combiner pass-through, texture unit 1 off). On (preset safe: off)
  bool fog_stage_skip = true;
  // (AI-assisted) fog_alpha: level draws whose vertex alpha is 0x80 (~99.5%) get the fog amount in
  // the vertex color's alpha (ctr_mesh_old_fa / ctr_mesh_opt_fa) and mix in the fog color as a
  // combiner constant: no fog ramp texture lookups, one vertex output less. The rest keep the
  // ramp. tex1_gating: texture unit 1 (the ramp) enabled only for draws whose combiner reads it
  // (raw draws switch the unit's enable bit, no texture cache clear; merc, sprites, sky, HUD and
  // shadows never read it). Both on (preset safe: off).
  // (AI-assisted) levels converted with ctr_level_converter --vista-flat hold an untextured copy of
  // the vista tier (each texture's average color in the vertex colors, one draw per cell): on uses
  // it instead of the textured one (Snowy fort: 60 -> 22 vista draws, host). Read at level load
  // (not switchable while running)
  bool vista_untextured = true;
  bool fog_alpha = true;
  bool tex1_gating = true;
  // (AI-assisted) shader_opt: the shorter vertex programs (platform/3ds/shaders/ctr_*_opt.v.pica:
  // level meshes 21 -> 10 instructions and 4 -> 3 output registers, merc 53 -> 33, merc shine
  // 56 -> 33, PS2 ocean 14 -> 9 and 12 -> 6). Same picture in Azahar. On (the user's decision,
  // Oct 12: "we will keep the optimised shaders on"); pica_bench B3.mesh_opt / B3.skin_opt price
  // it. Off (preset safe): the Oct 2 programs (ctr_mesh_old, ctr_skin_old, ...)
  bool shader_opt = true;
  // (AI-assisted) cmd_dedup: register writes that would give a register the value it already has
  // are left out of the command list (texture unit 0's type at a texture change). The same
  // registers, the same picture; on. cmd_dedup_draw: the raw draws' PRIMITIVE_CONFIG mode and
  // VERTEX_OFFSET writes too (4 of their ~30 words; citro3d writes them before every draw)
  bool cmd_dedup = true;
  bool cmd_dedup_draw = true;
  // (AI-assisted) skin_clip_f24: each merc model's clip matrix and lights as float24 uniforms like
  // the bone rows (skin_rows_f24): 11 words less per model. Not bit-identical (rounding): off until
  // priced (pica_bench B2.unif_f24 vs B2.unif_f32, the same line as skin_rows_f24)
  bool skin_clip_f24 = true;
  // (AI-assisted) back faces of closed tie surfaces (c3l v13 draws marked by the converter) are
  // culled. cull_flip: the other screen winding (if the PICA's turns out mirrored). On: no face
  // went missing in Azahar at Sandover, Snowy's fort and start, Fire Canyon (2026-10-11)
  // (AI-assisted) impostors (CtrImpostor): far tie objects (each visibility group, up to 8
  // instances: trees, rocks, houses) whose nearest point is beyond impostor_distance (meters) and
  // that cover at most 128x128 pixels are drawn into a texture at screen resolution and shown as
  // a card (one quad per cell on the plane of the group's visible surface there) in their place,
  // until the picture could be off by impostor_error pixels (camera moved), the time of day moved
  // its colors, or they come nearer: then drawn again. At most impostor_budget triangles are drawn
  // into impostors per frame (the rest stay geometry that frame). impostor_atlas: the texture's
  // size in KB (256 | 512 | 1024; VRAM when there is room at start, else linear memory). On by
  // default since Oct 17 (the Sentinel Beach arch depth error fixed: CtrImpostor.h)
  bool impostors = true;  // (AI-assisted) on since Oct 17 (beach depth fix; preset safe: off)
  float impostor_distance = 120.f;
  float impostor_error = 1.f;
  int impostor_budget = 6000;
  int impostor_atlas = 512;
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
  // (AI-assisted) with raw_cmds: draws of the same vertices and matrices one after the other keep
  // the GPU's post-vertex cache (fewer command words; off until checked on hardware)
  bool vcache_reuse = true;
  // (AI-assisted, gpu-rt) static_cmdlists: the level meshes' raw command words of a stretch of
  // draws (same program, no citro3d update) are kept after two frames drew them alike and copied
  // in again while the draws, the GPU state before them and the settings stay the same; only the
  // chunk matrices are written anew. Same words (static_cmdlists = compare checks every replay
  // against the per-frame path and logs mismatches). 0 off, 1 on, 2 compare
  int static_cmdlists = 1;
  // (AI-assisted, gpu-rt) gpu_breakdown: every 4th frame the GPU runs each part of the frame alone
  // (clear, sky, level, merc, translucent, shadows, ocean, sprites, distort copy, HUD, transfer)
  // and its time is logged per stats window ([ctr] gpu breakdown). Serialises the GPU: slower
  // while on. Profiling only, off; needs pipeline = on
  bool gpu_breakdown = false;
  // (AI-assisted, gpu-rt) ocean_tex_half: the flat / waves ocean's shading texture is made every
  // second frame (the heights still every frame for the near grid); ocean = full has
  // ocean_texture_rate
  bool ocean_tex_half = true;
  // (AI-assisted) c3l v15 opaque alpha: the alpha tests / blending ctr_level_converter took out of
  // level draws whose texture alpha is 1.0 in every texel of every mip level and whose vertex alpha
  // is >= 0x80 (kDrawAlphaTestDropped / kDrawBlendDropped: they can't change a pixel) stay out; off
  // puts them back. Each frame (gpu_profile = ab). Older files: no such draws
  bool opaque_alpha = true;
  // (AI-assisted) cmd_dump: a frame's GPU command list to data/log/cmd_NN.bin every 20 s
  // (platform/3ds/tools/cmd_report.py); diagnostic, off
  bool cmd_dump = false;
  // (AI-assisted) tex_cache_clear = always | uploads (default): always clears the GPU's texture
  // cache at every texture bind (citro3d's way, ~300 per frame); uploads skips it only for binds
  // that keep a level pool texture and change its sampling settings (switches between level pool
  // textures skipped it too until the minecart red patches, see bind_texture)
  bool tex_cache_uploads = true;
  // (AI-assisted) hardware hang bisect switches (Oct 3 optimisations; off = the Oct 2 behaviour):
  // asm_tex: the 4x2 group / LDM-STM texture tiling, 64-bit PSMCT32 uploads and SIMD sky loops
  // (off: the old per texel loops); asm_mem: ctr_mem.S memcpy/memmove/memset (off: byte/word C
  // loops; read by platform/3ds/port at boot); fast_clock: steady_clock without divides (off:
  // clock_gettime)
  bool asm_tex = true;
  // (AI-assisted) defer_flush: the render thread's cache flushes held until the frame is submitted
  // (e165bc719); off: each one right away, as on Oct 2
  bool defer_flush = true;
  // (AI-assisted) shader_basic new | old: the 7 instruction ctr_basic (hardware proven) or the
  // Oct 2 one (ctr_basic_old). (The Oct 3 ctr_mesh / ctr_skin, which hung the real GPU, and their
  // switches new_shaders / shader_mesh / shader_skin were removed on Oct 16.)
  int shader_basic = 1;
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
/*! (AI-assisted) The same, changeable: only for gpu_profile = ab (render thread, between frames;
 * the switches it changes are read by the renderers each frame). */
CtrSettings& ctr_settings_ab();

namespace ctr_gfx {
/*! One line describing the renderer settings, for status displays. */
std::string settings_summary();
}  // namespace ctr_gfx
