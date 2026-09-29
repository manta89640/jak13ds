# 32-bit host audit (3DS port)

(AI-assisted.) Report only; nothing in `game/kernel/**` or `game/runtime.cpp` was changed.

Target: Nintendo 3DS, ARM11 MPCore (ARMv6K, VFPv2, no NEON), ILP32, devkitARM GCC 15 + newlib.
Scope: Jak 1 only, no OpenGL renderer, GOAL compiled via the C backend (`docs/3ds-port/c_backend.md`).

## How this was checked

1. grep over `game/` and `common/` for pointer/integer casts, `sizeof(void*)`/`size_t`,
   `uintptr_t`, `mmap`, `EE_MAIN_MEM_*`, printf length modifiers, inline asm, SIMD.
2. Every Jak 1 relevant translation unit (`game/kernel/common`, `game/kernel/jak1`,
   `game/mips2c/jak1_functions`, `game/overlord/{common,jak1}`, `game/sce`, `game/system`,
   `common/util`, `common/log`, 111 files) was syntax-checked with
   `arm-none-eabi-g++ -std=gnu++20 -march=armv6k -mfloat-abi=hard -fsyntax-only`.
   Result: **no `static_assert(sizeof(...))` in these files fails on 32-bit**; all 25
   `game/mips2c/jak1_functions/*.cpp` compile cleanly (`-c -O2`) for ARMv6K with the scalar SIMD
   path. The remaining failures are listed below (mostly platform headers, plus one real
   ARM-EABI type issue: `s32` is `long`).

Legend for "matters?": **yes** = must be handled for the Jak-1 / no-OpenGL / C-backend 3DS build,
*later* = only once that subsystem is ported, no = PC-only code (OpenGL renderer, jak2/3/X, tools).

## Severity 1: blocks the port

| # | Location | Issue | Matters? |
|---|---|---|---|
| 1.1 | `common/goal_constants.h:109` | `EE_MAIN_MEM_SIZE = 128 MB`. The Old 3DS application region is 64 MB total (New 3DS: 124 MB, 178 MB in extended mode), shared with code, the linear heap for the GPU and audio. A 128 MB GOAL space does not fit. | yes |
| 1.2 | `game/kernel/common/memory_layout.h:7,19,27` + `goal_src/jak1/kernel/gcommon.gc:19` | `BIG_MEMORY = true` puts the global heap end at `0x3eb82e0` (~62.7 MB of heap); debug heap at `0x5000000..0x7ff0000` (only with `MasterDebug`). With `BIG_MEMORY = false` (must be changed in both files together), the global heap is `0x13fd20..0x1ffc000` (~30.7 MB), i.e. the whole used range fits in 32 MB. | yes |
| 1.3 | `game/kernel/jak1/kboot.cpp:109`, `game/kernel/jak1/klink.cpp:565`, `game/kernel/jak1/kmachine.cpp:310` | GOAL stack is placed at `g_ee_main_mem + EE_MAIN_MEM_SIZE - GOAL_STACK_TOP_OFFSET` and the debug heap end is computed as `(0xffffffff - DEBUG_HEAP_SPACE_FOR_STACK + 1) & 0x7ffffff` (hardcodes the 128 MB layout). With a smaller EE space the stack must move to just above `GLOBAL_HEAP_END` (or a separate region inside GOAL memory) and the debug heap must stay disabled (`MasterDebug = 0`). | yes |
| 1.4 | `common/goal_constants.h:110`, `game/runtime.cpp:157-227` | `EE_MAIN_MEM_MAP = 0x2123000000` (">32-bit on purpose") is passed to `mmap` as a hint; on ILP32 the cast `(void*)EE_MAIN_MEM_MAP` silently truncates to `0x23000000`. The 3DS has no `mmap`/`mprotect`/`MAP_32BIT`/`mach_vm_remap`: EE memory must come from the app heap (`malloc`/`memalign`, or `svcControlMemory` for a fixed virtual address). The `mprotect(PROT_NONE)` of the low 512 KB (`runtime.cpp:227`) has no 3DS equivalent (null-deref detection is lost; optional). No executable mapping is needed in C mode. | yes |
| 1.5 | `common/common_types.h` (via `<cstdint>`) | On arm-none-eabi/newlib, `int32_t` is **`long int`**, `uint32_t` is `unsigned long`. `s32`/`u32` are therefore distinct types from `int`/`unsigned`, which breaks function-pointer conversions and pointer comparisons that are fine on every PC ABI. Compile errors found: `game/system/IOP_Kernel.h:198-199` (`int (*)(void*)` stored in an `s32 (*)(void*)`), `game/overlord/jak1/overlord.cpp:41` (`VBlank_Handler`), `game/overlord/jak1/dma.cpp:40` (`sceSdSetTransIntrHandler`), `game/kernel/common/kdsnetm.cpp:27` (`GoalProtoHandler`), `game/kernel/common/kprint.cpp:274` (`s32*` vs `int*`). Simplest fix: on the 3DS define `s32`/`u32` as `int`/`unsigned int` in `common/common_types.h` (with `static_assert(sizeof == 4)`), instead of touching each site. Also affects overload sets and `fmt`/`printf` formats of `s32`. | yes |
| 1.6 | `game/kernel/common/kscheme.h:34-35`, `game/mips2c/mips2c_private.h:377-387`, `game/mips2c/mips2c_table.cpp:18,25`, `game/kernel/jak1/kscheme.cpp` (`_arg_call_*`/`_stack_call_*`), `game/kernel/asm_funcs_*.{s,asm}` | Host<->GOAL calls go through x86-64/arm64 assembly trampolines. Already covered by the C-backend design (`goalc_fn`, `goalc_ctx`), but the ARM32 "call on stack" helper and the "`u32` parameters through `goalc_fn8`" issue from `c_backend.md` ("Known later work") are both hard blockers. | yes (runtime agent) |

## Severity 2: wrong results / crashes at runtime, but compiles

| # | Location | Issue | Matters? |
|---|---|---|---|
| 2.1 | `game/kernel/jak1/kboot.cpp:134`, `game/kernel/common/kscheme.cpp:289,297`, `game/kernel/common/ksocket.cpp:38`, `game/kernel/common/kprint.cpp:135` | `%ld`/`%lx` used with `s64`/`u64` (and `(uintptr_t)g_ee_main_mem` at `kprint.cpp:136`). `long` is 32 bits on ARM32, so varargs are misaligned: garbage output or a crash in `cprintf`/`sprintf`. Use `%lld`/`PRIx64` or fmt. `kprint.cpp:135-136` only runs on listener reset; `kscheme.cpp:289,297` is `print_integer`/`print_binteger` (hot, used by `format`). | yes |
| 2.2 | GOAL `format` (`game/kernel/common/kprint.cpp`, `format_impl_*`) | Check any host `sprintf` with a width/length built from GOAL arguments; the C-backend passes 64-bit arguments as `u64` on a 32-bit host. | yes (verify) |
| 2.3 | Unaligned access, e.g. `common/util/crc32.h` (aarch64 path `*reinterpret_cast<const u32*>`), `Ptr<T>::operator*` (`game/kernel/common/Ptr.h:40-49`), mips2c `memcpy` helpers | ARMv6K allows unaligned `LDR/STR/LDRH` only when SCTLR.U is set (believed to be the case on Horizon; verify on hardware) but **not** `LDRD/STRD/LDM/STM/VLDR/VSTR`. GCC emits these for `u64`, `double`, `float` pairs and struct copies. Any `*(u64*)` / `*(float*)` through a non-word-aligned GOAL address will fault on hardware (emulators usually do not fault). GOAL data is mostly 4/16-aligned, but packed art/DMA data is not always. The scalar crc32 path is byte-wise and safe. | yes (runtime risk) |
| 2.4 | `game/kernel/common/goalc_runtime.cpp:194-196,415-417`, `game/kernel/jak1/goalc_kernel.cpp:76,206` | Host pointer <-> GOAL offset arithmetic via `uintptr_t` and `goalc_mem`: correct on ILP32 as long as the result is kept as a 32-bit offset; `(uint64_t)(sp - (uintptr_t)goalc_mem)` is fine. No truncation bug found, listed for review when the ARM32 context code lands. | yes (review) |
| 2.5 | `game/mips2c/mips2c_private.h:305` | `gprs[gpr].du64[0] = ((const u8*)sym_addr) - g_ee_main_mem;` pointer difference is `ptrdiff_t` (32-bit) then zero/sign extended into a 64-bit register: fine as long as GOAL memory < 2 GB (it is). | no action |
| 2.6 | `game/runtime.cpp:209-214`, `game/kernel/jak1/kmachine.cpp:369`, `game/overlord/jak3/pagemanager.cpp:363` etc. | `(u64)host_ptr` widening casts for logging / autosplitter: harmless on 32-bit (zero-extended). | no |

## Severity 3: data formats / PC-only code

| # | Location | Issue | Matters? |
|---|---|---|---|
| 3.1 | `common/util/compress.cpp:18-41` | `compress_zstd` writes a `size_t` length header (8 bytes on 64-bit hosts). An `.fr3` produced on a PC would be misread on a 32-bit host. | *later* (only if `.fr3` is reused) |
| 3.2 | `common/util/Serializer.h:119-163` | `Serializer` stores string/vector lengths as `size_t`: same cross-host format problem (`.fr3`, level data built by `goalc/build_level`, decompiler output). Fix: serialize as `u64`. | *later* |
| 3.3 | `game/graphics/pipelines/opengl.cpp:87`, `common/dma/dma_copy.h:22` | `FixedChunkDmaCopier(EE_MAIN_MEM_SIZE)` asserts the size is a multiple of its 128 KB chunk; the copied DMA data can grow large. Any 3DS DMA path should size it from the reduced EE size. | no (OpenGL) |
| 3.4 | `game/overlord/jak1/ramdisk.cpp:28,77` | `DEVTOOL_IOP_MEM_ALLOC` (~2 MB) is allocated and wasted in devtool mode; IOP allocations are plain `malloc` (`game/system/iop_thread.cpp:53`). Make sure the devtool path is off on 3DS. | yes (memory budget) |
| 3.5 | `game/sound/sdshim.h:24` | `spu_memory[0x15160 * 10]` (~864 KB) static. OK, but counts against the budget. | *later* |
| 3.6 | `common/goos/Object.h:490,510,561` | Hashes `sizeof(const char*)` bytes of a pointer: 4 on ARM32. Consistent within one process, so fine. | no action |
| 3.7 | `game/kernel/jak1/kscheme.cpp:1514` | `*scratch-top*` = `0x70000000` (PS2 scratchpad address) is only compared against, never dereferenced by host code (the PC port uses a fake scratchpad in GOAL memory). | no action |

## Platform (not pointer size, found by the devkitARM syntax check)

These are not 32-bit bugs but will be hit immediately by a 3DS build. They belong to the
platform layer (`platform/3ds/**`) or need small `#ifdef __3DS__` shims:

- `third-party/filesystem.hpp:84` (ghc::filesystem) `#error "Operating system currently not
  supported!"` — pulled in by `common/util/FileUtil.h` into most kernel/overlord/system files.
  Options: use libstdc++ `std::filesystem` if devkitARM's build supports it (untested), or teach
  ghc about `__3DS__` (newlib is POSIX-like).
- `game/system/SystemThread.cpp:14`: falls through to the Windows branch (`Windows.h`,
  `SetThreadDescription`) because only `__linux__`/`__APPLE__` are checked.
- `common/cross_sockets/XSocket.h:11` (`arpa/inet.h`): libctru provides BSD sockets after
  `socInit`; the listener/DECI2 server (`game/system/Deci2Server.cpp`, `game/sce/deci2.cpp`)
  should simply be compiled out on 3DS.
- `common/util/os.cpp:37` (`rusage::ru_maxrss`), `common/util/term_util.cpp:14`
  (`sys/ioctl.h`), `game/system/background_worker.cpp:5` (`curl`), `common/util/ast_util.h`
  (tree-sitter): exclude or stub.
- `third-party/SDL` (`SDL_endian.h` needs `endian.h` when `__linux__` is defined) and
  `third-party/fpng` (`__BYTE_ORDER`): not needed without the PC renderer/input; keep them out of
  the 3DS build.
- `game/common/vu.h` `REALLY_INLINE` fell back to MSVC `__forceinline` on unknown platforms;
  fixed (platform-owned file) to use `__attribute__((always_inline))` on non-Windows targets.

## SIMD (fixed, see below)

All x86/NEON intrinsics now have a scalar path selected by `OPENGOAL_SCALAR_SIMD`
(`common/util/simd_config.h`, automatically on for targets with neither SSE nor NEON):

- hand-written scalar code: `game/common/vu.h`, `game/mips2c/jak{1,2,3}_functions/generic_merc.cpp`,
  `common/util/crc32.h` (table CRC32C), `game/graphics/opengl_renderer/SkyBlendCPU.cpp`
- scalar SSE emulation (`common/util/simd_scalar.h`) for the rest: `common/custom_data/TFrag3Data.cpp`,
  `game/graphics/opengl_renderer/{background/background_common,foreground/Merc2,ocean/OceanMid_PS2}.cpp`.

## Suggested order for the Jak 1 / 3DS runtime

1. `s32`/`u32` as `int`/`unsigned` on `__3DS__` (1.5) and printf formats (2.1).
2. Make `EE_MAIN_MEM_SIZE` a per-platform constant (e.g. 32 MB + stack) with `BIG_MEMORY = false`
   in both C++ and GOAL, stack below the top of the smaller region, `MasterDebug = 0` (1.1-1.3).
3. Platform allocation of EE memory instead of `mmap` (1.4).
4. ARM32 context switch helper + typed `goalc_fn` adapters (1.6, runtime agent).
5. Unaligned-access review once the game boots on hardware (2.3).
