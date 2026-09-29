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

## Runtime implementation (Jak 1)

Files: `game/kernel/common/goalc_runtime.{h,cpp}` (state, function ids, modules, calls),
`game/kernel/common/goalc_context.cpp` (per-architecture assembly),
`game/kernel/jak1/goalc_kernel.{h,cpp}` (kernel context primitives),
`game/mips2c/mips2c_goalc.cpp` (calling mips2c from GOAL), tests in `test/test_goalc_runtime.cpp`.

### Selecting C mode

The runtime must know before the symbol table is built, because every C function exported to GOAL
is a stub whose format depends on the mode (a C stub can't also be machine code). So C mode is
chosen at startup, not detected from the first `LINK_C_MODULE`:

- `OPENGOAL_C_BACKEND=1` in the environment (same variable as the compiler, so
  `OPENGOAL_C_BACKEND=1 goalc-test` runs everything through the C backend), or
- the `-cbackend` kernel argument (`gk -- -boot -fakeiso -cbackend`), or
- always, on hosts without a native backend (neither x86-64 nor AArch64: the 3DS).

A `LINK_C_MODULE` entry when the runtime isn't in C mode is a fatal error that says so. Only Jak 1
is supported (Jak 2/3/X kernels don't check the flag).

### Function ids

`goalc_fn_table` grows by doubling; old arrays are never freed, so a thread reading it while it
grows (the renderer's VIF interrupt callback) stays safe. Id 0 is a handler that logs and breaks.
Kernel functions that are callable as `goalc_fn8` are put in the table directly (deduplicated).
Functions that need an adapter (`arg3_is_pp`, stack-argument functions, mips2c functions) use a
fixed pool of 1024 template thunks; each thunk gathers the 8 arguments in an array and calls
`adapter(fn, extra, args)`. Modules get a contiguous range of ids on their first link since the
last heap init; relinking a module with the same hash reuses them. `goalc_register_static_modules`
is called (weak, empty default) every time the GOAL heap is initialized.

`nothing` returns its first argument (like the native `ret` on AArch64), `zero-func` returns 0.

### Calling GOAL from C

- `call_goal`: sets `goalc_pp` to st (#f) for the call and restores it after, like the native
  trampolines. If the host stack is not in GOAL memory (plain C, for example `play` during boot),
  it switches to the kernel stack at the top of GOAL memory.
- Calls from other host threads (the renderer's fake VIF interrupts) run on a separate 32 kB
  stack allocated from the global heap, under a mutex, and don't touch `goalc_pp`.
- `call_goal_on_stack`: switches stacks with `goalc_call_on_stack`.
- mips2c `jalr` calls `goalc_call_goal8` with pp from s6. GOAL calling mips2c goes through an
  adapter that builds the MIPS register context and the fake GOAL stack on the (GOAL) stack.

### Kernel context switching

Primitives are C++ functions registered as GOAL functions at heap init (C mode only). The
`'c` branches in `gkernel.gc` / `gstate.gc` are ordinary GOAL functions calling them.

| GOAL | C mode |
|---|---|
| `reset-and-call` | sets pp/status/top-thread, then `__goalc-reset-and-call` |
| `return-from-thread`, `deactivate`, `abandon-thread`, gstate "abandon" | `__goalc-return-to-kernel` |
| `return-from-thread-dead` | `(deactivate pp)` then `__goalc-return-to-kernel` |
| `thread-suspend` (cpu-thread) | `__goalc-thread-suspend` (thread in pp) |
| `thread-resume` (cpu-thread) | `__goalc-thread-resume` |
| `set-to-run-bootstrap` | calls rreg 0 with rreg 1-6, then `return-from-thread-dead` (the runtime does this directly when a thread's pc is `set-to-run-bootstrap`, to save a frame) |
| `enter-state` "reset stack and jump to code" | `__goalc-reset-stack-and-call` |
| `new catch-frame` | `__goalc-catch` |
| `throw-dispatch` | `__goalc-throw` |

Design:

- Contexts (`goalc_ctx`) hold callee-saved registers, sp and the return address.
  `goalc_ctx_save`/`goalc_ctx_restore` are setjmp/longjmp without signal masks.
  `goalc_call_on_stack(top, fn, arg)` calls a function on another stack. Assembly for AArch64,
  ARM32 (ARMv6, ARM mode, interworking, VFP d8-d15 when there's a hardware FPU) and x86-64 SysV.
- Kernel side: `thread-resume` and `reset-and-call` save a kernel context in their own C frame on
  the kernel stack and push it on a small stack of kernel contexts (nesting works). Returning to
  the kernel restores the innermost one with a value. `*kernel-sp*` is still set.
- Suspend: `__goalc-thread-suspend` is the assembly `goalc_suspend_entry`, which saves the
  context of its caller (as if saved at the call site), so only GOAL frames end up in the saved
  stack. The context is stored in the thread object, right after the backup stack
  (`GOALC_THREAD_CTX_SIZE` = 192 extra bytes for main threads in C mode: `new cpu-thread`,
  `stack-size-set!` and `asize-of` account for it, so relocation moves it with the process). pc
  becomes `0xc0de5000`, sp the caller's sp, then [sp, stack-top) is copied to the backup stack and
  the kernel context is restored. Resume copies the stack back to the same addresses, sets
  top-thread/status/pp and restores the thread context. C frames survive because they only point
  into their own stack or GOAL memory.
- A thread whose pc is anything else (after `set-to-run`) is started by calling pc on a fresh
  stack at thread sp (the stack top).
- catch: the context lives in the C frame of `__goalc-catch` (on the GOAL stack), the catch
  frame's `ra` field holds its GOAL address and `sp` the stack pointer. throw pops frames through
  the catch frame (stack-frame-top = next) and restores the context; pp is not restored, like the
  native version.
- Backup stack sizes: compiled C frames are about twice as big as native ones on AArch64, so in C
  mode `PROCESS_STACK_SAVE_SIZE` is 512 and `stack-size-set!` doubles the requested size, with a
  floor of 512 (`GOALC_STACK_SCALE` in gkernel-h.gc). An overflow prints the process name and a
  backtrace, then breaks.

### Testing

- `./build-rt/goalc-test --gtest_filter='Goalc*'`: unit tests for the context primitives (several
  threads suspending on one shared execution stack, reset-and-call, reset-stack-and-call,
  catch/throw), function ids and module linking.
- `OPENGOAL_C_BACKEND=1 ./build-rt/goalc-test --gtest_filter='-Jak2*:Jak3*:JakX*:Jak1NoDebugSegment*'`:
  the Jak 1 compiler/runtime tests through the C backend, including the kernel tests (suspend,
  states, throw, vector/float preservation across suspend).
- Full game: build Jak 1 with `goalc --instruction-set c` into a separate project dir (symlink
  everything but `out/`), then `gk --proj-path <dir> --no-display -- -debug -fakeiso -cbackend`
  and `(mi) (lt) (lg) (test-play)` from the REPL. Tested: title screen, village1, Jak in
  target-stance, dying and respawning.

### Typed kernel functions (ARM32)

GOAL calls every function as `u64 f(u64 x8)`. On ARM32, a C function with `u32`/`s32`
parameters, `float` parameters (hard-float: in VFP registers) or a 32-bit return can't be called
that way. `game/kernel/common/kernel_function.h`: every registration site
(`make_function_from_c`, `make_function_symbol_from_c`, `make_stack_arg_function_symbol_from_c`,
the common `init_common_pc_port_functions`) takes a `KernelFunction`, implicitly built from the
function pointer, which keeps the signature. In C mode, functions whose parameters and return are
not all 64-bit integers get a generated adapter (called through the thunk pool):

- integer parameters are truncated to their type, pointers get the raw value (like native),
  `float` parameters are the low 32 bits of the argument (GOAL passes floats as bit patterns);
- returns: narrower integers are zero-extended (like a 32-bit return in eax/w0), floats become
  their bits, void is 0;
- `arg3_is_pp` replaces argument 3 with `goalc_pp`; stack-argument functions get a pointer to the
  8 arguments and their return is converted the same way.

The same adapters run on the Mac in C mode, so the C-mode test suite covers them. Unit test:
`GoalcRuntime.TypedAdapters`.

## Known later work

- Performance: generated code uses `uint64_t` everywhere; narrowing to 32 bits where types allow
  matters on ARM11.

## Performance notes (ARM11)

- Stack frames are much bigger on ARM32 than on AArch64 (camera-combiner's state code: 1632 vs 528
  bytes, measured with `-fstack-usage`). Cause: `goalc_v128` is a union, which GCC keeps in memory,
  so every 128-bit temporary gets its own 16-byte stack slot; ARM32 also has fewer registers for
  64-bit values. This drives the suspended-stack sizes in C mode.
- Planned: represent `v128` in generated code as a struct of four floats (scalarized into VFP
  registers by GCC), with integer lane views through bit casts, keeping the `goalc_v128` layout
  and by-value ABI. Expected to shrink frames and speed up vector-heavy code. Do this after the
  game boots on the 3DS, and measure.
- Also planned: 32-bit arithmetic where the GOAL type guarantees the upper half doesn't matter
  (pointers, structures, 32-bit integers), since every `u64` operation costs 2+ instructions on
  ARM11.
