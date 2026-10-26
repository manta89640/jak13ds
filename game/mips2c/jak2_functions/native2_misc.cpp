/*!
 * @file native2_misc.cpp
 * (AI-assisted)
 * Native versions of small Jak 2 mips2c functions: sparticle-motion-blur (sparticle_launcher.cpp)
 * and add-light-sphere-to-light-group (lights.cpp). See game/mips2c/mips2c_native.h for the rules:
 * the same float operations, grouped the same way as the mips2c code.
 */

#include <algorithm>
#include <cmath>

#include "native2.h"
#include "game/mips2c/native_goalc_ops.h"

namespace Mips2C::jak2::native2 {

namespace {

//! 1.0f that the compiler can't fold into a multiply (the mips2c code multiplies by a register
//! that holds 1.0, which quiets a signaling NaN like any multiply)
float g_one = 1.0f;

inline void load4(float out[4], u32 addr) {
  gload_q(out, addr);
}

inline void store4(u32 addr, const float v[4]) {
  gstore_q(addr, v);
}

//! the VU0 clip flags of xyz against +-|w| (ExecutionContext::clip, the low 6 bits)
inline u32 clip_bits(const float v[4]) {
  const float plus = std::abs(v[3]);
  const float minus = -plus;
  u32 r = 0;
  if (v[0] > plus) {
    r |= 1;
  }
  if (v[0] < minus) {
    r |= 2;
  }
  if (v[1] > plus) {
    r |= 4;
  }
  if (v[1] < minus) {
    r |= 8;
  }
  if (v[2] > plus) {
    r |= 16;
  }
  if (v[2] < minus) {
    r |= 32;
  }
  return r;
}

//! vmulax / vmadday / vmaddaz / vmaddw.xyzw with vf0 (w = 1): m[0..3] rows, p the vector
inline void transform(float out[4], const float m[4][4], const float p[4]) {
  for (int i = 0; i < 4; i++) {
    float acc = m[0][i] * p[0];
    acc += m[1][i] * p[1];
    acc += m[2][i] * p[2];
    out[i] = acc + m[3][i] * 1.f;
  }
}

}  // namespace

/*!
 * (sparticle-motion-blur system cpuinfo vecdata): particle callback (a1 = sparticle-cpuinfo,
 * a2 = the sprite's vecdata). If the velocity (cpuinfo + 16) isn't zero, the particle's position and
 * the position 32 frames of velocity on are projected to the screen; when both are inside the
 * clip volume the sprite is rotated along the screen motion (atan) and stretched by its length.
 * Otherwise its scale (vecdata + 12) is set to 0 unless cpuinfo + 12 is 0.
 */
u64 sparticle_motion_blur_impl(const NativeArgs& args) {
  static const u32 math_camera_sym = sym_value_addr("*math-camera*");
  static const u32 atan_sym = sym_value_addr("atan");
  const u32 info = (u32)args.a[1];
  const u32 vec = (u32)args.a[2];
  const u32 cam = gload<u32>(math_camera_sym);

  // lq a0, 16(s3): the velocity; zero in x, y and z (as words) means no motion
  u32 vel_bits[4];
  gload_q(vel_bits, (info + 16) & ~15u);
  float pos[4];
  load4(pos, vec);

  bool done = false;
  if (vel_bits[0] != 0 || vel_bits[1] != 0 || vel_bits[2] != 0) {
    float m[4][4];
    load4(m[0], cam + 572);
    load4(m[1], cam + 588);
    load4(m[2], cam + 604);
    load4(m[3], cam + 620);
    float hvdf[4], hmge[4];
    load4(hvdf, cam + 812);
    load4(hmge, cam + 780);
    float vel[4];
    memcpy(vel, vel_bits, 16);

    float p10[4], p12[4];
    transform(p10, m, pos);
    for (int i = 0; i < 4; i++) {
      p12[i] = p10[i] * hmge[i];
    }
    // vf1 = pos + vel * 32
    float pos2[4];
    for (int i = 0; i < 4; i++) {
      const float acc = pos[i] * 1.f;
      pos2[i] = acc + vel[i] * 32.f;
    }
    float q = 1.f / p12[3];
    const u32 clip1 = clip_bits(p12);
    float p11[4], p13[4];
    transform(p11, m, pos2);
    for (int i = 0; i < 3; i++) {
      p10[i] = p10[i] * q;
    }
    for (int i = 0; i < 4; i++) {
      p13[i] = p11[i] * hmge[i];
    }
    for (int i = 0; i < 4; i++) {
      p10[i] = p10[i] + hvdf[i];
    }
    if (clip1 == 0) {
      q = 1.f / p13[3];
      const u32 clip2 = clip_bits(p13);
      p10[3] = std::max(p10[3], 0.f);
      s32 i2[4];
      for (int i = 0; i < 4; i++) {
        i2[i] = p10[i] * 16.f;
      }
      for (int i = 0; i < 3; i++) {
        p11[i] = p11[i] * q;
      }
      float f6[4];
      for (int i = 0; i < 4; i++) {
        f6[i] = i2[i];
      }
      for (int i = 0; i < 4; i++) {
        p11[i] = p11[i] + hvdf[i];
      }
      q = 1.f / f6[2];
      if (clip2 == 0) {
        p11[3] = std::max(p11[3], 0.f);
        float scale = 0.f + 1.f;  // vaddw.x vf9, vf0, vf0
        s32 i3[4];
        for (int i = 0; i < 4; i++) {
          i3[i] = p11[i] * 16.f;
        }
        float f7[2];
        for (int i = 0; i < 2; i++) {
          f7[i] = i3[i];
        }
        const float dx = f7[0] - f6[0];
        const float dy = f7[1] - f6[1];
        const u64 s4 = (u64)f2u(dx) | ((u64)f2u(dy) << 32);
        const u64 s5 = (u64)(s64)(s32)f2u(dy);
        scale = scale * q;
        // (AI-assisted) atan inlined (native_goalc_ops.h: goalc's C of the GOAL function, the same
        // bits) while the symbol holds the function it held at the first call; otherwise the call
        static const u32 atan_fn = gload<u32>(atan_sym);
        float angle;
        if (gload<u32>(atan_sym) == atan_fn) {
          angle = goalc_ops::atan(dx, dy);
        } else {
          const u64 call_args[8] = {s4, s5, 0, args.a[3], args.a[4], args.a[5], args.a[6], args.a[7]};
          angle = u2f((u32)native_call_goal(gload<u32>(atan_sym), call_args, args));
        }
        const u32 omega = gload<u32>(info + 12);
        gstore<float>(vec + 24, -16384.f + angle);
        if (omega != 0) {
          float f2 = scale;
          const float c3 = u2f(0x358637bd);
          const float c4 = u2f(0x34196446);
          f2 = f2 - c3;
          const float range = c4 - c3;
          f2 = f2 / range;
          float len2 = (dx * dx) + (dy * dy);
          f2 = std::max(f2, 0.f);
          const float len = std::sqrt(std::abs(len2));
          f2 = std::min(f2, 1.f);
          float f0 = 1.f - f2;
          f0 = (f0 * 3.f) + (f2 * 0.25f);
          f0 = f0 * len;
          f0 = f0 * u2f(omega);
          gstore<float>(vec + 12, f0);
        }
        done = true;
      }
    }
  }
  if (!done) {
    // block_5
    if (!(gload<float>(info + 12) == 0.f)) {
      gstore<float>(vec + 12, 0.f);
    }
  }
  return 0;
}

// atan (GOAL) stores nothing outside its stack: the logged stores are the whole result
const NativeImpl sparticle_motion_blur = MIPS2C_NATIVE_IMPL(sparticle_motion_blur_impl, 0, 0);

/*!
 * (add-light-sphere-to-light-group group sphere pos palette): adds a light sphere's light at pos to
 * the light group (three directional lights at +0 / +48 / +96: direction, color, levels; the
 * ambient at +160). The sphere's falloff (decay start, brightness, the palette entry at
 * palette + 16 * index + 1660) gives the factor; the light goes into the first empty slot, or is
 * blended into the existing directions by weight.
 */
u64 add_light_sphere_to_light_group_impl(const NativeArgs& args) {
  const u32 a0 = (u32)args.a[0];
  const u32 sph = (u32)args.a[1];
  const u32 pos_addr = (u32)args.a[2];
  const u32 a3 = (u32)args.a[3];
  const float one = g_one;  // vf1 (vmaxw.xyzw vf1, vf0, vf0: all 1)

  float v2[4], v3[4], v19[4], v4[4], v6[4], v5[4];
  float v8[4], v9[4], v10[4], v11[4], v25[4], v26[4], v27[4];
  float v15[4], v16[4], v17[4], v18[4];
  load4(v2, pos_addr);
  load4(v3, sph + 16);
  load4(v19, sph);
  for (int i = 0; i < 4; i++) {
    v4[i] = v3[i] - v2[i];
  }
  load4(v8, a0 + 16);
  load4(v9, a0 + 64);
  load4(v10, a0 + 112);
  load4(v11, a0 + 160);
  for (int i = 0; i < 4; i++) {
    v6[i] = v4[i] * v4[i];
  }
  load4(v5, sph + 32);
  load4(v25, a0);
  load4(v26, a0 + 48);
  load4(v27, a0 + 96);
  {
    float acc = 1.f * v6[0];
    acc += 1.f * v6[1];
    v6[3] = acc + 1.f * v6[2];
  }
  load4(v15, a0 + 32);
  load4(v16, a0 + 80);
  load4(v17, a0 + 128);
  float q = std::sqrt(std::abs(v6[3]));
  load4(v18, a0 + 176);
  for (int i = 0; i < 4; i++) {
    v8[i] = v8[i] * v15[0];
  }
  const float f2 = gload<float>(sph + 28);
  for (int i = 0; i < 4; i++) {
    v9[i] = v9[i] * v16[0];
  }
  const float f3 = gload<float>(sph + 4);
  for (int i = 0; i < 4; i++) {
    v10[i] = v10[i] * v17[0];
  }
  const float f4 = gload<float>(sph + 44);
  for (int i = 0; i < 4; i++) {
    v11[i] = v11[i] * v18[0];
  }
  v19[3] = 1.f - v19[2];
  const float f1 = one;
  for (int i = 0; i < 4; i++) {
    v25[i] = v25[i] * v15[0];
  }
  float f6 = gload<float>(sph + 12);
  for (int i = 0; i < 4; i++) {
    v26[i] = v26[i] * v16[0];
  }
  const s64 idx = (s8)gload<u8>(sph + 63);
  const float f7 = f1 - f3;
  float v21[4], v20[4];
  load4(v21, sph + 48);
  for (int i = 0; i < 4; i++) {
    v20[i] = v21[i] * v19[2];
  }
  const u32 pal = (u32)((idx << 4) + (s64)a3);
  for (int i = 0; i < 4; i++) {
    v21[i] = v21[i] * v19[3];
  }
  for (int i = 0; i < 4; i++) {
    v27[i] = v27[i] * v17[0];
  }
  float v7w = one * q;  // vmulq.xyzw vf7, vf1, Q (all lanes equal)
  // the x lanes of the old vf15 / vf16 / vf17 (as words): the slots in use
  const u32 x15 = f2u(v15[0]);
  const u32 x16 = f2u(v16[0]);
  const u32 x17 = f2u(v17[0]);
  q = 1.f / v7w;
  float f5 = v7w;
  const bool outside = f2 < f5;
  f5 = f5 / f2;
  if (outside) {
    return 0;
  }
  if (f4 == 0.f) {
    for (int i = 0; i < 4; i++) {
      v6[i] = v4[i] * q;
    }
  } else {
    memcpy(v6, v5, 16);
  }
  float f8 = f5 - f3;
  if (!(f3 == f1)) {
    f8 = std::max(f8, 0.f);
    f8 = f8 / f7;
    f8 = f1 - f8;
    f6 = f6 * f8;
  }
  if (idx >= 0) {
    f6 = f6 * gload<float>(pal + 1660);
  }
  if (f2u(f6) == 0) {
    return 0;
  }
  // vf23: the gpr a3 moved in: x = f6, y = its sign extension, z w = the old vf15's
  float v23[4];
  v23[0] = f6;
  v23[1] = u2f((s32)f2u(f6) < 0 ? 0xffffffffu : 0u);
  v23[2] = v15[2];
  v23[3] = v15[3];
  for (int i = 0; i < 4; i++) {
    v21[i] = v21[i] * v23[0];
  }

  auto store_slot = [&](u32 off) {
    for (int i = 0; i < 4; i++) {
      v11[i] = v11[i] + v21[i];
    }
    gstore<float>(a0 + 176, f1);
    store4(a0 + off, v6);
    store4(a0 + off + 16, v20);
    gstore<float>(a0 + off + 32, f6);
    store4(a0 + 160, v11);
  };
  if (x15 == 0) {
    store_slot(0);
    return 0;
  }
  if (x16 == 0) {
    store_slot(48);
    return 0;
  }
  if (x17 == 0) {
    store_slot(96);
    return 0;
  }

  // all three in use: blend by weight
  float v22[4];
  for (int i = 0; i < 4; i++) {
    float acc = v15[i] * v15[0];
    acc += v16[i] * v16[0];
    v22[i] = acc + v17[i] * v17[0];
  }
  for (int i = 0; i < 4; i++) {
    v11[i] = v11[i] + v21[i];
  }
  gstore<float>(a0 + 176, f1);
  for (int i = 0; i < 4; i++) {
    v20[i] = v20[i] * v23[0];
  }
  for (int i = 0; i < 4; i++) {
    v6[i] = v6[i] * v23[0];
  }
  {
    const float s = std::sqrt(std::abs(v22[0]));
    q = (s == 0) ? 0.f : 1.f / s;
  }
  store4(a0 + 160, v11);
  for (int i = 0; i < 4; i++) {
    v15[i] = v15[i] * q;
    v16[i] = v16[i] * q;
    v17[i] = v17[i] * q;
  }
  {
    const float x5 = v15[0], x6 = v16[0], x7 = v17[0];
    for (int i = 0; i < 4; i++) {
      v15[i] = one - x5;
      v16[i] = one - x6;
      v17[i] = one - x7;
    }
  }
  for (int i = 0; i < 4; i++) {
    float acc = one * v15[0];
    acc += one * v16[0];
    v22[i] = acc + one * v17[0];
  }
  q = 1.f / v22[0];
  float v12[4], v13[4], v14[4];
  load4(v12, a0 + 32);
  load4(v13, a0 + 80);
  load4(v14, a0 + 128);
  for (int i = 0; i < 4; i++) {
    v15[i] = v15[i] * q;
    v16[i] = v16[i] * q;
    v17[i] = v17[i] * q;
  }
  for (int i = 0; i < 4; i++) {
    float acc = one * v25[i];
    v25[i] = acc + v6[i] * v15[0];
  }
  for (int i = 0; i < 4; i++) {
    float acc = one * v26[i];
    v26[i] = acc + v6[i] * v16[0];
  }
  for (int i = 0; i < 4; i++) {
    float acc = one * v27[i];
    v27[i] = acc + v6[i] * v17[0];
  }
  const u32 y15 = f2u(v15[0]);
  const u32 y16 = f2u(v16[0]);
  const u32 y17 = f2u(v17[0]);
  for (int i = 0; i < 4; i++) {
    float acc = one * v8[i];
    v8[i] = acc + v20[i] * v15[0];
  }
  for (int i = 0; i < 4; i++) {
    float acc = one * v9[i];
    v9[i] = acc + v20[i] * v16[0];
  }
  for (int i = 0; i < 4; i++) {
    float acc = one * v10[i];
    v10[i] = acc + v20[i] * v17[0];
  }

  // renormalize each direction and scale its color by 1 / its new weight
  auto finish = [&](float* vdir, float* vw, const float* vlev, float* vcol, u32 off) {
    float v24[4];
    for (int i = 0; i < 4; i++) {
      v24[i] = vdir[i] * vdir[i];
    }
    // only the x lane of vw is used (stored as a word, then 1 / it). vmul then vadd: two
    // roundings, not fused
    float w = vw[0] * v23[0];
    {
      float acc = one * v24[0];
      acc += one * v24[1];
      const float x = acc + one * v24[2];
      const float s = std::sqrt(std::abs(x));
      q = (s == 0) ? 0.f : 1.f / s;
    }
    w = w + vlev[0];
    const float inv = f1 / w;
    gstore<u32>(a0 + off + 32, f2u(w));
    for (int i = 0; i < 4; i++) {
      vcol[i] = vcol[i] * inv;
    }
    for (int i = 0; i < 4; i++) {
      vdir[i] = vdir[i] * q;
    }
    store4(a0 + off + 16, vcol);
    store4(a0 + off, vdir);
  };
  if (y15 != 0) {
    finish(v25, v15, v12, v8, 0);
  }
  if (y16 != 0) {
    finish(v26, v16, v13, v9, 48);
  }
  if (y17 != 0) {
    finish(v27, v17, v14, v10, 96);
  }
  return 0;
}

const NativeImpl add_light_sphere_to_light_group =
    MIPS2C_NATIVE_IMPL(add_light_sphere_to_light_group_impl, 0, 0);

}  // namespace Mips2C::jak2::native2

namespace Mips2C::jak2::native2 {
/*!
 * (AI-assisted) (mem-copy! dst src size) -> dst: gcommon.gc's increasing-address byte copy, with
 * memmove when the ranges don't overlap that way (dst <= src, or no overlap). A copy forward onto
 * an overlapping higher dst repeats the source bytes, as the GOAL loop does, so that case stays a
 * byte loop.
 */
u64 mem_copy_impl(const NativeArgs& args) {
  const u32 dst = (u32)args.a[0];
  const u32 src = (u32)args.a[1];
  const s64 n = (s64)args.a[2];
  if (n > 0) {
    MIPS2C_NATIVE_LOG_STORE(dst, (u32)n);
    u8* const m = g_ee_main_mem;
    if (dst <= src || (u64)(dst - src) >= (u64)n) {
      memmove(m + dst, m + src, (size_t)n);
    } else {
      for (s64 i = 0; i < n; i++) {
        m[dst + i] = m[src + i];
      }
    }
  }
  return args.a[0];
}

const NativeImpl mem_copy = MIPS2C_NATIVE_IMPL(mem_copy_impl, 0, 0);
}  // namespace Mips2C::jak2::native2

namespace Mips2C::jak2::mem_copy {
/*!
 * (AI-assisted) The reference for the native mem-copy! (verify mode compares the two;
 * OPENGOAL_MIPS2C_NATIVE=0 runs it): the GOAL function's loop, one byte at a time.
 */
u64 execute(void* ctxt) {
  auto* c = (ExecutionContext*)ctxt;
  const u32 dst = c->gprs[a0].du32[0];
  const u32 src = c->gprs[a1].du32[0];
  const s64 n = c->gprs[a2].ds64[0];
  for (s64 i = 0; i < n; i++) {
    MIPS2C_LOG_STORE(dst + i, 1);
    g_ee_main_mem[dst + i] = g_ee_main_mem[src + i];
  }
  c->gprs[v0].du64[0] = c->gprs[a0].du64[0];
  return c->gprs[v0].du64[0];
}

void link() {
  gLinkedFunctionTable.reg("mem-copy!", execute, 0, &native2::mem_copy);
}
}  // namespace Mips2C::jak2::mem_copy
