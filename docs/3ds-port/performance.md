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
   (docs/3ds-port/3ds_build.md, "Native versions of hot mips2c functions"), and the native files
   are built with -O3. ARM instructions per call against mips2c (qemu, test/mips2c_native/run.sh
   bench): collide-probe-instance-tie 8x, collide-probe-node 6x, sp-launch-particles-var 2x,
   ocean-interp-wave 5x, (method 28 collide-cache) 4x; and for the ones measured above:
   moving-sphere-triangle-intersect 2.0x -> 3.6x, (method 9 collide-cache-prim) 2.1x -> 3.6x,
   cspace<- 4.7x -> 6.3x, sp-process-block-2d 2.5x -> 4.4x.
4. target-real-post runs Jak's physics `time-ratio` times per frame (2 at 30-60 fps, 3 at 22 fps,
   4 below 15): faster frames make it cheaper by themselves.
5. GOAL C code quality (u64 everywhere, owner: compiler): helps every row above.
