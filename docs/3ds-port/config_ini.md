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
overlap = off              # on: the GPU starts drawing each part of a frame while the CPU builds the rest (experimental)
pipeline = off             # on: the CPU builds the next frame while the GPU draws this one (experimental; works with overlap)
distort = off              # sprite distorter: warp gate portals, heat haze (one screen copy in frames that have them)
envmap = on                # shine on power cells, precursor metal, ... (needs .c3l v9 files)
shadows = full             # (AI-assisted) full (= on): the game's shadows of Jak, enemies and objects (the PS2's shadow volumes: the game runs shadow-execute-all, the renderer draws the volumes into the stencil buffer and darkens what they cover, like the PC port). blob: only a dark spot under Jak (the game's fake shadow, as before; no volumes, nothing on the game thread). off: none
                           # cost of full: the game thread computes each shadow-casting object's volume (shadow-execute); the render thread turns them into GPU triangles (~65-70 ARM instructions per triangle and per vertex: Jak, ~400 triangles and ~240 vertices, takes a few hundredths of a millisecond) and draws three passes for all of them together: the volumes' faces twice (stencil only, no color: their screen area), then one rectangle per object over its shadow. Log line (debug): [ctr] shadow: N objects ..., and "shadow" in [ctr] build ms/frame by renderer
                           # full needs the stencil buffer: with depth16 = on it is blob (the log's settings line says so). The z-pass method of the PS2 and PC: with the camera inside a shadow volume (rare: the volumes reach from the object down to just below the ground) part of that shadow is missing

# ---- textures ----
mipmaps = on               # off / on / trilinear: smaller texture versions for far surfaces (needs .c3l v8+)
vram_textures = auto       # level textures in VRAM while there is room. auto: on (3DS and Azahar)
rgba4_as_rgba8 = auto      # 16-bit textures with alpha stored as 32-bit. auto: on in Azahar, off on the 3DS

# ---- 3DS hardware features (AI-assisted) ----
color16 = off              # on: 16-bit (RGB565) color buffer and top screen (half the memory traffic per pixel); faint blended layers like waterfall mist vanish and gradients band
early_depth = off          # the GPU's early depth test for opaque level / model draws (drawn nearest first): hidden pixels skip texturing. Off by default: on a New 3DS it leaves large blocks of the screen where the level never draws (Azahar ignores it)
                           # block32: early depth with the render target in 32x32 block mode (256x416, cropped to the screen by an extra texture copy; needs frame_pacing = steady and 0.4 MB linear memory, else the usual target with early depth off). Hardware only: Azahar doesn't emulate early depth or block mode. Screenshots are skipped in this mode
depth16 = off              # on: 16-bit depth buffer instead of 24-bit + stencil (the stencil is used by ocean = waves and shadows = full, which becomes blob): half the depth buffer traffic and 192 KB more VRAM for textures, but the game's depth is 1 / distance, so 16 bits leave ~6 cm steps at 30 m and ~0.6 m at 100 m: far geometry may z-fight
compact_textures = on      # sprite, HUD and font textures in 8 / 16-bit formats (L8, LA8, RGB565, RGBA5551, RGBA4), and in 384 KB of reserved VRAM with vram_textures
proctex_glows = on         # glows (round gradient textures) from the procedural texture unit: no texels read
sprite_max_size = 0.5      # world particles and glows at most this fraction of the screen height across (0 = no limit)
vis_culling = on           # the game's own visibility data hides level parts behind walls and hills (needs .c3l v10 files)
run_culling = on           # (AI-assisted) a level chunk (200 m cell) that crosses the edge of the view has its pieces tested on their own (the visibility runs: up to 8 tfragments or tie instances; chunks without them in pieces of ~96 triangles), so only what is on screen is sent to the GPU. Bounding spheres made at load (8 bytes per piece, ~30-50 KB per level). Log line: [ctr] l0-tfrag: run culling tested N runs, hid ... per level draw
neighbour_vis = on         # (AI-assisted) a level the camera isn't in (its visibility string is computed from the other level's data) has its tfrag hidden by the bits too, and its low resolution tfrag drawn where they say, as the PS2 and PC do. off: only its ties use the bits
lowres_vista = on          # (AI-assisted) the levels' low resolution tfrag (the game's own far scenery: Snowy's forest floor below, the hills around Sandover) is drawn as vista: also when the camera is in the level, where no loaded level's detailed tfrag is (+50 m), out to vista_distance, with a haze towards the fog color from draw_distance on instead of the draw distance fog (solid fog color from draw_distance on). off: only for a level seen from a neighbouring one (far mode), within draw_distance. Log line: [ctr] l0-tfrag: vista N chunks M tris, low resolution chunks in near mode K
vista_distance = 3000      # (AI-assisted) meters; far scenery (lowres_vista, and the vista tier of .c3l v13 files) is drawn up to here, its haze reaching the fog color there. Levels with a vista tier (the converter's far scenery: the low resolution tfrag, the ties without a distance limit and, without low resolution tfrag, the coarse tfrag, simplified and merged into 1.5 km cells, one draw per texture) show it wherever their chunks are beyond their draw distance, and use one haze from fog_start * draw_distance to vista_distance for all their geometry (no solid fog ring at the draw distance)
fog_stage_skip = off       # (AI-assisted) level chunks entirely in front of where both fogs start (the game fog with fog = on, and the draw distance fog from fog_start * draw_distance) are drawn without the fog stage: texture combiner pass-through and texture unit 1 (the fog ramp) off for those draws. Off until priced on hardware with gpu_profile = ab: switching the unit on and off between draws may cost more than the ramp lookups save. Log: [ctr] textures: ... N level draws without the fog stage (M texture unit 1 switches)
cull_ties = on             # (AI-assisted) on: back faces of closed tie surfaces (rocks, houses, walls marked by the converter in .c3l v13 files: welded, consistently wound, at most 10% open edges or open only at the bottom, a clear inside) are culled by the GPU; flip: the same with the other winding (if faces go missing on hardware). Needs levels converted with the current ctr_level_converter; other draws are never culled
raw_cmds = on              # (AI-assisted) draws write their GPU commands themselves instead of through citro3d: less render thread CPU per draw, ~40% fewer command words. off: the citro3d path, for comparing
vcache_reuse = off         # (AI-assisted) with raw_cmds: a draw with the same vertices and matrices as the one before keeps the GPU's post-vertex cache (a few command words less). Try on hardware: wrong or jumping vertices on models mean it has to stay off
merc_lod_scale = 0.5       # characters switch to their lower detail models at this fraction of the game's distances (0.05 .. 1)
anim_lod = on              # (AI-assisted) actors that weren't drawn last frame and are more than 30 m (plus their size) from both the camera and Jak update their skeleton every 4th frame only. Never Jak, Daxter, cutscenes or actors with joint callbacks. off: every frame, like the PS2
anim_native = on           # (AI-assisted) the animation decompressor's per-joint work (decompress, normalize) in native code instead of the GOAL code; same results. off: the GOAL versions, to compare

# ---- hardware hang bisect (AI-assisted, 2026-10-03): off = the Oct 2 behaviour of each Oct 3 optimisation ----
new_shaders = off          # (off: the new ctr_mesh hangs the real GPU) the shorter vertex shaders (DPH/MADI/rsq, repacked mesh fog uniforms). off: the Oct 2 ctr_basic / ctr_mesh / ctr_skin / ctr_skin_env (ctr_*_old.v.pica) with the old fog uniforms. ocean = waves always uses the new ocean shader
shader_basic = new         # new: the 7 instruction ctr_basic (verified on hardware). old: the Oct 2 one
shader_mesh =              # old | new | dph (Oct 2 shader + dph transform); empty: follows new_shaders. new hangs the real GPU
defer_flush = on           # (hang bisect) the render thread's cache flushes held until the frame goes to the GPU. off: each right away, as on Oct 2
asm_tex = on               # texture tiling in 4x2 groups (LDM/STM for RGBA8), 64-bit PSMCT32 VRAM uploads, SIMD sky blend / cloud premultiply. off: the Oct 2 per texel / per byte loops
asm_mem = on               # ctr_mem.S memcpy / memmove / memset (LDM/STM). off: plain C byte / word loops (ctr_mem_c.c; also used before config.ini is read)
fast_clock = on            # steady_clock from the system tick without divides. off: clock_gettime(CLOCK_MONOTONIC), as libstdc++

# ---- measuring ----
gpu_profile = off          # every 2.5 s leaves out one group of renderers and logs what each costs the GPU (flickers); also logs the GPU's own counters once per level (`[ctr] gpu profile <level>: hw busy % ...`: which pipeline stage is busy, and memory traffic per frame; zeros in Azahar)
stats_period = 15          # (AI-assisted) seconds between the render thread's statistics lines ([ctr] render ms/frame ... triangles, draws; culling); 5 for measuring fixed views
gpu_counters = off         # (AI-assisted) the GPU's own counters over 10 s of each level, 10 s after it started, with nothing left out of the picture: the same `[ctr] gpu profile <level>: hw busy % ...` line (read through the GSP service, no extra system calls). Off by default: not yet run on hardware
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
