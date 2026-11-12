# GPU cost model (PICA200, New 3DS)

Predicts the hardware GPU time of a frame from what the host renderer draws, so that LOD, culling
and residency designs can be priced before a console test. Azahar charges nothing for the GPU (its
"gpu busy" is its render thread), so the host software renderer (`gk --ctr-gfx`) supplies the
features and the hardware logs and `pica_bench` supply the prices.

Tool: `platform/3ds/tools/gpu_model.py` (Python 3, no modules). Calibration:
`platform/3ds/tools/gpu_model_calib.json`. Routes: `platform/3ds/tests/gpumodel_jak1.pad`,
`gpumodel_jak2.pad`.

## Where PICA200 time goes, and the features

Predicted ms = sum over features of count x cost, plus a per-frame constant. The GPU runs its
stages as a pipeline, so the sum is an approximation: a frame dominated by one stage hides part of
the others. Each feature is counted by the host per frame, per renderer bucket.

| group | feature (`[gpumodel]` key) | what it counts | hardware log has it |
|---|---|---|---|
| frame | `frame` | color + depth fill, display transfer, fence copy | constant |
| frame | `copy_rows` | distort screen copy, 8-pixel bands of 50 | no |
| commands | `draws` | draw calls (about 30 register writes and a kick) | yes |
| commands | `binds` | texture changes (texture cache clear) | yes |
| commands | `stchg` | draw state changes (blend, depth, combiner) | from now on (`gpumodel hw`) |
| commands | `progsw` | vertex program switches | from now on |
| commands | `unif` | uniform vectors uploaded (merc bones, env) | no |
| vertex | `vmiss` | vertices shaded: misses of a 16-entry post-vertex cache cleared at every draw | no |
| vertex | `vinstr` | vmiss x the program's instructions (shader_opt lengths) | from now on (3 x triangles x the bound program's length) |
| vertex | `vbufs` | vmiss x attribute buffers (the split color stream, morph streams) | no |
| triangles | `tri_in` | triangles submitted (assembly, clip and cull test) | yes |
| triangles | `tri_setup` | triangles that reach the rasterizer (not culled, on screen) | no |
| fill | `px` | fragments rasterized (depth is tested after texturing: early depth is off) | no |
| fill | `px_tex` | fragments reading texture unit 0 | no |
| fill | `px_u2` | fragments reading a second unit (alpha masks, env maps, ocean sky) | no |
| fill | `px_z` | fragments passing the depth test (color and depth write) | no |
| fill | `px_blend` | blended fragments passing (color buffer read) | no |
| fill | `tex_kb16` | KB of 8x8 texture tiles missed by a 16-tile LRU cache | no |

Not modelled: early depth (off on the console), fog in the vertex alpha vs the fog stage, VRAM vs
FCRAM texture placement (see calibration), the merc envmap pass's fill (the host does not raster
it; its vertices are counted), the HUD.

## Instrumentation

- Host, every `CTR_SOFT_STATS=N` frames: a `[gpumodel] win frame F frames N` block with one
  `[gpumodel] b <bucket> key=value ...` line per bucket (per frame values) and `[gpumodel] end`
  (`game/graphics/ctr/ctr_gpu_soft.cpp`, next to the `[softstats]` / `[tb]` lines).
- Host, `CTR_GPU_MODEL_DRAWS=file.csv`: one CSV line per draw (frame, bucket, program,
  instructions, buffers, indices, vertices shaded, triangles in / at setup, fragments, passing,
  second unit, texture size and format, blend, depth and alpha state, bind, state change). Needs
  `CTR_SOFT_STATS` too. `gpu_model.py draws file.csv [--frame F]` lists a frame's costliest draws.
- 3DS, every stats window, after `render ms/frame`: `[ctr] gpumodel hw: K vertex program kinstr,
  P program switches, S state changes (B blended) per frame`. The program length is the
  shader's main..endmain span (straight-line programs).

## What the hardware logs measure (and do not)

The `render ms/frame` line's `busy` and `active` are fence times, not GPU engine time. With
`overlap = on` (default) a frame's first part is handed to the GPU after about 200 draws; from then
on `active` counts the CPU build of the rest too whenever the GPU waits for it. So:

- In a window under 29.3 fps where the render thread waited for the GPU (`gpu-wait` > 0.3 ms) and
  `active` is the frame period, `active` is the GPU's frame time ("GPU-bound"). The tool fits these.
- Everywhere else `active` is only an upper bound. At 30 fps, Jak 1 windows show `active` = build
  time + 5-10 ms: the 27-28 ms "GPU" of the Nov 11 beach / jungle windows is mostly CPU build time.
- The Jak II city view (142.7k triangles, 1053 draws, 415 binds, 36.5 ms) is GPU-bound by that
  test, but its render thread's own work is 33 of the 37.4 ms period, so the 36.5 ms may still hold
  some wait for the CPU. Marked `*` in the tool's output.
- `gpu_perflog = on` logs the GPU engines' own time per window (P3D + PPF + PSC); the tool uses it
  as an exact target when present. No hardware log has it yet.
- Windows whose level textures were not in VRAM (`N level pools ... 0 in VRAM`: the Nov 10
  `vram_textures = off` A/B sessions) are a heavier regime (Jak II slums b at 74-89k triangles took
  35-38 ms) and are left out of the calibration.

From the hardware logs alone the costs cannot be separated: in the Nov 10-11 windows triangles,
draws and binds explain little (best fit, triangles + binds with the other windows as upper
bounds: cross-validated rms 5.0 ms; `gpu_model.py hwfit`), because the fill varies from view to
view and the logs don't see it.

## Costs from the console's bench (Nov 12, hw-nov12e): what `predict` uses

pica_bench ran on the console, and `gpu_perflog` gives the GPU's 3D time (P3D) per window. `predict`
now prices P3D with the bench's own costs (`gpu_model_calib.json` "p3d"), no fitting:

- every index is shaded: the hardware reuses no vertex across a list (B4: a 16-entry cache does not
  exist). Per index: max(48.6 ns, 17.1 + 14 per attribute buffer + 2.1 per instruction) (B3 / B5:
  the level mesh 52.1 ns, the skin program 100.4 ns; 48.6 ns is the setup stage's floor per index);
- a triangle costs nothing beyond its indices, drawn or culled (B7), so a strip's triangle costs one
  index and a list's three;
- 4.8 ns per fragment, 45 ns per KB of texture missed (B6), 0.46 us per draw, +0.19 per bind,
  +1.37 per program switch (B2). The clears and the display transfer are not P3D.

Check: jungle d0 on the host route is 249 ns per triangle; the console's jungle window is 242 (94k
triangles, 22.8 ms P3D). The sections below describe the model before the bench: its vertex cache,
its group scales and its absolute numbers are superseded.

## Calibration (status: Nov 11, no pica_bench run yet)

`gpu_model.py calibrate` gives each hardware window the host features of the same place (its loaded
levels -> a host view of the route; triangle-driven features scaled by the window's triangles,
draws and binds its own, fill from the heavier half of the place's host views), then fits one scale
per group (commands, vertex, triangles, fill) over the first-principles priors: least squares on the
GPU-bound windows, the rest as upper bounds, ridge towards 1.

Data: 133 windows of the routed places in the Nov 10-11 logs (VRAM textures), 14 of them GPU-bound
(Sandover / beach, Sandover hut, Haven slums a). Result:

| model | fit rms | leave one session out | leave one place out | leave one game out |
|---|---|---|---|---|
| priors alone | 2.4 ms | 2.4 | 2.4 | 2.4 |
| one common scale | 1.7 | 2.0 | 2.1 | 2.0 |
| group scales (used) | 1.2 | 1.4 | 1.4 | 1.1 |

18% of the non-bound windows are predicted more than 1 ms above their upper bound, so some views are
overpredicted. With 14 bound windows from three places the group scales are weakly determined;
treat the absolute error as about +-3 ms (the worst single bound window: -3.5 ms) and the split
between groups as a hypothesis for pica_bench to test.

Costs in use (ns, prior x scale):

| group | scale | costs |
|---|---|---|
| commands | 1.19 | draw 887, bind 1331, state change 266, program switch 1774, uniform vector 9 |
| vertex | 1.65 | vertex shaded 49, per program instruction 3.1, per extra buffer 12 |
| triangles | 1.16 | triangle in 17, triangle at setup 52 |
| fill | 0.25 | fragment 0.9, textured 0.9, second unit 0.9, written 0.5, blended 0.5, missed texture KB 960 |
| frame | (prior) | 2.0 ms per frame |

What can't be fitted without fill data: the fill group's scale rests on the between-place
differences of the host's fill only, and the per-fragment vs per-texture-miss split is the prior's.
The fill scale of 0.25 says the priors overpriced fill (mostly texture misses), not by how much each
part.

## Validation

Scales fitted without the game being predicted (`calibrate`'s validation table, Nov 11 windows):

| view class | hardware | predicted |
|---|---|---|
| Jak II slums a, 142.7k tris / 1053 draws / 415 binds (10 GPU-bound windows) | 36.6 ms (36.0-37.4) | 36.1 ms with all data; 37.0 ms fitted on Jak 1 alone (vertex 19.8, triangles 8.8, fill 3.4, commands 1.6, frame 2.0) |
| Jak 1 Sandover hut, GPU-bound (2 windows, 119-124k tris) | 32.2-32.7 ms | -1.0 ms mean error |
| Jak 1 beach, GPU-bound (1 window, 120k tris) | 32.3 ms | 28.8 (-3.5 ms) |
| Jak 1 jungle, Nov 11 (15 windows, 81k tris, not GPU-bound) | active 27.5 (upper bound) | 26.4 ms |
| Jak 1 beach, Nov 11 (25 windows, 69k tris, not GPU-bound) | active 26.4 (upper bound) | 18.8 ms |

The host route's own views (calibrated costs; `predict`, mean of each 30-frame window / worst):

| view | k tris | draws | binds | screens of fragments | GPU ms | worst |
|---|---|---|---|---|---|---|
| Sandover hut, 5 directions | 29-79 | 203-712 | 91-241 | 5.4-8.0 | 11.6-22.6 | 22.8 |
| beach start | 39-80 | 321-745 | 120-237 | 3.6-7.2 | 11.3-21.3 | 22.1 |
| jungle start | 66-116 | 522-1003 | 162-236 | 8.0-11.5 | 19.6-32.1 | 34.2 |
| Haven slums a | 29-97 | 476-1314 | 209-354 | 6.8-9.8 | 10.5-27.4 | 30.2 |
| Haven slums b | 47-98 | 673-1332 | 235-350 | 6.3-8.3 | 14.8-26.8 | 29.1 |
| Haven port | 17-64 | 214-809 | 141-316 | 5.7-7.7 | 9.3-22.5 | 23.1 |

In the worst views vertex work dominates (jungle d0: tie 13.0 ms of 34.2, merc 5.7; slums a d4:
tie 11.4 of 30.2, merc 9.0: vertices shaded and program instructions ~15 ms together), then
triangle setup, then texture misses. The host's slums views draw fewer triangles than the console's
heaviest window (107k vs 142.7k: another camera and position), so they are not the same view.

## How to run

```
# host build (small-memory C mode)
cmake -S . -B build-host-small -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_FLAGS=-DOPENGOAL_SMALL_MEMORY=1 -DCMAKE_CXX_FLAGS=-DOPENGOAL_SMALL_MEMORY=1
ninja -C build-host-small gk

# a route (Jak 1; Jak II: add -g jak2, --proj-path the Jak II mirror, gpumodel_jak2.pad).
# ~10 host fps: ~6 min per route. gk keeps running after "gpumodel route end": stop it then.
cd $PROJ && OPENGOAL_NO_SOUND=1 OPENGOAL_OCEAN_FULL=1 CTR_SOFT_STATS=30 \
  OPENGOAL_PAD_SCRIPT=$W/platform/3ds/tests/gpumodel_jak1.pad \
  $W/build-host-small/game/gk --proj-path $PROJ --config-path $SCRATCH/cfg --ctr-gfx \
  -- -boot -fakeiso -nosound -cbackend > j1.log 2>&1

# predicted hardware GPU ms per view, the worst window and what dominates it
python3 platform/3ds/tools/gpu_model.py predict j1.log --skip-title [--windows]

# recalibrate against hardware logs (the report folders; --since: the first folder used)
python3 platform/3ds/tools/gpu_model.py calibrate --hw reports/hw-* --host j1.log j2.log \
  --out platform/3ds/tools/gpu_model_calib.json
python3 platform/3ds/tools/gpu_model.py hwfit reports/hw-* --since hw-nov10b   # logs only

# per draw: CTR_GPU_MODEL_DRAWS=draws.csv on the run, then
python3 platform/3ds/tools/gpu_model.py draws draws.csv --frame F
```

To price a design: run the route on the branch with the design and on its base with the same pad,
then compare `predict` per view. Phase-B levers move `tri_in`, `vmiss`, `draws` and `binds` (well
priced by the fit's places) more than fill (weakly priced).

## The hardware bench boot (one boot)

Add to `sdmc:/3ds/jak2/config.ini` (or jak1's; any current build):

```
pica_bench = on
gpu_perflog = on
gpu_counters = on
overlap = off
```

Boot, wait for the bench (about 20-60 s before the title; the bottom screen names the test), then
play as usual for 5 minutes, the heavier the better (Haven slums, the port). Bring back
`data/log/gk.log` (and `sdmc:/3ds/jak1/data/log/bench.txt` if the game never starts: its last
`start` line names the test). Remove the four lines afterwards: the bench runs at every boot and
`overlap = off` costs CPU time.

What each line gives the model: `pica_bench` prices every feature (B1 frame, B2 draw / bind /
state / program / uniform, B3 vertex and per instruction, B4 triangle in, B5 extra buffer, B6
fragment / texture / second unit / blend / texture misses, B7 setup); `gpu_perflog` the GPU's
engine time per stats window (exact targets for `calibrate` instead of `active`); `overlap = off`
removes the partial submits, so `active` is GPU time too; `gpu_counters` the stage hotspots.

Then:

```
python3 platform/3ds/tools/gpu_model.py bench gk.log --out platform/3ds/tools/gpu_model_costs.json
python3 platform/3ds/tools/gpu_model.py calibrate --hw gk.log reports/hw-* --host j1.log j2.log \
  --costs platform/3ds/tools/gpu_model_costs.json --out platform/3ds/tools/gpu_model_calib.json
python3 platform/3ds/tools/gpu_model.py predict j1.log --costs platform/3ds/tools/gpu_model_costs.json
```

Group scales near 1 after that say the bench costs carry to game frames; far from 1, the pipeline
overlap between stages (the sum overcounts) or a missing feature.
