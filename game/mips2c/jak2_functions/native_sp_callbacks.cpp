/*!
 * @file native_sp_callbacks.cpp
 * (AI-assisted)
 * Jak 2: GOAL functions that the particle natives (sp-process-block-2d / -3d, the launcher) call
 * for single particles, run inline instead of through native_call_goal (an adapter thunk into
 * goalc's C and back for every particle): sparticle-mode-animate, sparticle-texture-day-night,
 * sp-relaunch-particle-2d (with sp-relaunch-setup-fields), the check-drop-level-* callbacks of the
 * city levels while the particle stays above its drop height, sparticle-motion-blur (a native: its
 * entry called directly), and sp-init-fields! (pc-sp-init-fields!).
 *
 * Each is goalc's C of the GOAL function (out/jak2/csrc/sparticle-launcher.c ...): the same loads,
 * integer ops (gc_udiv32 / gc_imod32 with their zero divisor results), float operations in the same
 * order and compares (native_goalc_ops.h), and the functions they call through symbols: inline
 * while those symbols hold what they held at the first call (engine functions), or for the level
 * callbacks, while the particle's function is the symbol's current value. Otherwise the GOAL call.
 * Verify mode (OPENGOAL_MIPS2C_VERIFY=1) runs the GOAL function and compares each call
 * (native_verify_inline: the particle, its sprite, the adgif, the random generators).
 */

#include <cstring>

#include "game/kernel/jak2/kscheme.h"
#include "game/mips2c/jak2_functions/native2.h"
#include "game/mips2c/jak2_functions/native_functions.h"
#include "game/mips2c/native_goalc_ops.h"
#include "game/mips2c/native_sp_init_fields.h"

namespace Mips2C::jak2::native2 {
u64 sparticle_motion_blur_impl(const NativeArgs& args);
}

namespace Mips2C::jak2::native {

namespace {

using goalc_ops::fcmp_xk;
using goalc_ops::i2f;

//! a symbol's value address and the function it held at the first call
struct FnSym {
  const char* name;
  u32 val = 0;
  u32 fn = 0;
  bool resolved = false;
  explicit FnSym(const char* n) : name(n) {}
  //! the symbol still holds the function it held when first asked
  bool unchanged() {
    if (!resolved) {
      resolved = true;
      val = sym_val_addr(name);
      fn = gload<u32>(val);
    }
    return fn != 0 && gload<u32>(val) == fn;
  }
};

FnSym s_mode_animate("sparticle-mode-animate");
FnSym s_day_night("sparticle-texture-day-night");
FnSym s_relaunch_2d("sp-relaunch-particle-2d");
FnSym s_relaunch_setup("sp-relaunch-setup-fields");
FnSym s_init_fields("sp-init-fields!");
FnSym s_rand_vu("rand-vu");
FnSym s_rand_range("rand-vu-float-range");
FnSym s_adgif_callback("particle-adgif-callback");
FnSym s_pc_rand("pc-rand");

u32 vu_r() {
  static const u32 a = sym_val_addr("*_vu-reg-R_*");
  return a;
}
u32 tod_context() {
  static const u32 a = sym_val_addr("*time-of-day-context*");
  return gload<u32>(a);
}

//! rand-vu (math.gc) as its C: the VU0 random register, pc-rand's draw
inline float rand_vu() {
  return sp_init::rand_vu(g_ee_main_mem, vu_r());
}

//! rand-vu-float-range: lo + rand * (hi - lo)
inline float rand_vu_float_range(float lo, float hi) {
  GC_NATIVE_FP_EXACT
  const float r = rand_vu();
  const float d = hi - lo;
  const float p = r * d;
  return lo + p;
}

//! particle-adgif (native, or its GOAL value) with (adgif tex)
void particle_adgif_call(u32 adgif, u64 tex, const NativeArgs& caller) {
  static const u32 sym = sym_val_addr("particle-adgif");
  static const u32* stub = native_stub_slot("particle-adgif");
  const u64 a[8] = {adgif, tex, 0, 0, 0, 0, 0, 0};
  const u32 fn = gload<u32>(sym);
  if (fn == *stub) {
    particle_adgif_impl(NativeArgs{a, caller.pp, caller.st, caller.stack});
  } else {
    native_call_goal(fn, a, caller);
  }
}

//! particle-adgif-callback: particle-adgif, keeping the adgif's quadwords at 48 and 64 (8 bytes
//! each)
void particle_adgif_callback(u32 adgif, u64 tex, const NativeArgs& caller) {
  const u64 q64 = gload<u64>(adgif + 64);
  const u64 q48 = gload<u64>(adgif + 48);
  particle_adgif_call(adgif, tex, caller);
  gstore<u64>(adgif + 64, q64);
  gstore<u64>(adgif + 48, q48);
}

bool mode_animate_ok() {
  return s_mode_animate.unchanged() && s_rand_range.unchanged() && s_rand_vu.unchanged() &&
         s_pc_rand.unchanged() && s_adgif_callback.unchanged();
}

//! sparticle-mode-animate (info vec): the texture and grey level of a particle animated by the
//! mode table its user-float points at
void mode_animate(u32 info, u32 vec, const NativeArgs& caller) {
  GC_NATIVE_FP_EXACT
  const u32 key = gload<u32>(info + 132);
  const u32 uf = gload<u32>(info + 108);
  if (fcmp_xk(goalc_ops::bitsf(uf), 0, goalc_ops::GC_FEQ)) {
    return;
  }
  // (-> (the-as (array symbol) uf) 0 value)
  const u64 table = (u64)(s64)gload<s32>(gload<u32>(uf + 12) - 1);
  if (table == 0) {
    return;
  }
  const s64 want = (s64)gload<u8>(key + 29) + 1;
  const s64 last = (s64)gload<s32>((u32)table) - 1;
  const s64 idx = want <= last ? want : last;
  const u32 entry = gload<u32>((u32)(((u64)idx << 2) + 12 + table));
  // (mod (/ state-counter (/ (-> entry w) 8)) (+ (-> entry 0) -1)), as goalc's 32-bit ops
  const u32 counter = gload<u32>(key + 32);
  const u32 div = (u32)((s64)gload<s32>(entry + 12) >> 3);
  const u64 q = div == 0 ? 0 : (u64)(s64)(s32)(counter / div);
  const s32 m = (s32)((s64)gload<s32>(entry) - 1);
  const u64 frame = (m == 0 || m == -1) ? 0 : (u64)(s64)((s32)q % m);
  const u64 bits_addr = (u64)(s64)gload<s32>((u32)(((frame + 1) << 2) + 12 + entry));
  const s64 bit = (s64)gload<s32>(uf + 16) >> 3;
  const u64 word = gload<u64>((u32)(((u64)(bit >> 6) << 3) + bits_addr));
  const bool hit = (word & ((u64)1 << (bit & 63))) != 0;
  const s64 tex = hit ? (s64)gload<s32>(uf + 24) : (s64)gload<s32>(uf + 20);
  const float grey = hit ? rand_vu_float_range(64.f, 192.f) : rand_vu_float_range(32.f, 48.f);
  gstore<float>(vec + 32, grey);
  gstore<float>(vec + 36, gload<float>(vec + 32));
  gstore<float>(vec + 40, gload<float>(vec + 32));
  if (tex != 0) {
    particle_adgif_callback(gload<u32>(info + 4), (u64)tex, caller);
  }
}

bool day_night_ok() {
  return s_day_night.unchanged() && s_rand_vu.unchanged() && s_pc_rand.unchanged() &&
         s_adgif_callback.unchanged();
}

//! sparticle-texture-day-night (info vec): the day or night texture and color of the particle's
//! user-object table (int32 array: 3 day texture, 4 base, 5 random, 6 tod flag, 7-10 the same
//! for the night)
void day_night(u32 info, u32 vec, const NativeArgs& caller) {
  GC_NATIVE_FP_EXACT
  static const u32 tod_sym = sym_val_addr("*time-of-day*");
  const u32 ud = gload<u32>(info + 108);
  if (ud == 0) {
    return;
  }
  const u32 tod = gload<u32>(tod_sym);
  const s64 hours = tod != (u32)caller.st ? (s64)gload<s32>(gload<u32>(tod) + 124) : 0;
  const float f = rand_vu();
  const bool night = !(hours >= 6) || !(18 >= hours);
  const u32 base_off = night ? 40 : 24;  // the texture's word; random, base, flag after it
  const s64 tex = (s64)gload<s32>(ud + base_off);
  if (tex == 0) {
    return;
  }
  const u64 rnd = (u64)(s64)gload<s32>(ud + base_off + 8);
  const u64 base = (u64)(s64)gload<s32>(ud + base_off + 4);
  for (int k = 0; k < 4; k++) {
    const u64 b = (rnd << (56 - 8 * k)) >> 56;
    gstore<float>(vec + 32 + 4 * k, f * i2f((s64)b));
  }
  for (int k = 0; k < 4; k++) {
    const u64 b = (base << (56 - 8 * k)) >> 56;
    gstore<float>(vec + 32 + 4 * k, gload<float>(vec + 32 + 4 * k) + i2f((s64)b));
  }
  if (gload<s32>(ud + base_off + 12) != 0) {
    for (int k = 0; k < 4; k++) {
      gstore<float>(vec + 32 + 4 * k,
                    gload<float>(vec + 32 + 4 * k) * gload<float>(tod_context() + 108 + 4 * k));
    }
  }
  const u32 r = gload<u32>(vec + 32);
  gstore<u32>(vec + 32, (gload<u32>(info + 104) & 0x4000) ? (r | 1) : (r & ~1u));
  particle_adgif_callback(gload<u32>(info + 4), (u64)tex, caller);
}

//! sp-init-fields! (pc-sp-init-fields!'s result; the GOAL call when it says #f)
u64 init_fields(u32 dst, u32 spec, u64 first, u64 end, const NativeArgs& caller) {
  const u64 a[8] = {dst, spec, first, end, caller.st, 0, 0, 0};
  const u64 r = pc_sp_init_fields_impl(NativeArgs{a, caller.pp, caller.st, caller.stack});
  if ((u32)r != (u32)caller.st) {
    return r;
  }
  return native_call_goal(gload<u32>(s_init_fields.val), a, caller);
}

bool relaunch_ok() {
  return s_relaunch_2d.unchanged() && s_relaunch_setup.unchanged() && s_init_fields.unchanged();
}

//! sp-relaunch-particle-2d (obj launcher info vec): sp-relaunch-setup-fields, then the distort
//! flag's alpha
void relaunch_2d(u32 launcher, u32 info, u32 vec, const NativeArgs& caller) {
  GC_NATIVE_FP_EXACT
  // sp-relaunch-setup-fields
  const u32 spec0 = gload<u32>(launcher + 8);
  const u32 keep = gload<u32>(info + 104) & 0xe00;
  gstore<u32>(info + 120, 0);
  const u32 fl = gload<u32>(info + 104);
  if ((fl & 0x1000) && !(fl & 0x80) && !(fl & 0x8000)) {
    const u32 addr[6] = {vec + 32, vec + 36, vec + 40, info + 48, info + 52, info + 56};
    float saved[6];
    for (int k = 0; k < 6; k++) {
      saved[k] = gload<float>(addr[k]);
    }
    for (int k = 0; k < 6; k++) {
      gstore<float>(addr[k], 99999.f);
    }
    const u64 next = init_fields(vec, spec0, 9, 22, caller);
    init_fields(info + 12, (u32)next, 23, 52, caller);
    gstore<u32>(info + 104, gload<u32>(info + 104) | keep);
    const u32 color = tod_context() + 108;
    for (int k = 0; k < 6; k++) {
      const float v = gload<float>(addr[k]);
      if (fcmp_xk(v, 0x47c34f80u, goalc_ops::GC_FNE)) {
        gstore<float>(addr[k], v * gload<float>(color + 4 * (k % 3)));
      } else {
        gstore<float>(addr[k], saved[k]);
      }
    }
  } else {
    const u64 next = init_fields(vec, spec0, 9, 22, caller);
    init_fields(info + 12, (u32)next, 23, 52, caller);
  }
  if (gload<u32>(info + 104) & 0x80) {
    gstore<float>(vec + 44, 0.f);
    gstore<float>(info + 60, 0.f);
    gstore<u32>(info + 104, gload<u32>(info + 104) & ~4u);
  }
}

/*!
 * The check-drop-level-* callbacks of the form
 *   (when (< (-> pos y) (-> info user-float)) <kill and launch splashes>)
 *   (sparticle-motion-blur system info pos)
 * (checked against goal_src). While the particle is not below its drop height only the motion
 * blur runs: that part is done here; a drop runs the GOAL function.
 */
const char* const kDropChecks[] = {
    "check-drop-level-ctysluma-drop-userdata", "check-drop-level-ctyslumb-drop-userdata",
    "check-drop-level-ctyport-drop-userdata",  "check-drop-level-ctyfarma-drop-userdata",
    "check-drop-level-ctyfarmb-drop-userdata", "check-drop-level-ctypal-drop-userdata",
};
constexpr int kNumDropChecks = sizeof(kDropChecks) / sizeof(kDropChecks[0]);

int drop_check_index(u32 fn) {
  // the symbols exist once a level that uses them is linked: looked up (not interned) until found,
  // at most once per 256 calls
  static u32 vals[kNumDropChecks] = {};
  static u32 tries = 0;
  if ((tries++ & 255) == 0) {
    for (int i = 0; i < kNumDropChecks; i++) {
      if (!vals[i]) {
        const u32 sym = ::jak2::find_symbol_from_c(kDropChecks[i]).offset;
        if (sym > 1) {
          vals[i] = sym - 1;
        }
      }
    }
  }
  for (int i = 0; i < kNumDropChecks; i++) {
    if (vals[i] && gload<u32>(vals[i]) == fn) {
      return i;
    }
  }
  return -1;
}

//! sparticle-motion-blur (a native: its entry, or the symbol's value)
void motion_blur_call(const u64 a[8], const NativeArgs& caller) {
  static const u32 sym = sym_val_addr("sparticle-motion-blur");
  static const u32* stub = native_stub_slot("sparticle-motion-blur");
  const u32 fn = gload<u32>(sym);
  if (fn == *stub) {
    native2::sparticle_motion_blur_impl(NativeArgs{a, caller.pp, caller.st, caller.stack});
  } else {
    native_call_goal(fn, a, caller);
  }
}

enum class Kind { NONE, MOTION_BLUR, MODE_ANIMATE, DAY_NIGHT, RELAUNCH_2D, DROP_CHECK };

Kind classify(u32 fn) {
  static const u32* blur_stub = native_stub_slot("sparticle-motion-blur");
  if (fn == 0) {
    return Kind::NONE;
  }
  if (fn == *blur_stub) {
    return Kind::MOTION_BLUR;
  }
  if (s_mode_animate.unchanged() && fn == s_mode_animate.fn) {
    return mode_animate_ok() ? Kind::MODE_ANIMATE : Kind::NONE;
  }
  if (s_day_night.unchanged() && fn == s_day_night.fn) {
    return day_night_ok() ? Kind::DAY_NIGHT : Kind::NONE;
  }
  if (s_relaunch_2d.unchanged() && fn == s_relaunch_2d.fn) {
    return relaunch_ok() ? Kind::RELAUNCH_2D : Kind::NONE;
  }
  if (drop_check_index(fn) >= 0) {
    return Kind::DROP_CHECK;
  }
  return Kind::NONE;
}

//! the inline version of a call (fn a0 a1 a2 a3); false when there is none for fn
bool run_inline(Kind kind, const u64 a[8], const NativeArgs& caller) {
  switch (kind) {
    case Kind::MOTION_BLUR:
      motion_blur_call(a, caller);
      return true;
    case Kind::MODE_ANIMATE:
      mode_animate((u32)a[1], (u32)a[2], caller);
      return true;
    case Kind::DAY_NIGHT:
      day_night((u32)a[1], (u32)a[2], caller);
      return true;
    case Kind::RELAUNCH_2D:
      relaunch_2d((u32)a[1], (u32)a[2], (u32)a[3], caller);
      return true;
    case Kind::DROP_CHECK:
      // (< y user-float) as goalc's general compare: !(y >= uf)
      if (gload<float>((u32)a[2] + 4) >= gload<float>((u32)a[1] + 108)) {
        motion_blur_call(a, caller);
        return true;
      }
      return false;
    default:
      return false;
  }
}

const char* kind_name(Kind k) {
  switch (k) {
    case Kind::MODE_ANIMATE:
      return "sparticle-mode-animate";
    case Kind::DAY_NIGHT:
      return "sparticle-texture-day-night";
    case Kind::RELAUNCH_2D:
      return "sp-relaunch-particle-2d";
    case Kind::DROP_CHECK:
      return "check-drop-level-* (no drop)";
    default:
      return "sparticle-motion-blur (direct)";
  }
}

}  // namespace

/*!
 * (AI-assisted) a particle callback or relaunch (fn a0 a1 a2 a3 ...) from the particle natives:
 * run inline when there is a version for fn (above), else through native_call_goal.
 */
void sp_call_particle_fn(u32 fn, const u64 a[8], const NativeArgs& caller) {
  const Kind kind = classify(fn);
  if (kind != Kind::NONE) {
#if MIPS2C_WRITE_LOG
    if (native_verify_inline_on() && kind != Kind::MOTION_BLUR) {
      const u32 info = (u32)(kind == Kind::RELAUNCH_2D ? a[2] : a[1]);
      const u32 vec = (u32)(kind == Kind::RELAUNCH_2D ? a[3] : a[2]);
      const u32 adgif = gload<u32>(info + 4);
      bool ran = true;
      native_verify_inline(
          kind_name(kind), {{info, 144}, {vec, 48}, {adgif, 80}, {vu_r(), 4}},
          [&]() -> u64 {
            ran = run_inline(kind, a, caller);
            if (!ran) {
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
    if (run_inline(kind, a, caller)) {
      return;
    }
  }
  native_call_goal(fn, a, caller);
}

}  // namespace Mips2C::jak2::native
