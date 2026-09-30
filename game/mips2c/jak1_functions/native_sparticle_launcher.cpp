/*!
 * @file native_sparticle_launcher.cpp
 * (AI-assisted)
 * Native versions of the mips2c functions in sparticle_launcher.cpp. See
 * game/mips2c/mips2c_native.h for the rules (same float operations, grouped the same way, as the
 * mips2c code).
 */

#include <algorithm>
#include <cstdio>

#include "game/kernel/jak1/kscheme.h"
#include "game/mips2c/jak1_functions/native_functions.h"

namespace Mips2C::jak1::native {

namespace {

u32 sym_addr(const char* name) {
  return ::jak1::intern_from_c(name).offset;
}

//! lw: a word, sign-extended like the 64-bit register it goes to
inline u64 lw(u32 addr) {
  return (u64)(s64)gload<s32>(addr);
}

//! lh: a halfword, sign-extended
inline u64 lh(u32 addr) {
  return (u64)(s64)gload<s16>(addr);
}

}  // namespace

/*!
 * (particle-adgif adgif texture-id): copies the particle adgif shader for the texture (80 bytes)
 * from *particle-adgif-cache*, which sets it up with particle-setup-adgif the first time the
 * texture is used. The cache is keyed by a 16-bit hash of the id, the last one used is checked
 * first. When it is full (80 textures), the shader is set up in place instead.
 */
u64 particle_adgif_impl(const NativeArgs& args) {
  static const u32 cache_sym = sym_addr("*particle-adgif-cache*");
  static const u32 setup_sym = sym_addr("particle-setup-adgif");

  const u64 tex = args.a[1];
  const u64 key = (u64)((((s64)tex >> 20) ^ ((s64)tex >> 8)) & 0xffff);
  const u32 cache = gload<u32>(cache_sym);
  // (particle-setup-adgif adgif texture-id), with the other registers as the original has them
  auto setup = [&](u64 adgif, u32 key_slot) {
    const u64 call_args[8] = {adgif, tex,       args.a[2], key, (u64)((s64)tex >> 8),
                              lw(cache_sym), 0, key_slot};
    native_call_goal(gload<u32>(setup_sym), call_args, args);
  };

  u32 entry = gload<u32>(cache + 8);  // the last one used
  if (gload<u16>(cache + 4) != key || entry == 0) {
    if (gload<u16>(cache + 4) == key) {
      printf("HACK: ignoring cached adgif to avoid crash on particle with unset texture.\n");
    }
    // tidhash (80 u16 at 12), spadgif (80 adgif shaders at 172)
    s64 n = (s32)gload<u32>(cache);
    u32 slot = cache + 12;
    entry = cache + 172;
    bool found = false;
    if (n != 0) {
      do {
        const u16 k = gload<u16>(slot);
        slot += 2;
        n--;
        if (k == key) {
          found = true;
          break;
        }
        entry += 80;
      } while (n != 0);
    }
    if (!found) {
      const s64 used = (s32)gload<u32>(cache);
      if (used - 80 == 0) {
        // full: set up the shader in place, every time
        setup(args.a[0], slot);
        return 0;
      }
      gstore<u16>(slot, (u16)key);
      gstore<u32>(cache, (u32)(used + 1));
      setup(entry, slot);
    }
  }
  u8 shader[80];
  memcpy(shader, gptr(entry), 80);
  gstore_bytes((u32)args.a[0], shader, 80);
  gstore<u32>(cache + 8, entry);
  gstore<u16>(cache + 4, (u16)key);
  return 0;
}

namespace {

/*!
 * (sp-launch-particles-var system launcher pos launch-state launch-control rate)
 * Launches the particles of a launcher at pos: num-to-birth times rate is added to the launcher's
 * (or the launch state's) accumulator, and a particle is launched for each whole one: its fields
 * from the launcher's init-specs (sp-init-fields!), the launch adjustments and rotations, its
 * adgif (particle-adgif), the birth function and the launch sound, and its sprite.
 * While particles are being processed (*sp-launcher-lock*), the launch is queued.
 *
 * The stack frame (GOAL memory: the functions called get addresses in it) is the original's:
 * 12: the specs after the launcher's, 64: pos, 80: the specs after the particle's, 96: the
 * sparticle-birthinfo (sprite, anim, anim-speed, birth-func, joint-ppoint, num-to-birth, sound),
 * 128: the sparticle-launchinfo / sprite (x y z sx, flag matrix rot sy, r g b a).
 */
u64 sp_launch_particles_var_impl(const NativeArgs& args) {
  static const u32 level_sym = sym_addr("*level*");
  static const u32 enable_sym = sym_addr("*sp-launcher-enable*");
  static const u32 lock_sym = sym_addr("*sp-launcher-lock*");
  static const u32 time_of_day_sym = sym_addr("*time-of-day-context*");
  static const u32 aux_list_sym = sym_addr("add-to-sprite-aux-list");
  static const u32 cos_sym = sym_addr("cos");
  static const u32 new_sound_id_sym = sym_addr("new-sound-id");
  static const u32 particle_adgif_sym = sym_addr("particle-adgif");
  static const u32 axis_angle_sym = sym_addr("quaternion-axis-angle!");
  static const u32 sin_sym = sym_addr("sin");
  static const u32 play_sym = sym_addr("sound-play-by-spec");
  static const u32 adjust_sym = sym_addr("sp-adjust-launch");
  static const u32 euler_sym = sym_addr("sp-euler-convert");
  static const u32 get_particle_sym = sym_addr("sp-get-particle");
  static const u32 init_fields_sym = sym_addr("sp-init-fields!");
  static const u32 queue_sym = sym_addr("sp-queue-launch");
  static const u32 rotate_sym = sym_addr("sp-rotate-system");
  static const u32* particle_adgif_stub = native_stub_slot("particle-adgif");

  const u64 st = args.st;
  // the argument registers (a0-a3, t0-t3) as the original has them at each call
  u64 r[8];
  memcpy(r, args.a, sizeof(r));
  u64 v0 = 0;
  // the functions it calls get the original's s6 as their process pointer: pp at first, then
  // the system's is-3d (the original uses s6 for it)
  NativeArgs callee = args;
  auto call = [&](u32 fn) {
    v0 = native_call_goal(fn, r, callee);
    return v0;
  };

  if (lw(enable_sym) == st) {
    return 0;
  }
  if (lw(lock_sym) != st) {
    // launched while the particles are processed: queued, unless there is a launch state, a
    // launch control or a rate other than 1, where the original crashes on purpose
    if (args.a[3] == st && args.a[4] == st && args.a[5] == 0x3f800000) {
      call(gload<u32>(queue_sym));
      return v0;
    }
    gstore<u64>(2, 0);  // sd r0, 2(r0)
  }

  const u64 system = args.a[0];
  const u64 launcher = args.a[1];
  const u64 state = args.a[3];
  const u64 control = args.a[4];
  const float rate = u2f((u32)args.a[5]);
  const u32 sp = args.stack - 256;

  float pos[4];
  memcpy(pos, gptr((u32)args.a[2]), 16);
  gstore_bytes(sp + 64, pos, 16);
  const u64 is_3d = lw((u32)system + 24);
  callee.pp = is_3d;
  callee.stack = sp;

  // the birthinfo
  r[0] = sp + 96;
  r[1] = lw((u32)launcher + 8);
  r[2] = 0;
  r[3] = 8;
  r[4] = st + 8;
  call(gload<u32>(init_fields_sym));
  gstore<u32>(sp + 12, (u32)v0);
  const float births = gload<float>(sp + 116) * rate;
  // the accumulator: the launch state's, or the launcher's
  const u32 accum_addr = state != st ? (u32)state + 24 : (u32)launcher;
  {
    const float accum = gload<float>(accum_addr) + births;
    gstore<float>(accum_addr, accum);
    if ((s32)accum == 0) {
      return v0;
    }
  }

  while (true) {
    // the particle, from group 1 and bound to the launch state if the process says so
    r[1] = 0;
    r[2] = st;
    if (control != st) {
      const u64 proc = lw((u32)control + 12);
      if (proc != st) {
        const u64 mask = lh((u32)proc + 6);
        r[0] = mask & 4;
        r[3] = 1;
        if (r[0]) {
          r[1] = 1;
        }
        r[0] = mask & 8;
        if (r[0]) {
          r[2] = state;
        }
      }
    }
    r[0] = system;
    const u64 cpu = call(gload<u32>(get_particle_sym));
    if (cpu == st) {
      return v0;
    }
    const u32 info = (u32)cpu;

    // the launchinfo / sprite, then the particle's cpuinfo from omega on
    r[0] = sp + 128;
    r[1] = lw(sp + 12);
    r[2] = 9;
    r[3] = 22;
    r[4] = st + 8;
    const u32 init_fields = gload<u32>(init_fields_sym);
    call(init_fields);
    r[0] = cpu + 12;
    r[1] = v0;
    r[2] = 23;
    r[3] = 52;
    r[4] = st + 8;
    call(init_fields);
    gstore<u32>(sp + 80, (u32)v0);
    u64 flags = lw(info + 104);

    if (is_3d == st) {
      // 2d: the sprite's matrix, and its rotation wraps like a 16-bit int
      bool matrix_set = false;
      if (control != st) {
        const u64 proc = lw((u32)control + 12);
        if (proc != st && (lh((u32)proc + 6) & 4)) {
          gstore<u32>(sp + 148, (flags & 256) ? 1 : gload<u32>((u32)control + 28));
          matrix_set = true;
        }
      }
      if (!matrix_set) {
        gstore<u32>(sp + 148, (u32)((flags & 16384) >> 14));
      }
      const s32 rot = (s32)gload<float>(sp + 152);
      gstore<float>(sp + 152, (float)(s32)(s16)rot);
    }

    // the color is at most 255 (not for the aux list)
    {
      float color[4];
      memcpy(color, gptr(sp + 160), 16);
      for (int i = 0; i < 3; i++) {
        color[i] = std::min(color[i], 255.f);
      }
      if (!(flags & 256)) {
        gstore_bytes(sp + 160, color, 16);
      }
    }

    // orbiting particles: the orbit's rotation (a quaternion from the angle at 128 and the angle
    // at 132), the radius from 136, the orbit's center from the launch state's sprite
    if (control != st && (flags & 128)) {
      r[0] = lw(sp + 128);
      call(gload<u32>(cos_sym));
      r[1] = v0;
      r[0] = lw(sp + 128);
      call(gload<u32>(sin_sym));
      r[0] = lw(sp + 136);
      r[3] = v0 ^ 0x80000000;
      gstore<u32>(info + 8, (u32)r[0]);
      r[2] = 0;
      r[4] = lw(sp + 132);
      r[0] = cpu + 80;
      call(gload<u32>(axis_angle_sym));
      gstore<u32>(info + 108, gload<u32>(gload<u32>((u32)state + 16)));
    }

    // binding: the first free launch state of the launch control for this launcher
    gstore<u32>(info + 136, (u32)st);
    if (state != st) {
      const u64 binding = lw(gload<u32>((u32)state) + 24);
      if (binding != 0) {
        u64 n = lw((u32)control);
        u32 s = (u32)control + 60;
        do {
          const u64 bound_launcher = lw(gload<u32>(s));
          n--;
          const u64 s_flags = lh(s + 4);
          if (bound_launcher == binding && !(s_flags & 1)) {
            gstore<u16>(s + 4, (u16)(s_flags | 1));
            gstore<u32>(s + 8, gload<u32>(info));
            gstore<u32>(s + 12, is_3d != st ? gload<u32>(info) : (u32)st);
            gstore<u32>(s + 16, info);
            gstore<u32>(info + 136, s);
            break;
          }
          s += 32;
        } while (n != 0);
      }
    }

    // launch adjustments (fields below 64 left), rotations
    r[0] = sp + 128;
    r[1] = cpu;
    r[2] = lw(sp + 80);
    {
      const u32 adjust = gload<u32>(adjust_sym);
      if ((s64)lh((u32)r[2]) - 64 < 0) {
        call(adjust);
      }
    }
    if (is_3d != st) {
      r[0] = sp + 128;
      r[1] = cpu;
      call(gload<u32>(euler_sym));
    }
    r[2] = lw((u32)state + 12);  // loaded even without a launch state
    if (state != st) {
      r[0] = sp + 128;
      if (r[2] != st) {
        r[1] = cpu;
        if (r[2] != 0) {
          call(gload<u32>(rotate_sym));
        }
      }
    }

    // the position is relative to pos
    {
      float p[4];
      memcpy(p, gptr(sp + 128), 16);
      for (int i = 0; i < 3; i++) {
        p[i] = p[i] + pos[i];
      }
      r[0] = lw(info + 4);
      r[1] = lw(sp + 96);
      gstore_bytes(sp + 128, p, 16);
    }
    {
      const u32 fn = gload<u32>(particle_adgif_sym);
      if (fn == *particle_adgif_stub) {
        v0 = particle_adgif_impl(NativeArgs{r, is_3d, args.st, sp});
      } else {
        call(fn);
      }
    }
    // the adgif's alpha blending and zbuf
    {
      const u32 adgif = gload<u32>(info + 4);
      u64 alpha = lw(adgif + 64);
      if (flags & 32) {
        alpha = 134;
      }
      if (flags & 16) {
        alpha = 66;
      }
      if (flags & 8) {
        alpha = 72;
      }
      gstore<u32>(adgif + 64, (u32)alpha);
      r[2] = adgif;
      r[3] = 0x1000000;
      if (flags & 512) {
        gstore<u64>(adgif + 48, 0x10001c0);
      }
    }

    // the birth function
    r[0] = system;
    r[1] = cpu;
    r[2] = sp + 128;
    r[4] = state;
    if (const u32 birth_func = gload<u32>(sp + 108)) {
      r[3] = launcher;
      call(birth_func);
    }

    // the sound, every time its accumulator reaches 1
    if (const u32 sound = gload<u32>(sp + 120)) {
      float accum = gload<float>((u32)launcher + 4) + gload<float>(sound + 4);
      gstore<float>((u32)launcher + 4, accum);
      accum = accum - 1.f;
      if ((s32)f2u(accum) >= 0) {
        gstore<float>((u32)launcher + 4, accum);
        r[0] = lw(sp + 120);
        r[1] = f2gpr(accum);
        call(gload<u32>(new_sound_id_sym));
        r[0] = lw(sp + 120);
        r[1] = v0;
        r[2] = sp + 64;
        call(gload<u32>(play_sym));
      }
    }

    // particles on the aux list: drawn by add-to-sprite-aux-list, transparent
    if (flags & 256) {
      gstore<u32>(info + 112, gload<u32>(aux_list_sym));
      flags = flags & ~(u64)4;
      gstore<u32>(sp + 172, 0);
      gstore<u32>(info + 60, 0);
      gstore<s32>(sp + 144, std::min(std::max(gload<s32>(sp + 144), 3), 11));
    }

    // which level's heap the launcher is in
    {
      const u32 level = gload<u32>(level_sym);
      if ((s64)(launcher - lw(level + 124)) >= 0 && (s64)(launcher - lw(level + 128)) < 0) {
        flags |= 1024;
      } else if ((s64)(launcher - lw(level + 2732)) >= 0 &&
                 (s64)(launcher - lw(level + 2736)) < 0) {
        flags |= 2048;
      }
    }

    // time of day colors
    if ((flags & 4352) == 4096) {
      const u32 tod = gload<u32>(time_of_day_sym);
      float k[4], color[4], fade[4];
      memcpy(k, gptr(tod + 108), 16);
      memcpy(color, gptr(sp + 160), 16);
      memcpy(fade, gptr(info + 48), 16);
      for (int i = 0; i < 3; i++) {
        color[i] = color[i] * k[i];
        fade[i] = fade[i] * k[i];
      }
      gstore_bytes(sp + 160, color, 16);
      gstore_bytes(info + 48, fade, 16);
    }

    // the key, the sprite (alpha 0 until the particle is processed), flags and cached alpha
    gstore<u32>(info + 132, control == st ? 0 : (u32)control);
    {
      u8 sprite[48];
      memcpy(sprite, gptr(sp + 128), 48);
      const float zero = 1.f - 1.f;
      memcpy(sprite + 44, &zero, 4);
      gstore_bytes(gload<u32>(info), sprite, 48);
    }
    flags |= 64;
    const u32 alpha = gload<u32>(sp + 172);
    gstore<u32>(info + 104, (u32)flags);
    gstore<u32>(info + 124, alpha);

    const float accum = gload<float>(accum_addr) - 1.f;
    gstore<float>(accum_addr, accum);
    if ((s32)accum == 0) {
      return v0;
    }
  }
}

}  // namespace

const NativeImpl particle_adgif =
    MIPS2C_NATIVE_IMPL(particle_adgif_impl, NATIVE_CALLS_GOAL | NATIVE_V0_UNDEFINED, 0);
// scratch: the original's stack frame
const NativeImpl sp_launch_particles_var =
    MIPS2C_NATIVE_IMPL(sp_launch_particles_var_impl, NATIVE_CALLS_GOAL | NATIVE_V0_UNDEFINED, 256);

}  // namespace Mips2C::jak1::native
