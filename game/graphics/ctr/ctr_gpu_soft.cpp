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
#include <unordered_map>
#include <vector>
#include <map>

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
  void* ext = nullptr;  // (AI-assisted, streaming) ctr_gpu_mesh_begin_ext's block (given back)
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
  // (AI-assisted) the stencil part of the 3DS's D24S8 depth buffer (CTR_STATE_STENCIL_*), cleared
  // with the depth at frame begin; none with config.ini depth16 (ctr_gpu_has_stencil)
  std::vector<uint8_t> stencil;
  int depth16 = 0;
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
// (AI-assisted) ctr_gpu_stream_alloc: the frame's vertices, until the next frame begins
static std::vector<std::vector<uint8_t>> s_stream;

// (AI-assisted) where draws go: the screen, or a tile of a render to texture (ctr_gpu_rt_begin)
struct SoftTarget {
  uint8_t* color = nullptr;  // RGBA8, top row first
  float* depth = nullptr;
  uint8_t* stencil = nullptr;  // none in a tile (D16)
  int stride = kW;
  int x0 = 0, y0 = 0, x1 = kW, y1 = kH;  // scissor, buffer pixels (rows from the top)
  float ox = 200.f, sx = 200.f, oy = 120.f, sy = 120.f;  // pixel = (ox + sx * x, oy - sy * y)
  bool rt = false;
};
SoftTarget s_target;
struct SoftRt {
  int tex = -1;
  int lw = 0, lh = 0;
  std::vector<float> depth;
};
std::vector<SoftRt> s_rts;

// ---- (AI-assisted) snow-gpu-audit: per-bucket draw/fill statistics (env CTR_SOFT_STATS=N) ----
namespace softstats {
struct B {
  double draws = 0, tris = 0, px = 0, px_atest = 0, px_z = 0, px_blend = 0, px_invis = 0,
         px_proc = 0, tex_changes = 0, state_changes = 0, area_bbox = 0, prog_switches = 0;
  double copy_rows = 0;  // (AI-assisted) screen copy: 8-column tile rows (50 = the whole screen)
  double unif = 0;       // (AI-assisted) skin / env vertex uniforms uploaded (vectors), as the
                         // 3DS backend caches them
  double sub = 0;  // (AI-assisted) level mesh triangles submitted (what the 3DS GPU's vertex stage
                   // gets: before clipping and back face culling; tris counts the drawn ones)
};
std::map<std::string, B> acc;
B* cur = &acc["(none)"];
std::string cur_name = "(none)";
int frames = 0;
int every = -1;
std::vector<uint32_t> od;  // fragments per pixel over the window
ctr_draw_state last_st{};
bool last_valid = false;
int last_tex = -2;
std::map<std::pair<std::string, int>, std::pair<double, double>> tex_px;
int cur_prog = -1;
std::pair<double, double>* cur_tp = nullptr;
void prog(int p) {
  if (p != cur_prog) {
    cur->prog_switches++;
    cur_prog = p;
  }
}  // (bucket, tex): px, draws
int enabled() {
  if (every < 0) {
    const char* e = getenv("CTR_SOFT_STATS");
    every = e ? atoi(e) : 0;
    od.assign(400 * 240, 0);
  }
  return every;
}
void draw(const ctr_draw_state& st) {
  if (!enabled()) return;
  cur->draws++;
  cur_tp = &tex_px[{cur_name, st.tex}];
  cur_tp->second++;
  if (!last_valid || memcmp(&st, &last_st, sizeof(st))) cur->state_changes++;
  if (st.tex != last_tex) cur->tex_changes++;
  last_tex = st.tex;
  last_st = st;
  last_valid = true;
}
}  // namespace softstats
extern "C" void ctr_gpu_soft_bucket(const char* name) {
  softstats::cur_name = name;
  softstats::cur = &softstats::acc[name];
}

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

// (AI-assisted) sky_last / depth_far, as ctr_gpu_citro3d.c
ctr_gpu_hook_fn s_sky_hook = nullptr;
void* s_sky_hook_arg = nullptr;
bool s_depth_far = false;
bool s_skip_color_clear = false;
bool keeps_sky(const ctr_draw_state* st) {
  return st->blend == CTR_BLEND_OFF && st->zwrite &&
         !(st->flags & ~(CTR_STATE_EARLY_DEPTH | CTR_STATE_PROCTEX));
}
void sky_hook(const ctr_draw_state* st) {
  if (s_sky_hook && !s_target.rt && (!st || !keeps_sky(st))) {
    ctr_gpu_run_sky_hook();
  }
}

// (AI-assisted, far-fog) the fog, as the 3DS backend draws it (it had none here: host frames showed
// every level without its fog). Level meshes and the ocean: the larger of the game fog and the
// draw distance fog from the values of ctr_gpu_set_mesh_fog (mesh_fog_uniforms' form in
// ctr_gpu_citro3d.c); merc draws with CTR_TCC_FOG: merc2.vert's fog (ctr_gpu_set_merc_fog); the PS2
// ocean: its vertices' fog. Mixed towards the fog color after texturing, like the combiner's fog
// stage. Env CTR_SOFT_NO_FOG=1: none (the old frames).
float s_fog0[4] = {0.f, 255.f, 255.f, -1.f / 255.f}, s_fog1[4] = {0.f, 1.f, 0.f, 0.f};
uint8_t s_fog_rgb[3] = {0, 0, 0};
bool s_nofog = false;
float s_mfog_hvdf_w = 255.f, s_mfog_lo = 0.f, s_mfog_hi = 0.f;
int s_shader_opt = 1;
bool fog_on() {
  static const bool off = getenv("CTR_SOFT_NO_FOG") != nullptr;
  return !off;
}
// the fog amount of a level mesh / ocean vertex at clip w
float mesh_fog(float w) {
  const float k = s_fog0[3];
  const float fmin = std::clamp(s_fog0[1], 0.f, 255.f), fmax = std::clamp(s_fog0[2], 0.f, 255.f);
  const float upper = 1.f + k * fmin, lower = std::max(0.f, 1.f + k * fmax);
  const float game = 1.f + k * (s_fog0[0] - w);
  const float dd = (s_fog1[0] * w - s_fog1[1]) * s_fog1[2];
  return std::clamp(std::max(std::min(game, upper), std::max(dd, lower)), 0.f, 1.f);
}
// merc2.vert's fog amount at clip w
float merc_fog(float w) {
  return std::clamp(w / 255.f + 1.f - s_mfog_hvdf_w / 255.f, s_mfog_lo, s_mfog_hi);
}

// (AI-assisted) ctr_gpu_soft_query_begin: count the fragments that pass, write nothing
bool s_query = false;
long s_query_px = 0;
ctr_gpu_stats s_query_saved{};

// (AI-assisted, Jak II texture animation) ctr_gpu_tex_set_anim_slot / ctr_gpu_anim_slot_set, as
// the 3DS backend: a texture's slot + 1, each slot's texture + 1 (this frame, the next)
std::vector<uint8_t> s_tex_anim_slot;  // by handle: slot + 1, 0: none
int s_anim_slot_tex[CTR_ANIM_SLOTS], s_anim_slot_next[CTR_ANIM_SLOTS];
uint64_t s_anim_drawn = 0, s_anim_drawn_last = 0;
int soft_tex_res(int t) {
  if (t < 0 || t >= (int)s_tex_anim_slot.size() || !s_tex_anim_slot[t]) {
    return t;
  }
  s_anim_drawn |= 1ull << (s_tex_anim_slot[t] - 1);
  const int a = s_anim_slot_tex[s_tex_anim_slot[t] - 1] - 1;
  return a >= 0 && a < (int)g_soft.textures.size() && g_soft.textures[a].used ? a : t;
}

void shade_pixel(const ctr_draw_state& st,
                 int px,
                 int py,
                 float z,
                 const float col[4],
                 float s,
                 float t,
                 bool tex_alpha_full = false,
                 float fog = 0.f) {
  // color in GS units (0x80 = 1.0 for vertex color / alpha)
  float r = col[0], g = col[1], b = col[2], a = col[3];
  if (s_depth_far) {
    z = 0.f;  // (AI-assisted) ctr_gpu_set_depth_far
  }
  static const bool force_white = getenv("CTR_SOFT_WHITE") != nullptr;
  if (force_white) {
    r = g = b = 128.f;
  }
  const int stex = soft_tex_res(st.tex);  // (AI-assisted) an animated slot's texture
  if (stex >= 0 && stex < (int)g_soft.textures.size() && g_soft.textures[stex].used) {
    float tc[4];
    sample(g_soft.textures[stex], st, s, t, tc);
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
  if (fog > 0.f) {
    // (AI-assisted, far-fog) the fog stage: rgb = mix(rgb, fog color, amount)
    const float f = std::min(fog, 1.f);
    r = std::min(r, 255.f) * (1.f - f) + s_fog_rgb[0] * f;
    g = std::min(g, 255.f) * (1.f - f) + s_fog_rgb[1] * f;
    b = std::min(b, 255.f) * (1.f - f) + s_fog_rgb[2] * f;
  }
  const bool ss_on = softstats::enabled() != 0 && !s_query;
  if (ss_on) {
    softstats::cur->px++;
    if (!s_target.rt) softstats::od[px + py * kW]++;
    if (st.flags & CTR_STATE_PROCTEX) softstats::cur->px_proc++;
    if ((st.blend == CTR_BLEND_ALPHA || st.blend == CTR_BLEND_ADD ||
         st.blend == CTR_BLEND_OVER_CLEAR) && a < 2.f) {
      softstats::cur->px_invis++;
    }
    if (softstats::cur_tp) softstats::cur_tp->first++;
  }
  if (!test(st.atest, a, (float)st.aref)) {
    return;
  }
  if (ss_on) softstats::cur->px_atest++;
  int idx = px + py * s_target.stride;
  // (AI-assisted) the stencil test and operations, as ctr_gpu_citro3d.c sets them
  uint8_t* sten = g_soft.depth16 || !s_target.stencil ? nullptr : &s_target.stencil[idx];
  if (sten && (st.flags & CTR_STATE_STENCIL_OUT) && *sten == 1) {
    return;
  }
  if (sten && (st.flags & CTR_STATE_STENCIL_CLEAR) && *sten == 0) {
    return;
  }
  if (!test(st.ztest, z, s_target.depth[idx])) {
    if (sten && (st.flags & CTR_STATE_STENCIL_CLEAR) && !s_query) {
      *sten = 0;
    }
    return;
  }
  if (s_query) {
    s_query_px++;  // (AI-assisted) occlusion query: counted, nothing written
    return;
  }
  if (sten) {
    if (st.flags & CTR_STATE_STENCIL_SET) {
      *sten = 1;
    } else if (st.flags & CTR_STATE_STENCIL_INCR) {
      *sten = (uint8_t)std::min(255, *sten + 1);
    } else if (st.flags & CTR_STATE_STENCIL_DECR) {
      *sten = (uint8_t)std::max(0, *sten - 1);
    } else if (st.flags & CTR_STATE_STENCIL_CLEAR) {
      *sten = 0;
    }
  }
  if (ss_on) {
    softstats::cur->px_z++;
    if (st.blend != CTR_BLEND_OFF) softstats::cur->px_blend++;
  }
  if (st.zwrite) {
    s_target.depth[idx] = z;
  }
  if (st.flags & CTR_STATE_NO_COLOR) {
    return;
  }
  uint8_t* d = &s_target.color[4 * idx];
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
      case CTR_BLEND_MUL:  // (AI-assisted) the shadows' darkening: Cs (0xff = 1) * Cd
        o = src[i] * dst / 255.f;
        break;
      default:
        o = src[i];
        break;
    }
    d[i] = (uint8_t)std::clamp(o, 0.f, 255.f);
  }
  if (st.blend == CTR_BLEND_OFF) {
    // (AI-assisted) a render to texture keeps alpha as the 3DS color buffer does (0xff = 1.0)
    d[3] = (uint8_t)std::clamp(s_target.rt ? 2.f * a : a, 0.f, 255.f);
  }
}

void raster_triangle(const ctr_draw_state& st,
                     const ctr_vertex* v,
                     bool tex_alpha_full = false,
                     const float* fog = nullptr) {
  // (AI-assisted, j2-mem) env CTR_SOFT_NO_RASTER: no pixels (long memory / streaming runs: the
  // renderers' decisions are the same, ~10x faster)
  static const bool no_raster = getenv("CTR_SOFT_NO_RASTER") != nullptr;
  if (no_raster) {
    return;
  }
  // to screen: x in [-1, 1] -> [40, 360], y in [-1, 1] -> [240, 0]
  float sx[3], sy[3];
  for (int i = 0; i < 3; i++) {
    sx[i] = s_target.ox + v[i].x * s_target.sx;  // x in [-1, 1]: the whole 400 pixel width (5:3)
    sy[i] = s_target.oy - v[i].y * s_target.sy;
  }
  float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
  if (std::abs(area) < 1e-6f) {
    return;
  }
  int x0 = std::max(s_target.x0, (int)std::floor(std::min({sx[0], sx[1], sx[2]})));
  int x1 = std::min(s_target.x1 - 1, (int)std::ceil(std::max({sx[0], sx[1], sx[2]})));
  int y0 = std::max(s_target.y0, (int)std::floor(std::min({sy[0], sy[1], sy[2]})));
  int y1 = std::min(s_target.y1 - 1, (int)std::ceil(std::max({sy[0], sy[1], sy[2]})));
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
      const float f = fog ? w0 * fog[0] + w1 * fog[1] + w2 * fog[2] : 0.f;
      shade_pixel(st, px, py, z, col, s, t, tex_alpha_full, f);
    }
  }
}
}  // namespace

extern "C" {

int ctr_gpu_init(void) {
  g_soft.color.assign(kW * kH * 4, 0);
  g_soft.depth.assign(kW * kH, 0.f);
  g_soft.stencil.assign(kW * kH, 0);
  g_soft.textures.clear();
  s_target = SoftTarget{};
  s_target.color = g_soft.color.data();
  s_target.depth = g_soft.depth.data();
  s_target.stencil = g_soft.stencil.data();
  return 0;
}

void ctr_gpu_exit(void) {
  g_soft.textures.clear();
}

void ctr_gpu_frame_begin(uint8_t r, uint8_t g, uint8_t b) {
  memcpy(s_anim_slot_tex, s_anim_slot_next, sizeof(s_anim_slot_tex));  // (AI-assisted)
  s_anim_drawn_last = s_anim_drawn;
  s_anim_drawn = 0;
  s_sky_hook = nullptr;
  s_nofog = false;  // (AI-assisted, far-fog) as ctr_gpu_set_mesh_nofog says
  g_soft.cur = ctr_gpu_stats{};
  g_soft.cur.color_clear_skipped = s_skip_color_clear;
  for (int i = 0; i < kW * kH && !s_skip_color_clear; i++) {
    g_soft.color[4 * i] = r;
    g_soft.color[4 * i + 1] = g;
    g_soft.color[4 * i + 2] = b;
    g_soft.color[4 * i + 3] = 255;
  }
  std::fill(g_soft.depth.begin(), g_soft.depth.end(), 0.f);
  std::fill(g_soft.stencil.begin(), g_soft.stencil.end(), 0);
  s_stream.clear();
}

void ctr_gpu_frame_end(void) {
  ctr_gpu_rt_end();  // (AI-assisted) impostors: a render to texture left open
  ctr_gpu_run_sky_hook();
  g_soft.frame++;
  if (softstats::enabled() && ++softstats::frames >= softstats::every) {
    const double n = softstats::frames;
    std::string line = fmt::format("[softstats] frame {} ({} frames, per frame):", g_soft.frame, softstats::frames);
    softstats::B tot;
    for (auto& [name, b] : softstats::acc) {
      if (!b.draws && !b.px) continue;
      line += fmt::format("\n  {:<14} draws {:6.1f} tris {:8.0f} px {:8.0f} ({:5.2f} scr) atest {:8.0f} zpass {:8.0f} blend {:8.0f} invis {:8.0f} proc {:7.0f} texchg {:5.1f} stchg {:5.1f} progsw {:4.1f}",
          name, b.draws / n, b.tris / n, b.px / n, b.px / n / 96000.0, b.px_atest / n, b.px_z / n, b.px_blend / n, b.px_invis / n, b.px_proc / n, b.tex_changes / n, b.state_changes / n, b.prog_switches / n);
      if (b.copy_rows) line += fmt::format(" copyrows {:5.1f}", b.copy_rows / n);
      if (b.unif) line += fmt::format(" unif {:6.0f}", b.unif / n);
      if (b.sub) line += fmt::format(" sub {:8.0f}", b.sub / n);
      tot.sub += b.sub;
      tot.draws += b.draws; tot.tris += b.tris; tot.px += b.px; tot.px_blend += b.px_blend; tot.px_invis += b.px_invis; tot.px_z += b.px_z; tot.prog_switches += b.prog_switches; tot.tex_changes += b.tex_changes;
    }
    line += fmt::format("\n  {:<14} draws {:6.1f} tris {:8.0f} px {:8.0f} ({:5.2f} scr) zpass {:8.0f} blend {:8.0f} invis {:8.0f} texchg {:5.1f} progsw {:4.1f}", "TOTAL", tot.draws / n, tot.tris / n, tot.px / n, tot.px / n / 96000.0, tot.px_z / n, tot.px_blend / n, tot.px_invis / n, tot.tex_changes / n, tot.prog_switches / n);
    line += fmt::format(" level mesh tris submitted {:.0f}", tot.sub / n);
    // the textures with the most fragments in sprite / merc buckets
    std::vector<std::pair<double, std::pair<std::string, int>>> top;
    for (auto& [k, v] : softstats::tex_px) top.push_back({v.first, k});
    std::sort(top.rbegin(), top.rend());
    line += "\n  top textures by fragments:";
    for (size_t i = 0; i < top.size() && i < 14; i++) {
      const int t = top[i].second.second;
      int w = 0, h = 0;
      if (t >= 0 && t < (int)g_soft.textures.size()) { w = g_soft.textures[t].w; h = g_soft.textures[t].h; }
      line += fmt::format(" [{} tex{} {}x{}: {:.0f}]", top[i].second.first, t, w, h, top[i].first / n);
    }
    lg::info("{}", line);
    if (!g_soft.dump_dir.empty()) {
      std::vector<uint8_t> img(400 * 240 * 4);
      for (int i = 0; i < 400 * 240; i++) {
        const double o = softstats::od[i] / n;
        const uint8_t v = (uint8_t)std::min(255.0, o * 32.0);  // 8 layers = white
        img[4 * i] = v; img[4 * i + 1] = o >= 4 ? 0 : v; img[4 * i + 2] = o >= 8 ? 0 : v; img[4 * i + 3] = 255;
      }
      file_util::write_rgba_png(fs::path(g_soft.dump_dir) / fmt::format("od_{:05d}.png", g_soft.frame), img.data(), 400, 240);
    }
    softstats::acc.clear();
    softstats::tex_px.clear();
    softstats::cur_tp = nullptr;
    softstats::cur = &softstats::acc[softstats::cur_name];
    std::fill(softstats::od.begin(), softstats::od.end(), 0);
    softstats::frames = 0;
  }
  softstats::last_valid = false;
  softstats::last_tex = -2;
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
                              int palette_count_env,
                              const uint32_t*,
                              uint32_t,
                              const float*,
                              int,
                              int,
                              int index_count_env) {
  sky_hook(nullptr);
  softstats::prog(3);
  // (AI-assisted) ctr_gpu_citro3d.c uploads the env program's matrix, bones and fade every draw
  if (softstats::enabled()) softstats::cur->unif += 4 + 3 * std::min(palette_count_env, 24) + 1;
  // (not in the PC software renderer: the envmap shine pass is GPU only)
  // (AI-assisted) snow-gpu-audit: counted (draws, triangles) under "<bucket>-envmap"
  if (softstats::enabled()) {
    std::string keep = softstats::cur_name;
    ctr_gpu_soft_bucket((keep + "-envmap").c_str());
    softstats::cur->draws++;
    softstats::cur->tris += index_count_env / 3;
    ctr_gpu_soft_bucket(keep.c_str());
  }
}

namespace {
void clip_draw_triangle(const ctr_draw_state& st, const ctr_clip_vertex* const tri[3]);
}
void ctr_gpu_draw_clip(const ctr_draw_state* state, const ctr_clip_vertex* verts, int count) {
  sky_hook(state);
  softstats::prog(4);
  // (AI-assisted) snow-gpu-audit: clipped against near/far like the GPU (the sky reaches behind
  // the camera: dropping those triangles undercounted its fill)
  if (getenv("CTR_SOFT_STATS")) {
    g_soft.cur.draws++;
    softstats::draw(*state);
    for (int i = 0; i + 2 < count; i += 3) {
      const ctr_clip_vertex* tri[3] = {&verts[i], &verts[i + 1], &verts[i + 2]};
      clip_draw_triangle(*state, tri);
      g_soft.cur.triangles++;
      softstats::cur->tris++;
    }
    return;
  }
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

// (AI-assisted) indexed clip space draws (the shadow volumes, ctr_gpu_clip_reserve): clipped
// against the near and far planes (-w <= z <= w, so w > 0) like the GPU; x, y by the screen
namespace {
std::vector<ctr_clip_vertex> g_clip_verts;
std::vector<uint16_t> g_clip_indices;
bool g_clip_open = false;

void clip_draw_triangle(const ctr_draw_state& st, const ctr_clip_vertex* const tri[3]) {
  struct V {
    float c[10];  // x y z w s t r g b a
  };
  V poly[8], tmp[8];
  int n = 3;
  for (int k = 0; k < 3; k++) {
    const ctr_clip_vertex& v = *tri[k];
    poly[k] = V{{v.x, v.y, v.z, v.w, v.s, v.t, (float)v.r, (float)v.g, (float)v.b, (float)v.a}};
  }
  for (int plane = 0; plane < 2; plane++) {
    auto dist = [&](const V& v) { return plane == 0 ? v.c[3] - v.c[2] : v.c[3] + v.c[2]; };
    int m = 0;
    for (int i = 0; i < n; i++) {
      const V& a = poly[i];
      const V& b = poly[(i + 1) % n];
      const float da = dist(a), db = dist(b);
      if (da >= 0) {
        tmp[m++] = a;
      }
      if ((da >= 0) != (db >= 0)) {
        const float t = da / (da - db);
        V v;
        for (int j = 0; j < 10; j++) {
          v.c[j] = a.c[j] + (b.c[j] - a.c[j]) * t;
        }
        tmp[m++] = v;
      }
    }
    n = m;
    if (n < 3) {
      return;
    }
    for (int i = 0; i < n; i++) {
      poly[i] = tmp[i];
    }
  }
  ctr_vertex out[8];
  for (int i = 0; i < n; i++) {
    const float w = poly[i].c[3];
    if (!(w > 0)) {
      return;
    }
    out[i].x = poly[i].c[0] / w;
    out[i].y = poly[i].c[1] / w;
    out[i].z = (poly[i].c[2] / w + 1.f) * 0.5f;
    out[i].s = poly[i].c[4];
    out[i].t = poly[i].c[5];
    out[i].r = (uint8_t)std::clamp(poly[i].c[6], 0.f, 255.f);
    out[i].g = (uint8_t)std::clamp(poly[i].c[7], 0.f, 255.f);
    out[i].b = (uint8_t)std::clamp(poly[i].c[8], 0.f, 255.f);
    out[i].a = (uint8_t)std::clamp(poly[i].c[9], 0.f, 255.f);
  }
  for (int i = 1; i + 1 < n; i++) {
    const ctr_vertex t[3] = {out[0], out[i], out[i + 1]};
    raster_triangle(st, t);
  }
}
}  // namespace

int ctr_gpu_clip_reserve(int max_verts, int max_indices, ctr_clip_vertex** verts,
                         uint16_t** indices) {
  sky_hook(nullptr);
  if (g_clip_open || max_verts <= 0 || max_verts > 65536 || max_indices <= 0) {
    return 0;
  }
  g_clip_verts.resize(max_verts);
  g_clip_indices.resize(max_indices);
  *verts = g_clip_verts.data();
  *indices = g_clip_indices.data();
  g_clip_open = true;
  return 1;
}

void ctr_gpu_clip_draw_ranges(const ctr_draw_state* states, const int* first, const int* count,
                              int n, int vert_count, int index_count) {
  softstats::prog(4);
  if (!g_clip_open) {
    return;
  }
  g_clip_open = false;
  if (vert_count > (int)g_clip_verts.size() || index_count > (int)g_clip_indices.size()) {
    return;
  }
  for (int i = 0; i < n; i++) {
    if (first[i] < 0 || count[i] < 0 || first[i] + count[i] > index_count) {
      return;
    }
  }
  for (int i = 0; i < n; i++) {
    if (count[i] < 3) {
      continue;
    }
    g_soft.cur.draws++;
    softstats::draw(states[i]);
    for (int k = first[i]; k + 2 < first[i] + count[i]; k += 3) {
      const ctr_clip_vertex* tri[3];
      bool ok = true;
      for (int j = 0; j < 3; j++) {
        const int vi = g_clip_indices[k + j];
        ok &= vi < vert_count;
        tri[j] = &g_clip_verts[ok ? vi : 0];
      }
      if (ok) {
        clip_draw_triangle(states[i], tri);
        g_soft.cur.triangles++;
        softstats::cur->tris++;
      }
    }
  }
}

// (AI-assisted) the PS2 ocean (CtrOceanFull): to screen space, affine texturing, no fog
namespace {
std::vector<ctr_ocean_full_vertex> g_ocean_full_verts;
std::vector<uint16_t> g_ocean_full_indices;
bool g_ocean_full_open = false;
}  // namespace

int ctr_gpu_ocean_full_reserve(int max_verts,
                          int max_indices,
                          ctr_ocean_full_vertex** verts,
                          uint16_t** indices) {
  sky_hook(nullptr);
  if (g_ocean_full_open || max_verts <= 0 || max_indices <= 0) {
    return 0;
  }
  g_ocean_full_verts.resize(max_verts);
  g_ocean_full_indices.resize(max_indices);
  *verts = g_ocean_full_verts.data();
  *indices = g_ocean_full_indices.data();
  g_ocean_full_open = true;
  return 1;
}

namespace {
void ocean_full_draw(const ctr_draw_state* state, int vert_count, int index_count, bool clip) {
  if (!g_ocean_full_open) {
    return;
  }
  g_ocean_full_open = false;
  if (vert_count > (int)g_ocean_full_verts.size() || index_count > (int)g_ocean_full_indices.size()) {
    return;
  }
  std::vector<ctr_vertex> out;
  std::vector<float> fog;  // (AI-assisted, far-fog) the vertices' fog amounts
  out.reserve(index_count);
  fog.reserve(index_count);
  for (int i = 0; i + 2 < index_count; i += 3) {
    if (clip) {
      // clip space (ctr_gpu_ocean_full_draw_clip): no clipper here, triangles behind the camera
      // are left out
      bool behind = false;
      for (int k = 0; k < 3; k++) {
        behind |= !(g_ocean_full_verts[g_ocean_full_indices[i + k]].q > 0);
      }
      if (behind) {
        continue;
      }
    }
    for (int k = 0; k < 3; k++) {
      const auto& v = g_ocean_full_verts[g_ocean_full_indices[i + k]];
      ctr_vertex o;
      const float w = clip ? v.q : 1.f;  // screen position = (x, y, z) / w
      o.x = (v.x / w / 16.f - 2048.f) / 256.f;
      o.y = (2048.f - v.y / w / 16.f) / 112.f;
      o.z = v.z / w / 16777215.f;
      o.s = clip ? v.s : v.s / v.q;
      o.t = clip ? v.t : v.t / v.q;
      o.r = v.r;
      o.g = v.g;
      o.b = v.b;
      o.a = v.a;
      out.push_back(o);
      fog.push_back(fog_on() ? std::clamp(v.fog, 0.f, 1.f) : 0.f);
    }
  }
  if (!out.empty()) {
    sky_hook(state);
    softstats::prog(0);
    g_soft.cur.draws++;
    if (state) softstats::draw(*state);
    for (size_t i = 0; i + 2 < out.size(); i += 3) {
      raster_triangle(*state, out.data() + i, false, fog.data() + i);
      g_soft.cur.triangles++;
      softstats::cur->tris++;
    }
  }
}
}  // namespace

void ctr_gpu_ocean_full_draw(const ctr_draw_state* state,
                             int vert_count,
                             int index_count,
                             uint8_t fog_r,
                             uint8_t fog_g,
                             uint8_t fog_b) {
  softstats::prog(6);
  s_fog_rgb[0] = fog_r;  // (AI-assisted, far-fog) the fog ramp's color, as on the 3DS
  s_fog_rgb[1] = fog_g;
  s_fog_rgb[2] = fog_b;
  ocean_full_draw(state, vert_count, index_count, false);
}

void ctr_gpu_ocean_full_draw_clip(const ctr_draw_state* state,
                                  int vert_count,
                                  int index_count,
                                  uint8_t fog_r,
                                  uint8_t fog_g,
                                  uint8_t fog_b) {
  softstats::prog(7);
  s_fog_rgb[0] = fog_r;  // (AI-assisted, far-fog)
  s_fog_rgb[1] = fog_g;
  s_fog_rgb[2] = fog_b;
  ocean_full_draw(state, vert_count, index_count, true);
}

// (AI-assisted) the two cloud layers as two added passes (GS modulate with the constant color)
void ctr_gpu_draw_clip2(const ctr_draw_state* state,
                        uint32_t const0,
                        uint32_t const1,
                        const ctr_clip_vertex2* verts,
                        int count) {
  sky_hook(state);
  softstats::prog(5);
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
    {
      const double sw = softstats::cur->prog_switches, dr = softstats::cur->draws;
      ctr_gpu_draw_clip(&st, out.data(), count);
      softstats::cur->prog_switches = sw;
      softstats::cur->draws = dr + (layer ? 0 : 1);
      softstats::cur_prog = 5;
    }
  }
}

void ctr_gpu_prepare_mesh_matrix(const float clip[16], ctr_mesh_matrix* out) {
  memcpy(out->m, clip, sizeof(out->m));
  out->id = 0;
}

void ctr_gpu_draw_mesh_prepared(const ctr_draw_state* state,
                                const ctr_mesh_matrix* matrix,
                                int mesh,
                                int first_index,
                                int index_count) {
  ctr_gpu_draw_mesh(state, matrix->m, mesh, first_index, index_count);
}

static void soft_mesh_tris(const ctr_draw_state* state, const float clip[16], const uint8_t* verts,
                           const uint16_t* indices, int first_index, int index_count);

void ctr_gpu_draw_mesh_stream(const ctr_draw_state* state, const ctr_mesh_matrix* matrix,
                              const void* verts, const uint16_t* indices, int index_count) {
  sky_hook(state);
  softstats::prog(1);
  if (verts && indices) {
    soft_mesh_tris(state, matrix->m, (const uint8_t*)verts, indices, 0, index_count);
  }
}

int ctr_gpu_copy_screen_part(float x0, float x1) {
  // (AI-assisted) the whole screen as well (no tiles here); counted like ctr_gpu_citro3d.c's rows
  const auto clampf = [](float v) { return std::clamp(v, -1.f, 1.f); };
  const int p0 = std::clamp((int)((clampf(x0) + 1.f) * 0.5f * 399.f) - 1, 0, 399);
  const int p1 = std::max(p0, std::clamp((int)((clampf(x1) + 1.f) * 0.5f * 399.f) + 2, 0, 399));
  const int h = ctr_gpu_copy_screen();
  if (softstats::enabled()) softstats::cur->copy_rows += (p1 >> 3) - (p0 >> 3) + 1 - 50;
  return h;
}

static int s_screen_copy = -1;  // (AI-assisted) ctr_gpu_copy_screen's texture

void ctr_gpu_screen_copy_release(void) {
  if (s_screen_copy >= 0) {
    ctr_gpu_tex_delete(s_screen_copy);
    s_screen_copy = -1;
  }
}

int ctr_gpu_copy_screen(void) {
  sky_hook(nullptr);
  if (softstats::enabled()) softstats::cur->copy_rows += 50;
  int& handle = s_screen_copy;
  if (handle < 0 || handle >= (int)g_soft.textures.size() || !g_soft.textures[handle].used ||
      g_soft.textures[handle].w != kW) {
    std::vector<uint8_t> empty(kW * kH * 4, 0);
    handle = ctr_gpu_tex_create(kW, kH, empty.data());
  }
  g_soft.textures[handle].rgba = g_soft.color;
  return handle;
}

double ctr_gpu_take_screen_copy_ms(void) {
  return 0;  // (AI-assisted) (no command lists here)
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
  if (handle >= 0 && handle < (int)s_tex_anim_slot.size()) {
    s_tex_anim_slot[handle] = 0;  // (AI-assisted)
  }
}

void ctr_gpu_tex_set_anim_slot(int handle, int slot) {
  if (handle < 0) {
    return;
  }
  if (handle >= (int)s_tex_anim_slot.size()) {
    s_tex_anim_slot.resize(handle + 1024, 0);
  }
  s_tex_anim_slot[handle] = slot >= 0 && slot < CTR_ANIM_SLOTS ? (uint8_t)(slot + 1) : 0;
}

uint64_t ctr_gpu_anim_slots_drawn(void) {
  return s_anim_drawn_last | s_anim_drawn;
}

void ctr_gpu_anim_slot_set(int slot, int tex) {
  if (slot >= 0 && slot < CTR_ANIM_SLOTS) {
    s_anim_slot_next[slot] = tex >= 0 ? tex + 1 : 0;
  }
}

// (AI-assisted) render to texture (impostors): as ctr_gpu_citro3d.c, with the texture's image
// in its own layout (lw x lh, top row first: logical y from the bottom)
int ctr_gpu_rt_create(int lw, int lh, int max_tile_w) {
  if (lw < 16 || lh < 16 || lw > 1024 || lh > 1024 || (lw & (lw - 1)) || (lh & (lh - 1)) ||
      max_tile_w < 16 || (max_tile_w & 15) || max_tile_w > lw) {
    return -1;
  }
  std::vector<uint8_t> empty((size_t)lw * lh * 4, 0);
  const int tex = ctr_gpu_tex_create(lw, lh, empty.data());
  if (tex < 0) {
    return -1;
  }
  SoftRt r;
  r.tex = tex;
  r.lw = lw;
  r.lh = lh;
  r.depth.assign((size_t)lw * lh, 0.f);
  s_rts.push_back(std::move(r));
  lg::info("[ctr] render to texture: {}x{} RGBA8 + D16 {}x{} ({} KB)", lw, lh, max_tile_w, lh,
           (lw * lh * 4 + max_tile_w * lh * 2) / 1024);
  return tex;
}

static SoftRt* soft_rt(int tex) {
  for (auto& r : s_rts) {
    if (r.tex == tex && tex >= 0) {
      return &r;
    }
  }
  return nullptr;
}

void ctr_gpu_rt_end(void) {
  if (!s_target.rt) {
    return;
  }
  s_target = SoftTarget{};
  s_target.color = g_soft.color.data();
  s_target.depth = g_soft.depth.data();
  s_target.stencil = g_soft.stencil.data();
}

void ctr_gpu_rt_delete(int tex) {
  for (size_t i = 0; i < s_rts.size(); i++) {
    if (s_rts[i].tex == tex) {
      ctr_gpu_rt_end();
      ctr_gpu_tex_delete(tex);
      s_rts.erase(s_rts.begin() + i);
      return;
    }
  }
}

int ctr_gpu_rt_begin(int tex, int x, int y, int w, int h) {
  SoftRt* r = soft_rt(tex);
  if (!r || x < 0 || y < 0 || w < 1 || h < 1 || (x & 15) || x + w > r->lw ||
      y + h > r->lh || tex >= (int)g_soft.textures.size() || !g_soft.textures[tex].used) {
    return 0;
  }
  s_target = SoftTarget{};
  s_target.color = g_soft.textures[tex].rgba.data();
  s_target.depth = r->depth.data();
  s_target.stencil = nullptr;
  s_target.stride = r->lw;
  s_target.x0 = x;
  s_target.x1 = x + w;
  s_target.y0 = r->lh - y - h;
  s_target.y1 = r->lh - y;
  s_target.ox = x + 0.5f * w;
  s_target.sx = 0.5f * w;
  s_target.oy = r->lh - y - 0.5f * h;
  s_target.sy = 0.5f * h;
  s_target.rt = true;
  return 1;
}

void ctr_gpu_rt_uv(int tex, float lx, float ly, float* s, float* t) {
  SoftRt* r = soft_rt(tex);
  if (!r) {
    *s = *t = 0.f;
    return;
  }
  *s = lx / r->lw;
  *t = 1.f - ly / r->lh;
}

// (AI-assisted) debugging: a texture's texels as a PNG (alpha forced to 255 where it isn't 0)
void ctr_gpu_soft_dump_tex(int tex, const char* path) {
  if (tex < 0 || tex >= (int)g_soft.textures.size() || !g_soft.textures[tex].used) {
    return;
  }
  const auto& t = g_soft.textures[tex];
  std::vector<uint8_t> out = t.rgba;
  for (size_t i = 0; i < out.size(); i += 4) {
    if (!out[i + 3]) {
      out[i] = 255;
      out[i + 1] = 0;
      out[i + 2] = 255;
    }
    out[i + 3] = 255;
  }
  file_util::write_rgba_png(path, out.data(), t.w, t.h);
}

int ctr_gpu_mesh_peek(int mesh, const void** verts, int* vertex_count, const uint16_t** indices,
                      int* index_count) {
  if (mesh < 0 || mesh >= (int)g_soft.meshes.size() || !g_soft.meshes[mesh].used ||
      g_soft.meshes[mesh].stride != 16) {
    return 0;
  }
  const auto& m = g_soft.meshes[mesh];
  *verts = m.verts.data();
  *vertex_count = (int)(m.verts.size() / 16);
  *indices = m.indices.data();
  *index_count = (int)m.indices.size();
  return 1;
}

void ctr_gpu_draw(const ctr_draw_state* state, const ctr_vertex* verts, int count) {
  sky_hook(state);
  softstats::prog(0);
  g_soft.cur.draws++;
  if (state) softstats::draw(*state);
  for (int i = 0; i + 2 < count; i += 3) {
    raster_triangle(*state, verts + i);
    g_soft.cur.triangles++;
    softstats::cur->tris++;
  }
}

// (AI-assisted) ctr_gpu_draw_quads_again: the last call's quads (the caller keeps them)
static const ctr_vertex* s_lastq_verts;
static int s_lastq_count;

void ctr_gpu_draw_quads_again(const ctr_draw_state* state) {
  if (s_lastq_verts) {
    ctr_gpu_draw_quads(state, s_lastq_verts, s_lastq_count);
  }
}

void ctr_gpu_draw_quads(const ctr_draw_state* state, const ctr_vertex* verts, int quad_count) {
  s_lastq_verts = verts;
  s_lastq_count = quad_count;
  sky_hook(state);
  softstats::prog(0);
  g_soft.cur.draws++;
  if (state) softstats::draw(*state);
  for (int q = 0; q < quad_count; q++) {
    const ctr_vertex* v = verts + 4 * q;
    const ctr_vertex t0[3] = {v[0], v[1], v[3]};
    const ctr_vertex t1[3] = {v[3], v[1], v[2]};
    raster_triangle(*state, t0);
    raster_triangle(*state, t1);
    g_soft.cur.triangles += 2;
    softstats::cur->tris += 2;
  }
}

// (AI-assisted) triangle k: vertices k, k + 1, k + 2 (the joins between strips have no area)
void ctr_gpu_draw_strip(const ctr_draw_state* state, const ctr_vertex* verts, int count) {
  sky_hook(state);
  softstats::prog(0);
  g_soft.cur.draws++;
  if (state) softstats::draw(*state);
  for (int k = 0; k + 2 < count; k++) {
    const ctr_vertex t[3] = {verts[k], verts[k + 1], verts[k + 2]};
    raster_triangle(*state, t);
    g_soft.cur.triangles++;
    softstats::cur->tris++;
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

int ctr_gpu_mesh_begin(int vertex_count, int index_count, void** verts, uint16_t** indices) {
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
  m.used = true;  // (AI-assisted) reserved; drawn once filled (the loader runs between frames)
  m.verts.resize((size_t)vertex_count * 16);
  m.indices.resize(index_count);
  *verts = m.verts.data();
  *indices = m.indices.data();
  return slot;
}

// (AI-assisted, streaming) the host keeps its own copy of the vertices and indices (as for every
// mesh) and gives the caller's block back when the mesh goes
static void (*s_soft_ext_free)(void* mem) = nullptr;
void ctr_gpu_set_ext_free(void (*fn)(void* mem)) {
  s_soft_ext_free = fn;
}
static std::map<void*, unsigned int> s_soft_blocks;
void* ctr_gpu_linear_block(unsigned int bytes) {
  void* p = malloc(bytes);
  if (p) {
    s_soft_blocks[p] = bytes;
  }
  return p;
}
void ctr_gpu_linear_block_free(void* p) {
  s_soft_blocks.erase(p);
  free(p);
}
// (AI-assisted, streaming) a model of the 3DS linear heap: env CTR_SOFT_LINEAR_KB (default 34000:
// what levels, their textures and the stream arena have of it there, about) less the level texture
// pools, the meshes and the arena's blocks
unsigned int ctr_gpu_linear_free(void) {
  static const double total =
      (getenv("CTR_SOFT_LINEAR_KB") ? atof(getenv("CTR_SOFT_LINEAR_KB")) : 34000.0) * 1024.0;
  double used = 0;
  for (const auto& p : g_soft.pools) {
    used += p.used ? (double)p.data.size() : 0.0;
  }
  for (const auto& m : g_soft.meshes) {
    used += m.used && !m.ext ? (double)(m.verts.size() + m.indices.size() * 2) : 0.0;
  }
  for (const auto& [p, n] : s_soft_blocks) {
    used += n;
  }
  return used >= total ? 0u : (unsigned int)(total - used);
}
static void soft_mesh_free(int mesh) {
  void* ext = g_soft.meshes[mesh].ext;
  g_soft.meshes[mesh] = SoftMesh();
  if (ext && s_soft_ext_free) {
    s_soft_ext_free(ext);
  }
}
int ctr_gpu_mesh_begin_ext(void* mem, int vertex_count, int index_count, void** verts,
                           uint16_t** indices) {
  if (!mem) {
    return -1;
  }
  const int slot = ctr_gpu_mesh_begin(vertex_count, index_count, verts, indices);
  if (slot >= 0) {
    g_soft.meshes[slot].ext = mem;
  }
  return slot;
}

int ctr_gpu_mesh_end(int mesh, int ok) {
  if (mesh < 0 || mesh >= (int)g_soft.meshes.size()) {
    return -1;
  }
  if (!ok) {
    soft_mesh_free(mesh);
    return -1;
  }
  return mesh;
}

int ctr_gpu_mesh_create(const void* verts,
                        int vertex_count,
                        const uint16_t* indices,
                        int index_count) {
  void* v;
  uint16_t* ix;
  const int slot = ctr_gpu_mesh_begin(vertex_count, index_count, &v, &ix);
  memcpy(v, verts, (size_t)vertex_count * 16);
  memcpy(ix, indices, (size_t)index_count * 2);
  return slot;
}

void ctr_gpu_free_pending_now(void) {}

void ctr_gpu_mesh_delete(int mesh) {
  if (mesh >= 0 && mesh < (int)g_soft.meshes.size()) {
    soft_mesh_free(mesh);
  }
}

// (AI-assisted) cull_ties (ctr_gpu_set_cull_flip)
static int g_soft_cull_flip = 0;
static int g_soft_cull_enable = 0;
static int g_soft_fog_alpha = 0;  // (AI-assisted) fog_alpha: only the program choice (statistics)

void ctr_gpu_draw_mesh(const ctr_draw_state* state,
                       const float clip[16],
                       int mesh,
                       int first_index,
                       int index_count) {
  sky_hook(state);
  // (AI-assisted) fog_alpha: the 3DS backend's second mesh program (9) for CTR_TCC_ALPHA80 draws
  softstats::prog(g_soft_fog_alpha && state && (state->tcc & CTR_TCC_ALPHA80) ? 9 : 1);
  if (mesh < 0 || mesh >= (int)g_soft.meshes.size() || !g_soft.meshes[mesh].used) {
    return;
  }
  const auto& m = g_soft.meshes[mesh];
  soft_mesh_tris(state, clip, m.verts.data(), m.indices.data(), first_index, index_count);
}

// (AI-assisted) the triangles of ctr_gpu_draw_mesh / ctr_gpu_draw_mesh_stream
static void soft_mesh_tris(const ctr_draw_state* state, const float clip[16], const uint8_t* verts,
                           const uint16_t* indices, int first_index, int index_count) {
  g_soft.cur.draws++;
  if (state) softstats::draw(*state);
  if (softstats::enabled()) softstats::cur->sub += index_count / 3;  // (AI-assisted)
  // (AI-assisted) c3l v15 atlas draws: texture coordinates in 1/32768
  const float st_scale = state && (state->tcc & CTR_TCC_ST_ATLAS) ? 1.f / 32768.f : 1.f / 1024.f;
  const bool fogged = fog_on() && !s_nofog;  // (AI-assisted, far-fog)
  for (int i = first_index; i + 2 < first_index + index_count; i += 3) {
    ctr_vertex tri[3];
    float fog[3] = {0.f, 0.f, 0.f};
    bool ok = true;
    for (int k = 0; k < 3; k++) {
      const uint8_t* src = &verts[16 * indices[i + k]];
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
      fog[k] = fogged ? mesh_fog(c[3]) : 0.f;
      tri[k].x = c[0] / c[3];
      tri[k].y = c[1] / c[3];
      tri[k].z = (1.f + c[2] / c[3]) * 0.5f;  // GS z: larger = closer (zn = +1 near)
      tri[k].s = st[0] * st_scale;
      tri[k].t = st[1] * st_scale;
      tri[k].r = src[12];
      tri[k].g = src[13];
      tri[k].b = src[14];
      tri[k].a = src[15];
    }
    if (!ok) {
      continue;
    }
    // (AI-assisted) cull_ties
    if (state && (state->tcc & CTR_TCC_CULL_BACK) && g_soft_cull_enable) {
      const float a = (tri[1].x - tri[0].x) * (tri[2].y - tri[0].y) -
                      (tri[2].x - tri[0].x) * (tri[1].y - tri[0].y);
      if (g_soft_cull_flip ? a > 0 : a < 0) {
        continue;
      }
    }
    // behind the far plane / outside z
    if ((tri[0].z < 0 && tri[1].z < 0 && tri[2].z < 0) ||
        (tri[0].z > 1 && tri[1].z > 1 && tri[2].z > 1)) {
      continue;
    }
    raster_triangle(*state, tri, true, fog);
    g_soft.cur.triangles++;
    softstats::cur->tris++;
  }
}

// (AI-assisted) the ocean with waves: no second texture layer here. The stencil buffer (its marks,
// the shadow volumes) as on the 3DS: none with config.ini depth16.

int ctr_gpu_has_stencil(void) {
  return !g_soft.depth16;
}

void* ctr_gpu_stream_alloc(int bytes) {
  if (bytes <= 0) {
    return nullptr;
  }
  s_stream.emplace_back((size_t)bytes);
  return s_stream.back().data();
}

void ctr_gpu_ocean_draw(const ctr_draw_state* state,
                        int,
                        const ctr_ocean_draw* d,
                        int mesh,
                        const void* verts,
                        const uint16_t* indices,
                        int first_index,
                        int index_count) {
  sky_hook(state);
  softstats::prog(8);
  const uint8_t* vbase = (const uint8_t*)verts;
  const uint16_t* ibase = indices;
  if (mesh >= 0) {
    if (mesh >= (int)g_soft.meshes.size() || !g_soft.meshes[mesh].used) {
      return;
    }
    vbase = g_soft.meshes[mesh].verts.data();
    ibase = g_soft.meshes[mesh].indices.data();
  }
  if (!vbase || !ibase) {
    return;
  }
  const float* clip = d->clip.m;  // row-major (the soft prepare keeps the GL matrix)
  g_soft.cur.draws++;
  if (state) softstats::draw(*state);
  for (int i = first_index; i + 2 < first_index + index_count; i += 3) {
    ctr_vertex tri[3];
    float fog[3] = {0.f, 0.f, 0.f};  // (AI-assisted, far-fog) the ocean's fog stage
    bool ok = true;
    for (int k = 0; k < 3; k++) {
      const uint8_t* src = vbase + 16 * ibase[i + k];
      int16_t pos[3];
      memcpy(pos, src, 6);
      const float in[3] = {(float)pos[0], (float)pos[1], (float)pos[2]};
      float c[4];
      for (int r = 0; r < 4; r++) {
        c[r] = clip[4 * r] * in[0] + clip[4 * r + 1] * in[1] + clip[4 * r + 2] * in[2] +
               clip[4 * r + 3];
      }
      if (c[3] < 1e-3f) {
        ok = false;
        break;
      }
      tri[k].x = c[0] / c[3];
      tri[k].y = c[1] / c[3];
      tri[k].z = (1.f + c[2] / c[3]) * 0.5f;
      fog[k] = fog_on() ? mesh_fog(c[3]) : 0.f;
      tri[k].s = d->tex[0][0] * in[0] + d->tex[0][1] * in[1] + d->tex[0][2] * in[2] + d->tex[0][3];
      tri[k].t = d->tex[1][0] * in[0] + d->tex[1][1] * in[1] + d->tex[1][2] * in[2] + d->tex[1][3];
      const float f = 1.f + d->crest[0] * 255.f * in[1];
      auto ch = [&](uint8_t v) { return (uint8_t)std::clamp(v * f, 0.f, 255.f); };
      tri[k].r = ch(src[12]);
      tri[k].g = ch(src[13]);
      tri[k].b = ch(src[14]);
      tri[k].a = src[15];
    }
    if (!ok || (tri[0].z < 0 && tri[1].z < 0 && tri[2].z < 0) ||
        (tri[0].z > 1 && tri[1].z > 1 && tri[2].z > 1)) {
      continue;
    }
    raster_triangle(*state, tri, true, fog);
    g_soft.cur.triangles++;
    softstats::cur->tris++;
  }
}

void ctr_gpu_ocean_end(void) {}

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
void ctr_gpu_flush_range(const void*, unsigned int) {}  // (AI-assisted, micro-mg)
void ctr_gpu_set_micro_mask(unsigned int) {}

void* ctr_gpu_mesh_vertices_inplace(int mesh, int* vertex_count) {
  if (mesh < 0 || mesh >= (int)g_soft.meshes.size() || !g_soft.meshes[mesh].used ||
      g_soft.meshes[mesh].stride != 16) {
    return nullptr;
  }
  *vertex_count = (int)(g_soft.meshes[mesh].verts.size() / 16);
  return g_soft.meshes[mesh].verts.data();
}

void ctr_gpu_draw_skinned(const ctr_draw_state* state,
                          const float clip[16],
                          const float* bones,
                          int palette_count,
                          const uint32_t*,
                          uint32_t,
                          const float lights[28],
                          int mesh,
                          int first_index,
                          int index_count) {
  sky_hook(state);
  // (AI-assisted) the uniforms ctr_gpu_citro3d.c uploads: what changed since the last skin draw,
  // everything after a program switch
  if (softstats::enabled()) {
    static std::vector<float> last_clip, last_bones, last_lights;
    const bool fresh = softstats::cur_prog != 2;
    const int pc = std::min(palette_count, 24);
    auto changed = [&](std::vector<float>& last, const float* v, size_t n) {
      if (fresh || last.size() != n || memcmp(last.data(), v, n * 4)) {
        last.assign(v, v + n);
        return true;
      }
      return false;
    };
    softstats::cur->unif += (changed(last_clip, clip, 16) ? 4 : 0) +
                            (changed(last_bones, bones, 12 * (size_t)pc) ? 3 * pc : 0) +
                            (changed(last_lights, lights, 28) ? 7 : 0);
  }
  softstats::prog(2);
  if (mesh < 0 || mesh >= (int)g_soft.meshes.size() || !g_soft.meshes[mesh].used) {
    return;
  }
  const auto& m = g_soft.meshes[mesh];
  g_soft.cur.draws++;
  if (state) softstats::draw(*state);
  // (AI-assisted, far-fog) merc_fog
  const bool fogged = fog_on() && state && (state->tcc & CTR_TCC_FOG);
  for (int i = first_index; i + 2 < first_index + index_count; i += 3) {
    ctr_vertex tri[3];
    float fog[3] = {0.f, 0.f, 0.f};
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
      fog[k] = fogged ? merc_fog(c[3]) : 0.f;
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
    // (AI-assisted) cull_merc (CTR_TCC_CULL_OWN), the level meshes' winding test
    if (state && (state->tcc & CTR_TCC_CULL_BACK) &&
        (g_soft_cull_enable || (state->tcc & CTR_TCC_CULL_OWN))) {
      const float a = (tri[1].x - tri[0].x) * (tri[2].y - tri[0].y) -
                      (tri[2].x - tri[0].x) * (tri[1].y - tri[0].y);
      if (g_soft_cull_flip ? a > 0 : a < 0) {
        continue;
      }
    }
    raster_triangle(*state, tri, true, fog);
    g_soft.cur.triangles++;
    softstats::cur->tris++;
  }
}

void ctr_gpu_set_merc_fog(float hvdf_w, float fog_min, float fog_max, uint8_t r, uint8_t g,
                          uint8_t b) {
  fog_min = std::clamp(fog_min, 0.f, 255.f);
  fog_max = std::clamp(fog_max, 0.f, 255.f);
  s_mfog_hvdf_w = hvdf_w;
  s_mfog_lo = 1.f - fog_max / 255.f;
  s_mfog_hi = std::max(s_mfog_lo, 1.f - fog_min / 255.f);
  s_fog_rgb[0] = r;
  s_fog_rgb[1] = g;
  s_fog_rgb[2] = b;
}

// (AI-assisted, far-fog) as the 3DS: only shader_opt's skin program has the fog
int ctr_gpu_merc_fog_ready(void) {
  return s_shader_opt;
}

void ctr_gpu_request_screenshot(const char* path) {
  g_soft.screenshot_path = path;
}

void ctr_gpu_get_stats(ctr_gpu_stats* out) {
  *out = g_soft.last;
}

// (AI-assisted, far-fog) the fog values (see mesh_fog)
void ctr_gpu_set_mesh_fog(const float* fog0, const float* fog1, uint8_t r, uint8_t g, uint8_t b) {
  memcpy(s_fog0, fog0, sizeof(s_fog0));
  memcpy(s_fog1, fog1, sizeof(s_fog1));
  s_fog_rgb[0] = r;
  s_fog_rgb[1] = g;
  s_fog_rgb[2] = b;
}
void ctr_gpu_set_mesh_nofog(int on) {  // (AI-assisted) fog_stage_skip: no fog stage
  s_nofog = on != 0;
}
// (AI-assisted) fog_alpha, tex1_gating: no fog and one texture unit here
void ctr_gpu_set_fog_alpha(int on) {
  g_soft_fog_alpha = on;
}
void ctr_gpu_set_tex1_gating(int) {}
void ctr_gpu_set_shader_opt(int on) {  // (AI-assisted) the vertex math is the same here
  s_shader_opt = on ? 1 : 0;              // (far-fog: but merc fog needs it, as on the 3DS)
}
int ctr_gpu_shader_opt_ready(void) {
  return 1;
}
int ctr_gpu_fog_alpha_ready(void) {
  return 1;
}
// (AI-assisted) cull_ties: back faces of CTR_TCC_CULL_BACK draws (counter-clockwise front faces
// on screen, x right / y up; flip: the other way)
void ctr_gpu_set_cull(int enable, int flip) {
  g_soft_cull_enable = enable ? 1 : 0;
  g_soft_cull_flip = flip ? 1 : 0;
}
void ctr_gpu_cull_reset(void) {}
void ctr_gpu_set_rgba4_as_rgba8(int on) {
  g_soft.rgba4_as_rgba8 = on;
}
int ctr_gpu_rgba4_as_rgba8(void) {
  return g_soft.rgba4_as_rgba8;
}

void ctr_gpu_set_vram_textures(int) {}
void ctr_gpu_set_color16(int) {}
void ctr_gpu_set_early_depth(int) {}
void ctr_gpu_set_early_depth_params(int, int, unsigned int) {}  // (AI-assisted) no early test
void ctr_gpu_set_tex_cache_clear(int) {}  // (AI-assisted) no texture cache
void ctr_gpu_set_mip_bias(float) {}         // (AI-assisted) the soft textures have no mips
void ctr_gpu_set_skin_rows_f24(int) {}
// (AI-assisted) no float24 uniforms here: the draws never get the words
int ctr_gpu_skin_rows_f24_on(void) {
  return 0;
}
void ctr_gpu_pack_skin_rows_f24(const float*, int, uint32_t*) {}
void ctr_gpu_set_skin_clip_f24(int) {}  // (AI-assisted)      // (AI-assisted) no uniform formats here
void ctr_gpu_set_framebuffer(int, int depth16) {
  g_soft.depth16 = depth16 ? 1 : 0;  // (AI-assisted) no stencil, like the 3DS's D16 buffer
}
int ctr_gpu_describe_target(char*, int) {
  return 0;
}
int ctr_gpu_hwprof_begin(void) {
  return 1;
}
int ctr_gpu_hwprof_stop_stages(void) {
  return 1;
}
int ctr_gpu_hwprof_end(ctr_gpu_hwprof* out) {
  memset(out, 0, sizeof(*out));
  return 1;
}
void ctr_gpu_set_steady_pacing(int) {}
void ctr_gpu_note_release(void) {}
void ctr_gpu_pacing_stats(unsigned int hist[5], int* latency, unsigned int* dropped,
                          double* buffer_wait_ms, unsigned int* just_in_time, int* floor) {
  *buffer_wait_ms = 0;
  for (int i = 0; i < 5; i++) {
    hist[i] = 0;
  }
  *latency = -1;
  *dropped = 0;
  *just_in_time = 0;
  *floor = 0;
}
void ctr_gpu_want_detailed_stats(void) {}
void ctr_gpu_set_proctex(int) {}
void ctr_gpu_set_compact_textures(int) {}
int ctr_gpu_tex_create_compact(int w, int h, const uint8_t* rgba, int /*flags*/) {
  return ctr_gpu_tex_create(w, h, rgba);
}
int ctr_gpu_tex_radial(int) {
  return 0;
}
void ctr_gpu_set_overlap(int) {}
void ctr_gpu_set_raw_cmds(int) {}
void ctr_gpu_set_vcache_reuse(int) {}
void ctr_gpu_set_cmd_dedup(int, int) {}  // (AI-assisted) no command lists here
void ctr_gpu_set_cmd_dump(int) {}  // (AI-assisted)
// (AI-assisted, gpu-rt) static_cmdlists: no command words here, the draws one by one
void ctr_gpu_set_static_cmdlists(int) {}
void ctr_gpu_draw_mesh_calls(const ctr_mesh_call* calls, int n, ctr_cmd_cache**) {
  for (int i = 0; i < n; i++) {
    ctr_gpu_set_mesh_nofog(calls[i].nofog);
    ctr_gpu_draw_mesh_prepared(calls[i].state, calls[i].matrix, calls[i].mesh,
                               calls[i].first_index, calls[i].index_count);
  }
}
void ctr_gpu_cmd_cache_free(ctr_cmd_cache** cache) {
  if (cache) {
    *cache = nullptr;
  }
}
int ctr_gpu_cmdlist_line(char*, int) {
  return 0;
}
// (AI-assisted, gpu-rt) gpu_breakdown: no GPU to time here
void ctr_gpu_set_gpu_breakdown(int) {}
void ctr_gpu_phase(int) {}
int ctr_gpu_breakdown_line(char*, int, const char*) {
  return 0;
}
void ctr_gpu_cmd_dump_next(void) {}
void ctr_gpu_set_asm_tex(int) {}
void ctr_gpu_set_defer_flush(int) {}
void ctr_gpu_set_shader_basic(int) {}
int ctr_gpu_asm_tex(void) {
  return 1;
}
void ctr_gpu_diag_stage(const char*) {}
void ctr_gpu_diag_start(void) {}
void ctr_gpu_submit_partial(void) {}
unsigned int ctr_gpu_cmd_buffer_kb() {
  return 0;  // (AI-assisted, j2-mem2) no command buffer here
}

int ctr_gpu_set_pipeline(int) {
  return 0;
}
void ctr_gpu_set_frame_overlap2(int) {}  // (AI-assisted) no GPU queue here
// (AI-assisted) sky_last / skip_clear
void ctr_gpu_set_sky_hook(ctr_gpu_hook_fn fn, void* arg) {
  s_sky_hook = fn;
  s_sky_hook_arg = arg;
}
void ctr_gpu_run_sky_hook(void) {
  const ctr_gpu_hook_fn fn = s_sky_hook;
  if (fn) {
    s_sky_hook = nullptr;
    fn(s_sky_hook_arg);
  }
}
void ctr_gpu_set_depth_far(int on) {
  s_depth_far = on != 0;
}
void ctr_gpu_set_skip_color_clear(int on) {
  s_skip_color_clear = on != 0;
}
void ctr_gpu_soft_query_begin(void) {
  s_query = true;
  s_query_px = 0;
  s_query_saved = g_soft.cur;
}

long ctr_gpu_soft_query_count(void) {
  return s_query_px;
}

void ctr_gpu_soft_query_end(void) {
  s_query = false;
  g_soft.cur = s_query_saved;
}

const char* ctr_gpu_soft_bucket_name(void) {
  return softstats::cur_name.c_str();
}
void ctr_gpu_set_mip_mode(int) {}
int ctr_gpu_is_emulator(void) {
  return 0;
}

double ctr_gpu_time_ms(void) {
  using namespace std::chrono;
  return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

unsigned long long ctr_gpu_ticks(void) {
  using namespace std::chrono;
  return (unsigned long long)duration_cast<nanoseconds>(steady_clock::now().time_since_epoch())
      .count();
}

double ctr_gpu_ticks_to_ms(unsigned long long ticks) {
  return (double)ticks / 1e6;
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
