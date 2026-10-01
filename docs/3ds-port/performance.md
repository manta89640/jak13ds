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
Lower draw distances (`render.ini`: 300 / 100) didn't help: the triangle count isn't the problem.
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
  thread (the number in the `sdmc:/3ds/jak1/sound` flag file).

To see where the GPU time goes on hardware: `gpu_profile = 1` in `render.ini` leaves out one
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
