// (AI-assisted) The culling math of the 3DS level renderer (game/graphics/ctr/CtrCull.h) and the
// converter's culling data (tools/ctr_level_converter), against brute force.

#include <array>
#include <cmath>
#include <random>
#include <vector>

#include "check.h"

#include "game/graphics/ctr/CtrCull.h"
#include "tools/ctr_level_converter/horizon.h"
#include "tools/ctr_level_converter/occluders.h"

#include "fmt/format.h"

namespace {

constexpr float kM = 4096.f;

using Tri = occluders::Tri;

void add_quad(std::vector<Tri>& out, const std::array<double, 3>& a, const std::array<double, 3>& b,
              const std::array<double, 3>& c, const std::array<double, 3>& d) {
  out.push_back({a, b, c});
  out.push_back({a, c, d});
}

// a closed prism: the polygon (x, z) from y0 to y1, its caps as a fan of `fan` triangles
std::vector<Tri> prism(const std::vector<std::array<double, 2>>& poly,
                       const std::vector<std::array<int, 3>>& cap, double y0, double y1,
                       double yaw = 0.0, std::array<double, 3> at = {0, 0, 0}) {
  auto P = [&](int i, double y) {
    const double x = poly[i][0], z = poly[i][1];
    const double c = std::cos(yaw), s = std::sin(yaw);
    return std::array<double, 3>{at[0] + c * x - s * z, at[1] + y, at[2] + s * x + c * z};
  };
  std::vector<Tri> t;
  for (const auto& f : cap) {
    t.push_back({P(f[0], y0), P(f[1], y0), P(f[2], y0)});
    t.push_back({P(f[0], y1), P(f[2], y1), P(f[1], y1)});
  }
  const int n = (int)poly.size();
  for (int i = 0; i < n; i++) {
    const int j = (i + 1) % n;
    add_quad(t, P(i, y0), P(j, y0), P(j, y1), P(i, y1));
  }
  return t;
}

std::vector<Tri> cube(double half, double yaw, std::array<double, 3> at) {
  const std::vector<std::array<double, 2>> sq = {{-half, -half}, {half, -half}, {half, half}, {-half, half}};
  return prism(sq, {{0, 1, 2}, {0, 2, 3}}, -half, half, yaw, at);
}

bool corners_inside(const std::vector<Tri>& mesh, const ctr_cull::Box& b) {
  for (int k = 0; k < 8; k++) {
    double p[3];
    for (int m = 0; m < 3; m++) {
      p[m] = b.c[m];
      for (int a = 0; a < 3; a++) {
        p[m] += ((k >> a) & 1 ? 1 : -1) * 0.999 * b.h[a] * b.ax[a][m];
      }
    }
    if (!occluders::point_inside(mesh, p)) {
      return false;
    }
  }
  return true;
}

void test_shadow_vs_rays() {
  std::mt19937 rng(1234);
  std::uniform_real_distribution<float> u(-1.f, 1.f);
  int hidden = 0, tested = 0;
  for (int iter = 0; iter < 400; iter++) {
    ctr_cull::Box b;
    // a random orthonormal frame
    float a0[3] = {u(rng), u(rng), u(rng)}, a1[3] = {u(rng), u(rng), u(rng)};
    auto norm = [](float* v) {
      const float l = std::sqrt(ctr_cull::dot3(v, v));
      for (int i = 0; i < 3; i++) {
        v[i] /= l;
      }
    };
    norm(a0);
    float a2[3];
    ctr_cull::cross3(a0, a1, a2);
    norm(a2);
    ctr_cull::cross3(a2, a0, a1);
    for (int i = 0; i < 3; i++) {
      b.ax[0][i] = a0[i];
      b.ax[1][i] = a1[i];
      b.ax[2][i] = a2[i];
      b.c[i] = 1000.f * kM + u(rng) * 50.f * kM;
      b.h[i] = (1.f + 5.f * (u(rng) + 1.f)) * kM;
    }
    float eye[3];
    for (int i = 0; i < 3; i++) {
      eye[i] = b.c[i] + u(rng) * 60.f * kM;
    }
    ctr_cull::Shadow sh;
    if (!ctr_cull::make_shadow(b, eye, 1.f * kM, &sh)) {
      continue;
    }
    check(sh.n >= 5 && sh.n <= 9, "a box's shadow has 5 to 9 planes");
    for (int k = 0; k < 40; k++) {
      // spheres behind the box (along a ray through it) and anywhere
      float c[3];
      const float t = 1.2f + 3.f * (u(rng) + 1.f);
      for (int i = 0; i < 3; i++) {
        c[i] = k & 1 ? eye[i] + t * (b.c[i] + u(rng) * b.h[0] * 0.5f - eye[i])
                     : b.c[i] + u(rng) * 80.f * kM;
      }
      const float r = (0.1f + (u(rng) + 1.f)) * kM;
      const int cls = ctr_cull::classify(sh, c, r);
      check((cls == 2) == ctr_cull::hidden(sh, c, r), "hidden() agrees with classify()");
      tested++;
      if (cls != 2) {
        continue;
      }
      hidden++;
      for (int s = 0; s < 64; s++) {
        float d[3] = {u(rng), u(rng), u(rng)};
        float l = std::sqrt(ctr_cull::dot3(d, d));
        const float rr = s < 32 ? r / l : r * (u(rng) + 1.f) * 0.5f / l;
        float p[3] = {c[0] + d[0] * rr, c[1] + d[1] * rr, c[2] + d[2] * rr};
        check(ctr_cull::segment_hits_box(b, eye, p),
              "every point of a sphere the shadow hides has the box between it and the eye");
      }
    }
  }
  check(hidden > tested / 20, "the shadow test hides some of the spheres behind boxes");
  fmt::print("  occluder shadows: {} of {} spheres hidden, all of them behind their box\n", hidden,
             tested);
}

void test_shadow_eye_inside() {
  ctr_cull::Box b{};
  b.ax[0][0] = b.ax[1][1] = b.ax[2][2] = 1.f;
  b.h[0] = b.h[1] = b.h[2] = 5.f * kM;
  ctr_cull::Shadow sh;
  const float in[3] = {1.f * kM, 0, 0};
  check(!ctr_cull::make_shadow(b, in, 0.f, &sh), "no shadow from inside the box");
  const float near[3] = {6.f * kM, 0, 0};
  check(!ctr_cull::make_shadow(b, near, 3.f * kM, &sh), "no shadow within the margin");
  const float far[3] = {20.f * kM, 0, 0};
  check(ctr_cull::make_shadow(b, far, 3.f * kM, &sh) && sh.n == 5, "one face and four edges");
  const float behind[3] = {-30.f * kM, 0, 0};
  check(ctr_cull::hidden(sh, behind, 1.f * kM), "a sphere straight behind is hidden");
  const float beside[3] = {-30.f * kM, 40.f * kM, 0};
  check(ctr_cull::classify(sh, beside, 1.f * kM) == 0, "a sphere off to the side is not");
}

void test_inscribed_cube() {
  const auto mesh = cube(5.0 * kM, 0.0, {100.0 * kM, 10.0 * kM, -40.0 * kM});
  ctr_cull::Box b;
  check(occluders::inscribed_box(mesh, {}, &b), "a box inside a 10 m cube");
  for (int k = 0; k < 3; k++) {
    check(b.h[k] > 4.0f * kM && b.h[k] < 5.0f * kM, "nearly the whole cube");
  }
  check(occluders::box_fits(mesh, b, 0.0) && corners_inside(mesh, b), "the box is inside");
  // turned 30 degrees: the trial at 30 degrees finds it
  const auto turned = cube(5.0 * kM, 0.5235987756, {0, 0, 0});
  check(occluders::inscribed_box(turned, {}, &b), "a box inside a turned cube");
  check(b.h[0] * b.h[2] > 16.f * kM * kM && occluders::box_fits(turned, b, 0.0) &&
            corners_inside(turned, b),
        "the turned cube's box is big and inside");
}

void test_inscribed_l_shape() {
  // an L: 12 x 4 m and 4 x 12 m, 6 m high
  const double m = kM;
  const std::vector<std::array<double, 2>> l = {{0, 0}, {12 * m, 0}, {12 * m, 4 * m}, {4 * m, 4 * m},
                                                {4 * m, 12 * m}, {0, 12 * m}};
  const auto mesh = prism(l, {{0, 1, 2}, {0, 2, 3}, {0, 3, 4}, {0, 4, 5}}, 0, 6 * m);
  ctr_cull::Box b;
  check(occluders::inscribed_box(mesh, {}, &b), "a box inside an L");
  check(occluders::box_fits(mesh, b, 0.0) && corners_inside(mesh, b),
        "the L's box touches no wall and its corners are inside");
  check(b.h[0] * b.h[2] >= 8.f * kM * kM, "one arm of the L at least");
}

void test_no_box_in_thin_plate() {
  const double m = kM;
  const std::vector<std::array<double, 2>> r = {{0, 0}, {10 * m, 0}, {10 * m, 0.4 * m}, {0, 0.4 * m}};
  const auto mesh = prism(r, {{0, 1, 2}, {0, 2, 3}}, 0, 3 * m);
  ctr_cull::Box b;
  check(!occluders::inscribed_box(mesh, {}, &b), "no occluder in a 40 cm plate");
}

void test_tri_box() {
  ctr_cull::Box b{};
  b.ax[0][0] = b.ax[1][1] = b.ax[2][2] = 1.f;
  b.h[0] = b.h[1] = b.h[2] = 1.f;
  const Tri through = {std::array<double, 3>{-5, 0, 0}, std::array<double, 3>{5, 0, 0.1},
                       std::array<double, 3>{0, 5, 0}};
  const Tri away = {std::array<double, 3>{5, 5, 5}, std::array<double, 3>{6, 5, 5},
                    std::array<double, 3>{5, 6, 5}};
  // a triangle whose box overlaps the box's but that passes beside its corner
  const Tri corner = {std::array<double, 3>{3.2, 0, 0}, std::array<double, 3>{0, 3.2, 0},
                      std::array<double, 3>{0, 0, 3.2}};
  check(occluders::tri_box_overlap(b, through, 0.0), "a triangle through the box");
  check(!occluders::tri_box_overlap(b, away, 0.0), "a triangle away from it");
  check(!occluders::tri_box_overlap(b, corner, 0.0), "a triangle beyond its corner");
  check(occluders::tri_box_overlap(b, corner, 0.2), "... that touches it grown");
}

// (AI-assisted, horizon) a terrain of quads of `q` meters from -100 m to 200 m (x and z), a ridge
// 20 m high around x = 40 m; quads whose center is in the hole box are left out
struct Terrain {
  std::vector<horizon::TerrainTri> tris;
};
float ridge(float x) {
  const float d = std::fabs(x - 40.f * kM) / (12.f * kM);
  return d >= 1.f ? 0.f : 20.f * kM * (1.f - d * d);
}
Terrain make_terrain(float q, const float* hole) {
  Terrain t;
  const float lo = -100.f * kM, hi = 200.f * kM, step = q * kM;
  for (float z = lo; z < hi; z += step) {
    for (float x = lo; x < hi; x += step) {
      const float cx = x + step / 2, cz = z + step / 2;
      if (hole && cx > hole[0] && cx < hole[1] && cz > hole[2] && cz < hole[3]) {
        continue;
      }
      const float p[4][3] = {{x, ridge(x), z},
                             {x + step, ridge(x + step), z},
                             {x + step, ridge(x + step), z + step},
                             {x, ridge(x), z + step}};
      const int f[2][3] = {{0, 1, 2}, {0, 2, 3}};
      for (const auto& tri : f) {
        horizon::TerrainTri tt{};
        tt.ymin = 1e30f;
        tt.ymax = -1e30f;
        for (int c = 0; c < 3; c++) {
          for (int k = 0; k < 3; k++) {
            tt.p[c][k] = p[tri[c]][k];
          }
          tt.ymin = std::fmin(tt.ymin, tt.p[c][1]);
          tt.ymax = std::fmax(tt.ymax, tt.p[c][1]);
        }
        t.tris.push_back(tt);
      }
    }
  }
  return t;
}
// does the segment a -> b cross a terrain triangle before b
bool segment_blocked(const Terrain& t, const float* a, const float* b) {
  const double d[3] = {(double)b[0] - a[0], (double)b[1] - a[1], (double)b[2] - a[2]};
  for (const auto& tri : t.tris) {
    const double e1[3] = {(double)tri.p[1][0] - tri.p[0][0], (double)tri.p[1][1] - tri.p[0][1],
                          (double)tri.p[1][2] - tri.p[0][2]};
    const double e2[3] = {(double)tri.p[2][0] - tri.p[0][0], (double)tri.p[2][1] - tri.p[0][1],
                          (double)tri.p[2][2] - tri.p[0][2]};
    const double pv[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2],
                          d[0] * e2[1] - d[1] * e2[0]};
    const double det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
    if (std::abs(det) < 1e-9) {
      continue;
    }
    const double tv[3] = {(double)a[0] - tri.p[0][0], (double)a[1] - tri.p[0][1],
                          (double)a[2] - tri.p[0][2]};
    const double u = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) / det;
    if (u < 0 || u > 1) {
      continue;
    }
    const double qv[3] = {tv[1] * e1[2] - tv[2] * e1[1], tv[2] * e1[0] - tv[0] * e1[2],
                          tv[0] * e1[1] - tv[1] * e1[0]};
    const double v = (d[0] * qv[0] + d[1] * qv[1] + d[2] * qv[2]) / det;
    if (v < 0 || u + v > 1) {
      continue;
    }
    const double s = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) / det;
    if (s > 0 && s < 1) {
      return true;
    }
  }
  return false;
}
ctr_cull::HorizonGridView view_of(const horizon::Grid& g) {
  ctr_cull::HorizonGridView v;
  v.x0 = g.x0;
  v.z0 = g.z0;
  v.cell = g.cell;
  v.base_y = g.base_y;
  v.nx = g.nx;
  v.nz = g.nz;
  v.minh = g.minh.data();
  v.span = g.span.data();
  return v;
}

int horizon_trial(const float* hole, int* tested) {
  const Terrain t = make_terrain(4.f, hole);
  const horizon::Grid g = horizon::build(t.tris);
  check(g.covered > 0, "the terrain grid covers the terrain");
  ctr_cull::Horizon h;
  const float eye[3] = {0.f, 2.f * kM, 0.f};
  check(ctr_cull::build_horizon(view_of(g), eye, 64.f * kM, 0.5f * kM, &h),
        "a horizon from a camera over the terrain");
  std::mt19937 rng(77);
  std::uniform_real_distribution<float> u(0.f, 1.f);
  int hidden = 0;
  for (int i = 0; i < 300; i++) {
    const float c[3] = {(70.f + 120.f * u(rng)) * kM, 40.f * u(rng) * kM, (-60.f + 120.f * u(rng)) * kM};
    const float r = (0.5f + 4.5f * u(rng)) * kM;
    (*tested)++;
    if (ctr_cull::horizon_classify(h, c, r) != 2) {
      continue;
    }
    hidden++;
    for (int sidx = 0; sidx < 12; sidx++) {
      float d[3] = {u(rng) - 0.5f, u(rng) - 0.5f, u(rng) - 0.5f};
      const float l = std::sqrt(ctr_cull::dot3(d, d));
      const float p[3] = {c[0] + d[0] / l * r, c[1] + d[1] / l * r, c[2] + d[2] / l * r};
      check(segment_blocked(t, eye, p), "a sphere below the horizon is behind the terrain");
    }
  }
  return hidden;
}

void test_horizon() {
  int tested = 0;
  const int hidden = horizon_trial(nullptr, &tested);
  check(hidden > tested / 10, "the ridge hides some of what is behind it");
  // a hole in the ground in front of the ridge: nothing in its directions counts as hidden
  const float hole[4] = {20.f * kM, 26.f * kM, -6.f * kM, 6.f * kM};
  int tested2 = 0;
  const int hidden2 = horizon_trial(hole, &tested2);
  check(hidden2 < hidden, "a hole in the terrain hides less");
  {
    const Terrain t = make_terrain(4.f, hole);
    const horizon::Grid g = horizon::build(t.tris);
    ctr_cull::Horizon h;
    const float eye[3] = {0.f, 2.f * kM, 0.f};
    ctr_cull::build_horizon(view_of(g), eye, 64.f * kM, 0.5f * kM, &h);
    const float behind[3] = {120.f * kM, 1.f * kM, 0.f};
    check(ctr_cull::horizon_classify(h, behind, 1.f * kM) != 2,
          "nothing straight beyond the hole is hidden");
    const float aside[3] = {120.f * kM, 1.f * kM, -40.f * kM};
    check(ctr_cull::horizon_classify(h, aside, 1.f * kM) == 2, "but beside it, still");
  }
  // a camera under the terrain's top (a cave, an overhang) or off the grid: no horizon
  {
    const Terrain t = make_terrain(4.f, nullptr);
    const horizon::Grid g = horizon::build(t.tris);
    ctr_cull::Horizon h;
    const float low[3] = {40.f * kM, 5.f * kM, 0.f};
    check(!ctr_cull::build_horizon(view_of(g), low, 64.f * kM, 0.5f * kM, &h),
          "no horizon from under the ridge's top");
    const float off[3] = {-500.f * kM, 5.f * kM, 0.f};
    check(!ctr_cull::build_horizon(view_of(g), off, 64.f * kM, 0.5f * kM, &h),
          "no horizon off the grid");
  }
  // two layers (an overhang 8 m over the ground): those cells are not covered
  {
    Terrain t = make_terrain(4.f, nullptr);
    const size_t n0 = t.tris.size();
    for (size_t i = 0; i < n0; i++) {
      auto tt = t.tris[i];
      bool in = true;
      for (int c = 0; c < 3; c++) {
        in = in && tt.p[c][0] >= 100.f * kM && tt.p[c][0] <= 120.f * kM && tt.p[c][2] >= 0.f &&
             tt.p[c][2] <= 20.f * kM;
        tt.p[c][1] += 8.f * kM;
      }
      if (in) {
        tt.ymin += 8.f * kM;
        tt.ymax += 8.f * kM;
        t.tris.push_back(tt);
      }
    }
    const horizon::Grid g = horizon::build(t.tris);
    const int i = (int)((110.f * kM - g.x0) / g.cell), k = (int)((10.f * kM - g.z0) / g.cell);
    check(g.minh[k * g.nx + i] == ctr_cull::kHorizonUncovered, "an overhang's cells are not covered");
  }
  fmt::print("  horizon: {} of {} spheres hidden behind the ridge ({} with a hole in front of it)\n",
             hidden, tested, hidden2);
}

}  // namespace

void run_cull_tests() {
  test_shadow_eye_inside();
  test_shadow_vs_rays();
  test_tri_box();
  test_inscribed_cube();
  test_inscribed_l_shape();
  test_no_box_in_thin_plate();
  test_horizon();
}
