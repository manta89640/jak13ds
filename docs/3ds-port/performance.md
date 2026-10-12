# Performance on the New 3DS (Jak 1)

(AI-assisted.) Numbers are from Azahar (New 3DS), emulated time. They are repeatable (a few % run
to run) but real hardware may differ.

## Measuring

```sh
platform/3ds/tools/run_emu.sh --seconds 560 --out build-3ds/perf \
  --stage "--proj <mirror> --clean-logs --clean-user --pad-script $PWD/platform/3ds/tests/perf.pad"
platform/3ds/tools/perf_report.py build-3ds/perf/stdout.log      # --perf-sections for the breakdown
```

- `tests/perf.pad` is the gameplay.pad route with fixed windows marked by `perf-mark` log lines:
  `title` (600 frames), `menu` (600 frames in the title menu), `newgame` (menu taps + loading),
  `intro`, `skip`, `idle` (1800 frames, Jak idle in village1 at intro-start), `moves`, `idle2`
  (1800 frames after the moves), `end`. It takes about 5 emulated minutes.
- `perf_report.py` averages every window: kperf's `logic` (the whole game thread, including waits
  for the render thread), the renderer's game-thread split (`logic` without waits + `wait render
  thread`), render thread time per bucket renderer, the time the IOP and sound threads take from
  the game's core, and with `--perf-sections` every section (inclusive ms/frame and calls/frame).
- (AI-assisted, 2026-10-03) The 3DS writes kperf's lines every 5 s now (over the 5 s), and its
  `logic` leaves out the wait in `sync-path` (the game waiting for the render thread), which comes
  after `idle` as `wait`: logic numbers from before this are higher by that wait. A window's first
  line (skipped by `perf_report.py`) now covers up to 5 s; its "N s" count is in lines.
- Variance: within a window the per-second `logic` has a standard deviation of 0.2-0.7 ms; the
  same build and scene in two runs differed by ~1 ms (2%).
- Sections cost time: 3.6 us per timed section on the C side (logged at startup) plus the GOAL
  call, ~6 us in total. With ~750 sections per frame in gameplay that is ~4 ms/frame. Nested
  sections inflate their parents. With sections off, the GOAL profiler blocks cost a symbol test.
- The older numbers (f873b947: 68.5 ms vs d2dc5852: 84.9 ms) were not a game regression: the
  second run had sections on (its log says "per-section timing enabled"), and 0053978e had added a
  timed section around every mips2c call (thousands per frame, then timed with steady_clock).

## What was slow (3ds-port 42d589cd, village1 idle, 22 fps)

| cost | ms/frame | cause |
|---|---|---|
| game waits for the render thread | ~18 | `__pc-set-levels` (every frame, in level-update) made the renderer wait for the frame it had just been given: game and render thread ran one after the other |
| IOP thread | 7-11 | the ISO thread polled every 100 us: 5000+ IOP dispatches/s on the game's core |
| sound thread | 1-4, growing | 989snd voices never ended without audio output: handlers piled up |
| sky / shadow / generic merc DMA | ~3 (menus: +9.5 generic merc) | computed for buckets the 3DS renderer skips |
| memory card polling (menus) | ~40 | the progress menu stat'ed and read every save file every frame |

All five are fixed. After (same scenes): gameplay idle 22 -> 44 fps (frame 45 -> 23 ms), after the
moves 22 -> 56 fps, title menu 12 -> 39 fps, title 37 -> 53 fps. Level loading still stalls for
as long as the render thread takes to load the level's .c3l file (it loads it inside a frame).

## Where the time goes now (village1 idle, sections on)

Frame 23 ms = render thread bound (render thread 21.5 ms: sprites 10, tfrag 7.6, merc 1.8). The
game thread needs ~12.5 ms (sections off) and waits the rest at dma-sync.

| game thread | ms/frame |
|---|---|
| process code / post (77 processes) | run-code 17 incl. display, run-post 3.9 |
| camera-slave (line of sight probes) | 3.1 |
| joint animation (42 calls; cspace<- 233 calls) | 2.8 |
| target (physics 1.5, ja-post 1.0) | 2.5 |
| particles (process-particles) | 1.9 |
| draw-hook | 1.6 |
| collision mips2c still not native: collide-probe-instance-tie 1.0, collide-probe-node 0.4, sp-launch-particles-var 0.5, ocean-interp-wave 0.3, ripple 0.2 (all native now except ripple, see below) | ~2.4 |
| IOP 0.6, sound 0.02 | 0.6 |

Next steps, by expected gain:
1. Render thread (owner: renderer): sprites 10 ms and tfrag 7.6 ms decide the frame rate now.
2. Load .c3l files on a separate thread (loading hitches of 100+ ms).
3. Done, not measured in Azahar yet: every game-logic mips2c function has a native version now
   (docs/3ds-port/3ds_build.md, "Native versions of hot mips2c functions", with a table of ARM
   instructions per call), built with -O3. Against mips2c (qemu, test/mips2c_native/run.sh
   bench): collide-probe-instance-tie 10x, collide-probe-node 8x, sp-launch-particles-var 2x,
   ocean-interp-wave 5x, (method 28 collide-cache) 4x; and for the ones measured above, before ->
   now: moving-sphere-triangle-intersect 2.0x -> 4.0x, (method 9 collide-cache-prim) 2.1x -> 4.0x,
   cspace<- 4.7x -> 8.4x, sp-process-block-2d 2.5x -> 4.2x, collide-cache 32 / 26 / 27
   6.3x / 3.3x / 2.9x -> 11.6x / 4.2x / 5.2x.
4. target-real-post runs Jak's physics `time-ratio` times per frame (2 at 30-60 fps, 3 at 22 fps,
   4 below 15): faster frames make it cheaper by themselves.
5. GOAL C code quality (u64 everywhere, owner: compiler): helps every row above. (Checked for
   hand-written replacements of GOAL functions: goalc's C output compiled for ARMv6K is already
   close to hand-written code for float work, e.g. decomp-frame's inner loop is ldrsh / vcvt /
   vmul / vmla per value and pc-port-raw-ray-sphere-implementation is 93 instructions, so native
   versions of GOAL functions gain much less than the mips2c ones did. The exception is GOAL
   functions the natives call for every triangle, where the call itself was the cost: the four
   collision helpers in 3ds_build.md are native now and called directly.)

## Real hardware: the GPU (Sentinel Beach, ~10 fps)

On a real New 3DS the beach ran at ~10 fps with the GPU as the bottleneck (60-90 ms of GPU time
per frame, depending on the view). Azahar doesn't model GPU timing, so this doesn't show up there.
Lower draw distances (`config.ini`: 300 / 100) didn't help: the triangle count isn't the problem.
The cause is texture reads: level textures had no mip levels and were in linear memory (FCRAM),
so every far away surface read its texture at full size and missed the GPU's small texture cache.

What changed (c3l v8, see c3l_format.md):

- **ETC1 / ETC1A4 textures with mip levels** (converter defaults). A level's textures are 1/4 to
  1/2 of their 16-bit size, and far surfaces read small mip levels.
- **Level texture pools in VRAM:** all textures of a level are one linear block, copied to VRAM
  with a single GPU copy at frame begin (`ctr_gpu_pool_*`). The level the camera is in has
  priority; the other one's pool moves out if VRAM runs short.
- **Fewer texture binds:** a bind always clears the texture cache. Level draws are sorted by
  texture and state across chunks (opaque, depth written draws; blended ones keep their order),
  and binds with the same texture and parameters are skipped.
- **Sprites:** indexed quads written straight into the vertex buffer, cheaper sin/cos.
- **Sound:** the mixer mixes blocks of samples per voice instead of one sample at a time over all
  voices. It took 30-60% of the core it ran on at the beach; put it on a core without the render
  thread (`sound_core` in `config.ini`).

To see where the GPU time goes on hardware: `gpu_profile = 1` in `config.ini` leaves out one
group of renderers at a time (level, merc + eyes, sprites, ocean, direct; 2.5 s each, then
everything) and logs `[ctr] gpu profile: all X ms, level Y, ...`: the GPU time of a whole frame
and what each group costs (the difference). The frame statistics also log draws, texture
binds, command buffer KB and the texture pools in VRAM.

### Render thread CPU (later)

- Level draws: the drawing order (texture, state, chunk) is made when a level loads; a frame walks
  it and skips chunks that aren't visible, instead of sorting the visible draws every frame.
- Level chunk matrices are converted to the GPU's clip space once per visible chunk per frame;
  the backend sets the matrix and the vertex buffer only when each one changes.
- Merc: bone rows built once per palette (not per draw) and uploaded only when they change.
- CtrDirect (text, menus): no float divisions per vertex.
- Sound: the mixer mixes blocks per voice (see above).

Not tried yet, needs hardware to check: the PICA's own fog unit (a depth LUT) instead of the fog
ramp texture on the level's second texture stage (one texture fetch less per pixel, and fog on
merc too).

### C library (all threads)

- memcpy / memmove / memset (and the `__aeabi_` entry points): `platform/3ds/port/ctr_mem.S`
  replaces newlib's generic C versions (no LDM/STM, a byte loop for any misaligned copy). 8-register
  LDM/STM with PLD, a shift-merge path for misaligned sources, an LDM/STM path for copies under
  64 B. Test: `platform/3ds/tests/memtest/build.sh`, run the 3dsx in a private emulator.
- `-fno-math-errno` for all 3DS code (it was on the GOAL modules only): sqrtf is one VSQRT.
- `std::chrono::steady_clock::now()` (`platform/3ds/port/ctr_clock.cpp`): the system tick to ns
  with a multiply, not libctru's clock_gettime (two 64-bit divisions in libgcc; the ARM11 has no
  divide instruction).

## GPU timing in one session (AI-assisted)

Three config.ini switches give the GPU numbers no public source has (cycle costs per draw, per
vertex, per pixel; what the hotspot and traffic counters mean). All are off by default, change
nothing when off, use GSP requests and citro3d only (no new system calls or IO mappings), and only
mean something on hardware: Azahar returns zeros or garbage for the counters and the performance
log. Turn all three on for one playtest:

```
pica_bench = on      # the microbenchmark at boot, before the game starts
gpu_perflog = on     # GSP performance log, one line per 15 s stats window
gpu_counters = on    # hotspot stage samples + traffic counters, one line per stats window
```

Then play as usual for a few minutes (the two per-window switches cost one GSP request each per
window), and bring back `sdmc:/3ds/jak1/data/log/gk.log` and `bench.txt` from the same folder.
Set `pica_bench = off` afterwards: it runs at every boot.

**The bench** (`game/graphics/ctr/ctr_gpu_bench.c`) runs after the GPU starts and before the game
loads (13 s in Azahar, an estimated 20-60 s on a New 3DS; the bottom screen shows the test it is
on). Each test builds its command list once and queues it 3-8 times back to back, then logs one
`[bench] <test> key=value ...` line to gk.log and bench.txt. The order goes from safe to risky:
B1 floor, B2 per draw, B4 vertex cache, cull check, B5 attributes, B6 fill, B7 setup, B8 early
depth on the usual 240x400 target; then register settings the game never used on hardware
(COLORBUFFER_READ = 0, POST_VERTEX_CACHE_NUM 0x4 / 0x84, the 256x416 block32 target and its
display transfer); then the bench's own vertex shaders (program switches, B3) and the game's
unproven new `ctr_mesh` and `ctr_skin` (`shader_mesh = new`, `shader_skin = new`). Before each
test a `start <test>` line is appended to bench.txt and the file closed, so if the GPU hangs (the
game never starts; power off) the last `start` line names the culprit. A queue that doesn't finish
in 3 s stops the bench with a `[bench] HANG` line and the game won't draw either.

Read it with:

```
python3 platform/3ds/tools/bench_report.py gk.log bench.txt
```

It takes the last session and prints one table per group:

- **gpu**: the command lists' GPU time per repetition, from the GSP performance log's P3D total
  (falls back to `wall`, citro3d's queue timer, which also covers fills and transfers). The unit of
  the performance log is calibrated from the vblank interval (`upm` = raw units per ms in the
  header: ~1000 means microseconds as libctru says, ~1000000 nanoseconds).
- **per item**: B2 us/draw (and the difference to `B2.nostate`: what each state change adds), B3/B5
  ns/vertex (3 per culled triangle; the B3 line ends with a fit: ns per vertex per shader
  instruction), B4 ns/index per reuse distance k (k0 = no reuse; the knee is the cache size,
  per POST_VERTEX_CACHE_NUM value), B6 Mpixel/s and ns/pixel, B7 ns/triangle.
- **hs**: the three busiest hotspot stages (arrays, vsh, setup, raster, tex, light, comb, fb) and
  `(samples/expected)`: the sampler takes one stage per 8192 GPU cycles; B1.idle* shows where an
  idle GPU's samples go (subtract that share from short tests).
- **traffic**: the biggest traffic counters, in KB per repetition once B1 calibrated them (B1.clear
  writes 384000 bytes per fill unit, B1.transfer reads 384000 and writes 288000): the calibration
  line shows bytes per count for each.
- **B8** lines are pixel counts of a readback of the color buffer: `a` the near or small quad
  (red), `b` the full screen quad (green), `c` the clear color (blue), with `cbox`, the bounding box
  of the clear color left over, and the expected counts. `c2`: a 4x4 near quad at an unaligned spot,
  then a full screen quad between the depths with early depth on: clear color left = pixels the
  early depth test rejected wrongly, `cbox` = the granularity of the early depth data (the "black
  squares"). `c3.<func>.d<0|ff>`: each EARLYDEPTH_FUNC against a clear value of 0 and 0xFFFFFF.
  `c4a/b/c`: does a draw with depth write off / failing the alpha test / early depth off still
  update the early depth data? `c5a`: is the early depth data kept between two frames without
  EARLYDEPTH_CLEAR (GBATEK: it is separate storage, a depth buffer fill doesn't touch it)?
  `B8.*.ref` locates the 4x4 quad in the readback (rows are flipped: y = height - 1 - row).
- `B6.cbread0.probe bg_ok=1`: with COLORBUFFER_READ = 0 the cleared background next to an opaque
  quad survived (then an opaque pass may skip color reads).

**The per-window lines** in gk.log:

- `[ctr] gpu perflog: N frames in T ms; raw GSP units per frame: p3d .. ppf .. psc0 .. psc1 ..;
  vblank0 last ..; ms/frame: p3d .. ppf .. psc .., c3d draw .., fence busy .. -> bubble ..`:
  bubble = busy - (p3d + ppf + max(psc0, psc1)) is the time the GPU sat idle inside a frame's
  busy span (waiting for the CPU between command list parts, queue gaps). Compare p3d with the
  draw count and triangle count of the `render ms/frame` line next to it, using the bench's per
  draw and per vertex costs.
- `[ctr] gpu profile window <level>: hw busy % of N samples (M expected, F frames): ...;
  MB/frame: ...`: the stage shares over the window's first 10 s (the 16-bit sample counts restart
  after 16 s) and the traffic per frame over the whole window (assumed bytes until the bench's B1
  calibration says otherwise).

## Vertex programs and the command stream (AI-assisted, Oct 12)

Branch shader-opt. What the PICA200 does per vertex and per draw, what changed, and which
pica_bench line decides each default. Nothing here ran on hardware yet; the new programs are off
by default, the pure command dedup is on.

### What is known about the vertex units (public sources)

Well sourced (register docs, libctru/citro3d/picasso code, emulator code, vendor slides):

- 3 vertex processors plus the geometry shader unit, which runs as a 4th vertex unit when no
  geometry shader is used (3dbrew Hardware; GBATEK "Shader Registers": "three shader units at
  once (or all four units, when not using the geometry shader)"; libctru writes
  GPUREG_VSH_COM_MODE = 0 to "ensure all 4 units are correctly initialized"). Each is a 24-bit
  4-way SIMD unit (DMP, HPG 2011 slides). 268 MHz. Vertex code memory: 512 instructions.
- One instruction reads at most one float uniform: only SRC1 is 7 bits wide (SRC2 of the inverted
  forms DPHI/SGEI/SLTI, SRC2 of MAD, SRC3 of MADI); relative addressing only on that operand
  (3dbrew GPU/Shader_Instruction_Set, picasso Manual). Two *different* input registers in one
  instruction give wrong results (picasso errata; picasso refuses it). MAD's operand descriptors
  must be in the first 32 slots (picasso does that).
- The output map: OUTMAP_TOTAL (1-7 output registers), OUTMAP_Oi per component semantics,
  OUTATTR_CLOCK bits per semantic (position.z, color, texcoord0/1/2, texcoord0.w,
  normquat/view; GBATEK: "controls the clock supply"); libctru derives all of it from picasso's
  .out table (DVLE_GenerateOutmap), so an unused semantic is already not clocked. Two semantics
  can share one output register (picasso ".out - texcoord1 outtc.zw").
- The hardware takes the absolute value of vertex colors and saturates them to 1 before
  interpolation (Azahar src/video_core/pica/output_vertex.cpp).
- Post-vertex cache: GPUREG_POST_VERTEX_CACHE_NUM (0x22D) "usually 0x4 or 0x84"; cleared with
  GPUREG_VTX_FUNC (citro3d after every draw).
- citro3d's two dummy PRIMITIVE_CONFIG writes after that clear came with "Fix some cases of GPU
  freezing when changing programs" (citro3d 46117ec9): kept everywhere.

Not found anywhere: instruction latencies, dual issue, cycles per instruction, the cost of
RCP/RSQ, how vertices are spread over the units, whether fewer output registers or attributes
are cheaper, the vertex cache size. pica_bench (B2-B5) measures these on the console. Vendor
figures: 15.3 M polygons/s at 200 MHz (DMP via 4Gamer 2010), 40 Mtri/s at 100 MHz (DMP, EG 2008)
are peak numbers, speculative for this use.

### shader_opt: the shorter programs

Instructions per vertex (including `end`), output registers; the hardware default is the Oct 2
"old" set (new_shaders = off). `shader_stats.py build-3ds/*.shbin` prints this table.

| program | default (old) | new | shader_opt | how |
|---|---|---|---|---|
| level mesh, game fog on | 22 / 4 regs | 15 / 4 | `ctr_mesh_opt` 11 / 3 | fog factors as two extra matrix rows (dph), texcoord1 in texcoord0's .zw |
| level mesh, fog = off | 22 / 4 | 15 / 4 | `ctr_mesh_opt_df` 8 / 3 | only the draw distance fog: one dph straight into the output |
| level mesh, fog_alpha | 20 / 3 | 13 / 3 | `ctr_mesh_opt_fa` 11, `_df_fa` 9 | no clamp at 1 (the GPU saturates colors) |
| merc | 54 / 3 | 36 / 3 | `ctr_skin_opt` 34 / 3 | weights unscaled (clip x, y, z columns / 255), color scale in the light colors |
| merc shine | 57 / 3 | 36 / 3 | `ctr_skin_env_opt` 34 / 3 | the same; texcoord0 written in two parts |
| PS2 ocean | 15 / 4 | - | `ctr_ocean_full_opt` 10 / 3 | texcoord1 packed |
| PS2 ocean, clipped | 13 / 4 | - | `ctr_ocean_full_clip_opt` 7 / 3 | one mov with the swizzle xyzz |

The program code is shorter too: every switch between per-program shbins uploads it (mesh old
22 code words + 15 operand descriptors, mesh_opt_df 8 + 7; merc 54 + 20 against 34 + 14).
basic, clip and clip2 (6-7 instructions, sprites, HUD and sky) have nothing left to take out.

Per frame in Azahar (paused frames, fog off; indices drawn, an upper bound for the vertices
shaded, the post-vertex cache lowers it): Snowy start 237k mesh indices, 46k merc, 5k shine:
21 x 237k + 53 x 46k + 56 x 5k = 7.7 M vertex instructions with the default programs, 3.4 M with
shader_opt (-57%). The cost of an instruction on the console is B3's fit.

Same picture: in Azahar, paused frames with shader_opt off and on are the same pixels at Snowy
start and fort, Sandover (fog on and off, fog_alpha on), Fire Canyon, Jak and Daxter up close
(only the progress menu's animated icons and falling snow differ), except merc shine highlights:
ctr_skin_env_opt has the new ctr_skin_env's sphere map formula (no reciprocal), whose rounding
moves a highlight by a pixel here and there (~35 pixels on a shiny object at Snowy's fort).
With skin_clip_f24 on as well, a few dozen edge pixels of merc models flip (rounding). The mesh fog rows
and the packed texcoord1 (the fog ramp's 8 rows are the same, so t = the amount looks the same as
t = 0.5) give the same lookups.

Hardware risks: picasso and check_shaders.py pass (each output component written once, the MOVAs
apart, no flow control). Unproven on the console: MAD with a relative uniform operand (also in
the new ctr_skin), two semantics in one output register (B3.o3p), an input register as MAD's
first operand.

**Decided by pica_bench** (bench_report.py prints the verdict lines under B3):

- `B3.mesh_opt`, `B3.skin_opt`: they run at all (a hang stops the bench at them: the last `start`
  line in bench.txt) and their ns/vertex against `B3.mesh_new` / `B3.skin_new`; the report also
  estimates the old programs from the B3.v* fit (ns per vertex per instruction). On if they
  finish and are faster.
- `B3.o3p` against `B3.o3` / `B3.o4`: whether an output register costs (o3p ~ o3: packing pays)
  or a semantic (o3p ~ o4: packing changes nothing; harmless either way).
- `gpu_profile = ab` (shader_opt is in kAbSwitches) for the frame-level GPU time.

### The command stream per draw

`cmd_dump = on` (or the pad script's `set cmd_dump on`) writes a frame's command list;
`cmd_report.py --shbin-dir build-3ds cmd_*.bin` splits it by register group and draw kind and
lists writes that change nothing. Snowy start (paused, 748 draws): 46.4k words (181 KB) per
frame, 49% draw template (24 words per raw draw + 6 for the post-vertex cache clear), 36% float
uniforms (mesh matrices 16 words, merc bones 3 x (2 + 3 x palette) per model), 2% program code.
Words per draw: level mesh ~44, merc ~110, merc shine ~150 (citro3d path), sprites ~125.

| words per frame (paused) | everything new off | cmd_dedup (default) | + cmd_dedup_draw | + shader_opt |
|---|---|---|---|---|
| Snowy start | 46410 | 45850 (-1.2%) | 43014 (-7.3%) | 43448 (-6.4%) |
| Snowy fort | 36402 | 35474 (-2.5%) | 33582 (-7.7%) | 33694 (-7.4%) |
| Sandover | 40152 | 39536 (-1.5%) | 36984 (-7.9%) | 37490 (-6.6%) |
| Fire Canyon | 27028 | 26442 (-2.2%) | 24850 (-8.1%) | 25266 (-6.5%) |

(shader_opt adds 2 words per mesh matrix upload for the fog row, 6 with the game fog on, and
saves program code words.) The "everything new off" column already had the merc clip matrix and
lights as float24 (skin_clip_f24, ~11 words per model; off by default now).

- `cmd_dedup = on` (default): texture unit 0's type only when the format changes (~150-200 writes
  per frame), the merc shine pass's matrix, bones and fade only when they changed since the last
  shine draw (envmap_batch draws them one after another). Pure dedup: the registers end up the
  same.
- `cmd_dedup_draw = off`: the raw draws leave out PRIMITIVE_CONFIG's mode and VERTEX_OFFSET when
  the GPU has those values (4 words per draw; tracked across citro3d draws, the HOME Menu and the
  bench). Off until hardware ran it: citro3d writes them before every draw. Decided by
  `B2.nostate` (us per draw) and the per word price `(B2.unif_f32 - B2.unif_f24)` per word: 4
  words x draws per frame.
- `skin_clip_f24 = off`: the same price (B2.unif_f24 vs B2.unif_f32), and it is not
  bit-identical.
- Left alone: the dummy PRIMITIVE_CONFIG writes, RESTART_PRIMITIVE, GEOSTAGE_CONFIG toggles and
  START_DRAW_FUNC0 around each draw (hazard related or actions, not redundant).
- B4 (post-vertex cache) and B5 (attribute formats) don't change these keys: the level mesh is
  already 16 bytes (short4 + short2 + ubyte4), merc 24; B5.byte vs B5.short says whether a byte
  position format (a converter change) would pay, B4 whether vcache_reuse / a bigger
  POST_VERTEX_CACHE_NUM would.

Leads not done: a one-bone merc program for draws whose vertices all have one bone (about 20
instructions instead of 33; needs per-draw program switches, so shader_groups-style shared code
first); folding the shine pass into the base draw (two texture units); a shared shbin for the opt
programs (group E has no uniform room left for the fog rows).

## Level loads that stop the game (AI-assisted, Oct 11)

When both level slots are empty (a save load, a teleport / continue to another area), the game
loads the first level in a loop inside one frame (`load-state` `update!`: "Waiting for level to
load", as on the PS2) and draws nothing until it is in: the 2-2.5 s `[hitch]` lines with
`waiting for rpc #3` (the DGO channel). The second level loads in the background as usual. What
the loop waits for, in the order it happens:

1. the DGO from the SD card (IOP thread): `waiting for rpc #3` in the hitch line;
2. linking each object as it comes in (`link-begin` / `link-resume`, ~15-30 ms per level) and the
   level's login (`ld-login`, ~20-40 ms);
3. the level's `.c3l` (renderer data; the loader thread on core 2 starts it with the DGO):
   `[ctr] NAME: the game waited N ms for its .c3l`.

The log has a line for each: `[load] NAME loaded while the game waited: N ms` (the whole stop),
and `[ctr] loaded NAME ... reading N ms in M reads of K KB; linear free F KB`.

Emulator (emu-load, raw DGOs, Oct 11 batch): lavatube 2527 ms = DGO 2203 + .c3l wait 268 + link
and login ~50; maincave 2548 (DGO 2036, .c3l 451); village3 2447 (2175, 210); sunken 2053 (1971,
0). Hardware (user log, save load into Snowy): 2346 ms with 1196 ms of DGO; the .c3l took 2201 ms
(DGO 1149 ms): on hardware the `.c3l` is the long pole (~2.4 MB/s overall, decompressing 197 ms), slower than
the DGO (which reads 227 KB per SD card request, against the `.c3l`'s 64 KB stdio refills, each
seek starting a new one).

Changes:
- `.c3l` files are read unbuffered, 256 KB per request, through two windows (index and vertex data
  in turns) and two decoded-block caches: ~45 % fewer SD card requests for the same bytes (snow 64
  -> 35, maincave 73 -> 35, ogre 80 -> 41; host replay of the loader's reads). Each chunk's indices
  go straight into its index buffer: no 1.8-3 MB `std::vector` of all of a level's indices on the
  heap at the level-load peak (maincave's 2.5 MB failed once with `bad_alloc` in a test with a
  bigger stdio buffer; the level then had no background). `config.ini c3l_read_buffer` (KB; 0: the
  old reads).
- The emulator models the SD card per byte, so it shows no gain there (reading time within run to
  run noise); the request overhead is a hardware cost: check the `reading N ms` part of the
  `[ctr] loaded` lines in the next hardware log.

Not changed: the loop itself. `target-continue` waits for its levels to be active, so the loop
could end after the DGO and let the frames run, but the frames would take core 2 time from the
`.c3l` loader (the long pole on hardware) for a black screen.

### Linear memory with two levels (vista tier, c3l v13)

`platform/3ds/tests/level_pairs.pad` loads the tightest pairs (emulator, Oct 11 batch levels; the
emulator stores RGBA4 textures as RGBA8, so hardware has more left). Linear free memory with both
levels in, from the `[ctr] textures` line in the pair's window:

| pair | linear free | largest block |
|---|---|---|
| lavatube + citadel | 13603 KB | 13436 KB |
| finalboss + citadel | 17969 KB | 13580 KB |
| maincave + robocave | 14073 KB | 13904 KB |
| maincave + darkcave | 18364 KB | 14092 KB |
| sunken + sunkenb | 21789 KB | 21656 KB |
| village3 + ogre | 9769 KB | 9612 KB |
| snow + village3 | 14089 KB | 9848 KB |

No `could not be created` and no parts left out: the vista tier (15-690 KB per level) fits
everywhere, nothing to switch off. (The Oct 3 estimate of -4.5 MB for lavatube + citadel predates
ETC1 merc textures.) The tighter memory at a level load is the C heap (~3 MB free at the peak), see
"Level loads that stop the game".

### The malloc heap (AI-assisted, Oct 12)

On the New 3DS the newlib heap is 72.8 MB; the EE memory takes 48 MB of it. In the Oct 12 hardware
session, loads of `maincave.c3l` and `lavatube.c3l` failed with `bad_alloc`. Lava Tube's background
then came ~40 s late, and later the game stopped with `abort` in Spider Cave: the `LOAD_MUSIC` for
maincave ran out of memory on a thread-pool thread (`sound_load_job`), and an exception there is
`std::terminate`. A trace build (`cmake -DOG3DS_HEAP_TRACE=ON`, `platform/3ds/port/ctr_heap.cpp`)
logs who holds the heap at every `.c3l` load (`[heaptrace]`; map the addresses with
`arm-none-eabi-addr2line -e gk.elf`).

What held the ~17 MB outside the EE memory (Azahar, lavatube + village3):

| what | KB |
|---|---|
| IOP thread stacks (libco, 9 x 512 KB; the deepest used 2.8 KB) | 4608 |
| `CtrVram` (the GS memory) | 4096 |
| libctru thread stacks (14 threads) | 1305 |
| sound banks (the samples) | ~1760 |
| level data of the two levels (`CtrLevelData`) | ~1600 |
| RAMDISK (VIS files) | 811 |
| renderer buffers (sprites, merc, sky...), log ring, allocations under 2 KB | ~2600 |

Each level load also took a few big blocks for a moment: the reader's two 256 KB windows, all of
a level's merc indices (up to 250 KB), and, for a sound bank, the file plus a copy of its samples
(2 x 550 KB for music). Each load also left the free memory in smaller pieces.

Changes:
- IOP thread stacks are 64 KB each. `[iop] thread stacks used` logs how deep they went (a warning
  past half).
- The `.c3l` reader's windows, block caches and zstd decoder are made once at startup and kept
  (`c3l::Reader::Buffers`, ~640 KB). Merc indices are read per model. Detail reloads and repairs
  read each chunk's indices straight into its index buffer.
- A sound bank keeps the buffer its file was read into: one block of the file's size, not two.
- Failures don't stop the game. A sound bank load waits and tries again (every 500 ms, up to
  20 s). A failed `.c3l` load (also a detail reload or repair) fails the job, and
  `retry_failed_loads` loads the level again when the heap's largest free block has grown by
  256 KB (checked every 30 frames, at least 1 s after the failure), or after 10 s. Before, it
  waited until the level's background was drawn.
- Every failed malloc logs one line (at most one a second, from the `--wrap=malloc` wrapper):
  `[heap] N bytes (malloc) failed: free ... KB (... in the arena, ... never used), largest free
  block ... KB, in use ... KB of ... KB; called from <addresses>`. The largest block comes from
  walking newlib's free lists, not from test allocations.
- Host test hook: `OPENGOAL_CTR_FAIL_LOAD=NAME[:S]` fails NAME's `.c3l` loads for S seconds.

The same pad script in Azahar (village1, maincave + village3, lavatube + village3,
village3 + ogre, maincave + village3 again):

| after the load of | in use before / after (KB) | free before / after | largest free block before / after |
|---|---|---|---|
| village3 (with maincave) | 65416 / 62097 | 7619 / 10930 | 5347 / 10476 |
| lavatube (with village3) | 66250 / 62824 | 6785 / 10203 | 3871 / 9365 |
| ogre (with village3) | 66106 / 62767 | 6929 / 10260 | 3399 / 9610 |
| maincave again | 66184 / 62856 | 6851 / 10171 | 2890 / 8963 |

The cost: reading merc indices per model adds 1 SD card request and ~250 KB per level (maincave
36 -> 37 requests).
