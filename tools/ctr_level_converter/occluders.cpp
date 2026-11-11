/*!
 * @file occluders.cpp
 * (AI-assisted) See occluders.h.
 */

#include "occluders.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>

namespace occluders {

namespace {

using V3 = std::array<double, 3>;

double dot(const V3& a, const V3& b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
V3 sub(const V3& a, const V3& b) {
  return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
V3 cross(const V3& a, const V3& b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}

// Moller-Trumbore: the ray from o along d crosses triangle t at t > 0
bool ray_tri(const V3& o, const V3& d, const Tri& t) {
  const V3 e1 = sub(t[1], t[0]), e2 = sub(t[2], t[0]);
  const V3 p = cross(d, e2);
  const double det = dot(e1, p);
  if (std::abs(det) < 1e-12) {
    return false;
  }
  const double inv = 1.0 / det;
  const V3 s = sub(o, t[0]);
  const double u = dot(s, p) * inv;
  if (u < 0.0 || u > 1.0) {
    return false;
  }
  const V3 q = cross(s, e1);
  const double v = dot(d, q) * inv;
  if (v < 0.0 || u + v > 1.0) {
    return false;
  }
  return dot(e2, q) * inv > 0.0;
}

// one trial: the box upright, its first axis at yaw `yaw` around y
bool trial(const std::vector<Tri>& tris, const Params& p, double yaw, ctr_cull::Box* out,
           double* volume) {
  const V3 ax[3] = {V3{std::cos(yaw), 0.0, std::sin(yaw)}, V3{0.0, 1.0, 0.0},
                    V3{-std::sin(yaw), 0.0, std::cos(yaw)}};
  // the triangles in the box frame
  std::vector<Tri> lt(tris.size());
  V3 lo = {1e300, 1e300, 1e300}, hi = {-1e300, -1e300, -1e300};
  for (size_t i = 0; i < tris.size(); i++) {
    for (int c = 0; c < 3; c++) {
      for (int k = 0; k < 3; k++) {
        const double v = dot(tris[i][c], ax[k]);
        lt[i][c][k] = v;
        lo[k] = std::min(lo[k], v);
        hi[k] = std::max(hi[k], v);
      }
    }
  }
  const double longest = std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]});
  if (!(longest > 0)) {
    return false;
  }
  const double cell = std::max(longest / p.grid, p.min_cell);
  int n[3];
  for (int k = 0; k < 3; k++) {
    n[k] = std::max(1, (int)std::ceil((hi[k] - lo[k]) / cell));
    if (n[k] > 4 * p.grid) {
      return false;
    }
  }
  auto center = [&](int k, int i) { return lo[k] + (i + 0.5) * cell; };
  auto at = [&](int i, int j, int k) { return ((size_t)k * n[1] + j) * n[0] + i; };
  std::vector<uint8_t> inside((size_t)n[0] * n[1] * n[2], 0);
  // the triangles by the rows (j, k) their (v, w) box touches
  std::vector<std::vector<uint32_t>> rows((size_t)n[1] * n[2]);
  for (size_t t = 0; t < lt.size(); t++) {
    double l1 = 1e300, h1 = -1e300, l2 = 1e300, h2 = -1e300;
    for (int c = 0; c < 3; c++) {
      l1 = std::min(l1, lt[t][c][1]);
      h1 = std::max(h1, lt[t][c][1]);
      l2 = std::min(l2, lt[t][c][2]);
      h2 = std::max(h2, lt[t][c][2]);
    }
    const int j0 = std::max(0, (int)std::floor((l1 - lo[1]) / cell - 0.5));
    const int j1 = std::min(n[1] - 1, (int)std::ceil((h1 - lo[1]) / cell - 0.5));
    const int k0 = std::max(0, (int)std::floor((l2 - lo[2]) / cell - 0.5));
    const int k1 = std::min(n[2] - 1, (int)std::ceil((h2 - lo[2]) / cell - 0.5));
    for (int k = k0; k <= k1; k++) {
      for (int j = j0; j <= j1; j++) {
        rows[(size_t)k * n[1] + j].push_back((uint32_t)t);
      }
    }
  }
  // each row: the crossings of the line along the first axis, inside between odd and even ones
  std::vector<double> xs;
  for (int k = 0; k < n[2]; k++) {
    for (int j = 0; j < n[1]; j++) {
      // (off the grid by a little: no line through a corner or along an edge)
      const double v = center(1, j) + cell * 0.000731, w = center(2, k) + cell * 0.000419;
      xs.clear();
      for (uint32_t t : rows[(size_t)k * n[1] + j]) {
        const auto& q = lt[t];
        const double d = (q[1][1] - q[0][1]) * (q[2][2] - q[0][2]) -
                         (q[2][1] - q[0][1]) * (q[1][2] - q[0][2]);
        if (std::abs(d) < 1e-12) {
          continue;
        }
        const double a = ((v - q[0][1]) * (q[2][2] - q[0][2]) - (q[2][1] - q[0][1]) * (w - q[0][2])) / d;
        const double b = ((q[1][1] - q[0][1]) * (w - q[0][2]) - (v - q[0][1]) * (q[1][2] - q[0][2])) / d;
        if (a < 0 || b < 0 || a + b > 1) {
          continue;
        }
        xs.push_back(q[0][0] + a * (q[1][0] - q[0][0]) + b * (q[2][0] - q[0][0]));
      }
      if (xs.size() < 2) {
        continue;
      }
      std::sort(xs.begin(), xs.end());
      size_t c = 0;
      for (int i = 0; i < n[0]; i++) {
        const double u = center(0, i);
        while (c < xs.size() && xs[c] < u) {
          c++;
        }
        inside[at(i, j, k)] = (c & 1) ? 1 : 0;
      }
    }
  }
  // the chessboard distance of each inside voxel to the outside (the grid's border is outside)
  std::vector<int> dist(inside.size(), -1);
  std::deque<size_t> queue;
  for (int k = 0; k < n[2]; k++) {
    for (int j = 0; j < n[1]; j++) {
      for (int i = 0; i < n[0]; i++) {
        const size_t id = at(i, j, k);
        const bool border = i == 0 || j == 0 || k == 0 || i == n[0] - 1 || j == n[1] - 1 ||
                            k == n[2] - 1;
        if (!inside[id]) {
          dist[id] = 0;
          queue.push_back(id);
        } else if (border) {
          dist[id] = 1;
          queue.push_back(id);
        }
      }
    }
  }
  size_t best = SIZE_MAX;
  int best_d = 0;
  while (!queue.empty()) {
    const size_t id = queue.front();
    queue.pop_front();
    const int i = (int)(id % n[0]), j = (int)((id / n[0]) % n[1]), k = (int)(id / ((size_t)n[0] * n[1]));
    for (int dk = -1; dk <= 1; dk++) {
      for (int dj = -1; dj <= 1; dj++) {
        for (int di = -1; di <= 1; di++) {
          const int a = i + di, b = j + dj, c = k + dk;
          if (a < 0 || b < 0 || c < 0 || a >= n[0] || b >= n[1] || c >= n[2]) {
            continue;
          }
          const size_t nb = at(a, b, c);
          if (dist[nb] < 0) {
            dist[nb] = dist[id] + 1;
            queue.push_back(nb);
            if (dist[nb] > best_d) {
              best_d = dist[nb];
              best = nb;
            }
          }
        }
      }
    }
  }
  if (best == SIZE_MAX || best_d < 2) {
    return false;
  }
  // grow a box of inside voxels from the deepest one, a layer at a time on each side in turn
  int b0[3] = {(int)(best % n[0]), (int)((best / n[0]) % n[1]), (int)(best / ((size_t)n[0] * n[1]))};
  int b1[3] = {b0[0], b0[1], b0[2]};
  auto slab_inside = [&](int axis, int layer) {
    int l[3] = {b0[0], b0[1], b0[2]}, h[3] = {b1[0], b1[1], b1[2]};
    l[axis] = h[axis] = layer;
    for (int k = l[2]; k <= h[2]; k++) {
      for (int j = l[1]; j <= h[1]; j++) {
        for (int i = l[0]; i <= h[0]; i++) {
          if (!inside[at(i, j, k)]) {
            return false;
          }
        }
      }
    }
    return true;
  };
  for (bool grew = true; grew;) {
    grew = false;
    for (int dir = 0; dir < 6; dir++) {
      const int axis = dir >> 1;
      if (dir & 1) {
        if (b1[axis] + 1 < n[axis] && slab_inside(axis, b1[axis] + 1)) {
          b1[axis]++;
          grew = true;
        }
      } else if (b0[axis] > 0 && slab_inside(axis, b0[axis] - 1)) {
        b0[axis]--;
        grew = true;
      }
    }
  }
  // the box between the voxel centers (half a voxel inside the voxels' faces), then checked exactly
  ctr_cull::Box box{};
  double c_local[3], h[3];
  for (int k = 0; k < 3; k++) {
    const double a = center(k, b0[k]), b = center(k, b1[k]);
    c_local[k] = 0.5 * (a + b);
    h[k] = 0.5 * (b - a);
  }
  for (int k = 0; k < 3; k++) {
    for (int m = 0; m < 3; m++) {
      box.ax[k][m] = (float)ax[k][m];
    }
  }
  for (int shrink = 0; shrink < 8; shrink++) {
    if (h[0] < p.min_half || h[1] < p.min_half || h[2] < p.min_half) {
      return false;
    }
    for (int m = 0; m < 3; m++) {
      box.c[m] = (float)(c_local[0] * ax[0][m] + c_local[1] * ax[1][m] + c_local[2] * ax[2][m]);
    }
    for (int k = 0; k < 3; k++) {
      box.h[k] = (float)h[k];
    }
    if (box_fits(tris, box, 0.0)) {
      if (h[0] * h[1] + h[1] * h[2] + h[2] * h[0] < p.min_faces) {
        return false;
      }
      *out = box;
      *volume = h[0] * h[1] * h[2];
      return true;
    }
    for (double& x : h) {
      x *= 0.85;
    }
  }
  return false;
}

}  // namespace

bool tri_box_overlap(const ctr_cull::Box& b, const Tri& t, double grow) {
  // the triangle in the box's frame, the box at the origin
  V3 v[3];
  for (int c = 0; c < 3; c++) {
    const V3 d = {t[c][0] - b.c[0], t[c][1] - b.c[1], t[c][2] - b.c[2]};
    for (int k = 0; k < 3; k++) {
      v[c][k] = d[0] * b.ax[k][0] + d[1] * b.ax[k][1] + d[2] * b.ax[k][2];
    }
  }
  const double h[3] = {b.h[0] + grow, b.h[1] + grow, b.h[2] + grow};
  auto separated = [&](const V3& axis) {
    const double r = h[0] * std::abs(axis[0]) + h[1] * std::abs(axis[1]) + h[2] * std::abs(axis[2]);
    const double p0 = dot(v[0], axis), p1 = dot(v[1], axis), p2 = dot(v[2], axis);
    return std::min({p0, p1, p2}) > r || std::max({p0, p1, p2}) < -r;
  };
  for (int k = 0; k < 3; k++) {
    V3 a = {0, 0, 0};
    a[k] = 1;
    if (separated(a)) {
      return false;
    }
  }
  const V3 e[3] = {sub(v[1], v[0]), sub(v[2], v[1]), sub(v[0], v[2])};
  if (separated(cross(e[0], e[1]))) {
    return false;
  }
  for (int k = 0; k < 3; k++) {
    V3 a = {0, 0, 0};
    a[k] = 1;
    for (const auto& ed : e) {
      const V3 ax = cross(a, ed);
      if (dot(ax, ax) > 1e-18 && separated(ax)) {
        return false;
      }
    }
  }
  return true;
}

bool point_inside(const std::vector<Tri>& tris, const double* p) {
  // three directions that no level geometry lines up with
  static const V3 dirs[3] = {V3{0.5731, 0.3122, 0.7577}, V3{-0.6411, 0.5217, -0.5629},
                             V3{0.2179, -0.9134, 0.3437}};
  const V3 o = {p[0], p[1], p[2]};
  int odd = 0;
  for (const auto& d : dirs) {
    int n = 0;
    for (const auto& t : tris) {
      n += ray_tri(o, d, t) ? 1 : 0;
    }
    odd += n & 1;
  }
  return odd >= 2;
}

bool box_fits(const std::vector<Tri>& tris, const ctr_cull::Box& b, double grow) {
  for (const auto& t : tris) {
    if (tri_box_overlap(b, t, grow)) {
      return false;
    }
  }
  const double c[3] = {b.c[0], b.c[1], b.c[2]};
  return point_inside(tris, c);
}

bool inscribed_box(const std::vector<Tri>& tris, const Params& p, ctr_cull::Box* out) {
  if (tris.size() < 4) {
    return false;
  }
  double best = 0;
  bool found = false;
  constexpr double kPi = 3.14159265358979323846;
  for (int a = 0; a < 6; a++) {
    ctr_cull::Box b;
    double vol = 0;
    if (trial(tris, p, a * kPi / 12.0, &b, &vol) && vol > best) {
      best = vol;
      *out = b;
      found = true;
    }
  }
  return found;
}

}  // namespace occluders
