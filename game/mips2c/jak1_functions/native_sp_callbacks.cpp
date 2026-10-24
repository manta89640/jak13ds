/*!
 * @file native_sp_callbacks.cpp
 * (AI-assisted)
 * Jak 1: GOAL functions that sp-process-block-2d / -3d call for single particles, run inline
 * instead of through native_call_goal (an adapter thunk into goalc's C and back for every
 * particle): sp-orbiter (sin, cos, quaternion*!, quaternion-normalize!, quaternion->matrix,
 * vector-matrix*!) and sp-relaunch-particle-2d (with sp-relaunch-setup-fields and
 * pc-sp-init-fields!; its time of day color case stays GOAL).
 *
 * Each is goalc's C of the GOAL function (out/jak1/csrc/sparticle.c, sparticle-launcher.c): the
 * same loads, float operations in the same order, compares (native_goalc_ops.h), inline while the
 * symbols of the function and of what it calls hold what they held at the first call; otherwise
 * the GOAL call. Verify mode (OPENGOAL_MIPS2C_VERIFY=1) runs the GOAL function and compares each
 * call (native_verify_inline).
 */

#include <cstring>

#include "game/kernel/jak1/kscheme.h"
#include "game/mips2c/jak1_functions/native_functions.h"
#include "game/mips2c/native_goalc_ops.h"

namespace Mips2C::jak1::native {

namespace {

namespace go = ::Mips2C::goalc_ops;

//! a symbol (its value is at the symbol in Jak 1) and the function it held at the first call
struct FnSym {
  const char* name;
  u32 sym = 0;
  u32 fn = 0;
  bool resolved = false;
  explicit FnSym(const char* n) : name(n) {}
  bool unchanged() {
    if (!resolved) {
      resolved = true;
      sym = ::jak1::intern_from_c(name).offset;
      fn = gload<u32>(sym);
    }
    return fn != 0 && gload<u32>(sym) == fn;
  }
};

FnSym s_orbiter("sp-orbiter");
FnSym s_sin("sin");
FnSym s_cos("cos");
FnSym s_qmul("quaternion*!");
FnSym s_qnorm("quaternion-normalize!");
FnSym s_qmat("quaternion->matrix");
FnSym s_vmat("vector-matrix*!");
FnSym s_relaunch_2d("sp-relaunch-particle-2d");
FnSym s_relaunch_setup("sp-relaunch-setup-fields");
FnSym s_init_fields("sp-init-fields!");

u32 sym_value(const char* name) {
  return gload<u32>(::jak1::intern_from_c(name).offset);
}

go::v4 load4(u32 a) {
  return {gload<float>(a), gload<float>(a + 4), gload<float>(a + 8), gload<float>(a + 12)};
}
void store4(u32 a, go::v4 v) {
  gstore<float>(a, v.x);
  gstore<float>(a + 4, v.y);
  gstore<float>(a + 8, v.z);
  gstore<float>(a + 12, v.w);
}

bool orbiter_ok() {
  return s_orbiter.unchanged() && s_sin.unchanged() && s_cos.unchanged() && s_qmul.unchanged() &&
         s_qnorm.unchanged() && s_qmat.unchanged() && s_vmat.unchanged();
}

//! sp-orbiter (system info pos): the orbit angle and radius advance, the orbit's rotation
//! (rotvel3d, info + 80) turns, and pos is the point on the orbit around the center that
//! user-float points at
void orbiter(u32 info, u32 pos) {
  GC_NATIVE_FP_EXACT
  static const u32 frame_time_sym = ::jak1::intern_from_c("*sp-frame-time*").offset;
  const float omega = gload<float>(info + 12);
  const float radius = gload<float>(info + 8);
  const float vx = gload<float>(info + 16);
  const float vy = gload<float>(info + 20);
  const float vz = gload<float>(info + 24);
  const float ft = gload<float>(gload<u32>(frame_time_sym) + 4);
  const float pv = vx * ft;
  const float a = omega + pv;
  gstore<float>(info + 12, a);
  const float pr = vz * ft;
  const float r = radius + pr;
  gstore<float>(info + 8, r);
  const float sa = go::sin(a);
  const float ca = go::cos(a);
  const float h = 0.5f * vy;
  const float sh = go::sin(h);
  const float ch = go::cos(h);
  const go::v4 q = {sh * ca, 0.f, sh * sa, ch};
  go::v4 rot = go::quaternion_mul(q, load4(info + 80));
  store4(info + 80, rot);
  rot = go::quaternion_normalize(load4(info + 80));
  store4(info + 80, rot);
  float p[4] = {sa * r, 0.f, ca * r, 1.f};
  go::v4 rows[4];
  go::quaternion_to_matrix(rows, load4(info + 80));
  float m[16];
  memcpy(m, rows, sizeof(m));
  go::vector_matrix_mul(p, p, m);
  const u32 center = gload<u32>(info + 108);
  for (int i = 0; i < 3; i++) {
    gstore<float>(pos + 4 * i, p[i] + gload<float>(center + 4 * i));
  }
}

//! sp-init-fields! (pc-sp-init-fields!'s result; the GOAL call when it says #f)
u64 init_fields(u32 dst, u32 spec, u64 first, u64 end, const NativeArgs& caller) {
  const u64 a[8] = {dst, spec, first, end, caller.st, 0, 0, 0};
  const u64 r = pc_sp_init_fields_impl(NativeArgs{a, caller.pp, caller.st, caller.stack});
  if ((u32)r != (u32)caller.st) {
    return r;
  }
  return native_call_goal(gload<u32>(s_init_fields.sym), a, caller);
}

bool relaunch_ok() {
  return s_relaunch_2d.unchanged() && s_relaunch_setup.unchanged() && s_init_fields.unchanged();
}

//! sp-relaunch-particle-2d (obj launcher info vec): sp-relaunch-setup-fields, then the aux list.
//! Only the usual case: a particle whose fields are re-initialized with the time of day color
//! (flag bit12 without aux-list) returns false and runs the GOAL code (no test pad reaches it, so
//! it isn't verified).
bool relaunch_2d(u32 launcher, u32 info, u32 vec, const NativeArgs& caller) {
  static const u32 aux_sym = ::jak1::intern_from_c("add-to-sprite-aux-list").offset;
  const u32 fl0 = gload<u32>(info + 104);
  if ((fl0 & 0x1000) && !(fl0 & 0x100)) {
    return false;
  }
  const u32 spec0 = gload<u32>(launcher + 8);
  gstore<u32>(info + 120, 0);
  const u64 next = init_fields(vec, spec0, 9, 22, caller);
  init_fields(info + 12, (u32)next, 23, 52, caller);
  if (gload<u32>(info + 104) & 0x100) {
    gstore<u32>(info + 112, gload<u32>(aux_sym));
    gstore<float>(vec + 44, 0.f);
    gstore<float>(info + 60, 0.f);
    gstore<u32>(info + 104, gload<u32>(info + 104) & ~4u);
  }
  return true;
}

enum class Kind { NONE, ORBITER, RELAUNCH_2D };

Kind classify(u32 fn) {
  if (fn == 0) {
    return Kind::NONE;
  }
  if (s_orbiter.unchanged() && fn == s_orbiter.fn) {
    return orbiter_ok() ? Kind::ORBITER : Kind::NONE;
  }
  if (s_relaunch_2d.unchanged() && fn == s_relaunch_2d.fn) {
    return relaunch_ok() ? Kind::RELAUNCH_2D : Kind::NONE;
  }
  return Kind::NONE;
}

//! false when the GOAL function must run
bool run_inline(Kind kind, const u64 a[8], const NativeArgs& caller) {
  if (kind == Kind::ORBITER) {
    orbiter((u32)a[1], (u32)a[2]);
    return true;
  }
  return relaunch_2d((u32)a[1], (u32)a[2], (u32)a[3], caller);
}

}  // namespace

/*!
 * (AI-assisted) a particle callback or relaunch (fn a0 a1 a2 a3 ...) from the particle natives:
 * inline when there is a version for fn (above), else through native_call_goal.
 */
void sp_call_particle_fn(u32 fn, const u64 a[8], const NativeArgs& caller) {
  const Kind kind = classify(fn);
  if (kind == Kind::NONE) {
    native_call_goal(fn, a, caller);
    return;
  }
#if MIPS2C_WRITE_LOG
  if (native_verify_inline_on()) {
    static const u32 vu_r = ::jak1::intern_from_c("*_vu-reg-R_*").offset;
    const u32 info = (u32)(kind == Kind::RELAUNCH_2D ? a[2] : a[1]);
    const u32 vec = (u32)(kind == Kind::RELAUNCH_2D ? a[3] : a[2]);
    native_verify_inline(
        kind == Kind::ORBITER ? "sp-orbiter" : "sp-relaunch-particle-2d",
        {{info, 144}, {vec, 48}, {vu_r, 4}},
        [&]() -> u64 {
          if (!run_inline(kind, a, caller)) {
            native_call_goal(fn, a, caller);
          }
          return 0;
        },
        [&]() -> u64 {
          native_call_goal(fn, a, caller);
          return 0;
        });
    return;
  }
#endif
  if (!run_inline(kind, a, caller)) {
    native_call_goal(fn, a, caller);
  }
}

}  // namespace Mips2C::jak1::native
