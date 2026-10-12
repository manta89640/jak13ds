/*!
 * @file ctr_vis_builder.cpp
 * (AI-assisted)
 * ctr_vis_builder: a level's finer visibility (.c3v sidecar, game/graphics/ctr/c3v_format.h) from
 * its finished .c3l. See docs/3ds-port/c3l_format.md, "Fine visibility (.c3v)".
 *
 *   ctr_vis_builder <in.c3l> [<out.c3v>] [options]     (default out: next to the .c3l)
 *   ctr_vis_builder --all <c3l dir> [options]          (every level in the folder)
 *
 * Method (conservative, no GPU):
 *  - Cells: a grid of --cell meters over the level; a cell is kept where the camera can be: within
 *    --above meters over (or --below under) a roughly horizontal surface of the level (tfrag, tie),
 *    widened by --spread cells sideways. The camera anywhere else: no set (nothing extra hidden).
 *  - Samples: the cells' corners and centers (a body-centered grid). Every point of a cell is within
 *    eps = 0.56 * cell of one of its 9 samples.
 *  - From each sample, the level's opaque tfrag (draws the runtime draws opaque, depth tested and
 *    written, with no alpha test that can fail; chunks without a distance limit; both LOD tiers on
 *    their own, a pixel only counting where both hide) is rasterized into a cube of depth maps
 *    (--face pixels per side, with a guard band). No ties, shrubs, merc (doors, platforms) or
 *    blended geometry: they never hide anything. Occluders nearer than --near-k * eps are left out.
 *  - Moving the camera by up to eps from the sample: a point behind the depth map at pixel p stays
 *    hidden unless a ray from the moved camera gets behind the map through a gap: a depth
 *    discontinuity (a silhouette, a hole, the map's edge) whose distance d is within asin(eps / d)
 *    of p. Such pixels are marked unsafe (a chamfer propagation over the map); the others hide what
 *    is behind the largest depth of their 3x3 neighbourhood (+2% + 0.25 m).
 *  - A unit (a visibility run, or a 96 triangle piece) is hidden from a sample when every pixel its
 *    bounding box (then, if needed, each of its triangles' boxes) covers on the cube hides it. The
 *    cell's set: the units seen from any of its samples.
 *  - Cells with similar sets are merged (neighbours, the fewest extra triangles first) until the
 *    sets fit in --budget-kb; then written as runs of cells along x.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/dma/gs.h"
#include "common/util/FileUtil.h"

#include "fmt/format.h"
#include "game/graphics/ctr/c3l_format.h"
#include "game/graphics/ctr/c3l_reader.h"
#include "game/graphics/ctr/c3v_format.h"
#include "third-party/CLI11.hpp"

namespace {

struct Options {
  float cell = 4.f;      // meters
  int face = 64;         // pixels per cube face (core)
  float lod_near = 160.f, lod_far = 240.f;  // LOD tiers 1 / 2 as occluders only nearer / further
  float above = 14.f;    // meters above a floor
  float below = 2.f;     // meters below a floor
  int spread = 2;        // cells sideways
  float occ_range = 400.f;  // meters: occluders only this close
  float budget_kb = 128.f;  // the sets
  int guard = 0;            // guard band (pixels) around each face's core
  float depth_rel = 0.02f, depth_abs = 0.25f;  // depth margin
  int threads = 0;
  bool verbose = false;
  int max_samples = 0;  // (testing)
  std::vector<std::string> camera_files;  // recorded camera positions: "level x y z" (meters)
  float cam_radius = 6.f, cam_up = 3.f, cam_down = 3.f;  // cells around them (meters)
  std::vector<float> debug_point;  // (testing) one sample here (meters), pictures of its maps
};

constexpr float kM = 4096.f;  // game units per meter

struct V3 {
  float x, y, z;
};
inline V3 sub(V3 a, V3 b) {
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}
inline float dot(V3 a, V3 b) {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline V3 cross(V3 a, V3 b) {
  return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline float len(V3 a) {
  return std::sqrt(dot(a, a));
}
inline float comp(V3 v, int a) {
  return a == 0 ? v.x : (a == 1 ? v.y : v.z);
}

struct Tri {
  V3 v[3];
};
struct Box {
  V3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
  void add(V3 p) {
    lo = {std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z)};
    hi = {std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z)};
  }
};
inline float box_dist(const Box& b, V3 p) {
  const float dx = std::max({b.lo.x - p.x, 0.f, p.x - b.hi.x});
  const float dy = std::max({b.lo.y - p.y, 0.f, p.y - b.hi.y});
  const float dz = std::max({b.lo.z - p.z, 0.f, p.z - b.hi.z});
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

struct OccCluster {  // up to 32 occluder triangles close to each other, of one chunk
  Box box;
  u32 first, count;
  u32 tier;       // the chunk's lod_tier (0 always drawn, 1 near, 2 far)
  V3 lod_center;  // tiers 1 and 2
};

struct Unit {
  u32 first_tri, tri_count;
  Box box;
};

struct LevelGeo {
  std::string name;
  u32 level_hash = 0;
  u32 covered = 0;
  std::vector<Unit> units;
  std::vector<Tri> unit_tris;
  std::vector<Box> unit_tri_boxes;
  std::vector<Tri> occ[1];  // tfrag tiers 0, 1, 2
  std::vector<OccCluster> occ_clusters[1];
  std::vector<u32> occ_vid[1];  // 3 per occluder triangle: its corners, welded by position
  std::vector<Tri> floors;
  Box bounds;
  u64 total_tris = 0;
};

int texture_min_alpha(const u8* data, int w, int h, int format) {
  const int n = w * h;
  int m = 255;
  switch (format) {
    case c3l::TEX_RGB565:
    case c3l::TEX_ETC1:
      return 255;
    case c3l::TEX_RGBA4:
      for (int i = 0; i < n; i++) {
        m = std::min(m, (data[2 * i] & 0xf) * 17);
      }
      return m;
    case c3l::TEX_ETC1A4:
      for (int b = 0; b < n / 16; b++) {
        for (int k = 0; k < 8; k++) {
          const u8 v = data[16 * b + k];
          m = std::min({m, (v & 0xf) * 17, (v >> 4) * 17});
        }
      }
      return m;
    case c3l::TEX_RGBA8:
      for (int i = 0; i < n; i++) {
        m = std::min<int>(m, data[4 * i]);
      }
      return m;
    default:
      return 0;
  }
}

bool load_level(const std::string& path, LevelGeo* out, std::string* err) {
  c3l::Reader f(path.c_str());
  c3l::Header hdr;
  if (!f.ok() || !f.read(0, &hdr, sizeof(hdr)) || memcmp(hdr.magic, c3l::kMagic, 4)) {
    *err = "not a .c3l file";
    return false;
  }
  if (hdr.version < 10) {
    *err = fmt::format("c3l v{}: no visibility runs (convert again)", hdr.version);
    return false;
  }
  out->name = std::string(hdr.level_name, strnlen(hdr.level_name, 32));
  std::vector<c3l::Chunk> chunks;
  std::vector<c3l::Draw> draws;
  std::vector<c3l::Texture> texs;
  std::vector<c3l::Vertex> verts;
  std::vector<u16> indices;
  if (!f.read_array(hdr.chunks_offset, hdr.num_chunks, &chunks) ||
      !f.read_array(hdr.draw_data_offset, hdr.draw_data_size / sizeof(c3l::Draw), &draws) ||
      !f.read_array(hdr.textures_offset, hdr.num_textures, &texs) ||
      !f.read_array(hdr.vertex_data_offset, hdr.vertex_data_size / sizeof(c3l::Vertex), &verts) ||
      !f.read_array(hdr.index_data_offset, hdr.index_data_size / 2, &indices)) {
    *err = "truncated file";
    return false;
  }
  // the visibility runs, checked as CtrLevels::load_file does (a chunk whose runs don't cover its
  // draws loses them there: the same here)
  std::vector<c3l::VisRun> runs;
  std::vector<u32> chunk_first_run(chunks.size(), 0), draw_first_run(draws.size(), 0),
      draw_run_count(draws.size(), 0);
  {
    u32 lo = UINT32_MAX, hi = 0;
    for (const auto& ch : chunks) {
      if (ch.vis_run_count) {
        lo = std::min<u32>(lo, ch.vis_runs_offset);
        hi = std::max<u32>(hi, ch.vis_runs_offset + ch.vis_run_count * sizeof(c3l::VisRun));
      }
    }
    if (lo < hi && !f.read_array(lo, (hi - lo) / sizeof(c3l::VisRun), &runs)) {
      runs.clear();
    }
    for (size_t ci = 0; ci < chunks.size(); ci++) {
      auto& ch = chunks[ci];
      if (!ch.vis_run_count) {
        continue;
      }
      const u32 first = (ch.vis_runs_offset - lo) / sizeof(c3l::VisRun);
      bool ok = !runs.empty() && first + ch.vis_run_count <= runs.size();
      std::vector<u32> covered(ch.draw_count, 0);
      for (u32 r = first; ok && r < first + ch.vis_run_count; r++) {
        const auto& run = runs[r];
        ok = run.draw < ch.draw_count;
        if (ok) {
          const u32 di = ch.first_draw + run.draw;
          if (!draw_run_count[di]) {
            draw_first_run[di] = r;
          }
          ok = draw_first_run[di] + draw_run_count[di] == r;
          draw_run_count[di]++;
          covered[run.draw] += run.index_count;
        }
      }
      for (u32 k = 0; ok && k < ch.draw_count; k++) {
        ok = !draw_run_count[ch.first_draw + k] ||
             covered[k] == draws[ch.first_draw + k].index_count;
      }
      if (!ok) {
        for (u32 k = 0; k < ch.draw_count; k++) {
          draw_run_count[ch.first_draw + k] = 0;
        }
        ch.vis_run_count = 0;
        continue;
      }
      chunk_first_run[ci] = first;
    }
  }
  out->covered = c3v::covered_chunks(chunks.data(), (u32)chunks.size());
  out->level_hash = c3v::fine_vis_level_hash(chunks.data(), out->covered, draws.data(),
                                             (u32)draws.size(), runs.data(),
                                             chunk_first_run.data());

  // textures whose alpha never fails the runtime's alpha test (first mip level)
  std::vector<int> tex_min_alpha(texs.size(), -1);
  auto min_alpha = [&](u32 t) {
    if (t >= texs.size()) {
      return 0;
    }
    if (tex_min_alpha[t] < 0) {
      const auto& tx = texs[t];
      std::vector<u8> data(c3l::texture_level_bytes(tx.w, tx.h, tx.format));
      tex_min_alpha[t] = f.read(tx.data_offset, data.data(), data.size())
                             ? texture_min_alpha(data.data(), tx.w, tx.h, tx.format)
                             : 0;
    }
    return tex_min_alpha[t];
  };
  // an occluder draw: drawn opaque, depth tested (GEQUAL) and written, every pixel of it
  auto opaque = [&](const c3l::Draw& d, const c3l::Chunk& ch) {
    if (d.kind & c3l::kDrawCullBack) {
      return false;  // seen from behind it isn't drawn
    }
    DrawMode mode;
    mode.as_int() = d.mode;
    if (mode.get_ab_enable() || !mode.get_depth_write_enable() || !mode.get_zt_enable() ||
        mode.get_depth_test() != GsTest::ZTest::GEQUAL) {
      return false;
    }
    if (mode.get_at_enable() && mode.get_alpha_test() != DrawMode::AlphaTest::ALWAYS) {
      // alpha test: only a texture opaque everywhere and vertices with alpha 0x80 or more (NEVER,
      // the PS2's afail tricks: never an occluder)
      if (mode.get_alpha_test() == DrawMode::AlphaTest::NEVER || min_alpha(d.texture) < 255) {
        return false;
      }
      for (u32 i = d.first_index; i < d.first_index + d.index_count && i < indices.size(); i++) {
        const u32 v = ch.first_vertex + indices[i];
        if (v >= verts.size() || verts[v].rgba[3] < 0x80) {
          return false;
        }
      }
    }
    return true;
  };

  auto world = [&](const c3l::Chunk& ch, u16 i) {
    const auto& v = verts[ch.first_vertex + i];
    return V3{(ch.origin[0] + v.pos[0] * ch.scale) / kM, (ch.origin[1] + v.pos[1] * ch.scale) / kM,
              (ch.origin[2] + v.pos[2] * ch.scale) / kM};
  };
  auto tri_at = [&](const c3l::Chunk& ch, u32 i, Tri* t) {
    if (i + 3 > indices.size()) {
      return false;
    }
    for (int k = 0; k < 3; k++) {
      if (ch.first_vertex + indices[i + k] >= verts.size()) {
        return false;
      }
      t->v[k] = world(ch, indices[i + k]);
    }
    return true;
  };

  out->occ[0].clear();
  out->occ_clusters[0].clear();
  out->occ_vid[0].clear();
  // occluder corners welded by their exact position (the converter makes a position shared by
  // several chunks identically in each)
  std::unordered_map<u64, std::vector<std::pair<V3, u32>>> weld;
  u32 next_vid = 0;
  for (u32 ci = 0; ci < out->covered; ci++) {
    const auto& ch = chunks[ci];
    // units
    auto add_unit = [&](const c3l::Draw& d, u32 first_index, u32 count) {
      Unit u;
      u.first_tri = (u32)out->unit_tris.size();
      for (u32 i = first_index; i + 3 <= first_index + count; i += 3) {
        Tri t;
        if (!tri_at(ch, i, &t)) {
          continue;
        }
        Box tb;
        for (auto& p : t.v) {
          tb.add(p);
          u.box.add(p);
        }
        out->unit_tris.push_back(t);
        out->unit_tri_boxes.push_back(tb);
      }
      u.tri_count = (u32)out->unit_tris.size() - u.first_tri;
      out->total_tris += u.tri_count;
      out->units.push_back(u);
      (void)d;
    };
    if (ch.vis_run_count) {
      std::vector<u32> next(ch.draw_count, 0);
      for (u32 k = 0; k < ch.draw_count; k++) {
        next[k] = draws[ch.first_draw + k].first_index;
      }
      for (u32 r = 0; r < ch.vis_run_count; r++) {
        const auto& run = runs[chunk_first_run[ci] + r];
        add_unit(draws[ch.first_draw + run.draw], next[run.draw], run.index_count);
        next[run.draw] += run.index_count;
      }
    } else {
      for (u32 k = 0; k < ch.draw_count && ch.first_draw + k < draws.size(); k++) {
        const auto& d = draws[ch.first_draw + k];
        for (u32 i = 0; i < d.index_count; i += c3v::kRunIndices) {
          add_unit(d, d.first_index + i, std::min(c3v::kRunIndices, d.index_count - i));
        }
      }
    }
    // occluders: tfrag without a distance limit
    const bool tfrag = ch.kind == c3l::CHUNK_KIND_TFRAG;
    if (tfrag && ch.max_dist == 0 && ch.lod_tier <= 2) {
      {
        const int b = 0;  // (one list: each cluster knows its tier)
        const u32 first = (u32)out->occ[b].size();
        Box gb;
        for (u32 k = 0; k < ch.draw_count && ch.first_draw + k < draws.size(); k++) {
          const auto& d = draws[ch.first_draw + k];
          if (!opaque(d, ch)) {
            continue;
          }
          for (u32 i = d.first_index; i + 3 <= d.first_index + d.index_count; i += 3) {
            Tri t;
            if (tri_at(ch, i, &t)) {
              out->occ[b].push_back(t);
              for (auto& p : t.v) {
                gb.add(p);
              }
            }
          }
        }
        // clusters of 32 triangles near each other (Morton order of their centers, 0.5 m)
        auto& tv = out->occ[b];
        auto morton = [&](const Tri& t) {
          u64 key = 0;
          u32 q[3];
          for (int c = 0; c < 3; c++) {
            const float lo = comp(gb.lo, c);
            const float m = (comp(t.v[0], c) + comp(t.v[1], c) + comp(t.v[2], c)) / 3 - lo;
            q[c] = (u32)std::clamp(m * 2.f, 0.f, 2097151.f);
          }
          for (int bit = 20; bit >= 0; bit--) {
            for (int c = 0; c < 3; c++) {
              key = (key << 1) | ((q[c] >> bit) & 1);
            }
          }
          return key;
        };
        std::vector<std::pair<u64, Tri>> sorted;
        for (u32 i = first; i < tv.size(); i++) {
          sorted.push_back({morton(tv[i]), tv[i]});
        }
        std::sort(sorted.begin(), sorted.end(),
                  [](const auto& x, const auto& y) { return x.first < y.first; });
        for (u32 i = 0; i < sorted.size(); i++) {
          tv[first + i] = sorted[i].second;
        }
        for (u32 i = first; i < tv.size(); i++) {
          for (int k = 0; k < 3; k++) {
            const V3 p = tv[i].v[k];
            u64 key[2];
            memcpy(&key[0], &p.x, 4);
            memcpy((char*)&key[0] + 4, &p.y, 4);
            key[1] = 0;
            memcpy(&key[1], &p.z, 4);
            const u64 h = key[0] * 0x9E3779B97F4A7C15ull ^ key[1];
            auto& lst = weld[h];
            u32 id = UINT32_MAX;
            for (const auto& [q, qid] : lst) {
              if (q.x == p.x && q.y == p.y && q.z == p.z) {
                id = qid;
                break;
              }
            }
            if (id == UINT32_MAX) {
              id = next_vid++;
              lst.push_back({p, id});
            }
            out->occ_vid[b].push_back(id);
          }
        }
        for (u32 i = first; i < tv.size(); i += 32) {
          OccCluster c;
          c.first = i;
          c.count = std::min<u32>(32, (u32)tv.size() - i);
          for (u32 j = i; j < i + c.count; j++) {
            for (auto& p : tv[j].v) {
              c.box.add(p);
            }
          }
          c.tier = ch.lod_tier;
          c.lod_center = {ch.lod_center[0] / kM, ch.lod_center[1] / kM, ch.lod_center[2] / kM};
          out->occ_clusters[b].push_back(c);
        }
      }
    }
    // floors (where the camera can be): the detailed tiers' tfrag and tie
    if ((ch.kind == c3l::CHUNK_KIND_TFRAG || ch.kind == c3l::CHUNK_KIND_TIE ||
         ch.kind == c3l::CHUNK_KIND_UNKNOWN) &&
        ch.lod_tier <= 1) {
      for (u32 k = 0; k < ch.draw_count && ch.first_draw + k < draws.size(); k++) {
        const auto& d = draws[ch.first_draw + k];
        for (u32 i = d.first_index; i + 3 <= d.first_index + d.index_count; i += 3) {
          Tri t;
          if (!tri_at(ch, i, &t)) {
            continue;
          }
          const V3 n = cross(sub(t.v[1], t.v[0]), sub(t.v[2], t.v[0]));
          const float l = len(n);
          if (l > 0 && std::abs(n.y) > 0.3f * l) {
            out->floors.push_back(t);
            for (auto& p : t.v) {
              out->bounds.add(p);
            }
          }
        }
      }
    }
  }
  return true;
}

// ---------------------------------------------------------------------------------------------
// the cube of depth maps
// ---------------------------------------------------------------------------------------------

inline V3 axis(int a, float s) {
  return {a == 0 ? s : 0.f, a == 1 ? s : 0.f, a == 2 ? s : 0.f};
}

// face: z = sf * d[af] (the face's axis), x = sr * d[ar], y = su * d[au]; its core is the
// directions with |x|, |y| <= z
struct Face {
  int af, ar, au;
  float sf, sr, su;
  V3 f, r, u;
};
Face make_face(int af, float sf, int ar, float sr, int au, float su) {
  return {af, ar, au, sf, sr, su, axis(af, sf), axis(ar, sr), axis(au, su)};
}
const Face kFaces[6] = {
    make_face(0, 1, 2, 1, 1, 1),  make_face(0, -1, 2, -1, 1, 1), make_face(1, 1, 0, 1, 2, 1),
    make_face(1, -1, 0, 1, 2, -1), make_face(2, 1, 0, -1, 1, 1), make_face(2, -1, 0, 1, 1, 1),
};

constexpr float kInf = 1e30f;

struct CubeGeom {
  int N = 64, G = 2, M = 68;
  float T = 1.f;              // tangent extent of the image (core: 1)
  std::vector<float> tx;      // per pixel column / row: tangent coordinate of the center
  std::vector<float> dist_k;  // per pixel: distance = z * dist_k
  std::vector<V3> dir[6];     // per face and pixel: the world direction of its center
  float half_px = 0;          // the largest angle from a pixel's center to its corner
  float cos_image = 0;        // cos of the largest angle from a face's axis to its image

  void init(int n, int guard) {
    N = n;
    G = guard;
    M = N + 2 * G;
    T = (float)M / N;
    tx.resize(M);
    for (int i = 0; i < M; i++) {
      tx[i] = ((i + 0.5f) - M / 2.f) * (2.f / N);
    }
    dist_k.resize(M * M);
    for (int y = 0; y < M; y++) {
      for (int x = 0; x < M; x++) {
        dist_k[y * M + x] = std::sqrt(1 + tx[x] * tx[x] + tx[y] * tx[y]);
      }
    }
    for (int f = 0; f < 6; f++) {
      const Face& fc = kFaces[f];
      dir[f].resize(M * M);
      for (int y = 0; y < M; y++) {
        for (int x = 0; x < M; x++) {
          const float k = 1.f / dist_k[y * M + x];
          dir[f][y * M + x] = {(fc.f.x + fc.r.x * tx[x] + fc.u.x * tx[y]) * k,
                               (fc.f.y + fc.r.y * tx[x] + fc.u.y * tx[y]) * k,
                               (fc.f.z + fc.r.z * tx[x] + fc.u.z * tx[y]) * k};
        }
      }
    }
    // the center pixel is the largest (in angle)
    const float h = 1.f / N;  // half a pixel, tangent units
    half_px = std::atan(std::sqrt(2.f) * h) * 1.01f;
    cos_image = 1.f / std::sqrt(1 + 2 * T * T);
  }
};

constexpr int kLayers = 4;

struct Params {
  float eps, occ_range;
  float cuts[kLayers];      // layer i: the nearest occluder at least this far
  float lod_near, lod_far;  // tier 1 occluders only nearer, tier 2 only further (lod_center)
  float depth_rel, depth_abs;
};

/*!
 * One sample: the cube of depth maps and the units that may be seen from within eps of it.
 *
 * A ray from a camera moved by up to eps toward a point P in the direction of pixel p, at
 * distance t from the sample, is within asin(eps / t) of p. If every direction within
 * R = asin(eps / dmin) of p (dmin: the nearest depth there) has an occluder no further than
 * dmax, a point further than dmax goes from in front of the occluders to behind them along the
 * ray, inside that window: through an occluder (hidden), or through a gap beside a nearer
 * surface into what's behind it (a hole in the far surface right behind a near object, which
 * the map can't see: the cell's other samples, and the margins, are for that). So a pixel hides
 * what's further than the largest depth within R of it (iterated: R from the window's nearest
 * depth), plus a margin; an empty direction (no occluder within the range) in the window hides
 * nothing.
 *
 * Layer i of a face's maps holds the nearest occluder at least cuts[i] away: each layer is a set
 * of real surfaces on its own, so each hides on its own; a pixel hides what's behind the best of
 * them. Leaving the near occluders out keeps a near object from widening the window (R) around
 * what a wall behind it hides.
 */
class SampleWork {
 public:
  static constexpr int kMaps = kLayers * 6;  // map layer * 6 + face
  double t_render = 0, t_occ = 0, t_units = 0;
  u64 clusters_drawn = 0, clusters_hidden = 0, windows_too_wide = 0;

  SampleWork(const CubeGeom& g, const LevelGeo& lev, const Params& p) : m_g(g), m_lev(lev), m_p(p) {
    const int MM = g.M * g.M;
    for (int f = 0; f < kMaps; f++) {
      m_w[f].resize(MM);
    }
    for (int f = 0; f < 6; f++) {
      m_occ[f].resize(g.N * g.N);
    }
    // min / max pyramids of each map's core depths
    for (int f = 0; f < kMaps; f++) {
      for (int n = g.N; n >= 1; n /= 2) {
        m_pmin[f].emplace_back(n * n);
        m_pmax[f].emplace_back(n * n);
      }
    }
    int levels = 0;
    for (int n = g.N; n >= 1; n /= 2) {
      levels++;
    }
    m_mip.resize(6);
    for (auto& fm : m_mip) {
      fm.resize(levels);
      int n = g.N;
      for (auto& l : fm) {
        l.resize(n * n);
        n /= 2;
      }
    }
  }

  // the units that may be seen from within eps of s (bits set in out)
  void run(V3 s, std::vector<u64>* out, u64* tri_tests) {
    using clk = std::chrono::steady_clock;
    auto ta = clk::now();
    render(s);
    auto tb = clk::now();
    t_render += std::chrono::duration<double>(tb - ta).count();
    ta = tb;
    bool any = false;  // does any core pixel hide anything? (else: everything may be seen)
    for (int f = 0; f < kMaps; f++) {
      build_pyramids(f);
    }
    for (int f = 0; f < 6; f++) {
      std::fill(m_occ[f].begin(), m_occ[f].end(), kInf);
      for (int layer = 0; layer < kLayers; layer++) {
        make_occ(layer, f);
      }
      build_mips(f);
      any = any || has_finite(f);
    }
    tb = clk::now();
    t_occ += std::chrono::duration<double>(tb - ta).count();
    ta = tb;
    const size_t nu = m_lev.units.size();
    out->assign((nu + 63) / 64, 0);
    for (size_t u = 0; u < nu; u++) {
      if (!any || unit_visible(s, m_lev.units[u], tri_tests)) {
        (*out)[u >> 6] |= 1ull << (u & 63);
      }
    }
    t_units += std::chrono::duration<double>(clk::now() - ta).count();
  }

  // (testing) PGM pictures per face: depth of layers 0 and 1, occluder distance (black: hides
  // nothing)
  void dump(const std::string& prefix) const {
    const int M = m_g.M, N = m_g.N;
    for (int f = 0; f < 6; f++) {
      FILE* fp = fopen(fmt::format("{}_f{}.pgm", prefix, f).c_str(), "wb");
      fmt::print(fp, "P5 {} {} 255\n", 3 * M, M);
      for (int y = M - 1; y >= 0; y--) {
        for (int l = 0; l < 2; l++) {
          for (int x = 0; x < M; x++) {
            const float w = m_w[l * 6 + f][y * M + x];
            const float d = w > 0 ? m_g.dist_k[y * M + x] / w : 0;
            fputc(w > 0 ? (int)std::clamp(255.f - d, 16.f, 255.f) : 0, fp);
          }
        }
        for (int x = 0; x < M; x++) {
          const int cx = x - m_g.G, cy = y - m_g.G;
          float v = kInf;
          if (cx >= 0 && cy >= 0 && cx < N && cy < N) {
            v = m_occ[f][cy * N + cx];
          }
          fputc(v < kInf ? (int)std::clamp(255.f - v, 16.f, 255.f) : 0, fp);
        }
      }
      fclose(fp);
    }
  }

 private:
  const CubeGeom& m_g;
  const LevelGeo& m_lev;
  const Params& m_p;
  std::vector<float> m_w[kMaps];  // 1 / z, 0: nothing
  std::vector<float> m_occ[6];    // core
  std::vector<std::vector<float>> m_pmin[kMaps], m_pmax[kMaps];  // [map][level], core
  std::vector<std::vector<std::vector<float>>> m_mip;  // [face][level] core occ, max
  std::vector<std::pair<float, u32>> m_order;

  bool has_finite(int f) const {
    for (float v : m_occ[f]) {
      if (v < kInf) {
        return true;
      }
    }
    return false;
  }

  // ---- occluders into the depth maps (front to back; clusters behind what's drawn skipped) ----
  void render(V3 s) {
    const int M = m_g.M;
    const float T = m_g.T;
    const float zclip = 0.02f;
    const float cut0 = m_p.cuts[0];
    for (int f = 0; f < kMaps; f++) {
      std::fill(m_w[f].begin(), m_w[f].end(), 0.f);
    }
    const auto& tris = m_lev.occ[0];
    const auto& cls = m_lev.occ_clusters[0];
    m_order.clear();
    for (u32 c = 0; c < cls.size(); c++) {
      const OccCluster& cl = cls[c];
      // LOD: the tier the runtime draws for certain (the camera is within eps of s)
      if (cl.tier == 1 || cl.tier == 2) {
        const float dl = len(sub(cl.lod_center, s));
        if (cl.tier == 1 ? dl > m_p.lod_near - m_p.eps : dl < m_p.lod_far + m_p.eps) {
          continue;
        }
      }
      const Box& bx = cl.box;
      const float dmin = box_dist(bx, s);
      if (dmin > m_p.occ_range) {
        continue;
      }
      const float fx = std::max(std::abs(bx.lo.x - s.x), std::abs(bx.hi.x - s.x));
      const float fy = std::max(std::abs(bx.lo.y - s.y), std::abs(bx.hi.y - s.y));
      const float fz = std::max(std::abs(bx.lo.z - s.z), std::abs(bx.hi.z - s.z));
      if (fx * fx + fy * fy + fz * fz < cut0 * cut0) {
        continue;  // all of it nearer than every layer's cut
      }
      m_order.push_back({dmin, c});
    }
    std::sort(m_order.begin(), m_order.end());
    for (const auto& [dmin, c] : m_order) {
      const auto& cl = cls[c];
      const Box& bx = cl.box;
      const V3 lo = sub(bx.lo, s), hi = sub(bx.hi, s);
      const bool check_range = len(V3{std::max(-lo.x, hi.x), std::max(-lo.y, hi.y),
                                      std::max(-lo.z, hi.z)}) > m_p.occ_range;
      u32 mask = 0;
      for (int f = 0; f < 6; f++) {
        const Face& fc = kFaces[f];
        auto rng = [&](int a, float sgn, float* mn, float* mx) {
          const float l = comp(lo, a) * sgn, h = comp(hi, a) * sgn;
          *mn = std::min(l, h);
          *mx = std::max(l, h);
        };
        float z0, z1, x0, x1, y0, y1;
        rng(fc.af, fc.sf, &z0, &z1);
        rng(fc.ar, fc.sr, &x0, &x1);
        rng(fc.au, fc.su, &y0, &y1);
        if (z1 < zclip) {
          continue;
        }
        // outside one of the image's side planes (x <= T z ...)
        if (x1 + T * z1 < 0 || x0 - T * z1 > 0 || y1 + T * z1 < 0 || y0 - T * z1 > 0) {
          continue;
        }
        // behind what the deepest layer has already at every pixel it covers: it loses in every
        // layer (each layer's map is at least as near)
        if (z0 > zclip) {
          const float k = m_g.N / 2.f, o = M / 2.f;
          const float px0 = std::min(x0 / z0, x0 / z1) * k + o, px1 = std::max(x1 / z0, x1 / z1) * k + o;
          const float py0 = std::min(y0 / z0, y0 / z1) * k + o, py1 = std::max(y1 / z0, y1 / z1) * k + o;
          const int ix0 = std::max(0, (int)std::floor(px0)), ix1 = std::min(M - 1, (int)std::floor(px1));
          const int iy0 = std::max(0, (int)std::floor(py0)), iy1 = std::min(M - 1, (int)std::floor(py1));
          if (ix0 <= ix1 && iy0 <= iy1 && (ix1 - ix0 + 1) * (iy1 - iy0 + 1) <= 1024) {
            const float wmin = 1.f / z0;
            bool hidden = true;
            const float* W = m_w[(kLayers - 1) * 6 + f].data();
            for (int y = iy0; y <= iy1 && hidden; y++) {
              for (int x = ix0; x <= ix1; x++) {
                if (W[y * M + x] <= wmin) {
                  hidden = false;
                  break;
                }
              }
            }
            if (hidden) {
              clusters_hidden++;
              continue;
            }
          }
        }
        mask |= 1u << f;
      }
      if (!mask) {
        continue;
      }
      clusters_drawn++;
      for (u32 ti = cl.first; ti < cl.first + cl.count; ti++) {
        const Tri& t = tris[ti];
        V3 d[3];
        for (int k = 0; k < 3; k++) {
          d[k] = sub(t.v[k], s);
        }
        if (check_range && std::max({dot(d[0], d[0]), dot(d[1], d[1]), dot(d[2], d[2])}) >
                               m_p.occ_range * m_p.occ_range) {
          continue;  // occluders only within the occluder range (all of the triangle)
        }
        const V3 n = cross(sub(d[1], d[0]), sub(d[2], d[0]));
        const float c = dot(n, d[0]);  // plane: dot(n, x) = c
        const float nl = len(n);
        if (nl == 0 || std::abs(c) < 1e-6f * nl) {
          continue;  // seen edge on
        }
        for (int f = 0; f < 6; f++) {
          if (!(mask & (1u << f))) {
            continue;
          }
          const Face& fc = kFaces[f];
          float z[3], x[3], y[3];
          int behind = 0, out_l = 0, out_r = 0, out_b = 0, out_t = 0;
          for (int k = 0; k < 3; k++) {
            z[k] = comp(d[k], fc.af) * fc.sf;
            x[k] = comp(d[k], fc.ar) * fc.sr;
            y[k] = comp(d[k], fc.au) * fc.su;
            behind += z[k] < zclip;
            out_l += x[k] < -T * z[k];
            out_r += x[k] > T * z[k];
            out_b += y[k] < -T * z[k];
            out_t += y[k] > T * z[k];
          }
          if (behind == 3 || out_l == 3 || out_r == 3 || out_b == 3 || out_t == 3) {
            continue;
          }
          float px[4], py[4];
          int np = 0;
          for (int k = 0; k < 3; k++) {
            const int k2 = (k + 1) % 3;
            const bool in1 = z[k] >= zclip, in2 = z[k2] >= zclip;
            if (in1) {
              px[np] = x[k] / z[k];
              py[np] = y[k] / z[k];
              np++;
            }
            if (in1 != in2) {
              const float a = (zclip - z[k]) / (z[k2] - z[k]);
              px[np] = (x[k] + (x[k2] - x[k]) * a) / zclip;
              py[np] = (y[k] + (y[k2] - y[k]) * a) / zclip;
              np++;
            }
          }
          if (np < 3) {
            continue;
          }
          const float k = m_g.N / 2.f, o = M / 2.f;
          for (int i = 0; i < np; i++) {
            px[i] = px[i] * k + o;
            py[i] = py[i] * k + o;
          }
          // w = 1 / z on the triangle's plane: (nr x + nu y + nf) / c, x = (px - o) / k
          const float nr = comp(n, fc.ar) * fc.sr, nu = comp(n, fc.au) * fc.su,
                      nf = comp(n, fc.af) * fc.sf;
          const float pa = nr / k / c, pb = nu / k / c, pc = (nf - nr * o / k - nu * o / k) / c;
          for (int i = 1; i + 1 < np; i++) {
            raster(f, px[0], py[0], px[i], py[i], px[i + 1], py[i + 1], pa, pb, pc);
          }
        }
      }
    }
  }

  void raster(int f, float x0, float y0, float x1, float y1, float x2, float y2, float pa,
              float pb, float pc) {
    const int M = m_g.M;
    float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
    if (std::abs(area) < 1e-12f) {
      return;
    }
    if (area < 0) {
      std::swap(x1, x2);
      std::swap(y1, y2);
      area = -area;
    }
    const int ix0 = std::max(0, (int)std::floor(std::min({x0, x1, x2}) - 0.5f));
    const int ix1 = std::min(M - 1, (int)std::ceil(std::max({x0, x1, x2}) - 0.5f));
    const int iy0 = std::max(0, (int)std::floor(std::min({y0, y1, y2}) - 0.5f));
    const int iy1 = std::min(M - 1, (int)std::ceil(std::max({y0, y1, y2}) - 0.5f));
    if (ix0 > ix1 || iy0 > iy1) {
      return;
    }
    const float e = -1e-4f * area;  // a little tolerance: no cracks on shared edges
    const float* DK = m_g.dist_k.data();
    float* Wl[kLayers];
    for (int l = 0; l < kLayers; l++) {
      Wl[l] = m_w[l * 6 + f].data();
    }
    for (int iy = iy0; iy <= iy1; iy++) {
      const float cy = iy + 0.5f;
      for (int ix = ix0; ix <= ix1; ix++) {
        const float cx = ix + 0.5f;
        const float w0 = (x1 - cx) * (y2 - cy) - (x2 - cx) * (y1 - cy);
        const float w1 = (x2 - cx) * (y0 - cy) - (x0 - cx) * (y2 - cy);
        const float w2 = area - w0 - w1;
        if (w0 < e || w1 < e || w2 < e) {
          continue;
        }
        const float w = pa * cx + pb * cy + pc;
        const int p = iy * M + ix;
        if (w <= Wl[kLayers - 1][p] || w <= 0) {
          continue;  // (the deepest layer is the furthest: every layer has it nearer)
        }
        const float d = DK[p] / w;
        for (int l = 0; l < kLayers && d >= m_p.cuts[l]; l++) {
          if (w > Wl[l][p]) {
            Wl[l][p] = w;
          }
        }
      }
    }
  }

  void build_pyramids(int mi) {
    const int M = m_g.M, N = m_g.N, G = m_g.G;
    const float* W = m_w[mi].data();
    auto& mn0 = m_pmin[mi][0];
    auto& mx0 = m_pmax[mi][0];
    for (int y = 0; y < N; y++) {
      for (int x = 0; x < N; x++) {
        const int p = (y + G) * M + x + G;
        const float d = W[p] > 0 ? m_g.dist_k[p] / W[p] : kInf;  // kInf: nothing
        mn0[y * N + x] = d;
        mx0[y * N + x] = d;
      }
    }
    int n = N;
    for (size_t l = 1; l < m_pmin[mi].size(); l++) {
      const int h = n / 2;
      const auto& a = m_pmin[mi][l - 1];
      const auto& b = m_pmax[mi][l - 1];
      auto& c = m_pmin[mi][l];
      auto& d = m_pmax[mi][l];
      for (int y = 0; y < h; y++) {
        for (int x = 0; x < h; x++) {
          const int i0 = 2 * y * n + 2 * x, i1 = i0 + 1, i2 = i0 + n, i3 = i2 + 1;
          c[y * h + x] = std::min({a[i0], a[i1], a[i2], a[i3]});
          d[y * h + x] = std::max({b[i0], b[i1], b[i2], b[i3]});
        }
      }
      n = h;
    }
  }

  // [x0, x1] x [y0, y1] core pixels of a map's pyramid, rounded out to cells
  float pyr_query(const std::vector<std::vector<float>>& pyr, bool want_max, int x0, int y0,
                  int x1, int y1) const {
    int l = 0;
    while ((x1 >> l) - (x0 >> l) > 2 || (y1 >> l) - (y0 >> l) > 2) {
      l++;
    }
    const int n = m_g.N >> l;
    const auto& m = pyr[l];
    float v = want_max ? 0.f : kInf;
    for (int y = y0 >> l; y <= (y1 >> l); y++) {
      for (int x = x0 >> l; x <= (x1 >> l); x++) {
        v = want_max ? std::max(v, m[y * n + x]) : std::min(v, m[y * n + x]);
      }
    }
    return v;
  }

  // the range of one face coordinate (x = a / z) over the cap of radius r around v, whose
  // components on that axis and the face's axis are va, vz: false if none of it has z > 0
  static bool cap_range(float va, float vz, float sin_r, float* lo, float* hi) {
    const float rho = std::sqrt(va * va + vz * vz);
    if (rho <= sin_r) {
      *lo = -kInf;  // (the cap holds the direction perpendicular to both)
      *hi = kInf;
      return true;
    }
    const float phi = std::atan2(va, vz), del = std::asin(std::min(1.f, sin_r / rho));
    const float a0 = phi - del, a1 = phi + del;
    constexpr float kH = (float)M_PI / 2;
    if (a1 - a0 >= 2 * kH || (a0 < -kH && a1 > kH)) {
      *lo = -kInf;
      *hi = kInf;
      return true;
    }
    if (a1 <= -kH || a0 >= kH) {
      return false;
    }
    *lo = a0 <= -kH ? -kInf : std::tan(a0);
    *hi = a1 >= kH ? kInf : std::tan(a1);
    return true;
  }

  // the smallest / largest depth of layer `layer` over the directions within r of v (all faces;
  // each face's part rounded out to a box of pixels)
  float cap_query(int layer, V3 v, float r, bool want_max) const {
    float out = want_max ? 0.f : kInf;
    if (r >= 1.5f) {
      return want_max ? kInf : 0.f;
    }
    const float sin_r = std::sin(r), k = m_g.N / 2.f;
    constexpr float kCornerAngle = 0.9553166f;  // a face's core corner from its axis
    for (int f = 0; f < 6; f++) {
      const Face& fc = kFaces[f];
      const float vz = comp(v, fc.af) * fc.sf;
      if (std::acos(std::clamp(vz, -1.f, 1.f)) > kCornerAngle + r) {
        continue;
      }
      float x0, x1, y0, y1;
      if (!cap_range(comp(v, fc.ar) * fc.sr, vz, sin_r, &x0, &x1) ||
          !cap_range(comp(v, fc.au) * fc.su, vz, sin_r, &y0, &y1)) {
        continue;
      }
      if (x0 > 1 || x1 < -1 || y0 > 1 || y1 < -1) {
        continue;
      }
      const int N = m_g.N;
      const int ix0 = std::max(0, (int)std::floor(std::max(x0, -1.f) * k + k) - 1);
      const int ix1 = std::min(N - 1, (int)std::floor(std::min(x1, 1.f) * k + k) + 1);
      const int iy0 = std::max(0, (int)std::floor(std::max(y0, -1.f) * k + k) - 1);
      const int iy1 = std::min(N - 1, (int)std::floor(std::min(y1, 1.f) * k + k) + 1);
      const int mi = layer * 6 + f;
      const float q = pyr_query(want_max ? m_pmax[mi] : m_pmin[mi], want_max, ix0, iy0, ix1, iy1);
      out = want_max ? std::max(out, q) : std::min(out, q);
      if (want_max && out >= kInf) {
        return out;
      }
    }
    return out;
  }

  // layer `layer`'s occluder distances on face f into m_occ[f] (the smaller one stays), per 2x2
  // block: the window around the block's center grown by the block's extent
  void make_occ(int layer, int f) {
    const int N = m_g.N;
    const float eps = m_p.eps;
    const float margin = 2.1f * m_g.half_px;  // the block's center to its corners
    const auto& mn = m_pmin[layer * 6 + f];
    float* O = m_occ[f].data();
    const Face& fc = kFaces[f];
    const float k = N / 2.f;
    for (int by = 0; by < N / 2; by++) {
      for (int bx = 0; bx < N / 2; bx++) {
        const float d0 = mn[1][by * (N / 2) + bx];
        if (d0 >= kInf) {
          continue;  // (an empty pixel: the max is kInf anyway)
        }
        const float tx = (2 * bx + 1 - k) / k, ty = (2 * by + 1 - k) / k;
        const float l = std::sqrt(1 + tx * tx + ty * ty);
        const V3 v{(fc.f.x + fc.r.x * tx + fc.u.x * ty) / l, (fc.f.y + fc.r.y * tx + fc.u.y * ty) / l,
                   (fc.f.z + fc.r.z * tx + fc.u.z * ty) / l};
        float r = std::asin(std::min(1.f, eps / d0)) + margin;
        bool ok = true;
        for (int it = 0; it < 8; it++) {
          if (r >= 1.5f) {
            ok = false;
            break;
          }
          const float dmin = cap_query(layer, v, r, false);
          const float r2 = std::asin(std::min(1.f, eps / dmin)) + margin;
          if (r2 <= r) {
            break;
          }
          r = r2;
        }
        if (!ok) {
          windows_too_wide++;
          continue;
        }
        const float dmax = cap_query(layer, v, r, true);
        if (dmax >= kInf) {
          continue;
        }
        const float o = dmax * (1 + m_p.depth_rel) + m_p.depth_abs;
        for (int dy = 0; dy < 2; dy++) {
          for (int dx = 0; dx < 2; dx++) {
            float& d = O[(2 * by + dy) * N + 2 * bx + dx];
            d = std::min(d, o);
          }
        }
      }
    }
  }

  void build_mips(int f) {
    m_mip[f][0] = m_occ[f];
    int n = m_g.N;
    for (size_t l = 1; l < m_mip[f].size(); l++) {
      const int h = n / 2;
      const auto& src = m_mip[f][l - 1];
      auto& dst = m_mip[f][l];
      for (int y = 0; y < h; y++) {
        for (int x = 0; x < h; x++) {
          dst[y * h + x] = std::max({src[2 * y * n + 2 * x], src[2 * y * n + 2 * x + 1],
                                     src[(2 * y + 1) * n + 2 * x],
                                     src[(2 * y + 1) * n + 2 * x + 1]});
        }
      }
      n = h;
    }
  }

  // the largest occluder distance over core pixels [x0, x1] x [y0, y1] (conservative: rounded up
  // to mip cells)
  float rect_max(int f, int x0, int y0, int x1, int y1) const {
    int l = 0;
    while ((x1 >> l) - (x0 >> l) > 2 || (y1 >> l) - (y0 >> l) > 2) {
      l++;
    }
    const int n = m_g.N >> l;
    const auto& m = m_mip[f][l];
    float v = 0;
    for (int y = y0 >> l; y <= (y1 >> l); y++) {
      for (int x = x0 >> l; x <= (x1 >> l); x++) {
        v = std::max(v, m[y * n + x]);
        if (v >= kInf) {
          return v;
        }
      }
    }
    return v;
  }

  // whether points (relative to the sample) at distance >= dmin may be seen through face f's
  // core: their footprint (points behind the face: all of it) against the occluder distances
  bool footprint_visible(int f, const V3* pts, int n, float dmin) const {
    const Face& fc = kFaces[f];
    const float k = m_g.N / 2.f;
    float lo_x = kInf, lo_y = kInf, hi_x = -kInf, hi_y = -kInf;
    int behind = 0;
    for (int i = 0; i < n; i++) {
      const float z = comp(pts[i], fc.af) * fc.sf;
      if (z <= 1e-4f) {
        behind++;
        continue;
      }
      const float x = comp(pts[i], fc.ar) * fc.sr / z, y = comp(pts[i], fc.au) * fc.su / z;
      lo_x = std::min(lo_x, x);
      hi_x = std::max(hi_x, x);
      lo_y = std::min(lo_y, y);
      hi_y = std::max(hi_y, y);
    }
    if (behind == n) {
      return false;  // (only directions behind the face: other faces)
    }
    if (behind) {
      lo_x = lo_y = -kInf;
      hi_x = hi_y = kInf;
    }
    const int N = m_g.N;
    const int x0 = std::max(0, (int)std::floor(std::max(lo_x, -2.f) * k + k) - 1);
    const int x1 = std::min(N - 1, (int)std::floor(std::min(hi_x, 2.f) * k + k) + 1);
    const int y0 = std::max(0, (int)std::floor(std::max(lo_y, -2.f) * k + k) - 1);
    const int y1 = std::min(N - 1, (int)std::floor(std::min(hi_y, 2.f) * k + k) + 1);
    if (x0 > x1 || y0 > y1) {
      return false;
    }
    return rect_max(f, x0, y0, x1, y1) >= dmin;
  }

  // can the box be in face f's core frustum (|x|, |y| <= z)
  static bool box_in_face(int f, const Box& b, V3 s) {
    const Face& fc = kFaces[f];
    const V3 ns[4] = {sub(fc.f, fc.r), {fc.f.x + fc.r.x, fc.f.y + fc.r.y, fc.f.z + fc.r.z},
                      sub(fc.f, fc.u), {fc.f.x + fc.u.x, fc.f.y + fc.u.y, fc.f.z + fc.u.z}};
    for (const V3& n : ns) {
      const V3 c{(n.x >= 0 ? b.hi.x : b.lo.x) - s.x, (n.y >= 0 ? b.hi.y : b.lo.y) - s.y,
                 (n.z >= 0 ? b.hi.z : b.lo.z) - s.z};
      if (dot(c, n) < 0) {
        return false;
      }
    }
    return true;
  }

  bool box_visible(V3 s, const Box& b, float dmin) const {
    V3 c[8];
    for (int i = 0; i < 8; i++) {
      c[i] = sub(V3{(i & 1) ? b.hi.x : b.lo.x, (i & 2) ? b.hi.y : b.lo.y, (i & 4) ? b.hi.z : b.lo.z},
                 s);
    }
    for (int f = 0; f < 6; f++) {
      if (box_in_face(f, b, s) && footprint_visible(f, c, 8, dmin)) {
        return true;
      }
    }
    return false;
  }

  bool tri_visible(V3 s, const Tri& t, const Box& tb) const {
    const float dmin = box_dist(tb, s);
    if (dmin < m_p.cuts[0]) {
      return true;
    }
    V3 d[3];
    for (int k = 0; k < 3; k++) {
      d[k] = sub(t.v[k], s);
    }
    for (int f = 0; f < 6; f++) {
      if (!box_in_face(f, tb, s)) {
        continue;
      }
      // clipped against z >= zc: a point at distance >= cuts[0] with z < zc is far outside the
      // core (a tangent coordinate of at least cuts[0] / zc / 2), so the part cut off never is
      // in it
      const Face& fc = kFaces[f];
      V3 pts[4];
      int np = 0;
      const float zc = 1e-3f;
      float z[3];
      for (int k = 0; k < 3; k++) {
        z[k] = comp(d[k], fc.af) * fc.sf;
      }
      for (int k = 0; k < 3; k++) {
        const int k2 = (k + 1) % 3;
        if (z[k] >= zc) {
          pts[np++] = d[k];
        }
        if ((z[k] >= zc) != (z[k2] >= zc)) {
          const float a = (zc - z[k]) / (z[k2] - z[k]);
          pts[np++] = {d[k].x + (d[k2].x - d[k].x) * a, d[k].y + (d[k2].y - d[k].y) * a,
                       d[k].z + (d[k2].z - d[k].z) * a};
        }
      }
      if (np >= 3 && footprint_visible(f, pts, np, dmin)) {
        return true;
      }
    }
    return false;
  }

  bool unit_visible(V3 s, const Unit& u, u64* tri_tests) const {
    if (!u.tri_count) {
      return false;
    }
    const float dmin = box_dist(u.box, s);
    if (dmin < m_p.cuts[0]) {
      return true;
    }
    if (!box_visible(s, u.box, dmin)) {
      return false;
    }
    for (u32 i = 0; i < u.tri_count; i++) {
      (*tri_tests)++;
      if (tri_visible(s, m_lev.unit_tris[u.first_tri + i], m_lev.unit_tri_boxes[u.first_tri + i])) {
        return true;
      }
    }
    return false;
  }
};

// ---------------------------------------------------------------------------------------------

struct Grid {
  V3 origin;  // meters
  float cell;
  int nx, ny, nz;
  u32 key(int x, int y, int z) const { return ((u32)z * ny + y) * nx + x; }
};

bool build(const std::string& in, const std::string& out_path, const Options& opt) {
  const auto t0 = std::chrono::steady_clock::now();
  LevelGeo lev;
  std::string err;
  if (!load_level(in, &lev, &err)) {
    fmt::print("{}: {}\n", in, err);
    return false;
  }
  if (lev.units.empty() || lev.floors.empty() || lev.occ[0].empty()) {
    fmt::print("{}: nothing to do ({} units, {} floor triangles, {} occluder triangles)\n", in,
               lev.units.size(), lev.floors.size(), lev.occ[0].size());
    // no file: the level renders as without fine visibility
    if (fs::exists(out_path)) {
      fs::remove(out_path);
    }
    return true;
  }
  Params prm;
  prm.eps = opt.cell * 0.56f + 0.02f;
  {
    const float k[kLayers] = {1.5f, 4.f, 8.f, 16.f};
    for (int l = 0; l < kLayers; l++) {
      prm.cuts[l] = k[l] * prm.eps;
    }
  }
  prm.lod_near = opt.lod_near;
  prm.lod_far = opt.lod_far;
  prm.occ_range = opt.occ_range;
  prm.depth_rel = opt.depth_rel;
  prm.depth_abs = opt.depth_abs;
  CubeGeom geom;
  geom.init(opt.face, opt.guard);

  // cells
  Grid grid;
  grid.cell = opt.cell;
  const float margin = (opt.spread + 1) * opt.cell + opt.above;
  grid.origin = {std::floor((lev.bounds.lo.x - margin) / opt.cell) * opt.cell,
                 std::floor((lev.bounds.lo.y - opt.below - opt.cell) / opt.cell) * opt.cell,
                 std::floor((lev.bounds.lo.z - margin) / opt.cell) * opt.cell};
  grid.nx = (int)std::ceil((lev.bounds.hi.x + margin - grid.origin.x) / opt.cell);
  grid.ny = (int)std::ceil((lev.bounds.hi.y + opt.above + opt.cell - grid.origin.y) / opt.cell);
  grid.nz = (int)std::ceil((lev.bounds.hi.z + margin - grid.origin.z) / opt.cell);
  std::vector<u8> cand((size_t)grid.nx * grid.ny * grid.nz, 0);
  // camera positions recorded in runs of the game (--camera): only the cells around them
  std::vector<V3> cams;
  for (const auto& path : opt.camera_files) {
    FILE* fp = fopen(path.c_str(), "r");
    if (!fp) {
      fmt::print("{}: can't read {}\n", lev.name, path);
      return false;
    }
    char name[64];
    float x, y, z;
    while (fscanf(fp, "%63s %f %f %f", name, &x, &y, &z) == 4) {
      if (lev.name == name) {
        cams.push_back({x, y, z});
      }
    }
    fclose(fp);
  }
  if (!opt.camera_files.empty() && cams.empty()) {
    fmt::print("{}: no camera positions for this level in the camera files: no .c3v\n", lev.name);
    if (fs::exists(out_path)) {
      fs::remove(out_path);
    }
    return true;
  }
  for (const V3& c : cams) {
    const int r = (int)std::ceil(opt.cam_radius / opt.cell);
    const int cx = (int)std::floor((c.x - grid.origin.x) / opt.cell);
    const int cz = (int)std::floor((c.z - grid.origin.z) / opt.cell);
    const int y0 = std::max(0, (int)std::floor((c.y - opt.cam_down - grid.origin.y) / opt.cell));
    const int y1 =
        std::min(grid.ny - 1, (int)std::floor((c.y + opt.cam_up - grid.origin.y) / opt.cell));
    for (int z = std::max(0, cz - r); z <= std::min(grid.nz - 1, cz + r); z++) {
      for (int x = std::max(0, cx - r); x <= std::min(grid.nx - 1, cx + r); x++) {
        for (int y = y0; y <= y1; y++) {
          cand[grid.key(x, y, z)] = 1;
        }
      }
    }
  }
  for (const auto& t : cams.empty() ? lev.floors : std::vector<Tri>{}) {
    Box b;
    for (auto& p : t.v) {
      b.add(p);
    }
    const int x0 = std::max(0, (int)std::floor((b.lo.x - grid.origin.x) / opt.cell) - opt.spread);
    const int x1 =
        std::min(grid.nx - 1, (int)std::floor((b.hi.x - grid.origin.x) / opt.cell) + opt.spread);
    const int z0 = std::max(0, (int)std::floor((b.lo.z - grid.origin.z) / opt.cell) - opt.spread);
    const int z1 =
        std::min(grid.nz - 1, (int)std::floor((b.hi.z - grid.origin.z) / opt.cell) + opt.spread);
    const int y0 = std::max(0, (int)std::floor((b.lo.y - opt.below - grid.origin.y) / opt.cell));
    const int y1 = std::min(grid.ny - 1,
                            (int)std::floor((b.hi.y + opt.above - grid.origin.y) / opt.cell));
    for (int z = z0; z <= z1; z++) {
      for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
          cand[grid.key(x, y, z)] = 1;
        }
      }
    }
  }
  std::vector<u32> cells;  // keys
  for (u32 k = 0; k < cand.size(); k++) {
    if (cand[k]) {
      cells.push_back(k);
    }
  }
  std::unordered_map<u32, u32> cell_index;
  cell_index.reserve(cells.size() * 2);
  for (u32 i = 0; i < cells.size(); i++) {
    cell_index[cells[i]] = i;
  }
  // samples: corners (lattice nodes, shared) and centers
  struct Sample {
    V3 p;
    std::vector<u32> cells;
  };
  std::vector<Sample> samples;
  {
    std::unordered_map<u64, u32> corner;
    corner.reserve(cells.size() * 2);
    const int nx1 = grid.nx + 1, ny1 = grid.ny + 1;
    for (u32 ci = 0; ci < cells.size(); ci++) {
      const u32 k = cells[ci];
      const int x = k % grid.nx, y = (k / grid.nx) % grid.ny, z = k / grid.nx / grid.ny;
      for (int c = 0; c < 8; c++) {
        const int cx = x + (c & 1), cy = y + ((c >> 1) & 1), cz = z + (c >> 2);
        const u64 ck = ((u64)cz * ny1 + cy) * nx1 + cx;
        auto it = corner.find(ck);
        if (it == corner.end()) {
          it = corner.emplace(ck, (u32)samples.size()).first;
          Sample s;
          s.p = {grid.origin.x + cx * opt.cell, grid.origin.y + cy * opt.cell,
                 grid.origin.z + cz * opt.cell};
          samples.push_back(std::move(s));
        }
        samples[it->second].cells.push_back(ci);
      }
      Sample s;
      s.p = {grid.origin.x + (x + 0.5f) * opt.cell, grid.origin.y + (y + 0.5f) * opt.cell,
             grid.origin.z + (z + 0.5f) * opt.cell};
      s.cells.push_back(ci);
      samples.push_back(std::move(s));
    }
  }
  const size_t nu = lev.units.size();
  const size_t words = (nu + 63) / 64;
  if (!opt.debug_point.empty()) {
    SampleWork w(geom, lev, prm);
    std::vector<u64> bits;
    u64 tests = 0;
    const V3 p{opt.debug_point[0], opt.debug_point[1], opt.debug_point[2]};
    w.run(p, &bits, &tests);
    u64 vis = 0, vt = 0;
    for (size_t u = 0; u < nu; u++) {
      if (bits[u >> 6] >> (u & 63) & 1) {
        vis++;
        vt += lev.units[u].tri_count;
      }
    }
    fmt::print("debug point ({}, {}, {}): {} of {} units, {} of {} triangles may be seen; {} "
               "windows too wide\n",
               p.x, p.y, p.z, vis, nu, vt, lev.total_tris, w.windows_too_wide);
    w.dump(out_path);
    // the 9 samples of the cell holding the point (corners on the grid of opt.cell)
    {
      const float c = opt.cell;
      const V3 o{std::floor(p.x / c) * c, std::floor(p.y / c) * c, std::floor(p.z / c) * c};
      std::vector<u64> uni(bits.size(), 0);
      for (int i = 0; i < 9; i++) {
        const V3 q = i < 8 ? V3{o.x + (i & 1) * c, o.y + ((i >> 1) & 1) * c, o.z + (i >> 2) * c}
                           : V3{o.x + c / 2, o.y + c / 2, o.z + c / 2};
        w.run(q, &bits, &tests);
        u64 t = 0;
        for (size_t u = 0; u < nu; u++) {
          if (bits[u >> 6] >> (u & 63) & 1) {
            t += lev.units[u].tri_count;
          }
        }
        for (size_t k = 0; k < bits.size(); k++) {
          uni[k] |= bits[k];
        }
        fmt::print("  sample ({:.1f}, {:.1f}, {:.1f}): {:.1f}% of the triangles may be seen\n", q.x,
                   q.y, q.z, 100.0 * t / lev.total_tris);
      }
      u64 t = 0;
      for (size_t u = 0; u < nu; u++) {
        if (uni[u >> 6] >> (u & 63) & 1) {
          t += lev.units[u].tri_count;
        }
      }
      fmt::print("  the cell: {:.1f}%\n", 100.0 * t / lev.total_tris);
    }
    return true;
  }
  fmt::print("{}: {} units ({} triangles), occluders {} triangles ({} clusters), {} cells of {} m ({}x{}x{} "
             "grid), {} samples, eps {:.2f} m, layers from {:.1f} m, cube {}+2x{} px\n",
             lev.name, nu, lev.total_tris, lev.occ[0].size(), lev.occ_clusters[0].size(), cells.size(),
             opt.cell, grid.nx, grid.ny, grid.nz, samples.size(), prm.eps, prm.cuts[0], geom.N,
             geom.G);
  fflush(stdout);
  std::vector<std::atomic<u64>> cell_bits(cells.size() * words);
  for (auto& b : cell_bits) {
    b.store(0, std::memory_order_relaxed);
  }
  std::atomic<size_t> next{0};
  std::atomic<u64> tri_tests{0};
  const int nthreads =
      opt.threads > 0 ? opt.threads : std::max(1u, std::thread::hardware_concurrency());
  std::vector<std::thread> pool;
  std::mutex print_lock;
  double t_render_all = 0, t_occ_all = 0, t_units_all = 0;
  u64 n_seeds = 0, n_splat = 0, n_drawn = 0, n_hidden = 0;
  if (opt.max_samples > 0 && samples.size() > (size_t)opt.max_samples) {
    // (testing) every k-th sample only
    std::vector<Sample> keep;
    const size_t k = samples.size() / opt.max_samples;
    for (size_t i = 0; i < samples.size(); i += k) {
      keep.push_back(samples[i]);
    }
    samples = std::move(keep);
  }
  for (int t = 0; t < nthreads; t++) {
    pool.emplace_back([&]() {
      SampleWork w(geom, lev, prm);
      std::vector<u64> bits;
      u64 tests = 0;
      for (;;) {
        const size_t i = next.fetch_add(1);
        if (i >= samples.size()) {
          break;
        }
        w.run(samples[i].p, &bits, &tests);
        for (u32 ci : samples[i].cells) {
          std::atomic<u64>* dst = &cell_bits[(size_t)ci * words];
          for (size_t k = 0; k < words; k++) {
            if (bits[k]) {
              dst[k].fetch_or(bits[k], std::memory_order_relaxed);
            }
          }
        }
        if (opt.verbose && i % 2000 == 0) {
          std::lock_guard<std::mutex> l(print_lock);
          fmt::print("  {} / {}\n", i, samples.size());
          fflush(stdout);
        }
      }
      tri_tests += tests;
      std::lock_guard<std::mutex> l(print_lock);
      t_render_all += w.t_render;
      t_occ_all += w.t_occ;
      t_units_all += w.t_units;
      n_seeds += w.windows_too_wide;
      n_drawn += w.clusters_drawn;
      n_hidden += w.clusters_hidden;
    });
  }
  for (auto& t : pool) {
    t.join();
  }
  const double sample_s =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  fmt::print("{}: per sample: render {:.2f} ms, occlusion maps {:.2f} ms, units {:.2f} ms; {:.0f} "
             "wide windows, {:.0f} clusters drawn, {:.0f} skipped\n",
             lev.name, 1e3 * t_render_all / samples.size(), 1e3 * t_occ_all / samples.size(),
             1e3 * t_units_all / samples.size(), (double)n_seeds / samples.size(),
             (double)n_drawn / samples.size(),
             (double)n_hidden / samples.size());

  // ---- merge cells into sets ----
  std::vector<u32> tri_w(nu);
  for (size_t u = 0; u < nu; u++) {
    tri_w[u] = lev.units[u].tri_count;
  }
  auto weight = [&](const u64* a) {
    u64 s = 0;
    for (size_t k = 0; k < words; k++) {
      u64 v = a[k];
      while (v) {
        const int b = __builtin_ctzll(v);
        s += tri_w[k * 64 + b];
        v &= v - 1;
      }
    }
    return s;
  };
  auto weight_diff = [&](const u64* a, const u64* b) {  // weight(b & ~a)
    u64 s = 0;
    for (size_t k = 0; k < words; k++) {
      u64 v = b[k] & ~a[k];
      while (v) {
        const int bit = __builtin_ctzll(v);
        s += tri_w[k * 64 + bit];
        v &= v - 1;
      }
    }
    return s;
  };
  struct Cluster {
    std::vector<u64> bits;
    u64 n = 0;  // cells
    u64 w = 0;  // weight of bits
    bool alive = true;
    u32 version = 0;
    std::vector<u32> nb;
  };
  std::vector<Cluster> cl;
  std::vector<u32> cell_cluster(cells.size());
  {
    // identical sets share a cluster
    std::unordered_map<u64, std::vector<u32>> by_hash;
    for (u32 ci = 0; ci < cells.size(); ci++) {
      const u64* b = (const u64*)&cell_bits[(size_t)ci * words];
      u64 h = 1469598103934665603ull;
      for (size_t k = 0; k < words; k++) {
        h = (h ^ b[k]) * 1099511628211ull;
      }
      auto& lst = by_hash[h];
      u32 found = UINT32_MAX;
      for (u32 c : lst) {
        if (!memcmp(cl[c].bits.data(), b, words * 8)) {
          found = c;
          break;
        }
      }
      if (found == UINT32_MAX) {
        found = (u32)cl.size();
        Cluster c;
        c.bits.assign(b, b + words);
        c.w = weight(b);
        cl.push_back(std::move(c));
        lst.push_back(found);
      }
      cl[found].n++;
      cell_cluster[ci] = found;
    }
  }
  const size_t unique_sets = cl.size();
  // statistics before merging: the visible fraction (triangle weighted), averaged over cells
  double vis_frac_cells = 0;
  for (u32 ci = 0; ci < cells.size(); ci++) {
    vis_frac_cells += (double)cl[cell_cluster[ci]].w / std::max<u64>(1, lev.total_tris);
  }
  vis_frac_cells /= std::max<size_t>(1, cells.size());
  // neighbours
  {
    std::vector<std::vector<u32>> nbs(cl.size());
    auto link = [&](u32 a, u32 b) {
      if (a != b) {
        nbs[a].push_back(b);
        nbs[b].push_back(a);
      }
    };
    for (u32 ci = 0; ci < cells.size(); ci++) {
      const u32 k = cells[ci];
      const int x = k % grid.nx, y = (k / grid.nx) % grid.ny, z = k / grid.nx / grid.ny;
      const int dx[3] = {1, 0, 0}, dy[3] = {0, 1, 0}, dz[3] = {0, 0, 1};
      for (int d = 0; d < 3; d++) {
        const int x2 = x + dx[d], y2 = y + dy[d], z2 = z + dz[d];
        if (x2 >= grid.nx || y2 >= grid.ny || z2 >= grid.nz) {
          continue;
        }
        auto it = cell_index.find(grid.key(x2, y2, z2));
        if (it != cell_index.end()) {
          link(cell_cluster[ci], cell_cluster[it->second]);
        }
      }
    }
    for (u32 c = 0; c < cl.size(); c++) {
      auto& v = nbs[c];
      std::sort(v.begin(), v.end());
      v.erase(std::unique(v.begin(), v.end()), v.end());
      cl[c].nb = std::move(v);
    }
  }
  const size_t set_bytes = ((nu + 31) / 32) * 4;
  const size_t max_sets =
      std::max<size_t>(1, std::min<size_t>(65535, (size_t)(opt.budget_kb * 1024) / set_bytes));
  size_t alive = cl.size();
  {
    struct Cand {
      double cost;
      u32 a, b, va, vb;
      bool operator<(const Cand& o) const { return cost > o.cost; }
    };
    std::priority_queue<Cand> pq;
    auto cost = [&](u32 a, u32 b) {
      return (double)cl[a].n * weight_diff(cl[a].bits.data(), cl[b].bits.data()) +
             (double)cl[b].n * weight_diff(cl[b].bits.data(), cl[a].bits.data());
    };
    if (alive > max_sets) {
      for (u32 a = 0; a < cl.size(); a++) {
        for (u32 b : cl[a].nb) {
          if (a < b) {
            pq.push({cost(a, b), a, b, 0, 0});
          }
        }
      }
    }
    std::vector<u32> forward(cl.size());
    for (u32 c = 0; c < cl.size(); c++) {
      forward[c] = c;
    }
    while (alive > max_sets && !pq.empty()) {
      const Cand c = pq.top();
      pq.pop();
      if (!cl[c.a].alive || !cl[c.b].alive || cl[c.a].version != c.va || cl[c.b].version != c.vb) {
        continue;
      }
      // merge b into a
      Cluster& A = cl[c.a];
      Cluster& B = cl[c.b];
      for (size_t k = 0; k < words; k++) {
        A.bits[k] |= B.bits[k];
      }
      A.n += B.n;
      A.w = weight(A.bits.data());
      A.version++;
      B.alive = false;
      B.bits.clear();
      B.bits.shrink_to_fit();
      forward[c.b] = c.a;
      std::vector<u32> nb;
      for (u32 x : A.nb) {
        if (x != c.b && cl[x].alive) {
          nb.push_back(x);
        }
      }
      for (u32 x : B.nb) {
        if (x != c.a && cl[x].alive) {
          nb.push_back(x);
          auto& xn = cl[x].nb;
          std::replace(xn.begin(), xn.end(), c.b, c.a);
        }
      }
      std::sort(nb.begin(), nb.end());
      nb.erase(std::unique(nb.begin(), nb.end()), nb.end());
      A.nb = nb;
      for (u32 x : A.nb) {
        auto& xn = cl[x].nb;
        if (std::find(xn.begin(), xn.end(), c.a) == xn.end()) {
          xn.push_back(c.a);
        }
        pq.push({cost(c.a, x), std::min(c.a, x), std::max(c.a, x),
                 c.a < x ? A.version : cl[x].version, c.a < x ? cl[x].version : A.version});
      }
      alive--;
    }
    // clusters not connected to others (islands) beyond the budget: merge the cheapest pairs
    // regardless of adjacency (rare)
    while (alive > max_sets) {
      u32 a = UINT32_MAX, b = UINT32_MAX;
      double best = 1e300;
      std::vector<u32> live;
      for (u32 x = 0; x < cl.size(); x++) {
        if (cl[x].alive) {
          live.push_back(x);
        }
      }
      for (size_t i = 0; i < live.size() && i < 256; i++) {
        for (size_t j = i + 1; j < live.size(); j++) {
          const double c = cost(live[i], live[j]);
          if (c < best) {
            best = c;
            a = live[i];
            b = live[j];
          }
        }
      }
      for (size_t k = 0; k < words; k++) {
        cl[a].bits[k] |= cl[b].bits[k];
      }
      cl[a].n += cl[b].n;
      cl[a].w = weight(cl[a].bits.data());
      cl[b].alive = false;
      forward[b] = a;
      alive--;
    }
    for (u32 ci = 0; ci < cells.size(); ci++) {
      u32 c = cell_cluster[ci];
      while (forward[c] != c) {
        c = forward[c];
      }
      cell_cluster[ci] = c;
    }
  }
  // set numbers
  std::vector<int> set_of(cl.size(), -1);
  std::vector<u32> set_cluster;
  for (u32 c = 0; c < cl.size(); c++) {
    if (cl[c].alive) {
      set_of[c] = (int)set_cluster.size();
      set_cluster.push_back(c);
    }
  }
  double vis_frac_final = 0;
  for (u32 ci = 0; ci < cells.size(); ci++) {
    vis_frac_final += (double)cl[cell_cluster[ci]].w / std::max<u64>(1, lev.total_tris);
  }
  vis_frac_final /= std::max<size_t>(1, cells.size());
  // runs of cells along x (cells are sorted by key)
  std::vector<c3v::CellRun> runs;
  for (u32 ci = 0; ci < cells.size(); ci++) {
    const u16 s = (u16)set_of[cell_cluster[ci]];
    if (!runs.empty()) {
      auto& r = runs.back();
      if (r.set == s && r.key + r.count == cells[ci] && r.count < 0xffff &&
          (r.key % grid.nx) + r.count < (u32)grid.nx) {
        r.count++;
        continue;
      }
    }
    runs.push_back({cells[ci], 1, s});
  }

  // ---- write ----
  c3v::Header h{};
  memcpy(h.magic, c3v::kMagic, 4);
  h.version = c3v::kVersion;
  h.level_hash = lev.level_hash;
  h.num_chunks = lev.covered;
  h.num_units = (u32)nu;
  h.num_sets = (u32)set_cluster.size();
  h.set_words = (u32)((nu + 31) / 32);
  h.num_cell_runs = (u32)runs.size();
  h.origin[0] = grid.origin.x * kM;
  h.origin[1] = grid.origin.y * kM;
  h.origin[2] = grid.origin.z * kM;
  h.cell = opt.cell * kM;
  h.nx = grid.nx;
  h.ny = grid.ny;
  h.nz = grid.nz;
  h.occluder_range = opt.occ_range * kM;
  h.eps = prm.eps * kM;
  h.lod_near = opt.lod_near * kM;
  h.lod_far = opt.lod_far * kM;
  std::vector<u8> file(sizeof(h));
  memcpy(file.data(), &h, sizeof(h));
  auto append = [&](const void* p, size_t n) {
    const size_t o = file.size();
    file.resize(o + n);
    memcpy(file.data() + o, p, n);
  };
  append(runs.data(), runs.size() * sizeof(c3v::CellRun));
  for (u32 c : set_cluster) {
    append(cl[c].bits.data(), h.set_words * 4);  // (little endian: u64 words = u32 words)
  }
  {
    FILE* fp = fopen(out_path.c_str(), "wb");
    if (!fp || fwrite(file.data(), 1, file.size(), fp) != file.size()) {
      fmt::print("{}: can't write {}\n", lev.name, out_path);
      if (fp) {
        fclose(fp);
      }
      return false;
    }
    fclose(fp);
  }
  const double total_s =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  fmt::print("{}: {} unique sets -> {} sets of {} B, {} cell runs: {:.1f} KB; visible from a cell "
             "{:.1f}% of the triangles (unmerged {:.1f}%); {:.1f} M triangle tests, {:.0f} s "
             "sampling, {:.0f} s total -> {}\n",
             lev.name, unique_sets, set_cluster.size(), h.set_words * 4, runs.size(),
             file.size() / 1024.0, 100 * vis_frac_final, 100 * vis_frac_cells,
             tri_tests.load() / 1e6, sample_s, total_s, out_path);
  fflush(stdout);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  std::string in, out;
  bool all = false;
  CLI::App app{"ctr_vis_builder: finer visibility (.c3v) for a .c3l level (AI-assisted)"};
  app.add_option("input", in, "the .c3l file (or the folder with --all)")->required();
  app.add_option("output", out, "the .c3v file (default: next to the .c3l)");
  app.add_flag("--all", all, "every .c3l in the folder (each .c3v next to its .c3l)");
  app.add_option("--cell", opt.cell, "cell size (meters)");
  app.add_option("--face", opt.face, "cube face size (pixels)");
  app.add_option("--guard", opt.guard, "guard band around a face (pixels)");
  app.add_option("--lod-near", opt.lod_near, "detailed tfrag occludes only nearer than (m)");
  app.add_option("--lod-far", opt.lod_far, "coarse tfrag occludes only further than (m)");
  app.add_option("--above", opt.above, "cells up to this far above a floor (meters)");
  app.add_option("--below", opt.below, "cells down to this far below a floor (meters)");
  app.add_option("--spread", opt.spread, "cells sideways around floors");
  app.add_option("--occ-range", opt.occ_range, "occluders only this close (meters)");
  app.add_option("--budget-kb", opt.budget_kb, "the sets' size (KB)");
  app.add_option("--threads", opt.threads, "threads (0: all cores)");
  app.add_flag("-v,--verbose", opt.verbose, "progress");
  app.add_option("--max-samples", opt.max_samples, "(testing) only about this many samples");
  app.add_option("--camera", opt.camera_files,
                 "camera position files (lines: level x y z, meters): cells only around them");
  app.add_option("--cam-radius", opt.cam_radius, "cells this far sideways of a camera position (m)");
  app.add_option("--cam-up", opt.cam_up, "... above it (m)");
  app.add_option("--cam-down", opt.cam_down, "... below it (m)");
  app.add_option("--debug-point", opt.debug_point, "(testing) x y z: one sample, PGM pictures")
      ->expected(3);
  CLI11_PARSE(app, argc, argv);
  if (opt.face < 8 || (opt.face & (opt.face - 1))) {
    fmt::print("--face: a power of two >= 8\n");
    return 1;
  }
  std::vector<std::pair<std::string, std::string>> jobs;
  if (all) {
    for (auto& e : fs::directory_iterator(in)) {
      const auto p = e.path();
      if (p.extension() == ".c3l") {
        auto o = p;
        o.replace_extension(".c3v");
        jobs.push_back({p.string(), o.string()});
      }
    }
    std::sort(jobs.begin(), jobs.end());
  } else {
    if (out.empty()) {
      fs::path o(in);
      o.replace_extension(".c3v");
      out = o.string();
    }
    jobs.push_back({in, out});
  }
  bool ok = true;
  for (auto& [i, o] : jobs) {
    ok = build(i, o, opt) && ok;
  }
  return ok ? 0 : 1;
}
