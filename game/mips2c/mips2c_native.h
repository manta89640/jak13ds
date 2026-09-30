#pragma once

/*!
 * @file mips2c_native.h
 * (AI-assisted)
 * Native C++ replacements for mips2c functions.
 *
 * A mips2c function emulates the PS2's MIPS/VU0 registers in an ExecutionContext in memory, which
 * costs about 10 ARM instructions per MIPS instruction on the 3DS. Some hot ones have a native
 * version written as plain C++ (game/mips2c/jak1_functions/native_*.cpp) that computes the same
 * results, with the float operations in the same order and grouped the same way (see "Float
 * results" below), so results are bit-identical to the mips2c code on the same machine.
 *
 * The mips2c version is kept. A function is registered with both:
 *   gLinkedFunctionTable.reg(name, execute, stack_size, &kNative);
 * and GOAL calls the native one. With OPENGOAL_MIPS2C_VERIFY=1 (C backend, not on the 3DS), every
 * call runs both on the same inputs and compares what they wrote to GOAL memory and v0
 * (mips2c_native.cpp). OPENGOAL_MIPS2C_NATIVE=0 (not on the 3DS) uses the mips2c versions.
 *
 * Float results: the mips2c helpers do each VU0 op as one C++ expression per lane, e.g.
 * vmadd: `acc + a * b`, vmadda: `acc += a * b`. A compiler may fuse a multiply-add that is inside
 * one expression (clang on arm64 does, -ffp-contract=on), and never one that spans statements.
 * Native code must group the same operations into the same expressions to get the same bits.
 */

#include <cfloat>
#include <cstring>

#include "common/common_types.h"

#include "game/mips2c/mips2c_private.h"

namespace Mips2C {

//! Arguments of a native function, as GOAL passes them (a0-a3, t0-t3) plus pp and st.
struct NativeArgs {
  const u64* a;
  u64 pp;     // s6: process pointer
  u64 st;     // s7: symbol table
  u32 stack;  // GOAL address: NativeImpl::scratch bytes below it are free for the function
};

using NativeFn = u64 (*)(const NativeArgs&);

enum NativeFlags : u32 {
  //! The function doesn't set v0 (mips2c returns whatever was in the register): don't compare it.
  NATIVE_V0_UNDEFINED = 1,
  //! The function calls GOAL code that stores to GOAL memory (callbacks): verify mode checks it by
  //! comparing all of GOAL memory, on some of the calls.
  NATIVE_CALLS_GOAL = 2,
};

struct NativeImpl {
  NativeFn fn;
  //! the same function called with a mips2c ExecutionContext (for the native GOAL backends)
  u64 (*as_exec)(void*);
  u32 flags;
  //! bytes of GOAL memory the function needs below NativeArgs::stack (for vectors it passes by
  //! address to other functions, like the mips2c version's stack frame)
  u32 scratch;
};

//! Native function F callable as a mips2c execute function.
template <NativeFn F>
u64 native_as_exec(void* ctxt) {
  auto* c = (ExecutionContext*)ctxt;
  const u64 args[8] = {c->gprs[a0].du64[0], c->gprs[a1].du64[0], c->gprs[a2].du64[0],
                       c->gprs[a3].du64[0], c->gprs[t0].du64[0], c->gprs[t1].du64[0],
                       c->gprs[t2].du64[0], c->gprs[t3].du64[0]};
  // the trampoline's fake GOAL stack (goal_stack_size bytes) is below sp
  return F(NativeArgs{args, c->gprs[s6].du64[0], c->gprs[s7].du64[0], c->gprs[sp].du32[0]});
}

#define MIPS2C_NATIVE_IMPL(fn, flags, scratch) \
  ::Mips2C::NativeImpl{&fn, &::Mips2C::native_as_exec<&fn>, flags, scratch}

//! Call a GOAL function from native code (like ExecutionContext::jalr).
u64 native_call_goal(u32 fn, const u64 args[8], const NativeArgs& caller);

//! Where the GOAL address of the stub of a registered mips2c/native function is kept (0 until it
//! is registered). The pointer stays valid; the address changes if the function is registered again.
const u32* native_stub_slot(const char* name);

// ---------------------------------------------------------------------------
// GOAL memory access
// ---------------------------------------------------------------------------

//! for reading only (see gstore)
inline const u8* gptr(u32 addr) {
  return g_ee_main_mem + addr;
}

template <typename T>
inline T gload(u32 addr) {
  T v;
  memcpy(&v, g_ee_main_mem + addr, sizeof(T));
  return v;
}

// Native code must store to GOAL memory only with these: verify mode logs the stores.
#if MIPS2C_WRITE_LOG
extern bool g_native_log_on;
void native_log_record(u32 addr, u32 size);
#define MIPS2C_NATIVE_LOG_STORE(addr, size) \
  do {                                      \
    if (g_native_log_on) {                  \
      native_log_record((addr), (size));    \
    }                                       \
  } while (0)
#else
#define MIPS2C_NATIVE_LOG_STORE(addr, size) \
  do {                                      \
  } while (0)
#endif

template <typename T>
inline void gstore(u32 addr, T v) {
  MIPS2C_NATIVE_LOG_STORE(addr, (u32)sizeof(T));
  memcpy(g_ee_main_mem + addr, &v, sizeof(T));
}

inline void gstore_bytes(u32 addr, const void* src, u32 size) {
  MIPS2C_NATIVE_LOG_STORE(addr, size);
  memcpy(g_ee_main_mem + addr, src, size);
}

//! a quadword as 4 floats (a VU0 register)
struct Vec4f {
  float x, y, z, w;
};
static_assert(sizeof(Vec4f) == 16);

inline Vec4f gload_vec(u32 addr) {
  return gload<Vec4f>(addr);
}
inline void gstore_vec(u32 addr, const Vec4f& v) {
  gstore<Vec4f>(addr, v);
}

inline u32 f2u(float f) {
  u32 u;
  memcpy(&u, &f, 4);
  return u;
}
inline float u2f(u32 u) {
  float f;
  memcpy(&f, &u, 4);
  return f;
}
//! float bits sign-extended to 64 bits, like mfc1
inline u64 f2gpr(float f) {
  return (u64)(s64)(s32)f2u(f);
}

//! div.s as ExecutionContext::divs_accurate does it
inline float divs_accurate(float a, float b) {
  if (b == 0) {
    return a < 0 ? -FLT_MAX : FLT_MAX;
  }
  return a / b;
}

}  // namespace Mips2C
