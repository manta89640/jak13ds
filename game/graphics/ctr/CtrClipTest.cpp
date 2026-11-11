/*!
 * @file CtrClipTest.cpp
 * Boot self-test: what the GPU does with big triangles next to the camera (CtrClipTest.h).
 */

#include "CtrClipTest.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#include "common/log/log.h"

#include "game/graphics/ctr/CtrSettings.h"
#include "game/graphics/ctr/ctr_gpu.h"

namespace {
// camera space (meters): x right, y up, z forward; clip = (kPx x, kPy y, a z + b, z), GL style
// (z = w at the near plane, -w at the far plane), as the level draws' matrices
constexpr float kNear = 0.25f, kFar = 100.f, kPx = 1.f, kPy = 400.f / 240.f;
constexpr float kA = -(kFar + kNear) / (kFar - kNear), kB = 2.f * kFar * kNear / (kFar - kNear);
constexpr uint8_t kBgBlue = 100, kQuadBlue = 200;

ctr_clip_vertex cv(float x, float y, float z) {
  ctr_clip_vertex v{};
  v.x = kPx * x;
  v.y = kPy * y;
  v.z = kA * z + kB;
  v.w = z;
  v.b = kQuadBlue;
  v.a = 0x80;
  return v;
}

struct Case {
  const char* name;
  float half_width, z_back, z_front;  // a ground quad at y = -0.5 m: x in +-half_width, z range
  bool affine = false;                // divided by w on the CPU (w = 1 to the GPU: the ocean's)
  bool cull = false;                  // back faces culled (the quad faces the camera)
  bool near_plane = false;            // ctr_gpu_set_near_clip on
};

// pixels of the quad (blue 200) over a background at 50 m (blue 100), depth tested GEQUAL
int run_case(const Case& c) {
  ctr_gpu_set_skip_color_clear(0);
  ctr_gpu_set_near_clip(c.near_plane ? 1 : 0);
  ctr_gpu_set_cull(0, ctr_settings().cull_flip ? 1 : 0);
  ctr_gpu_frame_begin(0, 0, 0);
  ctr_clip_vertex* v = nullptr;
  uint16_t* idx = nullptr;
  int px = -1;
  if (ctr_gpu_clip_reserve(8, 12, &v, &idx)) {
    // the background: a screen quad at 50 m
    const float bz = kA * 50.f + kB, bw = 50.f;
    const float bx[4] = {-1, 1, -1, 1}, by[4] = {-1, -1, 1, 1};
    for (int i = 0; i < 4; i++) {
      v[i] = ctr_clip_vertex{bx[i] * bw, by[i] * bw, bz, bw, 0, 0, 0, 0, kBgBlue, 0x80};
    }
    // the ground: counter-clockwise on screen seen from above (front face)
    const float y = -0.5f;
    v[4] = cv(-c.half_width, y, c.z_back);
    v[5] = cv(c.half_width, y, c.z_back);
    v[6] = cv(-c.half_width, y, c.z_front);
    v[7] = cv(c.half_width, y, c.z_front);
    if (c.affine) {
      for (int i = 4; i < 8; i++) {
        const float w = v[i].w;
        v[i].x /= w;
        v[i].y /= w;
        v[i].z /= w;
        v[i].w = 1.f;
      }
    }
    static const uint16_t kIdx[12] = {0, 1, 2, 2, 1, 3, 4, 6, 5, 5, 6, 7};
    memcpy(idx, kIdx, sizeof(kIdx));
    ctr_draw_state st[2];
    memset(st, 0, sizeof(st));
    for (auto& s : st) {
      s.tex = -1;
      s.atest = CTR_TEST_ALWAYS;
      s.ztest = CTR_TEST_GEQUAL;
      s.zwrite = 1;
    }
    st[1].tcc = c.cull ? (uint8_t)((int)CTR_TCC_CULL_BACK | (int)CTR_TCC_CULL_OWN) : 0;
    const int first[2] = {0, 6}, count[2] = {6, 6};
    ctr_gpu_clip_draw_ranges(st, first, count, 2, 8, 12);
  }
  ctr_gpu_frame_end();
  unsigned int blue[256];
  if (ctr_gpu_top_histogram(blue)) {
    px = (int)(blue[kQuadBlue - 1] + blue[kQuadBlue] + blue[kQuadBlue + 1]);
  }
  ctr_gpu_set_near_clip(-1);
  return px;
}
}  // namespace

void ctr_clip_selftest() {
  // the quad's pixels with exact clipping (host soft backend: near and far planes, then the
  // screen), for the cases in order
  static const Case kCases[] = {
      {"in front", 2.f, 0.5f, 20.f},
      {"behind 2 m", 2.f, -2.f, 20.f},
      {"behind 20 m", 20.f, -20.f, 20.f},
      {"behind 200 m", 200.f, -200.f, 60.f},
      {"cut at near", 20.f, kNear * 1.0001f, 20.f},
      {"cut at near w=1", 20.f, kNear * 1.0001f, 20.f, true},
      {"in front culled", 2.f, 0.5f, 20.f, false, true},
      {"behind 20 m culled", 20.f, -20.f, 20.f, false, true},
      {"behind 20 m near plane", 20.f, -20.f, 20.f, false, false, true},
      {"behind 200 m near plane", 200.f, -200.f, 60.f, false, false, true},
  };
  static const int kExpect[] = {37900, 37900, 46000, 47200, 46000,
                                46000, 37900, 46000, 46000, 47200};
  constexpr int n = sizeof(kCases) / sizeof(kCases[0]);
  int got[n];
  for (int i = 0; i < n; i++) {
    got[i] = run_case(kCases[i]);
  }
  if (got[0] < 0) {
    lg::info("[ctr] near clip self-test: not run (the top screen can't be read back)");
    return;
  }
  auto ok = [&](int i) { return std::abs(got[i] - kExpect[i]) * 50 <= kExpect[i] + 50; };
  std::string line;
  for (int i = 0; i < n; i++) {
    line += fmt::format("{}{} {} px ({}{})", i ? ", " : "", kCases[i].name, got[i],
                        kExpect[i], ok(i) ? "" : " WRONG");
  }
  // the PICA's user clip plane at the near plane, where the GPU's own clipping loses pixels the
  // plane gives back (cases 2, 3 against 8, 9)
  const bool lost = !ok(2) || !ok(3);
  const bool fixed = ok(8) && ok(9);
  const bool use = lost && fixed;
  ctr_gpu_set_near_clip_default(use ? 1 : 0);
  // back faces culled: right with the quad in front (the winding is the level draws'), pixels
  // lost with it reaching behind the camera: the facing is decided before the clipping (CtrLevel
  // draws such culled draws both sides)
  const bool pre_clip = ok(6) && !ok(7) && ok(2);
  lg::info("[ctr] near clip self-test (pixels, expected): {} -> near clip plane {}; facing {}",
           line, use ? "on" : (lost ? "doesn't help, off" : "not needed, off"),
           !ok(6) ? "unknown (culled in front)" : pre_clip ? "decided before clipping" : "right");
}
