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

### Install and start (recommended: CIA)

1. Put the SD card back, start FBI, open `cias/`, select `jak1.cia`, "Install CIA".
2. Start "OpenGOAL Jak1" from the HOME Menu (it has an icon but no banner animation).

To try the CIA in Azahar first: `run_emu.sh --cia build-3ds/jak1.cia --stage "--proj ../p3ds"`
(installs it in the emulator and runs the installed title).

The CIA asks for the New 3DS 124 MB memory mode (`platform/3ds/cia/gk.rsf`: `SystemModeExt:
124MB`, 804 MHz, L2 cache). The game needs about 95 MB: 48 MB of PS2 memory, 12 MB of code, 24 MB
of GPU memory, plus buffers.

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
- Optional flag files in `sdmc:/3ds/jak1/`: `args.txt` (game arguments, default `-boot
  -cbackend`), `listener` (Wi-Fi REPL), `screenshots` (a number N: save the top screen every N
  frames to `data/log`), `pad_script.txt` (scripted input, see below), `use_syscore`
  (experimental: IOP / IO threads on core 1; hangs at boot in Azahar, untested on hardware).

### Automated gameplay test (scripted input)

`game/sce/pad_script.h` replays a timeline of pad states on controller 0 (format in the header):
buttons and sticks per game frame, `wait *target* <state>` (stops the script's clock until Jak is
in that state), `print SYM`, `pos *target*` (position in meters and state), `crash` (tests the
crash screen), `exit`.

- 3DS: `sdmc:/3ds/jak1/pad_script.txt` (`stage_sd.sh --pad-script FILE`).
- PC: `OPENGOAL_PAD_SCRIPT=FILE gk ...` (with `--null-gfx`, or `--ctr-gfx` to get frames).
- `platform/3ds/tests/gameplay.pad`: title -> Start -> New Game (taps Cross, fine with an empty
  save) -> skips the intro cutscene (taps Triangle) -> waits for `target-stance` on Geyser Rock ->
  walks forward, jumps while walking, jumps, punches, spin kicks, crouches, walks left and right,
  logging Jak's position and state after each step.

```sh
platform/3ds/tools/run_emu.sh --seconds 900 --out build-3ds/emu-game \
  --stage "--proj ../p3ds --clean-logs --clean-user --screenshots 300 --pad-script $PWD/platform/3ds/tests/gameplay.pad"
grep -a "pad script" build-3ds/emu-game/stdout.log
```

`run_emu.sh` takes a lock (`$TMPDIR/opengoal-azahar.lock`) because Azahar and its virtual SD card
are shared; `--stage` stages the SD card inside the lock.

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
- **Sound:** `snd::Player` without cubeb. On the 3DS a thread advances the 240 Hz handler tick,
  so sounds start and stop and `snd_GetTick()` moves, but no samples are synthesized. Audio
  output (ndsp) is later work. FakePlayer was not used because its API doesn't match what
  `sndshim.cpp` calls.
- **Listener / DECI2:** `common/cross_sockets` compiles against libctru's BSD sockets.
  - `set_socket_timeout` does nothing; `SO_REUSEPORT` is skipped when it isn't defined.
  - `gk` starts `soc:U` (1 MB of RAM) only if `sdmc:/3ds/jak1/listener` exists. Otherwise the
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
  args.txt                    optional: game args, whitespace separated (default: -boot)
  listener                    optional, empty file: enable Wi-Fi REPL (soc:U, 1 MB RAM)
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
  - It runs synchronously on the EE thread: `send_chain` draws the frame, `vsync` waits for the
    vertical blank.
- **GPU interface:** `ctr_gpu.h`, a small C API with plain types (see "Types" above). There are
  two implementations:
  - `platform/3ds/port/ctr_gpu_citro3d.c`: citro3d, with two picasso shaders
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
- **Not drawn yet:** merc (characters), generic, shrub, sprites / HUD, sky, ocean, particles,
  shadows, eyes, debug lines, fog, scissor.
- **Screenshots:** `stage_sd.sh --screenshots N` makes gk save the top screen every N frames to
  `data/log/shot_<frame>.bmp`, read back from the GPU render target. `run_emu.sh` converts them
  to PNG in `build-3ds/emu-run/`.

```sh
build-plat-native/tools/ctr_level_converter --all out/jak1/fr3 ../p3ds/out/jak1/c3l
platform/3ds/tools/stage_sd.sh --proj ../p3ds --clean-logs --screenshots 120
platform/3ds/tools/run_emu.sh --seconds 150
```

- **Status in Azahar (New 3DS):** the title screen shows village1 (tfrag + tie) and the PRESS
  START text.
  - About 1250 draws and 103k triangles per frame (the title camera sees nearly the whole level).
  - About 8-9 fps, against about 13 fps for the game logic alone with the null renderer.
- **Memory:**
  - `ctr_port.c` sets the linear heap to 24 MB. The regular heap gets about 80 MB.
  - IOP coroutine stacks are 512 KB on the 3DS (3 MB on PC; about 9 threads).
