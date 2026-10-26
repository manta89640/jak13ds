/*!
 * @file native2_font.cpp
 * (AI-assisted)
 * Native version of Jak 2's draw-string-asm (font.cpp): the string walk, the control codes and
 * the per-character GIF data, for every HUD / menu / subtitle string.
 *
 * The body is the mips2c code with the MIPS / VU0 registers as locals instead of an
 * ExecutionContext in memory (each GPR as two u64 halves, each VF register as 4 floats), every op
 * written as its ExecutionContext helper (mips2c_private.h) computes it: the same float
 * expressions lane by lane, the same 64-bit integer arithmetic, the same loads and stores. It was
 * generated from the mips2c function (scratch tool m2n.py) and kept in the same block order, so a
 * line here maps to a line of font.cpp (the MIPS instruction is in each comment).
 * No GOAL calls and no host state (font.cpp keeps no VU0 registers between functions).
 */

#include <cstring>

#include "native2.h"

namespace Mips2C::jak2::native2 {

namespace {

//! a VU0 register
struct Vf {
  float f[4];
};
constexpr Vf kVf0 = {{0, 0, 0, 1.f}};

inline s32 get_i(const Vf& v, int i) {
  s32 r;
  memcpy(&r, &v.f[i], 4);
  return r;
}
inline void set_i(Vf& v, int i, s32 x) {
  memcpy(&v.f[i], &x, 4);
}

//! ExecutionContext::float_to_int_sat (vftoi4_sat)
inline s32 float_to_int_sat(float f) {
  if (f >= (float)INT32_MAX) {
    return INT32_MAX;
  } else if (f <= (float)INT32_MIN) {
    return INT32_MIN;
  } else {
    return f;
  }
}

//! qmtc2 / qmfc2
inline void vf_from_gpr(Vf& v, u64 lo, u64 hi) {
  memcpy(&v.f[0], &lo, 8);
  memcpy(&v.f[2], &hi, 8);
}
inline void gpr_from_vf(u64& lo, u64& hi, const Vf& v) {
  memcpy(&lo, &v.f[0], 8);
  memcpy(&hi, &v.f[2], 8);
}

//! pextlb: interleave the low 8 bytes of s1 (even bytes) and s0 (odd bytes)
inline void pextlb(u64& lo, u64& hi, u64 s0, u64 s1) {
  u8 a[8], b[8], o[16];
  memcpy(a, &s0, 8);
  memcpy(b, &s1, 8);
  for (int i = 0; i < 8; i++) {
    o[2 * i] = b[i];
    o[2 * i + 1] = a[i];
  }
  memcpy(&lo, o, 8);
  memcpy(&hi, o + 8, 8);
}
//! pextlh: the same with the low 4 halfwords
inline void pextlh(u64& lo, u64& hi, u64 s0, u64 s1) {
  u16 a[4], b[4], o[8];
  memcpy(a, &s0, 8);
  memcpy(b, &s1, 8);
  for (int i = 0; i < 4; i++) {
    o[2 * i] = b[i];
    o[2 * i + 1] = a[i];
  }
  memcpy(&lo, o, 8);
  memcpy(&hi, o + 4, 8);  // o is u16: the upper 4 halfwords
}

//! GOAL memory with the base pointer in a register (a store doesn't make the compiler reload it)
struct Mem {
  u8* const base = g_ee_main_mem;
  template <typename T>
  T ld(u32 addr, s32 off) const {
    T v;
    memcpy(&v, base + addr + off, sizeof(T));
    return v;
  }
  void lq(u64& lo, u64& hi, u32 addr) const {
    memcpy(&lo, base + addr, 8);
    memcpy(&hi, base + addr + 8, 8);
  }
  void lqc2(Vf& v, u32 addr) const { memcpy(&v, MIPS2C_ASSUME_ALIGNED(base + addr, 16), 16); }
  void sq(u32 addr, u64 lo, u64 hi) const {
    MIPS2C_NATIVE_LOG_STORE(addr, 16);
    memcpy(base + addr, &lo, 8);
    memcpy(base + addr + 8, &hi, 8);
  }
  void sqc2(u32 addr, const Vf& v) const {
    MIPS2C_NATIVE_LOG_STORE(addr, 16);
    memcpy(MIPS2C_ASSUME_ALIGNED(base + addr, 16), &v, 16);
  }
  void sw(u32 addr, u32 v) const {
    MIPS2C_NATIVE_LOG_STORE(addr, 4);
    memcpy(base + addr, &v, 4);
  }
};

}  // namespace

/*!
 * (draw-string-asm string dma-buffer font-context) -> the final position (vf23 - origin, xy)
 */
u64 draw_string_asm_impl(const NativeArgs& args) {
  static const u32 sym_font_work = sym_value_addr("*font-work*");
  static const u32 sym_font12_table = sym_value_addr("*font12-table*");
  static const u32 sym_font24_table = sym_value_addr("*font24-table*");
  static const u32 sym_math_camera = sym_value_addr("*math-camera*");
  static const u32 sym_video_params = sym_value_addr("*video-params*");
  const Mem m;
  // clang-format off
  u64 x_a0 = args.a[0];
  u64 x_a1 = args.a[1];
  u64 x_a2 = args.a[2];
  u64 x_a3 = args.a[3], xh_a3 = 0;
  u64 x_t0 = args.a[4];
  u64 x_t1 = args.a[5], xh_t1 = 0;
  u64 x_t2 = args.a[6], xh_t2 = 0;
  u64 x_t3 = args.a[7], xh_t3 = 0;
  u64 x_t4 = 0, xh_t4 = 0;
  u64 x_t5 = 0, xh_t5 = 0;
  u64 x_t6 = 0;
  u64 x_v0 = 0, xh_v0 = 0;
  u64 x_v1 = 0;
  Vf f_vf1 = {};
  Vf f_vf2 = {};
  Vf f_vf3 = {};
  Vf f_vf4 = {};
  Vf f_vf5 = {};
  Vf f_vf6 = {};
  Vf f_vf7 = {};
  Vf f_vf8 = {};
  Vf f_vf9 = {};
  Vf f_vf10 = {};
  Vf f_vf11 = {};
  Vf f_vf12 = {};
  Vf f_vf13 = {};
  Vf f_vf14 = {};
  Vf f_vf15 = {};
  Vf f_vf16 = {};
  Vf f_vf17 = {};
  Vf f_vf18 = {};
  Vf f_vf19 = {};
  Vf f_vf20 = {};
  Vf f_vf23 = {};
  Vf f_vf24 = {};
  Vf f_vf25 = {};
  Vf f_vf26 = {};
  Vf f_vf27 = {};
  Vf f_vf28 = {};
  Vf f_vf29 = {};
  Vf f_vf30 = {};
  Vf f_vf31 = {};
  Vf acc = {};
  float Q = 0;
  bool bc = false;
  x_v1 = (u64)(s64)m.ld<s32>(sym_math_camera, 0);  // lw v1, *math-camera*(s7)
  m.lqc2(f_vf26, (u32)x_v1 + (u32)(812));  // lqc2 vf26, 812(v1)
  m.lqc2(f_vf27, (u32)x_v1 + (u32)(812));  // lqc2 vf27, 812(v1)
  x_v1 = (u64)(s64)m.ld<s32>((u32)x_a2, 68);  // lw v1, 68(a2)
  m.lqc2(f_vf25, (u32)x_a2 + (u32)(44));  // lqc2 vf25, 44(a2)
  m.lqc2(f_vf23, (u32)x_a2 + (u32)(12));  // lqc2 vf23, 12(a2)
  m.lqc2(f_vf24, (u32)x_a2 + (u32)(12));  // lqc2 vf24, 12(a2)
  m.lqc2(f_vf28, (u32)x_v1 + (u32)(0));  // lqc2 vf28, 0(v1)
  m.lqc2(f_vf29, (u32)x_v1 + (u32)(16));  // lqc2 vf29, 16(v1)
  m.lqc2(f_vf30, (u32)x_v1 + (u32)(32));  // lqc2 vf30, 32(v1)
  m.lqc2(f_vf31, (u32)x_v1 + (u32)(48));  // lqc2 vf31, 48(v1)
  x_v1 = (u64)(s64)m.ld<s32>(sym_video_params, 0);  // lw v1, *video-params*(s7)
  // (no-op)  // or v1, v1, r0
  x_t0 = (u64)(s64)m.ld<s32>((u32)x_v1, 16);  // lw t0, 16(v1)
  f_vf26.f[0] += u2f((u32)x_t0) * 2;  // (mips2c pc hack)
  f_vf26.f[1] += 1;  // (mips2c pc hack)
  m.lqc2(f_vf1, (u32)x_v1 + (u32)(32));  // lqc2 vf1, 32(v1)
  Q = kVf0.f[3] / f_vf25.f[3];  // vdiv Q, vf0.w, vf25.w
  m.lqc2(f_vf2, (u32)x_v1 + (u32)(16));  // lqc2 vf2, 16(v1)
  { const Vf s0 = f_vf25, s1 = f_vf1; f_vf25.f[0] = s0.f[0] * s1.f[0]; }  // vmul.x vf25, vf25, vf1
  { const Vf s0 = f_vf23, s1 = f_vf1; f_vf23.f[0] = s0.f[0] * s1.f[0]; }  // vmul.x vf23, vf23, vf1
  x_v1 = (u64)(s64)m.ld<s32>((u32)x_a2, 64);  // lw v1, 64(a2)
  if (!(((u32)x_v1) & (1 << 6))) {  // (mips2c pc hack)
  { const Vf s0 = f_vf24, s1 = f_vf1; f_vf24.f[0] = s0.f[0] * s1.f[0]; }  // vmul.x vf24, vf24, vf1
  }
  if (!(((u32)x_v1) & (1 << 5))) {  // (mips2c pc hack)
  f_vf26.f[0] -= (u2f((u32)x_t0) * 2) - (f_vf25.f[3] * u2f((u32)x_t0) * 2);  // (mips2c pc hack)
  f_vf26.f[1] -= 1 - (f_vf25.f[3] * 1);  // (mips2c pc hack)
  }
  // vwaitq  // vwaitq
  { const Vf s0 = f_vf25; f_vf25.f[0] = s0.f[0] * Q; f_vf25.f[1] = s0.f[1] * Q; }  // vmulq.xy vf25, vf25, Q
  { const Vf s0 = f_vf23; f_vf23.f[0] = s0.f[0] * Q; f_vf23.f[1] = s0.f[1] * Q; }  // vmulq.xy vf23, vf23, Q
  { const Vf s0 = f_vf24; f_vf24.f[0] = s0.f[0] * Q; f_vf24.f[1] = s0.f[1] * Q; }  // vmulq.xy vf24, vf24, Q
  { const Vf s0 = f_vf25, s1 = f_vf24; f_vf25.f[0] = s0.f[0] + s1.f[0]; f_vf25.f[1] = s0.f[1] + s1.f[1]; }  // vadd.xy vf25, vf25, vf24
  { const Vf s0 = f_vf28, s1 = f_vf25; f_vf28.f[0] = s0.f[0] * s1.f[3]; }  // vmulw.x vf28, vf28, vf25
  { const Vf s0 = f_vf29, s1 = f_vf25; f_vf29.f[1] = s0.f[1] * s1.f[3]; }  // vmulw.y vf29, vf29, vf25
  { const Vf s0 = f_vf28, s1 = f_vf2; f_vf28.f[0] = s0.f[0] * s1.f[0]; }  // vmul.x vf28, vf28, vf2
  x_v1 = (u64)(s64)m.ld<s32>(sym_font_work, 0);  // lw v1, *font-work*(s7)
  // (no-op)  // or v1, v1, r0
  m.sw((u32)x_v1 + (u32)(2872), (u32)x_a1);  // sw a1, 2872(v1)
  x_a1 = (u64)(s64)m.ld<s32>((u32)x_a1, 4);  // lw a1, 4(a1)
  m.sw((u32)x_v1 + (u32)(2876), (u32)x_a0);  // sw a0, 2876(v1)
  x_t0 = (u64)(s64)m.ld<s32>((u32)x_a2, 64);  // lw t0, 64(a2)
  { const Vf s0 = kVf0; f_vf1.f[0] = s0.f[0]; f_vf1.f[1] = s0.f[1]; f_vf1.f[2] = s0.f[2]; f_vf1.f[3] = s0.f[3]; }  // vmove.xyzw vf1, vf0
  { const Vf s0 = kVf0; f_vf2.f[0] = s0.f[0]; f_vf2.f[1] = s0.f[1]; f_vf2.f[2] = s0.f[2]; f_vf2.f[3] = s0.f[3]; }  // vmove.xyzw vf2, vf0
  { const Vf s0 = kVf0; f_vf3.f[0] = s0.f[0]; f_vf3.f[1] = s0.f[1]; f_vf3.f[2] = s0.f[2]; f_vf3.f[3] = s0.f[3]; }  // vmove.xyzw vf3, vf0
  { const Vf s0 = kVf0; f_vf4.f[0] = s0.f[0]; f_vf4.f[1] = s0.f[1]; f_vf4.f[2] = s0.f[2]; f_vf4.f[3] = s0.f[3]; }  // vmove.xyzw vf4, vf0
  m.sw((u32)x_v1 + (u32)(2880), (u32)x_t0);  // sw t0, 2880(v1)
  m.lqc2(f_vf16, (u32)x_v1 + (u32)(416));  // lqc2 vf16, 416(v1)
  m.lqc2(f_vf17, (u32)x_v1 + (u32)(432));  // lqc2 vf17, 432(v1)
  m.lqc2(f_vf18, (u32)x_v1 + (u32)(448));  // lqc2 vf18, 448(v1)
  x_a3 = (u64)(s64)m.ld<s32>(sym_video_params, 0);  // lw a3, *video-params*(s7)
  // (no-op)  // or a3, a3, r0
  m.lqc2(f_vf1, (u32)x_a3 + (u32)(16));  // lqc2 vf1, 16(a3)
  x_a3 = x_t0 & (u64)(32);  // andi a3, t0, 32
  bc = x_a3 != 0;  // bne a3, r0, L35
  x_a3 = (u64)(s64)m.ld<s32>(sym_font12_table, 0);  // lw a3, *font12-table*(s7)
  if (bc) {goto block_2;}
  // (no-op)  // or a3, a3, r0
  m.lq(x_t1, xh_t1, ((u32)x_v1 + (u32)(80)) & ~15u);  // lq t1, 80(v1)
  m.lq(x_t2, xh_t2, ((u32)x_v1 + (u32)(96)) & ~15u);  // lq t2, 96(v1)
  m.lq(x_t3, xh_t3, ((u32)x_v1 + (u32)(112)) & ~15u);  // lq t3, 112(v1)
  m.lq(x_t4, xh_t4, ((u32)x_v1 + (u32)(128)) & ~15u);  // lq t4, 128(v1)
  m.sq(((u32)x_v1 + (u32)(2800)) & ~15u, x_t1, xh_t1);  // sq t1, 2800(v1)
  m.sq(((u32)x_v1 + (u32)(2816)) & ~15u, x_t2, xh_t2);  // sq t2, 2816(v1)
  m.sq(((u32)x_v1 + (u32)(2832)) & ~15u, x_t3, xh_t3);  // sq t3, 2832(v1)
  m.sq(((u32)x_v1 + (u32)(2848)) & ~15u, x_t4, xh_t4);  // sq t4, 2848(v1)
  m.lqc2(f_vf13, (u32)x_v1 + (u32)(208));  // lqc2 vf13, 208(v1)
  m.lqc2(f_vf14, (u32)x_v1 + (u32)(224));  // lqc2 vf14, 224(v1)
  m.lqc2(f_vf15, (u32)x_v1 + (u32)(240));  // lqc2 vf15, 240(v1)
  m.sqc2((u32)x_v1 + (u32)(352), f_vf14);  // sqc2 vf14, 352(v1)
  m.sqc2((u32)x_v1 + (u32)(368), f_vf14);  // sqc2 vf14, 368(v1)
  m.sqc2((u32)x_v1 + (u32)(384), f_vf14);  // sqc2 vf14, 384(v1)
  m.sqc2((u32)x_v1 + (u32)(400), f_vf14);  // sqc2 vf14, 400(v1)
  goto block_3;
  block_2:
  x_a3 = (u64)(s64)m.ld<s32>(sym_font24_table, 0);  // lw a3, *font24-table*(s7)
  // (no-op)  // or a3, a3, r0
  m.lq(x_t1, xh_t1, ((u32)x_v1 + (u32)(144)) & ~15u);  // lq t1, 144(v1)
  m.lq(x_t2, xh_t2, ((u32)x_v1 + (u32)(160)) & ~15u);  // lq t2, 160(v1)
  m.lq(x_t3, xh_t3, ((u32)x_v1 + (u32)(176)) & ~15u);  // lq t3, 176(v1)
  m.lq(x_t4, xh_t4, ((u32)x_v1 + (u32)(192)) & ~15u);  // lq t4, 192(v1)
  m.sq(((u32)x_v1 + (u32)(2800)) & ~15u, x_t1, xh_t1);  // sq t1, 2800(v1)
  m.sq(((u32)x_v1 + (u32)(2816)) & ~15u, x_t2, xh_t2);  // sq t2, 2816(v1)
  m.sq(((u32)x_v1 + (u32)(2832)) & ~15u, x_t3, xh_t3);  // sq t3, 2832(v1)
  m.sq(((u32)x_v1 + (u32)(2848)) & ~15u, x_t4, xh_t4);  // sq t4, 2848(v1)
  m.lqc2(f_vf13, (u32)x_v1 + (u32)(256));  // lqc2 vf13, 256(v1)
  m.lqc2(f_vf15, (u32)x_v1 + (u32)(336));  // lqc2 vf15, 336(v1)
  m.lqc2(f_vf14, (u32)x_v1 + (u32)(272));  // lqc2 vf14, 272(v1)
  m.sqc2((u32)x_v1 + (u32)(352), f_vf14);  // sqc2 vf14, 352(v1)
  m.lqc2(f_vf14, (u32)x_v1 + (u32)(288));  // lqc2 vf14, 288(v1)
  m.sqc2((u32)x_v1 + (u32)(368), f_vf14);  // sqc2 vf14, 368(v1)
  m.lqc2(f_vf14, (u32)x_v1 + (u32)(304));  // lqc2 vf14, 304(v1)
  m.sqc2((u32)x_v1 + (u32)(384), f_vf14);  // sqc2 vf14, 384(v1)
  m.lqc2(f_vf14, (u32)x_v1 + (u32)(320));  // lqc2 vf14, 320(v1)
  m.sqc2((u32)x_v1 + (u32)(400), f_vf14);  // sqc2 vf14, 400(v1)
  block_3:
  x_t1 = (u64)(s64)m.ld<s32>((u32)x_a2, 60);  // lw t1, 60(a2)
  m.sw((u32)x_v1 + (u32)(2864), (u32)x_t1);  // sw t1, 2864(v1)
  x_t1 = (u64)(s64)(s32)((u32)x_t1 << 4);  // sll t1, t1, 4
  x_t2 = x_t1 + x_v1;  // daddu t2, t1, v1
  x_t1 = (u64)m.ld<u32>((u32)x_t2, 2160);  // lwu t1, 2160(t2)
  x_t3 = (u64)m.ld<u32>((u32)x_t2, 2164);  // lwu t3, 2164(t2)
  pextlb(x_t4, xh_t4, u64(0), x_t1);  // pextlb t4, r0, t1
  x_t1 = (u64)m.ld<u32>((u32)x_t2, 2168);  // lwu t1, 2168(t2)
  pextlh(x_t4, xh_t4, u64(0), x_t4);  // pextlh t4, r0, t4
  x_t2 = (u64)m.ld<u32>((u32)x_t2, 2172);  // lwu t2, 2172(t2)
  pextlb(x_t3, xh_t3, u64(0), x_t3);  // pextlb t3, r0, t3
  m.sq(((u32)x_v1 + (u32)(608)) & ~15u, x_t4, xh_t4);  // sq t4, 608(v1)
  pextlh(x_t3, xh_t3, u64(0), x_t3);  // pextlh t3, r0, t3
  m.sq(((u32)x_v1 + (u32)(992)) & ~15u, x_t4, xh_t4);  // sq t4, 992(v1)
  pextlb(x_t1, xh_t1, u64(0), x_t1);  // pextlb t1, r0, t1
  m.sq(((u32)x_v1 + (u32)(624)) & ~15u, x_t3, xh_t3);  // sq t3, 624(v1)
  pextlh(x_t1, xh_t1, u64(0), x_t1);  // pextlh t1, r0, t1
  m.sq(((u32)x_v1 + (u32)(1008)) & ~15u, x_t3, xh_t3);  // sq t3, 1008(v1)
  pextlb(x_t2, xh_t2, u64(0), x_t2);  // pextlb t2, r0, t2
  m.sq(((u32)x_v1 + (u32)(640)) & ~15u, x_t1, xh_t1);  // sq t1, 640(v1)
  pextlh(x_t2, xh_t2, u64(0), x_t2);  // pextlh t2, r0, t2
  m.sq(((u32)x_v1 + (u32)(1024)) & ~15u, x_t1, xh_t1);  // sq t1, 1024(v1)
  m.sq(((u32)x_v1 + (u32)(656)) & ~15u, x_t2, xh_t2);  // sq t2, 656(v1)
  m.sq(((u32)x_v1 + (u32)(1040)) & ~15u, x_t2, xh_t2);  // sq t2, 1040(v1)
  x_t1 = x_v1;  // or t1, v1, r0
  block_4:
  x_t2 = (u64)m.ld<u8>((u32)x_a0, 4);  // lbu t2, 4(a0)
  x_a0 = x_a0 + (u64)(s64)(1);  // daddiu a0, a0, 1
  m.lqc2(f_vf20, (u32)x_v1 + (u32)(2800));  // lqc2 vf20, 2800(v1)
  bc = x_t2 == 0;  // beq t2, r0, L63
  x_t3 = x_t2 + (u64)(s64)(-3);  // daddiu t3, t2, -3
  if (bc) {goto block_78;}
  bc = ((s64)x_t3) <= 0;  // blez t3, L52
  x_t3 = x_t2 + (u64)(s64)(-126);  // daddiu t3, t2, -126
  if (bc) {goto block_59;}
  bc = x_t3 != 0;  // bne t3, r0, L56
  if (bc) {goto block_65;}
  x_t2 = (u64)m.ld<u8>((u32)x_a0, 4);  // lbu t2, 4(a0)
  x_a0 = x_a0 + (u64)(s64)(1);  // daddiu a0, a0, 1
  x_t3 = (u64)(s64)(s32)(u64(0) + (u64)(s64)(0));  // addiu t3, r0, 0
  x_t4 = (u64)(s64)(s32)(u64(0) + (u64)(s64)(0));  // addiu t4, r0, 0
  bc = x_t2 == 0;  // beq t2, r0, L63
  x_t5 = x_t2 + (u64)(s64)(-43);  // daddiu t5, t2, -43
  if (bc) {goto block_78;}
  if (x_t5 == 0) { x_t3 = x_t2; }  // movz t3, t2, t5
  x_t5 = x_t2 + (u64)(s64)(-45);  // daddiu t5, t2, -45
  if (x_t5 == 0) { x_t3 = x_t2; }  // movz t3, t2, t5
  bc = x_t3 != 0;  // bne t3, r0, L38
  x_t5 = x_t2 + (u64)(s64)(-91);  // daddiu t5, t2, -91
  if (bc) {goto block_18;}
  bc = x_t5 == 0;  // beq t5, r0, L37
  x_t4 = x_t2 + (u64)(s64)(-93);  // daddiu t4, t2, -93
  if (bc) {goto block_4;}
  bc = x_t4 == 0;  // beq t4, r0, L37
  x_t4 = x_t2 + (u64)(s64)(-121);  // daddiu t4, t2, -121
  if (bc) {goto block_4;}
  bc = x_t4 == 0;  // beq t4, r0, L50
  x_t4 = x_t2 + (u64)(s64)(-89);  // daddiu t4, t2, -89
  if (bc) {goto block_57;}
  bc = x_t4 == 0;  // beq t4, r0, L50
  x_t4 = x_t2 + (u64)(s64)(-122);  // daddiu t4, t2, -122
  if (bc) {goto block_57;}
  bc = x_t4 == 0;  // beq t4, r0, L51
  x_t4 = x_t2 + (u64)(s64)(-90);  // daddiu t4, t2, -90
  if (bc) {goto block_58;}
  bc = x_t4 == 0;  // beq t4, r0, L51
  x_t4 = x_t2 + (u64)(s64)(-48);  // daddiu t4, t2, -48
  if (bc) {goto block_58;}
  bc = ((s64)x_t4) < 0;  // bltz t4, L56
  x_t4 = x_t2 + (u64)(s64)(-57);  // daddiu t4, t2, -57
  if (bc) {goto block_65;}
  bc = ((s64)x_t4) > 0;  // bgtz t4, L56
  x_t4 = x_t2 + (u64)(s64)(-126);  // daddiu t4, t2, -126
  if (bc) {goto block_65;}
  bc = x_t4 == 0;  // beq t4, r0, L56
  x_t4 = x_t2 + (u64)(s64)(-48);  // daddiu t4, t2, -48
  if (bc) {goto block_65;}
  block_18:
  x_t2 = (u64)m.ld<u8>((u32)x_a0, 4);  // lbu t2, 4(a0)
  x_a0 = x_a0 + (u64)(s64)(1);  // daddiu a0, a0, 1
  bc = x_t2 == 0;  // beq t2, r0, L63
  x_t5 = x_t2 + (u64)(s64)(-110);  // daddiu t5, t2, -110
  if (bc) {goto block_78;}
  bc = x_t5 == 0;  // beq t5, r0, L39
  x_t5 = x_t2 + (u64)(s64)(-78);  // daddiu t5, t2, -78
  if (bc) {goto block_38;}
  bc = x_t5 == 0;  // beq t5, r0, L39
  x_t5 = x_t2 + (u64)(s64)(-108);  // daddiu t5, t2, -108
  if (bc) {goto block_38;}
  bc = x_t5 == 0;  // beq t5, r0, L37
  x_t5 = x_t2 + (u64)(s64)(-76);  // daddiu t5, t2, -76
  if (bc) {goto block_4;}
  bc = x_t5 == 0;  // beq t5, r0, L37
  x_t5 = x_t2 + (u64)(s64)(-119);  // daddiu t5, t2, -119
  if (bc) {goto block_4;}
  bc = x_t5 == 0;  // beq t5, r0, L37
  x_t5 = x_t2 + (u64)(s64)(-87);  // daddiu t5, t2, -87
  if (bc) {goto block_4;}
  bc = x_t5 == 0;  // beq t5, r0, L37
  x_t5 = x_t2 + (u64)(s64)(-107);  // daddiu t5, t2, -107
  if (bc) {goto block_4;}
  bc = x_t5 == 0;  // beq t5, r0, L41
  x_t5 = x_t2 + (u64)(s64)(-75);  // daddiu t5, t2, -75
  if (bc) {goto block_41;}
  bc = x_t5 == 0;  // beq t5, r0, L41
  x_t5 = x_t2 + (u64)(s64)(-106);  // daddiu t5, t2, -106
  if (bc) {goto block_41;}
  bc = x_t5 == 0;  // beq t5, r0, L42
  x_t5 = x_t2 + (u64)(s64)(-74);  // daddiu t5, t2, -74
  if (bc) {goto block_43;}
  bc = x_t5 == 0;  // beq t5, r0, L42
  x_t5 = x_t2 + (u64)(s64)(-104);  // daddiu t5, t2, -104
  if (bc) {goto block_43;}
  bc = x_t5 == 0;  // beq t5, r0, L44
  x_t5 = x_t2 + (u64)(s64)(-72);  // daddiu t5, t2, -72
  if (bc) {goto block_47;}
  bc = x_t5 == 0;  // beq t5, r0, L44
  x_t5 = x_t2 + (u64)(s64)(-118);  // daddiu t5, t2, -118
  if (bc) {goto block_47;}
  bc = x_t5 == 0;  // beq t5, r0, L47
  x_t5 = x_t2 + (u64)(s64)(-86);  // daddiu t5, t2, -86
  if (bc) {goto block_52;}
  bc = x_t5 == 0;  // beq t5, r0, L47
  x_t5 = x_t2 + (u64)(s64)(-117);  // daddiu t5, t2, -117
  if (bc) {goto block_52;}
  bc = x_t5 == 0;  // beq t5, r0, L37
  x_t5 = x_t2 + (u64)(s64)(-85);  // daddiu t5, t2, -85
  if (bc) {goto block_4;}
  bc = x_t5 == 0;  // beq t5, r0, L37
  x_t5 = x_t2 + (u64)(s64)(-48);  // daddiu t5, t2, -48
  if (bc) {goto block_4;}
  bc = ((s64)x_t5) < 0;  // bltz t5, L56
  x_t6 = x_t2 + (u64)(s64)(-57);  // daddiu t6, t2, -57
  if (bc) {goto block_65;}
  bc = ((s64)x_t6) > 0;  // bgtz t6, L56
  x_t6 = (u64)(s64)(s32)((u32)x_t4 << 2);  // sll t6, t4, 2
  if (bc) {goto block_65;}
  x_t2 = x_t4 + x_t6;  // daddu t2, t4, t6
  x_t2 = (u64)(s64)(s32)((u32)x_t2 << 1);  // sll t2, t2, 1
  x_t4 = x_t2 + x_t5;  // daddu t4, t2, t5
  goto block_18;
  block_38:
  bc = x_t4 != 0;  // bne t4, r0, L40
  x_t2 = (u64)(s64)(s32)(u64(0) + (u64)(s64)(-33));  // addiu t2, r0, -33
  if (bc) {goto block_40;}
  m.lq(x_a3, xh_a3, ((u32)x_v1 + (u32)(80)) & ~15u);  // lq a3, 80(v1)
  m.lq(x_t3, xh_t3, ((u32)x_v1 + (u32)(96)) & ~15u);  // lq t3, 96(v1)
  m.lq(x_t4, xh_t4, ((u32)x_v1 + (u32)(112)) & ~15u);  // lq t4, 112(v1)
  m.lq(x_t5, xh_t5, ((u32)x_v1 + (u32)(128)) & ~15u);  // lq t5, 128(v1)
  m.sq(((u32)x_v1 + (u32)(2800)) & ~15u, x_a3, xh_a3);  // sq a3, 2800(v1)
  m.sq(((u32)x_v1 + (u32)(2816)) & ~15u, x_t3, xh_t3);  // sq t3, 2816(v1)
  m.sq(((u32)x_v1 + (u32)(2832)) & ~15u, x_t4, xh_t4);  // sq t4, 2832(v1)
  m.sq(((u32)x_v1 + (u32)(2848)) & ~15u, x_t5, xh_t5);  // sq t5, 2848(v1)
  x_a3 = (u64)(s64)m.ld<s32>(sym_font12_table, 0);  // lw a3, *font12-table*(s7)
  // (no-op)  // or a3, a3, r0
  m.lqc2(f_vf13, (u32)x_v1 + (u32)(208));  // lqc2 vf13, 208(v1)
  m.lqc2(f_vf14, (u32)x_v1 + (u32)(224));  // lqc2 vf14, 224(v1)
  m.lqc2(f_vf15, (u32)x_v1 + (u32)(240));  // lqc2 vf15, 240(v1)
  m.sqc2((u32)x_v1 + (u32)(352), f_vf14);  // sqc2 vf14, 352(v1)
  m.sqc2((u32)x_v1 + (u32)(368), f_vf14);  // sqc2 vf14, 368(v1)
  m.sqc2((u32)x_v1 + (u32)(384), f_vf14);  // sqc2 vf14, 384(v1)
  m.sqc2((u32)x_v1 + (u32)(400), f_vf14);  // sqc2 vf14, 400(v1)
  x_t0 = x_t0 & x_t2;  // and t0, t0, t2
  goto block_4;
  block_40:
  m.lq(x_a3, xh_a3, ((u32)x_v1 + (u32)(144)) & ~15u);  // lq a3, 144(v1)
  m.lq(x_t2, xh_t2, ((u32)x_v1 + (u32)(160)) & ~15u);  // lq t2, 160(v1)
  m.lq(x_t3, xh_t3, ((u32)x_v1 + (u32)(176)) & ~15u);  // lq t3, 176(v1)
  m.lq(x_t4, xh_t4, ((u32)x_v1 + (u32)(192)) & ~15u);  // lq t4, 192(v1)
  m.sq(((u32)x_v1 + (u32)(2800)) & ~15u, x_a3, xh_a3);  // sq a3, 2800(v1)
  m.sq(((u32)x_v1 + (u32)(2816)) & ~15u, x_t2, xh_t2);  // sq t2, 2816(v1)
  m.sq(((u32)x_v1 + (u32)(2832)) & ~15u, x_t3, xh_t3);  // sq t3, 2832(v1)
  m.sq(((u32)x_v1 + (u32)(2848)) & ~15u, x_t4, xh_t4);  // sq t4, 2848(v1)
  x_a3 = (u64)(s64)m.ld<s32>(sym_font24_table, 0);  // lw a3, *font24-table*(s7)
  // (no-op)  // or a3, a3, r0
  m.lqc2(f_vf13, (u32)x_v1 + (u32)(256));  // lqc2 vf13, 256(v1)
  m.lqc2(f_vf15, (u32)x_v1 + (u32)(336));  // lqc2 vf15, 336(v1)
  m.lqc2(f_vf14, (u32)x_v1 + (u32)(272));  // lqc2 vf14, 272(v1)
  m.sqc2((u32)x_v1 + (u32)(352), f_vf14);  // sqc2 vf14, 352(v1)
  m.lqc2(f_vf14, (u32)x_v1 + (u32)(288));  // lqc2 vf14, 288(v1)
  m.sqc2((u32)x_v1 + (u32)(368), f_vf14);  // sqc2 vf14, 368(v1)
  m.lqc2(f_vf14, (u32)x_v1 + (u32)(304));  // lqc2 vf14, 304(v1)
  m.sqc2((u32)x_v1 + (u32)(384), f_vf14);  // sqc2 vf14, 384(v1)
  m.lqc2(f_vf14, (u32)x_v1 + (u32)(320));  // lqc2 vf14, 320(v1)
  m.sqc2((u32)x_v1 + (u32)(400), f_vf14);  // sqc2 vf14, 400(v1)
  x_t0 = x_t0 | (u64)(32);  // ori t0, t0, 32
  goto block_4;
  block_41:
  x_t2 = (u64)(s64)(s32)(u64(0) + (u64)(s64)(-3));  // addiu t2, r0, -3
  bc = x_t4 == 0;  // beq t4, r0, L37
  x_t0 = x_t0 & x_t2;  // and t0, t0, t2
  if (bc) {goto block_4;}
  x_t0 = x_t0 | (u64)(2);  // ori t0, t0, 2
  goto block_4;
  block_43:
  x_t2 = (u64)(s64)(s32)(u64(0) + (u64)(s64)(-21));  // addiu t2, r0, -21
  x_t3 = x_t4 + (u64)(s64)(-2);  // daddiu t3, t4, -2
  bc = x_t4 == 0;  // beq t4, r0, L37
  x_t0 = x_t0 & x_t2;  // and t0, t0, t2
  if (bc) {goto block_4;}
  bc = x_t3 == 0;  // beq t3, r0, L43
  if (bc) {goto block_46;}
  x_t0 = x_t0 | (u64)(16);  // ori t0, t0, 16
  goto block_4;
  block_46:
  x_t0 = x_t0 | (u64)(4);  // ori t0, t0, 4
  goto block_4;
  block_47:
  vf_from_gpr(f_vf1, x_t4, xh_t4);  // qmtc2.i vf1, t4
  x_t2 = x_t3 + (u64)(s64)(-45);  // daddiu t2, t3, -45
  bc = x_t3 == 0;  // beq t3, r0, L46
  { const Vf s0 = f_vf1; f_vf1.f[0] = (float)get_i(s0, 0); f_vf1.f[1] = (float)get_i(s0, 1); f_vf1.f[2] = (float)get_i(s0, 2); f_vf1.f[3] = (float)get_i(s0, 3); }  // vitof0.xyzw vf1, vf1
  if (bc) {goto block_51;}
  bc = x_t2 == 0;  // beq t2, r0, L45
  if (bc) {goto block_50;}
  { const Vf s0 = f_vf23, s1 = f_vf1; f_vf23.f[0] = s0.f[0] + s1.f[0]; }  // vaddx.x vf23, vf23, vf1
  goto block_4;
  block_50:
  { const Vf s0 = f_vf23, s1 = f_vf1; f_vf23.f[0] = s0.f[0] - s1.f[0]; }  // vsubx.x vf23, vf23, vf1
  goto block_4;
  block_51:
  { const Vf s0 = kVf0, s1 = f_vf1; f_vf23.f[0] = s0.f[0] + s1.f[0]; }  // vaddx.x vf23, vf0, vf1
  goto block_4;
  block_52:
  vf_from_gpr(f_vf1, x_t4, xh_t4);  // qmtc2.i vf1, t4
  x_t2 = x_t3 + (u64)(s64)(-45);  // daddiu t2, t3, -45
  bc = x_t3 == 0;  // beq t3, r0, L49
  { const Vf s0 = f_vf1; f_vf1.f[0] = (float)get_i(s0, 0); f_vf1.f[1] = (float)get_i(s0, 1); f_vf1.f[2] = (float)get_i(s0, 2); f_vf1.f[3] = (float)get_i(s0, 3); }  // vitof0.xyzw vf1, vf1
  if (bc) {goto block_56;}
  bc = x_t2 == 0;  // beq t2, r0, L48
  if (bc) {goto block_55;}
  { const Vf s0 = f_vf23, s1 = f_vf1; f_vf23.f[1] = s0.f[1] + s1.f[0]; }  // vaddx.y vf23, vf23, vf1
  goto block_4;
  block_55:
  { const Vf s0 = f_vf23, s1 = f_vf1; f_vf23.f[1] = s0.f[1] - s1.f[0]; }  // vsubx.y vf23, vf23, vf1
  goto block_4;
  block_56:
  { const Vf s0 = kVf0, s1 = f_vf1; f_vf23.f[1] = s0.f[1] + s1.f[0]; }  // vaddx.y vf23, vf0, vf1
  goto block_4;
  block_57:
  m.sqc2((u32)x_v1 + (u32)(464), f_vf23);  // sqc2 vf23, 464(v1)
  goto block_4;
  block_58:
  m.lqc2(f_vf23, (u32)x_v1 + (u32)(464));  // lqc2 vf23, 464(v1)
  goto block_4;
  block_59:
  x_t3 = x_t2 + (u64)(s64)(-3);  // daddiu t3, t2, -3
  x_t0 = x_t0 | (u64)(64);  // ori t0, t0, 64
  bc = x_t3 == 0;  // beq t3, r0, L54
  x_t2 = x_t2 + (u64)(s64)(-2);  // daddiu t2, t2, -2
  if (bc) {goto block_63;}
  bc = x_t2 == 0;  // beq t2, r0, L53
  m.lqc2(f_vf14, (u32)x_v1 + (u32)(384));  // lqc2 vf14, 384(v1)
  if (bc) {goto block_62;}
  m.lqc2(f_vf14, (u32)x_v1 + (u32)(368));  // lqc2 vf14, 368(v1)
  m.lqc2(f_vf20, (u32)x_v1 + (u32)(2816));  // lqc2 vf20, 2816(v1)
  goto block_64;
  block_62:
  m.lqc2(f_vf20, (u32)x_v1 + (u32)(2832));  // lqc2 vf20, 2832(v1)
  goto block_64;
  block_63:
  m.lqc2(f_vf14, (u32)x_v1 + (u32)(400));  // lqc2 vf14, 400(v1)
  m.lqc2(f_vf20, (u32)x_v1 + (u32)(2848));  // lqc2 vf20, 2848(v1)
  block_64:
  x_t2 = (u64)m.ld<u8>((u32)x_a0, 4);  // lbu t2, 4(a0)
  x_a0 = x_a0 + (u64)(s64)(1);  // daddiu a0, a0, 1
  { const Vf s0 = f_vf23, s1 = f_vf15; f_vf4.f[0] = s0.f[0] + s1.f[0]; f_vf4.f[1] = s0.f[1] + s1.f[1]; f_vf4.f[2] = s0.f[2] + s1.f[2]; }  // vadd.xyz vf4, vf23, vf15
  x_t3 = (u64)(s64)(s32)((u32)x_t2 << 4);  // sll t3, t2, 4
  { const Vf s0 = f_vf25, s1 = f_vf23; f_vf1.f[0] = s0.f[0] - s1.f[0]; f_vf1.f[1] = s0.f[1] - s1.f[1]; f_vf1.f[2] = s0.f[2] - s1.f[2]; f_vf1.f[3] = s0.f[3] - s1.f[3]; }  // vsub.xyzw vf1, vf25, vf23
  goto block_72;
  block_65:
  x_t3 = (u64)(s64)(s32)(u64(0) + (u64)(s64)(-65));  // addiu t3, r0, -65
  x_t0 = x_t0 & x_t3;  // and t0, t0, t3
  m.lqc2(f_vf14, (u32)x_v1 + (u32)(352));  // lqc2 vf14, 352(v1)
  { const Vf s0 = f_vf23, s1 = f_vf15; f_vf4.f[0] = s0.f[0] + s1.f[0]; f_vf4.f[1] = s0.f[1] + s1.f[1]; f_vf4.f[2] = s0.f[2] + s1.f[2]; }  // vadd.xyz vf4, vf23, vf15
  x_t3 = (u64)(s64)(s32)((u32)x_t2 << 4);  // sll t3, t2, 4
  { const Vf s0 = f_vf25, s1 = f_vf23; f_vf1.f[0] = s0.f[0] - s1.f[0]; f_vf1.f[1] = s0.f[1] - s1.f[1]; f_vf1.f[2] = s0.f[2] - s1.f[2]; f_vf1.f[3] = s0.f[3] - s1.f[3]; }  // vsub.xyzw vf1, vf25, vf23
  x_t4 = x_t2 + (u64)(s64)(-10);  // daddiu t4, t2, -10
  bc = x_t4 == 0;  // beq t4, r0, L57
  x_t2 = x_t2 + (u64)(s64)(-13);  // daddiu t2, t2, -13
  if (bc) {goto block_67;}
  bc = x_t2 != 0;  // bne t2, r0, L60
  if (bc) {goto block_72;}
  block_67:
  { const Vf s0 = f_vf23, s1 = f_vf24; f_vf1.f[0] = s0.f[0] - s1.f[0]; f_vf1.f[1] = s0.f[1] - s1.f[1]; f_vf1.f[2] = s0.f[2] - s1.f[2]; f_vf1.f[3] = s0.f[3] - s1.f[3]; }  // vsub.xyzw vf1, vf23, vf24
  x_t2 = x_t0 & (u64)(16);  // andi t2, t0, 16
  bc = x_t2 != 0;  // bne t2, r0, L58
  x_t2 = x_t0 & (u64)(4);  // andi t2, t0, 4
  if (bc) {goto block_70;}
  bc = x_t2 != 0;  // bne t2, r0, L59
  if (bc) {goto block_71;}
  { const Vf s0 = kVf0, s1 = f_vf24; f_vf23.f[0] = s0.f[0] + s1.f[0]; }  // vaddx.x vf23, vf0, vf24
  m.sqc2((u32)x_t1 + (u32)(1120), f_vf23);  // sqc2 vf23, 1120(t1)
  { const Vf s0 = f_vf23, s1 = f_vf15; f_vf23.f[1] = s0.f[1] + s1.f[3]; }  // vaddw.y vf23, vf23, vf15
  x_t1 = x_t1 + (u64)(s64)(16);  // daddiu t1, t1, 16
  goto block_4;
  block_70:
  { const Vf s0 = f_vf24, s1 = f_vf1; f_vf23.f[0] = s0.f[0] - s1.f[0]; }  // vsub.x vf23, vf24, vf1
  m.sqc2((u32)x_t1 + (u32)(1120), f_vf23);  // sqc2 vf23, 1120(t1)
  { const Vf s0 = kVf0, s1 = f_vf24; f_vf23.f[0] = s0.f[0] + s1.f[0]; }  // vaddx.x vf23, vf0, vf24
  { const Vf s0 = f_vf23, s1 = f_vf15; f_vf23.f[1] = s0.f[1] + s1.f[3]; }  // vaddw.y vf23, vf23, vf15
  x_t1 = x_t1 + (u64)(s64)(16);  // daddiu t1, t1, 16
  goto block_4;
  block_71:
  { const Vf s0 = f_vf1, s1 = f_vf16; f_vf1.f[0] = s0.f[0] * s1.f[3]; }  // vmulw.x vf1, vf1, vf16
  { const Vf s0 = f_vf24, s1 = f_vf1; f_vf23.f[0] = s0.f[0] - s1.f[0]; }  // vsub.x vf23, vf24, vf1
  m.sqc2((u32)x_t1 + (u32)(1120), f_vf23);  // sqc2 vf23, 1120(t1)
  { const Vf s0 = kVf0, s1 = f_vf24; f_vf23.f[0] = s0.f[0] + s1.f[0]; }  // vaddx.x vf23, vf0, vf24
  { const Vf s0 = f_vf23, s1 = f_vf15; f_vf23.f[1] = s0.f[1] + s1.f[3]; }  // vaddw.y vf23, vf23, vf15
  x_t1 = x_t1 + (u64)(s64)(16);  // daddiu t1, t1, 16
  goto block_4;
  block_72:
  x_t2 = (u64)(s64)(s32)(x_t3 + x_a3);  // addu t2, t3, a3
  m.lqc2(f_vf5, (u32)x_t2 + (u32)(-96));  // lqc2 vf5, -96(t2)
  gpr_from_vf(x_t2, xh_t2, f_vf1);  // qmfc2.i t2, vf1
  bc = ((s64)x_t2) < 0;  // bltz t2, L63
  x_t2 = (u64)(s64)((s32)x_t2 >> 31);  // sra t2, t2, 31
  if (bc) {goto block_78;}
  { const Vf s0 = f_vf5, s1 = f_vf13; f_vf19.f[0] = s0.f[0] * s1.f[0]; f_vf19.f[1] = s0.f[1] * s1.f[1]; f_vf19.f[2] = s0.f[2] * s1.f[2]; f_vf19.f[3] = s0.f[3] * s1.f[3]; }  // vmul.xyzw vf19, vf5, vf13
  x_t2 = x_t0 & (u64)(2);  // andi t2, t0, 2
  bc = x_t2 == 0;  // beq t2, r0, L61
  x_t2 = x_t0 & (u64)(64);  // andi t2, t0, 64
  if (bc) {goto block_76;}
  bc = x_t2 != 0;  // bne t2, r0, L61
  if (bc) {goto block_76;}
  { const Vf s0 = f_vf23, s1 = f_vf19; f_vf23.f[0] = s0.f[0] + s1.f[3]; }  // vaddw.x vf23, vf23, vf19
  goto block_77;
  block_76:
  { const Vf s0 = f_vf23, s1 = f_vf14; f_vf23.f[0] = s0.f[0] + s1.f[3]; }  // vaddw.x vf23, vf23, vf14
  block_77:
  goto block_4;
  block_78:
  { const Vf s0 = f_vf23, s1 = f_vf24; f_vf1.f[0] = s0.f[0] - s1.f[0]; f_vf1.f[1] = s0.f[1] - s1.f[1]; f_vf1.f[2] = s0.f[2] - s1.f[2]; f_vf1.f[3] = s0.f[3] - s1.f[3]; }  // vsub.xyzw vf1, vf23, vf24
  x_a0 = x_t0 & (u64)(16);  // andi a0, t0, 16
  bc = x_a0 != 0;  // bne a0, r0, L64
  x_a0 = x_t0 & (u64)(4);  // andi a0, t0, 4
  if (bc) {goto block_81;}
  bc = x_a0 != 0;  // bne a0, r0, L65
  if (bc) {goto block_82;}
  { const Vf s0 = kVf0, s1 = f_vf24; f_vf23.f[0] = s0.f[0] + s1.f[0]; }  // vaddx.x vf23, vf0, vf24
  m.sqc2((u32)x_t1 + (u32)(1120), f_vf23);  // sqc2 vf23, 1120(t1)
  goto block_83;
  block_81:
  { const Vf s0 = f_vf24, s1 = f_vf1; f_vf23.f[0] = s0.f[0] - s1.f[0]; }  // vsub.x vf23, vf24, vf1
  m.sqc2((u32)x_t1 + (u32)(1120), f_vf23);  // sqc2 vf23, 1120(t1)
  goto block_83;
  block_82:
  { const Vf s0 = f_vf1, s1 = f_vf16; f_vf1.f[0] = s0.f[0] * s1.f[3]; }  // vmulw.x vf1, vf1, vf16
  { const Vf s0 = f_vf24, s1 = f_vf1; f_vf23.f[0] = s0.f[0] - s1.f[0]; }  // vsub.x vf23, vf24, vf1
  m.sqc2((u32)x_t1 + (u32)(1120), f_vf23);  // sqc2 vf23, 1120(t1)
  block_83:
  x_a0 = (u64)(s64)m.ld<s32>((u32)x_v1, 2880);  // lw a0, 2880(v1)
  x_t0 = x_v1;  // or t0, v1, r0
  x_t1 = (u64)(s64)m.ld<s32>((u32)x_v1, 2876);  // lw t1, 2876(v1)
  m.lqc2(f_vf23, (u32)x_t0 + (u32)(1120));  // lqc2 vf23, 1120(t0)
  block_84:
  x_t2 = (u64)m.ld<u8>((u32)x_t1, 4);  // lbu t2, 4(t1)
  x_t1 = x_t1 + (u64)(s64)(1);  // daddiu t1, t1, 1
  m.lqc2(f_vf20, (u32)x_v1 + (u32)(2800));  // lqc2 vf20, 2800(v1)
  bc = x_t2 == 0;  // beq t2, r0, L90
  x_t3 = x_t2 + (u64)(s64)(-3);  // daddiu t3, t2, -3
  if (bc) {goto block_158;}
  bc = ((s64)x_t3) <= 0;  // blez t3, L81
  x_t3 = x_t2 + (u64)(s64)(-126);  // daddiu t3, t2, -126
  if (bc) {goto block_137;}
  bc = x_t3 != 0;  // bne t3, r0, L85
  if (bc) {goto block_143;}
  x_t2 = (u64)m.ld<u8>((u32)x_t1, 4);  // lbu t2, 4(t1)
  x_t1 = x_t1 + (u64)(s64)(1);  // daddiu t1, t1, 1
  x_t3 = (u64)(s64)(s32)(u64(0) + (u64)(s64)(0));  // addiu t3, r0, 0
  x_t4 = (u64)(s64)(s32)(u64(0) + (u64)(s64)(0));  // addiu t4, r0, 0
  bc = x_t2 == 0;  // beq t2, r0, L90
  x_t5 = x_t2 + (u64)(s64)(-43);  // daddiu t5, t2, -43
  if (bc) {goto block_158;}
  if (x_t5 == 0) { x_t3 = x_t2; }  // movz t3, t2, t5
  x_t5 = x_t2 + (u64)(s64)(-45);  // daddiu t5, t2, -45
  if (x_t5 == 0) { x_t3 = x_t2; }  // movz t3, t2, t5
  bc = x_t3 != 0;  // bne t3, r0, L68
  x_t5 = x_t2 + (u64)(s64)(-91);  // daddiu t5, t2, -91
  if (bc) {goto block_98;}
  bc = x_t5 == 0;  // beq t5, r0, L67
  x_t4 = x_t2 + (u64)(s64)(-93);  // daddiu t4, t2, -93
  if (bc) {goto block_84;}
  bc = x_t4 == 0;  // beq t4, r0, L67
  x_t4 = x_t2 + (u64)(s64)(-121);  // daddiu t4, t2, -121
  if (bc) {goto block_84;}
  bc = x_t4 == 0;  // beq t4, r0, L79
  x_t4 = x_t2 + (u64)(s64)(-89);  // daddiu t4, t2, -89
  if (bc) {goto block_135;}
  bc = x_t4 == 0;  // beq t4, r0, L79
  x_t4 = x_t2 + (u64)(s64)(-122);  // daddiu t4, t2, -122
  if (bc) {goto block_135;}
  bc = x_t4 == 0;  // beq t4, r0, L80
  x_t4 = x_t2 + (u64)(s64)(-90);  // daddiu t4, t2, -90
  if (bc) {goto block_136;}
  bc = x_t4 == 0;  // beq t4, r0, L80
  x_t4 = x_t2 + (u64)(s64)(-48);  // daddiu t4, t2, -48
  if (bc) {goto block_136;}
  bc = ((s64)x_t4) < 0;  // bltz t4, L85
  x_t4 = x_t2 + (u64)(s64)(-57);  // daddiu t4, t2, -57
  if (bc) {goto block_143;}
  bc = ((s64)x_t4) > 0;  // bgtz t4, L85
  x_t4 = x_t2 + (u64)(s64)(-126);  // daddiu t4, t2, -126
  if (bc) {goto block_143;}
  bc = x_t4 == 0;  // beq t4, r0, L85
  x_t4 = x_t2 + (u64)(s64)(-48);  // daddiu t4, t2, -48
  if (bc) {goto block_143;}
  block_98:
  x_t2 = (u64)m.ld<u8>((u32)x_t1, 4);  // lbu t2, 4(t1)
  x_t1 = x_t1 + (u64)(s64)(1);  // daddiu t1, t1, 1
  bc = x_t2 == 0;  // beq t2, r0, L90
  x_t5 = x_t2 + (u64)(s64)(-110);  // daddiu t5, t2, -110
  if (bc) {goto block_158;}
  bc = x_t5 == 0;  // beq t5, r0, L69
  x_t5 = x_t2 + (u64)(s64)(-78);  // daddiu t5, t2, -78
  if (bc) {goto block_118;}
  bc = x_t5 == 0;  // beq t5, r0, L69
  x_t5 = x_t2 + (u64)(s64)(-108);  // daddiu t5, t2, -108
  if (bc) {goto block_118;}
  bc = x_t5 == 0;  // beq t5, r0, L67
  x_t5 = x_t2 + (u64)(s64)(-76);  // daddiu t5, t2, -76
  if (bc) {goto block_84;}
  bc = x_t5 == 0;  // beq t5, r0, L67
  x_t5 = x_t2 + (u64)(s64)(-119);  // daddiu t5, t2, -119
  if (bc) {goto block_84;}
  bc = x_t5 == 0;  // beq t5, r0, L71
  x_t5 = x_t2 + (u64)(s64)(-87);  // daddiu t5, t2, -87
  if (bc) {goto block_121;}
  bc = x_t5 == 0;  // beq t5, r0, L71
  x_t5 = x_t2 + (u64)(s64)(-107);  // daddiu t5, t2, -107
  if (bc) {goto block_121;}
  bc = x_t5 == 0;  // beq t5, r0, L72
  x_t5 = x_t2 + (u64)(s64)(-75);  // daddiu t5, t2, -75
  if (bc) {goto block_123;}
  bc = x_t5 == 0;  // beq t5, r0, L72
  x_t5 = x_t2 + (u64)(s64)(-106);  // daddiu t5, t2, -106
  if (bc) {goto block_123;}
  bc = x_t5 == 0;  // beq t5, r0, L67
  x_t5 = x_t2 + (u64)(s64)(-74);  // daddiu t5, t2, -74
  if (bc) {goto block_84;}
  bc = x_t5 == 0;  // beq t5, r0, L67
  x_t5 = x_t2 + (u64)(s64)(-104);  // daddiu t5, t2, -104
  if (bc) {goto block_84;}
  bc = x_t5 == 0;  // beq t5, r0, L73
  x_t5 = x_t2 + (u64)(s64)(-72);  // daddiu t5, t2, -72
  if (bc) {goto block_125;}
  bc = x_t5 == 0;  // beq t5, r0, L73
  x_t5 = x_t2 + (u64)(s64)(-118);  // daddiu t5, t2, -118
  if (bc) {goto block_125;}
  bc = x_t5 == 0;  // beq t5, r0, L76
  x_t5 = x_t2 + (u64)(s64)(-86);  // daddiu t5, t2, -86
  if (bc) {goto block_130;}
  bc = x_t5 == 0;  // beq t5, r0, L76
  x_t5 = x_t2 + (u64)(s64)(-117);  // daddiu t5, t2, -117
  if (bc) {goto block_130;}
  bc = x_t5 == 0;  // beq t5, r0, L67
  x_t5 = x_t2 + (u64)(s64)(-85);  // daddiu t5, t2, -85
  if (bc) {goto block_84;}
  bc = x_t5 == 0;  // beq t5, r0, L67
  x_t5 = x_t2 + (u64)(s64)(-48);  // daddiu t5, t2, -48
  if (bc) {goto block_84;}
  bc = ((s64)x_t5) < 0;  // bltz t5, L85
  x_t6 = x_t2 + (u64)(s64)(-57);  // daddiu t6, t2, -57
  if (bc) {goto block_143;}
  bc = ((s64)x_t6) > 0;  // bgtz t6, L85
  x_t6 = (u64)(s64)(s32)((u32)x_t4 << 2);  // sll t6, t4, 2
  if (bc) {goto block_143;}
  x_t2 = x_t4 + x_t6;  // daddu t2, t4, t6
  x_t2 = (u64)(s64)(s32)((u32)x_t2 << 1);  // sll t2, t2, 1
  x_t4 = x_t2 + x_t5;  // daddu t4, t2, t5
  goto block_98;
  block_118:
  bc = x_t4 != 0;  // bne t4, r0, L70
  x_t2 = (u64)(s64)(s32)(u64(0) + (u64)(s64)(-33));  // addiu t2, r0, -33
  if (bc) {goto block_120;}
  m.lq(x_a3, xh_a3, ((u32)x_v1 + (u32)(80)) & ~15u);  // lq a3, 80(v1)
  m.lq(x_t3, xh_t3, ((u32)x_v1 + (u32)(96)) & ~15u);  // lq t3, 96(v1)
  m.lq(x_t4, xh_t4, ((u32)x_v1 + (u32)(112)) & ~15u);  // lq t4, 112(v1)
  m.lq(x_t5, xh_t5, ((u32)x_v1 + (u32)(128)) & ~15u);  // lq t5, 128(v1)
  m.sq(((u32)x_v1 + (u32)(2800)) & ~15u, x_a3, xh_a3);  // sq a3, 2800(v1)
  m.sq(((u32)x_v1 + (u32)(2816)) & ~15u, x_t3, xh_t3);  // sq t3, 2816(v1)
  m.sq(((u32)x_v1 + (u32)(2832)) & ~15u, x_t4, xh_t4);  // sq t4, 2832(v1)
  m.sq(((u32)x_v1 + (u32)(2848)) & ~15u, x_t5, xh_t5);  // sq t5, 2848(v1)
  x_a3 = (u64)(s64)m.ld<s32>(sym_font12_table, 0);  // lw a3, *font12-table*(s7)
  // (no-op)  // or a3, a3, r0
  m.lqc2(f_vf13, (u32)x_v1 + (u32)(208));  // lqc2 vf13, 208(v1)
  m.lqc2(f_vf14, (u32)x_v1 + (u32)(224));  // lqc2 vf14, 224(v1)
  m.lqc2(f_vf15, (u32)x_v1 + (u32)(240));  // lqc2 vf15, 240(v1)
  m.sqc2((u32)x_v1 + (u32)(352), f_vf14);  // sqc2 vf14, 352(v1)
  m.sqc2((u32)x_v1 + (u32)(368), f_vf14);  // sqc2 vf14, 368(v1)
  m.sqc2((u32)x_v1 + (u32)(384), f_vf14);  // sqc2 vf14, 384(v1)
  m.sqc2((u32)x_v1 + (u32)(400), f_vf14);  // sqc2 vf14, 400(v1)
  x_a0 = x_a0 & x_t2;  // and a0, a0, t2
  goto block_84;
  block_120:
  m.lq(x_a3, xh_a3, ((u32)x_v1 + (u32)(144)) & ~15u);  // lq a3, 144(v1)
  m.lq(x_t2, xh_t2, ((u32)x_v1 + (u32)(160)) & ~15u);  // lq t2, 160(v1)
  m.lq(x_t3, xh_t3, ((u32)x_v1 + (u32)(176)) & ~15u);  // lq t3, 176(v1)
  m.lq(x_t4, xh_t4, ((u32)x_v1 + (u32)(192)) & ~15u);  // lq t4, 192(v1)
  m.sq(((u32)x_v1 + (u32)(2800)) & ~15u, x_a3, xh_a3);  // sq a3, 2800(v1)
  m.sq(((u32)x_v1 + (u32)(2816)) & ~15u, x_t2, xh_t2);  // sq t2, 2816(v1)
  m.sq(((u32)x_v1 + (u32)(2832)) & ~15u, x_t3, xh_t3);  // sq t3, 2832(v1)
  m.sq(((u32)x_v1 + (u32)(2848)) & ~15u, x_t4, xh_t4);  // sq t4, 2848(v1)
  x_a3 = (u64)(s64)m.ld<s32>(sym_font24_table, 0);  // lw a3, *font24-table*(s7)
  // (no-op)  // or a3, a3, r0
  m.lqc2(f_vf13, (u32)x_v1 + (u32)(256));  // lqc2 vf13, 256(v1)
  m.lqc2(f_vf15, (u32)x_v1 + (u32)(336));  // lqc2 vf15, 336(v1)
  m.lqc2(f_vf14, (u32)x_v1 + (u32)(272));  // lqc2 vf14, 272(v1)
  m.sqc2((u32)x_v1 + (u32)(352), f_vf14);  // sqc2 vf14, 352(v1)
  m.lqc2(f_vf14, (u32)x_v1 + (u32)(288));  // lqc2 vf14, 288(v1)
  m.sqc2((u32)x_v1 + (u32)(368), f_vf14);  // sqc2 vf14, 368(v1)
  m.lqc2(f_vf14, (u32)x_v1 + (u32)(304));  // lqc2 vf14, 304(v1)
  m.sqc2((u32)x_v1 + (u32)(384), f_vf14);  // sqc2 vf14, 384(v1)
  m.lqc2(f_vf14, (u32)x_v1 + (u32)(320));  // lqc2 vf14, 320(v1)
  m.sqc2((u32)x_v1 + (u32)(400), f_vf14);  // sqc2 vf14, 400(v1)
  x_a0 = x_a0 | (u64)(32);  // ori a0, a0, 32
  goto block_84;
  block_121:
  x_t2 = (u64)(s64)(s32)(u64(0) + (u64)(s64)(-2));  // addiu t2, r0, -2
  bc = x_t4 == 0;  // beq t4, r0, L67
  x_a0 = x_a0 & x_t2;  // and a0, a0, t2
  if (bc) {goto block_84;}
  x_a0 = x_a0 | (u64)(1);  // ori a0, a0, 1
  goto block_84;
  block_123:
  x_t2 = (u64)(s64)(s32)(u64(0) + (u64)(s64)(-3));  // addiu t2, r0, -3
  bc = x_t4 == 0;  // beq t4, r0, L67
  x_a0 = x_a0 & x_t2;  // and a0, a0, t2
  if (bc) {goto block_84;}
  x_a0 = x_a0 | (u64)(2);  // ori a0, a0, 2
  goto block_84;
  block_125:
  vf_from_gpr(f_vf1, x_t4, xh_t4);  // qmtc2.i vf1, t4
  x_t2 = x_t3 + (u64)(s64)(-45);  // daddiu t2, t3, -45
  bc = x_t3 == 0;  // beq t3, r0, L75
  { const Vf s0 = f_vf1; f_vf1.f[0] = (float)get_i(s0, 0); f_vf1.f[1] = (float)get_i(s0, 1); f_vf1.f[2] = (float)get_i(s0, 2); f_vf1.f[3] = (float)get_i(s0, 3); }  // vitof0.xyzw vf1, vf1
  if (bc) {goto block_129;}
  bc = x_t2 == 0;  // beq t2, r0, L74
  if (bc) {goto block_128;}
  { const Vf s0 = f_vf23, s1 = f_vf1; f_vf23.f[0] = s0.f[0] + s1.f[0]; }  // vaddx.x vf23, vf23, vf1
  goto block_84;
  block_128:
  { const Vf s0 = f_vf23, s1 = f_vf1; f_vf23.f[0] = s0.f[0] - s1.f[0]; }  // vsubx.x vf23, vf23, vf1
  goto block_84;
  block_129:
  { const Vf s0 = kVf0, s1 = f_vf1; f_vf23.f[0] = s0.f[0] + s1.f[0]; }  // vaddx.x vf23, vf0, vf1
  goto block_84;
  block_130:
  vf_from_gpr(f_vf1, x_t4, xh_t4);  // qmtc2.i vf1, t4
  x_t2 = x_t3 + (u64)(s64)(-45);  // daddiu t2, t3, -45
  bc = x_t3 == 0;  // beq t3, r0, L78
  { const Vf s0 = f_vf1; f_vf1.f[0] = (float)get_i(s0, 0); f_vf1.f[1] = (float)get_i(s0, 1); f_vf1.f[2] = (float)get_i(s0, 2); f_vf1.f[3] = (float)get_i(s0, 3); }  // vitof0.xyzw vf1, vf1
  if (bc) {goto block_134;}
  bc = x_t2 == 0;  // beq t2, r0, L77
  if (bc) {goto block_133;}
  { const Vf s0 = f_vf23, s1 = f_vf1; f_vf23.f[1] = s0.f[1] + s1.f[0]; }  // vaddx.y vf23, vf23, vf1
  goto block_84;
  block_133:
  { const Vf s0 = f_vf23, s1 = f_vf1; f_vf23.f[1] = s0.f[1] - s1.f[0]; }  // vsubx.y vf23, vf23, vf1
  goto block_84;
  block_134:
  { const Vf s0 = kVf0, s1 = f_vf1; f_vf23.f[1] = s0.f[1] + s1.f[0]; }  // vaddx.y vf23, vf0, vf1
  goto block_84;
  block_135:
  m.sqc2((u32)x_v1 + (u32)(464), f_vf23);  // sqc2 vf23, 464(v1)
  goto block_84;
  block_136:
  m.lqc2(f_vf23, (u32)x_v1 + (u32)(464));  // lqc2 vf23, 464(v1)
  goto block_84;
  block_137:
  x_t3 = x_t2 + (u64)(s64)(-3);  // daddiu t3, t2, -3
  x_a0 = x_a0 | (u64)(64);  // ori a0, a0, 64
  bc = x_t3 == 0;  // beq t3, r0, L83
  x_t2 = x_t2 + (u64)(s64)(-2);  // daddiu t2, t2, -2
  if (bc) {goto block_141;}
  bc = x_t2 == 0;  // beq t2, r0, L82
  m.lqc2(f_vf14, (u32)x_v1 + (u32)(384));  // lqc2 vf14, 384(v1)
  if (bc) {goto block_140;}
  m.lqc2(f_vf14, (u32)x_v1 + (u32)(368));  // lqc2 vf14, 368(v1)
  m.lqc2(f_vf20, (u32)x_v1 + (u32)(2816));  // lqc2 vf20, 2816(v1)
  goto block_142;
  block_140:
  m.lqc2(f_vf20, (u32)x_v1 + (u32)(2832));  // lqc2 vf20, 2832(v1)
  goto block_142;
  block_141:
  m.lqc2(f_vf14, (u32)x_v1 + (u32)(400));  // lqc2 vf14, 400(v1)
  m.lqc2(f_vf20, (u32)x_v1 + (u32)(2848));  // lqc2 vf20, 2848(v1)
  block_142:
  x_t2 = (u64)m.ld<u8>((u32)x_t1, 4);  // lbu t2, 4(t1)
  x_t1 = x_t1 + (u64)(s64)(1);  // daddiu t1, t1, 1
  { const Vf s0 = f_vf23, s1 = f_vf15; f_vf4.f[0] = s0.f[0] + s1.f[0]; f_vf4.f[1] = s0.f[1] + s1.f[1]; f_vf4.f[2] = s0.f[2] + s1.f[2]; }  // vadd.xyz vf4, vf23, vf15
  x_t3 = (u64)(s64)(s32)((u32)x_t2 << 4);  // sll t3, t2, 4
  { const Vf s0 = f_vf25, s1 = f_vf23; f_vf1.f[0] = s0.f[0] - s1.f[0]; f_vf1.f[1] = s0.f[1] - s1.f[1]; f_vf1.f[2] = s0.f[2] - s1.f[2]; f_vf1.f[3] = s0.f[3] - s1.f[3]; }  // vsub.xyzw vf1, vf25, vf23
  goto block_146;
  block_143:
  x_t3 = (u64)(s64)(s32)(u64(0) + (u64)(s64)(-65));  // addiu t3, r0, -65
  x_a0 = x_a0 & x_t3;  // and a0, a0, t3
  m.lqc2(f_vf14, (u32)x_v1 + (u32)(352));  // lqc2 vf14, 352(v1)
  { const Vf s0 = f_vf23, s1 = f_vf15; f_vf4.f[0] = s0.f[0] + s1.f[0]; f_vf4.f[1] = s0.f[1] + s1.f[1]; f_vf4.f[2] = s0.f[2] + s1.f[2]; }  // vadd.xyz vf4, vf23, vf15
  x_t3 = (u64)(s64)(s32)((u32)x_t2 << 4);  // sll t3, t2, 4
  { const Vf s0 = f_vf25, s1 = f_vf23; f_vf1.f[0] = s0.f[0] - s1.f[0]; f_vf1.f[1] = s0.f[1] - s1.f[1]; f_vf1.f[2] = s0.f[2] - s1.f[2]; f_vf1.f[3] = s0.f[3] - s1.f[3]; }  // vsub.xyzw vf1, vf25, vf23
  x_t4 = x_t2 + (u64)(s64)(-10);  // daddiu t4, t2, -10
  bc = x_t4 == 0;  // beq t4, r0, L86
  x_t2 = x_t2 + (u64)(s64)(-13);  // daddiu t2, t2, -13
  if (bc) {goto block_145;}
  bc = x_t2 != 0;  // bne t2, r0, L87
  if (bc) {goto block_146;}
  block_145:
  x_t0 = x_t0 + (u64)(s64)(16);  // daddiu t0, t0, 16
  m.lqc2(f_vf23, (u32)x_t0 + (u32)(1120));  // lqc2 vf23, 1120(t0)
  goto block_84;
  block_146:
  x_t2 = (u64)(s64)(s32)(x_t3 + x_a3);  // addu t2, t3, a3
  m.lqc2(f_vf5, (u32)x_t2 + (u32)(-96));  // lqc2 vf5, -96(t2)
  gpr_from_vf(x_t2, xh_t2, f_vf1);  // qmfc2.i t2, vf1
  bc = ((s64)x_t2) < 0;  // bltz t2, L90
  { const Vf s0 = f_vf5, s1 = f_vf16; f_vf6.f[0] = s0.f[0] + s1.f[0]; f_vf6.f[1] = s0.f[1] + s1.f[1]; f_vf6.f[2] = s0.f[2] + s1.f[2]; }  // vadd.xyz vf6, vf5, vf16
  if (bc) {goto block_158;}
  x_t2 = (u64)(s64)((s32)x_t2 >> 31);  // sra t2, t2, 31
  { const Vf s0 = f_vf5, s1 = f_vf17; f_vf7.f[0] = s0.f[0] + s1.f[0]; f_vf7.f[1] = s0.f[1] + s1.f[1]; f_vf7.f[2] = s0.f[2] + s1.f[2]; }  // vadd.xyz vf7, vf5, vf17
  bc = ((s64)x_t2) < 0;  // bltz t2, L67
  { const Vf s0 = f_vf5, s1 = f_vf18; f_vf8.f[0] = s0.f[0] + s1.f[0]; f_vf8.f[1] = s0.f[1] + s1.f[1]; f_vf8.f[2] = s0.f[2] + s1.f[2]; }  // vadd.xyz vf8, vf5, vf18
  if (bc) {goto block_84;}
  { const Vf s0 = f_vf23, s1 = kVf0; f_vf1.f[0] = s0.f[0] + s1.f[0]; f_vf1.f[1] = s0.f[1] + s1.f[1]; f_vf1.f[2] = s0.f[2] + s1.f[2]; }  // vadd.xyz vf1, vf23, vf0
  x_t2 = x_a0 & (u64)(1);  // andi t2, a0, 1
  { const Vf s0 = f_vf23, s1 = f_vf13; f_vf2.f[0] = s0.f[0] + s1.f[0]; f_vf2.f[1] = s0.f[1] + s1.f[1]; f_vf2.f[2] = s0.f[2] + s1.f[2]; }  // vadd.xyz vf2, vf23, vf13
  m.sqc2((u32)x_v1 + (u32)(672), f_vf5);  // sqc2 vf5, 672(v1)
  { const Vf s0 = f_vf23, s1 = f_vf14; f_vf3.f[0] = s0.f[0] + s1.f[0]; f_vf3.f[1] = s0.f[1] + s1.f[1]; f_vf3.f[2] = s0.f[2] + s1.f[2]; }  // vadd.xyz vf3, vf23, vf14
  m.sqc2((u32)x_v1 + (u32)(688), f_vf6);  // sqc2 vf6, 688(v1)
  { const Vf s0 = f_vf5, s1 = f_vf13; f_vf19.f[0] = s0.f[0] * s1.f[0]; f_vf19.f[1] = s0.f[1] * s1.f[1]; f_vf19.f[2] = s0.f[2] * s1.f[2]; f_vf19.f[3] = s0.f[3] * s1.f[3]; }  // vmul.xyzw vf19, vf5, vf13
  m.sqc2((u32)x_v1 + (u32)(704), f_vf7);  // sqc2 vf7, 704(v1)
  bc = x_t2 == 0;  // beq t2, r0, L67
  m.sqc2((u32)x_v1 + (u32)(720), f_vf8);  // sqc2 vf8, 720(v1)
  if (bc) {goto block_84;}
  m.sqc2((u32)x_v1 + (u32)(544), f_vf1);  // sqc2 vf1, 544(v1)
  m.sqc2((u32)x_v1 + (u32)(560), f_vf2);  // sqc2 vf2, 560(v1)
  m.sqc2((u32)x_v1 + (u32)(576), f_vf3);  // sqc2 vf3, 576(v1)
  m.sqc2((u32)x_v1 + (u32)(592), f_vf4);  // sqc2 vf4, 592(v1)
  m.lqc2(f_vf1, (u32)x_v1 + (u32)(544));  // lqc2 vf1, 544(v1)
  m.lqc2(f_vf2, (u32)x_v1 + (u32)(560));  // lqc2 vf2, 560(v1)
  { const Vf s0 = f_vf31, s1 = kVf0; acc.f[0] = s0.f[0] * s1.f[3]; acc.f[1] = s0.f[1] * s1.f[3]; acc.f[2] = s0.f[2] * s1.f[3]; acc.f[3] = s0.f[3] * s1.f[3]; }  // vmulaw.xyzw acc, vf31, vf0
  m.lqc2(f_vf3, (u32)x_v1 + (u32)(576));  // lqc2 vf3, 576(v1)
  { const Vf s0 = f_vf28, s1 = f_vf1; acc.f[0] += s0.f[0] * s1.f[0]; acc.f[1] += s0.f[1] * s1.f[0]; acc.f[2] += s0.f[2] * s1.f[0]; acc.f[3] += s0.f[3] * s1.f[0]; }  // vmaddax.xyzw acc, vf28, vf1
  m.lqc2(f_vf4, (u32)x_v1 + (u32)(592));  // lqc2 vf4, 592(v1)
  { const Vf s0 = f_vf29, s1 = f_vf1; acc.f[0] += s0.f[0] * s1.f[1]; acc.f[1] += s0.f[1] * s1.f[1]; acc.f[2] += s0.f[2] * s1.f[1]; acc.f[3] += s0.f[3] * s1.f[1]; }  // vmadday.xyzw acc, vf29, vf1
  { const Vf s0 = f_vf30, s1 = f_vf1; f_vf1.f[0] = acc.f[0] + s0.f[0] * s1.f[2]; f_vf1.f[1] = acc.f[1] + s0.f[1] * s1.f[2]; f_vf1.f[2] = acc.f[2] + s0.f[2] * s1.f[2]; f_vf1.f[3] = acc.f[3] + s0.f[3] * s1.f[2]; }  // vmaddz.xyzw vf1, vf30, vf1
  { const Vf s0 = f_vf31, s1 = kVf0; acc.f[0] = s0.f[0] * s1.f[3]; acc.f[1] = s0.f[1] * s1.f[3]; acc.f[2] = s0.f[2] * s1.f[3]; acc.f[3] = s0.f[3] * s1.f[3]; }  // vmulaw.xyzw acc, vf31, vf0
  { const Vf s0 = f_vf28, s1 = f_vf2; acc.f[0] += s0.f[0] * s1.f[0]; acc.f[1] += s0.f[1] * s1.f[0]; acc.f[2] += s0.f[2] * s1.f[0]; acc.f[3] += s0.f[3] * s1.f[0]; }  // vmaddax.xyzw acc, vf28, vf2
  { const Vf s0 = f_vf29, s1 = f_vf2; acc.f[0] += s0.f[0] * s1.f[1]; acc.f[1] += s0.f[1] * s1.f[1]; acc.f[2] += s0.f[2] * s1.f[1]; acc.f[3] += s0.f[3] * s1.f[1]; }  // vmadday.xyzw acc, vf29, vf2
  { const Vf s0 = f_vf30, s1 = f_vf2; f_vf2.f[0] = acc.f[0] + s0.f[0] * s1.f[2]; f_vf2.f[1] = acc.f[1] + s0.f[1] * s1.f[2]; f_vf2.f[2] = acc.f[2] + s0.f[2] * s1.f[2]; f_vf2.f[3] = acc.f[3] + s0.f[3] * s1.f[2]; }  // vmaddz.xyzw vf2, vf30, vf2
  { const Vf s0 = f_vf31, s1 = kVf0; acc.f[0] = s0.f[0] * s1.f[3]; acc.f[1] = s0.f[1] * s1.f[3]; acc.f[2] = s0.f[2] * s1.f[3]; acc.f[3] = s0.f[3] * s1.f[3]; }  // vmulaw.xyzw acc, vf31, vf0
  { const Vf s0 = f_vf28, s1 = f_vf3; acc.f[0] += s0.f[0] * s1.f[0]; acc.f[1] += s0.f[1] * s1.f[0]; acc.f[2] += s0.f[2] * s1.f[0]; acc.f[3] += s0.f[3] * s1.f[0]; }  // vmaddax.xyzw acc, vf28, vf3
  { const Vf s0 = f_vf29, s1 = f_vf3; acc.f[0] += s0.f[0] * s1.f[1]; acc.f[1] += s0.f[1] * s1.f[1]; acc.f[2] += s0.f[2] * s1.f[1]; acc.f[3] += s0.f[3] * s1.f[1]; }  // vmadday.xyzw acc, vf29, vf3
  { const Vf s0 = f_vf30, s1 = f_vf3; f_vf3.f[0] = acc.f[0] + s0.f[0] * s1.f[2]; f_vf3.f[1] = acc.f[1] + s0.f[1] * s1.f[2]; f_vf3.f[2] = acc.f[2] + s0.f[2] * s1.f[2]; f_vf3.f[3] = acc.f[3] + s0.f[3] * s1.f[2]; }  // vmaddz.xyzw vf3, vf30, vf3
  { const Vf s0 = f_vf31, s1 = kVf0; acc.f[0] = s0.f[0] * s1.f[3]; acc.f[1] = s0.f[1] * s1.f[3]; acc.f[2] = s0.f[2] * s1.f[3]; acc.f[3] = s0.f[3] * s1.f[3]; }  // vmulaw.xyzw acc, vf31, vf0
  { const Vf s0 = f_vf28, s1 = f_vf4; acc.f[0] += s0.f[0] * s1.f[0]; acc.f[1] += s0.f[1] * s1.f[0]; acc.f[2] += s0.f[2] * s1.f[0]; acc.f[3] += s0.f[3] * s1.f[0]; }  // vmaddax.xyzw acc, vf28, vf4
  { const Vf s0 = f_vf29, s1 = f_vf4; acc.f[0] += s0.f[0] * s1.f[1]; acc.f[1] += s0.f[1] * s1.f[1]; acc.f[2] += s0.f[2] * s1.f[1]; acc.f[3] += s0.f[3] * s1.f[1]; }  // vmadday.xyzw acc, vf29, vf4
  { const Vf s0 = f_vf30, s1 = f_vf4; f_vf4.f[0] = acc.f[0] + s0.f[0] * s1.f[2]; f_vf4.f[1] = acc.f[1] + s0.f[1] * s1.f[2]; f_vf4.f[2] = acc.f[2] + s0.f[2] * s1.f[2]; f_vf4.f[3] = acc.f[3] + s0.f[3] * s1.f[2]; }  // vmaddz.xyzw vf4, vf30, vf4
  Q = f_vf25.f[2] / f_vf1.f[3];  // vdiv Q, vf25.z, vf1.w
  m.lq(x_t2, xh_t2, ((u32)x_v1 + (u32)(32)) & ~15u);  // lq t2, 32(v1)
  m.lq(x_t3, xh_t3, ((u32)x_v1 + (u32)(48)) & ~15u);  // lq t3, 48(v1)
  m.sq(((u32)x_a1 + (u32)(0)) & ~15u, x_t2, xh_t2);  // sq t2, 0(a1)
  m.sq(((u32)x_a1 + (u32)(16)) & ~15u, x_t3, xh_t3);  // sq t3, 16(a1)
  m.lqc2(f_vf5, (u32)x_v1 + (u32)(672));  // lqc2 vf5, 672(v1)
  m.lqc2(f_vf6, (u32)x_v1 + (u32)(688));  // lqc2 vf6, 688(v1)
  m.lqc2(f_vf7, (u32)x_v1 + (u32)(704));  // lqc2 vf7, 704(v1)
  { const Vf s0 = f_vf1; f_vf1.f[0] = s0.f[0] * Q; f_vf1.f[1] = s0.f[1] * Q; f_vf1.f[2] = s0.f[2] * Q; }  // vmulq.xyz vf1, vf1, Q
  m.sqc2((u32)x_a1 + (u32)(32), f_vf20);  // sqc2 vf20, 32(a1)
  { const Vf s0 = f_vf5; f_vf5.f[0] = s0.f[0] * Q; f_vf5.f[1] = s0.f[1] * Q; f_vf5.f[2] = s0.f[2] * Q; }  // vmulq.xyz vf5, vf5, Q
  Q = f_vf25.f[2] / f_vf2.f[3];  // vdiv Q, vf25.z, vf2.w
  m.lqc2(f_vf8, (u32)x_v1 + (u32)(720));  // lqc2 vf8, 720(v1)
  { const Vf s0 = f_vf1, s1 = f_vf26; f_vf1.f[0] = s0.f[0] + s1.f[0]; f_vf1.f[1] = s0.f[1] + s1.f[1]; f_vf1.f[2] = s0.f[2] + s1.f[2]; f_vf1.f[3] = s0.f[3] + s1.f[3]; }  // vadd.xyzw vf1, vf1, vf26
  m.lqc2(f_vf9, (u32)x_v1 + (u32)(2144));  // lqc2 vf9, 2144(v1)
  { const Vf s0 = f_vf1; set_i(f_vf1, 0, float_to_int_sat(s0.f[0] * 16.f)); set_i(f_vf1, 1, float_to_int_sat(s0.f[1] * 16.f)); set_i(f_vf1, 2, float_to_int_sat(s0.f[2] * 16.f)); set_i(f_vf1, 3, float_to_int_sat(s0.f[3] * 16.f)); }  // vftoi4.xyzw vf1, vf1
  { const Vf s0 = f_vf2; f_vf2.f[0] = s0.f[0] * Q; f_vf2.f[1] = s0.f[1] * Q; f_vf2.f[2] = s0.f[2] * Q; }  // vmulq.xyz vf2, vf2, Q
  { const Vf s0 = f_vf6; f_vf6.f[0] = s0.f[0] * Q; f_vf6.f[1] = s0.f[1] * Q; f_vf6.f[2] = s0.f[2] * Q; }  // vmulq.xyz vf6, vf6, Q
  m.sqc2((u32)x_a1 + (u32)(48), f_vf5);  // sqc2 vf5, 48(a1)
  Q = f_vf25.f[2] / f_vf3.f[3];  // vdiv Q, vf25.z, vf3.w
  m.sqc2((u32)x_a1 + (u32)(64), f_vf9);  // sqc2 vf9, 64(a1)
  m.sqc2((u32)x_a1 + (u32)(80), f_vf1);  // sqc2 vf1, 80(a1)
  { const Vf s0 = f_vf2, s1 = f_vf26; f_vf2.f[0] = s0.f[0] + s1.f[0]; f_vf2.f[1] = s0.f[1] + s1.f[1]; f_vf2.f[2] = s0.f[2] + s1.f[2]; f_vf2.f[3] = s0.f[3] + s1.f[3]; }  // vadd.xyzw vf2, vf2, vf26
  { const Vf s0 = f_vf2; set_i(f_vf2, 0, float_to_int_sat(s0.f[0] * 16.f)); set_i(f_vf2, 1, float_to_int_sat(s0.f[1] * 16.f)); set_i(f_vf2, 2, float_to_int_sat(s0.f[2] * 16.f)); set_i(f_vf2, 3, float_to_int_sat(s0.f[3] * 16.f)); }  // vftoi4.xyzw vf2, vf2
  { const Vf s0 = f_vf3; f_vf3.f[0] = s0.f[0] * Q; f_vf3.f[1] = s0.f[1] * Q; f_vf3.f[2] = s0.f[2] * Q; }  // vmulq.xyz vf3, vf3, Q
  { const Vf s0 = f_vf7; f_vf7.f[0] = s0.f[0] * Q; f_vf7.f[1] = s0.f[1] * Q; f_vf7.f[2] = s0.f[2] * Q; }  // vmulq.xyz vf7, vf7, Q
  m.sqc2((u32)x_a1 + (u32)(96), f_vf6);  // sqc2 vf6, 96(a1)
  Q = f_vf25.f[2] / f_vf4.f[3];  // vdiv Q, vf25.z, vf4.w
  m.sqc2((u32)x_a1 + (u32)(112), f_vf9);  // sqc2 vf9, 112(a1)
  m.sqc2((u32)x_a1 + (u32)(128), f_vf2);  // sqc2 vf2, 128(a1)
  { const Vf s0 = f_vf3, s1 = f_vf26; f_vf3.f[0] = s0.f[0] + s1.f[0]; f_vf3.f[1] = s0.f[1] + s1.f[1]; f_vf3.f[2] = s0.f[2] + s1.f[2]; f_vf3.f[3] = s0.f[3] + s1.f[3]; }  // vadd.xyzw vf3, vf3, vf26
  { const Vf s0 = f_vf3; set_i(f_vf3, 0, float_to_int_sat(s0.f[0] * 16.f)); set_i(f_vf3, 1, float_to_int_sat(s0.f[1] * 16.f)); set_i(f_vf3, 2, float_to_int_sat(s0.f[2] * 16.f)); set_i(f_vf3, 3, float_to_int_sat(s0.f[3] * 16.f)); }  // vftoi4.xyzw vf3, vf3
  { const Vf s0 = f_vf4; f_vf4.f[0] = s0.f[0] * Q; f_vf4.f[1] = s0.f[1] * Q; f_vf4.f[2] = s0.f[2] * Q; }  // vmulq.xyz vf4, vf4, Q
  { const Vf s0 = f_vf8; f_vf8.f[0] = s0.f[0] * Q; f_vf8.f[1] = s0.f[1] * Q; f_vf8.f[2] = s0.f[2] * Q; }  // vmulq.xyz vf8, vf8, Q
  m.sqc2((u32)x_a1 + (u32)(144), f_vf7);  // sqc2 vf7, 144(a1)
  m.sqc2((u32)x_a1 + (u32)(160), f_vf9);  // sqc2 vf9, 160(a1)
  { const Vf s0 = f_vf4, s1 = f_vf26; f_vf4.f[0] = s0.f[0] + s1.f[0]; f_vf4.f[1] = s0.f[1] + s1.f[1]; f_vf4.f[2] = s0.f[2] + s1.f[2]; f_vf4.f[3] = s0.f[3] + s1.f[3]; }  // vadd.xyzw vf4, vf4, vf26
  m.sqc2((u32)x_a1 + (u32)(176), f_vf3);  // sqc2 vf3, 176(a1)
  x_t2 = x_a0 & (u64)(2);  // andi t2, a0, 2
  bc = x_t2 == 0;  // beq t2, r0, L88
  x_t2 = x_a0 & (u64)(64);  // andi t2, a0, 64
  if (bc) {goto block_152;}
  bc = x_t2 != 0;  // bne t2, r0, L88
  if (bc) {goto block_152;}
  { const Vf s0 = f_vf23, s1 = f_vf19; f_vf23.f[0] = s0.f[0] + s1.f[3]; }  // vaddw.x vf23, vf23, vf19
  goto block_153;
  block_152:
  { const Vf s0 = f_vf23, s1 = f_vf14; f_vf23.f[0] = s0.f[0] + s1.f[3]; }  // vaddw.x vf23, vf23, vf14
  block_153:
  { const Vf s0 = f_vf4; set_i(f_vf4, 0, float_to_int_sat(s0.f[0] * 16.f)); set_i(f_vf4, 1, float_to_int_sat(s0.f[1] * 16.f)); set_i(f_vf4, 2, float_to_int_sat(s0.f[2] * 16.f)); set_i(f_vf4, 3, float_to_int_sat(s0.f[3] * 16.f)); }  // vftoi4.xyzw vf4, vf4
  m.sqc2((u32)x_a1 + (u32)(192), f_vf8);  // sqc2 vf8, 192(a1)
  m.sqc2((u32)x_a1 + (u32)(208), f_vf9);  // sqc2 vf9, 208(a1)
  m.sqc2((u32)x_a1 + (u32)(224), f_vf4);  // sqc2 vf4, 224(a1)
  x_t3 = (u64)(s64)m.ld<s32>((u32)x_a1, 80);  // lw t3, 80(a1)
  x_t2 = (u64)(s64)m.ld<s32>((u32)x_a1, 84);  // lw t2, 84(a1)
  x_t4 = u64(0) | (u64)(36864);  // ori t4, r0, 36864
  x_t3 = x_t3 - x_t4;  // dsubu t3, t3, t4
  x_t4 = (u64)(s64)m.ld<s32>((u32)x_a1, 224);  // lw t4, 224(a1)
  x_t5 = u64(0) | (u64)(36096);  // ori t5, r0, 36096
  x_t2 = x_t2 - x_t5;  // dsubu t2, t2, t5
  x_t5 = (u64)(s64)m.ld<s32>((u32)x_a1, 228);  // lw t5, 228(a1)
  bc = ((s64)x_t3) > 0;  // bgtz t3, L67
  x_t3 = x_t4 + (u64)(s64)(-28672);  // daddiu t3, t4, -28672
  if (bc) {goto block_84;}
  bc = ((s64)x_t2) > 0;  // bgtz t2, L67
  x_t2 = x_t5 + (u64)(s64)(-29440);  // daddiu t2, t5, -29440
  if (bc) {goto block_84;}
  bc = ((s64)x_t3) < 0;  // bltz t3, L67
  if (bc) {goto block_84;}
  bc = ((s64)x_t2) < 0;  // bltz t2, L67
  if (bc) {goto block_84;}
  x_a1 = x_a1 + (u64)(s64)(240);  // daddiu a1, a1, 240
  goto block_84;
  block_158:
  x_a0 = (u64)(s64)m.ld<s32>((u32)x_v1, 2880);  // lw a0, 2880(v1)
  x_t0 = x_v1;  // or t0, v1, r0
  x_t1 = (u64)(s64)m.ld<s32>((u32)x_v1, 2876);  // lw t1, 2876(v1)
  m.lqc2(f_vf23, (u32)x_t0 + (u32)(1120));  // lqc2 vf23, 1120(t0)
  block_159:
  x_t2 = (u64)m.ld<u8>((u32)x_t1, 4);  // lbu t2, 4(t1)
  x_t1 = x_t1 + (u64)(s64)(1);  // daddiu t1, t1, 1
  m.lqc2(f_vf20, (u32)x_v1 + (u32)(2800));  // lqc2 vf20, 2800(v1)
  bc = x_t2 == 0;  // beq t2, r0, L117
  x_t3 = x_t2 + (u64)(s64)(-3);  // daddiu t3, t2, -3
  if (bc) {goto block_234;}
  bc = ((s64)x_t3) <= 0;  // blez t3, L108
  x_t3 = x_t2 + (u64)(s64)(-126);  // daddiu t3, t2, -126
  if (bc) {goto block_214;}
  bc = x_t3 != 0;  // bne t3, r0, L112
  if (bc) {goto block_220;}
  x_t2 = (u64)m.ld<u8>((u32)x_t1, 4);  // lbu t2, 4(t1)
  x_t1 = x_t1 + (u64)(s64)(1);  // daddiu t1, t1, 1
  x_t3 = (u64)(s64)(s32)(u64(0) + (u64)(s64)(0));  // addiu t3, r0, 0
  x_t4 = (u64)(s64)(s32)(u64(0) + (u64)(s64)(0));  // addiu t4, r0, 0
  bc = x_t2 == 0;  // beq t2, r0, L117
  x_t5 = x_t2 + (u64)(s64)(-43);  // daddiu t5, t2, -43
  if (bc) {goto block_234;}
  if (x_t5 == 0) { x_t3 = x_t2; }  // movz t3, t2, t5
  x_t5 = x_t2 + (u64)(s64)(-45);  // daddiu t5, t2, -45
  if (x_t5 == 0) { x_t3 = x_t2; }  // movz t3, t2, t5
  bc = x_t3 != 0;  // bne t3, r0, L92
  x_t5 = x_t2 + (u64)(s64)(-91);  // daddiu t5, t2, -91
  if (bc) {goto block_173;}
  bc = x_t5 == 0;  // beq t5, r0, L104
  x_t4 = x_t2 + (u64)(s64)(-93);  // daddiu t4, t2, -93
  if (bc) {goto block_210;}
  bc = x_t4 == 0;  // beq t4, r0, L105
  x_t4 = x_t2 + (u64)(s64)(-121);  // daddiu t4, t2, -121
  if (bc) {goto block_211;}
  bc = x_t4 == 0;  // beq t4, r0, L106
  x_t4 = x_t2 + (u64)(s64)(-89);  // daddiu t4, t2, -89
  if (bc) {goto block_212;}
  bc = x_t4 == 0;  // beq t4, r0, L106
  x_t4 = x_t2 + (u64)(s64)(-122);  // daddiu t4, t2, -122
  if (bc) {goto block_212;}
  bc = x_t4 == 0;  // beq t4, r0, L107
  x_t4 = x_t2 + (u64)(s64)(-90);  // daddiu t4, t2, -90
  if (bc) {goto block_213;}
  bc = x_t4 == 0;  // beq t4, r0, L107
  x_t4 = x_t2 + (u64)(s64)(-48);  // daddiu t4, t2, -48
  if (bc) {goto block_213;}
  bc = ((s64)x_t4) < 0;  // bltz t4, L112
  x_t4 = x_t2 + (u64)(s64)(-57);  // daddiu t4, t2, -57
  if (bc) {goto block_220;}
  bc = ((s64)x_t4) > 0;  // bgtz t4, L112
  x_t4 = x_t2 + (u64)(s64)(-126);  // daddiu t4, t2, -126
  if (bc) {goto block_220;}
  bc = x_t4 == 0;  // beq t4, r0, L112
  x_t4 = x_t2 + (u64)(s64)(-48);  // daddiu t4, t2, -48
  if (bc) {goto block_220;}
  block_173:
  x_t2 = (u64)m.ld<u8>((u32)x_t1, 4);  // lbu t2, 4(t1)
  x_t1 = x_t1 + (u64)(s64)(1);  // daddiu t1, t1, 1
  bc = x_t2 == 0;  // beq t2, r0, L117
  x_t5 = x_t2 + (u64)(s64)(-110);  // daddiu t5, t2, -110
  if (bc) {goto block_234;}
  bc = x_t5 == 0;  // beq t5, r0, L93
  x_t5 = x_t2 + (u64)(s64)(-78);  // daddiu t5, t2, -78
  if (bc) {goto block_193;}
  bc = x_t5 == 0;  // beq t5, r0, L93
  x_t5 = x_t2 + (u64)(s64)(-108);  // daddiu t5, t2, -108
  if (bc) {goto block_193;}
  bc = x_t5 == 0;  // beq t5, r0, L95
  x_t5 = x_t2 + (u64)(s64)(-76);  // daddiu t5, t2, -76
  if (bc) {goto block_196;}
  bc = x_t5 == 0;  // beq t5, r0, L95
  x_t5 = x_t2 + (u64)(s64)(-119);  // daddiu t5, t2, -119
  if (bc) {goto block_196;}
  bc = x_t5 == 0;  // beq t5, r0, L91
  x_t5 = x_t2 + (u64)(s64)(-87);  // daddiu t5, t2, -87
  if (bc) {goto block_159;}
  bc = x_t5 == 0;  // beq t5, r0, L91
  x_t5 = x_t2 + (u64)(s64)(-107);  // daddiu t5, t2, -107
  if (bc) {goto block_159;}
  bc = x_t5 == 0;  // beq t5, r0, L97
  x_t5 = x_t2 + (u64)(s64)(-75);  // daddiu t5, t2, -75
  if (bc) {goto block_198;}
  bc = x_t5 == 0;  // beq t5, r0, L97
  x_t5 = x_t2 + (u64)(s64)(-106);  // daddiu t5, t2, -106
  if (bc) {goto block_198;}
  bc = x_t5 == 0;  // beq t5, r0, L91
  x_t5 = x_t2 + (u64)(s64)(-74);  // daddiu t5, t2, -74
  if (bc) {goto block_159;}
  bc = x_t5 == 0;  // beq t5, r0, L91
  x_t5 = x_t2 + (u64)(s64)(-104);  // daddiu t5, t2, -104
  if (bc) {goto block_159;}
  bc = x_t5 == 0;  // beq t5, r0, L98
  x_t5 = x_t2 + (u64)(s64)(-72);  // daddiu t5, t2, -72
  if (bc) {goto block_200;}
  bc = x_t5 == 0;  // beq t5, r0, L98
  x_t5 = x_t2 + (u64)(s64)(-118);  // daddiu t5, t2, -118
  if (bc) {goto block_200;}
  bc = x_t5 == 0;  // beq t5, r0, L101
  x_t5 = x_t2 + (u64)(s64)(-86);  // daddiu t5, t2, -86
  if (bc) {goto block_205;}
  bc = x_t5 == 0;  // beq t5, r0, L101
  x_t5 = x_t2 + (u64)(s64)(-117);  // daddiu t5, t2, -117
  if (bc) {goto block_205;}
  bc = x_t5 == 0;  // beq t5, r0, L96
  x_t5 = x_t2 + (u64)(s64)(-85);  // daddiu t5, t2, -85
  if (bc) {goto block_197;}
  bc = x_t5 == 0;  // beq t5, r0, L96
  x_t5 = x_t2 + (u64)(s64)(-48);  // daddiu t5, t2, -48
  if (bc) {goto block_197;}
  bc = ((s64)x_t5) < 0;  // bltz t5, L112
  x_t6 = x_t2 + (u64)(s64)(-57);  // daddiu t6, t2, -57
  if (bc) {goto block_220;}
  bc = ((s64)x_t6) > 0;  // bgtz t6, L112
  x_t6 = (u64)(s64)(s32)((u32)x_t4 << 2);  // sll t6, t4, 2
  if (bc) {goto block_220;}
  x_t2 = x_t4 + x_t6;  // daddu t2, t4, t6
  x_t2 = (u64)(s64)(s32)((u32)x_t2 << 1);  // sll t2, t2, 1
  x_t4 = x_t2 + x_t5;  // daddu t4, t2, t5
  goto block_173;
  block_193:
  bc = x_t4 != 0;  // bne t4, r0, L94
  x_t2 = (u64)(s64)(s32)(u64(0) + (u64)(s64)(-33));  // addiu t2, r0, -33
  if (bc) {goto block_195;}
  m.lq(x_a3, xh_a3, ((u32)x_v1 + (u32)(80)) & ~15u);  // lq a3, 80(v1)
  m.lq(x_t3, xh_t3, ((u32)x_v1 + (u32)(96)) & ~15u);  // lq t3, 96(v1)
  m.lq(x_t4, xh_t4, ((u32)x_v1 + (u32)(112)) & ~15u);  // lq t4, 112(v1)
  m.lq(x_t5, xh_t5, ((u32)x_v1 + (u32)(128)) & ~15u);  // lq t5, 128(v1)
  m.sq(((u32)x_v1 + (u32)(2800)) & ~15u, x_a3, xh_a3);  // sq a3, 2800(v1)
  m.sq(((u32)x_v1 + (u32)(2816)) & ~15u, x_t3, xh_t3);  // sq t3, 2816(v1)
  m.sq(((u32)x_v1 + (u32)(2832)) & ~15u, x_t4, xh_t4);  // sq t4, 2832(v1)
  m.sq(((u32)x_v1 + (u32)(2848)) & ~15u, x_t5, xh_t5);  // sq t5, 2848(v1)
  x_a3 = (u64)(s64)m.ld<s32>(sym_font12_table, 0);  // lw a3, *font12-table*(s7)
  // (no-op)  // or a3, a3, r0
  m.lqc2(f_vf13, (u32)x_v1 + (u32)(208));  // lqc2 vf13, 208(v1)
  m.lqc2(f_vf14, (u32)x_v1 + (u32)(224));  // lqc2 vf14, 224(v1)
  m.lqc2(f_vf15, (u32)x_v1 + (u32)(240));  // lqc2 vf15, 240(v1)
  m.sqc2((u32)x_v1 + (u32)(352), f_vf14);  // sqc2 vf14, 352(v1)
  m.sqc2((u32)x_v1 + (u32)(368), f_vf14);  // sqc2 vf14, 368(v1)
  m.sqc2((u32)x_v1 + (u32)(384), f_vf14);  // sqc2 vf14, 384(v1)
  m.sqc2((u32)x_v1 + (u32)(400), f_vf14);  // sqc2 vf14, 400(v1)
  x_a0 = x_a0 & x_t2;  // and a0, a0, t2
  goto block_159;
  block_195:
  m.lq(x_a3, xh_a3, ((u32)x_v1 + (u32)(144)) & ~15u);  // lq a3, 144(v1)
  m.lq(x_t2, xh_t2, ((u32)x_v1 + (u32)(160)) & ~15u);  // lq t2, 160(v1)
  m.lq(x_t3, xh_t3, ((u32)x_v1 + (u32)(176)) & ~15u);  // lq t3, 176(v1)
  m.lq(x_t4, xh_t4, ((u32)x_v1 + (u32)(192)) & ~15u);  // lq t4, 192(v1)
  m.sq(((u32)x_v1 + (u32)(2800)) & ~15u, x_a3, xh_a3);  // sq a3, 2800(v1)
  m.sq(((u32)x_v1 + (u32)(2816)) & ~15u, x_t2, xh_t2);  // sq t2, 2816(v1)
  m.sq(((u32)x_v1 + (u32)(2832)) & ~15u, x_t3, xh_t3);  // sq t3, 2832(v1)
  m.sq(((u32)x_v1 + (u32)(2848)) & ~15u, x_t4, xh_t4);  // sq t4, 2848(v1)
  x_a3 = (u64)(s64)m.ld<s32>(sym_font24_table, 0);  // lw a3, *font24-table*(s7)
  // (no-op)  // or a3, a3, r0
  m.lqc2(f_vf13, (u32)x_v1 + (u32)(256));  // lqc2 vf13, 256(v1)
  m.lqc2(f_vf15, (u32)x_v1 + (u32)(336));  // lqc2 vf15, 336(v1)
  m.lqc2(f_vf14, (u32)x_v1 + (u32)(272));  // lqc2 vf14, 272(v1)
  m.sqc2((u32)x_v1 + (u32)(352), f_vf14);  // sqc2 vf14, 352(v1)
  m.lqc2(f_vf14, (u32)x_v1 + (u32)(288));  // lqc2 vf14, 288(v1)
  m.sqc2((u32)x_v1 + (u32)(368), f_vf14);  // sqc2 vf14, 368(v1)
  m.lqc2(f_vf14, (u32)x_v1 + (u32)(304));  // lqc2 vf14, 304(v1)
  m.sqc2((u32)x_v1 + (u32)(384), f_vf14);  // sqc2 vf14, 384(v1)
  m.lqc2(f_vf14, (u32)x_v1 + (u32)(320));  // lqc2 vf14, 320(v1)
  m.sqc2((u32)x_v1 + (u32)(400), f_vf14);  // sqc2 vf14, 400(v1)
  x_a0 = x_a0 | (u64)(32);  // ori a0, a0, 32
  goto block_159;
  block_196:
  m.sw((u32)x_v1 + (u32)(2864), (u32)x_t4);  // sw t4, 2864(v1)
  x_t2 = (u64)(s64)(s32)((u32)x_t4 << 4);  // sll t2, t4, 4
  m.lq(x_t3, xh_t3, ((u32)x_v1 + (u32)(608)) & ~15u);  // lq t3, 608(v1)
  x_t2 = x_t2 + x_v1;  // daddu t2, t2, v1
  m.lq(x_t4, xh_t4, ((u32)x_v1 + (u32)(624)) & ~15u);  // lq t4, 624(v1)
  m.sq(((u32)x_v1 + (u32)(800)) & ~15u, x_t3, xh_t3);  // sq t3, 800(v1)
  m.sq(((u32)x_v1 + (u32)(816)) & ~15u, x_t4, xh_t4);  // sq t4, 816(v1)
  m.lq(x_t3, xh_t3, ((u32)x_v1 + (u32)(640)) & ~15u);  // lq t3, 640(v1)
  m.lq(x_t4, xh_t4, ((u32)x_v1 + (u32)(656)) & ~15u);  // lq t4, 656(v1)
  m.sq(((u32)x_v1 + (u32)(832)) & ~15u, x_t3, xh_t3);  // sq t3, 832(v1)
  m.sq(((u32)x_v1 + (u32)(848)) & ~15u, x_t4, xh_t4);  // sq t4, 848(v1)
  x_t4 = (u64)m.ld<u32>((u32)x_t2, 2160);  // lwu t4, 2160(t2)
  x_t3 = (u64)m.ld<u32>((u32)x_t2, 2164);  // lwu t3, 2164(t2)
  pextlb(x_t4, xh_t4, u64(0), x_t4);  // pextlb t4, r0, t4
  // mfc1 r0  // mfc1 r0, f31
  pextlh(x_t4, xh_t4, u64(0), x_t4);  // pextlh t4, r0, t4
  // mfc1 r0  // mfc1 r0, f31
  pextlb(x_t3, xh_t3, u64(0), x_t3);  // pextlb t3, r0, t3
  m.sq(((u32)x_v1 + (u32)(608)) & ~15u, x_t4, xh_t4);  // sq t4, 608(v1)
  pextlh(x_t3, xh_t3, u64(0), x_t3);  // pextlh t3, r0, t3
  m.sq(((u32)x_v1 + (u32)(992)) & ~15u, x_t4, xh_t4);  // sq t4, 992(v1)
  m.sq(((u32)x_v1 + (u32)(624)) & ~15u, x_t3, xh_t3);  // sq t3, 624(v1)
  m.sq(((u32)x_v1 + (u32)(1008)) & ~15u, x_t3, xh_t3);  // sq t3, 1008(v1)
  x_t3 = (u64)m.ld<u32>((u32)x_t2, 2168);  // lwu t3, 2168(t2)
  x_t2 = (u64)m.ld<u32>((u32)x_t2, 2172);  // lwu t2, 2172(t2)
  pextlb(x_t3, xh_t3, u64(0), x_t3);  // pextlb t3, r0, t3
  // mfc1 r0  // mfc1 r0, f31
  pextlh(x_t3, xh_t3, u64(0), x_t3);  // pextlh t3, r0, t3
  // mfc1 r0  // mfc1 r0, f31
  pextlb(x_t2, xh_t2, u64(0), x_t2);  // pextlb t2, r0, t2
  m.sq(((u32)x_v1 + (u32)(640)) & ~15u, x_t3, xh_t3);  // sq t3, 640(v1)
  pextlh(x_t2, xh_t2, u64(0), x_t2);  // pextlh t2, r0, t2
  m.sq(((u32)x_v1 + (u32)(1024)) & ~15u, x_t3, xh_t3);  // sq t3, 1024(v1)
  m.sq(((u32)x_v1 + (u32)(656)) & ~15u, x_t2, xh_t2);  // sq t2, 656(v1)
  m.sq(((u32)x_v1 + (u32)(1040)) & ~15u, x_t2, xh_t2);  // sq t2, 1040(v1)
  goto block_159;
  block_197:
  m.lq(x_t2, xh_t2, ((u32)x_v1 + (u32)(608)) & ~15u);  // lq t2, 608(v1)
  m.lq(x_t3, xh_t3, ((u32)x_v1 + (u32)(624)) & ~15u);  // lq t3, 624(v1)
  m.sq(((u32)x_v1 + (u32)(800)) & ~15u, x_t2, xh_t2);  // sq t2, 800(v1)
  m.sq(((u32)x_v1 + (u32)(816)) & ~15u, x_t3, xh_t3);  // sq t3, 816(v1)
  m.lq(x_t2, xh_t2, ((u32)x_v1 + (u32)(640)) & ~15u);  // lq t2, 640(v1)
  m.lq(x_t3, xh_t3, ((u32)x_v1 + (u32)(656)) & ~15u);  // lq t3, 656(v1)
  m.sq(((u32)x_v1 + (u32)(832)) & ~15u, x_t2, xh_t2);  // sq t2, 832(v1)
  m.sq(((u32)x_v1 + (u32)(848)) & ~15u, x_t3, xh_t3);  // sq t3, 848(v1)
  pextlb(x_t2, xh_t2, u64(0), x_t4);  // pextlb t2, r0, t4
  // mfc1 r0  // mfc1 r0, f31
  pextlh(x_t2, xh_t2, u64(0), x_t2);  // pextlh t2, r0, t2
  // mfc1 r0  // mfc1 r0, f31
  m.sq(((u32)x_v1 + (u32)(608)) & ~15u, x_t2, xh_t2);  // sq t2, 608(v1)
  m.sq(((u32)x_v1 + (u32)(992)) & ~15u, x_t2, xh_t2);  // sq t2, 992(v1)
  m.sq(((u32)x_v1 + (u32)(624)) & ~15u, x_t2, xh_t2);  // sq t2, 624(v1)
  m.sq(((u32)x_v1 + (u32)(1008)) & ~15u, x_t2, xh_t2);  // sq t2, 1008(v1)
  m.sq(((u32)x_v1 + (u32)(640)) & ~15u, x_t2, xh_t2);  // sq t2, 640(v1)
  m.sq(((u32)x_v1 + (u32)(1024)) & ~15u, x_t2, xh_t2);  // sq t2, 1024(v1)
  m.sq(((u32)x_v1 + (u32)(656)) & ~15u, x_t2, xh_t2);  // sq t2, 656(v1)
  m.sq(((u32)x_v1 + (u32)(1040)) & ~15u, x_t2, xh_t2);  // sq t2, 1040(v1)
  goto block_159;
  block_198:
  x_t2 = (u64)(s64)(s32)(u64(0) + (u64)(s64)(-3));  // addiu t2, r0, -3
  bc = x_t4 == 0;  // beq t4, r0, L91
  x_a0 = x_a0 & x_t2;  // and a0, a0, t2
  if (bc) {goto block_159;}
  x_a0 = x_a0 | (u64)(2);  // ori a0, a0, 2
  goto block_159;
  block_200:
  vf_from_gpr(f_vf1, x_t4, xh_t4);  // qmtc2.i vf1, t4
  x_t2 = x_t3 + (u64)(s64)(-45);  // daddiu t2, t3, -45
  bc = x_t3 == 0;  // beq t3, r0, L100
  { const Vf s0 = f_vf1; f_vf1.f[0] = (float)get_i(s0, 0); f_vf1.f[1] = (float)get_i(s0, 1); f_vf1.f[2] = (float)get_i(s0, 2); f_vf1.f[3] = (float)get_i(s0, 3); }  // vitof0.xyzw vf1, vf1
  if (bc) {goto block_204;}
  bc = x_t2 == 0;  // beq t2, r0, L99
  if (bc) {goto block_203;}
  { const Vf s0 = f_vf23, s1 = f_vf1; f_vf23.f[0] = s0.f[0] + s1.f[0]; }  // vaddx.x vf23, vf23, vf1
  goto block_159;
  block_203:
  { const Vf s0 = f_vf23, s1 = f_vf1; f_vf23.f[0] = s0.f[0] - s1.f[0]; }  // vsubx.x vf23, vf23, vf1
  goto block_159;
  block_204:
  { const Vf s0 = kVf0, s1 = f_vf1; f_vf23.f[0] = s0.f[0] + s1.f[0]; }  // vaddx.x vf23, vf0, vf1
  goto block_159;
  block_205:
  vf_from_gpr(f_vf1, x_t4, xh_t4);  // qmtc2.i vf1, t4
  x_t2 = x_t3 + (u64)(s64)(-45);  // daddiu t2, t3, -45
  bc = x_t3 == 0;  // beq t3, r0, L103
  { const Vf s0 = f_vf1; f_vf1.f[0] = (float)get_i(s0, 0); f_vf1.f[1] = (float)get_i(s0, 1); f_vf1.f[2] = (float)get_i(s0, 2); f_vf1.f[3] = (float)get_i(s0, 3); }  // vitof0.xyzw vf1, vf1
  if (bc) {goto block_209;}
  bc = x_t2 == 0;  // beq t2, r0, L102
  if (bc) {goto block_208;}
  { const Vf s0 = f_vf23, s1 = f_vf1; f_vf23.f[1] = s0.f[1] + s1.f[0]; }  // vaddx.y vf23, vf23, vf1
  goto block_159;
  block_208:
  { const Vf s0 = f_vf23, s1 = f_vf1; f_vf23.f[1] = s0.f[1] - s1.f[0]; }  // vsubx.y vf23, vf23, vf1
  goto block_159;
  block_209:
  { const Vf s0 = kVf0, s1 = f_vf1; f_vf23.f[1] = s0.f[1] + s1.f[0]; }  // vaddx.y vf23, vf0, vf1
  goto block_159;
  block_210:
  x_t2 = (u64)(s64)m.ld<s32>((u32)x_v1, 2864);  // lw t2, 2864(v1)
  m.lqc2(f_vf9, (u32)x_v1 + (u32)(608));  // lqc2 vf9, 608(v1)
  m.lqc2(f_vf10, (u32)x_v1 + (u32)(624));  // lqc2 vf10, 624(v1)
  m.lqc2(f_vf11, (u32)x_v1 + (u32)(640));  // lqc2 vf11, 640(v1)
  m.lqc2(f_vf12, (u32)x_v1 + (u32)(656));  // lqc2 vf12, 656(v1)
  m.sw((u32)x_v1 + (u32)(2868), (u32)x_t2);  // sw t2, 2868(v1)
  m.sqc2((u32)x_v1 + (u32)(480), f_vf9);  // sqc2 vf9, 480(v1)
  m.sqc2((u32)x_v1 + (u32)(496), f_vf10);  // sqc2 vf10, 496(v1)
  m.sqc2((u32)x_v1 + (u32)(512), f_vf11);  // sqc2 vf11, 512(v1)
  m.sqc2((u32)x_v1 + (u32)(528), f_vf12);  // sqc2 vf12, 528(v1)
  goto block_159;
  block_211:
  x_t2 = (u64)(s64)m.ld<s32>((u32)x_v1, 2868);  // lw t2, 2868(v1)
  m.lqc2(f_vf9, (u32)x_v1 + (u32)(480));  // lqc2 vf9, 480(v1)
  m.lqc2(f_vf10, (u32)x_v1 + (u32)(496));  // lqc2 vf10, 496(v1)
  m.lqc2(f_vf11, (u32)x_v1 + (u32)(512));  // lqc2 vf11, 512(v1)
  m.lqc2(f_vf12, (u32)x_v1 + (u32)(528));  // lqc2 vf12, 528(v1)
  m.sw((u32)x_v1 + (u32)(2864), (u32)x_t2);  // sw t2, 2864(v1)
  m.sqc2((u32)x_v1 + (u32)(608), f_vf9);  // sqc2 vf9, 608(v1)
  m.sqc2((u32)x_v1 + (u32)(992), f_vf9);  // sqc2 vf9, 992(v1)
  m.sqc2((u32)x_v1 + (u32)(624), f_vf10);  // sqc2 vf10, 624(v1)
  m.sqc2((u32)x_v1 + (u32)(1008), f_vf10);  // sqc2 vf10, 1008(v1)
  m.sqc2((u32)x_v1 + (u32)(640), f_vf11);  // sqc2 vf11, 640(v1)
  m.sqc2((u32)x_v1 + (u32)(1024), f_vf11);  // sqc2 vf11, 1024(v1)
  m.sqc2((u32)x_v1 + (u32)(656), f_vf12);  // sqc2 vf12, 656(v1)
  m.sqc2((u32)x_v1 + (u32)(1040), f_vf12);  // sqc2 vf12, 1040(v1)
  goto block_159;
  block_212:
  x_t2 = (u64)(s64)m.ld<s32>((u32)x_v1, 2864);  // lw t2, 2864(v1)
  m.lqc2(f_vf9, (u32)x_v1 + (u32)(608));  // lqc2 vf9, 608(v1)
  m.lqc2(f_vf10, (u32)x_v1 + (u32)(624));  // lqc2 vf10, 624(v1)
  m.lqc2(f_vf11, (u32)x_v1 + (u32)(640));  // lqc2 vf11, 640(v1)
  m.lqc2(f_vf12, (u32)x_v1 + (u32)(656));  // lqc2 vf12, 656(v1)
  m.sw((u32)x_v1 + (u32)(2868), (u32)x_t2);  // sw t2, 2868(v1)
  m.sqc2((u32)x_v1 + (u32)(480), f_vf9);  // sqc2 vf9, 480(v1)
  m.sqc2((u32)x_v1 + (u32)(496), f_vf10);  // sqc2 vf10, 496(v1)
  m.sqc2((u32)x_v1 + (u32)(512), f_vf11);  // sqc2 vf11, 512(v1)
  m.sqc2((u32)x_v1 + (u32)(528), f_vf12);  // sqc2 vf12, 528(v1)
  m.sqc2((u32)x_v1 + (u32)(464), f_vf23);  // sqc2 vf23, 464(v1)
  goto block_159;
  block_213:
  x_t2 = (u64)(s64)m.ld<s32>((u32)x_v1, 2868);  // lw t2, 2868(v1)
  m.lqc2(f_vf9, (u32)x_v1 + (u32)(480));  // lqc2 vf9, 480(v1)
  m.lqc2(f_vf10, (u32)x_v1 + (u32)(496));  // lqc2 vf10, 496(v1)
  m.lqc2(f_vf11, (u32)x_v1 + (u32)(512));  // lqc2 vf11, 512(v1)
  m.lqc2(f_vf12, (u32)x_v1 + (u32)(528));  // lqc2 vf12, 528(v1)
  m.sw((u32)x_v1 + (u32)(2864), (u32)x_t2);  // sw t2, 2864(v1)
  m.sqc2((u32)x_v1 + (u32)(608), f_vf9);  // sqc2 vf9, 608(v1)
  m.sqc2((u32)x_v1 + (u32)(992), f_vf9);  // sqc2 vf9, 992(v1)
  m.sqc2((u32)x_v1 + (u32)(624), f_vf10);  // sqc2 vf10, 624(v1)
  m.sqc2((u32)x_v1 + (u32)(1008), f_vf10);  // sqc2 vf10, 1008(v1)
  m.sqc2((u32)x_v1 + (u32)(640), f_vf11);  // sqc2 vf11, 640(v1)
  m.sqc2((u32)x_v1 + (u32)(1024), f_vf11);  // sqc2 vf11, 1024(v1)
  m.sqc2((u32)x_v1 + (u32)(656), f_vf12);  // sqc2 vf12, 656(v1)
  m.sqc2((u32)x_v1 + (u32)(1040), f_vf12);  // sqc2 vf12, 1040(v1)
  m.lqc2(f_vf23, (u32)x_v1 + (u32)(464));  // lqc2 vf23, 464(v1)
  goto block_159;
  block_214:
  x_t3 = x_t2 + (u64)(s64)(-3);  // daddiu t3, t2, -3
  x_a0 = x_a0 | (u64)(64);  // ori a0, a0, 64
  bc = x_t3 == 0;  // beq t3, r0, L110
  x_t2 = x_t2 + (u64)(s64)(-2);  // daddiu t2, t2, -2
  if (bc) {goto block_218;}
  bc = x_t2 == 0;  // beq t2, r0, L109
  m.lqc2(f_vf14, (u32)x_v1 + (u32)(384));  // lqc2 vf14, 384(v1)
  if (bc) {goto block_217;}
  m.lqc2(f_vf14, (u32)x_v1 + (u32)(368));  // lqc2 vf14, 368(v1)
  m.lqc2(f_vf20, (u32)x_v1 + (u32)(2816));  // lqc2 vf20, 2816(v1)
  goto block_219;
  block_217:
  m.lqc2(f_vf20, (u32)x_v1 + (u32)(2832));  // lqc2 vf20, 2832(v1)
  goto block_219;
  block_218:
  m.lqc2(f_vf14, (u32)x_v1 + (u32)(400));  // lqc2 vf14, 400(v1)
  m.lqc2(f_vf20, (u32)x_v1 + (u32)(2848));  // lqc2 vf20, 2848(v1)
  block_219:
  x_t2 = (u64)m.ld<u8>((u32)x_t1, 4);  // lbu t2, 4(t1)
  x_t1 = x_t1 + (u64)(s64)(1);  // daddiu t1, t1, 1
  { const Vf s0 = f_vf23, s1 = f_vf15; f_vf4.f[0] = s0.f[0] + s1.f[0]; f_vf4.f[1] = s0.f[1] + s1.f[1]; f_vf4.f[2] = s0.f[2] + s1.f[2]; }  // vadd.xyz vf4, vf23, vf15
  x_t3 = (u64)(s64)(s32)((u32)x_t2 << 4);  // sll t3, t2, 4
  { const Vf s0 = f_vf25, s1 = f_vf23; f_vf1.f[0] = s0.f[0] - s1.f[0]; f_vf1.f[1] = s0.f[1] - s1.f[1]; f_vf1.f[2] = s0.f[2] - s1.f[2]; f_vf1.f[3] = s0.f[3] - s1.f[3]; }  // vsub.xyzw vf1, vf25, vf23
  goto block_223;
  block_220:
  x_t3 = (u64)(s64)(s32)(u64(0) + (u64)(s64)(-65));  // addiu t3, r0, -65
  x_a0 = x_a0 & x_t3;  // and a0, a0, t3
  m.lqc2(f_vf14, (u32)x_v1 + (u32)(352));  // lqc2 vf14, 352(v1)
  { const Vf s0 = f_vf23, s1 = f_vf15; f_vf4.f[0] = s0.f[0] + s1.f[0]; f_vf4.f[1] = s0.f[1] + s1.f[1]; f_vf4.f[2] = s0.f[2] + s1.f[2]; }  // vadd.xyz vf4, vf23, vf15
  x_t3 = (u64)(s64)(s32)((u32)x_t2 << 4);  // sll t3, t2, 4
  { const Vf s0 = f_vf25, s1 = f_vf23; f_vf1.f[0] = s0.f[0] - s1.f[0]; f_vf1.f[1] = s0.f[1] - s1.f[1]; f_vf1.f[2] = s0.f[2] - s1.f[2]; f_vf1.f[3] = s0.f[3] - s1.f[3]; }  // vsub.xyzw vf1, vf25, vf23
  x_t4 = x_t2 + (u64)(s64)(-10);  // daddiu t4, t2, -10
  bc = x_t4 == 0;  // beq t4, r0, L113
  x_t2 = x_t2 + (u64)(s64)(-13);  // daddiu t2, t2, -13
  if (bc) {goto block_222;}
  bc = x_t2 != 0;  // bne t2, r0, L114
  if (bc) {goto block_223;}
  block_222:
  x_t0 = x_t0 + (u64)(s64)(16);  // daddiu t0, t0, 16
  m.lqc2(f_vf23, (u32)x_t0 + (u32)(1120));  // lqc2 vf23, 1120(t0)
  goto block_159;
  block_223:
  x_t2 = (u64)(s64)(s32)(x_t3 + x_a3);  // addu t2, t3, a3
  m.lqc2(f_vf5, (u32)x_t2 + (u32)(-96));  // lqc2 vf5, -96(t2)
  gpr_from_vf(x_t2, xh_t2, f_vf1);  // qmfc2.i t2, vf1
  bc = ((s64)x_t2) < 0;  // bltz t2, L117
  { const Vf s0 = f_vf5, s1 = f_vf16; f_vf6.f[0] = s0.f[0] + s1.f[0]; f_vf6.f[1] = s0.f[1] + s1.f[1]; f_vf6.f[2] = s0.f[2] + s1.f[2]; }  // vadd.xyz vf6, vf5, vf16
  if (bc) {goto block_234;}
  x_t2 = (u64)(s64)((s32)x_t2 >> 31);  // sra t2, t2, 31
  { const Vf s0 = f_vf5, s1 = f_vf17; f_vf7.f[0] = s0.f[0] + s1.f[0]; f_vf7.f[1] = s0.f[1] + s1.f[1]; f_vf7.f[2] = s0.f[2] + s1.f[2]; }  // vadd.xyz vf7, vf5, vf17
  bc = ((s64)x_t2) < 0;  // bltz t2, L91
  { const Vf s0 = f_vf5, s1 = f_vf18; f_vf8.f[0] = s0.f[0] + s1.f[0]; f_vf8.f[1] = s0.f[1] + s1.f[1]; f_vf8.f[2] = s0.f[2] + s1.f[2]; }  // vadd.xyz vf8, vf5, vf18
  if (bc) {goto block_159;}
  { const Vf s0 = f_vf23, s1 = kVf0; f_vf1.f[0] = s0.f[0] + s1.f[0]; f_vf1.f[1] = s0.f[1] + s1.f[1]; f_vf1.f[2] = s0.f[2] + s1.f[2]; }  // vadd.xyz vf1, vf23, vf0
  { const Vf s0 = f_vf23, s1 = f_vf13; f_vf2.f[0] = s0.f[0] + s1.f[0]; f_vf2.f[1] = s0.f[1] + s1.f[1]; f_vf2.f[2] = s0.f[2] + s1.f[2]; }  // vadd.xyz vf2, vf23, vf13
  { const Vf s0 = f_vf23, s1 = f_vf14; f_vf3.f[0] = s0.f[0] + s1.f[0]; f_vf3.f[1] = s0.f[1] + s1.f[1]; f_vf3.f[2] = s0.f[2] + s1.f[2]; }  // vadd.xyz vf3, vf23, vf14
  m.sqc2((u32)x_v1 + (u32)(672), f_vf5);  // sqc2 vf5, 672(v1)
  { const Vf s0 = f_vf23, s1 = f_vf15; f_vf4.f[0] = s0.f[0] + s1.f[0]; f_vf4.f[1] = s0.f[1] + s1.f[1]; f_vf4.f[2] = s0.f[2] + s1.f[2]; }  // vadd.xyz vf4, vf23, vf15
  m.sqc2((u32)x_v1 + (u32)(688), f_vf6);  // sqc2 vf6, 688(v1)
  { const Vf s0 = f_vf5, s1 = f_vf13; f_vf19.f[0] = s0.f[0] * s1.f[0]; f_vf19.f[1] = s0.f[1] * s1.f[1]; f_vf19.f[2] = s0.f[2] * s1.f[2]; f_vf19.f[3] = s0.f[3] * s1.f[3]; }  // vmul.xyzw vf19, vf5, vf13
  m.sqc2((u32)x_v1 + (u32)(704), f_vf7);  // sqc2 vf7, 704(v1)
  m.sqc2((u32)x_v1 + (u32)(720), f_vf8);  // sqc2 vf8, 720(v1)
  m.sqc2((u32)x_v1 + (u32)(544), f_vf1);  // sqc2 vf1, 544(v1)
  m.sqc2((u32)x_v1 + (u32)(560), f_vf2);  // sqc2 vf2, 560(v1)
  m.sqc2((u32)x_v1 + (u32)(576), f_vf3);  // sqc2 vf3, 576(v1)
  m.sqc2((u32)x_v1 + (u32)(592), f_vf4);  // sqc2 vf4, 592(v1)
  m.lqc2(f_vf1, (u32)x_v1 + (u32)(544));  // lqc2 vf1, 544(v1)
  m.lqc2(f_vf2, (u32)x_v1 + (u32)(560));  // lqc2 vf2, 560(v1)
  { const Vf s0 = f_vf31, s1 = kVf0; acc.f[0] = s0.f[0] * s1.f[3]; acc.f[1] = s0.f[1] * s1.f[3]; acc.f[2] = s0.f[2] * s1.f[3]; acc.f[3] = s0.f[3] * s1.f[3]; }  // vmulaw.xyzw acc, vf31, vf0
  m.lqc2(f_vf3, (u32)x_v1 + (u32)(576));  // lqc2 vf3, 576(v1)
  { const Vf s0 = f_vf28, s1 = f_vf1; acc.f[0] += s0.f[0] * s1.f[0]; acc.f[1] += s0.f[1] * s1.f[0]; acc.f[2] += s0.f[2] * s1.f[0]; acc.f[3] += s0.f[3] * s1.f[0]; }  // vmaddax.xyzw acc, vf28, vf1
  m.lqc2(f_vf4, (u32)x_v1 + (u32)(592));  // lqc2 vf4, 592(v1)
  { const Vf s0 = f_vf29, s1 = f_vf1; acc.f[0] += s0.f[0] * s1.f[1]; acc.f[1] += s0.f[1] * s1.f[1]; acc.f[2] += s0.f[2] * s1.f[1]; acc.f[3] += s0.f[3] * s1.f[1]; }  // vmadday.xyzw acc, vf29, vf1
  { const Vf s0 = f_vf30, s1 = f_vf1; f_vf1.f[0] = acc.f[0] + s0.f[0] * s1.f[2]; f_vf1.f[1] = acc.f[1] + s0.f[1] * s1.f[2]; f_vf1.f[2] = acc.f[2] + s0.f[2] * s1.f[2]; f_vf1.f[3] = acc.f[3] + s0.f[3] * s1.f[2]; }  // vmaddz.xyzw vf1, vf30, vf1
  { const Vf s0 = f_vf31, s1 = kVf0; acc.f[0] = s0.f[0] * s1.f[3]; acc.f[1] = s0.f[1] * s1.f[3]; acc.f[2] = s0.f[2] * s1.f[3]; acc.f[3] = s0.f[3] * s1.f[3]; }  // vmulaw.xyzw acc, vf31, vf0
  { const Vf s0 = f_vf28, s1 = f_vf2; acc.f[0] += s0.f[0] * s1.f[0]; acc.f[1] += s0.f[1] * s1.f[0]; acc.f[2] += s0.f[2] * s1.f[0]; acc.f[3] += s0.f[3] * s1.f[0]; }  // vmaddax.xyzw acc, vf28, vf2
  { const Vf s0 = f_vf29, s1 = f_vf2; acc.f[0] += s0.f[0] * s1.f[1]; acc.f[1] += s0.f[1] * s1.f[1]; acc.f[2] += s0.f[2] * s1.f[1]; acc.f[3] += s0.f[3] * s1.f[1]; }  // vmadday.xyzw acc, vf29, vf2
  { const Vf s0 = f_vf30, s1 = f_vf2; f_vf2.f[0] = acc.f[0] + s0.f[0] * s1.f[2]; f_vf2.f[1] = acc.f[1] + s0.f[1] * s1.f[2]; f_vf2.f[2] = acc.f[2] + s0.f[2] * s1.f[2]; f_vf2.f[3] = acc.f[3] + s0.f[3] * s1.f[2]; }  // vmaddz.xyzw vf2, vf30, vf2
  { const Vf s0 = f_vf31, s1 = kVf0; acc.f[0] = s0.f[0] * s1.f[3]; acc.f[1] = s0.f[1] * s1.f[3]; acc.f[2] = s0.f[2] * s1.f[3]; acc.f[3] = s0.f[3] * s1.f[3]; }  // vmulaw.xyzw acc, vf31, vf0
  { const Vf s0 = f_vf28, s1 = f_vf3; acc.f[0] += s0.f[0] * s1.f[0]; acc.f[1] += s0.f[1] * s1.f[0]; acc.f[2] += s0.f[2] * s1.f[0]; acc.f[3] += s0.f[3] * s1.f[0]; }  // vmaddax.xyzw acc, vf28, vf3
  { const Vf s0 = f_vf29, s1 = f_vf3; acc.f[0] += s0.f[0] * s1.f[1]; acc.f[1] += s0.f[1] * s1.f[1]; acc.f[2] += s0.f[2] * s1.f[1]; acc.f[3] += s0.f[3] * s1.f[1]; }  // vmadday.xyzw acc, vf29, vf3
  { const Vf s0 = f_vf30, s1 = f_vf3; f_vf3.f[0] = acc.f[0] + s0.f[0] * s1.f[2]; f_vf3.f[1] = acc.f[1] + s0.f[1] * s1.f[2]; f_vf3.f[2] = acc.f[2] + s0.f[2] * s1.f[2]; f_vf3.f[3] = acc.f[3] + s0.f[3] * s1.f[2]; }  // vmaddz.xyzw vf3, vf30, vf3
  { const Vf s0 = f_vf31, s1 = kVf0; acc.f[0] = s0.f[0] * s1.f[3]; acc.f[1] = s0.f[1] * s1.f[3]; acc.f[2] = s0.f[2] * s1.f[3]; acc.f[3] = s0.f[3] * s1.f[3]; }  // vmulaw.xyzw acc, vf31, vf0
  { const Vf s0 = f_vf28, s1 = f_vf4; acc.f[0] += s0.f[0] * s1.f[0]; acc.f[1] += s0.f[1] * s1.f[0]; acc.f[2] += s0.f[2] * s1.f[0]; acc.f[3] += s0.f[3] * s1.f[0]; }  // vmaddax.xyzw acc, vf28, vf4
  { const Vf s0 = f_vf29, s1 = f_vf4; acc.f[0] += s0.f[0] * s1.f[1]; acc.f[1] += s0.f[1] * s1.f[1]; acc.f[2] += s0.f[2] * s1.f[1]; acc.f[3] += s0.f[3] * s1.f[1]; }  // vmadday.xyzw acc, vf29, vf4
  { const Vf s0 = f_vf30, s1 = f_vf4; f_vf4.f[0] = acc.f[0] + s0.f[0] * s1.f[2]; f_vf4.f[1] = acc.f[1] + s0.f[1] * s1.f[2]; f_vf4.f[2] = acc.f[2] + s0.f[2] * s1.f[2]; f_vf4.f[3] = acc.f[3] + s0.f[3] * s1.f[2]; }  // vmaddz.xyzw vf4, vf30, vf4
  Q = f_vf25.f[2] / f_vf1.f[3];  // vdiv Q, vf25.z, vf1.w
  m.lq(x_t2, xh_t2, ((u32)x_v1 + (u32)(32)) & ~15u);  // lq t2, 32(v1)
  m.lq(x_t3, xh_t3, ((u32)x_v1 + (u32)(48)) & ~15u);  // lq t3, 48(v1)
  m.sq(((u32)x_a1 + (u32)(0)) & ~15u, x_t2, xh_t2);  // sq t2, 0(a1)
  m.sq(((u32)x_a1 + (u32)(16)) & ~15u, x_t3, xh_t3);  // sq t3, 16(a1)
  m.lqc2(f_vf5, (u32)x_v1 + (u32)(672));  // lqc2 vf5, 672(v1)
  m.lqc2(f_vf6, (u32)x_v1 + (u32)(688));  // lqc2 vf6, 688(v1)
  m.lqc2(f_vf7, (u32)x_v1 + (u32)(704));  // lqc2 vf7, 704(v1)
  { const Vf s0 = f_vf1; f_vf1.f[0] = s0.f[0] * Q; f_vf1.f[1] = s0.f[1] * Q; f_vf1.f[2] = s0.f[2] * Q; }  // vmulq.xyz vf1, vf1, Q
  m.sqc2((u32)x_a1 + (u32)(32), f_vf20);  // sqc2 vf20, 32(a1)
  { const Vf s0 = f_vf5; f_vf5.f[0] = s0.f[0] * Q; f_vf5.f[1] = s0.f[1] * Q; f_vf5.f[2] = s0.f[2] * Q; }  // vmulq.xyz vf5, vf5, Q
  Q = f_vf25.f[2] / f_vf2.f[3];  // vdiv Q, vf25.z, vf2.w
  m.lqc2(f_vf8, (u32)x_v1 + (u32)(720));  // lqc2 vf8, 720(v1)
  { const Vf s0 = f_vf1, s1 = f_vf27; f_vf1.f[0] = s0.f[0] + s1.f[0]; f_vf1.f[1] = s0.f[1] + s1.f[1]; f_vf1.f[2] = s0.f[2] + s1.f[2]; f_vf1.f[3] = s0.f[3] + s1.f[3]; }  // vadd.xyzw vf1, vf1, vf27
  m.lqc2(f_vf9, (u32)x_v1 + (u32)(608));  // lqc2 vf9, 608(v1)
  m.lqc2(f_vf10, (u32)x_v1 + (u32)(624));  // lqc2 vf10, 624(v1)
  m.lqc2(f_vf11, (u32)x_v1 + (u32)(640));  // lqc2 vf11, 640(v1)
  { const Vf s0 = f_vf1; set_i(f_vf1, 0, float_to_int_sat(s0.f[0] * 16.f)); set_i(f_vf1, 1, float_to_int_sat(s0.f[1] * 16.f)); set_i(f_vf1, 2, float_to_int_sat(s0.f[2] * 16.f)); set_i(f_vf1, 3, float_to_int_sat(s0.f[3] * 16.f)); }  // vftoi4.xyzw vf1, vf1
  { const Vf s0 = f_vf2; f_vf2.f[0] = s0.f[0] * Q; f_vf2.f[1] = s0.f[1] * Q; f_vf2.f[2] = s0.f[2] * Q; }  // vmulq.xyz vf2, vf2, Q
  { const Vf s0 = f_vf6; f_vf6.f[0] = s0.f[0] * Q; f_vf6.f[1] = s0.f[1] * Q; f_vf6.f[2] = s0.f[2] * Q; }  // vmulq.xyz vf6, vf6, Q
  m.sqc2((u32)x_a1 + (u32)(48), f_vf5);  // sqc2 vf5, 48(a1)
  Q = f_vf25.f[2] / f_vf3.f[3];  // vdiv Q, vf25.z, vf3.w
  m.sqc2((u32)x_a1 + (u32)(64), f_vf9);  // sqc2 vf9, 64(a1)
  m.lqc2(f_vf12, (u32)x_v1 + (u32)(656));  // lqc2 vf12, 656(v1)
  m.sqc2((u32)x_a1 + (u32)(80), f_vf1);  // sqc2 vf1, 80(a1)
  { const Vf s0 = f_vf2, s1 = f_vf27; f_vf2.f[0] = s0.f[0] + s1.f[0]; f_vf2.f[1] = s0.f[1] + s1.f[1]; f_vf2.f[2] = s0.f[2] + s1.f[2]; f_vf2.f[3] = s0.f[3] + s1.f[3]; }  // vadd.xyzw vf2, vf2, vf27
  { const Vf s0 = f_vf2; set_i(f_vf2, 0, float_to_int_sat(s0.f[0] * 16.f)); set_i(f_vf2, 1, float_to_int_sat(s0.f[1] * 16.f)); set_i(f_vf2, 2, float_to_int_sat(s0.f[2] * 16.f)); set_i(f_vf2, 3, float_to_int_sat(s0.f[3] * 16.f)); }  // vftoi4.xyzw vf2, vf2
  { const Vf s0 = f_vf3; f_vf3.f[0] = s0.f[0] * Q; f_vf3.f[1] = s0.f[1] * Q; f_vf3.f[2] = s0.f[2] * Q; }  // vmulq.xyz vf3, vf3, Q
  { const Vf s0 = f_vf7; f_vf7.f[0] = s0.f[0] * Q; f_vf7.f[1] = s0.f[1] * Q; f_vf7.f[2] = s0.f[2] * Q; }  // vmulq.xyz vf7, vf7, Q
  m.sqc2((u32)x_a1 + (u32)(96), f_vf6);  // sqc2 vf6, 96(a1)
  Q = f_vf25.f[2] / f_vf4.f[3];  // vdiv Q, vf25.z, vf4.w
  m.sqc2((u32)x_a1 + (u32)(112), f_vf10);  // sqc2 vf10, 112(a1)
  m.sqc2((u32)x_a1 + (u32)(128), f_vf2);  // sqc2 vf2, 128(a1)
  { const Vf s0 = f_vf3, s1 = f_vf27; f_vf3.f[0] = s0.f[0] + s1.f[0]; f_vf3.f[1] = s0.f[1] + s1.f[1]; f_vf3.f[2] = s0.f[2] + s1.f[2]; f_vf3.f[3] = s0.f[3] + s1.f[3]; }  // vadd.xyzw vf3, vf3, vf27
  { const Vf s0 = f_vf3; set_i(f_vf3, 0, float_to_int_sat(s0.f[0] * 16.f)); set_i(f_vf3, 1, float_to_int_sat(s0.f[1] * 16.f)); set_i(f_vf3, 2, float_to_int_sat(s0.f[2] * 16.f)); set_i(f_vf3, 3, float_to_int_sat(s0.f[3] * 16.f)); }  // vftoi4.xyzw vf3, vf3
  { const Vf s0 = f_vf4; f_vf4.f[0] = s0.f[0] * Q; f_vf4.f[1] = s0.f[1] * Q; f_vf4.f[2] = s0.f[2] * Q; }  // vmulq.xyz vf4, vf4, Q
  { const Vf s0 = f_vf8; f_vf8.f[0] = s0.f[0] * Q; f_vf8.f[1] = s0.f[1] * Q; f_vf8.f[2] = s0.f[2] * Q; }  // vmulq.xyz vf8, vf8, Q
  m.sqc2((u32)x_a1 + (u32)(144), f_vf7);  // sqc2 vf7, 144(a1)
  m.sqc2((u32)x_a1 + (u32)(160), f_vf11);  // sqc2 vf11, 160(a1)
  { const Vf s0 = f_vf4, s1 = f_vf27; f_vf4.f[0] = s0.f[0] + s1.f[0]; f_vf4.f[1] = s0.f[1] + s1.f[1]; f_vf4.f[2] = s0.f[2] + s1.f[2]; f_vf4.f[3] = s0.f[3] + s1.f[3]; }  // vadd.xyzw vf4, vf4, vf27
  m.sqc2((u32)x_a1 + (u32)(176), f_vf3);  // sqc2 vf3, 176(a1)
  x_t2 = x_a0 & (u64)(2);  // andi t2, a0, 2
  bc = x_t2 == 0;  // beq t2, r0, L115
  x_t2 = x_a0 & (u64)(64);  // andi t2, a0, 64
  if (bc) {goto block_228;}
  bc = x_t2 != 0;  // bne t2, r0, L115
  if (bc) {goto block_228;}
  { const Vf s0 = f_vf23, s1 = f_vf19; f_vf23.f[0] = s0.f[0] + s1.f[3]; }  // vaddw.x vf23, vf23, vf19
  goto block_229;
  block_228:
  { const Vf s0 = f_vf23, s1 = f_vf14; f_vf23.f[0] = s0.f[0] + s1.f[3]; }  // vaddw.x vf23, vf23, vf14
  block_229:
  { const Vf s0 = f_vf4; set_i(f_vf4, 0, float_to_int_sat(s0.f[0] * 16.f)); set_i(f_vf4, 1, float_to_int_sat(s0.f[1] * 16.f)); set_i(f_vf4, 2, float_to_int_sat(s0.f[2] * 16.f)); set_i(f_vf4, 3, float_to_int_sat(s0.f[3] * 16.f)); }  // vftoi4.xyzw vf4, vf4
  m.sqc2((u32)x_a1 + (u32)(192), f_vf8);  // sqc2 vf8, 192(a1)
  m.sqc2((u32)x_a1 + (u32)(208), f_vf12);  // sqc2 vf12, 208(a1)
  m.sqc2((u32)x_a1 + (u32)(224), f_vf4);  // sqc2 vf4, 224(a1)
  x_t3 = (u64)(s64)m.ld<s32>((u32)x_a1, 80);  // lw t3, 80(a1)
  x_t2 = (u64)(s64)m.ld<s32>((u32)x_a1, 84);  // lw t2, 84(a1)
  x_t4 = u64(0) | (u64)(36864);  // ori t4, r0, 36864
  x_t3 = x_t3 - x_t4;  // dsubu t3, t3, t4
  x_t4 = (u64)(s64)m.ld<s32>((u32)x_a1, 224);  // lw t4, 224(a1)
  x_t5 = u64(0) | (u64)(36096);  // ori t5, r0, 36096
  x_t2 = x_t2 - x_t5;  // dsubu t2, t2, t5
  x_t5 = (u64)(s64)m.ld<s32>((u32)x_a1, 228);  // lw t5, 228(a1)
  bc = ((s64)x_t3) > 0;  // bgtz t3, L91
  x_t3 = x_t4 + (u64)(s64)(-28672);  // daddiu t3, t4, -28672
  if (bc) {goto block_159;}
  bc = ((s64)x_t2) > 0;  // bgtz t2, L91
  x_t2 = x_t5 + (u64)(s64)(-29440);  // daddiu t2, t5, -29440
  if (bc) {goto block_159;}
  bc = ((s64)x_t3) < 0;  // bltz t3, L91
  if (bc) {goto block_159;}
  bc = ((s64)x_t2) < 0;  // bltz t2, L91
  if (bc) {goto block_159;}
  x_a1 = x_a1 + (u64)(s64)(240);  // daddiu a1, a1, 240
  goto block_159;
  block_234:
  x_v1 = (u64)(s64)m.ld<s32>((u32)x_v1, 2872);  // lw v1, 2872(v1)
  m.sw((u32)x_v1 + (u32)(4), (u32)x_a1);  // sw a1, 4(v1)
  m.lqc2(f_vf24, (u32)x_a2 + (u32)(12));  // lqc2 vf24, 12(a2)
  { const Vf s0 = f_vf23, s1 = f_vf24; f_vf23.f[0] = s0.f[0] - s1.f[0]; f_vf23.f[1] = s0.f[1] - s1.f[1]; f_vf23.f[2] = s0.f[2] - s1.f[2]; f_vf23.f[3] = s0.f[3] - s1.f[3]; }  // vsub.xyzw vf23, vf23, vf24
  gpr_from_vf(x_v0, xh_v0, f_vf23);  // qmfc2.i v0, vf23
  // (no-op)  // daddu sp, sp, r0
  goto end_of_function;
  end_of_function:
  return x_v0;
  // clang-format on
}

const NativeImpl draw_string_asm = MIPS2C_NATIVE_IMPL_GOAL(draw_string_asm_impl, 0);

}  // namespace Mips2C::jak2::native2
