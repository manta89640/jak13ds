/*!
 * @file native_shadow.cpp
 * (AI-assisted)
 * Native version of Jak 2's shadow-execute (jak2_functions/shadow.cpp), the EE half of the shadow
 * volume renderer, with the 15 mips2c functions it calls and their VU0 microcode
 * (shadow-vu0-block, the same as Jak 1's) inlined. Ported from Jak 1's
 * (jak1_functions/native_shadow.cpp) to Jak 2's code: see game/mips2c/mips2c_native.h for the
 * rules (same float operations, grouped the same way, as the mips2c code).
 *
 * Jak 2's differences from Jak 1: the packet's DMA chain goes to the scratchpad at 48 (settings at
 * 64, the shadow-geo fragment's header at 160), the fragment's qwc (the packet tag's vif0) to the
 * dcache's frag-qwc; the shadow-dcache has the stats at 48 and everything from center on 16 bytes
 * later; *shadow-data* is a smaller structure; shadow-xform-verts zeroes a joint matrix whose first
 * lane is NaN (one-joint vertices); shadow-scissor-edges doesn't clip, it only counts the
 * vertices in front of the near plane (or whose dual isn't behind it) into the scratchpad
 * dcache's ptr-dual-verts; settings flag 64 sends only the vertices and the single triangles
 * (shadow-add-single-tris); the stats are added up; *shadow-debug* calls debug-draw-settings.
 *
 * Like the mips2c version, the work happens on the copy in the scratchpad (the vertices
 * transformed in place, faces = 1 written into the triangle tables) and in the shadow-dcache
 * (*gsf-buffer*: the duals and the lists), so every quirk of the original reads the same bytes:
 * the groups of 4 triangles that run past the end of a table, the vertices loaded past the end of
 * the vertex table, and the VU0 register that shadow-scissor-edges reads without setting it
 * (see StaleRegs).
 */

#include <cmath>

#include "game/kernel/jak2/kscheme.h"
#include "game/mips2c/jak2_functions/native_functions.h"

namespace Mips2C::jak2::native {

namespace {

/*!
 * GOAL memory, with the base pointer in a local: a store to GOAL memory doesn't make the compiler
 * load g_ee_main_mem again. Accesses are as aligned as the MIPS loads and stores of the mips2c
 * code (lw / sw: 4 bytes, lh / sh: 2, ld / sd: 8, lq / sq: the address aligned down to 16, lqc2 /
 * sqc2: 16), so each one is a single ARM or VFP load or store.
 */
struct Mem {
  u8* const base = g_ee_main_mem;

  template <typename T, int A = sizeof(T)>
  T ld(u32 a) const {
    T v;
    memcpy(&v, MIPS2C_ASSUME_ALIGNED(base + a, A), sizeof(T));
    return v;
  }
  float f(u32 a) const { return ld<float>(a); }
  template <typename T, int A = sizeof(T)>
  void st(u32 a, T v) const {
    MIPS2C_NATIVE_LOG_STORE(a, (u32)sizeof(T));
    memcpy(MIPS2C_ASSUME_ALIGNED(base + a, A), &v, sizeof(T));
  }
  //! lqc2 (the address is 16-byte aligned: lqc2 asserts it)
  void qa(float out[4], u32 a) const {
    const auto* p = (const mips2c_alias_f32*)MIPS2C_ASSUME_ALIGNED(base + a, 16);
    out[0] = p[0];
    out[1] = p[1];
    out[2] = p[2];
    out[3] = p[3];
  }
  //! sqc2 (the address is 16-byte aligned: sqc2 asserts it)
  void stqa(u32 a, const float in[4]) const {
    MIPS2C_NATIVE_LOG_STORE(a, 16);
    auto* p = (mips2c_alias_f32*)MIPS2C_ASSUME_ALIGNED(base + a, 16);
    p[0] = in[0];
    p[1] = in[1];
    p[2] = in[2];
    p[3] = in[3];
  }
  //! lq (the address aligned down)
  void q(float out[4], u32 a) const {
    const auto* p = (const mips2c_alias_f32*)MIPS2C_ASSUME_ALIGNED(base + (a & ~15u), 16);
    out[0] = p[0];
    out[1] = p[1];
    out[2] = p[2];
    out[3] = p[3];
  }
  void q(u32 out[4], u32 a) const {
    const auto* p = (const mips2c_alias_u32*)MIPS2C_ASSUME_ALIGNED(base + (a & ~15u), 16);
    out[0] = p[0];
    out[1] = p[1];
    out[2] = p[2];
    out[3] = p[3];
  }
  //! sq (the address aligned down)
  void stq(u32 a, const float in[4]) const {
    a &= ~15u;
    MIPS2C_NATIVE_LOG_STORE(a, 16);
    auto* p = (mips2c_alias_f32*)MIPS2C_ASSUME_ALIGNED(base + a, 16);
    p[0] = in[0];
    p[1] = in[1];
    p[2] = in[2];
    p[3] = in[3];
  }
  void stq(u32 a, const u32 in[4]) const {
    a &= ~15u;
    MIPS2C_NATIVE_LOG_STORE(a, 16);
    auto* p = (mips2c_alias_u32*)MIPS2C_ASSUME_ALIGNED(base + a, 16);
    p[0] = in[0];
    p[1] = in[1];
    p[2] = in[2];
    p[3] = in[3];
  }
  //! sq r0
  void zeroq(u32 a) const {
    const u32 z[4] = {0, 0, 0, 0};
    stq(a, z);
  }
  //! lq then sq
  void copyq(u32 dst, u32 src) const {
    dst &= ~15u;
    MIPS2C_NATIVE_LOG_STORE(dst, 16);
    memcpy(MIPS2C_ASSUME_ALIGNED(base + dst, 16), MIPS2C_ASSUME_ALIGNED(base + (src & ~15u), 16),
           16);
  }
  //! memcpy (emulate_dma's copies, and quadwords copied with lq / sq at aligned addresses)
  void copy(u32 dst, u32 src, u32 size) const {
    if (size) {
      MIPS2C_NATIVE_LOG_STORE(dst, size);
      memcpy(base + dst, base + src, size);
    }
  }
};

// shadow-header (the copy in the scratchpad, hdr): qwc-data 0, num-joints 4, num-verts 8 (s16
// like the lh of the mips2c code), num-twos 10, num-single-tris 12, num-single-edges 14,
// num-double-tris 16, num-double-edges 18, ofs-verts 20, ofs-refs 24, ofs-single-tris 28,
// ofs-single-edges 32, ofs-double-tris 36, ofs-double-edges 40 (from hdr).
// shadow-dcache (*gsf-buffer*, dc): vtx-table 0, single-edge-table 4, double-edge-table 8,
// double-tri-table 12, dcache-top 16, num-facing-single-tris 20, num-single-edges 24,
// num-double-edges 28, single-tri-list 32, single-edge-list 36, double-edge-list 40,
// ptr-dual-verts 44, stats 48 (num-single-tris, num-double-tris, num-single-edges,
// num-double-edges: u32, num-fragments 64, num-objects 66: u16), frag-qwc 68, center 80, plane 96,
// top-plane 112, near-plane 128, light-dir 144, vtx-min 160, data 176.
// shadow-data (*shadow-data*): dma-unpack-template 0, dma-cnt 16, vif-nop 24, vif-unpack-v4-8 28.

/*!
 * spad_to_dma_blerc_chain (emulate_dma): the DMA chain at tadr copied to dst, tags included
 * (cnt / next: the tag and its data, ref / refe: the tag, then the data at its address). For a
 * shadow-dma-packet: its tag and settings (cnt), the shadow-geo (ref) and the joint matrices
 * (ref), then end.
 */
void dma_chain_copy(Mem m, u32 dst, u32 tadr) {
  for (;;) {
    u64 tag;
    memcpy(&tag, m.base + tadr, 8);
    const u32 qwc = (u32)(tag & 0xffff);
    const u32 addr = (u32)(tag >> 32) & 0x7fffffff;
    const u32 kind = (u32)(tag >> 28) & 7;
    switch (kind) {
      case 1:  // cnt
        m.copy(dst, tadr, (1 + qwc) * 16);
        dst += (1 + qwc) * 16;
        tadr += 16 + qwc * 16;
        break;
      case 2:  // next
        m.copy(dst, tadr, (1 + qwc) * 16);
        dst += (1 + qwc) * 16;
        tadr = addr;
        break;
      case 3:  // ref
        m.copy(dst, tadr, 16);
        dst += 16;
        m.copy(dst, addr, qwc * 16);
        dst += qwc * 16;
        tadr += 16;
        break;
      case 0:  // refe
        m.copy(dst, tadr, 16);
        dst += 16;
        m.copy(dst, addr, qwc * 16);
        return;
      case 7:  // end
        return;
      default:
        ASSERT_MSG(false, fmt::format("bad tag: {}", (int)kind));
    }
  }
}

//! the x y z lanes of the 4 rows of a joint matrix (the w lanes are never used)
struct JointMatrix {
  float r[4][3];
};

inline void load_joint(Mem m, u32 mat, JointMatrix* out) {
  for (int k = 0; k < 4; k++) {
    float row[4];
    m.qa(row, mat + 16 * k);
    out->r[k][0] = row[0];
    out->r[k][1] = row[1];
    out->r[k][2] = row[2];
  }
}

/*!
 * vmulaw acc, r3, vf0; vmaddax acc, r0, v; vmadday acc, r1, v; vmaddz.xyz out, r2, v: the joint
 * matrix times the vertex, lanes x y z.
 */
inline void transform_xyz(const JointMatrix& a, float px, float py, float pz, float out[3]) {
  for (int i = 0; i < 3; i++) {
    float acc = a.r[3][i] * 1.f;
    acc += a.r[0][i] * px;
    acc += a.r[1][i] * py;
    out[i] = acc + a.r[2][i] * pz;
  }
}

//! transform_xyz of two vertices with the same matrix, side by side (independent chains)
inline void transform_xyz2(const JointMatrix& a,
                           const float p[4],
                           const float q[4],
                           float op[3],
                           float oq[3]) {
  for (int i = 0; i < 3; i++) {
    float acc_p = a.r[3][i] * 1.f;
    float acc_q = a.r[3][i] * 1.f;
    acc_p += a.r[0][i] * p[0];
    acc_q += a.r[0][i] * q[0];
    acc_p += a.r[1][i] * p[1];
    acc_q += a.r[1][i] * q[1];
    op[i] = acc_p + a.r[2][i] * p[2];
    oq[i] = acc_q + a.r[2][i] * q[2];
  }
}

//! a vertex with two joints: joint-0's position * w + joint-1's * (1 - w), w = 1
inline void blend_two(const float p[4], const float a[3], const float b[3], float o[4]) {
  const float w1 = 1.f - p[3];  // vsubw.w vf10, vf0, vf9
  for (int i = 0; i < 3; i++) {
    const float acc = a[i] * p[3];  // vmulaw.xyz acc, vf10, vf9
    o[i] = acc + b[i] * w1;         // vmaddw.xyz vf9, vf9, vf10
  }
  o[3] = 1.f + 0.f;  // vaddx.w vf9, vf0, vf0
}

/*!
 * Jak 2's shadow-xform-verts zeroes the matrix of a one-joint vertex whose first lane is NaN
 * (bones gone NaN; in Jak 3 they made every triangle single and overflowed a VIF unpack's count).
 * The vertex is then transformed with the zero matrix (0 * an infinite coordinate is still NaN).
 */
inline void load_joint_one(Mem m, u32 mat, JointMatrix* out) {
  load_joint(m, mat, out);
  if (std::isnan(out->r[0][0])) {
    for (int k = 0; k < 4; k++) {
      out->r[k][0] = 0.f;
      out->r[k][1] = 0.f;
      out->r[k][2] = 0.f;
    }
  }
}

/*!
 * shadow-xform-verts: the vertices transformed in place (the scratchpad copy), first the ones
 * with one joint (w kept), then the ones with two. The joints' matrices are 128 bytes apart, from
 * the second one after the fragment (dcache frag-qwc quadwords). When the stores can't change what later vertices read (the
 * vertex table apart from the joint indices and the matrices: always, for real shadow-geos), a
 * matrix stays in registers while the joint repeats (the vertices are grouped by joint: Jak's 118
 * in 26 runs), and two vertices of a run are transformed together.
 */
void xform_verts(Mem m, u32 hdr, u32 dc) {
  const u32 mtx = hdr + (m.ld<u32>(dc + 68) << 4) + 144;
  u32 vtx = hdr + m.ld<u32>(hdr + 20);
  u32 ref = hdr + m.ld<u32>(hdr + 24);
  const s32 num_verts = m.ld<s16>(hdr + 8);
  const s32 num_twos = m.ld<s16>(hdr + 10);
  m.st<u32>(dc, vtx);
  // (counted down to 0 like the original: a negative count doesn't end there either)
  u32 ones = (u32)(num_verts - num_twos);
  u32 twos = (u32)num_twos;

  const u64 vtx_end = (u64)vtx + 16 * (u64)(u16)num_verts;
  const u64 ref_end = (u64)ref + 2 * (u64)(u16)num_verts;
  const bool apart = num_verts >= 0 && num_twos >= 0 && num_twos <= num_verts &&
                     (vtx_end <= ref || ref_end <= vtx) &&
                     (vtx_end <= mtx || (u64)mtx + 256 * 128 <= vtx);
  if (!apart) {
    // the original's order: each vertex's matrices loaded after the previous vertex's store
    for (; ones != 0; ones--, vtx += 16, ref += 2) {
      JointMatrix a;
      float p[4], o[4];
      load_joint_one(m, mtx + ((u32)m.ld<u8>(ref) << 7), &a);
      m.qa(p, vtx);
      transform_xyz(a, p[0], p[1], p[2], o);
      o[3] = p[3];
      m.stqa(vtx, o);
    }
    for (; twos != 0; twos--, vtx += 16, ref += 2) {
      JointMatrix a, b;
      float p[4], ta[3], tb[3], o[4];
      load_joint(m, mtx + ((u32)m.ld<u8>(ref) << 7), &a);
      m.qa(p, vtx);
      load_joint(m, mtx + ((u32)m.ld<u8>(ref + 1) << 7), &b);
      transform_xyz(a, p[0], p[1], p[2], ta);
      transform_xyz(b, p[0], p[1], p[2], tb);
      blend_two(p, ta, tb, o);
      m.stqa(vtx, o);
    }
    return;
  }
  JointMatrix a, b;
  u32 ja = ~0u, jb = ~0u;
  while (ones != 0) {
    const u32 j = m.ld<u8>(ref);
    if (j != ja) {
      load_joint_one(m, mtx + (j << 7), &a);
      ja = j;
    }
    if (ones >= 2 && m.ld<u8>(ref + 2) == j) {
      float p0[4], p1[4], o0[4], o1[4];
      m.qa(p0, vtx);
      m.qa(p1, vtx + 16);
      transform_xyz2(a, p0, p1, o0, o1);
      o0[3] = p0[3];
      o1[3] = p1[3];
      m.stqa(vtx, o0);
      m.stqa(vtx + 16, o1);
      vtx += 32;
      ref += 4;
      ones -= 2;
    } else {
      float p[4], o[4];
      m.qa(p, vtx);
      transform_xyz(a, p[0], p[1], p[2], o);
      o[3] = p[3];
      m.stqa(vtx, o);
      vtx += 16;
      ref += 2;
      ones--;
    }
  }
  ja = ~0u;  // (a may be a zeroed matrix: the two-joint loop doesn't zero them)
  for (; twos != 0; twos--, vtx += 16, ref += 2) {
    const u32 j0 = m.ld<u8>(ref), j1 = m.ld<u8>(ref + 1);
    if (j0 != ja) {
      load_joint(m, mtx + (j0 << 7), &a);
      ja = j0;
    }
    if (j1 != jb) {
      load_joint(m, mtx + (j1 << 7), &b);
      jb = j1;
    }
    float p[4], ta[3], tb[3], o[4];
    m.qa(p, vtx);
    transform_xyz(a, p[0], p[1], p[2], ta);
    transform_xyz(b, p[0], p[1], p[2], tb);
    blend_two(p, ta, tb, o);
    m.stqa(vtx, o);
  }
}

/*!
 * shadow-init-vars: the light direction, the planes and the locus to camera space with
 * *math-camera*'s camera-rot (rows 364..412): light-dir and the planes' normals rotated, the
 * planes' d moved (d * r3.w - r3.x * n.x - r3.y * n.y - r3.z * n.z), the locus transformed.
 */
void init_vars(Mem m, u32 dc, u32 cam) {
  float r0[4], r1[4], r2[4], r3[4];
  m.qa(r0, cam + 364);
  m.qa(r1, cam + 380);
  m.qa(r2, cam + 396);
  m.qa(r3, cam + 412);
  float l[4], p[4], t[4], c[4];
  m.qa(l, dc + 144);
  m.qa(p, dc + 96);
  m.qa(t, dc + 112);
  m.qa(c, dc + 80);
  float lo[4], po[4], to[4], co[4];
  for (int i = 0; i < 4; i++) {
    float acc = r0[i] * l[0];
    acc += r1[i] * l[1];
    lo[i] = acc + r2[i] * l[2];
  }
  for (int i = 0; i < 3; i++) {
    float acc = r0[i] * p[0];
    acc += r1[i] * p[1];
    po[i] = acc + r2[i] * p[2];
  }
  for (int i = 0; i < 3; i++) {
    float acc = r0[i] * t[0];
    acc += r1[i] * t[1];
    to[i] = acc + r2[i] * t[2];
  }
  // vf13 = r3 * vf11, vf14 = r3 * vf12 (the rotated normals, w still the plane's d)
  float p13[4], p14[4];
  for (int i = 0; i < 3; i++) {
    p13[i] = r3[i] * po[i];
    p14[i] = r3[i] * to[i];
  }
  p13[3] = r3[3] * p[3];
  p14[3] = r3[3] * t[3];
  for (int i = 0; i < 4; i++) {
    float acc = r3[i] * 1.f;
    acc += r0[i] * c[0];
    acc += r1[i] * c[1];
    co[i] = acc + r2[i] * c[2];
  }
  float w = p13[3] - p13[0];
  w = w - p13[1];
  po[3] = w - p13[2];
  w = p14[3] - p14[0];
  w = w - p14[1];
  to[3] = w - p14[2];
  m.stqa(dc + 80, co);
  m.stqa(dc + 144, lo);
  m.stqa(dc + 96, po);
  m.stqa(dc + 112, to);
}

/*!
 * The VU0 register that shadow-scissor-edges reads without setting it, left by the functions
 * before it (bits of one lane):
 *   vf8.x: the upper half of the 64 bits that bgtz tests is the dual's z + near d, vf8.x is the
 *     lower half: when that sum is +0, only vf8.x decides. It is vertex 1 of the last group of 4
 *     shadow-calc-dual-verts transformed (as loaded from the table; shadow-scissor-top doesn't
 *     use vf8).
 * (Jak 1's shadow-scissor-edges also read a stale vf4.w; Jak 2's only counts.)
 */
struct StaleRegs {
  u32 vf8_x = 0;
};

//! Vf::saturate_infs of one lane: +-inf to +-FLT_MAX
inline float saturate_inf(float x) {
  const u32 u = f2u(x);
  return (u & 0x7fffffff) == 0x7f800000 ? u2f(u - 1) : x;
}

/*!
 * shadow-calc-dual-verts (shadow-vu0-block 28 and 68, 4 vertices in flight): the dual of vertex p
 * is p - (c - p) * ((p . plane) / ((c - p) . plane.xyz)), c the light locus: where the line from
 * the locus through p meets the plane. For the vertices in slots 2 and 3 of each group of 4 the
 * microcode clamps the products of p . plane and their sum to +-FLT_MAX: when the plain sum is
 * finite no product was infinite, so only an infinite or NaN sum takes the clamped path.
 */
void calc_dual_verts(Mem m, u32 hdr, u32 dc, StaleRegs* regs) {
  const u32 dual = (m.ld<u32>(dc + 16) + 15) & ~15u;
  const u32 vtx = m.ld<u32>(dc);
  float c[4], n[4];
  m.qa(c, dc + 80);
  m.qa(n, dc + 96);
  const s64 count = m.ld<s16>(hdr + 8);
  m.st<u32>(dc + 44, dual);
  if (count == 0) {
    m.st<u32>(dc + 16, dual);
    return;
  }
  {
    // see StaleRegs
    const u32 groups = (u32)((count + 3) >> 2);
    regs->vf8_x = m.ld<u32>((vtx + 64 * (groups - 1) + 16) & ~15u);
  }
  // (lq loads the vertices: the table is 16-byte aligned, shadow-xform-verts' lqc2 asserted it)
  u32 src = vtx, dst = dual;
  s64 left = count;
  for (; left > 0; left -= 4, src += 64, dst += 64) {
    const int k_n = left < 4 ? (int)left : 4;
    float p[4][4], d[4][4], num[4], den[4];
    for (int k = 0; k < 4; k++) {
      m.qa(p[k], src + 16 * k);
    }
    for (int k = 0; k < 4; k++) {
      for (int i = 0; i < 4; i++) {
        d[k][i] = c[i] - p[k][i];
      }
      float pr[4];
      for (int i = 0; i < 4; i++) {
        pr[i] = p[k][i] * n[i];
      }
      float s = pr[0] + pr[1];
      s = s + pr[2];
      num[k] = s + pr[3];
      const float q0 = d[k][0] * n[0], q1 = d[k][1] * n[1], q2 = d[k][2] * n[2];
      const float e = q0 + q1;
      den[k] = e + q2;
    }
    for (int k = 2; k < 4; k++) {
      if ((f2u(num[k]) & 0x7f800000) == 0x7f800000) {
        float pr[4];
        for (int i = 0; i < 4; i++) {
          pr[i] = saturate_inf(p[k][i] * n[i]);
        }
        float s = pr[0] + pr[1];
        s = s + pr[2];
        num[k] = saturate_inf(s + pr[3]);
      }
    }
    float out[4][4];
    for (int k = 0; k < 4; k++) {
      const float t = num[k] / den[k];
      for (int i = 0; i < 4; i++) {
        const float mul = d[k][i] * t;
        out[k][i] = p[k][i] - mul;
      }
    }
    for (int k = 0; k < k_n; k++) {
      m.stqa(dst + 16 * k, out[k]);
    }
  }
  m.st<u32>(dc + 16, dual + 16 * (u32)count);
}

/*!
 * shadow-scissor-top (settings flag 8): a vertex above the top plane ((vertex . top) not
 * negative, the sign bit like bltz) is moved to it along the line to its dual.
 */
void scissor_top(Mem m, u32 hdr, u32 dc) {
  u32 dual = m.ld<u32>(dc + 44);
  u32 vtx = m.ld<u32>(dc);
  float t[4];
  m.qa(t, dc + 112);
  for (u32 k = (u32)(s32)m.ld<s16>(hdr + 8); k != 0; k--, vtx += 16, dual += 16) {
    float w[4], d[4];
    m.qa(w, vtx);
    m.qa(d, dual);
    float e[4], p[4];
    for (int i = 0; i < 4; i++) {
      e[i] = d[i] - w[i];
      p[i] = w[i] * t[i];
    }
    const float q0 = e[0] * t[0], q1 = e[1] * t[1], q2 = e[2] * t[2];
    float s = p[1] + p[0];
    float den = q0 + q1;
    s = s + p[2];
    den = den + q2;
    s = s + p[3];
    if ((s32)f2u(s) < 0) {
      continue;
    }
    const float q = s / den;
    for (int i = 0; i < 4; i++) {
      e[i] = e[i] * q;
      w[i] = w[i] - e[i];
    }
    m.stqa(vtx, w);
  }
}

/*!
 * Jak 2's shadow-scissor-edges: no clipping, it counts into the scratchpad's shadow-dcache
 * ptr-dual-verts (shadow-execute-all zeroes it; "inside the volume" for the camera) the vertices
 * in front of the near plane (z + near d, d = -2 * the camera's d) and the ones whose dual isn't
 * behind it. The tests are the sign bits of 0 + (z + near d) (bltz / bgtz on the qmfc2'd lanes,
 * see StaleRegs for bgtz): bltz's delay slot counts every vertex in front, whatever its dual.
 */
void scissor_edges(Mem m, u32 hdr, u32 dc, u32 spad, StaleRegs regs) {
  u32 dual = m.ld<u32>(dc + 44);
  u32 vtx = m.ld<u32>(dc);
  const float near_d = m.f(dc + 140);
  const u32 n = (u32)(s32)m.ld<s16>(hdr + 8);
  if (n == 0) {
    return;
  }
  u32 count = m.ld<u32>(spad + 44);
  for (u32 k = n; k != 0; k--, vtx += 16, dual += 16) {
    const float z5 = m.f(vtx + 8) + near_d;   // vaddw.z vf7
    const float z8 = m.f(dual + 8) + near_d;  // vaddw.z vf8
    const u32 y7 = f2u(0.f + z5);             // vaddz.y vf7, vf0, vf7
    const u32 y8 = f2u(0.f + z8);
    if ((s32)y7 < 0 || !((s32)y8 >= 0 && (y8 | regs.vf8_x) != 0)) {
      count++;
    }
  }
  m.st<u32>(spad + 44, count);
}

//! x y z of the quadword at p (16-byte aligned, like the lq / lqc2 that load it)
inline void load_xyz(const u8* p, float out[3]) {
  const auto* f = (const mips2c_alias_f32*)MIPS2C_ASSUME_ALIGNED(p, 16);
  out[0] = f[0];
  out[1] = f[1];
  out[2] = f[2];
}

/*!
 * (shadow-vu0-block 0, or the one-at-a-time loops) the sign bit of
 * (c.y * l.y + c.x * l.x) + c.z * l.z, c = (v1 - v0) x (v2 - v0): 1 when the triangle faces the
 * light.
 */
inline u32 tri_facing(const u8* p0, const u8* p1, const u8* p2, const float l[4]) {
  float v0[3], v1[3], v2[3];
  load_xyz(p0, v0);
  load_xyz(p1, v1);
  load_xyz(p2, v2);
  float e1[3], e2[3];
  for (int i = 0; i < 3; i++) {
    e1[i] = v1[i] - v0[i];
    e2[i] = v2[i] - v0[i];
  }
  // vopmula, vopmsub
  const float ax = e1[1] * e2[2], ay = e1[2] * e2[0], az = e1[0] * e2[1];
  const float cx = ax - e2[1] * e1[2];
  const float cy = ay - e2[2] * e1[0];
  const float cz = az - e2[0] * e1[1];
  const float px = cx * l[0], py = cy * l[1], pz = cz * l[2];
  float f = py + px;
  f = f + pz;
  return f2u(f) >> 31;
}

/*!
 * The vertex of index byte i of a triangle: i * 16 + the vertex table (paddw), aligned down (lq).
 * kAligned: the vertex table is 16-byte aligned (always, for real shadow-geos), so that is vb
 * (the table's host address) + i * 16.
 */
template <bool kAligned>
inline const u8* tri_vert(Mem m, const u8* vb, u32 vtx, u32 i) {
  if (kAligned) {
    return vb + (i << 4);
  }
  return m.base + (((i << 4) + vtx) & ~15u);
}

//! the facing bit of the triangle whose 3 index bytes are at t
template <bool kAligned>
inline u32 tri_at_facing(Mem m, const u8* vb, u32 vtx, u32 t, const float l[4]) {
  return tri_facing(tri_vert<kAligned>(m, vb, vtx, m.ld<u8>(t)),
                    tri_vert<kAligned>(m, vb, vtx, m.ld<u8>(t + 1)),
                    tri_vert<kAligned>(m, vb, vtx, m.ld<u8>(t + 2)), l);
}

//! the facing bits of the 4 triangles in the quadword at a (aligned), shadow-vu0-block 0
template <bool kAligned>
inline u32 group_facing(Mem m, const u8* vb, u32 vtx, u32 a, const float l[4]) {
  return tri_at_facing<kAligned>(m, vb, vtx, a, l) |
         (tri_at_facing<kAligned>(m, vb, vtx, a + 4, l) << 1) |
         (tri_at_facing<kAligned>(m, vb, vtx, a + 8, l) << 2) |
         (tri_at_facing<kAligned>(m, vb, vtx, a + 12, l) << 3);
}

/*!
 * shadow-find-facing-single-tris (kList: the addresses of the facing triangles go to the list at
 * out) and shadow-find-facing-double-tris: faces = 1 for the triangles that face the light. With
 * 4 or more triangles, in groups of 4 that end past the table when the count isn't a multiple of
 * 4 (what is there is tested, and marked, like triangles), the next group loaded before this
 * one's marks are written; else one at a time. Returns the end of the list.
 */
template <bool kList, bool kAligned>
u32 facing_tris_impl(Mem m, u32 tri, s32 count, u32 vtx, const float l[4], u32 out) {
  const u8* const vb = m.base + vtx;
  if (count - 4 >= 0) {
    s32 left = count;
    u32 bits = group_facing<kAligned>(m, vb, vtx, tri & ~15u, l);
    for (;;) {
      left -= 4;
      const u32 next = left > 0 ? group_facing<kAligned>(m, vb, vtx, (tri + 16) & ~15u, l) : 0;
      for (int k = 0; k < 4; k++, tri += 4) {
        if (bits & (1u << k)) {
          m.st<u8>(tri + 3, 1);
          if (kList) {
            m.st<u32>(out, tri);
            out += 4;
          }
        }
      }
      if (left <= 0) {
        break;
      }
      bits = next;
    }
  } else {
    for (s32 left = count; left > 0; left--, tri += 4) {
      if (tri_at_facing<kAligned>(m, vb, vtx, tri, l)) {
        if (kList) {
          m.st<u32>(out, tri);
          out += 4;
        }
        m.st<u8>(tri + 3, 1);
      }
    }
  }
  return out;
}

template <bool kList>
u32 facing_tris(Mem m, u32 tri, s32 count, u32 vtx, const float l[4], u32 out) {
  if (vtx & 15) {
    return facing_tris_impl<kList, false>(m, tri, count, vtx, l, out);
  }
  return facing_tris_impl<kList, true>(m, tri, count, vtx, l, out);
}

//! shadow-find-facing-single-tris: the list of the facing ones at dcache-top
void find_facing_single_tris(Mem m, u32 hdr, u32 dc) {
  const u32 list = m.ld<u32>(dc + 16);
  const s32 count = m.ld<s16>(hdr + 12);
  const u32 tri = hdr + m.ld<u32>(hdr + 28);
  float l[4];
  m.qa(l, dc + 144);
  const u32 vtx = m.ld<u32>(dc);
  const u32 end = facing_tris<true>(m, tri, count, vtx, l, list);
  m.st<u32>(dc + 20, (u32)((s32)(end - list) >> 2));
  m.st<u32>(dc + 32, list);
  m.st<u32>(dc + 16, end);
}

//! shadow-find-facing-double-tris
void find_facing_double_tris(Mem m, u32 hdr, u32 dc) {
  const s32 count = m.ld<s16>(hdr + 16);
  const u32 tri = hdr + m.ld<u32>(hdr + 36);
  m.st<u32>(dc + 12, tri);
  float l[4];
  m.qa(l, dc + 144);
  const u32 vtx = m.ld<u32>(dc);
  facing_tris<false>(m, tri, count, vtx, l, 0);
}

/*!
 * shadow-find-single-edges: the silhouette edges of the single triangles: edges between a
 * facing and a non facing triangle (flag 1 when tri-0 isn't facing), and open edges (tri-1 255)
 * of a facing triangle (flag 0). The list entries: the edge's offset in the table, the flag
 * (16 bits each).
 */
void find_single_edges(Mem m, u32 hdr, u32 dc) {
  const u32 start = m.ld<u32>(dc + 16);
  const s64 count = m.ld<s16>(hdr + 14);
  u32 out = start;
  if (count != 0) {
    const u32 edges = hdr + m.ld<u32>(hdr + 32);
    const u32 tris = hdr + m.ld<u32>(hdr + 28);
    m.st<u32>(dc + 36, start);
    m.st<u32>(dc + 4, edges);
    u32 e = edges;
    for (u32 k = (u32)count; k != 0; k--, e += 4) {
      const u32 t1 = m.ld<u8>(e + 3), t0 = m.ld<u8>(e + 2);
      u32 flag;
      if (t1 == 255) {
        if (m.ld<u8>(tris + 4 * t0 + 3) == 0) {
          continue;
        }
        flag = 0;
      } else {
        const u32 f0 = m.ld<u8>(tris + 4 * t0 + 3), f1 = m.ld<u8>(tris + 4 * t1 + 3);
        if (f0 == f1) {
          continue;
        }
        flag = f0 == 0;
      }
      m.st<u16>(out + 2, (u16)flag);
      m.st<u16>(out, (u16)(e - edges));
      out += 4;
    }
  }
  m.st<u32>(dc + 24, (u32)((s32)(out - start) >> 2));
  m.st<u32>(dc + 16, out);
}

/*!
 * shadow-find-double-edges: edges between a facing and a non facing double triangle get two
 * entries (flags: tri-0 not facing, tri-1 facing), open edges one (flag: tri-0 not facing).
 */
void find_double_edges(Mem m, u32 hdr, u32 dc) {
  const u32 start = m.ld<u32>(dc + 16);
  const s64 count = m.ld<s16>(hdr + 18);
  const u32 tris = m.ld<u32>(dc + 12);
  u32 out = start;
  if (count != 0) {
    const u32 edges = hdr + m.ld<u32>(hdr + 40);
    m.st<u32>(dc + 40, start);
    m.st<u32>(dc + 8, edges);
    u32 e = edges;
    for (u32 k = (u32)count; k != 0; k--, e += 4) {
      const u32 t1 = m.ld<u8>(e + 3), t0 = m.ld<u8>(e + 2);
      if (t1 == 255) {
        m.st<u16>(out + 2, (u16)(m.ld<u8>(tris + 4 * t0 + 3) == 0));
        m.st<u16>(out, (u16)(e - edges));
        out += 4;
      } else {
        const u32 f0 = m.ld<u8>(tris + 4 * t0 + 3), f1 = m.ld<u8>(tris + 4 * t1 + 3);
        if (f0 == f1) {
          continue;
        }
        const u16 off = (u16)(e - edges);
        m.st<u16>(out + 2, (u16)(f0 == 0));
        m.st<u16>(out + 6, (u16)(f1 != 0));
        m.st<u16>(out, off);
        m.st<u16>(out + 4, off);
        out += 8;
      }
    }
  }
  m.st<u32>(dc + 28, (u32)((s32)(out - start) >> 2));
  m.st<u32>(dc + 16, out);
}

/*!
 * shadow-add-verts: the vertices (unpack to VU1 address 4) and, unless verts_only (a3, settings
 * flag 64), the duals (to 174), each after a copy of *shadow-data*'s dma-unpack-template (cnt with
 * qwc and num = the vertex count, the duals' vif0 a nop instead of flusha).
 */
u64 add_verts(Mem m, u32 hdr, u32 dc, u32 data, u64 out, bool verts_only) {
  const s64 count = m.ld<s16>(hdr + 8);
  u32 tmpl[4];
  m.q(tmpl, data);
  const u32 o = (u32)out;
  m.stq(o, tmpl);
  m.st<u16>(o, (u16)count);
  m.st<u8>(o + 14, (u8)count);
  m.st<u16>(o + 12, 4);
  const s64 bytes = (s32)((u32)count << 4);  // sll
  const u32 n = count > 0 ? 16 * (u32)count : 0;
  m.copy((o + 16) & ~15u, m.ld<u32>(dc) & ~15u, n);
  const u64 out2 = out + 16 + (u64)bytes;
  if (verts_only) {
    return out2;
  }
  const u32 o2 = (u32)out2;
  m.stq(o2, tmpl);
  m.st<u16>(o2, (u16)count);
  m.st<u8>(o2 + 14, (u8)count);
  m.st<u32>(o2 + 8, 0);
  m.st<u16>(o2 + 12, 174);
  m.copy((o2 + 16) & ~15u, m.ld<u32>(dc + 44) & ~15u, n);
  return out2 + 16 + (u64)bytes;
}

/*!
 * The cnt packet of a list for VU1: dma tag *shadow-data* dma-cnt + qwc, vif0 0, vif1
 * vif-unpack-v4-8 with num = words and immediate unpack (unsigned, VU1 address), then the words
 * (count first, or-ed with first_or, rounded up to a quadword, the last quadword zeroed first),
 * then a quadword of nops and mscalf. fill(o) writes the count entries from o. Returns the next DMA address.
 */
template <typename Fill>
u64 add_list(Mem m,
             s64 count,
             u32 data,
             u64 out,
             u16 unpack,
             u32 mscalf,
             Fill fill,
             u32 first_or = 0) {
  const s64 q = (count + 1 + 3) >> 2;
  const s64 words = q << 2;
  const u32 o = (u32)out;
  m.st<u64>(o, m.ld<u64>(data + 16) + (u64)(q + 1));
  m.st<u32>(o + 8, 0);
  m.st<u32>(o + 12, m.ld<u32>(data + 28));
  m.st<u8>(o + 14, (u8)words);
  m.st<u16>(o + 12, unpack);
  const u64 end = out + 16 + (u64)(words << 2);
  m.zeroq((u32)end - 16);
  m.st<u32>(o + 16, (u32)count | first_or);
  fill(o + 20);
  m.zeroq((u32)end);
  m.st<u32>((u32)end + 12, mscalf);
  return end + 16;
}

//! (the lists' loops are do-whiles: at least one entry, even for a negative count)
inline u32 list_entries(s64 count) {
  return count > 0 ? (u32)count : 1;
}

//! shadow-add-facing-single-tris: the facing triangles' words (to 344), mscalf 2
u64 add_facing_single_tris(Mem m, u32 dc, u32 data, u64 out) {
  const s64 count = m.ld<s32>(dc + 20);
  const u32 list = m.ld<u32>(dc + 32);
  if (count == 0) {
    return out;
  }
  const u32 n = list_entries(count);
  return add_list(m, count, data, out, 0x4158, 0x15000002, [=](u32 o) {
    for (u32 k = 0; k < n; k++) {
      m.st<u32>(o + 4 * k, m.ld<u32>(m.ld<u32>(list + 4 * k)));
    }
  });
}

/*!
 * shadow-add-single-edges / -double-edges: the edges' vertex indices and flags (to 600), mscalf
 * 4. A list entry is the edge's offset in the table (lh) and the flag; the output word is the
 * edge's first 16 bits (lhu: ind-0 ind-1) and the flag.
 */
u64 add_edges(Mem m, s64 count, u32 list, u32 table, u32 data, u64 out) {
  if (count == 0) {
    return out;
  }
  const u32 n = list_entries(count);
  return add_list(m, count, data, out, 0x4258, 0x15000004, [=](u32 o) {
    for (u32 k = 0; k < n; k++) {
      const u32 e = m.ld<u32>(list + 4 * k);
      const u16 inds = m.ld<u16, 1>(table + (u32)(s32)(s16)(e & 0xffff));
      m.st<u32>(o + 4 * k, (e & 0xffff0000u) | inds);
    }
  });
}

//! shadow-add-double-tris: all the double triangles' words (to 344), mscalf 6
u64 add_double_tris(Mem m, u32 hdr, u32 dc, u32 data, u64 out) {
  const s64 count = m.ld<s16>(hdr + 16);
  const u32 tris = m.ld<u32>(dc + 12);
  if (count == 0) {
    return out;
  }
  const u32 n = list_entries(count);
  return add_list(m, count, data, out, 0x4158, 0x15000006, [=](u32 o) { m.copy(o, tris, 4 * n); });
}

/*!
 * shadow-add-single-tris (settings flag 64): all the single triangles' words (to 344), the count
 * word with 256 set, mscalf 6
 */
u64 add_single_tris(Mem m, u32 hdr, u32 data, u64 out) {
  const s64 count = m.ld<s16>(hdr + 12);
  const u32 tris = hdr + m.ld<u32>(hdr + 28);
  if (count == 0) {
    return out;
  }
  const u32 n = list_entries(count);
  return add_list(
      m, count, data, out, 0x4158, 0x15000006, [=](u32 o) { m.copy(o, tris, 4 * n); }, 256);
}

/*!
 * (shadow-execute packet dma-ptr) -> dma-ptr after the shadows: for each shadow-dma-packet of the
 * chain (tag vif1: the next one, 0 at the end), the shadow volume's VU1 data (see the top). A
 * packet with settings flag 32 (disable-draw) adds nothing, with flag 1 none either when the
 * camera (camera-pos) is under the bottom plane. Flag 4: the planes as they are (else their d
 * relative to the center's y), flag 8: clip to the top plane, flag 64: only the vertices and the
 * single triangles.
 */
u64 shadow_execute_impl(const NativeArgs& args) {
  static const u32 spad_sym = sym_val_addr("*fake-scratchpad-data*");
  static const u32 gsf_sym = sym_val_addr("*gsf-buffer*");
  static const u32 cam_sym = sym_val_addr("*math-camera*");
  static const u32 debug_sym = sym_val_addr("*shadow-debug*");
  static const u32 camera_pos_sym = sym_val_addr("camera-pos");
  static const u32 debug_draw_sym = sym_val_addr("debug-draw-settings");
  static const u32 flush_cache_sym = sym_val_addr("flush-cache");
  static const u32 data_sym = sym_val_addr("*shadow-data*");
  const Mem m;
  const u32 st = (u32)args.st;
  {
    // (flush-cache 0): nothing on the PC port, called like the original does
    const u64 a[8] = {0,         args.a[1], args.a[2], args.a[3],
                      args.a[4], args.a[5], args.a[6], args.a[7]};
    native_call_goal(m.ld<u32>(flush_cache_sym), a, args);
  }
  u64 out = args.a[1];
  u64 packet = args.a[0];
  while (packet != 0) {
    dma_chain_copy(m, m.ld<u32>(spad_sym) + 48, (u32)packet);
    const u32 spad = m.ld<u32>(spad_sym);
    const u32 set = spad + 64;   // the packet's shadow-settings
    const u32 hdr = spad + 160;  // the shadow-geo fragment's header
    const u32 dc = m.ld<u32>(gsf_sym);
    // frag-qwc: the packet tag's vif0
    m.st<u32>(dc + 68, m.ld<u32>(spad + 56));
    // (the settings' flags are read again for each test, like the original)
    if (!(m.ld<u32>(set + 12) & 32)) {
      if (m.ld<u32>(debug_sym) != st) {
        const u64 a[8] = {set,       set + 16,  args.a[2], args.a[3],
                          args.a[4], args.a[5], args.a[6], args.a[7]};
        native_call_goal(m.ld<u32>(debug_draw_sym), a, args);
      }
      // the light locus: shadow-dir * dist-to-locus + center (vmulax, vmaddw), w = 1
      {
        float dir[4], ctr[4], loc[4];
        m.qa(dir, set + 16);
        m.qa(ctr, set);
        const float dist = m.f(set + 28);
        for (int i = 0; i < 3; i++) {
          const float acc = dir[i] * dist;
          loc[i] = acc + ctr[i] * 1.f;
        }
        loc[3] = 1.f + 0.f;
        m.stqa(dc + 80, loc);
      }
      if (m.ld<u32>(set + 12) & 4) {
        m.copyq(dc + 96, set + 32);
        m.copyq(dc + 112, set + 48);
      } else {
        for (int i = 0; i < 3; i++) {
          m.st<float>(dc + 96 + 4 * i, m.f(set + 32 + 4 * i));
        }
        m.st<float>(dc + 108, m.f(set + 44) - m.f(set + 4));
        for (int i = 0; i < 3; i++) {
          m.st<float>(dc + 112 + 4 * i, m.f(set + 48 + 4 * i));
        }
        m.st<float>(dc + 124, m.f(set + 60) - m.f(set + 4));
      }
      bool camera_below = false;
      if (m.ld<u32>(set + 12) & 1) {
        // (camera-pos) takes no arguments (these are what the original's registers hold)
        const u64 a[8] = {f2gpr(m.f(set + 28)), set + 16,  args.a[2], args.a[3],
                          args.a[4],            args.a[5], args.a[6], args.a[7]};
        const u32 pos = (u32)native_call_goal(m.ld<u32>(camera_pos_sym), a, args);
        const float f0 = m.f(pos), f1 = m.f(pos + 4), f2 = m.f(pos + 8);
        const float f3 = m.f(dc + 96), f4 = m.f(dc + 100), f5 = m.f(dc + 104);
        // mula.s / madda.s / madd.s, as the mips2c code writes them
        const float dot = (f2 * f5) + (f1 * f4) + (f0 * f3);
        camera_below = dot < u2f(f2u(m.f(dc + 108)) ^ 0x80000000u);
      }
      if (!camera_below) {
        float dot_p, dot_t;
        {
          const float f0 = m.f(dc + 80), f1 = m.f(dc + 84), f2 = m.f(dc + 88);
          const float f3 = m.f(dc + 96), f4 = m.f(dc + 100), f5 = m.f(dc + 104);
          dot_p = (f2 * f5) + (f1 * f4) + (f0 * f3);
        }
        {
          const float f1 = m.f(dc + 80), f2 = m.f(dc + 84), f3 = m.f(dc + 88);
          const float f4 = m.f(dc + 112), f5 = m.f(dc + 116), f6 = m.f(dc + 120);
          dot_t = (f3 * f6) + (f2 * f5) + (f1 * f4);
        }
        // the locus between the planes: the one on the side of shadow-dir goes through it
        if (m.f(dc + 108) < dot_p && dot_t < m.f(dc + 124)) {
          if (0.f < m.f(set + 28)) {
            m.st<u32>(dc + 108, f2u(dot_p) ^ 0x80000000u);  // neg.s
          } else {
            m.st<u32>(dc + 124, f2u(dot_t) ^ 0x80000000u);
          }
        }
        m.copyq(dc + 144, set + 16);
        m.st<u32>(dc + 128, 0);
        m.st<u32>(dc + 132, 0);
        m.st<float>(dc + 136, 1.f);
        m.st<float>(dc + 140, -2.f * m.f(m.ld<u32>(cam_sym)));
        m.st<u32>(dc + 16, dc + 176);
        m.st<u16>(dc + 66, (u16)(m.ld<u16>(dc + 66) + 1));  // stats num-objects

        if (m.ld<u32>(set + 12) & 64) {
          xform_verts(m, hdr, dc);
          init_vars(m, dc, m.ld<u32>(cam_sym));
          out = add_verts(m, hdr, dc, m.ld<u32>(data_sym), out, true);
          out = add_single_tris(m, hdr, m.ld<u32>(data_sym), out);
        } else {
          xform_verts(m, hdr, dc);
          init_vars(m, dc, m.ld<u32>(cam_sym));
          StaleRegs regs;
          calc_dual_verts(m, hdr, dc, &regs);
          if (m.ld<u32>(set + 12) & 8) {
            scissor_top(m, hdr, dc);
          }
          scissor_edges(m, hdr, dc, m.ld<u32>(spad_sym), regs);
          find_facing_single_tris(m, hdr, dc);
          find_single_edges(m, hdr, dc);
          find_facing_double_tris(m, hdr, dc);
          find_double_edges(m, hdr, dc);
          out = add_verts(m, hdr, dc, m.ld<u32>(data_sym), out, false);
          out = add_facing_single_tris(m, dc, m.ld<u32>(data_sym), out);
          out = add_edges(m, m.ld<s32>(dc + 24), m.ld<u32>(dc + 36), m.ld<u32>(dc + 4),
                          m.ld<u32>(data_sym), out);
          out = add_double_tris(m, hdr, dc, m.ld<u32>(data_sym), out);
          out = add_edges(m, m.ld<s32>(dc + 28), m.ld<u32>(dc + 40), m.ld<u32>(dc + 8),
                          m.ld<u32>(data_sym), out);
          // the stats
          m.st<u32>(dc + 48, m.ld<u32>(dc + 48) + m.ld<u32>(dc + 20));
          m.st<u32>(dc + 52, m.ld<u32>(dc + 52) + m.ld<u16>(hdr + 16));
          m.st<u32>(dc + 56, m.ld<u32>(dc + 56) + m.ld<u32>(dc + 24));
          m.st<u32>(dc + 60, m.ld<u32>(dc + 60) + m.ld<u32>(dc + 28));
        }
      }
    }
    packet = (u64)(s64)m.ld<s32>(spad + 60);  // the packet's tag vif1: the next packet
  }
  return out;
}

}  // namespace

const NativeImpl shadow_execute = MIPS2C_NATIVE_IMPL(shadow_execute_impl, 0, 0);

}  // namespace Mips2C::jak2::native
