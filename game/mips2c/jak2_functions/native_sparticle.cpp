/*!
 * @file native_sparticle.cpp
 * (AI-assisted)
 * Jak 2: native versions of the mips2c functions in sparticle.cpp, ported from Jak 1's
 * (jak1_functions/native_sparticle.cpp). See game/mips2c/mips2c_native.h for the rules (same
 * float operations, grouped the same way, as the mips2c code).
 *
 * What Jak 2 changed against Jak 1: each particle reads its own time vector from a table on the
 * scratchpad (byte 129 of the cpuinfo indexes it; x: frames as an int, y: velocity scale, z:
 * acceleration scale, w: friction scale) instead of *sp-frame-time*, and a particle whose frame
 * count is 0 is "paused" (no paused? argument). The in-use test is byte 128 (0: free). The flag
 * bits moved (cache alpha 64 -> 32, orbiter 128 -> 64), 2d particles with the aux flags (0x8080)
 * go to add-to-sprite-aux-list instead of their callback, and 3d particles apply rotvel3d once per
 * 10 frames, then normalize the quaternion.
 */

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "common/goal_constants.h"
#include "common/log/log.h"

#include "game/kernel/common/kperf.h"
#include "game/mips2c/jak2_functions/native_functions.h"
#include "game/mips2c/native_goalc_ops.h"
#include "game/graphics/ctr/CtrHot.h"  // (AI-assisted, hottext)

#if defined(__GNUC__) || defined(__clang__)
#define NATIVE_ALIGNED4(p) __builtin_assume_aligned((p), 4)
#else
#define NATIVE_ALIGNED4(p) (p)
#endif

namespace Mips2C::jak2::native {

namespace {

/*!
 * Words of the particles in GOAL memory. They are aligned (the mips2c code loads their quadwords
 * with lqc2), so each access is one ARM or VFP load or store, and the base pointer is a local:
 * a store to GOAL memory doesn't make the compiler load g_ee_main_mem again.
 */
struct ParticleMem {
  u8* const base = g_ee_main_mem;
  float f(u32 addr) const {
    float v;
    memcpy(&v, NATIVE_ALIGNED4(base + addr), 4);
    return v;
  }
  u32 u(u32 addr) const {
    u32 v;
    memcpy(&v, NATIVE_ALIGNED4(base + addr), 4);
    return v;
  }
  //! lw: sign-extended
  s64 s(u32 addr) const { return (s32)u(addr); }
  //! lb: sign-extended
  s64 b(u32 addr) const { return (s8)base[addr]; }
  void set_f(u32 addr, float v) const {
    MIPS2C_NATIVE_LOG_STORE(addr, 4);
    memcpy(NATIVE_ALIGNED4(base + addr), &v, 4);
  }
  void set_u(u32 addr, u32 v) const {
    MIPS2C_NATIVE_LOG_STORE(addr, 4);
    memcpy(NATIVE_ALIGNED4(base + addr), &v, 4);
  }
};

// sparticle-cpuinfo (144 bytes): sprite 0, adgif 4, radius 8, omega 12, vel-sxvel 16,
// rot-syvel 32, fade 48, acc 64, rotvel3d 80, friction 96, timer 100, flags 104, user 108,
// func 112, next-time 116, next-launcher 120, cache-alpha 124, valid (byte) 128, clock index
// (byte) 129, ...
// The vecdata (48 bytes): x y z sx, then (2d) flag matrix rot sy or (3d) qx qy qz sy, r g b a.

//! The particle's time vector (lqc2 from the scratchpad table): x frames (bits), y z w scales
struct TimeVec {
  u64 frames;  // x & 255
  float vel, acc, friction;
};

inline TimeVec time_vec(const ParticleMem& m, u32 spad, u32 info) {
  const u32 t = spad + (u32)(m.b(info + 129) * 16);
  return {m.u(t) & 255u, m.f(t + 4), m.f(t + 8), m.f(t + 12)};
}

//! the done tests: color r g b all 0, alpha <= 0, x or y size below 0 (with the flags' bits)
inline bool particle_done(u32 flags, u32 r, u32 g, u32 b, u32 a, float sx, float sy) {
  if ((flags & 2) && r == 0 && g == 0 && b == 0) {
    return true;
  }
  if ((flags & 4) && (s32)a <= 0) {
    return true;
  }
  return (flags & 1) && ((s32)f2u(sx) < 0 || (s32)f2u(sy) < 0);
}

/*!
 * (sp-process-block-2d system cpuinfo vecdata index count paused?) -> index + count
 * Updates count 2d particles (sparticle-cpuinfo and sprite-vec-data-2d): timers, the user
 * callback (or add-to-sprite-aux-list), relaunching, velocity (with acceleration and friction),
 * position, rotation and color, the orbiter, and frees the ones that are done.
 */
CTR_HOT u64 sp_process_block_2d_impl(const NativeArgs& args) {
  static const u32 spad_sym = sym_val_addr("*fake-scratchpad-data*");
  static const u32 aux_list_sym = sym_val_addr("add-to-sprite-aux-list");
  static const u32 free_particle_sym = sym_val_addr("sp-free-particle");
  static const u32 orbiter_sym = sym_val_addr("sp-orbiter");
  static const u32 relaunch_sym = sym_val_addr("sp-relaunch-particle-2d");

  const ParticleMem m;
  const u64 system = args.a[0];
  u32 info = (u32)args.a[1];
  u32 vec = (u32)args.a[2];
  u64 index = args.a[3];
  u64 count = args.a[4];

  // a3 is not set before the orbiter call: it is the argument, or what the last call got
  u64 reg_a3 = args.a[3];
  auto call = [&](u32 fn, u64 a0, u64 a1, u64 a2, u64 a3) {
    reg_a3 = a3;
    const u64 call_args[8] = {a0, a1, a2, a3, args.a[4], args.a[5], args.a[6], args.a[7]};
    // (AI-assisted) callbacks and relaunches inline when they can be (native_sp_callbacks.cpp)
    sp_call_particle_fn(fn, call_args, args);
  };
  auto free_particle = [&]() { call(m.u(free_particle_sym), system, index, info, vec); };

  do {
    if (m.b(info + 128) == 0) {
      // not in use
    } else {
      const TimeVec tv = time_vec(m, m.u(spad_sym), info);
      const u64 frames = tv.frames;
      if (frames == 0) {
        // paused: the aux list, then only free the ones whose timer is done
        if (m.u(info + 104) & 0x8080) {
          call(m.u(aux_list_sym), system, info, vec, index);
        }
        if (m.s(info + 100) == 0) {
          free_particle();
        } else {
          const u32 flags = m.u(info + 104);
          m.set_u(info + 104, flags & ~32u);
          if (flags & 32) {
            m.set_u(vec + 44, m.u(info + 124));
          }
        }
      } else {
        bool do_free = false;
        const s64 timer = m.s(info + 100);
        if (timer != -1) {
          if (timer == 0) {
            do_free = true;
          } else {
            m.set_u(info + 100, (u32)std::max((s32)(u32)(timer - (s64)frames), 0));
          }
        }
        if (!do_free) {
          const u32 flags0 = m.u(info + 104);
          const u32 flags = flags0 & ~32u;
          m.set_u(info + 104, flags);
          if (flags0 & 32) {
            m.set_u(vec + 44, m.u(info + 124));
          }

          // the user callback, or add-to-sprite-aux-list for the aux flags
          u32 callback = m.u(info + 112);
          if (flags & 0x8080) {
            callback = m.u(aux_list_sym);
          }
          if (callback) {
            call(callback, system, info, vec, index);
          }

          // relaunch
          const s64 next_launcher = m.s(info + 120);
          const s64 next_time = m.s(info + 116) - (s64)frames;
          if (next_launcher != 0) {
            m.set_u(info + 116, (u32)next_time);
            if (next_time - 1 < 0) {
              call(m.u(relaunch_sym), system, (u64)next_launcher, info, vec);
            }
          }

          // motion: vmulz (acceleration), vadd.xyz, friction, vmuly, then vadd and vmaxx
          float vx = m.f(info + 16), vy = m.f(info + 20), vz = m.f(info + 24);
          const float vw = m.f(info + 28);
          const float rot_vz = m.f(info + 40), rot_vw = m.f(info + 44);
          const float fade_x = m.f(info + 48), fade_y = m.f(info + 52), fade_z = m.f(info + 56),
                      fade_w = m.f(info + 60);
          const float ax = m.f(info + 64) * tv.acc;
          const float ay = m.f(info + 68) * tv.acc;
          const float az = m.f(info + 72) * tv.acc;
          const u32 friction_bits = m.u(info + 96);
          float px = m.f(vec), py = m.f(vec + 4), pz = m.f(vec + 8), pw = m.f(vec + 12);
          float rot = m.f(vec + 24), sy = m.f(vec + 28);
          float cx = m.f(vec + 32), cy = m.f(vec + 36), cz = m.f(vec + 40), cw = m.f(vec + 44);
          vx = vx + ax;
          vy = vy + ay;
          vz = vz + az;
          if (friction_bits != 0) {
            const float a = 1.f - u2f(friction_bits);
            const float b = a * tv.friction;
            const float c = 1.f - b;
            vx = vx * c;
            vy = vy * c;
            vz = vz * c;
          }
          const float ft_vel = tv.vel;
          const float dpx = vx * ft_vel, dpy = vy * ft_vel, dpz = vz * ft_vel, dpw = vw * ft_vel;
          const float drot = rot_vz * ft_vel, dsy = rot_vw * ft_vel;
          const float dcx = fade_x * ft_vel, dcy = fade_y * ft_vel, dcz = fade_z * ft_vel,
                      dcw = fade_w * ft_vel;
          px = px + dpx;
          py = py + dpy;
          pz = pz + dpz;
          pw = pw + dpw;
          cx = cx + dcx;
          cy = cy + dcy;
          cz = cz + dcz;
          cw = cw + dcw;
          rot = rot + drot;
          sy = sy + dsy;
          cx = std::max(cx, 0.f);
          cy = std::max(cy, 0.f);
          cz = std::max(cz, 0.f);
          cw = std::max(cw, 0.f);
          m.set_f(info + 16, vx);
          m.set_f(info + 20, vy);
          m.set_f(info + 24, vz);
          m.set_f(vec, px);
          m.set_f(vec + 4, py);
          m.set_f(vec + 8, pz);
          m.set_f(vec + 12, pw);
          // the rotation wraps like a 16-bit int
          m.set_f(vec + 24, (float)(s32)(s16)(s32)rot);
          m.set_f(vec + 28, sy);
          m.set_f(vec + 32, cx);
          m.set_f(vec + 36, cy);
          m.set_f(vec + 40, cz);
          m.set_f(vec + 44, cw);

          if (m.u(info + 104) & 64) {
            call(m.u(orbiter_sym), system, info, vec, reg_a3);
          }

          // done? (the color as it is now in memory, the sizes as computed)
          do_free = particle_done(m.u(info + 104), m.u(vec + 32), m.u(vec + 36), m.u(vec + 40),
                                  m.u(vec + 44), pw, sy);
        }
        if (do_free) {
          free_particle();
        }
      }
    }
    count--;
    info += 144;
    vec += 48;
    index++;
  } while (count != 0);
  return index;
}

/*!
 * (sp-process-block-3d system cpuinfo vecdata index count paused?) -> index + count
 * Like sp-process-block-2d, for 3d particles (sprite-vec-data-3d: position and x size, the
 * rotation quaternion's x y z and the y size, the color): timers, the user callback, relaunching,
 * velocity, position and sizes, the color, and the rotation: the quaternion (its w from x y z)
 * times the particle's rotvel3d once per 10 frames, normalized, stored with w >= 0. No orbiter,
 * no aux list. Frees the ones that are done.
 */
CTR_HOT u64 sp_process_block_3d_impl(const NativeArgs& args) {
  static const u32 spad_sym = sym_val_addr("*fake-scratchpad-data*");
  static const u32 quaternion_mul_sym = sym_val_addr("quaternion*!");
  static const u32 quaternion_normalize_sym = sym_val_addr("quaternion-normalize!");
  static const u32 free_particle_sym = sym_val_addr("sp-free-particle");
  static const u32 relaunch_sym = sym_val_addr("sp-relaunch-particle-3d");

  const ParticleMem m;
  const u64 system = args.a[0];
  u32 info = (u32)args.a[1];
  u32 vec = (u32)args.a[2];
  u64 index = args.a[3];
  u64 count = args.a[4];
  // the quaternion passed to quaternion*! (the mips2c version's stack frame)
  const u32 quat = args.stack - 16;

  // a1 a2 a3 are not all set before every call: what the last call got (or the arguments)
  u64 reg_a1 = args.a[1], reg_a2 = args.a[2], reg_a3 = args.a[3];
  auto call = [&](u32 fn, u64 a0, u64 a1, u64 a2, u64 a3) {
    reg_a1 = a1;
    reg_a2 = a2;
    reg_a3 = a3;
    const u64 call_args[8] = {a0, a1, a2, a3, args.a[4], args.a[5], args.a[6], args.a[7]};
    // (AI-assisted) callbacks and relaunches inline when they can be (native_sp_callbacks.cpp)
    sp_call_particle_fn(fn, call_args, args);
  };
  auto free_particle = [&]() { call(m.u(free_particle_sym), system, index, info, vec); };

  do {
    if (m.b(info + 128) == 0) {
      // not in use
    } else {
      const TimeVec tv = time_vec(m, m.u(spad_sym), info);
      const u64 frames = tv.frames;
      if (frames == 0) {
        // paused: only free the ones whose timer is done
        if (m.s(info + 100) == 0) {
          free_particle();
        } else {
          const u32 flags = m.u(info + 104);
          m.set_u(info + 104, flags & ~32u);
          if (flags & 32) {
            m.set_u(vec + 44, m.u(info + 124));
          }
        }
      } else {
        bool do_free = false;
        const s64 timer = m.s(info + 100);
        if (timer != -1) {
          if (timer == 0) {
            do_free = true;
          } else {
            m.set_u(info + 100, (u32)std::max((s32)(u32)(timer - (s64)frames), 0));
          }
        }
        if (!do_free) {
          const u32 flags = m.u(info + 104);
          m.set_u(info + 104, flags & ~32u);
          if (flags & 32) {
            m.set_u(vec + 44, m.u(info + 124));
          }

          // user callback
          if (const u32 callback = m.u(info + 112)) {
            call(callback, system, info, vec, reg_a3);
          }

          // relaunch
          const s64 next_launcher = m.s(info + 120);
          const s64 next_time = m.s(info + 116) - (s64)frames;
          if (next_launcher != 0) {
            m.set_u(info + 116, (u32)next_time);
            if (next_time < 0) {
              call(m.u(relaunch_sym), system, (u64)next_launcher, info, vec);
            }
          }

          // motion: vmulz (acceleration), vadd.xyz, friction, vmuly, then vadd and vmaxx
          float vx = m.f(info + 16), vy = m.f(info + 20), vz = m.f(info + 24);
          const float vw = m.f(info + 28);
          const float rot_vw = m.f(info + 44);
          const float fade_x = m.f(info + 48), fade_y = m.f(info + 52), fade_z = m.f(info + 56),
                      fade_w = m.f(info + 60);
          const float ax = m.f(info + 64) * tv.acc;
          const float ay = m.f(info + 68) * tv.acc;
          const float az = m.f(info + 72) * tv.acc;
          const u32 friction_bits = m.u(info + 96);
          float px = m.f(vec), py = m.f(vec + 4), pz = m.f(vec + 8), pw = m.f(vec + 12);
          const float qx = m.f(vec + 16), qy = m.f(vec + 20), qz = m.f(vec + 24);
          float sy = m.f(vec + 28);
          float cx = m.f(vec + 32), cy = m.f(vec + 36), cz = m.f(vec + 40), cw = m.f(vec + 44);
          vx = vx + ax;
          vy = vy + ay;
          vz = vz + az;
          if (friction_bits != 0) {
            const float a = 1.f - u2f(friction_bits);
            const float b = a * tv.friction;
            const float c = 1.f - b;
            vx = vx * c;
            vy = vy * c;
            vz = vz * c;
          }
          const float ft_vel = tv.vel;
          const float dpx = vx * ft_vel, dpy = vy * ft_vel, dpz = vz * ft_vel, dpw = vw * ft_vel;
          const float dsy = rot_vw * ft_vel;
          const float dcx = fade_x * ft_vel, dcy = fade_y * ft_vel, dcz = fade_z * ft_vel,
                      dcw = fade_w * ft_vel;
          px = px + dpx;
          py = py + dpy;
          pz = pz + dpz;
          pw = pw + dpw;
          sy = sy + dsy;
          cx = cx + dcx;
          cy = cy + dcy;
          cz = cz + dcz;
          cw = cw + dcw;
          cx = std::max(cx, 0.f);
          cy = std::max(cy, 0.f);
          cz = std::max(cz, 0.f);
          cw = std::max(cw, 0.f);
          m.set_f(info + 16, vx);
          m.set_f(info + 20, vy);
          m.set_f(info + 24, vz);
          m.set_f(vec, px);
          m.set_f(vec + 4, py);
          m.set_f(vec + 8, pz);
          m.set_f(vec + 12, pw);
          m.set_f(vec + 28, sy);
          m.set_f(vec + 32, cx);
          m.set_f(vec + 36, cy);
          m.set_f(vec + 40, cz);
          m.set_f(vec + 44, cw);

          // the rotation: w from x y z, times rotvel3d once per 10 frames, normalized
          {
            m.set_f(quat, qx);
            m.set_f(quat + 4, qy);
            m.set_f(quat + 8, qz);
            const float zz = qz * qz;
            float w = 1.f - zz;
            const float yy = qy * qy;
            w = w - yy;
            const float xx = qx * qx;
            w = w - xx;
            m.set_f(quat + 12, std::sqrt(std::abs(w)));
          }
          const u32 rot_vel_3d = info + 80;
          s64 left = (s64)frames;
          do {
            call(m.u(quaternion_mul_sym), quat, quat, rot_vel_3d, reg_a3);
            left -= 10;
          } while (left > 0);
          call(m.u(quaternion_normalize_sym), quat, reg_a1, reg_a2, reg_a3);
          const float rx = m.f(quat), ry = m.f(quat + 4), rz = m.f(quat + 8);
          if (m.f(quat + 12) < 0.f) {
            m.set_f(vec + 16, 0.f - rx);
            m.set_f(vec + 20, 0.f - ry);
            m.set_f(vec + 24, 0.f - rz);
          } else {
            m.set_f(vec + 16, 0.f + rx);
            m.set_f(vec + 20, 0.f + ry);
            m.set_f(vec + 24, 0.f + rz);
          }

          // done? (the color and sizes as computed)
          do_free = particle_done(m.u(info + 104), f2u(cx), f2u(cy), f2u(cz), f2u(cw), pw, sy);
        }
        if (do_free) {
          free_particle();
        }
      }
    }
    count--;
    info += 144;
    vec += 48;
    index++;
  } while (count != 0);
  return index;
}

// sparticle-system (a basic): is-3d 24, cpuinfo-table 36, vecdata-table 40, adgifdata-table 44
constexpr u32 kSysIs3d = 24, kSysCpuinfo = 36, kSysVecdata = 40, kSysAdgif = 44;
constexpr u32 kInfoSize = 144, kVecSize = 48, kAdgifSize = 80, kInfoValid = 128;
constexpr u32 kSpadBlock = 208;
// sp-process-particle-system's block: what fits in the scratchpad's 16 kB after the clock table
constexpr u32 kMaxBlock = (0x4000 - kSpadBlock) / (kInfoSize + kVecSize + kAdgifSize);

inline void copy_bytes(u32 dst, u32 src, u32 size) {
  MIPS2C_NATIVE_LOG_STORE(dst, size);
  memcpy(NATIVE_ALIGNED4(g_ee_main_mem + dst), NATIVE_ALIGNED4(g_ee_main_mem + src), size);
}

//! a block before its update: the particles in use, the class of each one's callback, and whether
//! every function the update will call only touches its own particle
struct BlockScan {
  u8 valid[kMaxBlock];
  u8 cls[kMaxBlock];
  u32 used = 0;
  bool plain = false;
  const char* why = "";
};

//! the specs a relaunch by this launcher applies leave the orbiter flag (64) off
bool relaunch_no_orbiter(u32 launcher) {
  // the scan also asks for a particle the update frees before its relaunch: stay in memory
  const u32 limit = (u32)EE_MAIN_MEM_SIZE - 96 * 16;
  if (launcher >= limit) {
    return false;
  }
  u32 spec = gload<u32>(launcher + 8);
  if (spec >= limit) {
    return false;
  }
  for (int n = 0; n < 96; n++, spec += 16) {
    const u32 field = gload<u16>(spec);
    if (field < 47) {
      continue;
    }
    return field != 47 || (gload<u16>(spec + 2) == 0 && gload<s32>(spec + 8) == 0 &&
                           !(gload<u32>(spec + 4) & 64));
  }
  return false;
}

void scan_block(const ParticleMem& m,
                bool is_3d,
                u32 m_info,
                u32 m_vec,
                u32 count,
                u32 clocks,
                BlockScan& s) {
  static const bool off = native_turned_off("sp-in-place");
  s.used = 0;
  // the natives load quadwords of the particle (aligned like the scratchpad's copies)
  s.plain = !off && ((m_info | m_vec) & 15) == 0 && sp_in_place_ready();
  s.why = s.plain ? "" : "off";
  const bool relaunch_ok = s.plain && sp_relaunch_own_data(is_3d);
  const bool orbiter_ok = s.plain && !is_3d && sp_orbiter_own_data();
  // the classes of the last two functions seen
  u32 fn_a = 0, fn_b = 0;
  u8 cls_a = SP_FN_NONE, cls_b = SP_FN_NONE;
  for (u32 i = 0; i < count; i++) {
    const u32 info = m_info + kInfoSize * i;
    const u8 valid = m.base[info + kInfoValid];
    s.valid[i] = valid;
    s.cls[i] = SP_FN_NONE;
    if (!valid) {
      continue;
    }
    s.used++;
    if (!s.plain) {
      continue;
    }
    const u32 fn = m.u(info + 112);
    u8 cls = SP_FN_NONE;
    if (fn == fn_a) {
      cls = cls_a;
    } else if (fn == fn_b) {
      cls = cls_b;
    } else if (fn != 0) {
      cls = sp_particle_fn_class(fn);
      fn_b = fn_a;
      cls_b = cls_a;
      fn_a = fn;
      cls_a = cls;
    }
    s.cls[i] = cls;
    if (cls == SP_FN_FOREIGN) {
      s.plain = false;
      s.why = "callback";
      continue;
    }
    const u32 launcher = m.u(info + 120);
    // sp-orbiter reads its centre's sprite: the same bytes in place unless that sprite is this
    // block's and already updated (or the particle's own)
    if (!is_3d && (m.u(info + 104) & 64)) {
      const u32 centre = m.u(info + 108);
      if (!orbiter_ok || launcher != 0 ||
          (centre + kVecSize > m_vec && centre < m_vec + kVecSize * (i + 1))) {
        s.plain = false;
        s.why = "orbiter";
        continue;
      }
    }
    if (launcher == 0) {
      continue;
    }
    if (!relaunch_ok) {
      s.plain = false;
      s.why = "relaunch";
    } else if (!is_3d) {
      // a 2d relaunch due now must not turn the particle into an orbiter
      const s64 frames = (s64)(m.u(clocks + (u32)(m.b(info + 129) * 16)) & 255u);
      if (m.s(info + 116) - frames <= 0 && !relaunch_no_orbiter(launcher)) {
        s.plain = false;
        s.why = "relaunch flags";
      }
    }
  }
}

/*!
 * sp-process-block-2d on the particle tables themselves, for a block whose scan says plain: the
 * same steps in the same order as sp_process_block_2d_impl, the callbacks by their class,
 * add-to-sprite-aux-list and sp-free-particle inline.
 */
void block_2d_in_place(const ParticleMem& m,
                       const NativeArgs& args,
                       const BlockScan& s,
                       u64 system,
                       u32 m_info,
                       u32 m_vec,
                       u64 index0,
                       u32 count,
                       u32 clocks) {
  static const u32 orbiter_sym = sym_val_addr("sp-orbiter");
  static const u32 relaunch_sym = sym_val_addr("sp-relaunch-particle-2d");
  for (u32 i = 0; i < count; i++) {
    if (!s.valid[i]) {
      continue;
    }
    const u32 info = m_info + kInfoSize * i;
    const u32 vec = m_vec + kVecSize * i;
    const u64 index = index0 + i;
    const TimeVec tv = time_vec(m, clocks, info);
    const u64 frames = tv.frames;
    if (frames == 0) {
      if (m.u(info + 104) & 0x8080) {
        sp_aux_list_add(s.cls[i], system, info, vec, index, args);
      }
      if (m.s(info + 100) == 0) {
        sp_free_particle(system, index, info, vec, args);
      } else {
        const u32 flags = m.u(info + 104);
        m.set_u(info + 104, flags & ~32u);
        if (flags & 32) {
          m.set_u(vec + 44, m.u(info + 124));
        }
      }
      continue;
    }
    bool do_free = false;
    const s64 timer = m.s(info + 100);
    if (timer != -1) {
      if (timer == 0) {
        do_free = true;
      } else {
        m.set_u(info + 100, (u32)std::max((s32)(u32)(timer - (s64)frames), 0));
      }
    }
    if (!do_free) {
      const u32 flags0 = m.u(info + 104);
      const u32 flags = flags0 & ~32u;
      m.set_u(info + 104, flags);
      if (flags0 & 32) {
        m.set_u(vec + 44, m.u(info + 124));
      }

      if (flags & 0x8080) {
        sp_aux_list_add(s.cls[i], system, info, vec, index, args);
      } else if (s.cls[i] != SP_FN_NONE) {
        const u64 a[8] = {system, info, vec, index, 0, 0, 0, 0};
        sp_run_particle_fn(s.cls[i], m.u(info + 112), a, args);
      }

      const s64 next_launcher = m.s(info + 120);
      const s64 next_time = m.s(info + 116) - (s64)frames;
      if (next_launcher != 0) {
        m.set_u(info + 116, (u32)next_time);
        if (next_time - 1 < 0) {
          const u64 a[8] = {system, (u64)next_launcher, info, vec, 0, 0, 0, 0};
          sp_call_particle_fn(m.u(relaunch_sym), a, args);
        }
      }

      float vx = m.f(info + 16), vy = m.f(info + 20), vz = m.f(info + 24);
      const float vw = m.f(info + 28);
      const float rot_vz = m.f(info + 40), rot_vw = m.f(info + 44);
      const float fade_x = m.f(info + 48), fade_y = m.f(info + 52), fade_z = m.f(info + 56),
                  fade_w = m.f(info + 60);
      const float ax = m.f(info + 64) * tv.acc;
      const float ay = m.f(info + 68) * tv.acc;
      const float az = m.f(info + 72) * tv.acc;
      const u32 friction_bits = m.u(info + 96);
      float px = m.f(vec), py = m.f(vec + 4), pz = m.f(vec + 8), pw = m.f(vec + 12);
      float rot = m.f(vec + 24), sy = m.f(vec + 28);
      float cx = m.f(vec + 32), cy = m.f(vec + 36), cz = m.f(vec + 40), cw = m.f(vec + 44);
      vx = vx + ax;
      vy = vy + ay;
      vz = vz + az;
      if (friction_bits != 0) {
        const float a = 1.f - u2f(friction_bits);
        const float b = a * tv.friction;
        const float c = 1.f - b;
        vx = vx * c;
        vy = vy * c;
        vz = vz * c;
      }
      const float ft_vel = tv.vel;
      const float dpx = vx * ft_vel, dpy = vy * ft_vel, dpz = vz * ft_vel, dpw = vw * ft_vel;
      const float drot = rot_vz * ft_vel, dsy = rot_vw * ft_vel;
      const float dcx = fade_x * ft_vel, dcy = fade_y * ft_vel, dcz = fade_z * ft_vel,
                  dcw = fade_w * ft_vel;
      px = px + dpx;
      py = py + dpy;
      pz = pz + dpz;
      pw = pw + dpw;
      cx = cx + dcx;
      cy = cy + dcy;
      cz = cz + dcz;
      cw = cw + dcw;
      rot = rot + drot;
      sy = sy + dsy;
      cx = std::max(cx, 0.f);
      cy = std::max(cy, 0.f);
      cz = std::max(cz, 0.f);
      cw = std::max(cw, 0.f);
      m.set_f(info + 16, vx);
      m.set_f(info + 20, vy);
      m.set_f(info + 24, vz);
      m.set_f(vec, px);
      m.set_f(vec + 4, py);
      m.set_f(vec + 8, pz);
      m.set_f(vec + 12, pw);
      m.set_f(vec + 24, (float)(s32)(s16)(s32)rot);
      m.set_f(vec + 28, sy);
      m.set_f(vec + 32, cx);
      m.set_f(vec + 36, cy);
      m.set_f(vec + 40, cz);
      m.set_f(vec + 44, cw);

      // in a plain block: an orbiter whose centre the scan found untouched
      if (m.u(info + 104) & 64) {
        const u64 a[8] = {system, info, vec, index, 0, 0, 0, 0};
        native_call_goal(m.u(orbiter_sym), a, args);
      }

      do_free = particle_done(m.u(info + 104), m.u(vec + 32), m.u(vec + 36), m.u(vec + 40),
                              m.u(vec + 44), pw, sy);
    }
    if (do_free) {
      sp_free_particle(system, index, info, vec, args);
    }
  }
}

//! quaternion*! and quaternion-normalize! are GOAL's: their C++ (native_goalc_ops.h) can stand in
bool quaternion_ops_unchanged() {
  static const u32 mul_sym = sym_val_addr("quaternion*!");
  static const u32 normalize_sym = sym_val_addr("quaternion-normalize!");
  static const u32 mul_fn = gload<u32>(mul_sym);
  static const u32 normalize_fn = gload<u32>(normalize_sym);
  return mul_fn != 0 && normalize_fn != 0 && gload<u32>(mul_sym) == mul_fn &&
         gload<u32>(normalize_sym) == normalize_fn;
}

/*!
 * sp-process-block-3d on the particle tables themselves, for a block whose scan says plain: the
 * same steps in the same order as sp_process_block_3d_impl, with quaternion*! and
 * quaternion-normalize! as their C++ while the symbols hold GOAL's.
 */
void block_3d_in_place(const ParticleMem& m,
                       const NativeArgs& args,
                       const BlockScan& s,
                       u64 system,
                       u32 m_info,
                       u32 m_vec,
                       u64 index0,
                       u32 count,
                       u32 clocks) {
  static const u32 quaternion_mul_sym = sym_val_addr("quaternion*!");
  static const u32 quaternion_normalize_sym = sym_val_addr("quaternion-normalize!");
  static const u32 relaunch_sym = sym_val_addr("sp-relaunch-particle-3d");
  const bool quat_inline = quaternion_ops_unchanged();
  const u32 quat = args.stack - 16;
  for (u32 i = 0; i < count; i++) {
    if (!s.valid[i]) {
      continue;
    }
    const u32 info = m_info + kInfoSize * i;
    const u32 vec = m_vec + kVecSize * i;
    const u64 index = index0 + i;
    const TimeVec tv = time_vec(m, clocks, info);
    const u64 frames = tv.frames;
    if (frames == 0) {
      if (m.s(info + 100) == 0) {
        sp_free_particle(system, index, info, vec, args);
      } else {
        const u32 flags = m.u(info + 104);
        m.set_u(info + 104, flags & ~32u);
        if (flags & 32) {
          m.set_u(vec + 44, m.u(info + 124));
        }
      }
      continue;
    }
    bool do_free = false;
    const s64 timer = m.s(info + 100);
    if (timer != -1) {
      if (timer == 0) {
        do_free = true;
      } else {
        m.set_u(info + 100, (u32)std::max((s32)(u32)(timer - (s64)frames), 0));
      }
    }
    if (!do_free) {
      const u32 flags = m.u(info + 104);
      m.set_u(info + 104, flags & ~32u);
      if (flags & 32) {
        m.set_u(vec + 44, m.u(info + 124));
      }

      if (s.cls[i] != SP_FN_NONE) {
        const u64 a[8] = {system, info, vec, index, 0, 0, 0, 0};
        sp_run_particle_fn(s.cls[i], m.u(info + 112), a, args);
      }

      const s64 next_launcher = m.s(info + 120);
      const s64 next_time = m.s(info + 116) - (s64)frames;
      if (next_launcher != 0) {
        m.set_u(info + 116, (u32)next_time);
        if (next_time < 0) {
          const u64 a[8] = {system, (u64)next_launcher, info, vec, 0, 0, 0, 0};
          native_call_goal(m.u(relaunch_sym), a, args);
        }
      }

      float vx = m.f(info + 16), vy = m.f(info + 20), vz = m.f(info + 24);
      const float vw = m.f(info + 28);
      const float rot_vw = m.f(info + 44);
      const float fade_x = m.f(info + 48), fade_y = m.f(info + 52), fade_z = m.f(info + 56),
                  fade_w = m.f(info + 60);
      const float ax = m.f(info + 64) * tv.acc;
      const float ay = m.f(info + 68) * tv.acc;
      const float az = m.f(info + 72) * tv.acc;
      const u32 friction_bits = m.u(info + 96);
      float px = m.f(vec), py = m.f(vec + 4), pz = m.f(vec + 8), pw = m.f(vec + 12);
      const float qx = m.f(vec + 16), qy = m.f(vec + 20), qz = m.f(vec + 24);
      float sy = m.f(vec + 28);
      float cx = m.f(vec + 32), cy = m.f(vec + 36), cz = m.f(vec + 40), cw = m.f(vec + 44);
      vx = vx + ax;
      vy = vy + ay;
      vz = vz + az;
      if (friction_bits != 0) {
        const float a = 1.f - u2f(friction_bits);
        const float b = a * tv.friction;
        const float c = 1.f - b;
        vx = vx * c;
        vy = vy * c;
        vz = vz * c;
      }
      const float ft_vel = tv.vel;
      const float dpx = vx * ft_vel, dpy = vy * ft_vel, dpz = vz * ft_vel, dpw = vw * ft_vel;
      const float dsy = rot_vw * ft_vel;
      const float dcx = fade_x * ft_vel, dcy = fade_y * ft_vel, dcz = fade_z * ft_vel,
                  dcw = fade_w * ft_vel;
      px = px + dpx;
      py = py + dpy;
      pz = pz + dpz;
      pw = pw + dpw;
      sy = sy + dsy;
      cx = cx + dcx;
      cy = cy + dcy;
      cz = cz + dcz;
      cw = cw + dcw;
      cx = std::max(cx, 0.f);
      cy = std::max(cy, 0.f);
      cz = std::max(cz, 0.f);
      cw = std::max(cw, 0.f);
      m.set_f(info + 16, vx);
      m.set_f(info + 20, vy);
      m.set_f(info + 24, vz);
      m.set_f(vec, px);
      m.set_f(vec + 4, py);
      m.set_f(vec + 8, pz);
      m.set_f(vec + 12, pw);
      m.set_f(vec + 28, sy);
      m.set_f(vec + 32, cx);
      m.set_f(vec + 36, cy);
      m.set_f(vec + 40, cz);
      m.set_f(vec + 44, cw);

      float qw;
      {
        const float zz = qz * qz;
        float w = 1.f - zz;
        const float yy = qy * qy;
        w = w - yy;
        const float xx = qx * qx;
        w = w - xx;
        qw = std::sqrt(std::abs(w));
      }
      float rx, ry, rz, rw;
      const u32 rot_vel_3d = info + 80;
      if (quat_inline) {
        goalc_ops::v4 q = {qx, qy, qz, qw};
        s64 left = (s64)frames;
        do {
          const goalc_ops::v4 rv = {m.f(rot_vel_3d), m.f(rot_vel_3d + 4), m.f(rot_vel_3d + 8),
                                    m.f(rot_vel_3d + 12)};
          q = goalc_ops::quaternion_mul(q, rv);
          left -= 10;
        } while (left > 0);
        q = goalc_ops::quaternion_normalize(q);
        rx = q.x;
        ry = q.y;
        rz = q.z;
        rw = q.w;
      } else {
        m.set_f(quat, qx);
        m.set_f(quat + 4, qy);
        m.set_f(quat + 8, qz);
        m.set_f(quat + 12, qw);
        s64 left = (s64)frames;
        do {
          const u64 a[8] = {quat, quat, rot_vel_3d, 0, 0, 0, 0, 0};
          native_call_goal(m.u(quaternion_mul_sym), a, args);
          left -= 10;
        } while (left > 0);
        const u64 a[8] = {quat, 0, 0, 0, 0, 0, 0, 0};
        native_call_goal(m.u(quaternion_normalize_sym), a, args);
        rx = m.f(quat);
        ry = m.f(quat + 4);
        rz = m.f(quat + 8);
        rw = m.f(quat + 12);
      }
      if (rw < 0.f) {
        m.set_f(vec + 16, 0.f - rx);
        m.set_f(vec + 20, 0.f - ry);
        m.set_f(vec + 24, 0.f - rz);
      } else {
        m.set_f(vec + 16, 0.f + rx);
        m.set_f(vec + 20, 0.f + ry);
        m.set_f(vec + 24, 0.f + rz);
      }

      do_free = particle_done(m.u(info + 104), f2u(cx), f2u(cy), f2u(cz), f2u(cw), pw, sy);
    }
    if (do_free) {
      sp_free_particle(system, index, info, vec, args);
    }
  }
}

struct BlockStats {
  u32 blocks = 0, in_place = 0, copied = 0, empty = 0, particles = 0;
  const char* why = "";
};
BlockStats g_block_stats;

enum BlockPath { BLOCK_EMPTY, BLOCK_IN_PLACE, BLOCK_COPIED };

/*!
 * One block's update. A block whose particles' functions only touch their own particle is
 * updated where it is: no function reads or writes the tables behind the copies then, so the
 * copy in, the update on the copies and the copy back give the same bytes. Any other block is
 * updated in copies on the scratchpad as sp-process-block does it, without the copies of free
 * particles (the copy gets its in-use byte cleared) and of the adgifs.
 */
BlockPath process_block(const NativeArgs& args,
                        u32 count,
                        bool is_3d,
                        u32 m_info,
                        u32 m_vec,
                        u32 spad) {
  const ParticleMem m;
  const u32 s_info = spad + kSpadBlock;
  const u32 s_vec = s_info + kInfoSize * count;
  BlockScan s;
  scan_block(m, is_3d, m_info, m_vec, count, spad, s);
  g_block_stats.particles += s.used;
  if (s.used == 0) {
    return BLOCK_EMPTY;
  }
  if (s.plain) {
    sp_set_block_range(m_info, m_info + kInfoSize * count);
    if (is_3d) {
      block_3d_in_place(m, args, s, args.a[0], m_info, m_vec, args.a[1], count, spad);
    } else {
      block_2d_in_place(m, args, s, args.a[0], m_info, m_vec, args.a[1], count, spad);
    }
    sp_set_block_range(0, 0);
    return BLOCK_IN_PLACE;
  }
  g_block_stats.why = s.why;
  for (u32 i = 0; i < count; i++) {
    if (s.valid[i]) {
      copy_bytes(s_info + kInfoSize * i, m_info + kInfoSize * i, kInfoSize);
      copy_bytes(s_vec + kVecSize * i, m_vec + kVecSize * i, kVecSize);
    } else {
      MIPS2C_NATIVE_LOG_STORE(s_info + kInfoSize * i + kInfoValid, 1);
      g_ee_main_mem[s_info + kInfoSize * i + kInfoValid] = 0;
    }
  }
  const u64 a[8] = {args.a[0], s_info, s_vec, args.a[1], args.a[2], args.a[3], 0, 0};
  const NativeArgs inner{a, args.pp, args.st, args.stack};
  if (is_3d) {
    sp_process_block_3d_impl(inner);
  } else {
    sp_process_block_2d_impl(inner);
  }
  for (u32 i = 0; i < count; i++) {
    if (s.valid[i]) {
      copy_bytes(m_info + kInfoSize * i, s_info + kInfoSize * i, kInfoSize);
      copy_bytes(m_vec + kVecSize * i, s_vec + kVecSize * i, kVecSize);
    }
  }
  return BLOCK_COPIED;
}

#if MIPS2C_WRITE_LOG
// host check (OPENGOAL_MIPS2C_VERIFY=1)
int g_block_verify = -1;
u64 g_block_verify_blocks = 0, g_block_verify_free = 0, g_block_verify_differ = 0;
u64 g_block_verify_whole = 0, g_block_verify_path[3] = {0, 0, 0};

bool block_verify_on() {
  if (g_block_verify < 0) {
    const char* v = getenv("OPENGOAL_MIPS2C_VERIFY");
    g_block_verify = (v && v[0] && v[0] != '0') ? 1 : 0;
    if (g_block_verify) {
      atexit([] {
        lg::info("sp-process-block verify: {} blocks by sp-process-block's steps, {} free "
                 "particles compared, {} differ; {} blocks compared whole with "
                 "pc-sp-process-block ({} in place, {} in copies, {} empty: see the inline verify "
                 "line for mismatches)",
                 g_block_verify_blocks, g_block_verify_free, g_block_verify_differ,
                 g_block_verify_whole, g_block_verify_path[BLOCK_IN_PLACE],
                 g_block_verify_path[BLOCK_COPIED], g_block_verify_path[BLOCK_EMPTY]);
      });
    }
  }
  return g_block_verify == 1;
}
#endif

/*!
 * (pc-sp-process-block system index count paused?) -> #t: sp-process-block (sparticle.gc) for the
 * count particles from index (process_block). #f: the GOAL code runs.
 * Verify mode: one call in OPENGOAL_SP_BLOCK_VERIFY_EVERY (default 8) runs sp-process-block's own
 * steps and process_block from the same state and compares all of GOAL memory and the random
 * generators (native_verify_inline). The other calls run sp-process-block's steps with the block
 * functions through their symbols (so they are compared with mips2c as before) and check that the
 * copy back of every free particle would write the bytes that are there.
 */
CTR_HOT u64 pc_sp_process_block_impl(const NativeArgs& args) {
  static const u32 spad_sym = sym_val_addr("*fake-scratchpad-data*");
  static const bool off = native_turned_off("sp-process-block-2d") ||
                          native_turned_off("sp-process-block-3d") ||
                          native_turned_off("pc-sp-process-block");
  const u32 sys = (u32)args.a[0];
  const u32 index = (u32)args.a[1];
  const u32 count = (u32)args.a[2];
  if (off || count == 0 || count > kMaxBlock) {
    return args.st;
  }
  const bool is_3d = gload<u32>(sys + kSysIs3d) != (u32)args.st;
  const u32 m_info = gload<u32>(sys + kSysCpuinfo) + kInfoSize * index;
  const u32 m_vec = gload<u32>(sys + kSysVecdata) + kVecSize * index;
  const u32 spad = gload<u32>(spad_sym);

#if MIPS2C_WRITE_LOG
  if (block_verify_on()) {
    const u32 s_info = spad + kSpadBlock;
    const u32 s_vec = s_info + kInfoSize * count;
    const u32 m_adgif = gload<u32>(sys + kSysAdgif) + kAdgifSize * index;
    const u64 a[8] = {args.a[0], s_info, s_vec, args.a[1], args.a[2], args.a[3], 0, 0};
    static const u64 every = [] {
      const char* v = getenv("OPENGOAL_SP_BLOCK_VERIFY_EVERY");
      return v ? strtoull(v, nullptr, 10) : (u64)8;
    }();
    static u64 calls = 0;
    if (every && (calls++ % every) == 0 && native_verify_inline_on()) {
      // the scratchpad copies of the reference run (scratch data: made the same after both runs)
      static u8 ref_copies[0x4000];
      const u32 copies_size = (kInfoSize + kVecSize + kAdgifSize) * count;
      BlockPath path = BLOCK_EMPTY;
      native_verify_inline(
          "pc-sp-process-block", {},
          [&]() -> u64 {
            path = process_block(args, count, is_3d, m_info, m_vec, spad);
            memcpy(g_ee_main_mem + s_info, ref_copies, copies_size);
            return 0;
          },
          [&]() -> u64 {
            copy_bytes(s_info, m_info, kInfoSize * count);
            copy_bytes(s_vec, m_vec, kVecSize * count);
            copy_bytes(s_vec + kVecSize * count, m_adgif, kAdgifSize * count);
            const NativeArgs inner{a, args.pp, args.st, args.stack};
            if (is_3d) {
              sp_process_block_3d_impl(inner);
            } else {
              sp_process_block_2d_impl(inner);
            }
            copy_bytes(m_info, s_info, kInfoSize * count);
            copy_bytes(m_vec, s_vec, kVecSize * count);
            memcpy(ref_copies, g_ee_main_mem + s_info, copies_size);
            return 0;
          },
          true);
      g_block_verify_whole++;
      g_block_verify_path[path]++;
      return goal_true(args.st);
    }
    static const u32 fn_2d = sym_val_addr("sp-process-block-2d");
    static const u32 fn_3d = sym_val_addr("sp-process-block-3d");
    u8 valid[kMaxBlock];
    for (u32 i = 0; i < count; i++) {
      valid[i] = g_ee_main_mem[m_info + kInfoSize * i + kInfoValid];
    }
    copy_bytes(s_info, m_info, kInfoSize * count);
    copy_bytes(s_vec, m_vec, kVecSize * count);
    copy_bytes(s_vec + kVecSize * count, m_adgif, kAdgifSize * count);
    native_call_goal(gload<u32>(is_3d ? fn_3d : fn_2d), a, args);
    g_block_verify_blocks++;
    for (u32 i = 0; i < count; i++) {
      if (valid[i]) {
        continue;
      }
      g_block_verify_free++;
      if (memcmp(g_ee_main_mem + s_info + kInfoSize * i, g_ee_main_mem + m_info + kInfoSize * i,
                 kInfoSize) ||
          memcmp(g_ee_main_mem + s_vec + kVecSize * i, g_ee_main_mem + m_vec + kVecSize * i,
                 kVecSize)) {
        if (g_block_verify_differ++ < 8) {
          lg::error("sp-process-block verify MISMATCH: free particle {} of a {} block changed",
                    index + i, is_3d ? "3d" : "2d");
        }
      }
    }
    copy_bytes(m_info, s_info, kInfoSize * count);
    copy_bytes(m_vec, s_vec, kVecSize * count);
    return goal_true(args.st);
  }
#endif

  const BlockPath path = process_block(args, count, is_3d, m_info, m_vec, spad);
  BlockStats& stats = g_block_stats;
  stats.blocks++;
  stats.in_place += path == BLOCK_IN_PLACE;
  stats.copied += path == BLOCK_COPIED;
  stats.empty += path == BLOCK_EMPTY;
  if (stats.blocks >= 4096) {
    if (kperf::g_sections_enabled) {
      lg::info("particles: {} blocks: {} in place, {} in copies ({}), {} empty; {} particles",
               stats.blocks, stats.in_place, stats.copied, stats.why, stats.empty,
               stats.particles);
    }
    stats = BlockStats();
  }
  return goal_true(args.st);
}

}  // namespace

const NativeImpl sp_process_block_2d =
    MIPS2C_NATIVE_IMPL(sp_process_block_2d_impl, NATIVE_CALLS_GOAL, 0);
// scratch: the quaternion passed to quaternion*!
const NativeImpl sp_process_block_3d =
    MIPS2C_NATIVE_IMPL(sp_process_block_3d_impl, NATIVE_CALLS_GOAL, 16);
// scratch: sp-process-block-3d's
const NativeImpl pc_sp_process_block =
    MIPS2C_NATIVE_IMPL(pc_sp_process_block_impl, NATIVE_CALLS_GOAL, 16);

}  // namespace Mips2C::jak2::native
