/*!
 * @file stencil_test.cpp
 * Host test of the 3DS shadow volumes' stencil result. A closed, non-convex caster (Jak's size,
 * Jak's shadow settings: target-util.gc *target-shadow-control*) over a ground with a ledge is
 * turned into the shadow bucket's DMA as shadow-execute builds it (facing triangles, silhouette
 * edges, duals on the bottom plane towards the light locus, the near scissor at twice the near
 * distance), for many camera poses: far, close, inside the volume, below the bottom plane, at
 * steep angles. CtrShadowVolumes turns it into the GPU's draws, and a rasterizer with the GPU's
 * stencil passes draws them over the ground's depth:
 *  - the exact count: per pixel the faces in front of the ground, front + 1, back - 1, after
 *    the near plane, clamped at 0 (front faces first, saturating like the GPU)
 *  - the GPU models: env STENCIL_F24=1 rounds the clip space vertices to float24 (the PICA's
 *    vertex input), STENCIL_GUARD=T drops a triangle with a corner beyond T (x / w or y / w after
 *    the near clip), STENCIL_NOCLIP=1 doesn't clip at the near plane (only w > 0),
 *    STENCIL_BACK_INCR=1 counts the back faces up too (the front pass's op kept)
 *  - STENCIL_SCENE=ledge: Jak in the air beside a ledge, a cliff face below (the console's band)
 * It reports the darkened pixels where the exact count is 0 (a shadow where there is none) and
 * the poses where they cover most of the darkening area (the solid quad).
 *
 *   test/ctr_shadow/build_stencil.sh && /tmp/ctr_stencil_test
 */

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "common/dma/dma.h"
#include "common/dma/dma_chain_read.h"
#include "common/log/log.h"

#include "game/graphics/ctr/CtrSettings.h"
#include "game/graphics/ctr/CtrShadow.h"

int ctr_gpu_clip_reserve(int, int, ctr_clip_vertex**, uint16_t**) {
  return 0;
}
void ctr_gpu_clip_draw_ranges(const ctr_draw_state*, const int*, const int*, int, int, int) {}
namespace ctr_settings_detail {
const CtrSettings* g_settings = nullptr;
const CtrSettings& first_use() {
  static const CtrSettings s;
  g_settings = &s;
  return s;
}
}  // namespace ctr_settings_detail
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

namespace {

// ---------------------------------------------------------------------------------------------
// the game's camera (math-camera.gc) and the shadow bucket's DMA (as shadow_test.cpp)
// ---------------------------------------------------------------------------------------------

struct Camera {
  float matrix[4][4] = {};
  float hmg[4], inv_hmg[4], hvdf[4], pfog0;
};

Camera make_camera(float fov_deg) {
  const double d = 1024.0, f = 40960000.0, x_pix = 256.0, y_pix = 112.0, x_clip = 1024.0,
               y_clip = 448.0, fog_start = 40960.0, fog_end = 819200.0, fog_max = 255.0,
               fog_min = 150.0;
  const double x_ratio = std::tan(0.5 * fov_deg * M_PI / 180.0), y_ratio = 0.75 * x_ratio;
  const double min_depth = 100.0, max_depth = 16760631.0;
  const double fog_slope = d * (fog_min - fog_max) / (fog_end - fog_start);
  const double dbhr = -0.5 * (max_depth - min_depth);
  const double hdbs = dbhr / (d * (f - d));
  Camera c;
  c.matrix[0][0] = (float)(-(x_pix / (x_ratio * d)));
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

u32 vif(VifCode::Kind kind, u32 num, u32 imm) {
  return ((u32)kind << 24) | ((num & 0xff) << 16) | (imm & 0xffff);
}

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

struct Object {
  std::vector<std::array<float, 4>> top, dual;
  std::vector<std::array<u8, 4>> tris, edges;  // MSCAL 2, 4
};

void add_init(Dma& dma, const Camera& cam) {
  u32 c[13][4] = {};
  memcpy(c[0], cam.hmg, 16);
  memcpy(c[1], cam.inv_hmg, 16);
  memcpy(c[4], cam.hvdf, 16);
  memcpy(c[5], &cam.pfog0, 4);
  dma.add(vif(VifCode::Kind::STCYCL, 0, 0x404), vif(VifCode::Kind::UNPACK_V4_32, 13, 0x370), c,
          13 * 16);
  dma.add(vif(VifCode::Kind::STCYCL, 0, 0x404), vif(VifCode::Kind::UNPACK_V4_32, 4, 0),
          cam.matrix, 64);
  dma.add(vif(VifCode::Kind::MSCALF, 0, 10), vif(VifCode::Kind::FLUSHE, 0, 0), nullptr, 0);
}

void add_list(Dma& dma, const std::vector<std::array<u8, 4>>& list, u32 entry, u32 addr) {
  if (list.empty()) {
    return;
  }
  const u32 n = (u32)list.size();
  const u32 num = ((n + 1 + 3) / 4) * 4;
  std::vector<u8> data(4 * num + 16, 0);
  memcpy(data.data(), &n, 4);
  for (u32 k = 0; k < n; k++) {
    memcpy(data.data() + 4 + 4 * k, list[k].data(), 4);
  }
  const u32 mscal = vif(VifCode::Kind::MSCALF, 0, entry);
  memcpy(data.data() + 4 * num + 12, &mscal, 4);
  dma.add(0, vif(VifCode::Kind::UNPACK_V4_8, num, 0x4000 | addr), data.data(), (u32)data.size());
}

void add_object(Dma& dma, const Object& o) {
  dma.add(vif(VifCode::Kind::FLUSHA, 0, 0),
          vif(VifCode::Kind::UNPACK_V4_32, (u32)o.top.size(), 4), o.top.data(),
          (u32)o.top.size() * 16);
  dma.add(0, vif(VifCode::Kind::UNPACK_V4_32, (u32)o.dual.size(), 174), o.dual.data(),
          (u32)o.dual.size() * 16);
  add_list(dma, o.tris, 2, 0x158);
  add_list(dma, o.edges, 4, 0x258);
}

// ---------------------------------------------------------------------------------------------
// the caster and its volume, as shadow-execute makes it (native_shadow.cpp)
// ---------------------------------------------------------------------------------------------

using V3 = std::array<double, 3>;
V3 sub(const V3& a, const V3& b) {
  return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
V3 add(const V3& a, const V3& b) {
  return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}
V3 mul(const V3& a, double s) {
  return {a[0] * s, a[1] * s, a[2] * s};
}
double dot(const V3& a, const V3& b) {
  return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
V3 cross(const V3& a, const V3& b) {
  return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
V3 norm(const V3& a) {
  return mul(a, 1.0 / std::sqrt(dot(a, a)));
}

constexpr double kM = 4096.0;  // game units per meter

struct Mesh {
  std::vector<V3> v;                  // world
  std::vector<std::array<int, 3>> t;  // outward winding
};

// a bumpy ellipsoid (non-convex, closed): Jak's size, his center 0.8 m above the ground
Mesh make_caster() {
  Mesh m;
  const int nu = 12, nv = 8;
  const V3 c = {0, 0.85 * kM, 0};
  m.v.push_back(add(c, {0, 0.85 * kM, 0}));  // top
  for (int j = 1; j < nv; j++) {
    const double phi = M_PI * j / nv;
    for (int i = 0; i < nu; i++) {
      const double th = 2 * M_PI * i / nu;
      const double r = 1.0 + 0.35 * std::sin(3 * th) * std::sin(2 * phi);
      m.v.push_back(add(c, {0.3 * kM * r * std::sin(phi) * std::cos(th), 0.85 * kM * std::cos(phi),
                            0.22 * kM * r * std::sin(phi) * std::sin(th)}));
    }
  }
  m.v.push_back(add(c, {0, -0.85 * kM, 0}));  // bottom
  const int bot = (int)m.v.size() - 1;
  auto ring = [&](int j, int i) { return 1 + (j - 1) * nu + (i % nu); };
  for (int i = 0; i < nu; i++) {
    m.t.push_back({0, ring(1, i + 1), ring(1, i)});
  }
  for (int j = 1; j < nv - 1; j++) {
    for (int i = 0; i < nu; i++) {
      m.t.push_back({ring(j, i), ring(j, i + 1), ring(j + 1, i + 1)});
      m.t.push_back({ring(j, i), ring(j + 1, i + 1), ring(j + 1, i)});
    }
  }
  for (int i = 0; i < nu; i++) {
    m.t.push_back({bot, ring(nv - 1, i), ring(nv - 1, i + 1)});
  }
  // outward check (the ellipsoid's center): flip any inward triangle
  for (auto& t : m.t) {
    const V3 n = cross(sub(m.v[t[1]], m.v[t[0]]), sub(m.v[t[2]], m.v[t[0]]));
    if (dot(n, sub(m.v[t[0]], c)) < 0) {
      std::swap(t[1], t[2]);
    }
  }
  return m;
}

struct Pose {
  V3 pos, look;  // world
};

// world -> camera space (x left, y up, z forward: the game's)
struct View {
  V3 ax, ay, az, pos;
  V3 to_cam(const V3& p) const {
    const V3 d = sub(p, pos);
    return {dot(d, ax), dot(d, ay), dot(d, az)};
  }
  V3 dir_to_cam(const V3& d) const { return {dot(d, ax), dot(d, ay), dot(d, az)}; }
};

View make_view(const Pose& p) {
  View v;
  v.pos = p.pos;
  v.az = norm(sub(p.look, p.pos));
  V3 up = {0, 1, 0};
  if (std::abs(v.az[1]) > 0.999) {
    up = {0, 0, 1};
  }
  const V3 right = norm(cross(v.az, up));
  v.ax = mul(right, -1);  // left
  v.ay = cross(v.az, v.ax);
  return v;
}

struct Volume {
  Object obj;
  std::vector<V3> T, D;  // camera space after the scissor
};

Volume make_volume(const Mesh& m, const View& view) {
  // Jak: shadow-dir (-0.4226, -0.9063, 0), dist-to-locus 100 m, bottom plane 9.2 m below the
  // center (flags: disable-fade only)
  const V3 center_w = {0, 0.85 * kM, 0};
  const V3 dir_w = {-0.4226, -0.9063, 0};
  const V3 locus = view.to_cam(add(center_w, mul(dir_w, 409600.0)));
  const V3 n = view.dir_to_cam({0, 1, 0});
  double d = (37683.2 - center_w[1]) + dot(V3{0, 1, 0}, view.pos);
  if (dot(locus, n) + d > 0) {
    d = -dot(locus, n);
  }
  const V3 light = view.dir_to_cam(dir_w);
  Volume vol;
  for (const auto& p : m.v) {
    vol.T.push_back(view.to_cam(p));
  }
  for (const auto& p : vol.T) {
    const V3 cp = sub(locus, p);
    const double t = (dot(p, n) + d) / dot(cp, n);
    vol.D.push_back(sub(p, mul(cp, t)));
  }
  // shadow-scissor-edges: twice the near distance
  for (size_t i = 0; i < vol.T.size(); i++) {
    V3& T = vol.T[i];
    V3& D = vol.D[i];
    const double z5 = T[2] - 2048.0, z8 = D[2] - 2048.0;
    const bool vtx_in_front = z5 < 0;
    if (vtx_in_front ? z8 < 0 : z8 >= 0) {
      continue;
    }
    const double q = z5 / (T[2] - D[2]);
    const V3 w = add(T, mul(sub(D, T), q));
    (vtx_in_front ? T : D) = w;
  }
  // facing (light) before the scissor moved anything: shadow-find-facing-single-tris runs after
  // it, on the moved vertices, like the game
  std::vector<u8> facing(m.t.size());
  for (size_t k = 0; k < m.t.size(); k++) {
    const auto& t = m.t[k];
    const V3 c = cross(sub(vol.T[t[1]], vol.T[t[0]]), sub(vol.T[t[2]], vol.T[t[0]]));
    facing[k] = dot(c, light) < 0;
    if (facing[k]) {
      vol.obj.tris.push_back({(u8)t[0], (u8)t[1], (u8)t[2], 1});
    }
  }
  // the edges, oriented as in tri-0
  for (size_t k = 0; k < m.t.size(); k++) {
    for (int e = 0; e < 3; e++) {
      const int a = m.t[k][e], b = m.t[k][(e + 1) % 3];
      for (size_t k2 = 0; k2 < m.t.size(); k2++) {
        if (k2 == k) {
          continue;
        }
        bool shares = false;
        for (int e2 = 0; e2 < 3; e2++) {
          shares |= m.t[k2][e2] == b && m.t[k2][(e2 + 1) % 3] == a;
        }
        if (shares && k < k2 && facing[k] != facing[k2]) {
          vol.obj.edges.push_back({(u8)a, (u8)b, (u8)(facing[k] == 0), 0});
        }
      }
    }
  }
  for (size_t i = 0; i < vol.T.size(); i++) {
    vol.obj.top.push_back({(float)vol.T[i][0], (float)vol.T[i][1], (float)vol.T[i][2], 1.f});
    vol.obj.dual.push_back({(float)vol.D[i][0], (float)vol.D[i][1], (float)vol.D[i][2], 1.f});
  }
  return vol;
}

// ---------------------------------------------------------------------------------------------
// the receivers: the ground with a ledge 0.4 m beside Jak, 2 m down, and a wall
// ---------------------------------------------------------------------------------------------

struct Plane {
  V3 n;  // world
  double d;
  double xmin, xmax;  // world x range
  double ymin = -1e30, ymax = 1e30;  // world y range
};
const Plane kReceivers[] = {
    {{0, 1, 0}, 0.0, -1e30, 0.4 * kM},
    {{0, 1, 0}, 2.0 * kM, 0.4 * kM, 1e30},
    {{0, 0, -1}, 1.2 * kM, -1e30, 1e30},  // z = 1.2 m
};
// STENCIL_SCENE=ledge: Jak in the air 0.5 m above a path whose edge is 0.35 m to his left, a
// cliff face down to the ground 4 m below him
const Plane kLedge[] = {
    {{0, 1, 0}, 0.5 * kM, -1e30, -0.35 * kM},
    {{1, 0, 0}, 0.35 * kM, -1e30, 1e30, -4.0 * kM, -0.5 * kM},
    {{0, 1, 0}, 4.0 * kM, -0.35 * kM, 1e30},
};

// ---------------------------------------------------------------------------------------------
// the GPU: clip, raster with a top-left rule, depth GEQUAL, stencil incr / decr
// ---------------------------------------------------------------------------------------------

constexpr int kW = 400, kH = 240;

struct Model {
  bool f24 = false, noclip = false;
  bool back_incr = false;  // the back pass kept the front pass's INCR op
  float guard = 0.f;
};

float f24(float f) {
  u32 u;
  memcpy(&u, &f, 4);
  u = (u + 0x40u) & ~0x7fu;
  memcpy(&f, &u, 4);
  return f;
}

struct CV {
  double x, y, z, w;
};

void raster(const CV* v3, const float* depth, std::vector<int>& sten, int delta) {
  double sx[3], sy[3], sz[3];
  for (int i = 0; i < 3; i++) {
    sx[i] = 200.0 + 200.0 * v3[i].x / v3[i].w;
    sy[i] = 120.0 - 120.0 * v3[i].y / v3[i].w;
    sz[i] = (v3[i].z / v3[i].w + 1.0) * 0.5;
  }
  double area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
  if (std::abs(area) < 1e-12) {
    return;
  }
  const int x0 = std::max(0, (int)std::floor(std::min({sx[0], sx[1], sx[2]})));
  const int x1 = std::min(kW - 1, (int)std::ceil(std::max({sx[0], sx[1], sx[2]})));
  const int y0 = std::max(0, (int)std::floor(std::min({sy[0], sy[1], sy[2]})));
  const int y1 = std::min(kH - 1, (int)std::ceil(std::max({sy[0], sy[1], sy[2]})));
  auto edge = [&](int a, int b, double px, double py) {
    return ((sx[b] - sx[a]) * (py - sy[a]) - (sy[b] - sy[a]) * (px - sx[a])) / area;
  };
  // top-left: an edge on which w == 0 counts for one of its two triangles only
  auto owns = [&](int a, int b) {
    const double ex = sx[b] - sx[a], ey = sy[b] - sy[a];
    const double s = area > 0 ? 1 : -1;
    return (ey * s < 0) || (ey == 0 && ex * s > 0);
  };
  const bool o0 = owns(1, 2), o1 = owns(2, 0), o2 = owns(0, 1);
  for (int py = y0; py <= y1; py++) {
    for (int px = x0; px <= x1; px++) {
      const double fx = px + 0.5, fy = py + 0.5;
      const double w0 = edge(1, 2, fx, fy), w1 = edge(2, 0, fx, fy), w2 = edge(0, 1, fx, fy);
      if (w0 < 0 || w1 < 0 || w2 < 0 || (w0 == 0 && !o0) || (w1 == 0 && !o1) ||
          (w2 == 0 && !o2)) {
        continue;
      }
      const double z = w0 * sz[0] + w1 * sz[1] + w2 * sz[2];
      const int i = px + py * kW;
      if (z >= depth[i]) {
        sten[i] = delta > 0 ? std::min(255, sten[i] + 1) : std::max(0, sten[i] - 1);
      }
    }
  }
}

void draw_tri(const Model& md, const ctr_clip_vertex* const t[3], const float* depth,
              std::vector<int>& sten, int delta, int& dropped) {
  CV poly[8], tmp[8];
  int n = 3;
  for (int k = 0; k < 3; k++) {
    float x = t[k]->x, y = t[k]->y, z = t[k]->z, w = t[k]->w;
    if (md.f24) {
      x = f24(x), y = f24(y), z = f24(z), w = f24(w);
    }
    poly[k] = {x, y, z, w};
  }
  const int planes = md.noclip ? 1 : 3;
  for (int plane = 0; plane < planes; plane++) {
    auto dist = [&](const CV& v) {
      return md.noclip || plane == 2 ? v.w - 1e-5 : plane == 0 ? v.w - v.z : v.w + v.z;
    };
    int m = 0;
    for (int i = 0; i < n; i++) {
      const CV& a = poly[i];
      const CV& b = poly[(i + 1) % n];
      const double da = dist(a), db = dist(b);
      if (da >= 0) {
        tmp[m++] = a;
      }
      if ((da >= 0) != (db >= 0)) {
        const double s = da / (da - db);
        tmp[m++] = {a.x + (b.x - a.x) * s, a.y + (b.y - a.y) * s, a.z + (b.z - a.z) * s,
                    a.w + (b.w - a.w) * s};
      }
    }
    n = m;
    if (n < 3) {
      return;
    }
    std::copy(tmp, tmp + n, poly);
  }
  if (md.guard > 0) {
    for (int i = 0; i < n; i++) {
      if (std::abs(poly[i].x / poly[i].w) > md.guard ||
          std::abs(poly[i].y / poly[i].w) > md.guard) {
        dropped++;
        return;
      }
    }
  }
  for (int i = 1; i + 1 < n; i++) {
    const CV tri[3] = {poly[0], poly[i], poly[i + 1]};
    raster(tri, depth, sten, delta);
  }
}

// ---------------------------------------------------------------------------------------------
// one pose
// ---------------------------------------------------------------------------------------------

void inv4(const double m[16], double out[16]) {
  double a[4][8];
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      a[r][c] = m[4 * r + c];
      a[r][4 + c] = r == c;
    }
  }
  for (int c = 0; c < 4; c++) {
    int p = c;
    for (int r = c + 1; r < 4; r++) {
      if (std::abs(a[r][c]) > std::abs(a[p][c])) {
        p = r;
      }
    }
    std::swap(a[c], a[p]);
    const double s = 1.0 / a[c][c];
    for (int k = 0; k < 8; k++) {
      a[c][k] *= s;
    }
    for (int r = 0; r < 4; r++) {
      if (r != c) {
        const double f = a[r][c];
        for (int k = 0; k < 8; k++) {
          a[r][k] -= f * a[c][k];
        }
      }
    }
  }
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      out[4 * r + c] = a[r][4 + c];
    }
  }
}

// ray (camera origin, dir) against triangle (Moller-Trumbore): t or -1
double ray_tri(const V3& dir, const V3& a, const V3& b, const V3& c) {
  const V3 e1 = sub(b, a), e2 = sub(c, a);
  const V3 p = cross(dir, e2);
  const double det = dot(e1, p);
  if (std::abs(det) < 1e-18) {
    return -1;
  }
  const V3 s = mul(a, -1.0);
  const double u = dot(s, p) / det;
  if (u < 0 || u > 1) {
    return -1;
  }
  const V3 q = cross(s, e1);
  const double v = dot(dir, q) / det;
  if (v < 0 || u + v > 1) {
    return -1;
  }
  return dot(e2, q) / det;
}

struct Result {
  int dark = 0;        // pixels with stencil != 0 inside the darkening area
  int false_dark = 0;  // ... where the exact count is 0
  int missing = 0;     // exact count > 0, stencil 0
  int rect_area = 0;
  int dropped = 0;
  long fill = 0;  // fragments of the darkening pass
  long old_rect = 0;  // the bounding rectangle the darkening pass covered before
  bool full_screen = false;
  int clipped = 0;     // triangles cut to the guard band
  int tris = 0;        // triangles sent to the GPU (front + back)
  double build_us = 0;  // the volume's CPU build (host, best of a few runs)
};

Result run_pose(const Mesh& mesh, const Camera& cam, const Pose& pose, const Model& md,
                bool dump) {
  const View view = make_view(pose);
  const Volume vol = make_volume(mesh, view);
  Dma dma;
  add_init(dma, cam);
  add_object(dma, vol.obj);
  static std::vector<ctr_clip_vertex> verts(CtrShadowVolumes::kMaxVerts);
  static std::vector<u16> front(CtrShadowVolumes::kMaxIndices), back(CtrShadowVolumes::kMaxIndices);
  CtrShadowVolumes v;
  std::vector<CtrShadowVolumes::Prim> trace;
  v.set_trace(&trace);
  v.begin(verts.data(), CtrShadowVolumes::kMaxVerts, front.data(), back.data(),
          CtrShadowVolumes::kMaxIndices);
  for (const auto& t : dma.transfers()) {
    v.transfer(t);
  }
  v.end();
  Result res;
  res.clipped = v.stats().clipped;
  res.tris = (v.front_count() + v.back_count()) / 3;
  {
    // the CPU cost of the build alone (no trace), best of 5
    static std::vector<ctr_clip_vertex> tv(CtrShadowVolumes::kMaxVerts);
    static std::vector<u16> tf(CtrShadowVolumes::kMaxIndices), tb(CtrShadowVolumes::kMaxIndices);
    double best = 1e30;
    for (int rep = 0; rep < 5; rep++) {
      CtrShadowVolumes tvol;
      const auto t0 = std::chrono::steady_clock::now();
      tvol.begin(tv.data(), CtrShadowVolumes::kMaxVerts, tf.data(), tb.data(),
                 CtrShadowVolumes::kMaxIndices);
      for (const auto& t : dma.transfers()) {
        tvol.transfer(t);
      }
      tvol.end();
      const auto t1 = std::chrono::steady_clock::now();
      best = std::min(best, std::chrono::duration<double, std::micro>(t1 - t0).count());
    }
    res.build_us = best;
  }
  if (dump) {
    printf("tris %zu edges %zu: front %d back %d verts %d clipped %d (%d); T0 (%.0f %.0f %.0f) D0 "
           "(%.0f %.0f %.0f)\n",
           vol.obj.tris.size(), vol.obj.edges.size(), v.front_count() / 3, v.back_count() / 3,
           v.vert_count(), v.stats().clipped, 0, vol.T[0][0], vol.T[0][1], vol.T[0][2],
           vol.D[0][0], vol.D[0][1], vol.D[0][2]);
  }

  // the receivers' depth, and the exact count per pixel
  double M[16], Mi[16];
  for (int i = 0; i < 16; i++) {
    M[i] = v.clip_matrix()[i];
  }
  inv4(M, Mi);
  auto unproject = [&](double x, double y, double z) {
    const double c[4] = {x, y, z, 1};
    double o[4] = {0, 0, 0, 0};
    for (int r = 0; r < 4; r++) {
      for (int k = 0; k < 4; k++) {
        o[r] += Mi[4 * r + k] * c[k];
      }
    }
    return V3{o[0] / o[3], o[1] / o[3], o[2] / o[3]};
  };
  // the GPU's near plane in camera space (z where clip z = w)
  const double near_z = unproject(0, 0, 1)[2];
  std::vector<float> depth(kW * kH, 0.f);
  std::vector<int> exact(kW * kH, 0);
  // the exact count from the program's own (uncut) triangles
  std::vector<std::array<V3, 3>> ftris, btris;
  auto vert = [&](u16 i) { return i < vol.T.size() ? vol.T[i] : vol.D[i - vol.T.size()]; };
  for (const auto& pr : trace) {
    auto& out = pr.front ? ftris : btris;
    out.push_back({vert(pr.v[0]), vert(pr.v[1]), vert(pr.v[2])});
    if (pr.n == 4) {
      out.push_back({vert(pr.v[0]), vert(pr.v[2]), vert(pr.v[3])});
    }
  }
  for (int py = 0; py < kH; py++) {
    for (int px = 0; px < kW; px++) {
      const double nx = (px + 0.5 - 200.0) / 200.0, ny = (120.0 - (py + 0.5)) / 120.0;
      const V3 dir = norm(sub(unproject(nx, ny, -0.5), unproject(nx, ny, 0.0)));
      // nearest receiver
      double best = 1e30;
      static const bool ledge = getenv("STENCIL_SCENE") && !strcmp(getenv("STENCIL_SCENE"), "ledge");
      for (const auto& pl : ledge ? std::vector<Plane>(std::begin(kLedge), std::end(kLedge))
                                  : std::vector<Plane>(std::begin(kReceivers), std::end(kReceivers))) {
        const V3 nc = view.dir_to_cam(pl.n);
        const double dc = pl.d + dot(pl.n, view.pos);
        const double den = dot(nc, dir);
        if (std::abs(den) < 1e-12) {
          continue;
        }
        const double t = -dc / den;
        if (t <= 0 || t >= best) {
          continue;
        }
        const V3 hit_w = add(view.pos, add(add(mul(view.ax, t * dir[0]), mul(view.ay, t * dir[1])),
                                           mul(view.az, t * dir[2])));
        if (hit_w[0] < pl.xmin || hit_w[0] >= pl.xmax || hit_w[1] < pl.ymin || hit_w[1] >= pl.ymax) {
          continue;
        }
        best = t;
      }
      if (best < 1e29) {
        const V3 h = mul(dir, best);
        const double cz = M[8] * h[0] + M[9] * h[1] + M[10] * h[2] + M[11];
        const double cw = M[12] * h[0] + M[13] * h[1] + M[14] * h[2] + M[15];
        if (cw > 0 && std::abs(cz) <= cw) {
          depth[px + py * kW] = (float)((cz / cw + 1.0) * 0.5);
        }
      }
      // faces crossed in front of the receiver and beyond the GPU's near plane
      int count = 0;
      for (int pass = 0; pass < 2; pass++) {
        for (const auto& t : pass ? btris : ftris) {
          const double tt = ray_tri(dir, t[0], t[1], t[2]);
          if (tt > 0 && tt < best && tt * dir[2] >= near_z) {
            count += pass ? -1 : 1;
          }
        }
      }
      exact[px + py * kW] = std::max(0, count);
    }
  }
  std::vector<int> sten(kW * kH, 0);
  for (int pass = 0; pass < 2; pass++) {
    const u16* list = pass ? back.data() : front.data();
    const int nidx = pass ? v.back_count() : v.front_count();
    for (int k = 0; k + 2 < nidx; k += 3) {
      const ctr_clip_vertex* t[3] = {&verts[list[k]], &verts[list[k + 1]], &verts[list[k + 2]]};
      draw_tri(md, t, depth.data(), sten, pass && !md.back_incr ? -1 : 1, res.dropped);
    }
  }
  // the darkening pass: the front faces again (where the stencil isn't 0)
  std::vector<int> cover(kW * kH, 0);
  std::vector<float> zero(kW * kH, -1.f);
  for (int k = 0; k + 2 < v.front_count(); k += 3) {
    const ctr_clip_vertex* t[3] = {&verts[front[k]], &verts[front[k + 1]], &verts[front[k + 2]]};
    draw_tri(md, t, zero.data(), cover, 1, res.dropped);
  }
  {
    // the old darkening rectangle: every vertex's x / w, y / w (+2 px), the screen if one has
    // w <= 1e-4
    double x0 = 1e30, x1 = -1e30, y0 = 1e30, y1 = -1e30;
    bool full = false;
    for (size_t i = 0; i < 2 * vol.T.size(); i++) {
      const V3 q = vert((u16)i);
      const double cx = M[0] * q[0] + M[1] * q[1] + M[2] * q[2] + M[3];
      const double cy = M[4] * q[0] + M[5] * q[1] + M[6] * q[2] + M[7];
      const double cw = M[12] * q[0] + M[13] * q[1] + M[14] * q[2] + M[15];
      if (!(cw > 1e-4)) {
        full = true;
        break;
      }
      x0 = std::min(x0, cx / cw), x1 = std::max(x1, cx / cw);
      y0 = std::min(y0, cy / cw), y1 = std::max(y1, cy / cw);
    }
    if (full) {
      x0 = y0 = -1, x1 = y1 = 1;
    }
    x0 = std::max(-1.0, x0 - 0.01), x1 = std::min(1.0, x1 + 0.01);
    y0 = std::max(-1.0, y0 - 1.0 / 60), y1 = std::min(1.0, y1 + 1.0 / 60);
    if (x0 < x1 && y0 < y1 && !trace.empty()) {
      res.old_rect = (long)((x1 - x0) * 200 * (y1 - y0) * 120);
    }
  }
  std::vector<u8> in_rect(kW * kH, 0);
  for (int i = 0; i < kW * kH; i++) {
    in_rect[i] = cover[i] != 0;
    res.fill += cover[i];
  }
  for (int i = 0; i < kW * kH; i++) {
    res.rect_area += in_rect[i];
    const bool dark = sten[i] && in_rect[i];
    res.dark += dark;
    res.false_dark += dark && !exact[i];
    res.missing += !sten[i] && exact[i];
  }
  if (dump) {
    // P6: red = darkened, green = exact shadow, blue = the darkening rectangle
    FILE* f = fopen("/tmp/ctr_stencil_pose.ppm", "wb");
    fprintf(f, "P6 %d %d 255\n", kW, kH);
    for (int i = 0; i < kW * kH; i++) {
      const u8 px[3] = {(u8)(sten[i] && in_rect[i] ? 255 : 0), (u8)(exact[i] ? 200 : 0),
                        (u8)(in_rect[i] ? 90 : depth[i] > 0 ? 40 : 0)};
      fwrite(px, 1, 3, f);
    }
    fclose(f);
  }
  return res;
}

}  // namespace

int main(int argc, char** argv) {
  Model md;
  md.f24 = getenv("STENCIL_F24") != nullptr;
  md.noclip = getenv("STENCIL_NOCLIP") != nullptr;
  md.back_incr = getenv("STENCIL_BACK_INCR") != nullptr;
  md.guard = getenv("STENCIL_GUARD") ? (float)atof(getenv("STENCIL_GUARD")) : 0.f;
  const Mesh mesh = make_caster();
  const Camera cam = make_camera(64.f);
  // poses: around Jak at distances 0.3 .. 12 m, heights -10 .. +8 m (below the bottom plane, in
  // the volume's column, above), looking at points around him and down the column
  std::vector<Pose> poses;
  const double dists[] = {0.3, 0.6, 1.0, 1.6, 2.5, 4.0, 6.0, 9.0, 12.0};
  const double heights[] = {-10.0, -6.0, -3.0, -1.5, -0.5, 0.3, 1.0, 1.8, 3.0, 5.0, 8.0};
  const double looks[] = {0.85, 0.0, -3.0, -8.0};
  for (double dist : dists) {
    for (double h : heights) {
      for (int a = 0; a < 16; a++) {
        for (double ly : looks) {
          const double ang = 2 * M_PI * a / 16;
          // the volume's column leans along the light: put some cameras over it
          const double ox = (h < 0 ? -0.4226 / 0.9063 * h : 0.0) * kM;
          Pose p;
          p.pos = {ox + dist * kM * std::cos(ang), h * kM, dist * kM * std::sin(ang)};
          p.look = {(ly < 0 ? -0.4226 / 0.9063 * ly : 0.0) * kM, ly * kM, 0};
          poses.push_back(p);
        }
      }
    }
  }
  if (argc > 2) {
    Pose p;
    p.pos = {atof(argv[1]) * kM, atof(argv[2]) * kM, atof(argv[3]) * kM};
    p.look = {atof(argv[4]) * kM, atof(argv[5]) * kM, atof(argv[6]) * kM};
    const Result r = run_pose(mesh, cam, p, md, true);
    printf("dark %d false %d missing %d rect %d dropped %d\n", r.dark, r.false_dark, r.missing,
           r.rect_area, r.dropped);
    return 0;
  }
  int quads = 0, bad = 0;
  long false_total = 0, dark_total = 0, missing_total = 0, fill_total = 0, dropped_total = 0,
       old_rect_total = 0, clipped_total = 0, tris_total = 0;
  double build_total = 0;
  int worst = -1;
  double worst_frac = 0;
  Result worst_r;
  for (size_t i = 0; i < poses.size(); i++) {
    const Result r = run_pose(mesh, cam, poses[i], md, false);
    false_total += r.false_dark;
    dark_total += r.dark;
    missing_total += r.missing;
    fill_total += r.fill;
    old_rect_total += r.old_rect;
    dropped_total += r.dropped;
    clipped_total += r.clipped;
    tris_total += r.tris;
    build_total += r.build_us;
    const double frac = r.rect_area ? (double)r.false_dark / r.rect_area : 0;
    bad += r.false_dark > 20;
    if (frac > 0.5 && r.false_dark > 500) {
      quads++;
    }
    if (r.false_dark > 0 && frac > worst_frac) {
      worst_frac = frac;
      worst = (int)i;
      worst_r = r;
    }
  }
  printf("model f24 %d noclip %d guard %.1f: %zu poses, darkened %ld px, false %ld px, missing %ld "
         "px; poses with > 20 false px: %d, solid quads (false > half the darkening area): %d\n",
         md.f24, md.noclip, md.guard, poses.size(), dark_total, false_total, missing_total, bad,
         quads);
  printf("darkening pass fragments %ld (the bounding rectangles: %ld), triangles dropped by the "
         "GPU model %ld\n",
         fill_total, old_rect_total, dropped_total);
  printf("volume build (host CPU): %.1f ms over the poses, %ld triangles cut to the guard band, %ld "
         "triangles to the GPU\n",
         build_total / 1000.0, clipped_total, tris_total);
  if (worst >= 0) {
    const Pose& p = poses[worst];
    printf("worst pose %d: camera (%.2f %.2f %.2f) m looking at (%.2f %.2f %.2f) m: false %d of "
           "%d darkening px (%.0f%%), dropped %d, full screen %d\n",
           worst, p.pos[0] / kM, p.pos[1] / kM, p.pos[2] / kM, p.look[0] / kM, p.look[1] / kM,
           p.look[2] / kM, worst_r.false_dark, worst_r.rect_area, 100 * worst_frac,
           worst_r.dropped, worst_r.full_screen);
    run_pose(mesh, cam, p, md, true);
  }
  if (argc > 1) {
    const int k = atoi(argv[1]);
    const Result r = run_pose(mesh, cam, poses[k], md, true);
    printf("pose %d: dark %d false %d missing %d rect %d dropped %d\n", k, r.dark, r.false_dark,
           r.missing, r.rect_area, r.dropped);
  }
  return quads ? 1 : 0;
}
