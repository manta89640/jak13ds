# render.ini: 3DS renderer settings

(AI-assisted.) The 3DS renderer reads `render.ini` once at startup. Every setting has a default,
so the file is optional: write only the lines you want to change. The log (`stdout.log`) prints
the settings in use: `[ctr] settings from ...: dist 500m lod 200m ...` (or `[ctr] no render.ini,
default settings: ...`), and warns about unknown keys.

## Where it goes

- 3DS: `sdmc:/3ds/jak1/render.ini` (next to the game's data; the parent folder also works).
- PC (`gk --ctr-gfx`): `<project>/render.ini`.

## Format

- One `key = value` per line. `#` or `;` starts a comment (also after a value).
- On / off: `1`, `true`, `on`, `yes` turn a setting on; anything else turns it off.
- Distances are in meters.
- `auto` (for `rgba4_as_rgba8` and `vram_textures`) keeps the automatic choice: what works in
  the emulator (Azahar) vs on a real 3DS. The game detects the emulator itself.

## A complete render.ini

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
sky = on                   # the sky (time of day texture, clouds); off: cleared to the fog color
overlap = off              # on: the GPU starts drawing each part of a frame while the CPU builds the rest (experimental)
distort = on               # sprite distorter: warp gate portals, heat haze (one screen copy in frames that have them)
envmap = on                # shine on power cells, precursor metal, ... (needs .c3l v9 files)

# ---- textures ----
mipmaps = on               # off / on / trilinear: smaller texture versions for far surfaces (needs .c3l v8+)
vram_textures = auto       # level textures in VRAM while there is room. auto: on on the 3DS, off in Azahar
rgba4_as_rgba8 = auto      # 16-bit textures with alpha stored as 32-bit. auto: on in Azahar, off on the 3DS

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
  `vram_textures = off` there (Azahar draws RGBA4 and VRAM textures as noise). Azahar doesn't
  model the GPU's speed, so frame rates there say nothing about the GPU on hardware.
- **Best looking:** `mipmaps = trilinear` (smoother far textures, a little slower).

## Things outside render.ini

- **Level files:** the features above need levels converted with the current converter
  (`ctr_level_converter --all out/jak1/fr3 <sd>/out/jak1/c3l`, see `c3l_format.md`). Files from
  an older converter still load, with a warning in the log: v7 has no mip levels and 16-bit
  textures (slow on hardware), v8 has no envmap shine. Shrubs and the base pass of shiny
  objects also need a current conversion.
- **Sound:** the flag file `sdmc:/3ds/jak1/sound` turns audio on; its content is the CPU core for
  the mixer (default 1). Put the mixer on a core without the render thread (see
  `3ds_build.md`, "Where the threads run").
- **Sky and the death effect** need the game code built with the current `goal_src` (they are
  partly GOAL changes for the 3DS build).
