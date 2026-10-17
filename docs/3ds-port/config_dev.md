# config.ini: every key (testing and development)

(AI-assisted.) The player's page is [config_ini.md](config_ini.md): the 8 user keys `stage_sd.sh`
writes (`preset`, `sound`, `fog`, `draw_distance`, `shadows`, `particles`, `text_size`, `ocean`).
This page lists all the other keys. They still work when written into `config.ini` (a line wins
over the preset and the defaults), but staging doesn't write them, except the keys of its testing
options (`--args`, `--listener`, `--screenshots`, `--pad-script`, `--perf-sections`,
`--io-checksum`, `--sound CORE`), which go at the end of the file under `# testing keys`.

The defaults are the settled play values (Oct 16: the user's release config.ini, all finished
improvements on). The log (`stdout.log`) prints the settings in use:
`[ctr] settings from <file> (preset normal; lines that differ from it: fog = off): dist 500m ...
| sound on sound_core auto ...`: the preset, the config.ini lines whose value differs from what
the preset gives, every renderer setting, then the keys read outside the renderer. Unknown keys
get a warning, removed ones a note.

## Presets and precedence

A key's value is, first match wins (one table for every reader:
`game/graphics/ctr/ctr_config_presets.h`, used by `CtrSettings.cpp` on the 3DS and PC and by
`ctr_config_get` in `platform/3ds/port/ctr_port.c`):

1. its own line in config.ini (the last one if there are several);
2. a user key that stands for it: `particles = full` (`max_sprites = 2176`,
   `sprite_max_size = 0`, `sprite_min_alpha = 2`) or `reduced` (1000, 0.5, 6); `text_size = X`
   (`text_scale = X`);
3. the preset (`preset = normal | safe | bench | breakdown`; presets set only these keys, never
   the user keys):
   - `normal`: nothing (the defaults);
   - `safe`: the Oct 11 behaviour (3ds-sdcard-oct12d config-safe.ini; its particle limits are the
     user key `particles = reduced`, which `stage_sd.sh --preset safe` writes): `overlap`, `shader_opt`,
     `frame_overlap2`, `skin_rows_f24`, `skin_clip_f24`, `cmd_dedup`, `cmd_dedup_draw`,
     `fog_alpha`, `tex1_gating`, `cull_merc`, `sky_last`, `vista_untextured`, `bones_native`,
     `fog_stage_skip`, `static_cmdlists`, `ocean_tex_half`, `opaque_alpha`, `fine_vis`,
     `vcache_reuse`, `skip_clear`, `tie_lod` off, `tex_cache_clear = always`, `c3l_read_buffer = 0`;
   - `bench`: `pica_bench = on`, `gpu_profile = ab`, `gpu_perflog = on`, `gpu_counters = on`,
     `overlap = off`;
   - `breakdown`: `gpu_breakdown = on`;
4. the default (below; `ctr_cfg_defaults` for the keys read outside the renderer).

Removed on Oct 16 (with their code; ignored with a note in the log): `new_shaders` (the Oct 3
`ctr_mesh` hung the real GPU), `shader_mesh`, `shader_skin` (they only chose the removed
programs), `shader_groups` (shared shbins, never better than `shader_opt`), `cb_read_opaque`
(corrupted partly covered 8x8 tiles on hardware), `io_on_system_core` (boot hang).

## Sound and system

```ini
sound = on                 # (user key) sound output (off: silent; it costs CPU time). On by default since Oct 16
sound_core = auto          # CPU core of the sound mixer: 2 (default on New 3DS: the render thread's core, which has room; core 0 runs the game logic), 0, 1
mips2c_native_off = off    # all: the mips2c versions instead of the native functions; or names
collide_frag_cache = on    # (AI-assisted) pc-collide-frag-may-hit? keeps each collide mesh's vertex box; off: scans the vertices every call
collide_native = on        # (AI-assisted) the drawable-tree walks of collide-with-box / fill-from-foreground-using-box in C++; off: the GOAL code
bones_native = on          # (AI-assisted) the skinning matrices (new-bones-mtx-calc-asm) in C++ / VFP assembly; off: the GOAL code (also off with mips2c_native_off = all or a list naming pc-new-bones-mtx-calc)
perf_sections = off        # on: per-section timing in the log (slower)
args = -boot -cbackend     # the game's arguments (-debug etc. like on PC)
listener = off             # on: Wi-Fi REPL (goalc can connect; 1 MB of RAM)
screenshots = 0            # N: save the top screen every N frames to data/log
pad_script =               # FILE (relative to sdmc:/3ds/jak1): scripted controller input for tests
debug_log = off            # on: debug lines in data/log/gk.log (thousands in a level load; with log_async = off every line is an SD card write)
log_async = on             # (AI-assisted) the log writer thread writes the logs about once a second (3ds_build.md "Screens and logs"); off: every thread writes its own lines, as before
io_checksum = off          # on: log the CRC-32 of the bytes the game read from each big file (debug; "(ok)" for checked .ogz files; with it, every .ogz file gets an [io] line)
io_cache = on              # off: no files kept open between loads, no read-ahead (big SD card reads), no cache of spool chunk 0 (the Oct 3 file reading)
c3l_read_buffer = 256      # (AI-assisted) KB per read from a level's .c3l file (16..1024; unbuffered, through two windows: index and vertex data; each chunk's indices go straight into its index buffer); 0: the old reads through a 64 KB stdio buffer, with all of a level's indices (1-3 MB) in one piece of the heap
```

## Text and button symbols

(AI-assisted.) The game's text was made for the PS2's 448 lines. On the 400x240 top screen its
smallest text (the small font, menu captions and subtitles at half the large font's size) had
capital letters only 5 to 7 pixels tall.

```ini
text_scale = 1             # all game text x this (0.5 .. 2); the user key text_size sets it
text_min_height = 9        # the smallest capital letter height, in pixels of the top screen (0 .. 16; 0 = the PS2's sizes). Text drawn smaller is drawn this big
button_art = off           # (off by default since Oct 12: the PS2 symbols) on: the PS2 button symbols in the text and the progress menu are drawn as the New 3DS buttons (A red, B yellow, X blue, Y green; by position: circle = A, cross = B, triangle = X, square = Y); off: the PS2 symbols
```

- The size applies to all of the game's text: menus, hints, the talk prompts, subtitles, the HUD
  and progress menu numbers (`print-game-text`, `print-game-subtitle`, `draw-string-xy`;
  `pc-3ds-text-scale` in `goal_src/jak1/engine/gfx/font-h.gc`). Text keeps its place: it grows
  from where the game puts it (to the right of left-aligned text, around centered text). Text
  that grows wraps within the width of its box and may take lines below it (down to just above
  the bottom of the screen), so nothing is left out.
- On the PC (`gk --ctr-gfx`, small memory build) the same values come from
  `OPENGOAL_TEXT_SCALE`, `OPENGOAL_TEXT_MIN_HEIGHT` and `OPENGOAL_BUTTON_ART`.
- The button symbols are drawn when the textures are read from the game's VRAM, the game files
  stay as they are (`game/graphics/ctr/CtrButtonArt.h`). Memory: the two font textures that hold
  the symbols are kept at 32 bits per texel instead of 16 (colors), +256 KB of texture memory for
  the large font (256x512) and +64 KB for the small one (128x256); the progress menu's four icons
  (64x64) were 32-bit already. Drawing the symbols takes a few ms once, when the font is first
  read (`[ctr] 3DS button symbols drawn into the ... font in N ms`).
- The 3DS build's wording (`game/assets/jak1/game_text_3ds.gp`, used by `goal_src/jak1/game.gp`
  when goalc runs with `OPENGOAL_SMALL_MEMORY=1`): the memory card and disc messages say SD card
  and game files, in the seven original languages. The options menus have no graphic options
  (window, resolution, V-sync, aspect ratio, MSAA, frame rate, the PS2 options: this file sets
  the renderer), no input options (keyboard, mouse, controller choice and binds; the camera
  options are in the game options) and no Discord option (`progress-pc.gc`, TARGET_3DS).

## Where it goes

- 3DS: `sdmc:/3ds/jak1/config.ini`.
- PC (`gk --ctr-gfx`): `<project>/config.ini`.

## Format

- One `key = value` per line. `#` or `;` starts a comment (also after a value).
- On / off: `1`, `true`, `on`, `yes` turn a setting on; anything else turns it off.
- Distances are in meters.
- `auto` (for `rgba4_as_rgba8` and `vram_textures`) keeps the automatic choice: what works in
  the emulator (Azahar) vs on a real 3DS. The game detects the emulator itself.

## Every renderer key with its default

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
shrub_scale = 0.8          # (AI-assisted) .c3l v14 files (current converter): each shrub is drawn up to its prototype's PS2 distance (the middle of the PS2's fade-out: the Snowy fort wall ropes 155 m, rope ties 90 m, grass 70 m, flowers 50 m; big bushes the PS2 shows as billboards 70 m) times this, times detail_scale. shrub_distance / shrub_distance_max only apply to older files. Log: [ctr] auto detail T: shrubs N m (v14 files: xK), ...
shrub_scale_max = 1        # (AI-assisted) the far end for v14 shrubs with auto_detail (1 = the PS2's distances)
                           # shrub_distance, tie_distance_scale and the far mode ties need .c3l files with chunk kinds (the current converter); with older files only detail_scale applies

# ---- what is drawn ----
merc = on                  # characters and objects
sprites = on               # world particles; the HUD is always drawn
max_sprites = 2176         # world particles per frame. (AI-assisted) 2176 (default since Oct 12) = the game's own capacity (1920 2d + 256 3d), nothing the PS2 draws is left out; 1000 was the 30 fps era cut
ocean = full               # the ocean: off, flat (= on, static grid), waves (+ waves near the camera), full (the PS2 ocean: the game builds its DMA, more CPU on both threads, see 3ds_build.md) (default full since Oct 16)
                           # (AI-assisted) waves: a 3 m grid out to ~45 m around the camera moves with the game's own wave heights (what boats and floating things ride), crests brighter, plus a second scrolling wave texture over all of the ocean. ~0.1 ms render thread CPU, a few hundred microseconds of GPU. Log line: [ctr] ocean waves. With depth16 (no stencil) the flat ocean beyond is lowered by the deepest trough
ocean_texture_rate = 2     # (AI-assisted) ocean = full: the wave texture is made every nth frame (1 = every frame, up to 8); it costs the render thread 1-1.7 ms each time. Log line: [ctr] ocean full (... texture ... in N% of frames)
ocean_near_alpha = 0.6     # (AI-assisted) ocean = full: the near ocean is at least this opaque (0..1). The PS2 / PC draw it at 0.5 within 12 m of the camera (1 from 24 m) and add a sky reflection pass the 3DS leaves out; 0.6 makes up for that pass's average brightening at the beach (0.5 = exactly the PS2's textured pass, see CtrOceanFull.h)
fps_cap = 30               # 30 (default): steady 30 fps (2 vblanks per frame); 60: up to 60
frame_pacing = steady      # steady (default): every frame stays on screen as long as the game time it covers (even motion, ~1 frame more latency); fast: shown as soon as the GPU is done (uneven at ~30 fps). Log line: [ctr] pacing. (AI-assisted) The latency (2-4 vblanks) goes up after a late frame and down after ~5 s without one and with at most 2 frames done just in time, but for ~30 s not below the latency a frame was late at
sky = on                   # the sky (time of day texture, clouds); off: cleared to the fog color
clouds = on                # (AI-assisted) the clouds over the sky; off: the sky without them (one added pass less over the sky's part of the screen)
sky_last = on              # (AI-assisted) the sky drawn after the opaque level and models instead of first: right before the first draw that needs what is behind it (anything blended, without depth write, stencil, a screen copy) or at the latest before the translucent buckets, with fragment depth 0 (the GPU's depth map: scale 0) and the depth test, so it only fills the pixels nothing opaque covered. Same picture (every blended pixel still blends over the sky). The sky's pixels behind the level are dropped before blending and color writes (the clouds read the framebuffer), and with the early depth test (early_depth = on / block32 / sky / sky32) before texturing. (default on since Oct 16; preset safe: off)
skip_clear = on            # (AI-assisted) frames whose opaque sky roof covers the whole screen (looking up; tested on a 25 x 15 grid a little beyond the edges, every roof vertex in front of the camera) clear only the depth buffer, not the color: every pixel's color is drawn over anyway. Same picture. Small: the color and depth fills run on the GPU's two fill units at the same time. The `[ctr] render ms/frame` log counts nothing for it; sky = off keeps the clear (default on since Oct 16; preset safe: off)
overlap = on               # on: the GPU starts drawing each part of a frame while the CPU builds the rest (default on since Oct 16; preset safe: off)
pipeline = on              # on: the CPU builds the next frame while the GPU draws this one (works with overlap) (default on since Oct 16)
frame_overlap2 = on        # (AI-assisted) with pipeline = on, the GPU's idle gap between two frames. off: the frame end waits for the GPU, then hands the frame's command list over with a cache flush the GSP does while the GPU waits (as before). clean: the list (130-260 KB) is cleaned from the CPU cache before that wait and handed over without the flush (same commands, same order). on: no wait at the frame end: the list is queued behind the frame the GPU is drawing; the next frame begin waits for the frame before that (whose buffers it reuses) instead. on needs frame_pacing = steady (else clean). The `[ctr] render ms/frame` line shows busy X (... active Y): active counts each frame from its command list on, busy also the CPU's build time of a pipelined frame. In gpu_profile = ab (on = the config's mode, clean if off; off = off) (default on since Oct 16; preset safe: clean)
distort = on               # sprite distorter: warp gate portals, heat haze (one screen copy in frames that have them) (default on since Oct 16)
distort_copy = rows        # (AI-assisted) rows: the distorter's screen copy covers only the screen columns its sprites read (8-pixel tile rows of the color buffer), the copy texture is made in VRAM if there is room for it then (512 KB, 256 KB with color16), and distort sprites under 8 pixels are left out (no copy in frames with only those). full: the whole screen into linear memory every frame with distort sprites, as before. Log line: [ctr] distorter screen copy: texture in VRAM | linear memory
envmap = on                # shine on power cells, precursor metal, ... (needs .c3l v9 files)
merc_tex_sort = on         # (AI-assisted) the opaque draws of a model bucket's models (not those with blend shapes or rewritten vertices) are queued and drawn draw by draw across the instances of a model (draw 0 of every crate, then draw 1, ...; each instance keeps its own order): one texture bind per draw instead of per draw and instance, but each draw then uploads its instance's bones (3 vectors per bone) instead of once per instance. Same picture. Implies envmap_batch for those models
envmap_batch = on          # (AI-assisted) the shine draws of a model bucket are queued and drawn together at its end (before a blended or no-depth-write model draw, if one comes first): one switch to the shine shader and back per bucket instead of per draw, and the model draws keep their uploaded bones. Same picture (the shine adds to the base it passes the depth test on). off: each right after its base, as before
shadows = full             # (AI-assisted) full (= on): the game's shadows of Jak, enemies and objects (the PS2's shadow volumes: the game runs shadow-execute-all, the renderer draws the volumes into the stencil buffer and darkens what they cover, like the PC port). blob: only a dark spot under Jak (the game's fake shadow, as before; no volumes, nothing on the game thread). off: none
                           # cost of full: the game thread computes each shadow-casting object's volume (shadow-execute); the render thread turns them into GPU triangles (~65-70 ARM instructions per triangle and per vertex: Jak, ~400 triangles and ~240 vertices, takes a few hundredths of a millisecond) and draws three passes for all of them together: the volumes' faces twice (stencil only, no color: their screen area), then one rectangle per object over its shadow. Log line (debug): [ctr] shadow: N objects ..., and "shadow" in [ctr] build ms/frame by renderer
shadow_casters = 0         # (AI-assisted) shadows = full: at most this many objects' shadows per frame, the ones that look biggest from the camera (their shadow mesh's bounding sphere over its distance; Jak's normally is one of them). 0: all of them, as before. The game thread still computes every volume. Log line (debug): [ctr] shadow: ... N over shadow_casters
                           # full needs the stencil buffer: with depth16 = on it is blob (the log's settings line says so). The z-pass method of the PS2 and PC: with the camera inside a shadow volume (rare: the volumes reach from the object down to just below the ground) part of that shadow is missing

# ---- textures ----
mipmaps = on               # off / on / trilinear: smaller texture versions for far surfaces (needs .c3l v8+)
mip_bias = 0               # (AI-assisted) LOD bias of textures with mip levels (GPUREG_TEXUNITn_LOD): +0.5 / +1 switch to the next smaller level sooner (blurrier up close, fewer texels read per pixel), negative sharper. -4..4; 0 (default) = none. In gpu_profile = ab (on = the config's bias, +1 if it is 0). pica_bench B6.rgba8.min4mip.bias1 vs B6.rgba8.min4mip prices +1
skin_rows_f24 = on         # (AI-assisted) merc bone matrices (3 rows per bone, up to 24 bones per draw) uploaded as float24 uniforms (3 words per vector, rounded to nearest) instead of float32 (4): 216 instead of 288 words per palette, a quarter fewer uniform words for the GPU's command processor. The vertex shader computes in float24 either way (relative precision 2^-17 after rounding: ~0.04 mm on a bone 5 m from the camera). In gpu_profile = ab; pica_bench B2.unif_f24 vs B2.unif_f32 prices a word.
skin_clip_f24 = on         # (AI-assisted, Oct 12) each merc model's clip matrix (12 instead of 16 words) and lights (21 instead of 28) as float24 uniforms too, like skin_rows_f24. Not bit-identical: rounded to nearest where the GPU converts float32 its own way, so a few edge pixels of models flip (Azahar, Sandover: ~90 of Jak's pixels). In gpu_profile = ab; decided by the same bench line as skin_rows_f24 (B2.unif_f24 vs B2.unif_f32: what a uniform word costs) (default on since Oct 16; preset safe: off)
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
sprite_max_size = 0        # world particles and glows at most this fraction of the screen height across (0 = no limit, the default since Oct 12, as the PS2; 0.5 was the 30 fps era cut)
sprite_min_alpha = 2       # (AI-assisted) default 2 since Oct 12 (6 was the 30 fps era cut): world particles bigger than ~20 pixels (half size) are left out below this alpha (the game's 0..128: 6 = under 5% opaque): faint mist and smoke that adds a few color steps but blends a big area. 2: the old rule (only sprites under 2, any size). Log line (debug): [ctr] sprite: ... faint big ones left out
sprite_alpha_discard = on  # (AI-assisted) blended world particles drop their fully transparent pixels (alpha test greater than 0; only the blend modes where alpha 0 adds nothing), so those pixels cost no framebuffer read and write. Same picture. off: no alpha test, as before
vis_culling = on           # the game's own visibility data hides level parts behind walls and hills (needs .c3l v10 files)
tie_lod = on               # (AI-assisted) c3l v17 tie levels of detail: the tie instance groups the converter gave a near version (geometry 0, the PS2's full detail: round warp gate ring, pipes, rock edges) are drawn with it while the camera is within the prototype's PS2 far-plane distance (45-90 m, beyond it the PS2 too draws geometry 3) of the group's sphere, times detail_scale, with 10% hysteresis; else with the far version (geometry 3, as before v17). auto_detail pulls the distances in to half while the GPU is over auto_detail_gpu_ms. off: the far versions only (the memory stays used). Old files: no effect. Log: [ctr] <level>: tie levels of detail: N groups, M near runs (load); [ctr] l0-tfrag: tie levels of detail: N groups near, A near + B far version tris drawn per level draw. In gpu_profile = ab (preset safe: off)
run_culling = on           # (AI-assisted) a level chunk (200 m cell) that crosses the edge of the view has its pieces tested on their own (the visibility runs: up to 8 tfragments or tie instances; chunks without them in pieces of ~96 triangles), so only what is on screen is sent to the GPU. Bounding spheres made at load (8 bytes per piece, ~30-50 KB per level). Log line: [ctr] l0-tfrag: run culling tested N runs, hid ... per level draw
neighbour_vis = on         # (AI-assisted) a level the camera isn't in (its visibility string is computed from the other level's data) has its tfrag hidden by the bits too, and its low resolution tfrag drawn where they say, as the PS2 and PC do. off: only its ties use the bits
fine_vis = on              # (AI-assisted) the levels' finer precomputed visibility: <level>.c3v next to the .c3l (made by ctr_vis_builder, see c3l_format.md "Fine visibility"). Per cell of the camera's space, the level pieces (visibility runs / 96-triangle pieces of static tfrag, tie, shrub) that can't be seen from anywhere in it are not drawn; merc is drawn as before. No .c3v, one made from another conversion of the level (checked at load), the camera outside every cell, far mode, a level loaded incompletely, draw_distance under the occluder range (400 m) or lod_distance outside 178..218 m: nothing more is hidden. Memory: the sets, read at load (KB in the [ctr] <level>: fine visibility log line). Log: [ctr] l0-tfrag: fine vis hid N chunks, M tris per level draw with a cell. off (config-safe.ini): the files aren't read
lowres_vista = on          # (AI-assisted) the levels' low resolution tfrag (the game's own far scenery: Snowy's forest floor below, the hills around Sandover) is drawn as vista: also when the camera is in the level, where no loaded level's detailed tfrag is (+50 m), out to vista_distance, with a haze towards the fog color from draw_distance on instead of the draw distance fog (solid fog color from draw_distance on). off: only for a level seen from a neighbouring one (far mode), within draw_distance. Log line: [ctr] l0-tfrag: vista N chunks M tris, low resolution chunks in near mode K
vista_distance = 3000      # (AI-assisted) meters; far scenery (lowres_vista, and the vista tier of .c3l v13 files) is drawn up to here, its haze reaching the fog color there. Levels with a vista tier (the converter's far scenery: the low resolution tfrag, the ties without a distance limit and, without low resolution tfrag, the coarse tfrag, simplified and merged into 1.5 km cells, one draw per texture) show it wherever their chunks are beyond their draw distance, and use one haze from fog_start * draw_distance to vista_distance for all their geometry (no solid fog ring at the draw distance)
vista_untextured = on      # (AI-assisted) levels converted with ctr_level_converter --vista-flat have an untextured copy of their vista tier (each texture's average color baked into the vertex colors, one draw per 1.5 km cell instead of one per texture): on draws that one, off the textured one (the other's meshes aren't made). Read when a level loads. Files without the copy: no effect. Log: [ctr] <level>: untextured vista tier (N of M vista chunks) (default on since Oct 16; preset safe: off)
fog_stage_skip = on        # (AI-assisted) level chunks entirely in front of where both fogs start (the game fog with fog = on, and the draw distance fog from fog_start * draw_distance) are drawn without the fog stage: texture combiner pass-through and texture unit 1 (the fog ramp) off for those draws. Log: [ctr] textures: ... N level draws without the fog stage (M texture unit 1 switches) (default on since Oct 16; preset safe: off)
fog_alpha = on             # (AI-assisted) level draws whose vertex alpha is 0x80 everywhere (~99.5% of them; found at load) take the fog amount in the vertex color's alpha (ctr_mesh_old_fa / ctr_mesh_opt_fa.v.pica) and mix in the fog color as a texture combiner constant: no fog ramp texture lookups and one vertex shader output (texcoord1) less. The other draws keep the fog ramp. Same picture (the amount differs from the ramp's by at most 0.8%). Log: [ctr] fog_alpha program: ready; [ctr] textures: ... N with the fog in the vertex alpha (default on since Oct 16; preset safe: off)
shader_opt = on            # (AI-assisted, Oct 12; on by default since the round-3 integration, the user's decision) the shorter vertex programs (platform/3ds/shaders/ctr_*_opt.v.pica): level meshes 10 instructions (the hardware default ctr_mesh_old: 21), 7 with fog = off (only the draw distance fog: one dph); merc 33 (53), merc shine 33 (56), the PS2 ocean 9 and 6 (14 and 12). How: the fog factors folded into two extra rows of the mesh matrix (dph with the position, made on the CPU per matrix: 2-6 more uniform words per matrix upload), texcoord1 (the fog amount) packed into texcoord0's output register (.zw: 3 output registers instead of 4), merc weights used unscaled (the clip matrix's x, y, z columns / 255 undo it) and the vertex color scale folded into the light colors, no clamp where the GPU saturates vertex colors anyway (fog_alpha). Pixel-identical to the default programs in Azahar (paused frames at Snowy start and fort, Sandover with fog on and off, fog_alpha, Fire Canyon, Jak/Daxter up close), except a few pixels of merc shine highlights (ctr_skin_env_opt uses the new ctr_skin_env's sphere map formula, without the old one's reciprocal: ~35 pixels on a shiny object at Snowy's fort). shader_opt wins when both are on (the opt programs aren't in the groups). Priced by pica_bench B3.mesh_opt / B3.skin_opt (cost against B3.mesh_new / skin_new and the fit's estimate for the old programs) and B3.o3p (packed output register: o3p ~ o3 means it pays). In gpu_profile = ab. Log: [ctr] shader_opt: ready (fog_alpha program ready, game fog off programs ready)
cmd_dedup = on             # (AI-assisted, Oct 12) register writes that would give a register the value it already has are left out of the command list: texture unit 0's type at a texture change (when the format is the same), and the merc shine pass's matrix and bones when the draw before was the same model's shine. Same registers, same picture. In gpu_profile = ab. The render line shows the words saved: "(N KB left out by cmd_dedup)"
cmd_dedup_draw = on        # (AI-assisted, Oct 12) with raw_cmds: the draws' PRIMITIVE_CONFIG mode and VERTEX_OFFSET writes are left out when the GPU has those values already (4 of each draw's ~30 template words, ~2-3k words per frame). Same registers(citro3d writes them before every draw, and its two dummy PRIMITIVE_CONFIG writes after the post-vertex cache clear stay). pica_bench B2 (us per draw against B2.nostate) prices a command word; gpu_profile = ab (default on since Oct 16; preset safe: off)
static_cmdlists = on       # (AI-assisted, gpu-rt) the level meshes' raw command words of each stretch of draws with one program are kept (in one 512 KB block of linear memory, taken at boot) once two frames in a row drew them alike, and copied in again while the draws, the GPU state before them and the settings stay the same: only the chunk matrices (and their fog rows) are written anew. The GPU gets the same words (compare: every copy is also drawn the per-frame way and the words, the trackers' state after it and the statistics compared; mismatches logged). Pays when the view holds still (Azahar: village1 idle 97% of the level draws copied, l1-tfrag build 4.36 -> 3.97 ms; Snowy fort 2.98 -> 2.86), nothing while turning (the culled draws change every frame). off / on / compare. In gpu_profile = ab. Log: [ctr] static_cmdlists on: per frame N stretches, M copied ... (once per stats window)
gpu_micro = on             # (AI-assisted, micro-gpu, Oct 17) the backend's micro-optimisations of the command stream (performance.md "Micro-optimisations of the GPU backend"): the vertex programs in one resident code image (shader_resident), texture unit 0's registers only where they change and GAS_DELTAZ_DEPTH only with a new depth function (both with cmd_dedup), the glows' procedural texture tables kept in the GPU, the atlas draws' scales written raw (static_cmdlists records those stretches again), steady pacing's fence words flushed with the vertices, near cache flush ranges in one request; (micro-mg, round 2) the glows drawn raw, a draw state that differs only in its texture only binds it, deferred cache flush ranges up to 64 KB apart in one request (performance.md "Round 2"). The same GPU state at every draw (cmd_report.py --kicks). Pad script "set gpu_micro_mask N" (a test): the changes as bits from the next frame (127 = round 1 only, 1023 = all). off: all of it as before (for the safe preset). Read by ctr_config_get (3DS only). Log: [ctr] gpu_micro: on
shader_resident = on       # (AI-assisted, micro-gpu) with gpu_micro: all vertex programs (both shader_opt sets, 18 programs) in one code image (351 of 512 instructions, operand descriptors deduplicated: 49 of 128), uploaded once: a program switch uploads no code (15-17 uploads per frame before). Programs with flow control or unknown instructions aren't merged (then the shbins as before). pica_bench B2.prog_sep - B2.prog_entry prices one upload. Log: [ctr] shader_resident: 18 programs from 18 shbins in one image, 351 instructions, 49 operand descriptors
gpu_micro_ident = off      # (AI-assisted, micro-gpu) test: each cmd_dump request (pad script "set cmd_dump on") dumps one frame with gpu_micro off and the frame after the next with it on, for cmd_report.py --kicks (pause the game first)
logic_micro = on           # (AI-assisted, micro-logic, Oct 17) the game thread's micro-optimised natives (performance.md "Game thread micro-optimisations"): render-ocean-quad without the clip passes for quads inside all planes, the collide cache mesh fill with per-vertex outcodes, a native ripple-apply-wave-table. Same results (OPENGOAL_MIPS2C_VERIFY host runs). off: the code as before (for the safe preset). Read by mips2c_native.cpp (3DS; host: OPENGOAL_LOGIC_MICRO=0). Not covered: goal_c_ops.h's lane-by-lane 128-bit ops are compiled into the game code. Log: logic_micro: on
ocean_tex_half = on        # (AI-assisted, gpu-rt) ocean = flat / waves: the ocean's shading texture (32x32 from the wave heights, with mip levels) is made every second frame; the heights are still read every frame (the waves' near grid moves with them). ocean = full has ocean_texture_rate. In gpu_profile = ab
cmd_dump = off             # (AI-assisted, Oct 12) diagnostic: a frame's whole GPU command list to data/log/cmd_NN.bin every 20 s (12 at most; the pad script's "set cmd_dump on" dumps the next frame); read with platform/3ds/tools/cmd_report.py --shbin-dir build-3ds cmd_*.bin (words per register group, per draw kind, writes that change nothing)
tex1_gating = on           # (AI-assisted) texture unit 1 (the fog ramp) on only for the draws whose texture combiner reads it (level draws with the ramp fog, the ocean's fog stage; clouds bind their own): merc, sprites, sky, HUD, shadows and fog_alpha draws switch it off by its enable bit, without the texture cache clear of a bind. Log: [ctr] textures: ... (M texture unit 1 switches) (default on since Oct 16; preset safe: off)
cull_merc = on             # (AI-assisted) back faces of merc draws the converter marked (.c3l from the current ctr_level_converter, MercDraw palette[30] kMercDrawCullBack: wound like their vertex normals, at most 2% of their triangles at an open edge, opaque: ~10-20% of a level's merc triangles: the yeti, lurkers, rams, ice cubes, crates, parts of Jak and Daxter) are culled by the GPU, with cull_ties' winding (flip). Not HUD models, not draws whose palette has a mirroring bone (determinant <= 0). Old files: no marks, no effect. Log (debug): [ctr] merc: ... N with back faces culled (M not: mirroring bone) (default on since Oct 16; preset safe: off)
cull_ties = on             # (AI-assisted) on: back faces of closed tie surfaces (rocks, houses, walls marked by the converter in .c3l v13 files: welded, consistently wound, at most 10% open edges or open only at the bottom, a clear inside) are culled by the GPU; flip: the same with the other winding (if faces go missing on hardware). Needs levels converted with the current ctr_level_converter; other draws are never culled
raw_cmds = on              # (AI-assisted) draws write their GPU commands themselves instead of through citro3d: less render thread CPU per draw, ~40% fewer command words. off: the citro3d path, for comparing
vcache_reuse = on          # (AI-assisted) with raw_cmds: a draw with the same vertices and matrices as the one before keeps the GPU's post-vertex cache (a few command words less) (default on since Oct 16; preset safe: off)
opaque_alpha = on          # (AI-assisted, Oct 12) .c3l v15 files: level draws whose alpha test (or alpha blending) can't change a pixel (texture alpha 1.0 in every texel of every mip level, vertex alpha >= 0x80: alpha is 1.0 everywhere; the converter marks them kDrawAlphaTestDropped / kDrawBlendDropped, c3l_format.md "Draw optimisations") are drawn without it: ~1100 alpha tested draws over all levels (village1 85, Snowy 71, village3 119; 50-75% of each level's alpha tested draws, a third to half of its triangles), no blended ones in the current levels. Blended ones would keep their place among the blended draws. off: the test / blending back on those draws (the converter's merges of such draws with opaque ones of the same texture stay). Same picture. In gpu_profile = ab. Older files: no effect. Put opaque_alpha = off in config-safe.ini
tex_cache_clear = uploads  # (AI-assisted) always: every texture bind clears the GPU's texture cache (TEXUNIT_CONFIG bit 16, as citro3d does; ~300 per frame). uploads: a bind that keeps a level pool texture and only changes its filter, wrap or mip settings skips the clear unless a pool was filled or moved to VRAM since the last one; every switch to another texture, the first bind of each frame and binds of all other textures (sky, clouds, eyes, HUD, the screen copy) clear. Until Oct 12 uploads also skipped it at switches between level pool textures: the oct12b hardware build (the first with uploads) showed red patches on the Volcanic Crater minecarts, whose body is one 8x8 texture (suspected: stale texels of the texture bound before it, in a level of red lava textures). Hardware only (Azahar has no texture cache): wrong texels show as stale patches of another texture; pica_bench B2.rebind vs B2.rebind_clr prices the clear. Log: [ctr] textures: ... N texture binds without the cache clear. In gpu_profile = ab (default uploads since Oct 16; preset safe: always)
merc_lod_scale = 1         # characters switch to their lower detail models at this fraction of the game's distances (0.05 .. 1). (AI-assisted) 1 = the PS2's (crates, orbs, lurkers: lod0 to 20 m, lod1 to 40 m from the camera, which is ~13 m behind Jak). Below ~0.7 the objects right next to Jak show their low detail models
merc_gen_lod = on          # (AI-assisted) models that come with only one detail level (the yeti, flut flut and its platforms, snow ball, spatula, ...) switch to a simplified copy the converter made (same vertices, ~30-65% of the triangles; ctr_level_converter --gen-lod) beyond max(25 m, 12.5 x the model's size) x merc_lod_scale, back 20% nearer (no flicker at the edge). Not Jak, Daxter or models with face animation / rewritten vertices. Needs .c3l files from the current converter (older ones: no effect). Log line (debug): [ctr] merc: ... N with the generated low LOD
impostors = off            # (AI-assisted, gpu-imp, Oct 12) far tie objects as cards. OFF by default: at Sentinel Beach's start a card covers the stone arch that stands among the rocks it was made from (host screenshots; the depth of a card cell is its nearest triangle's, and the arch is behind that but in front of the rest of the cell). How: a tie visibility group (1-8 instances the converter keeps together: trees, rocks, houses) whose nearest point is beyond impostor_distance, whose draws are all opaque, with no blended group in front of part of it, and that covers at most 128x128 pixels is drawn into a tile of a texture at screen resolution with that frame's camera (the same vertices, textures and states, cropped to its rectangle, no fog), then shown in its place as a 4x4 grid card (32 triangles) whose points lie on the capture camera's rays at the depth of the nearest of the group's triangles in the cells around them (CPU binning at the capture), drawn with the level mesh program (each point's own fog) and the alpha test on nearest texels at 1:1 (the geometry's hard edges). A card is drawn again when the camera moved enough for the parallax between the grid and each cell's farthest triangles to reach impostor_error pixels (rotation alone never invalidates it), its size on screen changed by 15%, or the group comes nearer than impostor_distance (then geometry again: within the error, no pop). Level colors are baked (no time of day change at run time). Memory: linear only (no VRAM): impostor_atlas KB RGBA8 + 64 KB D16 depth, ~0.4 KB heap per tile; the cards' vertices come from the frame's vertex stream (~1 KB per card). Host, on vs off (same views): Sentinel Beach start 58.7k -> 40.9k triangles (draws 667 -> 542), Sandover hut 101.8k -> 85.0k (draws 645 -> 696), Snowy pass to fort 77.8k -> 73.6k (922 -> 892). In gpu_profile = ab. Log (every stats_period): [ctr] impostors: N cards (T tris) per frame, captures, recaptures, atlas use, triangles not as cards by reason, render thread ms
impostor_distance = 120    # (AI-assisted) meters: groups whose nearest point is further than this may become cards (back to geometry 10% nearer)
impostor_error = 1         # (AI-assisted) pixels: a card is drawn again when a corner of its group would be this far from where the geometry puts it (0.25 .. 8)
impostor_budget = 6000     # (AI-assisted) triangles drawn into the impostor texture per frame at most (the groups and what may hide parts of them); the others stay geometry that frame
impostor_atlas = 512       # (AI-assisted) KB of the impostor texture (256 | 512 | 1024: 256 pixels high, 256 / 512 / 1024 wide). Fuller views keep more groups as geometry ("no room" in the log)
anim_lod = on              # (AI-assisted) actors that weren't drawn last frame and are more than 30 m (plus their size) from both the camera and Jak update their skeleton every 4th frame only. Never Jak, Daxter, cutscenes or actors with joint callbacks. off: every frame, like the PS2
anim_native = on           # (AI-assisted) the animation decompressor's per-joint work (decompress, normalize) in native code instead of the GOAL code; same results. off: the GOAL versions, to compare

# ---- hardware hang bisect (AI-assisted, 2026-10-03): off = the Oct 2 behaviour of each Oct 3 optimisation (new_shaders, shader_mesh, shader_skin removed Oct 16) ----
shader_basic = new         # new: the 7 instruction ctr_basic (verified on hardware). old: the Oct 2 one
defer_flush = on           # (hang bisect) the render thread's cache flushes held until the frame goes to the GPU. off: each right away, as on Oct 2
asm_tex = on               # texture tiling in 4x2 groups (LDM/STM for RGBA8), 64-bit PSMCT32 VRAM uploads, SIMD sky blend / cloud premultiply. off: the Oct 2 per texel / per byte loops
asm_mem = on               # ctr_mem.S memcpy / memmove / memset (LDM/STM). off: plain C byte / word loops (ctr_mem_c.c; also used before config.ini is read)
fast_clock = on            # steady_clock from the system tick without divides. off: clock_gettime(CLOCK_MONOTONIC), as libstdc++

# ---- measuring ----
gpu_profile = off          # every 2.5 s leaves out one group of renderers and logs what each costs the GPU (flickers); also logs the GPU's own counters once per level (`[ctr] gpu profile <level>: hw busy % ...`: which pipeline stage is busy, and memory traffic per frame; zeros in Azahar)
                           # (AI-assisted) gpu_profile = ab: A/B windows instead. Each switch of the table (run_culling, fine_vis, cull_ties, vista, fog_stage_skip, shadows (the volumes' draws), envmap, distort, distort_copy, shadow_casters, sprite_min_alpha, sprite_alpha_discard, envmap_batch, merc_tex_sort, merc_gen_lod, clouds, frame_overlap2, sky_last, skip_clear, early_depth, tex_cache_clear, mip_bias, skin_rows_f24, fog_alpha, tex1_gating, cull_merc, shader_opt, cmd_dedup, cmd_dedup_draw, skin_clip_f24, static_cmdlists, ocean_tex_half, impostors, opaque_alpha; CtrRenderer.cpp kAbSwitches) is turned on for ~1.5 s, then off for ~1.5 s (the next cycle off first), the others staying at their config.ini values; from 10 s after each level starts, for as long as the setting is there. Log, after each cycle (~45 s), per switch: [ctr] gpu ab <level> cycle N: <key> on X ms, off Y ms (on - off D ms; triangles, draws ...): the GPU busy time per frame (needs frame_pacing = steady). color16 and depth16 change the render target and can't be switched while running: price them with two sessions
stats_period = 15          # (AI-assisted) seconds between the render thread's statistics lines ([ctr] render ms/frame ... triangles, draws; culling); 5 for measuring fixed views
render_watchdog = off      # (AI-assisted) a watchdog thread (core 0, 16 KB stack) logs once per second for the first 60 s that the render thread is alive: `[ctr] render alive Ns: frame F, last stage S` ("(no frame in the last second)" when it stalled), to find where a boot hangs. Off by default since Oct 12 (it was always on: ~60 log lines per session)
gpu_counters = off         # (AI-assisted) the GPU's own counters over every stats window (15 s), with nothing left out of the picture: `[ctr] gpu profile window <level>: hw busy % ...` (which pipeline stage is busy, sampled over the window's first 10 s; memory traffic per frame over all of it). Read through the GSP service, no extra system calls or IO mappings. Off by default: not yet run on hardware
gpu_perflog = off          # (AI-assisted) the GSP's performance log once per stats window (`[ctr] gpu perflog: ...`): GPU time per frame of the command lists (p3d), display transfers (ppf) and memory fills (psc), raw and in ms, against the fence's busy time (bubble = busy - p3d - ppf - max psc); the vblank interval calibrates the disputed unit. One GSP request per window, no extra system calls
gpu_breakdown = off        # (AI-assisted, gpu-rt) profiling, needs pipeline = on: every 4th frame each part of the frame (clear, sky, level, merc, trans, shadow, ocean, sprite, distort copy, hud, transfer; split at the bucket renderers, the sky hook and the screen copy) goes to the GPU alone and is timed from its submission to its GSP event (P3D, PSC0/1, PPF). The GPU and the CPU take turns in those frames: the game runs slower while it is on (frames late, pacing latency up). One line per stats window: [ctr] gpu breakdown <level> over N frames ...: GPU ms per frame (lists per frame) per part, their sum, and the submit overhead of an empty list (included in each part's time). platform/3ds/tools/bench_report.py gk.log prints the table per level and a fit of fixed + per 1k triangles. Hardware only (Azahar's GPU takes no time)
pica_bench = off           # (AI-assisted) a GPU microbenchmark at boot, before the game starts (~20-60 s on hardware; progress on the bottom screen): per draw, per vertex, vertex cache, attribute, fill, setup and early depth tests, one `[bench] ...` line each in gk.log and data/log/bench.txt; `platform/3ds/tools/bench_report.py gk.log` prints the table (docs/3ds-port/performance.md "GPU timing in one session"). The tests that could hang the GPU run last; after a hang the last `start` line of bench.txt names the test. Turn it off again after one run
```

## Recommended settings

- **Real New 3DS (default):** the staged file (or none). If the frame rate is low, run once with
  `gpu_profile = on` and look for `[ctr] gpu profile: all X ms, level Y, merc Z, sprites ...` in
  the log: it shows which group costs the GPU the most. Then:
  - level expensive: lower `shrub_scale` (e.g. 0.6; older levels: `shrub_distance`, e.g. 35), `draw_distance` (e.g. 300) and
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
  `sprite_max_size = 0`, `shrub_scale = 1` (v14 levels; older ones: `shrub_distance = 0`,
  shrubs to 100 m: slower in village2 and rolling).
- **3DS hardware features:** all on by default except `early_depth`, `color16` and `depth16`. The log line `[ctr] textures: ...` shows the
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
