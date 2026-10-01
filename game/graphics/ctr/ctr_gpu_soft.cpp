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

struct SoftMesh {
  bool used = false;
  int stride = 16;             // 16: c3l::Vertex, 20: c3l::MercVertex
  std::vector<uint8_t> verts;
  std::vector<uint16_t> indices;
};

struct SoftPool {
  bool used = false;
  std::vector<uint8_t> data;
  // textures made from the pool: decoded when the pool is ready (the loader fills it first)
  struct Pending {
    int handle;
    unsigned offset;
    int w, h, format, levels;
  };
  std::vector<Pending> pending;
};

struct SoftState {
  std::vector<uint8_t> color;  // RGBA8, top row first
  std::vector<float> depth;
  std::vector<SoftTex> textures;
  std::vector<SoftMesh> meshes;
  std::vector<SoftPool> pools;
  int rgba4_as_rgba8 = 1;
  int frame = 0;
  std::string dump_dir;
  int dump_every = 0;
  ctr_gpu_stats cur{}, last{};
  std::string screenshot_path;
  std::chrono::steady_clock::time_point next_vblank;
  bool vblank_started = false;
} g_soft;

uint32_t morton8(uint32_t x, uint32_t y) {
  return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) |
         ((y & 4) << 3);
}

// One ETC1 texel (x, y in the 4x4 block, y = memory row) of a little-endian block (the GPU's).
void etc1_texel(uint64_t raw, unsigned x, unsigned y, uint8_t out[3]) {
  static const int kMod[8][2] = {{2, 8},   {5, 17},  {9, 29},   {13, 42},
                                 {18, 60}, {24, 80}, {33, 106}, {47, 183}};
  auto bits = [&](int pos, int n) { return (int)((raw >> pos) & ((1ull << n) - 1)); };
  auto sbits3 = [&](int pos) {
    int v = bits(pos, 3);
    return v >= 4 ? v - 8 : v;
  };
  const int texel = 4 * x + y;
  if (bits(32, 1)) {
    std::swap(x, y);
  }
  int c[3];
  if (bits(33, 1)) {
    const int base[3] = {bits(59, 5), bits(51, 5), bits(43, 5)};
    const int delta[3] = {sbits3(56), sbits3(48), sbits3(40)};
    for (int i = 0; i < 3; i++) {
      const int v = base[i] + (x >= 2 ? delta[i] : 0);
      c[i] = (v << 3) | (v >> 2);
    }
  } else {
    const int shift = x < 2 ? 4 : 0;
    const int pos[3] = {56, 48, 40};
    for (int i = 0; i < 3; i++) {
      const int v = bits(pos[i] + shift, 4);
      c[i] = (v << 4) | v;
    }
  }
  const int table = x < 2 ? bits(37, 3) : bits(34, 3);
  int m = kMod[table][(raw >> texel) & 1];
  if ((raw >> (16 + texel)) & 1) {
    m = -m;
  }
  for (int i = 0; i < 3; i++) {
    out[i] = (uint8_t)std::clamp(c[i] + m, 0, 255);
  }
}

// the first level of a tiled texture (ctr_tex_format) -> RGBA8, top row first
std::vector<uint8_t> untile(const uint8_t* data, int w, int h, int format) {
  std::vector<uint8_t> rgba(w * h * 4);
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      const uint32_t ty = h - 1 - y;
      uint8_t* p = &rgba[4 * (x + y * w)];
      if (format == CTR_TEX_ETC1 || format == CTR_TEX_ETC1A4) {
        const bool alpha = format == CTR_TEX_ETC1A4;
        const uint32_t tile = (ty / 8) * (w / 8) + (x / 8);
        const uint32_t sub = ((x & 7) / 4) + 2 * ((ty & 7) / 4);
        const uint8_t* b = data + tile * (alpha ? 64 : 32) + sub * (alpha ? 16 : 8);
        const unsigned fx = x & 3, fy = ty & 3;
        uint64_t raw;
        p[3] = 255;
        if (alpha) {
          memcpy(&raw, b, 8);
          const int a = (int)((raw >> (4 * (4 * fx + fy))) & 0xf);
          p[3] = (uint8_t)((a << 4) | a);
          b += 8;
        }
        memcpy(&raw, b, 8);
        etc1_texel(raw, fx, fy, p);
        continue;
      }
      const uint32_t idx = ((ty / 8) * (w / 8) + (x / 8)) * 64 + morton8(x & 7, ty & 7);
      if (format == CTR_TEX_RGBA8) {
        const uint8_t* t = data + 4 * idx;  // A, B, G, R
        p[0] = t[3];
        p[1] = t[2];
        p[2] = t[1];
        p[3] = t[0];
        continue;
      }
      uint16_t v;
      memcpy(&v, data + 2 * idx, 2);
      if (format == CTR_TEX_RGB565) {
        p[0] = ((v >> 11) & 31) << 3;
        p[1] = ((v >> 5) & 63) << 2;
        p[2] = (v & 31) << 3;
        p[3] = 255;
      } else {
        p[0] = ((v >> 12) & 15) * 17;
        p[1] = ((v >> 8) & 15) * 17;
        p[2] = ((v >> 4) & 15) * 17;
        p[3] = (v & 15) * 17;
      }
    }
  }
  return rgba;
}

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

void shade_pixel(const ctr_draw_state& st,
                 int px,
                 int py,
                 float z,
                 const float col[4],
                 float s,
                 float t,
                 bool tex_alpha_full = false) {
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
      if (tex_alpha_full) {
        a = tc[3] / 255.f * a;
      } else if (st.tcc) {
        a = tc[3];
      }
    } else {
      r = tc[0] * r / 128.f;
      g = tc[1] * g / 128.f;
      b = tc[2] * b / 128.f;
      if (tex_alpha_full) {
        a = tc[3] / 255.f * a;
      } else if (st.tcc) {
        a = tc[3] * a / 128.f;
      }
    }
  } else {
    // untextured: the GS outputs the vertex color as it is (see ctr_gpu_citro3d.c)
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
      case CTR_BLEND_OVER_CLEAR:
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
      case CTR_BLEND_ONE_ONE:
        o = src[i] + dst;
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

void raster_triangle(const ctr_draw_state& st, const ctr_vertex* v, bool tex_alpha_full = false) {
  // to screen: x in [-1, 1] -> [40, 360], y in [-1, 1] -> [240, 0]
  float sx[3], sy[3];
  for (int i = 0; i < 3; i++) {
    sx[i] = 200.f + v[i].x * 200.f;  // x in [-1, 1]: the whole 400 pixel width (5:3)
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
      shade_pixel(st, px, py, z, col, s, t, tex_alpha_full);
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

void ctr_gpu_wait_vblank(int /*min_vblanks*/) {
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

void ctr_gpu_draw_skinned_env(const ctr_draw_state*,
                              const float*,
                              const float*,
                              int,
                              const float*,
                              int,
                              int,
                              int) {
  // (not in the PC software renderer: the envmap shine pass is GPU only)
}

void ctr_gpu_draw_clip(const ctr_draw_state* state, const ctr_clip_vertex* verts, int count) {
  // (no clipping here: triangles with a point behind the camera are left out; affine texturing)
  std::vector<ctr_vertex> out;
  for (int i = 0; i + 2 < count; i += 3) {
    if (verts[i].w <= 0 || verts[i + 1].w <= 0 || verts[i + 2].w <= 0) {
      continue;
    }
    for (int k = 0; k < 3; k++) {
      const auto& v = verts[i + k];
      ctr_vertex o;
      o.x = v.x / v.w;
      o.y = v.y / v.w;
      o.z = (v.z / v.w + 1.f) * 0.5f;
      o.s = v.s;
      o.t = v.t;
      o.r = v.r;
      o.g = v.g;
      o.b = v.b;
      o.a = v.a;
      out.push_back(o);
    }
  }
  if (!out.empty()) {
    ctr_gpu_draw(state, out.data(), (int)out.size());
  }
}

// (AI-assisted) the two cloud layers as two added passes (GS modulate with the constant color)
void ctr_gpu_draw_clip2(const ctr_draw_state* state,
                        uint32_t const0,
                        uint32_t const1,
                        const ctr_clip_vertex2* verts,
                        int count) {
  for (int layer = 0; layer < 2; layer++) {
    const uint32_t c = layer ? const1 : const0;
    std::vector<ctr_clip_vertex> out((size_t)count);
    for (int i = 0; i < count; i++) {
      const auto& v = verts[i];
      auto& o = out[i];
      o.x = v.x;
      o.y = v.y;
      o.z = v.z;
      o.w = v.w;
      o.s = layer ? v.s1 : v.s0;
      o.t = layer ? v.t1 : v.t0;
      o.r = (uint8_t)(c & 0xff);
      o.g = (uint8_t)((c >> 8) & 0xff);
      o.b = (uint8_t)((c >> 16) & 0xff);
      o.a = v.a;
    }
    ctr_draw_state st = *state;
    st.tcc = 0;
    st.blend = CTR_BLEND_ADD;
    ctr_gpu_draw_clip(&st, out.data(), count);
  }
}

void ctr_gpu_prepare_mesh_matrix(const float clip[16], ctr_mesh_matrix* out) {
  memcpy(out->m, clip, sizeof(out->m));
}

void ctr_gpu_draw_mesh_prepared(const ctr_draw_state* state,
                                const ctr_mesh_matrix* matrix,
                                int mesh,
                                int first_index,
                                int index_count) {
  ctr_gpu_draw_mesh(state, matrix->m, mesh, first_index, index_count);
}

int ctr_gpu_copy_screen(void) {
  static int handle = -1;
  if (handle < 0 || handle >= (int)g_soft.textures.size() || !g_soft.textures[handle].used ||
      g_soft.textures[handle].w != kW) {
    std::vector<uint8_t> empty(kW * kH * 4, 0);
    handle = ctr_gpu_tex_create(kW, kH, empty.data());
  }
  g_soft.textures[handle].rgba = g_soft.color;
  return handle;
}

void ctr_gpu_screen_uv(float x, float y, float* s, float* t) {
  *s = (x + 1.f) * 0.5f;
  *t = (1.f - y) * 0.5f;  // soft textures: top row first
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

void ctr_gpu_draw_quads(const ctr_draw_state* state, const ctr_vertex* verts, int quad_count) {
  g_soft.cur.draws++;
  for (int q = 0; q < quad_count; q++) {
    const ctr_vertex* v = verts + 4 * q;
    const ctr_vertex t0[3] = {v[0], v[1], v[3]};
    const ctr_vertex t1[3] = {v[3], v[1], v[2]};
    raster_triangle(*state, t0);
    raster_triangle(*state, t1);
    g_soft.cur.triangles += 2;
  }
}

int ctr_gpu_tex_create_tiled(int w, int h, int format, const void* data, int /*size*/) {
  auto rgba = untile((const uint8_t*)data, w, h, format);
  return ctr_gpu_tex_create(w, h, rgba.data());
}

int ctr_gpu_tex_create_mipmapped(int w, int h, const uint8_t* rgba) {
  return ctr_gpu_tex_create(w, h, rgba);  // (the software renderer samples the first level)
}

void ctr_gpu_tex_update(int handle, const uint8_t* rgba) {
  if (handle >= 0 && handle < (int)g_soft.textures.size() && g_soft.textures[handle].used) {
    auto& t = g_soft.textures[handle];
    t.rgba.assign(rgba, rgba + t.w * t.h * 4);
  }
}

unsigned int ctr_gpu_tex_bytes(int w, int h, int format, int levels) {
  const unsigned bits = format == CTR_TEX_ETC1     ? 4
                        : format == CTR_TEX_ETC1A4 ? 8
                        : format == CTR_TEX_RGBA8  ? 32
                                                   : 16;
  unsigned total = 0;
  for (int l = 0; l < levels; l++) {
    total += (unsigned)((w >> l) * (h >> l)) * bits / 8;
  }
  return total;
}

int ctr_gpu_pool_create(unsigned int bytes) {
  for (size_t i = 0; i < g_soft.pools.size(); i++) {
    if (!g_soft.pools[i].used) {
      g_soft.pools[i].used = true;
      g_soft.pools[i].data.assign(bytes, 0);
      g_soft.pools[i].pending.clear();
      return (int)i;
    }
  }
  g_soft.pools.emplace_back();
  g_soft.pools.back().used = true;
  g_soft.pools.back().data.assign(bytes, 0);
  return (int)g_soft.pools.size() - 1;
}

void* ctr_gpu_pool_data(int pool) {
  if (pool < 0 || pool >= (int)g_soft.pools.size() || !g_soft.pools[pool].used) {
    return nullptr;
  }
  return g_soft.pools[pool].data.data();
}

int ctr_gpu_pool_tex(int pool, unsigned int offset, int w, int h, int format, int levels) {
  if (pool < 0 || pool >= (int)g_soft.pools.size() || !g_soft.pools[pool].used ||
      offset + ctr_gpu_tex_bytes(w, h, format, levels) > g_soft.pools[pool].data.size()) {
    return -1;
  }
  // a placeholder until the pool is ready
  std::vector<uint8_t> blank(w * h * 4, 0);
  const int handle = ctr_gpu_tex_create(w, h, blank.data());
  g_soft.pools[pool].pending.push_back({handle, offset, w, h, format, levels});
  return handle;
}

void ctr_gpu_pool_ready(int pool) {
  if (pool < 0 || pool >= (int)g_soft.pools.size() || !g_soft.pools[pool].used) {
    return;
  }
  auto& p = g_soft.pools[pool];
  for (const auto& t : p.pending) {
    auto rgba = untile(p.data.data() + t.offset, t.w, t.h, t.format);
    ctr_gpu_tex_update(t.handle, rgba.data());
  }
  p.pending.clear();
}

void ctr_gpu_pool_delete(int pool) {
  if (pool >= 0 && pool < (int)g_soft.pools.size()) {
    g_soft.pools[pool] = SoftPool();
  }
}

void ctr_gpu_pool_set_priority(int, int) {}

int ctr_gpu_mesh_create(const void* verts,
                        int vertex_count,
                        const uint16_t* indices,
                        int index_count) {
  int slot = -1;
  for (size_t i = 0; i < g_soft.meshes.size(); i++) {
    if (!g_soft.meshes[i].used) {
      slot = (int)i;
      break;
    }
  }
  if (slot < 0) {
    slot = (int)g_soft.meshes.size();
    g_soft.meshes.emplace_back();
  }
  auto& m = g_soft.meshes[slot];
  m.used = true;
  m.verts.assign((const uint8_t*)verts, (const uint8_t*)verts + vertex_count * 16);
  m.indices.assign(indices, indices + index_count);
  return slot;
}

void ctr_gpu_free_pending_now(void) {}

void ctr_gpu_mesh_delete(int mesh) {
  if (mesh >= 0 && mesh < (int)g_soft.meshes.size()) {
    g_soft.meshes[mesh] = SoftMesh();
  }
}

void ctr_gpu_draw_mesh(const ctr_draw_state* state,
                       const float clip[16],
                       int mesh,
                       int first_index,
                       int index_count) {
  if (mesh < 0 || mesh >= (int)g_soft.meshes.size() || !g_soft.meshes[mesh].used) {
    return;
  }
  const auto& m = g_soft.meshes[mesh];
  g_soft.cur.draws++;
  for (int i = first_index; i + 2 < first_index + index_count; i += 3) {
    ctr_vertex tri[3];
    bool ok = true;
    for (int k = 0; k < 3; k++) {
      const uint8_t* src = &m.verts[16 * m.indices[i + k]];
      int16_t pos[3], st[2];
      memcpy(pos, src, 6);
      memcpy(st, src + 8, 4);
      float in[4] = {(float)pos[0], (float)pos[1], (float)pos[2], 1.f};
      float c[4];
      for (int r = 0; r < 4; r++) {
        c[r] = clip[4 * r] * in[0] + clip[4 * r + 1] * in[1] + clip[4 * r + 2] * in[2] +
               clip[4 * r + 3];
      }
      if (c[3] < 1e-3f) {
        ok = false;  // crude near clipping: drop the triangle
        break;
      }
      tri[k].x = c[0] / c[3];
      tri[k].y = c[1] / c[3];
      tri[k].z = (1.f + c[2] / c[3]) * 0.5f;  // GS z: larger = closer (zn = +1 near)
      tri[k].s = st[0] / 1024.f;
      tri[k].t = st[1] / 1024.f;
      tri[k].r = src[12];
      tri[k].g = src[13];
      tri[k].b = src[14];
      tri[k].a = src[15];
    }
    if (!ok) {
      continue;
    }
    // behind the far plane / outside z
    if ((tri[0].z < 0 && tri[1].z < 0 && tri[2].z < 0) ||
        (tri[0].z > 1 && tri[1].z > 1 && tri[2].z > 1)) {
      continue;
    }
    raster_triangle(*state, tri, true);
    g_soft.cur.triangles++;
  }
}

int ctr_gpu_skinned_mesh_create(const void* verts,
                                int vertex_count,
                                const uint16_t* indices,
                                int index_count) {
  int h = ctr_gpu_mesh_create(verts, 0, indices, index_count);
  auto& m = g_soft.meshes[h];
  m.stride = 20;
  m.verts.assign((const uint8_t*)verts, (const uint8_t*)verts + vertex_count * 24);
  return h;
}

void* ctr_gpu_mesh_vertices(int mesh) {
  if (mesh < 0 || mesh >= (int)g_soft.meshes.size() || !g_soft.meshes[mesh].used) {
    return nullptr;
  }
  return g_soft.meshes[mesh].verts.data();
}

void ctr_gpu_mesh_flush(int, int, int) {}

void ctr_gpu_draw_skinned(const ctr_draw_state* state,
                          const float clip[16],
                          const float* bones,
                          int palette_count,
                          const float lights[28],
                          int mesh,
                          int first_index,
                          int index_count) {
  if (mesh < 0 || mesh >= (int)g_soft.meshes.size() || !g_soft.meshes[mesh].used) {
    return;
  }
  const auto& m = g_soft.meshes[mesh];
  g_soft.cur.draws++;
  for (int i = first_index; i + 2 < first_index + index_count; i += 3) {
    ctr_vertex tri[3];
    bool ok = true;
    for (int k = 0; k < 3; k++) {
      const uint8_t* src = &m.verts[24 * m.indices[i + k]];
      int16_t pos[3], st[2];
      memcpy(pos, src, 6);
      const uint8_t* bidx = src + 6;
      const uint8_t* bw = src + 9;
      memcpy(st, src + 12, 4);
      const uint8_t* rgba = src + 16;
      const int8_t* nrm = (const int8_t*)(src + 20);
      float cam[4] = {0, 0, 0, 1};
      float rn[3] = {0, 0, 0};
      for (int b = 0; b < 3; b++) {
        int bi = bidx[b] < palette_count ? bidx[b] : 0;
        const float* r = bones + 12 * bi;
        float w = bw[b] / 255.f;
        for (int row = 0; row < 3; row++) {
          cam[row] += w * (r[4 * row] * pos[0] + r[4 * row + 1] * pos[1] + r[4 * row + 2] * pos[2] +
                           r[4 * row + 3]);
          rn[row] += w * (r[4 * row] * nrm[0] + r[4 * row + 1] * nrm[1] + r[4 * row + 2] * nrm[2]);
        }
      }
      // lighting as merc2.vert (and ctr_skin.v.pica): the rows are the negated bones
      float len = std::sqrt(rn[0] * rn[0] + rn[1] * rn[1] + rn[2] * rn[2]) + 1e-6f;
      float l[3];
      for (int c = 0; c < 3; c++) {
        l[c] = -(lights[c] * rn[0] + lights[4 + c] * rn[1] + lights[8 + c] * rn[2]) / len;
        l[c] = l[c] < 0.f ? 0.f : l[c];
      }
      float lit[3];
      for (int c = 0; c < 3; c++) {
        lit[c] = lights[24 + c] + l[0] * lights[12 + c] + l[1] * lights[16 + c] +
                 l[2] * lights[20 + c];
      }
      float c[4];
      for (int r = 0; r < 4; r++) {
        c[r] = clip[4 * r] * cam[0] + clip[4 * r + 1] * cam[1] + clip[4 * r + 2] * cam[2] +
               clip[4 * r + 3];
      }
      if (c[3] < 1e-3f) {
        ok = false;
        break;
      }
      tri[k].x = c[0] / c[3];
      tri[k].y = c[1] / c[3];
      tri[k].z = (1.f + c[2] / c[3]) * 0.5f;  // GS z: larger = closer (zn = +1 near)
      tri[k].s = st[0] / 1024.f;
      tri[k].t = st[1] / 1024.f;
      tri[k].r = (uint8_t)std::clamp(rgba[0] * lit[0], 0.f, 255.f);
      tri[k].g = (uint8_t)std::clamp(rgba[1] * lit[1], 0.f, 255.f);
      tri[k].b = (uint8_t)std::clamp(rgba[2] * lit[2], 0.f, 255.f);
      // (AI-assisted) alpha times the lights' alpha, as ctr_skin.v.pica (color-mult's alpha)
      const float lit_a = lights[27] + l[0] * lights[15] + l[1] * lights[19] + l[2] * lights[23];
      tri[k].a = (uint8_t)std::clamp(rgba[3] * lit_a, 0.f, 255.f);
    }
    if (!ok) {
      continue;
    }
    raster_triangle(*state, tri, true);
    g_soft.cur.triangles++;
  }
}

void ctr_gpu_request_screenshot(const char* path) {
  g_soft.screenshot_path = path;
}

void ctr_gpu_get_stats(ctr_gpu_stats* out) {
  *out = g_soft.last;
}

// the software backend has no fog
void ctr_gpu_set_mesh_fog(const float*, const float*, uint8_t, uint8_t, uint8_t) {}
void ctr_gpu_set_rgba4_as_rgba8(int on) {
  g_soft.rgba4_as_rgba8 = on;
}
int ctr_gpu_rgba4_as_rgba8(void) {
  return g_soft.rgba4_as_rgba8;
}

void ctr_gpu_set_vram_textures(int) {}
void ctr_gpu_set_color16(int) {}
void ctr_gpu_set_early_depth(int) {}
void ctr_gpu_set_proctex(int) {}
void ctr_gpu_set_compact_textures(int) {}
int ctr_gpu_tex_create_compact(int w, int h, const uint8_t* rgba, int /*flags*/) {
  return ctr_gpu_tex_create(w, h, rgba);
}
int ctr_gpu_tex_radial(int) {
  return 0;
}
void ctr_gpu_set_overlap(int) {}
void ctr_gpu_submit_partial(void) {}
int ctr_gpu_set_pipeline(int) {
  return 0;
}
void ctr_gpu_set_mip_mode(int) {}
int ctr_gpu_is_emulator(void) {
  return 0;
}

double ctr_gpu_time_ms(void) {
  using namespace std::chrono;
  return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// the software backend renders synchronously
int ctr_gpu_async_start(ctr_gpu_job_fn, void*) {
  return 0;
}
void ctr_gpu_async_submit(void) {}
double ctr_gpu_async_wait(void) {
  return 0;
}
void ctr_gpu_async_stop(void) {}

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
