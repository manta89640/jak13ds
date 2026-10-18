/*!
 * @file native2_collide_hash.cpp
 * (AI-assisted)
 * Native versions of the mips2c functions in collide_hash.cpp (Jak 2's spatial hash of the
 * background collision and of the collidable objects): (method 11 collide-hash) and
 * (method 12 collide-hash) (box and line-sphere queries of the object hash),
 * fill-bg-using-box-new and fill-bg-using-line-sphere-new (the triangles of one
 * collide-hash-fragment into the collide cache). See game/mips2c/mips2c_native.h for the rules:
 * the same float operations, grouped the same way, as the mips2c code.
 */

#include <algorithm>
#include <cstring>

#include "native2.h"

#include "game/kernel/jak2/kscheme.h"

#if defined(__GNUC__) || defined(__clang__)
#define N2CH_ALIGNED16(p) __builtin_assume_aligned((p), 16)
#else
#define N2CH_ALIGNED16(p) (p)
#endif

namespace Mips2C::jak2::native2 {

namespace {

/*!
 * GOAL memory with the base pointer in a local (a store doesn't make the compiler reload
 * g_ee_main_mem). Stores are logged like gstore (verify mode).
 */
struct Mem {
  u8* const base = g_ee_main_mem;
  template <typename T>
  T ld(u32 addr) const {
    T v;
    memcpy(&v, base + addr, sizeof(T));
    return v;
  }
  //! lw (sign-extended)
  s64 lw(u32 addr) const { return ld<s32>(addr); }
  //! lqc2 (16-byte aligned)
  void qf(float out[4], u32 addr) const { memcpy(out, N2CH_ALIGNED16(base + addr), 16); }
  //! lq as 4 words (aligns down)
  void qi(s32 out[4], u32 addr) const { memcpy(out, N2CH_ALIGNED16(base + (addr & ~15u)), 16); }
  template <typename T>
  void st(u32 addr, T v) const {
    MIPS2C_NATIVE_LOG_STORE(addr, (u32)sizeof(T));
    memcpy(base + addr, &v, sizeof(T));
  }
  //! sqc2 (16-byte aligned)
  void stq(u32 addr, const void* v) const {
    MIPS2C_NATIVE_LOG_STORE(addr, 16);
    memcpy(N2CH_ALIGNED16(base + addr), v, 16);
  }
  //! sq (aligns down)
  void sq(u32 addr, const void* v) const { stq(addr & ~15u, v); }
};

//! mult3: the low 32 bits of the product, sign-extended
inline s64 mult3(s64 a, s64 b) {
  return (s64)(s32)(u32)((s64)(s32)a * (s64)(s32)b);
}
//! multu3
inline s64 multu3(s64 a, s64 b) {
  return (s64)(s32)(u32)((u64)(u32)a * (u64)(u32)b);
}
//! subu
inline s64 subu(s64 a, s64 b) {
  return (s64)(s32)((u32)a - (u32)b);
}

//! the clamp limits of the grid: the bytes of (dimension word - 0x10101) (dsubu, pextlb, pextlh)
inline void dim_limits(s32 lim[4], s64 dim) {
  const u32 d = (u32)(dim - 0x10101);
  for (int i = 0; i < 4; i++) {
    lim[i] = (s32)((d >> (8 * i)) & 0xff);
  }
}

//! pcgtw amin > bmax, pcgtw bmin > amax, por, ppach, dsll 16: a miss in x, y or z
inline bool box_miss(const s32 amin[4], const s32 amax[4], const s32 bmin[4], const s32 bmax[4]) {
  return amin[0] > bmax[0] || bmin[0] > amax[0] || amin[1] > bmax[1] || bmin[1] > amax[1] ||
         amin[2] > bmax[2] || bmin[2] > amax[2];
}

//! vftoi0 of 4 lanes, pmaxw 0, pminw lim
inline void grid_clamp(s32 out[4], const float in[4], const s32 lim[4]) {
  for (int i = 0; i < 4; i++) {
    s32 v = in[i];
    v = std::max(v, 0);
    out[i] = std::min(v, lim[i]);
  }
}

//! vftoi0 with the saturation patch of collide_hash.cpp (float_to_int_sat)
inline s32 ftoi_sat(float f) {
  if (f >= (float)INT32_MAX) {
    return INT32_MAX;
  } else if (f <= (float)INT32_MIN) {
    return INT32_MIN;
  } else {
    return f;
  }
}

/*!
 * One item of a collide-hash bucket: set its bit in the scratchpad (once per query) and return
 * true if it wasn't set yet.
 */
inline bool take_bit(const Mem& m, u32 spad, s64 id) {
  const u32 byte_addr = (u32)(s64)((s32)id >> 3) + spad;
  const s64 bit = (s64)((s32)1 << (id & 7));
  const s64 b = m.ld<s8>(byte_addr);
  if (b & bit) {
    return false;
  }
  m.st<u8>(byte_addr, (u8)(b | bit));
  return true;
}

/*!
 * Adds a collidable to the collide list (a2: count, then 8-byte entries from 16). Returns false
 * when the list is full (256).
 */
inline bool add_to_list(const Mem& m, u32 list, u32 obj, u64 st) {
  const s64 count = m.lw(list);
  if (count - 256 >= 0) {
    return false;
  }
  const u32 e = (u32)(s32)((u32)count << 3) + list;
  m.st<u32>(list, (u32)(count + 1));
  m.st<u32>(e + 20, (u32)st);
  m.st<u32>(e + 16, obj);
  return true;
}

/*!
 * The start of methods 11 and 12: clears the bit array of the items in the scratchpad, then
 * the query's box in grid cells (into query + 400 / 416, clamped to the grid).
 * Returns false if the query's box misses the hash's box.
 */
bool hash_query_cells(const Mem& m,
                      u32 hash,
                      u32 query,
                      u32 spad,
                      s32 qmin_i[4],
                      s32 qmax_i[4]) {
  const u64 n = m.ld<u32>(hash + 8);
  if ((spad & 15) == 0) {
    if (n) {
      MIPS2C_NATIVE_LOG_STORE(spad, (u32)(n * 16));
      memset(m.base + spad, 0, n * 16);
    }
  } else {
    const u32 zero[4] = {0, 0, 0, 0};
    for (s64 i = 0; i < (s64)n; i++) {
      m.sq((u32)(i << 4) + spad, zero);
    }
  }
  float hmin[4], qmin[4], qmax[4], scale[4];
  m.qf(hmin, hash + 28);
  m.qf(qmin, query + 128);
  m.qf(qmax, query + 144);
  m.qf(scale, hash + 44);
  s32 hmin_i[4], hmax_i[4];
  m.qi(qmin_i, query + 160);
  m.qi(qmax_i, query + 176);
  m.qi(hmin_i, hash + 60);
  m.qi(hmax_i, hash + 76);
  for (int i = 0; i < 4; i++) {
    qmin[i] = qmin[i] - hmin[i];
    qmax[i] = qmax[i] - hmin[i];
  }
  for (int i = 0; i < 4; i++) {
    qmin[i] = qmin[i] * scale[i];
    qmax[i] = qmax[i] * scale[i];
  }
  if (box_miss(hmin_i, hmax_i, qmin_i, qmax_i)) {
    return false;
  }
  s32 lim[4], lo[4], hi[4];
  dim_limits(lim, m.lw(hash + 72));
  grid_clamp(lo, qmin, lim);
  grid_clamp(hi, qmax, lim);
  m.sq(query + 400, lo);
  m.sq(query + 416, hi);
  return true;
}

/*!
 * (method 11 collide-hash) (this, ?, collide-list, query): the collidables of the hash whose
 * bounding sphere's box touches the query's box, added to the list (each one once).
 */
u64 method_11_collide_hash_impl(const NativeArgs& args) {
  static const u32 spad_sym = sym_value_addr("*fake-scratchpad-data*");
  const Mem m;
  const u32 hash = (u32)args.a[0];
  const u32 list = (u32)args.a[2];
  const u32 query = (u32)args.a[3];
  const u32 spad = m.ld<u32>(spad_sym);
  s32 qmin_i[4], qmax_i[4];
  if (!hash_query_cells(m, hash, query, spad, qmin_i, qmax_i)) {
    return 0;
  }
  const s64 xstride = 4;
  const s64 zstride = multu3(m.ld<u8>(hash + 72), xstride);
  const s64 ystride = multu3(m.ld<u8>(hash + 74), zstride);
  const s64 lx = m.lw(query + 400), ly = m.lw(query + 404), lz = m.lw(query + 408);
  const s64 cx = 1 - lx + m.lw(query + 416);
  const s64 cy = 1 - ly + m.lw(query + 420);
  const s64 cz = 1 - lz + m.lw(query + 424);
  const u32 items_base = (u32)m.lw(hash + 56);
  u32 yrow = (u32)((u64)m.ld<u32>(hash + 40) +
                   (u64)(mult3(lx, xstride) + mult3(ly, ystride) + mult3(lz, zstride)));
  s64 ycnt = cy;
  do {
    s64 zcnt = cz;
    u32 zrow = yrow;
    do {
      s64 xcnt = cx;
      u32 cell = zrow;
      do {
        const u32 first = m.ld<u16>(cell);
        s64 cnt = m.ld<u16>(cell + 2);
        u32 item = (u32)(s32)(first << 3) + items_base;
        if (cnt != 0) {
          do {
            const s64 id = m.lw(item);
            const u32 obj = (u32)m.lw(item + 4);
            cnt--;
            if (take_bit(m, spad, id)) {
              float sph[4];
              m.qf(sph, obj + 12);
              s32 smin[4], smax[4];
              for (int i = 0; i < 3; i++) {
                smin[i] = sph[i] - sph[3];
                smax[i] = sph[i] + sph[3];
              }
              if (!box_miss(smin, smax, qmin_i, qmax_i)) {
                if (!add_to_list(m, list, obj, args.st)) {
                  return 0;
                }
              }
            }
            item += 8;
          } while (cnt > 0);
        }
        xcnt--;
        cell += (u32)xstride;
      } while (xcnt != 0);
      zcnt--;
      zrow += (u32)zstride;
    } while (zcnt != 0);
    ycnt--;
    yrow += (u32)ystride;
  } while (ycnt != 0);
  return 0;
}

/*!
 * The lanes 0..2 of the slab test of methods 12 and fill-bg-using-line-sphere-new: with
 * acc = dir * (corner - start) per lane, the entry (vf13) and exit (vf15) values of both corners
 * (lo, hi) widened by e10 / e11, then max / min over the lanes as the VU0 code does (vmax / vmini,
 * then .y and .z broadcasts). Writes the max lane 0, the min lanes 0 and 1.
 */
inline void slab_reduce(const float lo[3],
                        const float hi[3],
                        const float dir[4],
                        const float start[4],
                        const float e10[4],
                        const float e11[4],
                        float* mx0,
                        float* mn0,
                        float* mn1) {
  float a[3], b[3];
  for (int i = 0; i < 3; i++) {
    float acc = dir[i] * lo[i];
    acc -= dir[i] * start[i];
    const float m13 = acc - 1.0f * e10[i];
    const float m15 = acc + 1.0f * e11[i];
    acc = dir[i] * hi[i];
    acc -= dir[i] * start[i];
    const float m14 = acc - 1.0f * e11[i];
    const float m16 = acc + 1.0f * e10[i];
    a[i] = std::max(m13, m14);
    b[i] = std::min(m15, m16);
  }
  // vmax.y / vmini.y (from copies), then .z
  const float a0 = std::max(a[0], a[1]);
  const float a2 = std::max(a[2], a[1]);
  const float b0 = std::min(b[0], b[1]);
  const float b1 = std::min(b[1], b[1]);
  const float b2 = std::min(b[2], b[1]);
  *mx0 = std::max(a0, a2);
  *mn0 = std::min(b0, b2);
  *mn1 = std::min(b1, b2);
}

/*!
 * The cell / triangle test after slab_reduce, with the values converted by vftoi4 or vftoi12:
 * (bltz of min - max, of 4096 - max, and of the 64-bit register of the min (lane 1's sign)).
 */
inline bool slab_pass(s32 imx0, s32 imn0, s32 imn1) {
  if (subu(imn0, imx0) < 0) {
    return false;
  }
  if (subu(4096, imx0) < 0) {
    return false;
  }
  return imn1 >= 0;
}

/*!
 * (method 12 collide-hash) (this, ?, collide-list, query): the collidables of the hash in the
 * cells the query's line-sphere crosses, whose sphere is within the radius of the line, added to
 * the list (each one once).
 */
u64 method_12_collide_hash_impl(const NativeArgs& args) {
  static const u32 spad_sym = sym_value_addr("*fake-scratchpad-data*");
  const Mem m;
  const u32 hash = (u32)args.a[0];
  const u32 list = (u32)args.a[2];
  const u32 query = (u32)args.a[3];
  const u32 spad = m.ld<u32>(spad_sym);
  s32 qmin_i[4], qmax_i[4];
  if (!hash_query_cells(m, hash, query, spad, qmin_i, qmax_i)) {
    return 0;
  }
  float hmin[4], e10[4], e11[4], start[4], dir9[4], cell_size[4], dir7[4];
  m.qf(hmin, hash + 28);
  m.qf(e10, query + 256);
  m.qf(e11, query + 272);
  m.qf(start, query + 208);
  m.qf(dir9, query + 224);
  m.qf(cell_size, hash + 12);
  m.qf(dir7, query + 240);
  const float one = std::max(0.0f, 1.0f);  // vmaxw.xyzw vf1, vf0, vf0
  const s64 xstride = 4;
  const s64 zstride = multu3(m.ld<u8>(hash + 72), xstride);
  const s64 ystride = multu3(m.ld<u8>(hash + 74), zstride);
  const s64 lx = m.lw(query + 400), ly = m.lw(query + 404), lz = m.lw(query + 408);
  const s64 cx = 1 - lx + m.lw(query + 416);
  const s64 cy = 1 - ly + m.lw(query + 420);
  const s64 cz = 1 - lz + m.lw(query + 424);
  const u32 items_base = (u32)m.lw(hash + 56);
  const float r = e10[3];
  // the item test's constants: vf14 = vf1 * vf7.w
  const float v14 = one * dir7[3];
  u32 yrow = (u32)((u64)m.ld<u32>(hash + 40) +
                   (u64)(mult3(lx, xstride) + mult3(ly, ystride) + mult3(lz, zstride)));
  s64 ycnt = cy;
  do {
    s64 zcnt = cz;
    u32 zrow = yrow;
    do {
      s64 xcnt = cx;
      u32 cell = zrow;
      do {
        // the cell's index (sw 432..444, lqc2, vitof0)
        const s64 ix = (cx - xcnt) + m.lw(query + 400);
        const s64 iy = (cy - ycnt) + m.lw(query + 404);
        const s64 iz = (cz - zcnt) + m.lw(query + 408);
        m.st<u32>(query + 444, 0);
        m.st<u32>(query + 432, (u32)ix);
        m.st<u32>(query + 436, (u32)iy);
        m.st<u32>(query + 440, (u32)iz);
        const float cf[3] = {(float)(s32)ix, (float)(s32)iy, (float)(s32)iz};
        float lo[3], hi[3];
        for (int i = 0; i < 3; i++) {
          float acc = cf[i] * cell_size[i];
          acc += one * hmin[i];
          lo[i] = acc - one * r;
          acc += one * cell_size[i];
          hi[i] = acc + one * r;
        }
        float mx0, mn0, mn1;
        slab_reduce(lo, hi, dir7, start, e10, e11, &mx0, &mn0, &mn1);
        const s32 imx0 = mx0 * 16.f;
        const s32 imn0 = mn0 * 16.f;
        const s32 imn1 = mn1 * 16.f;
        if (slab_pass(imx0, imn0, imn1)) {
          const u32 first = m.ld<u16>(cell);
          s64 cnt = m.ld<u16>(cell + 2);
          u32 item = (u32)(s32)(first << 3) + items_base;
          if (cnt != 0) {
            do {
              const s64 id = m.lw(item);
              const u32 obj = (u32)m.lw(item + 4);
              cnt--;
              if (take_bit(m, spad, id)) {
                float sph[4];
                m.qf(sph, obj + 12);
                float v13[3];
                for (int i = 0; i < 3; i++) {
                  const float acc = dir9[i] * sph[i];
                  v13[i] = acc - dir9[i] * start[i];
                }
                float acc = one * sph[3];
                const float v15 = acc + one * r;
                acc = v13[0] * dir7[3];
                acc += v14 * v13[1];
                float t = acc + v14 * v13[2];
                t = std::min(t, 1.0f);
                t = std::max(t, 0.0f);
                float e[3];
                for (int i = 0; i < 3; i++) {
                  float a = dir9[i] * t;
                  a += one * start[i];
                  e[i] = a - one * sph[i];
                }
                for (int i = 0; i < 3; i++) {
                  e[i] = e[i] * e[i];
                }
                // lanes 0 and 1 (the 64-bit register bgtz tests); vf15's lanes are all v15
                float d0 = 0.0f * 0.0f;
                d0 -= v15 * v15;
                d0 += one * e[0];
                d0 += one * e[1];
                d0 = d0 + one * e[2];
                const s64 reg = (s64)(((u64)f2u(d0) << 32) | f2u(d0));
                if (reg <= 0) {
                  if (!add_to_list(m, list, obj, args.st)) {
                    return 0;
                  }
                }
              }
              item += 8;
            } while (cnt > 0);
          }
        }
        xcnt--;
        cell += (u32)xstride;
      } while (xcnt != 0);
      zcnt--;
      zrow += (u32)zstride;
    } while (zcnt != 0);
    ycnt--;
    yrow += (u32)ystride;
  } while (ycnt != 0);
  return 0;
}

//! ldr / ldl pair: 8 unaligned bytes, then pextlh (zero-extended halfwords) and psllw 4
inline void load_vert(s32 out[4], const Mem& m, u32 addr) {
  const u64 d = m.ld<u64>(addr);
  for (int i = 0; i < 4; i++) {
    out[i] = (s32)((u32)((d >> (16 * i)) & 0xffff));
  }
}

//! the triangle's vertex addresses: vertex index * 6 + the vertex array
inline void tri_verts(const Mem& m, u32 poly, u32 verts, u32* p0, u32* p1, u32* p2) {
  *p0 = (u32)(m.ld<u8>(poly) * 6) + verts;
  *p1 = (u32)(m.ld<u8>(poly + 1) * 6) + verts;
  *p2 = (u32)(m.ld<u8>(poly + 2) * 6) + verts;
}

//! vmulax / vmadday / vmaddaz / vmaddw with vf0.w: a vertex through the query's matrix
inline void xform(float out[4], const float v[4], const float mt[16]) {
  for (int i = 0; i < 4; i++) {
    float acc = mt[i] * v[0];
    acc += mt[4 + i] * v[1];
    acc += mt[8 + i] * v[2];
    out[i] = acc + mt[12 + i] * 1.0f;
  }
}

/*!
 * The end of the fill-bg functions: collide-stats, or the debug complaint when the collide cache
 * is full (only in debug cheat mode). Returns v0.
 */
u64 cache_full(const Mem& m, const NativeArgs& args, u64 a3, u64 t0, u64 t1) {
  static const u32 cheat_sym = sym_value_addr("*cheat-mode*");
  static const u32 debug_sym = ::jak2::intern_from_c("debug").offset;
  static const u32 print_sym = sym_value_addr("print-exceeded-max-cache-tris");
  const s64 cheat = m.lw(cheat_sym);
  if (cheat == (s64)(u64)debug_sym) {
    const u64 call_args[8] = {(u64)cheat, args.a[1], args.a[2], a3, t0, t1, 0, 0};
    native_call_goal((u32)m.lw(print_sym), call_args, args);
  }
  return 0;
}

void count_query(const Mem& m) {
  static const u32 stats_sym = sym_value_addr("*collide-stats*");
  const u32 stats = (u32)m.lw(stats_sym);
  m.st<u32>(stats + 12, m.ld<u32>(stats + 12) + 1);
}

/*!
 * fill-bg-using-box-new (collide-cache, collide-hash-fragment, query) -> #f, or 0 when the cache
 * is full. The fragment's triangles in the grid cells the query's box covers (each once), whose
 * pat isn't masked and whose box touches the query's box, into the collide cache (through the
 * query's matrix unless it is #f).
 */
u64 fill_bg_using_box_new_impl(const NativeArgs& args) {
  static const u32 spad_sym = sym_value_addr("*fake-scratchpad-data*");
  const Mem m;
  const u32 cache = (u32)args.a[0];
  const u32 frag = (u32)args.a[1];
  const u32 query = (u32)args.a[2];
  const u64 st = args.st;

  float fmin[4], q2[4], q3[4], scale[4];
  m.qf(fmin, frag + 44);
  m.qf(q2, query + 128);
  m.qf(q3, query + 144);
  m.qf(scale, frag + 60);
  for (int i = 0; i < 4; i++) {
    q2[i] = q2[i] - fmin[i];
    q3[i] = q3[i] - fmin[i];
  }
  s32 qmin_i[4], qmax_i[4], fmin_i[4], fmax_i[4];
  m.qi(qmin_i, query + 160);
  m.qi(qmax_i, query + 176);
  m.qi(fmin_i, frag + 76);
  m.qi(fmax_i, frag + 92);
  s32 d4[4], d5[4];
  for (int i = 0; i < 4; i++) {
    d4[i] = q2[i];
    d5[i] = q3[i];
  }
  for (int i = 0; i < 4; i++) {
    q2[i] = q2[i] * scale[i];
    q3[i] = q3[i] * scale[i];
  }
  if (box_miss(fmin_i, fmax_i, qmin_i, qmax_i)) {
    return st;
  }
  // the query's box in fragment units / 16 (psraw 4), for the triangles' box test
  s32 bmin[4], bmax[4];
  for (int i = 0; i < 4; i++) {
    bmin[i] = d4[i] >> 4;
    bmax[i] = d5[i] >> 4;
  }
  m.sq(query + 368, bmin);
  m.sq(query + 384, bmax);
  s32 lim[4], lo[4], hi[4];
  dim_limits(lim, m.lw(frag + 40));
  for (int i = 0; i < 4; i++) {
    s32 v = ftoi_sat(q2[i]);
    v = std::max(v, 0);
    lo[i] = std::min(v, lim[i]);
    v = ftoi_sat(q3[i]);
    v = std::max(v, 0);
    hi[i] = std::min(v, lim[i]);
  }
  float mt[16];
  m.qf(mt, query + 448);
  m.qf(mt + 4, query + 464);
  m.qf(mt + 8, query + 480);
  m.qf(mt + 12, query + 496);
  m.sq(query + 400, lo);
  m.sq(query + 416, hi);
  // reloaded by the original from the stores above
  m.qi(bmin, query + 368);
  m.qi(bmax, query + 384);

  const s64 xstride = 4;
  const s64 zstride = multu3(m.ld<u8>(frag + 40), xstride);  // t8
  const s64 ystride = multu3(m.ld<u8>(frag + 42), zstride);  // t6
  const s64 lx = m.lw(query + 400), ly = m.lw(query + 404), lz = m.lw(query + 408);
  const s64 cx = 1 - lx + m.lw(query + 416);  // a3
  const s64 cy = 1 - ly + m.lw(query + 420);  // t1
  const s64 cz = 1 - lz + m.lw(query + 424);  // t0
  u32 yrow = (u32)((u64)m.ld<u32>(frag + 8) +
                   (u64)(mult3(lx, xstride) + mult3(ly, ystride) + mult3(lz, zstride)));
  const u32 poly_base = (u32)m.lw(frag + 104);
  const u32 poly_data = (u32)m.lw(frag + 72);
  const u32 pats = (u32)m.lw(frag + 4);
  const u32 verts = (u32)m.lw(frag + 88);
  const u32 mask = m.ld<u32>(query + 96);
  const u32 xf = m.ld<u32>(query + 512);
  const bool use_matrix = (u64)xf != st;
  const u32 tris = cache + 4908;
  const u32 spad = m.ld<u32>(spad_sym);

  s64 ycnt = cy;
  do {
    s64 zcnt = cz;
    u32 zrow = yrow;
    m.st<u32>(query + 524, yrow);
    m.st<u32>(query + 528, (u32)ystride);
    do {
      s64 xcnt = cx;
      u32 cell = zrow;
      m.st<u32>(query + 532, zrow);
      m.st<u32>(query + 536, (u32)zstride);
      do {
        u32 poly = (u32)m.ld<u16>(cell) + poly_base;
        s64 cnt = m.ld<u16>(cell + 2);
        if (cnt != 0) {
          do {
            const u32 idx = m.ld<u8>(poly);
            const u32 tri = (idx << 2) + poly_data;
            cnt--;
            if (take_bit(m, spad, idx)) {
              const u32 pat = m.ld<u32>((u32)(m.ld<u8>(tri + 3) << 2) + pats);
              if (!(mask & pat)) {
                u32 p0, p1, p2;
                tri_verts(m, tri, verts, &p0, &p1, &p2);
                s32 v0[4], v1[4], v2[4];
                load_vert(v0, m, p0);
                load_vert(v1, m, p1);
                load_vert(v2, m, p2);
                s32 tmin[4], tmax[4];
                for (int i = 0; i < 3; i++) {
                  tmin[i] = std::min(std::min(v0[i], v1[i]), v2[i]);
                  tmax[i] = std::max(std::max(v0[i], v1[i]), v2[i]);
                }
                if (!box_miss(tmin, tmax, bmin, bmax)) {
                  m.st<u32>(spad + 2048, (u32)(m.lw(spad + 2048) + 1));
                  float w0[4], w1[4], w2[4];
                  for (int i = 0; i < 4; i++) {
                    w0[i] = (float)(s32)((u32)v0[i] << 4);
                    w1[i] = (float)(s32)((u32)v1[i] << 4);
                    w2[i] = (float)(s32)((u32)v2[i] << 4);
                  }
                  for (int i = 0; i < 4; i++) {
                    w0[i] = w0[i] + fmin[i];
                    w1[i] = w1[i] + fmin[i];
                    w2[i] = w2[i] + fmin[i];
                  }
                  if (use_matrix) {
                    float t[4];
                    xform(t, w0, mt);
                    memcpy(w0, t, 16);
                    xform(t, w1, mt);
                    memcpy(w1, t, 16);
                    xform(t, w2, mt);
                    memcpy(w2, t, 16);
                  }
                  const s64 count = m.lw(cache);
                  if (count - 460 >= 0) {
                    return cache_full(m, args, (u64)cx, (u64)cz, (u64)ycnt);
                  }
                  const u32 dst = (u32)((u64)count << 6) + tris;
                  m.st<u32>(cache, (u32)(count + 1));
                  const u32 pq[4] = {pat, xf, 0, 0};
                  m.sq(dst + 48, pq);
                  m.stq(dst, w0);
                  m.stq(dst + 16, w1);
                  m.stq(dst + 32, w2);
                }
              }
            }
            poly++;
          } while (cnt > 0);
        }
        xcnt--;
        cell += (u32)xstride;
      } while (xcnt != 0);
      zcnt--;
      zrow = (u32)((u64)m.lw(query + 532) + (u64)m.lw(query + 536));
    } while (zcnt != 0);
    ycnt--;
    yrow = (u32)((u64)m.lw(query + 524) + (u64)m.lw(query + 528));
  } while (ycnt != 0);
  count_query(m);
  return st;
}

/*!
 * fill-bg-using-line-sphere-new (collide-cache, collide-hash-fragment, query) -> #f, or 0 when the
 * cache is full. Like fill-bg-using-box-new for the grid cells and the triangles whose boxes the
 * query's line-sphere crosses.
 */
u64 fill_bg_using_line_sphere_new_impl(const NativeArgs& args) {
  static const u32 spad_sym = sym_value_addr("*fake-scratchpad-data*");
  const Mem m;
  const u32 cache = (u32)args.a[0];
  const u32 frag = (u32)args.a[1];
  const u32 query = (u32)args.a[2];
  const u64 st = args.st;

  float fmin[4], q3[4], q4[4], scale[4];
  m.qf(fmin, frag + 44);
  m.qf(q3, query + 128);
  m.qf(q4, query + 144);
  m.qf(scale, frag + 60);
  for (int i = 0; i < 4; i++) {
    q3[i] = q3[i] - fmin[i];
    q4[i] = q4[i] - fmin[i];
  }
  s32 qmin_i[4], qmax_i[4], fmin_i[4], fmax_i[4];
  m.qi(qmin_i, query + 160);
  m.qi(qmax_i, query + 176);
  m.qi(fmin_i, frag + 76);
  m.qi(fmax_i, frag + 92);
  for (int i = 0; i < 4; i++) {
    q3[i] = q3[i] * scale[i];
    q4[i] = q4[i] * scale[i];
  }
  if (box_miss(fmin_i, fmax_i, qmin_i, qmax_i)) {
    return st;
  }
  float e10[4], e11[4], start[4], cell_size[4], dir7[4], mt[16];
  m.qf(e10, query + 256);
  m.qf(e11, query + 272);
  m.qf(start, query + 208);
  m.qf(cell_size, frag + 28);
  m.qf(dir7, query + 240);
  s32 lim[4], lo[4], hi[4];
  dim_limits(lim, m.lw(frag + 40));
  for (int i = 0; i < 4; i++) {
    s32 v = ftoi_sat(q3[i]);
    v = std::max(v, 0);
    lo[i] = std::min(v, lim[i]);
    v = ftoi_sat(q4[i]);
    v = std::max(v, 0);
    hi[i] = std::min(v, lim[i]);
  }
  const float one = std::max(0.0f, 1.0f);  // vmaxw.xyzw vf1, vf0, vf0
  m.qf(mt, query + 448);
  m.qf(mt + 4, query + 464);
  m.qf(mt + 8, query + 480);
  m.qf(mt + 12, query + 496);
  m.sq(query + 400, lo);
  m.sq(query + 416, hi);
  // the line's start relative to the fragment
  for (int i = 0; i < 4; i++) {
    start[i] = start[i] - fmin[i];
  }
  const float r = e10[3];

  const s64 xstride = 4;                                     // t8
  const s64 zstride = multu3(m.ld<u8>(frag + 40), xstride);  // t9
  const s64 ystride = multu3(m.ld<u8>(frag + 42), zstride);  // t6
  const s64 lx = m.lw(query + 400), ly = m.lw(query + 404), lz = m.lw(query + 408);
  const s64 cx = 1 - lx + m.lw(query + 416);  // v1
  const s64 cy = 1 - ly + m.lw(query + 420);  // a3
  const s64 cz = 1 - lz + m.lw(query + 424);  // t0
  u32 yrow = (u32)((u64)m.ld<u32>(frag + 8) +
                   (u64)(mult3(lx, xstride) + mult3(ly, ystride) + mult3(lz, zstride)));
  const u32 poly_base = (u32)m.lw(frag + 104);
  const u32 poly_data = (u32)m.lw(frag + 72);
  const u32 pats = (u32)m.lw(frag + 4);
  const u32 verts = (u32)m.lw(frag + 88);
  const u32 mask = m.ld<u32>(query + 96);
  const u32 tris = cache + 4908;
  const u32 spad = m.ld<u32>(spad_sym);

  s64 ycnt = cy;
  do {
    s64 zcnt = cz;
    u32 zrow = yrow;
    m.st<u32>(query + 524, yrow);
    m.st<u32>(query + 528, (u32)ystride);
    do {
      s64 xcnt = cx;
      u32 cell = zrow;
      m.st<u32>(query + 532, zrow);
      m.st<u32>(query + 536, (u32)zstride);
      do {
        m.st<u32>(query + 520, (u32)xstride);
        const s64 ix = (cx - xcnt) + m.lw(query + 400);
        const s64 iy = (cy - ycnt) + m.lw(query + 404);
        const s64 iz = (cz - zcnt) + m.lw(query + 408);
        m.st<u32>(query + 444, 0);
        m.st<u32>(query + 432, (u32)ix);
        m.st<u32>(query + 436, (u32)iy);
        m.st<u32>(query + 440, (u32)iz);
        const float cf[3] = {(float)(s32)ix, (float)(s32)iy, (float)(s32)iz};
        float clo[3], chi[3];
        for (int i = 0; i < 3; i++) {
          float acc = cf[i] * cell_size[i];
          clo[i] = acc - one * r;
          acc += one * cell_size[i];
          chi[i] = acc + one * r;
        }
        float mx0, mn0, mn1;
        slab_reduce(clo, chi, dir7, start, e10, e11, &mx0, &mn0, &mn1);
        const s32 imx0 = mx0 * 4096.f;
        const s32 imn0 = mn0 * 4096.f;
        const s32 imn1 = mn1 * 4096.f;
        s64 cnt = m.ld<u16>(cell + 2);
        if (slab_pass(imx0, imn0, imn1) && cnt != 0) {
          u32 poly = (u32)m.ld<u16>(cell) + poly_base;
          do {
            const u32 idx = m.ld<u8>(poly);
            const u32 tri = (idx << 2) + poly_data;
            cnt--;
            if (take_bit(m, spad, idx)) {
              const u32 pat = m.ld<u32>((u32)(m.ld<u8>(tri + 3) << 2) + pats);
              if (!(mask & pat)) {
                u32 p0, p1, p2;
                tri_verts(m, tri, verts, &p0, &p1, &p2);
                s32 i0[4], i1[4], i2[4];
                load_vert(i0, m, p0);
                load_vert(i1, m, p1);
                load_vert(i2, m, p2);
                float v17[4], v18[4], v19[4];
                for (int i = 0; i < 4; i++) {
                  v17[i] = (float)(s32)((u32)i0[i] << 4);
                  v18[i] = (float)(s32)((u32)i1[i] << 4);
                  v19[i] = (float)(s32)((u32)i2[i] << 4);
                }
                m.st<u32>(spad + 2048, (u32)(m.lw(spad + 2048) + 1));
                float tlo[3], thi[3];
                for (int i = 0; i < 3; i++) {
                  tlo[i] = std::min(v17[i], v18[i]);
                  thi[i] = std::max(v17[i], v18[i]);
                }
                for (int i = 0; i < 4; i++) {
                  v17[i] = v17[i] + fmin[i];
                  v18[i] = v18[i] + fmin[i];
                }
                for (int i = 0; i < 3; i++) {
                  tlo[i] = std::min(tlo[i], v19[i]);
                  thi[i] = std::max(thi[i], v19[i]);
                }
                for (int i = 0; i < 4; i++) {
                  v19[i] = v19[i] + fmin[i];
                }
                for (int i = 0; i < 3; i++) {
                  tlo[i] = tlo[i] - r;
                  thi[i] = thi[i] + r;
                }
                slab_reduce(tlo, thi, dir7, start, e10, e11, &mx0, &mn0, &mn1);
                const s32 tmx0 = mx0 * 4096.f;
                const s32 tmn0 = mn0 * 4096.f;
                const s32 tmn1 = mn1 * 4096.f;
                if (slab_pass(tmx0, tmn0, tmn1)) {
                  const u32 xf = m.ld<u32>(query + 512);
                  const s64 count = m.lw(cache);
                  if (count - 460 >= 0) {
                    return cache_full(m, args, (u64)cy, (u64)cz, (u64)ycnt);
                  }
                  const u32 dst = (u32)((u64)count << 6) + tris;
                  m.st<u32>(cache, (u32)(count + 1));
                  const u32 pq[4] = {pat, xf, 0, 0};
                  m.sq(dst + 48, pq);
                  if ((u64)xf != st) {
                    float w[4];
                    xform(w, v17, mt);
                    m.stq(dst, w);
                    xform(w, v18, mt);
                    m.stq(dst + 16, w);
                    xform(w, v19, mt);
                    m.stq(dst + 32, w);
                  } else {
                    m.stq(dst, v17);
                    m.stq(dst + 16, v18);
                    m.stq(dst + 32, v19);
                  }
                }
              }
            }
            poly++;
          } while (cnt > 0);
        }
        xcnt--;
        cell += (u32)m.lw(query + 520);
      } while (xcnt != 0);
      zcnt--;
      zrow = (u32)((u64)m.lw(query + 532) + (u64)m.lw(query + 536));
    } while (zcnt != 0);
    ycnt--;
    yrow = (u32)((u64)m.lw(query + 524) + (u64)m.lw(query + 528));
  } while (ycnt != 0);
  count_query(m);
  return st;
}

}  // namespace

const NativeImpl method_11_collide_hash = MIPS2C_NATIVE_IMPL(method_11_collide_hash_impl, 0, 0);
const NativeImpl method_12_collide_hash = MIPS2C_NATIVE_IMPL(method_12_collide_hash_impl, 0, 0);
const NativeImpl fill_bg_using_box_new = MIPS2C_NATIVE_IMPL(fill_bg_using_box_new_impl, 0, 0);
const NativeImpl fill_bg_using_line_sphere_new =
    MIPS2C_NATIVE_IMPL(fill_bg_using_line_sphere_new_impl, 0, 0);

}  // namespace Mips2C::jak2::native2
