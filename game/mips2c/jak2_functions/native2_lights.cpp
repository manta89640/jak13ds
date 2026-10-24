/*!
 * @file native2_lights.cpp
 * (AI-assisted)
 * Jak 2: light-hash-get-bucket-index (lights.cpp's mips2c function) in C++, and
 * pc-light-hash-add-lights!, the light-hash loop of dma-add-process-drawable (drawable.gc's
 * light-hash-add-lights!, which calls it first) in C++ with the bucket lookup and
 * add-light-sphere-to-light-group called directly. See game/mips2c/mips2c_native.h for the rules.
 */

#include <cstdlib>
#include <cstring>

#include "common/log/log.h"

#include "game/kernel/jak2/kscheme.h"
#include "game/mips2c/jak2_functions/native2.h"
#include "game/mips2c/native_goalc_ops.h"

#ifdef __3DS__
extern "C" int ctr_config_get(const char* key, char* out, int size);  // platform/3ds/port
#endif

namespace Mips2C::jak2::native2 {

u64 add_light_sphere_to_light_group_impl(const NativeArgs& args);

/*!
 * (light-hash-get-bucket-index hash pos): the bucket of pos in the light hash's grid: (pos - base)
 * * scale truncated to ints (vftoi0), -1 when x, y or z is outside 0 .. dimension - 1, else
 * x + y * dim-x + z * dim-xz (32-bit products, as mult3).
 */
u64 light_hash_get_bucket_index_impl(const NativeArgs& args) {
  const u32 h = (u32)args.a[0];
  float p[4], base[4], scale[4];
  gload_q(p, (u32)args.a[1]);
  gload_q(base, h + 12);
  gload_q(scale, h + 28);
  s32 dims[4];
  memcpy(dims, gptr((h + 44) & ~15u), 16);  // lq: the address aligned down
  s32 i[4];
  for (int l = 0; l < 4; l++) {
    float t = p[l] - base[l];
    t = t * scale[l];
    i[l] = t;  // vftoi0: the C++ conversion, as the mips2c code does it
  }
  for (int l = 0; l < 3; l++) {
    const s32 last = (s32)((u32)dims[l] + 0xffffffffu);
    if (0 > i[l] || i[l] > last) {
      return (u64)(s64)-1;
    }
  }
  const s64 y = (s64)i[1] * (s64)gload<u8>(h + 6);
  const s64 z = (s64)i[2] * (s64)gload<u8>(h + 7);
  return (u64)(s64)i[0] + (u64)(s64)(s32)(y & 0xffffffff) + (u64)(s64)(s32)(z & 0xffffffff);
}

const NativeImpl light_hash_get_bucket_index =
    MIPS2C_NATIVE_IMPL_GOAL(light_hash_get_bucket_index_impl, 0);

namespace {

u32 sym_val(const char* name) {
  return ::jak2::intern_from_c(name).offset - 1;
}

//! on unless turned off for debugging (config.ini mips2c_native_off naming it or all; host
//! OPENGOAL_MIPS2C_NATIVE=0)
bool add_lights_native_on() {
  static int on = -1;
  if (on < 0) {
    on = 1;
#ifdef __3DS__
    char list[256];
    if (ctr_config_get("mips2c_native_off", list, sizeof(list)) &&
        (strstr(list, "pc-light-hash-add-lights!") || !strcmp(list, "all") ||
         !strcmp(list, "on") || !strcmp(list, "1") || !strcmp(list, "yes") ||
         !strcmp(list, "true"))) {
      on = 0;
    }
#else
    const char* all = getenv("OPENGOAL_MIPS2C_NATIVE");
    if (all && all[0] == '0') {
      on = 0;
    }
#endif
    lg::info("[lights] native light-hash-add-lights! {}", on ? "on" : "off");
  }
  return on == 1;
}

//! a native through its symbol: its entry when the symbol holds its stub, else the GOAL call
u64 call_native_sym(u32 sym_val_addr,
                    const u32* stub,
                    u64 (*impl)(const NativeArgs&),
                    const u64 a[8],
                    const NativeArgs& caller) {
  const u32 fn = gload<u32>(sym_val_addr);
  if (fn == *stub) {
    return impl(NativeArgs{a, caller.pp, caller.st, caller.stack});
  }
  return native_call_goal(fn, a, caller);
}

//! the loop of light-hash-add-lights! (drawable.gc), as goalc's C of it
void add_lights(u32 lg, u32 dc, const NativeArgs& caller) {
  GC_NATIVE_FP_EXACT
  static const u32 level_sym = sym_val("*level*");
  static const u32 bucket_sym = sym_val("light-hash-get-bucket-index");
  static const u32 add_sym = sym_val("add-light-sphere-to-light-group");
  static const u32* bucket_stub = native_stub_slot("light-hash-get-bucket-index");
  static const u32* add_stub = native_stub_slot("add-light-sphere-to-light-group");
  static const u32 active = ::jak2::intern_from_c("active").offset;
  const u32 origin = dc + 0x7c;
  for (s64 i = 0; i < (s64)gload<s32>(gload<u32>(level_sym)); i++) {
    const u32 lev = (u32)((u64)(s64)(s32)((u32)0x1470 * (u32)i) + 0x100 + gload<u32>(level_sym));
    if (gload<u32>(lev + 16) != active) {
      continue;
    }
    const u32 hash = gload<u32>(lev + 5132);
    const u32 mood = lev + 0x19c;
    if (hash == 0) {
      continue;
    }
    const u64 la[8] = {hash, origin, 0, 0, 0, 0, 0, 0};
    const u64 idx =
        call_native_sym(bucket_sym, bucket_stub, light_hash_get_bucket_index_impl, la, caller);
    if ((s64)idx == -1) {
      continue;
    }
    const u32 bucket = (u32)((idx << 2) + gload<u32>(hash + 60));
    const u32 ip = (u32)((u64)gload<u16>(bucket) + gload<u32>(hash + 64));
    for (s64 k = 0; k < (s64)gload<u16>(bucket + 2); k++) {
      const u32 sph = (u32)(((u64)gload<u8>((u32)(ip + k)) << 6) + gload<u32>(hash + 68));
      const s64 pal = gload<s8>(sph + 63);
      const float interp =
          pal == -1 ? 1.f : gload<float>((u32)(((u64)pal << 4) + 0x670 + mood) + 12);
      const float b = gload<float>(sph + 12) * interp;
      if (goalc_ops::fcmp_xk(b, 0, goalc_ops::GC_FEQ)) {
        continue;
      }
      const u64 aa[8] = {lg, sph, origin, mood, 0, 0, 0, 0};
      call_native_sym(add_sym, add_stub, add_light_sphere_to_light_group_impl, aa, caller);
    }
  }
}

}  // namespace

/*!
 * (pc-light-hash-add-lights! lg dc): #t after adding the lights, or #f when the GOAL code must run
 * (debugging; verify mode's GOAL run).
 */
u64 pc_light_hash_add_lights_impl(const NativeArgs& args) {
  const u64 f = args.st;
  const u64 t = args.st + 4;
  if (!add_lights_native_on()) {
    return f;
  }
  const u32 lg = (u32)args.a[0];
  const u32 dc = (u32)args.a[1];
#if MIPS2C_WRITE_LOG
  static bool nested = false;
  if (nested) {
    return f;
  }
  if (native_verify_inline_on()) {
    static const u32 goal_sym = sym_val("light-hash-add-lights!");
    native_verify_inline(
        "light-hash-add-lights!", {{lg, 192}},
        [&]() -> u64 {
          add_lights(lg, dc, args);
          return 0;
        },
        [&]() -> u64 {
          nested = true;
          const u64 a[8] = {lg, dc, 0, 0, 0, 0, 0, 0};
          native_call_goal(gload<u32>(goal_sym), a, args);
          nested = false;
          return 0;
        });
    return t;
  }
#endif
  add_lights(lg, dc, args);
  return t;
}

const NativeImpl pc_light_hash_add_lights = MIPS2C_NATIVE_IMPL_GOAL(pc_light_hash_add_lights_impl, 0);

}  // namespace Mips2C::jak2::native2

namespace Mips2C::jak2::pc_light_hash_add_lights {
void link() {
  gLinkedFunctionTable.reg("pc-light-hash-add-lights!", native2::pc_light_hash_add_lights.as_exec,
                           64, &native2::pc_light_hash_add_lights);
}
}  // namespace Mips2C::jak2::pc_light_hash_add_lights
