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
| `og3ds_stubs` | TEMPORARY link stubs for Jak 2/3/X symbols (see "Changes needed" below) |
| `og3ds_fmt`, `og3ds_libco` | third party (libco uses its ARM32 backend) |

Status: everything compiles and `gk.3dsx` links. Sizes: 2.3 MB of code and 1.3 MB of BSS. It has
not been run on hardware or in an emulator yet. Expected first failure: allocating the 128 MB of
EE memory (see "Changes needed" below).

## Platform pieces

- **Types:** on `__3DS__`, `s32`/`u32` are `int`/`unsigned int` (`common/common_types.h`),
  because newlib's `int32_t` is `long`. As a result, `<3ds.h>` (whose `u32` is `uint32_t`) can't
  be included in the same file as `common_types.h`. All libctru calls go through
  `platform/3ds/port/ctr_port.h`, which uses plain C types.
- **POSIX:** `OS_POSIX` is defined on the 3DS. newlib and libctru provide pthreads,
  `clock_gettime`, `unistd` and BSD sockets. `platform/3ds/include/sys/mman.h` is a
  malloc-backed `mmap`/`munmap`/`mprotect`, used only by the kernel library so that
  `runtime.cpp` compiles unchanged.
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

## Changes needed in runtime-owned files

The 3DS build currently works around each of these. Items marked (workaround) should be fixed
properly; the workarounds can then be removed.

1. **`game/runtime.cpp`**
   - 157-227: `mmap`/`mprotect`/`munmap` of `EE_MAIN_MEM_SIZE` (128 MB).
     - (workaround) The `sys/mman.h` shim compiles it, but a 128 MB `memalign` fails on every
       3DS model.
     - Needs a `__3DS__` path that allocates a smaller EE space; see item 7.
     - `mprotect` of the low 512 KB does nothing on the 3DS.
   - 52-84, 230-275, 318-343, 373-377: includes and unconditional calls to jak2/jak3/jakx
     (`kboot/kdgo/klisten/kscheme_init_globals`, overlord init globals, `goal_main`,
     `start_overlord_wrapper`, `vag_init_globals`, `init_globals_streamlist`, ...).
     - (workaround) `platform/3ds/stubs/other_games.cpp`.
     - Fix: guard with a "games built" macro, e.g. `OPENGOAL_JAK1_ONLY` or
       `#ifndef __3DS__`.
2. **`game/mips2c/mips2c_table.cpp`**
   - Declares and registers the `link()` functions of all games.
   - Calls `jak2/jak3/jakx::alloc_heap_object` and `u32_in_fixed_sym` (around line 930).
   - (workaround) `platform/3ds/tools/gen_mips2c_stubs.py` generates no-op `link()`s for the
     other games at build time. Fix: guard per game.
3. **`game/kernel/common/klink.cpp:31-38`:** passes a `uint32_t*` to `arm64_write_mov32(u32*)`.
   On the 3DS `uint32_t` is `unsigned long`, so this is a hard error. (workaround) The file is
   compiled with `-fpermissive`. Fix: use `u32` (`Ptr<u32>`, `cast<u32>()`).
4. **`game/kernel/common/goal_c_abi.h:86-87`:** `int32_t*` / `uint32_t*` fields. These are
   `long` on the 3DS, while kernel code uses `s32`/`u32` = `int`. Sizes match, but converting
   pointers needs casts. Keep the C side on `<stdint.h>` types and cast in C++, or use
   `int`/`unsigned` in the ABI header.
5. **`printf` formats that are wrong on ARM32** (from `-Wformat`, devkitARM):
   - `kscheme.cpp:289,297`: `%lx`/`%ld` with `u64`/`s64` in `inspect_integer` /
     `inspect_binteger`, so output is garbage and varargs are misread. Use `%llx`/`%lld` or
     `PRIx64`.
   - `ksocket.cpp:38`: `%lx`/`%ld` with `int64_t`/`u64`.
   - `kprint.cpp:135`: `%lx` with `uintptr_t`.
   - `%d`/`%x` with `uint32_t` (warnings only, harmless on ARM32): `kprint.cpp:166`,
     `klink.cpp:112-116`, `kmalloc.cpp:136`, `jak1/klink.cpp:239,255,271,395`.
   - Not caught because it isn't compiled for the 3DS: `jak1/kboot.cpp:134` (`%ld`).
6. **`game/kernel/jak1/kscheme.cpp:272-295, 408-434`:** outside `__aarch64__`, the x86
   `_arg_call_systemv`/`_stack_call_systemv` trampolines are referenced. They only link today
   because `--gc-sections` removes the unused functions (a build without `-ffunction-sections`
   fails to link). Guard them for C mode / `__arm__`.
7. **Memory layout** (`common/goal_constants.h:109-110`, `game/kernel/common/memory_layout.h`,
   `goal_src/jak1/kernel/gcommon.gc:19`, `jak1/kboot.cpp:109`, `jak1/klink.cpp:565`,
   `jak1/kmachine.cpp:310`):
   - The 3DS needs an `EE_MAIN_MEM_SIZE` of about 32 MB with `BIG_MEMORY` off, set the same in
     C++ and GOAL.
   - The GOAL stack should sit just above `GLOBAL_HEAP_END` instead of at the top of 128 MB.
   - `MasterDebug` off (no debug heap at `0x5000000`).
   - Budget: Old 3DS app region 64 MB, New 3DS 124 MB (178 MB in extended mode).
   - Already used by gk: code 2.3 MB, BSS 1.3 MB, thread stacks about 1 MB, IOP / overlord
     buffers, and the `soc:U` 1 MB if enabled.
8. **Host stack for the EE thread** (`game/system/SystemThread.cpp`, platform-owned): 512 KB
   right now. If the C backend runs deep native recursion on the host stack before switching to
   the GOAL stack, change the size there.

Nothing else in `game/kernel/**` failed to compile. All Jak 1 kernel files build with devkitARM
as they are.

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
