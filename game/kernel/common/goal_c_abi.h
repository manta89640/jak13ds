#pragma once

/*!
 * @file goal_c_abi.h
 * Interface between GOAL code compiled to C by goalc (--instruction-set c) and the runtime.
 * Pure C so it can be included by both generated modules and the C++ runtime.
 * See docs/3ds-port/c_backend.md for the full contract.
 */

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

//! A 128-bit GOAL value (INT_128 / VECTOR_FLOAT register class). Lane 0 is the lowest address.
typedef union goalc_v128 {
  uint64_t du64[2];
  int64_t ds64[2];
  uint32_t du32[4];
  int32_t ds32[4];
  uint16_t du16[8];
  int16_t ds16[8];
  uint8_t du8[16];
  int8_t ds8[16];
  float f[4];
} __attribute__((aligned(16))) goalc_v128;

// ---------------------------------------------------------------------------
// Runtime state, owned and initialized by the runtime.
// ---------------------------------------------------------------------------

//! Host address of GOAL address 0 (g_ee_main_mem).
extern uint8_t* goalc_mem;
//! GOAL address of the symbol table (s7.offset). Symbol k lives at goalc_st + offset.
extern uint64_t goalc_st;
//! The GOAL process register (pp). A global because there is one EE thread.
extern uint64_t goalc_pp;
//! Function id -> host function pointer. Id 0 is reserved for "not linked".
extern void** goalc_fn_table;

//! Current host stack pointer as a GOAL address (C code always runs on a stack in GOAL memory).
uint64_t goalc_get_sp(void);
//! Implementation of (break). Never returns in practice.
void goalc_break(void);

// ---------------------------------------------------------------------------
// Function values
// ---------------------------------------------------------------------------
// A GOAL function value f is the GOAL address of an 8-byte stub inside a code segment. In object
// files the stub is
//   u32 0xffffffff               : placeholder
//   u32 0x80000000 | func_index  : index of the function in its module (debug only)
// The linker (and goalc_write_stub) replaces the start of the stub with the host address of the
// function (sizeof(void*) bytes: the whole stub on 64-bit hosts), so a call is a load and an
// indirect branch. Every host function also has an id in goalc_fn_table, used by the runtime to
// allocate and deduplicate entry points.
// The stub is preceded by the usual 4-byte `function` type tag, like native code.

static inline void* goalc_fn(uint64_t f) {
  void* p;
  memcpy(&p, goalc_mem + (uint32_t)f, sizeof(void*));
  return p;
}

// Calling convention for compiled GOAL functions (ABI version 3, GOALC_ABI_VERSION in
// goal_c_ops.h, docs/3ds-port/c_backend.md):
//   - A function with n arguments is declared
//       uint64_t f(uint32_t a0, ..., uint32_t a(n-1), float h0, ..., float h(k-1))
//     (128-bit arguments are goalc_v128 by value instead of a uint32_t). a_i is the low half of
//     argument i, h_i the bits of its upper half. On 32-bit ARM, four arguments fit in core
//     registers instead of two, and the upper halves travel in VFP registers.
//   - The callee only reads h_i for arguments whose type is "wide" (any 64-bit value: integers,
//     object, enums...). Arguments whose type is a GOAL address (structure, basic, pointer, symbol,
//     function, pair...) are rebuilt as the zero-extended low half, floats as the sign-extended low
//     half (the bits of a float in a GPR). Callers pass h_i for the wide arguments of the function
//     type they see, and anything for the others; GOAL's type checks guarantee that a function is
//     only called through function types whose arguments are the same types or parents of its own,
//     so an argument that the callee reads as wide is wide for the caller too.
//   - Return uint64_t (all 64 bits, always), or goalc_v128 for 128-bit return types.
//   - Callers cast goalc_fn(f) to the call-site signature. Arity mismatches are tolerated, as they
//     are harmless with caller-cleaned argument passing (AAPCS, SysV, AAPCS64).
// The runtime calls GOAL with all 8 arguments wide (goalc_call8). Host functions exposed to GOAL
// have the same signature (goalc_fnx); the runtime builds adapters for C++ functions.
typedef uint64_t (*goalc_fnx)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                              uint32_t, float, float, float, float, float, float, float, float);
//! the old all-uint64_t signature, used by some C++ functions exposed to GOAL (through adapters)
typedef uint64_t (*goalc_fn8)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                              uint64_t);

//! upper half of a 64-bit argument, as passed in h_i
static inline float goalc_hi_bits(uint64_t v) {
  uint32_t hi = (uint32_t)(v >> 32);
  float f;
  memcpy(&f, &hi, 4);
  return f;
}
//! a 64-bit argument from its two halves
static inline uint64_t goalc_arg64(uint32_t lo, float hi) {
  uint32_t h;
  memcpy(&h, &hi, 4);
  return (uint64_t)lo | ((uint64_t)h << 32);
}

//! Check mode (modules built with -DGOALC_ABI_CHECK, goal_c_ops.h): report an argument that
//! doesn't follow the ABI.
void goalc_abi_check_failed(const char* file, int line, const char* what, uint64_t value);

//! Call a GOAL function value (or host function exposed to GOAL) with 8 wide arguments.
static inline uint64_t goalc_call8(uint64_t f, const uint64_t* a) {
  goalc_fnx fn = (goalc_fnx)goalc_fn(f);
  return fn((uint32_t)a[0], (uint32_t)a[1], (uint32_t)a[2], (uint32_t)a[3], (uint32_t)a[4],
            (uint32_t)a[5], (uint32_t)a[6], (uint32_t)a[7], goalc_hi_bits(a[0]),
            goalc_hi_bits(a[1]), goalc_hi_bits(a[2]), goalc_hi_bits(a[3]), goalc_hi_bits(a[4]),
            goalc_hi_bits(a[5]), goalc_hi_bits(a[6]), goalc_hi_bits(a[7]));
}

// ---------------------------------------------------------------------------
// Modules
// ---------------------------------------------------------------------------
// goalc generates one module per object file. The object file carries the module hash in a
// LINK_C_MODULE link table entry (see common/link_types.h).

typedef struct GoalCModule {
  uint64_t hash;                  //! identifies the module, matches the object file
  const char* name;               //! object file name, for debugging
  uint32_t n_funcs;               //! number of functions
  void* const* funcs;             //! host function pointers, by function index
  uint32_t n_syms;                //! number of symbols referenced by the code
  const char* const* sym_names;   //! symbol names, by symbol index
  int32_t* sym_offsets;           //! filled by the linker: symbol address - goalc_st
  uint32_t* seg_base;             //! filled by the linker: GOAL address of each of the 3 segments
  //! (AI-assisted, ABI 6) called by the linker after it filled sym_offsets and seg_base (the module
  //! computes its own link-time state from them), or null
  void (*after_link)(void);
} GoalCModule;

//! Register a statically linked module (called from module constructors when GOALC_STATIC).
void goalc_register_module(const GoalCModule* mod);

//! Name of the function exported by a dynamically loaded module (out/<game>/cmod/<hash>.so).
#define GOALC_MODULE_DESC_FUNC "goalc_module_desc"
typedef const GoalCModule* (*goalc_module_desc_fn)(void);

#ifdef __cplusplus
}
#endif
