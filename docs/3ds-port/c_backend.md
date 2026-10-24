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
  `u32 0xffffffff` (placeholder) and `u32 0x80000000 | func_index` (debug only). The linker
  replaces the start of the stub with the host address of the C function (`sizeof(void*)`
  bytes, so the whole stub on 64-bit hosts): a call is one load and an indirect branch.
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
4. Give every module function a global id (entries in `goalc_fn_table`), and write the host
   address of `func_index` at each `stub_offset`.

Loading a module with the same hash twice re-links it (the latest seg_base/syms win).

Known hazard (not fixed): the module keeps one `seg_base` per segment, so if two loaded levels
contain the same object file (same code, so the same hash), linking the second copy points the
first copy's functions at the second copy's static data (constants, static structures, strings).
That is harmless while both are loaded and the data is read-only, but the first level then
modifies the other copy's static data, and reads freed memory after the second level unloads.
A fix would give each linked copy its own segment bases (for example, one module instance per
link, with the bases passed through the stub).

(AI-assisted, Oct 2026) Not reachable in Jak 1: no level DGO shares an object with GAME.CGO, and
the objects that several level DGOs share (racer/target-racer: fic lav mis ogr rol; flutflut: sno
swa; oracle/villagep-obs: vi1 vi2 vi3 cit; battlecontroller: mis swa cit; blocking-plane,
launcherdoor, snow-bunny, firecanyon-obs, babak-with-cannon, mistycannon, static-screen) are only
in levels that are never neighbours (two-level load slots). Not checked: whether a warp-gate
transition between villages (vi1/vi2/vi3 share oracle and villagep-obs) can keep the old village
loaded while the new one links.

## Runtime duties

- Own `goalc_mem`, `goalc_st`, `goalc_pp`, `goalc_fn_table`, `goalc_get_sp`, `goalc_break`,
  `goalc_register_module`.
- `call_goal` / `call_goal_on_stack` dispatch through `goalc_fn()` in C mode. Compiled code must
  run on a stack inside GOAL memory (stack variables are C locals whose GOAL address is
  `host address - goalc_mem`), so `call_goal_on_stack` switches the host stack pointer.
- `make_function_from_c` & friends: in C mode write a stub (`goalc_write_stub`: the host address
  of the entry point of that id) for a host function callable as `goalc_fn8`. `arg3_is_pp` functions receive `goalc_pp` as argument 3. Stack-arg
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
| `set-to-run`, `deactivate` | also `__goalc-thread-release` (frees a suspended thread's saved state) |
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
  stack. The context and a copy of [sp, stack-top) go to host memory (a `SuspendedThread` slot),
  not to the backup stack in the process heap: pc becomes `0xc0de5000`, rreg 0 holds
  `(token << 32) | slot`, sp is the caller's sp. Resume copies the stack back to the same
  addresses, sets top-thread/status/pp, frees the slot and restores the context. C frames
  survive because they only point into their own stack or GOAL memory.
  - Why host memory: compiled C frames are bigger than native ones by an amount that depends on
    the host and the code. Measured with `-fstack-usage` over all of Jak 1: the median frame is
    the same size, but vector heavy functions are up to ~3.5x bigger on AArch64 and ~12x on ARM32
    (camera-combiner suspends with 1704 bytes where the game reserves 512). Scaling the backup
    stacks to cover that made small process heaps overflow on the 3DS. Process heaps are now laid
    out exactly like in the native version (the backup stack is allocated but unused).
  - Slots are freed when the thread is resumed, and by `__goalc-thread-release`, which
    `set-to-run` and `deactivate` (for the main thread) call in C mode, so a thread that is thrown
    away while suspended doesn't leak. The token guards against stale handles.
  - The only limit is 64 kB of suspended stack.
- A thread whose pc is anything else (after `set-to-run`) is started by calling pc on a fresh
  stack at thread sp (the stack top).
- catch: the context lives in the C frame of `__goalc-catch` (on the GOAL stack), the catch
  frame's `ra` field holds its GOAL address and `sp` the stack pointer. throw pops frames through
  the catch frame (stack-frame-top = next) and restores the context; pp is not restored, like the
  native version.

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

(Before ABI version 3, see below.) GOAL calls every function as `u64 f(u64 x8)`. On ARM32, a C function with `u32`/`s32`
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

### Argument passing (ABI version 3)

(AI-assisted) With `u64` arguments, ARM32 passed only two arguments in registers (3-argument calls
are 39% of the call sites). Now every compiled GOAL function is

```c
u64 f(u32 a0, ..., u32 a(n-1), float h0, ..., float h(k-1))   // 128-bit args stay v128
```

`a_i` is the low half of argument i (4 in core registers on ARM32), `h_i` the bits of its upper
half (VFP registers). Each argument has a class from its type (`c_arg_class`, CEmitter.cpp):

- PTR: subtypes of `structure`, `pointer`, `pair` (basic, symbol, type, function, string,
  process...). A GOAL address: the callee uses the zero-extended low half.
- FLOAT: subtypes of `float` (meters, degrees). The callee sign-extends the low half (what a float
  moved to a GPR looks like).
- WIDE: everything else (int, uint, object, binteger, enums, bitfields, time-frame, `_type_`,
  varargs). The callee reads `lo | hi << 32`.

Callers pass `h_i` for the WIDE arguments of the function type they see (`IR_FunctionCall` keeps
the caller's view of the argument types) and an unspecified value (`gc_anyf`, no instructions) for
narrow ones before the last WIDE one. Callees read `h_i` only for their WIDE parameters. Returns
are unchanged (always the full `u64`).

Why it's safe: GOAL type checks let a function value be called only through a function type whose
argument types are the same as, or parents of, its own (`typecheck_and_throw` is covariant in the
arguments). Every child of a PTR/FLOAT type is PTR/FLOAT, so a parameter the callee reads as WIDE
is WIDE for every type-checked caller. The holes are:

- casts of function values (`the-as`/`the`): goalc warns in C mode when a cast between known
  function types makes a WIDE argument narrow (`check_c_abi_function_cast`); the three cases in
  Jak 1 were fixed in goal_src. Casts from untyped values (`function`, fields typed `basic`) can't
  be checked statically: use the check mode below.
- `go` types `enter-state` with the state's argument types: the call uses enter-state's own type
  for argument passing (`compile_go_hook`).
- a method implemented on a WIDE type and inherited by reference types (Jak 1: `relocate` and
  `mem-usage` of `object`): `this` (`_type_`) is read as PTR there.
- assumption: values of PTR types are GOAL addresses (< 4 GB, hi = 0), and float-typed GPR values
  are sign-extended. A PTR- or FLOAT-typed value that holds something else (a 64-bit value cast to
  the type to get past the type checker) loses its upper half when passed. Returns are always the
  full `u64`, so only arguments are affected. goalc warns in C mode (`check_c_abi_narrowing_cast`)
  about:
  1. a value of a named 64-bit integer type (handle, time-frame, seconds, binteger, 64-bit enums and
     bitfields), directly or through other casts (`(the-as foo (the-as uint64 h))`), cast to a PTR
     or FLOAT type, anywhere (argument, variable, return value);
  2. a PTR- or FLOAT-typed parameter cast to a named 64-bit integer type: the callee half of the same
     mistake, for callers that need no cast (`process-spawn`, `go` and other untyped paths).

  Jak 1 had two cases, both halves each, fixed in goal_src by typing the parameter `handle`:
  `add-rider!` (the rider's handle was passed as a `process-drawable`, its pid was lost,
  `handle->process` failed in `pull-riders!`, and no platform carried Jak: the Volcanic Crater
  minecarts, every `plat`, ...) and the `target-final-door` state (a handle passed as a `basic`).

  The plain integer types (int, uint, int64, uint64, integer, sinteger, uinteger) are not checked:
  GOAL code keeps addresses, float bits and untyped 64-bit slots in them. An audit of every such
  cast in Jak 1 (6,420 reports, Oct 2026: 3,535 casts, 2,694 of them direct call arguments, and 191
  parameter casts) found only 32-bit values:
  - constants (2,591): null (2,018), `#x70004000` (457), float bit patterns, small integers; the
    one negative constant is stored into a float field (`puffer`), the same on every backend;
  - address arithmetic and float bit tricks (~590): `(+ ... (scratchpad-object int))`, `&-`,
    `align64`, `(logior #x3f800000 ...)` random floats, `get-vm-ptr`;
  - event-message-block params (248, plus ~20 variables holding them): the sender's pointers,
    symbols and floats (a handle in a param is read as `handle`, which is WIDE);
  - other variables and fields (~115): decompiler temporaries holding pointers, connection params,
    sparticle userdata (floats, process pointers), a thread register (a function), floats as ints
    (debug menu; a FLOAT argument keeps any signed 32-bit integer), `gnawer-method-28` returning a
    bit as a symbol (returns are 64 bits), unused state arguments (`double-lurker`), and the never
    written `launch` fields of target's `control-info` (dead code);
  - parameter casts: pointer parameters cast to int/uint for address arithmetic, and a texture page
    pointer put in a 64-bit DMA slot.

  Not checked: values of type `object` (every downcast would be reported).

Runtime side (`goal_c_abi.h`, `goalc_runtime.cpp`, `kernel_function.h`):

- The runtime calls GOAL with `goalc_call8` (all 8 arguments WIDE).
- Host functions callable directly: C functions with only `u32`/`s32` parameters and a 64-bit
  return (`KernelFunction::direct`), `nothing`/`zero-func`, `goalc_suspend_entry`, and the
  native mips2c `goal` entries when no argument is WIDE. Everything else goes through an adapter
  thunk with a wide mask: bit i = rebuild argument i from both halves, otherwise zero-extend.
  For kernel functions the mask comes from the C signature (`u64`/`s64` parameters);
  `CAbiKernelFunctionTypes` (test/goalc/test_goal_kernel.cpp, C mode) checks every named kernel
  function against its GOAL type (a C `u64` parameter must be WIDE in GOAL; it found 11 that
  weren't, now `u32`). Stack-argument functions take an explicit mask
  (`make_stack_arg_function_symbol_from_c`).
- mips2c functions get their mask from GOAL: `(__pc-get-mips2c name (__c-abi-wide-mask type))`
  (`def-mips2c`; `__c-abi-method-wide-mask` for `defmethod-mips2c`). Until then the stub reads all
  arguments as WIDE (right for calls from C++/mips2c).

Check mode (PC host only): build the modules with `-DGOALC_ABI_CHECK`
(`OPENGOAL_C_BACKEND_CC="cc -DGOALC_ABI_CHECK"` and a fresh `out/<game>/cmod`) and run gk with
`OPENGOAL_C_ABI_CHECK=1`. Narrow arguments pass a marker instead of an unspecified upper half, and
`[goalc abi check]` warnings report WIDE parameters that got the marker (also from the thunks),
pointer arguments with an upper half and floats that aren't sign-extended. Expected reports: WIDE
parameters beyond the caller's argument count (`go` with fewer than 6 arguments,
`run-function-in-process`, `set-to-run`), which are garbage like in the native backends.

Measured (Jak 1, 518 modules, devkitARM -O2, instructions in the module objects): 1,490,480 ->
1,377,293 (-7.6%; -7.7% without the inspect methods), `strd` to the stack 7,681 -> 713, `str` to
the stack 93,372 -> 34,948.

`OPENGOAL_DECI2_PORT` overrides the listener port of gk/goalc/goalc-test, to run tests while
another gk holds the default port.

### Guarded direct calls and float compares (ABI version 4)

(AI-assisted) The ARM11 never predicts a branch to a register (`blx reg`: 5-7 cycles), and every
GOAL call was one. The calling convention is the same as version 3; version 4 changes the code
shape (and needs the matching `goal_c_ops.h`):

- A call of the value of a global symbol is `GC_GCALL(ret, GC_Tk, f, type, (args))`: if the host
  function in f's stub is the expected target, it is called directly (`bl`), otherwise through the
  pointer as before. Both calls use the call site's signature, so the result is the same whichever
  branch runs: a stale or wrong expectation only costs time, and symbols can still be redefined at
  any time. The fallback stays out of line.
- Targets (`goal_c_ops.h`, `CModuleEmitter::finish`): a module that stores one of its functions in
  a symbol (`define`/`defun`, found by following the registers in `CFunctionEmitter`) exports the
  weak alias `gc_d_<symbol>_<hash>`; callers reference it weakly, so it is 0 (never taken) when
  nothing defines it. Only static GCC builds (`GC_WEAK_TARGETS`, the 3DS) use aliases. Calls inside
  a module go through the alias too: a weak alias can be replaced at link time, so GCC never
  inlines it or applies IPA results that assume the callee's own parameter list (inlining a call
  whose signature differs would map the float halves positionally, which isn't the ABI). With
  clang (PC `.so` modules) calls inside a module use the static function (LLVM doesn't inline or
  analyze calls whose type differs from the function's), other calls are never taken (`gc_nofn`).
- Not guarded: functions of the C++ runtime (symbols declared in `kernel-defs.gc`, e.g.
  `_format`, `method-set!`), symbols last defined to something that isn't a lambda (`(define
  format _format)`, mips2c functions), and method calls (the target depends on the object's type).
- Float constants are C constants (`gc_bitsf(0x...)`) instead of loads from static data, and float
  compares are `gc_fcmp(a, b, op)`: when one side is a constant (zero or a normal number), the
  compare is done on the bits in integer registers (signed order for positive constants, unsigned
  reversed order for negative ones, exponent tests for zero). VCMP + VMRS waits until every VFP
  instruction in flight has finished; a float loaded from memory now goes straight to a core
  register. Exact for every non-NaN value with flush-to-zero semantics (denormals compare like 0,
  as on the PS2 and in RunFast mode).
- `gc_cvtt` (float to int) uses `vcvt.s32.f32` on ARM, which already saturates and gives 0 for NaN,
  instead of three compares.

Measured (Jak 1, 518 modules, devkitARM -O2, module objects): instructions 1,377,731 -> 1,417,153
(+2.9%; the float changes alone are -6.9%, the guards add ~+10% on top, mostly the out-of-line
fallbacks), `vcmp`/`vmrs` 11,682/11,722 -> 1,780/1,782, direct `bl` 103 -> 22,367 (23,340 guarded
sites; the static `blx reg` count stays ~55k because each guarded site keeps its fallback). The
hot path of a guarded call is `cmp` + a predicted `bne` + `bl` (the expected address is a literal,
often hoisted out of loops). Not measured on hardware yet.

### NaN compares like x86, stack fill hook (ABI version 5)

(AI-assisted, Oct 2026 audit of semantic differences between the x86 backend and C.)

- Float compares with a NaN on either side give what the x86 backend gives: `<`, `<=` and `=` are
  true, `>`, `>=` and `!=` are false. x86 compiles a float compare as `comiss` + an unsigned jump,
  and an unordered `comiss` sets ZF = PF = CF = 1; every condition and its inversion agree, so the
  result doesn't depend on how the compiler arranged the branch. C compares are IEEE (all false
  except `!=`), and the integer form for constants gave sign-dependent results. GOAL code depends on
  the x86 behaviour, on purpose (`is-nan?` was `(and (< 0.0 x) (< x 0.0))`, true only on x86: the
  Snowy Mountain heap corruption) or by accident (any `(if (< x limit) ...)` that a NaN reaches
  takes the same branch as on the PC port). `gc_fcmp` implements it: general compares are written
  so that GCC folds the unordered case into the condition (`!(a >= b)` is `lt` after `vcmp` on ARM),
  compares with a constant test one range of the other side's bits, plus a NaN test where x86
  includes the NaNs in a set of the other sign (`(< x 0.0)`: two compares). Exhaustively tested
  against a model of the x86 rules (2 x 10^8 cases, both operand orders, special values). Cost:
  +11,975 ARM instructions over the 518 modules (+0.8%).
- Float to int conversion keeps the PS2/ARM64 behaviour (saturating, NaN gives 0), not x86's
  `0x80000000` for every out of range value: no Jak 1 code depends on the x86 value, and a NaN or
  infinite index then stays in range more often.
- `GC_STACK_INIT(gc_stack)` at the start of every function with stack variables: nothing by
  default. Build the modules with `-DGOALC_STACK_POISON` (fill with `0xff`: NaN floats, -1
  integers, faulting pointers) to find code that reads `stack-no-clear` memory before writing it
  (natively it holds whatever the stack held, which differs between x86 and C, see the Misty lurker
  normal), or `-DGOALC_STACK_ZERO`. Host: `OPENGOAL_C_BACKEND_CC="cc -DGOALC_STACK_POISON"` and an
  empty `out/jak1/cmod`.
- Process stacks on the fake scratchpad: most processes (and their trans hooks) run on the
  "scratchpad stack", which the PC port starts at `*fake-scratchpad-stack*` + 32 kB, growing down
  towards the scratchpad data at + 0..16 kB; the temporary cpu-thread of a trans hook is stored at
  stack-top - 24 kB, inside that data. In C mode the stack top is + 64 kB instead (`activate`,
  gkernel.gc): the 128 kB allocation aligned to 64 kB always contains it and nothing else uses
  + 32..64 kB (checked on the host: untouched over 300,000 process runs and level loads), so
  stacks and temporary threads are 16..32 kB clear of the scratchpad data, at no memory cost.
  Why: C frames on ARM32 are bigger than native ones (collide-cache functions up to 1.4 kB, 4x
  the host's), and on PC hosts the module `.so` files are `dlopen`ed while a level links, inside
  the display process: dyld then uses ~20 kB of the GOAL stack (27 kB deep in total), which ran
  into the scratchpad data with the native layout. Measured on the host in gameplay (level
  sweeps): game processes use up to ~5 kB of stack, display ~3.5 kB, the dram stack ~5.4 kB
  (sidekick, target).
- Checked and identical to x86: integer division and modulo (division by zero gives 0 instead of
  a trap), shift counts (masked to 6 bits), 32-bit multiply sign extension, 128-bit lane ops,
  register class moves, `minss`/`maxss` NaN operand order. Memory: 64-bit accesses compile to
  unaligned-safe `ldr` pairs; 128-bit accesses are 16-byte aligned on x86 (`movaps`), so VLDR's
  word alignment holds.

### Module context and symbol value pointers (ABI version 6)

(AI-assisted, Oct 24 2026, branch j2codegen) The calling convention is the same as version 5;
version 6 changes the module's link-time state:

- Each module keeps it in one struct, `gc_ctx` (`CModuleEmitter::finish`): copies of `goalc_mem`
  and `goalc_st`, the host address of each referenced symbol's value (`sym[k]`), the segment bases
  and the symbol offsets (`gc_sym_offsets` / `gc_seg_base` are macros for its fields, and the
  runtime's linker fills them as before). A function reaches all of it from one literal address
  (before: `goalc_mem`, `goalc_st`, `gc_sym_offsets` and `gc_seg_base` were four).
- Loads and stores of symbol values are `gc_symv_u32(k)` / `gc_symv_s32(k)` / `gc_symv_set(k, v)`:
  a load of `sym[k]` and the access. Before, a symbol value was `goalc_mem + st + offset[k]`
  (Jak 2: `- 1`), an add or two more per access and two registers kept live for the bases.
- `GoalCModule.after_link` (new, last field): the runtime calls it after filling `sym_offsets` and
  `seg_base`; the module's `gc_after_link` copies the bases and computes `sym[k]` (Jak 2 and
  later: the symbol's address - 1, Symbol4.h). Symbols never move, and every relink (module
  instances, heap init) calls it again.
- The mips2c-native test fixtures (`test/mips2c_native/goalc_ref`) are ABI 5 code; they compile
  with the version 6 header (the old accessors are still there), `extract.py` handles both shapes.

### 32-bit locals (AI-assisted, Oct 24 2026, j2codegen)

GPR variables whose every write stores a value below 2^32 are declared `u32` instead of `u64`
(`CFunctionEmitter::narrow_vars`). No type assumption: each `do_codegen_c` says what its write
stores (`write_is_32`: zero-extending loads of 1-4 bytes, symbol values, symbol / static / stack /
function addresses, constants below 2^32, address-typed parameters; `write_is_32_if_all`: moves,
`or`, `xor`, right shifts; `write_is_32_if_any`: `and`), every other write may need 64 bits
(call results, sign-extending loads, arithmetic, float bits...). The greatest fixed point over a
function is exact by induction: every variable starts at 0 and every write keeps it below 2^32.
Variables whose address is taken, `volatile` stack variables, the special registers and mixed-class
variables stay `u64`. Reads promote the value back to `u64`, so expressions are unchanged; GCC
then drops the upper halves through phis and loops (64-bit compares of loop variables, the upper
half of WIDE arguments: `gc_hi` of a `u32` is 0).

## Known later work

- Performance: function bodies still use `u64` for every GPR. Declaring locals whose types are all
  PTR as `u32` measured another -2.5% (prototype), at the price of extending the "PTR values are
  addresses" assumption from calls to every variable (with constants >= 2^32 excluded). Format
  calls (debug code) pass every argument WIDE.

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

### Code shape (ABI version 2)

Measured on the whole of Jak 1 compiled for the 3DS (devkitARM, `-O2`, instructions in the
module objects): 1.904M -> 1.489M instructions (-22%), loads -32%, 64-bit carry instructions
(`adc`/`movcs`) 36k -> 11k.

- Memory accesses are `gc_ld_T(base, offset)` / `gc_st_T(base, offset, value)`, like the native
  backends' base + constant offset addressing. On 32-bit hosts the host address is
  `gc_mb + (u32)base + offset` (host pointer arithmetic wraps at 4 GB like the GOAL address), so
  the offset folds into the `ldr`/`vldr` and `gc_mb + base` is shared by the fields of an object.
  Truncating `base + offset` first cost an extra instruction per access.
- Calls read the host function from the stub (`gc_fn`, with the local memory base): a load and a
  `blx`, instead of reloading `goalc_mem` and `goalc_fn_table` after every call plus the table
  lookup.
- `gc_stl`, `GC_SYM`, `#t`/`_empty_` and static data addresses are zero-extended 32-bit values,
  so symbol addresses and `#f` compares are 32-bit operations.
- The process register lives in the local `gc_pp` when a function uses it. Writes also update
  `goalc_pp` right away (callees and the runtime read the global), and `gc_pp` is read again after
  every call, since the callee can change it (a suspended process can be moved to another heap
  location before it resumes). Unlike the global, the local isn't reloaded after every memory
  store (stores through GOAL pointers may alias any global).
- The function's own segment base is read once (`gc_sb`).
- `GOALC_ABI_VERSION` in `goal_c_ops.h` is checked by a `_Static_assert` in every module and is
  part of the module source (and hash): a module and a header from different versions don't
  compile together, and cached `cmod/*.so` files of another version are not reused.
