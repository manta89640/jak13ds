# Jak II game logic in Haven City: profile and the N3DS plan (AI-assisted, Oct 18 2026)

(AI-assisted.) Branch `jak2-prof` (from jak2-3ds f9ab7e1189). Mac only (M3 Pro, C mode, null
renderer, silent). These N3DS numbers are estimates calibrated on Jak 1. Nothing here ran on a 3DS.

## How it was measured

- **Frames back to back.** At 30 Hz the game thread sleeps ~95% of the time and macOS moves it to
  the efficiency cores: the same city scene gave 0.83 ms (jak2_status.md) and 2.6-3.0 ms of
  "logic" in two runs. Use no-sync windows instead: `OPENGOAL_NULL_GFX_NO_SYNC=<file>` runs frames
  back to back while the file exists (time ratio 1, performance core). Jak 2's kernel loop now
  skips its 1 ms sleep in that mode, as Jak 1's already did (`game/kernel/jak2/kboot.cpp`). Then
  ms/frame = 1000 / fps of the perf lines.
- **Sampler.** `platform/3ds/tools/sample_run.sh` runs a pad and starts macOS `sample` (1 ms) in
  each `perf-mark M` ... `perf-mark M-end` window. `platform/3ds/tools/sample_profile.py fold`
  maps the C modules' `gc_fN` symbols to GOAL names (the `// name` comment above each definition
  in `out/<game>/cmod/<hash>.c`). `tree` gives the inclusive call tree, `est` the categories,
  kinds and top functions with an N3DS estimate, `win` the ms/frame per window. A category is
  decided by the deepest frame that matches one, so a vector or string helper counts for its
  caller. Process code runs on its own stacks, so ~40% of the stacks lose their callers above
  the stack switch. Self time and the categories are still right, but the inclusive tree's
  top level is incomplete.
- **Pads** (`platform/3ds/tests`): `jak2_city_prof.pad` (slums, 30000 frames standing and 6000
  walking), `jak2_route_prof.pad` (slums, port, market, main town, industrial, farms, sewer: 40000
  frames standing and 40000 turning the camera at each stop), `jak1_calib_prof.pad` (Jak 1 spots
  for the calibration). Sampling slows the game ~15-20%, but it doesn't change the shares.

```sh
T=platform/3ds/tools; WT=<worktree>
$T/sample_run.sh jak2 <jak2 C mirror> $WT/build/game/gk <out> $PWD/platform/3ds/tests/jak2_route_prof.pad \
  10 slums,slumslook,port,portlook,market,marketlook,main,mainlook,indus,induslook,farms,farmslook,sewer,sewerlook
$T/sample_profile.py win <out>/out.log
$T/sample_profile.py fold <out>/sample-slums.txt "Thread_<id>: EE" <mirror>/out/jak2/cmod > slums.folded
$T/sample_profile.py est slums.folded 0.695 85 85 200 30     # Mac ms/frame, then R_goal R_c R_m2c
```

## Mac numbers (no-sync, median ms/frame)

| scene | Mac ms | processes | mips2c share | N3DS est. as is | N3DS est., all mips2c native |
|---|---|---|---|---|---|
| slums stand / camera turn | 0.695 / 0.665 | 291 | 46% | 96 / 91 | 59 / 57 |
| port | 0.439 / 0.490 | 289 | 31% | 53 / 59 | 37 / 42 |
| market (ctymarkb) | 0.675 / 0.660 | 299 | 41% | 89 / 88 | 57 / 56 |
| main town (ctygenb) | 0.647 / 0.569 | 374 | 38% | 83 / 73 | 55 / 48 |
| industrial (ctyinda) | 0.615 / 0.609 | 312 | 38% | 79 / 80 | 52 / 52 |
| farms (ctyfarma) | 0.468 / 0.484 | 346 | 40% | 61 / 64 | 40 / 41 |
| sewer (not city) | 0.222 / 0.222 | 85 | 36% | 28 / 28 | 19 / 19 |
| city average (12 windows) | 0.585 | | 40% | 76 | 50 |

Jak 1 in the same mode (natives on): jungle-start 0.121, misty-silo 0.140, robocave 0.145,
village2-dock 0.157, snow-start 0.099 ms. So the city is ~4.5x Jak 1, as the 30 Hz estimate said.
The time doesn't follow the process count: main town has the most processes and costs less than
the slums.

## Calibration (Mac -> N3DS)

- Jak 1, the same build of the logic on both sides: Azahar logic after micro-logic (performance.md
  table: jungle 10.37, misty 11.45, village2 12.67, robocave 12.48 ms) over the Mac numbers above
  gives **81-86x**. Hardware Snowy (`reports/logs-oct12/gk.log`, median logic 9.0 ms at 30 fps
  over 91 lines) over 0.099 gives 91x. On the 3DS, CPU logic runs at about Azahar speed
  (3ds-hardware-debugging). Swamp was left out: its Mac window was unstable (0.22-0.55 ms).
  The Azahar/hardware runs used time ratio 2 and the Mac ratio 1. That is in the ratio, and it
  matters less in the city (target 0.6%), so the city estimates are slightly high.
- That 85x covers Jak 1's mix: GOAL C 50-64%, natives (mips2c-replacing C++) 19-31%, glue/libc
  10-19%. It is used for Jak 2's GOAL code, C and future natives (`R_goal = R_c = 85`).
- **mips2c costs more on the 3DS than on the Mac.** Jak 1 with `OPENGOAL_MIPS2C_NATIVE=0` on the
  Mac: the mips2c versions take 2.3-2.8x the natives' time (jungle 0.037 -> 0.094 ms). On ARM11,
  Jak 1's natives were 4-10x fewer instructions than mips2c (3ds_build.md: sp-process-block-2d
  4.2x, cspace<- 8.4x, collide-probe 8-10x). So on the 3DS, mips2c is roughly 1.7-3.4x worse
  relative to the Mac than native code: **R_m2c = 150-270, 200 used**. That gives the ranges below
  (city average 65-93 ms as is; slums 80-119 ms).

## Where the time goes (city average, all six districts, standing + camera turn)

N3DS ms at R = 85 / 85 / 200 (mips2c as now). "m2c" = share of the category's Mac time in mips2c.

| category | Mac us | % | m2c | N3DS ms | N3DS, natives |
|---|---|---|---|---|---|
| particles (update, launch, motion blur) | 93.5 | 16.0 | 55% | 13.9 | 8.0 |
| shadow volumes (CPU, shadow-execute) | 58.2 | 10.0 | 96% | 11.4 | 4.9 |
| joints / bones / anim decompression | 91.9 | 15.7 | 27% | 10.6 | 7.8 |
| sky (sky-work clouds + quads, DMA) | 49.2 | 8.4 | 71% | 8.2 | 4.2 |
| collision (collide-hash fills, cache) | 35.6 | 6.1 | 62% | 5.6 | 3.0 |
| spatial hash (actor / avoidance queries) | 28.6 | 4.9 | 87% | 5.3 | 2.4 |
| kernel / process dispatch, suspend-resume | 43.4 | 7.4 | 0 | 3.7 | 3.7 |
| foreground DMA (merc requests, dma-add-process-drawable) | 38.7 | 6.6 | 4% | 3.5 | 3.3 |
| engine misc (res-lump lookups, entity-by-name, level, settings) | 36.5 | 6.2 | 0 | 3.1 | 3.1 |
| PC-port extras (discord RPC with std::regex, subtitle scene lookup, string-upcase) | 31.2 | 5.3 | 0 | 2.7 | 2.7 |
| ocean | 9.3 | 1.6 | 65% | 1.5 | 0.8 |
| text / HUD (draw-string-asm) | 8.9 | 1.5 | 49% | 1.3 | 0.8 |
| nav-mesh / nav-control | 8.6 | 1.5 | 24% | 1.0 | 0.7 |
| time of day / lights | 6.3 | 1.1 | 59% | 1.0 | 0.5 |
| traffic manager / engine | 9.9 | 1.7 | 0 | 0.8 | 0.8 |
| vehicles AI / physics | 8.3 | 1.4 | 0 | 0.7 | 0.7 |
| citizens / guards AI | 7.3 | 1.2 | 0 | 0.6 | 0.6 |
| background, camera, target, sound, enemy framework | 17.1 | 2.9 | 0 | 1.5 | 1.5 |
| **total** | **585** | | **40%** | **76** | **50** |

**The traffic AI itself is small.** Citizens, guards, vehicles, traffic engine and nav add up to
~3 ms of N3DS time. The traffic costs through what every one of its ~150 processes drags along:
joints, a shadow volume, merc DMA, collision and spatial-hash queries, and the kernel's per-process
work. Slums frame by phase (stand window): main-draw-hook 31% (shadows, bones, sky, foreground DMA,
nav-mesh update, actors-update), process-particles 25%, part-tracker spawning 6%,
execute-math-engine (joints) 8%, the process tree (trans/code/post) 20%, level-update/settings 3%,
pc-settings update 2%.

### Top 30 functions (city average, self time; glue charged to the nearest named frame)

| # | function | kind | Mac % | N3DS ms (as is) |
|---|---|---|---|---|
| 1 | sp-process-block-2d | mips2c | 5.9 | 6.9 |
| 2 | cspace<-parented-transformq-joint! | mips2c | 4.2 | 4.9 |
| 3 | shadow-find-facing-single-tris | mips2c | 3.2 | 3.7 |
| 4 | new-bones-mtx-calc-asm (GOAL in Jak 2) | GOAL | 3.3 | 1.7 |
| 5 | shadow-find-single-edges | mips2c | 1.3 | 1.5 |
| 6 | clip-polygon-against-positive-hyperplane (sky) | mips2c | 1.3 | 1.5 |
| 7 | (method 32 sky-work) | mips2c | 1.2 | 1.4 |
| 8 | sparticle-motion-blur | mips2c | 1.1 | 1.3 |
| 9 | process dispatch lambda (gkernel anon-function-5) | GOAL | 2.6 | 1.3 |
| 10 | sp-launch-particles-var | mips2c | 1.1 | 1.3 |
| 11 | (method 28 sphere-hash) | mips2c | 1.1 | 1.3 |
| 12 | shadow-execute | mips2c | 1.1 | 1.3 |
| 13 | shadow-calc-dual-verts | mips2c | 1.0 | 1.2 |
| 14 | render-sky-quad | mips2c | 1.0 | 1.2 |
| 15 | fill-bg-using-box-new (collide-hash) | mips2c | 1.0 | 1.2 |
| 16 | shadow-xform-verts | mips2c | 1.0 | 1.2 |
| 17 | sp-process-block (incl. 3.4 ms of memmove block copies in the slums) | GOAL + C | 2.3 | 1.1 |
| 18 | (method 22 grid-hash) | mips2c | 1.0 | 1.1 |
| 19 | (method 11 collide-hash) | mips2c | 0.9 | 1.1 |
| 20 | (method 33 sky-work) | mips2c | 0.9 | 1.1 |
| 21 | mem-copy! | GOAL | 2.0 | 1.0 |
| 22 | clip-polygon-against-negative-hyperplane (sky) | mips2c | 0.9 | 1.0 |
| 23 | (method do-joint-math draw-control) | GOAL | 2.0 | 1.0 |
| 24 | (method 20 grid-hash) | mips2c | 0.8 | 1.0 |
| 25 | (method lookup-tag-idx res-lump) | GOAL | 1.8 | 0.9 |
| 26 | draw-string-asm | mips2c | 0.6 | 0.7 |
| 27 | draw-large-polygon (sky) | mips2c | 0.6 | 0.7 |
| 28 | shadow-scissor-edges | mips2c | 0.6 | 0.7 |
| 29 | pc-merc-draw-request | GOAL | 1.3 | 0.7 |
| 30 | string-upcase (discord / subtitles) | GOAL | 1.3 | 0.6 |

Next after that: create-interpolated-joint-animation-frame, sp-init-fields!, atan, decomp-frame /
decomp-fixed, (method 30 sphere-hash), add-light-sphere-to-light-group, (method 9
collide-cache-prim), (method 37 / 39 spatial-hash), thread-suspend/resume.

## Plan for <= 15 ms (N3DS game thread, city)

Cumulative estimate. city avg / slums (the worst district) / port (the lightest) in N3DS ms, R as
above, plus ~1 ms of IOP/IO threads on core 0 (Jak 1: 0.6-1.6 ms).

| step | lever (unmodified logic) | city | slums | port |
|---|---|---|---|---|
| 0 | baseline: mips2c as now | 77 | 97 | 54 |
| 1 | natives for the hot mips2c functions (Jak 1-style C++, direct calls) | 51 | 60 | 38 |
| 2 | VFP-tuned versions of the top natives (another -30%) | 45 | 52 | 35 |
| 3 | particles: process the blocks in place, without the scratchpad memmove | 44 | 51 | 35 |
| 4 | PC-port extras off on the 3DS (discord RPC, per-frame subtitle scene lookup) | 42 | 49 | 33 |
| 5 | cache per-frame lookups (cam-master-set-entity -> entity-by-name, res-lump tags) | 41 | 47 | 31 |
| 6 | C-backend codegen round (-12% GOAL code) | 38 | 44 | 29 |
| 7 | cheaper process dispatch / suspend-resume | 37 | 43 | 28 |
| 8 | IOP / overlord threads to core 2 | 36 | 42 | 27 |
| 9 | job on core 2: shadow volumes + sky DMA (6.7 ms moved) | 29 | 33 | 23 |
| 10 | job: particle block update (3.8 ms moved) | 25 | 25 | 21 |
| 11 | job: anim decompression + bones (3.3 ms moved) | **22** | **22** | **19** |

So the unmodified-logic levers get to ~20-22 ms on core 0, not 15 ms (range at R_m2c 150-270 and
+-15% on the 85x: ~18-26 ms). What is left on core 0 after step 11 (city average): joints
3.3, foreground DMA 2.9, collision 2.3, kernel 2.1, spatial hash 1.8, particles 1.6, engine misc
1.4, ocean 0.8, traffic 0.7, HUD text 0.7, nav 0.7, vehicles 0.6. The last ~7 ms needs one or
more of the presentation or gameplay options below.

### 1. Natives (largest single lever, ~26 ms)

None of Jak 2's 28 mips2c files has a native version yet. By N3DS ms (as is -> native):
- **Port from Jak 1** (same or close algorithm, `game/mips2c/jak1_functions/native_*.cpp`):
  sp-process-block-2d / 3d (6.9+0.7 -> ~3.5), sp-launch-particles-var, particle-adgif,
  cspace<-parented-transformq-joint! (4.9 -> 2.1; also called directly, `native_as_goal`),
  shadow-execute and its 14 helpers (11.4 -> 4.9; Jak 1 `native_shadow.cpp`), collide-cache-prim
  9/10, collide-do-primitives, moving-sphere-triangle-intersect, the collide-mesh methods.
  Jak 1's bones native (`pc_new_bones_mtx_calc`, 1.6x) applies to Jak 2's GOAL
  new-bones-mtx-calc-asm (1.7 ms).
- **New in Jak 2, no Jak 1 model**: sparticle-motion-blur (1.3), sky-work 32/33 + render-sky-quad +
  clip-polygon x2 + draw-large-polygon (sky, 6.2 ms: or drop, see below), collide-hash 11/12 +
  fill-bg-using-box-new / fill-bg-using-line-sphere-new (3.4), the spatial hash
  (grid-hash 18-22, sphere-hash 28-33, spatial-hash 35-39: 5.3 -> 2.4, the traffic's avoidance
  queries), add-light-sphere-to-light-group, draw-string-asm, nav-mesh / nav-engine methods
  (small), ocean (render-ocean-quad: Jak 1's micro-logic version).
- Verify every one with `OPENGOAL_MIPS2C_VERIFY=1` over the city pads (as Jak 1 did).

#### Done: the ports from Jak 1 (AI-assisted, Oct 18, j2-nat-port)

`game/mips2c/jak2_functions/native_*.cpp`, registered next to the mips2c versions through
`native::on()` (native_functions.h). `config.ini logic_native = off` (preset safe; host
`OPENGOAL_LOGIC_NATIVE=0`) registers none of them; `mips2c_native_off` still names single ones.

| native | Jak 2 changes against Jak 1's native |
|---|---|
| `sp-process-block-2d` / `-3d` | per-particle time vector from the scratchpad table (byte 129), frames 0 = paused, valid is byte 128, flag bits moved (alpha 32, orbiter 64), aux flags 0x8080 call add-to-sprite-aux-list instead of the callback (2d); 3d: rotvel3d once per 10 frames, then quaternion-normalize! |
| `sp-launch-particles-var`, `particle-adgif` | a matrix instead of a position (rows on the frame, row 4 = (0 0 0 1) for sp-adjust-launch), the clock byte, colors truncated to whole numbers, 5 levels, the state's launch control; adgif: uncache a shader with bit 1024 |
| `cspace<-parented-transformq-joint!` | the w lane of 1/scale cleared (Jak 2's mips2c patch) |
| `new-bones-mtx-calc-asm` | `pc-new-bones-mtx-calc` (bones.gc `#when TARGET_3DS`, like Jak 1): bone stride 80; host verify mode runs the GOAL loop in a nested call and compares (`bones verify`) |
| `shadow-execute` (+ its 15 helpers and the VU0 code inlined) | j2-nat-shadow: Jak 2's offsets, debug-draw path, add-single-tris, scissor-edges without clipping |
| `moving-sphere-triangle-intersect`, `collide-do-primitives`, collide-cache-prim 9 / 10, collide-mesh 12 / 14 / 15 | j2-nat-collide: the GOAL helpers (ray-sphere-intersect, closest-pt-in-triangle...) stay GOAL calls; collide-puss-work 9 / 10 written but not registered (Jak 2 never calls them) |

Not here: the natives without a Jak 1 model (j2-nat-new, below).

**Identity** (host small-memory C mode, `OPENGOAL_MIPS2C_VERIFY=1`, 0 mismatches everywhere):
jak2_route.pad, all natives (`OPENGOAL_MIPS2C_VERIFY_FULL_EVERY=200`): 3.95M calls (cspace 3.84M,
collide-cache-prim 9: 86k, shadow-execute 5600, collide-mesh 14 / 15: 3392 / 2440, msti 8711,
launch 1098, adgif 798, block-2d 324, -3d 58 full-memory checks), bones 92942 calls / 1.39M bones;
jak2_city.pad, particles with a full check every 8th call: adgif 25859, launch 11455, 2d 4183,
3d 1015; cspace 1.53M; bones 39902. Shadow and collision on both pads in their branches. Jak's
end positions are the mips2c runs' (city 1065.98 0.52 194.46, route 1161.20 -50.58 513.13).
Weakly covered: collide-mesh 12 (9 calls, no hit), collide-cache-prim 10 (73 calls).

**Mac** (slums stand, sampled, on vs off, loaded machine: shares against the unchanged rest of
the frame): particles 1.6x, cspace 1.6x, shadow 3.7x, bones (C++ vs goalc C) 4.5x, collision 2.3x.

**Azahar** (CIA, jak2_city.pad, null renderer): city-stand 75.6 -> **49.7 ms** logic (13.2 ->
20.1 fps), city-walk 74.7 -> 50.0 ms. iop ~1.7 ms of that. The Mac ratios predicted ~21 ms; the
ARM gain is larger (Jak 1: 4-8x fewer instructions than mips2c).

#### Done: the natives without a Jak 1 model (AI-assisted, Oct 18, j2-nat-new)

`game/mips2c/jak2_functions/native2_*.cpp` (native2.h), registered next to the mips2c versions,
always on (`OPENGOAL_MIPS2C_NATIVE=0` / `mips2c_native_off` still pick the mips2c ones):

| native | notes |
|---|---|
| `render-sky-quad`, `render-sky-tri` | draw-large-polygon and both clip passes inlined (GOAL never calls them); the scratchpad buffers and the DMA get the same bytes, so the sky DMA is unchanged |
| `(method 32 sky-work)`, `(method 33 sky-work)` | stars transform / DMA; vector-normalize!, lookup-texture-by-id-fast and adgif-shader<-texture-simple! stay GOAL calls; the VU0 registers they leave in `sky_regs_vfs` are the mips2c's |
| grid-hash 18 / 19 / 20 / 22, sphere-hash 28-33, spatial-hash 33 / 35 / 36 / 37 / 39 | 18 / 19 inlined into their callers; 20 and 22 called directly when the method slot holds their stub; update-grid and line-sphere-to-grid-box stay GOAL |
| collide-hash 11 / 12, `fill-bg-using-box-new`, `fill-bg-using-line-sphere-new` | the scratchpad bit array, the query state and the collide cache entries as the mips2c writes them |
| `sparticle-motion-blur` | particle callback; atan stays a GOAL call |
| `add-light-sphere-to-light-group`, `draw-string-asm` | draw-string-asm is a register-for-register translation (locals instead of the ExecutionContext) |
| `mem-copy!` | gcommon.gc's byte loop: memmove unless dst overlaps above src; `types-h.gc` (first GAME file, after `__pc-get-mips2c` exists) points the symbol at it; the reference "mips2c" side is the byte loop |

Verify mode now also compares host memory that mips2c functions keep state in
(`native_verify_host_state`: the sky's shared VU0 registers) and knows Jak 2's scratchpad.

**Identity** (host C mode, `OPENGOAL_MIPS2C_VERIFY=1`, 0 mismatches): jak2_city.pad 3.93M calls
(`FULL_EVERY=4`: motion blur 2.39M, render-sky-quad 458k, add-light-sphere 359k, mem-copy! 353k,
fill-bg-box 84k, grid-hash 22: 76k, collide-hash 11: 50k, sphere-hash 30: 46k, sky-work 32 / 33:
525 full-memory checks each, sphere-hash 28: 1130 calls); jak2_route.pad 6.11M calls
(`FULL_EVERY=8`, all districts). Jak ends where the mips2c run ends (city 1065.99 0.52 194.46).
Not reached by the pads: grid-hash 18 / 19 (inlined in the callers, which are checked),
sphere-hash 29 / 31 / 33 (few or no calls).

**Mac** (slums stand, sampled, `OPENGOAL_MIPS2C_NATIVE=0` vs on, the same build): these functions
129 -> 68 us of the 0.70 ms frame (frame 0.699 -> 0.642 ms): motion blur 29 -> 14 us, sky 41 ->
~25, spatial hash 27 -> 10, collide hash 11 -> ~5, add-light-sphere 4.7 -> 3.3, draw-string-asm
3.5 -> 2.0, mem-copy! (C++ byte loop as the reference) 9.3 -> 1.2. N3DS estimate (mips2c 200x,
natives 85x): ~21 ms -> ~5 ms, so **~16 ms** less in the slums (the plan's list was ~14 ms).

**Azahar** (CIA of jak2-3ds with j2-nat-port, j2-cheap and these natives merged; jak2_city.pad,
null renderer, private instance): city-stand **26.3-28.4 ms logic, mean 27.0** (30.0 fps, the cap;
vsync 5-7 ms left), city-walk 26.3-27.1 ms (30.0 fps); iop 1.4 ms of that. Before: 75.6 ms
(baseline), 49.7 ms with j2-nat-port alone, 66.4 ms with j2-cheap alone, so roughly 40 ms with both
and ~13 ms of the drop to 27 ms from these natives (the Mac estimate was ~16 ms). Jak ends at
(1065.98, 0.52, 194.46) in target-hide, as before. The merged host build's verify run
(jak2_city.pad): 3.87M calls, 0 mismatches over all natives (these and j2-nat-port's). Logs:
`port_test/reports/j2-nat-new`.

Left here: the sky clip passes are hardly faster than mips2c on the Mac (most cloud quads are
inside all four planes, where a pass only copies: skipping the passes and storing the copies
once, as Jak 1's micro-logic render-ocean-quad does, is next), ocean (render-ocean-quad), the
nav-mesh methods (~0.3 ms of mips2c).

### 2. Codegen and C-mode overhead (~4-5 ms)

- The thunk / adapter path costs time by itself: on the Mac 6.4% of the slums frame is in
  `thunk<>`, 2.3% in `mips2c_goalc_adapter`, 1.4% in `ExecutionContext::jalr` (mips2c calling
  GOAL through `goalc_call_goal8`). Natives registered with `MIPS2C_NATIVE_IMPL_GOAL` skip all
  three. Typed kernel functions (`typed_adapter`) could get direct entries the same way.
- GOAL code is ~54% of the Mac time (27 ms on the N3DS). The 32-bit argument ABI and u64 narrowing
  (3ds-perf-codegen) are the next codegen steps. Hot GOAL code: do-joint-math, decomp-frame /
  decomp-fixed, sp-init-fields!, atan, mem-copy!, res-lump lookup-tag-idx, the gkernel dispatch
  lambda and thread-suspend/resume (the process tree's per-process cost: 291-374 processes).

### 3. Cheap fixes outside the game logic (~3 ms)

- `update-discord-rpc` runs a `std::regex` and `string-upcase` / `string=` every frame
  (`pckernel` / kmachine_extras): off on the 3DS.
- `subtitle2` `get-scene-by-name` every frame (0.3-0.5 ms): cache by the current scene.
- `(method update setting-control)` -> `cam-master-set-entity` -> `entity-by-name` every frame
  (res-lump string lookups): cache the entity until the setting changes.
- sp-process-block copies each particle block to the scratchpad and back with `memmove`
  (`typed_adapter` + `_platform_memmove`: ~3% of the slums frame). On the 3DS the scratchpad is
  ordinary memory, so the native can work in place.

### 4. Multi-core

Cores: core 0 = the game thread + IOP/IO threads; core 1 = the system core (an app can get up to
~80% with APT_SetAppCpuTimeLimit; Jak 1's io_on_system_core attempt hung at boot); core 2 = render
thread, sound mixer, loader (idle while the render thread waits on the GPU); core 3 = system only.

GOAL code is single-threaded. The process tree, the GOAL heaps, symbol values, `*stack*` / the
scratchpad, the kernel's suspend/resume and every GOAL callback assume one thread. So jobs on
another core must be pure data work, with no GOAL calls and their own scratchpad, and must be
joined before anything reads their output.

Safe candidates, in order:
1. **IOP / overlord / IO threads to core 2** (a): ~1 ms, no game-side change; core 2 already runs
   the loader. Not core 1: it hung at boot in Jak 1.
2. **Shadow volumes** (b, ~4.9 ms after the native): shadow-execute reads the bone matrices and
   the shadow geometry and writes only its own DMA output. Kick it as a job at the point the
   game calls `shadow-execute-all`, and join before the frame's DMA chain goes to the render
   thread (or at the next frame's bones). It needs a double-buffered shadow DMA area (the PS2 one
   is per frame already). On core 2 it competes with the render thread, so use core 2's GPU-wait
   time or the core-1 share.
3. **Sky DMA** (~4.2 ms native): it depends only on time of day and camera. Kick it after the
   camera update, join at the end of the draw hook. Better: have the Ctr renderer draw the sky
   from the time-of-day state on the render thread (Jak 1 skips DMA for buckets the 3DS doesn't
   draw). Then the work leaves core 0 entirely.
4. **Particle block update** (~2.8-3.8 ms): sp-process-block-2d/3d works on the particle arrays.
   Blocks whose particles have GOAL callbacks (`func` fields) must stay on the game thread, or the
   job records the callbacks and the game thread runs them after the join. Join before the sprite
   DMA build. Launching (sp-launch-particles-var, part-tracker spawns) stays on core 0.
5. **Animation decompression + bones** (~3.3 ms): per-process joint decompression (Jak 1's
   pc-anim-decomp-request) and the bone matrices are pure math on per-process buffers. Batch all
   processes' requests in execute-math-engine, run them as a job, join before the
   joint-callbacks / post that read the joints. cspace<- with joint-mod callbacks stays serial.
6. Not worth it or not safe: traffic path planning (the whole traffic engine is ~0.8 ms), nav
   queries (synchronous from AI code, which needs the result in the same call), collision and
   spatial hash queries (synchronous), process code.

A share of core 1 suits jobs with a frame of slack: the result is used one frame late, like a
skinned-shadow update. Running sky and shadows one frame late is a visual compromise (a lag of
one frame), so measure it with the soak test before using it.

### 5. Presentation options (no gameplay change; "30 fps over fidelity")

- Shadows `full|blob|off` like Jak 1 (3ds-shadow-volumes): blob or off saves the whole 4.9-11 ms.
  Fewer shadow volumes: only the nearest N.
- Sky drawn natively by the Ctr renderer (no EE sky-work DMA): -4 to -8 ms on core 0.
- Animation LOD: update far citizens' joints every other frame (render interpolation), -2 to -4 ms.
- Particle budget: fewer motion-blur / ambient particles in the city (-2 to -5 ms).

### 6. Gameplay changes (not for the unmodified baseline)

- Fewer simultaneous citizens, vehicles and riders (the traffic-manager population settings).
  Not measured. The process-proportional categories (joints, shadows, foreground DMA, kernel,
  collision, spatial hash, traffic AI) are ~60% of the frame, so half the traffic would be
  roughly -25 to -30% (city ~22 -> ~16 ms after the steps above).
- Lower traffic update rate for far processes (AI every other frame).

## Done: per-frame lookups, PC-port extras, kernel glue (j2-cheap, Oct 18)

(AI-assisted.) Branch `j2-cheap` (from jak2-3ds 6d9c7881e2). Plan steps 4, 5 and part of 7.

**Azahar (CIA, null renderer, `jak2_city.pad`, same windows as milestone 2):**

| window | before (baseline-a) | after | |
|---|---|---|---|
| city-stand (slums) | 13.2 fps, logic 75.8 ms | 15.0 fps, logic 66.4 ms | -9.4 ms (-12%) |
| city-walk | 13.5 fps, logic 74.3 ms | 15.0 fps, logic 66.7 ms | -7.6 ms |

IOP on core 0: 2.36 -> 2.09 ms/frame. 142 suspends/frame and 24.7 KB of stack are unchanged
(kernel copies only [sp, stack-top) already). Jak ends at (1065.98, 0.52, 194.46) in
`target-hide`, as before. The Mac estimate was ~4 ms (85x on the shares below). The 3DS saved
twice that: string and res-lump scans run at ARM11 memory speed, which the 85x doesn't model.

What changed (all unconditional; PC behaviour unchanged unless noted):
- **entity-by-name cache** (`engine/entity/entity.gc`). `(method update setting-control)` ->
  `cam-master-set-entity` looks up the camera entity every frame. The name is usually `#f`, so
  every actor, nav mesh, race mesh and camera of every active level was scanned with a res-lump
  `lookup-tag-idx` each. Results are cached by the name's bytes, which is what `string=` reads,
  whatever the argument's type. 8 entries. The cache is flushed when any `*level*` slot's
  (active, name, bsp) changes. Mac share in the slums: setting-control update 5.4% -> 0.4%.
- **subtitle2 scene lookup cache** (`pc/subtitle2.gc`). `get-scene-by-name` upcased every scene
  name of the subtitle file for every queued gui (the city's ambient speech), twice per frame.
  Results are cached per text file and upcased name, and the cache is cleared when the text loads.
  A hit leaves `*vag-temp-string*`/`-2` as the search did, and searches that printed a warning
  aren't cached. Mac: get-scene-by-name 2.8% -> 0.1%. What's left of draw-subtitles (1.7%) is
  `subtitle-bump-up?` (4 gui lookups) and the text drawing.
- **No controller LED on the 3DS** (`pc/pckernel.gc` `led-enabled?` is `#f` under TARGET_3DS).
  `update-led` searched the process tree three times per frame. Mac: pc-settings update
  2.1% -> 1.0%.
- **pc-prof only when wanted** (`kernel/gcommon.gc` macros check `*pc-prof-on*`, set by
  `jak2/kmachine.cpp` as in Jak 1: always on PC, on the 3DS only while kperf times sections). The
  kernel dispatcher made 677 `pc-prof` calls per frame in the slums, each through an adapter thunk.
- **Direct C-mode entries for hot kernel functions** (`GOALC_KFN`, c_backend.md "Typed kernel
  functions"): the Jak 2 kernel primitives (reset-and-call and thread-resume run for every
  process), `pc-rand`, `__mem-move`, `__read-ee-timer`. They skip the thunk's two indirect calls.
- Already off on the 3DS before this work: the discord RPC (std::regex) and the speedrun update
  (`#unless TARGET_3DS` in `pckernel-common.gc`). jak2-prof's profile was a PC-size build, which
  is why it listed them.

Identity: `jak2_city.pad` on the host (small memory, C mode) and in Azahar gives the same position
and state. `jak2_route.pad` on the host gives the same checkpoints as the base build. The swim
height at frame 2301 (`-2.6`..`-2.97`) and the main-town walk (one in four runs) vary between runs
of the base build too, because the time a load takes changes the wave phase and the traffic.
C-mode goalc-test (Jak 1 suite + Goalc*) passes: 1692 + 10 tests. Jak 1 `level_pairs.pad` (host
small-memory gk) gives the same pad-script output as before, exit 0.

**Tried and reverted: the IOP thread on core 2** (plan step 8). With the IOP SystemThread
created on core 2, Azahar's boot hung after "IOP: After inits". The IOP thread kept dispatching
(the stack report came 2 minutes later), but the overlord's start thread never finished, so the
EE waited forever in `wait_for_overlord_init_finish`. That is the same place Jak 1's IOP-on-core-1
attempt hung. Not understood. Leads:
- the fake ISO reader pool (`BS::thread_pool`, libctru pthreads: core 0, priority 0x3F until
  their first task raises them to `CTR_PRIO_IO`);
- the IOP's polling loops, which assume they share a core with the threads they wait for.

Note for a retry: `bootstrap_thread_func` (SystemThread.cpp) sets the IOP's priority back to
`CTR_PRIO_IOP` after creation, so a placement priority has to be applied there.

Kernel overhead left (not changed): 142 suspends/frame copying ~175 bytes each, plus
`new 'process 'cpu-thread` / `delete` and a setjmp/longjmp pair per trans/post hook. On ARM11
that is a few tenths of a ms in total.

## Caveats

- One Mac, background load from other sessions (Discord, other agents' gk): the per-window
  spread is ~10-15%. The ratio is calibrated on Jak 1 in five spots and does not include Jak 2's
  memory footprint: the city's data is bigger, so the N3DS's caches will miss more (the Mac's
  caches hide that). Treat the absolute numbers as +-25%.
- The city's process count makes the per-process C-mode overhead (thunks, suspend/resume) matter
  more than in Jak 1, so a logic-only 3DS boot with `perf_sections` is still the real check.
- Jak 2 has only a few `with-pc-profiler` sections (trans / code / post), so kperf sections say
  little about Jak 2. Use the sampler, or add sections around the display-loop phases.
