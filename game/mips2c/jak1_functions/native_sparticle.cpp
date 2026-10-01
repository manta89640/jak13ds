/*!
 * @file native_sparticle.cpp
 * (AI-assisted)
 * Native versions of the mips2c functions in sparticle.cpp. See game/mips2c/mips2c_native.h for
 * the rules (same float operations, grouped the same way, as the mips2c code).
 */

#include <algorithm>
#include <cmath>

#include "game/kernel/jak1/kscheme.h"
#include "game/mips2c/jak1_functions/native_functions.h"

#if defined(__GNUC__) || defined(__clang__)
#define NATIVE_ALIGNED4(p) __builtin_assume_aligned((p), 4)
#else
#define NATIVE_ALIGNED4(p) (p)
#endif

namespace Mips2C::jak1::native {

namespace {

u32 sym_addr(const char* name) {
  return ::jak1::intern_from_c(name).offset;
}

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
// func 112, next-time 116, next-launcher 120, cache-alpha 124, valid 128, key 132, binding 136.
// The vecdata (48 bytes): x y z sx, then (2d) flag matrix rot sy or (3d) qx qy qz sy, r g b a.

/*!
 * (sp-process-block-2d system cpuinfo vecdata index count paused?) -> index + count
 * Updates count 2d particles (sparticle-cpuinfo and sprite-vec-data-2d): timers, the user
 * callback, relaunching, velocity (with acceleration and friction), position, rotation and color,
 * the orbiter, and frees the ones that are done.
 */
u64 sp_process_block_2d_impl(const NativeArgs& args) {
  static const u32 frame_time_sym = sym_addr("*sp-frame-time*");
  static const u32 free_particle_sym = sym_addr("sp-free-particle");
  static const u32 orbiter_sym = sym_addr("sp-orbiter");
  static const u32 relaunch_sym = sym_addr("sp-relaunch-particle-2d");

  const ParticleMem m;
  const u64 system = args.a[0];
  u32 info = (u32)args.a[1];
  u32 vec = (u32)args.a[2];
  u64 index = args.a[3];
  u64 count = args.a[4];
  const bool paused = args.a[5] != args.st;
  const s64 st = (s64)args.st;

  // x: the frame count (int), y: velocity scale, z: acceleration scale, w: friction scale
  const u32 frame_time = m.u(frame_time_sym);
  const u64 frames = m.u(frame_time) & 255;
  const float ft_vel = m.f(frame_time + 4);
  const float ft_acc = m.f(frame_time + 8);
  const float ft_friction = m.f(frame_time + 12);

  // a3 is not set by this function before calling the callbacks: it is the argument, or the last
  // vecdata passed to sp-relaunch-particle-2d / sp-free-particle
  u64 reg_a3 = args.a[3];
  auto call = [&](u32 fn, u64 a0, u64 a1, u64 a2, u64 a3) {
    reg_a3 = a3;
    const u64 call_args[8] = {a0, a1, a2, a3, args.a[4], args.a[5], args.a[6], args.a[7]};
    native_call_goal(fn, call_args, args);
  };
  auto free_particle = [&]() { call(m.u(free_particle_sym), system, index, info, vec); };

  do {
    if (m.s(info + 128) == st) {
      // not in use
    } else if (paused && !(m.u(info + 104) & 8192)) {
      // paused: only free the ones whose timer is done
      if (m.s(info + 100) == 0) {
        free_particle();
      } else {
        const u32 flags = m.u(info + 104);
        m.set_u(info + 104, flags & ~64u);
        if (flags & 64) {
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
          m.set_u(info + 100, (u32)std::max((s32)(u32)(timer - frames), 0));
        }
      }
      if (!do_free) {
        const u32 flags = m.u(info + 104);
        m.set_u(info + 104, flags & ~64u);
        if (flags & 64) {
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
        const float ax = m.f(info + 64) * ft_acc;
        const float ay = m.f(info + 68) * ft_acc;
        const float az = m.f(info + 72) * ft_acc;
        const u32 friction_bits = m.u(info + 96);
        float px = m.f(vec), py = m.f(vec + 4), pz = m.f(vec + 8), pw = m.f(vec + 12);
        float rot = m.f(vec + 24), sy = m.f(vec + 28);
        float cx = m.f(vec + 32), cy = m.f(vec + 36), cz = m.f(vec + 40), cw = m.f(vec + 44);
        vx = vx + ax;
        vy = vy + ay;
        vz = vz + az;
        if (friction_bits != 0) {
          const float a = 1.f - u2f(friction_bits);
          const float b = a * ft_friction;
          const float c = 1.f - b;
          vx = vx * c;
          vy = vy * c;
          vz = vz * c;
        }
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

        if (m.u(info + 104) & 128) {
          call(m.u(orbiter_sym), system, info, vec, reg_a3);
        }

        // done? (the color as it is now in memory, the sizes as computed)
        const u32 flags2 = m.u(info + 104);
        if ((flags2 & 2) && m.u(vec + 32) == 0 && m.u(vec + 36) == 0 && m.u(vec + 40) == 0) {
          do_free = true;  // r, g and b are 0
        } else if ((flags2 & 4) && (s32)m.u(vec + 44) <= 0) {
          do_free = true;  // alpha
        } else if ((flags2 & 1) && ((s32)f2u(pw) < 0 || (s32)f2u(sy) < 0)) {
          do_free = true;  // x or y size below 0
        }
      }
      if (do_free) {
        free_particle();
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
 * times the particle's rotvel3d, twice at 30 frames per second and below, stored with w >= 0.
 * No orbiter. Frees the ones that are done.
 */
u64 sp_process_block_3d_impl(const NativeArgs& args) {
  static const u32 frame_time_sym = sym_addr("*sp-frame-time*");
  static const u32 quaternion_mul_sym = sym_addr("quaternion*!");
  static const u32 free_particle_sym = sym_addr("sp-free-particle");
  static const u32 relaunch_sym = sym_addr("sp-relaunch-particle-3d");

  const ParticleMem m;
  const u64 system = args.a[0];
  u32 info = (u32)args.a[1];
  u32 vec = (u32)args.a[2];
  u64 index = args.a[3];
  u64 count = args.a[4];
  const bool paused = args.a[5] != args.st;
  const s64 st = (s64)args.st;
  // the quaternion passed to quaternion*! (the mips2c version's stack frame)
  const u32 quat = args.stack - 16;

  // x: the frame count (int), y: velocity scale, z: acceleration scale, w: friction scale
  const u32 frame_time = m.u(frame_time_sym);
  const u64 frames = m.u(frame_time) & 255;
  const float ft_vel = m.f(frame_time + 4);
  const float ft_acc = m.f(frame_time + 8);
  const float ft_friction = m.f(frame_time + 12);

  // a3 is not set by this function before calling the callbacks and quaternion*!: it is the
  // argument, or the last vecdata passed to sp-relaunch-particle-3d / sp-free-particle
  u64 reg_a3 = args.a[3];
  auto call = [&](u32 fn, u64 a0, u64 a1, u64 a2, u64 a3) {
    reg_a3 = a3;
    const u64 call_args[8] = {a0, a1, a2, a3, args.a[4], args.a[5], args.a[6], args.a[7]};
    native_call_goal(fn, call_args, args);
  };
  auto free_particle = [&]() { call(m.u(free_particle_sym), system, index, info, vec); };

  do {
    if (m.s(info + 128) == st) {
      // not in use
    } else if (paused && !(m.u(info + 104) & 8192)) {
      // paused: only free the ones whose timer is done
      if (m.s(info + 100) == 0) {
        free_particle();
      } else {
        const u32 flags = m.u(info + 104);
        m.set_u(info + 104, flags & ~64u);
        if (flags & 64) {
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
          m.set_u(info + 100, (u32)std::max((s32)(u32)(timer - frames), 0));
        }
      }
      if (!do_free) {
        const u32 flags = m.u(info + 104);
        m.set_u(info + 104, flags & ~64u);
        if (flags & 64) {
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
        const float ax = m.f(info + 64) * ft_acc;
        const float ay = m.f(info + 68) * ft_acc;
        const float az = m.f(info + 72) * ft_acc;
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
          const float b = a * ft_friction;
          const float c = 1.f - b;
          vx = vx * c;
          vy = vy * c;
          vz = vz * c;
        }
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

        // the rotation: w from x y z, times rotvel3d (twice at 10 or more frames)
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
        const u32 frame_bits = m.u(m.u(frame_time_sym));
        const u32 rot_vel_3d = info + 80;
        if ((s64)(frame_bits & 255) - 10 >= 0) {
          call(m.u(quaternion_mul_sym), quat, quat, rot_vel_3d, reg_a3);
        }
        call(m.u(quaternion_mul_sym), quat, quat, rot_vel_3d, reg_a3);
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
        const u32 flags2 = m.u(info + 104);
        if ((flags2 & 2) && f2u(cx) == 0 && f2u(cy) == 0 && f2u(cz) == 0) {
          do_free = true;  // r, g and b are 0
        } else if ((flags2 & 4) && (s32)f2u(cw) <= 0) {
          do_free = true;  // alpha
        } else if ((flags2 & 1) && ((s32)f2u(pw) < 0 || (s32)f2u(sy) < 0)) {
          do_free = true;  // x or y size below 0
        }
      }
      if (do_free) {
        free_particle();
      }
    }
    count--;
    info += 144;
    vec += 48;
    index++;
  } while (count != 0);
  return index;
}

}  // namespace

const NativeImpl sp_process_block_2d =
    MIPS2C_NATIVE_IMPL(sp_process_block_2d_impl, NATIVE_CALLS_GOAL, 0);
// scratch: the quaternion passed to quaternion*!
const NativeImpl sp_process_block_3d =
    MIPS2C_NATIVE_IMPL(sp_process_block_3d_impl, NATIVE_CALLS_GOAL, 16);

}  // namespace Mips2C::jak1::native
