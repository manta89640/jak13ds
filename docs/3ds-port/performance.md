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
  voices, on core 1 (the system core, with a share of it), outside its lock (`3ds_build.md`
  "Mixing outside the lock"); `sound_core = 2` puts it on the render thread's core.

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
(COLORBUFFER_READ = 0, POST_VERTEX_CACHE_NUM 0x4 / 0x84; the 256x416 block32 target's B8.b32 run
went with early_depth = block32, Oct 29); then the bench's own vertex shaders (program switches, B3) and the game's
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
  position format (a converter change) would pay, B4 whether a bigger
  POST_VERTEX_CACHE_NUM would (keeping the cache across draws, vcache_reuse, broke the HUD on
  hardware and is gone: see "The red HUD shard").

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

### Level loads while the game plays (loadsmooth)

A neighbouring level loading in the background cost one frame at a time 40-65 ms (Azahar, the
route `reports/nov09/tools/vram.pad`, `hitch_ms = 38`). What those frames were:

| what | in one frame | why |
|---|---|---|
| `__pc-set-levels` | 11-32 ms | the game waited for the render thread in the middle of its frame, every time the level list changed (a level discarded, a level loaded) |
| the tfrag texture page's `upload-now!` (in `link-finish`) | 14-20 ms | 10-16 ms of the same wait, 4-5 ms writing the near textures into the VRAM copy |
| the level's bsp, linked in one call | 5-8 ms | the PS2's linker stops every 0.5 ms; the port had the checks commented out |
| `level-update-after-load` (the login) | 20-35 ms | the PS2 stops it by a timer; the port had that commented out |
| a level's first draw | 17-27 ms of render thread | every time of day group in view rewritten at once (below) |

Now:
- **One budget per frame** (`pc-3ds-load-begin`, `klink.h` `load_budget_*`): the time to the
  frame's flip less 4 ms, at least 2 ms, at most 12 ms. `load-continue`'s loop, the linker's v2
  pointer and symbol passes and the login's units all stop by it and go on in the next frame. A
  level's tfrag texture page (its upload can't be split) and the 1 MB texture move start a
  budget or wait for the next frame.
- **The load steps run last before `dma-sync`** (`level-load-work`, main.gc): the frame's other
  work is done, so the time left is known, and a step that needs the render thread idle waits
  where the frame would wait anyway.
- **The level list is taken at the frame's hand-over** (`ctr_send_chain`), where the render
  thread is idle. The frames drawn see the same list as before.
- **Check:** host, `OPENGOAL_LEVEL_DUMP=DIR` with `OPENGOAL_LOAD_BUDGET_MS=1000` twice and `0.2`
  once: the six levels' heaps at login end are the same bytes (the two runs of one budget differ
  only in the C function stubs' host addresses).
- **Result** (Azahar, same route, `hitch_ms = 35`): no frame of 38 ms or more from the link, the
  login, the texture upload or the level list at the three background loads (before: 5, 6 and
  3). The game side of a load is done 44-73 ms later; the level is there when its `.c3l` is,
  0.6-1.2 s after that, as before.
- **What a load still costs in one frame:** the tfrag page's VRAM write (4-5 ms) and the texture
  move (2 ms), each first in a frame; the GOAL side of a discard (`unload!`, 2.4 ms a level); the
  renderer's unload at the hand-over (1-5 ms a level); the IOP's 8-11 ms on the game's core when
  a DGO or a sound bank starts.

The time of day at a level's first draw: a level that is loaded and not drawn kept the colors of
its load, and a drawn level's groups out of view those of their last draw. `tod_sweep`
(`CtrLevel.cpp`, before each frame, 4000 vertices, after render frames under 26 ms) brings them
up to date, so a first draw, a camera turn or the end of a black finds them a day move behind
at most. While the screen is black a level draw rewrites 8000 vertices of groups; the rest
counts as late in view, so an arrival's black lasts until the colors are there. A day move's
rewrites are spread over 24 frames instead of 6 (the title keeps 6): 7-14 ms in each of 6
frames before, one frame of up to 13 ms and 3-7 ms in the others now.

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

(AI-assisted, gpu-home, Oct 29) **static_cmdlists was removed**: it saved ~0.1-0.4 ms of render
thread time only while the view held still (never in the worst frames that decide 30 fps), held
512 KB of linear memory from boot, mirrored every tracker of the mesh draw path in ~730 lines, and
stopped working after the first trip to the HOME Menu (citro3d's gas dirty bit, fixed now). The
level passes are drawn one by one through `ctr_gpu_draw_mesh_calls`. What it did and measured:

**static_cmdlists = on** (was the default). The level renderer hands each pass (vista, main) to
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
| shader_resident | the vertex programs of the program set in use in one code image (set 1 with the c3l v23 alpha mask programs: 16 programs, 300 of 512 instructions, 45 of 128 operand descriptors; set 0: 13, 351; MAD's < 32), made again when the set changes; a program switch writes only the entry point and the output map | -600 (15-17 uploads of 7-34 instructions + descriptors per frame -> 0) | small (shaderProgramConfigure without the code) |
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

(AI-assisted, Oct 18, hud2) That copy is right but didn't fix the hardware report ("misshapen HUD
elements, flickering light from orbs that jumps across the screen"). A host run with the render at
its latest (`CTR_SOFT_DEFER=1`, 3ds_build.md) shows a clean HUD: no other live read. The cause is
the PICA's float24: HUD models (the orb, power cell and scout fly icons, the orbs flying into the
HUD) are drawn with the isometric projection at GS depth 2^24 - camera z (1024 or 2048 below the
top of the range, or 9999999 for the flying orbs), ~50 GS units deep. A float24 step there is
~128 GS units (64 at the flying orbs, whose camera z of ~1e7 also loses ~150 units in the skin
shader's position), so each model had one or two depths: its back faces drew over its front faces
in mesh order and its envmap shine (GEQUAL against its own base pass) lit them, a speckled bright
orb that flickers as it spins. Azahar computes shaders in 32-bit floats and never showed it.
`CtrMerc.cpp` hud_depth_spread now spreads a HUD model's depth about its center to +-2048 GS units
(16 float24 steps; centers within 2304 of the top move down to 2304, still in front of the HUD
sprites at ~63500 and the world's near plane at 16584), and HUD models further than 32768 from the
camera take their bone translations relative to a point 16384 along the line to their center (the
clip matrix adds it back; same positions, same envmap view direction). Host check with
`CTR_SOFT_F24=1` and `hud.pad`: speckled icons and flying orbs before, the same as the 32-bit
render after.

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

**Title's first seconds (AI-assisted, Oct 22, title3).** "Still on the title screen geometry
changes in the first couple seconds". Every frame from the logo to 3 s after Sandover appears
(host `--ctr-gfx` with `CTR_POP_LOG`, Azahar with a pad script shot every game frame;
port_test/reports/title3) showed four causes at Sandover's reveal behind the logo's falling pieces:
(1) its near chunks were never read while it was hidden (streaming.md "Levels not drawn yet"):
coarse tie groups switching to their near versions on screen (Azahar frames 113-115 after
title-play: the island's hut, grass and palms); (2) the time of day rewrites of its first frames
capped by tod_max_ms (host: 194 groups waiting at the first frame): stale colors for a few
frames; (3) impostor cards coming in by the dozen over the first ~40 frames (0 -> 322) and going
back to geometry when the fast day starts; (4) single tie LOD switches (the switch distance:
lodnear's). Fixes: (1) hidden levels stream around the game's camera; (2) a level shown again
(no draw for 3+ frames) brings its groups up to date within 7 ms a frame for its first 6 frames,
not the budget or tod_max_ms (all at once took 20.2 ms in one frame in Azahar; spread: 9.4 ms at
most; removed Oct 23, todlag: every group drawn is current in its first frame now); (3) no
impostors while the title level is loaded. Azahar, 192 frames from the reveal:
late near chunks 71 -> 0, pop / flicker events (popdet) 37 -> 27 (the rest: the logo's pieces,
the day's colors and sky, single LOD switches). Boot: the title isn't held (no extra time).

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

(Removed on Oct 29 with fine_vis and occ_cull: see "Citadel and the far view, Oct 29" below. The
section is kept as the record of what they did.) What changed on Oct 17, and what it costs:

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

## Coarse ties close to the camera (AI-assisted, Oct 18-22, lodnear)

Hardware report: "coarse models are reloading really close". Two causes, both tie LOD (merc LOD is at
the PS2's distances, impostors start at 120 m, fine_vis is off, the vista tier is far scenery):

- **The switch by screen-space error alone** (title-popin, `tie_lod_px` 4): a group went to geometry
  3 where its shape error fell under 4 px, which for most groups is well inside the PS2 far-plane
  (30-100 m; the PS2 morphs toward geometry 3 only up to there). Groups whose error distance is
  nearer than their far-plane: village1 1107 of 1905 (p10 12 m), misty 994 of 1696, beach 370 of 815,
  jungle 299 of 792.
- **The late-data hold** (streaming): groups drawn far in view because their near chunk was late
  stayed far up to 4 s after it came. After a continue, jungle groups went detailed only at 3-5 m.

Fix (runtime only, no reconvert; `CtrLevelData::lod_switch_error`): a group is near within
max(error x focal / tie_lod_px, PS2 far-plane) x detail_scale + radius (where the PS2 distance
decides, the far version's error at the switch is under tie_lod_px). Stream units' bounds include
the floor (`need_d`, at load). Units a group needs by shape error go first and may free floor-only
units at the arena's capacity. The hold is 1 s, and none where the far error is 3 x tie_lod_px or
more. ctr_vis_builder and the fine_vis check assume the floor (detail_scale <= max(1, the set's)).

Host walks (`platform/3ds/tests/lod_walk.pad`, CTR_LOD_LOG=1; CTR_LOD_NO_FLOOR=1 is the old rule):

| | before | after |
|---|---|---|
| switches to far in view nearer than the PS2 far-plane | beach 1 at 15 m (PS2 60), village1 1 at 30 m (65) | 0 |
| jungle switches to near in view inside half the far-plane | 18-22, nearest 3-5 m | 0, nearest 53 m |
| other levels' switches to near | 9-49 m inside the far-plane | at it (59.x of 60 m) |
| frames with groups kept far after their data came (per ~15 s window) | 106-117 | 0-29 |

Cost: host fixed views (tie_views.pad) village1 hut 105.1k -> 106.7k triangles, beach start 44.0k
-> 44.7k (+0.1-0.2 ms on the GPU model 10.6 ms + 0.13 ms per 1k); walks +0.7-5k tie triangles; jungle
the heaviest at 26.6 ms model. Near data needed at once +10-40% (beach peak 4.3 -> 5.9 MB, within
the arena's growth). Azahar (private instance, sound off, the user's config, stats_period 5, the
village1 + beach part of lod_walk.pad): village1 walk 115.5-117.6k triangles, render thread build
18.2-18.8 ms, GPU busy 19.1-19.8 ms, model 25.6-25.9 ms; beach walk 65.6-86.3k triangles, build
15.2-17.0 ms, busy 16.0-17.8 ms, model 19.1-21.8 ms; no late times for lack of room during the walks
(arena 4.1 MB village1, 8.3 MB beach + village1). Oct 17 reference (impostors, Azahar): village1 hut
110.8k triangles. Still coarse: ~1 s after a continue while the area's near chunks load.

## Title flight pops, shrub distances (AI-assisted, Oct 24, titlemove)

User report (Azahar, Japanese): "parts of the geometry pop in as the title sequence's camera
begins to move" (the island's palms, grass and hut). Every frame on the host (`CTR_POP_LOG`, now
also `[popC]` chunk / `[popL]` tie LOD / `[popS]` streaming lines) and one real-time Azahar run of a
measurement build (`cmake -DOG3DS_POP_LOG=ON`: the same lines per frame in gk.log; game sound on,
no screenshots): no near chunk was late, no chunk came in by distance, vis and fine_vis hid nothing
new. What changed on screen was tie LOD: groups switching from geometry 3 to 0 in view as the
flight came nearer, at max(shape error distance, PS2 far-plane) = 35-75 m for the island: 807 in
40 s in Azahar, 80 in the flight's first ~8 s on the host, objects up to 137 px across (the island
rock, palms, grass tufts, the hut). Each is a step in shape (under tie_lod_px) and in shading: a far
version has its own vertex colors (the island rock's underside visibly darker / lighter), which the
shape error doesn't see. (The 1.5 s of host-only "missing" palms at the start is the logo's black
outline covering them, also on Azahar.)

Fix: while the title level is loaded, no group switches nearer than `kTitleLodMin` (100 m;
`CtrLevelData::lod_min_e`, also in the near chunks' need distances, so they are read before the
reveal). The island's groups are near from the reveal on; the flight's first ~8 s have 19 switches
in view, all at 102-113 m. Cost (host, CTR_AUTO_DETAIL_MODEL): +3-10k triangles in the flight's
later parts, stream arena 5 MB. 130 m left 7 switches but cost +14-34k (Azahar: 201k triangles and
28 ms render thread build where gameplay distances drew 138k / 19.5 ms). Azahar after-run (130 m
build, frames 100-400 after title-play shot): no geometry change on the island from the reveal on,
late near chunks 0, 29.9 fps once the shots stopped. Host test: env `CTR_TITLE_LOD_MIN` (meters).

Shrubs (gameplay report: "small things like shrubs pop in when i walk close, but if i walk away
they stay"): shrubs are resident, never streamed (only tie near versions are). A shrub cell
(chunk, ~20 m radius, one PS2 distance class) came in whole once its sphere was within the distance
and stayed up to 1.5 x that while in view (kHoldInView), and an auto_detail change never let one
in while on screen, so a cell culled while the GPU was busy came in only within the old, shorter
distance. Now: shrub runs (a visibility node, up to 8 instances, ~10 m radius) are drawn while
their sphere reaches within the distance (10% hysteresis, `run_dist_in`), no 1.5x hold for shrubs,
and auto_detail holds a chunk on screen only for going, never for coming. Host lod_walk: 12-150
shrub runs hidden per level draw (log `[ctr] <bucket>: shrub runs hidden by distance`).

Also: impostor cards (`ctr_gpu_draw_mesh_stream`) set the texture coordinate scale as the level
meshes do (a card after an atlas draw sampled at 1/32768: far objects missing until they came back
as geometry); the near chunks' bound uses this frame's tie LOD factor too (a cut to another focal
length decided units with the last frame's).

## Time of day every frame (AI-assisted, Oct 23, todlag)

The user saw the title's day/night colors update late and unevenly, and parts of surfaces losing
their tint for a frame or so. Both came from the time of day limits added for 60 fps (tod_max_ms
2, the 1.2 ms budget, a day move waiting, tod_busy_half, title3's reveal spread): on the title a
day move waited in 188-427 of 289-449 level draws, 116-26626 groups per 15 s waited for the cap,
and groups were drawn up to 102 day moves behind or with the file's baked colors (Azahar stats;
host with Azahar's clock: colors 26 frames old on average, 77% of the groups drawn 3+ frames old;
streamed tie chunks and a first-drawn shrub with the file's colors for 1-8 frames, 1431 groups for
2 frames at Sandover's reveal). Now every group drawn is rewritten before its draw (host check: 0
stale vertices in every frame of the title, village1 and citadel), with a whole-table refresh and
a check-free write loop when many vertices are drawn (c3l_format.md "Time of day"). With the target
at 30 fps there is room: Azahar render CPU title 14.1-20.4 ms (was 10.6-18.5), citadel 19.8-24.7
(was 15.8-17.7), 29.9 fps in both.


## Particle launch and callbacks inline (AI-assisted, Oct 24, core-opt part B)

The particle natives called five GOAL functions per particle through native_call_goal (an adapter
thunk into goalc's C and back each time). They now run them in C++ while the symbols of the
function and of what it calls still hold what they held at the first call (otherwise the GOAL
call):

| GOAL function | called by | C++ (file) | host instructions saved per call x calls per frame (village2) |
|---|---|---|---|
| sp-adjust-launch (sp-init-fields!, matrix-rotate-xyz! with sin / cos, vector3s products, the y rotation) | launcher | `native_sparticle_launcher.cpp` adjust_launch | 1956 x 65 |
| sp-init-fields! (3 per particle) | launcher | pc-sp-init-fields! directly (`native_sp_init_fields.h`) | the GOAL wrapper's hop (not measured) |
| sp-get-particle | launcher | `native_sp_init_fields.h` get_particle | 29 x 89 |
| sp-orbiter (quaternion*!, quaternion-normalize!, quaternion->matrix, vector-matrix*!) | process-block | `native_sp_callbacks.cpp` | 261 x 19 |
| sp-relaunch-particle-2d (+ sp-relaunch-setup-fields; the time of day color case stays GOAL: no test pad reaches it) | process-block | `native_sp_callbacks.cpp` | 140 x 38 |

`native_goalc_ops.h` has goalc's C of sin, cos, the matrix-rotate functions, matrix*!,
vector-matrix*!, vector-rotate*!, quaternion*!, quaternion-normalize! and quaternion->matrix (the
same float operations per lane in the same order, vf0 as a multiply by a 1.0 the compiler can't
fold). The launcher's scratch grew by 64 bytes for sp-adjust-launch's local launchinfo.
Host costs: `OPENGOAL_INLINE_INSTR=1` runs the GOAL function and the C++ on alternate calls and
counts each one's host (arm64) instructions; ~141k of village2's 2.3M instructions per frame. arm64
runs goalc's 64-bit C well, ARM11 doesn't: the Azahar numbers below are the real measure.

**Identity**: verify mode (`OPENGOAL_MIPS2C_VERIFY=1`) runs the GOAL function and the C++ on the
same state for every call (`native_verify_inline`: the particle, its sprite, the launch info,
the VU0 random register, pc-rand's generator, the result; all of GOAL memory on one call in
`OPENGOAL_MIPS2C_VERIFY_INLINE_FULL_EVERY`, default 64). level_pairs.pad: get-particle 119k,
adjust-launch 53k, orbiter 239k, relaunch 62k calls, 0 mismatches; mips2c verify 4.67M calls and
sp-init-fields! 841k calls, 0 mismatches. A deliberate error put into each was reported at once
(relaunch's at its 1002nd call, the first whose specs leave next-launcher alone), then removed.

**Azahar** (one run, A/B in the same session: a test-only toggle, -DPARTB_AB=1, the pad's
`log partb-off/on`; village2 dock standing, late-game state; 5 s perf lines, first of each window
left out): logic 13.6 / 13.6 ms with the inline paths (a1, a2), 14.3 / 14.2 ms without (b1, b2):
**0.7 ms less** (5%), steady to 0.1 ms within each window. Host (Mac, game thread instructions per
frame, `OPENGOAL_PERF_INSTR=1`, jak1_calib_prof.pad): village2 2393k -> 2313k, swamp 2184k ->
2100k, jungle 1868k -> 1847k (the scene's own variation is ~1-2%).

## Title chug, time of day at half rate, shrub fade (AI-assisted, Oct 24, fable1)

The user (Azahar, Japanese, `frame_overlap2 = off` as a test): "the game chugs on the title
screen". Azahar, title 100 s, 5 s windows, the user's config with sound on (the mixer on the
render thread's core, 2 ms/frame): render build 20.7-28.5 ms, of which the time of day rewrites
5.0-10.9 ms (the title's fast day moves palettes nearly every frame, so every group drawn is
rewritten every frame: 60-100k vertices, and village1's 22k-entry table is refreshed whole each
time, ~3 ms of it), l1-tfrag without that ~7 ms, ocean 2.5, sprites 0.9; logic 5.5-6.3 ms; 100-215k
triangles (the title flight at auto_detail's far end: ties x3, shrubs 120 m). A few 3-vblank frames
per window at the busiest views. `frame_overlap2` on and off measure the same in Azahar (no GPU
time there); on hardware on overlaps the GPU with the next frame's build: it goes back to its
default (on). It never had anything to do with the red HUD shard (see below).

- **Time of day at half rate on heavy frames.** A level draw whose last frame drew 3x the
  table's entries in vertices or more (the todlag `fresh_ok` test: the title's fast day, citadel's
  pulse) takes the game's new weights every second frame only (`CtrLevel.cpp` draw_level,
  `m_tod_heavy`): the table and the groups are rewritten once per two frames, together, so the
  colors drawn are at most one frame behind the PC's blend (2-8 color units at the title's 20x
  day, the same for every group: no unevenness). Gameplay (a move every few frames, few vertices)
  is unchanged. Azahar, sound on, like windows: time of day 9.4 / 6.7 / 5.0 / 6.9 / 10.9 -> 7.1 /
  4.0 / 4.7 / 4.8 / 7.4 ms per frame, build 23.4 / 20.7 / 21.2 / 21.7 / 28.5 -> 20.4 / 17.5 /
  19.9 / 18.9 / 24.0 ms. The camera-cut peaks (every group behind at once, 13-15 ms in one frame)
  stay. Tried first and dropped: writing every second group per move (alternating by group index):
  only ~10% less, because the whole-table refresh and the per-chunk bookkeeping cost as much as the
  vertex writes.
  Jak 1 now does this on the title only: Rock Village draws 3x its
  table every frame, so in play its lightning flashes (a new palette 2/3 weight every frame) and its
  flames showed every second weight. Jak II keeps every third frame, except when a day palette
  jumps (a flash). A day palette's jump (more than one 64th) is never spread over frames
  (`kTodSpreadFrames`): only the day's own 1/64 steps are. A whole group's rewrite stores only the
  colors that differ, so the lines whose vertices keep their colors stay clean (no FCRAM
  write-back). Azahar, village2-start at 22:00, standing, six 15 s windows: time of day 3.1 -> 4.4
  ms per frame, most in one frame 18.5-20.6 -> 21.9-28.5 ms, build 22.1-23.3 -> 23.3-25.2 ms (the
  flash frames now rewrite every changed vertex in the frame it changes). Then the list room grows
  (below): host model, flash frames after the lists are made, 217k -> 114k vertices written.
- **Shrubs to the PS2's far plane, thinning out** (`kShrubFarK`, `kShrubNearK`, `shrub_run_limit`):
  a c3l v14 shrub chunk's `max_dist` is its PS2 distance class, the middle of the prototype's
  fade (shrub_ps2_dists.inc: full detail to x, the fading version to w). The PS2 shows a shrub to
  w and the PC has no limit at all; with `shrub_scale` 0.8 a run came in at 0.8 x the middle, i.e.
  ~0.6 of the PS2's far plane: the user's "shrubs pop in so close". Now a chunk's shrubs reach
  1.3 x max_dist (~w) and each run (up to 8 instances) has its own limit between 0.55 and 1.0 of
  that (~x to w) from a hash of its index, with the 10% hysteresis as before: the shrubs thin out
  over the PS2's fade zone instead of a patch coming in at once (an approximation of the PS2's
  alpha fade of its trans version; a per-run alpha would need a draw per run with its own
  combiner constant). auto_detail still pulls `shrub_scale` to 0.8 when the GPU is busy.
- **The red HUD shard (solved Oct 25, hudshard).** Hardware only: one triangle of a HUD icon
  stretched off to one side (the progress menu's red shard at the circle button, the red wedge
  over the title menu's frame, the white streak at the pause screen's orb counter), always the same
  elements. The progress-menu shot: the shard's two sharp corners are the circle icon quad's
  top-right and bottom-left corners (vertices 1 and 3, the diagonal the quad's two triangles
  share: indices 0 1 3 / 3 1 2), the third lies ~64-72 px left of its top-left corner, and the
  icon itself is complete: so triangle (0, 1, 3) of a *second* draw of the same quad, with a wrong
  vertex 0.
  - The second draw is the HUD's faint pass (FB_ONLY alpha fail, b1032c8320): since a36940fb15
    (Oct 12) `ctr_gpu_draw_quads_again` draws the first pass's vertices again, the same buffer
    offset and the same uniforms. With `vcache_reuse` (bb1bcbeb74, Oct 3; on by default since
    4b201f1d77, Oct 16) such a draw was the one HUD draw that kept the GPU's post-vertex cache:
    no GPUREG_VTX_FUNC clear, no citro3d's two PRIMITIVE_CONFIG writes after it. Every other
    sprite draw has new vertices, so it cleared.
  - On the real 3DS the faint pass's first lookup (index 0, the oldest entry the first pass left)
    returns a wrong vertex; the newer ones (1, 3, 2) are right. Only single-quad buckets (an icon
    with its own texture: the four button icons, the frame pieces, the orb glow) still have index
    0 in the cache at the faint pass (a bucket of 2+ quads has evicted it), hence always the same
    elements. Why the PICA returns a bad entry rather than the kept one isn't documented anywhere
    (3dbrew / GBATEK only list POST_VERTEX_CACHE_NUM "0x4 or 0x84" and VTX_FUNC "clears it");
    the evidence is that the shard is exactly that triangle and that nothing else differs.
  - Ruled out: the CPU data (the sprite canary of 6034f061fe never fired for these quads on
    hardware in the oct25e log, and the REF snapshot reports 0 copies differ), the vertex cache
    flushes (`flush_vbuf` / deferred ranges cover every vertex before each submit; the faint pass
    reads bytes the first pass just read), the fence words (after the vertices, read only),
    frame_overlap2 (the Oct 23 shot had it off). Azahar keeps no vertex cache across draws, so it
    can't show it; the host neither.
  - Fix: the raw draws clear the cache after every indexed draw, as citro3d does, and vcache_reuse
    is gone (config key ignored, `ctr_cfg_removed`). Cost: 6 command words per raw draw that
    vcache_reuse had saved for consecutive same-buffer draws; no shading work (the cache was only
    kept, never shared between different vertices).
  - The Oct 17 "HUD / progress menu flicker" (b461054860: the progress menu's frame and the orb
    glow breaking on hardware, the day after the Oct 16 default) were most likely the same.
## Ocean sky reflection, particles, pickups (AI-assisted, Oct 25, j1ocean)

- **Sky reflection** (the PS2 ocean's env map pass, `ctr_gpu_ocean_full_draw_env`, see
  3ds_build.md): folded into the textured draw, so no extra triangles or draws; one more texture
  read per ocean pixel (texture unit 2) and a 64x64 reflection texture made again only when the
  env color changes (0-4% of frames). `ocean_near_alpha` default 0.6 -> 0 (the PS2's alpha).
  Host 3DS renderer vs PC renderer at the village1 shore (same pose, noon): the ocean's blue-teal
  and dark streaks now match the PC; before it was a flat lighter cyan.
- **particle_density** default 0.7 -> 1 (every particle, like the PS2); `particles = reduced`
  (the safe preset) keeps 0.7.
- **Pickups always full detail**: power cells, scout flies, orbs, orb caches, eco vents never
  use the generated low LOD (`c3l::merc_gen_lod_exempt`).
- Azahar (private instance, sound off, stats_period 5, defaults; one run): title build 14.4-20.0
  ms, GPU busy 15.3-20.8 ms, 124-163k triangles, ocean build 1.8-3.8 ms; beach-start (camera
  turned to the sea) build 16.6-18.1, busy 17.4-19.0, 61-123k triangles, ocean 3.0; misty-start
  build 12.6-15.4, busy 13.4-16.3, 63-87k triangles, ocean 3.05; 29.9 fps throughout. There was
  no same-run "before" (no switch for the reflection, one run). Earlier documented numbers from
  other builds and configs: fable1 title build 17.5-24.0 ms with sound on (ocean 2.5 ms); Oct 17
  beach-start 14.1 ms and misty-start 11.5 ms, standing. Azahar doesn't model the GPU's fill
  cost: the extra texture read and the extra particles are only priced on hardware.


## Citadel robot cage (AI-assisted, Oct 26)

Launcher 2420 now emits at one quarter density on 3DS (`shield_density = 0.25`;
1 restores the original). It retains the original panel dimensions, randomized ring, flash/fade
sequence and shield-off behavior. Launcher identity comes from the particle ID table, so other
uses of the texture are unchanged and level unload/reload updates the pointer. The global
particle density remains an upper bound.

The birth callback `birth-func-random-rot` runs inline in C++: the original random draw sequence,
matrix/vector operations and quaternion operations, with no intermediate GOAL ABI calls.
Always on (the `shield_native` key was removed on Oct 29); `mips2c_native_off` naming
`birth-func-random-rot` (or all) keeps the generated GOAL callback for debugging. Replaced math functions fall back to
GOAL. Host verify: 4,350 birth calls (68 comparing all GOAL memory), zero differences; overall
mips2c verify 971,892 calls, zero differences. The native birth takes about 24% fewer host
instructions per call including verification wrapper overhead; ARM11 time is measured below.

Azahar, private instance, sound on, the same binary and scripted Citadel view/camera sequence,
six five-second windows after warmup. Original settings `shield_density=1, shield_native=off` (that key is gone now)
vs defaults: logic 16.155 -> 15.863 ms/frame; process-particles 0.737 -> 0.435;
sp-process-block-3d (included in process-particles) 0.463 -> 0.162; launch 0.510 -> 0.438.
Render build 23.18 -> 22.91 ms/frame. Both 29.9 fps. These are scene totals, not shield-only
timings. Azahar does not model hardware fill cost, so this does not establish the real 3DS GPU
saving from the 75% reduction in shield births.

The complete arrival introduction (`green-sagecage-introduction`) was additionally captured in
Azahar every four game frames, with sound on. Capture timing is excluded from the performance
comparison. Evidence and scripts: `port_test/reports/citadel-effects/`.

## Citadel renderer follow-up (AI-assisted, Oct 27)

Real hardware was reported at 25–27 FPS against the two-vblank 29.9 FPS target. This follow-up
uses code inspection and CPU-only regression checks; no new emulator or hardware run was made.
Earlier emulator timing identifies candidates, but does not establish the current hardware
bottleneck or the FPS gain from these changes.

- Time-of-day lighting now tracks freshness per visibility group. Hidden groups wait until
  revealed; newly visible groups catch up before drawing. Full day-color rewrites run first
  and include current light colors, avoiding a second rewrite of the same light vertices.
  Older files with overlapping visibility ranges retain the whole-chunk fallback. The extra
  light stamp is four bytes per group and is included in the load-time heap estimate.
- Static command-cache metadata grows in pages of eight stretches instead of stopping at eight
  total. The recording arena retains its existing 512 KB limit. Allocation failures draw normally
  and retry caching on a later frame.
- Eligible 3D sprites now trim fully transparent texture borders, as 2D sprites already did.
  Trimming interpolates all four homogeneous position components and requires a planar affine
  quad with full 0..1 UV coverage. Repeating/subregion UVs, procedural textures and non-affine
  quads keep their original geometry. Particle counts, opacity and visible coverage are unchanged.
- When render-build CPU time reaches the existing busy threshold, new impostors need an
  estimated 15 frames of reuse rather than four. Held captures are rejected before candidate
  sorting and atlas work. Rejected captures remain full geometry.

Validation: `ctr_render_regression` covers 1,000 randomized lighting/visibility transitions,
duplicate visibility runs, day/light overlap, reveal catch-up and homogeneous sprite trimming
in all 24 UV corner orders. `python3 test/ctr_render/cmd_cache_test.py` compiles the actual cache
dispatcher with CPU stubs under ASan/UBSan and checks 20 stretches, reuse, cleanup and allocation
failure/recovery. Changed render paths also cross-compile with the 3DS SDK. Hardware FPS and
visual validation remain unmeasured.

## Citadel and the far view, Oct 29 (AI-assisted, levellod)

From the Oct 29 optimisation audit (reports/opt-audit: level-cull-lod.md F4-F12, shaders.md, the
user's choice of removals). The user's reports: Citadel slows down on hardware, the final boss's far
environment looks bad, parts of Lava Tube / Citadel are sometimes missing.

- **Time of day pulse hold back on (tod_pulse_step = 4).** The Oct 23 default 0 came from Azahar,
  whose timings don't show the FCRAM misses of the rewrites: the Oct 25 hardware log has ~0.19 us
  per recolored vertex, so each move of Citadel's palette 1 (every ~2 frames) cost 10-20 ms of
  render thread. The new detector (`CtrTodPulse.h`, c3l_format.md "Time of day") needs three
  turning points of 8/64 or more within 320 frames around a steady mean; the title screen never
  holds. Host check (the title flight's and Citadel's weights logged with CTR_TOD_LOG=2, run
  through the detector): village1's day palettes over 1977 title frames (the day moving 1/64 a
  frame): never a pulse; Citadel's palette 1 (40..72, a 128-frame sine): a pulse from its third
  turning point on (~150 frames). The light palettes (village1's 5, Citadel's 4-6) pulse too but
  are never held (their own path, as before).
- **auto_detail counts the time of day for stepping down** (config_dev.md `auto_detail`): two
  15-frame windows in a row with a frame's build (rewrites included) over the target step down, and
  no step up while one is; not on the title screen. Before, the GPU looked idle while the render
  thread recolored, and the ties went out to x3.
- **auto_detail's urgent mode** (distances applied on screen): from the window's average or two
  frames over 33 ms, never one; level load frames don't count.
- **Haze** (`CtrHaze.h`, config_dev.md `vista_distance`): only where the port's distances cut
  geometry. A level with a vista tier (finalboss) was hazed from 300 m for all its geometry (26-63%
  fog color at 1-2 km, the PC has none); now from fog_start x vista_distance (1.8 km) to
  vista_distance, like its vista pass. The same rule for merc models (ctr_haze_row).
- **neighbour_vis with a vista tier:** a neighbour's low resolution chunks went through their vista
  copies without the visibility bits or the detail rule (over the detailed tfrag); now by the rule,
  narrowed by the bits.
- **Small-gap merge:** a shrub piece hidden by its distance is never drawn along with its
  neighbours (paired shrub pops).
- **Smaller ones:** Jak II's distance freeing counts the shrubs' far plane (kShrubFarK) and the
  ties' 1.5x in-view hold; far mode forced with the camera inside a level without a vista tier
  (detail meshes not read back yet) draws its landmark ties out to draw_distance, not 200 m;
  detail_scale 0.1-4, the scale keys at least 0.05; run_culling = off only stops the view test.
- **Removed:** impostors, fine_vis (+ ctr_vis_builder, .c3v), occ_cull, vista_untextured, with
  their code, keys (a note in the log), the safe preset's and gpu_profile = ab's entries.

## Time of day: only the vertices whose color changed (AI-assisted, Oct 29, todperf)

convtex's exact Citadel pulse (bright groups follow every move of palette 1) cost 3.3-5.5 ms of
render thread per frame in Azahar, against 1.3-1.6 ms with the full hold. The PS2 blends every color
every frame (time-of-day-interp-colors in background.gc), so there is no lower rate to match.

- **Per-color change versions** (`Tod::cver`): the version at which a color's rgba last changed.
  A TodMul changes only with its color.
- **Per-group vertex lists** (`tod_make_list`): made as the group is first written whole. Each list
  holds the group's vertices by table entry, in runs by palette mask. A group that is behind writes
  only the vertices of the entries whose color changed since it was written. Runs whose palettes
  didn't move are passed over, and no vertex memory is read.
- **The light pass uses the lists too**, for its moved light palettes. Groups without a list keep
  their light vertices by table entry (one blend per entry, unchanged colors passed over without
  reading the vertex): host model, lavatube's mood light 54% fewer ARM instructions and 67% fewer
  vertex writes in the light pass, Haven City's flames and neon 13% / 24%.
- **The blend adds only the palettes a color isn't black in.**
- **The title screen keeps the old whole-group path.** Its fast day changes nearly every color every
  frame, so the lists don't pay there.
- **Memory.** One room for every level's lists, 384 KB, allocated at the first list. When full it
  grows once to 768 KB while the heap keeps 5 MB free after it (log line "room grown"; Rock
  Village's drawn set needs ~690 KB, and each lightning flash wrote its groups without a list
  whole), else it is emptied, at most every 300 frames. A group without a list is written whole, as before. On top of
  that, 4 bytes per color per level (Citadel 50 KB).
- **tod_pulse_step now 0 by default**: every move, exact as the PC and the PS2. It now costs less than
  the hold did.

The output is the same as the old code, vertex for vertex: `test/ctr_render/tod_diff/run.sh`. It
runs the base commit's time of day code and the working tree's side by side on a real level file.
- **Traces:** Citadel's host weight trace (875 frames) and three random 400-frame traces.
- **Settings:** pulse step 4 and 0.
- **Visibility:** all runs shown, or runs shown and hidden at random.
- **Variants:** an older file's table, and the title screen switched on or off halfway.
- **Levels:** Citadel, village1 and Jak II's atoll each pass 40 of 40 cases. Further runs on the title
  flight's trace, a 6K room (list drops) and every chunk drawn are also exact.

ARM instructions per Citadel level draw (`run.sh --arm`; devkitARM -O2 under Unicorn, mean after 32
warm-up draws, runs shown and hidden at random):

| drawn vertices | full hold (before convtex) | convtex head | this, pulse step 0 (exact) | this, step 4 |
|---|---|---|---|---|
| 118k | 765k | 1006k | 588k | 548k |
| 217k | 1166k | 1299k | 1013k | - |

Vertices written per draw (Citadel's trace, all runs shown, 118k drawn):
- full hold: 19.2k
- convtex head: 28.0k (34.8k at step 0)
- this: 10.2k

Each written vertex costs about 0.19 us on hardware (FCRAM). The lists also read no vertex
memory for the vertices they skip.

The title flight (village1, its fast day, whole groups) costs 715k against 701k (+2%).

One Azahar run (emu-fable, silent). It used a diagnostic A/B build: every 300 frames it switched
between the old path and the lists. Both phases ran without bright groups, because the level loaded
during a step-0 phase. That build had a 256 KB room:
- old path (full hold): 6.3-6.8 ms per Citadel draw, 37-46k vertices
- lists: 7.8-8.0 ms, 38-40k vertices

So the lists were slower there. The host model shows why: that room is too small for Citadel's drawn
set (150k+ vertices drawn in game), so most groups had no list. The same model, with a 217k-vertex
drawn set and that room, gives 1.28M instructions against 1.17M for the full hold, the same ordering
as Azahar. With the 384 KB room and the leaner lists of the final code it gives 1.01M. The final
build was not run in Azahar.

## Time of day: the vertex colors in their own attribute buffer (colorstream)

The time of day rewrites the level meshes' colors in place in FCRAM. With the colors inside the
16-byte vertices every rewritten color filled a 32-byte line of two vertices and wrote all of it
back. Each chunk mesh now keeps the same 16 bytes per vertex as two attribute buffers:
12-byte records (position, the entry in `pad`, texture coordinates) and then the colors
(`ctr_gpu_mesh_split_colors`). The loader reads the file's vertices whole as before and splits them
in place (a transient 4 bytes per vertex of heap). No file format change, no extra memory, the
shaders unchanged: the draws give the PICA two buffers (stride 12 with two attributes, stride 4
with the color; each buffer's attributes add up to its stride), wind and envmapped ties a third.

Host model, `run.sh --arm` with `TOD_ARM_LINES=1` (devkitARM -O2 under Unicorn, every load and
store into the meshes, 32-byte lines; FCRAM traffic = lines touched (filled) + lines written
(written back); 120 level draws, stride 5, pulse step 0, the first draw writing every group whole):

| level, trace | | line moves, mean | most | lines written, mean | most | ARM instructions, mean |
|---|---|---|---|---|---|---|
| village2, lightning | before | 8993 | 104473 | 3879 | 50305 | 538k |
| | after | 4896 (-46%) | 67045 (-36%) | 1305 | 12877 | 535k |
| lavatube, lamps | before | 30034 | 361504 | 15017 | 180752 | 1441k |
| | after | 12544 (-58%) | 226113 (-37%) | 5708 | 45361 | 1422k |
| citadel, its trace | before | 13435 | 92763 | 5888 | 33899 | 1093k |
| | after | 6730 (-50%) | 69382 (-25%) | 2219 | 10518 | 1085k |

- List writes (most frames) read no record and write 8 colors per line: up to 4x fewer line moves.
- A whole-group write (the "most" column: loads, camera cuts, the title start) still reads each
  vertex's entry from the records (12 of the 16 bytes, clean lines), so it fills as many lines as
  before and writes back a quarter: -25..-37%.
- The cache flushes cover 25-29% fewer bytes (the ranges are merged within 64 KB, which now spans
  the records between two meshes' colors).

What is left: the entries for a whole-group write could live in a 2-byte stream of their own (10 +
2 + 4 bytes, about 3x fewer line moves there), but then the GPU's record stride is 10, which no
hardware test has shown to be safe. `pica_bench`'s `B5.split` measures what the second attribute
buffer costs the GPU per vertex against `B5.short`.

Exact: `run.sh` (every vertex of every level draw against the base code) on village1, village2,
lavatube, citadel, swamp, ctysluma and ctyindb, and `run.sh --pixels` (each version's chunks drawn
from its own layout by the host soft renderer every 25th draw, textured, every pixel compared) on
village1, village2, lavatube, citadel and ctysluma: 0 vertices and 0 pixels differ.

## What the GPU time buys: a cost model from the hardware logs (gpuspeed)

**Hardware data.** Every `render ms/frame` window of the Jak 1 hardware logs (oct12, oct25, oct25b,
oct30; 531 windows of 100+ frames) fitted by least squares, with one intercept per log:

| fit (GPU busy ms) | per item | rms |
|---|---|---|
| oct30 alone, triangles | 14.9 ms + 0.136 ms per 1k triangles | 3.5 |
| oct30 alone, triangles + draws | draws add 0.14 ms per 100 (nothing) | 3.5 |
| all logs, triangles | 0.113 ms per 1k | 3.7 |
| all logs, texture binds | 7.5 ms per 100 | 3.3 |
| all logs, triangles + texture binds | 0.033 ms per 1k + 6.0 ms per 100 binds | 3.2 |

Triangles, draws, texture binds and command words move together (r = 0.7-0.99), so the windows
alone can't split the time between them. The oct30 log has the widest triangle range (35k-216k)
and fits triangles best. 60 us per texture bind (15k GPU cycles for a cache clear and a 256 byte
refill) is not a plausible cost of its own: binds stand in for something that grows with them.

**What a frame draws** (host soft renderer, `CTR_SOFT_STATS`, title flight, frames 1200-2310, 38
windows; the same defaults as the oct30 hardware build). Level triangles submitted per frame:
205k (the hardware's title window: 216k triangles in all, 35.3 ms busy, 35.3 active).

| level triangles | per frame | share |
|---|---|---|
| on screen, no pixel center inside (no fragment) | 103k | 50% |
| every fragment fails the depth test | 52k | 25% |
| some fragment passes, none left in the final picture | 21k | 10% |
| culled back faces (cull_ties) | 14.5k | 7% |
| off screen | 8k | 4% |
| near-clipped | 1k | 0.5% |
| a pixel of the final picture | 5k | 2.6% |

(Back faces drawn by draws without culling: 87k, 43%, counted in the rows above too. Triangles
without area in the file's own positions: 1.1k, 0.6%.) No pixel, by bucket: tfrag 46%, shrubs 48%,
ties 38%. Fragments: 4.5 screens in all (level 2.6, sky 1.1, merc 0.7); only 1.2 level fragments
per level triangle.

**Model.** At the title the frame is 216k triangles and ~430k fragments in 35.3 ms. If triangles
cost 33 ns (the binds fit), the fragments would need ~60 ns each (16 GPU cycles), far beyond any
texture fetch, depth test and colour write. So the title is triangle-bound: ~110-135 ns per
triangle (30-36 GPU cycles, 1.1 vertices of a 10-instruction program each) for ~25-30 ms, fill
~5 ms. Village2-like views (55-100k triangles at 26-31 ms) carry more fill per triangle (7-14
screens, perfv2). Confidence: moderate. Hardware numbers that settle it: pica_bench B3 (ns per
vertex per instruction), B7 (ns per triangle, zero area / one pixel), B6 (fill per format,
trilinear), B2.rebind vs B2.rebind_clr (a texture bind with its cache clear), and gpu_breakdown's
geometry / fill split.

**gpu_breakdown geometry.** In the measured frames each part's command list begins by sending the
GPU state its draws use (as after the HOME Menu, but not the framebuffer, viewport and scissor) and
runs a second time under a one pixel scissor: the part's geometry (vertex fetch and program,
clipping, triangle setup) without its fragments. The line lists per part the geometry ms with the
part's triangles, draws and texture binds; `bench_report.py` prints geometry and fill (the part
minus its geometry) per part and level and fits geometry per 1k triangles and 100 draws, fill per
100 texture binds. Partial submits are off in measured frames (they took the level's commands
before its part was timed). Lists that set the scissor or the framebuffer (a render to texture, the
frame's first part) run once only (counted). The top left pixel gets the blended and stencil draws
twice in measured frames.

**What follows, if the model holds.**
- Half the title's level triangles cover no pixel center. On a 400x240 screen the far geometry is
  denser than the pixels: each costs a full vertex and setup slot (~5-14 ms at the title). Only a
  level of detail removes them (not exact). The PS2 itself draws tie geometry 1 and 2 from 8 m up
  to the prototype's far plane (`tie_ps2_dists.inc`); the port draws geometry 0 there for its near
  groups. A per-group screen size switch to geometry 1/2 would be the PS2's own picture.
- A quarter are fully hidden behind nearer geometry (late depth test: their vertices, setup and
  texture fetches are spent). Only occlusion culling removes them.
- Exact options in the GPU backend are spent: 10-instruction programs, 16 byte vertices, vertex
  cache order at its bound (1.06-1.15 vertices per triangle, the converter log), colour and depth in
  different VRAM banks, texture unit 1 off for level draws, closed tie surfaces culled. Dropping
  the zero-area triangles saves 0.6%.
- `auto_detail` reads GPU busy, which counts the idle time between the clear and the first command
  list: Lava Tube / Citadel windows have busy 4-11 ms above active (33.6 vs 27.1), so they aren't
  GPU-bound although busy says so.

## What a gameplay frame asks of the GPU: the host's triangle budget (tribudget)

`CTR_SOFT_STATS=N gk --ctr-gfx ...` also writes `[tb]` lines per window of N frames, and
`platform/3ds/tools/tri_budget.py LOG [--view NAME] [--binds] [--extra] [--tex]` turns them into
tables. A view is the span between a pad script's `log view-begin NAME` and `log view-end NAME`.

- Per bucket (level chunk kind and tier, `+morph` / `+tmorph` / `wind` / `etie` / `shrubfade`
  passes, merc, sky, sprites, shadow; `CTR_SOFT_MERC_MODELS=1`: per merc model): draws, texture
  binds, indices, vertices used, fragments, fragments written, pixels of the final picture.
- Per bucket and view depth band (10 / 20 / 40 / 70 / 120 / 250 / 500 m): triangles by what
  became of them: reaching behind the camera, off screen, culled back face, no pixel center, no
  fragment written, written but covered later, in the final picture.
- Vertices: program instructions and attribute buffers per vertex, a 16-entry post-vertex cache
  cleared per draw or kept, vertices unique by position / texture coordinates / color.
- Textures: fragments by format and mip level, 8x8 tiles touched first after each bind at LOD
  bias 0 / 0.5 / 1, tile reads with a cache of 1 and of 16 tiles, the textures sampled.
- `zties`: opaque fragments at exactly another draw's depth with another color, the only pixels
  a reordering of opaque draws can change.

The host does not draw `ocean = full` in gameplay runs (CtrOceanFull: "unexpected DMA"), so the
ocean is missing from these tables. Results of Nov 9: `reports/nov09/tribudget.md`.

## The tie shine at the PS2's distance (shine)

**What the PS2 does** (`draw-inline-array-instance-tie`, `game/mips2c/jak1_functions/tie_methods.cpp`
block 49; `generic_tie.cpp` multiplies the tint): an instance of an envmapped prototype (flag 2) has
`fade = trunc(clamp((envmap-fade-far + r - d) * envmap-rfade, 0, 128))`, d the camera's distance to
its sphere's center, r the sphere's radius. Fade 0: the plain tie. Else the envmap pass with its
tint x fade / 128. `envmap-rfade` is 128 / the length of the fade, so the shine is full within
`fade-far + r - length` and gone at `fade-far + r`. The PC port multiplies `envmap-fade-far` by
10000 at level login and draws every envmapped tie with its shine.

**The port** (Jak 1):
- `level.gc` (TARGET_3DS) calls `pc-3ds-tie-shine` once per tie tree at its login instead of
  multiplying. `ctr_tie_shine` (CtrLevel.cpp) reads the tree in game memory: the instances of the
  envmapped prototypes under their tie nodes, with the prototype's two values (`CtrShineTable`, kept
  per level until its unload). No level file change: a run's `vis` is its tie node's id.
- `draw_level`: a run's fade is its node's, the largest of the node's envmapped instances' (a run
  holds up to 8 instances of a node; no instance has less shine than on the PS2). Fade 0: the run
  is drawn by the plain level program in its list place. Else through the shine program with the
  combiner's shine constant x fade / 128. Per chunk two camera distances settle most draws without
  a look at their runs: within the first all runs are at 128, beyond the second all at 0.
- Runs without a tie node, levels without a table and Jak II (same fields in its prototypes, but no
  such line in its `level.gc`; the PC draws its envmapped ties at any distance) keep their shine.
- Log: `[ctr] LEVEL: tie shine: ...` per tree, `envmapped ties: N draws, N tris ... with them, N
  tris beyond their shine drawn plain`. Host: `CTR_SHINE_LOG=1` lists the prototypes,
  `CTR_SHINE_AB=full` the shine at any distance, `alt` that in odd frames, `seam` the plain
  program in odd frames and the shine program without shine in even ones.

**Distances** (all 21 levels: 305 prototypes, 4766 instances): Precursor Basin 60 m fading over the
last 20; Citadel 40 / 15 (two prototypes 30-32); Lost Precursor City 25 / 17; Forbidden Jungle 65 /
30 and 90 / 65 (the machine), the plant boss's shafts 10 / 5; Misty 65 / 55; Snowy 40 / 15, its
dark eco barrels 100 / 50; Fire Canyon, Boggy Swamp and the final boss 100 / 20; Geyser Rock 120 /
40; Sentinel Beach 80 / 30; lamps and lanterns 32 / 24; the villages 12-80 (the oracles 65 / 30,
the warp gates 40 / 20, Sandover's sage machine joints 12 / 6); the cave crystals 100-180. The
object's radius (0.5-150 m) adds to each.

**Measured** (host, per frame, camera at rest): Precursor Basin start 26.7k triangles through the
shine program before, 5.7k after (478 -> 83 shine draws; vertex program instructions of the frame
2.23M -> 1.60M; all draws 1105 -> 1123, binds 162 -> 170: the plain runs are draws of their own);
Citadel entrance 10.7k -> 2.0k; Snowy fort 0.6k -> 0.6k (all within their distance; 1.1-1.7k ->
0.3-0.6k with the camera turned).

## Ties at the PS2's distances: its own geometries and its geomorph (ps2lod)

**What the PS2 does** (`draw-inline-array-instance-tie`, the tie VU program; `c3l_format.md` "The
PS2's tie geomorph"): an instance is drawn with geometry 0 up to its prototype's mid-plane
(morphing from the near-plane on toward geometry 2's surface), geometry 2 up to the far-plane
(morphing toward geometry 3's), geometry 3 beyond. Geometry 1 is geometry 0. The far versions the
port drew before were already the PS2's own geometry 3; what cost triangles was where it switched:
the screen-space floor (`tie_lod_px` 1) kept geometry 0 out to max(shape error distance, far-plane),
because switching 0 against 3 at the far-plane popped by 3-19 px. The PS2 shows no pop there because
of the morph. With the morph the port switches at the PS2's distances and nothing shows.

**Triangles per frame** (host soft renderer, `CTR_SOFT_STATS=60`, `platform/3ds/tests/ps2lod.pad`;
before: the same converter with `--no-tie-morph`):

| view | level triangles | ties | draws | est. GPU (0.136 ms / 1k) |
|---|---|---|---|---|
| title flight (18-19 windows) | 189.6k -> 124.7k | 89.5k -> 42.9k | 1183 -> 961 | -8.8 ms |
| Fire Canyon start (6) | 159.1k -> 145.6k (shrubs 47.9k / 53.2k: the camera) | 45.2k -> 22.8k | 789 -> 717 | -3.0 ms (ties) |
| village2 start (6) | 200.1k -> 114.1k | 94.7k -> 41.3k | 1431 -> 994 | -11.7 ms |

Title, triangles with no pixel center 54.9k -> 33.2k, all fragments behind 59.1k -> 41.6k.

**The same picture.** Host A/B in one run (`CTR_MORPH_AB`: on odd frames 1 every group's near
version with the morph, 2 the PS2's versions without the morph; pairs of consecutive frames at the
Fire Canyon and village2 starts, mean |difference| outside what moves between two frames of a
control run): the near version everywhere against the PS2's choice 0.33-0.48 levels, 0.4-0.7% of
pixels > 16, in the diff images on animated things (Jak, birds, the warp gate glow, lamps) and a few
tie spots where geometry 2's own vertex colors differ from geometry 0's (the PS2 changes them
there too); the morph itself 0.49-0.64 levels, 0.5-0.8% > 16, on the rock silhouettes (what a
switch at the PS2's distances without it would pop). A group of a visibility node's instances
instead of one per instance showed geometry 0 for instances past the mid-plane with their
geometry 2 vertices moving on an unflattened surface (7 levels): hence one group per instance.

**Memory.** Files +13-18% (village1 12.1 -> 14.3 MB, Fire Canyon 11.9 -> 13.5, village2 11.9 ->
13.9, beach 9.5 -> 11.0). Near chunks: geometry 2's indices and its vertices that differ from
geometry 0's (colors), 8 bytes of morph records per vertex in the mesh tail (village1 7.7 -> 9.5 MB
+ 2.7 MB records). What streaming needs at once fell: Azahar `needed now` 2.9-6.6 MB at the title
and Fire Canyon, against 8-11 MB in the oct30 hardware log, because the PS2's far-planes (30-140 m)
are nearer than the shape error distances at `tie_lod_px` 1.

**CPU.** Per-instance groups: +50-60% runs tested (title 8.5k -> 13.6k), the morph rows (one sqrt
per instance and band of the visible near chunks), the rows a draw uses sent with it (4-5 per draw).
Vertex program 13 instructions for the near chunks' draws (10 before), +8 bytes fetched per vertex.

Not done: wind instances (palms) keep the screen-space switch (the wind program has no room left for
the rows); the PS2's color morph of the moving vertices (the .fr3 has no indices of the colors they
blend to); Jak II (Haven City would need ~7x the near data at the PS2's distances).

**Title flight, render thread (Azahar, emu-final, same build, late flight window).** The first
version cost 10.7 -> 14.9 ms in the level lists (per-instance groups: 14k runs, 1372 draws walked;
the title's 100 m kept geometry 0). Now: groups per visibility node again (exact through the
two-band morph), the morph draws on the raw command path and after the other draws of their list
(program switches 163 -> 33 per frame), each chunk's rows made at its first morph draw:

| late window | build | l1-tfrag | lists | runs | draws walked | triangles |
|---|---|---|---|---|---|---|
| same build, no morph (`--no-tie-morph`) | 31.9 ms* | 21.5 | 9.7 | 8243 | 1622 | 237k |
| with the morph | 30.1 ms | 20.6 | 8.7 | 9846 | 1554 | 185k |
| the merged first version (lead's h-j1) | 38.0 ms | 27.7 | 14.9 | 14149 | 1372 | 195k |

(*a build with timing counters: ~+0.5 ms.) The rest of the window's 30 ms is the base, not the
morph: the same build without it takes as long.

## The early depth test, checked on the console (AI-assisted, earlydepth)

**What is public.** Registers only (3dbrew, GBATEK): `EARLYDEPTH_FUNC` 0x061 (>=, >, <=, <),
`EARLYDEPTH_TEST1` 0x062 and `EARLYDEPTH_TEST2` 0x118 (enables), `EARLYDEPTH_CLEAR` 0x063 (trigger;
it doesn't touch the depth buffer), `EARLYDEPTH_DATA` 0x06A (24-bit clear value). citro3d writes
both enables together and, copying Nintendo's driver, an `EARLYDEPTH_CLEAR` after the framebuffer
flush at every target switch and command list split. No emulator has the test (Azahar issue #551
is open), so only a console shows what it does.

**What the squares were (hypothesis, ~80% for 8x8 block mode).** The early data holds one depth per
32x32 screen block, filled from the 8x8 tiles the framebuffer unit writes, each write replacing the
block's value instead of combining it. With the usual 8x8 block render target, one near tile (a rock
in front) then stands for its 15 neighbours, and the farther level in the same 32x32 block is dropped
tile by tile: blocks of the screen where the level never draws. The evidence:
- The Oct 2 New 3DS report.
- The Diddy Kong Racing 3DS port's New 3DS snapshot: squares on a 32-pixel grid, made of whole 8x8
  tiles, near ground drawn and farther ground in the same square missing.
- OpenLara's citro3d backend: early depth left off with "TODO block32".
- The register map: 0x062 / 0x063 / 0x06A sit with the rasterizer's registers, 0x118 with the
  framebuffer's. So the test runs before shading, and its data comes from the framebuffer side.

In 32x32 block mode the tile and the block are the same. Whether the value is then the farthest
depth of the whole block (exact) or something less (DKR still lost scenery there) is unknown, ~50%.

**What the game does.**
- **Boot self-test** (`ctr_gpu_early.c`, after the GPU starts, unless `early_depth = off`). One frame
  goes into the real top render target, drawn twice: with the early test on the draws a game frame
  uses it on, and with it off.
  - It holds every event a frame has: the clear, the frame before, a near 8x8 tile, a 4x4 inside a
    tile, a 32x32 mosaic at two depths with a near square last, sloped quads over a covered block,
    an alpha-tested draw and a blended draw without depth write (both with the test off), a render
    target switch, a command list split (flush, invalidate), a depth write without the test going
    farther, the far layer, and the sky at depth 0.
  - Color and depth are read back (GX copy) and compared as 64-bit hashes. The frame is first drawn
    twice with the test off: a readback that doesn't repeat proves nothing and counts as a fail.
    Azahar's depth readback doesn't repeat, so the test fails there.
  - On a mismatch each event runs alone, and the log names the ones that differ (color / depth,
    clear color pixels gained).
  - A 256x416 target in 32x32 block mode gets the same check, for the log only.
- **Gate.** `early_depth` (default on) and the sky's use of the test work only if the 8x8 check
  passed with the configured function and clear value.
- **Rules in a frame.**
  - The early data is cleared after the top target is bound, not before: at the frame begin, after
    every render-to-texture (`rt_end`), and before the next early draw after any depth write that
    may go farther (depth test not >= / >: the ocean's far and mid, GS draws with the test off).
  - Partial submits keep the data (only flush and invalidate), as the self-test's split event does.
  - Only opaque, depth-writing, >= draws whose alpha test can't fail use the test (as before), plus
    the sky.
- **Order.** CtrLevel draws the early draws in one walk bucketed by distance band (<30 m, <100 m,
  farther), each band in its texture-sorted order with its morph draws last. It replaces three
  walks over the list.

**Host estimate** (`CTR_SOFT_EARLY_PROBE=3`, village2-start end view, band order on and off in
alternate 60-frame windows of one run). The model is an exact early test: per 32x32 block the
farthest depth of its pixels, updated between draws.

| | band order on | off |
|---|---|---|
| early draws' fragments / frame | 434k (4.5 screens) | 434k |
| fail the depth test | 266k | 250k |
| an exact 32x32 early test drops | **205k (47%)** | 176k (41%) |
| texture changes / frame | ~61 | ~58.6 |

At the fill cost in gpuspeed's model (~12 ns per shaded fragment), 205k fragments are at most
~2.4 ms of GPU time per frame in village2. The real gain is less: the hardware's test is per tile of
a triangle and its data updates later. The band order costs no CPU over the old path: one walk
instead of two or three (not timed on its own), about 2.5 more texture changes per frame.

**Hardware test.**
1. Boot. In gk.log and boot.txt, find `early depth self-test: block8 ..., block32 ... -> early
   depth on|off`. Expected if the hypothesis holds: block8 FAIL naming tile8 / mosaic / slope, and
   block32 pass or FAIL with its list. Either way the picture is the one without early depth.
2. If it says `on`: in village2, look at the cliffs for 5 minutes for squares or missing blocks.
   Compare `gpu active` / busy ms with a session where config.ini says `early_depth = off`.
3. If block8 fails and block32 passes: next comes a block32 top target without the extra copy (the
   LCD reads 240 of each 256-pixel line through the framebuffer stride). Pass it through the same
   self-test.

## tfrag at the PS2's distances: its three geometries and its geomorph (tfraglod)

**What the PS2 does** (`c3l_format.md` "The PS2's tfrag geomorph"): each tfragment is drawn with
its base geometry when its sphere's nearest point is beyond m0 (70 m of view depth by default),
level0 beyond m1 (50 m), else level1; the VU moves the interpolated points by their own view depth
onto their parents' midpoint, so the surface never switches. The port drew level1 (geometry 0)
out to the draw distance. Now each tfrag group (BVH node) shows the PS2's version by the view
depth of its nearest point and every vertex morphs as the PS2's does (ctr_tfrag_morph.v.pica,
from its own clip w). The near and mid versions stream in near chunks; the level's own chunks
hold the far versions.

**Triangles per frame** (host soft renderer, `CTR_SOFT_STATS=60`, `platform/3ds/tests/tfraglod.pad`;
the same gk on both level sets, the runs side by side; before: `--no-tfrag-morph`):

| view | level tris | tfrag tris | tfrag no pixel | draws | est. GPU (0.136 ms / 1k) |
|---|---|---|---|---|---|
| title flight (19 windows) | 144.3k -> 131.6k | 27.1k -> 15.7k | 8.6k -> 2.3k | 1078 -> 1145 | -1.7 ms |
| Fire Canyon start (6) | 148.4k -> 144.6k | 6.5k -> 2.8k | 3.1k -> 0.9k | 708 -> 709 | -0.5 ms |
| village2 start (6) | 124.2k -> 112.7k | 25.9k -> 15.1k | 11.4k -> 4.6k | 985 -> 998 | -1.6 ms |
| Lava Tube start (6) | 34.3k -> 34.1k | 0.8k -> 0.8k | | 338 -> 337 | 0 |
| Citadel start (6) | 23.0k -> 22.8k | 0.1k -> 0.05k | | 638 -> 625 | 0 |

Lava Tube and Citadel are ties; their tfrag is mostly base-only fragments (one geometry).

**Render thread** (host phases per level draw, short pad, side by side): title tod 0.30 -> 0.37 ms
(the fast day moves every palette every frame: ~0.7k-2.2k morph colors per level draw), the
vertices tod recolors 50-70k -> 45-60k (the far versions have fewer); village2 tod 0.141 -> 0.142,
runs tested 6526 -> 6806, draws walked 1116 -> 1219; lod and chunks phases +0.002 ms. The title's
+67 draws are the near chunks' own draws and the far versions' runs around the near groups.

**Memory** (`c3l_mem`, linear memory resident per level): village1 11555 -> 10676 KB, village2
13388 -> 12893, Fire Canyon 10513 -> 10292, ogre 16208 -> 15727, rolling 16141 -> 15416 (the level's
chunks hold geometry 2 where they held geometry 0); near chunk data +1.4 MB (village1); heap +0.6
MB at most (village1's color lists). The tightest pairs (ogre+village2, rolling+village2) keep 2.6-2.9
MB above the 3 MB reserve (1.6 before). Files +1-7%.

## CPU instruction costs on the console (AI-assisted, cpubench, Nov 11)

(AI-assisted.) Azahar counts about one tick per guest instruction ("The renderers' own CPU
code" above), so every CPU change so far was priced by its instruction count. What the ARM11 and
its VFP11 spend beyond that is invisible there:

- the VFP11 issues one single-precision add, multiply or multiply-add per cycle, but a result is
  ready ~8 cycles later (GCC's vfp11 pipeline model; the VFP11 divider paper: one FMAC per
  cycle). A chain of dependent operations runs at 1/8 of the rate of independent ones;
- a float compare that a branch reads (`vcmp` + `vmrs APSR_nzcv`) waits for the VFP pipeline;
  moving a float's bits to a core register (`vmov`) waits only for that value;
- `vdiv` / `vsqrt` take 15-19 cycles and can hold up the multiply-add pipeline;
- ARMv6K has no integer divide instruction: a non-constant `/` or `%` is `__aeabi_idiv`;
- calls through a register are not predicted like direct calls and returns (to be confirmed by
  the bench);
- L1 data cache 16 KB per core, 32-byte lines (New 3DS: a 2 MB L2 behind it).

**`cpu_bench = on`** (CtrCpuBench.h, `preset = bench` too) runs microbenchmarks at boot and writes
`data/log/cpu_bench.txt`: per case ns and cycles (804 MHz) per iteration, the best of three runs.
The primitive cases: `vfp.chain8` / `vfp.indep8` (latency against throughput), `vfp.cmp_branch` /
`vfp.sign_bit` (a branch on a float compare against its sign bit), `vfp.div` / `vfp.mul` /
`vfp.sqrt`, `int.div_var` / `int.div_const`, `call.direct` / `call.indirect`, `mem.seq256k` /
`mem.seq256k_pld` / `mem.line256k`. The commits that change kernels add their A (before) and B
(after) versions as pairs; a pair must give the same checksum (`same as pair`, else `MISMATCH`), so
a console run also checks the new kernels there. The host's regression test runs every case with
a few iterations for the checksums; `test/ctr_render/arm/run.sh` builds them as ARMv6K hard-float
code and runs them under qemu-arm's ARM11 MPCore with the 3DS's flush-to-zero / default NaN
FPSCR (times mean nothing under qemu).


### 1. Visibility tests without a branch per float compare (signbits)

The level renderer's per-run plane test (`run_loop`), the chunks' `sphere_in_view`, the
streaming look-ahead's view test and the occluder shadows' `classify` / `hidden` (CtrCull.h)
evaluated each plane and branched on its compare (`vcmp`, `vmrs`, a conditional branch), leaving
at the first plane that failed. Now every plane is evaluated (the planes' multiply-add chains are
independent, so the VFP overlaps them) and the sign bits of `t - a` are ANDed (`a > t`) or of
`d - x` ORed (`d < x`) in core registers: one branch at the end (CtrSimd.h `ctr_float_bits`).
Same results, equality and NaN included: `x - x` is +0, flush-to-zero keeps the sign, and with
the 3DS's default NaN a NaN fails as the compare did. ARM code (GCC 13, `-O2`, the bench's copy):
no `vcmp` / `vmrs` left in the loop, 41 instructions against 39. Bench pair
`cull.planes3_branch` / `cull.planes3_signs` (3 planes, spheres exactly on a plane and a NaN
among them): the same checksum on the host and as ARMv6K code under qemu.

### 2. VFP short vectors for the bone matrices (vfpvec)

The VFP11 still has VFPv2's short vectors: with FPSCR LEN = 3 (four lanes), an operation whose
destination and first source are in s8-s31 and whose second source is in s0-s7 runs on four
consecutive registers against one scalar, e.g. `vmul.f32 s8, s16, s0` is s8..s11 = s16..s19 * s0.
pc-new-bones-mtx-calc's hand-scheduled VFP code (Jak 1 and Jak II, now one copy in
`game/mips2c/native_bones_vfp.h`) starts with the bind pose times the bone: 64 scalar multiplies
and multiply-adds that are exactly 16 such vector instructions, lane by lane the same operations
in the same order (VFPv2 rounds the product, then the sum), so the same bits. Pass 1 runs up to
64 bones in one asm loop with LEN set (no compiled code runs meanwhile: GCC's code assumes scalar
mode; FPSCR written twice per 64 bones), pass 2 is the scalar code's rest per bone.

`vfp_vector = on` (default off). The VFP still does one lane per cycle, so the gain is in issue
slots and code size (48 fewer instructions per bone, the FPSCR writes aside): cpu_bench's
`bones.mtx_scalar32` / `bones.mtx_vector32` price it on a console. Checked under qemu-arm (ARM11
MPCore): bit-identical output with both FPSCR settings. Not known: whether Azahar (dynarmic)
emulates short vectors; check a frame there before turning it on.

### 3. Scheduling for the VFP11's latency (sched)

GCC 13 already runs both scheduling passes for this target at `-O2`, with register pressure
(`-Q --help=optimizers -march=armv6k -mtune=mpcore`: `-fschedule-insns`, `-fsched-pressure`,
`-fschedule-insns2` enabled), and `-mtune=mpcore` schedules for the ARM11 (its output differs from
`-mtune=cortex-a8`). `-fmodulo-sched` and `-O3` leave a mat4 x vec4 loop as it was. What is left:
a loop body of four independent float chains still waits about half its issue slots for the
8-cycle results; `-funroll-loops` (which also renames registers, so two iterations' chains
overlap; `#pragma GCC unroll` alone doesn't) brings that loop from 41 to 29 cycles a point, but
only when the compiler knows the stores don't alias the next loads (`__restrict`, not inlined
into code that loses it). The natives' loops over GOAL memory mostly use plain pointers, and
`-funroll-loops` makes their code 22% larger (488 -> 597 KB, the ARM test build).

- `platform/3ds/tools/vfp11_cycles.py FILE.s FUNCTION LABEL`: an in-order issue / latency model
  of a loop body in GCC's assembly (VFP arithmetic 8 cycles, divide / square root 19, VFP load 4,
  `vmrs` waits for the VFP). It doesn't model branches, mispredicts or caches: for straight-line
  float loops only.
- CMake option `OG3DS_NATIVE_UNROLL` (default OFF): `-O3 -funroll-loops` for the natives.
  `test/mips2c_native/run.sh arm` with `NATIVE_EXTRA_FLAGS="-O3 -funroll-loops"`: every native
  test that passes without it passes with it (60 under qemu-arm, cortex-a8: the harness's
  Ubuntu libc is ARMv7; `sp-launch-particles-var` fails with and without). The harness links again
  (`fakes.cpp`: the log's level check and config.ini lookups).
- cpu_bench `vfp.xform256` / `vfp.xform256_unrolled`: the loop through `__restrict` pointers as
  compiled and with `-funroll-loops` (the same checksum): the best case, priced on a console.

### 4. Integer divides (idiv)

ARMv6K has no divide instruction: a `/` or `%` by a value the compiler doesn't know is a call to
`__aeabi_idiv` / `__aeabi_uidivmod` (tens of cycles). The renderer's files compiled as ARMv6K code
(GCC 13 `-O2`, the host build's sources) have 28 such calls in 18 functions, almost all once per
frame, load or draw; the natives (the test build's objects) 2, in the particle spawner;
GOAL code's divides are 32-bit helpers (`gc_idiv32`: a constant divisor folds to a multiply or a
shift), 424 uses in all of Jak II, mostly menus, HUD and navigation set-up. Three were in
per-item loops and are gone, the same results:

- CtrTexAnimJak2's PSMT8 + CLUT animated textures: two divides per texel for the source index,
  now a column table per image and the row once per row;
- CtrVram's `sampled_hash` (texture change checks, 128-256 samples): `(k * 37) % stride` per sample,
  now a stepped remainder;
- the ocean's CPU clipper: `(k + 1) % count` per edge, now a compare.

cpu_bench pairs `int.mod_per_item` / `int.mod_stepped` and `int.texel_div64` / `int.texel_table64`.

### 5. Square roots and divides out of the per-run tests (fdiv)

The VFP11's `vdiv` / `vsqrt` take 15-19 cycles and hold its multiply-add pipeline. Counted per
function in the renderer's ARMv6K code, the ones inside per-chunk or per-run tests were:

- `cone_away` (cone_cull, per run in view): `cone_unpack`'s three `/ 127` and the axis
  normalization, then three square roots and a divide: 4 `vdiv` + 4 `vsqrt`. Now multiplies only:
  with p = v . axis and d2 = |v|^2, |v| cos(t + a) > r is L = p cos a - r > 0 and
  L^2 > (d2 - p^2)(1 - cos^2 a); the axis normalized by `rsqrt_nr` (the bit trick's estimate and
  two Newton steps, ~5e-6). The old formula stays as `cone_away_ref`.
- `sphere_px` (min_run_px, per chunk and run): a square root and a divide; the callers only
  compare it with the limit, so `sphere_px_below` compares 2 r f d < lim z (z - r) squared.
- `depth_test` (depth_reuse): two divides per box corner, now one reciprocal.

Tests (cull_test.cpp): over 200 000 random cones / views and spheres, the new tests and the
formulas they replace agree in every case; the cone test's 24 000 views still have every culled
run's triangles facing away (5571 culled, as before). cpu_bench pair `cull.cone_sqrt_div` /
`cull.cone_mul` (ARMv6K: 4 `vdiv` + 4 `vsqrt` against none). The ocean's VU code keeps its
divides: it reproduces the PS2's arithmetic.

### 6. ARMv6 packed integer instructions (armsimd)

Already there: the sound mixer (`voice.cpp`: SMLAWB, SSAT, QADD16, PKHBT, SMULxy, its own test in
`test/snd_voice`), the texture conversions (`CtrTexConv.h`: PKHBT / PKHTB, UXTB16, UQADD8, REV),
the sky's texel blends (`CtrSimd.h`: UQADD8) and the time of day's saturation (USAT). What was
left byte by byte: the time of day's palette blend (`tod_refresh_colors`, every color whenever the
time of day moves) loaded each palette's R and B with two `ldrb` and an `orr`, and saturated R,
G and B one at a time. Now R | B << 16 is one (unaligned) `ldr` and `uxtb16`, and R and B are
saturated by one `usat16` (`CtrSimd.h` `ctr_rb_of_rgb`, `ctr_sat_rb6`; the color table has 4
bytes more for the word read at its last triple). The same values; ARMv6K code of the bench's
copy (4 palettes): 48 -> 41 instructions. cpu_bench pair `tod.blend4_bytes` / `tod.blend4_simd`.

### 8. Hot code together (hottext)

Jak II's gk.elf has ~18 MB of code against a 16 KB instruction cache per core (and on the New 3DS
a 2 MB L2): the functions a frame runs are spread over all of it, one or two per object file.
Azahar doesn't model the misses this causes. devkitARM's `3dsx.ld` places `*(.text)` before
`*(.text.*)`, and with `-ffunction-sections` every function has its own `.text.<name>`: a
function put in plain `.text` lands in one block at the start of the program with those of every
other object. (It has no `.text.hot` rule, so GCC's `hot` attribute alone wouldn't gather them.)

- CMake option `OG3DS_HOT_TEXT` (default ON): `CtrHot.h`'s `CTR_HOT` (`hot` + `section(".text")`)
  on the per-frame natives (bone matrices, shadow-execute, the particle blocks; both games) and
  the renderer's `sphere_in_view`; not on inline functions or templates (COMDAT).
- goalc emits `GC_HOT` (goal_c_ops.h, the same attributes on the 3DS's static GCC build) before
  the functions named in `goal_src/<game>/c-hot-functions.txt`: Jak II's list has the process
  tree, the joint math (with anim_lod), the draw and shadow paths and the math helpers they call
  (34 names, 66 functions over its modules, every name checked against the generated C).
- Checked: process-drawable, drawable and vector compiled for ARM put exactly their hot functions
  in `.text`; linked with `3dsx.ld`'s text rule, those three blocks come first and contiguous
  (~10 KB), then every `.text.*`. Host builds: no change (the macros are empty). Libraries built
  without `-ffunction-sections` keep their whole `.text` in the same first group, in link order.

### 9. Prefetch in the streaming loops (pld)

The ARM11 has no hardware prefetcher: a loop that walks an array waits for every new 32-byte
line. `PLD` (a hint: no fault, no change to results, also past the end of an array) asks for a
line ahead of time.

- The level renderer's per-run loop (`run_loop`): every 4th run, the lines 16 runs ahead of
  `VisRun` (8 bytes), `RunSphere` (8, when the chunk straddles a plane) and `RunLod` (4).
- The particle blocks (`sp-process-block-2d` / `-3d` natives, both games): the next particle's
  144-byte entry (3 lines) and its 48-byte sprite while this one runs.

Checked: the natives' differential tests as ARM code (qemu-arm): sp-process-block-2d / -3d,
bones, shadow-execute all match their references. cpu_bench pair `mem.runs64k` /
`mem.runs64k_pld` (the run loop's arrays for 64K runs, 1.25 MB) prices it on a console; the
primitive pair `mem.seq256k` / `mem.seq256k_pld` too.

### 10. A one-bone merc vertex program (onebone)

Merc draws whose palette has one bone (rigid props, vehicles' and characters' rigid parts:
`palette_count == 1`, so every vertex's three bone indices are 0) ran the full blend of
ctr_skin_opt: two MOVAs and nine multiplies / multiply-adds through indexed uniforms.
`ctr_skin_opt1.v.pica` is ctr_skin_opt with that blend as bone 0's rows times the weights' sum,
read at a constant index: 5 instructions instead of 11 (33 in all). The sum is rounded once where
the three products were rounded three times (float24): the same picture up to rounding.

- `config.ini merc_one_bone` (default off), shader_opt's program set only; the backend checks its
  uniforms are at ctr_skin_opt's places (`skin1_ok`) and puts it in the resident code image. A
  program switch re-sends the clip matrix, lights and rows (citro3d may rewrite uniform registers
  on a bind), so models that mix one-bone and multi-bone draws pay that per switch.
- pica_bench `B3.skin_opt1` next to `B3.skin_opt` (the same one-bone vertices) prices the saving
  per vertex on a console.
- Checked: `check_shaders.py` (every output component written once, the hazards the PICA hangs
  on). Not assembled here: picasso is devkitPro's (the 3DS build assembles it).
