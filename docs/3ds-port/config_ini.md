# config.ini: 3DS settings (rendering, sound, system)

(AI-assisted.) One file holds all the 3DS settings: `config.ini`, read once at startup. Every
setting has a default, so the file is optional: write only the lines you want to change. The log
(`stdout.log`) prints the renderer settings in use: `[ctr] settings from ...: dist 500m lod 200m
...` (or `[ctr] no config.ini, default settings: ...`), and warns about unknown keys.

It is the only settings file: there are no flag files, and `render.ini` isn't read any more
(rename yours to `config.ini`). `stage_sd.sh` options set keys in it and keep the other lines.

## Sound and system

```ini
sound = on                 # sound output (off: silent; it costs CPU time)
sound_core = 2             # CPU core of the sound mixer: 2 (default on New 3DS: the render thread's core, which has room; core 0 runs the game logic), 0, 1
io_on_system_core = off    # on: the game's I/O threads on core 1 (the system core; experimental)
mips2c_native_off = off    # all: the mips2c versions instead of the native functions; or names
collide_frag_cache = on    # (AI-assisted) pc-collide-frag-may-hit? keeps each collide mesh's vertex box; off: scans the vertices every call
collide_native = on        # (AI-assisted) the drawable-tree walks of collide-with-box / fill-from-foreground-using-box in C++; off: the GOAL code
perf_sections = off        # on: per-section timing in the log (slower)
args = -boot -cbackend     # the game's arguments (-debug etc. like on PC)
listener = off             # on: Wi-Fi REPL (goalc can connect; 1 MB of RAM)
screenshots = 0            # N: save the top screen every N frames to data/log
pad_script =               # FILE (relative to sdmc:/3ds/jak1): scripted controller input for tests
debug_log = off            # on: debug lines in data/log/gk.log (thousands in a level load; with log_async = off every line is an SD card write)
log_async = on             # (AI-assisted) the log writer thread writes the logs about once a second (3ds_build.md "Screens and logs"); off: every thread writes its own lines, as before
io_checksum = off          # on: log the CRC-32 of the bytes the game read from each big file (debug; "(ok)" for checked .ogz files; with it, every .ogz file gets an [io] line)
io_cache = on              # off: no files kept open between loads, no read-ahead (big SD card reads), no cache of spool chunk 0 (the Oct 3 file reading)
```

## Where it goes

- 3DS: `sdmc:/3ds/jak1/config.ini`.
- PC (`gk --ctr-gfx`): `<project>/config.ini`.

## Format

- One `key = value` per line. `#` or `;` starts a comment (also after a value).
- On / off: `1`, `true`, `on`, `yes` turn a setting on; anything else turns it off.
- Distances are in meters.
- `auto` (for `rgba4_as_rgba8` and `vram_textures`) keeps the automatic choice: what works in
  the emulator (Azahar) vs on a real 3DS. The game detects the emulator itself.

## A complete config.ini (renderer part)

Every setting with its default. Copy it, then change what you need.

```ini
# ---- level geometry ----
draw_distance = 500        # meters; level chunks further away are not drawn (0 = no limit)
fog_start = 0.6            # the extra fog towards draw_distance starts at this fraction of it
fog = on                   # the game's own distance fog
lod_distance = 200         # meters; detailed tfrag up to here, the coarse version beyond (0 = always detailed)
far_level_distance = 80    # meters; a level seen from a neighbouring one is drawn with only its low resolution version (0 = off). (AI-assisted) Was 40: its objects popped in at 40 m. With chunk kinds (see below) it also keeps its ties without a distance limit (houses, towers, big rocks) in that mode
detail_scale = 1           # multiplies the draw distance of small and medium objects and shrubs (all of them, also with the two keys below)
shrub_distance = 50        # (AI-assisted) meters; shrubs (grass, bushes, flowers) up to here (0 = the converter's 100 m). They are most of the GPU's work in village2, rolling, ogre and swamp (200-300k triangles within 100 m)
tie_distance_scale = 1.5   # (AI-assisted) multiplies the converter's draw distance of small and medium objects (ties: 100 m and 300 m); the big ones have no limit
auto_detail = on           # (AI-assisted) shrub / tie distances follow the GPU's time per frame: from the two values above (the near end) toward the two below while the GPU has time to spare, back when a frame gets close to the 33 ms budget. Needs frame_pacing = steady. Log: "[ctr] auto detail T: shrubs N m, ties xK" every 15 s
shrub_distance_max = 120   # (AI-assisted) the far end for shrubs with auto_detail
tie_distance_scale_max = 3 # (AI-assisted) the far end for small and medium objects with auto_detail
auto_detail_gpu_ms = 27    # (AI-assisted) the GPU time per frame auto_detail stays under (10-33)
                           # shrub_distance, tie_distance_scale and the far mode ties need .c3l files with chunk kinds (the current converter); with older files only detail_scale applies

# ---- what is drawn ----
merc = on                  # characters and objects
sprites = on               # world particles; the HUD is always drawn
max_sprites = 1000         # world particles per frame
ocean = on                 # the ocean: off, flat (= on, static grid), waves (+ waves near the camera), full (the PS2 ocean: the game builds its DMA, more CPU on both threads, see 3ds_build.md)
                           # (AI-assisted) waves: a 3 m grid out to ~45 m around the camera moves with the game's own wave heights (what boats and floating things ride), crests brighter, plus a second scrolling wave texture over all of the ocean. ~0.1 ms render thread CPU, a few hundred microseconds of GPU. Log line: [ctr] ocean waves. With depth16 (no stencil) the flat ocean beyond is lowered by the deepest trough
ocean_texture_rate = 2     # (AI-assisted) ocean = full: the wave texture is made every nth frame (1 = every frame, up to 8); it costs the render thread 1-1.7 ms each time. Log line: [ctr] ocean full (... texture ... in N% of frames)
ocean_near_alpha = 0.6     # (AI-assisted) ocean = full: the near ocean is at least this opaque (0..1). The PS2 / PC draw it at 0.5 within 12 m of the camera (1 from 24 m) and add a sky reflection pass the 3DS leaves out; 0.6 makes up for that pass's average brightening at the beach (0.5 = exactly the PS2's textured pass, see CtrOceanFull.h)
fps_cap = 30               # 30 (default): steady 30 fps (2 vblanks per frame); 60: up to 60
frame_pacing = steady      # steady (default): every frame stays on screen as long as the game time it covers (even motion, ~1 frame more latency); fast: shown as soon as the GPU is done (uneven at ~30 fps). Log line: [ctr] pacing. (AI-assisted) The latency (2-4 vblanks) goes up after a late frame and down after ~5 s without one and with at most 2 frames done just in time, but for ~30 s not below the latency a frame was late at
sky = on                   # the sky (time of day texture, clouds); off: cleared to the fog color
clouds = on                # (AI-assisted) the clouds over the sky; off: the sky without them (one added pass less over the sky's part of the screen)
sky_last = off             # (AI-assisted) the sky drawn after the opaque level and models instead of first: right before the first draw that needs what is behind it (anything blended, without depth write, stencil, a screen copy) or at the latest before the translucent buckets, with fragment depth 0 (the GPU's depth map: scale 0) and the depth test, so it only fills the pixels nothing opaque covered. Same picture (every blended pixel still blends over the sky). The sky's pixels behind the level are dropped before blending and color writes (the clouds read the framebuffer), and with the early depth test (early_depth = on / block32 / sky / sky32) before texturing. Off until priced on hardware (gpu_profile ab)
skip_clear = off           # (AI-assisted) frames whose opaque sky roof covers the whole screen (looking up; tested on a 25 x 15 grid a little beyond the edges, every roof vertex in front of the camera) clear only the depth buffer, not the color: every pixel's color is drawn over anyway. Same picture. Small: the color and depth fills run on the GPU's two fill units at the same time. The `[ctr] render ms/frame` log counts nothing for it; sky = off keeps the clear
overlap = off              # on: the GPU starts drawing each part of a frame while the CPU builds the rest (experimental)
pipeline = off             # on: the CPU builds the next frame while the GPU draws this one (experimental; works with overlap)
frame_overlap2 = clean     # (AI-assisted) with pipeline = on, the GPU's idle gap between two frames. off: the frame end waits for the GPU, then hands the frame's command list over with a cache flush the GSP does while the GPU waits (as before). clean (default): the list (130-260 KB) is cleaned from the CPU cache before that wait and handed over without the flush (same commands, same order). on: no wait at the frame end: the list is queued behind the frame the GPU is drawing; the next frame begin waits for the frame before that (whose buffers it reuses) instead. on needs frame_pacing = steady (else clean). The `[ctr] render ms/frame` line shows busy X (... active Y): active counts each frame from its command list on, busy also the CPU's build time of a pipelined frame. In gpu_profile = ab (on = the config's mode, clean if off; off = off)
distort = off              # sprite distorter: warp gate portals, heat haze (one screen copy in frames that have them)
distort_copy = rows        # (AI-assisted) rows: the distorter's screen copy covers only the screen columns its sprites read (8-pixel tile rows of the color buffer), the copy texture is made in VRAM if there is room for it then (512 KB, 256 KB with color16), and distort sprites under 8 pixels are left out (no copy in frames with only those). full: the whole screen into linear memory every frame with distort sprites, as before. Log line: [ctr] distorter screen copy: texture in VRAM | linear memory
envmap = on                # shine on power cells, precursor metal, ... (needs .c3l v9 files)
merc_tex_sort = on         # (AI-assisted) the opaque draws of a model bucket's models (not those with blend shapes or rewritten vertices) are queued and drawn draw by draw across the instances of a model (draw 0 of every crate, then draw 1, ...; each instance keeps its own order): one texture bind per draw instead of per draw and instance, but each draw then uploads its instance's bones (3 vectors per bone) instead of once per instance. Same picture. Implies envmap_batch for those models
envmap_batch = on          # (AI-assisted) the shine draws of a model bucket are queued and drawn together at its end (before a blended or no-depth-write model draw, if one comes first): one switch to the shine shader and back per bucket instead of per draw, and the model draws keep their uploaded bones. Same picture (the shine adds to the base it passes the depth test on). off: each right after its base, as before
shadows = full             # (AI-assisted) full (= on): the game's shadows of Jak, enemies and objects (the PS2's shadow volumes: the game runs shadow-execute-all, the renderer draws the volumes into the stencil buffer and darkens what they cover, like the PC port). blob: only a dark spot under Jak (the game's fake shadow, as before; no volumes, nothing on the game thread). off: none
                           # cost of full: the game thread computes each shadow-casting object's volume (shadow-execute); the render thread turns them into GPU triangles (~65-70 ARM instructions per triangle and per vertex: Jak, ~400 triangles and ~240 vertices, takes a few hundredths of a millisecond) and draws three passes for all of them together: the volumes' faces twice (stencil only, no color: their screen area), then one rectangle per object over its shadow. Log line (debug): [ctr] shadow: N objects ..., and "shadow" in [ctr] build ms/frame by renderer
shadow_casters = 6         # (AI-assisted) shadows = full: at most this many objects' shadows per frame, the ones that look biggest from the camera (their shadow mesh's bounding sphere over its distance; Jak's normally is one of them). 0: all of them, as before. The game thread still computes every volume. Log line (debug): [ctr] shadow: ... N over shadow_casters
                           # full needs the stencil buffer: with depth16 = on it is blob (the log's settings line says so). The z-pass method of the PS2 and PC: with the camera inside a shadow volume (rare: the volumes reach from the object down to just below the ground) part of that shadow is missing

# ---- textures ----
mipmaps = on               # off / on / trilinear: smaller texture versions for far surfaces (needs .c3l v8+)
mip_bias = 0               # (AI-assisted) LOD bias of textures with mip levels (GPUREG_TEXUNITn_LOD): +0.5 / +1 switch to the next smaller level sooner (blurrier up close, fewer texels read per pixel), negative sharper. -4..4; 0 (default) = none. In gpu_profile = ab (on = the config's bias, +1 if it is 0). pica_bench B6.rgba8.min4mip.bias1 vs B6.rgba8.min4mip prices +1
skin_rows_f24 = on         # (AI-assisted) merc bone matrices (3 rows per bone, up to 24 bones per draw) uploaded as float24 uniforms (3 words per vector, rounded to nearest) instead of float32 (4): 216 instead of 288 words per palette, a quarter fewer uniform words for the GPU's command processor. The vertex shader computes in float24 either way (relative precision 2^-17 after rounding: ~0.04 mm on a bone 5 m from the camera). In gpu_profile = ab; pica_bench B2.unif_f24 vs B2.unif_f32 prices a word
vram_textures = auto       # level textures in VRAM while there is room. auto: on (3DS and Azahar)
rgba4_as_rgba8 = auto      # 16-bit textures with alpha stored as 32-bit. auto: on in Azahar, off on the 3DS

# ---- 3DS hardware features (AI-assisted) ----
color16 = off              # on: 16-bit (RGB565) color buffer and top screen (half the memory traffic per pixel); faint blended layers like waterfall mist vanish and gradients band
early_depth = off          # the GPU's early depth test for opaque level / model draws (drawn nearest first): hidden pixels skip texturing. Off by default: on a New 3DS it leaves large blocks of the screen where the level never draws (Azahar ignores it)
                           # block32: early depth with the render target in 32x32 block mode (256x416, cropped to the screen by an extra texture copy; needs frame_pacing = steady and 0.4 MB linear memory, else the usual target with early depth off). Hardware only: Azahar doesn't emulate early depth or block mode. Screenshots are skipped in this mode
                           # (AI-assisted) sky / sky32: the early depth test only for the sky, which is then drawn last (sky_last is forced on) at depth 0: its fragments in blocks the opaque geometry covered are dropped before texturing; the opaque draws keep their order and no early test (sky32: with the block32 target). on / block32 with sky_last = on: the sky uses it too. Which mode works is decided by pica_bench's B8 lines (B8.n.* for on / sky, B8.b32.* for block32 / sky32: c2 shows whether a partly covered block wrongly drops fragments, c4c whether draws without the early test still fill its data, which sky needs). gpu_profile = ab switches it (the config's mode or off; nothing when the config says off)
early_depth_func = ge      # (AI-assisted) the early test's function: ge (default, as before), gt, le, lt. The normal depth test stays >= (larger = closer). pica_bench B8.*.c3.<func>.d0 / dff show which function and clear value pass everything after a clear
early_depth_clear = 0      # (AI-assisted) the early depth data's clear value (GPUREG_EARLYDEPTH_DATA, its own storage, not the depth buffer; written by every early depth clear: at each frame begin and after each citro3d command list split): 0 (default, far), max (0xFFFFFF) or a number
depth16 = off              # on: 16-bit depth buffer instead of 24-bit + stencil (the stencil is used by ocean = waves and shadows = full, which becomes blob): half the depth buffer traffic and 192 KB more VRAM for textures, but the game's depth is 1 / distance, so 16 bits leave ~6 cm steps at 30 m and ~0.6 m at 100 m: far geometry may z-fight
compact_textures = on      # sprite, HUD and font textures in 8 / 16-bit formats (L8, LA8, RGB565, RGBA5551, RGBA4), and in 384 KB of reserved VRAM with vram_textures
proctex_glows = on         # glows (round gradient textures) from the procedural texture unit: no texels read
sprite_max_size = 0.5      # world particles and glows at most this fraction of the screen height across (0 = no limit)
sprite_min_alpha = 6       # (AI-assisted) world particles bigger than ~20 pixels (half size) are left out below this alpha (the game's 0..128: 6 = under 5% opaque): faint mist and smoke that adds a few color steps but blends a big area. 2: the old rule (only sprites under 2, any size). Log line (debug): [ctr] sprite: ... faint big ones left out
sprite_alpha_discard = on  # (AI-assisted) blended world particles drop their fully transparent pixels (alpha test greater than 0; only the blend modes where alpha 0 adds nothing), so those pixels cost no framebuffer read and write. Same picture. off: no alpha test, as before
vis_culling = on           # the game's own visibility data hides level parts behind walls and hills (needs .c3l v10 files)
run_culling = on           # (AI-assisted) a level chunk (200 m cell) that crosses the edge of the view has its pieces tested on their own (the visibility runs: up to 8 tfragments or tie instances; chunks without them in pieces of ~96 triangles), so only what is on screen is sent to the GPU. Bounding spheres made at load (8 bytes per piece, ~30-50 KB per level). Log line: [ctr] l0-tfrag: run culling tested N runs, hid ... per level draw
neighbour_vis = on         # (AI-assisted) a level the camera isn't in (its visibility string is computed from the other level's data) has its tfrag hidden by the bits too, and its low resolution tfrag drawn where they say, as the PS2 and PC do. off: only its ties use the bits
lowres_vista = on          # (AI-assisted) the levels' low resolution tfrag (the game's own far scenery: Snowy's forest floor below, the hills around Sandover) is drawn as vista: also when the camera is in the level, where no loaded level's detailed tfrag is (+50 m), out to vista_distance, with a haze towards the fog color from draw_distance on instead of the draw distance fog (solid fog color from draw_distance on). off: only for a level seen from a neighbouring one (far mode), within draw_distance. Log line: [ctr] l0-tfrag: vista N chunks M tris, low resolution chunks in near mode K
vista_distance = 3000      # (AI-assisted) meters; far scenery (lowres_vista, and the vista tier of .c3l v13 files) is drawn up to here, its haze reaching the fog color there. Levels with a vista tier (the converter's far scenery: the low resolution tfrag, the ties without a distance limit and, without low resolution tfrag, the coarse tfrag, simplified and merged into 1.5 km cells, one draw per texture) show it wherever their chunks are beyond their draw distance, and use one haze from fog_start * draw_distance to vista_distance for all their geometry (no solid fog ring at the draw distance)
fog_stage_skip = off       # (AI-assisted) level chunks entirely in front of where both fogs start (the game fog with fog = on, and the draw distance fog from fog_start * draw_distance) are drawn without the fog stage: texture combiner pass-through and texture unit 1 (the fog ramp) off for those draws. Off until priced on hardware with gpu_profile = ab: switching the unit on and off between draws may cost more than the ramp lookups save. Log: [ctr] textures: ... N level draws without the fog stage (M texture unit 1 switches)
cull_ties = on             # (AI-assisted) on: back faces of closed tie surfaces (rocks, houses, walls marked by the converter in .c3l v13 files: welded, consistently wound, at most 10% open edges or open only at the bottom, a clear inside) are culled by the GPU; flip: the same with the other winding (if faces go missing on hardware). Needs levels converted with the current ctr_level_converter; other draws are never culled
raw_cmds = on              # (AI-assisted) draws write their GPU commands themselves instead of through citro3d: less render thread CPU per draw, ~40% fewer command words. off: the citro3d path, for comparing
vcache_reuse = off         # (AI-assisted) with raw_cmds: a draw with the same vertices and matrices as the one before keeps the GPU's post-vertex cache (a few command words less). Try on hardware: wrong or jumping vertices on models mean it has to stay off
tex_cache_clear = always   # (AI-assisted) always (default): every texture bind clears the GPU's texture cache (TEXUNIT_CONFIG bit 16, as citro3d does; ~300 per frame). uploads: binds of level pool textures (texels that never change) skip the clear unless a pool was filled or moved to VRAM since the last one; the first bind of each frame and binds of all other textures (sky, clouds, eyes, HUD, the screen copy) still clear. Hardware only (Azahar has no texture cache): wrong texels would show as stale patches of another texture; pica_bench B2.rebind vs B2.rebind_clr prices the clear. Log: [ctr] textures: ... N texture binds without the cache clear. In gpu_profile = ab
cb_read_opaque = off       # (AI-assisted) GPUREG_COLORBUFFER_READ = 0 for draws whose color doesn't depend on the framebuffer (opaque, and the sky's blend over the clear color; not with stencil or no-color passes), 0xF again for blended ones, with a framebuffer flush before each switch (citro3d always reads: read = write = the color mask). GBATEK: unknown what read disable does. Use only if pica_bench's B6.cbread0.probe line says bg_ok=1 (the pixels next to a partly covered 8x8 block survive) and B6.cbread0 is faster than B6.untex. In gpu_profile = ab only when on in the config. Log: [ctr] textures: ... N color buffer read switches
merc_lod_scale = 0.5       # characters switch to their lower detail models at this fraction of the game's distances (0.05 .. 1)
merc_gen_lod = on          # (AI-assisted) models that come with only one detail level (the yeti, flut flut and its platforms, snow ball, spatula, ...) switch to a simplified copy the converter made (same vertices, ~30-65% of the triangles; ctr_level_converter --gen-lod) beyond max(25 m, 12.5 x the model's size) x merc_lod_scale, back 20% nearer (no flicker at the edge). Not Jak, Daxter or models with face animation / rewritten vertices. Needs .c3l files from the current converter (older ones: no effect). Log line (debug): [ctr] merc: ... N with the generated low LOD
anim_lod = on              # (AI-assisted) actors that weren't drawn last frame and are more than 30 m (plus their size) from both the camera and Jak update their skeleton every 4th frame only. Never Jak, Daxter, cutscenes or actors with joint callbacks. off: every frame, like the PS2
anim_native = on           # (AI-assisted) the animation decompressor's per-joint work (decompress, normalize) in native code instead of the GOAL code; same results. off: the GOAL versions, to compare

# ---- hardware hang bisect (AI-assisted, 2026-10-03): off = the Oct 2 behaviour of each Oct 3 optimisation ----
new_shaders = off          # (off: the new ctr_mesh hangs the real GPU) the shorter vertex shaders (DPH/MADI/rsq, repacked mesh fog uniforms). off: the Oct 2 ctr_basic / ctr_mesh / ctr_skin / ctr_skin_env (ctr_*_old.v.pica) with the old fog uniforms. ocean = waves always uses the new ocean shader
shader_basic = new         # new: the 7 instruction ctr_basic (verified on hardware). old: the Oct 2 one
shader_mesh =              # old | new | dph (Oct 2 shader + dph transform); empty: follows new_shaders. new hangs the real GPU
shader_skin =              # (AI-assisted) old | new: ctr_skin and ctr_skin_env; empty: follows new_shaders. new passes check_shaders.py (each output component written exactly once) but never ran on hardware; pica_bench's B3.skin_new runs it once
defer_flush = on           # (hang bisect) the render thread's cache flushes held until the frame goes to the GPU. off: each right away, as on Oct 2
asm_tex = on               # texture tiling in 4x2 groups (LDM/STM for RGBA8), 64-bit PSMCT32 VRAM uploads, SIMD sky blend / cloud premultiply. off: the Oct 2 per texel / per byte loops
asm_mem = on               # ctr_mem.S memcpy / memmove / memset (LDM/STM). off: plain C byte / word loops (ctr_mem_c.c; also used before config.ini is read)
fast_clock = on            # steady_clock from the system tick without divides. off: clock_gettime(CLOCK_MONOTONIC), as libstdc++

# ---- measuring ----
gpu_profile = off          # every 2.5 s leaves out one group of renderers and logs what each costs the GPU (flickers); also logs the GPU's own counters once per level (`[ctr] gpu profile <level>: hw busy % ...`: which pipeline stage is busy, and memory traffic per frame; zeros in Azahar)
                           # (AI-assisted) gpu_profile = ab: A/B windows instead. Each switch of the table (run_culling, cull_ties, vista, fog_stage_skip, shadows (the volumes' draws), envmap, distort, distort_copy, shadow_casters, sprite_min_alpha, sprite_alpha_discard, envmap_batch, merc_tex_sort, merc_gen_lod, clouds, frame_overlap2, sky_last, skip_clear, early_depth, tex_cache_clear, mip_bias, skin_rows_f24, cb_read_opaque; CtrRenderer.cpp kAbSwitches) is turned on for ~1.5 s, then off for ~1.5 s (the next cycle off first), the others staying at their config.ini values; from 10 s after each level starts, for as long as the setting is there. Log, after each cycle (~45 s), per switch: [ctr] gpu ab <level> cycle N: <key> on X ms, off Y ms (on - off D ms; triangles, draws ...): the GPU busy time per frame (needs frame_pacing = steady). color16 and depth16 change the render target and can't be switched while running: price them with two sessions
stats_period = 15          # (AI-assisted) seconds between the render thread's statistics lines ([ctr] render ms/frame ... triangles, draws; culling); 5 for measuring fixed views
gpu_counters = off         # (AI-assisted) the GPU's own counters over every stats window (15 s), with nothing left out of the picture: `[ctr] gpu profile window <level>: hw busy % ...` (which pipeline stage is busy, sampled over the window's first 10 s; memory traffic per frame over all of it). Read through the GSP service, no extra system calls or IO mappings. Off by default: not yet run on hardware
gpu_perflog = off          # (AI-assisted) the GSP's performance log once per stats window (`[ctr] gpu perflog: ...`): GPU time per frame of the command lists (p3d), display transfers (ppf) and memory fills (psc), raw and in ms, against the fence's busy time (bubble = busy - p3d - ppf - max psc); the vblank interval calibrates the disputed unit. One GSP request per window, no extra system calls
pica_bench = off           # (AI-assisted) a GPU microbenchmark at boot, before the game starts (~20-60 s on hardware; progress on the bottom screen): per draw, per vertex, vertex cache, attribute, fill, setup and early depth tests, one `[bench] ...` line each in gk.log and data/log/bench.txt; `platform/3ds/tools/bench_report.py gk.log` prints the table (docs/3ds-port/performance.md "GPU timing in one session"). The tests that could hang the GPU run last; after a hang the last `start` line of bench.txt names the test. Turn it off again after one run
```

## Recommended settings

- **Real New 3DS (default):** no file needed. If the frame rate is low, run once with
  `gpu_profile = on` and look for `[ctr] gpu profile: all X ms, level Y, merc Z, sprites ...` in
  the log: it shows which group costs the GPU the most. Then:
  - level expensive: lower `shrub_distance` (e.g. 35), `draw_distance` (e.g. 300) and
    `lod_distance` (e.g. 120), or `detail_scale = 0.7`;
  - sprites expensive: `max_sprites = 500`;
  - turn off `distort` or `envmap` to see what they cost.
  Keep `mipmaps = on` and `vram_textures = auto`: without them the GPU reads full size textures
  from main memory, which was the main cause of low frame rates on hardware.
- **Azahar (emulator):** no file needed; `auto` picks `rgba4_as_rgba8 = on` and
  `vram_textures = on` there too (Azahar draws RGBA4 textures as noise, VRAM textures are fine;
  with VRAM off the emulator needed ~4 MB more linear memory than the 3DS and levels didn't fit). Azahar doesn't
  model the GPU's speed, so frame rates there say nothing about the GPU on hardware.
- **Best looking:** `mipmaps = trilinear` (smoother far textures, a little slower),
  `sprite_max_size = 0`, `merc_lod_scale = 1`, `shrub_distance = 0` (shrubs to 100 m: slower in
  village2 and rolling).
- **3DS hardware features:** all on by default except `early_depth` and `color16`. The log line `[ctr] textures: ...` shows the
  compact textures (and how many are in the reserved VRAM, how many are radial glows) and the
  last frame's early depth and procedural texture draws; `[ctr] l0-tfrag: visibility hid ...`
  shows what the game's visibility data hides. Compare `gpu` in `[ctr] render ms/frame` with a
  feature on and off to see what it saves on hardware (Azahar doesn't model the GPU's speed).
- **Hardware fog:** not used. The PICA200 fog unit looks fog up by depth, and the game's depth
  (1 / distance, like the PS2) puts everything beyond about 32 m into the first of its 128 table
  entries; the fog stays the per-vertex ramp texture.

## Things outside config.ini

- **Level files:** the features above need levels converted with the current converter
  (`ctr_level_converter --all out/jak1/fr3 <sd>/out/jak1/c3l`, see `c3l_format.md`). Files from
  an older converter still load, with a warning in the log: v7 has no mip levels and 16-bit
  textures (slow on hardware), v8 has no envmap shine. Shrubs and the base pass of shiny
  objects also need a current conversion.
- **Sky and the death effect** need the game code built with the current `goal_src` (they are
  partly GOAL changes for the 3DS build).
