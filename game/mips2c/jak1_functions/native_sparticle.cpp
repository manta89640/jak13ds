/*!
 * @file native_sparticle.cpp
 * (AI-assisted)
 * Native versions of the mips2c functions in sparticle.cpp. See game/mips2c/mips2c_native.h for
 * the rules (same float operations, grouped the same way, as the mips2c code).
 */

#include <algorithm>

#include "game/kernel/jak1/kscheme.h"
#include "game/mips2c/jak1_functions/native_functions.h"

namespace Mips2C::jak1::native {

namespace {

u32 sym_addr(const char* name) {
  return ::jak1::intern_from_c(name).offset;
}

inline s64 load_s32(u32 addr) {
  return gload<s32>(addr);
}

/*!
 * (sp-process-block-2d system cpuinfo vecdata index count paused?) -> index + count
 * Updates count 2d particles (sparticle-cpuinfo, 144 bytes, and sprite-vec-data-2d, 48 bytes):
 * timers, the user callback, relaunching, velocity (with acceleration and friction), position,
 * rotation and scale, the orbiter, and frees the ones that are done.
 */
u64 sp_process_block_2d_impl(const NativeArgs& args) {
  static const u32 frame_time_sym = sym_addr("*sp-frame-time*");
  static const u32 free_particle_sym = sym_addr("sp-free-particle");
  static const u32 orbiter_sym = sym_addr("sp-orbiter");
  static const u32 relaunch_sym = sym_addr("sp-relaunch-particle-2d");

  const u64 system = args.a[0];
  u32 info = (u32)args.a[1];
  u32 vec = (u32)args.a[2];
  u64 index = args.a[3];
  u64 count = args.a[4];
  const bool paused = args.a[5] != args.st;
  const u64 st = args.st;

  // x: the frame count (int), y: velocity scale, z: acceleration scale, w: friction scale
  float frame_time[4];
  memcpy(frame_time, gptr(gload<u32>(frame_time_sym)), 16);
  const u64 frames = f2u(frame_time[0]) & 255;

  // a3 is not set by this function before calling the callbacks: it is the argument, or the last
  // vecdata passed to sp-relaunch-particle-2d / sp-free-particle
  u64 reg_a3 = args.a[3];
  auto call = [&](u32 fn, u64 a0, u64 a1, u64 a2, u64 a3) {
    reg_a3 = a3;
    const u64 call_args[8] = {a0, a1, a2, a3, args.a[4], args.a[5], args.a[6], args.a[7]};
    native_call_goal(fn, call_args, args);
  };
  auto free_particle = [&]() { call(gload<u32>(free_particle_sym), system, index, info, vec); };

  do {
    const s64 key = load_s32(info + 128);
    if (key == (s64)st) {
      // not in use
    } else if (paused && !(gload<u32>(info + 104) & 8192)) {
      // paused: only count down timers that are already done
      const s64 timer = load_s32(info + 100);
      if (timer != -1 && timer == 0) {
        free_particle();
      } else {
        const u32 flags = gload<u32>(info + 104);
        gstore<u32>(info + 104, flags ^ (flags & 64));
        if (flags & 64) {
          gstore<u32>(vec + 44, gload<u32>(info + 124));
        }
      }
    } else {
      bool do_free = false;
      const s64 timer = load_s32(info + 100);
      if (timer != -1) {
        if (timer == 0) {
          do_free = true;
        } else {
          gstore<s32>(info + 100, std::max((s32)(u32)(timer - frames), 0));
        }
      }
      if (!do_free) {
        const u32 flags = gload<u32>(info + 104);
        gstore<u32>(info + 104, flags ^ (flags & 64));
        if (flags & 64) {
          gstore<u32>(vec + 44, gload<u32>(info + 124));
        }

        // user callback
        if (const u32 callback = gload<u32>(info + 112)) {
          call(callback, system, info, vec, reg_a3);
        }

        // relaunch
        const s64 next_launcher = load_s32(info + 120);
        const s64 next_time = load_s32(info + 116) - (s64)frames;
        if (next_launcher != 0) {
          gstore<u32>(info + 116, (u32)next_time);
          if (next_time - 1 < 0) {
            call(gload<u32>(relaunch_sym), system, (u64)next_launcher, info, vec);
          }
        }

        // motion
        float v1[4], v2[4], v3[4], vel[4], rot_vel[4], scale_vel[4], accel[4];
        memcpy(v1, gptr(vec), 16);
        memcpy(v2, gptr(vec + 16), 16);
        memcpy(v3, gptr(vec + 32), 16);
        memcpy(vel, gptr(info + 16), 16);
        memcpy(rot_vel, gptr(info + 32), 16);
        memcpy(scale_vel, gptr(info + 48), 16);
        memcpy(accel, gptr(info + 64), 16);
        const u32 friction_bits = gload<u32>(info + 96);
        for (int i = 0; i < 4; i++) {
          accel[i] = accel[i] * frame_time[2];
        }
        for (int i = 0; i < 3; i++) {
          vel[i] = vel[i] + accel[i];
        }
        if (friction_bits != 0) {
          const float a = 1.f - u2f(friction_bits);
          const float b = a * frame_time[3];
          const float c = 1.f - b;
          for (int i = 0; i < 3; i++) {
            vel[i] = vel[i] * c;
          }
        }
        float d1[4], d2[4], d3[4];
        for (int i = 0; i < 4; i++) {
          d1[i] = vel[i] * frame_time[1];
          d2[i] = rot_vel[i] * frame_time[1];
          d3[i] = scale_vel[i] * frame_time[1];
        }
        for (int i = 0; i < 4; i++) {
          v1[i] = v1[i] + d1[i];
          v3[i] = v3[i] + d3[i];
        }
        for (int i = 2; i < 4; i++) {
          v2[i] = v2[i] + d2[i];
        }
        for (int i = 0; i < 4; i++) {
          v3[i] = std::max(v3[i], 0.f);
        }
        gstore_bytes(info + 16, vel, 16);
        gstore_bytes(vec, v1, 16);
        gstore_bytes(vec + 16, v2, 16);
        gstore_bytes(vec + 32, v3, 16);
        // the rotation (vec + 24) wraps like a 16-bit int
        const s32 rot = (s32)gload<float>(vec + 24);
        gstore<float>(vec + 24, (float)(s32)(s16)rot);

        if (gload<u32>(info + 104) & 128) {
          call(gload<u32>(orbiter_sym), system, info, vec, reg_a3);
        }

        // done?
        u32 scale[4];
        memcpy(scale, gptr(vec + 32), 16);
        const u32 flags2 = gload<u32>(info + 104);
        if ((flags2 & 2) && scale[0] == 0 && scale[1] == 0 && scale[2] == 0) {
          do_free = true;  // scale x, y and z are 0
        } else if ((flags2 & 4) && (s32)scale[3] <= 0) {
          do_free = true;  // alpha
        } else if ((flags2 & 1) && ((s32)f2u(v1[3]) < 0 || (s32)f2u(v2[3]) < 0)) {
          do_free = true;  // w of the position or of the second vector below 0
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

}  // namespace Mips2C::jak1::native
