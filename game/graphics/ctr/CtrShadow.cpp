/*!
 * @file CtrShadow.cpp
 * (AI-assisted)
 * See CtrShadow.h.
 */

#include "CtrShadow.h"

#include "game/graphics/ctr/CtrKeiraRig.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "common/dma/dma.h"
#include "common/log/log.h"

#include "game/graphics/ctr/CtrSettings.h"

namespace {
// VU1 data memory (shadow-vu1.gc, Shadow_PS2.cpp), in quadwords
constexpr u16 kVuMatrix = 0;
constexpr u16 kVuTop = 4;
constexpr u16 kVuDual = 174;
constexpr u16 kVuConstants = 0x370;
constexpr int kTableSize = 170;  // vertices per table (4..173, 174..343)
constexpr int kNumConstants = 13;
constexpr u16 kInitProgram = 10;  // MSCALF 10: the program's init (constants to registers)

inline u32 float_bits(float f) {
  u32 u;
  memcpy(&u, &f, 4);
  return u;
}

/*!
 * The program's facing test (Shadow_PS2.cpp, e.g. L3): e1 = v1 - v0, e2 = v2 - v0, the cross
 * product as opmula / opmsub compute it, times v0, then x + y and + z. Front (color clrs[1]) when
 * that is < 0. The same float operations in the same order (the 3DS's VFP doesn't fuse multiply-
 * adds; the host test builds with -ffp-contract=off), so the classes are the emulated program's.
 * The sign is tested on the bits (d < 0: not -0, not NaN) instead of with VCMP + VMRS, which stall
 * the VFP pipeline.
 */
inline u32 vu_front(const float* v0, const float* v1, const float* v2) {
  const float e1x = v1[0] - v0[0], e1y = v1[1] - v0[1], e1z = v1[2] - v0[2];
  const float e2x = v2[0] - v0[0], e2y = v2[1] - v0[1], e2z = v2[2] - v0[2];
  const float cx = e1y * e2z - e2y * e1z;
  const float cy = e1z * e2x - e2z * e1x;
  const float cz = e1x * e2y - e2x * e1y;
  const float d = (cx * v0[0] + cy * v0[1]) + cz * v0[2];
  return float_bits(d) - 0x80000001u < 0x7f800000u;
}

// camera space coordinates beyond this (24 km) are bad data, NaN too: with them and the clip
// matrix's entries under kMaxMatrix, clip space stays far from the GPU's float24 limit (~1e19)
constexpr float kMaxCamera = 1e8f;
constexpr float kMaxMatrix = 1e6f;
// the guard band (clip space |x|, |y| <= kGuard * w): a triangle with a corner beyond it is cut
// to it on the CPU, as the VU1 program clips to its own. The PICA drops a triangle whose corners
// are far off screen, and a dropped face leaves its stencil count wherever it covers. 1: the
// screen itself, so what the PICA does with a corner off screen never decides a count.
constexpr float kGuard = 1.f;

inline u32 sign_bit(float f) {
  return float_bits(f) >> 31;
}

//! the planes v is outside of: bits 0-3 the guard band (+x, -x, +y, -y), 4 near, 5 far (the
//! planes a face is cut to), bits 8-11 the screen's edges (+x, -x, +y, -y)
constexpr u32 kCutPlanes = 0x3f;
inline u16 outcode(float x, float y, float z, float w) {
  const float g = kGuard * w;
  return (u16)(sign_bit(g - x) | (sign_bit(g + x) << 1) | (sign_bit(g - y) << 2) |
               (sign_bit(g + y) << 3) | (sign_bit(w - z) << 4) | (sign_bit(w + z) << 5) |
               (sign_bit(w - x) << 8) | (sign_bit(w + x) << 9) | (sign_bit(w - y) << 10) |
               (sign_bit(w + y) << 11));
}

/*!
 * n vertices (x, y, z of each quadword) to clip space with the row-major matrix mp, their
 * outcodes to oc, and the largest |x|, |y|, |z| of the input (bits) to big. The matrix is in
 * locals: in registers for the whole loop (through the pointer, every store to dst would reload
 * it).
 */
void transform(const float* src, int n, const float* mp, ctr_clip_vertex* dst, u16* oc, u32& big) {
  const float m0 = mp[0], m1 = mp[1], m2 = mp[2], m3 = mp[3], m4 = mp[4], m5 = mp[5], m6 = mp[6],
              m7 = mp[7], m8 = mp[8], m9 = mp[9], m10 = mp[10], m11 = mp[11], m12 = mp[12],
              m13 = mp[13], m14 = mp[14], m15 = mp[15];
  u32 b = big;
  for (int i = 0; i < n; i++, src += 4, dst++) {
    const float x = src[0], y = src[1], z = src[2];
    u32 bits[3];
    memcpy(bits, src, 12);
    b = std::max(b, bits[0] & 0x7fffffffu);
    b = std::max(b, bits[1] & 0x7fffffffu);
    b = std::max(b, bits[2] & 0x7fffffffu);
    const float cx = m0 * x + m1 * y + m2 * z + m3;
    const float cy = m4 * x + m5 * y + m6 * z + m7;
    const float cz = m8 * x + m9 * y + m10 * z + m11;
    const float cw = m12 * x + m13 * y + m14 * z + m15;
    dst->x = cx;
    dst->y = cy;
    dst->z = cz;
    dst->w = cw;
    dst->s = 0.f;
    dst->t = 0.f;
    memset(&dst->r, 0, 4);
    oc[i] = outcode(cx, cy, cz, cw);
  }
  big = b;
}

//! the distance of clip space point c (x, y, z, w) to plane k of outcode (>= 0 inside)
inline float plane_dist(const float* c, int k) {
  // m * w - c (k even) or m * w + c (k odd), c = x, y, z for k / 2 = 0, 1, 2
  const float m = k < 4 ? kGuard : 1.f;
  return (k & 1) ? m * c[3] + c[k >> 1] : m * c[3] - c[k >> 1];
}

//! the point where an edge from inside point vi (distance di) to outside point vo (dout) meets
//! the plane, interpolated from its inside end (two faces sharing the edge get the same point)
inline void cut_point(const float* vi, const float* vo, float di, float dout, float* q) {
  const float s = di / (di - dout);
  for (int c = 0; c < 4; c++) {
    q[c] = vi[c] + (vo[c] - vi[c]) * s;
  }
}

// GIF tag of the shadow color's sprite (shadow-dma-end): NLOOP 1, REGLIST, NREG 2: PRIM, RGBAQ
bool is_color_packet(const DmaTransfer& t, u8 rgb[3]) {
  if (t.size_bytes < 32) {
    return false;
  }
  u64 tag, regs, prim;
  memcpy(&tag, t.data, 8);
  memcpy(&regs, t.data + 8, 8);
  memcpy(&prim, t.data + 16, 8);
  if ((tag & 0x7fff) != 1 || ((tag >> 58) & 3) != 1 || (tag >> 60) != 2 || (regs & 0xff) != 0x10 ||
      (prim & 7) != 6) {
    return false;
  }
  memcpy(rgb, t.data + 24, 3);
  return true;
}
}  // namespace

// ---------------------------------------------------------------------------------------------
// CtrShadowVolumes
// ---------------------------------------------------------------------------------------------

void CtrShadowVolumes::begin(ctr_clip_vertex* verts, int vert_cap, u16* front, u16* back,
                             int index_cap) {
  m_out = verts;
  m_front = front;
  m_back = back;
  m_vcap = std::min(vert_cap, 65536);
  m_icap = index_cap;
  m_nv = m_nf = m_nb = 0;
  m_obj_open = false;
  m_top = m_dual = nullptr;
  m_ntop = m_ndual = 0;
  m_have_clip = false;
  m_stats = Stats();
  m_keep = nullptr;
  m_keep_n = 0;
  m_obj_index = 0;
}

void CtrShadowVolumes::set_output(ctr_clip_vertex* verts, u16* front) {
  m_out = verts;
  m_front = front;
}

bool CtrShadowVolumes::starts_object(const DmaTransfer& t) {
  const VifCode v1 = t.vifcode1();
  return v1.kind == VifCode::Kind::UNPACK_V4_32 && VifCodeUnpack(v1).addr_qw == kVuTop;
}

float CtrShadowVolumes::object_score(const DmaTransfer& t, float center[3]) {
  const VifCode v1 = t.vifcode1();
  const int num = std::min(v1.num ? (int)v1.num : 256, kTableSize);
  if (t.size_bytes < (u32)num * 16 || num <= 0) {
    return 0.f;
  }
  // the vertex table is in camera space (game units, 4096 per meter)
  float lo[3] = {kMaxCamera, kMaxCamera, kMaxCamera}, hi[3] = {-kMaxCamera, -kMaxCamera, -kMaxCamera};
  const float* v = (const float*)t.data;
  for (int i = 0; i < num; i++, v += 4) {
    for (int c = 0; c < 3; c++) {
      if (!(v[c] < kMaxCamera && v[c] > -kMaxCamera)) {
        return 0.f;  // (NaN too) the object is left out later anyway
      }
      lo[c] = std::min(lo[c], v[c]);
      hi[c] = std::max(hi[c], v[c]);
    }
  }
  float r2 = 0.f, d2 = 0.f;
  for (int c = 0; c < 3; c++) {
    const float h = 0.5f * (hi[c] - lo[c]), m = 0.5f * (hi[c] + lo[c]);
    r2 += h * h;
    d2 += m * m;
    if (center) {
      center[c] = m;
    }
  }
  const float r = std::sqrt(r2);
  // (half a meter as the nearest distance: anything closer is as big as it gets)
  return (r + 1.f) / std::max(std::sqrt(d2) - r, 2048.f);
}

void CtrShadowVolumes::make_clip_matrix() {
  // the program: p = m0 * x + m1 * y + m2 * z + m3 (VU 0..3), screen = p.xyz * (pfog0 / p.w) +
  // hvdfoff, the GS's 12.4 x / y and 24-bit z. As OpenGL-style clip space for the 3DS (the GPU
  // divides): w = p.w / pfog0 (> 0 in front of the camera: the program's Q = pfog0 / p.w is), and
  // the mapping of CtrMercRenderer::handle_setup: x = (sx - 2048) / 256, y = -(sy - 2048) / 112
  // (Jak 1's 512 x 224 field), depth = sz / 2^24 (GS: larger = closer), as rows on p
  m_have_clip = false;
  if (!m_have_const || !m_have_matrix) {
    return;
  }
  const float* hvdf = m_const[4];
  const float pfog = m_const[5][0];
  if (!(pfog > 1e-20f || pfog < -1e-20f) || pfog != pfog) {
    return;
  }
  const float a = 1.f / pfog;
  for (int c = 0; c < 4; c++) {
    const float* col = m_matrix[c];  // p += col * (x, y, z, 1)[c]
    const float w = col[3] * a;
    m_clip[c] = (col[0] + (hvdf[0] - 2048.f) * w) * (1.f / 256.f);
    m_clip[4 + c] = -(col[1] + (hvdf[1] - 2048.f) * w) * (1.f / 112.f);
    m_clip[8 + c] = col[2] * (1.f / 8388608.f) + (hvdf[2] * (1.f / 8388608.f) - 1.f) * w;
    m_clip[12 + c] = w;
  }
  for (float f : m_clip) {
    if (!(f <= kMaxMatrix && f >= -kMaxMatrix)) {
      return;  // (the game's entries are under 10; NaN fails too)
    }
  }
  m_have_clip = true;
}

void CtrShadowVolumes::drop_object() {
  if (m_obj_transformed) {
    // take what it added back out (its vertices are the last ones)
    m_nv = m_obj_base;
    m_nf = m_obj_f0;
    m_nb = m_obj_b0;
  }
  m_obj_dropped = true;
  m_obj_any = false;
  m_stats.dropped_objects++;
}

bool CtrShadowVolumes::ensure_transformed() {
  if (m_obj_transformed || m_obj_dropped) {
    return !m_obj_dropped;
  }
  if (!m_have_clip) {
    m_stats.no_camera++;
    m_obj_dropped = true;  // (not counted as dropped: no camera at all)
    return false;
  }
  if (!m_obj_open || !m_out || !m_front || m_nv + m_ntop + m_ndual > m_vcap) {
    drop_object();
    return false;
  }
  u32 big = 0;
  transform(m_top, m_ntop, m_clip, m_out + m_nv, m_oc, big);
  if (m_dual) {
    transform(m_dual, m_ndual, m_clip, m_out + m_nv + m_ntop, m_oc + m_ntop, big);
  }
  if (big > float_bits(kMaxCamera)) {
    drop_object();  // NaN or huge numbers (m_nv not advanced yet: nothing to take back)
    return false;
  }
  m_obj_base = m_nv;
  m_obj_f0 = m_nf;
  m_obj_b0 = m_nb;
  m_nv += m_ntop + m_ndual;
  std::fill(m_dcut, m_dcut + std::min(m_ndual, kTableSize), -1);
  m_obj_transformed = true;
  return true;
}

void CtrShadowVolumes::keira_check_object() {
  m_keira_obj = false;
  const ctr_keira::ShadowFrame* f = ctr_keira::shadow_frame(m_frame);
  if (!f || m_ntop < 3) {
    return;
  }
#ifndef __3DS__
  // host A/B pictures: Jak's shadow mesh for her (env CTR_KEIRA_JAK_SHADOW)
  static const bool jak_shadow = getenv("CTR_KEIRA_JAK_SHADOW") != nullptr;
  if (jak_shadow) {
    return;
  }
#endif
  float c[3];
  ctr_keira::shadow_center(*f, c);
  double t[3] = {0, 0, 0};
  for (int i = 0; i < m_ntop; i++) {
    for (int k = 0; k < 3; k++) {
      t[k] += m_top[4 * i + k];
    }
  }
  float dp = 0, dm = 0;
  for (int k = 0; k < 3; k++) {
    const float m = (float)(t[k] / m_ntop);
    dp += (m - c[k]) * (m - c[k]);
    dm += (m + c[k]) * (m + c[k]);
  }
  constexpr float kMatch = 1.2f * 4096.f;  // Jak's shadow mesh's center near her body's
  if (std::min(dp, dm) < kMatch * kMatch) {
    m_keira_obj = true;
    m_keira_sign = dp <= dm ? 1.f : -1.f;
    m_obj_dropped = true;  // (not counted: replaced)
  }
}

void CtrShadowVolumes::keira_emit() {
  const ctr_keira::ShadowFrame* f = ctr_keira::shadow_frame(m_frame);
  if (!f || !m_top || !m_dual || m_keira_done) {
    return;
  }
  const int n = std::min(m_ntop, m_ndual);
  // the light (top -> bottom) and the bottom plane, from Jak's tables
  float L[3] = {0, 0, 0};
  int used = 0;
  for (int i = 0; i < n; i++) {
    float d[3];
    for (int k = 0; k < 3; k++) {
      d[k] = m_dual[4 * i + k] - m_top[4 * i + k];
    }
    const float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (l > 1.f) {
      for (int k = 0; k < 3; k++) {
        L[k] += d[k] / l;
      }
      used++;
    }
  }
  const float ll = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
  if (used < 3 || ll < 1e-3f) {
    return;
  }
  for (float& x : L) {
    x /= ll;
  }
  // three far apart points of the bottom table
  auto P = [&](int i) { return m_dual + 4 * i; };
  int a = 0, b = 0, c = 0;
  float best = -1;
  for (int i = 1; i < n; i++) {
    float d2 = 0;
    for (int k = 0; k < 3; k++) {
      d2 += (P(i)[k] - P(0)[k]) * (P(i)[k] - P(0)[k]);
    }
    if (d2 > best) {
      best = d2;
      b = i;
    }
  }
  float nrm[3] = {0, 0, 0};
  best = -1;
  for (int i = 1; i < n; i++) {
    const float u[3] = {P(b)[0] - P(a)[0], P(b)[1] - P(a)[1], P(b)[2] - P(a)[2]};
    const float v[3] = {P(i)[0] - P(a)[0], P(i)[1] - P(a)[1], P(i)[2] - P(a)[2]};
    const float x[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2],
                        u[0] * v[1] - u[1] * v[0]};
    const float l2 = x[0] * x[0] + x[1] * x[1] + x[2] * x[2];
    if (l2 > best) {
      best = l2;
      c = i;
      memcpy(nrm, x, sizeof(nrm));
    }
  }
  const float nl = std::sqrt(best);
  if (nl < 1.f) {
    return;  // (the bottom points on a line)
  }
  float plane[4];
  for (int k = 0; k < 3; k++) {
    plane[k] = nrm[k] / nl;
  }
  plane[3] = plane[0] * P(a)[0] + plane[1] * P(a)[1] + plane[2] * P(a)[2];
  (void)c;
  m_keira_done = true;
  if (!m_keira_replaced++) {
    lg::info("[ctr] shadow: Keira's hulls drawn for Jak's shadow ({} hulls, sign {}, light {:.2f} "
             "{:.2f} {:.2f})",
             f->mesh->nhulls, m_keira_sign, L[0], L[1], L[2]);
  }
  // her hulls, each an object of its own (its own tables)
  static ctr_keira::HullVolume hv;
  for (int h = 0; h < f->mesh->nhulls; h++) {
    if (!ctr_keira::hull_volume(*f, h, m_keira_sign, L, plane, &hv) || hv.nv < 4) {
      continue;
    }
    m_top = hv.T;
    m_dual = hv.D;
    m_ntop = m_ndual = hv.nv;
    m_obj_open = true;
    m_obj_transformed = m_obj_dropped = m_obj_any = false;
    if (!ensure_transformed()) {
      m_obj_open = false;
      continue;
    }
    if (m_nf + m_nb + 6 * (hv.ntris + hv.nedges) > m_icap) {
      drop_object();
      m_obj_open = false;
      break;
    }
    if (!tris<false>(hv.tris, hv.ntris, false, 6 * hv.nedges) ||
        !edges<false>(hv.edges, hv.nedges, 0)) {
      drop_object();
      m_obj_open = false;
      break;
    }
    m_obj_any = m_nf + m_nb > m_obj_f0 + m_obj_b0;
    close_object();
  }
  m_top = m_dual = nullptr;
  m_ntop = m_ndual = 0;
}

void CtrShadowVolumes::close_object() {
  if (m_keira_obj) {
    // (AI-assisted) Jak's (not drawn): hers instead
    m_keira_obj = false;
    m_obj_open = false;
    m_obj_any = false;
    keira_emit();
    return;
  }
  if (m_obj_open && m_obj_any) {
    m_stats.objects++;
  }
  m_obj_open = false;
  m_obj_any = false;
}

/*!
 * A triangle with a corner outside the guard band or the near / far plane (planes: the union of
 * its corners' outcodes), cut to them, as a fan at p: the corners that stay keep their vertices,
 * the new ones go after the frame's vertices. An edge cut in two triangles gives the same point
 * in both (interpolated from its inside end). rest: the indices budgeted for the primitives after
 * it. False: no room left (the object is left out).
 */
bool CtrShadowVolumes::clip_tri(u16*& p, const u16 idx[3], u32 planes, int rest) {
  struct V {
    float c[4];  // x, y, z, w
    int index;   // m_out index, -1: new
  };
  V buf[2][9];
  V* in = buf[0];
  V* out = buf[1];
  int n = 3;
  for (int k = 0; k < 3; k++) {
    const ctr_clip_vertex& v = m_out[idx[k]];
    in[k] = V{{v.x, v.y, v.z, v.w}, idx[k]};
  }
  for (u32 pl = planes; pl; pl &= pl - 1) {
    const int k = __builtin_ctz(pl);
    float d[9];
    for (int i = 0; i < n; i++) {
      d[i] = plane_dist(in[i].c, k);
    }
    int o = 0;
    for (int i = 0; i < n; i++) {
      const int j = i + 1 == n ? 0 : i + 1;
      const bool ina = d[i] >= 0, inb = d[j] >= 0;
      if (ina) {
        out[o++] = in[i];
      }
      if (ina != inb) {
        const V& vi = ina ? in[i] : in[j];
        const V& vo = ina ? in[j] : in[i];
        cut_point(vi.c, vo.c, ina ? d[i] : d[j], ina ? d[j] : d[i], out[o].c);
        out[o++].index = -1;
      }
    }
    n = o;
    if (n < 3) {
      return true;  // (nothing inside)
    }
    std::swap(in, out);
  }
  int fresh = 0;
  for (int i = 0; i < n; i++) {
    fresh += in[i].index < 0;
  }
  // (m_nf, m_nb: the lists' lengths before this triangle)
  if (m_nv + fresh > m_vcap || m_nf + m_nb + 3 * (n - 2) + rest > m_icap) {
    return false;
  }
  for (int i = 0; i < n; i++) {
    if (in[i].index < 0) {
      in[i].index = new_vertex(in[i].c);
    }
  }
  for (int i = 1; i + 1 < n; i++) {
    p[0] = (u16)in[0].index;
    p[1] = (u16)in[i].index;
    p[2] = (u16)in[i + 1].index;
    p += 3;
  }
  m_stats.clipped++;
  for (u32 pl = planes; pl; pl &= pl - 1) {
    m_stats.cut_planes[__builtin_ctz(pl)]++;
  }
  return true;
}

/*!
 * Bottom vertex i's edge from the top (T[i] inside the band, D[i] outside plane k alone) cut at
 * plane k, made once per object: its vertex, -2 if there is no room or the point is outside
 * another plane (then the faces are cut one by one).
 */
int CtrShadowVolumes::dual_cut(int i, int k) {
  int& c = m_dcut[i];
  if (c != -1) {
    return c;
  }
  c = -2;
  const ctr_clip_vertex& t = m_out[m_obj_base + i];
  const ctr_clip_vertex& d = m_out[m_obj_base + m_ntop + i];
  const float vt[4] = {t.x, t.y, t.z, t.w}, vd[4] = {d.x, d.y, d.z, d.w};
  float q[4];
  cut_point(vt, vd, plane_dist(vt, k), plane_dist(vd, k), q);
  if ((outcode(q[0], q[1], q[2], q[3]) & kCutPlanes & ~(1u << k)) || m_nv >= m_vcap) {
    return c;
  }
  c = new_vertex(q);
  return c;
}

int CtrShadowVolumes::new_vertex(const float c[4]) {
  ctr_clip_vertex& v = m_out[m_nv];
  v.x = c[0];
  v.y = c[1];
  v.z = c[2];
  v.w = c[3];
  v.s = v.t = 0.f;
  memset(&v.r, 0, 4);
  return m_nv++;
}

/*!
 * MSCAL 2 (facing single tris) and 6 (double-sided tris): count entries (i0, i1, i2, faces), the
 * top cap (T) for all of them, then the bottom cap (D) reversed (see CtrShadow.h). False: no room
 * for the triangles cut to the guard band. after: the indices budgeted for the lists after it.
 */
template <bool kTrace>
bool CtrShadowVolumes::tris(const u8* e, int count, bool double_sided, int after) {
  const int lim = std::min(m_ntop, m_dual ? m_ndual : 0);
  const float* T = m_top;
  const float* D = m_dual;
  const u16 tb = (u16)m_obj_base, db = (u16)(m_obj_base + m_ntop);
  u16* cur[2] = {m_back + m_nb, m_front + m_nf};  // by facing: 0 back, 1 front
  bool room = true;
  for (int cap = 0; cap < 2; cap++) {
    const float* V = cap ? D : T;
    const u16 base = cap ? db : tb;
    const u16* oc = m_oc + (cap ? m_ntop : 0);
    for (int k = 0; k < count; k++) {
      const u8* t = e + 4 * k;
      const int i0 = t[0];
      int i1 = t[1], i2 = t[2];
      if (i0 >= lim || i1 >= lim || i2 >= lim) {
        m_stats.bad_entries += cap == 0;
        continue;
      }
      // the program's vertex order: top (i0 i1 i2) / bottom (i0 i2 i1), double-sided with
      // faces == 0 the other way round
      const bool swap = (cap != 0) != (double_sided && t[3] == 0);
      if (swap) {
        std::swap(i1, i2);
      }
      const u32 o0 = oc[i0], o1 = oc[i1], o2 = oc[i2];
      if (!kTrace && (o0 & o1 & o2)) {
        continue;  // (off screen or outside a plane: no pixel)
      }
      const u32 f = vu_front(V + 4 * i0, V + 4 * i1, V + 4 * i2);
      const u16 idx[3] = {(u16)(base + i0), (u16)(base + i1), (u16)(base + i2)};
      if constexpr (kTrace) {
        m_trace->push_back(Prim{{idx[0], idx[1], idx[2], 0}, 3, (u8)f, tb});
      }
      const u32 any = o0 | o1 | o2;
      if (!any || (!(any & kCutPlanes) && !(o0 & o1 & o2))) {
        u16* p = cur[f];
        p[0] = idx[0];
        p[1] = idx[1];
        p[2] = idx[2];
        cur[f] = p + 3;
      } else if (!(o0 & o1 & o2) && room) {
        m_nb = (int)(cur[0] - m_back);
        m_nf = (int)(cur[1] - m_front);
        room = clip_tri(cur[f], idx, any & kCutPlanes,
                        after + 3 * ((1 - cap) * count + count - k - 1));
      }
    }
  }
  m_nb = (int)(cur[0] - m_back);
  m_nf = (int)(cur[1] - m_front);
  return room;
}

/*!
 * MSCAL 4 (silhouette edges): count entries (a, b, flip, 0), each the side quad between the caps.
 * False: no room for the triangles cut to the guard band. after: as in tris.
 */
template <bool kTrace>
bool CtrShadowVolumes::edges(const u8* e, int count, int after) {
  const int lim = std::min(m_ntop, m_dual ? m_ndual : 0);
  const float* T = m_top;
  const float* D = m_dual;
  const int tb = m_obj_base, db = m_obj_base + m_ntop;
  const u16* oct = m_oc;
  const u16* ocd = m_oc + m_ntop;
  u16* cur[2] = {m_back + m_nb, m_front + m_nf};
  bool room = true;
  for (int k = 0; k < count; k++) {
    const u8* t = e + 4 * k;
    int a = t[0], b = t[1];
    if (a >= lim || b >= lim) {
      m_stats.bad_entries++;
      continue;
    }
    if (t[2] == 0) {
      std::swap(a, b);  // (T[b], T[a], D[a], D[b])
    }
    const u32 o0 = oct[a], o1 = oct[b], o2 = ocd[b], o3 = ocd[a];
    if (!kTrace && (o0 & o1 & o2 & o3)) {
      continue;  // (off screen or outside a plane: no pixel)
    }
    const u32 f = vu_front(T + 4 * a, T + 4 * b, D + 4 * b);
    const u16 q0 = (u16)(tb + a), q1 = (u16)(tb + b), q2 = (u16)(db + b), q3 = (u16)(db + a);
    if constexpr (kTrace) {
      m_trace->push_back(Prim{{q0, q1, q2, q3}, 4, (u8)f, (u16)tb});
    }
    const u32 any4 = o0 | o1 | o2 | o3;
    if (!any4 || (!(any4 & kCutPlanes) && !(o0 & o1 & o2) && !(o0 & o2 & o3))) {
      u16* p = cur[f];
      p[0] = q0;
      p[1] = q1;
      p[2] = q2;
      p[3] = q0;
      p[4] = q2;
      p[5] = q3;
      cur[f] = p + 6;
      continue;
    }
    // the top edge inside the band and both bottom corners outside the same guard-band plane
    // alone: the quad is cut along its two side edges, at the points made once per bottom vertex
    // (the same pixels as its two triangles cut one by one: the part left out lies off screen)
    const u32 c2 = o2 & kCutPlanes;
    if (!((o0 | o1) & kCutPlanes) && c2 && c2 == (o3 & kCutPlanes) && !(c2 & (c2 - 1)) &&
        c2 < (1u << 4)) {
      const int k = __builtin_ctz(c2);
      const int ib = dual_cut(b, k), ia = ib >= 0 ? dual_cut(a, k) : -2;
      if (ia >= 0) {
        u16* p = cur[f];
        p[0] = q0;
        p[1] = q1;
        p[2] = (u16)ib;
        p[3] = q0;
        p[4] = (u16)ib;
        p[5] = (u16)ia;
        cur[f] = p + 6;
        m_stats.clipped += 2;
        m_stats.cut_planes[k] += 2;
        continue;
      }
    }
    // (q0 q1 q2), (q0 q2 q3), each whole, cut or left out
    const u16 tri[2][3] = {{q0, q1, q2}, {q0, q2, q3}};
    const u32 oc[2][3] = {{o0, o1, o2}, {o0, o2, o3}};
    for (int h = 0; h < 2; h++) {
      const u32 any = oc[h][0] | oc[h][1] | oc[h][2];
      if (oc[h][0] & oc[h][1] & oc[h][2]) {
        continue;  // (off screen or outside a plane: no pixel)
      }
      if (!(any & kCutPlanes)) {
        u16* p = cur[f];
        p[0] = tri[h][0];
        p[1] = tri[h][1];
        p[2] = tri[h][2];
        cur[f] = p + 3;
      } else if (room) {
        m_nb = (int)(cur[0] - m_back);
        m_nf = (int)(cur[1] - m_front);
        room =
            clip_tri(cur[f], tri[h], any & kCutPlanes, after + 6 * (count - k - 1) + 3 * (1 - h));
      }
    }
  }
  m_nb = (int)(cur[0] - m_back);
  m_nf = (int)(cur[1] - m_front);
  return room;
}

void CtrShadowVolumes::transfer(const DmaTransfer& t) {
  const VifCode v0 = t.vifcode0();
  const VifCode v1 = t.vifcode1();
  if (v1.kind == VifCode::Kind::UNPACK_V4_32) {
    const VifCodeUnpack up(v1);
    const int num = v1.num ? v1.num : 256;
    if (up.use_tops_flag || t.size_bytes < (u32)num * 16) {
      m_stats.bad_packets++;
      return;
    }
    const float* data = (const float*)t.data;
    if (up.addr_qw == kVuTop) {
      close_object();
      m_top = data;
      m_ntop = std::min(num, kTableSize);
      m_dual = nullptr;
      m_ndual = 0;
      m_obj_open = true;
      m_obj_transformed = m_obj_dropped = m_obj_any = false;
      // (AI-assisted) shadow_casters: not one of the biggest on screen
      const int k = m_obj_index++;
      if (m_keep && k < m_keep_n && !m_keep[k]) {
        m_obj_dropped = true;
        m_stats.capped++;
      }
      keira_check_object();
    } else if (up.addr_qw == kVuDual) {
      if (m_obj_open && !m_obj_transformed) {
        m_dual = data;
        m_ndual = std::min(num, kTableSize);
      }
    } else if (up.addr_qw == kVuConstants && num >= 6) {
      memcpy(m_const, data, (size_t)std::min(num, kNumConstants) * 16);
      m_have_const = true;
    } else if (up.addr_qw == kVuMatrix && num >= 4) {
      memcpy(m_matrix, data, 64);
      m_have_matrix = true;
    }
    return;
  }
  if (v0.kind == VifCode::Kind::MSCALF || v0.kind == VifCode::Kind::MSCAL) {
    if (v0.immediate == kInitProgram) {
      make_clip_matrix();
    }
    return;
  }
  if (v1.kind == VifCode::Kind::UNPACK_V4_8) {
    // the index list (count, then 4 bytes per entry) and in the next quadword the MSCAL
    const VifCodeUnpack up(v1);
    const u32 num = v1.num ? v1.num : 256;
    if (up.use_tops_flag || t.size_bytes < 4 * num + 16) {
      m_stats.bad_packets++;
      return;
    }
    u32 mscal_word;
    memcpy(&mscal_word, t.data + 4 * num + 12, 4);
    const VifCode mscal(mscal_word);
    const int count = std::min<int>(t.data[0], (int)num - 1);  // (the program reads x as u16)
    if (count < t.data[0]) {
      m_stats.bad_packets++;
    }
    const int entry = mscal.immediate;
    const bool tri_list = up.addr_qw == 0x158 && (entry == 2 || entry == 6);
    const bool edge_list = up.addr_qw == 0x258 && entry == 4;
    if ((mscal.kind != VifCode::Kind::MSCALF && mscal.kind != VifCode::Kind::MSCAL) ||
        !(tri_list || edge_list)) {
      m_stats.bad_packets++;
      return;
    }
    if (count <= 0 || !ensure_transformed()) {
      return;
    }
    // room for all of them in either list (front + back share the room)
    if (m_nf + m_nb + 6 * count > m_icap) {
      drop_object();
      return;
    }
    const u8* e = t.data + 4;
    const bool dbl = entry == 6;
    const bool room =
        edge_list ? (m_trace ? edges<true>(e, count, 0) : edges<false>(e, count, 0))
                  : (m_trace ? tris<true>(e, count, dbl, 0) : tris<false>(e, count, dbl, 0));
    if (!room) {
      drop_object();
      return;
    }
    m_obj_any |= m_nf + m_nb > m_obj_f0 + m_obj_b0;
    return;
  }
  if (v0.kind == VifCode::Kind::FLUSHA && v1.kind == VifCode::Kind::DIRECT) {
    is_color_packet(t, m_rgb);  // shadow-dma-end: the shadow color
  }
}

void CtrShadowVolumes::end() {
  close_object();
}

void CtrShadowVolumes::color(u8 out[3]) const {
  for (int i = 0; i < 3; i++) {
    out[i] = (u8)std::min(255, 2 * m_rgb[i]);
  }
}

// ---------------------------------------------------------------------------------------------
// CtrShadowRenderer
// ---------------------------------------------------------------------------------------------

CtrShadowRenderer::CtrShadowRenderer(std::string name, int id)
    : CtrBucketRenderer(std::move(name), id), m_back(CtrShadowVolumes::kMaxIndices) {}

void CtrShadowRenderer::render(DmaFollower& dma, CtrRenderState& rs) {
  if (dma.current_tag_offset() == rs.next_bucket) {
    return;
  }
  m_vol.begin(nullptr, CtrShadowVolumes::kMaxVerts, nullptr, m_back.data(),
              CtrShadowVolumes::kMaxIndices);
  m_vol.set_frame(rs.frame_idx);
  m_reserved = false;
  bool no_room = false;
  // (AI-assisted) config shadow_casters: only the N objects that look biggest from the camera
  // (nearest / largest); a first pass over the bucket's DMA scores each object's vertex table
  const int cap = ctr_settings().shadow_casters;
  if (cap > 0) {
    m_scores.clear();
    m_centers.clear();
    DmaFollower scan = dma;
    while (scan.current_tag_offset() != rs.next_bucket) {
      const auto t = scan.read_and_advance();
      if (CtrShadowVolumes::starts_object(t)) {
        std::array<float, 3> c = {0.f, 0.f, 0.f};
        float s = CtrShadowVolumes::object_score(t, c.data());
        // (AI-assisted) hysteresis: one drawn last frame (a center within ~1 m + 10% of its
        // distance of one of theirs: the camera and the object move a little per frame) keeps its
        // place unless another one is kIncumbent x bigger. Was chosen anew every frame, so objects
        // of about the same size at the cut swapped and their shadows blinked
        constexpr float kIncumbent = 1.25f;
        const float d2 = c[0] * c[0] + c[1] * c[1] + c[2] * c[2];
        const float tol2 = 4096.f * 4096.f + 0.01f * d2;
        for (const auto& k : m_kept_prev) {
          const float dx = k[0] - c[0], dy = k[1] - c[1], dz = k[2] - c[2];
          if (dx * dx + dy * dy + dz * dz < tol2) {
            s *= kIncumbent;
            break;
          }
        }
        m_scores.push_back(s);
        m_centers.push_back(c);
      }
    }
    const int n = (int)m_scores.size();
    m_kept_prev.clear();
    if (n > cap) {
      m_order.resize(n);
      for (int i = 0; i < n; i++) {
        m_order[i] = i;
      }
      // (AI-assisted) equal scores by DMA order: the same pick every frame
      std::nth_element(m_order.begin(), m_order.begin() + (cap - 1), m_order.end(), [&](int a, int b) {
        return m_scores[a] > m_scores[b] || (m_scores[a] == m_scores[b] && a < b);
      });
      m_keep.assign(n, 0);
      for (int i = 0; i < cap; i++) {
        m_keep[m_order[i]] = 1;
        m_kept_prev.push_back(m_centers[m_order[i]]);
      }
      m_vol.set_keep(m_keep.data(), n);
    } else {
      m_kept_prev = m_centers;
    }
  }
  while (dma.current_tag_offset() != rs.next_bucket) {
    const auto t = dma.read_and_advance();
    if (!m_reserved && !no_room && CtrShadowVolumes::starts_object(t)) {
      // this frame's vertex memory, from the first object on (nothing else is drawn until
      // draw()): the volumes are written straight into it
      m_reserved = ctr_gpu_clip_reserve(CtrShadowVolumes::kMaxVerts, CtrShadowVolumes::kMaxIndices,
                                        &m_verts, &m_indices) != 0;
      no_room = !m_reserved;
      if (m_reserved) {
        m_vol.set_output(m_verts, m_indices);
      }
    }
    m_vol.transfer(t);
  }
  m_vol.end();
  if (no_room) {
    // (the objects are left out: CtrShadowVolumes had no output)
    if (m_no_room++ % 300 == 0) {
      lg::warn("[ctr] shadow: no room in the frame's vertex memory, shadows not drawn ({} frames)",
               m_no_room);
    }
  }
  if (m_reserved) {
    draw();
  }
  if (rs.log_now) {
    const auto& st = m_vol.stats();
    lg::debug("[ctr] shadow: {} objects ({} left out, {} over shadow_casters), {} vertices, {} "
              "front + {} back triangles, {} cut to the guard band (planes +x {} -x {} +y {} -y {} "
              "near {} far {}); {} bad entries, {} bad packets, {} without camera; Keira's shadow "
              "drawn {} times",
              st.objects, st.dropped_objects, st.capped, m_vol.vert_count(), m_vol.front_count() / 3,
              m_vol.back_count() / 3, st.clipped, st.cut_planes[0], st.cut_planes[1],
              st.cut_planes[2], st.cut_planes[3], st.cut_planes[4], st.cut_planes[5],
              st.bad_entries, st.bad_packets, st.no_camera, m_vol.keira_replaced());
  }
}

void CtrShadowRenderer::draw() {
  const int nv = m_vol.vert_count(), nf = m_vol.front_count(), nb = m_vol.back_count();
  if (nf < 3 || nb < 3) {
    // no front faces: nothing counts up; no back faces: nothing behind one to darken
    ctr_gpu_clip_draw_ranges(nullptr, nullptr, nullptr, 0, 0, 0);
    return;
  }
  // the back list after the front list
  memcpy(m_indices + nf, m_back.data(), (size_t)nb * 2);
  // the darkening pass draws the back faces that lie behind the scene: a counted pixel has one
  // (each darkened once: it clears the count), and a count the back pass failed to take down
  // darkens nothing in front of the volume's far side
  u8 rgb[3];
  m_vol.color(rgb);
  const u32 rgba = rgb[0] | ((u32)rgb[1] << 8) | ((u32)rgb[2] << 16) | (0x80u << 24);
  for (int i = 0; i < nv; i++) {
    memcpy(&m_verts[i].r, &rgba, 4);
  }
  ctr_draw_state st[3];
  memset(st, 0, sizeof(st));
  for (auto& s : st) {
    s.tex = -1;
    s.blend = CTR_BLEND_OFF;
    s.atest = CTR_TEST_ALWAYS;
    s.ztest = CTR_TEST_GEQUAL;
    s.zwrite = 0;
  }
  st[0].flags = CTR_STATE_NO_COLOR | CTR_STATE_STENCIL_INCR;
  st[1].flags = CTR_STATE_NO_COLOR | CTR_STATE_STENCIL_DECR;
  st[2].blend = CTR_BLEND_MUL;
  st[2].ztest = CTR_TEST_LESS;
  st[2].flags = CTR_STATE_STENCIL_CLEAR;
  const int first[3] = {0, nf, nf};
  const int count[3] = {nf, nb, nb};
  ctr_gpu_clip_draw_ranges(st, first, count, 3, nv, nf + nb);
}

// ---------------------------------------------------------------------------------------------
// self-test
// ---------------------------------------------------------------------------------------------

namespace {
u32 vif_word(VifCode::Kind kind, u32 num, u32 imm) {
  return ((u32)kind << 24) | ((num & 0xff) << 16) | (imm & 0xffff);
}

// the shadow bucket's transfers for one prism, as shadow-execute makes them
struct TestDma {
  std::vector<std::vector<u8>> data;
  std::vector<DmaTransfer> list;
  void add(u32 vif0, u32 vif1, const void* src, u32 bytes) {
    data.emplace_back(bytes);
    if (bytes) {
      memcpy(data.back().data(), src, bytes);
    }
    DmaTransfer t;
    t.data = data.back().data();
    t.size_bytes = bytes;
    t.transferred_tag = (u64)vif0 | ((u64)vif1 << 32);
    list.push_back(t);
  }
  void add_list(const std::vector<std::array<u8, 4>>& entries, u32 mscal, u32 addr) {
    const u32 n = (u32)entries.size();
    const u32 num = ((n + 1 + 3) / 4) * 4;
    std::vector<u8> d(4 * num + 16, 0);
    memcpy(d.data(), &n, 4);
    memcpy(d.data() + 4, entries.data(), 4 * n);
    const u32 call = vif_word(VifCode::Kind::MSCALF, 0, mscal);
    memcpy(d.data() + 4 * num + 12, &call, 4);
    add(0, vif_word(VifCode::Kind::UNPACK_V4_8, num, 0x4000 | addr), d.data(), (u32)d.size());
  }
};

constexpr int kTestSides = 64;
constexpr float kTestNear = 0.25f, kTestFar = 1000.f;
constexpr float kTestA = (kTestNear + kTestFar) / (kTestNear - kTestFar);
constexpr float kTestB = kTestNear * (1.f - kTestA);

// camera space: x right, y up, z forward (meters); clip = (1.6 x, 2.67 y, A z + B, z)
void test_prism(TestDma& dma, const float bottom[3]) {
  u32 consts[13][4] = {};
  const float hvdf[4] = {2048.f, 2048.f, 8388608.f, 0.f}, pfog = 1.f;
  memcpy(consts[4], hvdf, 16);
  memcpy(consts[5], &pfog, 4);
  dma.add(vif_word(VifCode::Kind::STCYCL, 0, 0x404),
          vif_word(VifCode::Kind::UNPACK_V4_32, 13, kVuConstants), consts, sizeof(consts));
  const float matrix[4][4] = {{256.f * 1.6f, 0, 0, 0},
                              {0, -112.f * 2.67f, 0, 0},
                              {0, 0, 8388608.f * kTestA, 1.f},
                              {0, 0, 8388608.f * kTestB, 0}};
  dma.add(vif_word(VifCode::Kind::STCYCL, 0, 0x404),
          vif_word(VifCode::Kind::UNPACK_V4_32, 4, kVuMatrix), matrix, sizeof(matrix));
  dma.add(vif_word(VifCode::Kind::MSCALF, 0, kInitProgram), 0, nullptr, 0);

  const float top[3] = {0.3f, 0.6f, 6.f};
  const float axis[3] = {bottom[0] - top[0], bottom[1] - top[1], bottom[2] - top[2]};
  float T[kTestSides][4], D[kTestSides][4];
  for (int i = 0; i < kTestSides; i++) {
    const float a = 6.2831853f * (float)i / kTestSides;
    T[i][0] = top[0] + 0.5f * std::cos(a);
    T[i][1] = top[1];
    T[i][2] = top[2] + 0.35f * std::sin(a);
    T[i][3] = 1.f;
    for (int c = 0; c < 3; c++) {
      D[i][c] = T[i][c] + axis[c];
    }
    D[i][3] = 1.f;
  }
  dma.add(vif_word(VifCode::Kind::FLUSHA, 0, 0),
          vif_word(VifCode::Kind::UNPACK_V4_32, kTestSides, kVuTop), T, sizeof(T));
  dma.add(0, vif_word(VifCode::Kind::UNPACK_V4_32, kTestSides, kVuDual), D, sizeof(D));
  auto cross_dot = [](const float* o, const float* a, const float* b, const float* d) {
    const float e1[3] = {a[0] - o[0], a[1] - o[1], a[2] - o[2]};
    const float e2[3] = {b[0] - o[0], b[1] - o[1], b[2] - o[2]};
    return (e1[1] * e2[2] - e1[2] * e2[1]) * d[0] + (e1[2] * e2[0] - e1[0] * e2[2]) * d[1] +
           (e1[0] * e2[1] - e1[1] * e2[0]) * d[2];
  };
  // every face wound with its normal out of the prism
  std::vector<std::array<u8, 4>> tris, edges;
  const float up[3] = {-axis[0], -axis[1], -axis[2]};
  const bool cap_swap = cross_dot(T[0], T[1], T[2], up) < 0;
  for (int i = 1; i + 1 < kTestSides; i++) {
    tris.push_back({0, (u8)(cap_swap ? i + 1 : i), (u8)(cap_swap ? i : i + 1), 1});
  }
  for (int i = 0; i < kTestSides; i++) {
    const int j = (i + 1) % kTestSides;
    const float out[3] = {T[i][0] - top[0], 0.f, T[i][2] - top[2]};
    edges.push_back({(u8)i, (u8)j, (u8)(cross_dot(T[i], T[j], D[j], out) > 0), 0});
  }
  dma.add_list(tris, 2, 0x158);
  dma.add_list(edges, 4, 0x258);
}

struct TestResult {
  int dark = -1, left = -1;
};

// one frame: the receiver at distance receiver_z, the prism's three passes, then a quad over the
// screen where a count is left. uncut: the prism's faces as they are, for the GPU to clip
TestResult test_frame(const float bottom[3], float receiver_z, bool split, bool uncut) {
  static CtrShadowVolumes vol;
  static std::vector<u16> back(CtrShadowVolumes::kMaxIndices);
  std::vector<CtrShadowVolumes::Prim> prims;
  TestDma dma;
  test_prism(dma, bottom);
  ctr_gpu_set_skip_color_clear(0);
  ctr_gpu_frame_begin(0, 0, 0);
  ctr_draw_state rs;
  memset(&rs, 0, sizeof(rs));
  rs.tex = -1;
  rs.atest = CTR_TEST_ALWAYS;
  rs.ztest = CTR_TEST_ALWAYS;
  rs.zwrite = 1;
  const float depth = (kTestA + kTestB / receiver_z + 1.f) * 0.5f;
  ctr_vertex q[6];
  const float corner[6][2] = {{-1, -1}, {1, -1}, {-1, 1}, {-1, 1}, {1, -1}, {1, 1}};
  for (int i = 0; i < 6; i++) {
    q[i] = ctr_vertex{corner[i][0], corner[i][1], depth, 0.f, 0.f, 0, 0, 16, 0x80};
  }
  ctr_gpu_draw(&rs, q, 6);
  if (split) {
    ctr_gpu_submit_now();
  }
  ctr_clip_vertex* verts = nullptr;
  u16* indices = nullptr;
  const bool room = ctr_gpu_clip_reserve(CtrShadowVolumes::kMaxVerts, CtrShadowVolumes::kMaxIndices,
                                         &verts, &indices) != 0;
  if (room) {
    vol.begin(verts, CtrShadowVolumes::kMaxVerts - 4, indices, back.data(),
              CtrShadowVolumes::kMaxIndices - 6);
    vol.set_trace(uncut ? &prims : nullptr);
    for (const auto& t : dma.list) {
      vol.transfer(t);
    }
    vol.end();
    vol.set_trace(nullptr);
    const int nv = vol.vert_count();
    int nf = vol.front_count(), nb = vol.back_count();
    if (uncut) {
      nf = nb = 0;
      for (const auto& pr : prims) {
        u16* list = pr.front ? indices : back.data();
        int& n = pr.front ? nf : nb;
        for (int k = 0; k + 2 < pr.n; k++) {
          list[n++] = pr.v[0];
          list[n++] = pr.v[k + 1];
          list[n++] = pr.v[k + 2];
        }
      }
    }
    memcpy(indices + nf, back.data(), (size_t)nb * 2);
    for (int i = 0; i < nv; i++) {
      verts[i].r = verts[i].g = verts[i].b = 0x80;
      verts[i].a = 0x80;
    }
    static const float kQuad[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
    static const u16 kQuadIndex[6] = {0, 1, 2, 2, 1, 3};
    for (int i = 0; i < 4; i++) {
      verts[nv + i] = ctr_clip_vertex{kQuad[i][0], kQuad[i][1], 0.f, 1.f, 0.f, 0.f, 0x40, 0x40,
                                      0x40, 0x80};
    }
    for (int i = 0; i < 6; i++) {
      indices[nf + nb + i] = (u16)(nv + kQuadIndex[i]);
    }
    ctr_draw_state st[4];
    memset(st, 0, sizeof(st));
    for (auto& s : st) {
      s.tex = -1;
      s.blend = CTR_BLEND_OFF;
      s.atest = CTR_TEST_ALWAYS;
      s.ztest = CTR_TEST_GEQUAL;
    }
    st[0].flags = CTR_STATE_NO_COLOR | CTR_STATE_STENCIL_INCR;
    st[1].flags = CTR_STATE_NO_COLOR | CTR_STATE_STENCIL_DECR;
    st[2].blend = st[3].blend = CTR_BLEND_MUL;
    st[2].flags = st[3].flags = CTR_STATE_STENCIL_CLEAR;
    st[2].ztest = CTR_TEST_LESS;
    st[3].ztest = CTR_TEST_ALWAYS;
    const int first[4] = {0, nf, nf, nf + nb};
    const int count[4] = {nf, nb, nb, 6};
    ctr_gpu_clip_draw_ranges(st, first, count, 4, nv + 4, nf + nb + 6);
  }
  ctr_gpu_frame_end();
  // the receiver is blue 16 (next to black on the screen): 8 where darkened, 4 under the quad
  TestResult r;
  unsigned int blue[256];
  if (room && ctr_gpu_top_histogram(blue)) {
    r.dark = (int)(blue[7] + blue[8] + blue[9]);
    r.left = (int)(blue[3] + blue[4] + blue[5]);
  }
  return r;
}
}  // namespace

void ctr_shadow_selftest() {
  // the receiver darkened inside the prism on the host's rasterizer
  constexpr int kHitPixels = 5978, kUncutPixels = 6027;
  constexpr int kStray = 16;
  static const float kLong[3] = {-0.6f, -9.f, 6.6f}, kShort[3] = {0.1f, -1.f, 6.2f},
                     kFar[3] = {-2.f, -30.f, 8.f};
  const TestResult hit = test_frame(kLong, 6.3f, false, false);
  const TestResult air = test_frame(kLong, 30.f, false, false);
  const TestResult split = test_frame(kLong, 30.f, true, false);
  const TestResult on_screen = test_frame(kShort, 30.f, false, false);
  const TestResult uncut = test_frame(kFar, 6.3f, false, true);
  if (hit.dark < 0 || air.dark < 0 || split.dark < 0 || on_screen.dark < 0 || uncut.dark < 0) {
    lg::info("[ctr] shadow self-test: not run (the top screen can't be read back)");
    return;
  }
  const bool ok = std::abs(hit.dark - kHitPixels) * 10 <= kHitPixels && hit.left <= kStray &&
                  air.dark + air.left <= kStray && split.dark + split.left <= kStray &&
                  on_screen.dark + on_screen.left <= kStray;
  lg::info("[ctr] shadow self-test: inside {} px darkened + {} with a count left (expected {} + "
           "0); behind {} + {}, after a list split {} + {}, on screen {} + {} (expected 0 + 0) "
           "-> {}; inside a prism sent uncut, 1080 px past the screen's edge: {} + {} (expected "
           "{} + 0 if the GPU clips it)",
           hit.dark, hit.left, kHitPixels, air.dark, air.left, split.dark, split.left,
           on_screen.dark, on_screen.left, ok ? "pass" : "FAIL", uncut.dark, uncut.left,
           kUncutPixels);
}
