/*!
 * @file native_joint.cpp
 * (AI-assisted)
 * Native versions of the mips2c functions in joint.cpp. See game/mips2c/mips2c_native.h for the
 * rules (same float operations, grouped the same way, as the mips2c code).
 */

#include "game/mips2c/jak1_functions/native_functions.h"

namespace Mips2C::jak1::native {

namespace {

//! r.xyz = (acc = q2.yzx * r.zxy) - r.yzx * q2.zxy: vopmula acc, vf6, r then vopmsub r, r, vf6
inline void opm_sub(float r[4], const float q2[4]) {
  const float a0 = q2[1] * r[2];
  const float a1 = q2[2] * r[0];
  const float a2 = q2[0] * r[1];
  const float n0 = a0 - r[1] * q2[2];
  const float n1 = a1 - r[2] * q2[0];
  const float n2 = a2 - r[0] * q2[1];
  r[0] = n0;
  r[1] = n1;
  r[2] = n2;
}

//! out = p0 * r.x + p1 * r.y + p2 * r.z + p3 * r.w (vmula, vmadda, vmadda, vmadd)
inline void transform(float out[4], const float p[4][4], const float r[4]) {
  for (int i = 0; i < 4; i++) {
    float acc = p[0][i] * r[0];
    acc += p[1][i] * r[1];
    acc += p[2][i] * r[2];
    out[i] = acc + p[3][i] * r[3];
  }
}

/*!
 * (cspace<-parented-transformq-joint! cspace transformq)
 * bone = parent bone * (rotation and scale of the transformq, translation), with the parent's scale
 * divided out of the rotation rows when the parent bone's scale w is nonzero. Also copies the
 * transformq's scale into the bone.
 */
u64 cspace_parented_transformq_joint_impl(const NativeArgs& args) {
  const u32 cspace = (u32)args.a[0];
  const u32 tq = (u32)args.a[1];
  const u32 parent = gload<u32>(cspace + 0);
  const u32 pbone = gload<u32>(parent + 16);
  const u32 bone = gload<u32>(cspace + 16);

  const Vec4f q = gload_vec(tq + 16);
  const Vec4f trans = gload_vec(tq + 0);
  const Vec4f scale = gload_vec(tq + 32);
  const float psx = gload<float>(pbone + 64);
  const float psy = gload<float>(pbone + 68);
  float p[4][4];
  memcpy(p, gptr(pbone), 64);
  // stored before the parent's scale z and w are read, like the original
  gstore_vec(bone + 64, scale);
  const float psz = gload<float>(pbone + 72);
  const u32 psw_bits = gload<u32>(pbone + 76);

  // rows of the quaternion's rotation matrix
  const float q2[4] = {q.x + q.x, q.y + q.y, q.z + q.z, q.w + q.w};
  float r0[4] = {0.f + q.w, 0.f + q.z, 0.f - q.y, 1.f - 1.f};
  float r1[4] = {0.f - q.z, 0.f + q.w, 0.f + q.x, 1.f - 1.f};
  float r2[4] = {0.f + q.y, 0.f - q.x, 0.f + q.w, 1.f - 1.f};
  const float inv_x = divs_accurate(1.f, psx);
  const float inv_y = divs_accurate(1.f, psy);
  const float inv_z = divs_accurate(1.f, psz);
  opm_sub(r0, q2);
  opm_sub(r1, q2);
  opm_sub(r2, q2);
  r0[0] = r0[0] + 1.f;
  r1[1] = r1[1] + 1.f;
  r2[2] = r2[2] + 1.f;

  for (int i = 0; i < 4; i++) {
    r0[i] = r0[i] * scale.x;
    r1[i] = r1[i] * scale.y;
    r2[i] = r2[i] * scale.z;
  }
  if (psw_bits != 0) {
    // divide by the parent's scale. w: the high word of the sign-extended 1/scale.z (0 or NaN).
    const float inv[4] = {inv_x, inv_y, inv_z, u2f((s32)f2u(inv_z) < 0 ? 0xffffffff : 0)};
    for (int i = 0; i < 4; i++) {
      r0[i] = r0[i] * inv[i];
      r1[i] = r1[i] * inv[i];
      r2[i] = r2[i] * inv[i];
    }
  }

  const float t[4] = {trans.x, trans.y, trans.z, 1.f};
  float out[4][4];
  transform(out[0], p, r0);
  transform(out[1], p, r1);
  transform(out[2], p, r2);
  transform(out[3], p, t);
  gstore_bytes(bone, out, 64);
  return 0;
}

}  // namespace

const NativeImpl cspace_parented_transformq_joint =
    MIPS2C_NATIVE_IMPL(cspace_parented_transformq_joint_impl, NATIVE_V0_UNDEFINED, 0);

}  // namespace Mips2C::jak1::native
