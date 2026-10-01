# 3DS build of the runtime (M3 prep)

(AI-assisted.)

Jak 1 only. No OpenGL, SDL, imgui, discord, curl or cubeb. GOAL code comes from the C backend
(`c_backend.md`); no GOAL modules are linked in yet.

## Build

```sh
source platform/3ds/toolchain/env.sh          # toolchain: see toolchain.md
cmake -S platform/3ds -B build-3ds -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE=$PWD/platform/3ds/cmake/Toolchain-3DS.cmake
cmake --build build-3ds                       # -> build-3ds/gk.3dsx (+ gk.elf, gk.smdh)
```

`platform/3ds/CMakeLists.txt` is a standalone project, so the host build is not affected. It
builds these static libraries and links them into `gk.3dsx`:

| library | contents |
|---|---|
| `og3ds_common` | subset of `common/` (log, util, dma, sockets, xdbg stub, versions, fonts) |
| `og3ds_sound` | 989snd `Player` with no audio output (see below) |
| `og3ds_kernel` | files owned by the runtime work: `game/kernel/{common,jak1}`, `mips2c_table.cpp`, `mips2c_goalc.cpp`, `runtime.cpp` (unmodified) |
| `og3ds_runtime` | overlord (common + jak1), sce, system, settings, null renderer, all Jak 1 mips2c functions, discord logic on a no-op discord-rpc, HID stubs |
| `og3ds_port` | `platform/3ds/port/ctr_port.c`, the only file that includes `<3ds.h>` |
| `og3ds_fmt`, `og3ds_libco` | third party (libco uses its ARM32 backend) |

Status: everything compiles and `gk.3dsx` links. Sizes: 2.3 MB of code and 1.3 MB of BSS. It has
not been run on hardware or in an emulator yet. Expected first failure: allocating the 128 MB of
EE memory (see "Changes needed" below).

## Playing on a real 3DS

(AI-assisted.) Needs a New 3DS / New 2DS XL with custom firmware (Luma3DS) and the game files
built from your own copy of Jak 1 (same `out/jak1/iso` + `c3l` as for the emulator).

### What to copy to the SD card

Stage the files into a folder with `stage_sd.sh --sd <folder>` (or the SD card's mount point):

```sh
platform/3ds/tools/build_cmodules.sh                  # GOAL -> C modules, gk.3dsx (see M3 below)
platform/3ds/tools/make_cia.sh                        # build-3ds/jak1.cia
platform/3ds/tools/stage_sd.sh --proj ../p3ds --sd /Volumes/3DS --clean-logs
cp build-3ds/jak1.cia /Volumes/3DS/cias/              # any folder FBI can browse
```

On the card: `3ds/jak1/gk.3dsx`, `3ds/jak1/data/out/jak1/iso/*` (~1.3 GB), `3ds/jak1/data/out/jak1/c3l/*`.

**gk and the game files go together.** The GOAL code is compiled to C and linked into gk (the
3dsx and the CIA); each object in the `.CGO` / `.DGO` files only carries the hash of its C module.
After any GOAL change, copy gk (or reinstall the CIA) *and* `out/jak1/iso` from the same
`build_cmodules.sh` run. A mismatch stops at boot with `goalc: could not find C module ... for
object file ...`.

### Install and start (recommended: CIA)

1. Put the SD card back, start FBI, open `cias/`, select `jak1.cia`, "Install CIA".
2. Start "OpenGOAL Jak1" from the HOME Menu (it has an icon but no banner animation).

To try the CIA in Azahar first: `run_emu.sh --cia build-3ds/jak1.cia --stage "--proj ../p3ds"`
(installs it in the emulator and runs the installed title).

The CIA asks for the New 3DS 124 MB memory mode (`platform/3ds/cia/gk.rsf`: `SystemModeExt:
124MB`, 804 MHz, L2 cache). The game needs about 95 MB: 48 MB of PS2 memory, 12 MB of code, 24 MB
of GPU memory, plus buffers.

### Custom HOME Menu banner (title logo, power cell jingle)

The banner in the repo is a placeholder: game assets are never committed. To use the game's own
title logo and the power cell jingle, make them from your copy of the game and put them in
`platform/3ds/cia/local/` (ignored by git); `make_cia.sh` uses them when they are there
(it prints which banner it used).

1. Export with the decompiler: in `decompiler/config/jak1/jak1_config.jsonc` set `"rip_levels": true`
   and `"rip_streamed_audio": true`, then `task extract`. This writes the title logo model
   (`decompiler_out/jak1/levels/title/logo-english-lod0.glb`, textures embedded) and every sound
   effect as a .wav (`decompiler_out/jak1/audio/sfx/<bank>/cell-prize.wav` is the power cell jingle).
2. `python3 platform/3ds/tools/make_banner.py` (needs `python3 -m pip install numpy pillow`): renders
   the logo at 256 x 128 with a transparent background (straight on, from the side its faces point
   to) and cuts the jingle to 2.9 s with a fade-out, into `platform/3ds/cia/local/`. `--model` /
   `--sound` take other files.
3. `platform/3ds/tools/make_cia.sh` and reinstall the CIA.

(AI-assisted: the script was tested on a synthetic model and sound; the paths are the ones the
decompiler writes, checked in its source.)

### If the CIA stays on the launch screen

Found on hardware: the CIA settings listed `0004013000001202` (pm, part of the FIRM, not a NAND
title) as a dependency module. The system loads an app's dependencies when it starts it and kills
the app when one fails, so every CIA (game and boot tests) stayed on the launch screen with no
process in Rosalina's process list and no file written. Azahar doesn't load dependencies, so it ran
them anyway. The dependency list is now buildtools' template.rsf's (the same as FBI's). If a CIA
still doesn't start:

Look at `sdmc:/3ds/jak1/boot_cia.txt`: one line per boot step, the last one is where it stopped
(`0 services` without `0 APT`: stuck in the HOME Menu handshake). The file is the first thing the
game writes. If there is none at all, the title never ran the game's code: find out why with the
boot test, a small app packaged with the same CIA settings as its own title:

```sh
make -C platform/3ds/hello                    # (source platform/3ds/toolchain/env.sh)
platform/3ds/tools/make_cia.sh --boottest     # four boot test titles in build-3ds/, see below
platform/3ds/tools/make_cia.sh --mem legacy   # the game without the 124 MB mode (jak1_legacy.cia)
```

The boot test titles differ in one setting each (they install side by side):

| CIA | title id | settings |
|---|---|---|
| `boottest_124.cia` | 000400000F7A1200 | the game's: 124 MB mode, 804 MHz, L2 cache |
| `boottest_legacy.cia` | 000400000F7A1300 | without the 124 MB mode |
| `boottest_plain.cia` | 000400000F7A1400 | like most homebrew CIAs (FBI): no 124 MB mode, 268 MHz, no L2 |
| `boottest_nocompress.cia` | 000400000F7A1500 | the game's, code not compressed |

Install all four with FBI and start each from the HOME Menu (wait ~20 s, then go back to the HOME
Menu or hold POWER). Each appends lines with its title id to `sdmc:/boottest.txt` and
`sdmc:/3ds/jak1/boottest.txt` (`0 started`, `1 APT`, `2 main reached`, `3 screens up: app memory
...`) and, once it is up, shows a pulsing top screen and on the bottom screen its title id, the
SD card results (`fsInit`, `sdmc`) and whether the files were written:
- a title shows its screens but no file: the SD card access fails in installed titles (the bottom
  screen has the error codes).
- some variants start and others don't: the setting that differs is the cause.
- none starts, not even `plain`: the packaging (makerom version: `make_cia.sh` prints it; v0.18.4
  or later) or the install. FBI itself is a CIA built like `plain`.
- the boot tests start but the game doesn't: something about the game's binary (its size, a crash
  before the first boot step); `jak1_legacy.cia` tells whether the 124 MB mode matters for it.

Also useful: a crash dump in `sdmc:/luma/dumps/arm11/`, and the Rosalina menu (L + Down + Select)
while it hangs: "Process list" shows whether the title's process exists.

### Without installing: gk.3dsx

A `.3dsx` runs inside another title's memory:
- From the Homebrew Launcher started as an applet (Rosalina menu, Mii Maker...) there is much less
  memory: the game shows "Not enough memory" and waits for START.
- With title takeover (hold R while starting a game from the HOME Menu, which opens the
  Homebrew Launcher in that game's place) it gets that game's memory: 64 MB for most games,
  124 MB only for New 3DS exclusive titles (for example Xenoblade). Use the CIA instead.

### Screens and logs

- Top screen: the game. Bottom screen: 3 lines of stats (fps, ms per frame for game logic,
  rendering, waiting for vsync, heap use), then the log.
- On a crash (failed assert, CPU exception, GOAL `(break)`), the bottom screen shows what happened
  and the last log lines, and waits for START; then the app exits. CPU exceptions are shown only if
  Luma3DS lets the app handle them; otherwise Luma's exception screen appears (take a photo).
- Logs: `sdmc:/3ds/jak1/data/log/stdout.log` (everything printed) and `gk.log` (debug level).
- Old 3DS / Old 2DS: not enough memory (64 MB); the game says so and exits.
- All settings are in one file, `sdmc:/3ds/jak1/config.ini` (`config_ini.md`): renderer
  settings, `args` (game arguments, default `-boot -cbackend`), `listener` (Wi-Fi REPL),
  `screenshots` (N: save the top screen every N frames to `data/log`), `pad_script` (scripted
  input, see below), `io_on_system_core` (experimental: IOP / IO threads on core 1; hangs at boot
  in Azahar, untested on hardware), `perf_sections` (per-section frame timing in the log),
  `sound` / `sound_core` (audio output through the DSP, see "Sound" below). `stage_sd.sh` options
  set these keys.

### Automated gameplay test (scripted input)

`game/sce/pad_script.h` replays a timeline of pad states on controller 0 (format in the header):
buttons and sticks per game frame, `wait *target* <state>` (stops the script's clock until Jak is
in that state), `print SYM`, `pos *target*` (position in meters and state), `continue NAME`
(jumps to a checkpoint: `(start 'play (get-continue-by-name *game-info* NAME))`, for example
`beach-start`), `crash` (tests the crash screen), `exit`.

- 3DS: `pad_script = FILE` in `config.ini` (`stage_sd.sh --pad-script FILE` copies it to `sdmc:/3ds/jak1/pad_script.txt`).
- PC: `OPENGOAL_PAD_SCRIPT=FILE gk ...` (with `--null-gfx`, or `--ctr-gfx` to get frames).
- `platform/3ds/tests/gameplay.pad`: title -> Start -> New Game (taps Cross, fine with an empty
  save) -> skips the intro cutscene (taps Triangle) -> waits for `target-stance` on Geyser Rock ->
  walks forward, jumps while walking, jumps, punches, spin kicks, crouches, walks left and right,
  logging Jak's position and state after each step.
- `platform/3ds/tests/ocean.pad`: the title screen (village1), Geyser Rock, then the checkpoints
  `village1-hut` and `beach-start`, turning the camera around at each (use with `--screenshots`).

```sh
platform/3ds/tools/run_emu.sh --seconds 900 --out build-3ds/emu-game \
  --stage "--proj ../p3ds --clean-logs --clean-user --screenshots 300 --pad-script $PWD/platform/3ds/tests/gameplay.pad"
grep -a "pad script" build-3ds/emu-game/stdout.log
```

`run_emu.sh` takes a lock (`$TMPDIR/opengoal-azahar.lock`) because Azahar and its virtual SD card
are shared; `--stage` stages the SD card inside the lock.

Results in Azahar (New 3DS, CIA with the 124 MB mode, build of 3ds-port d8c3b9c7):
- Boot, title screen, Start, New Game, the intro cutscene skipped, Jak on Geyser Rock in
  `target-stance`, then walking, jumping (`target-jump` / `target-hit-ground`), punching,
  spin kick, crouching (`target-duck-stance`) and walking left / right all work, no crash.
- Game logic takes 12-15 ms per frame on the title screen (about 50 fps) and 65-85 ms in
  Geyser Rock and village1 (12-15 fps) in the emulator.

### Where the threads run (New 3DS)

Measured in Azahar with the gameplay script:
- Everything but the renderer on core 0 (default): title 50+ fps, gameplay 12-15 fps.
- IOP / listener / EE worker on core 2 (with the render thread): 1.4 fps (695 ms of logic per
  frame, the EE waits for the IOP). Not used.
- IOP on core 1 (`APT_SetAppCpuTimeLimit(80)`, `config.ini` `io_on_system_core = on`): boot hangs at the first
  IOP file load. Off by default.

### Controls

Face buttons by position, like the PS2 pad:

| 3DS | PS2 | Jak |
|---|---|---|
| B (bottom) | Cross | jump (hold for higher), confirm in menus |
| Y (left) | Square | punch; roll / dive while running |
| A (right) | Circle | spin kick |
| X (top) | Triangle | look around (first person), back in menus |
| L, R | L1, R1 | crouch (+ jump: high jump, + square: roll) |
| ZL, ZR | L2, R2 | not used in gameplay |
| Circle Pad | left stick | move |
| C-stick | right stick | camera (New 3DS only) |
| Start / Select | Start / Select | pause menu / (unused) |
| touch screen top / bottom half | L3 / R3 | (unused in Jak 1) |

## Platform pieces

- **Types:** on `__3DS__`, `s32`/`u32` are `int`/`unsigned int` (`common/common_types.h`),
  because newlib's `int32_t` is `long`. As a result, `<3ds.h>` (whose `u32` is `uint32_t`) can't
  be included in the same file as `common_types.h`. All libctru calls go through
  `platform/3ds/port/ctr_port.h`, which uses plain C types.
- **POSIX:** `OS_POSIX` is defined on the 3DS. newlib and libctru provide pthreads,
  `clock_gettime`, `unistd` and BSD sockets. There is no mmap; `runtime.cpp` uses `memalign`.
- **Filesystem:** `common/util/FileUtil.h` uses libstdc++ `std::filesystem` on the 3DS instead of
  ghc (which fails with `#error`). `fs` is a namespace that also provides `fs::ifstream` and
  `fs::ofstream`.
- **Threads:** `game/system/SystemThread` uses pthreads with an explicit stack size: 512 KB for
  EE, 128 KB for the others. libctru's default is 32 KB. All threads run on core 0 at priority
  0x3F.
- **Graphics:** new null renderer, `game/graphics/pipelines/null.{h,cpp}`
  (`GfxPipeline::Null`).
  - No window. `vsync()` sleeps to the next 60 Hz boundary. `Gfx::vsync` fires the IOP vblank
    callback first.
  - `Gfx::Loop` sleeps when there is no window. On the 3DS it also services APT (HOME / power);
    `MasterExit = EXIT` when the app is closed.
  - It is the default on the 3DS. On the host, use `gk --null-gfx`. Tested: `gk --null-gfx --
    -boot -fakeiso` boots Jak 1 to the title screen and loads village1 without a window.
    `--no-display` is unchanged: no Gfx at all, and no vsync pacing.
- **Input:** `game/sce/libpad.cpp` calls `ctr_pad_read()` on `__3DS__`. Port 0 only, digital
  buttons report 255 pressure. Mapping:

  | 3DS | DualShock 2 |
  |---|---|
  | A, B, Y, X | Circle, Cross, Square, Triangle (by position) |
  | L, R | L1, R1 |
  | ZL, ZR | L2, R2 (New 3DS / Circle Pad Pro) |
  | Start, Select, D-pad | the same buttons |
  | Circle Pad | left stick |
  | C-stick | right stick (New 3DS; centred on Old 3DS) |
  | touch screen, top half | L3 |
  | touch screen, bottom half | R3 |

  `game/system/hid/hid_null.cpp` provides link stubs for `DisplayManager`, `InputManager` and
  `sdl_util`. The kernel only reaches them through `Display::GetMainDisplay()`, which is always
  null on the 3DS.
- **Sound:** `snd::Player` without cubeb. The whole stack (overlord, RPCs, bank loading, 989snd
  handlers, VAG streaming) runs as on PC. Audio output is opt-in with `sound = on` in
  `config.ini` (`platform/3ds/port/ctr_port.c`, `ctr_audio_*`): the 989snd software
  mixer (48 kHz stereo, PS2 SPU emulation, unchanged) runs on its own thread at `CTR_PRIO_SOUND`
  on the core `sound_core` names (default 0 on New 3DS, else 1; core 1 is the system core: the
  app gets 80% of it with `APT_SetAppCpuTimeLimit` and may create one thread there),
  filling 3 buffers of 1024 frames (21 ms each) that one ndsp channel plays; the DSP resamples
  to its 32.7 kHz. It needs the DSP firmware `sdmc:/3ds/dspfirm.cdc` (real hardware: dump it
  once with the DSP1 homebrew; Azahar's HLE audio accepts any file, the scripts create a
  placeholder). Without `sound = on`, or if the DSP can't start, a thread advances the 240 Hz
  handler tick without synthesizing samples, voices are dropped as they start
  (`vagvoice.cpp`), and the VAG stream position for spooled cutscenes comes from the frame
  counter (`iso.cpp`, fake VAG clock) instead of the stream voice, which never moves then.
  `kperf` reports the mixer's time as `sound` in the "perf threads" line.
- **Listener / DECI2:** `common/cross_sockets` compiles against libctru's BSD sockets.
  - `set_socket_timeout` does nothing; `SO_REUSEPORT` is skipped when it isn't defined.
  - `gk` starts `soc:U` (1 MB of RAM) only with `listener = on` in `config.ini`. Otherwise the
    DECI2 server fails to init and logs "REPL will not work", as on a PC with no network.
- **Excluded:**
  - discord-rpc: replaced by `game/external/discord_rpc_null.cpp`.
  - curl: `BackgroundWorker` web requests fail immediately on `__3DS__`.
  - imgui: the `DebugTextFilter` json functions moved to
    `game/tools/filter_menu/filter_menu_json.cpp`.
  - OpenGL renderer, SDL, cubeb, tree-sitter, sqlite, zstd.

## SD card layout

```
sdmc:/3ds/jak1/
  gk.3dsx                     the app (Homebrew Launcher lists sdmc:/3ds/**.3dsx)
  config.ini                  optional: all settings (docs/3ds-port/config_ini.md)
  data/                       = project path (file_util::get_jak_project_dir())
    out/jak1/iso/             everything the fake ISO serves, same as on PC:
                                KERNEL.CGO GAME.CGO *.DGO, VAGDIR.AYB, VAGWAD.*, *.STR,
                                *.SBK, *.MUS, 0COMMON.TXT..., TWEAKVAL.MUS, *.VIS
    log/                      runtime log (created)
  user/OpenGOAL/jak1/
    settings/                 game settings (created)
    saves/                    memory card files (created)
```

`OPENGOAL_3DS_SD_ROOT` (`common/util/FileUtil.h`) is `/3ds/jak1`. gk changes directory to `sdmc:/` at startup, so `std::filesystem` treats the path as absolute. `sdmc:/...` would be a relative path to it. When compiled C modules
are loaded from files (`out/jak1/cmod`), they would go under `data/` too, but the plan is to link
them into `gk.3dsx` through the static registry.

## Runtime-side changes for the 3DS (done)

(AI-assisted.) The workarounds (`stubs/other_games.cpp`, `tools/gen_mips2c_stubs.py`, the
`include/sys/mman.h` shim, `-fpermissive` on `klink.cpp`) are gone:

- **Games:** `OPENGOAL_ONLY_JAK1` (`game/common/game_common_types.h`, on for `__3DS__`) compiles
  the Jak 2/3/X code out of `game/runtime.cpp` and `game/mips2c/mips2c_table.cpp`.
- **EE memory:** `runtime.cpp` allocates it with `memalign(4096, EE_MAIN_MEM_SIZE)` on the 3DS (no
  mmap, no executable view, no `PROT_NONE` guard).
- **Memory layout:** `OPENGOAL_SMALL_MEMORY` (`common/goal_constants.h`, default on for `__3DS__`,
  `OG3DS_SMALL_MEMORY` in this CMake project): 48 MB of EE memory. Layout in
  `game/kernel/common/memory_layout.h`:

  | GOAL address | contents |
  |---|---|
  | `0x0000000` | kernel data, symbol table |
  | `0x013fd20` | global heap (42.5 MB) |
  | `0x2bc0000` | kernel stack (256 kB, top at `0x2c00000`) |
  | `0x2c00000` | debug heap (4 MB, only with `-debug`/`-debug-mem`) |
  | `0x3000000` | end |

  The GOAL code must be compiled with `OPENGOAL_SMALL_MEMORY=1` in goalc's environment
  (`gcommon.gc`: `SMALL_MEMORY`, `BIG_MEMORY` off, `END_OF_MEMORY`). The kernel dies at boot with a
  clear message if `*goal-small-memory*` doesn't match the runtime.
  - Why not the original 32 MB: with BIG_MEMORY off, Jak 1 (PC port, C mode, so no code in GOAL
    memory) allocates 22.5 MB of level heaps (2 x 11 MB, `alloc-levels!` is always called with
    `#f`, so the debug size) plus 5.4 MB of DMA buffers, 3.3 MB of GAME.CGO data and ~2.5 MB
    of other data at boot: ~34 MB. It ran out at 30.5 MB. With 42.5 MB there are ~9.5 MB free in
    village1.
  - Tested on the Mac with a small-memory host build (`-DOPENGOAL_SMALL_MEMORY=1` in
    CMAKE_CXX_FLAGS) and the game compiled with the C backend: `-boot` reaches the title and
    loads village1; `-debug-mem` + REPL `(lg) (test-play)` spawns Jak and runs, dying and
    respawning works.
  - `-debug` (debug segments) won't fit in the 4 MB debug heap; use `-debug-mem` for the REPL.
  - If `play` fails with `kmalloc: !alloc mem global-object (11264000 bytes)`, the GOAL code was
    built with the big-memory sizes (3x process heap, 3x DMA buffers: ~19 MB more). The first
    M3 run used a mirror built while `SMALL_MEMORY` in gcommon.gc was still broken (it evaluated
    to a list, so both SMALL_MEMORY and BIG_MEMORY were true); rebuilding fixes it.
- **Types / formats:** `klink.cpp` uses `u32`; the `%ld`/`%lx` with 64-bit values are fixed.
  (`%d`/`%x` with `uint32_t` still warn on ARM32; harmless, same size.)
- **Calls from GOAL to C functions on ARM32:** see "Typed kernel functions" in `c_backend.md`.

## Running in the emulator (M3)

(AI-assisted.) This needs Azahar in `~/devkitpro-3ds/emu` (see `toolchain.md`). Four steps:

```sh
# 1. compile the game to C in a mirror project (../p3ds: symlinks to this repo, its own out/),
#    with the small memory layout, then link every module into gk.3dsx
platform/3ds/tools/build_cmodules.sh            # --no-goal: only relink, --big-memory: 128 MB layout
# 2. copy gk.3dsx and out/jak1/iso to the SD card (default: Azahar's virtual SD card)
platform/3ds/tools/stage_sd.sh --proj ../p3ds --clean-logs
# 3. run for N seconds, then collect the logs into build-3ds/emu-run/
platform/3ds/tools/run_emu.sh --seconds 90
# 4. optional: attach gdb, then dump every thread's backtrace at the end (or at a crash)
platform/3ds/tools/run_emu.sh --seconds 60 --gdb     # --gdb-script FILE for custom gdb commands
```

- **C modules:** `-DOG3DS_CSRC_DIR=<proj>/out/jak1/csrc` (`platform/3ds/cmake/cmodules.cmake`)
  compiles every `csrc/*.c` with `-DGOALC_STATIC` into `og3ds_goal_modules`.
  `scripts/3ds/gen_c_registry.py` generates the registry, which is compiled into gk itself (not
  an archive), so it replaces the weak `goalc_register_static_modules`.
  - The staged `out/jak1/iso` must come from the same goalc run, because modules are matched by
    hash.
  - `OG3DS_SMALL_MEMORY` (default ON) must match `OPENGOAL_SMALL_MEMORY` in goalc's environment;
    `build_cmodules.sh` sets both.
  - A full rebuild takes about 3 minutes of goalc plus about 1 minute of devkitARM. Result:
    11.8 MB of code.
- **Where things are on macOS:**
  - Azahar's virtual SD card is `~/Library/Application Support/Azahar/sdmc` (`sdmc_directory` in
    `config/qt-config.ini`).
  - Its log is `~/Library/Application Support/Azahar/log/azahar_log.txt`.
- **Logs:** gk writes these on the SD card, flushed line by line:
  - `data/log/stdout.log`: everything printed, including GOAL output.
  - `data/log/gk.log`: the lg log at debug level.
  - Everything printed also goes to `svcOutputDebugString`, which shows up as `Debug.Emulated`
    in Azahar's log.
  - `abort()` (asserts, `lg::die`) flushes the log and calls `svcBreak(PANIC)`. Azahar logs:
    "Emulated program broke execution! Reason: PANIC".
- **Running without a visible terminal:**
  - Azahar has no headless mode.
  - Starting its binary directly from a shell with no GUI session (for example an agent
    session) hangs before it loads the ROM: 0 % CPU, and SIGTERM is ignored.
  - `open -n -a Azahar.app --args -w <3dsx>` (LaunchServices) works. A window opens.
  - `run_emu.sh` turns off the first-start wizard, the update check and the close confirmation,
    and sets `instant_debug_log` so that a SIGKILL doesn't lose log lines. The original config
    is kept as `qt-config.ini.orig`.
  - The run ends with SIGKILL.
- **Memory:**
  - A `.3dsx` in Azahar with `is_new_3ds=true` gets the New 3DS 124 MB application region: about
    105 MB of heap after the 11.8 MB of code.
  - `ctr_port.c` shrinks libctru's linear heap to 8 MB (`__ctru_linear_heap_size`). Otherwise
    libctru caps the regular heap (malloc) at 24 MB.
  - Fixed along the way (all platform-side):
    - The listener buffer (`XSocketServer`) was 32 MB; it is 1 MB on the 3DS.
    - The profiler event buffer was 10 MB; it is 1024 events on the 3DS.
    - The font banks built all their tries at static init: ~21 MB on ARM32, ~40 MB on 64-bit PCs.
      They are now built lazily on first use, which helps the PC too.
- **Threads:** the 3DS scheduler never time-slices between threads of equal priority.
  - With everything at 0x3F, the IOP's polling loop during overlord init starved the fake-ISO
    reader threads, and boot hung at `FS Open VAGDIR.AYB`.
  - Now the priorities are set per thread (`ctr_port.h`): IO helpers > IOP > listener > worker
    > EE. `IOP_Kernel::dispatch` also sleeps 100 µs when nothing is runnable.
- **`char` signedness:** on ARM, `char` is unsigned by default, while x86 and macOS arm64 use
  signed `char`. GOAL's `format` (`jak1/kprint.cpp`, `format_struct::data` holding -1 as "not
  set") printed 255 pad characters of `0xff` for every `~D`. The toolchain file now passes
  `-fsigned-char` to all 3DS code, including the C modules.

### Boot status (M3)

Tested in Azahar 2126.1.2 as a New 3DS, about 9 s of emulated boot. The runtime:
- starts
- allocates the 48 MB EE space
- registers the 518 static C modules
- loads KERNEL.CGO and GAME.CGO (346 objects)
- initializes the settings
- calls `play` and loads `title-vis` (TIT.DGO)
- reaches `kernel: machine started`

It then stops on:

    goalc: thread-suspend of camera-combiner with 1704 bytes of stack used, but the thread only has 1024

C-mode frames are bigger than native ones. The runtime-side stack floor for suspended processes
needs to cover this (runtime work). Before that, the log shows `kmalloc: !alloc mem global-object
(11264000 bytes)` during `play`, which the small memory layout has to account for.


## Renderer (M4 phase 1)

(AI-assisted.) `GfxPipeline::Ctr`, in `game/graphics/ctr`. It is the default on the 3DS, and on
PC it runs with `gk --ctr-gfx`.

- **Structure:**
  - `CtrRenderer` walks the Jak 1 DMA chain bucket by bucket, like
    `OpenGLRenderer::dispatch_buckets_jak1`, using 3DS bucket renderers. Buckets without one are
    skipped.
  - It runs on a render thread on core 2 of the New 3DS, one frame behind the game like the PS2
    (`send_chain` hands the chain over, `sync_path` waits for it). The game's DMA buffers are
    double buffered; merc bone matrices are not, so `send_chain` copies them first
    (`CtrRenderer::prepare_frame`).
  - `vsync` only waits if no vertical blank happened since the last call.
- **GPU interface:** `ctr_gpu.h`, a small C API with plain types (see "Types" above). There are
  two implementations:
  - `game/graphics/ctr/ctr_gpu_citro3d.c`: citro3d, with two picasso shaders
    (`platform/3ds/shaders`).
  - `game/graphics/ctr/ctr_gpu_soft.cpp`: a software rasterizer for PC. It writes PNG frames
    (`gk --ctr-gfx --ctr-dump DIR --ctr-dump-every N`), which lets the renderer be tested without
    an emulator.
- **Text, menus, debug draws** (buckets `debug`, `debug-no-zbuf`, `subtitle`): `CtrDirect`
  interprets the GIF packets, like `DirectRenderer`.
  - Textures come from `CtrVram`, an emulation of the 4 MB GS VRAM.
  - Texture page uploads (`texture-upload-now` and the `*-tex` buckets) and the font's
    `texture-relocate` are written into it.
  - Textures are decoded on first use from the TEX0 register: PSMCT32/24/16, PSMT8, PSMT4,
    PSMT8H/4HH/4HL, with CLUTs.
  - Decoded textures are cached and dropped when their VRAM changes.
- **Level backgrounds:** tfrag and static tie from `.c3l` files (`docs/3ds-port/c3l_format.md`,
  made by `ctr_level_converter`).
  - They are drawn from the `TFRAG_LEVEL0/1` buckets with the camera the game puts in those
    buckets. The math is the PC tfrag shader's, folded into one matrix per chunk.
  - Chunks are frustum culled by bounding sphere.
  - Levels load on first use from `out/jak1/c3l/<name>.c3l` and unload through `set_levels`.
- **Merc** (characters): `CtrMercRenderer`, models from the `.c3l` files (GAME.c3l has Jak and
  the shared ones), GPU skinning with 24-bone palettes, lighting like `merc2.vert` (3 directional
  lights + ambient) in the vertex shader, blend shapes (faces), eyes (`CtrEyeRenderer` composites
  the game's eye sprites into textures). The 3DS has no generic renderer: `draw-bones` draws
  effects that would use it with merc, and the death effect (dying enemies) hides the model after
  its first frames and launches the death sparks from the skeleton (`merc-death-spawn-3ds`).
- **Sprites and HUD:** `CtrSpriteRenderer` (bucket `sprite`): world 2D sprites, 3D sprites and
  the HUD, the `sprite3_3d.vert` math on the CPU, grouped by GS state, drawn as indexed quads.
  The sprite distorter (warp gate portals, heat haze) draws its fans with a copy of the screen
  drawn so far (`ctr_gpu_copy_screen`: one GPU copy, only in frames with distort sprites).
- **Fog:** the game's fog (like `tfrag3.vert`) plus fog towards the draw distance, computed in
  the level vertex shader and applied with a fog ramp texture in the second TEV stage. The
  screen is cleared to the fog color, under the sky.
- **Ocean** (`CtrOceanRenderer`, bucket `ocean-near`, after the level, merc and water): the
  PS2 ocean code (`draw-ocean`: VU1 DMA for the far/mid/transition/near ocean and a wave texture
  rendered every frame) cost about 12 ms per frame on the 3DS, so the small memory build skips it
  (`SKIP_OCEAN_DRAW`) and runs `draw-ocean-3ds` (`goal_src/jak1/engine/gfx/ocean/ocean.gc`)
  instead: it computes the 32x32 wave heights (so `ocean-get-height` works for boats and floating
  objects), picks the ocean height like `draw-ocean`, and sends one 27-quadword packet with the
  ocean map addresses, the height and the camera.
  - The renderer turns the ocean map into one static mesh when the map changes: the "mid" ocean
    grid (6x6 tiles of 8x8 cells of 96 m, cells without water left out by the map's masks), shore
    cells cut to the 24 m sub-cells of the transition masks, open cells as 48 m quads, vertex
    colors from the map's color grid. village1: 2002 cells (44 shore), 16k triangles, 150 KB.
  - One draw per visible tile (frustum and draw distance culled), fog like the level.
  - Texture: 32x32, one repeat per 96 m, shaded every frame from the wave heights (brighter on
    slopes facing the light and on crests), so the waves move like the PS2's.
  - Cost in Azahar (New 3DS): game thread 0.5 ms per frame for the `ocean` section (update-ocean
    + draw-ocean-3ds, nearly all of it the `ocean-interp-wave` mips2c call), render thread
    0.45 ms (texture shading + 2-4 tile draws, about 1-2k triangles in view).
  - Not done: the near ocean's wave geometry, the env map (sky reflection) pass, the far ocean
    (beyond the 4.6 km map, always in the fog at the default draw distance).
- **Sky** (`CtrSky`): the game blends the time of day sky and cloud textures (`make-sky-textures`,
  the tfrag-trans/sky-blend buckets); `CtrSkyBlendRenderer` does that blend on the CPU like the
  PC's `SkyBlendCPU`. The 3DS build runs `render-sky-3ds` instead of `render-sky-tng` (whose
  polygon clipping cost ~1.5 ms per frame): it sends the camera, the cloud scroll and where the sky
  polygons are, and `CtrSky` draws the same roof, cloud layers and horizon polygons in clip space
  (`ctr_gpu_draw_clip`), clipped by the GPU.
- **Not drawn yet:** shadows, envmap shine passes of tie and the ocean, debug lines,
  scissor.
- **Memory:** a level's textures are one block of linear memory (a texture pool), copied to VRAM
  with one GPU copy when there is room: the level the camera is in first (`ctr_gpu_pool_*`, see
  `performance.md`). Other textures (sprites, text, eyes, ocean) and meshes and the 1.5 MB vertex
  ring are in linear memory (24 MB total). The frame statistics in the log show what is left
  (`linear free`, `vram free`) and the pools in VRAM (`[ctr] textures:`); a level that doesn't
  fit logs `N textures/meshes could not be created`.
- **Screenshots:** `stage_sd.sh --screenshots N` makes gk save the top screen every N frames to
  `data/log/shot_<frame>.bmp`, read back from the GPU render target. `run_emu.sh` converts them
  to PNG in `build-3ds/emu-run/`.

```sh
build-plat-native/tools/ctr_level_converter --all out/jak1/fr3 ../p3ds/out/jak1/c3l
platform/3ds/tools/stage_sd.sh --proj ../p3ds --clean-logs --screenshots 120
platform/3ds/tools/run_emu.sh --seconds 150
```

- **Settings (`config.ini`):** read at startup from `sdmc:/3ds/jak1/config.ini` (on PC:
  `<project>/config.ini`), `key = value` lines, `#` comments. The log prints the values in use.
  `config_ini.md` has a complete example file and recommended settings.

  | key | default | meaning |
  |---|---|---|
  | `draw_distance` | 500 | meters; level chunks further away are not drawn, fog hides the cut (0 = off) |
  | `fog_start` | 0.6 | the extra fog starts at this fraction of the draw distance |
  | `fog` | on | the game's own distance fog |
  | `lod_distance` | 200 | meters; near tfrag version up to here, coarse beyond (0 = always near) |
  | `far_level_distance` | 40 | meters; a level whose bounds are further away (seen from a neighbouring level) is drawn with only its lowres tfrag, no tie (0 = off) |
  | `detail_scale` | 1 | multiplies the draw distance of small and medium tie objects and shrubs |
  | `merc` | on | draw merc models |
  | `sprites` | on | draw world sprites (particles); the HUD is always drawn |
  | `max_sprites` | 1000 | world sprites per frame |
  | `ocean` | on | draw the ocean |
  | `envmap` | on | the envmap shine of merc models (power cells, precursor metal; c3l v9 files) |
  | `sky` | on | the sky (time of day texture, clouds); off: the screen is cleared to the fog color |
  | `distort` | off | the sprite distorter (portals, heat haze); costs one screen copy in frames that have distort sprites |
  | `rgba4_as_rgba8` | auto | store RGBA4 level and model textures as RGBA8 (twice their memory). Azahar (OpenGL and Vulkan) draws RGBA4 textures as noise or a solid color: crates, orbs, Jak's hair. `auto`: on in the emulator, off on the 3DS |
  | `vram_textures` | auto | level texture pools in VRAM while there is room (the GPU reads VRAM much faster). `auto`: on (also in the emulator since 2026-10-01: Azahar draws VRAM textures right; only RGBA4 was noise, and `rgba4_as_rgba8` handles that) |
  | `mipmaps` | on | mip levels of the level textures (c3l v8): `off`, `on` (nearest level), `trilinear` (blends two levels: smoother, slower) |
  | `gpu_profile` | off | every 2.5 s one group of renderers (level, merc + eyes, sprites, ocean, direct) is left out and `[ctr] gpu profile:` logs what each costs the GPU. The picture flickers |

- **Timing in the log:** every 300 frames or 5 seconds, `[ctr] render ms/frame` (render thread:
  waiting for the GPU, building commands, submitting; GPU time from citro3d), `[ctr] build
  ms/frame by renderer`, and `[ctr] game thread ms/frame` (logic, waiting for the render thread,
  vsync, texture uploads, bone snapshot).
- **Status in Azahar (New 3DS), old numbers from M4 phase 1:** the title screen shows village1 (tfrag + tie) and the PRESS
  START text.
  - About 1250 draws and 103k triangles per frame (the title camera sees nearly the whole level).
  - About 8-9 fps, against about 13 fps for the game logic alone with the null renderer.
- **Memory:**
  - `ctr_port.c` sets the linear heap to 24 MB. The regular heap gets about 80 MB.
  - IOP coroutine stacks are 512 KB on the 3DS (3 MB on PC; about 9 threads).

## Native versions of hot mips2c functions

(AI-assisted.) mips2c code emulates the MIPS/VU0 registers in memory (~10 ARM instructions per
MIPS instruction on the 3DS). Every mips2c function the game logic uses has a plain C++ version in
`game/mips2c/jak1_functions/native_*.cpp` (see `game/mips2c/mips2c_native.h`), used by default on
every platform:

- collision: `collide-do-primitives`, `moving-sphere-triangle-intersect`, `collide-probe-node`,
  `collide-probe-instance-tie`, `__pc-upload-collide-frag`, collide-cache methods 26-30 and 32,
  collide-cache-prim 9 and 10, collide-puss-work 9 and 10, collide-shape-prim-mesh 12-14,
  collide-mesh 11, 12, 14 and 15, collide-edge-work 15, 16 and 18, collide-edge-hold-list 10
- animation: `cspace<-parented-transformq-joint!`
- particles: `sp-process-block-2d`, `sp-process-block-3d`, `sp-launch-particles-var`,
  `particle-adgif`
- ocean: `ocean-interp-wave` (the wave heights `ocean-get-height` reads, every frame)
- GOAL functions the collision natives call for every triangle: `ray-sphere-intersect`,
  `ray-cylinder-intersect`, `moving-sphere-sphere-intersect` (collide-func.gc) and
  `closest-pt-in-triangle` (geometry.gc, with `vector-segment-distance-point!` inlined). They have
  no mips2c version: in goal_src they are `def-mips2c`, with the GOAL code kept in a comment, and
  they give the same results as the C code goalc makes of that GOAL code. The natives call them
  directly while their symbols hold them, and through the symbol after a GOAL redefinition.
  (A call from a native to compiled GOAL code went through `goalc_call_goal8` with 8 arguments,
  the vectors on the stack and a second call to `pc-port-raw-ray-sphere-implementation`.)
- GOAL code calls these four and `cspace<-parented-transformq-joint!` (every joint, every frame)
  without the adapter thunk the other natives go through (8 arguments copied, a perf section,
  stack scratch space): `MIPS2C_NATIVE_IMPL_GOAL` registers a direct entry, `native_as_goal`.

Still mips2c: the renderers' functions (bones, merc, generic, tie, tfrag, shadow, sky, ocean
drawing, ripple, time-of-day colors, draw-string, textures) and `calc-animation-from-spr` (never
called: `*use-new-decompressor*` is #t).

They do the same float operations grouped the same way as the mips2c code, so results are
bit-identical (clang fuses `a + b * c` within one expression on arm64; the 3DS has no FMA). They
also keep the original's quirks: registers passed to callees that the callee ignores or not, the
process pointer (s6) the original clobbers, stores in branch delay slots, deliberate crashes.

- `OPENGOAL_MIPS2C_VERIFY=1` (host, C backend): every call runs the mips2c and the native
  version on the same inputs and compares v0 and every byte stored (logged stores); functions
  with GOAL callbacks (`sp-process-block-2d`) are checked on one call in
  `OPENGOAL_MIPS2C_VERIFY_FULL_EVERY` (default 8) by saving, restoring and comparing all of GOAL
  memory. Mismatches are logged as `mips2c verify MISMATCH`, a summary line `mips2c verify:` every
  20 s. `OPENGOAL_MIPS2C_VERIFY_SELF=1` compares mips2c with itself (tests the checker).
- `OPENGOAL_MIPS2C_NATIVE=0` (host): use the mips2c versions. On the 3DS: `config.ini`
  `mips2c_native_off = all`, or the names of the functions to turn off, separated by spaces or
  commas (e.g. `sp-launch-particles-var`). stdout.log says which are off. A native
  that another native calls directly (`native_stub_slot`) still runs from there: turn its callers
  off too.
- Native code must store to GOAL memory only through `gstore*` (verify mode logs them).
- Quadwords that the mips2c code moves with `lqc2`/`sqc2` (which assert 16-byte alignment) or
  `lq`/`sq` (which align down) should use `gload_q`/`gstore_q`: the compiler then moves them with
  VFP loads or `ldm`/`stm` instead of byte copies through the stack. Only for those addresses: an
  unaligned address would fault on the 3DS. Hot loops can also keep `g_ee_main_mem` in a local
  (see `ParticleMem` in `native_sparticle.cpp`): every store to GOAL memory otherwise makes the
  compiler load it again.
- The 3DS build compiles the native files with -O3 (`platform/3ds/CMakeLists.txt`): their small
  loops over vector lanes stay in stack arrays at -O2.
- A native that calls another native through a symbol or method calls its `_impl` directly when
  the symbol holds that function's stub (`native_stub_slot`), with its scratch space below its
  own.
- Run on the Mac with scripted input and a clean user folder, e.g.
  `OPENGOAL_MIPS2C_VERIFY=1 OPENGOAL_PAD_SCRIPT=platform/3ds/tests/gameplay.pad gk --config-path <empty dir> --proj-path <C mirror> --null-gfx -- -boot -fakeiso -cbackend`
  (without `--config-path` the menu taps change your real settings).

Azahar, gameplay.pad, last 20 s (Jak on Geyser Rock), before (3ds-port 7e36955c) -> after:
game logic 94.4 -> 66.0 ms per frame (10.1 -> 14.4 fps); method 9 collide-cache-prim 9.3 -> 7.7
(now includes moving-sphere-triangle-intersect, 5.6 before, and collide-do-primitives 2.7),
cspace<- 8.9 -> 4.0, sp-process-block-2d 5.2 -> 2.0, collide-cache 32 / 26 / 27(+29)
5.0 / 4.7 / 4.2 -> 1.7 / 1.8 / 1.8 ms.

### Differential tests

`test/mips2c_native/run.sh [host] [fma] [arm] [coverage] [bench] [-- test args]` (default: host
arm) builds a test program that links the mips2c and the native versions without the rest of the
runtime: GOAL memory, symbols, types and the GOAL functions they call are fakes set up by each test
(`test/mips2c_native/tests_*.cpp`). For thousands of generated inputs per function (random data
with edge values: 0, -0, ties, full caches, overflowing lists, NaN), it runs both versions from the
same memory and compares v0, every byte of GOAL memory outside the stack, the GOAL calls made (the
arguments the callee uses and the process pointer) and the VU0 random generator.

- host: g++ -O2. fma: clang++ -mfma -ffp-contract=on (fuses `a + b * c` in one expression like
  clang on arm64, so a native that groups float operations differently fails).
- arm: ARMv6K + VFPv2 like the 3DS (arm-linux-gnueabihf-g++, run with qemu-arm), in the 3DS's VFP
  mode: default NaN and flush-to-zero, which libctru sets for every thread (FPSCR 0x03000000).
  Without default NaN, GCC's fused `vmls` (non-fused on VFPv2, same results otherwise) gives NaNs
  the opposite sign than a `vmul` and `vsub`, so natives and mips2c could differ in NaN signs only.
- GOAL functions with a native version (above) have no mips2c version: their reference is the C
  code goalc makes of the GOAL function (`test/mips2c_native/goalc_ref/<module>.c`, taken from
  `out/jak1/csrc` with the object file's constants by `goalc_ref/extract.py`), loaded into the
  test's GOAL memory like the linker would and run on a stack in GOAL memory (`goalc_ref.cpp`).
  The tests of the natives that call them run the compiled GOAL code on the mips2c side, and the
  `(GOAL callees)` variants bind the symbols to it on both sides (a GOAL redefinition).
- coverage: gcov line coverage of the tested mips2c functions (100% for all of them).
- bench: ARM instructions per call of each version (their own code, not the GOAL functions they
  call), counted with qemu-arm.
- Test args: `--filter SUBSTR`, `--scale X` (cases), `--seed N`, `--case N`, `--reports N`,
  `--self` (mips2c against mips2c: tests the harness).

ARM instructions per call on the tests' inputs (`run.sh bench`: the functions' own code, not the
GOAL functions they call; ARMv6K, -O2 for mips2c and -O3 for the natives like the 3DS build).
(`__pc-upload-collide-frag` is the same C++ in both.) Rows marked * include the GOAL collision
helpers that have native versions: goalc's C code on the mips2c side (the call itself not
counted), the inlined natives on the native side. The four GOAL functions are against goalc's C.

| function | mips2c | native | ratio |
|---|---:|---:|---:|
| `moving-sphere-triangle-intersect` * | 1992 | 533 | 3.7x |
| `collide-do-primitives` * | 2213 | 911 | 2.4x |
| `(method 9 collide-cache-prim)` * | 22709 | 5539 | 4.1x |
| `(method 26 collide-cache)` | 11725 | 2772 | 4.2x |
| `(method 29 collide-cache)` | 15952 | 2551 | 6.3x |
| `(method 27 collide-cache)` | 27830 | 5337 | 5.2x |
| `(method 32 collide-cache)` | 30038 | 2584 | 11.6x |
| `(method 28 collide-cache)` | 11317 | 2636 | 4.3x |
| `(method 30 collide-cache)` | 9413 | 581 | 16.2x |
| `(method 10 collide-cache-prim)` * | 1877 | 227 | 8.3x |
| `(method 9 collide-puss-work)` * | 12150 | 2714 | 4.5x |
| `(method 10 collide-puss-work)` | 2587 | 126 | 20.5x |
| `(method 12 collide-shape-prim-mesh)` | 17222 | 2341 | 7.4x |
| `(method 13 collide-shape-prim-mesh)` | 19617 | 2598 | 7.6x |
| `(method 14 collide-shape-prim-mesh)` | 16795 | 2302 | 7.3x |
| `cspace<-parented-transformq-joint!` | 2220 | 266 | 8.4x |
| `collide-probe-node` | 3731 | 444 | 8.4x |
| `collide-probe-instance-tie` | 4510 | 441 | 10.2x |
| `(method 11 collide-mesh)` * | 7181 | 2356 | 3.0x |
| `(method 12 collide-mesh)` * | 3850 | 1223 | 3.1x |
| `(method 14 collide-mesh)` | 5804 | 1165 | 5.0x |
| `(method 15 collide-mesh)` | 12513 | 2073 | 6.0x |
| `(method 16 collide-edge-work)` | 7723 | 1066 | 7.2x |
| `(method 15 collide-edge-work)` | 93947 | 20402 | 4.6x |
| `(method 10 collide-edge-hold-list)` | 150 | 86 | 1.8x |
| `(method 18 collide-edge-work)` | 3139 | 915 | 3.4x |
| `sp-process-block-2d` | 19215 | 4534 | 4.2x |
| `sp-process-block-3d` | 21529 | 6014 | 3.6x |
| `ocean-interp-wave` | 60782 | 12192 | 5.0x |
| `particle-adgif` | 803 | 342 | 2.3x |
| `sp-launch-particles-var` | 2760 | 1297 | 2.1x |
| `ray-sphere-intersect` (goalc) | 91 | 47 | 1.9x |
| `ray-cylinder-intersect` (goalc) | 106 | 73 | 1.5x |
| `moving-sphere-sphere-intersect` (goalc) | 257 | 174 | 1.5x |
| `closest-pt-in-triangle` (goalc) | 200 | 175 | 1.1x |
