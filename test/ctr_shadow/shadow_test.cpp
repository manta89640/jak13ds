/*!
 * @file shadow_test.cpp
 * (AI-assisted)
 * Host test of the 3DS shadow volumes (game/graphics/ctr/CtrShadow.h). Shadow DMA in the layout
 * of shadow-execute-all (constants, matrix, init, per object the two vertex tables and the four
 * index lists with their MSCALs, the end packets with the shadow color), made up from random
 * volumes and the game's math-camera, goes through the native decoder (CtrShadowVolumes) and
 * through the PC renderer's emulated VU1 program (Shadow_PS2.cpp with the real ShadowRenderer.h,
 * whose OpenGL base class comes from vu_ref/), compared primitive by primitive in the program's
 * order:
 *  - drawn whole by the program: the native vertices projected with the GS mapping are its
 *    vertices (within its 1/16 pixel truncation), and the facing is the same
 *  - clipped by the program: the same facing, and its clipped polygon is the native triangle(s)
 *    clipped against the program's planes (area, corners inside)
 *  - clipped away entirely: the native triangle(s) have no area inside those planes
 * Then the totals per facing, a test with garbage DMA (no crash, every index valid), and the
 * decoder's time for a frame with Jak-sized volumes.
 *
 *   test/ctr_shadow/build.sh && /tmp/ctr_shadow_test
 * Exit status 0 when everything matches.
 */

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "common/dma/dma.h"
#include "common/dma/dma_chain_read.h"
#include "common/dma/gs.h"
#include "common/log/log.h"

#include "game/common/vu.h"
#include "game/graphics/ctr/CtrShadow.h"
#include "game/graphics/opengl_renderer/BucketRenderer.h"  // (vu_ref/: no OpenGL)
#include "game/graphics/ctr/CtrSettings.h"

// the emulated program's entry points are private members of the PC class
#define private public
#include "game/graphics/opengl_renderer/ShadowRenderer.h"
#undef private

// ---------------------------------------------------------------------------------------------
// stubs: the GPU (the test calls the decoder directly), the log, asserts
// ---------------------------------------------------------------------------------------------

int ctr_gpu_clip_reserve(int, int, ctr_clip_vertex**, uint16_t**) {
  return 0;
}
void ctr_gpu_clip_draw_ranges(const ctr_draw_state*, const int*, const int*, int, int, int) {}
// (AI-assisted) the renderer reads shadow_casters (the test drives the decoder directly)
const CtrSettings& ctr_settings() {
  static const CtrSettings s;
  return s;
}
namespace lg::internal {
void log_message(level, LogTime&, const char* message) {
  printf("%s\n", message);
}
void log_print(const char* message) {
  printf("%s", message);
}
}  // namespace lg::internal
void private_assert_failed(const char* expr, const char* file, int line, const char* function,
                           const char* msg) {
  fprintf(stderr, "assert %s %s:%d %s %s\n", expr, file, line, function, msg);
  abort();
}
void private_assert_failed(const char* expr, const char* file, int line, const char* function,
                           const std::string_view& msg) {
  fprintf(stderr, "assert %s %s:%d %s %.*s\n", expr, file, line, function, (int)msg.size(),
          msg.data());
  abort();
}

// ---------------------------------------------------------------------------------------------
// the reference: the emulated VU1 program; xgkick records what it sends to the GS
// ---------------------------------------------------------------------------------------------

namespace {
struct Kick {
  u16 addr;   // 856 / 868: a whole triangle or quad; 940: a clipped fan; 1022: the Q reset
  bool back;  // rgba[0] > 0, ShadowRenderer::xgkick's rule
  std::vector<std::array<double, 3>> v;  // screen x, y in pixels (GS / 16), z (24 bits)
};
std::vector<Kick> g_kicks;
}  // namespace

ShadowRenderer::ShadowRenderer(const std::string& name, int my_id) : BucketRenderer(name, my_id) {}
ShadowRenderer::~ShadowRenderer() = default;
void ShadowRenderer::draw_debug_window() {}
void ShadowRenderer::render(DmaFollower&, SharedRenderState*, ScopedProfilerNode&) {}
void ShadowRenderer::draw(SharedRenderState*, ScopedProfilerNode&) {}

// ShadowRenderer::xgkick's GIF parsing, recording the vertices
void ShadowRenderer::xgkick(u16 imm) {
  Kick k;
  k.addr = imm;
  const u8* data = (const u8*)(m_vu_data + imm);
  u8 rgba[4] = {1, 2, 3, 4};
  bool eop = false;
  u32 offset = 0;
  while (!eop) {
    GifTag tag(data + offset);
    offset += 16;
    const u32 nreg = tag.nreg();
    if (tag.flg() != GifTag::Format::PACKED) {
      fprintf(stderr, "xgkick: not packed\n");
      abort();
    }
    for (u32 loop = 0; loop < tag.nloop(); loop++) {
      for (u32 reg = 0; reg < nreg; reg++) {
        switch (tag.reg(reg)) {
          case GifTag::RegisterDescriptor::AD: {
            GsRegisterAddress addr;
            memcpy(&addr, data + offset + 8, sizeof(addr));
            if (addr == GsRegisterAddress::RGBAQ) {
              memcpy(rgba, data + offset, 4);
            }
          } break;
          case GifTag::RegisterDescriptor::RGBAQ:
            for (int i = 0; i < 4; i++) {
              rgba[i] = data[offset + i * 4];
            }
            break;
          case GifTag::RegisterDescriptor::XYZF2: {
            s32 x, y;
            u64 upper;
            memcpy(&x, data + offset, 4);
            memcpy(&y, data + offset + 4, 4);
            memcpy(&upper, data + offset + 8, 8);
            k.v.push_back({x / 16.0, y / 16.0, (double)((upper >> 4) & 0xffffff)});
            k.back = rgba[0] > 0;
          } break;
          default:
            break;
        }
        offset += 16;
      }
    }
    eop = tag.eop();
  }
  g_kicks.push_back(std::move(k));
}

namespace {

// ---------------------------------------------------------------------------------------------
// the game's camera (math-camera.gc: update-math-camera, the defaults of new math-camera)
// ---------------------------------------------------------------------------------------------

struct Camera {
  float matrix[4][4] = {};  // VU 0..3: p = m0 * x + m1 * y + m2 * z + m3
  float hmg[4], inv_hmg[4], hvdf[4], pfog0;
};

Camera make_camera(float fov_deg, bool mirror) {
  const double d = 1024.0, f = 40960000.0, x_pix = 256.0, y_pix = 112.0, x_clip = 1024.0,
               y_clip = 448.0, fog_start = 40960.0, fog_end = 819200.0, fog_max = 255.0,
               fog_min = 150.0;
  const double x_ratio = std::tan(0.5 * fov_deg * M_PI / 180.0), y_ratio = 0.75 * x_ratio;
  const double min_depth = 100.0, max_depth = 16760631.0;
  const double fog_slope = d * (fog_min - fog_max) / (fog_end - fog_start);
  const double dbhr = -0.5 * (max_depth - min_depth);
  const double hdbs = dbhr / (d * (f - d));
  Camera c;
  c.matrix[0][0] = (float)(-(x_pix / (x_ratio * d)) * (mirror ? -1 : 1));
  c.matrix[1][1] = (float)(-(y_pix / (y_ratio * d)));
  c.matrix[2][2] = (float)((f + d) * hdbs);
  c.matrix[2][3] = (float)((1.0 / d) * fog_slope);
  c.matrix[3][2] = (float)(-2.0 * hdbs * f * d);
  const double hmg[4] = {1.0 / x_clip, 1.0 / y_clip, 1.0 / dbhr, 1.0 / fog_slope};
  const double inv[4] = {x_clip, y_clip, dbhr, fog_slope};
  const double hvdf_w = (fog_end * fog_max - fog_start * fog_min) / (fog_end - fog_start);
  const double hvdf[4] = {2048.0, 2048.0, 0.5 * (max_depth + min_depth), hvdf_w};
  for (int i = 0; i < 4; i++) {
    c.hmg[i] = (float)hmg[i];
    c.inv_hmg[i] = (float)inv[i];
    c.hvdf[i] = (float)hvdf[i];
  }
  c.pfog0 = (float)fog_slope;
  return c;
}

// ---------------------------------------------------------------------------------------------
// shadow DMA (the transfers of the shadow bucket)
// ---------------------------------------------------------------------------------------------

u32 vif(VifCode::Kind kind, u32 num, u32 imm) {
  return ((u32)kind << 24) | ((num & 0xff) << 16) | (imm & 0xffff);
}

struct Gif {
  u64 lo = 0, hi = 0;
};
Gif gif_tag(u32 nloop, bool eop, bool pre, u32 prim, u32 nreg, u64 regs) {
  Gif g;
  g.lo = (u64)nloop | ((u64)eop << 15) | ((u64)pre << 46) | ((u64)prim << 47) | ((u64)nreg << 60);
  g.hi = regs;
  return g;
}

struct Object {
  std::vector<std::array<float, 4>> top, dual;
  std::vector<std::array<u8, 4>> lists[4];  // MSCAL 2, 4 (single edges), 6, 4 (double edges)
};

class Dma {
 public:
  Dma() { m_arena.reserve(1 << 22); }
  void add(u32 vif0, u32 vif1, const void* data, u32 bytes) {
    const size_t at = m_arena.size();
    m_arena.resize(at + ((bytes + 15) & ~15u) + 16);
    if (bytes) {
      memcpy(m_arena.data() + at, data, bytes);
    }
    m_list.push_back({at, bytes, (u64)vif0 | ((u64)vif1 << 32)});
  }
  std::vector<DmaTransfer> transfers() const {
    std::vector<DmaTransfer> out;
    for (const auto& e : m_list) {
      DmaTransfer t;
      t.data = m_arena.data() + e.at;
      t.size_bytes = e.bytes;
      t.transferred_tag = e.tag;
      out.push_back(t);
    }
    return out;
  }

 private:
  struct Entry {
    size_t at;
    u32 bytes;
    u64 tag;
  };
  std::vector<u8> m_arena;
  std::vector<Entry> m_list;
};

void add_init(Dma& dma, const Camera& cam) {
  // shadow-vu1-add-constants: 13 qw at 0x370
  u32 c[13][4] = {};
  memcpy(c[0], cam.hmg, 16);
  memcpy(c[1], cam.inv_hmg, 16);
  const float texoffset[4] = {256.5f, 112.5f, 0, 0}, texscale[4] = {1.f / 512, 1.f / 256, 0, 0};
  memcpy(c[2], texoffset, 16);
  memcpy(c[3], texscale, 16);
  memcpy(c[4], cam.hvdf, 16);
  memcpy(c[5], &cam.pfog0, 4);
  const u32 clr0[4] = {0x80, 0, 0, 0x82}, clr1[4] = {0, 0x80, 0, 0x7f};
  memcpy(c[6], clr0, 16);
  memcpy(c[7], clr1, 16);
  const u32 prim_fan = 5 | (1 << 4);  // tri-fan, tme
  const Gif adgif = gif_tag(1, false, false, 0, 1, 0xe);
  const Gif trigif = gif_tag(1, true, true, prim_fan, 7, 0x4242421);
  const Gif quadgif = gif_tag(1, true, true, prim_fan, 9, 0x424242421);
  memcpy(c[8], &adgif, 16);
  const u64 texflush[2] = {0, 0x3f}, flush[2] = {0x3f80000000000000ull, 0x01};
  memcpy(c[9], texflush, 16);
  memcpy(c[10], flush, 16);
  memcpy(c[11], &trigif, 16);
  memcpy(c[12], &quadgif, 16);
  dma.add(vif(VifCode::Kind::STCYCL, 0, 0x404), vif(VifCode::Kind::UNPACK_V4_32, 13, 0x370), c,
          13 * 16);
  // the GIF template at 0x3ac: A+D texflush + RGBAQ, then the clipped fan's tag
  u64 g[8] = {};
  const Gif ad2 = gif_tag(1, false, false, 0, 2, 0x1e);
  const Gif fan = gif_tag(3, true, true, prim_fan, 2, 0x42);
  memcpy(g, &ad2, 16);
  g[2] = 0;
  g[3] = 63;
  memcpy(g + 6, &fan, 16);
  dma.add(vif(VifCode::Kind::STCYCL, 0, 0x404), vif(VifCode::Kind::UNPACK_V4_32, 4, 0x3ac), g,
          64);
  dma.add(vif(VifCode::Kind::STCYCL, 0, 0x404), vif(VifCode::Kind::UNPACK_V4_32, 4, 0),
          cam.matrix, 64);
  dma.add(vif(VifCode::Kind::MSCALF, 0, 10), vif(VifCode::Kind::FLUSHE, 0, 0), nullptr, 0);
  // shadow-dma-init: GS registers (ignored by both)
  const u64 init[4] = {gif_tag(1, true, false, 0, 1, 0xe).lo, 0xe, 0, 0x3f};
  dma.add(0, vif(VifCode::Kind::DIRECT, 0, 2), init, 32);
}

void add_object(Dma& dma, const Object& o) {
  dma.add(vif(VifCode::Kind::FLUSHA, 0, 0),
          vif(VifCode::Kind::UNPACK_V4_32, (u32)o.top.size(), 4), o.top.data(),
          (u32)o.top.size() * 16);
  dma.add(0, vif(VifCode::Kind::UNPACK_V4_32, (u32)o.dual.size(), 174), o.dual.data(),
          (u32)o.dual.size() * 16);
  static const u32 kEntry[4] = {2, 4, 6, 4};
  static const u32 kAddr[4] = {0x158, 0x258, 0x158, 0x258};
  for (int l = 0; l < 4; l++) {
    const auto& list = o.lists[l];
    if (list.empty()) {
      continue;  // (shadow-add-* send nothing then)
    }
    const u32 n = (u32)list.size();
    const u32 num = ((n + 1 + 3) / 4) * 4;
    std::vector<u8> data(4 * num + 16, 0);
    memcpy(data.data(), &n, 4);
    for (u32 k = 0; k < n; k++) {
      memcpy(data.data() + 4 + 4 * k, list[k].data(), 4);
    }
    const u32 mscal = vif(VifCode::Kind::MSCALF, 0, kEntry[l]);
    memcpy(data.data() + 4 * num + 12, &mscal, 4);
    dma.add(0, vif(VifCode::Kind::UNPACK_V4_8, num, 0x4000 | kAddr[l]), data.data(),
            (u32)data.size());
  }
}

void add_end(Dma& dma, const u8 rgb[3]) {
  const u64 regs[10] = {gif_tag(1, true, false, 0, 4, 0xeeee).lo, 0xeeee, 0, 0x3f, 0, 0x47, 0,
                        0x4c, 0, 0x3f};
  dma.add(vif(VifCode::Kind::FLUSHA, 0, 0), vif(VifCode::Kind::DIRECT, 0, 5), regs, 80);
  // the sprite strips with the shadow color (REGLIST: PRIM, RGBAQ)
  u64 sprite[6] = {};
  sprite[0] = 1 | (1ull << 58) | (2ull << 60);
  sprite[1] = 0x10;
  sprite[2] = 6 | (1 << 4) | (1 << 6);  // sprite, tme, abe
  sprite[3] = 0x3f80000080000000ull | rgb[0] | ((u64)rgb[1] << 8) | ((u64)rgb[2] << 16);
  sprite[4] = (1ull << 15) | (1ull << 58) | (4ull << 60);
  sprite[5] = 0x5252;
  dma.add(vif(VifCode::Kind::FLUSHA, 0, 0), vif(VifCode::Kind::DIRECT, 0, 3), sprite, 48);
  dma.add(0, vif(VifCode::Kind::DIRECT, 0, 7), regs, 80);
}

// the PC renderer's handling of the bucket (ShadowRenderer::render), into the emulated VU
void vu_feed(ShadowRenderer& vu, const DmaTransfer& t) {
  const VifCode v0 = t.vifcode0(), v1 = t.vifcode1();
  if (v1.kind == VifCode::Kind::UNPACK_V4_32) {
    const VifCodeUnpack up(v1);
    const u32 num = v1.num ? v1.num : 256;
    memcpy(vu.m_vu_data + up.addr_qw, t.data, num * 16);
  } else if (v0.kind == VifCode::Kind::MSCALF && v0.immediate == 10) {
    vu.run_mscal10_vu2c();
  } else if (v1.kind == VifCode::Kind::UNPACK_V4_8) {
    const VifCodeUnpack up(v1);
    const u32 num = v1.num ? v1.num : 256;
    for (u32 i = 0; i < num; i++) {
      u32 w[4];
      for (int j = 0; j < 4; j++) {
        w[j] = t.data[4 * i + j];
      }
      memcpy(vu.m_vu_data + up.addr_qw + i, w, 16);
    }
    u32 m;
    memcpy(&m, t.data + 4 * num + 12, 4);
    vu.run_mscal_vu2c(VifCode(m).immediate);
  }
}

// ---------------------------------------------------------------------------------------------
// comparison helpers
// ---------------------------------------------------------------------------------------------

// the program's trivial clip test of one vertex (the same float operations: ACC order, hmgescale)
bool vu_outside(const Camera& c, const float* v) {
  float p[4], s[4];
  for (int i = 0; i < 4; i++) {
    float acc = c.matrix[3][i] * 1.f;
    acc += c.matrix[0][i] * v[0];
    acc += c.matrix[1][i] * v[1];
    p[i] = acc + c.matrix[2][i] * v[2];
    s[i] = p[i] * c.hmg[i];
  }
  const float w = std::abs(s[3]);
  return s[0] > w || s[0] < -w || s[1] > w || s[1] < -w || s[2] > w || s[2] < -w;
}

struct P2 {
  double x, y;
};

// a native clip space vertex (ctr_clip_vertex) as the program's hmgescale'd clip space
std::array<double, 4> to_vu_space(const Camera& c, const ctr_clip_vertex& v) {
  const double W = v.w;
  const double px = 256.0 * v.x - (c.hvdf[0] - 2048.0) * W;
  const double py = -112.0 * v.y - (c.hvdf[1] - 2048.0) * W;
  const double pz = 8388608.0 * (v.z - (c.hvdf[2] / 8388608.0 - 1.0) * W);
  return {px * c.hmg[0], py * c.hmg[1], pz * c.hmg[2], W};
}

// a triangle (native vertices) clipped against the program's planes w +- x, y, z >= 0, in screen
// pixels
std::vector<P2> clip_to_screen(const Camera& c, const ctr_clip_vertex* const tri[3]) {
  std::vector<std::array<double, 4>> poly;
  for (int k = 0; k < 3; k++) {
    poly.push_back(to_vu_space(c, *tri[k]));
  }
  for (int plane = 0; plane < 6; plane++) {
    const int axis = plane / 2;
    const double sign = (plane & 1) ? -1.0 : 1.0;
    std::vector<std::array<double, 4>> out;
    for (size_t i = 0; i < poly.size(); i++) {
      const auto& a = poly[i];
      const auto& b = poly[(i + 1) % poly.size()];
      const double da = a[3] + sign * a[axis], db = b[3] + sign * b[axis];
      if (da >= 0) {
        out.push_back(a);
      }
      if ((da >= 0) != (db >= 0)) {
        const double t = da / (da - db);
        std::array<double, 4> v;
        for (int j = 0; j < 4; j++) {
          v[j] = a[j] + (b[j] - a[j]) * t;
        }
        out.push_back(v);
      }
    }
    poly = out;
    if (poly.size() < 3) {
      return {};
    }
  }
  std::vector<P2> s;
  for (const auto& v : poly) {
    s.push_back({v[0] / c.hmg[0] / v[3] + c.hvdf[0], v[1] / c.hmg[1] / v[3] + c.hvdf[1]});
  }
  return s;
}

double area(const std::vector<P2>& p) {
  double a = 0;
  for (size_t i = 0; i < p.size(); i++) {
    const P2& u = p[i];
    const P2& v = p[(i + 1) % p.size()];
    a += u.x * v.y - v.x * u.y;
  }
  return std::abs(a) * 0.5;
}

double perimeter(const std::vector<P2>& p) {
  double l = 0;
  for (size_t i = 0; i < p.size(); i++) {
    const P2& u = p[i];
    const P2& v = p[(i + 1) % p.size()];
    l += std::hypot(v.x - u.x, v.y - u.y);
  }
  return l;
}

// distance from q to the convex polygon p (0 inside)
double outside_distance(const std::vector<P2>& p, P2 q) {
  if (p.size() < 3) {
    return 1e30;
  }
  // orientation
  double s = 0;
  for (size_t i = 0; i < p.size(); i++) {
    const P2& u = p[i];
    const P2& v = p[(i + 1) % p.size()];
    s += u.x * v.y - v.x * u.y;
  }
  double worst = 0;
  for (size_t i = 0; i < p.size(); i++) {
    const P2& u = p[i];
    const P2& v = p[(i + 1) % p.size()];
    const double ex = v.x - u.x, ey = v.y - u.y;
    const double len = std::hypot(ex, ey);
    if (len < 1e-12) {
      continue;
    }
    double d = (ex * (q.y - u.y) - ey * (q.x - u.x)) / len;  // > 0: left of the edge
    if (s < 0) {
      d = -d;
    }
    worst = std::max(worst, -d);
  }
  return worst;
}

// ---------------------------------------------------------------------------------------------
// the test
// ---------------------------------------------------------------------------------------------

struct Totals {
  int prims = 0, whole = 0, clipped = 0, culled = 0;
  int tris_native[2] = {0, 0}, tris_vu[2] = {0, 0};  // [back, front]
  int failures = 0;
  double max_xy = 0, max_z = 0, max_area = 0;
  int rects = 0, full_screen = 0;
};

std::mt19937 g_rng(1234);
float rnd(float a, float b) {
  return std::uniform_real_distribution<float>(a, b)(g_rng);
}
int rndi(int a, int b) {
  return std::uniform_int_distribution<int>(a, b)(g_rng);
}

// a made-up volume: n points around center (camera space, 4096 = 1 m) and the same points moved by
// extrude; random triangles / edges in the four lists
Object make_object(const float center[3], float radius, const float extrude[3], int n,
                   const int counts[4]) {
  Object o;
  for (int i = 0; i < n; i++) {
    std::array<float, 4> v = {center[0] + rnd(-radius, radius), center[1] + rnd(-radius, radius),
                              center[2] + rnd(-radius, radius), rnd(0, 1)};
    o.top.push_back(v);
    o.dual.push_back({v[0] + extrude[0], v[1] + extrude[1], v[2] + extrude[2], v[3]});
  }
  for (int l = 0; l < 4; l++) {
    for (int k = 0; k < counts[l]; k++) {
      const int a = rndi(0, n - 1);
      int b = rndi(0, n - 1), c = rndi(0, n - 1);
      if (b == a) {
        b = (a + 1) % n;
      }
      if (c == a || c == b) {
        c = (std::max(a, b) + 1) % n;
      }
      if (l == 1 || l == 3) {
        o.lists[l].push_back({(u8)a, (u8)b, (u8)rndi(0, 1), 0});
      } else {
        o.lists[l].push_back({(u8)a, (u8)b, (u8)c, (u8)(l == 2 ? rndi(0, 1) : 0)});
      }
    }
  }
  return o;
}

void run_case(const char* name, const Camera& cam, const std::vector<Object>& objects,
              Totals& tot) {
  Dma dma;
  add_init(dma, cam);
  for (const auto& o : objects) {
    add_object(dma, o);
  }
  const u8 rgb[3] = {0x30, 0x38, 0x40};
  add_end(dma, rgb);
  const auto transfers = dma.transfers();

  // native
  static std::vector<ctr_clip_vertex> verts(CtrShadowVolumes::kMaxVerts);
  static std::vector<u16> front(CtrShadowVolumes::kMaxIndices), back(CtrShadowVolumes::kMaxIndices);
  CtrShadowVolumes vol;
  std::vector<CtrShadowVolumes::Prim> trace;
  vol.set_trace(&trace);
  vol.begin(verts.data(), CtrShadowVolumes::kMaxVerts, front.data(), back.data(),
            CtrShadowVolumes::kMaxIndices);
  for (const auto& t : transfers) {
    vol.transfer(t);
  }
  vol.end();
  if (vol.stats().dropped_objects || vol.stats().bad_entries || vol.stats().bad_packets ||
      vol.stats().no_camera) {
    printf("%s: unexpected decoder stats (dropped %d, bad entries %d, packets %d, no camera %d)\n",
           name, vol.stats().dropped_objects, vol.stats().bad_entries, vol.stats().bad_packets,
           vol.stats().no_camera);
    tot.failures++;
  }
  // the camera space position of each output vertex (objects in order: T, then D)
  std::vector<const float*> cam_of;
  for (const auto& o : objects) {
    for (const auto& v : o.top) {
      cam_of.push_back(v.data());
    }
    for (const auto& v : o.dual) {
      cam_of.push_back(v.data());
    }
  }
  if ((int)cam_of.size() != vol.vert_count()) {
    printf("%s: %d vertices, expected %zu\n", name, vol.vert_count(), cam_of.size());
    tot.failures++;
    return;
  }

  // reference
  g_kicks.clear();
  auto ref = std::make_unique<ShadowRenderer>("shadow", 0);
  for (const auto& t : transfers) {
    vu_feed(*ref, t);
  }

  size_t k = 0;
  int fails_here = 0;
  auto fail = [&](const std::string& msg) {
    if (fails_here++ < 8) {
      printf("%s: %s\n", name, msg.c_str());
    }
    tot.failures++;
  };
  for (size_t pi = 0; pi < trace.size(); pi++) {
    const auto& p = trace[pi];
    tot.prims++;
    tot.tris_native[p.front] += p.n - 2;
    bool outside = false;
    for (int j = 0; j < p.n; j++) {
      outside |= vu_outside(cam, cam_of[p.v[j]]);
    }
    if (!outside) {
      tot.whole++;
      if (k >= g_kicks.size() || (g_kicks[k].addr != 856 && g_kicks[k].addr != 868) ||
          (int)g_kicks[k].v.size() != p.n) {
        fail("primitive " + std::to_string(pi) + ": no matching whole kick");
        return;
      }
      const Kick& kk = g_kicks[k++];
      tot.tris_vu[!kk.back] += p.n - 2;
      if (kk.back == (p.front != 0)) {
        fail("primitive " + std::to_string(pi) + ": facing differs");
      }
      for (int j = 0; j < p.n; j++) {
        const ctr_clip_vertex& v = verts[p.v[j]];
        const double sx = v.x / (double)v.w * 256.0 + 2048.0;
        const double sy = -v.y / (double)v.w * 112.0 + 2048.0;
        const double sz = (v.z / (double)v.w + 1.0) * 8388608.0;
        const double dx = std::abs(sx - kk.v[j][0]), dy = std::abs(sy - kk.v[j][1]);
        tot.max_xy = std::max(tot.max_xy, std::max(dx, dy));
        tot.max_z = std::max(tot.max_z, std::abs(sz - kk.v[j][2]));
        // the program truncates to 1/16 pixel (ftoi4)
        if (dx > 1.0 / 16 + 2e-3 || dy > 1.0 / 16 + 2e-3 || std::abs(sz - kk.v[j][2]) > 256) {
          fail("primitive " + std::to_string(pi) + " vertex " + std::to_string(j) +
               ": position differs by " + std::to_string(dx) + ", " + std::to_string(dy) + ", " +
               std::to_string(std::abs(sz - kk.v[j][2])));
        }
      }
      continue;
    }
    // clipped by the program: compare with the native triangles clipped the same way
    std::vector<std::vector<P2>> mine;
    const int tris[2][3] = {{0, 1, 2}, {0, 2, 3}};
    double mine_area = 0, mine_perim = 0;
    for (int t = 0; t < p.n - 2; t++) {
      const ctr_clip_vertex* tri[3] = {&verts[p.v[tris[t][0]]], &verts[p.v[tris[t][1]]],
                                       &verts[p.v[tris[t][2]]]};
      mine.push_back(clip_to_screen(cam, tri));
      mine_area += area(mine.back());
      mine_perim += perimeter(mine.back());
    }
    // the program kicks a fan only when something is left (the next fan may be a later
    // primitive's): by the native polygon, or a tiny one that the program clipped away
    bool any_poly = false;
    for (const auto& m : mine) {
      any_poly |= !m.empty();
    }
    std::vector<P2> fan;
    if (any_poly && k < g_kicks.size() && g_kicks[k].addr == 940) {
      for (const auto& v : g_kicks[k].v) {
        fan.push_back({v[0], v[1]});
      }
    }
    const double fa = area(fan);
    const double tol = 0.1 * std::max(perimeter(fan), mine_perim) + 1e-3 * fa + 0.5;
    if (!fan.empty() && (mine_area >= 0.5 || std::abs(fa - mine_area) <= tol)) {
      tot.clipped++;
      const Kick& kk = g_kicks[k];
      k += 2;  // (and the Q reset at 1022)
      tot.tris_vu[!kk.back] += p.n - 2;
      if (kk.back == (p.front != 0)) {
        fail("clipped primitive " + std::to_string(pi) + ": facing differs");
      }
      tot.max_area = std::max(tot.max_area, std::abs(fa - mine_area));
      if (std::abs(fa - mine_area) > tol) {
        fail("clipped primitive " + std::to_string(pi) + ": area " + std::to_string(fa) +
             ", native " + std::to_string(mine_area));
      }
      for (const auto& q : fan) {
        double d = 1e30;
        for (const auto& m : mine) {
          d = std::min(d, outside_distance(m, q));
        }
        if (d > 0.15) {
          fail("clipped primitive " + std::to_string(pi) + ": corner " + std::to_string(q.x) +
               ", " + std::to_string(q.y) + " outside by " + std::to_string(d));
        }
      }
    } else {
      // nothing left after the program's clipping
      tot.culled++;
      tot.tris_vu[p.front] += p.n - 2;  // (no color to check: counted as the native's)
      if (mine_area > 0.5) {
        fail("culled primitive " + std::to_string(pi) + ": native area " +
             std::to_string(mine_area));
      }
    }
  }
  if (k != g_kicks.size()) {
    fail("the program sent " + std::to_string(g_kicks.size() - k) + " more packets");
  }
  // every index in range, lists as long as the trace says
  int nf = 0, nb = 0;
  for (const auto& p : trace) {
    (p.front ? nf : nb) += 3 * (p.n - 2);
  }
  if (nf != vol.front_count() || nb != vol.back_count()) {
    fail("list lengths differ from the trace");
  }
  for (int i = 0; i < vol.front_count(); i++) {
    if (front[i] >= vol.vert_count()) {
      fail("front index out of range");
      break;
    }
  }
  for (int i = 0; i < vol.back_count(); i++) {
    if (back[i] >= vol.vert_count()) {
      fail("back index out of range");
      break;
    }
  }
  // the darkening rectangles: every vertex of a front face that is on screen is inside one; a
  // vertex at or behind the camera makes them the whole screen
  for (int i = 0; i < vol.front_count(); i++) {
    const ctr_clip_vertex& v = verts[front[i]];
    if (!(v.w > 1e-4f)) {
      if (!vol.full_screen()) {
        fail("a front face reaches behind the camera, but the rectangle isn't the screen");
        break;
      }
      continue;
    }
    const float x = v.x / v.w, y = v.y / v.w;
    if (std::abs(x) > 1.f || std::abs(y) > 1.f) {
      continue;
    }
    bool in = false;
    for (int r = 0; r < vol.rect_count(); r++) {
      const auto rc = vol.rect(r);
      in |= x >= rc.x0 && x <= rc.x1 && y >= rc.y0 && y <= rc.y1;
    }
    if (!in) {
      fail("a front face's corner on screen is outside the rectangles");
      break;
    }
  }
  tot.rects += vol.rect_count();
  tot.full_screen += vol.full_screen();
  u8 col[3];
  vol.color(col);
  if (col[0] != 0x60 || col[1] != 0x70 || col[2] != 0x80) {
    fail("shadow color");
  }
  printf("%-34s %4zu primitives, %4zu packets from the program\n", name, trace.size(),
         g_kicks.size());
}

// random bytes in the index lists and vertex tables: no crash, indices valid
int garbage_test() {
  int failures = 0;
  const Camera cam = make_camera(64.f, false);
  static std::vector<ctr_clip_vertex> verts(CtrShadowVolumes::kMaxVerts);
  static std::vector<u16> front(CtrShadowVolumes::kMaxIndices), back(CtrShadowVolumes::kMaxIndices);
  for (int iter = 0; iter < 2000; iter++) {
    Dma dma;
    add_init(dma, cam);
    for (int t = 0; t < 12; t++) {
      std::vector<u8> junk(rndi(0, 4200));
      for (auto& b : junk) {
        b = (u8)rndi(0, 255);
      }
      const VifCode::Kind kinds[4] = {VifCode::Kind::UNPACK_V4_32, VifCode::Kind::UNPACK_V4_8,
                                      VifCode::Kind::DIRECT, VifCode::Kind::NOP};
      const u32 addrs[6] = {4, 174, 0x158, 0x258, 0x370, 0};
      const u32 v0 = rndi(0, 1) ? vif(VifCode::Kind::FLUSHA, 0, 0) : 0;
      dma.add(v0, vif(kinds[rndi(0, 3)], rndi(0, 255), addrs[rndi(0, 5)] | (rndi(0, 1) << 14)),
              junk.data(), (u32)junk.size());
    }
    const auto tr = dma.transfers();
    CtrShadowVolumes vol;
    vol.begin(verts.data(), 300, front.data(), back.data(), 1500);
    for (const auto& t : tr) {
      vol.transfer(t);
    }
    vol.end();
    if (vol.vert_count() > 300 || vol.front_count() + vol.back_count() > 1500) {
      failures++;
    }
    for (int i = 0; i < vol.front_count(); i++) {
      failures += front[i] >= vol.vert_count();
    }
    for (int i = 0; i < vol.back_count(); i++) {
      failures += back[i] >= vol.vert_count();
    }
    for (int i = 0; i < vol.vert_count(); i++) {
      const float c[4] = {verts[i].x, verts[i].y, verts[i].z, verts[i].w};
      for (float f : c) {
        failures += !(std::abs(f) <= 1e15f);
      }
    }
  }
  printf("garbage DMA: %d failures\n", failures);
  return failures;
}

// more objects than the frame's budget: the ones that don't fit are left out whole (vertices and
// triangles taken back out), the others drawn, every index valid
int budget_test() {
  int failures = 0;
  const Camera cam = make_camera(64.f, false);
  const float ex[3] = {0, -2.f * 4096, 0};
  const int counts[4] = {120, 60, 20, 20};
  std::vector<Object> objs;
  for (int i = 0; i < 40; i++) {
    const float c[3] = {rnd(-8192, 8192), rnd(-4096, 0), rnd(8 * 4096.f, 30 * 4096.f)};
    objs.push_back(make_object(c, 2048, ex, 118, counts));
  }
  Dma dma;
  add_init(dma, cam);
  for (const auto& o : objs) {
    add_object(dma, o);
  }
  const u8 rgb[3] = {0x40, 0x40, 0x40};
  add_end(dma, rgb);
  static std::vector<ctr_clip_vertex> verts(CtrShadowVolumes::kMaxVerts);
  static std::vector<u16> front(CtrShadowVolumes::kMaxIndices), back(CtrShadowVolumes::kMaxIndices);
  CtrShadowVolumes vol;
  vol.begin(verts.data(), CtrShadowVolumes::kMaxVerts, front.data(), back.data(),
            CtrShadowVolumes::kMaxIndices);
  for (const auto& t : dma.transfers()) {
    vol.transfer(t);
  }
  vol.end();
  const auto& st = vol.stats();
  // every kept object has all of its 236 vertices
  failures += st.objects + st.dropped_objects != 40 || st.dropped_objects == 0 ||
              vol.vert_count() != 236 * st.objects;
  failures += vol.front_count() + vol.back_count() > CtrShadowVolumes::kMaxIndices;
  for (int i = 0; i < vol.front_count(); i++) {
    failures += front[i] >= vol.vert_count();
  }
  for (int i = 0; i < vol.back_count(); i++) {
    failures += back[i] >= vol.vert_count();
  }
  printf("budget: %d objects drawn, %d left out, %d vertices, %d + %d triangles: %d failures\n",
         st.objects, st.dropped_objects, vol.vert_count(), vol.front_count() / 3,
         vol.back_count() / 3, failures);
  return failures;
}

// time of the decoder for a frame like Geyser Rock's: Jak (~100 vertices, 80 facing triangles,
// 40 silhouette edges) and two smaller objects
void timing() {
  const Camera cam = make_camera(64.f, false);
  std::vector<Object> objs;
  const float ex[3] = {0, -2.f * 4096, 0};
  const float c0[3] = {0, -2048, 6 * 4096.f}, c1[3] = {4096, 0, 12 * 4096.f},
              c2[3] = {-8192, 0, 15 * 4096.f};
  const int jak[4] = {80, 40, 0, 0}, small[4] = {20, 12, 0, 0};
  objs.push_back(make_object(c0, 2048, ex, 100, jak));
  objs.push_back(make_object(c1, 1024, ex, 30, small));
  objs.push_back(make_object(c2, 1024, ex, 30, small));
  Dma dma;
  add_init(dma, cam);
  for (const auto& o : objs) {
    add_object(dma, o);
  }
  const u8 rgb[3] = {0x40, 0x40, 0x40};
  add_end(dma, rgb);
  const auto tr = dma.transfers();
  static std::vector<ctr_clip_vertex> verts(CtrShadowVolumes::kMaxVerts);
  static std::vector<u16> front(CtrShadowVolumes::kMaxIndices), back(CtrShadowVolumes::kMaxIndices);
  CtrShadowVolumes vol;
  const int n = 20000;
  const auto t0 = std::chrono::steady_clock::now();
  for (int i = 0; i < n; i++) {
    vol.begin(verts.data(), CtrShadowVolumes::kMaxVerts, front.data(), back.data(),
              CtrShadowVolumes::kMaxIndices);
    for (const auto& t : tr) {
      vol.transfer(t);
    }
    vol.end();
  }
  const double us =
      std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / n;
  // the same through the emulated program
  auto ref = std::make_unique<ShadowRenderer>("shadow", 0);
  const int m = 2000;
  const auto t1 = std::chrono::steady_clock::now();
  for (int i = 0; i < m; i++) {
    g_kicks.clear();
    for (const auto& t : tr) {
      vu_feed(*ref, t);
    }
  }
  const double us_vu =
      std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t1).count() / m;
  printf("time per frame (%d vertices, %d front + %d back triangles): native %.2f us, emulated VU1 "
         "program %.2f us (host)\n",
         vol.vert_count(), vol.front_count() / 3, vol.back_count() / 3, us, us_vu);
}
}  // namespace

int main() {
  Totals tot;
  const float down[3] = {0, -3.f * 4096, 0};
  const float slant[3] = {2048, -4.f * 4096, 1024};
  const int counts[4] = {40, 30, 20, 15};
  for (int seed = 0; seed < 40; seed++) {
    g_rng.seed(seed);
    const bool mirror = seed % 7 == 3;
    const Camera cam = make_camera(seed % 2 ? 64.f : 50.f, mirror);
    std::vector<Object> objs;
    std::string name = "seed " + std::to_string(seed);
    for (int o = 0; o < 4; o++) {
      float c[3];
      const float z = rnd(2 * 4096.f, 40 * 4096.f);
      c[2] = z;
      c[0] = rnd(-0.6f, 0.6f) * z;
      c[1] = rnd(-0.45f, 0.45f) * z;
      objs.push_back(make_object(c, rnd(512, 4096), o % 2 ? down : slant, rndi(4, 120), counts));
    }
    // crossing the near plane / behind the camera (every other case: the others get one
    // darkening rectangle per object instead of the whole screen)
    if (seed % 2 == 0) {
      const float c[3] = {rnd(-2048, 2048), rnd(-2048, 2048), rnd(-1024, 3072)};
      objs.push_back(make_object(c, 3000, down, rndi(10, 60), counts));
    }
    // beyond the program's guard band (4x the half screen) and partly in view
    {
      const float z = rnd(4096, 20000);
      const float c[3] = {(seed % 2 ? 1 : -1) * 0.625f * z * rnd(1.f, 5.f), rnd(-0.3f, 0.3f) * z,
                          z};
      objs.push_back(make_object(c, z * 0.6f, slant, rndi(10, 60), counts));
    }
    run_case(name.c_str(), cam, objs, tot);
  }
  printf("primitives %d: %d whole, %d clipped by the program, %d clipped away; triangles native "
         "%d front %d back, program %d front %d back\n",
         tot.prims, tot.whole, tot.clipped, tot.culled, tot.tris_native[1], tot.tris_native[0],
         tot.tris_vu[1], tot.tris_vu[0]);
  printf("largest differences: screen x / y %.4f px, depth %.1f (of 2^24), clipped area %.3f px^2\n",
         tot.max_xy, tot.max_z, tot.max_area);
  printf("darkening rectangles: %d (%d frames full screen)\n", tot.rects, tot.full_screen);
  if (tot.tris_native[0] != tot.tris_vu[0] || tot.tris_native[1] != tot.tris_vu[1]) {
    printf("triangle counts per facing differ\n");
    tot.failures++;
  }
  tot.failures += garbage_test();
  tot.failures += budget_test();
  timing();
  printf("%s (%d failures)\n", tot.failures ? "FAILED" : "OK", tot.failures);
  return tot.failures ? 1 : 0;
}
