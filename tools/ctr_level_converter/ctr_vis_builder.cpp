/*!
 * @file ctr_vis_builder.cpp
 * (AI-assisted)
 * ctr_vis_builder: a level's finer visibility (.c3v sidecar, game/graphics/ctr/c3v_format.h) from
 * its finished .c3l. See docs/3ds-port/c3l_format.md, "Fine visibility (.c3v)".
 *
 *   ctr_vis_builder <in.c3l> [<out.c3v>] [options]       (default out: next to the .c3l)
 *   ctr_vis_builder --all <c3l dir> [--out-dir D] [...]  (every level in the folder)
 *
 * Method (conservative, no GPU):
 *  - Cells: a grid of --cell meters (2) where the camera can be: within --fine-above meters over
 *    (or --below under) a floor of the level, --fine-spread meters sideways; further up (to
 *    --above) and out (--coarse-spread) cells of twice the size. Floors: the level's collision
 *    (ground the game stands on, from its .fr3: --fr3, found next to the c3l folder by default),
 *    or (--draw-floors, or no .fr3) roughly horizontal tfrag / tie triangles. The camera anywhere
 *    else: no set (nothing extra hidden).
 *  - Samples: the cells' corners and centers (a body-centered lattice): every point of a cell is
 *    within eps = 0.56 cell of one of its 9 samples (--center-only: the center, eps 0.87 cell).
 *  - From each sample, the level's opaque static geometry (tfrag, ties, shrubs: draws the runtime
 *    draws opaque, depth tested and written, with no alpha test that can fail, and only where the
 *    runtime draws them for certain from every point within eps: their distance limits, LOD tiers
 *    and tie LOD versions; closed tie surfaces only from their front side) is rasterized into a
 *    cube of depth maps (--face pixels per side), in four layers (the nearest occluder beyond 1.5,
 *    4, 8, 16 eps). Merc (doors, platforms, anything that moves), blended and alpha tested
 *    geometry never hides anything.
 *  - Moving the camera by up to eps from the sample: a pixel hides what's further than the largest
 *    depth within asin(eps / dmin) of it (dmin: the nearest depth there, iterated), +2% + 0.25 m;
 *    a window with an empty direction hides nothing. The windows' pixel rectangles come from a
 *    table (per face, 2x2 block and radius, the radius rounded up to 64 steps).
 *  - A unit (a visibility run, or a 96 triangle piece) is hidden from a sample when every pixel its
 *    bounding box (then, if needed, each of its triangles' boxes) covers on the cube hides it. The
 *    cell's set: the units seen from any of its samples.
 *  - Blocks of --block cells: each block's distinct sets as a tree (each set the positions where it
 *    differs from its parent, the root from nothing; a spanning tree of the fewest differences),
 *    sets merged (union: conservative) only when a block would be over --block-kb.
 */

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "common/custom_data/Tfrag3Data.h"
#include "common/dma/gs.h"
#include "common/util/FileUtil.h"
#include "common/util/Serializer.h"
#include "common/util/compress.h"

#include "fmt/format.h"
#include "game/graphics/ctr/c3l_format.h"
#include "game/graphics/ctr/c3l_reader.h"
#include "game/graphics/ctr/c3v_format.h"
#include "third-party/CLI11.hpp"

namespace {

struct Options {
  // cells (meters)
  float cell = 2.f;
  float fine_above = 10.f, fine_spread = 6.f;
  float above = 20.f, coarse_spread = 10.f;
  float below = 2.f;
  bool coarse = true;
  bool center_only = false;  // one sample per cell (its center) instead of 9
  float eps_scale = 0.f;     // eps / cell (0: 0.56 with 9 samples, 0.866 with the center alone)
  std::string fr3;  // a .fr3 file or a folder of them ("": next to the c3l folder, ../fr3)
  bool draw_floors = false;
  // occlusion
  int face = 64;
  float occ_range = 250.f;
  float lod_lo = 180.f, lod_hi = 220.f;  // the lod_distance range the sets are for (m)
  float detail_min = 1.f, tie_scale_min = 1.f, shrub_scale_min = 0.8f;
  float tie_occ_max = 120.f;  // m
  float tie_k_lo = 58.f, tie_k_hi = 70.f;  // (gameplay: detail 1 x focal 256 / tie_lod_px 4 = 64)
  bool ties = true, shrubs = true, closed = true;
  float occ_min_area = 0.f;  // (testing) occluder triangles of at least this area (m^2)
  float depth_rel = 0.02f, depth_abs = 0.25f;
  std::vector<float> layers{1.5f, 4.f, 8.f, 16.f};  // the layers' cuts (x eps)
  // file
  int block = 8;
  float block_kb = 32.f;
  // units seen from at least this share of a block's cells are seen from all of them (0: off): the
  // sets of neighbouring cells then differ only in the rarely seen units (smaller files)
  float smooth = 0.f;
  // units of fewer triangles than this seen from any of a block's cells: from all of them
  int smooth_small = 0;
  // run
  int threads = 0;
  bool verbose = false;
  int max_samples = 0;
  bool count_only = false;
  std::string save_bits, load_bits;  // (testing) the sampled cell sets to / from a file
  std::string cam_stats;  // (testing) camera positions (level x y z, meters): in which cells
  std::vector<float> debug_point;
  std::vector<int> debug_units;  // (testing) with --debug-point: these units' visibility
};

constexpr float kM = 4096.f;  // game units per meter
constexpr float kHoldInView = 1.5f;  // CtrLevel.cpp: a tfrag cell in view keeps its detailed tier

struct V3 {
  float x, y, z;
};
inline V3 sub(V3 a, V3 b) {
  return {a.x - b.x, a.y - b.y, a.z - b.z};
}
inline V3 add(V3 a, V3 b) {
  return {a.x + b.x, a.y + b.y, a.z + b.z};
}
inline V3 mul(V3 a, float s) {
  return {a.x * s, a.y * s, a.z * s};
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
inline float comp(const V3& v, int a) {
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
  void add(const Box& b) {
    add(b.lo);
    add(b.hi);
  }
  bool empty() const { return lo.x > hi.x; }
};
inline float box_dist(const Box& b, V3 p) {
  const float dx = std::max({b.lo.x - p.x, 0.f, p.x - b.hi.x});
  const float dy = std::max({b.lo.y - p.y, 0.f, p.y - b.hi.y});
  const float dz = std::max({b.lo.z - p.z, 0.f, p.z - b.hi.z});
  return std::sqrt(dx * dx + dy * dy + dz * dz);
}

/*!
 * Up to 32 occluder triangles near each other, of one chunk, drawn under the same conditions. The
 * runtime draws them for certain from a camera c when |c - in_c[i]| <= in_r[i] for each i < n_in
 * and |c - out_c| >= out_r (out_r 0: no such condition); closed: only their front faces (the
 * runtime may cull the back faces).
 */
struct OccCluster {
  Box box;
  u32 first, count;
  V3 in_c[3];
  float in_r[3];
  int n_in = 0;
  V3 out_c{0, 0, 0};
  float out_r = 0;
  bool closed = false;
  u8 kind = 0;  // chunk kind (statistics)
};

struct Unit {
  u32 first_tri, tri_count;
  Box box;
  u32 first_group = 0, group_count = 0;  // groups of up to kGroupTris triangles (in index order)
};
constexpr u32 kGroupTris = 16;
struct TriGroup {
  u32 first_tri, tri_count;
  Box box;
};

struct LevelGeo {
  std::string name;
  u32 version = 0;
  u32 level_hash = 0;
  u32 covered = 0;
  std::vector<Unit> units;
  std::vector<u32> chunk_unit_first;  // per covered chunk, and the end
  std::vector<Box> chunk_box;         // the union of the chunk's units' boxes
  std::vector<Tri> unit_tris;
  std::vector<Box> unit_tri_boxes;
  std::vector<TriGroup> groups;
  std::vector<Tri> occ;
  std::vector<OccCluster> clusters;
  std::vector<Tri> draw_floors;
  u64 total_tris = 0;
  u64 kind_tris[4] = {0, 0, 0, 0};    // unit triangles per chunk kind
  u64 occ_kind_tris[4] = {0, 0, 0, 0};
  u32 occ_flags = 0;  // c3v::kOcc*
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

bool load_level(const std::string& path, const Options& opt, LevelGeo* out, std::string* err) {
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
  out->version = hdr.version;
  out->name = std::string(hdr.level_name, strnlen(hdr.level_name, sizeof(hdr.level_name)));
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
  u32 runs_lo = UINT32_MAX;
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
    runs_lo = lo;
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
  if (opt.verbose) {
    u32 kinds[6] = {0, 0, 0, 0, 0, 0}, after = 0;
    for (size_t ci = 0; ci < chunks.size(); ci++) {
      kinds[std::min<u32>(chunks[ci].kind, 5)]++;
      after += ci >= out->covered && !c3v::is_vista(chunks[ci]);
    }
    fmt::print("{}: c3l v{}: {} chunks (tfrag {}, tie {}, shrub {}, vista {}+{}), {} covered, {} "
               "after the vista chunks\n",
               out->name, hdr.version, chunks.size(), kinds[1], kinds[2], kinds[3], kinds[4],
               kinds[5], out->covered, after);
  }
  out->level_hash = c3v::fine_vis_level_hash(chunks.data(), out->covered, draws.data(),
                                             (u32)draws.size(), runs.data(),
                                             chunk_first_run.data());

  // c3l v17: tie levels of detail (each file run's version and group), as load_tie_lod reads them
  std::vector<c3l::TieLodGroup> lod_groups;
  std::vector<c3l::RunLod> run_lod;  // per run of `runs`
  std::vector<float> lod_error;
  if (hdr.version >= c3l::kTieLodVersion && hdr.ext_offset && !runs.empty()) {
    c3l::ExtHeader eh{};
    std::vector<c3l::RunLod> all;
    if (f.read(hdr.ext_offset, &eh, sizeof(eh)) && !memcmp(eh.magic, c3l::kExtMagic, 4) &&
        eh.lod_group_count && eh.run_lod_count && runs_lo >= eh.runs_offset &&
        (runs_lo - eh.runs_offset) % sizeof(c3l::VisRun) == 0 &&
        (runs_lo - eh.runs_offset) / sizeof(c3l::VisRun) + runs.size() <= eh.run_lod_count &&
        f.read_array(eh.lod_groups_offset, eh.lod_group_count, &lod_groups) &&
        f.read_array(eh.run_lod_offset, eh.run_lod_count, &all)) {
      const size_t first = (runs_lo - eh.runs_offset) / sizeof(c3l::VisRun);
      run_lod.assign(all.begin() + first, all.begin() + first + runs.size());
      if (eh.lod_error_offset &&
          !f.read_array(eh.lod_error_offset, eh.lod_group_count, &lod_error)) {
        lod_error.clear();
      }
    } else {
      lod_groups.clear();
    }
  }
  const bool lod_by_error = !lod_groups.empty() && lod_error.size() == lod_groups.size();
  // a file with tie levels of detail that this reader couldn't take (the runtime then draws one
  // version of each group, which one isn't known here): no tie occludes
  bool ties_ok = true;
  if (hdr.version >= c3l::kTieLodVersion && hdr.ext_offset && run_lod.empty()) {
    c3l::ExtHeader eh{};
    if (!f.read(hdr.ext_offset, &eh, sizeof(eh)) || memcmp(eh.magic, c3l::kExtMagic, 4) ||
        eh.lod_group_count) {
      ties_ok = false;
      fmt::print("{}: tie levels of detail not read: no tie occludes\n", out->name);
    }
  }
  const bool tie_lod_occ = lod_by_error && opt.tie_k_lo > 0 && opt.tie_k_hi >= opt.tie_k_lo;

  // textures whose alpha never fails the runtime's alpha test (first mip level; the others are
  // averages of it)
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
  // an occluder draw: drawn opaque, depth tested (GEQUAL) and written, every pixel of it: no
  // blending, an alpha test (GEQUAL aref) that the texture's alpha (>= 2 aref, the GPU's scale)
  // and vertex alpha (>= 0x80) always pass
  auto opaque = [&](const c3l::Draw& d, const c3l::Chunk& ch) {
    DrawMode mode;
    mode.as_int() = d.mode;
    if (mode.get_ab_enable() || !mode.get_depth_write_enable() || !mode.get_zt_enable() ||
        mode.get_depth_test() != GsTest::ZTest::GEQUAL) {
      return false;
    }
    if (mode.get_at_enable() && mode.get_alpha_test() != DrawMode::AlphaTest::ALWAYS) {
      if (mode.get_alpha_test() != DrawMode::AlphaTest::GEQUAL ||
          min_alpha(d.texture) < std::min(255, 2 * (int)mode.get_aref())) {
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

  // occluder triangles of one class (chunk, conditions) into clusters of 32 near each other
  // (Morton order of their centers, 0.5 m)
  auto add_clusters = [&](std::vector<Tri>& tv, const OccCluster& proto, bool impostor_limit) {
    if (tv.empty()) {
      return;
    }
    Box gb;
    for (const auto& t : tv) {
      for (auto& p : t.v) {
        gb.add(p);
      }
    }
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
    std::vector<std::pair<u64, u32>> order(tv.size());
    for (u32 i = 0; i < tv.size(); i++) {
      order[i] = {morton(tv[i]), i};
    }
    std::sort(order.begin(), order.end());
    for (u32 i = 0; i < order.size(); i += 32) {
      OccCluster c = proto;
      c.first = (u32)out->occ.size();
      c.count = std::min<u32>(32, (u32)order.size() - i);
      for (u32 j = i; j < i + c.count; j++) {
        const Tri& t = tv[order[j].second];
        out->occ.push_back(t);
        for (auto& p : t.v) {
          c.box.add(p);
        }
      }
      if (impostor_limit) {
        // impostors: a tie group whose nearest point is beyond impostor_distance may be a card:
        // occludes only while all of the cluster is nearer than tie_occ_max
        const V3 ctr = mul(add(c.box.lo, c.box.hi), 0.5f);
        c.in_c[c.n_in] = ctr;
        c.in_r[c.n_in] = opt.tie_occ_max - 0.5f * len(sub(c.box.hi, c.box.lo));
        c.n_in++;
      }
      out->occ_kind_tris[c.kind & 3] += c.count;
      out->clusters.push_back(c);
    }
    tv.clear();
  };

  out->chunk_unit_first.assign(out->covered + 1, 0);
  out->chunk_box.assign(out->covered, Box());
  for (u32 ci = 0; ci < out->covered; ci++) {
    const auto& ch = chunks[ci];
    out->chunk_unit_first[ci] = (u32)out->units.size();
    // units
    auto add_unit = [&](u32 first_index, u32 count) {
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
      u.first_group = (u32)out->groups.size();
      for (u32 g = 0; g < u.tri_count; g += kGroupTris) {
        TriGroup tg;
        tg.first_tri = u.first_tri + g;
        tg.tri_count = std::min(kGroupTris, u.tri_count - g);
        for (u32 t = tg.first_tri; t < tg.first_tri + tg.tri_count; t++) {
          tg.box.add(out->unit_tri_boxes[t]);
        }
        out->groups.push_back(tg);
      }
      u.group_count = (u32)out->groups.size() - u.first_group;
      out->total_tris += u.tri_count;
      out->kind_tris[std::min<u32>(ch.kind, 3)] += u.tri_count;
      if (u.tri_count) {
        out->chunk_box[ci].add(u.box);
      }
      out->units.push_back(u);
    };
    if (ch.vis_run_count) {
      std::vector<u32> next(ch.draw_count, 0);
      for (u32 k = 0; k < ch.draw_count; k++) {
        next[k] = draws[ch.first_draw + k].first_index;
      }
      for (u32 r = 0; r < ch.vis_run_count; r++) {
        const auto& run = runs[chunk_first_run[ci] + r];
        add_unit(next[run.draw], run.index_count);
        next[run.draw] += run.index_count;
      }
    } else {
      for (u32 k = 0; k < ch.draw_count && ch.first_draw + k < draws.size(); k++) {
        const auto& d = draws[ch.first_draw + k];
        for (u32 i = 0; i < d.index_count; i += c3v::kRunIndices) {
          add_unit(d.first_index + i, std::min(c3v::kRunIndices, d.index_count - i));
        }
      }
    }

    // ---- occluders ----
    const bool tfrag = ch.kind == c3l::CHUNK_KIND_TFRAG && ch.lod_tier <= 2;
    const bool tie = ch.kind == c3l::CHUNK_KIND_TIE && ch.lod_tier == 0 && opt.ties && ties_ok;
    // shrubs: with the PS2 distances (v14), max_dist x shrub_scale x detail_scale
    const bool shrub = ch.kind == c3l::CHUNK_KIND_SHRUB && ch.lod_tier == 0 && opt.shrubs &&
                       hdr.version >= c3l::kShrubPs2DistVersion;
    if (tfrag || tie || shrub) {
      OccCluster proto;
      proto.kind = (u8)ch.kind;
      const V3 bc{ch.bsphere[0] / kM, ch.bsphere[1] / kM, ch.bsphere[2] / kM};
      const float br = ch.bsphere[3] / kM;
      // the chunk's own distance limit at the smallest factors the sets are for (the runtime:
      // drawn while the camera is within limit x hysteresis (>= 1) + radius of its sphere)
      if (ch.max_dist > 0) {
        const float k = tfrag ? opt.detail_min
                              : (tie ? opt.tie_scale_min * opt.detail_min
                                     : opt.shrub_scale_min * opt.detail_min);
        proto.in_c[proto.n_in] = bc;
        proto.in_r[proto.n_in] = ch.max_dist / kM * k + br;
        proto.n_in++;
      }
      // the tfrag LOD tiers
      const V3 lc{ch.lod_center[0] / kM, ch.lod_center[1] / kM, ch.lod_center[2] / kM};
      if (tfrag && ch.lod_tier == 1) {
        proto.in_c[proto.n_in] = lc;
        proto.in_r[proto.n_in] = 0.9f * opt.lod_lo;
        proto.n_in++;
      } else if (tfrag && ch.lod_tier == 2) {
        proto.out_c = lc;
        proto.out_r = std::max(1.1f, kHoldInView) * opt.lod_hi;
      }
      // triangle lists per class: (closed, tie LOD version). Runs carry the LOD versions.
      std::vector<Tri> open_tris, closed_tris;
      auto add_draw_range = [&](const c3l::Draw& d, u32 first, u32 count) {
        const bool closed = d.kind & c3l::kDrawCullBack;
        if (closed && !opt.closed) {
          return;
        }
        auto& dst = closed ? closed_tris : open_tris;
        for (u32 i = first; i + 3 <= first + count; i += 3) {
          Tri t;
          if (tri_at(ch, i, &t) &&
              (opt.occ_min_area <= 0 ||
               0.5f * len(cross(sub(t.v[1], t.v[0]), sub(t.v[2], t.v[0]))) >= opt.occ_min_area)) {
            dst.push_back(t);
          }
        }
      };
      auto flush = [&](const OccCluster& p) {
        OccCluster a = p;
        a.closed = false;
        add_clusters(open_tris, a, tie);
        a.closed = true;
        add_clusters(closed_tris, a, tie);
      };
      std::vector<u8> draw_ok(ch.draw_count, 0);
      for (u32 k = 0; k < ch.draw_count && ch.first_draw + k < draws.size(); k++) {
        draw_ok[k] = opaque(draws[ch.first_draw + k], ch);
      }
      if (ch.vis_run_count) {
        std::vector<u32> next(ch.draw_count, 0);
        for (u32 k = 0; k < ch.draw_count; k++) {
          next[k] = draws[ch.first_draw + k].first_index;
        }
        // always-drawn runs together; each LOD group's version on its own
        std::vector<std::pair<u32, u32>> lod_runs;  // (run, first index)
        for (u32 r = 0; r < ch.vis_run_count; r++) {
          const u32 ri = chunk_first_run[ci] + r;
          const auto& run = runs[ri];
          const u32 fi = next[run.draw];
          next[run.draw] += run.index_count;
          if (!draw_ok[run.draw]) {
            continue;
          }
          const bool lod = ri < run_lod.size() && run_lod[ri].lod != c3l::kRunLodAlways;
          if (lod) {
            lod_runs.push_back({ri, fi});
          } else {
            add_draw_range(draws[ch.first_draw + run.draw], fi, run.index_count);
          }
        }
        flush(proto);
        if (tie_lod_occ) {
          std::sort(lod_runs.begin(), lod_runs.end(), [&](const auto& a, const auto& b) {
            const auto &la = run_lod[a.first], &lb = run_lod[b.first];
            return la.group != lb.group ? la.group < lb.group : la.lod < lb.lod;
          });
          for (size_t i = 0; i < lod_runs.size();) {
            const auto& rl = run_lod[lod_runs[i].first];
            size_t j = i;
            while (j < lod_runs.size() && run_lod[lod_runs[j].first].group == rl.group &&
                   run_lod[lod_runs[j].first].lod == rl.lod) {
              const auto& run = runs[lod_runs[j].first];
              add_draw_range(draws[ch.first_draw + run.draw], lod_runs[j].second, run.index_count);
              j++;
            }
            if (rl.group < lod_groups.size() && rl.lod <= 2) {
              const auto& g = lod_groups[rl.group];
              const V3 gc{g.bsphere[0] / kM, g.bsphere[1] / kM, g.bsphere[2] / kM};
              const float gr = g.bsphere[3] / kM, sw = lod_error[rl.group] / kM;
              OccCluster p = proto;
              // CtrLevel.cpp: near while |c - sphere| < error x k x hysteresis (1 / 1.1) + r.
              // v19: a near version is in a near chunk the runtime streams in (the far version
              // drawn until it's there): never certain, no occluder
              if (rl.lod == c3l::kRunLodNear && hdr.version >= c3l::kStreamVersion) {
                open_tris.clear();
                closed_tris.clear();
              } else if (rl.lod == c3l::kRunLodNear) {
                out->occ_flags |= c3v::kOccTieNear;
                p.in_c[p.n_in] = gc;
                p.in_r[p.n_in] = sw * opt.tie_k_lo + gr;
                p.n_in++;
              } else {
                p.out_c = gc;
                p.out_r = sw * opt.tie_k_hi * 1.1f + gr;
              }
              flush(p);
            } else {
              open_tris.clear();
              closed_tris.clear();
            }
            i = j;
          }
        }
      } else {
        for (u32 k = 0; k < ch.draw_count && ch.first_draw + k < draws.size(); k++) {
          if (draw_ok[k]) {
            const auto& d = draws[ch.first_draw + k];
            add_draw_range(d, d.first_index, d.index_count);
          }
        }
        flush(proto);
      }
    }

    // floors from the draws (--draw-floors, or no collision): the detailed tiers' tfrag and tie
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
            out->draw_floors.push_back(t);
          }
        }
      }
    }
  }
  out->chunk_unit_first[out->covered] = (u32)out->units.size();
  if (out->occ_kind_tris[c3l::CHUNK_KIND_TIE]) {
    out->occ_flags |= c3v::kOccTie;
  }
  if (out->occ_kind_tris[c3l::CHUNK_KIND_SHRUB]) {
    out->occ_flags |= c3v::kOccShrub;
  }
  for (const auto& c : out->clusters) {
    if (c.closed) {
      out->occ_flags |= c3v::kOccClosed;
      break;
    }
  }
  if (tie_lod_occ) {
    out->occ_flags |= c3v::kOccTieLod;
  }
  return true;
}

/*! The level's collision (ground the game stands on) from its .fr3; false: none there. */
bool load_collision_floors(const std::string& path, std::vector<Tri>* out, std::string* why) {
  if (!fs::exists(path)) {
    *why = "no " + path;
    return false;
  }
  auto data = file_util::read_binary_file(path);
  auto decomp = compression::decompress_zstd(data.data(), data.size());
  if (decomp.size() < 4) {
    *why = "empty .fr3";
    return false;
  }
  u16 version = 0;
  memcpy(&version, decomp.data(), 2);
  if (version != tfrag3::TFRAG3_VERSION) {
    *why = fmt::format(".fr3 version {} (this tool reads {})", version, tfrag3::TFRAG3_VERSION);
    return false;
  }
  Serializer ser(decomp.data(), decomp.size());
  tfrag3::Level level;
  level.serialize(ser);
  const auto& v = level.collision.vertices;
  for (size_t i = 0; i + 3 <= v.size(); i += 3) {
    Tri t;
    for (int k = 0; k < 3; k++) {
      t.v[k] = {v[i + k].x / kM, v[i + k].y / kM, v[i + k].z / kM};
    }
    const V3 n = cross(sub(t.v[1], t.v[0]), sub(t.v[2], t.v[0]));
    const float l = len(n);
    if (l > 0 && std::abs(n.y) > 0.5f * l) {
      out->push_back(t);
    }
  }
  if (out->empty()) {
    *why = "no collision in the .fr3";
    return false;
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
constexpr int kLayers = 4;

struct CubeGeom {
  int N = 64;
  std::vector<float> dist_k;  // per pixel: distance = z * dist_k
  float half_px = 0;          // the largest angle from a pixel's center to its corner
  int levels = 0;             // pyramid levels (N, N/2, ... 1)

  void init(int n) {
    N = n;
    dist_k.resize(N * N);
    for (int y = 0; y < N; y++) {
      for (int x = 0; x < N; x++) {
        const float tx = ((x + 0.5f) - N / 2.f) * (2.f / N), ty = ((y + 0.5f) - N / 2.f) * (2.f / N);
        dist_k[y * N + x] = std::sqrt(1 + tx * tx + ty * ty);
      }
    }
    const float h = 1.f / N;  // half a pixel, tangent units (the center pixel is the largest)
    half_px = std::atan(std::sqrt(2.f) * h) * 1.01f;
    levels = 0;
    for (int m = N; m >= 1; m /= 2) {
      levels++;
    }
  }
};

/*!
 * The windows: per face, 2x2 block of pixels and radius step k, the pixel rectangles (as pyramid
 * cells: each at most 3x3 cells of its level) of all faces that the directions within r_of[k] of
 * the block's center fall in. r_of[0] is the block's own extent (its center to its corners);
 * steps of a factor q up to 1.5 rad (wider: the window hides nothing).
 */
struct CapTable {
  struct Rect {
    u8 face, level, x0, y0, x1, y1;
  };
  int N = 0, NB = 0, K = 64;
  float margin = 0;
  std::vector<float> r_of;
  std::vector<u32> start;
  std::vector<Rect> rects;

  static bool cap_range(float va, float vz, float sin_r, float* lo, float* hi) {
    const float rho = std::sqrt(va * va + vz * vz);
    if (rho <= sin_r) {
      *lo = -kInf;
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

  void init(const CubeGeom& g) {
    N = g.N;
    NB = N / 2;
    margin = 2.1f * g.half_px;  // the block's center to its corners
    r_of.resize(K);
    const float q = std::pow(1.5f / margin, 1.f / (K - 1));
    for (int k = 0; k < K; k++) {
      r_of[k] = margin * std::pow(q, (float)k);
    }
    r_of[K - 1] = 1.5f;
    start.clear();
    rects.clear();
    const float kk = N / 2.f;
    constexpr float kCornerAngle = 0.9553166f;  // a face's core corner from its axis
    for (int f = 0; f < 6; f++) {
      const Face& fc = kFaces[f];
      for (int by = 0; by < NB; by++) {
        for (int bx = 0; bx < NB; bx++) {
          const float tx = (2 * bx + 1 - kk) / kk, ty = (2 * by + 1 - kk) / kk;
          const float l = std::sqrt(1 + tx * tx + ty * ty);
          const V3 v{(fc.f.x + fc.r.x * tx + fc.u.x * ty) / l,
                     (fc.f.y + fc.r.y * tx + fc.u.y * ty) / l,
                     (fc.f.z + fc.r.z * tx + fc.u.z * ty) / l};
          for (int k = 0; k < K; k++) {
            start.push_back((u32)rects.size());
            const float r = r_of[k], sin_r = std::sin(r);
            for (int g2 = 0; g2 < 6; g2++) {
              const Face& gf = kFaces[g2];
              const float vz = comp(v, gf.af) * gf.sf;
              if (std::acos(std::clamp(vz, -1.f, 1.f)) > kCornerAngle + r) {
                continue;
              }
              float x0, x1, y0, y1;
              if (!cap_range(comp(v, gf.ar) * gf.sr, vz, sin_r, &x0, &x1) ||
                  !cap_range(comp(v, gf.au) * gf.su, vz, sin_r, &y0, &y1)) {
                continue;
              }
              if (x0 > 1 || x1 < -1 || y0 > 1 || y1 < -1) {
                continue;
              }
              const int ix0 = std::max(0, (int)std::floor(std::max(x0, -1.f) * kk + kk) - 1);
              const int ix1 = std::min(N - 1, (int)std::floor(std::min(x1, 1.f) * kk + kk) + 1);
              const int iy0 = std::max(0, (int)std::floor(std::max(y0, -1.f) * kk + kk) - 1);
              const int iy1 = std::min(N - 1, (int)std::floor(std::min(y1, 1.f) * kk + kk) + 1);
              int lv = 0;
              while ((ix1 >> lv) - (ix0 >> lv) > 2 || (iy1 >> lv) - (iy0 >> lv) > 2) {
                lv++;
              }
              rects.push_back({(u8)g2, (u8)lv, (u8)(ix0 >> lv), (u8)(iy0 >> lv), (u8)(ix1 >> lv),
                               (u8)(iy1 >> lv)});
            }
          }
        }
      }
    }
    start.push_back((u32)rects.size());
  }
  u32 idx(int f, int b, int k) const { return ((u32)f * NB * NB + b) * K + k; }
};

struct Params {
  float eps, occ_range;
  int nl = kLayers;  // layers used (the others empty)
  float cuts[kLayers];  // layer i: the nearest occluder at least this far
  float depth_rel, depth_abs;
  // the smallest radius step whose window is wide enough for depth d: d >= dk[k] (dk falling)
  std::vector<float> dk;
  int level_for(float d) const {
    // the first k with d >= dk[k]
    int lo = 0, hi = (int)dk.size();
    while (lo < hi) {
      const int m = (lo + hi) / 2;
      if (d >= dk[m]) {
        hi = m;
      } else {
        lo = m + 1;
      }
    }
    return lo < (int)dk.size() ? lo : -1;
  }
  void init(float e, const Options& opt, const CapTable& ct) {
    eps = e;
    occ_range = opt.occ_range;
    nl = (int)std::min<size_t>(kLayers, opt.layers.size());
    for (int l = 0; l < kLayers; l++) {
      cuts[l] = (l < nl ? opt.layers[l] : 1e9f) * eps;
    }
    depth_rel = opt.depth_rel;
    depth_abs = opt.depth_abs;
    // window for depth d: asin(eps / d) + margin <= r_of[k]
    dk.resize(ct.K);
    for (int i = 0; i < ct.K; i++) {
      const float a = ct.r_of[i] - ct.margin;
      dk[i] = a <= 0 ? kInf : (a >= (float)M_PI / 2 ? eps : eps / std::sin(a));
    }
  }
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
  u64 clusters_drawn = 0, clusters_hidden = 0, windows_too_wide = 0, tri_tests = 0;
  double vis_tris = 0;  // triangles that may be seen, summed over the samples

  SampleWork(const CubeGeom& g, const CapTable& ct, const LevelGeo& lev)
      : m_g(g), m_ct(ct), m_lev(lev) {
    m_tiny = 4.f / g.N;  // two pixels (tangent units near a face's center)
    const int NN = g.N * g.N;
    int total = 0;
    for (int n = g.N; n >= 1; n /= 2) {
      m_lv_off.push_back(total);
      total += n * n;
    }
    for (int f = 0; f < kMaps; f++) {
      m_w[f].resize(NN);
      m_pmin[f].resize(total);
      m_pmax[f].resize(total);
    }
    for (int f = 0; f < 6; f++) {
      m_occ[f].resize(NN);
      m_mip[f].resize(g.levels);
      int n = g.N;
      for (auto& l : m_mip[f]) {
        l.resize(n * n);
        n /= 2;
      }
    }
  }

  // the units that may be seen from within p.eps of s (bits set in out)
  void run(V3 s, const Params& p, std::vector<u64>* out) {
    m_p = &p;
    using clk = std::chrono::steady_clock;
    auto ta = clk::now();
    render(s);
    auto tb = clk::now();
    t_render += std::chrono::duration<double>(tb - ta).count();
    ta = tb;
    bool any = false;  // does any core pixel hide anything? (else: everything may be seen)
    for (int f = 0; f < m_p->nl * 6; f++) {
      build_pyramids(f);
    }
    for (int f = 0; f < 6; f++) {
      std::fill(m_occ[f].begin(), m_occ[f].end(), kInf);
      for (int layer = 0; layer < m_p->nl; layer++) {
        make_occ(layer, f);
      }
      build_mips(f);
      any = any || m_mip[f].back()[0] < kInf || has_finite(f);
    }
    tb = clk::now();
    t_occ += std::chrono::duration<double>(tb - ta).count();
    ta = tb;
    const size_t nu = m_lev.units.size();
    out->assign((nu + 63) / 64, 0);
    for (u32 ci = 0; ci < m_lev.covered; ci++) {
      const u32 a = m_lev.chunk_unit_first[ci], b = m_lev.chunk_unit_first[ci + 1];
      if (a == b) {
        continue;
      }
      if (any && !m_lev.chunk_box[ci].empty()) {
        const float dmin = box_dist(m_lev.chunk_box[ci], s);
        if (dmin >= m_p->cuts[0] && !box_visible(s, m_lev.chunk_box[ci], dmin)) {
          continue;  // all of the chunk hidden
        }
      }
      for (u32 u = a; u < b; u++) {
        if (!any || unit_visible(s, m_lev.units[u])) {
          (*out)[u >> 6] |= 1ull << (u & 63);
          vis_tris += m_lev.units[u].tri_count;
        }
      }
    }
    t_units += std::chrono::duration<double>(clk::now() - ta).count();
  }

  // (testing) PGM pictures per face: depth of layers 0 and 1, occluder distance (black: hides
  // nothing)
  void dump(const std::string& prefix) const {
    const int N = m_g.N;
    for (int f = 0; f < 6; f++) {
      FILE* fp = fopen(fmt::format("{}_f{}.pgm", prefix, f).c_str(), "wb");
      if (!fp) {
        continue;
      }
      fmt::print(fp, "P5 {} {} 255\n", 3 * N, N);
      for (int y = N - 1; y >= 0; y--) {
        for (int l = 0; l < 2; l++) {
          for (int x = 0; x < N; x++) {
            const float w = m_w[l * 6 + f][y * N + x];
            const float d = w > 0 ? m_g.dist_k[y * N + x] / w : 0;
            fputc(w > 0 ? (int)std::clamp(255.f - d, 16.f, 255.f) : 0, fp);
          }
        }
        for (int x = 0; x < N; x++) {
          const float v = m_occ[f][y * N + x];
          fputc(v < kInf ? (int)std::clamp(255.f - v, 16.f, 255.f) : 0, fp);
        }
      }
      fclose(fp);
    }
  }

 private:
  const CubeGeom& m_g;
  const CapTable& m_ct;
  const LevelGeo& m_lev;
  const Params* m_p = nullptr;
  std::vector<float> m_w[kMaps];  // 1 / z, 0: nothing
  std::vector<float> m_occ[6];
  std::vector<float> m_pmin[kMaps], m_pmax[kMaps];  // [map]: the levels at m_lv_off[level]
  std::vector<int> m_lv_off;
  std::vector<std::vector<float>> m_mip[6];                      // [face][level] occ, max
  std::vector<std::pair<float, u32>> m_order;
  float m_tiny = 0;  // a cluster this small x its distance: too small to draw

  bool has_finite(int f) const {
    for (float v : m_occ[f]) {
      if (v < kInf) {
        return true;
      }
    }
    return false;
  }

  // does the runtime draw cluster c for certain from every camera within eps of s
  bool drawn_for_certain(const OccCluster& c, V3 s) const {
    const float eps = m_p->eps;
    for (int i = 0; i < c.n_in; i++) {
      if (len(sub(s, c.in_c[i])) + eps > c.in_r[i]) {
        return false;
      }
    }
    if (c.out_r > 0 && len(sub(s, c.out_c)) - eps < c.out_r) {
      return false;
    }
    return true;
  }

  // ---- occluders into the depth maps (front to back; clusters behind what's drawn skipped) ----
  __attribute__((noinline)) void render(V3 s) {
    const int N = m_g.N;
    const float zclip = 0.02f;
    const float cut0 = m_p->cuts[0];
    const float range = m_p->occ_range;
    const float eps = m_p->eps;
    for (int f = 0; f < kMaps; f++) {
      std::fill(m_w[f].begin(), m_w[f].end(), 0.f);
    }
    const auto& tris = m_lev.occ;
    const auto& cls = m_lev.clusters;
    m_order.clear();
    for (u32 c = 0; c < cls.size(); c++) {
      const OccCluster& cl = cls[c];
      const Box& bx = cl.box;
      const float dmin = box_dist(bx, s);
      if (dmin > range) {
        continue;
      }
      const float fx = std::max(std::abs(bx.lo.x - s.x), std::abs(bx.hi.x - s.x));
      const float fy = std::max(std::abs(bx.lo.y - s.y), std::abs(bx.hi.y - s.y));
      const float fz = std::max(std::abs(bx.lo.z - s.z), std::abs(bx.hi.z - s.z));
      if (fx * fx + fy * fy + fz * fz < cut0 * cut0) {
        continue;  // all of it nearer than every layer's cut
      }
      // smaller than two pixels from here: it would cover a pixel or two (occluders left out
      // only ever hide less)
      if (dmin > 0 && len(sub(bx.hi, bx.lo)) < m_tiny * dmin) {
        continue;
      }
      if (!drawn_for_certain(cl, s)) {
        continue;
      }
      m_order.push_back({dmin, c});
    }
    std::sort(m_order.begin(), m_order.end());
    for (const auto& [dmin, c] : m_order) {
      const auto& cl = cls[c];
      const Box& bx = cl.box;
      const V3 lo = sub(bx.lo, s), hi = sub(bx.hi, s);
      const bool check_range =
          len(V3{std::max(-lo.x, hi.x), std::max(-lo.y, hi.y), std::max(-lo.z, hi.z)}) > range;
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
        if (x1 + z1 < 0 || x0 - z1 > 0 || y1 + z1 < 0 || y0 - z1 > 0) {
          continue;  // outside one of the image's side planes
        }
        // behind what the deepest layer has already at every pixel it covers: it loses in every
        // layer (each layer's map is at least as near)
        if (z0 > zclip) {
          const float k = N / 2.f;
          const float px0 = std::min(x0 / z0, x0 / z1) * k + k,
                      px1 = std::max(x1 / z0, x1 / z1) * k + k;
          const float py0 = std::min(y0 / z0, y0 / z1) * k + k,
                      py1 = std::max(y1 / z0, y1 / z1) * k + k;
          const int ix0 = std::max(0, (int)std::floor(px0)),
                    ix1 = std::min(N - 1, (int)std::floor(px1));
          const int iy0 = std::max(0, (int)std::floor(py0)),
                    iy1 = std::min(N - 1, (int)std::floor(py1));
          if (ix0 <= ix1 && iy0 <= iy1 && (ix1 - ix0 + 1) * (iy1 - iy0 + 1) <= 1024) {
            const float wmin = 1.f / z0;
            bool hidden = true;
            const float* W = m_w[(m_p->nl - 1) * 6 + f].data();
            for (int y = iy0; y <= iy1 && hidden; y++) {
              for (int x = ix0; x <= ix1; x++) {
                if (W[y * N + x] <= wmin) {
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
        if (check_range &&
            std::max({dot(d[0], d[0]), dot(d[1], d[1]), dot(d[2], d[2])}) > range * range) {
          continue;  // occluders only within the occluder range (all of the triangle)
        }
        const V3 n = cross(sub(d[1], d[0]), sub(d[2], d[0]));
        const float c = dot(n, d[0]);  // plane: dot(n, x) = c (x relative to the sample)
        const float nl = len(n);
        if (nl == 0 || std::abs(c) < 1e-6f * nl) {
          continue;  // seen edge on
        }
        // closed surfaces (the runtime may cull back faces; front faces counter-clockwise from
        // outside: the sample in front means dot(n, s - v0) = -c > 0): from every point within eps
        if (cl.closed && c > -eps * nl) {
          continue;
        }
        const float dd[3][3] = {{d[0].x, d[0].y, d[0].z},
                                {d[1].x, d[1].y, d[1].z},
                                {d[2].x, d[2].y, d[2].z}};
        const float nn[3] = {n.x, n.y, n.z};
        for (int f = 0; f < 6; f++) {
          if (!(mask & (1u << f))) {
            continue;
          }
          const Face& fc = kFaces[f];
          float z[3], x[3], y[3];
          int behind = 0, out_l = 0, out_r = 0, out_b = 0, out_t = 0;
          for (int k = 0; k < 3; k++) {
            z[k] = dd[k][fc.af] * fc.sf;
            x[k] = dd[k][fc.ar] * fc.sr;
            y[k] = dd[k][fc.au] * fc.su;
            behind += z[k] < zclip;
            out_l += x[k] < -z[k];
            out_r += x[k] > z[k];
            out_b += y[k] < -z[k];
            out_t += y[k] > z[k];
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
          const float k = N / 2.f;
          for (int i = 0; i < np; i++) {
            px[i] = px[i] * k + k;
            py[i] = py[i] * k + k;
          }
          // w = 1 / z on the triangle's plane: (nr x + nu y + nf) / c, x = (px - k) / k
          const float nr = nn[fc.ar] * fc.sr, nu = nn[fc.au] * fc.su, nf = nn[fc.af] * fc.sf;
          const float pa = nr / k / c, pb = nu / k / c, pc = (nf - nr - nu) / c;
          for (int i = 1; i + 1 < np; i++) {
            raster(f, px[0], py[0], px[i], py[i], px[i + 1], py[i + 1], pa, pb, pc);
          }
        }
      }
    }
  }

  __attribute__((noinline)) void raster(int f, float x0, float y0, float x1, float y1, float x2, float y2, float pa,
              float pb, float pc) {
    const int N = m_g.N;
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
    const int ix1 = std::min(N - 1, (int)std::ceil(std::max({x0, x1, x2}) - 0.5f));
    const int iy0 = std::max(0, (int)std::floor(std::min({y0, y1, y2}) - 0.5f));
    const int iy1 = std::min(N - 1, (int)std::ceil(std::max({y0, y1, y2}) - 0.5f));
    if (ix0 > ix1 || iy0 > iy1) {
      return;
    }
    const float e = -1e-4f * area;  // a little tolerance: no cracks on shared edges
    const float* DK = m_g.dist_k.data();
    float* Wl[kLayers];
    for (int l = 0; l < kLayers; l++) {
      Wl[l] = m_w[l * 6 + f].data();
    }
    const float* cuts = m_p->cuts;
    const int nl = m_p->nl;
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
        const int p = iy * N + ix;
        if (w <= Wl[nl - 1][p] || w <= 0) {
          continue;  // (the deepest layer is the furthest: every layer has it nearer)
        }
        const float d = DK[p] / w;
        for (int l = 0; l < nl && d >= cuts[l]; l++) {
          if (w > Wl[l][p]) {
            Wl[l][p] = w;
          }
        }
      }
    }
  }

  void build_pyramids(int mi) {
    const int N = m_g.N;
    const float* W = m_w[mi].data();
    float* mn0 = m_pmin[mi].data();
    float* mx0 = m_pmax[mi].data();
    for (int p = 0; p < N * N; p++) {
      const float d = W[p] > 0 ? m_g.dist_k[p] / W[p] : kInf;  // kInf: nothing
      mn0[p] = d;
      mx0[p] = d;
    }
    int n = N;
    for (size_t l = 1; l < m_lv_off.size(); l++) {
      const int h = n / 2;
      const float* a = m_pmin[mi].data() + m_lv_off[l - 1];
      const float* b = m_pmax[mi].data() + m_lv_off[l - 1];
      float* c = m_pmin[mi].data() + m_lv_off[l];
      float* d = m_pmax[mi].data() + m_lv_off[l];
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

  // the smallest / largest depth of layer `layer` over window k of block b of face f
  float window(int layer, int f, int b, int k, bool want_max) const {
    const u32 i = m_ct.idx(f, b, k);
    float out = want_max ? 0.f : kInf;
    for (u32 r = m_ct.start[i]; r < m_ct.start[i + 1]; r++) {
      const auto& R = m_ct.rects[r];
      const int mi = layer * 6 + R.face;
      const int n = m_g.N >> R.level;
      const float* m = (want_max ? m_pmax[mi].data() : m_pmin[mi].data()) + m_lv_off[R.level];
      for (int y = R.y0; y <= R.y1; y++) {
        for (int x = R.x0; x <= R.x1; x++) {
          const float v = m[y * n + x];
          if (want_max) {
            out = std::max(out, v);
          } else {
            out = std::min(out, v);
          }
        }
      }
      if (want_max && out >= kInf) {
        return out;
      }
    }
    return out;
  }

  // layer `layer`'s occluder distances on face f into m_occ[f] (the smaller one stays), per 2x2
  // block: the window around the block's center grown by the block's extent
  __attribute__((noinline)) void make_occ(int layer, int f) {
    const int N = m_g.N, NB = N / 2;
    const float* mn1 = m_pmin[layer * 6 + f].data() + m_lv_off[1];
    float* O = m_occ[f].data();
    for (int by = 0; by < NB; by++) {
      for (int bx = 0; bx < NB; bx++) {
        const int b = by * NB + bx;
        const float d0 = mn1[b];
        if (d0 >= kInf) {
          continue;  // (an empty pixel: the max is kInf anyway)
        }
        // what the nearer layers hide already: this layer hides only beyond its cut (its depths
        // are at least that), so where the block's pixels hide from nearer than that, nothing to do
        {
          float* o0 = &O[(2 * by) * N + 2 * bx];
          if (std::max({o0[0], o0[1], o0[N], o0[N + 1]}) <= m_p->cuts[layer]) {
            continue;
          }
        }
        int k = m_p->level_for(d0);
        bool ok = k >= 0, done = false;
        for (int it = 0; ok && it < 64; it++) {
          const float dmin = window(layer, f, b, k, false);
          const int k2 = m_p->level_for(dmin);
          if (k2 < 0) {
            ok = false;
          } else if (k2 <= k) {
            done = true;
            break;
          } else {
            k = k2;
          }
        }
        ok = ok && done;  // (a window that kept growing: hides nothing)
        if (!ok) {
          windows_too_wide++;
          continue;
        }
        const float dmax = window(layer, f, b, k, true);
        if (dmax >= kInf) {
          continue;
        }
        const float o = dmax * (1 + m_p->depth_rel) + m_p->depth_abs;
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
    const V3 ns[4] = {sub(fc.f, fc.r), add(fc.f, fc.r), sub(fc.f, fc.u), add(fc.f, fc.u)};
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
    if (dmin < m_p->cuts[0]) {
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

  __attribute__((noinline)) bool unit_visible(V3 s, const Unit& u) {
    if (!u.tri_count) {
      return false;
    }
    const float dmin = box_dist(u.box, s);
    if (dmin < m_p->cuts[0]) {
      return true;
    }
    if (!box_visible(s, u.box, dmin)) {
      return false;
    }
    for (u32 g = u.first_group; g < u.first_group + u.group_count; g++) {
      const TriGroup& tg = m_lev.groups[g];
      if (u.group_count > 1) {
        const float gd = box_dist(tg.box, s);
        if (gd >= m_p->cuts[0] && !box_visible(s, tg.box, gd)) {
          continue;  // all of the group hidden
        }
      }
      for (u32 i = tg.first_tri; i < tg.first_tri + tg.tri_count; i++) {
        tri_tests++;
        if (tri_visible(s, m_lev.unit_tris[i], m_lev.unit_tri_boxes[i])) {
          return true;
        }
      }
    }
    return false;
  }
};

// ---------------------------------------------------------------------------------------------
// cells
// ---------------------------------------------------------------------------------------------

struct Grid {
  V3 origin;  // meters
  float cell;
  int nx, ny, nz;  // cells (multiples of the block)
  u32 key(int x, int y, int z) const { return ((u32)z * ny + y) * nx + x; }
};

// separable dilation of a 3D u8 mask (values 0/1): a seed at y sets y - down .. y + up, then
// +-r cells along x and z
void dilate(std::vector<u8>* m, int nx, int ny, int nz, int up, int down, int r) {
  auto at = [&](int x, int y, int z) { return ((size_t)z * ny + y) * nx + x; };
  std::vector<u8> tmp(m->size(), 0);
  for (int z = 0; z < nz; z++) {
    for (int x = 0; x < nx; x++) {
      for (int y = 0; y < ny; y++) {
        if ((*m)[at(x, y, z)]) {
          for (int k = std::max(0, y - down); k <= std::min(ny - 1, y + up); k++) {
            tmp[at(x, k, z)] = 1;
          }
        }
      }
    }
  }
  // one axis: dst = src dilated by r along it (stride s, n cells, lines over the other two)
  auto pass = [&](const std::vector<u8>& src, std::vector<u8>* dst, bool along_x) {
    const int n = along_x ? nx : nz;
    const int o1 = along_x ? nz : nx;
    for (int a = 0; a < o1; a++) {
      for (int y = 0; y < ny; y++) {
        auto idx = [&](int i) { return along_x ? at(i, y, a) : at(a, y, i); };
        int prev = -1000000;
        for (int i = 0; i < n; i++) {
          if (src[idx(i)]) {
            prev = i;
          }
          (*dst)[idx(i)] = i - prev <= r;
        }
        int nxt = 1000000;
        for (int i = n - 1; i >= 0; i--) {
          if (src[idx(i)]) {
            nxt = i;
          }
          if (nxt - i <= r) {
            (*dst)[idx(i)] = 1;
          }
        }
      }
    }
  };
  pass(tmp, m, true);
  tmp = *m;
  pass(tmp, m, false);
}

// the floor triangles cut until each piece is at most `size` meters on every axis; f(box) for each
void floor_pieces(const Tri& t, float size, const std::function<void(const Box&)>& f, int depth = 0) {
  Box b;
  for (auto& p : t.v) {
    b.add(p);
  }
  const float ex = std::max({b.hi.x - b.lo.x, b.hi.y - b.lo.y, b.hi.z - b.lo.z});
  if (ex <= size || depth > 16) {
    f(b);
    return;
  }
  // split the longest edge
  int e = 0;
  float best = -1;
  for (int k = 0; k < 3; k++) {
    const float l = len(sub(t.v[(k + 1) % 3], t.v[k]));
    if (l > best) {
      best = l;
      e = k;
    }
  }
  const V3 a = t.v[e], bb = t.v[(e + 1) % 3], c = t.v[(e + 2) % 3];
  const V3 m = mul(add(a, bb), 0.5f);
  floor_pieces(Tri{{a, m, c}}, size, f, depth + 1);
  floor_pieces(Tri{{m, bb, c}}, size, f, depth + 1);
}

// ---------------------------------------------------------------------------------------------
// blocks
// ---------------------------------------------------------------------------------------------

void put_varint(std::vector<u8>* o, u32 v) {
  while (v >= 0x80) {
    o->push_back((u8)(v | 0x80));
    v >>= 7;
  }
  o->push_back((u8)v);
}

struct BlockResult {
  std::vector<u8> data;
  double vis_sum = 0;  // over the block's cells: the share of the level's triangles they may see
  u32 cells = 0;
  u32 sets = 0, unique_sets = 0, merged = 0;
  u32 max_depth = 0;
  double depth_sum = 0;  // sum over cells of the decode path length
  u64 diff_positions = 0;
  bool ok = true;
};

/*!
 * One block: cells (slot in the block, set bits) -> its encoding (c3v_format.h). Sets over
 * max_bytes merge (the union, the fewest extra triangles first) until it fits.
 */
BlockResult encode_block(const std::vector<std::pair<u32, const u64*>>& cells,
                         u32 slots,
                         size_t words,
                         const std::vector<u32>& tri_w,
                         size_t max_bytes,
                         float smooth,
                         int smooth_small,
                         u64 total_tris) {
  BlockResult res;
  // distinct sets
  std::vector<std::vector<u64>> sets;
  std::vector<u32> count;  // cells per set
  std::vector<int> slot_set(slots, -1);
  {
    std::unordered_map<u64, std::vector<u32>> by_hash;
    for (const auto& [slot, b] : cells) {
      u64 h = 1469598103934665603ull;
      for (size_t k = 0; k < words; k++) {
        h = (h ^ b[k]) * 1099511628211ull;
      }
      auto& lst = by_hash[h];
      int found = -1;
      for (u32 c : lst) {
        if (!memcmp(sets[c].data(), b, words * 8)) {
          found = (int)c;
          break;
        }
      }
      if (found < 0) {
        found = (int)sets.size();
        sets.emplace_back(b, b + words);
        count.push_back(0);
        lst.push_back((u32)found);
      }
      count[found]++;
      slot_set[slot] = found;
    }
  }
  if (smooth_small > 0 && sets.size() > 1) {
    // small units seen from any cell of the block: seen from all of them
    std::vector<u64> any(words, 0);
    for (const auto& st : sets) {
      for (size_t k = 0; k < words; k++) {
        any[k] |= st[k];
      }
    }
    for (size_t k = 0; k < words; k++) {
      u64 v = any[k], keep = 0;
      while (v) {
        const int b = __builtin_ctzll(v);
        if (tri_w[k * 64 + b] < (u32)smooth_small) {
          keep |= 1ull << b;
        }
        v &= v - 1;
      }
      any[k] = keep;
    }
    for (auto& st : sets) {
      for (size_t k = 0; k < words; k++) {
        st[k] |= any[k];
      }
    }
    if (smooth <= 0) {
      smooth = 2.f;  // (only the dedupe below)
    }
  }
  if (smooth > 0 && sets.size() > 1) {
    // units seen from at least `smooth` of the block's cells: seen from all of them
    const size_t nb = words * 64;
    std::vector<u32> seen(nb, 0);
    u32 total = 0;
    for (size_t i = 0; i < sets.size(); i++) {
      total += count[i];
      for (size_t k = 0; k < words; k++) {
        u64 v = sets[i][k];
        while (v) {
          seen[k * 64 + __builtin_ctzll(v)] += count[i];
          v &= v - 1;
        }
      }
    }
    std::vector<u64> common(words, 0);
    for (size_t u = 0; u < nb; u++) {
      if (seen[u] && smooth <= 1.f && seen[u] >= smooth * total) {
        common[u >> 6] |= 1ull << (u & 63);
      }
    }
    // OR it in, then the sets that became the same are one
    std::unordered_map<u64, std::vector<u32>> by_hash;
    std::vector<int> remap(sets.size(), -1);
    std::vector<std::vector<u64>> ns;
    std::vector<u32> nc;
    for (size_t i = 0; i < sets.size(); i++) {
      for (size_t k = 0; k < words; k++) {
        sets[i][k] |= common[k];
      }
      u64 h = 1469598103934665603ull;
      for (size_t k = 0; k < words; k++) {
        h = (h ^ sets[i][k]) * 1099511628211ull;
      }
      auto& lst = by_hash[h];
      int found = -1;
      for (u32 c : lst) {
        if (ns[c] == sets[i]) {
          found = (int)c;
          break;
        }
      }
      if (found < 0) {
        found = (int)ns.size();
        ns.push_back(sets[i]);
        nc.push_back(0);
        lst.push_back((u32)found);
      }
      nc[found] += count[i];
      remap[i] = found;
    }
    for (u32 i = 0; i < slots; i++) {
      if (slot_set[i] >= 0) {
        slot_set[i] = remap[slot_set[i]];
      }
    }
    sets = std::move(ns);
    count = std::move(nc);
  }
  res.unique_sets = (u32)sets.size();
  auto weight_diff = [&](const std::vector<u64>& a, const std::vector<u64>& b) {  // w(b & ~a)
    u64 s = 0;
    for (size_t k = 0; k < words; k++) {
      u64 v = b[k] & ~a[k];
      while (v) {
        s += tri_w[k * 64 + __builtin_ctzll(v)];
        v &= v - 1;
      }
    }
    return s;
  };
  auto hamming = [&](const std::vector<u64>& a, const std::vector<u64>& b) {
    u32 s = 0;
    for (size_t k = 0; k < words; k++) {
      s += (u32)__builtin_popcountll(a[k] ^ b[k]);
    }
    return s;
  };
  for (int round = 0;; round++) {
    const u32 n = (u32)sets.size();
    // spanning tree of the fewest differences (Prim)
    std::vector<u32> parent(n, UINT32_MAX), best(n, UINT32_MAX);
    std::vector<u8> in(n, 0);
    std::vector<std::vector<u32>> kids(n);
    if (n) {
      best[0] = 0;
      for (u32 it = 0; it < n; it++) {
        u32 u = UINT32_MAX;
        for (u32 i = 0; i < n; i++) {
          if (!in[i] && (u == UINT32_MAX || best[i] < best[u])) {
            u = i;
          }
        }
        in[u] = 1;
        if (parent[u] != UINT32_MAX) {
          kids[parent[u]].push_back(u);
          kids[u].push_back(parent[u]);
        }
        for (u32 i = 0; i < n; i++) {
          if (!in[i]) {
            const u32 d = hamming(sets[u], sets[i]);
            if (d < best[i]) {
              best[i] = d;
              parent[i] = u;
            }
          }
        }
      }
    }
    // the root: the tree's center (the fewest steps down to any set); ties: the smaller set
    u32 root = 0;
    if (n > 1) {
      std::vector<u32> deg(n);
      std::vector<u32> leaves;
      for (u32 i = 0; i < n; i++) {
        deg[i] = (u32)kids[i].size();
        if (deg[i] <= 1) {
          leaves.push_back(i);
        }
      }
      u32 left = n;
      std::vector<u8> gone(n, 0);
      while (left > 2) {
        std::vector<u32> next;
        for (u32 l : leaves) {
          gone[l] = 1;
          left--;
          for (u32 m : kids[l]) {
            if (!gone[m] && --deg[m] == 1) {
              next.push_back(m);
            }
          }
        }
        leaves = next;
      }
      root = UINT32_MAX;
      for (u32 i = 0; i < n; i++) {
        if (!gone[i]) {
          u64 pc = 0, pr = 0;
          for (size_t k = 0; k < words; k++) {
            pc += __builtin_popcountll(sets[i][k]);
            if (root != UINT32_MAX) {
              pr += __builtin_popcountll(sets[root][k]);
            }
          }
          if (root == UINT32_MAX || pc < pr) {
            root = i;
          }
        }
      }
    }
    // breadth first from the root: parents before their children
    std::vector<u32> order, local(n, UINT32_MAX), tparent(n, UINT32_MAX), depth(n, 0);
    if (n) {
      order.push_back(root);
      local[root] = 0;
      for (size_t i = 0; i < order.size(); i++) {
        const u32 u = order[i];
        for (u32 v : kids[u]) {
          if (local[v] == UINT32_MAX) {
            local[v] = (u32)order.size();
            tparent[v] = u;
            depth[v] = depth[u] + 1;
            order.push_back(v);
          }
        }
      }
    }
    // encode
    std::vector<u8> head, cellmap, body;
    std::vector<u32> offs(n);
    // cell map
    {
      u32 at = 0;
      while (at < slots) {
        const int v = slot_set[at];
        u32 run = 1;
        while (at + run < slots && slot_set[at + run] == v) {
          run++;
        }
        put_varint(&cellmap, run);
        put_varint(&cellmap, v < 0 ? 0 : local[v] + 1);
        at += run;
      }
    }
    u64 positions = 0;
    for (u32 i = 0; i < n; i++) {
      const u32 s = order[i];
      offs[i] = (u32)body.size();
      const u32 p = tparent[s];
      put_varint(&body, p == UINT32_MAX ? 0 : i - local[p]);
      std::vector<u32> pos;
      for (size_t k = 0; k < words; k++) {
        u64 v = p == UINT32_MAX ? sets[s][k] : sets[s][k] ^ sets[p][k];
        while (v) {
          pos.push_back((u32)(k * 64 + __builtin_ctzll(v)));
          v &= v - 1;
        }
      }
      positions += pos.size();
      put_varint(&body, (u32)pos.size());
      for (size_t j = 0; j < pos.size(); j++) {
        put_varint(&body, j == 0 ? pos[0] : pos[j] - pos[j - 1] - 1);
      }
    }
    put_varint(&head, n);
    const size_t fixed = head.size() + 2 * (size_t)n + cellmap.size();
    const size_t total = fixed + body.size();
    if (total <= max_bytes || n <= 1) {
      std::vector<u8>& o = res.data;
      o = head;
      for (u32 i = 0; i < n; i++) {
        const u32 off = (u32)(fixed + offs[i]);
        if (off > 0xffff) {
          res.ok = false;
        }
        o.push_back((u8)off);
        o.push_back((u8)(off >> 8));
      }
      o.insert(o.end(), cellmap.begin(), cellmap.end());
      o.insert(o.end(), body.begin(), body.end());
      res.sets = n;
      res.diff_positions = positions;
      for (u32 i = 0; i < slots; i++) {
        if (slot_set[i] >= 0) {
          const u32 d = depth[slot_set[i]] + 1;
          res.depth_sum += d;
          res.max_depth = std::max(res.max_depth, d);
        }
      }
      res.ok = res.ok && total <= 0xffff;
      break;
    }
    // over the budget: merge about a tenth of the sets (the cheapest pairs: extra triangles seen,
    // times the cells that would see them)
    struct Cand {
      double cost;
      u32 a, b;
      bool operator<(const Cand& o) const { return cost > o.cost; }
    };
    std::priority_queue<Cand> pq;
    for (u32 a = 0; a < n; a++) {
      for (u32 b = a + 1; b < n; b++) {
        pq.push({(double)count[a] * weight_diff(sets[a], sets[b]) +
                     (double)count[b] * weight_diff(sets[b], sets[a]),
                 a, b});
      }
    }
    std::vector<u32> fwd(n);
    for (u32 i = 0; i < n; i++) {
      fwd[i] = i;
    }
    std::vector<u8> dirty(n, 0);
    u32 merges = std::max<u32>(1, n / 10);
    while (merges && !pq.empty()) {
      const Cand c = pq.top();
      pq.pop();
      if (fwd[c.a] != c.a || fwd[c.b] != c.b || dirty[c.a] || dirty[c.b]) {
        continue;
      }
      for (size_t k = 0; k < words; k++) {
        sets[c.a][k] |= sets[c.b][k];
      }
      count[c.a] += count[c.b];
      fwd[c.b] = c.a;
      dirty[c.a] = 1;  // (its costs changed: next round)
      merges--;
      res.merged++;
    }
    std::vector<int> remap(n, -1);
    std::vector<std::vector<u64>> ns;
    std::vector<u32> nc;
    for (u32 i = 0; i < n; i++) {
      if (fwd[i] == i) {
        remap[i] = (int)ns.size();
        ns.push_back(std::move(sets[i]));
        nc.push_back(count[i]);
      }
    }
    for (u32 i = 0; i < slots; i++) {
      if (slot_set[i] >= 0) {
        u32 c = (u32)slot_set[i];
        while (fwd[c] != c) {
          c = fwd[c];
        }
        slot_set[i] = remap[c];
      }
    }
    sets = std::move(ns);
    count = std::move(nc);
  }
  for (u32 i = 0; i < slots; i++) {
    if (slot_set[i] >= 0) {
      u64 w = 0;
      for (size_t k = 0; k < words; k++) {
        u64 v = sets[slot_set[i]][k];
        while (v) {
          w += tri_w[k * 64 + __builtin_ctzll(v)];
          v &= v - 1;
        }
      }
      res.vis_sum += (double)w / std::max<u64>(1, total_tris);
      res.cells++;
    }
  }
  // check: every cell decodes to its set
  {
    std::vector<u32> bits((words * 64) / 32);
    std::vector<u16> path(std::max<u32>(1, res.sets));
    std::vector<int> local_of(slots, -1);
    for (u32 i = 0; i < slots && res.ok; i++) {
      const int ls = c3v::block_cell_set(res.data.data(), (u32)res.data.size(), i);
      if ((ls < 0) != (slot_set[i] < 0)) {
        res.ok = false;
        break;
      }
      if (ls < 0) {
        continue;
      }
      if (!c3v::block_decode_set(res.data.data(), (u32)res.data.size(), (u32)ls, bits.data(),
                                 (u32)bits.size(), path.data()) ||
          memcmp(bits.data(), sets[slot_set[i]].data(), words * 8)) {
        res.ok = false;
      }
    }
  }
  return res;
}

// ---------------------------------------------------------------------------------------------

std::string fr3_for(const std::string& c3l_path, const std::string& name, const Options& opt) {
  if (!opt.fr3.empty()) {
    if (fs::is_directory(opt.fr3)) {
      return (fs::path(opt.fr3) / (name + ".fr3")).string();
    }
    return opt.fr3;
  }
  return (fs::path(c3l_path).parent_path().parent_path() / "fr3" / (name + ".fr3")).string();
}

bool build(const std::string& in, const std::string& out_path, const Options& opt) {
  const auto t0 = std::chrono::steady_clock::now();
  auto secs = [&]() {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  };
  LevelGeo lev;
  std::string err;
  if (!load_level(in, opt, &lev, &err)) {
    fmt::print("{}: {}\n", in, err);
    return false;
  }
  // floors
  std::vector<Tri> floors;
  std::string floor_src = "draws";
  if (!opt.draw_floors) {
    std::string why;
    const std::string fr3 = fr3_for(in, fs::path(in).stem().string(), opt);
    if (load_collision_floors(fr3, &floors, &why)) {
      floor_src = "collision";
    } else {
      fmt::print("{}: {}: floors from the draws\n", lev.name, why);
      floors.clear();
    }
  }
  if (floors.empty()) {
    floors = lev.draw_floors;
  }
  if (lev.units.empty() || floors.empty() || lev.occ.empty()) {
    fmt::print("{}: nothing to do ({} units, {} floor triangles, {} occluder triangles)\n", in,
               lev.units.size(), floors.size(), lev.occ.size());
    // no file: the level renders as without fine visibility
    if (fs::exists(out_path)) {
      fs::remove(out_path);
    }
    return true;
  }
  CubeGeom geom;
  geom.init(opt.face);
  CapTable ct;
  ct.init(geom);
  const float cell = opt.cell, ccell = 2 * opt.cell;
  Params pf, pc;
  const float eps_k = opt.eps_scale > 0 ? opt.eps_scale : (opt.center_only ? 0.866f : 0.56f);
  pf.init(cell * eps_k + 0.02f, opt, ct);
  pc.init(ccell * eps_k + 0.02f, opt, ct);

  const size_t nu = lev.units.size();
  const size_t words = (nu + 63) / 64;
  if (!opt.debug_point.empty()) {
    SampleWork w(geom, ct, lev);
    std::vector<u64> bits;
    const V3 p{opt.debug_point[0], opt.debug_point[1], opt.debug_point[2]};
    w.run(p, pf, &bits);
    u64 vis = 0, vt = 0;
    for (size_t u = 0; u < nu; u++) {
      if (bits[u >> 6] >> (u & 63) & 1) {
        vis++;
        vt += lev.units[u].tri_count;
      }
    }
    fmt::print("debug point ({}, {}, {}): {} of {} units, {} of {} triangles may be seen; {} "
               "windows too wide; render {:.1f} ms, maps {:.1f} ms, units {:.1f} ms; {} clusters drawn\n",
               p.x, p.y, p.z, vis, nu, vt, lev.total_tris, w.windows_too_wide, w.t_render * 1e3,
               w.t_occ * 1e3, w.t_units * 1e3, w.clusters_drawn);
    w.dump(out_path);
    for (int u : opt.debug_units) {
      if (u >= 0 && (size_t)u < nu) {
        u32 ci = 0;
        while (ci + 1 < lev.chunk_unit_first.size() && lev.chunk_unit_first[ci + 1] <= (u32)u) {
          ci++;
        }
        const auto& un = lev.units[u];
        fmt::print("  unit {} (chunk {}): {} from here ({} tris, {:.1f} m away)\n", u, ci,
                   (bits[u >> 6] >> (u & 63) & 1) ? "may be seen" : "HIDDEN", un.tri_count,
                   box_dist(un.box, p));
      }
    }
    {
      const float offs[] = {0.05f, 0.25f, 0.5f, 1.f, 2.f};
      for (float o : offs) {
        std::vector<u64> b2;
        w.run({p.x + o, p.y, p.z}, pf, &b2);
        u32 d = 0, a = 0;
        for (size_t k = 0; k < bits.size(); k++) {
          d += __builtin_popcountll(bits[k] ^ b2[k]);
          a += __builtin_popcountll(bits[k]);
        }
        fmt::print("  moved {} m along x: {} of {} units differ\n", o, d, a);
      }
    }
    // the largest units that may be seen: their distance and size
    std::vector<std::pair<u32, u32>> vu;
    for (size_t u = 0; u < nu; u++) {
      if (bits[u >> 6] >> (u & 63) & 1) {
        vu.push_back({lev.units[u].tri_count, (u32)u});
      }
    }
    std::sort(vu.rbegin(), vu.rend());
    for (size_t i = 0; i < vu.size() && i < 25; i++) {
      const auto& un = lev.units[vu[i].second];
      u32 ci = 0;
      while (ci + 1 < lev.chunk_unit_first.size() && lev.chunk_unit_first[ci + 1] <= vu[i].second) {
        ci++;
      }
      const V3 e = sub(un.box.hi, un.box.lo);
      const V3 c = sub(mul(add(un.box.lo, un.box.hi), 0.5f), p);
      fmt::print("  unit {} (chunk {} kind {}): {} tris, {:.1f} m away, box {:.1f} x {:.1f} x {:.1f} "
                 "m, center at {:.0f} {:.0f} {:.0f}\n",
                 vu[i].second, ci, (int)0, un.tri_count, box_dist(un.box, p), e.x, e.y, e.z, c.x,
                 c.y, c.z);
    }
    return true;
  }

  // ---- the grid (fine cells; coarse cells are 2x2x2 of them) ----
  const int B = opt.block;
  Grid grid;
  grid.cell = cell;
  Box fb;
  for (const auto& t : floors) {
    for (auto& p : t.v) {
      fb.add(p);
    }
  }
  const float mxz = std::max(opt.fine_spread, opt.coarse_spread) + 2 * ccell;
  const float align = ccell;
  grid.origin = {std::floor((fb.lo.x - mxz) / align) * align,
                 std::floor((fb.lo.y - opt.below - 2 * ccell) / align) * align,
                 std::floor((fb.lo.z - mxz) / align) * align};
  auto round_up = [&](float extent) {
    const int n = (int)std::ceil(extent / cell);
    return (n + B - 1) / B * B;
  };
  grid.nx = round_up(fb.hi.x + mxz - grid.origin.x);
  grid.ny = round_up(fb.hi.y + std::max(opt.above, opt.fine_above) + 2 * ccell - grid.origin.y);
  grid.nz = round_up(fb.hi.z + mxz - grid.origin.z);
  const int cnx = grid.nx / 2, cny = grid.ny / 2, cnz = grid.nz / 2;
  // seeds: the cells the floors' pieces are in
  std::vector<u8> fine((size_t)grid.nx * grid.ny * grid.nz, 0);
  std::vector<u8> coarse(opt.coarse ? (size_t)cnx * cny * cnz : 0, 0);
  for (const auto& t : floors) {
    floor_pieces(t, cell, [&](const Box& b) {
      const int x0 = (int)std::floor((b.lo.x - grid.origin.x) / cell),
                x1 = (int)std::floor((b.hi.x - grid.origin.x) / cell);
      const int y0 = (int)std::floor((b.lo.y - grid.origin.y) / cell),
                y1 = (int)std::floor((b.hi.y - grid.origin.y) / cell);
      const int z0 = (int)std::floor((b.lo.z - grid.origin.z) / cell),
                z1 = (int)std::floor((b.hi.z - grid.origin.z) / cell);
      for (int z = std::max(0, z0); z <= std::min(grid.nz - 1, z1); z++) {
        for (int y = std::max(0, y0); y <= std::min(grid.ny - 1, y1); y++) {
          for (int x = std::max(0, x0); x <= std::min(grid.nx - 1, x1); x++) {
            fine[grid.key(x, y, z)] = 1;
            if (opt.coarse) {
              coarse[((size_t)(z / 2) * cny + y / 2) * cnx + x / 2] = 1;
            }
          }
        }
      }
    });
  }
  dilate(&fine, grid.nx, grid.ny, grid.nz, (int)std::ceil(opt.fine_above / cell),
         (int)std::ceil(opt.below / cell), (int)std::ceil(opt.fine_spread / cell));
  if (opt.coarse) {
    dilate(&coarse, cnx, cny, cnz, (int)std::ceil(opt.above / ccell),
           (int)std::ceil(opt.below / ccell), (int)std::ceil(opt.coarse_spread / ccell));
  }
  // a coarse cell with any fine cell in it: all eight are fine
  std::vector<u32> fine_cells, coarse_cells;
  for (int z = 0; z < cnz; z++) {
    for (int y = 0; y < cny; y++) {
      for (int x = 0; x < cnx; x++) {
        bool any = false;
        for (int c = 0; c < 8 && !any; c++) {
          any = fine[grid.key(2 * x + (c & 1), 2 * y + ((c >> 1) & 1), 2 * z + (c >> 2))];
        }
        if (any) {
          for (int c = 0; c < 8; c++) {
            fine[grid.key(2 * x + (c & 1), 2 * y + ((c >> 1) & 1), 2 * z + (c >> 2))] = 1;
          }
        } else if (opt.coarse && coarse[((size_t)z * cny + y) * cnx + x]) {
          coarse_cells.push_back(((u32)z * cny + y) * cnx + x);
        }
      }
    }
  }
  for (u32 k = 0; k < fine.size(); k++) {
    if (fine[k]) {
      fine_cells.push_back(k);
    }
  }
  fine.clear();
  fine.shrink_to_fit();
  coarse.clear();
  coarse.shrink_to_fit();
  const size_t ncells = fine_cells.size() + coarse_cells.size();
  if (!opt.cam_stats.empty()) {
    FILE* fp = fopen(opt.cam_stats.c_str(), "r");
    char name[64];
    float x, y, z;
    u64 n = 0, in_fine = 0, in_coarse = 0;
    while (fp && fscanf(fp, "%63s %f %f %f", name, &x, &y, &z) == 4) {
      if (lev.name != name) {
        continue;
      }
      n++;
      const int cx = (int)std::floor((x - grid.origin.x) / cell),
                cy = (int)std::floor((y - grid.origin.y) / cell),
                cz = (int)std::floor((z - grid.origin.z) / cell);
      if (cx < 0 || cy < 0 || cz < 0 || cx >= grid.nx || cy >= grid.ny || cz >= grid.nz) {
        continue;
      }
      if (std::binary_search(fine_cells.begin(), fine_cells.end(), grid.key(cx, cy, cz))) {
        in_fine++;
      } else if (std::binary_search(coarse_cells.begin(), coarse_cells.end(),
                                    ((u32)(cz / 2) * cny + cy / 2) * cnx + cx / 2)) {
        in_coarse++;
      }
    }
    if (fp) {
      fclose(fp);
    }
    fmt::print("{}: cameras: {} positions, {:.1f}% in {} m cells, {:.1f}% in {} m cells, {:.1f}% "
               "outside\n",
               lev.name, n, 100.0 * in_fine / std::max<u64>(1, n), cell,
               100.0 * in_coarse / std::max<u64>(1, n), ccell,
               100.0 * (n - in_fine - in_coarse) / std::max<u64>(1, n));
  }

  // ---- samples: corners (lattice nodes, shared) and centers, per cell size ----
  struct Sample {
    V3 p;
    u8 coarse;
  };
  std::vector<Sample> samples;
  std::vector<u32> s_first, s_cells;  // CSR: sample -> cells (fine 0.., coarse after)
  {
    std::vector<std::vector<u32>> lists;
    // each cell's corners (lattice nodes, shared by the cells around them) and its center (a
    // body-centered lattice: every point of the cell within 0.56 cell of one); --center-only: the
    // center alone (half the cell's diagonal: the same sets with half the samples, but a hole
    // behind a near object can escape the one sample: maincave, Oct 17, 15 pixels in one frame)
    auto add_lattice = [&](const std::vector<u32>& cells, int nx, int ny, float sz, u8 c,
                           u32 cell_base) {
      std::unordered_map<u64, u32> corner;
      corner.reserve(cells.size() * 2);
      const u64 nx1 = nx + 1, ny1 = ny + 1;
      for (u32 ci = 0; ci < cells.size(); ci++) {
        const u32 k = cells[ci];
        const int x = k % nx, y = (k / nx) % ny, z = k / nx / ny;
        for (int cc = 0; cc < 8 && !opt.center_only; cc++) {
          const int cx = x + (cc & 1), cy = y + ((cc >> 1) & 1), cz = z + (cc >> 2);
          const u64 ck = ((u64)cz * ny1 + cy) * nx1 + cx;
          auto it = corner.find(ck);
          if (it == corner.end()) {
            it = corner.emplace(ck, (u32)samples.size()).first;
            samples.push_back({{grid.origin.x + cx * sz, grid.origin.y + cy * sz,
                                grid.origin.z + cz * sz},
                               c});
            lists.emplace_back();
          }
          lists[it->second].push_back(cell_base + ci);
        }
        samples.push_back({{grid.origin.x + (x + 0.5f) * sz, grid.origin.y + (y + 0.5f) * sz,
                            grid.origin.z + (z + 0.5f) * sz},
                           c});
        lists.push_back({cell_base + ci});
      }
    };
    add_lattice(fine_cells, grid.nx, grid.ny, cell, 0, 0);
    add_lattice(coarse_cells, cnx, cny, ccell, 1, (u32)fine_cells.size());
    s_first.reserve(samples.size() + 1);
    for (auto& l : lists) {
      s_first.push_back((u32)s_cells.size());
      s_cells.insert(s_cells.end(), l.begin(), l.end());
    }
    s_first.push_back((u32)s_cells.size());
  }
  size_t nsamples = samples.size();
  std::vector<u32> sample_order(nsamples);
  for (u32 i = 0; i < nsamples; i++) {
    sample_order[i] = i;
  }
  if (opt.max_samples > 0 && nsamples > (size_t)opt.max_samples) {
    // (testing) every k-th sample only
    std::vector<u32> keep;
    const size_t k = nsamples / opt.max_samples;
    for (size_t i = 0; i < nsamples; i += k) {
      keep.push_back((u32)i);
    }
    sample_order = std::move(keep);
  }
  const int nthreads =
      opt.threads > 0 ? opt.threads : std::max(1u, std::thread::hardware_concurrency());
  fmt::print("{}: {} units ({} triangles: tfrag {}, tie {}, shrub {}), occluders {} triangles "
             "(tfrag {}, tie {}, shrub {}; {} clusters), floors {} triangles ({}); {} fine cells "
             "of {} m + {} of {} m ({}x{}x{} grid), {} samples, eps {:.2f} / {:.2f} m, {} MB of "
             "sets, {} threads\n",
             lev.name, nu, lev.total_tris, lev.kind_tris[1], lev.kind_tris[2], lev.kind_tris[3],
             lev.occ.size(), lev.occ_kind_tris[1], lev.occ_kind_tris[2], lev.occ_kind_tris[3],
             lev.clusters.size(), floors.size(), floor_src, fine_cells.size(), cell,
             coarse_cells.size(), ccell, grid.nx, grid.ny, grid.nz, nsamples, pf.eps, pc.eps,
             ncells * words * 8 / (1 << 20), nthreads);
  fflush(stdout);
  if (opt.count_only) {
    return true;
  }

  // ---- sampling ----
  std::vector<u64> cell_bits(ncells * words, 0);
  bool loaded_bits = false;
  if (!opt.load_bits.empty()) {
    FILE* fp = fopen(opt.load_bits.c_str(), "rb");
    u64 n = 0, w = 0;
    loaded_bits = fp && fread(&n, 8, 1, fp) == 1 && fread(&w, 8, 1, fp) == 1 && n == ncells &&
                  w == words &&
                  fread(cell_bits.data(), 8, cell_bits.size(), fp) == cell_bits.size();
    if (fp) {
      fclose(fp);
    }
    fmt::print("{}: sets {} from {}\n", lev.name, loaded_bits ? "read" : "NOT read", opt.load_bits);
    if (!loaded_bits) {
      return false;
    }
    sample_order.clear();
  }
  std::atomic<size_t> next{0};
  std::mutex print_lock;
  double t_render_all = 0, t_occ_all = 0, t_units_all = 0;
  u64 n_wide = 0, n_drawn = 0, n_hidden = 0, n_tests = 0;
  double n_vis = 0;
  const double ts0 = secs();
  std::vector<std::thread> pool;
  for (int t = 0; t < nthreads; t++) {
    pool.emplace_back([&, t]() {
      SampleWork w(geom, ct, lev);
      std::vector<u64> bits;
      for (;;) {
        const size_t oi = next.fetch_add(1);
        if (oi >= sample_order.size()) {
          break;
        }
        const u32 i = sample_order[oi];
        w.run(samples[i].p, samples[i].coarse ? pc : pf, &bits);
        for (u32 j = s_first[i]; j < s_first[i + 1]; j++) {
          u64* dst = &cell_bits[(size_t)s_cells[j] * words];
          for (size_t k = 0; k < words; k++) {
            if (bits[k]) {
              __atomic_fetch_or(&dst[k], bits[k], __ATOMIC_RELAXED);
            }
          }
        }
        if (opt.verbose && t == 0 && oi % 5000 < (size_t)nthreads) {
          std::lock_guard<std::mutex> l(print_lock);
          const double el = secs() - ts0;
          fmt::print("  {} / {} samples, {:.0f} s, ~{:.0f} s left\n", oi, sample_order.size(), el,
                     el / std::max<size_t>(1, oi) * (sample_order.size() - oi));
          fflush(stdout);
        }
      }
      std::lock_guard<std::mutex> l(print_lock);
      t_render_all += w.t_render;
      t_occ_all += w.t_occ;
      t_units_all += w.t_units;
      n_wide += w.windows_too_wide;
      n_drawn += w.clusters_drawn;
      n_hidden += w.clusters_hidden;
      n_tests += w.tri_tests;
      n_vis += w.vis_tris;
    });
  }
  for (auto& t : pool) {
    t.join();
  }
  const double sample_s = secs() - ts0;
  if (!opt.save_bits.empty() && !loaded_bits) {
    FILE* fp = fopen(opt.save_bits.c_str(), "wb");
    const u64 n = ncells, w = words;
    if (fp) {
      fwrite(&n, 8, 1, fp);
      fwrite(&w, 8, 1, fp);
      fwrite(cell_bits.data(), 8, cell_bits.size(), fp);
      fclose(fp);
    }
  }
  const double ns = std::max<double>(1, sample_order.size());
  fmt::print("{}: per sample: render {:.2f} ms, windows {:.2f} ms, units {:.2f} ms; {:.0f} wide "
             "windows, {:.0f} clusters drawn, {:.0f} skipped, {:.0f} triangle tests; {:.1f}% of "
             "the triangles may be seen from a sample\n",
             lev.name, 1e3 * t_render_all / ns, 1e3 * t_occ_all / ns, 1e3 * t_units_all / ns,
             n_wide / ns, n_drawn / ns, n_hidden / ns, n_tests / ns,
             100.0 * n_vis / ns / std::max<u64>(1, lev.total_tris));

  if (opt.verbose) {
    // (statistics) how much the sets of neighbouring fine cells (along x) differ
    std::vector<u32> hd;
    for (size_t ci = 0; ci + 1 < fine_cells.size(); ci++) {
      if (fine_cells[ci + 1] == fine_cells[ci] + 1 && (fine_cells[ci] % grid.nx) + 1 < (u32)grid.nx) {
        u32 d = 0;
        for (size_t k = 0; k < words; k++) {
          d += __builtin_popcountll(cell_bits[ci * words + k] ^ cell_bits[(ci + 1) * words + k]);
        }
        hd.push_back(d);
      }
    }
    std::sort(hd.begin(), hd.end());
    if (!hd.empty()) {
      double mean = 0;
      for (u32 v : hd) {
        mean += v;
      }
      fmt::print("{}: neighbouring cells' sets differ in {:.0f} units on average; median {}, 75% "
                 "{}, 90% {}, 99% {}; {:.1f}% the same\n",
                 lev.name, mean / hd.size(), hd[hd.size() / 2], hd[hd.size() * 3 / 4],
                 hd[hd.size() * 9 / 10], hd[hd.size() * 99 / 100],
                 100.0 * (std::upper_bound(hd.begin(), hd.end(), 0u) - hd.begin()) / hd.size());
    }
  }
  // ---- blocks ----
  std::vector<u32> tri_w(words * 64, 0);
  for (size_t u = 0; u < nu; u++) {
    tri_w[u] = lev.units[u].tri_count;
  }
  auto weight = [&](const u64* a) {
    u64 s = 0;
    for (size_t k = 0; k < words; k++) {
      u64 v = a[k];
      while (v) {
        s += tri_w[k * 64 + __builtin_ctzll(v)];
        v &= v - 1;
      }
    }
    return s;
  };
  const int bnx = grid.nx / B, bny = grid.ny / B, bnz = grid.nz / B;
  const u32 slots = (u32)(B * B * B);
  std::map<u32, std::vector<std::pair<u32, const u64*>>> blocks;  // key -> (slot, bits)
  double vis_fine = 0, vis_coarse = 0;
  for (u32 ci = 0; ci < fine_cells.size(); ci++) {
    const u32 k = fine_cells[ci];
    const int x = k % grid.nx, y = (k / grid.nx) % grid.ny, z = k / grid.nx / grid.ny;
    const u32 bk = ((u32)(z / B) * bny + y / B) * bnx + x / B;
    const u32 slot = ((u32)(z % B) * B + y % B) * B + x % B;
    const u64* b = &cell_bits[(size_t)ci * words];
    blocks[bk].push_back({slot, b});
    vis_fine += (double)weight(b) / std::max<u64>(1, lev.total_tris);
  }
  for (u32 cj = 0; cj < coarse_cells.size(); cj++) {
    const u32 k = coarse_cells[cj];
    const int x = k % cnx, y = (k / cnx) % cny, z = k / cnx / cny;
    const u64* b = &cell_bits[(fine_cells.size() + cj) * words];
    vis_coarse += (double)weight(b) / std::max<u64>(1, lev.total_tris);
    for (int c = 0; c < 8; c++) {
      const int fx = 2 * x + (c & 1), fy = 2 * y + ((c >> 1) & 1), fz = 2 * z + (c >> 2);
      const u32 bk = ((u32)(fz / B) * bny + fy / B) * bnx + fx / B;
      const u32 slot = ((u32)(fz % B) * B + fy % B) * B + fx % B;
      blocks[bk].push_back({slot, b});
    }
  }
  std::vector<u32> keys;
  for (auto& [k, v] : blocks) {
    keys.push_back(k);
  }
  std::vector<BlockResult> results(keys.size());
  {
    std::atomic<size_t> nb{0};
    std::vector<std::thread> bp;
    for (int t = 0; t < nthreads; t++) {
      bp.emplace_back([&]() {
        for (;;) {
          const size_t i = nb.fetch_add(1);
          if (i >= keys.size()) {
            break;
          }
          results[i] = encode_block(blocks[keys[i]], slots, words, tri_w,
                                    (size_t)(opt.block_kb * 1024), opt.smooth, opt.smooth_small,
                                    lev.total_tris);
        }
      });
    }
    for (auto& t : bp) {
      t.join();
    }
  }
  // ---- write ----
  c3v::Header h{};
  memcpy(h.magic, c3v::kMagic, 4);
  h.version = c3v::kVersion;
  h.level_hash = lev.level_hash;
  h.num_chunks = lev.covered;
  h.num_units = (u32)nu;
  h.set_words = (u32)((nu + 31) / 32);
  h.origin[0] = grid.origin.x * kM;
  h.origin[1] = grid.origin.y * kM;
  h.origin[2] = grid.origin.z * kM;
  h.cell = cell * kM;
  h.nx = grid.nx;
  h.ny = grid.ny;
  h.nz = grid.nz;
  h.block = B;
  h.bnx = bnx;
  h.bny = bny;
  h.bnz = bnz;
  h.num_blocks = (u32)keys.size();
  h.index_offset = sizeof(h);
  h.occluder_range = opt.occ_range * kM;
  h.eps = pf.eps * kM;
  h.eps_coarse = coarse_cells.empty() ? 0.f : pc.eps * kM;
  h.lod_near = 0.9f * opt.lod_lo * kM;
  h.lod_far = std::max(1.1f, kHoldInView) * opt.lod_hi * kM;
  h.occluders = lev.occ_flags;
  h.detail_min = opt.detail_min;
  h.tie_scale_min = opt.tie_scale_min;
  h.shrub_scale_min = opt.shrub_scale_min;
  h.tie_occ_max = opt.tie_occ_max * kM;
  h.tie_k_lo = (lev.occ_flags & c3v::kOccTieLod) ? opt.tie_k_lo : 0.f;
  h.tie_k_hi = (lev.occ_flags & c3v::kOccTieLod) ? opt.tie_k_hi : 0.f;
  std::vector<c3v::BlockEntry> index(keys.size() + 1);
  u32 off = (u32)(sizeof(h) + index.size() * sizeof(c3v::BlockEntry));
  u64 sets_total = 0, unique_total = 0, merged_total = 0, positions = 0;
  double depth_sum = 0;
  u32 max_depth = 0, max_sets = 0;
  size_t max_block = 0;
  double vis_final = 0;
  u64 slots_final = 0;
  bool ok = true;
  for (size_t i = 0; i < keys.size(); i++) {
    index[i] = {keys[i], off};
    off += (u32)results[i].data.size();
    ok = ok && results[i].ok;
    max_block = std::max(max_block, results[i].data.size());
    sets_total += results[i].sets;
    unique_total += results[i].unique_sets;
    merged_total += results[i].merged;
    positions += results[i].diff_positions;
    depth_sum += results[i].depth_sum;
    max_depth = std::max(max_depth, results[i].max_depth);
    max_sets = std::max(max_sets, results[i].sets);
    vis_final += results[i].vis_sum;
    slots_final += results[i].cells;
  }
  index.back() = {0xffffffffu, off};
  h.max_block = (u32)max_block;
  h.max_sets = max_sets;
  if (!ok) {
    fmt::print("{}: a block didn't encode (or decode) right: no file\n", lev.name);
    return false;
  }
  {
    FILE* fp = fopen(out_path.c_str(), "wb");
    bool wr = fp && fwrite(&h, sizeof(h), 1, fp) == 1 &&
              fwrite(index.data(), sizeof(c3v::BlockEntry), index.size(), fp) == index.size();
    for (size_t i = 0; wr && i < keys.size(); i++) {
      wr = fwrite(results[i].data.data(), 1, results[i].data.size(), fp) == results[i].data.size();
    }
    if (fp) {
      fclose(fp);
    }
    if (!wr) {
      fmt::print("{}: can't write {}\n", lev.name, out_path);
      return false;
    }
  }
  // the merged sets' visible fraction (what the runtime gets)
  fmt::print("{}: {} blocks of {} m ({} KB index), {:.1f} KB in all, largest block {:.1f} KB "
             "({} sets at most), {} sets ({} distinct before {} merges), {:.1f} differences per "
             "set, decode path {:.1f} sets on average ({} at most); visible from a cell (triangle "
             "weighted): {:.1f}% at {} m, {:.1f}% at {} m, {:.1f}% in the file (per {} m of "
             "space); {:.0f} s sampling, {:.0f} s total -> {}\n",
             lev.name, keys.size(), B * cell, index.size() * 8 / 1024, off / 1024.0,
             max_block / 1024.0, max_sets, sets_total, unique_total, merged_total,
             (double)positions / std::max<u64>(1, sets_total),
             depth_sum / std::max<size_t>(1, ncells), max_depth,
             100 * vis_fine / std::max<size_t>(1, fine_cells.size()), cell,
             100 * vis_coarse / std::max<size_t>(1, coarse_cells.size()), ccell,
             100 * vis_final / std::max<u64>(1, slots_final), cell, sample_s, secs(), out_path);
  fflush(stdout);
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  Options opt;
  std::string in, out, out_dir, only;
  bool all = false;
  CLI::App app{"ctr_vis_builder: finer visibility (.c3v) for a .c3l level (AI-assisted)"};
  app.add_option("input", in, "the .c3l file (or the folder with --all)")->required();
  app.add_option("output", out, "the .c3v file (default: next to the .c3l)");
  app.add_flag("--all", all, "every .c3l in the folder");
  app.add_option("--out-dir", out_dir, "write the .c3v files here (default: next to each .c3l)");
  app.add_option("--only", only, "with --all: only these levels (comma separated)");
  app.add_option("--cell", opt.cell, "cell size (meters)");
  app.add_option("--fine-above", opt.fine_above, "cells up to this far above a floor (m)");
  app.add_option("--fine-spread", opt.fine_spread, "... and this far sideways (m)");
  app.add_option("--above", opt.above, "cells of twice the size up to this far above a floor (m)");
  app.add_option("--coarse-spread", opt.coarse_spread, "... and this far sideways (m)");
  app.add_option("--below", opt.below, "cells down to this far below a floor (m)");
  app.add_flag("!--no-coarse", opt.coarse, "no cells of twice the size");
  app.add_option("--fr3", opt.fr3, "the level's .fr3 (or a folder of them) for its collision");
  app.add_flag("--draw-floors", opt.draw_floors, "floors from the draws, not the collision");
  app.add_option("--face", opt.face, "cube face size (pixels)");
  app.add_option("--occ-range", opt.occ_range, "occluders only this close (meters)");
  app.add_option("--lod-lo", opt.lod_lo, "the smallest lod_distance the sets are for (m)");
  app.add_option("--lod-hi", opt.lod_hi, "the largest lod_distance the sets are for (m)");
  app.add_option("--detail-min", opt.detail_min, "the smallest detail_scale the sets are for");
  app.add_option("--tie-scale-min", opt.tie_scale_min, "the smallest tie_distance_scale");
  app.add_option("--shrub-scale-min", opt.shrub_scale_min, "the smallest shrub_scale");
  app.add_option("--tie-occ-max", opt.tie_occ_max, "ties occlude only nearer than this (m)");
  app.add_option("--tie-k-lo", opt.tie_k_lo, "tie LOD switch factor: the smallest (0: no versions)");
  app.add_option("--tie-k-hi", opt.tie_k_hi, "tie LOD switch factor: the largest");
  app.add_flag("!--no-ties", opt.ties, "ties never occlude");
  app.add_option("--occ-min-area", opt.occ_min_area, "(testing) occluders: triangles of this area (m^2)");
  app.add_flag("!--no-shrubs", opt.shrubs, "shrubs never occlude");
  app.add_flag("!--no-closed", opt.closed, "closed (back face culled) draws never occlude");
  app.add_option("--layers", opt.layers, "(testing) the depth layers' cuts (x eps), up to 4")
      ->delimiter(',');
  app.add_option("--eps-scale", opt.eps_scale, "(testing) eps / cell");
  app.add_flag("--center-only", opt.center_only, "one sample per cell (half the time, less safe)");
  app.add_option("--block", opt.block, "cells per block side (even)");
  app.add_option("--block-kb", opt.block_kb, "the largest block (KB; sets merge above it)");
  app.add_option("--smooth", opt.smooth, "units seen from this share of a block's cells: all (0 off)");
  app.add_option("--smooth-small", opt.smooth_small, "units of fewer triangles seen in a block: all");
  app.add_option("--threads", opt.threads, "threads (0: all cores)");
  app.add_flag("-v,--verbose", opt.verbose, "progress");
  app.add_option("--max-samples", opt.max_samples, "(testing) only about this many samples");
  app.add_flag("--count", opt.count_only, "(testing) only count the cells and samples");
  app.add_option("--cam-stats", opt.cam_stats, "(testing) camera positions (level x y z, m): in cells?");
  app.add_option("--save-bits", opt.save_bits, "(testing) write the sampled sets to this file");
  app.add_option("--load-bits", opt.load_bits, "(testing) the sets from --save-bits (same options)");
  app.add_option("--debug-point", opt.debug_point, "(testing) x y z: one sample, PGM pictures")
      ->expected(3);
  app.add_option("--debug-units", opt.debug_units, "(testing) with --debug-point: these units")
      ->delimiter(',');
  CLI11_PARSE(app, argc, argv);
  if (opt.face < 8 || (opt.face & (opt.face - 1)) || opt.face > 256) {
    fmt::print("--face: a power of two, 8..256\n");
    return 1;
  }
  if (opt.block < 2 || (opt.block & 1) || opt.block > 32) {
    fmt::print("--block: even, 2..32\n");
    return 1;
  }
  opt.block_kb = std::min(opt.block_kb, 63.f);
  std::vector<std::pair<std::string, std::string>> jobs;
  auto out_for = [&](const fs::path& p) {
    fs::path o = out_dir.empty() ? p : fs::path(out_dir) / p.filename();
    o.replace_extension(".c3v");
    return o.string();
  };
  if (all) {
    std::vector<std::string> names;
    for (size_t a = 0; a < only.size();) {
      size_t b = only.find(',', a);
      if (b == std::string::npos) {
        b = only.size();
      }
      names.push_back(only.substr(a, b - a));
      a = b + 1;
    }
    for (auto& e : fs::directory_iterator(in)) {
      const auto p = e.path();
      if (p.extension() == ".c3l" &&
          (names.empty() ||
           std::find(names.begin(), names.end(), p.stem().string()) != names.end())) {
        jobs.push_back({p.string(), out_for(p)});
      }
    }
    std::sort(jobs.begin(), jobs.end());
  } else {
    jobs.push_back({in, out.empty() ? out_for(in) : out});
  }
  if (!out_dir.empty()) {
    fs::create_directories(out_dir);
  }
  const auto t0 = std::chrono::steady_clock::now();
  bool ok = true;
  for (auto& [i, o] : jobs) {
    ok = build(i, o, opt) && ok;
  }
  if (jobs.size() > 1) {
    fmt::print("all: {} levels, {:.0f} s\n", jobs.size(),
               std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
  }
  return ok ? 0 : 1;
}
