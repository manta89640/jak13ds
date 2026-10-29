/*!
 * @file native_sp_spawn.cpp
 * Native version of (method spawn sparticle-launch-control) (sparticle-launcher.gc), which every
 * particle emitter calls every frame. Same results as goalc's C code of the GOAL method: the same
 * float operations in the same order, goalc's compares and integer semantics (native_goalc_ops.h).
 * The GOAL functions it calls (is-visible?, math-camera-pos, vector-vector-distance,
 * sp-launch-particles-var) are called through their symbols / the method table, as the GOAL code
 * does.
 *
 * (pc-sp-spawn launch-control pos) -> symbol: #t when done, #f when the GOAL code must run
 * (turned off: mips2c_native_off).
 */

#include "game/kernel/jak1/kscheme.h"
#include "game/mips2c/jak1_functions/native_functions.h"
#include "game/mips2c/native_goalc_ops.h"

#if defined(__clang__)
#pragma clang fp contract(off)
#endif

namespace Mips2C::jak1::native {

namespace {

using namespace ::Mips2C::goalc_ops;

u32 sym_addr(const char* name) {
  return ::jak1::intern_from_c(name).offset;
}

// sparticle-launch-control
constexpr u32 kLcLength = 0;
constexpr u32 kLcGroup = 12;
constexpr u32 kLcLocalClock = 20;
constexpr u32 kLcMatrix = 28;
constexpr u32 kLcLastSpawnFrame = 32;
constexpr u32 kLcLastSpawnTime = 36;
constexpr u32 kLcCenter = 44;
constexpr u32 kLcData = 60;  // sparticle-launch-state, 32 bytes each
// sparticle-launch-state
constexpr u32 kStGroupItem = 0;
constexpr u32 kStFlags = 4;  // uint16: launcher-active 1, particles-active 2
constexpr u32 kStOrigin = 8;
constexpr u32 kStOffset = 20;
constexpr u32 kStSpawnTime = 28;
// sparticle-group-item
constexpr u32 kGiLauncher = 0;
constexpr u32 kGiFadeAfter = 4;
constexpr u32 kGiFalloffTo = 8;
constexpr u32 kGiFlags = 12;  // uint16: is-3d 1, bit1 2, launch-asap 8, bit6 0x40
constexpr u32 kGiPeriod = 14;
constexpr u32 kGiLength = 16;
constexpr u32 kGiHourMask = 20;
// sparticle-launch-group flags (uint16 at +6): use-local-clock 1, always-draw 2, screen-space 4
constexpr u32 kGroupFlags = 6;

constexpr u32 kZero = 0;
constexpr u32 kPoint2 = 0x3e4ccccdu;  // 0.2

inline u64 call_goal(u32 fn, std::initializer_list<u64> a, const NativeArgs& caller) {
  u64 args[8] = {};
  int i = 0;
  for (u64 v : a) {
    args[i++] = v;
  }
  return native_call_goal(fn, args, caller);
}

u64 spawn_impl(const NativeArgs& args) {
  static const bool off = native_turned_off("pc-sp-spawn");
  if (off) {
    return args.st;
  }
  static const u32 timer_sym = sym_addr("*particle-300hz-timer*");
  static const u32 display_sym = sym_addr("*display*");
  static const u32 frame_time_sym = sym_addr("*sp-frame-time*");
  static const u32 vvd_sym = sym_addr("vector-vector-distance");
  static const u32 camera_pos_sym = sym_addr("math-camera-pos");
  static const u32 tod_proc_sym = sym_addr("*time-of-day-proc*");
  static const u32 pc_settings_sym = sym_addr("*pc-settings*");
  static const u32 part_id_table_sym = sym_addr("*part-id-table*");
  static const u32 launcher_type_sym = sym_addr("sparticle-launcher");
  static const u32 launch_sym = sym_addr("sp-launch-particles-var");
  static const u32 system_3d_sym = sym_addr("*sp-particle-system-3d*");
  static const u32 system_2d_sym = sym_addr("*sp-particle-system-2d*");
  static const u32 error_sym = sym_addr("pc-sp-spawn-error");

  const u64 st = args.st;
  const u32 lc = (u32)args.a[0];
  const u32 pos = (u32)args.a[1];

  gcopy_q(lc + kLcCenter, pos);
  // (is-visible? this pos), through the type's method table
  {
    const u32 fn = gload<u32>(gload<u32>(lc - 4) + 56);
    const u64 vis = call_goal(fn, {lc, pos}, args);
    if ((u32)vis == (u32)st &&
        !(gload<u16>(gload<u32>(lc + kLcGroup) + kGroupFlags) & 6)) {
      return st + 8;
    }
  }

  u64 now = (u64)(s64)gload<s32>(timer_sym);
  u64 last = (u64)(s64)gload<s32>(lc + kLcLastSpawnTime);
  const u64 frame = gload<u64>(gload<u32>(display_sym) + 820);
  if ((s64)frame != (s64)((u64)(s64)gload<s32>(lc + kLcLastSpawnFrame) + 1)) {
    last = now - (f2gpr(gload<float>(gload<u32>(frame_time_sym))) & 255);
  }
  gstore<u32>(lc + kLcLastSpawnFrame, (u32)gload<u64>(gload<u32>(display_sym) + 820));
  gstore<u32>(lc + kLcLastSpawnTime, (u32)now);
  if (gload<u16>(gload<u32>(lc + kLcGroup) + kGroupFlags) & 1) {
    last = (u64)(s64)gload<s32>(lc + kLcLocalClock);
    const u64 clock = (u64)(s64)gload<s32>(lc + kLcLocalClock) +
                      (f2gpr(gload<float>(gload<u32>(frame_time_sym))) & 255);
    gstore<u32>(lc + kLcLocalClock, (u32)clock);
    now = (u64)(s64)gload<s32>(lc + kLcLocalClock);
  }

  // (vector-vector-distance pos (math-camera-pos))
  const u32 vvd_fn = gload<u32>(vvd_sym);
  const u64 cam = call_goal(gload<u32>(camera_pos_sym), {}, args);
  float dist = u2f((u32)call_goal(vvd_fn, {pos, (u64)(u32)cam}, args));

  // hour mask
  u64 hour_mask;
  {
    s64 hour = 0;
    if (gload<u32>(tod_proc_sym) != (u32)st) {
      hour = gload<s32>(gload<u32>(gload<u32>(tod_proc_sym)) + 124);
    }
    hour_mask = hour > 0 ? (u64)1 << (hour & 63) : (u64)((s64)1 >> ((u64)(-hour) & 63));
  }
  if (gload<s32>(lc + kLcMatrix) != 0) {
    dist = 0.f;
  }
  if (gload<u32>(pc_settings_sym) != (u32)st &&
      gload<u32>(gload<u32>(pc_settings_sym) + 276) == (u32)st) {
    dist = 0.f;
  }

  auto launch = [&](u32 gi, u32 launcher, u32 state, float rate) {
    const u32 system =
        gload<u32>((gload<u16>(gi + kGiFlags) & 1) ? system_3d_sym : system_2d_sym);
    call_goal(gload<u32>(launch_sym),
              {system, launcher, gload<u32>(state + kStOrigin), state, lc, f2gpr(rate)}, args);
  };
  //! goalc's (< 0.0 x) skips the launch when x <= 0
  auto positive = [](float x) { return !gc_fcmp_k(fbits(x), kZero, GC_FLE, 0); };
  //! (* 0.2 (the float n) f)
  auto rate_of = [](u64 n, float f) {
    float r = bitsf(kPoint2);
    r = r * i2f((s64)n);
    r = r * f;
    return r;
  };

  s64 i = gload<s32>(lc + kLcLength);
  while (i != 0) {
    i--;
    const u32 state = lc + kLcData + 32 * (u32)i;
    const u32 gi = gload<u32>(state + kStGroupItem);
    const u32 launcher =
        gload<u32>(gload<u32>(part_id_table_sym) + 12 + 4 * gload<u32>(gi + kGiLauncher));
    if (launcher == (u32)st || launcher == 0 || !(gload<u16>(state + kStFlags) & 1)) {
      continue;
    }
    float falloff = 1.f;
    {
      const float to = gload<float>(gi + kGiFalloffTo);
      if (!fcmp_xk(to, kZero, GC_FEQ)) {
        falloff = 1.f - div_ps2(dist, to);
      }
    }
    if (gload<u32>(launcher - 4) != gload<u32>(launcher_type_sym)) {
      call_goal(gload<u32>(error_sym), {}, args);
      continue;
    }
    const u16 gflags = gload<u16>(gi + kGiFlags);
    if (gflags & 0x48) {
      if (!(gload<u16>(state + kStFlags) & 2) || (gflags & 0x40)) {
        gstore<u32>(state + kStSpawnTime, (u32)now);
        gstore<u16>(state + kStFlags, gload<u16>(state + kStFlags) | 2);
        if (positive(falloff)) {
          launch(gi, launcher, state, falloff);
        }
      }
      continue;
    }
    if ((s64)(gload<u32>(gi + kGiHourMask) & hour_mask) != 0) {
      continue;
    }
    {
      const float fade = gload<float>(gi + kGiFadeAfter);
      if (fcmp_xk(fade, kZero, GC_FNE) && dist >= fade) {
        continue;
      }
    }
    float rate;
    const u64 period = gload<u16>(gi + kGiPeriod);
    if (period == 0) {
      rate = rate_of(now - last, falloff);
    } else {
      const u64 a2 = gload<u32>(state + kStOffset);
      const s64 m_last = [&] {
        const s32 x = (s32)(last + a2), y = (s32)period;
        return (y == 0 || y == -1) ? (s64)0 : (s64)(x % y);
      }();
      const u64 m_now = (u64)(s64)(s32)((u32)(now + a2) % (u32)period);
      const u64 len = gload<u16>(gi + kGiLength);
      if (m_last < (s64)len && m_now < len) {
        rate = rate_of(now - last, falloff);
      } else if (m_last < (s64)len && m_now >= len) {
        rate = rate_of(len - (u64)m_last, falloff);
      } else if (m_last >= (s64)len && m_now < len) {
        rate = rate_of(m_now, falloff);
      } else {
        if (!(gflags & 2)) {
          continue;
        }
        if (now - (u64)gload<u32>(state + kStSpawnTime) < period) {
          continue;
        }
        gstore<u32>(state + kStOffset, (u32)(period - m_now));
        rate = rate_of(now - last, falloff);
      }
    }
    gstore<u32>(state + kStSpawnTime, (u32)now);
    gstore<u16>(state + kStFlags, gload<u16>(state + kStFlags) | 2);
    if (positive(rate)) {
      launch(gi, launcher, state, rate);
    }
  }
  return st + 8;
}

}  // namespace

const NativeImpl sp_spawn = MIPS2C_NATIVE_IMPL_GOAL(spawn_impl, NATIVE_CALLS_GOAL | NATIVE_V0_UNDEFINED);

}  // namespace Mips2C::jak1::native

namespace Mips2C::jak1::pc_sp_spawn {
void link() {
  gLinkedFunctionTable.reg("pc-sp-spawn", native::sp_spawn.as_exec, 64, &native::sp_spawn);
}
}  // namespace Mips2C::jak1::pc_sp_spawn
