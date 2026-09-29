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
// A GOAL function value f is the GOAL address of an 8-byte stub inside a code segment:
//   u32 id     : index into goalc_fn_table, written by the linker (placeholder 0xffffffff)
//   u32 index  : index of the function in its module (debug only)
// The stub is preceded by the usual 4-byte `function` type tag, like native code.

static inline void* goalc_fn(uint64_t f) {
  uint32_t id;
  memcpy(&id, goalc_mem + (uint32_t)f, 4);
  return goalc_fn_table[id];
}

// Calling convention for compiled GOAL functions:
//   - Declared with their exact arity (0-8 params). GPR params are uint64_t, 128-bit params are
//     goalc_v128 by value. Floats travel as bit patterns in uint64_t, like native GOAL.
//   - Return uint64_t, or goalc_v128 when the function returns a 128-bit type.
//   - Callers cast goalc_fn(f) to the call-site signature. Arity mismatches are tolerated, as they
//     are harmless with caller-cleaned argument passing (AAPCS, SysV, AAPCS64).
// Functions the runtime exposes to GOAL (kernel functions, mips2c) must be callable as
//   uint64_t (*)(uint64_t a0, ..., uint64_t a7)
typedef uint64_t (*goalc_fn8)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
                              uint64_t);

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
} GoalCModule;

//! Register a statically linked module (called from module constructors when GOALC_STATIC).
void goalc_register_module(const GoalCModule* mod);

//! Name of the function exported by a dynamically loaded module (out/<game>/cmod/<hash>.so).
#define GOALC_MODULE_DESC_FUNC "goalc_module_desc"
typedef const GoalCModule* (*goalc_module_desc_fn)(void);

#ifdef __cplusplus
}
#endif
