/*!
 * @file native2_traffic.cpp
 * (AI-assisted)
 * Jak 2 Haven City traffic (traffic-engine.gc) in C++: the GOAL methods call these first and run
 * their own code only when the native returns #f / 0.
 *
 *  - pc-per-frame-cell-update: (method per-frame-cell-update traffic-level-data), the per-frame pass
 *    over all vis-cells of a city level (distance and view tests, the active cell lists and box),
 *    with vector-vector-distance-squared, sphere-in-view-frustum?, math-camera-pos and the
 *    add-active-cell / remove-active-cell / add-newly-active-cell methods inline.
 *  - pc-sphere-in-loaded-city-infos?: (method sphere-in-loaded-city-infos? traffic-engine), with
 *    sphere-in-grid?, lookup-box-for-sphere and lookup-cell-for-point inline.
 *
 * Each is goalc's C of the GOAL code (out/jak2/cmod, the traffic-engine module): the same loads,
 * 64-bit integer ops, float operations in the same order (no fused multiply-adds) and compares with
 * goalc's NaN results. The functions and methods they run inline must hold what they held at the
 * first call (symbol values, method table entries), otherwise the native returns "run the GOAL
 * code". Verify mode (OPENGOAL_MIPS2C_VERIFY=1) runs the GOAL code on every call and compares
 * (native_verify_inline: the traffic level data, the cells, the result).
 */

#include <cstring>

#include "game/kernel/jak2/kscheme.h"
#include "game/mips2c/jak2_functions/native2.h"
#include "game/mips2c/native_goalc_ops.h"

namespace Mips2C::jak2::native2 {

namespace {

using goalc_ops::f2i;

//! a symbol's value address and the function it held at the first call
struct FnSym {
  const char* name;
  u32 val = 0;
  u32 fn = 0;
  bool resolved = false;
  explicit FnSym(const char* n) : name(n) {}
  bool unchanged() {
    if (!resolved) {
      resolved = true;
      val = sym_value_addr(name);
      fn = gload<u32>(val);
    }
    return fn != 0 && gload<u32>(val) == fn;
  }
};

//! a method table entry of a type and the function it held at the first call
struct MethodSlot {
  const char* type;
  u32 idx;
  u32 slot = 0;
  u32 fn = 0;
  bool resolved = false;
  MethodSlot(const char* t, u32 i) : type(t), idx(i) {}
  bool unchanged() {
    if (!resolved) {
      resolved = true;
      const u32 ty = gload<u32>(sym_value_addr(type));
      if (ty) {
        slot = ty + 16 + 4 * idx;
        fn = gload<u32>(slot);
      }
    }
    return fn != 0 && gload<u32>(slot) == fn;
  }
};

//! (< a b) of two float variables in goalc's C: !(a >= b) (true when either is NaN)
inline bool flt(float a, float b) {
  return !(a >= b);
}

//! *math-camera*'s value address (read on each use)
u32 math_camera_val() {
  static const u32 v = sym_value_addr("*math-camera*");
  return v;
}
FnSym s_math_camera_pos("math-camera-pos");
FnSym s_vvds("vector-vector-distance-squared");
FnSym s_in_frustum("sphere-in-view-frustum?");

MethodSlot m_add_active("traffic-level-data", 10);
MethodSlot m_remove_active("traffic-level-data", 11);
MethodSlot m_add_newly("traffic-level-data", 12);
MethodSlot m_sphere_in_grid("city-level-info", 15);
MethodSlot m_lookup_cell("grid-info", 10);
MethodSlot m_lookup_box("grid-info", 11);

bool native_enabled(const char* name, int& state) {
  if (state < 0) {
    state = native_turned_off(name) ? 0 : 1;
  }
  return state == 1;
}

//! sphere-in-view-frustum? (drawable.gc) as goalc's C of it: acc = p0 * s.x + p1 * s.y + p2 * s.z,
//! v = (acc - p3 * vf0.w) + s.w on all four lanes, in view when no lane has its sign bit set
//! (pcgtw 0 > v, ppach, zero?)
bool sphere_in_view_frustum(u32 sphere) {
  GC_NATIVE_FP_EXACT
  const u32 cam = gload<u32>(math_camera_val());
  float s[4], p0[4], p1[4], p2[4], p3[4];
  gload_q(s, sphere);
  gload_q(p0, cam + 940);
  gload_q(p1, cam + 956);
  gload_q(p2, cam + 972);
  gload_q(p3, cam + 988);
  const float one = goalc_ops::one();
  u32 neg = 0;
  for (int l = 0; l < 4; l++) {
    float a = p0[l] * s[0];
    const float b = p1[l] * s[1];
    a = a + b;
    const float c = p2[l] * s[2];
    a = a + c;
    const float d = p3[l] * one;
    a = a - d;
    a = a + s[3];
    neg |= f2u(a);
  }
  return (neg >> 31) == 0;
}

//! vector-vector-distance-squared (vector.gc): ((b - a)^2.x + (b - a)^2.y) + (b - a)^2.z
float vector_vector_distance_squared(u32 a, u32 b) {
  GC_NATIVE_FP_EXACT
  float va[4], vb[4];
  gload_q(va, a);
  gload_q(vb, b);
  const float dx = vb[0] - va[0];
  const float dy = vb[1] - va[1];
  const float dz = vb[2] - va[2];
  const float x = dx * dx;
  const float y = dy * dy;
  const float z = dz * dz;
  float r = x + y;
  r = r + z;
  return r;
}

constexpr u32 kActiveCount = 4;
constexpr u32 kNewlyCount = 5;
constexpr u32 kActiveList = 8;
constexpr u32 kNewlyList = 0x404;
constexpr u32 kActiveBox = 0x800;
constexpr u32 kLevelDataSize = 0x820;

//! the methods of traffic-level-data that per-frame-cell-update calls
void add_to_list(u32 tld, u32 count_off, u32 list_off, u32 cell) {
  const u64 n = gload<u8>(tld + count_off);
  if (n < 255) {
    gstore<u32>((u32)((n << 2) + list_off + tld), cell);
    gstore<u8>(tld + count_off, (u8)(gload<u8>(tld + count_off) + 1));
  }
}

void remove_active(u32 tld, u64 idx) {
  const s64 last = (s64)gload<u8>(tld + kActiveCount) + -1;
  if (last >= 0) {
    gstore<u32>((u32)((idx << 2) + kActiveList + tld),
                gload<u32>((u32)(((u64)last << 2) + kActiveList + tld)));
    gstore<u8>(tld + kActiveCount, (u8)(gload<u8>(tld + kActiveCount) + -1));
  }
}

void per_frame_cell_update(u32 tld) {
  GC_NATIVE_FP_EXACT
  gstore<u8>(tld + kNewlyCount, 0);
  for (s64 i = 0; i < (s64)gload<u8>(tld + kActiveCount); i++) {
    const u32 cell = gload<u32>((u32)(((u64)i << 2) + kActiveList + tld));
    gstore<u8>(cell + 27, gload<u8>(cell + 26));
  }
  const u32 cam = 0x39c + gload<u32>(math_camera_val());
  const float near_r = goalc_ops::bitsf(0x47a00000u);  // 81920.0
  const float far_r = goalc_ops::bitsf(0x49480000u);   // 819200.0
  const float ped_r = goalc_ops::bitsf(0x48f00000u);   // 491520.0
  for (s64 i = 0; i < (s64)gload<u16>(gload<u32>(tld) + 70); i++) {
    const u32 cell = (u32)(((u64)i << 5) + gload<u32>(gload<u32>(tld) + 64));
    const float d = vector_vector_distance_squared(cell, cam);
    float a = gload<float>(cell + 12) + far_r;
    float b = gload<float>(cell + 12) + far_r;
    bool off = flt(a * b, d);
    if (!off) {
      a = gload<float>(cell + 12) + near_r;
      b = gload<float>(cell + 12) + near_r;
      off = flt(a * b, d) && !sphere_in_view_frustum(cell);
    }
    if (off) {
      gstore<u8>(cell + 26, (u8)(gload<u8>(cell + 26) & ~3u));
    } else {
      a = gload<float>(cell + 12) + ped_r;
      b = gload<float>(cell + 12) + ped_r;
      if (flt(a * b, d)) {
        gstore<u8>(cell + 26, (u8)((gload<u8>(cell + 26) | 1u) & ~2u));
      } else {
        gstore<u8>(cell + 26, (u8)(gload<u8>(cell + 26) | 3u));
      }
    }
    if ((gload<u8>(cell + 27) & 3u) < (gload<u8>(cell + 26) & 3u)) {
      add_to_list(tld, kNewlyCount, kNewlyList, cell);
    }
  }
  for (u64 i = gload<u8>(tld + kActiveCount); i != 0;) {
    i = i - 1;
    const u32 cell = gload<u32>((u32)((i << 2) + kActiveList + tld));
    if ((gload<u8>(cell + 26) & 3u) == 0) {
      gstore<u8>(cell + 27, (u8)(gload<u8>(cell + 27) & ~3u));
      remove_active(tld, i);
    }
  }
  for (s64 i = 0; i < (s64)gload<u8>(tld + kNewlyCount); i++) {
    add_to_list(tld, kActiveCount, kActiveList,
                gload<u32>((u32)(((u64)i << 2) + kNewlyList + tld)));
  }
  const u32 box = tld + kActiveBox;
  gstore<u32>(box + 0, 0x7cf0bdc2u);
  gstore<u32>(box + 4, 0x7cf0bdc2u);
  gstore<u32>(box + 8, 0x7cf0bdc2u);
  gstore<u32>(box + 12, 0x3f800000u);
  gstore<u32>(box + 16, 0xfcf0bdc2u);
  gstore<u32>(box + 20, 0xfcf0bdc2u);
  gstore<u32>(box + 24, 0xfcf0bdc2u);
  gstore<u32>(box + 28, 0x3f800000u);
  for (u64 i = gload<u8>(tld + kActiveCount); i != 0;) {
    i = i - 1;
    const u32 cell = gload<u32>((u32)((i << 2) + kActiveList + tld));
    for (u32 k = 0; k < 3; k++) {
      float lo = gload<float>(cell + 4 * k);
      lo = lo - gload<float>(cell + 12);
      const float m = gload<float>(box + 4 * k);
      gstore<float>(box + 4 * k, m < lo ? m : lo);
    }
    for (u32 k = 0; k < 3; k++) {
      float hi = gload<float>(cell + 4 * k);
      hi = hi + gload<float>(cell + 12);
      const float m = gload<float>(box + 16 + 4 * k);
      gstore<float>(box + 16 + 4 * k, m > hi ? m : hi);
    }
  }
}

bool cell_update_inline_ok() {
  return s_math_camera_pos.unchanged() && s_vvds.unchanged() &&
         s_in_frustum.unchanged() && m_add_active.unchanged() && m_remove_active.unchanged() &&
         m_add_newly.unchanged();
}

//! lookup-cell-for-point (grid-info method 10): pos[k] = max(0, min((the int ((p[k] - box.min[k])
//! * axis-scale[k])), dim[k] - 1)), stored as bytes
void lookup_cell_for_point(u32 grid, s64 out[3], const float p[3]) {
  GC_NATIVE_FP_EXACT
  for (u32 k = 0; k < 3; k++) {
    float t = p[k] - gload<float>(grid + 16 + 4 * k);
    t = t * gload<float>(grid + 4 * k);
    const s64 i = f2i(t);
    const s64 lim = (s64)gload<s8>(grid + 12 + k) + -1;
    const s64 m = i <= lim ? i : lim;
    out[k] = (s64)(s8)(u8)(0 <= m ? m : 0);
  }
}

//! sphere-in-grid? (city-level-info method 15) with lookup-box-for-sphere: #t when a cell of the
//! sphere's box has flag bit `bit`
bool sphere_in_grid(u32 info, u32 sphere, s64 bit, bool& ok) {
  GC_NATIVE_FP_EXACT
  float v[4];
  gload_q(v, sphere);
  float lo[3], hi[3];
  const float neg_r = 0.0f - v[3];
  const float r = v[3];
  for (int k = 0; k < 3; k++) {
    lo[k] = v[k] + neg_r;
    hi[k] = v[k] + r;
  }
  s64 mn[3], mx[3];
  lookup_cell_for_point(info, mn, lo);
  lookup_cell_for_point(info, mx, hi);
  // the GOAL loops count down from 1 - min + max to 0: a box from lookup-cell-for-point always
  // has min <= max (both clamped to 0 .. dim - 1 and lo <= hi), otherwise let the GOAL code run
  for (int k = 0; k < 3; k++) {
    if (mx[k] < mn[k]) {
      ok = false;
      return false;
    }
  }
  const s64 dimx = gload<s8>(info + 12);
  const s64 dimz = gload<s8>(info + 14);
  const u32 cells = gload<u32>(info + 64);
  const u64 mask = bit > 0 ? (u64)1 << (bit & 63) : (u64)((s64)1 >> ((0 - bit) & 63));
  for (s64 y = mn[1]; y <= mx[1]; y++) {
    for (s64 z = mn[2]; z <= mx[2]; z++) {
      for (s64 x = mn[0]; x <= mx[0]; x++) {
        const s64 zi = (s64)(s32)((u32)z * (u32)dimx);
        const s64 yi = (s64)(s32)((u32)(s64)(s32)((u32)y * (u32)dimx) * (u32)dimz);
        const u64 idx = (u64)(x + zi + yi);
        if ((gload<u8>((u32)((idx << 5) + cells) + 26) & mask) != 0) {
          return true;
        }
      }
    }
  }
  return false;
}

bool city_infos_inline_ok() {
  return m_sphere_in_grid.unchanged() && m_lookup_box.unchanged() && m_lookup_cell.unchanged();
}

//! sphere-in-loaded-city-infos?: 1 = #t, 0 = #f, -1 = run the GOAL code
int sphere_in_loaded_city_infos(u32 engine, u32 sphere, s64 bit) {
  for (s64 i = 0; i < 2; i++) {
    const u32 tld = (u32)((u64)(s64)(s32)((u32)kLevelDataSize * (u32)i) + 0x40c + engine);
    const u32 info = gload<u32>(tld);
    if (info == 0) {
      continue;
    }
    bool ok = true;
    const bool in = sphere_in_grid(info, sphere, bit, ok);
    if (!ok) {
      return -1;
    }
    if (in) {
      return 1;
    }
  }
  return 0;
}

}  // namespace

/*!
 * (pc-per-frame-cell-update this): #t after doing the update, #f when the GOAL code must run.
 */
u64 pc_per_frame_cell_update_impl(const NativeArgs& args) {
  static int on = -1;
  const u64 f = args.st;
  const u64 t = args.st + 4;
  if (!native_enabled("pc-per-frame-cell-update", on) || !cell_update_inline_ok()) {
    return f;
  }
  const u32 tld = (u32)args.a[0];
#if MIPS2C_WRITE_LOG
  static bool nested = false;
  if (nested) {
    return f;
  }
  if (native_verify_inline_on()) {
    static MethodSlot goal_fn("traffic-level-data", 13);
    goal_fn.unchanged();
    const u32 info = gload<u32>(tld);
    native_verify_inline(
        "per-frame-cell-update", {{tld, kLevelDataSize}, {gload<u32>(info + 64), 32u * gload<u16>(info + 70)}},
        [&]() -> u64 {
          per_frame_cell_update(tld);
          return 0;
        },
        [&]() -> u64 {
          nested = true;
          const u64 a[8] = {tld, 0, 0, 0, 0, 0, 0, 0};
          native_call_goal(goal_fn.fn, a, args);
          nested = false;
          return 0;
        });
    return t;
  }
#endif
  per_frame_cell_update(tld);
  return t;
}

/*!
 * (pc-sphere-in-loaded-city-infos? this sphere bit): #t / #f, or 0 when the GOAL code must run.
 */
u64 pc_sphere_in_loaded_city_infos_impl(const NativeArgs& args) {
  static int on = -1;
  if (!native_enabled("pc-sphere-in-loaded-city-infos?", on) || !city_infos_inline_ok()) {
    return 0;
  }
  const u32 engine = (u32)args.a[0];
  const u32 sphere = (u32)args.a[1];
  const s64 bit = (s64)args.a[2];
  const u64 f = args.st;
  const u64 t = args.st + 4;
#if MIPS2C_WRITE_LOG
  static bool nested = false;
  if (nested) {
    return 0;
  }
  if (native_verify_inline_on()) {
    static MethodSlot goal_fn("traffic-engine", 14);
    goal_fn.unchanged();
    return native_verify_inline(
        "sphere-in-loaded-city-infos?", {},
        [&]() -> u64 {
          const int r = sphere_in_loaded_city_infos(engine, sphere, bit);
          return r < 0 ? 0 : (r ? t : f);
        },
        [&]() -> u64 {
          nested = true;
          const u64 a[8] = {engine, sphere, (u64)bit, 0, 0, 0, 0, 0};
          const u64 r = native_call_goal(goal_fn.fn, a, args);
          nested = false;
          return r;
        });
  }
#endif
  const int r = sphere_in_loaded_city_infos(engine, sphere, bit);
  return r < 0 ? 0 : (r ? t : f);
}

const NativeImpl pc_per_frame_cell_update = MIPS2C_NATIVE_IMPL_GOAL(pc_per_frame_cell_update_impl, 0);
const NativeImpl pc_sphere_in_loaded_city_infos =
    MIPS2C_NATIVE_IMPL_GOAL(pc_sphere_in_loaded_city_infos_impl, 0);

}  // namespace Mips2C::jak2::native2

namespace Mips2C::jak2::pc_traffic_natives {
void link() {
  gLinkedFunctionTable.reg("pc-per-frame-cell-update", native2::pc_per_frame_cell_update.as_exec,
                           64, &native2::pc_per_frame_cell_update);
  gLinkedFunctionTable.reg("pc-sphere-in-loaded-city-infos?",
                           native2::pc_sphere_in_loaded_city_infos.as_exec, 64,
                           &native2::pc_sphere_in_loaded_city_infos);
}
}  // namespace Mips2C::jak2::pc_traffic_natives
