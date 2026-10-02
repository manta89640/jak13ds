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
sound_core = 0             # CPU core of the sound mixer: 0 (default on New 3DS), 1, 2 (the render thread's)
io_on_system_core = off    # on: the game's I/O threads on core 1 (the system core; experimental)
mips2c_native_off = off    # all: the mips2c versions instead of the native functions; or names
perf_sections = off        # on: per-section timing in the log (slower)
args = -boot -cbackend     # the game's arguments (-debug etc. like on PC)
listener = off             # on: Wi-Fi REPL (goalc can connect; 1 MB of RAM)
screenshots = 0            # N: save the top screen every N frames to data/log
pad_script =               # FILE (relative to sdmc:/3ds/jak1): scripted controller input for tests
debug_log = off            # on: debug lines in data/log/gk.log (slow: every line is an SD card write)
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
far_level_distance = 40    # meters; a level seen from a neighbouring one is drawn with only its low resolution version (0 = off)
detail_scale = 1           # multiplies the draw distance of small and medium objects and shrubs

# ---- what is drawn ----
merc = on                  # characters and objects
sprites = on               # world particles; the HUD is always drawn
max_sprites = 1000         # world particles per frame
ocean = on                 # the ocean
fps_cap = 30               # 30 (default): steady 30 fps (2 vblanks per frame); 60: up to 60
sky = on                   # the sky (time of day texture, clouds); off: cleared to the fog color
overlap = off              # on: the GPU starts drawing each part of a frame while the CPU builds the rest (experimental)
pipeline = off             # on: the CPU builds the next frame while the GPU draws this one (experimental; works with overlap)
distort = off              # sprite distorter: warp gate portals, heat haze (one screen copy in frames that have them)
envmap = on                # shine on power cells, precursor metal, ... (needs .c3l v9 files)

# ---- textures ----
mipmaps = on               # off / on / trilinear: smaller texture versions for far surfaces (needs .c3l v8+)
vram_textures = auto       # level textures in VRAM while there is room. auto: on (3DS and Azahar)
rgba4_as_rgba8 = auto      # 16-bit textures with alpha stored as 32-bit. auto: on in Azahar, off on the 3DS

# ---- 3DS hardware features (AI-assisted) ----
color16 = off              # on: 16-bit (RGB565) color buffer and top screen (half the memory traffic per pixel); faint blended layers like waterfall mist vanish and gradients band
early_depth = off          # the GPU's early depth test for opaque level / model draws (drawn nearest first): hidden pixels skip texturing. Off by default: on a New 3DS it leaves large blocks of the screen where the level never draws (Azahar ignores it)
compact_textures = on      # sprite, HUD and font textures in 8 / 16-bit formats (L8, LA8, RGB565, RGBA5551, RGBA4), and in 384 KB of reserved VRAM with vram_textures
proctex_glows = on         # glows (round gradient textures) from the procedural texture unit: no texels read
sprite_max_size = 0.5      # world particles and glows at most this fraction of the screen height across (0 = no limit)
vis_culling = on           # the game's own visibility data hides level parts behind walls and hills (needs .c3l v10 files)
merc_lod_scale = 0.5       # characters switch to their lower detail models at this fraction of the game's distances (0.05 .. 1)

# ---- measuring ----
gpu_profile = off          # every 2.5 s leaves out one group of renderers and logs what each costs the GPU (flickers)
```

## Recommended settings

- **Real New 3DS (default):** no file needed. If the frame rate is low, run once with
  `gpu_profile = on` and look for `[ctr] gpu profile: all X ms, level Y, merc Z, sprites ...` in
  the log: it shows which group costs the GPU the most. Then:
  - level expensive: lower `draw_distance` (e.g. 300) and `lod_distance` (e.g. 120), or
    `detail_scale = 0.7`;
  - sprites expensive: `max_sprites = 500`;
  - turn off `distort` or `envmap` to see what they cost.
  Keep `mipmaps = on` and `vram_textures = auto`: without them the GPU reads full size textures
  from main memory, which was the main cause of low frame rates on hardware.
- **Azahar (emulator):** no file needed; `auto` picks `rgba4_as_rgba8 = on` and
  `vram_textures = on` there too (Azahar draws RGBA4 textures as noise, VRAM textures are fine;
  with VRAM off the emulator needed ~4 MB more linear memory than the 3DS and levels didn't fit). Azahar doesn't
  model the GPU's speed, so frame rates there say nothing about the GPU on hardware.
- **Best looking:** `mipmaps = trilinear` (smoother far textures, a little slower),
  `sprite_max_size = 0`, `merc_lod_scale = 1`.
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
