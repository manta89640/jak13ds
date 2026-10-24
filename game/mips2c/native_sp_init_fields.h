#pragma once

/*!
 * @file native_sp_init_fields.h
 * (AI-assisted)
 * Native version of the GOAL function sp-init-fields! (sparticle-launcher.gc, Jak 1 and Jak 2: the
 * PC port's rewrite of the original assembly), with rand-vu (math.gc) inlined. Shared by
 * jak1_functions/native_sp_init.cpp and jak2_functions/native_sp_init.cpp, which register it as
 * pc-sp-init-fields!; sp-init-fields! calls it first (#when TARGET_3DS) and runs its own code when
 * it returns #f.
 *
 * Same results as goalc's C code of the GOAL functions: the same float operations in the same
 * order (each on its own: no multiply-adds), the int conversions of goalc (gc_cvtt: truncate,
 * saturate, NaN -> 0; (the float i) of the low 32 bits), the same field id compares (unsigned, the
 * field ids zero-extended), and rand-vu's register update with pc-rand (the same mt19937 draws in
 * the same order).
 *
 * The two games differ in: where a symbol's value is (Jak 1: at the symbol, Jak 2: at the symbol
 * - 1), and Jak 2's float-store (flag 7: a float that is also stored in *sp-temp*), which Jak 1's
 * case doesn't have (flag 7 takes its int branch).
 */

#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "common/log/log.h"

#include "game/mips2c/mips2c_native.h"
#include "game/mips2c/mips2c_private.h"
#include "game/mips2c/native_goalc_ops.h"

u32 pc_rand();                                // pc-rand (game/kernel/common/kmachine.cpp)
extern std::mt19937 extra_random_generator;  // its generator

#ifdef __3DS__
extern "C" int ctr_config_get(const char* key, char* out, int size);  // platform/3ds/port
#endif

namespace Mips2C::sp_init {

using ::Mips2C::goalc_ops::f2i;
using ::Mips2C::goalc_ops::i2f;

template <typename T>
inline T ld(const u8* m, u32 addr) {
  T v;
  memcpy(&v, m + addr, sizeof(T));
  return v;
}
template <typename T>
inline void st(u8* m, u32 addr, T v) {
  MIPS2C_NATIVE_LOG_STORE(addr, (u32)sizeof(T));
  memcpy(m + addr, &v, sizeof(T));
}

//! rand-vu: the next value of the VU0 random register (*_vu-reg-R_*, at r_addr), minus 1.0
inline float rand_vu(u8* m, u32 r_addr) {
  GC_NATIVE_FP_EXACT
  const u64 cur = (u64)(s64)ld<s32>(m, r_addr);
  const u64 x = 1 & (cur >> 4);
  const u64 y = 1 & (cur >> 22);
  u64 v = cur << 1;
  v = v ^ (x ^ y);
  v = v ^ (u64)pc_rand();
  const u32 r = 0x3f800000u | (u32)(v & 0x7fffff);
  st<u32>(m, r_addr, r);
  float f;
  memcpy(&f, &r, 4);
  return f - 1.f;
}

//! the GOAL memory addresses the function uses (symbol values)
struct Syms {
  u32 vu_r;           //! *_vu-reg-R_*'s value
  u32 part_id_table;  //! *part-id-table*'s value
  u32 sp_temp;        //! *sp-temp*'s value (Jak 2)
};

/*!
 * (sp-init-fields! dst specs first end write-missing): the fields first + 1 .. end - 1 of a
 * particle (floats or words at dst, one per field) from the init specs (16 bytes: field u16,
 * flags u16, initial value, random range, random mult), skipping the specs of lower fields;
 * a missing field is 0.0 when write-missing. Returns the spec after the last one used.
 * kSymValOff: the offset of a symbol's value from the symbol; kFloatStore: Jak 2's flag 7.
 */
template <int kSymValOff, bool kFloatStore>
u32 init_fields(u32 dst, u32 spec, u64 first, u64 end, bool write_missing, const Syms& syms) {
  GC_NATIVE_FP_EXACT
  u8* const m = g_ee_main_mem;
  u64 field = first + 1;
  while ((u64)ld<u16>(m, spec) < field) {
    spec += 16;
  }
  if (!(field < end)) {
    return spec;
  }
  do {
    if ((u64)ld<u16>(m, spec) != field) {
      if (write_missing) {
        st<float>(m, dst, 0.f);
      }
      spec -= 16;
    } else if (field == 7) {
      // spt-sound
      st<u32>(m, dst, ld<u32>(m, spec + 4));
    } else {
      const u16 flags = ld<u16>(m, spec + 2);
      if (flags == 1 || (kFloatStore && flags == 7)) {
        // float (float-store: the same, then the value into *sp-temp*)
        if (ld<s32>(m, spec + 8) == 0) {
          st<u32>(m, dst, ld<u32>(m, spec + 4));
        } else {
          const float init = ld<float>(m, spec + 4);
          const float mult = ld<float>(m, spec + 12);
          float r = rand_vu(m, syms.vu_r);
          r = r * ld<float>(m, spec + 8);
          const float p = mult * r;
          st<float>(m, dst, init + p);
        }
        if (kFloatStore && flags == 7) {
          st<u32>(m, syms.sp_temp, ld<u32>(m, dst));
        }
      } else if (flags == 2) {
        // float-int-rand
        if (ld<s32>(m, spec + 8) == 0) {
          st<u32>(m, dst, ld<u32>(m, spec + 4));
        } else {
          const float init = ld<float>(m, spec + 4);
          const float mult = ld<float>(m, spec + 12);
          float r = rand_vu(m, syms.vu_r);
          r = r * i2f((s64)ld<s32>(m, spec + 8) + 1);
          const float p = mult * i2f(f2i(r));
          st<float>(m, dst, init + p);
        }
      } else if (flags == 3) {
        // copy-from-other
        st<u32>(m, dst, ld<u32>(m, (u32)(((u64)(s64)ld<s32>(m, spec + 4) << 2) + dst)));
      } else if (flags == 5) {
        // symbol
        st<u32>(m, dst, ld<u32>(m, ld<u32>(m, spec + 4) + kSymValOff));
      } else if (flags == 6) {
        // launcher: (-> *part-id-table* initial-value)
        st<u32>(m, dst,
                ld<u32>(m, (u32)(((u64)(s64)ld<s32>(m, spec + 4) << 2) + 12 + syms.part_id_table)));
      } else if (flags == 4) {
        // object
        st<u32>(m, dst, ld<u32>(m, spec + 4));
      } else {
        // int
        if (ld<s32>(m, spec + 8) == 0) {
          st<u32>(m, dst, ld<u32>(m, spec + 4));
        } else {
          const s64 init = ld<s32>(m, spec + 4);
          const u64 mult = (u64)(s64)ld<s32>(m, spec + 12);
          float r = rand_vu(m, syms.vu_r);
          r = r * i2f(ld<s32>(m, spec + 8));
          const u64 p = (u64)(s64)(s32)((u32)mult * (u32)f2i(r));
          st<u32>(m, dst, (u32)((u64)init + p));
        }
      }
    }
    field++;
    dst += 4;
    spec += 16;
  } while (field != end);
  return spec;
}

//! on unless turned off for debugging: config.ini mips2c_native_off lists pc-sp-init-fields! or
//! is all (3DS); OPENGOAL_SP_INIT_NATIVE=0 or OPENGOAL_MIPS2C_NATIVE=0 (host)
inline bool native_on() {
  static int on = -1;
  if (on < 0) {
    on = 1;
#ifdef __3DS__
    char list[256];
    if (ctr_config_get("mips2c_native_off", list, sizeof(list)) &&
        (strstr(list, "pc-sp-init-fields!") || !strcmp(list, "all") || !strcmp(list, "on") ||
         !strcmp(list, "1") || !strcmp(list, "yes") || !strcmp(list, "true"))) {
      on = 0;
    }
#else
    const char* env = getenv("OPENGOAL_SP_INIT_NATIVE");
    if (env && env[0] == '0') {
      on = 0;
    }
    const char* all = getenv("OPENGOAL_MIPS2C_NATIVE");
    if (all && all[0] == '0') {
      on = 0;
    }
#endif
    lg::info("[particles] native sp-init-fields! {}",
             on ? "on" : "off (mips2c_native_off / OPENGOAL_*_NATIVE=0)");
  }
  return on == 1;
}

#if MIPS2C_WRITE_LOG
/*!
 * Verify mode (host, OPENGOAL_MIPS2C_VERIFY=1): the native on the memory it writes (the fields, the
 * random register, *sp-temp*) and on pc-rand's generator, then both put back, then sp-init-fields!
 * itself (the nested call returns #f, so its GOAL code runs), then the memory, the generator's
 * state and the returned spec compared. "sp-init-fields! verify" lines, a summary at exit.
 */
struct Verify {
  int on = -1;
  bool nested = false;
  u64 calls = 0, mismatches = 0;

  bool enabled() {
    if (on < 0) {
      const char* v = getenv("OPENGOAL_MIPS2C_VERIFY");
      on = (v && v[0] && v[0] != '0') ? 1 : 0;
    }
    return on == 1;
  }
};

template <int kSymValOff, bool kFloatStore>
u32 verify(Verify& v,
            const NativeArgs& args,
            const Syms& syms,
            u32 goal_fn_val_addr,
            u32 dst,
            u32 spec,
            u64 first,
            u64 end,
            bool write_missing) {
  u8* const m = g_ee_main_mem;
  const u32 n = end > first + 1 ? (u32)(end - first - 1) : 0;
  struct Region {
    u32 addr, size;
  };
  const Region regions[3] = {{dst, 4 * n}, {syms.vu_r, 4}, {syms.sp_temp, kFloatStore ? 4u : 0u}};
  std::vector<u8> before[3], native[3];
  for (int r = 0; r < 3; r++) {
    before[r].assign(m + regions[r].addr, m + regions[r].addr + regions[r].size);
  }
  const std::mt19937 rng_before = extra_random_generator;
  const u32 ret = init_fields<kSymValOff, kFloatStore>(dst, spec, first, end, write_missing, syms);
  const std::mt19937 rng_native = extra_random_generator;
  for (int r = 0; r < 3; r++) {
    native[r].assign(m + regions[r].addr, m + regions[r].addr + regions[r].size);
    memcpy(m + regions[r].addr, before[r].data(), regions[r].size);
  }
  extra_random_generator = rng_before;
  v.nested = true;
  const u32 goal_ret = (u32)native_call_goal(gload<u32>(goal_fn_val_addr), args.a, args);
  v.nested = false;
  v.calls++;
  const char* what = nullptr;
  if (goal_ret != ret) {
    what = "returned spec";
  } else if (!(extra_random_generator == rng_native)) {
    what = "pc-rand draws";
  } else {
    static const char* names[3] = {"fields", "random register", "*sp-temp*"};
    for (int r = 0; r < 3 && !what; r++) {
      if (memcmp(m + regions[r].addr, native[r].data(), regions[r].size)) {
        what = names[r];
      }
    }
  }
  if (what) {
    v.mismatches++;
    if (v.mismatches <= 10) {
      lg::error("sp-init-fields! verify MISMATCH (call {}, fields {}..{} at #x{:x}): {}", v.calls,
                first + 1, end, dst, what);
    }
  }
  static bool summary = false;
  if (!summary) {
    summary = true;
    static Verify* vp = &v;
    atexit([] {
      lg::info("sp-init-fields! verify: {} calls, {} mismatches", vp->calls, vp->mismatches);
    });
  }
  return goal_ret;
}
#endif

/*!
 * (pc-sp-init-fields! dst specs first end write-missing): sp-init-fields!'s result (the spec
 * after the last one used), or #f when the GOAL code must run (debugging; verify mode's nested
 * call).
 */
template <int kSymValOff, bool kFloatStore>
u64 entry(const NativeArgs& args, const Syms& syms, u32 goal_fn_val_addr) {
  if (!native_on()) {
    return args.st;  // #f
  }
  const u32 dst = (u32)args.a[0];
  const u32 spec = (u32)args.a[1];
  const u64 first = args.a[2];
  const u64 end = args.a[3];
  const bool write_missing = (u32)args.a[4] != (u32)args.st;
#if MIPS2C_WRITE_LOG
  static Verify v;
  if (v.enabled()) {
    if (v.nested) {
      return args.st;  // #f: the GOAL code runs (the reference)
    }
    // the GOAL code's results stay in memory; its return value (compared with the native's)
    return verify<kSymValOff, kFloatStore>(v, args, syms, goal_fn_val_addr, dst, spec, first, end,
                                           write_missing);
  }
#else
  (void)goal_fn_val_addr;
#endif
  return init_fields<kSymValOff, kFloatStore>(dst, spec, first, end, write_missing, syms);
}

/*!
 * (AI-assisted) sp-get-particle (sparticle.gc, the PC port's version: blocks searched in order) as
 * goalc's C of it: the first free particle of group (0: the first blocks0 blocks, 1: the next
 * blocks1) in the system's alloc table (a set bit is a free particle), marked used, counted
 * (num-alloc), its valid field set (Jak 1: #t as a word, Jak 2: 1 as a byte). #f when full.
 */
template <bool kJak2>
u64 get_particle(u32 sys, u64 group, u64 s7) {
  u8* const m = g_ee_main_mem;
  u64 first = 0;
  s64 n = ld<s32>(m, sys);
  s64 a3 = 0;
  if ((s64)group == 1) {
    first = (u64)n;
    n = ld<s32>(m, sys + 4);
  }
  for (s64 blk = 0; blk < n; blk++) {
    const u64 idx = first + (u64)a3;
    const u32 word_addr = (u32)((idx << 3) + ld<u32>(m, sys + 32));
    if (ld<u64>(m, word_addr) != 0) {
      for (s64 bit = 0; bit < 64; bit++) {
        const u64 mask = (u64)1 << bit;
        const u64 w = ld<u64>(m, (u32)((idx << 3) + ld<u32>(m, sys + 32)));
        if ((s64)(mask & w) != 0) {
          const u32 a = (u32)((idx << 3) + ld<u32>(m, sys + 32));
          st<u64>(m, a, ld<u64>(m, a) ^ mask);
          const u32 cnt = (u32)((group << 2) + 16 + sys);
          st<u32>(m, cnt, (u32)((u64)(s64)ld<s32>(m, cnt) + 1));
          const u64 i = (u64)bit + (idx << 6);
          const u64 cpu =
              (u64)(s64)(s32)((u32)0x90 * (u32)i) + (u64)ld<u32>(m, sys + 36);
          if (kJak2) {
            st<u8>(m, (u32)cpu + 128, 1);
          } else {
            st<u32>(m, (u32)cpu + 128, (u32)(s7 + 8));
          }
          return cpu;
        }
      }
    }
    a3++;
    if (a3 == n) {
      a3 = 0;
    }
  }
  return s7;
}

}  // namespace Mips2C::sp_init
