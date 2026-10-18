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

// floats as integers in the same order (min / max without VFP compares)
inline s32 fkey(float f) {
  const u32 b = float_bits(f);
  return (s32)(b ^ ((u32)((s32)b >> 31) >> 1));
}
inline float fkey_value(s32 k) {
  const u32 b = (u32)k ^ ((u32)(k >> 31) >> 1);
  float f;
  memcpy(&f, &b, 4);
  return f;
}

// camera space coordinates beyond this (24 km) are bad data, NaN too: with them and the clip
// matrix's entries under kMaxMatrix, clip space stays far from the GPU's float24 limit (~1e19)
constexpr float kMaxCamera = 1e8f;
constexpr float kMaxMatrix = 1e6f;
// w at or below this: the volume reaches behind the camera, its screen rectangle is the screen
constexpr float kMinW = 1e-4f;
// the darkening rectangles' margin: 2 pixels (400 x 240 for x, y in [-1, 1])
constexpr float kPadX = 4.f / 400.f, kPadY = 4.f / 240.f;

struct Bounds {
  s32 x0 = INT32_MAX, x1 = INT32_MIN, y0 = INT32_MAX, y1 = INT32_MIN;  // x / w, y / w (keys)
  s32 w0 = INT32_MAX;                                                  // the smallest w (key)
  u32 big = 0;  // the largest |x|, |y|, |z| of the input (bits)
};

/*!
 * n vertices (x, y, z of each quadword) to clip space with the row-major matrix mp, on the way
 * their screen bounds (x / w, y / w: the divide runs in the VFP's divide pipeline while the stores
 * go on), the smallest w and the largest input. The matrix and the bounds are locals: in
 * registers for the whole loop (through the pointers, every store to dst would reload them).
 */
void transform(const float* src, int n, const float* mp, ctr_clip_vertex* dst, Bounds& b) {
  const float m0 = mp[0], m1 = mp[1], m2 = mp[2], m3 = mp[3], m4 = mp[4], m5 = mp[5], m6 = mp[6],
              m7 = mp[7], m8 = mp[8], m9 = mp[9], m10 = mp[10], m11 = mp[11], m12 = mp[12],
              m13 = mp[13], m14 = mp[14], m15 = mp[15];
  s32 x0 = b.x0, x1 = b.x1, y0 = b.y0, y1 = b.y1, w0 = b.w0;
  u32 big = b.big;
  for (int i = 0; i < n; i++, src += 4, dst++) {
    const float x = src[0], y = src[1], z = src[2];
    u32 bits[3];
    memcpy(bits, src, 12);
    big = std::max(big, bits[0] & 0x7fffffffu);
    big = std::max(big, bits[1] & 0x7fffffffu);
    big = std::max(big, bits[2] & 0x7fffffffu);
    const float cx = m0 * x + m1 * y + m2 * z + m3;
    const float cy = m4 * x + m5 * y + m6 * z + m7;
    const float cz = m8 * x + m9 * y + m10 * z + m11;
    const float cw = m12 * x + m13 * y + m14 * z + m15;
    const float rw = 1.f / cw;  // (w <= 0: garbage bounds, the object is full screen anyway)
    dst->x = cx;
    dst->y = cy;
    dst->z = cz;
    dst->w = cw;
    dst->s = 0.f;
    dst->t = 0.f;
    memset(&dst->r, 0, 4);
    w0 = std::min(w0, fkey(cw));
    const s32 kx = fkey(cx * rw), ky = fkey(cy * rw);
    x0 = std::min(x0, kx);
    x1 = std::max(x1, kx);
    y0 = std::min(y0, ky);
    y1 = std::max(y1, ky);
  }
  b.x0 = x0;
  b.x1 = x1;
  b.y0 = y0;
  b.y1 = y1;
  b.w0 = w0;
  b.big = big;
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
  m_nrects = 0;
  m_full_screen = false;
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

float CtrShadowVolumes::object_score(const DmaTransfer& t) {
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
  Bounds b;
  transform(m_top, m_ntop, m_clip, m_out + m_nv, b);
  if (m_dual) {
    transform(m_dual, m_ndual, m_clip, m_out + m_nv + m_ntop, b);
  }
  if (b.big > float_bits(kMaxCamera)) {
    drop_object();  // NaN or huge numbers (m_nv not advanced yet: nothing to take back)
    return false;
  }
  // the screen rectangle (x / w, y / w of every vertex), unless a vertex is at or behind the
  // camera: then the GPU's near clipping can put the volume anywhere on screen
  m_obj_full = !(fkey_value(b.w0) > kMinW);
  if (!m_obj_full) {
    m_obj_rect.x0 = std::max(-1.f, fkey_value(b.x0) - kPadX);
    m_obj_rect.x1 = std::min(1.f, fkey_value(b.x1) + kPadX);
    m_obj_rect.y0 = std::max(-1.f, fkey_value(b.y0) - kPadY);
    m_obj_rect.y1 = std::min(1.f, fkey_value(b.y1) + kPadY);
  }
  m_obj_base = m_nv;
  m_obj_f0 = m_nf;
  m_obj_b0 = m_nb;
  m_nv += m_ntop + m_ndual;
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
  // her hulls, each an object of its own (its own tables and screen rectangle)
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
    tris<false>(hv.tris, hv.ntris, false);
    edges<false>(hv.edges, hv.nedges);
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
    if (m_obj_full) {
      m_full_screen = true;
    } else if (m_obj_rect.x0 < m_obj_rect.x1 && m_obj_rect.y0 < m_obj_rect.y1) {
      if (m_nrects < kMaxRects) {
        m_rects[m_nrects++] = m_obj_rect;
      } else {
        Rect& r = m_rects[kMaxRects - 1];
        r.x0 = std::min(r.x0, m_obj_rect.x0);
        r.y0 = std::min(r.y0, m_obj_rect.y0);
        r.x1 = std::max(r.x1, m_obj_rect.x1);
        r.y1 = std::max(r.y1, m_obj_rect.y1);
      }
    }
  }
  m_obj_open = false;
  m_obj_any = false;
}

/*!
 * MSCAL 2 (facing single tris) and 6 (double-sided tris): count entries (i0, i1, i2, faces), the
 * top cap (T) for all of them, then the bottom cap (D) reversed (see CtrShadow.h).
 */
template <bool kTrace>
void CtrShadowVolumes::tris(const u8* e, int count, bool double_sided) {
  const int lim = std::min(m_ntop, m_dual ? m_ndual : 0);
  const float* T = m_top;
  const float* D = m_dual;
  const u16 tb = (u16)m_obj_base, db = (u16)(m_obj_base + m_ntop);
  u16* cur[2] = {m_back + m_nb, m_front + m_nf};  // by facing: 0 back, 1 front
  for (int cap = 0; cap < 2; cap++) {
    const float* V = cap ? D : T;
    const u16 base = cap ? db : tb;
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
      const u32 f = vu_front(V + 4 * i0, V + 4 * i1, V + 4 * i2);
      u16* p = cur[f];
      p[0] = (u16)(base + i0);
      p[1] = (u16)(base + i1);
      p[2] = (u16)(base + i2);
      cur[f] = p + 3;
      if constexpr (kTrace) {
        m_trace->push_back(Prim{{p[0], p[1], p[2], 0}, 3, (u8)f});
      }
    }
  }
  m_nb = (int)(cur[0] - m_back);
  m_nf = (int)(cur[1] - m_front);
}

/*!
 * MSCAL 4 (silhouette edges): count entries (a, b, flip, 0), each the side quad between the caps.
 */
template <bool kTrace>
void CtrShadowVolumes::edges(const u8* e, int count) {
  const int lim = std::min(m_ntop, m_dual ? m_ndual : 0);
  const float* T = m_top;
  const float* D = m_dual;
  const int tb = m_obj_base, db = m_obj_base + m_ntop;
  u16* cur[2] = {m_back + m_nb, m_front + m_nf};
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
    const u32 f = vu_front(T + 4 * a, T + 4 * b, D + 4 * b);
    const u16 q0 = (u16)(tb + a), q1 = (u16)(tb + b), q2 = (u16)(db + b), q3 = (u16)(db + a);
    u16* p = cur[f];
    p[0] = q0;
    p[1] = q1;
    p[2] = q2;
    p[3] = q0;
    p[4] = q2;
    p[5] = q3;
    cur[f] = p + 6;
    if constexpr (kTrace) {
      m_trace->push_back(Prim{{q0, q1, q2, q3}, 4, (u8)f});
    }
  }
  m_nb = (int)(cur[0] - m_back);
  m_nf = (int)(cur[1] - m_front);
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
    if (edge_list) {
      m_trace ? edges<true>(e, count) : edges<false>(e, count);
    } else {
      m_trace ? tris<true>(e, count, entry == 6) : tris<false>(e, count, entry == 6);
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

CtrShadowVolumes::Rect CtrShadowVolumes::rect(int i) const {
  if (m_full_screen) {
    return Rect{-1.f, -1.f, 1.f, 1.f};
  }
  return m_rects[i];
}

void CtrShadowVolumes::color(u8 out[3]) const {
  for (int i = 0; i < 3; i++) {
    out[i] = (u8)std::min(255, 2 * m_rgb[i]);
  }
}

// ---------------------------------------------------------------------------------------------
// CtrShadowRenderer
// ---------------------------------------------------------------------------------------------

namespace {
constexpr int kReserveVerts = CtrShadowVolumes::kMaxVerts + 4 * CtrShadowVolumes::kMaxRects;
constexpr int kReserveIndices = CtrShadowVolumes::kMaxIndices + 6 * CtrShadowVolumes::kMaxRects;
}  // namespace

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
    DmaFollower scan = dma;
    while (scan.current_tag_offset() != rs.next_bucket) {
      const auto t = scan.read_and_advance();
      if (CtrShadowVolumes::starts_object(t)) {
        m_scores.push_back(CtrShadowVolumes::object_score(t));
      }
    }
    const int n = (int)m_scores.size();
    if (n > cap) {
      m_order.resize(n);
      for (int i = 0; i < n; i++) {
        m_order[i] = i;
      }
      std::nth_element(m_order.begin(), m_order.begin() + (cap - 1), m_order.end(),
                       [&](int a, int b) { return m_scores[a] > m_scores[b]; });
      m_keep.assign(n, 0);
      for (int i = 0; i < cap; i++) {
        m_keep[m_order[i]] = 1;
      }
      m_vol.set_keep(m_keep.data(), n);
    }
  }
  while (dma.current_tag_offset() != rs.next_bucket) {
    const auto t = dma.read_and_advance();
    if (!m_reserved && !no_room && CtrShadowVolumes::starts_object(t)) {
      // this frame's vertex memory, from the first object on (nothing else is drawn until
      // draw()): the volumes are written straight into it
      m_reserved = ctr_gpu_clip_reserve(kReserveVerts, kReserveIndices, &m_verts, &m_indices) != 0;
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
              "front + {} back triangles, {} rectangles{}; {} bad entries, {} bad packets, {} "
              "without camera; Keira's shadow drawn {} times",
              st.objects, st.dropped_objects, st.capped, m_vol.vert_count(), m_vol.front_count() / 3,
              m_vol.back_count() / 3, m_vol.rect_count(),
              m_vol.full_screen() ? " (full screen)" : "", st.bad_entries, st.bad_packets,
              st.no_camera, m_vol.keira_replaced());
  }
}

void CtrShadowRenderer::draw() {
  const int nv = m_vol.vert_count(), nf = m_vol.front_count(), nb = m_vol.back_count();
  if (nf < 3) {
    // no front faces: the stencil stays 0 (the back faces only take away), nothing to darken
    ctr_gpu_clip_draw_ranges(nullptr, nullptr, nullptr, 0, 0, 0);
    return;
  }
  // the back list after the front list, then the rectangles (vertices after the volumes')
  memcpy(m_indices + nf, m_back.data(), (size_t)nb * 2);
  const int nr = m_vol.rect_count();
  u8 rgb[3];
  m_vol.color(rgb);
  int ni = nf + nb;
  for (int i = 0; i < nr; i++) {
    const auto r = m_vol.rect(i);
    const float xy[4][2] = {{r.x0, r.y0}, {r.x1, r.y0}, {r.x1, r.y1}, {r.x0, r.y1}};
    ctr_clip_vertex* v = m_verts + nv + 4 * i;
    for (int k = 0; k < 4; k++) {
      v[k].x = xy[k][0];
      v[k].y = xy[k][1];
      v[k].z = 0.f;  // (any depth inside the clip volume: the depth test always passes)
      v[k].w = 1.f;
      v[k].s = v[k].t = 0.f;
      v[k].r = rgb[0];
      v[k].g = rgb[1];
      v[k].b = rgb[2];
      v[k].a = 0x80;
    }
    static const u16 kQuad[6] = {0, 1, 2, 0, 2, 3};
    for (int k = 0; k < 6; k++) {
      m_indices[ni++] = (u16)(nv + 4 * i + kQuad[k]);
    }
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
  st[2].ztest = CTR_TEST_ALWAYS;
  st[2].flags = CTR_STATE_STENCIL_CLEAR;
  const int first[3] = {0, nf, nf + nb};
  const int count[3] = {nf, nb, 6 * nr};
  ctr_gpu_clip_draw_ranges(st, first, count, 3, nv + 4 * nr, ni);
}
