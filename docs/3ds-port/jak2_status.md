# Jak II -> New 3DS: status and handoff (milestone 1, Oct 12 2026)

(AI-assisted.) Branch `jak2-3ds` (worktree `jak-project/.claude/worktrees/jak2`), started from
`batch-oct11` c1da64bc1. Jak 1 behaviour is unchanged: every shared-code change is either Jak 2
only or a hook that stays null for Jak 1.

## Where it stands

| Step | State |
|---|---|
| Disc | Verified: SCUS-97265, `SYSTEM.CNF` VER=2.01, NTSC; the extractor's hash check passed |
| Extract / decompile data | Done. `iso_data/jak2` 4.1 GB, `decompiler_out/jak2` 509 MB, 147 fr3 levels |
| Native PC build (arm64) | Done. Boots to the title and renders Haven City (slums, HUD minimap); silent |
| Native `(mi)` | 2273 targets build |
| C-backend compile of all of Jak 2 | **840/840 modules, 0 functions unsupported** (lenient pass, 137 s); warnings below |
| C-mode runtime and GOAL kernel | Written and building (commit db229ab71). **Not run yet** |
| C-mode boot on the Mac | Not started |
| Haven City timing (the feasibility number) | Not measured yet |
| Scripted route into city gameplay | Not working yet: the title level puts Jak back in `target-title` (see below) |

Decompiler config: OpenGOAL's extractor database files v1 and v2.01 under one entry
(`ntsc_v1`). Both have the same serial and ELF hash, and the database lists both ISO hashes. The
`ntsc_v2` overrides only change label and anonymous-function types, so they only matter for
decompiling code. Data extraction and the build use `ntsc_v1`, as upstream does.

## Paths

| What | Where | Needed to resume? |
|---|---|---|
| Worktree (source, branch jak2-3ds) | `jak-project/.claude/worktrees/jak2` | yes (commits are in the repo) |
| Native PC build | `<worktree>/build` (Release, arm64, Ninja, `-DOPENSSL_ROOT_DIR=/opt/homebrew/opt/openssl@3`) | regenerable, ~15 min |
| Extracted disc | `jak-project/iso_data/jak2` (real files, main checkout, git-ignored); `<worktree>/iso_data/jak2/*` are symlinks to it | keep (regenerable from the ISO in ~5 min) |
| Decompiler data output | `jak-project/decompiler_out/jak2` (`<worktree>/decompiler_out` symlinks to the main checkout's) | regenerable, ~1 min |
| Native game output | `<worktree>/out/jak2` (fr3, obj, iso) | regenerable: decompile and `(mi)` |
| C-mode mirror | `port_test/j2mirror`: symlinks to the worktree except `out/`; `out/jak2/{csrc,cmod,obj,iso}`, `out/jak2/fr3` symlinked to the worktree's | regenerable, ~2.5 min |
| Project-path link | `port_test/j2link/jak-project` -> worktree (binaries find the project by the last "jak-project" in their path) | yes (tiny) |
| Logs | `port_test/j2data` (extract, decompile, native and C build logs; `cbuild1.log` = the lenient C build) | no |

The VAG/STR/MUS copies in both `out/jak2/iso` folders are APFS clones of `iso_data/jak2` (`cp -c`),
so they take no extra space. A new `(mi)` copies them again, so clone them again afterwards (see the
commands below).

Disk (du counts clones in full): iso_data 4.1 GB, decompiler_out 0.5 GB, worktree source ~0.6 GB,
build 1.3 GB, worktree out 5.4 GB, j2mirror out 5.0 GB. Space actually used is about **9.6 GB**:
the out/ iso folders hold only ~0.6 GB each of their own data (DGO/CGO/TXT); the rest is clones.

## Commands

```sh
W=/Users/user/Projects/port_test/j2link/jak-project      # the worktree, through the link
# extract (into a staging dir, then move into the main checkout's iso_data/jak2 and symlink)
$W/build/decompiler/extractor "<Jak II iso>" --proj-path $W --extract-path port_test/j2data -g jak2 -e
# decompile data (fr3 levels, textures, text)
$W/build/decompiler/extractor $W/iso_data/jak2 --folder --proj-path $W -g jak2 -d
# native compile ((mi) with *iso-data* set)
$W/build/decompiler/extractor $W/iso_data/jak2 --folder --proj-path $W -g jak2 -c
# C-backend compile into the mirror (drop LENIENT for a strict build)
OPENGOAL_C_BACKEND_LENIENT=1 $W/build/goalc/goalc --game jak2 --proj-path port_test/j2mirror \
  --instruction-set c -c '(mi)'
# re-clone the copied audio after a (mi)
cd <out>/jak2/iso && for f in *; do s=$(find -L $W/iso_data/jak2 -name "$f" -type f | head -1); \
  [ -n "$s" ] && cmp -s "$f" "$s" && rm "$f" && cp -c "$s" "$f"; done
# silent PC run (OpenGL window; screenshots via the pad script's "shot", one file:
# <config>/OpenGOAL/jak2/screenshots/screenshot.png)
OPENGOAL_NO_SOUND=1 OPENGOAL_PAD_SCRIPT=city.pad $W/build/game/gk -g jak2 --proj-path $W \
  --config-path <scratch>/cfg -- -boot -fakeiso -nosound
# C mode (next step): add -cbackend, run from the mirror
OPENGOAL_NO_SOUND=1 OPENGOAL_NULL_GFX_FPS=30 $W/build/game/gk -g jak2 --proj-path port_test/j2mirror \
  --null-gfx -- -boot -fakeiso -nosound -cbackend
```

Notes:
- `-boot -debug` (boots to the prison, without the title) segfaults natively on arm64 within
  ~10 s. Not investigated.
- `-level ctysluma` fails: `bg` looks for `CTYSLUMA.DGO`, but the file is `CTA.DGO`.

## Pad script on Jak 2

The hooks for `print`, `wait`, `pos`, `continue` and `exit` are in `game/kernel/jak2/kscheme.cpp`
(Jak 2 layouts: process state +64, process-drawable root +128). `continue NAME` calls the GOAL
`pad-script-continue` (`goal_src/jak2/pc/features/speedruns.gc`). That function spawns a process in
`*pc-pool*`, because the hook runs inside `cpad-get-data` and `reset-actors` kills
`*entity-pool*`. The process then does the speedrun act-2 setup: mode `debug`, `initialize!`, mode
`play`, closes `city-oracle-level0-training` and `ruins-enemy-resolution`, `start 'play`.

**Open problem.** From the title screen, Jak ends up in `target-title` again. The title level stays
loaded and its title-control keeps re-spawning. The helper kills it every frame for 1200 frames, but
that didn't help: the last run still showed `target-title` 900 frames after `start`.

Next things to try:
- (a) wait until the title menu is up, then press X / navigate (`progress-intro-start` path);
- (b) take the title level out of `*load-state*` before `start`, e.g. `want-levels` with the
  continue's levels, or `(set! (-> *load-state* want 0 name) ...)`;
- (c) find who sends `change-mode`/`go target-title`: title-obs.gc `target-title`, or a continue
  point with the `title` flag (`title-start`/`title-restart` from `initialize!`);
- (d) boot without the title: `*kernel-boot-message*`, or make `-level` map to the DGO name.

## C-backend warnings (from `j2data/cbuild1.log`), reviewed Oct 18

(AI-assisted) The bug class that broke Jak 1's platforms (c-abi-v3-narrowing). Branch jak2-abi
fixed the real ones; a C build after the fixes has 310 warnings, all shown harmless below.

Narrowing casts (11 -> 6):
- fixed: `warp-gate.gc` `target-warp-out` arg2 (`target` cast to `handle`): the warp gate sends its
  handle through `change-state`, the `target`-typed parameter dropped the pid, so `handle->process`
  failed and the gate never got `'effect`. arg2 is now `object` (target-h.gc): it is the gate's
  handle, or the entity that `target-warp-in` passes on (a pointer, the same as native).
- fixed (was harmless): civilian / guard / crimson-guard-level / crimson-guard-hover passed
  `(the-as handle this)` to `speech-control-method-13`, which compares it with `handle->process`:
  it was a process all along, never a real handle, and passed whole (hi = 0, as native).
  `speech-control-method-13` and `speech-channel-method-10` now take a `process`.
- harmless: `timer.gc` puts the frame delta (a small `time-frame`) into `sparticle-data x` as float
  bits; the low half is the whole value, and a float field store keeps only it on every backend.
- harmless: 5x `gs-tex0/tex1/clamp` cast to `adgif-shader` in `default-menu.gc` (debug): the value
  is cast straight back to `int` and shifted, never passed as an argument (locals keep 64 bits).

Function-type casts (306 -> 304):
- fixed: `editable-player.gc` (debug editor): a `(function editable symbol symbol)` lambda called
  with an `editable-region`; `symbol` is WIDE in Jak 2, so `(= (-> arg0 region) arg1)` compared a
  garbage upper half. The lambda takes `editable-region`, like its twin a few lines below.
- fixed: `menu.gc` int items (debug menu) called their `(function int debug-menu-msg int int int)`
  callbacks through the float view: the ints got a garbage upper half. They now store and call
  `iactivate-func` with ints, as Jak 1 does.
- harmless, the affected parameter is never read (a WIDE parameter only reads a float register):
  300 bot course callbacks (`*-course.gc`: sewer mog/jinx/grim/hal, atoll sig0, under sig5, city
  kor/kid/hal3, ash4, kiddogescort), `(lambda ((arg0 object) (arg1 <bot>)) ...)` stored as
  `check-done` / `(function <wait-spot> <bot> symbol)`: arg0 unused in all of them; 2x
  `leg-ik-callback` (arg1/arg2 `object`, unused); `pilot-states.gc` rotate-surface hook
  (arg0-2 `object`, unused; Jak 1 retyped its twin to `surface`).

Not checked statically: casts from untyped values (`function`, `basic` fields). Run the ABI check
mode (`-DGOALC_ABI_CHECK` + `OPENGOAL_C_ABI_CHECK=1`, c_backend.md) once C mode boots.

## Next steps

Milestone 1 (Mac):
1. Strict C build (no LENIENT), then boot C mode from the mirror: `gk ... -cbackend`. Expect
   kernel-port bugs: suspend/resume with the Jak 2 offsets, `set-to-run-bootstrap`,
   catch/throw. Jak 1's runtime tests (`GoalcRuntime*`, C-mode kernel tests) have Jak 2
   equivalents only for the compiler (`Jak2*`).
2. Fix the city route above, then time Haven City. Method: `OPENGOAL_NULL_GFX_FPS=30
   OPENGOAL_PERF_SECTIONS` / kperf `logic` per frame in C mode on the Mac. Measure Jak 1 village1
   and Snowy the same way in the same session (p3ds / rtmirror-style mirror). Scale by the Jak 1
   Mac-to-N3DS ratio: Jak 1 village1 game thread ~12.5 ms on N3DS, sections off; Snowy logic
   11-13 ms on hardware.
3. Memory: log the level heap sizes and the levels loaded at once in the city (title showed
   ctysluma + ctywide + lwidea + title), the global heap (61.5 MB on PC), and the process pools
   (`*city-dead-pool*` showed "empty" on the first continue). Compare with the 124 MB app region
   (Jak 1 small memory = 48 MB EE).

Milestone 2 (boot on the 3DS) and the baseline the user asked for (Oct 12):
- Unmodified Jak II logic: crash fixes only, plus mandatory porting (C-mode kernel, 3DS platform
  layer, heap sizes to fit 124 MB, SD I/O for the overlord). No gameplay or performance cuts.
- First a logic-only boot with the null renderer, which gives a real Haven City frame time and
  gk.log perf lines. Then as much of the presentation as possible, reusing the Jak 1 Ctr renderers
  adapted to Jak 2's buckets: background (tfrag/tie/shrub via the converter on Jak 2 fr3), merc incl.
  envmap, sprites/particles, HUD/text, shadows, eyes, sky, ocean, texture animation (CPU fallback
  OK), screen effects, and sound/music/streaming (Jak 1 ndsp mixer path).
- Separate CIA/title: its own title ID, data in `sdmc:/3ds/jak2/`, a jak2 `config.ini` whose
  switches default to the unmodified behaviour. Staging in `port_test/3ds-sdcard-jak2-baseline`
  (intermediate baselines `-a`, `-b`). Before staging: check_svc, check_shaders, a silent host run,
  and a silent run in a muted private emulator instance (not emu-cell).
- 3DS build work: `platform/3ds/CMakeLists.txt` lists only Jak 1 kernel sources. Jak 2 needs its
  kernel, overlord (jak2) and mips2c sources, plus `SMALL_MEMORY` for Jak 2 (gcommon.gc has it at
  #f now; END_OF_MEMORY, heap sizes, `*goal-small-memory*` check).
- Renderer work that can go to parallel agents once the base boots: (1) background via
  converter + CtrLevel for Jak 2 fr3, (2) merc/eyes/envmap/shadow on Jak 2 buckets, (3)
  sprites/particles/HUD/text, (4) sky/ocean/texture animation/screen effects, (5) sound/overlord
  streaming. Boundaries: game/graphics/ctr/* per renderer, decompiler/level_extractor + converter,
  game/overlord/jak2 + game/sound.

## Open questions and risks

- Speed is still unknown. Jak 2 runs more code per frame than Jak 1: the city has traffic,
  citizens and guards, and nav meshes.
- Memory: Jak 2 PC uses `BIG_MEMORY`. The PS2 levels were sized for 32 MB, and the city keeps
  several large levels plus `*city-dead-pool*` in memory.
- `-boot -debug` crashes natively on arm64. This may hide a real bug that C mode also hits.
- The C module hazard (c_backend.md): two loaded levels with the same object file share seg_base.
  Jak 2 may share objects across the city's neighbouring levels. Check the DGO lists.
- mips2c: Jak 2 has 28 mips2c files (`game/mips2c/jak2_functions`) and no native ARM versions.
  The `(method 33 sky-work)` mips2c function is registered twice (warning at boot).
- `jak1/goalc_kernel.cpp` now includes the hook. The 3DS Jak 1 build was not rebuilt to confirm
  it still links. It only uses a pointer defined in that file, so it should.

## CIA art (for the baseline package)

- Icon: `port_test/cia-art/jak2/icon.png` (48x48, the user's Jak + Daxter art, a top-left 412x412
  crop of `icon-source.png`). Use it as-is.
- Banner: render from Jak II's own title logo model, like Jak 1 (memory note
  3ds-banner-icon.md): logo only on a transparent background, with
  `platform/3ds/tools/make_banner.py` from the exported logo model. Do the export in a scratch
  mirror without touching jak-project/out or decompiler_out.
- Jingle: not chosen yet. Pick a short iconic Jak II sound effect (Jak 1 used the power cell's
  "cell-prize"), say which, and the user can swap it.
- Keep the Jak II art in its own git-ignored folder (e.g. `platform/3ds/cia/local/jak2/`), so Jak
  1's banner and icon stay untouched.

## Scripted route and memory survey (jak2-route, Oct 18)

(AI-assisted.) Branch `jak2-route`. Native PC (arm64) only; C mode not tried.

### Route into Haven City: solved

Cause of the `target-title` loop: on the title screen the intro movie runs in a `scene-player`
("title-disk-intro"). When it ends (or sees a new Jak), it sends `*target*` to its end point, the
`title-movie-end` continue, whose `title-movie` flag puts Jak in `target-title`. `target-title`'s
enter then spawns a new `title-control`. Killing the title-control alone could not help.
`pad-script-continue` (`goal_src/jak2/pc/features/speedruns.gc`) now kills the scene-player and
the title-control, then starts. Jak stands in the city 20-46 frames after `continue` (85 for
Strip), from the title screen or from another continue point. Ideas (a), (b) and (d) were not
needed.

Second problem: **no traffic in the native PC city.** ctywide's level heap (small-center, PC
1.1x = 4178 KB) is full after loading: 4163 KB used, 1240 KB of it arm64 code. `ctywide-login`'s
32 KB `*city-dead-pool*` allocation then fails (`kmalloc: !alloc mem global-object`), the
traffic-manager can't be spawned ("*city-dead-pool* was empty"), and there are no citizens,
guards or cars. Commit 907ce45a9 sets `DEBUG_LEVEL_HEAP_MULT` to 1.15 (PC only). With it the
slums have about 290 live processes: 45 citizens, 10 guards, 45 riders, 55 vehicles. It is a
separate commit, so it can be dropped. C mode doesn't put code in the level heap, so the 3DS
doesn't need it.

Pads (`platform/3ds/tests`):
- `jak2_city.pad`: title, then `continue ctysluma-start`. Waits until Jak stands, then 400
  frames for traffic and a memory report. Timing windows `perf-mark city-stand` (600 frames
  standing in traffic) and `city-walk` (300 frames walking forward, ends against a wall),
  then exit at frame 2400.
- `jak2_route.pad`: the slums, port, market (`ctymarkb-burning-bush`), main town (`ctygenb-start`),
  industrial (`ctyinda-start`), farms (`ctyfarma-start`), then the sewer (`sewer-start`, a
  non-city level). Each stop: wait to stand, memory report, walk 200 frames. Exit at 5900.
  (`strip-start` works too, but walking forward there kills Jak.)

Run them with `--null-gfx` (`OPENGOAL_NULL_GFX_FPS=30` or 60). Windowed OpenGL runs stalled
before the first frame twice on Oct 18 (probably while the display was asleep); null-gfx runs
were fine. `print *pad-script-mem*` in any pad prints the memory report: heaps, pools, levels,
live processes by type, and global heap categories. For the categories, the Jak 2 kernel's
`kmemopen`/`kmemclose` now keep totals (`game/kernel/jak2/kmalloc.cpp`, printed through
`pad-script-kmem-report`).

### Memory on native PC (arm64, level heap 1.15x)

Global heap: 61.5 MB, 55.3 MB used (the same in every spot: everything big is preallocated).
By kmemopen category:

| Category | PC KB | PC multiplier | PS2-size estimate |
|---|---|---|---|
| level-heaps | 21024 | 1.15x (1.1x upstream: 20148) | 18396 (146 pages x 126 KB) |
| scheme-globals (kernel + GAME.CGO code and data) | 10115 | arm64 code included | less in C mode (no code) |
| dma-buffers | 10265 | 4x (`PROCESS_HEAP_MULT`) | ~2600 |
| process-buffers (`*nk-dead-pool*` 6160 + fixed pools) | 6400 | 4x | ~1780 |
| part-systems | 3693 | 12x (`SPRITE_MAX_AMOUNT_MULT`) | ~310 |
| sprite-tables | 3330 | 12x | ~280 |
| tpages | 879 | | 879 |
| other (anim-manager, collide lists, game-info, text, bigmap...) | ~930 | | ~930 |

Levels loaded at once:

| Spot | Levels (mode, heap used / size KB, code KB) | Live processes | nk pool used |
|---|---|---|---|
| title | title (s-edge 2637/6624), ctywide (s-center 4164/4178), lwidea (borrow 2088/2304), ctysluma (s-edge 5852/6624) | 161 | 222 KB |
| slums | ctysluma (5852/6912, 73), ctyindb (5949/6912, 12), ctywide (4196/4354, 1240), lwidea (2088/2409) | 291 | 918 KB |
| port | ctyport (5899, 54), ctypal (5783, 22), ctywide, lwidea | 288 | 979 KB |
| market | ctymarkb (5774, 49), ctyindb, ctywide, lwidea | 299 | 978 KB |
| main town | ctygenb (5525, 13), ctygena (5309, 27), ctywide, lwidea | 374 | 980 KB |
| industrial | ctyinda (5893, 12), ctyindb, ctywide, lwidea | 312 | 920 KB |
| farms | ctyfarma (5537, 69), ctygena, ctywide, lwidea | 346 | 1078 KB |
| sewer | sewer (large 10190/13824), sewerb (s-edge 5986/6912, 629) | 85 | 145 KB |
| strip | strip (large 11411/13824, 389), vinroom (s-edge 5574/6912, 114) | 150 | 282 KB |

The city fills the whole level heap: two districts take the edge sections (48 pages each), and
ctywide takes the centre (50 pages). lwidea (and a 380 KB second slot) is borrowed from
ctywide's top. District data is 5.3-5.95 MB per 6048 KB PS2 section, which fits at 1.0x. All
traffic processes (up to ~360) come from `*nk-dead-pool*`, 0.9-1.1 MB at peak. That is 60-70% of
the PS2 1540 KB pool, under its 768-process cap. `*city-dead-pool*` is only 32 KB, and only the
traffic-manager lives in it.

Renderer-side data (fr3, a proxy for the C heap / linear of the Ctr renderers): the slums set
(ctysluma 7.1 + ctyindb 5.5 + ctywide 2.5 + lwidea 4.7 MB) is 19.9 MB. The biggest district
pair (slumb + slumc + ctywide + lwidec) is 21.2 MB, and strip + vinroom is 21.4 MB. Jak 1's two
loaded levels are about 23-28 MB (village1 11.5, beach 11.6, citadel 16.3 MB). So the
renderer-side budget is about Jak 1's.

### What a small-memory (3DS) Jak 2 layout needs

The EE global heap, in C mode with PS2-like multipliers: level heap 18.4 MB at 1.0x (20.2 MB at
1.1x), DMA 2.6-5 MB (1-2x), process heap 1.8-3.3 MB (1-2x; 2x gives the city pool margin),
particles/sprites 0.6-2.4 MB (1-4x), tpages and the rest ~1.8 MB, plus engine data (at most
10.1 MB; less without code). That is **~35-43 MB of global heap**, so an EE of ~40-48 MB. Jak 1's
48 MB small layout (42.5 MB global heap) is the right size. Jak 2 needs its own END_OF_MEMORY /
`SMALL_MEMORY` constants in gcommon.gc and `PROCESS_HEAP_MULT` / `SPRITE_MAX_AMOUNT_MULT` /
`DEBUG_LEVEL_HEAP_MULT` small-memory values. Measure the first C-mode boot with this report
(scheme-globals without code) before choosing the multipliers. With the renderer data about Jak
1's, the 124 MB app region looks feasible at 48 EE + ~25 C heap + ~40 linear. The risk is the
C-mode binary size: Jak 2 has 840 modules compiled to C, against Jak 1's.
