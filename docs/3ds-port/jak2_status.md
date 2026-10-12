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

## C-backend warnings to review (from `j2data/cbuild1.log`)

These are the bug class that broke Jak 1's platforms (c-abi-v3-narrowing). Review each one before
trusting gameplay. Fix each one in goal_src (type the parameter `handle`, as Jak 1 did), or show it
is harmless.

Narrowing casts (11):
- `levels/common/warp-gate.gc`: parameter `arg2` of type `target` cast to `handle`
- `levels/city/traffic/citizen/civilian.gc`: `this` (civilian) cast to `handle`
- `levels/city/traffic/citizen/guard.gc`: `this` (crimson-guard) cast to `handle`
- `levels/common/enemy/guards/crimson-guard-level.gc`: `this` cast to `handle`
- `levels/common/enemy/hover/crimson-guard-hover.gc`: `this` cast to `handle`
- `engine/ps2/timer.gc`: a `time-frame` cast to `float` and passed as an argument
- 5x `gs-*` register values cast to `adgif-shader` (`engine/debug/default-menu.gc` and others;
  debug code, probably harmless)

The four citizen/guard ones run constantly in Haven City (traffic), so check them first.

Function-type casts (306): "a function of type X called as Y gets arguments without their upper
half". Jak 1 had three and fixed them in goal_src. Most are probably like `leg-ik-callback` or
debug-menu callbacks (an object parameter seen as matrix/float). Sort them by which mask bit is a
real 64-bit value: handles, time-frames, 64-bit bitfields. The ABI check mode (c_backend.md:
`-DGOALC_ABI_CHECK` + `OPENGOAL_C_ABI_CHECK=1`) finds the ones that matter at run time.

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
