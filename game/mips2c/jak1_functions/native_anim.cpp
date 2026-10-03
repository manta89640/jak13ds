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

inline float s16f(u32 addr) {
  return (float)(s32)gload<s16>(addr);
}

//! x < 0.0 as goalc compares with a constant (gc_fcmp_bits): negative with a nonzero exponent
inline bool below_zero(float x) {
  return f2u(x) > 0x807fffffu;
}

//! the data streams of one part (fixed part or frame) of the animation
struct Stream {
  u32 d64, d32, d16;
  float amount;

  void init(u32 base, u32 offsets, float amt) {
    d64 = base + gload<u32>(offsets);
    d32 = base + gload<u32>(offsets + 4);
    d16 = base + gload<u32>(offsets + 8);
    amount = amt;
  }

  //! add-scaled-matrix! of the next matrix
  void matrix(u32 dst) {
    float d[16];
    float m[16];
    gload_q(d, dst);
    gload_q(d + 4, dst + 16);
    gload_q(d + 8, dst + 32);
    gload_q(d + 12, dst + 48);
    for (int i = 0; i < 16; i++) {
      m[i] = gload<float>(d64 + 4 * i);
    }
    for (int i = 0; i < 16; i++) {
      const float p = amount * m[i];
      d[i] = d[i] + p;
    }
    gstore_q(dst, d);
    gstore_q(dst + 16, d + 4);
    gstore_q(dst + 32, d + 8);
    gstore_q(dst + 48, d + 12);
    d64 += 64;
  }

  void trans(float t[3], bool big) {
    if (big) {
      const float x = gload<float>(d64);
      const float y = gload<float>(d64 + 4);
      const float z = gload<float>(d32);
      d64 += 8;
      d32 += 4;
      const float px = amount * x;
      const float py = amount * y;
      const float pz = amount * z;
      t[0] = t[0] + px;
      t[1] = t[1] + py;
      t[2] = t[2] + pz;
    } else {
      const float x = s16f(d32);
      const float y = s16f(d32 + 2);
      const float z = s16f(d16);
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

  void quat(float q[4]) {
    float t[4];
    for (int i = 0; i < 4; i++) {
      t[i] = s16f(d64 + 2 * i);
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

  void scale(float s[3]) {
    const float x = s16f(d32);
    const float y = s16f(d32 + 2);
    const float z = s16f(d16);
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
  const u32 out = (u32)args.a[0];
  // (C mode passes the low half: the count is small)
  const s64 num_joints = (s32)args.a[1];
  const u32 req = (u32)args.a[2];

  const u32 jacc = gload<u32>(req);
  const u32 frame = (u32)gload<u64>(req + 8);
  const float interp = gload<float>(req + 16);
  const float amount = gload<float>(req + 20);
  // the fixed part: a joint-anim-compressed-fixed, whose header is also the frames' header
  const u32 fixed = gload<u32>(jacc + 12);
  const u32 mbits = gload<u32>(fixed + 60);
  // (!= 0.0 frame-interp) on the bits, like goalc: denormals count as zero
  const bool two = (f2u(interp) & 0x7f800000u) != 0;

  Stream fx, f0, f1;
  fx.init(fixed + 80, fixed + 64, amount);
  const float w0 = 1.f - interp;
  f0.init(gload<u32>(jacc + 16 + 4 * frame) + 16, gload<u32>(jacc + 16 + 4 * frame), w0 * amount);
  if (two) {
    const u32 next = gload<u32>(jacc + 16 + 4 * (frame + 1));
    f1.init(next + 16, next, interp * amount);
  }

  // matrices (the GOAL passes do these before their joints; they are separate outputs)
  if (!(mbits & 1)) {
    fx.matrix(out);
  }
  if (!(mbits & 2)) {
    fx.matrix(out + 64);
  }
  if (mbits & 1) {
    f0.matrix(out);
  }
  if (mbits & 2) {
    f0.matrix(out + 64);
  }
  if (two) {
    if (mbits & 1) {
      f1.matrix(out);
    }
    if (mbits & 2) {
      f1.matrix(out + 64);
    }
  }

  u32 tq = out + 128;
  for (s64 i = 0; i < num_joints; i++, tq += 48) {
    const u32 ctrl = (gload<u32>(fixed + 4 * (u32)(i >> 3)) >> (4 * (i & 7))) & 0xf;
    if (ctrl & 1) {
      float t[3] = {gload<float>(tq), gload<float>(tq + 4), gload<float>(tq + 8)};
      f0.trans(t, ctrl & 8);
      if (two) {
        f1.trans(t, ctrl & 8);
      }
      gstore<float>(tq, t[0]);
      gstore<float>(tq + 4, t[1]);
      gstore<float>(tq + 8, t[2]);
    } else {
      float t[3] = {gload<float>(tq), gload<float>(tq + 4), gload<float>(tq + 8)};
      fx.trans(t, ctrl & 8);
      gstore<float>(tq, t[0]);
      gstore<float>(tq + 4, t[1]);
      gstore<float>(tq + 8, t[2]);
    }
    float q[4];
    gload_q(q, tq + 16);
    if (ctrl & 2) {
      f0.quat(q);
      if (two) {
        f1.quat(q);
      }
    } else {
      fx.quat(q);
    }
    gstore_q(tq + 16, q);
    float s[3] = {gload<float>(tq + 32), gload<float>(tq + 36), gload<float>(tq + 40)};
    if (ctrl & 4) {
      f0.scale(s);
      if (two) {
        f1.scale(s);
      }
    } else {
      fx.scale(s);
    }
    gstore<float>(tq + 32, s[0]);
    gstore<float>(tq + 36, s[1]);
    gstore<float>(tq + 40, s[2]);
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
  const u32 out = (u32)args.a[0];
  // (C mode passes the low half: the count is small)
  const s64 num_joints = (s32)args.a[1];
  u32 tq = out + 128;
  // two at a time: one's square root and divide overlap the other's multiplies
  s64 i = 0;
  for (; i + 1 < num_joints - 2; i += 2, tq += 96) {
    float q0[4], q1[4];
    gload_q(q0, tq + 16);
    gload_q(q1, tq + 64);
    normalize(q0);
    normalize(q1);
    gstore<float>(tq + 12, 1.f);
    gstore<float>(tq + 44, 1.f);
    gstore_q(tq + 16, q0);
    gstore<float>(tq + 60, 1.f);
    gstore<float>(tq + 92, 1.f);
    gstore_q(tq + 64, q1);
  }
  if (i < num_joints - 2) {
    float q[4];
    gload_q(q, tq + 16);
    normalize(q);
    gstore<float>(tq + 12, 1.f);
    gstore<float>(tq + 44, 1.f);
    gstore_q(tq + 16, q);
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
