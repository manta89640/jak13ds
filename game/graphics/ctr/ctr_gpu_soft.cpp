/*!
 * @file ctr_gpu_soft.cpp
 * (AI-assisted)
 * Software implementation of ctr_gpu.h for PC builds. It follows the 3DS backend's math (see the
 * conventions in ctr_gpu.h) so the 3DS renderer can be tested without an emulator:
 *   gk --ctr-gfx [--ctr-dump DIR --ctr-dump-every N]
 * Not built for the 3DS.
 */

#ifndef __3DS__

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "common/log/log.h"
#include "common/util/FileUtil.h"

#include "fmt/format.h"
#include "game/graphics/ctr/CtrRenderer.h"
#include "game/graphics/ctr/ctr_gpu.h"

namespace {
constexpr int kW = 400;
constexpr int kH = 240;

struct SoftTex {
  bool used = false;
  int w = 0, h = 0;
  std::vector<uint8_t> rgba;
};

struct SoftState {
  std::vector<uint8_t> color;  // RGBA8, top row first
  std::vector<float> depth;
  std::vector<SoftTex> textures;
  int frame = 0;
  std::string dump_dir;
  int dump_every = 0;
  ctr_gpu_stats cur{}, last{};
  std::string screenshot_path;
  std::chrono::steady_clock::time_point next_vblank;
  bool vblank_started = false;
} g_soft;

bool test(uint8_t func, float a, float b) {
  switch (func) {
    case CTR_TEST_NEVER:
      return false;
    case CTR_TEST_ALWAYS:
      return true;
    case CTR_TEST_GEQUAL:
      return a >= b;
    case CTR_TEST_GREATER:
      return a > b;
    case CTR_TEST_LESS:
      return a < b;
    case CTR_TEST_LEQUAL:
      return a <= b;
    case CTR_TEST_EQUAL:
      return a == b;
    case CTR_TEST_NOTEQUAL:
      return a != b;
  }
  return true;
}

void sample(const SoftTex& tex, const ctr_draw_state& st, float s, float t, float out[4]) {
  auto wrap = [](float c, int size, bool clamp) {
    int i = (int)std::floor(c * size);
    if (clamp) {
      return std::clamp(i, 0, size - 1);
    }
    i %= size;
    return i < 0 ? i + size : i;
  };
  int x = wrap(s, tex.w, st.clamp_s);
  int y = wrap(t, tex.h, st.clamp_t);
  const uint8_t* p = &tex.rgba[4 * (x + y * tex.w)];
  for (int i = 0; i < 4; i++) {
    out[i] = p[i];
  }
}

void shade_pixel(const ctr_draw_state& st, int px, int py, float z, const float col[4], float s,
                 float t) {
  // color in GS units (0x80 = 1.0 for vertex color / alpha)
  float r = col[0], g = col[1], b = col[2], a = col[3];
  static const bool force_white = getenv("CTR_SOFT_WHITE") != nullptr;
  if (force_white) {
    r = g = b = 128.f;
  }
  if (st.tex >= 0 && st.tex < (int)g_soft.textures.size() && g_soft.textures[st.tex].used) {
    float tc[4];
    sample(g_soft.textures[st.tex], st, s, t, tc);
    if (st.decal) {
      r = tc[0];
      g = tc[1];
      b = tc[2];
      if (st.tcc) {
        a = tc[3];
      }
    } else {
      r = tc[0] * r / 128.f;
      g = tc[1] * g / 128.f;
      b = tc[2] * b / 128.f;
      if (st.tcc) {
        a = tc[3] * a / 128.f;
      }
    }
  } else {
    // untextured: vertex color 0x80 = full intensity
    r *= 2.f;
    g *= 2.f;
    b *= 2.f;
  }
  if (!test(st.atest, a, (float)st.aref)) {
    return;
  }
  int idx = px + py * kW;
  if (!test(st.ztest, z, g_soft.depth[idx])) {
    return;
  }
  if (st.zwrite) {
    g_soft.depth[idx] = z;
  }
  uint8_t* d = &g_soft.color[4 * idx];
  float as = std::min(a / 128.f, 2.f);
  float src[3] = {std::min(r, 255.f), std::min(g, 255.f), std::min(b, 255.f)};
  for (int i = 0; i < 3; i++) {
    float dst = d[i];
    float o;
    switch (st.blend) {
      case CTR_BLEND_ALPHA:
        o = src[i] * as + dst * (1.f - std::min(as, 1.f));
        break;
      case CTR_BLEND_ADD:
        o = src[i] * as + dst;
        break;
      case CTR_BLEND_SUB:
        o = dst - src[i] * as;
        break;
      case CTR_BLEND_FIX: {
        float f = st.fix / 128.f;
        o = src[i] * f + dst * (1.f - f);
      } break;
      case CTR_BLEND_ADD_DST_A:
        o = src[i] * (d[3] / 128.f) + dst;
        break;
      default:
        o = src[i];
        break;
    }
    d[i] = (uint8_t)std::clamp(o, 0.f, 255.f);
  }
  if (st.blend == CTR_BLEND_OFF) {
    d[3] = (uint8_t)std::clamp(a, 0.f, 255.f);
  }
}

void raster_triangle(const ctr_draw_state& st, const ctr_vertex* v) {
  // to screen: x in [-1, 1] -> [40, 360], y in [-1, 1] -> [240, 0]
  float sx[3], sy[3];
  for (int i = 0; i < 3; i++) {
    sx[i] = 200.f + v[i].x * 160.f;
    sy[i] = 120.f - v[i].y * 120.f;
  }
  float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
  if (std::abs(area) < 1e-6f) {
    return;
  }
  int x0 = std::max(0, (int)std::floor(std::min({sx[0], sx[1], sx[2]})));
  int x1 = std::min(kW - 1, (int)std::ceil(std::max({sx[0], sx[1], sx[2]})));
  int y0 = std::max(0, (int)std::floor(std::min({sy[0], sy[1], sy[2]})));
  int y1 = std::min(kH - 1, (int)std::ceil(std::max({sy[0], sy[1], sy[2]})));
  for (int py = y0; py <= y1; py++) {
    for (int px = x0; px <= x1; px++) {
      float fx = px + 0.5f, fy = py + 0.5f;
      float w0 = ((sx[1] - fx) * (sy[2] - fy) - (sx[2] - fx) * (sy[1] - fy)) / area;
      float w1 = ((sx[2] - fx) * (sy[0] - fy) - (sx[0] - fx) * (sy[2] - fy)) / area;
      float w2 = 1.f - w0 - w1;
      if (w0 < 0 || w1 < 0 || w2 < 0) {
        continue;
      }
      float col[4] = {
          w0 * v[0].r + w1 * v[1].r + w2 * v[2].r, w0 * v[0].g + w1 * v[1].g + w2 * v[2].g,
          w0 * v[0].b + w1 * v[1].b + w2 * v[2].b, w0 * v[0].a + w1 * v[1].a + w2 * v[2].a};
      float z = w0 * v[0].z + w1 * v[1].z + w2 * v[2].z;
      float s = w0 * v[0].s + w1 * v[1].s + w2 * v[2].s;
      float t = w0 * v[0].t + w1 * v[1].t + w2 * v[2].t;
      shade_pixel(st, px, py, z, col, s, t);
    }
  }
}
}  // namespace

extern "C" {

int ctr_gpu_init(void) {
  g_soft.color.assign(kW * kH * 4, 0);
  g_soft.depth.assign(kW * kH, 0.f);
  g_soft.textures.clear();
  return 0;
}

void ctr_gpu_exit(void) {
  g_soft.textures.clear();
}

void ctr_gpu_frame_begin(uint8_t r, uint8_t g, uint8_t b) {
  for (int i = 0; i < kW * kH; i++) {
    g_soft.color[4 * i] = r;
    g_soft.color[4 * i + 1] = g;
    g_soft.color[4 * i + 2] = b;
    g_soft.color[4 * i + 3] = 255;
  }
  std::fill(g_soft.depth.begin(), g_soft.depth.end(), 0.f);
  g_soft.cur = ctr_gpu_stats{};
}

void ctr_gpu_frame_end(void) {
  g_soft.frame++;
  int tex_count = 0;
  unsigned bytes = 0;
  for (auto& t : g_soft.textures) {
    if (t.used) {
      tex_count++;
      bytes += t.rgba.size();
    }
  }
  g_soft.cur.textures = tex_count;
  g_soft.cur.tex_bytes = bytes;
  g_soft.last = g_soft.cur;
  if (!g_soft.screenshot_path.empty()) {
    auto out = g_soft.color;
    for (int i = 0; i < kW * kH; i++) {
      out[4 * i + 3] = 255;
    }
    file_util::write_rgba_png(g_soft.screenshot_path, out.data(), kW, kH);
    g_soft.screenshot_path.clear();
  }
  if (!g_soft.dump_dir.empty() && g_soft.dump_every > 0 && g_soft.frame % g_soft.dump_every == 0) {
    auto out = g_soft.color;
    for (int i = 0; i < kW * kH; i++) {
      out[4 * i + 3] = 255;
    }
    file_util::write_rgba_png(fs::path(g_soft.dump_dir) / fmt::format("frame_{:05d}.png", g_soft.frame),
                              out.data(), kW, kH);
  }
}

void ctr_gpu_wait_vblank(void) {
  using clock = std::chrono::steady_clock;
  constexpr auto kFrame = std::chrono::nanoseconds(1000000000LL / 60);
  auto now = clock::now();
  if (!g_soft.vblank_started || g_soft.next_vblank < now - 4 * kFrame) {
    g_soft.vblank_started = true;
    g_soft.next_vblank = now;
  }
  g_soft.next_vblank += kFrame;
  std::this_thread::sleep_until(g_soft.next_vblank);
}

int ctr_gpu_tex_create(int w, int h, const uint8_t* rgba) {
  int slot = -1;
  for (size_t i = 0; i < g_soft.textures.size(); i++) {
    if (!g_soft.textures[i].used) {
      slot = (int)i;
      break;
    }
  }
  if (slot < 0) {
    slot = (int)g_soft.textures.size();
    g_soft.textures.emplace_back();
  }
  auto& t = g_soft.textures[slot];
  t.used = true;
  t.w = w;
  t.h = h;
  t.rgba.assign(rgba, rgba + w * h * 4);
  return slot;
}

void ctr_gpu_tex_delete(int handle) {
  if (handle >= 0 && handle < (int)g_soft.textures.size()) {
    g_soft.textures[handle] = SoftTex();
  }
}

void ctr_gpu_draw(const ctr_draw_state* state, const ctr_vertex* verts, int count) {
  g_soft.cur.draws++;
  for (int i = 0; i + 2 < count; i += 3) {
    raster_triangle(*state, verts + i);
    g_soft.cur.triangles++;
  }
}

void ctr_gpu_request_screenshot(const char* path) {
  g_soft.screenshot_path = path;
}

void ctr_gpu_get_stats(ctr_gpu_stats* out) {
  *out = g_soft.last;
}
}

namespace ctr_gfx {
void set_soft_frame_dump(const std::string& dir, int every_nth) {
  g_soft.dump_dir = dir;
  g_soft.dump_every = every_nth;
  if (!dir.empty()) {
    file_util::create_dir_if_needed(dir);
  }
}
}  // namespace ctr_gfx

#endif  // __3DS__
