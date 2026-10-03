/*!
 * @file ocean_replay.cpp
 * (AI-assisted)
 * Host replay test of the 3DS full ocean (game/graphics/ctr/CtrOceanFull.h): one frame's two ocean
 * buckets, dumped by the 3DS build (flag file sdmc:/3ds/jak1/ocean_dump, see CtrOceanFull.cpp),
 * run through CtrOceanFull with the emulated VU programs (the reference) and with the native ones,
 * with the GPU calls captured. Compares the triangles in screen space and times both.
 *
 *   test/ctr_ocean/build.sh && /tmp/ocean_replay ocean_dump.bin [iterations]
 *
 * Exit status 0 when the native triangles match: every native triangle that the VU program would
 * draw whole (all vertices inside its guard band) is one of its triangles (position within 1/8
 * pixel, color, fog, texture coordinates up to whole repeats), and every reference triangle is
 * covered (equal to a native one, or a piece the VU clipped out of one).
 */

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "common/dma/dma_chain_read.h"
#include "common/log/log.h"

#include "game/graphics/ctr/CtrOceanFull.h"
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
void ctr_gpu_tex_update(int, const uint8_t* rgba) {
  memcpy(g_tex.data(), rgba, g_tex.size());
}
void ctr_gpu_tex_delete(int) {}
double ctr_gpu_time_ms(void) {
  using namespace std::chrono;
  return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}
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

int main(int argc, char** argv) {
  if (argc < 2) {
    printf("usage: ocean_replay ocean_dump.bin [iterations]\n");
    return 2;
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

  int errors = compare(ref, nat, 0) + compare(ref, nat, 1);
  printf("wave texture: %s\n", tex_same ? "same" : "DIFFERENT");
  errors += !tex_same;

  const double t_ref = time_frames(ref_ocean, vram, mid, near, iters);
  const double t_nat = time_frames(nat_ocean, vram, mid, near, iters);
  printf("host ms/frame: reference %.3f, native %.3f\n", t_ref, t_nat);
  printf("%s\n", errors ? "FAIL" : "OK");
  return errors ? 1 : 0;
}
