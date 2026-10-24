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

// ---------------------------------------------------------------------------
// (AI-assisted) trigonometry.gc / matrix.gc, as goalc's C of them (the same in Jak 1 and Jak 2):
// sin, cos, the matrix-rotate-x/y/z! matrices, matrix*!, vector-matrix*!, vector-rotate*! and
// matrix-rotate-xyz!. Matrices are 16 floats, row by row (GOAL's (-> m data i)). Every lane is
// computed, the zero entries too (signed zeros, NaNs as the GOAL code has them).
// ---------------------------------------------------------------------------

//! sin (trigonometry.gc): the angle (rotation units) wrapped to 16 bits, in radians, the odd series
inline float sin(float a) {
  GC_NATIVE_FP_EXACT
  const s64 i = f2i(a);
  const float x = bitsf(0x38c90fdau) * i2f((s64)(s16)i);
  const float p1 = bitsf(0x3f7fffdeu) * x;
  const float x2 = x * x;
  const float x3 = x * x2;
  float acc = p1 + bitsf(0xbe2aa8f5u) * x3;
  const float x5 = x3 * x2;
  acc = acc + bitsf(0x3c086bf6u) * x5;
  const float x7 = x5 * x2;
  acc = acc + bitsf(0xb94d2072u) * x7;
  const float x9 = x7 * x2;
  return acc + bitsf(0x361aa27fu) * x9;
}

//! cos (trigonometry.gc): (sin (+ 16384.0 a))
inline float cos(float a) {
  GC_NATIVE_FP_EXACT
  return ::Mips2C::goalc_ops::sin(16384.f + a);
}

//! matrix-rotate-x! / -y! / -z! (matrix.gc)
inline void matrix_rotate_x(float m[16], float a) {
  GC_NATIVE_FP_EXACT
  const float s = ::Mips2C::goalc_ops::sin(a);
  const float c = ::Mips2C::goalc_ops::cos(a);
  for (int i = 0; i < 16; i++) {
    m[i] = 0.f;
  }
  m[0] = 1.f;
  m[5] = c;
  m[6] = s;
  m[9] = 0.f - s;
  m[10] = c;
  m[15] = 1.f;
}
inline void matrix_rotate_y(float m[16], float a) {
  GC_NATIVE_FP_EXACT
  const float s = ::Mips2C::goalc_ops::sin(a);
  const float c = ::Mips2C::goalc_ops::cos(a);
  for (int i = 0; i < 16; i++) {
    m[i] = 0.f;
  }
  m[0] = c;
  m[2] = 0.f - s;
  m[5] = 1.f;
  m[8] = s;
  m[10] = c;
  m[15] = 1.f;
}
inline void matrix_rotate_z(float m[16], float a) {
  GC_NATIVE_FP_EXACT
  const float s = ::Mips2C::goalc_ops::sin(a);
  const float c = ::Mips2C::goalc_ops::cos(a);
  for (int i = 0; i < 16; i++) {
    m[i] = 0.f;
  }
  m[0] = c;
  m[1] = s;
  m[4] = 0.f - s;
  m[5] = c;
  m[10] = 1.f;
  m[15] = 1.f;
}

//! vector-matrix*! (matrix.gc): dst = ((m0 * v.x + m1 * v.y) + m2 * v.z) + m3 * v.w; dst may be v
inline void vector_matrix_mul(float dst[4], const float v[4], const float m[16]) {
  GC_NATIVE_FP_EXACT
  const float x = v[0], y = v[1], z = v[2], w = v[3];
  for (int l = 0; l < 4; l++) {
    float acc = m[l] * x;
    const float py = m[4 + l] * y;
    acc = acc + py;
    const float pz = m[8 + l] * z;
    acc = acc + pz;
    const float pw = m[12 + l] * w;
    dst[l] = acc + pw;
  }
}

//! vector-rotate*! (matrix.gc): dst = (m0 * v.x + m1 * v.y) + m2 * v.z, all four lanes
inline void vector_rotate_mul(float dst[4], const float v[4], const float m[16]) {
  GC_NATIVE_FP_EXACT
  const float x = v[0], y = v[1], z = v[2];
  for (int l = 0; l < 4; l++) {
    float acc = m[l] * x;
    const float py = m[4 + l] * y;
    acc = acc + py;
    const float pz = m[8 + l] * z;
    dst[l] = acc + pz;
  }
}

//! matrix*! (matrix.gc): dst = a * b (each row of a times b); dst may be a or b
inline void matrix_mul(float dst[16], const float a[16], const float b[16]) {
  float r[16];
  for (int i = 0; i < 4; i++) {
    ::Mips2C::goalc_ops::vector_matrix_mul(r + 4 * i, a + 4 * i, b);
  }
  memcpy(dst, r, sizeof(r));
}

//! matrix-rotate-xyz! (matrix.gc): rot-x * (rot-y * rot-z), the angles at rot[0..2]
inline void matrix_rotate_xyz(float dst[16], const float rot[3]) {
  float z[16], t[16], yz[16];
  ::Mips2C::goalc_ops::matrix_rotate_z(z, rot[2]);
  ::Mips2C::goalc_ops::matrix_rotate_y(t, rot[1]);
  ::Mips2C::goalc_ops::matrix_mul(yz, t, z);
  ::Mips2C::goalc_ops::matrix_rotate_x(t, rot[0]);
  ::Mips2C::goalc_ops::matrix_mul(dst, t, yz);
}

// ---------------------------------------------------------------------------
// (AI-assisted) goalc's vector float ops (goal_c_ops.h gc_vf_*: one operation per lane, each on its
// own) and quaternion.gc's quaternion*!, quaternion-normalize! and quaternion->matrix as goalc's C
// of them (vf0 = (0 0 0 1); the registers they blend into start at 0 like goalc's locals).
// ---------------------------------------------------------------------------
struct v4 {
  float x, y, z, w;
  float lane(int l) const { return l == 0 ? x : l == 1 ? y : l == 2 ? z : w; }
};
//! a 1.0 the compiler can't fold away (vf0.w times a value: the same multiply as the GOAL code)
inline float one() {
  static volatile float v = 1.0f;
  return v;
}
inline v4 vadd(v4 a, v4 b) {
  return {a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w};
}
inline v4 vsub(v4 a, v4 b) {
  return {a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w};
}
inline v4 vmul(v4 a, v4 b) {
  return {a.x * b.x, a.y * b.y, a.z * b.z, a.w * b.w};
}
inline v4 vdiv(v4 a, v4 b) {
  return {a.x / b.x, a.y / b.y, a.z / b.z, a.w / b.w};
}
inline v4 vsqrt(v4 a) {
  return {__builtin_sqrtf(a.x), __builtin_sqrtf(a.y), __builtin_sqrtf(a.z), __builtin_sqrtf(a.w)};
}
inline v4 vsplat(v4 a, int l) {
  const float f = a.lane(l);
  return {f, f, f, f};
}
//! shufps a, a, ctrl
inline v4 vswz(v4 a, u32 c) {
  return {a.lane(c & 3), a.lane((c >> 2) & 3), a.lane((c >> 4) & 3), a.lane((c >> 6) & 3)};
}
//! blendps: mask bit i takes lane i from b
inline v4 vblend(v4 a, v4 b, u32 m) {
  return {(m & 1) ? b.x : a.x, (m & 2) ? b.y : a.y, (m & 4) ? b.z : a.z, (m & 8) ? b.w : a.w};
}
inline v4 vf0() {
  return {0.f, 0.f, 0.f, one()};
}

//! quaternion*! (quaternion.gc): a * b
inline v4 quaternion_mul(v4 a, v4 b) {
  GC_NATIVE_FP_EXACT
  const v4 c = vf0();
  v4 r12 = {0.f, 0.f, 0.f, 0.f};
  r12 = vblend(r12, vsub(c, c), 8);
  const v4 r11 = vmul(a, b);
  v4 r7 = vmul(vswz(a, 9), vswz(b, 18));
  v4 r16 = vmul(vswz(b, 9), vswz(a, 18));
  r12 = vsub(r7, r16);
  r7 = vmul(a, vsplat(b, 3));
  r7 = vadd(r7, vmul(b, vsplat(a, 3)));
  for (int l = 3; l >= 0; l--) {
    r7 = vblend(r7, vsub(r7, vmul(c, vsplat(r11, l))), 8);
  }
  return vadd(r7, vmul(r12, vsplat(c, 3)));
}

//! quaternion-normalize! (quaternion.gc)
inline v4 quaternion_normalize(v4 q) {
  GC_NATIVE_FP_EXACT
  const v4 c = vf0();
  v4 r3 = {0.f, 0.f, 0.f, 0.f};
  v4 r7 = vmul(q, q);
  r3 = vblend(r3, vadd(r7, vsplat(r7, 2)), 8);
  r3 = vblend(r3, vadd(r3, vmul(c, vsplat(r7, 1))), 8);
  r7 = vblend(r7, vadd(r3, vmul(c, vsplat(r7, 0))), 8);
  const v4 r4 = vdiv(vsplat(c, 3), vsqrt(vsplat(r7, 3)));
  return vmul(q, r4);
}

//! quaternion->matrix (quaternion.gc): rows m[0..3]
inline void quaternion_to_matrix(v4 m[4], v4 q) {
  GC_NATIVE_FP_EXACT
  const v4 c = vf0();
  const v4 q2 = vadd(q, q);
  v4 r8 = {0.f, 0.f, 0.f, 0.f}, r9 = r8, r10 = r8;
  r8 = vblend(r8, vadd(c, vsplat(q, 3)), 1);
  r8 = vblend(r8, vadd(c, vsplat(q, 2)), 2);
  r8 = vblend(r8, vsub(c, vsplat(q, 1)), 4);
  r8 = vblend(r8, vsub(c, vsplat(c, 3)), 8);
  r9 = vblend(r9, vsub(c, vsplat(q, 2)), 1);
  r9 = vblend(r9, vadd(c, vsplat(q, 3)), 2);
  r9 = vblend(r9, vadd(c, vsplat(q, 0)), 4);
  r9 = vblend(r9, vsub(c, vsplat(c, 3)), 8);
  r10 = vblend(r10, vadd(c, vsplat(q, 1)), 1);
  r10 = vblend(r10, vsub(c, vsplat(q, 0)), 2);
  r10 = vblend(r10, vadd(c, vsplat(q, 3)), 4);
  r10 = vblend(r10, vsub(c, vsplat(c, 3)), 8);
  r8 = vsub(vmul(vswz(q2, 9), vswz(r8, 18)), vmul(vswz(r8, 9), vswz(q2, 18)));
  r9 = vsub(vmul(vswz(q2, 9), vswz(r9, 18)), vmul(vswz(r9, 9), vswz(q2, 18)));
  r10 = vsub(vmul(vswz(q2, 9), vswz(r10, 18)), vmul(vswz(r10, 9), vswz(q2, 18)));
  r8 = vblend(r8, vadd(r8, vsplat(c, 3)), 1);
  r9 = vblend(r9, vadd(r9, vsplat(c, 3)), 2);
  r10 = vblend(r10, vadd(r10, vsplat(c, 3)), 4);
  m[0] = r8;
  m[1] = r9;
  m[2] = r10;
  m[3] = c;
}

}  // namespace Mips2C::goalc_ops
