# GOAL → C backend (3DS port)

Personal fork. Goal: run Jak 1 on a 32-bit ARM11 (Nintendo 3DS), which neither native backend
(x86-64, AArch64) can target. goalc gains a C output mode; the generated C is compiled ahead of
time by the host compiler (clang on PC, devkitARM GCC for 3DS).

The shared interface is `game/kernel/common/goal_c_abi.h`. This document is the contract between
the compiler side (`goalc/`) and the runtime side (`game/`, `goal_src/jak1/kernel`).

## Ownership (while work runs in parallel)

| Area | Owner | Files |
|---|---|---|
| Compiler: C emission, object format | compiler | `goalc/**`, `common/link_types.h` |
| Runtime: module registry, linker, dispatch, kernel primitives | runtime | `game/kernel/**`, `game/runtime.cpp`, `game/mips2c/**` (call glue only) |
| GOAL kernel C branches | runtime | `goal_src/jak1/kernel/*.gc`, `goal_src/goal-lib.gc` |
| 3DS toolchain, SIMD fallbacks, platform layer | platform | `game/common/vu.h`, `common/util/simd_util.h`, other intrinsics users, `platform/3ds/**` |

Use separate CMake build directories (`build-rt`, `build-3ds`, ...) so builds do not collide.

## Selecting the backend

- goalc: `--instruction-set c`. Internally the front end behaves like ARM64 (register names,
  calling-convention constraints), but codegen emits C and register allocation is skipped.
- GOOS: `INSTRUCTION_SET` is `'c`. Kernel code that is written in assembly needs a
  `((eq? INSTRUCTION_SET 'c) ...)` branch. `asm-func` functions are a compile error in C mode.
- rlet registers in C mode: `pp`/`r13` → the `goalc_pp` global (read/write), `st`/`r14` →
  `goalc_st` (read), `off`/`r15` → host base address (read), `sp`/`rsp` → `goalc_get_sp()`
  (read only). Any other physical register is treated as an ordinary local variable.
  `.push`, `.pop`, `.ret`, `.jr` are compile errors in C mode.

## Object file

Unchanged v3 layout. Differences in C mode:

- Each function body in a segment is an 8-byte stub (after the usual `function` type tag):
  `u32 id` (placeholder `0xffffffff`, patched by the linker) and `u32 0x80000000 | func_index`
  (debug only).
- Static data, type links, symbol links and pointer links in data are exactly as before.
- There are no instruction relocations. Each segment that has functions gets one
  `LINK_C_MODULE` (8) entry in its link table:

```
u8  kind = 8
u64 hash            (little endian, unaligned)  -- FNV-1a 64 of the generated C source
u32 n
n x { u32 stub_offset_in_segment, u32 func_index }
```

## Linker duties (runtime) for a LINK_C_MODULE entry in segment s

1. Find the module by hash: first the static registry (`goalc_register_module`), then
   `out/<game>/cmod/<hash as 16 lowercase hex>.so` (dlopen, call `goalc_module_desc`). Fatal error
   if not found.
2. `mod->seg_base[i] = code_infos[i].offset` for all 3 segments (0 when absent).
3. Resolve symbols: `mod->sym_offsets[k] = intern(mod->sym_names[k]) - s7` (same meaning as the
   x86 symbol memory link: st-relative offset of the symbol).
4. Give every module function a global id (entries in `goalc_fn_table`), and write the id of
   `func_index` at each `stub_offset`.

Loading a module with the same hash twice re-links it (the latest seg_base/syms win).

## Runtime duties

- Own `goalc_mem`, `goalc_st`, `goalc_pp`, `goalc_fn_table`, `goalc_get_sp`, `goalc_break`,
  `goalc_register_module`.
- `call_goal` / `call_goal_on_stack` dispatch through `goalc_fn()` in C mode. Compiled code must
  run on a stack inside GOAL memory (stack variables are C locals whose GOAL address is
  `host address - goalc_mem`), so `call_goal_on_stack` switches the host stack pointer.
- `make_function_from_c` & friends: in C mode write a stub whose id refers to a host function
  callable as `goalc_fn8`. `arg3_is_pp` functions receive `goalc_pp` as argument 3. Stack-arg
  (varargs, e.g. format) functions receive a pointer to the 8 arguments, like the native trampoline.
- mips2c functions: callable from GOAL through stubs; mips2c code calling GOAL goes through
  `goalc_fn()`.
- Kernel context switching (replacing the assembly in `gkernel.gc` / `gstate.gc`): thread
  suspend/resume by stack copying, reset-and-call, return-from-thread(-dead), set-to-run
  bootstrap, catch/throw, gstate's "reset stack and jump to state code". Design: setjmp/longjmp
  style contexts plus a small per-architecture "call on stack" helper (AArch64 now, ARMv6/ARM32
  for the 3DS, x86-64 optional).

## Building and loading modules

- goalc writes `out/<game>/csrc/<object name>.c` (latest source per object, for static builds) and
  builds `out/<game>/cmod/<hash>.so` for dynamic loading (PC development, REPL, tests). The
  compiler for these is `cc` unless `OPENGOAL_C_BACKEND_CC` is set.
- `OPENGOAL_C_BACKEND=1` in the environment turns on C mode in any Compiler (goalc, extractor,
  goalc-test). goalc/extractor `--instruction-set c` set it.
- `OPENGOAL_C_BACKEND_LENIENT=1`: functions the backend can't compile become stubs that call
  `goalc_break()` (for finding all problems in one build).
- Static linking (3DS): compile every `csrc/*.c` with `-DGOALC_STATIC`; each exports
  `const GoalCModule* goalc_static_<name>_<hash>(void)`. `scripts/3ds/gen_c_registry.py` generates
  `void goalc_register_static_modules(void)` which registers them all; the runtime calls it at
  startup (declare it weak so builds without modules still link).

## Known later work

- ARM32: kernel functions with `u32` parameters cannot be called through `goalc_fn8` (64-bit
  arguments occupy register pairs). Needs typed adapters.
- Performance: generated code uses `uint64_t` everywhere; narrowing to 32 bits where types allow
  matters on ARM11.
