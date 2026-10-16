# config.ini: the game's settings

(AI-assisted.) The 3DS game reads its settings from `sdmc:/3ds/jak1/config.ini` (next to
`gk.3dsx`) when it starts; on the PC (`gk --ctr-gfx`) from `<project>/config.ini`. Change a value
after the `=`, save the file, then restart the game. A missing line, or a missing file, means the
value shown here. `#` starts a comment.

`platform/3ds/tools/stage_sd.sh` writes this file (`platform/3ds/tools/config_template.ini`):

```ini
# Jak and Daxter (3DS) settings. Change a value after the "=", save, then restart the game.

# normal = the usual settings | safe = older, well-tested settings (try it if the game hangs or
# looks broken) | bench = measures the GPU at start (testing) | breakdown = GPU timing log (testing)
preset = normal

# game sound and music: on | off
sound = on

# distance fog like the PS2 (on), or a clearer view into the distance (off)
fog = on

# how far away scenery is drawn, in meters (bigger = further but slower; 0 = no limit)
draw_distance = 500

# shadows: full (every shadow, like the PS2) | blob (only a dark spot under Jak) | off
shadows = full

# particles (smoke, sparks, glows): full (like the PS2) | reduced (fewer and smaller, faster)
particles = full

# size of all game text: 1 = normal, 2 = twice as big, 0.5 = half
text_size = 1

# water: full (like the PS2) | waves (simpler, waves only near Jak) | flat (fastest) | off
ocean = full
```

## Presets

- **normal**: everything that is finished and works on the New 3DS (the default).
- **safe**: the Oct 11 behaviour, every renderer and loading improvement made after it switched
  back off. Use it if a new build hangs or draws something wrong, and say so in the bug report.
  Without a `particles` line it also draws fewer, smaller particles (1000, at most half the
  screen); `stage_sd.sh --preset safe` leaves that line out.
- **bench**: normal plus a GPU benchmark when the game starts (20-60 s, progress on the bottom
  screen) and GPU timing logs; send back `data/log/gk.log` and `data/log/bench.txt`.
- **breakdown**: normal plus a log of what each part of the frame costs the GPU (a little slower).

The user keys above always win over the preset. `stage_sd.sh --preset safe` (or `bench`,
`breakdown`) writes the line for you.

## Testing keys

Everything else (renderer switches, logging, diagnostics) is in
[config_dev.md](config_dev.md). Those keys still work when added to config.ini, but a normal game
doesn't need any of them. The log (`data/log/gk.log`) shows the preset and every line that
differs from it: `[ctr] settings from ... (preset normal; lines that differ from it: fog = off)`.
