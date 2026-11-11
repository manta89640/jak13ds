#pragma once

/*!
 * @file CtrCull.h
 * (AI-assisted) Culling math of the 3DS level renderer that has no 3DS dependencies: used by
 * CtrLevel.cpp (draw_level), by the level converter (tools/ctr_level_converter) and by
 * test/ctr_render. World units are the game's (4096 = 1 meter).
 *
 * Occluders (config.ini `occluders`, docs/3ds-port/c3l_format.md "Occluders"): an oriented box
 * that lies inside the solid geometry of a closed tie surface (a rock, a house). Every ray from the
 * camera that enters the box has crossed that surface first, so whatever lies behind the box (seen
 * from the camera) is hidden. make_shadow() turns a box and the camera into the planes of the
 * region it hides; a sphere is hidden when it lies inside all of them.
 */

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ctr_cull {

inline float dot3(const float* a, const float* b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
inline void cross3(const float* a, const float* b, float* out) {
  out[0] = a[1] * b[2] - a[2] * b[1];
  out[1] = a[2] * b[0] - a[0] * b[2];
  out[2] = a[0] * b[1] - a[1] * b[0];
}

//! An oriented box: center, unit axes (rows, right handed), half extents along them.
struct Box {
  float c[3];
  float ax[3][3];
  float h[3];
};

/*!
 * The region a box hides from an eye: the cone of the rays that hit the box, beyond the faces
 * that face the eye. Planes relative to the eye: a point p is in the region when
 * dot(n, p - eye) - d >= 0 for each plane (n unit length). Up to 3 front faces and 6 silhouette
 * edges.
 */
struct Shadow {
  float eye[3];
  float pl[9][4];  // n.x, n.y, n.z, d
  int n = 0;
};

/*!
 * The planes of what box b hides from eye. False when the eye is inside the box grown by
 * `margin` (no region: from inside the solid, or too close for float precision) or the box is
 * degenerate.
 */
inline bool make_shadow(const Box& b, const float* eye, float margin, Shadow* out) {
  float rel[3] = {eye[0] - b.c[0], eye[1] - b.c[1], eye[2] - b.c[2]};
  float e[3];
  int s[3];
  bool outside = false;
  for (int i = 0; i < 3; i++) {
    if (!(b.h[i] > 0.f)) {
      return false;
    }
    e[i] = dot3(b.ax[i], rel);
    s[i] = e[i] > b.h[i] ? 1 : (e[i] < -b.h[i] ? -1 : 0);
    outside = outside || std::fabs(e[i]) > b.h[i] + margin;
  }
  if (!outside) {
    return false;
  }
  for (int i = 0; i < 3; i++) {
    out->eye[i] = eye[i];
  }
  out->n = 0;
  // the faces that face the eye: the region is beyond each (the box's side of its plane)
  for (int i = 0; i < 3; i++) {
    if (!s[i]) {
      continue;
    }
    float* p = out->pl[out->n++];
    for (int k = 0; k < 3; k++) {
      p[k] = -(float)s[i] * b.ax[i][k];
    }
    // dot(n, face point - eye) with face point = c + s h ax: -s (ax . (c - eye)) - h
    p[3] = (float)s[i] * e[i] - b.h[i];
  }
  // the silhouette: the edges between a face that faces the eye and one that doesn't. An edge
  // along axis k joins face (i, si) and face (j, sj).
  for (int k = 0; k < 3; k++) {
    const int i = (k + 1) % 3, j = (k + 2) % 3;
    for (int si = -1; si <= 1; si += 2) {
      for (int sj = -1; sj <= 1; sj += 2) {
        const bool fi = s[i] == si, fj = s[j] == sj;
        if (fi == fj) {
          continue;
        }
        // the edge's ends relative to the eye
        float a[3], c[3];
        for (int m = 0; m < 3; m++) {
          const float mid = b.c[m] + si * b.h[i] * b.ax[i][m] + sj * b.h[j] * b.ax[j][m] - eye[m];
          a[m] = mid - b.h[k] * b.ax[k][m];
          c[m] = mid + b.h[k] * b.ax[k][m];
        }
        float* p = out->pl[out->n];
        cross3(a, c, p);
        const float len = std::sqrt(dot3(p, p));
        if (!(len > 0.f)) {
          return false;
        }
        const float inv = 1.f / len;
        // the box's center on the inner side
        const float side = -dot3(p, rel);  // dot(p, c - eye)
        const float sg = side >= 0.f ? inv : -inv;
        for (int m = 0; m < 3; m++) {
          p[m] *= sg;
        }
        p[3] = 0.f;
        out->n++;
      }
    }
  }
  return out->n > 0;
}

//! A sphere (center c, radius r) against a shadow: 2 all of it hidden, 0 none of it, 1 partly.
inline int classify(const Shadow& sh, const float* c, float r) {
  const float rel[3] = {c[0] - sh.eye[0], c[1] - sh.eye[1], c[2] - sh.eye[2]};
  int res = 2;
  for (int i = 0; i < sh.n; i++) {
    const float d = dot3(sh.pl[i], rel) - sh.pl[i][3];
    if (d < -r) {
      return 0;
    }
    if (d < r) {
      res = 1;
    }
  }
  return res;
}

//! classify(...) == 2, with an early out at the first plane that fails it
inline bool hidden(const Shadow& sh, const float* c, float r) {
  const float rel[3] = {c[0] - sh.eye[0], c[1] - sh.eye[1], c[2] - sh.eye[2]};
  for (int i = 0; i < sh.n; i++) {
    if (dot3(sh.pl[i], rel) - sh.pl[i][3] < r) {
      return false;
    }
  }
  return true;
}

//! Does the segment from p0 to p1 pass through the box (slab test)? Tests use it to check shadows.
inline bool segment_hits_box(const Box& b, const float* p0, const float* p1) {
  float t0 = 0.f, t1 = 1.f;
  for (int i = 0; i < 3; i++) {
    const float d0[3] = {p0[0] - b.c[0], p0[1] - b.c[1], p0[2] - b.c[2]};
    const float dir[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
    const float o = dot3(b.ax[i], d0), v = dot3(b.ax[i], dir);
    if (std::fabs(v) < 1e-12f) {
      if (std::fabs(o) > b.h[i]) {
        return false;
      }
      continue;
    }
    float a = (-b.h[i] - o) / v, c = (b.h[i] - o) / v;
    if (a > c) {
      const float t = a;
      a = c;
      c = t;
    }
    t0 = a > t0 ? a : t0;
    t1 = c < t1 ? c : t1;
    if (t0 > t1) {
      return false;
    }
  }
  return true;
}

//! How big a box looks from a distance d: the sum of its three face areas over d^2.
inline float box_score(const Box& b, float d) {
  const float a = b.h[0] * b.h[1] + b.h[1] * b.h[2] + b.h[2] * b.h[0];
  return a / std::fmax(d * d, 1.f);
}

/*
 * (AI-assisted, horizon) Horizon culling (config.ini `horizon`, docs/3ds-port/culling.md "2."):
 * the level's terrain as a grid of cells (c3l::ExtHorizon). A covered cell's opaque tfrag covers
 * all of it in every version that may be drawn, in one layer, and lies nowhere below minh. In a
 * vertical half-plane through the camera along which every cell is covered, the terrain is a
 * continuous sheet that starts below the camera: a ray that is below a cell's minh there has
 * crossed it. The horizon keeps, per direction (azimuth bin), the highest elevation angle below
 * which every ray is under some covered cell's minh before radius R; a sphere entirely beyond R
 * and below the horizon in all of its bins is hidden.
 */
constexpr int kHorizonBins = 128;
constexpr float kPi = 3.14159265358979323846f;
// the grid's height units (game units): minh in quarter meters above base_y, span (maxh - minh) in
// half meters (255: unknown, at least 127.5 m)
constexpr float kHorizonMinUnit = 1024.f, kHorizonSpanUnit = 2048.f;
constexpr uint16_t kHorizonUncovered = 0xffff;

struct HorizonGridView {
  float x0 = 0, z0 = 0, cell = 0, base_y = 0;  // the corner of cell (0, 0), the cell size
  int nx = 0, nz = 0;
  const uint16_t* minh = nullptr;  // [nz][nx]: kHorizonUncovered or quarter meters above base_y
  const uint8_t* span = nullptr;   // [nz][nx]: maxh - minh in half meters (255: or more)
};

struct Horizon {
  float eye[3];
  float R = 0;
  float max_elev = -10.f;
  float elev[kHorizonBins];  // radians; below -pi/2: nothing hidden in the bin
};

inline int horizon_bin_floor(float a) {  // the bin of angle a (any real), not wrapped
  return (int)std::floor((a + kPi) * (kHorizonBins / (2.f * kPi)));
}
inline int horizon_wrap(int b) {
  b %= kHorizonBins;
  return b < 0 ? b + kHorizonBins : b;
}

/*!
 * The horizon of the covered cells within R (horizontal, game units) of the eye. False (nothing
 * hidden) when the eye isn't over a covered cell, above its highest point by `clear` or more.
 */
inline bool build_horizon(const HorizonGridView& g, const float* eye, float R, float clear,
                          Horizon* out) {
  out->max_elev = -10.f;
  for (int b = 0; b < kHorizonBins; b++) {
    out->elev[b] = -10.f;
  }
  if (!g.nx || !g.nz || !(g.cell > 0) || !(R > 0)) {
    return false;
  }
  out->eye[0] = eye[0];
  out->eye[1] = eye[1];
  out->eye[2] = eye[2];
  out->R = R;
  const int ex = (int)std::floor((eye[0] - g.x0) / g.cell);
  const int ez = (int)std::floor((eye[2] - g.z0) / g.cell);
  if (ex < 0 || ez < 0 || ex >= g.nx || ez >= g.nz) {
    return false;
  }
  {
    const int id = ez * g.nx + ex;
    if (g.minh[id] == kHorizonUncovered || g.span[id] == 255) {
      return false;
    }
    const float maxh = g.base_y + g.minh[id] * kHorizonMinUnit + g.span[id] * kHorizonSpanUnit;
    if (!(eye[1] > maxh + clear)) {
      return false;
    }
  }
  float reach[kHorizonBins];
  for (int b = 0; b < kHorizonBins; b++) {
    reach[b] = R;
  }
  const int k = (int)std::ceil(R / g.cell) + 1;
  const float w = 2.f * kPi / kHorizonBins;
  // a cell's distances and azimuth span from the eye; full: the eye is in it
  auto geom = [&](int i, int j, float* dmin, float* dmax, float* a0, float* a1, bool* full) {
    const float x0 = g.x0 + i * g.cell - eye[0], x1 = x0 + g.cell;
    const float z0 = g.z0 + j * g.cell - eye[2], z1 = z0 + g.cell;
    const float nx = x0 > 0 ? x0 : (x1 < 0 ? x1 : 0.f);
    const float nz = z0 > 0 ? z0 : (z1 < 0 ? z1 : 0.f);
    *dmin = std::sqrt(nx * nx + nz * nz);
    const float fx = std::fmax(std::fabs(x0), std::fabs(x1)), fz = std::fmax(std::fabs(z0), std::fabs(z1));
    *dmax = std::sqrt(fx * fx + fz * fz);
    *full = nx == 0.f && nz == 0.f;
    if (*full) {
      *a0 = -kPi;
      *a1 = kPi;
      return;
    }
    const float ac = std::atan2((z0 + z1) * 0.5f, (x0 + x1) * 0.5f);
    float lo = 0, hi = 0;
    const float cx[4] = {x0, x1, x0, x1}, cz[4] = {z0, z0, z1, z1};
    for (int c = 0; c < 4; c++) {
      float d = std::atan2(cz[c], cx[c]) - ac;
      d = d > kPi ? d - 2.f * kPi : (d < -kPi ? d + 2.f * kPi : d);
      lo = std::fmin(lo, d);
      hi = std::fmax(hi, d);
    }
    *a0 = ac + lo;
    *a1 = ac + hi;
  };
  // 1: how far each bin's rays stay over covered cells (an uncovered cell or the grid's edge
  // anywhere in the bin ends it)
  for (int j = ez - k; j <= ez + k; j++) {
    for (int i = ex - k; i <= ex + k; i++) {
      const bool inside = i >= 0 && j >= 0 && i < g.nx && j < g.nz;
      if (inside && g.minh[j * g.nx + i] != kHorizonUncovered) {
        continue;
      }
      float dmin, dmax, a0, a1;
      bool full;
      geom(i, j, &dmin, &dmax, &a0, &a1, &full);
      if (dmin >= R) {
        continue;
      }
      const int b0 = horizon_bin_floor(a0), b1 = horizon_bin_floor(a1);
      for (int b = b0; b <= b1 && b < b0 + kHorizonBins; b++) {
        float& r = reach[horizon_wrap(b)];
        r = std::fmin(r, dmin);
      }
    }
  }
  // 2: each covered cell within R raises the bins whose rays all cross it, if they reach it
  for (int j = ez - k; j <= ez + k; j++) {
    for (int i = ex - k; i <= ex + k; i++) {
      if (i < 0 || j < 0 || i >= g.nx || j >= g.nz) {
        continue;
      }
      const uint16_t m = g.minh[j * g.nx + i];
      if (m == kHorizonUncovered) {
        continue;
      }
      float dmin, dmax, a0, a1;
      bool full;
      geom(i, j, &dmin, &dmax, &a0, &a1, &full);
      if (dmax > R) {
        continue;
      }
      const float dy = g.base_y + m * kHorizonMinUnit - eye[1];
      const float e = std::fmin(std::atan2(dy, std::fmax(dmin, 1.f)), std::atan2(dy, dmax));
      // the bins entirely within [a0, a1]
      const int f0 = full ? 0 : (int)std::ceil((a0 + kPi) / w - 1e-4f);
      const int f1 = full ? kHorizonBins - 1 : (int)std::floor((a1 + kPi) / w + 1e-4f) - 1;
      for (int b = f0; b <= f1 && b < f0 + kHorizonBins; b++) {
        const int wb = horizon_wrap(b);
        if (reach[wb] >= dmax && e > out->elev[wb]) {
          out->elev[wb] = e;
        }
      }
    }
  }
  for (int b = 0; b < kHorizonBins; b++) {
    out->max_elev = std::fmax(out->max_elev, out->elev[b]);
  }
  return out->max_elev > -kPi;
}

//! A sphere against a horizon: 2 hidden, 1 partly below it, 0 not (or within R).
inline int horizon_classify(const Horizon& h, const float* c, float r) {
  const float dx = c[0] - h.eye[0], dy = c[1] - h.eye[1], dz = c[2] - h.eye[2];
  const float rc = std::sqrt(dx * dx + dz * dz);
  if (!(rc - r >= h.R)) {
    return 0;
  }
  const float d = std::sqrt(rc * rc + dy * dy);
  const float ang = std::asin(std::fmin(1.f, r / d));
  const float ec = std::atan2(dy, rc);
  if (ec - ang >= h.max_elev) {
    return 0;
  }
  const float emax = ec + ang;
  const float az = std::atan2(dz, dx), da = std::asin(std::fmin(1.f, r / rc));
  const int b0 = horizon_bin_floor(az - da), b1 = horizon_bin_floor(az + da);
  bool all = true, any = false;
  for (int b = b0; b <= b1 && b < b0 + kHorizonBins; b++) {
    const float e = h.elev[horizon_wrap(b)];
    all = all && emax < e;
    any = any || ec - ang < e;
  }
  return all ? 2 : (any ? 1 : 0);
}

/*
 * (AI-assisted, depthreuse) The depth of an earlier frame (config.ini `depth_reuse`,
 * docs/3ds-port/culling.md "4."): the top screen in 8x8 blocks (50 x 30, rows from the top), each
 * block's farthest and nearest depth as the GPU stored it (0 far .. 1 near: 0.5 (1 + z/w) of the
 * GL clip space the level renderer draws in), and that frame's camera. A sphere all of whose
 * points were farther than the farthest depth of every block it covered was hidden then.
 */
constexpr int kDepthBX = 50, kDepthBY = 30;

struct DepthSnap {
  float m[16];  // world -> GL clip, row major: clip_r = m[4r] p.x + m[4r+1] p.y + m[4r+2] p.z + m[4r+3]
  float eye[3];
  float zfar[kDepthBX * kDepthBY];
  float znear[kDepthBX * kDepthBY];
};

inline void depth_clip(const float* m, const float* p, float* c) {
  for (int r = 0; r < 4; r++) {
    c[r] = m[4 * r] * p[0] + m[4 * r + 1] * p[1] + m[4 * r + 2] * p[2] + m[4 * r + 3];
  }
}

//! The stored depth of a point at view distance d straight along the view (the w gradient).
inline float depth_at_distance(const DepthSnap& s, float d) {
  const float* g = s.m + 12;
  const float gl = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
  if (!(gl > 0)) {
    return 1.f;
  }
  const float p[3] = {s.eye[0] + d * g[0] / gl, s.eye[1] + d * g[1] / gl, s.eye[2] + d * g[2] / gl};
  float c[4];
  depth_clip(s.m, p, c);
  return c[3] > 0 ? 0.5f * (1.f + c[2] / c[3]) : 1.f;
}

/*!
 * Was the sphere hidden in that frame: on screen there, behind the farthest depth of the blocks it
 * covered (by `eps`), with nothing in those blocks nearer than stored depth `z_limit` (things near
 * the camera may have moved since: Jak, an enemy)? 2: behind all of its blocks, 1: behind some of
 * them (its pieces may be), 0: none (or off screen, or reaching behind the camera).
 */
inline int depth_test(const DepthSnap& s, const float* c, float r, float z_limit, float eps) {
  float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
  for (int k = 0; k < 8; k++) {
    const float p[3] = {c[0] + (k & 1 ? r : -r), c[1] + (k & 2 ? r : -r), c[2] + (k & 4 ? r : -r)};
    float q[4];
    depth_clip(s.m, p, q);
    if (!(q[3] > 1e-3f)) {
      return 0;
    }
    const float sx = (q[0] / q[3] + 1.f) * (kDepthBX * 4.f);
    const float sy = (1.f - q[1] / q[3]) * (kDepthBY * 4.f);
    x0 = std::fmin(x0, sx);
    x1 = std::fmax(x1, sx);
    y0 = std::fmin(y0, sy);
    y1 = std::fmax(y1, sy);
  }
  const int bx0 = std::max(0, (int)std::floor(x0 / 8.f)), bx1 = std::min(kDepthBX - 1, (int)std::floor(x1 / 8.f));
  const int by0 = std::max(0, (int)std::floor(y0 / 8.f)), by1 = std::min(kDepthBY - 1, (int)std::floor(y1 / 8.f));
  if (bx0 > bx1 || by0 > by1) {
    return 0;
  }
  // the sphere's nearest point: the least w
  const float* g = s.m + 12;
  const float gl = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
  if (!(gl > 0)) {
    return 0;
  }
  const float p[3] = {c[0] - r * g[0] / gl, c[1] - r * g[1] / gl, c[2] - r * g[2] / gl};
  float q[4];
  depth_clip(s.m, p, q);
  if (!(q[3] > 1e-3f)) {
    return 0;
  }
  const float zn = 0.5f * (1.f + q[2] / q[3]);
  bool all = true, any = false;
  for (int by = by0; by <= by1; by++) {
    for (int bx = bx0; bx <= bx1; bx++) {
      const int b = by * kDepthBX + bx;
      const bool h = zn < s.zfar[b] - eps && s.znear[b] < z_limit;
      all = all && h;
      any = any || h;
    }
  }
  return all ? 2 : (any ? 1 : 0);
}
inline bool depth_hidden(const DepthSnap& s, const float* c, float r, float z_limit, float eps) {
  return depth_test(s, c, r, z_limit, eps) == 2;
}

//! The camera position of a world -> clip matrix: where x, y and w are 0.
inline bool depth_eye(const float* m, float* eye) {
  const float* a = m;       // x row
  const float* b = m + 4;   // y row
  const float* c = m + 12;  // w row
  const float det = a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) +
                    a[2] * (b[0] * c[1] - b[1] * c[0]);
  if (!(std::fabs(det) > 1e-20f)) {
    return false;
  }
  const float r0 = -a[3], r1 = -b[3], r2 = -c[3];
  eye[0] = (r0 * (b[1] * c[2] - b[2] * c[1]) - a[1] * (r1 * c[2] - b[2] * r2) +
            a[2] * (r1 * c[1] - b[1] * r2)) / det;
  eye[1] = (a[0] * (r1 * c[2] - b[2] * r2) - r0 * (b[0] * c[2] - b[2] * c[0]) +
            a[2] * (b[0] * r2 - r1 * c[0])) / det;
  eye[2] = (a[0] * (b[1] * r2 - r1 * c[1]) - a[1] * (b[0] * r2 - r1 * c[0]) +
            r0 * (b[0] * c[1] - b[1] * c[0])) / det;
  return true;
}

//! The blocks of a depth buffer of kDepthBX x kDepthBY blocks' pixels (rows from the top).
inline void depth_blocks(const float* depth, float* zfar, float* znear) {
  constexpr int w = kDepthBX * 8;
  for (int by = 0; by < kDepthBY; by++) {
    for (int bx = 0; bx < kDepthBX; bx++) {
      float lo = 1e30f, hi = -1e30f;
      for (int y = by * 8; y < by * 8 + 8; y++) {
        for (int x = bx * 8; x < bx * 8 + 8; x++) {
          lo = std::fmin(lo, depth[y * w + x]);
          hi = std::fmax(hi, depth[y * w + x]);
        }
      }
      zfar[by * kDepthBX + bx] = lo;
      znear[by * kDepthBX + bx] = hi;
    }
  }
}

/*!
 * (AI-assisted, sizecull) An upper bound of how many pixels across a sphere looks: `focal` pixels per
 * unit of size at unit depth, `fwd` the view's unit direction. A sphere of center depth z, distance
 * d spans at most 2 r f d / (z (z - r)) along the screen (the off-axis stretch d / z included).
 * Reaching to the camera's plane: huge.
 */
inline float sphere_px(const float* c, float r, const float* eye, const float* fwd, float focal) {
  const float v[3] = {c[0] - eye[0], c[1] - eye[1], c[2] - eye[2]};
  const float z = dot3(v, fwd);
  if (!(z - r > 1e-3f * (r + 1.f))) {
    return 1e30f;
  }
  const float d = std::sqrt(dot3(v, v));
  return 2.f * r * focal * d / (z * (z - r));
}

/*
 * (AI-assisted, facecull) Normal cones (config.ini `cone_cull`, docs/3ds-port/culling.md "10."): a
 * run of back face culled triangles whose normals ((p1 - p0) x (p2 - p0), the converter's outward
 * side) are all within angle a of an axis. Seen from the camera every triangle faces away when, with
 * the run's sphere (c, r) and v = c - eye at angle t from the axis, |v| cos(t + a) > r: each normal
 * n is within t + a of v's direction, so n . (p - eye) >= n . v - r > 0 for every point p of the
 * run. Packed in a u32: the axis as three s8 (x 127), the cosine of a (s8, x 127, rounded down);
 * cos byte -128: no cone.
 */
constexpr uint32_t kNoCone = 0x80000000u;

inline uint32_t cone_pack(const float* axis, float cos_a) {
  uint32_t out = 0;
  for (int k = 0; k < 3; k++) {
    const int q = (int)std::lround(std::fmax(-127.f, std::fmin(127.f, axis[k] * 127.f)));
    out |= (uint32_t)(uint8_t)(int8_t)q << (8 * k);
  }
  const int qc = (int)std::floor(std::fmax(-127.f, std::fmin(127.f, cos_a * 127.f)));
  return out | (uint32_t)(uint8_t)(int8_t)qc << 24;
}
inline bool cone_unpack(uint32_t p, float* axis, float* cos_a) {
  const int8_t qc = (int8_t)(uint8_t)(p >> 24);
  if (qc == -128) {
    return false;
  }
  float l = 0;
  for (int k = 0; k < 3; k++) {
    axis[k] = (float)(int8_t)(uint8_t)(p >> (8 * k)) / 127.f;
    l += axis[k] * axis[k];
  }
  if (!(l > 0)) {
    return false;
  }
  l = 1.f / std::sqrt(l);
  for (int k = 0; k < 3; k++) {
    axis[k] *= l;
  }
  *cos_a = qc / 127.f;
  return true;
}

/*!
 * The cone of triangles (9 floats each: three corners), its half angle grown by `grow` radians and
 * checked against the packed (quantized) axis. kNoCone when it is 80 degrees or wider, or there is
 * no triangle with an area.
 */
inline uint32_t make_cone(const float* tris, size_t n, float grow) {
  double sum[3] = {0, 0, 0};
  size_t used = 0;
  for (size_t i = 0; i < n; i++) {
    const float* t = tris + 9 * i;
    const double e1[3] = {(double)t[3] - t[0], (double)t[4] - t[1], (double)t[5] - t[2]};
    const double e2[3] = {(double)t[6] - t[0], (double)t[7] - t[1], (double)t[8] - t[2]};
    const double c[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                         e1[0] * e2[1] - e1[1] * e2[0]};
    const double l = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
    if (!(l > 0)) {
      continue;
    }
    for (int k = 0; k < 3; k++) {
      sum[k] += c[k] / l;
    }
    used++;
  }
  const double sl = std::sqrt(sum[0] * sum[0] + sum[1] * sum[1] + sum[2] * sum[2]);
  if (!used || !(sl > 1e-9)) {
    return kNoCone;
  }
  float axis[3] = {(float)(sum[0] / sl), (float)(sum[1] / sl), (float)(sum[2] / sl)};
  // the axis as it will be read, and the widest normal from it
  float qa[3], dummy;
  if (!cone_unpack(cone_pack(axis, 1.f), qa, &dummy)) {
    return kNoCone;
  }
  double min_dot = 1;
  for (size_t i = 0; i < n; i++) {
    const float* t = tris + 9 * i;
    const double e1[3] = {(double)t[3] - t[0], (double)t[4] - t[1], (double)t[5] - t[2]};
    const double e2[3] = {(double)t[6] - t[0], (double)t[7] - t[1], (double)t[8] - t[2]};
    const double c[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                         e1[0] * e2[1] - e1[1] * e2[0]};
    const double l = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
    if (!(l > 0)) {
      continue;
    }
    min_dot = std::min(min_dot, (c[0] * qa[0] + c[1] * qa[1] + c[2] * qa[2]) / l);
  }
  const double a = std::acos(std::max(-1.0, std::min(1.0, min_dot))) + grow;
  if (a >= 80.0 * 3.14159265358979 / 180.0) {
    return kNoCone;
  }
  return cone_pack(qa, (float)std::cos(a));
}

//! Does every triangle of the run (inside sphere c, r) face away from the eye?
inline bool cone_away(uint32_t cone, const float* c, float r, const float* eye) {
  float a[3], ca;
  if (!cone_unpack(cone, a, &ca)) {
    return false;
  }
  const float v[3] = {c[0] - eye[0], c[1] - eye[1], c[2] - eye[2]};
  const float d = std::sqrt(dot3(v, v));
  if (!(d > r)) {
    return false;
  }
  const float ct = dot3(v, a) / d;
  const float st = std::sqrt(std::fmax(0.f, 1.f - ct * ct));
  const float sa = std::sqrt(std::fmax(0.f, 1.f - ca * ca));
  return d * (ct * ca - st * sa) > r;
}

}  // namespace ctr_cull
