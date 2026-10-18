/*!
 * @file native2_spatial_hash.cpp
 * (AI-assisted)
 * Native versions of the Jak 2 spatial hash's mips2c functions (spatial_hash.cpp): the grid-hash
 * bucket bits and searches, the sphere-hash queries and the spatial-hash actor lists (the traffic's
 * avoidance queries). See game/mips2c/mips2c_native.h for the rules: the same float operations,
 * grouped the same way as the mips2c code (one expression per mips2c helper), the same 64-bit
 * integer arithmetic, the same stores.
 *
 * grid-hash fields (offsets from the basic's address): search-box 4 (min x y z, max x y z),
 * bucket-size 10, axis-scale 12, dimension-array 24, bucket-array 28, box-min 32,
 * object-count 56, work 0 (its search result buffer at +12). sphere-hash: sphere-array 84,
 * max-object-count 88. spatial-hash: object-array 100.
 */

#include <algorithm>
#include <cmath>

#include "native2.h"

namespace Mips2C::jak2::native2 {

namespace {

//! GOAL memory with the base pointer in a local (a store doesn't make the compiler reload it)
struct M {
  u8* const b = g_ee_main_mem;
  template <typename T>
  T ld(u32 addr) const {
    T v;
    memcpy(&v, b + addr, sizeof(T));
    return v;
  }
  template <typename T>
  void st(u32 addr, T v) const {
    MIPS2C_NATIVE_LOG_STORE(addr, (u32)sizeof(T));
    memcpy(b + addr, &v, sizeof(T));
  }
  float f(u32 addr) const { return ld<float>(addr); }
  //! lwc1 then mfc1: the float's bits, sign-extended
  s64 fbits(u32 addr) const { return (s64)ld<s32>(addr); }
};

//! mult3: the low 32 bits of the product of the low words, sign-extended
inline s64 mult3(s64 a, s64 b) {
  return (s64)(s32)(u32)((s64)(s32)a * (s64)(s32)b);
}

//! the method of an object (Jak 2: type at obj - 4, method n at type + 16 + 4n)
inline u32 method_of(const M& m, u32 obj, int n) {
  return m.ld<u32>(m.ld<u32>(obj - 4) + 16 + 4 * n);
}

// *perf-stats* + 116: the counter the spatial hash queries bump, and the perf counters (no-ops on
// the PC: the mips2c code adds the control word to the counter words)
u32 perf_stats_116(const M& m) {
  static const u32 sym = sym_value_addr("*perf-stats*");
  return (u32)((s64)m.ld<s32>(sym) + 116);
}
void perf_begin(const M& m, u32 ps) {
  m.st<u32>(ps + 4, m.ld<u32>(ps + 4) + 1);
}
void perf_end(const M& m, u32 ps) {
  const u32 ctrl = m.ld<u32>(ps + 28);
  if (ctrl) {
    u64 a0 = ctrl;
    a0 = (u64)m.ld<u32>(ps + 32) + a0;
    m.st<u32>(ps + 32, (u32)a0);
    a0 = (u64)m.ld<u32>(ps + 36) + a0;
    m.st<u32>(ps + 36, (u32)a0);
  }
}

/*!
 * (method 18 / 19 grid-hash) inlined: sets (or clears) the bit of object id in every bucket of the
 * box. The loops count like the original (decrement, then test against 0).
 */
inline void box_bits(const M& m, u32 hash, u32 box, s64 id, bool clear) {
  const s64 bsize = m.ld<s16>(hash + 10);
  const s64 stride2 = mult3(m.ld<s8>(hash + 24), bsize);
  const s64 stride1 = mult3(m.ld<s8>(hash + 26), stride2);
  const s64 min0 = m.ld<s8>(box + 0), min1 = m.ld<s8>(box + 1), min2 = m.ld<s8>(box + 2);
  const s64 n0 = 1 - min0 + m.ld<s8>(box + 3);
  const s64 n2 = 1 - min2 + m.ld<s8>(box + 5);
  const s64 n1 = 1 - min1 + m.ld<s8>(box + 4);
  const s64 off = mult3(min0, bsize) + mult3(min1, stride1) + mult3(min2, stride2) + (id >> 3);
  u32 p1 = (u32)(off + (s64)m.ld<u32>(hash + 28));
  const u64 bit = (u64)1 << (id & 7);
  const u8 mask = clear ? (u8)~bit : (u8)bit;
  u8* const b = m.b;
  s64 c1 = n1;
  do {
    s64 c2 = n2;
    u32 p2 = p1;
    do {
      s64 c0 = n0;
      u32 p = p2;
      do {
        MIPS2C_NATIVE_LOG_STORE(p, 1);
        if (clear) {
          b[p] &= mask;
        } else {
          b[p] |= mask;
        }
        p += (u32)bsize;
      } while (--c0 != 0);
      p2 += (u32)stride2;
    } while (--c2 != 0);
    p1 += (u32)stride1;
  } while (--c1 != 0);
}

/*!
 * (method 22 grid-hash) sphere-to-grid-box: the grid box of a sphere, clamped to the grid
 * (vf lanes, then the integer lane ops pmaxw / pminw / ppach / ppacb), stored in the low 6 bytes of
 * the box's doubleword.
 */
void sphere_to_grid_box(const M& m, u32 hash, u32 box, u32 sphere) {
  float s[4], sc[4], bm[4];
  memcpy(s, m.b + (sphere & ~15u), 16);         // lq
  memcpy(sc, m.b + ((hash + 12) & ~15u), 16);   // lq: axis-scale, dims
  memcpy(bm, m.b + ((hash + 28) & ~15u), 16);   // lq: bucket-array, box-min
  const float v4[4] = {bm[1], bm[2], bm[3], bm[0]};  // vmr32
  float v3[4], v2[4], v6[4], v7[4];
  for (int i = 0; i < 4; i++) {
    v3[i] = s[i] - s[3];
    v2[i] = s[i] + s[3];
  }
  for (int i = 0; i < 4; i++) {
    v6[i] = v3[i] - v4[i];
    v7[i] = v2[i] - v4[i];
  }
  for (int i = 0; i < 4; i++) {
    v6[i] = v6[i] * sc[i];
    v7[i] = v7[i] * sc[i];
  }
  s32 i6[4], i7[4];
  for (int i = 0; i < 3; i++) {
    i6[i] = (s32)v6[i];
    i7[i] = (s32)v7[i];
  }
  memcpy(&i6[3], &v6[3], 4);
  memcpy(&i7[3], &v7[3], 4);
  // the dimensions - 1, one per byte
  const s64 dims = (s64)(s32)(((u32)(s32)m.ld<s32>(hash + 24) << 8) >> 8);
  const u64 lim_bytes = (u64)(dims - 0x10101);
  u64 lo = 0, hi = 0;
  for (int i = 0; i < 4; i++) {
    const s32 lim = (s32)((lim_bytes >> (8 * i)) & 0xff);
    const s32 a = std::min(std::max(i6[i], 0), lim);
    const s32 c = std::min(std::max(i7[i], 0), lim);
    lo |= (u64)((u32)a & 0xff) << (8 * i);
    hi |= (u64)((u32)c & 0xff) << (8 * i);
  }
  const u64 packed = lo | (hi << 24);
  const u64 old = m.ld<u64>(box);
  m.st<u64>(box, (old & 0xffff000000000000ull) | packed);
}

/*!
 * (method 20 grid-hash) do-search!: the OR of the buckets of the box into buf (the result bits),
 * 8 bytes at a time with unaligned loads like ldr / ldl. Each cell ORs ceil(bucket-size / 8)
 * doublewords (the last one past the bucket when the size isn't a multiple of 8, as the
 * original).
 */
u32 do_search(const M& m, u32 hash, u32 box, u32 buf) {
  const s64 bsize = m.ld<s16>(hash + 10);
  const s64 stride2 = mult3(bsize, m.ld<s8>(hash + 24));
  const s64 stride1 = mult3(stride2, m.ld<s8>(hash + 26));
  const s64 min0 = m.ld<s8>(box + 0), min1 = m.ld<s8>(box + 1), min2 = m.ld<s8>(box + 2);
  const s64 n0 = m.ld<s8>(box + 3) - min0;
  const s64 n2 = m.ld<s8>(box + 5) - min2;
  const s64 n1 = m.ld<s8>(box + 4) - min1;
  u32 p1 = (u32)(mult3(min0, bsize) + mult3(min1, stride1) + mult3(min2, stride2) +
                 (s64)m.ld<u32>(hash + 28));
  const u64 zero[2] = {0, 0};
  gstore_bytes(buf & ~15u, zero, 16);
  gstore_bytes((buf + 16) & ~15u, zero, 16);
  u8* const b = m.b;
  // doublewords per cell, and how far the source moves
  const s64 words = bsize < 9 ? 1 : bsize / 8 + ((bsize % 8) != 0);
  constexpr int kMaxWords = 16;
  if (bsize > 0 && words <= kMaxWords) {
    u64 acc[kMaxWords];
    for (s64 k = 0; k < words; k++) {
      memcpy(&acc[k], b + (u32)(buf + 8 * k), 8);
    }
    s64 c1 = n1;
    do {
      s64 c2 = n2;
      u32 p2 = p1;
      do {
        s64 c0 = n0;
        u32 p = p2;
        do {
          for (s64 k = 0; k < words; k++) {
            u64 v;
            memcpy(&v, b + (u32)(p + 8 * k), 8);
            acc[k] |= v;
          }
          p += (u32)bsize;
        } while (c0-- > 0);
        p2 += (u32)stride2;
      } while (c2-- > 0);
      p1 += (u32)stride1;
    } while (c1-- > 0);
    for (s64 k = 0; k < words; k++) {
      m.st<u64>((u32)(buf + 8 * k), acc[k]);
    }
    return buf;
  }
  // general case (bucket-size <= 0 or very large): the original's loop
  s64 c1 = n1;
  do {
    s64 c2 = n2;
    u32 p2 = p1;
    do {
      s64 c0 = n0;
      u32 p = p2;
      do {
        s64 t7 = bsize;
        u32 dst = buf;
        if (!(bsize < 9)) {
          bool more;
          do {
            u64 v;
            memcpy(&v, b + p, 8);
            const u32 d = dst;
            dst += 8;
            t7 -= 8;
            more = !(t7 < 8);
            m.st<u64>(d, m.ld<u64>(d) | v);
            p += 8;
          } while (more);
          if (t7 == 0) {
            continue;
          }
        }
        u64 v;
        memcpy(&v, b + p, 8);
        p += (u32)t7;
        m.st<u64>(dst, m.ld<u64>(dst) | v);
      } while (c0-- > 0);
      p2 += (u32)stride2;
    } while (c2-- > 0);
    p1 += (u32)stride1;
  } while (c1-- > 0);
  return buf;
}

//! (method 22 this box sphere) through the method table; the native when it holds the stub
void call_sphere_to_grid_box(const M& m, u32 hash, u32 box, u32 sphere, const NativeArgs& args) {
  static const u32* stub = native_stub_slot("(method 22 grid-hash)");
  const u32 fn = method_of(m, hash, 22);
  if (fn == *stub) {
    sphere_to_grid_box(m, hash, box, sphere);
  } else {
    const u64 a[8] = {hash, box, sphere, args.a[3], args.a[4], args.a[5], args.a[6], args.a[7]};
    native_call_goal(fn, a, args);
  }
}

//! (method 20 this box buf) through the method table; the native when it holds the stub
void call_do_search(const M& m, u32 hash, u32 box, u32 buf, const NativeArgs& args) {
  static const u32* stub = native_stub_slot("(method 20 grid-hash)");
  const u32 fn = method_of(m, hash, 20);
  if (fn == *stub) {
    do_search(m, hash, box, buf);
  } else {
    const u64 a[8] = {hash, box, buf, args.a[3], args.a[4], args.a[5], args.a[6], args.a[7]};
    native_call_goal(fn, a, args);
  }
}

//! the work's search result buffer (work + 12)
inline u32 search_buf(const M& m, u32 hash) {
  return m.ld<u32>(hash) + 12;
}

/*!
 * The set bits of the search result, as the original walks them: byte by byte (at least one),
 * bit by bit while bits are left. f(index) returns true to stop.
 */
template <typename F>
bool for_each_bit(const M& m, u32 buf, s64 nbytes, F&& f) {
  s64 i = 0;
  u32 p = buf;
  do {
    s64 idx = i * 8;
    s64 bits = m.ld<u8>(p);
    if (bits != 0) {
      do {
        if (bits & 1) {
          if (f(idx)) {
            return true;
          }
        }
        bits >>= 1;
        idx++;
      } while (bits != 0);
    }
    i++;
    p++;
  } while (i < nbytes);
  return false;
}

//! clears the bit of id (unless it's -1) in the search result
inline void clear_result_bit(const M& m, u32 buf, s64 id) {
  if (id != -1) {
    const u32 a = (u32)(buf + (id >> 3));
    m.st<u8>(a, m.ld<u8>(a) & (u8) ~((u64)1 << (id & 7)));
  }
}

inline u64 sym_bool(const NativeArgs& args, bool v) {
  return (u64)(u32)(v ? args.st + 4 : args.st);
}

/*!
 * The 1 / |dir|^2 of the line-sphere queries (methods 32 sphere-hash and 37 spatial-hash), 0 when
 * |dir|^2 isn't > 0.
 */
inline float inv_len2(const M& m, u32 dir) {
  float d[4];
  memcpy(d, m.b + dir, 16);  // lqc2
  const float one = 0.f + 1.f;  // vaddw.x vf2, vf0, vf0
  float sq[4];
  for (int i = 0; i < 4; i++) {
    sq[i] = d[i] * d[i];
  }
  float acc = one * sq[0];
  acc += one * sq[1];
  const float len2 = acc + one * sq[2];
  float inv = 0;
  if (0.f < len2) {
    inv = 1.f / len2;
  }
  return inv;
}

/*!
 * The point of the line nearest to the sphere (the parameter clamped to 0..1), stored at the
 * scratch vector `out` (vector-vector-distance-squared reads it from GOAL memory).
 */
inline void line_closest(const M& m, u32 start, u32 dir, u32 sph, float inv, u32 out) {
  float s4[4], s5[4], v6[3];
  memcpy(s4, m.b + sph, 16);
  memcpy(s5, m.b + start, 16);
  for (int i = 0; i < 3; i++) {
    v6[i] = s4[i] - s5[i];
  }
  float f0 = m.f(dir + 0), f1 = m.f(dir + 4), f2 = m.f(dir + 8);
  const float f3 = v6[0], f4 = v6[1], f5 = v6[2];
  f0 = (f0 * f3) + (f1 * f4) + (f2 * f5);
  f1 = f0 * inv;
  f0 = 0;
  f2 = 1.f;
  f1 = std::min(f2, f1);
  f0 = std::max(f0, f1);
  float vd[4], vs[4];
  memcpy(vd, m.b + dir, 16);
  memcpy(vs, m.b + start, 16);
  float r[4];
  for (int i = 0; i < 3; i++) {
    const float acc = vd[i] * f0;
    r[i] = acc + vs[i] * 1.f;
  }
  r[3] = 1.f + 0.f;  // vaddx.w vf4, vf0, vf0
  gstore_q(out, r);
}

}  // namespace

// ---------------------------------------------------------------------------
// grid-hash
// ---------------------------------------------------------------------------

//! (AI-assisted) (method 18 grid-hash this box id): sets the object's bit in the box's buckets
u64 method_18_grid_hash_impl(const NativeArgs& args) {
  const M m;
  box_bits(m, (u32)args.a[0], (u32)args.a[1], (s64)args.a[2], false);
  return 0;
}

//! (AI-assisted) (method 19 grid-hash this box id): clears the object's bit in the box's buckets
u64 method_19_grid_hash_impl(const NativeArgs& args) {
  const M m;
  box_bits(m, (u32)args.a[0], (u32)args.a[1], (s64)args.a[2], true);
  return 0;
}

//! (AI-assisted) (method 20 grid-hash this box buf) do-search!: the OR of the box's buckets
u64 method_20_grid_hash_impl(const NativeArgs& args) {
  const M m;
  do_search(m, (u32)args.a[0], (u32)args.a[1], (u32)args.a[2]);
  return args.a[2];  // v0 = a2
}

//! (AI-assisted) (method 22 grid-hash this box sphere) sphere-to-grid-box
u64 method_22_grid_hash_impl(const NativeArgs& args) {
  const M m;
  sphere_to_grid_box(m, (u32)args.a[0], (u32)args.a[1], (u32)args.a[2]);
  return 0;
}

// ---------------------------------------------------------------------------
// sphere-hash
// ---------------------------------------------------------------------------

/*!
 * (AI-assisted) (method 28 sphere-hash this) update-from-spheres: update-grid (GOAL), then every
 * sphere's bit in the buckets of its grid box.
 */
u64 method_28_sphere_hash_impl(const NativeArgs& args) {
  const M m;
  const u32 h = (u32)args.a[0];
  {
    const u64 a[8] = {h, args.a[1], args.a[2], args.a[3], args.a[4], args.a[5], args.a[6],
                      args.a[7]};
    native_call_goal(method_of(m, h, 24), a, args);
  }
  for (s64 i = 0; i < (s64)m.ld<s16>(h + 56); i++) {
    const u32 sphere = (u32)((s64)m.ld<u32>(h + 84) + i * 16);
    call_sphere_to_grid_box(m, h, h + 4, sphere, args);
    box_bits(m, h, h + 4, i, false);
  }
  return 0;
}

/*!
 * (AI-assisted) (method 29 sphere-hash this sphere mask out max): the ids of the spheres (with a
 * mask bit) whose xz distance to the sphere is in (0, r1 + r2), as bytes at out; returns the count.
 */
u64 method_29_sphere_hash_impl(const NativeArgs& args) {
  const M m;
  const u32 h = (u32)args.a[0];
  const u32 sph = (u32)args.a[1];
  const u64 mask = args.a[2];
  const u32 out = (u32)args.a[3];
  const s64 max = (s64)args.a[4];
  const u32 ps = perf_stats_116(m);
  perf_begin(m, ps);
  call_sphere_to_grid_box(m, h, h + 4, sph, args);
  const u32 buf = search_buf(m, h);
  call_do_search(m, h, h + 4, buf, args);
  s64 count = 0;
  for_each_bit(m, buf, m.ld<s16>(h + 10), [&](s64 idx) {
    const u32 t0 = (u32)((s64)m.ld<u32>(h + 84) + idx * 16);
    if ((mask & (u64)m.fbits(t0 + 12)) != 0) {
      float f3 = m.f(sph + 0);
      float f1 = m.f(sph + 8);
      const float f4 = m.f(t0 + 0);
      float f2 = m.f(t0 + 8);
      float f0 = m.f(sph + 12);
      f3 = f4 - f3;
      f1 = f2 - f1;
      f1 = (f3 * f3) + (f1 * f1);
      if (0.f < f1) {
        f2 = m.f(t0 + 12);
        f0 = f2 + f0;
        f0 = f0 * f0;
        if (f1 < f0 && count < max) {
          m.st<u8>((u32)(out + count), (u8)idx);
          count++;
        }
      }
    }
    return false;
  });
  perf_end(m, ps);
  return (u64)count;
}

/*!
 * (AI-assisted) (method 30 sphere-hash this params) find-nav-sphere-ids: like method 29 with a y
 * threshold, the ids appended to params' array (len / max-len).
 */
u64 method_30_sphere_hash_impl(const NativeArgs& args) {
  const M m;
  const u32 h = (u32)args.a[0];
  const u32 p = (u32)args.a[1];
  const u32 ps = perf_stats_116(m);
  perf_begin(m, ps);
  call_sphere_to_grid_box(m, h, h + 4, p, args);
  const u32 buf = search_buf(m, h);
  call_do_search(m, h, h + 4, buf, args);
  m.st<u16>(p + 20, 0);
  for_each_bit(m, buf, m.ld<s16>(h + 10), [&](s64 idx) {
    const u32 a3 = (u32)((s64)m.ld<u32>(h + 84) + idx * 16);
    if (((u64)m.ld<u8>(p + 24) & (u64)m.fbits(a3 + 12)) != 0) {
      float f3 = m.f(p + 0);
      float f1 = m.f(p + 8);
      const float f4 = m.f(a3 + 0);
      float f2 = m.f(a3 + 8);
      float f0 = m.f(p + 12);
      f3 = f4 - f3;
      f1 = f2 - f1;
      f1 = (f3 * f3) + (f1 * f1);
      if (0.f < f1) {
        f2 = m.f(p + 4);
        f3 = m.f(a3 + 4);
        f3 = f3 - f2;
        f2 = m.f(p + 16);
        f3 = std::abs(f3);
        if (f3 < f2) {
          f2 = m.f(a3 + 12);
          f0 = f2 + f0;
          f0 = f0 * f0;
          if (f1 < f0) {
            const s64 len = m.ld<s16>(p + 20);
            if (len < (s64)m.ld<s16>(p + 22)) {
              m.st<u8>((u32)((s64)m.ld<u32>(p + 28) + len), (u8)idx);
              m.st<u16>(p + 20, (u16)(m.ld<s16>(p + 20) + 1));
            }
          }
        }
      }
    }
    return false;
  });
  perf_end(m, ps);
  return 0;
}

/*!
 * (AI-assisted) (method 31 sphere-hash this sphere mask id) add-sphere-with-mask-and-id's test:
 * #t when a sphere with a mask bit (other than id) overlaps the sphere (x and z only, as the
 * original), else #f.
 */
u64 method_31_sphere_hash_impl(const NativeArgs& args) {
  const M m;
  const u32 h = (u32)args.a[0];
  const u32 g = (u32)args.a[1];
  const u64 mask = args.a[2];
  const s64 id = (s64)args.a[3];
  const u32 ps = perf_stats_116(m);
  perf_begin(m, ps);
  call_sphere_to_grid_box(m, h, h + 4, g, args);
  const u32 buf = search_buf(m, h);
  call_do_search(m, h, h + 4, buf, args);
  clear_result_bit(m, buf, id);
  const bool found = for_each_bit(m, buf, m.ld<s16>(h + 10), [&](s64 idx) {
    const u32 t0 = (u32)((s64)m.ld<u32>(h + 84) + idx * 16);
    if ((mask & (u64)m.fbits(t0 + 12)) == 0) {
      return false;
    }
    float f0 = m.f(g + 0);
    float f1 = m.f(t0 + 0);
    float f2 = m.f(g + 8);
    const float f3 = m.f(t0 + 8);
    f0 = f1 - f0;
    f1 = f3 - f2;
    f2 = f3 - f2;
    f0 = (f2 * f2) + (f1 * f1) + (f0 * f0);
    f1 = m.f(t0 + 12);
    f2 = m.f(g + 12);
    f1 = f1 + f2;
    f1 = f1 * f1;
    return f0 < f1;
  });
  perf_end(m, ps);
  return sym_bool(args, found);
}

/*!
 * (AI-assisted) (method 32 sphere-hash this start dir radius id): #t when a sphere (other than id)
 * is within radius + its radius of the segment.
 * line-sphere-to-grid-box and vector-vector-distance-squared are GOAL code.
 */
u64 method_32_sphere_hash_impl(const NativeArgs& args) {
  static const u32 vvds_sym = sym_value_addr("vector-vector-distance-squared");
  const M m;
  const u32 h = (u32)args.a[0];
  const u32 start = (u32)args.a[1];
  const u32 dir = (u32)args.a[2];
  const s64 radius_bits = (s64)(s32)(u32)args.a[3];
  const s64 id = (s64)args.a[4];
  const u32 vec = (args.stack - 16) & ~15u;  // the stack vector the distance is taken from
  const u32 ps = perf_stats_116(m);
  perf_begin(m, ps);
  {
    const u64 a[8] = {h, h + 4, start, dir, (u64)radius_bits, args.a[5], args.a[6], args.a[7]};
    native_call_goal(method_of(m, h, 23), a, args);
  }
  const u32 buf = search_buf(m, h);
  call_do_search(m, h, h + 4, buf, args);
  clear_result_bit(m, buf, id);
  const float inv = inv_len2(m, dir);
  const float radius = u2f((u32)radius_bits);
  const bool found = for_each_bit(m, buf, m.ld<s16>(h + 10), [&](s64 idx) {
    const u32 s0 = (u32)((s64)m.ld<u32>(h + 84) + idx * 16);
    line_closest(m, start, dir, s0, inv, vec);
    const u64 a[8] = {vec, s0, (u64)(args.stack - 32), args.a[3], args.a[4],
                      args.a[5], args.a[6], args.a[7]};
    const float f0 = u2f((u32)native_call_goal(m.ld<u32>(vvds_sym), a, args));
    float f1 = radius;
    const float f2 = m.f(s0 + 12);
    f1 = f1 + f2;
    f1 = f1 * f1;
    return f0 < f1;
  });
  perf_end(m, ps);
  return sym_bool(args, found);
}

/*!
 * (AI-assisted) (method 33 sphere-hash this sphere id) remove-by-id: clears the id's bit in the
 * buckets of the sphere's grid box.
 */
u64 method_33_sphere_hash_impl(const NativeArgs& args) {
  const M m;
  const u32 h = (u32)args.a[0];
  call_sphere_to_grid_box(m, h, h + 4, (u32)args.a[1], args);
  box_bits(m, h, h + 4, (s64)args.a[2], true);
  return 0;
}

// ---------------------------------------------------------------------------
// spatial-hash
// ---------------------------------------------------------------------------

//! (AI-assisted) (method 33 spatial-hash this sphere id): method 33 sphere-hash, then the
//! object-array entry's object = #f
u64 method_33_spatial_hash_impl(const NativeArgs& args) {
  const M m;
  const u32 h = (u32)args.a[0];
  const s64 id = (s64)args.a[2];
  call_sphere_to_grid_box(m, h, h + 4, (u32)args.a[1], args);
  box_bits(m, h, h + 4, id, true);
  m.st<u32>((u32)((u64)(id << 4) + (u64)m.ld<u32>(h + 100)), (u32)args.st);
  return 0;
}

/*!
 * (AI-assisted) (method 35 spatial-hash this bbox out max) fill-actor-list-for-box: the grid box of
 * the bounding box, then the objects whose sphere overlaps the box, appended to out (max).
 */
u64 method_35_spatial_hash_impl(const NativeArgs& args) {
  const M m;
  const u32 h = (u32)args.a[0];
  const u32 bb = (u32)args.a[1];
  const u32 out = (u32)args.a[2];
  const s64 max = (s64)args.a[3];
  const u32 ps = perf_stats_116(m);
  perf_begin(m, ps);
  for (u32 i = 0; i < 3; i++) {
    for (u32 k = 0; k < 2; k++) {
      float f0 = 0;
      float f1 = m.f(bb + 16 * k + 4 * i);
      float f2 = m.f(h + 32 + 4 * i);
      f1 = f1 - f2;
      f2 = m.f(h + 12 + 4 * i);
      f1 = f1 * f2;
      const s64 t3 = (s64)m.ld<s8>(h + 24 + i) - 1;
      f2 = (float)(s32)(u32)t3;
      f1 = std::min(f1, f2);
      f0 = std::max(f0, f1);
      const s32 v = (s32)f0;
      m.st<u8>(h + 4 + i + 3 * k, (u8)v);
    }
  }
  const u32 buf = search_buf(m, h);
  call_do_search(m, h, h + 4, buf, args);
  s64 count = 0;
  for_each_bit(m, buf, m.ld<s16>(h + 10), [&](s64 idx) {
    const u32 t0 = (u32)((s64)m.ld<u32>(h + 84) + idx * 16);
    const float sx = m.f(t0 + 0), sy = m.f(t0 + 4), sz = m.f(t0 + 8), r = m.f(t0 + 12);
    bool in = !((sx + r) < m.f(bb + 0));
    in = in && !(m.f(bb + 16) < (sx - r));
    in = in && !((sy + r) < m.f(bb + 4));
    in = in && !(m.f(bb + 20) < (sy - r));
    in = in && !((sz + r) < m.f(bb + 8));
    in = in && !(m.f(bb + 24) < (sz - r));
    if (in && count < max) {
      const u32 obj = m.ld<u32>((u32)((u64)(idx << 4) + (u64)m.ld<u32>(h + 100)));
      m.st<u32>((u32)(out + (u64)(count << 2)), obj);
      count++;
    }
    return false;
  });
  perf_end(m, ps);
  return (u64)count;
}

/*!
 * (AI-assisted) (method 36 spatial-hash this sphere out max) fill-actor-list-for-sphere: the objects
 * whose sphere overlaps the sphere, appended to out (max).
 */
u64 method_36_spatial_hash_impl(const NativeArgs& args) {
  const M m;
  const u32 h = (u32)args.a[0];
  const u32 g = (u32)args.a[1];
  const u32 out = (u32)args.a[2];
  const s64 max = (s64)args.a[3];
  const u32 ps = perf_stats_116(m);
  perf_begin(m, ps);
  call_sphere_to_grid_box(m, h, h + 4, g, args);
  const u32 buf = search_buf(m, h);
  call_do_search(m, h, h + 4, buf, args);
  s64 count = 0;
  float v2[4];
  for_each_bit(m, buf, m.ld<s16>(h + 10), [&](s64 idx) {
    const u32 t0 = (u32)((s64)m.ld<u32>(h + 84) + idx * 16);
    float v3[4], v1[4];
    memcpy(v2, m.b + g, 16);
    memcpy(v3, m.b + t0, 16);
    for (int i = 0; i < 4; i++) {
      v1[i] = v3[i] - v2[i];
    }
    for (int i = 0; i < 4; i++) {
      v1[i] = v1[i] * v1[i];
    }
    v1[0] = v1[0] + v1[1];
    v1[0] = v1[0] + v1[2];
    const float f0 = v1[0];
    float f1 = m.f(g + 12);
    const float f2 = m.f(t0 + 12);
    f1 = f1 + f2;
    f1 = f1 * f1;
    if (f0 < f1 && count < max) {
      const u32 obj = m.ld<u32>((u32)((u64)(idx << 4) + (u64)m.ld<u32>(h + 100)));
      m.st<u32>((u32)(out + (u64)(count << 2)), obj);
      count++;
    }
    return false;
  });
  perf_end(m, ps);
  return (u64)count;
}

/*!
 * (AI-assisted) (method 37 spatial-hash this start dir radius out max id)
 * fill-actor-list-for-line-sphere: the objects (other than id) within radius + their radius of the
 * segment, appended to out (max). line-sphere-to-grid-box and vector-vector-distance-squared are
 * GOAL code.
 */
u64 method_37_spatial_hash_impl(const NativeArgs& args) {
  static const u32 vvds_sym = sym_value_addr("vector-vector-distance-squared");
  const M m;
  const u32 h = (u32)args.a[0];
  const u32 start = (u32)args.a[1];
  const u32 dir = (u32)args.a[2];
  const s64 radius_bits = (s64)(s32)(u32)args.a[3];
  const u32 out = (u32)args.a[4];
  const s64 max = (s64)args.a[5];
  const s64 id = (s64)args.a[6];
  const u32 vec = (args.stack - 16) & ~15u;
  {
    const u64 a[8] = {h, h + 4, start, dir, (u64)radius_bits, args.a[5], args.a[6], args.a[7]};
    native_call_goal(method_of(m, h, 23), a, args);
  }
  const u32 buf = search_buf(m, h);
  call_do_search(m, h, h + 4, buf, args);
  clear_result_bit(m, buf, id);
  const float inv = inv_len2(m, dir);
  const float radius = u2f((u32)radius_bits);
  s64 count = 0;
  for_each_bit(m, buf, m.ld<s16>(h + 10), [&](s64 idx) {
    const u32 s0 = (u32)((s64)m.ld<u32>(h + 84) + idx * 16);
    line_closest(m, start, dir, s0, inv, vec);
    const u64 a[8] = {vec, s0, (u64)(args.stack - 32), args.a[3], args.a[4],
                      args.a[5], args.a[6], args.a[7]};
    const float f0 = u2f((u32)native_call_goal(m.ld<u32>(vvds_sym), a, args));
    float f1 = radius;
    const float f2 = m.f(s0 + 12);
    f1 = f1 + f2;
    f1 = f1 * f1;
    if (f0 < f1 && count < max) {
      const u32 obj = m.ld<u32>((u32)((u64)(idx << 4) + (u64)m.ld<u32>(h + 100)));
      m.st<u32>((u32)(out + (u64)(count << 2)), obj);
      count++;
    }
    return false;
  });
  return (u64)count;
}

/*!
 * (AI-assisted) (method 39 spatial-hash this sphere info) add-an-object: copies the sphere into the
 * next sphere-array entry (mem-copy!, a forward byte copy, inlined), the object into the
 * object-array, and sets its bit in the buckets of its grid box. Returns its id, -1 when full.
 */
u64 method_39_spatial_hash_impl(const NativeArgs& args) {
  const M m;
  const u32 h = (u32)args.a[0];
  const s64 id = m.ld<s16>(h + 56);
  if (!(id < (s64)m.ld<s16>(h + 88))) {
    return (u64)(s64)-1;
  }
  const u32 s3 = (u32)((s64)m.ld<u32>(h + 84) + (id << 4));
  const u32 s2 = (u32)((s64)m.ld<u32>(h + 100) + (id << 4));
  const u32 src = (u32)args.a[1];
  for (u32 i = 0; i < 16; i++) {
    m.st<u8>(s3 + i, m.ld<u8>(src + i));
  }
  m.st<u32>(s2, (u32)args.a[2]);
  call_sphere_to_grid_box(m, h, h + 4, s3, args);
  box_bits(m, h, h + 4, id, false);
  m.st<u16>(h + 56, (u16)(m.ld<s16>(h + 56) + 1));
  return (u64)id;
}

const NativeImpl method_18_grid_hash = MIPS2C_NATIVE_IMPL_GOAL(method_18_grid_hash_impl, 0);
const NativeImpl method_19_grid_hash = MIPS2C_NATIVE_IMPL_GOAL(method_19_grid_hash_impl, 0);
const NativeImpl method_20_grid_hash = MIPS2C_NATIVE_IMPL_GOAL(method_20_grid_hash_impl, 0);
const NativeImpl method_22_grid_hash = MIPS2C_NATIVE_IMPL_GOAL(method_22_grid_hash_impl, 0);
const NativeImpl method_28_sphere_hash =
    MIPS2C_NATIVE_IMPL(method_28_sphere_hash_impl, NATIVE_CALLS_GOAL, 0);
const NativeImpl method_29_sphere_hash = MIPS2C_NATIVE_IMPL(method_29_sphere_hash_impl, 0, 0);
const NativeImpl method_30_sphere_hash = MIPS2C_NATIVE_IMPL(method_30_sphere_hash_impl, 0, 0);
const NativeImpl method_31_sphere_hash = MIPS2C_NATIVE_IMPL(method_31_sphere_hash_impl, 0, 0);
const NativeImpl method_32_sphere_hash =
    MIPS2C_NATIVE_IMPL(method_32_sphere_hash_impl, NATIVE_CALLS_GOAL, 32);
const NativeImpl method_33_sphere_hash = MIPS2C_NATIVE_IMPL_GOAL(method_33_sphere_hash_impl, 0);
const NativeImpl method_33_spatial_hash = MIPS2C_NATIVE_IMPL_GOAL(method_33_spatial_hash_impl, 0);
const NativeImpl method_35_spatial_hash = MIPS2C_NATIVE_IMPL(method_35_spatial_hash_impl, 0, 0);
const NativeImpl method_36_spatial_hash = MIPS2C_NATIVE_IMPL(method_36_spatial_hash_impl, 0, 0);
const NativeImpl method_37_spatial_hash =
    MIPS2C_NATIVE_IMPL(method_37_spatial_hash_impl, NATIVE_CALLS_GOAL, 32);
const NativeImpl method_39_spatial_hash =
    MIPS2C_NATIVE_IMPL(method_39_spatial_hash_impl, NATIVE_CALLS_GOAL, 0);

}  // namespace Mips2C::jak2::native2
