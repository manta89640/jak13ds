#pragma once

// (AI-assisted) Trim a planar sprite in homogeneous space, preserving perspective and clipping.
#include <algorithm>
#include <cmath>

#include "game/graphics/ctr/ctr_gpu.h"

inline bool ctr_trim_clip_quad(ctr_clip_vertex quad[4],
                               int k00, int k10, int k01,
                               const int select[4][2],
                               const float box[4],
                               const float st[4]) {
  if (k00 < 0 || k10 < 0 || k01 < 0 || k00 > 3 || k10 > 3 || k01 > 3 ||
      k00 == k10 || k00 == k01 || k10 == k01) {
    return false;
  }
  const int k11 = 6 - k00 - k10 - k01;
  const auto& a = quad[k00];
  const auto& b = quad[k10];
  const auto& c = quad[k01];
  const auto& d = quad[k11];
  const float origin[4] = {a.x, a.y, a.z, a.w};
  const float u[4] = {b.x - a.x, b.y - a.y, b.z - a.z, b.w - a.w};
  const float v[4] = {c.x - a.x, c.y - a.y, c.z - a.z, c.w - a.w};
  const float fourth[4] = {d.x, d.y, d.z, d.w};
  for (int i = 0; i < 4; i++) {
    const float expect = origin[i] + u[i] + v[i];
    const float scale = std::max({1.f, std::abs(origin[i]), std::abs(u[i]), std::abs(v[i])});
    if (!std::isfinite(expect) || !std::isfinite(fourth[i]) ||
        std::abs(expect - fourth[i]) > 1e-5f * scale) {
      return false;  // non-affine quad: keep its original triangulation
    }
  }
  ctr_clip_vertex trimmed[4]{};
  for (int k = 0; k < 4; k++) {
    const int ss = select[k][0], ts = select[k][1];
    const float fs = box[ss], ft = box[ts];
    trimmed[k].x = origin[0] + fs * u[0] + ft * v[0];
    trimmed[k].y = origin[1] + fs * u[1] + ft * v[1];
    trimmed[k].z = origin[2] + fs * u[2] + ft * v[2];
    trimmed[k].w = origin[3] + fs * u[3] + ft * v[3];
    trimmed[k].s = st[ss];
    trimmed[k].t = st[ts];
  }
  for (int k = 0; k < 4; k++) {
    quad[k].x = trimmed[k].x;
    quad[k].y = trimmed[k].y;
    quad[k].z = trimmed[k].z;
    quad[k].w = trimmed[k].w;
    quad[k].s = trimmed[k].s;
    quad[k].t = trimmed[k].t;
  }
  return true;
}
