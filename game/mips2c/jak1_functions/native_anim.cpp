/*!
 * @file native_anim.cpp
 * (AI-assisted)
 * Native versions of GOAL functions of the joint decompressor (anim/joint.gc): decomp-request
 * (decomp-fixed, then decomp-frame for the frame and, when interpolating, the next one) and
 * finalize-joint-frame. They give the same results as goalc's C code of the GOAL functions
 * (test/mips2c_native/tests_anim.cpp): the same float operations in the same order, each on its
 * own (the GOAL code has no multiply-adds; clang would fuse an a + b * c written as one expression).
 *
 * The GOAL code runs three passes over the joints, each loading and storing every value it adds
 * to. Every transformq's trans, quat and scale come either from the fixed part or from the
 * frames (the same control bits pick it), so one pass per joint gives the same sums in the same
 * order, with the values kept in registers in between.
 */

#include "game/mips2c/jak1_functions/native_functions.h"

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace Mips2C::jak1::native {

namespace {

constexpr float kTransScale = 4.f;
constexpr float kQuatScale = 0.000030517578125f;  // QUAT_SCALE
constexpr float kScaleScale = 0.000244140625f;    // SCALE_SCALE

// GOAL memory through a base pointer kept in a register (gload / gstore read g_ee_main_mem again
// after every store, since a store could change it as far as the compiler knows)
template <typename T>
inline T ld(const u8* m, u32 addr) {
  T v;
  memcpy(&v, m + addr, sizeof(T));
  return v;
}
template <typename T>
inline void st(u8* m, u32 addr, T v) {
  MIPS2C_NATIVE_LOG_STORE(addr, (u32)sizeof(T));
  memcpy(m + addr, &v, sizeof(T));
}
inline void ld4(float out[4], const u8* m, u32 addr) {
  memcpy(out, MIPS2C_ASSUME_ALIGNED(m + addr, 16), 16);
}
inline void st4(u8* m, u32 addr, const float in[4]) {
  MIPS2C_NATIVE_LOG_STORE(addr, 16);
  memcpy(MIPS2C_ASSUME_ALIGNED(m + addr, 16), in, 16);
}

inline float s16f(const u8* m, u32 addr) {
  return (float)(s32)ld<s16>(m, addr);
}

//! x < 0.0 as goalc compares with a constant (gc_fcmp_bits): negative with a nonzero exponent
inline bool below_zero(float x) {
  return f2u(x) > 0x807fffffu;
}

//! the data streams of one part (fixed part or frame) of the animation
struct Stream {
  u32 d64, d32, d16;
  float amount;

  void init(const u8* m, u32 base, u32 offsets, float amt) {
    d64 = base + ld<u32>(m, offsets);
    d32 = base + ld<u32>(m, offsets + 4);
    d16 = base + ld<u32>(m, offsets + 8);
    amount = amt;
  }

  //! add-scaled-matrix! of the next matrix
  void matrix(u8* m, u32 dst) {
    float d[16];
    float src[16];
    for (int i = 0; i < 16; i += 4) {
      ld4(d + i, m, dst + 4 * i);
    }
    for (int i = 0; i < 16; i++) {
      src[i] = ld<float>(m, d64 + 4 * i);
    }
    for (int i = 0; i < 16; i++) {
      const float p = amount * src[i];
      d[i] = d[i] + p;
    }
    for (int i = 0; i < 16; i += 4) {
      st4(m, dst + 4 * i, d + i);
    }
    d64 += 64;
  }

  void trans(const u8* m, float t[3], bool big) {
    if (big) {
      const float x = ld<float>(m, d64);
      const float y = ld<float>(m, d64 + 4);
      const float z = ld<float>(m, d32);
      d64 += 8;
      d32 += 4;
      const float px = amount * x;
      const float py = amount * y;
      const float pz = amount * z;
      t[0] = t[0] + px;
      t[1] = t[1] + py;
      t[2] = t[2] + pz;
    } else {
      const float x = s16f(m, d32);
      const float y = s16f(m, d32 + 2);
      const float z = s16f(m, d16);
      d32 += 4;
      d16 += 2;
      float px = amount * x;
      float py = amount * y;
      float pz = amount * z;
      px = px * kTransScale;
      py = py * kTransScale;
      pz = pz * kTransScale;
      t[0] = t[0] + px;
      t[1] = t[1] + py;
      t[2] = t[2] + pz;
    }
  }

  void quat(const u8* m, float q[4]) {
    float t[4];
    for (int i = 0; i < 4; i++) {
      t[i] = s16f(m, d64 + 2 * i);
    }
    d64 += 8;
    // ((x x' + y y') + z z') + w w': the sign that keeps the sum on the same hemisphere
    const float p0 = t[0] * q[0];
    const float p1 = t[1] * q[1];
    const float p2 = t[2] * q[2];
    const float p3 = t[3] * q[3];
    float dot = p0 + p1;
    dot = dot + p2;
    dot = dot + p3;
    if (below_zero(dot)) {
      for (int i = 0; i < 4; i++) {
        t[i] = -t[i];
      }
    }
    for (int i = 0; i < 4; i++) {
      float p = amount * t[i];
      p = p * kQuatScale;
      q[i] = q[i] + p;
    }
  }

  void scale(const u8* m, float s[3]) {
    const float x = s16f(m, d32);
    const float y = s16f(m, d32 + 2);
    const float z = s16f(m, d16);
    d32 += 4;
    d16 += 2;
    float px = amount * x;
    float py = amount * y;
    float pz = amount * z;
    px = px * kScaleScale;
    py = py * kScaleScale;
    pz = pz * kScaleScale;
    s[0] = s[0] + px;
    s[1] = s[1] + py;
    s[2] = s[2] + pz;
  }
};

/*!
 * (decomp-request output-frame num-joints request): adds the request's animation (fixed part,
 * frame, and the next frame weighted by frame-interp) times its amount to the output frame.
 */
u64 decomp_request_impl(const NativeArgs& args) {
  u8* const m = g_ee_main_mem;
  const u32 out = (u32)args.a[0];
  // (C mode passes the low half: the count is small)
  const s64 num_joints = (s32)args.a[1];
  const u32 req = (u32)args.a[2];

  const u32 jacc = ld<u32>(m, req);
  const u32 frame = (u32)ld<u64>(m, req + 8);
  const float interp = ld<float>(m, req + 16);
  const float amount = ld<float>(m, req + 20);
  // the fixed part: a joint-anim-compressed-fixed, whose header is also the frames' header
  const u32 fixed = ld<u32>(m, jacc + 12);
  const u32 mbits = ld<u32>(m, fixed + 60);
  // (!= 0.0 frame-interp) on the bits, like goalc: denormals count as zero
  const bool two = (f2u(interp) & 0x7f800000u) != 0;

  Stream fx, f0, f1;
  fx.init(m, fixed + 80, fixed + 64, amount);
  const float w0 = 1.f - interp;
  const u32 cur = ld<u32>(m, jacc + 16 + 4 * frame);
  f0.init(m, cur + 16, cur, w0 * amount);
  if (two) {
    const u32 next = ld<u32>(m, jacc + 16 + 4 * (frame + 1));
    f1.init(m, next + 16, next, interp * amount);
  }

  // matrices (the GOAL passes do these before their joints; they are separate outputs)
  if (!(mbits & 1)) {
    fx.matrix(m, out);
  }
  if (!(mbits & 2)) {
    fx.matrix(m, out + 64);
  }
  if (mbits & 1) {
    f0.matrix(m, out);
  }
  if (mbits & 2) {
    f0.matrix(m, out + 64);
  }
  if (two) {
    if (mbits & 1) {
      f1.matrix(m, out);
    }
    if (mbits & 2) {
      f1.matrix(m, out + 64);
    }
  }

  u32 tq = out + 128;
  for (s64 i = 0; i < num_joints; i++, tq += 48) {
    const u32 ctrl = (ld<u32>(m, fixed + 4 * (u32)(i >> 3)) >> (4 * (i & 7))) & 0xf;
    float t[3] = {ld<float>(m, tq), ld<float>(m, tq + 4), ld<float>(m, tq + 8)};
    if (ctrl & 1) {
      f0.trans(m, t, ctrl & 8);
      if (two) {
        f1.trans(m, t, ctrl & 8);
      }
    } else {
      fx.trans(m, t, ctrl & 8);
    }
    st<float>(m, tq, t[0]);
    st<float>(m, tq + 4, t[1]);
    st<float>(m, tq + 8, t[2]);
    float q[4];
    ld4(q, m, tq + 16);
    if (ctrl & 2) {
      f0.quat(m, q);
      if (two) {
        f1.quat(m, q);
      }
    } else {
      fx.quat(m, q);
    }
    st4(m, tq + 16, q);
    float s[3] = {ld<float>(m, tq + 32), ld<float>(m, tq + 36), ld<float>(m, tq + 40)};
    if (ctrl & 4) {
      f0.scale(m, s);
      if (two) {
        f1.scale(m, s);
      }
    } else {
      fx.scale(m, s);
    }
    st<float>(m, tq + 32, s[0]);
    st<float>(m, tq + 36, s[1]);
    st<float>(m, tq + 40, s[2]);
  }
  return 0;
}

//! quaternion-normalize!: q * (1 / sqrt(((q.w^2 + q.z^2) + q.y^2) + q.x^2))
inline void normalize(float q[4]) {
  float sq[4];
  for (int i = 0; i < 4; i++) {
    sq[i] = q[i] * q[i];
  }
  float len2 = sq[3] + sq[2];
  len2 = len2 + sq[1];
  len2 = len2 + sq[0];
  const float inv = 1.f / __builtin_sqrtf(len2);
  for (int i = 0; i < 4; i++) {
    q[i] = q[i] * inv;
  }
}

/*!
 * (finalize-joint-frame output-frame num-joints): trans.w and scale.w = 1, quaternion
 * normalized, for the num-joints - 2 transformqs.
 */
u64 finalize_frame_impl(const NativeArgs& args) {
  u8* const m = g_ee_main_mem;
  const u32 out = (u32)args.a[0];
  const s64 num_joints = (s32)args.a[1];
  u32 tq = out + 128;
  // two at a time: one's square root and divide overlap the other's multiplies
  s64 i = 0;
  for (; i + 1 < num_joints - 2; i += 2, tq += 96) {
    float q0[4], q1[4];
    ld4(q0, m, tq + 16);
    ld4(q1, m, tq + 64);
    normalize(q0);
    normalize(q1);
    st<float>(m, tq + 12, 1.f);
    st<float>(m, tq + 44, 1.f);
    st4(m, tq + 16, q0);
    st<float>(m, tq + 60, 1.f);
    st<float>(m, tq + 92, 1.f);
    st4(m, tq + 64, q1);
  }
  if (i < num_joints - 2) {
    float q[4];
    ld4(q, m, tq + 16);
    normalize(q);
    st<float>(m, tq + 12, 1.f);
    st<float>(m, tq + 44, 1.f);
    st4(m, tq + 16, q);
  }
  return 0;
}

}  // namespace

// GOAL code calls them directly (MIPS2C_NATIVE_IMPL_GOAL): once per animation request / per
// animated process and frame
const NativeImpl anim_decomp_request =
    MIPS2C_NATIVE_IMPL_GOAL(decomp_request_impl, NATIVE_V0_UNDEFINED);
const NativeImpl anim_finalize_frame =
    MIPS2C_NATIVE_IMPL_GOAL(finalize_frame_impl, NATIVE_V0_UNDEFINED);

}  // namespace Mips2C::jak1::native

// GOAL functions with only a native version (def-mips2c in joint.gc; the GOAL versions are
// decomp-request and finalize-joint-frame, which joint.gc runs with anim_native = off).
namespace Mips2C::jak1 {
namespace pc_anim_decomp_request {
void link() {
  gLinkedFunctionTable.reg("pc-anim-decomp-request", native::anim_decomp_request.as_exec, 64,
                           &native::anim_decomp_request);
}
}  // namespace pc_anim_decomp_request
namespace pc_anim_finalize_frame {
void link() {
  gLinkedFunctionTable.reg("pc-anim-finalize-frame", native::anim_finalize_frame.as_exec, 64,
                           &native::anim_finalize_frame);
}
}  // namespace pc_anim_finalize_frame
}  // namespace Mips2C::jak1
