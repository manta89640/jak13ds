#pragma once

/*!
 * @file native_goalc_ops.h
 * (AI-assisted)
 * goalc's C semantics for native code that must give the same bits as GOAL code compiled by the C
 * backend (goalc/cbackend/goal_c_ops.h, GOALC_ABI_VERSION 5): float compares with a constant, the
 * float/int conversions, and native versions of small GOAL math functions that natives call often
 * (atan, vector-length, vector-normalize!), written as goalc's C of the GOAL code: the same float
 * operations in the same order, each on its own (no multiply-adds: fp contract off for clang; the
 * 3DS's VFPv2 has no fused multiply-add), the PS2 divide-by-zero result where the GOAL code has it.
 * Shared by the games: trigonometry.gc and vector.gc have the same code in Jak 1, 2 and 3.
 */

#include <cstdint>
#include <cstring>

#include "common/common_types.h"

//! first statement of a function whose float operations must not be fused (clang on arm64 fuses
//! a + b * c within an expression by default); scoped to the function, so a file that includes
//! this keeps its own setting
#if defined(__clang__)
#define GC_NATIVE_FP_EXACT _Pragma("clang fp contract(off)")
#else
#define GC_NATIVE_FP_EXACT
#endif

namespace Mips2C::goalc_ops {

inline u32 fbits(float f) {
  u32 u;
  memcpy(&u, &f, 4);
  return u;
}
inline float bitsf(u32 u) {
  float f;
  memcpy(&f, &u, 4);
  return f;
}

// ---------------------------------------------------------------------------
// goalc's float compares with a constant (goalc/cbackend/goal_c_ops.h, GOALC_ABI_VERSION 5):
// on the bits, denormals compare like 0, NaN gives the x86 backend's result.
// ---------------------------------------------------------------------------
enum { GC_FLT, GC_FGT, GC_FLE, GC_FGE, GC_FEQ, GC_FNE };
inline int gc_fnanres(int op) {
  return op == GC_FLT || op == GC_FLE || op == GC_FEQ;
}
inline int gc_bnan(u32 b) {
  return (b << 1) > 0xff000000u;
}
inline int gc_bin(u32 x, u32 lo, u32 hi) {
  return x - lo <= hi - lo;
}
inline int gc_fcmp_k(u32 xb, u32 cb, int op, int nr) {
  int in_s, s_is_true;
  if ((cb << 1) == 0) {
    switch (op) {
      case GC_FGT:
      case GC_FLE:
        in_s = gc_bin(xb, 0x00800000u, 0x7f800000u);
        s_is_true = op == GC_FGT;
        break;
      case GC_FLT:
      case GC_FGE:
        in_s = gc_bin(xb, 0x80800000u, 0xff800000u);
        s_is_true = op == GC_FLT;
        break;
      default:
        if ((op == GC_FEQ) == nr) {
          const int eq_or_nan = (xb << 1) - 0xff000001u <= 0x01fffffeu;
          return op == GC_FEQ ? eq_or_nan : !eq_or_nan;
        }
        in_s = (xb << 1) < 0x01000000u;
        s_is_true = op == GC_FEQ;
        break;
    }
  } else if ((s32)cb > 0) {
    switch (op) {
      case GC_FGE:
      case GC_FLT:
        in_s = gc_bin(xb, cb, 0x7f800000u);
        s_is_true = op == GC_FGE;
        break;
      case GC_FGT:
      case GC_FLE:
        in_s = gc_bin(xb, cb + 1, 0x7f800000u);
        s_is_true = op == GC_FGT;
        break;
      default:
        in_s = xb == cb;
        s_is_true = op == GC_FEQ;
        break;
    }
  } else {
    switch (op) {
      case GC_FLT:
      case GC_FGE:
        in_s = gc_bin(xb, cb + 1, 0xff800000u);
        s_is_true = op == GC_FLT;
        break;
      case GC_FLE:
      case GC_FGT:
        in_s = gc_bin(xb, cb, 0xff800000u);
        s_is_true = op == GC_FLE;
        break;
      default:
        in_s = xb == cb;
        s_is_true = op == GC_FEQ;
        break;
    }
  }
  if (nr != s_is_true) {
    return s_is_true ? in_s : !in_s;
  }
  int s_or_nan;
  if (op == GC_FEQ || op == GC_FNE) {
    s_or_nan = in_s | gc_bnan(xb);
  } else if ((s32)cb > 0 || ((cb << 1) == 0 && (op == GC_FGT || op == GC_FLE))) {
    const u32 lo = (cb << 1) == 0 ? 0x00800000u : ((op == GC_FGE || op == GC_FLT) ? cb : cb + 1);
    s_or_nan = ((s32)xb >= (s32)lo) | (xb > 0xff800000u);
  } else {
    const u32 lo = (cb << 1) == 0 ? 0x80800000u : ((op == GC_FLE || op == GC_FGT) ? cb : cb + 1);
    s_or_nan = (xb >= lo) | ((s32)xb > (s32)0x7f800000u);
  }
  return s_is_true ? s_or_nan : !s_or_nan;
}
//! x op c, c a constant (goalc: gc_fcmp(x, c, op))
inline bool fcmp_xk(float x, u32 cb, int op) {
  return gc_fcmp_k(fbits(x), cb, op, gc_fnanres(op));
}

//! goalc's (the int f): truncate and saturate like CVT.W.S, NaN -> 0 (gc_cvtt), sign-extended
inline s64 f2i(float f) {
  if (f != f) {
    return 0;
  }
  if (f >= 2147483648.0f) {
    return INT32_MAX;
  }
  if (f <= -2147483648.0f) {
    return INT32_MIN;
  }
  return (s32)f;
}
//! goalc's (the float i) of an int register: the low 32 bits
inline float i2f(s64 i) {
  return (float)(s32)i;
}


//! the C backend's float divide where the GOAL code divides (`(/ a b)`): b = 0 (or denormal, or
//! NaN: goalc's compare with 0.0) gives the largest float with the sign of a xor b, like the PS2
inline float div_ps2(float a, float b) {
  GC_NATIVE_FP_EXACT
  if (gc_fcmp_k(fbits(b), 0, GC_FEQ, gc_fnanres(GC_FEQ))) {
    return bitsf(0x7f7fffffu ^ (fbits(a) & 0x80000000u) ^ (fbits(b) & 0x80000000u));
  }
  return a / b;
}

//! atan0 (trigonometry.gc): the series for y, x >= 0, rotation units
inline float atan0(float y, float x) {
  GC_NATIVE_FP_EXACT
  const float d = y - x;
  const float s = y + x;
  const float q = div_ps2(d, s);
  const float q2 = q * q;
  const float q3 = q * q2;
  const float q1 = q * bitsf(0x4622f97c);
  const float q4 = q2 * q2;
  const float q5 = q3 * q2;
  const float q7 = q4 * q3;
  const float q9 = q5 * q4;
  const float q11 = q7 * q4;
  const float q13 = q9 * q4;
  const float q15 = q11 * q4;
  float acc = q1 + bitsf(0x46000000);
  float p = q3 * bitsf(0xc55946e1);
  acc = acc + p;
  p = q5 * bitsf(0x450207fd);
  acc = acc + p;
  p = q7 * bitsf(0xc4b556ce);
  acc = acc + p;
  p = q9 * bitsf(0x447b6ca4);
  acc = acc + p;
  p = q11 * bitsf(0xc411ca52);
  acc = acc + p;
  p = q13 * bitsf(0x43640558);
  acc = acc + p;
  p = q15 * bitsf(0xc2292434);
  return acc + p;
}

//! atan (trigonometry.gc): (atan y x) in rotation units, the signs like atan2
inline float atan(float y, float x) {
  GC_NATIVE_FP_EXACT
  const bool x_zero = !gc_fcmp_k(fbits(x), 0, GC_FNE, gc_fnanres(GC_FNE));
  if (x_zero && !gc_fcmp_k(fbits(y), 0, GC_FNE, gc_fnanres(GC_FNE))) {
    return 0.f;
  }
  const bool x_neg = !gc_fcmp_k(fbits(x), 0, GC_FGE, gc_fnanres(GC_FGE));
  const bool y_neg = !gc_fcmp_k(fbits(y), 0, GC_FGE, gc_fnanres(GC_FGE));
  if (x_neg && y_neg) {
    return -32768.f + atan0(0.f - y, 0.f - x);
  }
  if (y_neg) {
    return 0.f - atan0(0.f - y, x);
  }
  if (x_neg) {
    return 32768.f - atan0(y, 0.f - x);
  }
  return atan0(y, x);
}

//! vector-length (vector.gc): sqrt(((x * x) + (y * y)) + (z * z)) (the VU0 code's products by
//! vf0.w = 1.0 are exact)
inline float vector_length(const float v[4]) {
  GC_NATIVE_FP_EXACT
  const float x2 = v[0] * v[0];
  const float y2 = v[1] * v[1];
  const float z2 = v[2] * v[2];
  float len2 = x2 + y2;
  len2 = len2 + z2;
  return __builtin_sqrtf(len2);
}

//! vector-normalize! (vector.gc): x, y, z times (/ length (vector-length v)); w unchanged
inline void vector_normalize(float v[4], float length) {
  GC_NATIVE_FP_EXACT
  const float f = div_ps2(length, vector_length(v));
  v[0] = v[0] * f;
  v[1] = v[1] * f;
  v[2] = v[2] * f;
}

}  // namespace Mips2C::goalc_ops
