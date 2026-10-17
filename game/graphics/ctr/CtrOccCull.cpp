/*!
 * @file CtrOccCull.cpp
 * (AI-assisted)
 * config.ini occ_cull: the CPU occlusion buffer (see CtrOccCull.h).
 */

#include "CtrOccCull.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {
constexpr float kInf = 1e30f;
// a sample counts as covered when it is inside a triangle (or on its edge): the triangles of a
// surface leave no sample between them, as on the GPU; the edges of the covered area are then
// taken in by a sample (finish_occluders) against any difference in where the samples are
constexpr float kInside = 0.f;
// clip w (~ meters in front of the camera): triangles are clipped to w >= kClipW, and to a guard
// band of kGuard times the screen
constexpr float kNearW = 1e-3f;
constexpr float kClipW = 0.05f;
constexpr float kGuard = 4.f;
}  // namespace

void CtrOccCull::begin_frame(u64 frame) {
  m_frame = frame;
  memset(m_mask, 0, sizeof(m_mask));
  for (int i = 0; i < kCW * kCH; i++) {
    m_w[i] = kInf;
  }
  if (m_pyr[0].empty()) {
    int w = kCW, h = kCH;
    for (int l = 0; l < kLevels; l++) {
      m_pw[l] = w;
      m_ph[l] = h;
      m_pyr[l].assign((size_t)w * h, kInf);
      m_min[l].assign((size_t)w * h, kInf);
      w = (w + 1) / 2;
      h = (h + 1) / 2;
    }
  }
  for (auto& p : m_pyr) {
    std::fill(p.begin(), p.end(), kInf);
  }
}

void CtrOccCull::add_triangle(const float* a, const float* b, const float* c) {
  tris_in++;
  // clipped to w >= kClipW (in front of the camera) and a guard band of 4x the screen: the part
  // left is a part of the triangle (a smaller occluder hides less, never more)
  constexpr int kMax = 9;
  float poly[2][kMax][4];
  int n = 3;
  for (int k = 0; k < 4; k++) {
    poly[0][0][k] = a[k];
    poly[0][1][k] = b[k];
    poly[0][2][k] = c[k];
  }
  int cur = 0;
  bool clipped = false;
  for (int plane = 0; plane < 5 && n >= 3; plane++) {
    // plane value: >= 0 inside
    auto val = [&](const float* v) {
      switch (plane) {
        case 0:
          return v[3] - kClipW;
        case 1:
          return kGuard * v[3] - v[0];
        case 2:
          return kGuard * v[3] + v[0];
        case 3:
          return kGuard * v[3] - v[1];
        default:
          return kGuard * v[3] + v[1];
      }
    };
    bool all_in = true;
    for (int i = 0; i < n && all_in; i++) {
      all_in = val(poly[cur][i]) >= 0;
    }
    if (all_in) {
      continue;
    }
    clipped = true;
    int m = 0;
    for (int i = 0; i < n && m + 2 <= kMax; i++) {
      const float* p0 = poly[cur][i];
      const float* p1 = poly[cur][(i + 1) % n];
      const float v0 = val(p0), v1 = val(p1);
      if (v0 >= 0) {
        for (int k = 0; k < 4; k++) {
          poly[1 - cur][m][k] = p0[k];
        }
        m++;
      }
      if ((v0 >= 0) != (v1 >= 0)) {
        const float t = v0 / (v0 - v1);
        for (int k = 0; k < 4; k++) {
          poly[1 - cur][m][k] = p0[k] + (p1[k] - p0[k]) * t;
        }
        m++;
      }
    }
    cur = 1 - cur;
    n = m;
  }
  if (n < 3) {
    near_rejects++;
    return;
  }
  if (!clipped) {
    raster(a, b, c);
    return;
  }
  for (int i = 1; i + 1 < n; i++) {
    raster(poly[cur][0], poly[cur][i], poly[cur][i + 1]);
  }
}

void CtrOccCull::raster(const float* a, const float* b, const float* c) {
  if (!(a[3] > kNearW && b[3] > kNearW && c[3] > kNearW)) {
    return;
  }
  // screen pixels (the GPU's viewport: x = 200 + 200 x / w, y = 120 - 120 y / w), 1 / w
  float sx[3], sy[3], iw[3];
  const float* v[3] = {a, b, c};
  for (int k = 0; k < 3; k++) {
    iw[k] = 1.f / v[k][3];
    // in samples: sample i's center (pixel kStep i + 0.5) at i + 0.5
    sx[k] = (200.f + 200.f * v[k][0] * iw[k] - 0.5f) * (1.f / kStep) + 0.5f;
    sy[k] = (120.f - 120.f * v[k][1] * iw[k] - 0.5f) * (1.f / kStep) + 0.5f;
  }
  const float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
  if (!(std::abs(area) > 0.1f)) {
    area_rejects++;
    return;
  }
  // edges as half planes A x + B y + C >= 0 (inside), oriented by the third corner
  float A[3], B[3], C[3];
  for (int k = 0; k < 3; k++) {
    const int k2 = (k + 1) % 3, k3 = (k + 2) % 3;
    A[k] = sy[k] - sy[k2];
    B[k] = sx[k2] - sx[k];
    C[k] = -(A[k] * sx[k] + B[k] * sy[k]);
    if (A[k] * sx[k3] + B[k] * sy[k3] + C[k] < 0) {
      A[k] = -A[k];
      B[k] = -B[k];
      C[k] = -C[k];
    }
  }
  float IA[3];
  for (int k = 0; k < 3; k++) {
    IA[k] = A[k] != 0 ? 1.f / A[k] : 0.f;
  }
  // 1 / w over the screen: a plane through the corners
  const float dx1 = sx[1] - sx[0], dy1 = sy[1] - sy[0], dx2 = sx[2] - sx[0], dy2 = sy[2] - sy[0];
  const float det = dx1 * dy2 - dx2 * dy1;
  const float d1 = iw[1] - iw[0], d2 = iw[2] - iw[0];
  const float P = (d1 * dy2 - d2 * dy1) / det, Q = (dx1 * d2 - dx2 * d1) / det;
  const float R = iw[0] - P * sx[0] - Q * sy[0];
  const float miny = std::min({sy[0], sy[1], sy[2]}), maxy = std::max({sy[0], sy[1], sy[2]});
  const float minx = std::min({sx[0], sx[1], sx[2]}), maxx = std::max({sx[0], sx[1], sx[2]});
  const int cy0 = std::max(0, (int)std::floor(miny) / kCell);
  const int cy1 = std::min(kCH - 1, (int)std::floor(maxy) / kCell);
  if (cy0 > cy1 || maxy < 0 || maxx < 0 || minx >= kSW) {
    return;
  }
  tris_drawn++;
  // 1 / w is smallest (w farthest) at the cell corner picked by the signs of P and Q
  const float ox = P < 0 ? kCell - 1 : 0, oy = Q < 0 ? kCell - 1 : 0;
  // the edges as x bounds along a sample row y: x >= a0 + a1 y (left), x <= a0 + a1 y (right);
  // an edge along a row (A = 0) bounds y instead
  float la0[3], la1[3], ra0[3], ra1[3];
  int nl = 0, nr = 0;
  float ylo = -1e9f, yhi = 1e9f;
  for (int k = 0; k < 3; k++) {
    if (A[k] > 0) {
      la0[nl] = -C[k] * IA[k];
      la1[nl++] = -B[k] * IA[k];
    } else if (A[k] < 0) {
      ra0[nr] = -C[k] * IA[k];
      ra1[nr++] = -B[k] * IA[k];
    } else if (B[k] > 0) {
      ylo = std::max(ylo, -C[k] / B[k]);  // B y + C >= 0
    } else if (B[k] < 0) {
      yhi = std::min(yhi, -C[k] / B[k]);
    } else if (C[k] < 0) {
      return;
    }
  }
  // floor / ceil of values clamped to the samples' range (positive after the offset: a cast)
  auto ifloor = [](float v) {
    v = std::min(std::max(v, -(float)kSW - 8.f), (float)kSW + 8.f);
    return (int)(v + (float)(2 * kSW)) - 2 * kSW;
  };
  for (int cy = cy0; cy <= cy1; cy++) {
    // per sample row j: the samples inside are x in [lo, hi] (sample centers x + 0.5)
    int lo[kCell], hi[kCell];
    int span_lo = 1 << 20, span_hi = -1;  // cells some sample of which is covered
    int full_lo = 0, full_hi = kCW - 1;     // cells all of whose samples are covered
    for (int j = 0; j < kCell; j++) {
      const float y = cy * kCell + j + 0.5f;
      float xl = -1e9f, xr = 1e9f;
      for (int k = 0; k < nl; k++) {
        xl = std::max(xl, la0[k] + la1[k] * y);
      }
      for (int k = 0; k < nr; k++) {
        xr = std::min(xr, ra0[k] + ra1[k] * y);
      }
      if (y < ylo || y > yhi) {
        xl = 1e9f;
      }
      // sample x index i (center i + 0.5) inside: xl <= i + 0.5 <= xr
      lo[j] = xl > (float)kSW ? kSW : std::max(0, -ifloor(0.5f - xl));
      hi[j] = xr < 0.f ? -1 : std::min(kSW - 1, ifloor(xr - 0.5f));
      if (lo[j] > hi[j]) {
        full_lo = kCW;
        continue;
      }
      span_lo = std::min(span_lo, lo[j] / kCell);
      span_hi = std::max(span_hi, hi[j] / kCell);
      full_lo = std::max(full_lo, (lo[j] + kCell - 1) / kCell);
      full_hi = std::min(full_hi, (hi[j] + 1) / kCell - 1);
    }
    if (span_hi < span_lo) {
      continue;
    }
    for (int cx = span_lo; cx <= span_hi; cx++) {
      u16 mask = 0xffff;
      if (cx < full_lo || cx > full_hi) {
        mask = 0;
        for (int j = 0; j < kCell; j++) {
          const int l = std::max(lo[j], cx * kCell), h = std::min(hi[j], cx * kCell + kCell - 1);
          if (l <= h) {
            const u32 bits = ((1u << (h - l + 1)) - 1) << (l - cx * kCell);
            mask |= (u16)(bits << (4 * j));
          }
        }
        if (!mask) {
          continue;
        }
      }
      cells_written++;
      const float m = P * (cx * kCell + 0.5f + ox) + Q * (cy * kCell + 0.5f + oy) + R;
      if (!(m > 0)) {
        continue;
      }
      const float wf = 1.f / m;
      const int ci = cy * kCW + cx;
      u16& old = m_mask[ci];
      float& w = m_w[ci];
      if (mask == 0xffff) {
        w = old == 0xffff ? std::min(w, wf) : wf;
        old = 0xffff;
      } else if (old != 0xffff) {
        // two partial coverings together: covered as far as the farther of them
        w = old ? std::max(w, wf) : wf;
        old |= mask;
      }
    }
  }
}

void CtrOccCull::finish_occluders() {
  auto& p0 = m_pyr[0];
  full_cells = partial_cells = 0;
  // a sample stays covered only if its four neighbours are covered too (the samples beyond the
  // screen count as covered: nothing is drawn there): the covered area shrinks by a sample at its
  // edges, so a sample of the GPU's half a pixel off ours is still covered where ours is
  auto mask_at = [&](int cx, int cy) -> u32 {
    if (cx < 0 || cy < 0 || cx >= kCW || cy >= kCH) {
      return 0xffff;
    }
    return m_mask[cy * kCW + cx];
  };
  for (int cy = 0; cy < kCH; cy++) {
    for (int cx = 0; cx < kCW; cx++) {
      const int i = cy * kCW + cx;
      const u32 m = m_mask[i];
      if (!m) {
        p0[i] = kInf;
        continue;
      }
      const u32 ml = mask_at(cx - 1, cy), mr = mask_at(cx + 1, cy);
      if (m == 0xffff && ml == 0xffff && mr == 0xffff && mask_at(cx, cy - 1) == 0xffff &&
          mask_at(cx, cy + 1) == 0xffff) {
        p0[i] = m_w[i];
        full_cells++;
        continue;
      }
      const u32 mu = mask_at(cx, cy - 1), md = mask_at(cx, cy + 1);
      u32 e = 0;
      for (int j = 0; j < kCell; j++) {
        const u32 row = (m >> (4 * j)) & 0xf;
        const u32 left = ((row << 1) | ((ml >> (4 * j + 3)) & 1)) & 0xf;
        const u32 right = (row >> 1) | (((mr >> (4 * j)) & 1) << 3);
        const u32 up = j > 0 ? (m >> (4 * (j - 1))) & 0xf : (mu >> 12) & 0xf;
        const u32 down = j < kCell - 1 ? (m >> (4 * (j + 1))) & 0xf : md & 0xf;
        e |= (row & left & right & up & down) << (4 * j);
      }
      const bool full = e == 0xffff;
      p0[i] = full ? m_w[i] : kInf;
      full_cells += full;
      partial_cells += !full;
    }
  }
  m_min[0] = p0;
  for (int l = 1; l < kLevels; l++) {
    const auto& sm = m_min[l - 1];
    auto& dm = m_min[l];
    const int sw = m_pw[l - 1], sh = m_ph[l - 1];
    for (int y = 0; y < m_ph[l]; y++) {
      for (int x = 0; x < m_pw[l]; x++) {
        const int x1 = std::min(2 * x + 1, sw - 1), y1 = std::min(2 * y + 1, sh - 1);
        dm[y * m_pw[l] + x] = std::min(std::min(sm[(2 * y) * sw + 2 * x], sm[(2 * y) * sw + x1]),
                                       std::min(sm[y1 * sw + 2 * x], sm[y1 * sw + x1]));
      }
    }
  }
  for (int l = 1; l < kLevels; l++) {
    const auto& s = m_pyr[l - 1];
    auto& d = m_pyr[l];
    const int sw = m_pw[l - 1], sh = m_ph[l - 1];
    for (int y = 0; y < m_ph[l]; y++) {
      for (int x = 0; x < m_pw[l]; x++) {
        float v = s[(2 * y) * sw + 2 * x];
        if (2 * x + 1 < sw) {
          v = std::max(v, s[(2 * y) * sw + 2 * x + 1]);
        }
        if (2 * y + 1 < sh) {
          v = std::max(v, s[(2 * y + 1) * sw + 2 * x]);
          if (2 * x + 1 < sw) {
            v = std::max(v, s[(2 * y + 1) * sw + 2 * x + 1]);
          }
        }
        d[y * m_pw[l] + x] = v;
      }
    }
  }
}

float CtrOccCull::rect_max(int x0, int y0, int x1, int y1) const {
  int l = 0;
  while (l + 1 < kLevels && ((x1 >> l) - (x0 >> l) > 1 || (y1 >> l) - (y0 >> l) > 1)) {
    l++;
  }
  const auto& p = m_pyr[l];
  float v = 0;
  for (int y = y0 >> l; y <= (y1 >> l); y++) {
    for (int x = x0 >> l; x <= (x1 >> l); x++) {
      v = std::max(v, p[y * m_pw[l] + x]);
      if (v >= kInf) {
        return v;
      }
    }
  }
  return v;
}

bool CtrOccCull::hidden(const float* q, float rx, float ry, float rw) const {
  const float w0 = q[3] - rw, w1 = q[3] + rw;
  if (!(w0 > kNearW)) {
    return false;
  }
  // the sphere's clip x / w and y / w over w0..w1 (the extremes are at an end)
  const float i0 = 1.f / w0, i1 = 1.f / w1;
  const float xa = std::min((q[0] - rx) * i0, (q[0] - rx) * i1);
  const float xb = std::max((q[0] + rx) * i0, (q[0] + rx) * i1);
  const float ya = std::min((q[1] - ry) * i0, (q[1] - ry) * i1);
  const float yb = std::max((q[1] + ry) * i0, (q[1] + ry) * i1);
  // pixels, a pixel wider each way
  const float px0 = 200.f + 200.f * xa - 1.f, px1 = 200.f + 200.f * xb + 1.f;
  const float py0 = 120.f - 120.f * yb - 1.f, py1 = 120.f - 120.f * ya + 1.f;
  if (!(px1 >= 0 && px0 < kW && py1 >= 0 && py0 < kH)) {
    return false;  // (off screen: the view test's business)
  }
  // the cells holding the samples nearest to the rectangle's pixels (a pixel is between two
  // samples at most kStep apart: both must be covered)
  auto sx_of = [](float px) { return (px - 0.5f) * (1.f / kStep); };
  const int cx0 = std::max(0, (int)std::floor(sx_of(std::max(px0, 0.f))) / kCell);
  const int cx1 = std::min(kCW - 1, ((int)std::floor(sx_of(std::min(px1, (float)kW - 1))) + 1) / kCell);
  const int cy0 = std::max(0, (int)std::floor(sx_of(std::max(py0, 0.f))) / kCell);
  const int cy1 = std::min(kCH - 1, ((int)std::floor(sx_of(std::min(py1, (float)kH - 1))) + 1) / kCell);
  const float m = rect_max(cx0, cy0, cx1, cy1);
  // behind it by 2% (depth buffer precision; GEQUAL passes what's level with the occluder)
  return m < kInf && m * 1.02f < w0;
}

bool CtrOccCull::on_screen(const float* q, float rx, float ry, float rw) {
  const float w1 = q[3] + rw;
  if (!(w1 > 0)) {
    return false;  // all of it behind the camera
  }
  // the view volume's side planes: |x| <= w, |y| <= w (each moves by the half extents)
  return q[0] - rx <= w1 && q[0] + rx >= -w1 && q[1] - ry <= w1 && q[1] + ry >= -w1 &&
         std::abs(q[0]) - rx <= q[3] + rw && std::abs(q[1]) - ry <= q[3] + rw;
}

void CtrOccCull::dump(const char* path) const {
  FILE* fp = fopen(path, "wb");
  if (!fp) {
    return;
  }
  fprintf(fp, "P5 %d %d 255\n", kCW, kCH);
  for (int i = 0; i < kCW * kCH; i++) {
    int v = 0;
    if (m_mask[i] == 0xffff) {
      v = 255 - std::min(200, (int)m_w[i]);
    } else if (m_mask[i]) {
      v = 30;
    }
    fputc(v, fp);
  }
  fclose(fp);
}

bool CtrOccCull::may_hide(const float* q, float rx, float ry, float rw) const {
  const float w0 = q[3] - rw, w1 = q[3] + rw;
  if (!(w0 > kNearW)) {
    return true;  // (the camera near or in it: its parts may be anywhere on screen)
  }
  const float i0 = 1.f / w0, i1 = 1.f / w1;
  const float xa = std::min((q[0] - rx) * i0, (q[0] - rx) * i1);
  const float xb = std::max((q[0] + rx) * i0, (q[0] + rx) * i1);
  const float ya = std::min((q[1] - ry) * i0, (q[1] - ry) * i1);
  const float yb = std::max((q[1] + ry) * i0, (q[1] + ry) * i1);
  const float px0 = std::max(0.5f, 200.f + 200.f * xa - 1.f),
              px1 = std::min((float)kW - 1, 200.f + 200.f * xb + 1.f);
  const float py0 = std::max(0.5f, 120.f - 120.f * yb - 1.f),
              py1 = std::min((float)kH - 1, 120.f - 120.f * ya + 1.f);
  if (!(px1 >= px0 && py1 >= py0)) {
    return false;
  }
  const int x0 = (int)((px0 - 0.5f) * (1.f / kStep)) / kCell,
            x1 = std::min(kCW - 1, ((int)((px1 - 0.5f) * (1.f / kStep)) + 1) / kCell);
  const int y0 = (int)((py0 - 0.5f) * (1.f / kStep)) / kCell,
            y1 = std::min(kCH - 1, ((int)((py1 - 0.5f) * (1.f / kStep)) + 1) / kCell);
  // the nearest full cell over the rectangle (rounded out to the level's cells: a lower bound)
  int l = 0;
  while (l + 1 < kLevels && ((x1 >> l) - (x0 >> l) > 1 || (y1 >> l) - (y0 >> l) > 1)) {
    l++;
  }
  float m = kInf;
  for (int y = y0 >> l; y <= (y1 >> l); y++) {
    for (int x = x0 >> l; x <= (x1 >> l); x++) {
      m = std::min(m, m_min[l][y * m_pw[l] + x]);
    }
  }
  return m < kInf && m * 1.02f < w1;
}
