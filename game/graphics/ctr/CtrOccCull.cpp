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
    sx[k] = 200.f + 200.f * v[k][0] * iw[k];
    sy[k] = 120.f - 120.f * v[k][1] * iw[k];
  }
  float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
  if (!(std::abs(area) > 0.25f)) {
    area_rejects++;
    return;  // (no pixel center a half pixel inside it)
  }
  if (area < 0) {
    std::swap(sx[1], sx[2]);
    std::swap(sy[1], sy[2]);
    std::swap(iw[1], iw[2]);
    area = -area;
  }
  // edge k: from corner k to k + 1, inside where e = A x + B y + C >= kInside (pixels)
  float A[3], B[3], C[3];
  for (int k = 0; k < 3; k++) {
    const int k2 = (k + 1) % 3;
    const float ex = sx[k2] - sx[k], ey = sy[k2] - sy[k];
    const float len = std::sqrt(ex * ex + ey * ey);
    if (!(len > 1e-6f)) {
      return;
    }
    // e(p) = (ex (py - sy) - ey (px - sx)) / len: positive inside (counter-clockwise, y down: the
    // area above is positive for this orientation)
    A[k] = -ey / len;
    B[k] = ex / len;
    C[k] = -(A[k] * sx[k] + B[k] * sy[k]);
  }
  // (orientation: the third corner must be inside every edge)
  if (A[0] * sx[2] + B[0] * sy[2] + C[0] < 0) {
    for (int k = 0; k < 3; k++) {
      A[k] = -A[k];
      B[k] = -B[k];
      C[k] = -C[k];
    }
  }
  // 1 / w over the screen: a plane through the corners
  const float dx1 = sx[1] - sx[0], dy1 = sy[1] - sy[0], dx2 = sx[2] - sx[0], dy2 = sy[2] - sy[0];
  const float det = dx1 * dy2 - dx2 * dy1;
  if (!(std::abs(det) > 1e-9f)) {
    return;
  }
  const float d1 = iw[1] - iw[0], d2 = iw[2] - iw[0];
  const float P = (d1 * dy2 - d2 * dy1) / det, Q = (dx1 * d2 - dx2 * d1) / det;
  const float R = iw[0] - P * sx[0] - Q * sy[0];
  const float minx = std::min({sx[0], sx[1], sx[2]}), maxx = std::max({sx[0], sx[1], sx[2]});
  const float miny = std::min({sy[0], sy[1], sy[2]}), maxy = std::max({sy[0], sy[1], sy[2]});
  const int cx0 = std::max(0, (int)std::floor(minx) / kCell);
  const int cx1 = std::min(kCW - 1, (int)std::floor(maxx) / kCell);
  const int cy0 = std::max(0, (int)std::floor(miny) / kCell);
  const int cy1 = std::min(kCH - 1, (int)std::floor(maxy) / kCell);
  if (cx0 > cx1 || cy0 > cy1 || maxx < 0 || maxy < 0) {
    return;
  }
  tris_drawn++;
  for (int cy = cy0; cy <= cy1; cy++) {
    const float y0 = cy * kCell + 0.5f, y3 = y0 + (kCell - 1);
    for (int cx = cx0; cx <= cx1; cx++) {
      const float x0 = cx * kCell + 0.5f, x3 = x0 + (kCell - 1);
      // each edge at the cell's corner samples: all outside -> nothing, all inside -> all
      bool all = true, none = false;
      for (int k = 0; k < 3 && !none; k++) {
        const float e00 = A[k] * x0 + B[k] * y0 + C[k];
        const float ea = A[k] * (kCell - 1), eb = B[k] * (kCell - 1);
        const float emin = e00 + std::min(0.f, ea) + std::min(0.f, eb);
        const float emax = e00 + std::max(0.f, ea) + std::max(0.f, eb);
        if (emax < kInside) {
          none = true;
        } else if (emin < kInside) {
          all = false;
        }
      }
      if (none) {
        continue;
      }
      u16 mask = 0xffff;
      if (!all) {
        mask = 0;
        for (int j = 0; j < kCell; j++) {
          for (int i = 0; i < kCell; i++) {
            const float px = x0 + i, py = y0 + j;
            if (A[0] * px + B[0] * py + C[0] >= kInside &&
                A[1] * px + B[1] * py + C[1] >= kInside &&
                A[2] * px + B[2] * py + C[2] >= kInside) {
              mask |= (u16)(1u << (j * kCell + i));
            }
          }
        }
        if (!mask) {
          continue;
        }
      }
      // the farthest w over the cell's samples (1 / w is a plane: its smallest at a corner)
      const float m = std::min(std::min(P * x0 + Q * y0, P * x3 + Q * y0),
                               std::min(P * x0 + Q * y3, P * x3 + Q * y3)) +
                      R;
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
      const u32 mu = mask_at(cx, cy - 1), md = mask_at(cx, cy + 1);
      u32 e = 0;
      for (int j = 0; j < kCell; j++) {
        const u32 row = (m >> (4 * j)) & 0xf;
        const u32 left = ((row << 1) | ((ml >> (4 * j + 3)) & 1)) & 0xf;
        const u32 right = (row >> 1) | (((mr >> (4 * j)) & 1) << 3);
        const u32 up = j > 0 ? (m >> (4 * (j - 1))) & 0xf : (mu >> 12) & 0xf;
        const u32 down = j < kCell - 1 ? (m >> (4 * (j + 1))) & 0xf : mu & 0 | (md & 0xf);
        e |= (row & left & right & up & down) << (4 * j);
      }
      const bool full = e == 0xffff;
      p0[i] = full ? m_w[i] : kInf;
      full_cells += full;
      partial_cells += !full;
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
  const float xa = std::min((q[0] - rx) / w0, (q[0] - rx) / w1);
  const float xb = std::max((q[0] + rx) / w0, (q[0] + rx) / w1);
  const float ya = std::min((q[1] - ry) / w0, (q[1] - ry) / w1);
  const float yb = std::max((q[1] + ry) / w0, (q[1] + ry) / w1);
  // pixels, a pixel wider each way
  const float px0 = 200.f + 200.f * xa - 1.f, px1 = 200.f + 200.f * xb + 1.f;
  const float py0 = 120.f - 120.f * yb - 1.f, py1 = 120.f - 120.f * ya + 1.f;
  if (!(px1 >= 0 && px0 < kW && py1 >= 0 && py0 < kH)) {
    return false;  // (off screen: the view test's business)
  }
  const int cx0 = std::max(0, (int)std::floor(px0) / kCell);
  const int cx1 = std::min(kCW - 1, (int)std::floor(std::min(px1, (float)kW - 1)) / kCell);
  const int cy0 = std::max(0, (int)std::floor(py0) / kCell);
  const int cy1 = std::min(kCH - 1, (int)std::floor(std::min(py1, (float)kH - 1)) / kCell);
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
