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
