/*!
 * @file horizon.cpp
 * (AI-assisted, horizon) See horizon.h.
 */

#include "horizon.h"

#include <algorithm>
#include <cmath>
#include <set>

namespace horizon {

namespace {
constexpr float kMinUnit = 1024.f, kSpanUnit = 2048.f;  // ctr_cull::kHorizonMinUnit, SpanUnit
constexpr uint16_t kUncovered = 0xffff;
}  // namespace

Grid build(const std::vector<TerrainTri>& tris, const Params& p) {
  Grid g;
  if (tris.empty()) {
    return g;
  }
  float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
  for (const auto& t : tris) {
    for (int c = 0; c < 3; c++) {
      lo[0] = std::min(lo[0], t.p[c][0]);
      hi[0] = std::max(hi[0], t.p[c][0]);
      lo[1] = std::min(lo[1], t.p[c][2]);
      hi[1] = std::max(hi[1], t.p[c][2]);
    }
  }
  float cell = p.cell_m * 4096.f;
  const float ext = std::max(hi[0] - lo[0], hi[1] - lo[1]);
  if (ext / cell > p.max_cells) {
    cell = ext / p.max_cells * 1.001f;
  }
  g.cell = cell;
  g.x0 = lo[0];
  g.z0 = lo[1];
  g.nx = std::max(1, (int)std::ceil((hi[0] - lo[0]) / cell));
  g.nz = std::max(1, (int)std::ceil((hi[1] - lo[1]) / cell));
  const size_t n = (size_t)g.nx * g.nz;
  // the triangles by the cells their box touches; which are walls
  std::vector<std::vector<uint32_t>> bins(n);
  std::vector<uint8_t> steep(tris.size(), 0);
  for (size_t ti = 0; ti < tris.size(); ti++) {
    const auto& t = tris[ti];
    float a[2] = {1e30f, 1e30f}, b[2] = {-1e30f, -1e30f};
    for (int c = 0; c < 3; c++) {
      a[0] = std::min(a[0], t.p[c][0]);
      b[0] = std::max(b[0], t.p[c][0]);
      a[1] = std::min(a[1], t.p[c][2]);
      b[1] = std::max(b[1], t.p[c][2]);
    }
    const int i0 = std::max(0, (int)std::floor((a[0] - g.x0) / cell));
    const int i1 = std::min(g.nx - 1, (int)std::floor((b[0] - g.x0) / cell));
    const int k0 = std::max(0, (int)std::floor((a[1] - g.z0) / cell));
    const int k1 = std::min(g.nz - 1, (int)std::floor((b[1] - g.z0) / cell));
    for (int k = k0; k <= k1; k++) {
      for (int i = i0; i <= i1; i++) {
        bins[(size_t)k * g.nx + i].push_back((uint32_t)ti);
      }
    }
    const double e1[3] = {(double)t.p[1][0] - t.p[0][0], (double)t.p[1][1] - t.p[0][1],
                          (double)t.p[1][2] - t.p[0][2]};
    const double e2[3] = {(double)t.p[2][0] - t.p[0][0], (double)t.p[2][1] - t.p[0][1],
                          (double)t.p[2][2] - t.p[0][2]};
    const double nrm[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                           e1[0] * e2[1] - e1[1] * e2[0]};
    const double len = std::sqrt(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
    steep[ti] = !(len > 0) || std::abs(nrm[1]) < p.steep * len;
  }
  std::vector<float> minh(n, 0.f), maxh(n, 0.f);
  std::vector<uint8_t> cov(n, 0);
  const int s = std::max(2, p.samples);
  std::vector<uint32_t> classes;
  for (int k = 0; k < g.nz; k++) {
    for (int i = 0; i < g.nx; i++) {
      const size_t id = (size_t)k * g.nx + i;
      const auto& list = bins[id];
      if (list.empty()) {
        continue;
      }
      std::set<uint32_t> cls;
      float mn = 1e30f, mx = -1e30f;
      for (uint32_t ti : list) {
        cls.insert(tris[ti].cls);
        mn = std::min(mn, tris[ti].ymin);
        mx = std::max(mx, tris[ti].ymax);
      }
      bool ok = true;
      for (int sz = 0; sz < s && ok; sz++) {
        for (int sx = 0; sx < s && ok; sx++) {
          const double x = g.x0 + (i + 0.02 + 0.96 * sx / (s - 1)) * cell;
          const double z = g.z0 + (k + 0.02 + 0.96 * sz / (s - 1)) * cell;
          for (uint32_t c : cls) {
            bool covered = false;
            double hlo = 1e300, hhi = -1e300;
            for (uint32_t ti : list) {
              const auto& t = tris[ti];
              if (t.cls != c) {
                continue;
              }
              const double ax = t.p[0][0], az = t.p[0][2];
              const double bx = t.p[1][0] - ax, bz = t.p[1][2] - az;
              const double cx = t.p[2][0] - ax, cz = t.p[2][2] - az;
              const double d = bx * cz - cx * bz;
              if (std::abs(d) < 1e-9) {
                continue;  // a wall seen from above: covers nothing
              }
              const double u = ((x - ax) * cz - cx * (z - az)) / d;
              const double v = (bx * (z - az) - (x - ax) * bz) / d;
              constexpr double e = 1e-6;
              if (u < -e || v < -e || u + v > 1 + e) {
                continue;
              }
              covered = true;
              if (!steep[ti]) {
                const double y = t.p[0][1] + u * (t.p[1][1] - t.p[0][1]) + v * (t.p[2][1] - t.p[0][1]);
                hlo = std::min(hlo, y);
                hhi = std::max(hhi, y);
              }
            }
            if (!covered || (hhi > hlo && hhi - hlo > p.layer_m * 4096.0)) {
              ok = false;
              break;
            }
          }
        }
      }
      if (ok) {
        cov[id] = 1;
        minh[id] = mn;
        maxh[id] = mx;
      }
    }
  }
  float base = 1e30f;
  for (size_t id = 0; id < n; id++) {
    if (cov[id]) {
      base = std::min(base, minh[id]);
    }
  }
  g.minh.assign(n, kUncovered);
  g.span.assign(n, 255);
  if (base > 1e29f) {
    return g;
  }
  g.base_y = std::floor(base);
  for (size_t id = 0; id < n; id++) {
    if (!cov[id]) {
      continue;
    }
    const double q = std::floor((minh[id] - g.base_y) / kMinUnit);
    if (q < 0 || q >= kUncovered) {
      continue;
    }
    g.minh[id] = (uint16_t)q;
    const double lo_w = g.base_y + q * kMinUnit;
    g.span[id] = (uint8_t)std::clamp(std::ceil((maxh[id] - lo_w) / kSpanUnit), 0.0, 255.0);
    g.covered++;
  }
  return g;
}

}  // namespace horizon
