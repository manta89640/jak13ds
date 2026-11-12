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
2. a user key that stands for it: `particles = full` or `reduced` (`particle_density = 0.7`);
   `text_size = X`
   (`text_scale = X`);
3. the preset (`preset = normal | full | fast | safe | bench | breakdown`; presets set only these keys, never
   the user keys):
   - `normal`: nothing (the defaults);
   - `safe`: the Oct 11 behaviour (3ds-sdcard-oct12d config-safe.ini; its particle limits are the
     user key `particles = reduced`, which `stage_sd.sh --preset safe` writes): `overlap`, `cull_merc`,
     `ocean_tex_half`, `tie_lod`, `merc_fog`, `streaming`, `vfp_vector`, `anim_lod_far` off,
     `c3l_read_buffer = 0`, `merc_lod_scale = 1`, `shadow_count = 0`;
   - `bench`: `pica_bench = on`, `gpu_perflog = on`, `gpu_counters = on`,
     `overlap = off`;
   - `breakdown`: `gpu_breakdown = on`;
4. the default (below; `ctr_cfg_defaults` for the keys read outside the renderer).

Removed on Oct 16 (with their code; ignored with a note in the log): `new_shaders` (the Oct 3
`ctr_mesh` hung the real GPU), `shader_mesh`, `shader_skin` (they only chose the removed
programs), `shader_groups` (shared shbins, never better than `shader_opt`), `cb_read_opaque`
(corrupted partly covered 8x8 tiles on hardware), `io_on_system_core` (boot hang).

(AI-assisted) Removed on Oct 18 (the switches only; the native code they chose is always on, it
gives the same results as the code it replaces; ignored with a note in the log): `logic_native`
(Jak II's native mips2c functions and new-bones-mtx-calc), `logic_micro` (the game thread's
micro-optimised natives), `bones_native`, `collide_native`, `collide_frag_cache`, `anim_native`.
For debugging, `mips2c_native_off` (3DS) and the host environment variables still pick the old
code: `OPENGOAL_MIPS2C_NATIVE=0`, `OPENGOAL_BONES_NATIVE=0`, `OPENGOAL_COLLIDE_NATIVE=0`,
`OPENGOAL_FRAG_CACHE=0`, `OPENGOAL_LOGIC_MICRO=0` (and the `*_VERIFY=1` comparisons).

(AI-assisted, levellod) Removed on Oct 29 with their code (ignored with a note in the log; the
Oct 29 optimisation audit, reports/opt-audit): `impostors`, `impostor_distance`, `impostor_error`,
`impostor_budget`, `impostor_atlas` (far tie groups as cards: 0.9-2.9 ms of render thread for a
smaller GPU saving, 576 KB of VRAM always, stale cards), `fine_vis`, `fine_vis_kb` (the .c3v
sidecars and `ctr_vis_builder`: none was shipped, 0-4% cut), `occ_cull`, `occ_cull_tris`,
`occ_cull_dist` (CPU occlusion culling: more render thread than it saved GPU), `vista_untextured`
(no shipped file had the untextured vista copy; the converter's `--vista-flat` went with it).

## Sound and system

```ini
sound = on                 # (user key) sound output (off: silent; it costs CPU time). On by default since Oct 16
sound_core = auto          # CPU core of the sound mixer: 1 (the default: the system core, 40% share on New 3DS; the mixer runs its voices without its lock, so sound calls don't wait while the system holds core 1 up), 2 (the render thread's core). Each falls back to the other; other values: auto
mips2c_native_off = off    # all: the mips2c versions instead of the native functions; or names (also pc-new-bones-mtx-calc). Debugging only
perf_sections = off        # on: per-section timing in the log (slower)
hitch_ms = 100             # game frames this long get a "[hitch]" line with their sections (perf_sections = on for those). Below 100 (38 for level load work: a frame that missed its 33 ms slot) every long frame gets its own line instead of one per second, and the render thread's frames of 26 ms or more get "[rhitch]" lines: level loads, begin, build, end, the time of day's share, the buckets over 1 ms. Host: OPENGOAL_HITCH_MS
stack_watch = off          # on: measure the GOAL process stacks (pattern fill, scan after every run; slower): "[stack]" lines with the deepest run per process type and thread every 30 s
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
```

- The size applies to all of the game's text: menus, hints, the talk prompts, subtitles, the HUD
  and progress menu numbers (`print-game-text`, `print-game-subtitle`, `draw-string-xy`;
  `pc-3ds-text-scale` in `goal_src/jak1/engine/gfx/font-h.gc`). Text keeps its place: it grows
  from where the game puts it (to the right of left-aligned text, around centered text). Text
  that grows wraps within the width of its box and may take lines below it (down to just above
  the bottom of the screen), so nothing is left out.
- On the PC (`gk --ctr-gfx`, small memory build) the same values come from
  `OPENGOAL_TEXT_SCALE` and `OPENGOAL_TEXT_MIN_HEIGHT`. The text shows the PS2 button symbols.
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
detail_scale = 1           # multiplies the draw distance of small and medium objects and shrubs (all of them, also with the two keys below). (AI-assisted, levellod) 0.1-4 (0 drew every tie LOD group far)
shrub_distance = 50        # (AI-assisted) meters; shrubs (grass, bushes, flowers) up to here (0 = the converter's 100 m). They are most of the GPU's work in village2, rolling, ogre and swamp (200-300k triangles within 100 m)
tie_distance_scale = 1.5   # 1.5 (Jak 1 and Jak II default): small ties drawn to 150 m, medium to 450 m (the converter's 100 / 300 m x this), the big ones without a limit; none: no limit, as the PS2 (geometry 3 beyond the far-plane). A tie chunk is drawn whole up to its limit and fades over the next tenth of it (with its vista copy fading in first where it has one): no hysteresis, nothing held. At least 0.05, as shrub_scale. Jak 1 was none until auto_detail's rings: the host title flight draws ~1.4k triangles more at none than at 1.5 and ~1.4k less at 1.0
                           # the fixed detail floor (CtrDetail.h, no key): beyond 40 m shrub fade bands at half their distance, tie chunks with a vista copy drawn by it from 150 m (both games; 120 m on the Jak II title screen)
shrub_scale = 0.8          # 0.8 (default, both games); 1: the PS2's distances. (AI-assisted) .c3l v14 files (current converter): each shrub chunk carries its prototype's PS2 distance class (the middle of the PS2's fade-out: the Snowy fort wall ropes 155 m, rope ties 90 m, grass 70 m, flowers 50 m; big bushes the PS2 shows as billboards 70 m). (AI-assisted, fable1, Oct 24) A shrub run (a visibility node: up to 8 instances) is drawn up to its own limit between 0.55 and 1.0 of 1.3 x that class distance x this x detail_scale (the PS2's near plane to its far plane, by a hash of the run: the shrubs thin out over the PS2's fade zone instead of a patch coming in at once; the PS2 fades its "trans" version there, the PC draws every shrub without a limit). 1 = the PS2's far plane. shrub_distance / shrub_distance_max only apply to older files. Log: [ctr] auto detail ... shrubs N m (v14 files: xK) Each run fades out (blend FIX with alpha (w - d) / (w - x), w = the limit, x = 0.55 w; drawn after the opaque level draws) instead of the hashed per-run cut.
                           # shrub_distance and tie_distance_scale need .c3l files with chunk kinds (the current converter); with older files only detail_scale applies

# ---- what is drawn ----
merc = on                  # characters and objects
sprites = on               # world particles; the HUD is always drawn
shadow_count = 0             # Jak II default 4; (AI-assisted, j2cuts, Nov 11) Jak II: only the N shadow casters nearest the camera get a shadow volume, and Jak always (foreground.gc foreground-shadow: the N-th nearest distance of the frame before + 1 m). Less shadow-execute work on the game thread and fewer stencil volumes. 0: no limit (the PS2's distances, shadows = full). 0 .. 64. Not yet run on a console
traffic_density = 0.5      # Jak II: the share of Haven City's civilians (citizen types 0-2) and civilian vehicles (bikes, cars: 11-16) the traffic manager keeps (traffic-manager.gc init-params: their want-counts x this, at least 2; targets follow as want - 1); guards, guard bikes, hellcats, metalheads and mission actors unchanged. 0.1 .. 1; 1 = the PS2's (preset full). Log: "city traffic: civilians and their vehicles x0.50"
particle_density = 1       # Jak II default 0.7 (Jak 1: 1). (AI-assisted) the share of particles launched (bursts, periodic emitters; 0.1 .. 1). (j1ocean) 1 = every particle, like the PS2 (default since Oct 25; 0.7 before, now only with particles = reduced, e.g. the safe preset). (AI-assisted, mercsprite) Both games (Jak II's launcher had no density cut)
shield_density = 1         # Citadel robot cage birth density (0.125..1); 1 is the original (240 live panels). The smaller of this and particle_density is used. Other shield-texture effects are unchanged.
ocean = full               # the ocean: on (full) or off. The PS2 ocean: the game builds its DMA (CtrOceanFull, see 3ds_build.md); off: the game makes only the wave heights
                           # (AI-assisted) waves: a 3 m grid out to ~45 m around the camera moves with the game's own wave heights (what boats and floating things ride), crests brighter, plus a second scrolling wave texture over all of the ocean. ~0.1 ms render thread CPU, a few hundred microseconds of GPU. Log line: [ctr] ocean waves. With depth16 (no stencil) the flat ocean beyond is lowered by the deepest trough
ocean_texture_rate = 2     # (AI-assisted) ocean = full: the wave texture is made every nth frame. Jak 1: only when there is no render texture for it (the GPU draws it every frame), the CPU makes it every nth frame; Jak II: the GPU's render texture too (1 = every frame, up to 8); it costs the render thread 1-1.7 ms each time. Log line: [ctr] ocean full (... texture ... in N% of frames)
fps_cap = 30               # 30 (default): steady 30 fps (2 vblanks per frame); 60: up to 60
frame_pacing = steady      # steady (default): every frame stays on screen as long as the game time it covers (even motion, ~1 frame more latency); fast: shown as soon as the GPU is done (uneven at ~30 fps). Log line: [ctr] pacing. The latency is fixed at 4 vblanks (two frames after the game hands a frame over: game thread, render thread and GPU each on their own frame)
sky = on                   # the sky (time of day texture, clouds); off: cleared to the fog color ((AI-assisted, mercsprite) and no CPU sky blend either)
clouds = on                # (AI-assisted) the clouds over the sky; off: the sky without them (one added pass less over the sky's part of the screen)
overlap = on               # on: the GPU starts drawing each part of a frame while the CPU builds the rest (default on since Oct 16; preset safe: off)
pipeline = on              # on: the CPU builds the next frame while the GPU draws this one (works with overlap) (default on since Oct 16)
distort = on               # sprite distorter: warp gate portals, heat haze (one screen copy in frames that have them) (default on since Oct 16)
envmap = on                # shine on power cells, precursor metal, ... (needs .c3l v9 files)
shadows = full             # full (= on): the game's shadows of Jak, enemies and objects (the PS2's shadow volumes: the game runs shadow-execute-all, the renderer draws the volumes into the stencil buffer and darkens what they cover, like the PC port); Jak II: the PS2's shadow distances (citizens within 40 m, vehicles 60 m, others their fade-dist), whatever pc-settings' ps2-shadow? says. off: none, and no volume work on the game thread. The removed blob is off, all is full
                           # cost of full: the game thread computes each shadow-casting object's volume (shadow-execute); the render thread turns them into GPU triangles (~65-70 ARM instructions per triangle and per vertex: Jak, ~400 triangles and ~240 vertices, takes a few hundredths of a millisecond) and draws three passes for all of them together: the volumes' faces twice (stencil only, no color: their screen area), then the faces turned away from the camera again, darkening where the stencil isn't 0 and the face lies behind what is drawn (so a count left over in front of a volume's far side, as when the GPU loses the second pass, darkens nothing). Faces that leave the screen (or cross the near / far plane) are cut to it on the CPU: the GPU drops triangles far off screen, which left the stencil counted up over a whole area (a solid dark quad). Log lines: [ctr] shadow self-test: ... -> pass|FAIL once after boot (five test frames through the same passes: the pixels darkened and the pixels a count was left on, with the receiver inside a test volume, behind it, behind it after a command list split, behind one that stays on screen; and what the GPU does with faces sent uncut far off screen), (debug) [ctr] shadow: N objects ..., and "shadow" in [ctr] build ms/frame by renderer
                           # The z-pass method of the PS2 and PC: with the camera inside a shadow volume (rare: the volumes reach from the object down to just below the ground) part of that shadow is missing

# ---- textures ----
mipmaps = on               # off / on (default: the nearest mip level, one texel fetch per pixel) / trilinear (two levels blended, as the PC; unfiltered draws use the top level only)
vram_textures = auto       # the level texture pools copied to VRAM once each, in the order they are loaded, while 1 MB of VRAM stays free; a pool keeps its place until its level is unloaded (no eviction). Linear memory keeps every pool; after the HOME Menu or sleep the copies are made again from it. The CPU never reads VRAM. auto: on
rgba4_as_rgba8 = auto      # 16-bit textures with alpha stored as 32-bit. auto: on in Azahar, off on the 3DS

# ---- 3DS hardware features (AI-assisted) ----
compact_textures = on      # sprite, HUD and font textures in 8 / 16-bit formats (L8, LA8, RGB565, RGBA5551, RGBA4), in linear memory
sprite_alpha_discard = on  # (AI-assisted) blended world particles drop their fully transparent pixels (alpha test greater than 0; only the blend modes where alpha 0 adds nothing), so those pixels cost no framebuffer read and write. Same picture. off: no alpha test, as before
vis_culling = on           # the game's own visibility data hides level parts behind walls and hills (needs .c3l v10 files)
tie_lod = on               # (AI-assisted) c3l v17 tie levels of detail: the tie instance groups the converter gave a near version (geometry 0, the PS2's full detail: round warp gate ring, pipes, rock edges) are drawn with it near the camera, else with the far version (geometry 3, as before v17). Files from the current converter (title-popin, Oct 17: each group's shape error in the file) switch by screen-space error: near while the far version's error would be more than tie_lod_px pixels on the top screen (this frame's projection), times detail_scale, 10% hysteresis; auto_detail moves it toward 4 only while the GPU is over its budget (auto_detail's relief). (AI-assisted, lodnear, Oct 18) and never nearer than the PS2: near at least within the prototype's PS2 far-plane (30-100 m) x detail_scale (+ the radius), where the PS2 has morphed to geometry 3; by error alone half of the groups went coarse nearer than that (village1 p10 12 m). Older v17 files: near within the prototype's PS2 far-plane distance (45-90 m) times detail_scale, pulled in to half by auto_detail (the PS2 morphs geometry 2 into 3 up to there, the 3DS switched 0 against 3: pops of 3 px median, up to 19 px in village1). off: the far versions only (the memory stays used). Log: [ctr] <level>: tie levels of detail: N groups, M near runs, switched by screen-space error | at the PS2 distances (load); [ctr] l0-tfrag: tie levels of detail: N groups near, A near + B far version tris drawn per level draw. In gpu_profile = ab (preset safe: off)
streaming = on             # (AI-assisted, streaming, Oct 17) c3l v19 levels: the near versions of every tie LOD group (near chunks) are read from the file around the camera on the loader thread (now and 2 s ahead at the camera's speed) into the stream arena and freed far away when the room is needed; a group whose near chunks aren't there yet is drawn with its far version, never a hole (streaming.md). off: the near chunks of the groups the level's v18 budget took are loaded with the level and the others never (v18's ties). Older files load whole either way. Log: [ctr] <level>: near chunks (v19): ... (load); [ctr] streaming: arena ... (every stats period). Preset safe: off
stream_kb = 4096           # (AI-assisted, streaming) the stream arena's first segment (KB of linear memory, 512-16384), taken at the first level that streams and kept. (AI-assisted, j2-mem) Jak II without this line: 2048, fixed (no growth; its near chunks with the tie geomorph: streaming.md "Jak II")
stream_reserve_kb = 3072   # (AI-assisted, streaming; 3 MB since the stream-arena branch, Oct 17, was 3.5) the linear heap's floor: the arena grows past stream_kb (one segment of 1-4 MB for the near chunks needed now and 2 s ahead + 1 MB beyond its capacity) while the heap keeps this + 256 KB free after the growth; the units not needed now given back under this + 128 KB, all of them under it, while a level loads unless 12 MB more stay free, and when a level load runs out of memory (1024-32768). streaming.md "Memory"
tie_lod_px = 4             # 4 (default; Jak 1 was 1): with the PS2 far-plane floor every group keeps the PS2's detail; 1 drew the wind ties (palms) near far beyond it, ~+15k triangles on the title (host) and ~16x the near data to stream. (AI-assisted, title-popin) pixels of shape error at which a tie group switches to its far version (0.25-16; files with shape errors). Host, Oct 17, with auto_detail fed the hardware GPU fit of each frame's triangles: the title's flight 109 -> 77 switches in view, largest 19.7 -> 4.0 px; 1 px would cost +30-70k triangles on the title and in Snowy (~+4-9 ms GPU), 2 px +10-45k, 4 px ~+0-10k. With the PS2's tie geomorph (c3l ExtMorph, levels converted with their .tiemorph): only the wind instances switch by it; every other group is drawn as the PS2 draws it (geometry 0 morphing toward 2 from the near-plane, geometry 2 morphing toward 3 from the mid-plane, geometry 3 beyond the far-plane, x detail_scale; no switch shows); auto_detail's relief doesn't move those distances (pulled in, geometry 3's own colors showed as the light changing on the rocks as Jak walked).
run_culling = on           # (AI-assisted) a level chunk (200 m cell) that crosses the edge of the view has its pieces tested on their own (the visibility runs: up to 8 tfragments or tie instances; chunks without them in pieces of ~96 triangles), so only what is on screen is sent to the GPU. Bounding spheres made at load (8 bytes per piece, ~30-50 KB per level). Log line: [ctr] l0-tfrag: run culling tested N runs, hid ... per level draw. (AI-assisted, levellod) The pieces and spheres are made whatever this says (the per-run shrub distances need them): off only stops the view test
neighbour_vis = on         # (AI-assisted) a level the camera isn't in (its visibility string is computed from the other level's data) has its tfrag hidden by the bits too, and its low resolution tfrag drawn where they say, as the PS2 and PC do. off: only its ties use the bits
lowres_vista = on          # (AI-assisted) the levels' low resolution tfrag (the game's own far scenery: Snowy's forest floor below, the hills around Sandover) is drawn as vista: also when the camera is in the level, where no loaded level's detailed tfrag is (+50 m), out to vista_distance, with the vista's haze (vista_distance) instead of the draw distance fog (solid fog color from draw_distance on). (AI-assisted, levellod) With neighbour_vis, a level with a vista tier draws a neighbour's low resolution chunks by this rule only, and not those whose runs the string hides (its vista copies have no visibility test: they lay over the detailed tfrag). off: only for a level seen from a neighbouring one (far mode), within draw_distance. Log line: [ctr] l0-tfrag: vista N chunks M tris, low resolution chunks in near mode K
vista_distance = 10000     # 10000 (default): the PS2's far clip plane (math-camera f); no haze then, as the PS2. (AI-assisted) meters; far scenery (lowres_vista, and the vista tier of .c3l v13 files) is drawn up to here, its haze reaching the fog color there. Levels with a vista tier (the converter's far scenery: the low resolution tfrag, the ties without a distance limit and, without low resolution tfrag, the coarse tfrag, simplified and merged into 1.5 km cells, one draw per texture) show it wherever their chunks are beyond their draw distance. (AI-assisted, levellod, Oct 29) The haze (CtrHaze.h) is only where the port's distances cut geometry: from fog_start x vista_distance (1.8 km) to vista_distance for the vista pass and for all of a vista-tier level's geometry, from fog_start x draw_distance to draw_distance for the rest, on top of the game's own fog (the larger of the two). Was: from 300 m for all of a vista-tier level (finalboss's far view 26-63% fog color at 1-2 km; the PC has none) and from draw_distance for the vista
ocean_tex_half = on        # no effect: it was the flat / waves ocean's, removed (kept for a hardware A/B of the shading rate; ocean_texture_rate is the PS2 ocean's)
cull_merc = on             # (AI-assisted) back faces of merc draws the converter marked (.c3l from the current ctr_level_converter, MercDraw palette[30] kMercDrawCullBack: wound like their vertex normals, at most 2% of their triangles at an open edge, opaque: ~10-20% of a level's merc triangles: the yeti, lurkers, rams, ice cubes, crates, parts of Jak and Daxter) are culled by the GPU, with cull_ties' winding (flip). Not HUD models, not draws whose palette has a mirroring bone (determinant <= 0). Old files: no marks, no effect. Log (debug): [ctr] merc: ... N with back faces culled (M not: mirroring bone) (default on since Oct 16; preset safe: off)
merc_fog = on              # (AI-assisted, far-fog, Oct 17) the game's fog on merc models (characters, objects, and the far views of other levels: the game draws those as med-res-level merc models, e.g. Sandover seen from Misty Island, which had no fog and stood out in full color against the fogged sea and sky). Per vertex like merc2.vert/merc2.frag: amount = (255 - clamp(hvdf.w - w, fog min, fog max)) / 255 with the merc camera's values, mixed towards the frame's fog color; per draw by its mode's fog bit (as Merc2); not the shine pass (emerc has none), not HUD models, (AI-assisted, mercsprite) not models the game draws without fog (disable-fog bit, as Merc2: Jak II's searchlights, city and stadium objects). (AI-assisted, mercsprite) Plus the port's distance haze, by the level meshes' rule (max of the two; CtrHaze.h ctr_haze_row, the vista rule while a level with a vista tier is drawn near; also with fog = off, as the level meshes): models no longer look pasted on hazed scenery (2 shader instructions, 1 uniform vector more). How: ctr_skin_opt computes the amount (3 instructions more: one dph with a fog row made from the clip matrix's w row, two clamps) into texcoord1 = texcoord0.zw (no extra output register); combiner stage 2 mixes the fog ramp (texture unit 1) in, as the ocean's. A model whose fog is ~0 everywhere (its bones' origins plus its size: under 0.5/255) gets no fog stage, so texture unit 1 stays off for it (tex1_gating). Needs shader_opt (the Oct 2 ctr_skin_old has no fog: no merc fog with shader_opt = off). In gpu_profile = ab. Log (debug): [ctr] merc: ... N draws with fog (preset safe: off)
cull_ties = on             # (AI-assisted) on: back faces of closed tie surfaces (rocks, houses, walls marked by the converter in .c3l v13 files: welded, consistently wound, at most 10% open edges or open only at the bottom, a clear inside) are culled by the GPU; flip: the same with the other winding (if faces go missing on hardware). Needs levels converted with the current ctr_level_converter; other draws are never culled
merc_lod_scale = 1            # Jak II default 0.58; characters switch to their lower detail models at this fraction of the game's distances (0.05 .. 1). (AI-assisted) 1 = the PS2's (crates, orbs, lurkers: lod0 to 20 m, lod1 to 40 m from the camera, which is ~13 m behind Jak). Below ~0.7 the objects right next to Jak show their low detail models. (AI-assisted, j2cuts, Nov 11) Jak II too (drawable.gc), except Jak, his children (Daxter, the gun), HUD models and cutscenes; the distance cull stays at the game's distance. Fewer vertices per model, and joint math for the lower models' joints only. 0.58 is the 240-line screen's share of Jak II's 416 lines (the same error on screen). Not yet run on a console in Jak II
merc_gen_lod = on          # models at their lowest game detail level (the yeti, flut flut and its platforms, snow ball, spatula, ...) switch to a simplified copy the converter made (same vertices; ctr_level_converter --gen-lod) while its simplification error (stored by the converter in the copy's model entry; older files: 2% of the model's extent, the converter's limit) projects to under 0.75 px on the top screen, back to full detail above 1 px (per instance, matched from frame to frame by model and place). Not Jak, Daxter, the pickups (c3l::merc_gen_lod_exempt) or models with face animation / rewritten vertices. Log line (debug): [ctr] merc: ... N with the generated low LOD
anim_lod = on              # (AI-assisted) actors that weren't drawn last frame and are more than 30 m (plus their size) from both the camera and Jak update their skeleton every 4th frame only. Never Jak, Daxter, cutscenes or actors with joint callbacks. off: every frame, like the PS2. (AI-assisted, j2cuts, Nov 11) Jak II too (process-drawable.gc execute-math-engine; not yet run on a console)
anim_lod_far = on          # Jak II default on (Jak 1: off); (AI-assisted, j2cuts, Nov 11) Jak II, with anim_lod: Haven City's citizens (civilians, guards, city metalheads) on screen and more than 40 m from the camera and Jak update their skeleton every 2nd frame, from 70 m every 3rd; between, their bones move with them (pose and facing hold, the place doesn't lag), also while anim_lod skips them off screen. Not after a LOD change or a jump of 1.5 m (a respawn). Not yet run on a console

# ---- hardware hang bisect (AI-assisted, 2026-10-03): off = the Oct 2 behaviour of each Oct 3 optimisation (new_shaders, shader_mesh, shader_skin removed Oct 16) ----
defer_flush = on           # (hang bisect) the render thread's cache flushes held until the frame goes to the GPU. off: each right away, as on Oct 2
asm_tex = on               # texture tiling in 4x2 groups (LDM/STM for RGBA8), 64-bit PSMCT32 VRAM uploads, SIMD sky blend / cloud premultiply. off: the Oct 2 per texel / per byte loops
asm_mem = on               # ctr_mem.S memcpy / memmove / memset (LDM/STM). off: plain C byte / word loops (ctr_mem_c.c; also used before config.ini is read)
fast_clock = on            # steady_clock from the system tick without divides. off: clock_gettime(CLOCK_MONOTONIC), as libstdc++

# ---- measuring ----
gpu_profile = off          # every 2.5 s leaves out one group of renderers and logs what each costs the GPU (flickers); also logs the GPU's own counters once per level (`[ctr] gpu profile <level>: hw busy % ...`: which pipeline stage is busy, and memory traffic per frame; zeros in Azahar)
stats_period = 15          # (AI-assisted) seconds between the render thread's statistics lines ([ctr] render ms/frame ... triangles, draws; culling); 5 for measuring fixed views
render_watchdog = off      # (AI-assisted) a watchdog thread (core 0, 16 KB stack) logs once per second for the first 60 s that the render thread is alive: `[ctr] render alive Ns: frame F, last stage S` ("(no frame in the last second)" when it stalled), to find where a boot hangs. Off by default since Oct 12 (it was always on: ~60 log lines per session)
texshare = on              # off: no shared texture groups between levels and 1 MB level texture pools (128/256 KB partial VRAM steps), as before texture sharing (656d653a14); for the hardware texture corruption bisect
gpu_counters = off         # (AI-assisted) the GPU's own counters over every stats window (15 s), with nothing left out of the picture: `[ctr] gpu profile window <level>: hw busy % ...` (which pipeline stage is busy, sampled over the window's first 10 s; memory traffic per frame over all of it). Read through the GSP service, no extra system calls or IO mappings. Off by default: not yet run on hardware
gpu_perflog = off          # (AI-assisted) the GSP's performance log once per stats window (`[ctr] gpu perflog: ...`): GPU time per frame of the command lists (p3d), display transfers (ppf) and memory fills (psc), raw and in ms, against the fence's busy time (bubble = busy - p3d - ppf - max psc); the vblank interval calibrates the disputed unit. One GSP request per window, no extra system calls
gpu_breakdown = off        # (AI-assisted, gpu-rt) profiling, needs pipeline = on: every 4th frame each part of the frame (clear, sky, level, merc, trans, shadow, ocean, sprite, distort copy, hud, transfer; split at the bucket renderers, the sky hook and the screen copy) goes to the GPU alone and is timed from its submission to its GSP event (P3D, PSC0/1, PPF). The GPU and the CPU take turns in those frames: the game runs slower while it is on (frames late). One line per stats window: [ctr] gpu breakdown <level> over N frames ...: GPU ms per frame (lists per frame) per part, their sum, and the submit overhead of an empty list (included in each part's time); then per part its geometry time (the part's list run a second time with only the top left pixel in the scissor: vertices, clipping and setup without fragments; each part's list begins by sending the GPU state it uses, so it can run again) with its triangles, draws and texture binds. platform/3ds/tools/bench_report.py gk.log prints the table per level, a fit of fixed + per 1k triangles, and per part geometry against fill (the part minus its geometry) with fits per 1k triangles, 100 draws and 100 texture binds. While it is on, the top left pixel gets the blended and stencil draws twice in the measured frames. Hardware only (Azahar's GPU takes no time)
pica_bench = off           # (AI-assisted) a GPU microbenchmark at boot, before the game starts (~20-60 s on hardware; progress on the bottom screen): per draw, per vertex, vertex cache, attribute, fill, setup and early depth tests, one `[bench] ...` line each in gk.log and data/log/bench.txt; `platform/3ds/tools/bench_report.py gk.log` prints the table (docs/3ds-port/performance.md "GPU timing in one session"). The tests that could hang the GPU run last; after a hang the last `start` line of bench.txt names the test. Turn it off again after one run, or use `once`: it runs at the first boot only (data/log/bench_once.txt marks it)
vfp_vector = on            # (AI-assisted, vfpvec, Nov 11) pc-new-bones-mtx-calc (Jak 1 and Jak II's bone matrices, game thread) computes the bind pose times the bone with the VFP11's short vectors (FPSCR LEN = 4 lanes: 16 instructions for 64): the same bits (qemu-arm: cpu_bench bones.mtx_scalar32 / bones.mtx_vector32). Off until a console's cpu_bench shows it faster and Azahar draws it right (dynarmic's vector mode)
```

## Recommended settings

- **Real New 3DS (default):** the staged file (or none). If the frame rate is low, run once with
  `gpu_profile = on` and look for `[ctr] gpu profile: all X ms, level Y, merc Z, sprites ...` in
  the log: it shows which group costs the GPU the most. Then:
  - level expensive: lower `shrub_scale` (e.g. 0.6; older levels: `shrub_distance`, e.g. 35), `draw_distance` (e.g. 300),
    or `detail_scale = 0.7`;
  - sprites expensive: `particles = reduced`;
  - turn off `distort` or `envmap` to see what they cost.
  Keep `mipmaps = on` and `vram_textures = auto`: without them the GPU reads full size textures
  from main memory, which was the main cause of low frame rates on hardware.
- **Azahar (emulator):** no file needed; `auto` picks `rgba4_as_rgba8 = on` and
  `vram_textures = on` there too (Azahar draws RGBA4 textures as noise, VRAM textures are fine;
  with VRAM off the emulator needed ~4 MB more linear memory than the 3DS and levels didn't fit). Azahar doesn't
  model the GPU's speed, so frame rates there say nothing about the GPU on hardware.
- **Best looking:** `mipmaps = trilinear` (smoother far textures, a little slower),
  `shrub_scale = 1` (v14 levels; older ones: `shrub_distance = 0`,
  shrubs to 100 m: slower in village2 and rolling).
- **3DS hardware features:** all on by default. The log line `[ctr] textures: ...` shows the
  compact textures (and how many are in the reserved VRAM); `[ctr] l0-tfrag: visibility hid ...`
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
