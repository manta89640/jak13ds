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

}  // namespace ctr_cull
