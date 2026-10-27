/*!
 * @file ocean_replay.cpp
 * (AI-assisted)
 * Host replay test of the 3DS full ocean (game/graphics/ctr/CtrOceanFull.h): one frame's two ocean
 * buckets, dumped by the 3DS build (flag file sdmc:/3ds/jak1/ocean_dump, see CtrOceanFull.cpp),
 * run through CtrOceanFull with the emulated VU programs (the reference) and with the native ones,
 * with the GPU calls captured. Compares the triangles in screen space and times both.
 *
 *   test/ctr_ocean/build.sh && /tmp/ocean_replay [ocean_dump.bin [iterations]]
 *
 * Exit status 0 when the native triangles match: every native triangle that the VU program would
 * draw whole (all vertices inside its guard band) is one of its triangles (position within 1/8
 * pixel, color, fog, texture coordinates up to whole repeats), and every reference triangle is
 * covered (equal to a native one, or a piece the VU clipped out of one).
 *
 * (AI-assisted) Without a dump file too: the transition pieces (transition_test) of every table the
 * game sends to the ocean-mid program's calls 107 / 275 (7, 10, 11 and 18 vertices, read from
 * goal_src/jak1/engine/gfx/ocean/ocean-trans-tables.gc), seen whole by a made-up camera: the
 * native triangles must be the emulated program's, one for one.
 */

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "common/dma/dma_chain_read.h"
#include "common/log/log.h"

#include "game/graphics/ctr/CtrOceanBatch.h"
#include "game/graphics/ctr/CtrOceanFull.h"
#include "game/graphics/ctr/CtrOceanVu.h"
#include "game/graphics/ctr/CtrSettings.h"
#include "game/graphics/ctr/CtrVram.h"
#include "game/graphics/ctr/ctr_gpu.h"

// ---------------------------------------------------------------------------------------------
// stubs: the GPU (captured), the env map, the log
// ---------------------------------------------------------------------------------------------

namespace {
struct Draw {
  bool clip;
  int bucket;
  std::vector<ctr_ocean_full_vertex> v;
  std::vector<u16> idx;
};
std::vector<Draw> g_draws;
int g_bucket = 0;
bool g_capture = true;
std::vector<ctr_ocean_full_vertex> g_v(1 << 16);
std::vector<u16> g_i(3 << 16);
std::vector<u32> g_env;
int g_env_w = 1, g_env_h = 1;
std::vector<u8> g_tex;

void draw(int nv, int ni, bool clip) {
  if (!g_capture) {
    return;
  }
  Draw d;
  d.clip = clip;
  d.bucket = g_bucket;
  d.v.assign(g_v.begin(), g_v.begin() + nv);
  d.idx.assign(g_i.begin(), g_i.begin() + ni);
  g_draws.push_back(std::move(d));
}
}  // namespace

int ctr_gpu_ocean_full_reserve(int max_verts, int max_indices, ctr_ocean_full_vertex** verts,
                               uint16_t** indices) {
  if (max_verts > (int)g_v.size() || max_indices > (int)g_i.size()) {
    return 0;
  }
  *verts = g_v.data();
  *indices = g_i.data();
  return 1;
}
void ctr_gpu_ocean_full_draw(const ctr_draw_state*, int nv, int ni, uint8_t, uint8_t, uint8_t) {
  draw(nv, ni, false);
}
void ctr_gpu_ocean_full_draw_clip(const ctr_draw_state*, int nv, int ni, uint8_t, uint8_t,
                                  uint8_t) {
  draw(nv, ni, true);
}
int ctr_gpu_tex_create_mipmapped(int w, int h, const uint8_t* rgba) {
  g_tex.assign(rgba, rgba + 4 * w * h);
  return 1;
}
// (AI-assisted, j1ocean) the sky reflection's texture (CtrOceanFull::ocean_env): handle 2
std::vector<u8> g_refl_tex;
int ctr_gpu_tex_create(int w, int h, const uint8_t* rgba) {
  g_refl_tex.assign(rgba, rgba + 4 * w * h);
  return 2;
}
void ctr_gpu_tex_update(int handle, const uint8_t* rgba) {
  auto& t = handle == 2 ? g_refl_tex : g_tex;
  memcpy(t.data(), rgba, t.size());
}
void ctr_gpu_tex_delete(int) {}
// (AI-assisted, j1ocean) the sky reflection's draws: captured like the clip space draws
void ctr_gpu_ocean_full_draw_env(const ctr_draw_state*, int nv, int ni, uint8_t, uint8_t, uint8_t,
                                 int) {
  draw(nv, ni, true);
}
void ctr_gpu_ocean_end(void) {}
double ctr_gpu_time_ms(void) {
  using namespace std::chrono;
  return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}
// (AI-assisted) config.ini: the defaults, changed in main
// (CtrSettings.h's inline ctr_settings reads ctr_settings_detail::g_settings: pointed at this one)
CtrSettings g_settings;
namespace ctr_settings_detail {
const CtrSettings* g_settings = &::g_settings;
const CtrSettings& first_use() {
  return ::g_settings;
}
}  // namespace ctr_settings_detail
bool CtrVram::decode_for_cpu(u64, std::vector<u32>* out, int* w, int* h) {
  *out = g_env;
  *w = g_env_w;
  *h = g_env_h;
  return true;
}
const void* CtrVram::upload_source(u32) const {
  return nullptr;
}
namespace lg::internal {
void log_message(level, LogTime&, const char* message) {
  printf("%s\n", message);
}
void log_print(const char* message) {
  printf("%s", message);
}
}  // namespace lg::internal
void private_assert_failed(const char* expr, const char* file, int line, const char* function) {
  fprintf(stderr, "assert %s %s:%d %s\n", expr, file, line, function);
  abort();
}
void private_assert_failed(const char* expr,
                           const char* file,
                           int line,
                           const char* function,
                           const char* msg) {
  fprintf(stderr, "assert %s %s:%d %s %s\n", expr, file, line, function, msg);
  abort();
}
void private_assert_failed(const char* expr,
                           const char* file,
                           int line,
                           const char* function,
                           const std::string_view& msg) {
  fprintf(stderr, "assert %s %s:%d %s %s\n", expr, file, line, function,
          std::string(msg).c_str());
  abort();
}

// ---------------------------------------------------------------------------------------------
// the dump: buckets as DMA chains of CNT tags
// ---------------------------------------------------------------------------------------------

namespace {
struct Bucket {
  std::vector<u8> mem;  // tags and data, then an END tag at `end`
  u32 end = 0;
};

bool load(const char* path, Bucket* mid, Bucket* near) {
  FILE* f = fopen(path, "rb");
  if (!f) {
    return false;
  }
  std::vector<u8> d;
  u8 buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    d.insert(d.end(), buf, buf + n);
  }
  fclose(f);
  size_t p = 0;
  auto get32 = [&]() {
    u32 v;
    memcpy(&v, &d[p], 4);
    p += 4;
    return v;
  };
  while (p + 4 <= d.size()) {
    const std::string tag((const char*)&d[p], 4);
    p += 4;
    if (tag == "OCNB") {
      const u32 is_near = get32(), count = get32();
      Bucket& b = is_near ? *near : *mid;
      b.mem.clear();
      for (u32 i = 0; i < count; i++) {
        const u32 qwc = get32();
        u64 vifs;
        memcpy(&vifs, &d[p], 8);
        p += 8;
        const u64 dma_tag = (u64)qwc | (1ull << 28);  // CNT
        const size_t at = b.mem.size();
        b.mem.resize(at + 16 + 16 * qwc);
        memcpy(&b.mem[at], &dma_tag, 8);
        memcpy(&b.mem[at + 8], &vifs, 8);
        memcpy(&b.mem[at + 16], &d[p], 16 * qwc);
        p += 16 * qwc;
      }
      b.end = (u32)b.mem.size();
      const u64 end_tag = 7ull << 28;
      b.mem.resize(b.end + 16);
      memset(&b.mem[b.end], 0, 16);
      memcpy(&b.mem[b.end], &end_tag, 8);
    } else if (tag == "OCNE") {
      g_env_w = (int)get32();
      g_env_h = (int)get32();
      g_env.resize((size_t)g_env_w * g_env_h);
      memcpy(g_env.data(), &d[p], 4 * g_env.size());
      p += 4 * g_env.size();
    } else {
      return false;
    }
  }
  return !mid->mem.empty() && !near->mem.empty();
}

u64 g_frame = 0;
void run_frame(CtrOceanFull& ocean, CtrVram& vram, const Bucket& mid, const Bucket& near) {
  CtrRenderState rs;
  rs.vram = &vram;
  rs.frame_idx = ++g_frame;
  g_bucket = 0;
  rs.next_bucket = mid.end;
  DmaFollower dm(mid.mem.data(), 0);
  ocean.render_mid_and_far(dm, rs);
  g_bucket = 1;
  rs.next_bucket = near.end;
  DmaFollower dn(near.mem.data(), 0);
  ocean.render_near(dn, rs);
}

// ---------------------------------------------------------------------------------------------
// comparison
// ---------------------------------------------------------------------------------------------

struct SV {
  float x, y, z, s, t, fog;
  int r, g, b, a;
  float w;  // clip space w (1: a GS vertex)
};
struct Tri {
  SV v[3];
  int bucket;
  bool clip;
};

SV screen(const ctr_ocean_full_vertex& v, bool clip) {
  SV o;
  if (clip) {
    o.x = v.x / v.q;
    o.y = v.y / v.q;
    o.z = v.z / v.q;
    o.s = v.s;
    o.t = v.t;
    o.w = v.q;
  } else {
    o.x = v.x;
    o.y = v.y;
    o.z = v.z;
    o.s = v.s / v.q;
    o.t = v.t / v.q;
    o.w = 1;
  }
  o.fog = v.fog;
  o.r = v.r;
  o.g = v.g;
  o.b = v.b;
  o.a = v.a;
  return o;
}

std::vector<Tri> triangles(const std::vector<Draw>& draws) {
  std::vector<Tri> out;
  for (auto& d : draws) {
    for (size_t i = 0; i + 2 < d.idx.size(); i += 3) {
      Tri t;
      t.bucket = d.bucket;
      t.clip = d.clip;
      for (int k = 0; k < 3; k++) {
        t.v[k] = screen(d.v[d.idx[i + k]], d.clip);
      }
      out.push_back(t);
    }
  }
  return out;
}

bool same_pos(const SV& a, const SV& b) {
  return std::abs(a.x - b.x) <= 2 && std::abs(a.y - b.y) <= 2 &&
         std::abs(a.z - b.z) <= 8 + 2e-5f * std::abs(a.z);  // (z: 8e6 + -8e6 in floats)
}

//! b's vertices are a's (any order); perm[k] = b's vertex for a's k
bool match(const Tri& a, const Tri& b, int perm[3]) {
  bool used[3] = {false, false, false};
  for (int k = 0; k < 3; k++) {
    perm[k] = -1;
    for (int j = 0; j < 3; j++) {
      if (!used[j] && same_pos(a.v[k], b.v[j])) {
        perm[k] = j;
        used[j] = true;
        break;
      }
    }
    if (perm[k] < 0) {
      return false;
    }
  }
  return true;
}

//! the attributes of matched triangles (texture coordinates up to the same whole repeat)
bool same_attributes(const Tri& a, const Tri& b, const int perm[3], std::string* why) {
  const float ds = a.v[0].s - b.v[perm[0]].s, dt = a.v[0].t - b.v[perm[0]].t;
  for (int k = 0; k < 3; k++) {
    const SV& p = a.v[k];
    const SV& q = b.v[perm[k]];
    if (std::abs(p.r - q.r) > 1 || std::abs(p.g - q.g) > 1 || std::abs(p.b - q.b) > 1 ||
        std::abs(p.a - q.a) > 1) {
      *why = fmt::format("color {} {} {} {} vs {} {} {} {}", p.r, p.g, p.b, p.a, q.r, q.g, q.b,
                         q.a);
      return false;
    }
    if (std::abs(p.fog - q.fog) > 1.5f / 255) {
      *why = fmt::format("fog {} vs {}", p.fog, q.fog);
      return false;
    }
    if (std::abs(p.s - q.s - ds) > 1e-3f || std::abs(p.t - q.t - dt) > 1e-3f ||
        std::abs(ds - std::round(ds)) > 1e-3f || std::abs(dt - std::round(dt)) > 1e-3f) {
      *why = fmt::format("st {} {} vs {} {}", p.s, p.t, q.s, q.t);
      return false;
    }
  }
  return true;
}

double area(const Tri& t) {
  return 0.5 * std::abs((t.v[1].x - t.v[0].x) * (t.v[2].y - t.v[0].y) -
                        (t.v[2].x - t.v[0].x) * (t.v[1].y - t.v[0].y));
}

//! a native (clip space) triangle the VU program would clip: a vertex outside its guard band
//! (the hmge scale) or behind the camera
bool vu_would_clip(const Tri& t) {
  // (Jak 1's hmge scale: x within 2048 +- 1024 pixels, y 2048 +- 448, z its 24 bits)
  for (auto& v : t.v) {
    if (!(v.w > 0) || std::abs(v.x / 16 - 2048) > 1023 || std::abs(v.y / 16 - 2048) > 447 ||
        v.z < 200 || v.z > 16760000) {
      return true;
    }
  }
  return false;
}

int compare(const std::vector<Tri>& ref, const std::vector<Tri>& nat, int bucket) {
  int errors = 0, matched = 0, nat_clipped = 0, nat_inside = 0;
  double ref_unmatched_area = 0;
  std::vector<bool> ref_used(ref.size(), false);
  int ref_n = 0;
  for (auto& t : ref) {
    ref_n += t.bucket == bucket;
  }
  for (auto& b : nat) {
    if (b.bucket != bucket) {
      continue;
    }
    bool found = false;
    for (size_t i = 0; i < ref.size() && !found; i++) {
      int perm[3];
      if (ref[i].bucket != bucket || ref_used[i] || !match(b, ref[i], perm)) {
        continue;
      }
      std::string why;
      // (ref vertex for the native's k)
      Tri r = ref[i];
      Tri rp = r;
      for (int k = 0; k < 3; k++) {
        rp.v[k] = r.v[perm[k]];
      }
      int id[3] = {0, 1, 2};
      if (!same_attributes(rp, b, id, &why)) {
        if (errors++ < 10) {
          printf("  bucket %d: attributes differ: %s\n", bucket, why.c_str());
        }
      }
      ref_used[i] = true;
      found = true;
      matched++;
    }
    if (!found) {
      if (vu_would_clip(b)) {
        if (getenv("OCEAN_DEBUG") && nat_clipped < 8) {
          printf("  clipped? w %g %g %g: (%.1f %.1f %.0f) (%.1f %.1f %.0f) (%.1f %.1f %.0f)\n",
                 b.v[0].w, b.v[1].w, b.v[2].w, b.v[0].x / 16 - 2048, b.v[0].y / 16 - 2048,
                 b.v[0].z, b.v[1].x / 16 - 2048, b.v[1].y / 16 - 2048, b.v[1].z,
                 b.v[2].x / 16 - 2048, b.v[2].y / 16 - 2048, b.v[2].z);
        }
        nat_clipped++;
      } else {
        nat_inside++;
        if (getenv("OCEAN_DEBUG")) {
          for (size_t i = 0; i < ref.size(); i++) {
            int perm[3];
            if (ref[i].bucket == bucket && match(b, ref[i], perm)) {
              printf("    (matches reference %zu, used %d)\n", i, (int)ref_used[i]);
            }
          }
          int n_same = 0;
          for (auto& o : nat) {
            int perm[3];
            n_same += o.bucket == bucket && match(b, o, perm);
          }
          printf("    (%d native copies)\n", n_same);
        }
        if (nat_inside <= 10) {
          printf(
              "  bucket %d: native triangle not in the reference: (%.1f %.1f %.0f) (%.1f %.1f %.0f) "
              "(%.1f %.1f %.0f)\n",
              bucket, b.v[0].x / 16 - 2048, b.v[0].y / 16 - 2048, b.v[0].z, b.v[1].x / 16 - 2048,
              b.v[1].y / 16 - 2048, b.v[1].z, b.v[2].x / 16 - 2048, b.v[2].y / 16 - 2048, b.v[2].z);
        }
      }
    }
  }
  int ref_unmatched = 0, ref_unclipped = 0;
  for (size_t i = 0; i < ref.size(); i++) {
    if (ref[i].bucket == bucket && !ref_used[i] && area(ref[i]) > 0.5) {
      ref_unmatched++;
      ref_unmatched_area += area(ref[i]) / 256;
      // a piece the VU's clipper made has a vertex on its guard band (or the near plane)
      bool on_band = false;
      for (auto& v : ref[i].v) {
        on_band |= std::abs(std::abs(v.x / 16 - 2048) - 1024) < 2 ||
                   std::abs(std::abs(v.y / 16 - 2048) - 448) < 2 || v.z < 300 || v.z > 16760000;
      }
      if (!on_band) {
        ref_unclipped++;
        if (ref_unclipped <= 10) {
          const Tri& b = ref[i];
          printf("  bucket %d: reference triangle not drawn: (%.1f %.1f %.0f) (%.1f %.1f %.0f) "
                 "(%.1f %.1f %.0f)\n",
                 bucket, b.v[0].x / 16 - 2048, b.v[0].y / 16 - 2048, b.v[0].z,
                 b.v[1].x / 16 - 2048, b.v[1].y / 16 - 2048, b.v[1].z, b.v[2].x / 16 - 2048,
                 b.v[2].y / 16 - 2048, b.v[2].z);
        }
      }
    }
  }
  printf(
      "bucket %d: reference %d triangles, native %d matched, %d not (%d the VU clips, %d inside: "
      "errors); reference not matched: %d (%.0f pixels: the VU's clipped pieces, %d not: errors); "
      "%d attribute errors\n",
      bucket, ref_n, matched, nat_clipped + nat_inside, nat_clipped, nat_inside, ref_unmatched,
      ref_unmatched_area, ref_unclipped, errors);
  return errors + nat_inside + ref_unclipped;
}

double time_frames(CtrOceanFull& ocean, CtrVram& vram, const Bucket& mid, const Bucket& near,
                   int n) {
  g_capture = false;
  const double t0 = ctr_gpu_time_ms();
  for (int i = 0; i < n; i++) {
    run_frame(ocean, vram, mid, near);
  }
  g_capture = true;
  return (ctr_gpu_time_ms() - t0) / n;
}
}  // namespace

// ---------------------------------------------------------------------------------------------
// (AI-assisted) the transition pieces, without a dump: the ocean-mid program's calls 107 (a strip
// of quads) and 275 (a fan) with the game's tables, native against emulated
// ---------------------------------------------------------------------------------------------

#ifndef CTR_OCEAN_REPO
#define CTR_OCEAN_REPO "."
#endif

namespace {

// ocean-mid-setup-constants as a beach frame had them (an ocean_dump.bin): hmge, inv-hmge, hvdf,
// fog, constants, constants2, drw-fan, env-fan, the drawing adgif, drw-strip-0 / 1, the env map
// adgif, env-strip, env-color, index-table, pos0..3 (VU addresses 733..768)
const u32 kMidConstants[36 * 4] = {
    0x3a800000, 0x3b124925, 0xb400209f, 0xc1b8fb4c, 0x44800000, 0x43e00000, 0xcaffbed3,
    0xbd312455, 0x45000000, 0x45000000, 0x4affbf9b, 0x43850923, 0xbd312455, 0x42424bbb,
    0x437f0000, 0x45400000, 0x3f000000, 0x3f000000, 0x00000000, 0x48c00000, 0x3f000000,
    0x3f000000, 0x3f800000, 0x00000000, 0x00008004, 0x301ec000, 0x00000412, 0x00000000,
    0x00008004, 0x302ec000, 0x00000412, 0x00000000, 0x00000005, 0x10000000, 0x0000000e,
    0x00000000, 0xdc009fe0, 0x00000005, 0x00000006, 0x00000000, 0x00080178, 0x00000eed,
    0x00000014, 0x00000000, 0x120060e0, 0x00613006, 0x00000034, 0x00000000, 0x00000000,
    0x00000000, 0x00000008, 0x00000000, 0x13506134, 0x00613606, 0x00000036, 0x00000000,
    0x00000012, 0x301e4000, 0x00000412, 0x00000000, 0x00008012, 0x301e4000, 0x00000412,
    0x00000000, 0x00000005, 0x10000000, 0x0000000e, 0x00000000, 0x9930a72a, 0x2004e745,
    0x00000006, 0x00000000, 0x00000120, 0x00000000, 0x00000014, 0x00000000, 0x00000000,
    0x00000000, 0x00000034, 0x00000000, 0x00000005, 0x00000000, 0x00000008, 0x00000000,
    0x00000058, 0x00000000, 0x00000042, 0x00000000, 0x00008012, 0x302e4000, 0x00000412,
    0x00000000, 0x42069d60, 0x42069d60, 0x429afb57, 0x43000000, 0x0000003f, 0x00000054,
    0x00000042, 0x00000000, 0x00000036, 0x00000048, 0x00000039, 0x00000000, 0x0000002d,
    0x0000003c, 0x00000030, 0x00000000, 0x00000024, 0x00000030, 0x00000027, 0x00000000,
    0x0000001b, 0x00000024, 0x0000001e, 0x00000000, 0x00000012, 0x00000018, 0x00000015,
    0x00000000, 0x00000009, 0x0000000c, 0x0000000c, 0x00000000, 0x00000000, 0x00000000,
    0x00000003, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x3f800000, 0x48c00000,
    0x00000000, 0x00000000, 0x3f800000, 0x00000000, 0x00000000, 0x48c00000, 0x3f800000,
    0x48c00000, 0x00000000, 0x48c00000, 0x3f800000,
};

//! (define <name> (new 'static 'array float <n> ...)) of a GOAL source: the n floats
std::vector<float> goal_float_table(const std::string& src, const std::string& name) {
  size_t at = src.find("(define " + name + "\n");
  if (at == std::string::npos) {
    return {};
  }
  at = src.find("'array", at);
  at = at == std::string::npos ? at : src.find("float", at);
  if (at == std::string::npos) {
    return {};
  }
  const char* p = src.c_str() + at + 5;
  char* end;
  const long n = strtol(p, &end, 10);
  std::vector<float> out;
  for (long i = 0; i < n; i++) {
    p = end;
    out.push_back(strtof(p, &end));
    if (end == p) {
      return {};
    }
  }
  return out;
}

//! a DMA chain of CNT tags (the data after each tag), ended by the VU end tag and an END tag
struct Chain {
  std::vector<u8> mem;
  void add(u32 qwc, u32 vif0, u32 vif1, const void* data = nullptr) {
    const u64 tag = (u64)qwc | (1ull << 28);  // CNT
    const size_t at = mem.size();
    mem.resize(at + 16 + 16 * qwc, 0);
    memcpy(&mem[at], &tag, 8);
    memcpy(&mem[at + 8], &vif0, 4);
    memcpy(&mem[at + 12], &vif1, 4);
    if (qwc && data) {
      memcpy(&mem[at + 16], data, 16 * qwc);
    }
  }
  void unpack(u32 addr, u32 qwc, const void* data) {
    add(qwc, 0x01000404, (0x6cu << 24) | (qwc << 16) | addr, data);  // STCYCL 0x404, UNPACK_V4_32
  }
  void call(u32 addr) { add(0, 0x01000404, (0x15u << 24) | addr); }   // MSCALF
  void flush(u32 addr) { add(0, (0x15u << 24) | addr, 0x13u << 24); }  // MSCALF, FLUSHA
  void finish() {
    add(2, 0, (0x50u << 24) | 2);  // NOP, DIRECT: the end of the ocean's VU DMA
    add(0, 0, 0);
    const u64 end = 7ull << 28;
    memcpy(&mem[mem.size() - 16], &end, 8);
  }
};

struct Piece {
  const char* table;  // symbol in ocean-trans-tables.gc
  int n;              // vertices (the game's for this table)
  bool fan;           // call 275 (#t in ocean-trans-add-upload-table) or 107
  u32 mask;           // 107: quads left out
  int strip = 0;      // *ocean-trans-strip-array*: which of its 4 strips (10 vertices each)
  bool vu_too = true; // the emulated program as the reference (false: native checks only)
};

//! the clip-space vertex matrices of a 24 m square at the origin (the corners at 765..768), seen
//! from 25 m away, 50 degrees down (ocean-mid-add-matrices: the camera matrix with the square's
//! corner as translation, then times the perspective of the beach frame's math-camera)
void piece_matrices(float out[8][4]) {
  const double pitch = 50.0 * M_PI / 180.0, dist = 25.0 * 4096.0;
  const double f[3] = {0, -std::sin(pitch), std::cos(pitch)};  // forward
  const double r[3] = {1, 0, 0};                                 // right (r x u = f)
  const double u[3] = {0, std::cos(pitch), std::sin(pitch)};
  const double center[3] = {49152, 0, 49152};
  double cam[3];
  for (int i = 0; i < 3; i++) {
    cam[i] = center[i] - dist * f[i];
  }
  const double* axis[3] = {r, u, f};
  double m[4][4] = {};
  for (int i = 0; i < 3; i++) {
    for (int j = 0; j < 3; j++) {
      m[i][j] = axis[j][i];  // world (row vector) to camera
      m[3][j] -= cam[i] * axis[j][i];
    }
  }
  m[3][3] = 1;
  // math-camera perspective (d 1024, f 4.04e7 units; the beach frame's)
  const double p[4][4] = {{-0.3200988, 0, 0, 0},
                          {0, -0.2334052, 0, 0},
                          {0, 0, -8184.2678, -4.2233925e-05},
                          {0, 0, 16760956.0, 0}};
  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 4; j++) {
      double mp = 0;
      for (int k = 0; k < 4; k++) {
        mp += m[i][k] * p[k][j];
      }
      out[i][j] = (float)m[i][j];
      out[4 + i][j] = (float)mp;
    }
  }
}

//! one piece through a fresh ocean-mid program: its triangles; false: the program refused the DMA
bool run_piece(const Piece& pc, const std::vector<float>& table, bool native, std::vector<Tri>* out,
               bool* indices_ok) {
  Chain c;
  c.add(0, 0x03000000, 0x02000000 | 0x76);  // BASE 0, OFFSET 0x76
  c.unpack(0x2dd, 36, kMidConstants);
  c.call(0);
  c.flush(41);
  c.flush(43);
  // ocean-trans-add-constants: the corners of a 24 m square
  const float corners[4][4] = {
      {0, 0, 0, 1}, {98304, 0, 0, 1}, {0, 0, 98304, 1}, {98304, 0, 98304, 1}};
  c.unpack(0x2fd, 4, corners);
  float mats[8][4];
  piece_matrices(mats);
  c.unpack(0x8000, 8, mats);
  // n and the mask, the texture coordinates and colors of the corners (ocean-trans-st-table
  // weighted for a quarter of a mid square, a beach frame's ocean colors)
  u32 head[9][4] = {};
  head[0][0] = (u32)pc.n;
  head[0][1] = pc.mask;
  const float st_col[8][4] = {{0.25f, 0.5f, 1, 0}, {0.5f, 0.5f, 1, 0}, {0.25f, 0.75f, 1, 0},
                              {0.5f, 0.75f, 1, 0}, {32, 76, 83, 128},   {30, 72, 79, 128},
                              {32, 76, 84, 128},   {32, 75, 81, 128}};
  memcpy(head[1], st_col, sizeof(st_col));
  c.unpack(0x8008, 9, head);
  c.unpack(0x8011, (u32)pc.n, table.data() + 40 * pc.strip);
  c.call(pc.fan ? 275 : 107);
  c.flush(41);
  c.finish();

  auto vu = std::make_unique<CtrOceanMidVu>();
  vu->set_native(native);
  CtrOceanBatch batch;
  ctr_draw_state st;
  memset(&st, 0, sizeof(st));
  const u8 fog[3] = {0, 0, 0};
  g_draws.clear();
  g_bucket = 0;
  batch.begin(st, fog);
  DmaFollower dma(c.mem.data(), 0);
  const bool ok = vu->run(dma, &batch);
  batch.end();
  *indices_ok = true;
  for (auto& d : g_draws) {
    for (u16 i : d.idx) {
      *indices_ok &= i < d.v.size();
    }
  }
  *out = triangles(g_draws);
  return ok;
}

int transition_test() {
  const std::string path =
      std::string(CTR_OCEAN_REPO) + "/goal_src/jak1/engine/gfx/ocean/ocean-trans-tables.gc";
  std::ifstream in(path);
  std::stringstream ss;
  ss << in.rdbuf();
  const std::string src = ss.str();
  // the tables and calls of ocean-trans-add-upload (the near ocean's seams), ocean-mid-add-upload-*
  // (the mid ocean's seams) and ocean-trans-add-upload-strip
  const Piece pieces[] = {
      {"*ocean-trans-up-table*", 11, true, 0},
      {"*ocean-trans-down-table*", 11, true, 0},
      {"*ocean-trans-left-table*", 11, true, 0},
      {"*ocean-trans-right-table*", 11, true, 0},
      {"*ocean-trans-up-left-table*", 18, false, 0},
      {"*ocean-trans-down-left-table*", 18, true, 0},
      {"*ocean-trans-up-right-table*", 18, true, 0},
      {"*ocean-trans-down-right-table*", 18, false, 0},
      {"*ocean-trans-down-right-table*", 18, false, 0x0155},  // (every other quad left out)
      {"*ocean-left-table*", 7, true, 0},
      {"*ocean-down-right-table*", 10, false, 0},
      {"*ocean-down-left-table*", 10, true, 0},
      {"*ocean-trans-strip-array*", 10, false, 0x5, 1},
      {"*ocean-trans-strip-array*", 10, false, 0xa, 3},
      // not sent by the game: a strip of an odd count (only whole quads), native only
      {"*ocean-trans-up-table*", 11, false, 0, 0, false},
  };
  int errors = 0;
  for (const auto& pc : pieces) {
    const auto table = goal_float_table(src, pc.table);
    if ((int)table.size() < 4 * pc.n + 40 * pc.strip) {
      printf("transition %s: table not read from %s\n", pc.table, path.c_str());
      errors++;
      continue;
    }
    int quads = 0;
    for (int q = 0; q < (pc.n - 2) / 2; q++) {
      quads += q >= 16 || !(pc.mask & (1u << q));
    }
    const int expect = pc.fan ? pc.n - 2 : 2 * quads;
    std::vector<Tri> ref, nat;
    bool ref_idx = true, nat_idx = true;
    const bool nat_ok = run_piece(pc, table, true, &nat, &nat_idx);
    const bool ref_ok = !pc.vu_too || run_piece(pc, table, false, &ref, &ref_idx);
    // every native triangle one of the emulated ones; the emulated ones left over drew nothing (a
    // fan's last triangle has its first vertex again: less than a pixel)
    int matched = 0, attr = 0, clipped = 0, ref_extra = 0;
    std::string why;
    for (auto& t : nat) {
      clipped += vu_would_clip(t);
    }
    std::vector<bool> used(ref.size(), false);
    for (auto& b : nat) {
      for (size_t i = 0; i < ref.size(); i++) {
        int perm[3];
        if (used[i] || !match(b, ref[i], perm)) {
          continue;
        }
        Tri rp = ref[i];
        for (int k = 0; k < 3; k++) {
          rp.v[k] = ref[i].v[perm[k]];
        }
        const int id[3] = {0, 1, 2};
        attr += !same_attributes(rp, b, id, &why);
        used[i] = true;
        matched++;
        break;
      }
    }
    for (size_t i = 0; i < ref.size(); i++) {
      ref_extra += !used[i] && area(ref[i]) >= 256;  // (area in 1/16 pixels squared)
    }
    const bool pass = nat_ok && ref_ok && nat_idx && (int)nat.size() == expect && !clipped &&
                      (!pc.vu_too || (matched == (int)nat.size() && !ref_extra && !attr));
    printf("transition %-32s n %2d %s mask %04x: native %2zu triangles%s, emulated %2zu, "
           "matched %2d, expected %2d%s%s%s%s %s\n",
           pc.table, pc.n, pc.fan ? "fan  " : "strip", pc.mask, nat.size(),
           nat_idx ? "" : " (INDEX OUT OF RANGE)", ref.size(), matched, expect,
           ref_extra ? fmt::format(" ({} emulated not drawn)", ref_extra).c_str() : "",
           clipped ? " (the VU would clip: camera?)" : "",
           attr ? fmt::format(", {} attribute errors ({})", attr, why).c_str() : "",
           nat_ok && ref_ok ? "" : " (DMA refused)", pass ? "ok" : "FAIL");
    errors += !pass;
  }
  return errors;
}
}  // namespace

// (AI-assisted) Projection textures require positive W and pre-clipped depth. Exercise a tile
// crossing each depth plane, using the real reservation/commit path and captured GPU buffers.
int depth_clip_test() {
  int errors = 0;
  for (int plane = 0; plane < 3; plane++) {
    g_draws.clear();
    CtrOceanBatch batch;
    ctr_draw_state state{};
    const u8 fog[3] = {};
    batch.begin(state, fog);
    ctr_ocean_full_vertex* v;
    u16* idx;
    int first;
    if (!batch.reserve_clip(3, 3, &v, &idx, &first)) return 1;
    for (int k = 0; k < 3; k++) {
      v[k] = {};
      v[k].q = 1;
      v[k].x = (32000.f + 1000.f * k);
      v[k].y = 32000.f;
      v[k].z = 8388608.f;
      v[k].r = 100;
      idx[k] = (u16)(first + k);
    }
    if (plane == 0) v[0].q = -1;
    if (plane == 1) v[0].z = -8388608.f;
    if (plane == 2) v[0].z = 25165824.f;
    batch.commit(3, 3);
    batch.end();
    errors += g_draws.size() != 1;
    for (const auto& draw : g_draws) {
      errors += draw.idx.size() < 3 || draw.idx.size() > 12;
      for (u16 i : draw.idx) {
        if (i >= draw.v.size()) { errors++; continue; }
        const auto& o = draw.v[i];
        errors += !(o.q > 0) || !std::isfinite(o.x / o.q) ||
                  o.z < -2.f || o.z > 16777215.f * o.q + 2.f || o.r != 100;
      }
    }
  }
  g_draws.clear();
  printf("projective ocean depth clipping: %d errors\n", errors);
  return errors;
}

int main(int argc, char** argv) {
  // (AI-assisted) as the 3DS draws, but the wave texture every frame and the near ocean's alpha
  // as the program's (config.ini ocean_texture_rate, ocean_near_alpha): the same as emulated
  g_settings.ocean_texture_rate = 1;
  g_settings.ocean_near_alpha = 0.f;
  const int transition_errors = depth_clip_test() + transition_test();
  if (argc < 2) {
    printf("(no dump file: ocean_replay ocean_dump.bin [iterations] replays one)\n%s\n",
           transition_errors ? "FAIL" : "OK");
    return transition_errors ? 1 : 0;
  }
  Bucket mid, near;
  if (!load(argv[1], &mid, &near)) {
    printf("can't read %s\n", argv[1]);
    return 2;
  }
  const int iters = argc > 2 ? atoi(argv[2]) : 200;
  // (only the stubbed decode_for_cpu / upload_source are called: no constructor needed)
  alignas(CtrVram) static u8 vram_mem[sizeof(CtrVram)];
  CtrVram& vram = *reinterpret_cast<CtrVram*>(vram_mem);

  CtrOceanFull ref_ocean;
  ref_ocean.set_native(false);
  g_draws.clear();
  run_frame(ref_ocean, vram, mid, near);
  const auto ref = triangles(g_draws);
  if (getenv("OCEAN_DEBUG")) {
    const float px = atof(getenv("OCEAN_DEBUG")), py = getenv("OCEAN_DEBUG_Y") ? atof(getenv("OCEAN_DEBUG_Y")) : 0;
    for (auto& t : ref) {
      bool near = false;
      for (auto& v : t.v) {
        near |= std::abs(v.x / 16 - 2048 - px) < 3 && std::abs(v.y / 16 - 2048 - py) < 3;
      }
      if (t.bucket == 0 && near) {
        printf("  ref: (%.1f %.1f %.0f %.2f %.2f) (%.1f %.1f %.0f) (%.1f %.1f %.0f)\n", t.v[0].x / 16 - 2048,
               t.v[0].y / 16 - 2048, t.v[0].z, t.v[0].s, t.v[0].t, t.v[1].x / 16 - 2048, t.v[1].y / 16 - 2048,
               t.v[1].z, t.v[2].x / 16 - 2048, t.v[2].y / 16 - 2048, t.v[2].z);
      }
    }
  }
  const auto ref_tex = g_tex;

  CtrOceanFull nat_ocean;
  g_draws.clear();
  run_frame(nat_ocean, vram, mid, near);
  const auto nat = triangles(g_draws);
  const bool tex_same = ref_tex == g_tex;

  // (AI-assisted) the native draws' bytes (vertices, indices) to a file / against an earlier
  // version's file: a change that must not change them (OCEAN_DRAWS_OUT, OCEAN_DRAWS_REF)
  int draws_errors = 0;
  {
    std::vector<u8> bytes;
    auto put = [&](const void* p, size_t n) {
      bytes.insert(bytes.end(), (const u8*)p, (const u8*)p + n);
    };
    for (const auto& d : g_draws) {
      const u32 head[4] = {d.clip, (u32)d.bucket, (u32)d.v.size(), (u32)d.idx.size()};
      put(head, sizeof(head));
      put(d.v.data(), d.v.size() * sizeof(d.v[0]));
      put(d.idx.data(), d.idx.size() * sizeof(d.idx[0]));
    }
    if (const char* out = getenv("OCEAN_DRAWS_OUT")) {
      FILE* f = fopen(out, "wb");
      fwrite(bytes.data(), 1, bytes.size(), f);
      fclose(f);
    }
    if (const char* in = getenv("OCEAN_DRAWS_REF")) {
      std::ifstream f(in, std::ios::binary);
      std::vector<u8> old((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      draws_errors = old != bytes;
      printf("native draws vs %s: %zu vs %zu bytes, %s\n", in, old.size(), bytes.size(),
             draws_errors ? "DIFFERENT" : "the same");
    }
  }
  int errors = compare(ref, nat, 0) + compare(ref, nat, 1) + transition_errors + draws_errors;
  printf("wave texture: %s\n", tex_same ? "same" : "DIFFERENT");
  errors += !tex_same;
  errors += g_tex.size() != 128 * 128 * 4;
  // the wave texture (128 x 128 RGBA8, as uploaded) to a file / against an earlier version's file
  if (const char* out = getenv("OCEAN_TEX_OUT")) {
    FILE* f = fopen(out, "wb");
    fwrite(g_tex.data(), 1, g_tex.size(), f);
    fclose(f);
  }
  if (const char* in = getenv("OCEAN_TEX_REF")) {
    std::vector<u8> old(g_tex.size());
    FILE* f = fopen(in, "rb");
    const bool ok = f && fread(old.data(), 1, old.size(), f) == old.size();
    if (f) {
      fclose(f);
    }
    int diff = 0, worst = 0;
    for (size_t i = 0; ok && i < old.size(); i++) {
      const int d = std::abs((int)old[i] - (int)g_tex[i]);
      diff += d != 0;
      worst = std::max(worst, d);
    }
    printf("wave texture vs %s: %s, %d bytes differ (at most by %d)\n", in,
           ok ? "read" : "NOT READ", diff, worst);
    errors += !ok || diff;
  }

  const double t_ref = time_frames(ref_ocean, vram, mid, near, iters);
  const double t_nat = time_frames(nat_ocean, vram, mid, near, iters);
  // (AI-assisted) as the 3DS has it by default: the wave texture every 2nd frame
  g_settings.ocean_texture_rate = 2;
  CtrOceanFull nat2_ocean;
  const double t_nat2 = time_frames(nat2_ocean, vram, mid, near, iters);
  printf("host ms/frame: reference %.3f, native %.3f (wave texture every 2nd frame: %.3f)\n",
         t_ref, t_nat, t_nat2);
  printf("%s\n", errors ? "FAIL" : "OK");
  return errors ? 1 : 0;
}
