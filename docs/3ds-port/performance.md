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
  GPUREG_VTX_FUNC (citro3d after every draw). (AI-assisted, Oct 12) Nothing public gives its
  size or replacement policy (3dbrew and GBATEK list only the register; citro3d never writes it;
  copetti's 3DS article: the vertex processors' outputs go to a "Sync Control" block of 384 bytes,
  32 96-bit vectors, i.e. ~10 vertices of 3 output registers, which may or may not be it). Azahar
  has a 32-entry cache of its own design (an emulator speed-up, not a model). The level draws are
  indexed triangle lists, the cache cold at every draw: c3l v15 orders each run's triangles for
  a FIFO of 4 to 16 entries (c3l_format.md "Vertex order"); pica_bench B4 (B4.cdef.kN: N vertices
  each used twice in a row of 2N indices) shows the real size: the time per index jumps where N
  passes the number of entries (then the second use misses too).
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
| merc | 54 / 3 | 36 / 3 | `ctr_skin_opt` 37 / 3 | weights unscaled (clip x, y, z columns / 255), color scale in the light colors; (far-fog, Oct 17) +3 for merc_fog (dph + 2 clamps, texcoord1 in texcoord0's .zw) |
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

## The GPU's fixed cost, static command lists, half-rate updates (AI-assisted, Oct 12, gpu-rt)

**gpu_breakdown = on** (profiling, off by default; config_ini.md). The hardware logs fit GPU busy
≈ 10.6 ms + 0.13 ms per 1k triangles; nothing said what the 10.6 ms is. Every 4th frame (pipeline
on) each part of the frame is its own GX queue entry that runs alone: clear (the two memory fills),
sky (the sky_last hook or the sky bucket), level, merc (+ eyes), trans (tfrag-trans / sky-blend),
shadow, ocean, sprite, distort (the in-frame texture copies), hud (sprite HUD group, debug,
subtitle, the PMODE fade), transfer (display transfer + the steady pacing fence copy, block32's
crop copy), other (the frame start before the first bucket). Each part is timed from its submit to
its GSP event, taken on the GSP event thread (P3D for a command list, PSC0 / PSC1 for the fills,
PPF for copies and transfers); an almost empty list (a framebuffer flush and the end) is timed the
same way every measured frame: the per-list submit overhead, included in every part. The split
adds a framebuffer flush per part (as the pipeline's partial submits). The CPU waits for each part,
so the game runs slower while it is on. `bench_report.py gk.log` prints the table per level and,
over the windows, each part as fixed + per 1k triangles (the triangles of the `render ms/frame`
line of the same window): what's left of the 10.6 ms at 0 triangles, by part.

Azahar (no GPU time; checks that every event comes): village1 + beach, 37 frames per 15 s window:
every part's list completes, no timeouts; the second fill unit's PSC1 event never comes there (the
clear waits 2 ms for it at most, then takes PSC0's tick; the line counts the frames that had it).
The hardware bench must say: the fixed parts (clear, transfer, overhead x lists, sky) against the
10.6 ms, and whether PSC1 comes.

**static_cmdlists = on** (default). The level renderer hands each pass (vista, main) to
`ctr_gpu_draw_mesh_calls` as one list. It is cut into stretches of raw draws with one program (a
draw that switches the program, runs the sky_last hook, binds a texture that isn't a level pool's,
goes through citro3d or draws nothing is drawn as usual and starts a new stretch). A stretch drawn
alike in two frames in a row is recorded: its words, where its matrix uploads are, the trackers'
state before and after (g, s_raw, the citro3d context's effect / texenv / texture unit 0 / buffer
config / flags), the settings that change words (fog_alpha, tex1_gating, cmd_dedup*, mip settings,
early depth, cull, cb_read_opaque, sky_last depth, fog color, program set ...), and the statistics
it adds. A later stretch whose draws (state, texture address / size / format, mesh buffer
addresses, index range, nofog, which draws share a matrix), state before and settings are the
same is copied in, each matrix upload written with this frame's matrix (and shader_opt fog rows),
and the trackers set to the recorded state after. The GPU reads the frame's command buffer as
always (copy, not GPUREG_CMDBUF_JUMP: 3dbrew only names the "kick" registers and says buffers are
16-byte aligned; a call / return use is undocumented and a list the GPU never finishes hangs the
console; it would save the copy, ~0.1 ms). So no GPU lifetimes: pipeline, frame_overlap2, level
unloads and detail / vis changes only make stretches differ (then they are drawn and recorded
again). Recordings live in one 512 KB block of linear memory taken at boot (no fragmentation of
what level loads need), filled from the start again when full; the heap holds ~8 KB per pass cache
(~4 in use with two levels) and a 16 KB scratch buffer.

`static_cmdlists = compare` copies and then draws every stretch the per-frame way and compares the
words, the state after and the statistics: Azahar, village1 idle / turning and walking / the
Snowy fort, two level loads: 0 mismatches in 6396 copies checked (two runs; the second after the stretches stopped keying on a sky / ocean texture left bound before them).

Azahar, render thread (5 s windows, the same views, copies vs. `set static_cmdlists off`):

| view | draws copied | l1/l0-tfrag build ms | render build ms | command KB |
|---|---|---|---|---|
| village1 idle | 532 of ~551 per frame | 3.97 vs 4.36 | 10.41 vs 10.75 | 156 both |
| Snowy fort idle | 259 of ~266 | 2.86 vs 2.98 | 10.81 vs 10.90 | 148 / 147 |
| village1 turning, walking | ~0 | (no saving; checking costs a little) | | |

The level renderers' build time is mostly culling, not the command words: the copies save ~0.1-0.4
ms per frame of the render thread when the view holds still, nothing on the GPU (same words).

**Half-rate updates.** `ocean_tex_half = on` (default): the flat / waves ocean's 32x32 shading
texture every second frame (the heights every frame: the near grid moves with them); host
screenshots at the beach with it on and off look the same. Not done, and why:
- the sky: its time of day blend and the cloud texture are already made every 4th frame
  (CtrSky::blend); its draw depends on the camera, so a reused picture would slide against the
  level when turning, and a camera-independent sky texture redrawn every frame costs a full screen
  pass, more than the clouds' pass it would save.
- shadow volumes: the game sends them in camera space, so last frame's volume is wrong as soon as
  the camera moves, and the stencil is cleared every frame, so a volume left out leaves its shadow
  out (flicker at 15 Hz); the render thread's part of making them is ~0.2 ms in all.
- the eyes are composited only when their sprites change; ocean = full has ocean_texture_rate = 2;
  envmap shine has no per-frame texture.

## Micro-optimisations of the GPU backend (AI-assisted, Oct 17, micro-gpu)

Branch micro-gpu: smaller command lists and less render-thread work for the same GPU state at
every draw. `config.ini gpu_micro = off` turns all of it off (the safe preset's line), except the
float24 conversion (bit-identical, no key).

| change | what | command words per frame | render thread (Azahar) |
|---|---|---|---|
| raw_f24 without branches | f32 -> float24 (matrices, merc bones and lights): 54 instead of 91 ARM instructions per raw_pack_f24 (4 values), same result for all 2^32 inputs (`platform/3ds/tests/f24_test.c`) | 0 | Snowy fort paused build 12.03 -> 11.53 ms, beach 11.91 -> 11.69, village1 13.24 -> 13.06 (with gpu_micro = off: only this) |
| shader_resident | the 18 vertex programs of both program sets in one code image (351 of 512 instructions, 49 of 128 operand descriptors, MAD's < 32), uploaded once; a program switch writes only the entry point and the output map | -600 (15-17 uploads of 7-34 instructions + descriptors per frame -> 0) | small (shaderProgramConfigure without the code) |
| texture unit 0 registers | a raw bind writes only the registers that change (mostly the address; one burst over the changed span when shorter), with cmd_dedup | -280 to -430 | |
| GAS_DELTAZ_DEPTH | written only when the depth function changes (not for a write mask change), with cmd_dedup | ~-70 | |
| procedural texture tables | the glows' LUTs (128 + 2 x 256 words) stay in the GPU's LUT memory when the unit goes off and on between glows and other draws; uploaded only for another table (or after the HOME Menu / pica_bench) | -640 per glow after other draws | |
| mesh scales written raw | the c3l v15 atlas draws' texture coordinate scale: the same 8 words C3D_UpdateUniforms writes, without running C3Di_UpdateContext (~2000 instructions); static_cmdlists can record a stretch with an atlas change again | 0 | village1 paused build 13.06 -> 11.95 ms: static_cmdlists copies 534 instead of 19 of ~540 level draws (each atlas change had stopped the stretch from being recorded) |
| fence words in the vertex buffer | steady pacing's frame number for the fence copy flushed with the vertices | 0 | one GSP flush request less per frame (village1 7.6 -> 6.5) |
| near flush ranges merged | deferred cache flushes of the linear heap less than 8 KB apart in one GSP request | 0 | (few in these views) |

Words per frame (Azahar, paused, the same frame with everything off and on, see below): village1
38790 -> 37444 (-3.5%), Snowy fort 35754 -> 34076 (-4.7%), beach 31794 -> 30108 (-5.3%). Program
switches with a shader upload: 15-17 -> 0 per frame.

**Same GPU state at every draw.** `cmd_report.py --kicks A.bin B.bin` decodes two command dumps
into the GPU state at each draw: every register (byte masks applied; triggers left out), the float
uniforms, the fixed attributes, the LUT memories and the vertex program the draw runs, resolved
from the entry point to `end` with each operand descriptor index replaced by the descriptor
(programs at another place in code memory compare equal). Memory a frame doesn't write (the
resident shader image, the procedural tables kept from frames before) is in the dump file's
prologue ("CMDE"). `gpu_micro_ident = on` makes each cmd_dump request dump frame k with
gpu_micro off and frame k + 2 with it on (the same vertex buffer copy; the game paused):
`platform/3ds/tests` has no pad for it, the micro-gpu runs used village1-hut, snow-fort and
beach-start paused with `set cmd_dump on`. Result: every draw paired, the same state at all draws
but those where the paused game itself changes between frames k and k + 2: a sky texture's other
copy (TEXUNIT0_ADDR1, 2-19 draws), a few animated merc models' bones (vsh_f, 9 draws), immediate
vertices further along the vertex buffer (ATTRIBBUFFER0_OFFSET, 20 draws); the same draws and
registers differ when both frames are off (gpu_micro = off with gpu_micro_ident: the noise floor).

On hardware (not measured; nothing here changes what the GPU computes):
- the program switches without a code upload: `pica_bench` B2.prog_sep (a switch between two
  shbins, with the upload) against B2.prog_entry (two entry points of one shbin) is the price of
  the upload per switch, 15-17 per frame;
- the command words: B2's per word price (B2.unif_f32 - B2.unif_f24 per word), ~1.3-1.7k words per
  frame;
- a GSP flush request is an IPC (tens of microseconds on hardware, the render thread waits).

Risks: the resident image puts programs at code offsets other than 0 and descriptors at other
indices (the shbins never did that on hardware, the shader_groups entry points didn't either);
merging refuses anything but plain arithmetic, `mov`, `mova`, `nop`, `end` and `mad` (no flow
control, check_shaders.py refuses it anyway) and keeps MAD's descriptors below 32, else it logs
`[ctr] shader_resident: ... off` and the shbins are used as before.

Leads that need files outside the backend: merc palettes that are windows of one bone array
shifted by one bone (consecutive draws of a model re-upload ~150 words of bones each; CtrMerc);
the level draws sorted by texture across chunks re-upload the 18-word chunk matrix at almost every
draw (534 of 716 draws in village1; CtrLevel's order).

## The renderers' own CPU code, instruction by instruction (AI-assisted, Oct 17, micro-cpu)

**What Azahar's milliseconds are.** Azahar counts emulated time per guest instruction: a loop of
10 integer adds, VFP multiplies, conversions, loads or stores takes 12 system ticks per iteration
(268 MHz ticks: 1 per instruction, 2 for the loop), a VFP compare + `vmrs` pair 3.4, a core <-> VFP
`vmov` 2 and a `vdiv` 15 (measured with inline assembly in the 3DS build). So an Azahar ms is
~268k instructions, cache misses and pipeline stalls don't count, and an SVC (svcGetSystemTick,
GSPGPU_FlushDataCache) costs much more than its instructions: timing probes with
svcGetSystemTick around small pieces inflate what they measure (~1.5 us per probe pair). Runs of
the same build in the same view repeat to 0.01 ms (the stats lines' resolution); the Snowy fort's
merc time moves by up to 0.3 ms between runs of one build (what the yetis do).

What changed (all output identical; Azahar build ms/frame, 5 s windows, Sandover hut / Snowy fort,
before -> after the change):

| change | where | Sandover | Snowy fort | identity |
|---|---|---|---|---|
| palette float24 words in ~10 instructions per value (CtrMicro.h) | merc | 1.88 -> 1.66 | 4.50 -> 3.36 | host test, all 2^32 floats + 200k palettes |
| bad bone test on the bits, OR-ed over the bones | merc | 1.66 -> 1.63 | 3.36 -> 3.29 | host test vs the old loop |
| palette rows read once, mirror test on the bits | merc | 1.63 -> 1.59 | 3.29 -> 3.19 | host test, 4M bones |
| ctr_settings() once per model / frame | merc, sprite | 1.59 -> 1.58 | (noise) | the same object |
| texture-sorted draws as 64-bit sort keys | merc | 1.58 -> 1.58 | 2.81 -> 2.79 | in-run: 0 of 19235 orders differ |
| model name lookup cache, name read in place | merc | 1.58 -> 1.55 | | in-run: 0 of 248650 lookups differ |
| bone snapshot without zeroing each matrix first | game thread | 0.152 -> 0.110 | 0.280 -> 0.176 | the same copies |
| one chunk loop per mode, matrix in registers | sprite | 1.39 -> 1.13 | 1.17 -> 1.03 | in-run: 0 of 64916 chunks differ |
| chunk sprite data read in place (no 6 KB copy) | sprite | 1.13 -> 1.02 | 1.03 -> 0.92 | the same bytes |
| draw states compared as four words | sprite | 1.02 -> 1.01 | 0.92 -> 0.90 | the same bytes compared |
| state made again only when its AdGif words change | sprite | ~0.005 | ~0.01 | in-run: the two tests never disagreed |
| eye texel columns once per sprite | eyes | (blinks vary) | | in-run: 0 of 7954 sprites differ |
| ocean: transition / tile loops without reloads, wave texture loops | ocean | 2.17 -> 2.04 | | host replay: draws and texture byte for byte; in-run (smultt): 0 of 6.0M texels |
| texture page info: only the fields kept | tex | 0.16 -> 0.11 | 0.29 -> 0.18 | in-run: 0 of 4.06M textures noted |

(AI-assisted, Oct 18) "chunk sprite data read in place" read the sprite arrays (the REF
transfers into *sprite-array-2d* / -3d*) while the game thread rewrites them: the render thread
draws frame N while the game runs process-particles and frame N + 1's particle launches (main.gc
display-loop; the PC port copies the chain first, DmaCopier). On hardware, with the render thread on
its own core, the progress menu's frame and the HUD orb's glow flickered and broke (oct18). The copy
before was taken on the render thread too, so it only narrowed the race. Now the game thread copies
the bucket's REF data in CtrRenderer::prepare_frame (CtrSpriteRenderer::snapshot, next to the bone
snapshot; ~128 bytes per sprite) and the chunks read the copy. Host runs take the same copy before
their synchronous render and compare it with game memory (a warning line if a copy differs or a
chunk finds none).

In-run checks: a build with the old code kept next to the new (not in the tree) ran both on the
same input every time and counted differences, Sandover + Snowy fort and the level_pairs.pad
route. Host tests: test/ctr_micro (CtrMicro.h against copies of the code it replaced), and
test/ctr_ocean's replay with OCEAN_DRAWS_OUT / OCEAN_DRAWS_REF (the native draws' bytes against
an earlier build's).

Left, outside these files: the skinned draws' command building (ctr_gpu_draw_skinned: ~20 us of
Azahar time per direct merc draw, the clip and light compares, the bone uploads) and the data cache
flushes and GX submits (SVCs, which a wall clock profile of Azahar shows as the render thread's
biggest non-waiting part). The far ocean's time includes the sky when sky_last draws it at the far
ocean's first vertex reservation (SKY_HOOK_ANY).

## Round 2: where the render thread's time goes per draw and per request (AI-assisted, Oct 17, micro-mg)

**Measured with tick probes.** A build with svcGetSystemTick around the backend's functions (not
in the tree; a probe pair costs 352 ticks, subtracted below) in the five views (village1 hut,
Snowy fort, village3 warp gate, sunkenb start, Misty bike), Azahar ticks (268 per us, ~1 per
instruction):

| what | per call | per frame |
|---|---|---|
| GSPGPU_FlushDataCache (an IPC to the GSP) | 16.7k ticks (62 us) | 10-35 (Snowy 23, Misty 32): 0.6-2.2 ms |
| pipeline_submit (vertex flush, deferred flushes, GX_ProcessCommandList) | 40-130k | 3-5 |
| ctr_gpu_draw_skinned (merc) | ~2.1k (without its nested calls' probes) | 115-195 |
| apply_state_tint (full path) | ~650 | 140-400 |
| raw_skin_rows_f24 (a palette upload) | ~250 | 150-180 (almost every merc draw) |
| raw_draw_elements | ~110 | |
| C3Di_UpdateContext after a program switch | ~3.6k | 12-18 |

The flush requests were the biggest item: almost all of them came from CtrLevel's time of day
recolor (one ctr_gpu_mesh_flush per chunk, tod_recolor; a Misty frame had 37 deferred ranges,
0.4-0.7 MB each, ~2.5 MB in all), a request each where the ranges were more than 8 KB apart.

**What changed** (gpu_micro, bits 128 / 256 / 512; `set gpu_micro_mask N` switches them in one
run: the numbers are 127 (round 1 only) against 1023, two segments each per view, the windows that
start inside a segment):

| change | what | identity | Azahar |
|---|---|---|---|
| raw glows (MICRO_RAWGLOW) | the procedural texture sprites were citro3d draws: the whole attribute buffer table, fragment operations and both combiner stages, C3Di_UpdateContext's uniform scans, per glow. Now raw like every other sprite (the proctex unit and unit 0's switch-off still through citro3d at raw_sync); unit 1's gating left as the citro3d draw left it | register-equivalent (--kicks) | ~2k ticks less per glow (sprite quads draws 4.1k -> 3.5k average with 2 glows in 7 in village1); 1-2 glows per frame in these views |
| texture-only state (MICRO_TEXSTATE) | a raw state that differs from the last full one only in its texture (or filter / clamp, which only the bind reads), with the same mesh, tint, texture kind and settings while the context still has what that one wrote: only the bind | the same command words | 45-84 of 160-240 states per frame; 25-77 ticks less per apply_state on average. A version that memoised the combiner and fragment parts separately hit more often but its checks cost more than it saved (+50-80 ticks per call): dropped |
| flush gap 64 KB (MICRO_FLUSHGAP) | deferred ranges of the linear heap up to 64 KB apart in one request (was 8 KB), and a range that finds the queue full (48) joins a near one instead of a request at once | the same bytes flushed, or more | flush requests per frame 127 -> 1023: village1 21.9 -> 18.2, Snowy 25.5 -> 21.2, village3 9.5 -> 7.7, sunkenb 9.2 -> 8.7, Misty 29.9 -> 21.0 (62 us each) |

Render thread build ms/frame, 127 -> 1023: village1 14.33 -> 14.07, Snowy 14.91 -> 14.51,
village3 16.96 -> 16.75, sunkenb 21.72 -> 21.84, Misty 19.62 -> 19.45 (the level renderers'
time moves by 0.3-0.5 ms between segments of one build: the time of day recolor's budget; the
partial submits' flushes are inside build). merc 2.72 -> 2.53 (Misty), 2.70 -> 2.59 (sunkenb).

`ctr_gpu_flush_range(addr, size)` (ctr_gpu.h) queues any dirty range for that merged flush before
the next submit, for the renderers that flush by themselves (CtrLevel's recolor is being changed to
pass its dirty ranges: the ranges' bytes matter on hardware too, ~2.5 MB per frame in Misty).

Same GPU state (cmd_report.py --kicks, gpu_micro_ident = on, paused, five views): the differences
are those of the noise floor (a sky texture's other copy, animated merc bones, immediate vertices
further along the buffer) and two to four immediate draws that one of the two frames has and the
other not (in either direction: the paused game's content, not the change).

Checked and left:
- raw_f24 with CtrMicro.h's formula: the same instruction count (55 vs 56 for raw_pack_f24 on this
  compiler): nothing to gain.
- merc palettes: consecutive uploads have 4-6 bones; 1-5% of the vectors are already in their slot
  and 1-2% are the previous palette shifted by one: uploading only the changed rows doesn't pay.
- the clip and light compares per merc draw (~110 ticks): an id would only skip them for draws
  that share the palette with the draw before (~20 per frame).
- sprites written in place: the buckets collect a frame's sprites by state before drawing, the
  copy is ~1-2k ticks per draw call (4-16 per frame).
- GX_FlushCacheRegions (three ranges per GSP command): submitted next to the GSP event thread's own
  submits without libctru's queue lock (a lost command is a hung GPU), and the gxCmdQueue can't
  take it (it counts completions by GPU events, which a flush doesn't send).
- svcFlushProcessDataCache directly: killed the render thread on hardware (Oct 3).

Leads: C3Di_UpdateContext after program switches (12-18 per frame x 3.6k ticks, ~0.2 ms: citro3d's
program configure plus its vertex and geometry shader uniform scans); the merc renderer's own CPU
outside the backend (~2 ms in Snowy); the sprites in village3 (3.4 ms); the time of day recolor's
flushed bytes.

## The level renderer's CPU code (AI-assisted, Oct 17, micro-level)

Measured in Azahar with the camera still: l0-tfrag + l1-tfrag from "build ms/frame by renderer",
minus the time of day lines' ms in the same window ("non-tod", because the time of day recolor is
not part of this work). 5 s windows (stats_period = 5), 4-5 windows per view after `continue` +
target-stance + 300 frames. Village1, Snowy, village3 and misty repeat to ~0.02 ms from run to
run. Sunkenb moved by up to 0.45 ms between runs of the same build (the camera settles
differently: different run counts in the culling lines).

| change | village1 hut | Snowy fort | village3 warp | sunkenb start | misty bike |
|---|---|---|---|---|---|
| before (2754bb2a6c) | 5.97 | 5.47 | 4.05 | 6.18 / 6.63 | 4.59 |
| ctr_settings() inline (pointer read) | 5.97 | 5.47 | 4.05 | (noise) | 4.59 |
| run loop: one copy per (visibility, view test, tie LOD), arrays in locals | 5.27 | 4.94 | 3.61 | 5.43 | 4.03 |
| draw walk tests the chunk inline; partly drawn runs in locals | 5.15 | 4.79 | 3.55 | 5.24 | 3.90 |
| chunk loop: distance factors once per frame, arrays in locals | 5.09 | 4.66 | 3.45 | 4.82 | 3.85 |
| chunk plane test lane by lane (no Vector4f temporaries) | 5.12 | 4.59 | 3.41 | 5.00 | 3.87 |
| VisibleChunk not zeroed by emplace_back | 5.10 | 4.57 | 3.40 | 5.17 | 3.85 |
| run loop per number of crossed planes, planes in registers | 4.91 | 4.45 | 3.33 | 4.97 | 3.73 |
| vista split walk in locals | 4.90 | 4.44 | 3.32 | 5.07 | 3.73 |

In total that is 0.7-1.1 ms per frame of the render thread. Most of it was codegen: every u8
store (m_run_shown, m_run_nomerge, the chunk states) may alias anything, so GCC loaded every
vector's data pointer again after each one, through `lev` and the lambdas' captures.

Tried and dropped: finding a frame's draws from its visible chunks (bit sets of their places in
the sorted lists) instead of walking the lists. A level has only 1-2.5k draws (village1 2112,
Snowy 1841, village3 2466), so after the inline walk there was nothing left to save: ±0.05 ms.

**Identity.** A check build (not in the tree) runs the baseline draw_level (2754bb2a6c, without
side effects: no time of day, no pool priority / fog / unload calls) next to the new one at every
level draw, with the hysteresis state put back in between. It compares the static_cmdlists call
lists of both passes (state pointer, the matrix's 16 floats and 12 float24 words, mesh, index
range, nofog), every statistics counter and the chunk / LOD states afterwards. Over the five views
and their loads (title, village1, beach, snow, village3, ogre, sunken, sunkenb, misty) it found
0 mismatches in ~2.6M calls (l0 + l1, ~8900 level draws), before and after the last two changes.
Host: level_pairs.pad, all 10 pairs loaded and drawn (`--ctr-gfx -cbackend`).

**Still there** (village1 profile after the changes): run_loop is still the largest piece of
draw_level (the plane test's s16 -> float conversions; a float copy of the run spheres would take
8 more bytes per run of heap); then the partly drawn draws' run walk, and the matrices
(make_clip + ctr_gpu_prepare_mesh_matrix, ~400 instructions per visible chunk). The time of day
recolor (0.8-8.7 ms in these views, most of it full blends nearly every frame in village3,
sunken and misty) is tod-fix's.

## Game thread micro-optimisations (AI-assisted, Oct 17, micro-logic)

Round 2 on the game thread's logic (GOAL code, the natives it calls), measured in Azahar over the
five checkpoints with the worst logic time of fps60_sweep.pad (`cp5.pad`: `continue` +
target-stance + 180 frames, then 900 frames walking, fps_cap = 60, stats_period = 5, sections
off): the game thread's "logic" ms per frame, mean of the full 5 s windows. Run-to-run noise is
~0.1-0.4 ms (misty-silo moves most). A gdb-stub pc sampler on the private emulator
(thread 10) and the perf sections (perf_sections = on, which add ~3-5 ms of their own) chose the
targets: the far ocean's render-ocean-quad (~1.5 ms), the collide cache mesh fill (method 27,
0.5-1.5 ms), ripple-apply-wave-table (0.6 ms jungle, 1.3 ms robocave) and the C backend's
128-bit integer helpers.

| change | jungle-start | misty-silo | village2-dock | swamp-start | robocave-start |
|---|---|---|---|---|---|
| before (2754bb2a6c) | 11.77 | 12.91 | 13.77 | 14.20 | 14.19 |
| render-ocean-quad: no clip passes for quads inside all planes, scratchpad stored once | 11.26 | 12.46 | 13.30 | 13.66 | 14.23 |
| collide cache mesh fill: an outcode per vertex, triangle rejected by AND of 3 | 11.10 | 11.90 | 13.11 | 13.29 | 13.82 |
| native ripple-apply-wave-table (no ExecutionContext) | 10.72 | 11.53 | 13.07 | 13.29 | 12.82 |
| goal_c_ops.h: 128-bit integer ops lane by lane in registers | 10.37 | 11.45 | 12.67 | 12.97 | 12.48 |

In total 1.4-1.7 ms of logic per frame (-10 to -12%). The first three are always on (the
config.ini key `logic_micro` was removed on Oct 18; host debugging: `OPENGOAL_LOGIC_MICRO=0`). The
header change is compiled into the game code and has no switch.

- **render-ocean-quad** (~140 quads per frame): about half are inside all four clip planes, where
  every clip pass returns its input; those skip the passes and both scratchpad buffers get the
  copies the passes would have stored. Clipped quads store each pass's vertices once at the end.
- **Mesh fill** (methods 26 / 27 of collide-cache): each vertex gets a 6-bit outcode (above bmax /
  below bmin per axis) the first time a triangle uses it; a triangle's box misses the query box
  exactly when the AND of its three outcodes isn't 0, the min / max test it replaces.
- **ripple-apply-wave-table**: plain byte loads and stores; GOAL calls it directly
  (MIPS2C_NATIVE_IMPL_GOAL).
- **goal_c_ops.h**: pcpyld / pcpyud / pextlw / pextuw are lane moves; pcgtw, pceqw, psubw, por,
  pxor, pand and the word shifts work on each lane's bits (gc_fbits / gc_bitsf are VFP-core
  register moves); pshuflw / pshufhw / psrldq / pslldq / pextlb / pextlh build the words from
  halfwords and shifted words, with the constant controls folded. Before, each went through a
  gc_m128 union in memory and 4-16 iteration byte loops. sphere-in-view-frustum? (the ppach-style
  pcgtw, pshuflw, pshufhw, psrldq, pcpyld sequence, called for every drawable) went from 167 to 77
  ARM instructions; the 154 modules that use these ops lost 11,296 instructions (1.4%). The byte
  compares, packuswb, paddb and halfword shifts (rare in Jak 1) still use the union versions.
  Header only: the generated C and the module hashes don't change, so no goalc regeneration; the
  3DS build recompiles the csrc objects through their dependency files. Host `cmod/*.so` caches
  are keyed by the source hash and keep the old header's code until `out/jak1/cmod` is emptied
  (same results either way).

**Identity.** The natives: OPENGOAL_MIPS2C_VERIFY=1 host runs over cp5.pad, every call against the
mips2c version (memory written and v0, the scratchpad included): render-ocean-quad 582,778 calls,
methods 26 / 27 126,593 / 128,280 calls, ripple-apply-wave-table 4,774 calls, 0 mismatches. The
header: `test/goal_c_ops/lanes.c` compares every changed op with its union version bit for bit,
2M random cases with NaN, infinity, sign, 0 and -1 lane patterns plus every shift amount (0-63)
and shuffle control (0-255), 43.66M checks: 0 mismatches with clang (arm64 and x86-64, -O0 and
-O2) and with devkitARM -O2 (the 3DS flags) run under Unicorn (ARM1176, FPSCR 0x03000000); then a
verify run with all host modules rebuilt with the new header (3.63M native calls, 0 mismatches,
the same positions at every checkpoint).

**Still there**: camera-slave (1.6-3.9 ms with sections on), the joint math and bones, the
collide cache's other methods (32: frag uploads), sp-process-block-2d / sp-launch-particles-var
in village2 and swamp.

## Title pop-in (AI-assisted, Oct 17, title-popin)

The user saw "pretty bad pop in" on the title (the attract camera over Sandover, the day ~20x
faster). Host `--ctr-gfx` frames of the flight (1450 frames, each frame dumped) with a per-frame log
of every distance / LOD decision that flipped for something in view, and auto_detail fed the
hardware GPU fit of each frame's triangles (`CTR_AUTO_DETAIL_MODEL=1`, host only):

| cause | before | after |
|---|---|---|
| tie LOD switches (geometry 3 <-> 0) in view | 109, up to 19.7 px of shape change (palms, huts) | 77, at most 4.0 px (`tie_lod_px`) |
| tie / shrub distance limits in view | 23 | 13 (the rest: things the camera comes nearer to) |
| tfrag detailed / coarse tier switch in view | 1 (a cliff changed shape) | 0 |
| sky texture steps (blended every 4th frame) | visible in Azahar every 4th frame | blended when the weights move (0.31-0.34 ms/frame render thread in Azahar with the title's fast day) |

Azahar (private instance, the user's short config, screenshots every frame): frame-to-frame step
blocks (8x8 px blocks that change in one frame and stay) 5290 -> 741, events of 20+ blocks 23 -> 2.
Triangles (host): title p95 136k -> 147k (+1.4 ms by the fit; auto_detail then shortens tie and
shrub distances off screen), village1 hut p95 88k -> 91k, Snowy fort p95 121k -> 111k (noise: the
distances moved), Snowy start 115k -> 114k. What is left: tie / shrub chunks the camera comes
nearer to (the PS2 fades shrubs; ties have no limit there), the time of day vertex colors stepping
(tod-fix), and on the host only, the sky drawn as hard-edged panels.

**Fire Canyon sky flicker (AI-assisted, Oct 17, sky-flicker).** The table's "blended when the
weights move" was decided per level. make-sky-textures runs once per level with a sky (level 0's
copies in bucket 32, level 1's added in bucket 39, each weighted by the mood interpolation), and a
frame where only one level's weights stepped was built from that level's share alone. Fire Canyon
mixes its sky with village1's or village2's at about 40/60, and the shares step at different
frames as the camera moves. The texture then flipped between the blend (Azahar, top of the screen
~47/33/75), firecanyon alone (~65/38/50, dark red) and the neighbour alone (blue): 20 colour jumps
in 90 frames standing at firecanyon-start, 165 partial blends in 1149 two-level host frames.
Sentinel Beach next to village1 was hit the same way (36 in 317 frames). `CtrSky` now collects the
frame's copies of both levels and decides and blends once, at the next sky draw (the same frame
of latency as before): 0 jumps in Azahar, 0 one-frame texture flips on the host. village2's sky
flashes (update-mood-lightning, sky weights 128 -> 0 -> 64) are the game's own and stay.

## Impostors on by default (AI-assisted, Oct 17, impostors)

config_dev.md `impostors` has the mechanism. What changed on Oct 17, and what it costs:

- **The beach arch depth error.** Root cause: a card's grid point took the depth of the nearest
  triangle whose screen box touched any of the four cells around it. The rock in front of the
  arch put the whole cell's card in front of the arch, which then failed the depth test.
  Now the CPU rasterizes the group's visible 1/w per texel at capture. Each cell gets one quad
  on the plane fitted to that surface, split where it has a depth step, so the card sits within
  ~0.6% of the distance of what it shows. On/off pictures at the arch, Sandover and Snowy differ
  only in animated objects, clouds and a few edge texels.
- **Render target on hardware.** The texture and its D16 depth are in VRAM (Azahar log:
  `render to texture: 512x256 RGBA8 in VRAM A + D16 128x256 in VRAM B`). They are made at
  renderer init, before the level pools take VRAM. The fallback is linear memory. A tile is
  cleared by a draw (never GX_MemoryFill). The framebuffer band offsets are 16 KB multiples.
  VRAM free in the level pairs: 1.19-1.68 MB. Linear free: unchanged (tightest ogre+village2
  3.67 MB).
- **Time of day.** Cards recapture when the palette weights move the group's colors by more
  than 4 units. They don't capture while the level's rewrites lag. While the day runs too fast
  (the title screen) the groups stay geometry. Before this, Sandover's cards kept stale
  colors, and a near-tie card showed the far LOD version's colors.
- **Popping.** A card is replaced while it is within `impostor_error` (1 px) of the geometry.
  Title flight: 261 pop/flicker events with impostors on, 265 off (popdet.py, 8 px blocks);
  1 event of 2 blocks only with impostors on. A walk and turn at the beach: 20 events on,
  68 off. No crossfade needed.
- **Cost (Azahar, steady).** The impostor code takes 1-3 ms of render thread time (village1:
  250 cards, ~0.4 ms of that drawing them). Village1 is the worst view, at +4 ms render
  thread build. The GPU model (10.6 ms + 0.13 ms per 1k triangles) saves 0.5-1.5 ms in the
  three views, and the cards' fill is the same as the geometry they replace. The views stay
  GPU-bound, so the frame time gains only a little. Most of the win is the triangle count
  (-18% beach in Azahar) and the headroom in busy views.
