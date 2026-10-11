/*!
 * @file main.cpp
 * (AI-assisted)
 * ctr_level_converter: .fr3 (PC port level background) -> .c3l (3DS renderer format).
 * See docs/3ds-port/c3l_format.md.
 *
 *   ctr_level_converter <in.fr3> <out.c3l> [options]
 *   ctr_level_converter --all <fr3 dir> <out dir> [options]
 */

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/custom_data/Tfrag3Data.h"
#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/util/Serializer.h"
#include "common/util/compress.h"

#include "c3l_pack.h"
#include "ctr_texture.h"
#include "fmt/format.h"
#include "game/graphics/ctr/c3l_format.h"
#include "third-party/BS_thread_pool.hpp"
#include "third-party/CLI11.hpp"
#include "third-party/lz4/lz4hc.h"
#include "third-party/meshoptimizer/meshoptimizer.h"
#include "third-party/zstd/lib/zstd.h"

namespace {

struct Options {
  int tfrag_geo = 1;  // 0 = most detailed, 2 = least (2 has large holes)
  int far_tfrag_geo = 2;  // coarse version for far away cells (-1: none)
  int tie_geo = 3;    // 0 = most detailed, 3 = least
  int palette = -1;   // time of day palette to bake (0..7; -1: auto_palette)
  int max_tex = 128;
  float cell_meters = 200.f;
  // tie instances smaller than detail_radius (bounding sphere radius, meters) are only drawn up
  // to detail_dist meters away; smaller than medium_radius: up to medium_dist
  float detail_radius = 4.f;
  float detail_dist = 100.f;
  float medium_radius = 12.f;
  float medium_dist = 300.f;
  bool no_tie = false;
  bool no_shrub = false;
  // shrubs (grass, bushes, small plants): drawn up to this distance (meters), in chunks of this size
  float shrub_dist = 100.f;
  float shrub_cell = 50.f;
  bool no_merc = false;
  bool merc_only = false;  // common file (GAME.fr3): merc models + their textures only
  // (AI-assisted) merc models whose skeleton group has only lod0 (no -lod1 / -lod2 model in the
  // level, e.g. the yeti): a simplified copy named <model> + c3l::kMercGenLodSuffix, with the
  // same vertices and ~gen_lod_ratio of the triangles (meshopt_simplify per draw, the draws'
  // borders kept), drawn by CtrMercRenderer far away (config merc_gen_lod). 0: none
  float gen_lod_ratio = 0.3f;
  float gen_lod_error = 0.02f;  // largest error, relative to the model's size
  // the model's open edges (holes, cards) stay where they are. Off by default: with the draws
  // simplified together the only open edges are the model's own, which move along themselves
  bool gen_lod_lock_border = false;
  // Textures: ETC1 (opaque) / ETC1A4 (with alpha) instead of RGB565 / RGBA4: 1/4 and 1/2 of the
  // memory, so a level's textures fit in VRAM. Textures of merc models (characters): see below;
  // etc1_merc: all ETC1 (faces and small details show ETC1's 4x4 blocks up close).
  bool etc1 = true;
  bool etc1_merc = false;
  // (AI-assisted) unless etc1_merc: merc textures of etc1_merc_min texels and more on both sides
  // become ETC1 / ETC1A4 when that is about as good as their 16-bit version (merc textures were
  // 75-95% of the levels' texture memory). Measured on the full size level against the image:
  // the PSNR is at least etc1_merc_psnr dB (about RGBA4's own error, the 16-bit format of the ones
  // with alpha) or the 16-bit version's, and no 4x4 block has an RMS error above etc1_merc_block
  // (0..255) or the 16-bit version's worst. Characters with face animation (effects with blend
  // shapes) and eye placeholders stay 16-bit. etc1_merc_psnr 0: all stay 16-bit.
  float etc1_merc_psnr = 34.f;
  int etc1_merc_min = 64;
  float etc1_merc_block = 16.f;
  int etc1_quality = 1;  // 0 low, 1 medium, 2 high (slow)
  // (AI-assisted) ETC1 rate-distortion optimisation strength (ctr_texture.h), 0: off
  float etc1_rdo = 10;
  // Mip levels: the GPU reads textures much faster when it can use a smaller version for surfaces
  // further away (and they don't shimmer).
  bool mips = true;
  int threads = 0;  // texture encoding threads (0: all cores)
  // (AI-assisted) 32-bit textures (the title screen: the logo's colors exact, 16-bit shifted them)
  bool full_color = false;
  // (AI-assisted) v11 block compression (c3l_pack.h), and reading every file back to check it
  c3l_pack::Options pack;
  bool verify = true;
  // (AI-assisted) meshoptimizer: triangles in vertex cache order (within each draw's visibility
  // runs, for draws whose triangle order doesn't matter), vertices in first-use order
  bool vcache = true;
  // (AI-assisted) v13: tie strips wound consistently (odd strip triangles flipped), closed tie
  // surfaces oriented outward and marked for back face culling (c3l::kDrawCullBack)
  bool tie_winding = true;
  // (AI-assisted) v13 vista tier (c3l::CHUNK_KIND_VISTA): the far content (low resolution tfrag,
  // ties without a distance limit, the coarse tfrag of levels without low resolution tfrag)
  // copied into cells of vista_cell meters, the ties and coarse tfrag simplified to vista_error
  // meters (meshoptimizer, topology kept)
  bool vista = true;
  float vista_cell = 1500.f;
  float vista_error = 3.f;
  // then (sloppy: may join or drop small parts) down to vista_ratio of the triangles, with at most
  // vista_sloppy_error meters of error
  float vista_ratio = 0.25f;
  float vista_sloppy_error = 10.f;
};

// (AI-assisted) post-vertex cache statistics (the GPU's cache is cleared before each draw):
// vertices shaded per triangle, for a FIFO cache of 8 and 16 entries
struct VcacheStats {
  double shaded8 = 0, shaded16 = 0;
  u64 tris = 0;
  void add(const u16* idx, size_t count, size_t vertex_count) {
    std::vector<unsigned int> v(idx, idx + count);
    shaded8 += meshopt_analyzeVertexCache(v.data(), count, vertex_count, 8, 0, 0).vertices_transformed;
    shaded16 +=
        meshopt_analyzeVertexCache(v.data(), count, vertex_count, 16, 0, 0).vertices_transformed;
    tris += count / 3;
  }
};

// how a texture is used by the level's draws
struct TexUse {
  bool merc = false;        // drawn by a merc model
  bool alpha_test = false;  // drawn with an alpha test (cut-outs: keep their coverage in the mips)
  int aref = 0x26;          // the alpha test reference (GS units, 0x80 = 1.0)
  // (AI-assisted) merc uses that keep a texture 16-bit (Options::etc1_merc_psnr): an eye draw's
  // placeholder, a draw of an effect with blend shapes (a face). envmap: an envmap pass's texture.
  bool eye = false, face = false, envmap = false;
};

// A triangle soup vertex before chunking
struct SrcVertex {
  float x, y, z;
  float s, t;
  u8 rgba[4];
};

struct SrcTri {
  u32 v[3];         // into the global SrcVertex list
  u32 draw_key;     // index into draw_keys
  u32 detail = 0;   // 1, 2: part of a small/medium object (drawn up to detail/medium_dist),
                    // 3: shrub (drawn up to shrub_dist)
  u32 tier = 0;     // c3l::Chunk::lod_tier
  u16 vis = 0xffff;  // (AI-assisted) drawable id in the level's visibility string (c3l::VisRun)
                     // (vista tier: the source chunk)
};

struct DrawKey {
  u32 mode;
  u16 texture;
  u16 kind;
  bool operator<(const DrawKey& o) const {
    return std::tie(mode, texture, kind) < std::tie(o.mode, o.texture, o.kind);
  }
};

struct Converter {
  Options opt;
  const tfrag3::Level* level = nullptr;
  std::vector<SrcVertex> verts;
  std::vector<SrcTri> tris;
  std::vector<DrawKey> draw_keys;
  std::map<DrawKey, u32> draw_key_ids;
  // level texture index -> output texture index
  std::unordered_map<u32, u16> tex_map;
  std::vector<u32> tex_order;  // output index -> level texture index
  std::vector<TexUse> tex_use;  // by output index

  void note_use(u16 tex, u32 mode_bits, bool merc) {
    if (tex == 0xffff || tex >= tex_use.size()) {
      return;
    }
    DrawMode mode;
    mode.as_int() = mode_bits;
    auto& u = tex_use[tex];
    u.merc |= merc;
    if (mode.get_at_enable() && mode.get_alpha_test() == DrawMode::AlphaTest::GEQUAL &&
        !u.alpha_test) {
      u.alpha_test = true;
      u.aref = mode.get_aref();
    }
  }

  // (AI-assisted) see TexUse
  void note_merc_kind(u16 tex, bool eye, bool face, bool envmap) {
    if (tex < tex_use.size()) {
      tex_use[tex].eye |= eye;
      tex_use[tex].face |= face;
      tex_use[tex].envmap |= envmap;
    }
  }

  u32 key_id(const DrawKey& k) {
    auto it = draw_key_ids.find(k);
    if (it != draw_key_ids.end()) {
      return it->second;
    }
    u32 id = draw_keys.size();
    draw_keys.push_back(k);
    draw_key_ids[k] = id;
    return id;
  }

  // (AI-assisted) Identical level textures (size and texels) are one output texture: the fr3 has
  // a copy per merc model that uses one (citadel: pal-environment-front 90 times, its metal
  // textures 20-30 times each; 937 textures for ~200 different ones).
  struct TexContentHash {
    size_t operator()(const tfrag3::Texture* t) const {
      size_t h = (size_t)t->w * 0x9E3779B97F4A7C15ull ^ t->h;
      for (u32 v : t->data) {
        h = (h ^ v) * 0x100000001B3ull;
      }
      return h;
    }
  };
  struct TexContentEq {
    bool operator()(const tfrag3::Texture* a, const tfrag3::Texture* b) const {
      return a->w == b->w && a->h == b->h && a->data == b->data;
    }
  };
  std::unordered_map<const tfrag3::Texture*, u16, TexContentHash, TexContentEq> tex_by_content;
  size_t tex_copies = 0;  // level textures that are a copy of an earlier one

  u16 texture_id(s32 tree_tex_id) {
    if (tree_tex_id < 0 || tree_tex_id >= (s32)level->textures.size()) {
      return 0xffff;  // animated / missing
    }
    auto it = tex_map.find(tree_tex_id);
    if (it != tex_map.end()) {
      return it->second;
    }
    const tfrag3::Texture* t = &level->textures[tree_tex_id];
    auto cit = tex_by_content.find(t);
    if (cit != tex_by_content.end()) {
      tex_map[tree_tex_id] = cit->second;
      tex_copies++;
      return cit->second;
    }
    u16 id = tex_order.size();
    tex_order.push_back(tree_tex_id);
    tex_use.emplace_back();
    tex_map[tree_tex_id] = id;
    tex_by_content.emplace(t, id);
    return id;
  }

  // (AI-assisted) the visibility id of the triangles being added (see c3l::VisRun): a BVH node's
  // id in the level's visibility string, 0xffff = none
  u16 cur_vis = 0xffff;
  static u16 vis_id(const tfrag3::BVH& bvh, u32 node) {
    return node < bvh.vis_nodes.size() ? bvh.vis_nodes[node].my_id : (u16)0xffff;
  }

  // strips with UINT32_MAX restarts -> triangles
  void add_strips(const std::vector<u32>& indices,
                  size_t first,
                  size_t count,
                  u32 vertex_base,
                  u32 key) {
    std::vector<u32> strip;
    auto flush = [&]() {
      for (size_t i = 2; i < strip.size(); i++) {
        u32 a = strip[i - 2], b = strip[i - 1], c = strip[i];
        if (a == b || b == c || a == c) {
          continue;
        }
        // (AI-assisted) every second triangle of a strip has the opposite vertex order: flipped,
        // all of the strip's triangles face the same way (tie_winding)
        if (opt.tie_winding && ((i - 2) & 1)) {
          std::swap(a, b);
        }
        tris.push_back(SrcTri{{vertex_base + a, vertex_base + b, vertex_base + c}, key});
        tris.back().vis = cur_vis;
      }
      strip.clear();
    };
    for (size_t i = first; i < first + count && i < indices.size(); i++) {
      if (indices[i] == UINT32_MAX) {
        flush();
      } else {
        strip.push_back(indices[i]);
      }
    }
    flush();
  }

  static u32 draw_index_count(const tfrag3::StripDraw& d) {
    u32 n = 0;
    for (auto& g : d.vis_groups) {
      n += g.num_inds;
    }
    return n;
  }

  u32 cur_tier = 0;
  void add_tfrag_tree(tfrag3::TfragTree& tree) {
    size_t first_tri = tris.size();
    add_tfrag_tree_impl(tree);
    for (size_t i = first_tri; i < tris.size(); i++) {
      tris[i].tier = cur_tier;
    }
  }
  void add_tfrag_tree_impl(tfrag3::TfragTree& tree) {
    tree.unpack();
    u32 base = verts.size();
    for (auto& v : tree.unpacked.vertices) {
      SrcVertex sv;
      sv.x = v.x;
      sv.y = v.y;
      sv.z = v.z;
      sv.s = v.s;
      sv.t = v.t;
      for (int c = 0; c < 4; c++) {
        sv.rgba[c] = tree.colors.read(v.color_index, opt.palette, c);
      }
      verts.push_back(sv);
    }
    for (auto& d : tree.draws) {
      DrawKey k{d.mode.as_int(), texture_id(d.tree_tex_id), 0};
      note_use(k.texture, k.mode, false);
      u32 id = key_id(k);
      // (AI-assisted) group by group: each is one node of the tree's BVH (up to 8 tfragments)
      size_t idx = d.unpacked.idx_of_first_idx_in_full_buffer;
      for (const auto& g : d.vis_groups) {
        cur_vis = vis_id(tree.bvh, g.vis_idx_in_pc_bvh);
        if (tree.use_strips) {
          add_strips(tree.unpacked.indices, idx, g.num_inds, base, id);
        } else {
          for (size_t i = idx; i + 2 < idx + g.num_inds; i += 3) {
            tris.push_back(SrcTri{{base + tree.unpacked.indices[i],
                                   base + tree.unpacked.indices[i + 1],
                                   base + tree.unpacked.indices[i + 2]},
                                  id});
            tris.back().vis = cur_vis;
          }
        }
        idx += g.num_inds;
      }
      cur_vis = 0xffff;
    }
  }

  // bounding sphere radius of the triangles from `first` on
  float tris_radius(size_t first) const {
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    for (size_t i = first; i < tris.size(); i++) {
      for (u32 vi : tris[i].v) {
        const auto& v = verts[vi];
        const float p[3] = {v.x, v.y, v.z};
        for (int c = 0; c < 3; c++) {
          mn[c] = std::min(mn[c], p[c]);
          mx[c] = std::max(mx[c], p[c]);
        }
      }
    }
    if (first >= tris.size()) {
      return 0;
    }
    float dx = mx[0] - mn[0], dy = mx[1] - mn[1], dz = mx[2] - mn[2];
    return 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
  }

  size_t radius_hist[8] = {};
  void mark_small(size_t first) {
    const float r = tris_radius(first);
    {
      int b = 0;
      for (float lim = 2 * 4096.f; b < 7 && r >= lim; lim *= 2) {
        b++;
      }
      radius_hist[b] += tris.size() - first;
    }
    u32 detail = r < opt.detail_radius * 4096.f ? 1 : (r < opt.medium_radius * 4096.f ? 2 : 0);
    if (detail) {
      for (size_t i = first; i < tris.size(); i++) {
        tris[i].detail = detail;
      }
      small_tris += tris.size() - first;
    }
  }
  size_t small_tris = 0;
  size_t wind_tris = 0;

  size_t shrub_tris = 0;
  void add_shrub_tree(tfrag3::ShrubTree& tree) {
    tree.unpack();
    const u32 base = verts.size();
    for (const auto& v : tree.unpacked.vertices) {
      SrcVertex sv;
      sv.x = v.x;
      sv.y = v.y;
      sv.z = v.z;
      // shrub.vert: texture coordinates in 1/4096, color = base color * time of day color * 4;
      // our level shader does texture * color * 2 (GS units), like the tfrag colors
      sv.s = v.s / 4096.f;
      sv.t = v.t / 4096.f;
      for (int c = 0; c < 3; c++) {
        const int tod = tree.time_of_day_colors.read(v.color_index, opt.palette, c);
        sv.rgba[c] = (u8)std::min(255, v.rgba_base[c] * tod * 2 / 255);
      }
      sv.rgba[3] = tree.time_of_day_colors.read(v.color_index, opt.palette, 3);
      verts.push_back(sv);
    }
    for (const auto& d : tree.static_draws) {
      const u16 tex = texture_id((s32)d.tree_tex_id);
      note_use(tex, d.mode.as_int(), false);
      const u32 key = key_id(DrawKey{d.mode.as_int(), tex, 2});
      const size_t first_tri = tris.size();
      add_strips(tree.indices, d.first_index_index, d.num_indices, base, key);
      for (size_t i = first_tri; i < tris.size(); i++) {
        tris[i].detail = 3;
      }
      shrub_tris += tris.size() - first_tri;
    }
  }

  // (AI-assisted) tie_winding: back face culling of closed tie surfaces.
  // Can a triangle of this draw key be culled: no blending, and the alpha test (if any) never
  // fails on its texture (a cut-out card shows its back through the holes)
  std::unordered_map<u32, bool> cullable_key;
  bool key_cullable(u32 key) {
    auto it = cullable_key.find(key);
    if (it != cullable_key.end()) {
      return it->second;
    }
    const auto& dk = draw_keys[key];
    DrawMode mode;
    mode.as_int() = dk.mode;
    bool ok = !mode.get_ab_enable() && mode.get_depth_write_enable();
    if (ok && mode.get_at_enable() && mode.get_alpha_test() == DrawMode::AlphaTest::GEQUAL &&
        dk.texture != 0xffff) {
      const auto& t = level->textures.at(tex_order.at(dk.texture));
      for (u32 p : t.data) {
        if (((p >> 24) & 0xff) < mode.get_aref()) {
          ok = false;
          break;
        }
      }
    }
    cullable_key[key] = ok;
    return ok;
  }
  // statistics: tie triangles looked at, marked for culling
  size_t orient_tris = 0, orient_culled = 0, orient_flipped = 0;
  /*!
   * (AI-assisted) The tie triangles [first, end): per visibility group (up to 8 instances), the
   * cullable triangles welded by position into surfaces (edges shared by exactly two triangles),
   * each surface oriented consistently from one of its triangles (the PS2 strips start either
   * way). A surface that is closed enough (at most 10% open edges, no edge with three or more
   * triangles, no contradiction) and has a clear inside (signed volume at least half of its
   * unsigned volume) gets its triangles turned to face outward and marked: their back faces can't
   * be seen from outside, and from inside they are the backs of the walls.
   */
  void orient_ties(size_t first, size_t end) {
    std::map<u16, std::vector<u32>> groups;
    for (size_t i = first; i < end; i++) {
      if (key_cullable(tris[i].draw_key)) {
        groups[tris[i].vis].push_back((u32)i);
      }
    }
    struct PK {
      float x, y, z;
      bool operator==(const PK& o) const { return x == o.x && y == o.y && z == o.z; }
    };
    struct PKH {
      size_t operator()(const PK& k) const {
        u32 a, b, c;
        memcpy(&a, &k.x, 4);
        memcpy(&b, &k.y, 4);
        memcpy(&c, &k.z, 4);
        return (size_t)a * 0x9E3779B97F4A7C15ull ^ (size_t)b * 0xC2B2AE3D27D4EB4Full ^ c;
      }
    };
    for (auto& [vis, ids] : groups) {
      orient_tris += ids.size();
      // welded corners
      std::unordered_map<PK, u32, PKH> weld;
      std::vector<std::array<u32, 3>> w(ids.size());
      std::vector<std::array<float, 3>> pos;
      for (size_t k = 0; k < ids.size(); k++) {
        for (int c = 0; c < 3; c++) {
          const auto& v = verts[tris[ids[k]].v[c]];
          // welded on a 8 unit (2 mm) grid: strips of one surface don't always repeat a corner
          // bit for bit
          auto q = [](float f) { return std::round(f / 8.f); };
          auto [it, fresh] = weld.emplace(PK{q(v.x) + 0.f, q(v.y) + 0.f, q(v.z) + 0.f},
                                          (u32)pos.size());
          if (fresh) {
            pos.push_back({v.x, v.y, v.z});
          }
          w[k][c] = it->second;
        }
      }
      // edges: (lo, hi) -> triangles and whether they run lo -> hi
      std::unordered_map<u64, std::vector<std::pair<u32, bool>>> edges;
      for (size_t k = 0; k < ids.size(); k++) {
        for (int c = 0; c < 3; c++) {
          const u32 a = w[k][c], b = w[k][(c + 1) % 3];
          if (a == b) {
            continue;
          }
          const u64 key = ((u64)std::min(a, b) << 32) | std::max(a, b);
          edges[key].push_back({(u32)k, a < b});
        }
      }
      std::vector<std::vector<u64>> tri_edges(ids.size());
      for (auto& [key, list] : edges) {
        for (auto& [t, dir] : list) {
          tri_edges[t].push_back(key);
        }
      }
      std::vector<int> comp(ids.size(), -1);
      std::vector<u8> flip(ids.size(), 0);
      for (size_t seed = 0; seed < ids.size(); seed++) {
        if (comp[seed] >= 0) {
          continue;
        }
        const int cid = (int)seed;
        std::vector<u32> members{(u32)seed}, stack{(u32)seed};
        comp[seed] = cid;
        bool bad = false;
        size_t open = 0, total = 0;
        float open_top = -1e30f;  // the highest corner of an open edge
        std::set<u64> seen;
        while (!stack.empty()) {
          const u32 t = stack.back();
          stack.pop_back();
          for (u64 key : tri_edges[t]) {
            if (!seen.insert(key).second) {
              continue;
            }
            const auto& list = edges[key];
            total++;
            if (list.size() == 1) {
              open++;
              open_top = std::max({open_top, pos[(u32)(key >> 32)][1], pos[(u32)key][1]});
              continue;
            }
            if (list.size() > 2) {
              bad = true;
              continue;
            }
            const auto [t1, d1] = list[0];
            const auto [t2, d2] = list[1];
            const u32 other = t1 == t ? t2 : t1;
            // consistent: the two run the edge in opposite directions
            const u8 want = (u8)(flip[t] ^ (d1 == d2 ? 1 : 0));
            if (comp[other] < 0) {
              comp[other] = cid;
              flip[other] = want;
              members.push_back(other);
              stack.push_back(other);
            } else if (comp[other] == cid && flip[other] != want) {
              bad = true;
            }
          }
        }
        // open edges allowed only at the bottom (on the ground: a rock or a house standing on the
        // terrain), up to half of the edges
        float ymin = 1e30f, ymax = -1e30f;
        for (u32 t : members) {
          for (int c = 0; c < 3; c++) {
            ymin = std::min(ymin, pos[w[t][c]][1]);
            ymax = std::max(ymax, pos[w[t][c]][1]);
          }
        }
        const bool open_ok = open * 10 <= total ||
                             (open * 2 <= total &&
                              open_top <= ymin + 0.15f * (ymax - ymin) + 0.5f * 4096.f);
        if (bad || members.size() < 8 || !open_ok) {
          continue;
        }
        // signed and unsigned volume around the surface's centroid
        double cen[3] = {0, 0, 0};
        for (u32 t : members) {
          for (int c = 0; c < 3; c++) {
            for (int k = 0; k < 3; k++) {
              cen[k] += pos[w[t][c]][k];
            }
          }
        }
        for (double& c : cen) {
          c /= 3.0 * members.size();
        }
        double vs = 0, va = 0;
        for (u32 t : members) {
          double p[3][3];
          for (int c = 0; c < 3; c++) {
            for (int k = 0; k < 3; k++) {
              p[c][k] = pos[w[t][c]][k] - cen[k];
            }
          }
          if (flip[t]) {
            for (int k = 0; k < 3; k++) {
              std::swap(p[1][k], p[2][k]);
            }
          }
          const double v = p[0][0] * (p[1][1] * p[2][2] - p[1][2] * p[2][1]) -
                           p[0][1] * (p[1][0] * p[2][2] - p[1][2] * p[2][0]) +
                           p[0][2] * (p[1][0] * p[2][1] - p[1][1] * p[2][0]);
          vs += v;
          va += std::abs(v);
        }
        if (va <= 0 || std::abs(vs) < 0.5 * va) {
          continue;
        }
        const u8 inward = vs < 0 ? 1 : 0;
        for (u32 t : members) {
          auto& tri = tris[ids[t]];
          if (flip[t] ^ inward) {
            std::swap(tri.v[1], tri.v[2]);
            orient_flipped++;
          }
          const auto dk = draw_keys[tri.draw_key];
          tri.draw_key = key_id(DrawKey{dk.mode, dk.texture, (u16)(dk.kind | c3l::kDrawCullBack)});
          orient_culled++;
        }
      }
    }
  }

  void add_tie_tree(tfrag3::TieTree& tree) {
    const size_t tree_first_tri = tris.size();
    tree.unpack();
    u32 base = verts.size();
    for (auto& v : tree.unpacked.vertices) {
      SrcVertex sv;
      sv.x = v.x;
      sv.y = v.y;
      sv.z = v.z;
      sv.s = v.s;
      sv.t = v.t;
      for (int c = 0; c < 4; c++) {
        sv.rgba[c] = tree.colors.read(v.color_index, opt.palette, c);
      }
      verts.push_back(sv);
    }
    // static instances (already in world space), one vis group per instance
    // categories: normal, trans, water, the base draws of envmapped ties (drawn like the others:
    // same colors as tfrag3, see etie_base.vert), then the envmap shine draws (not supported)
    u32 last_draw = tree.category_draw_indices[(int)tfrag3::TieCategory::WATER_ENVMAP + 1];
    if (last_draw == 0) {
      last_draw = tree.static_draws.size();
    }
    for (u32 di = 0; di < tree.static_draws.size() && di < last_draw; di++) {
      const auto& d = tree.static_draws[di];
      const u16 tex = texture_id(d.tree_tex_id);
      note_use(tex, d.mode.as_int(), false);
      u32 key = key_id(DrawKey{d.mode.as_int(), tex, 1});
      size_t idx = d.unpacked.idx_of_first_idx_in_full_buffer;
      for (auto& g : d.vis_groups) {
        size_t first_tri = tris.size();
        cur_vis = vis_id(tree.bvh, g.vis_idx_in_pc_bvh);  // (AI-assisted) up to 8 instances
        add_strips(tree.unpacked.indices, idx, g.num_inds, base, key);
        mark_small(first_tri);
        idx += g.num_inds;
      }
      cur_vis = 0xffff;
    }
    // instances moved by the wind (plants, trees): baked at rest with their instance matrix
    for (const auto& d : tree.instanced_wind_draws) {
      const u16 tex = texture_id(d.tree_tex_id);
      note_use(tex, d.mode.as_int(), false);
      u32 key = key_id(DrawKey{d.mode.as_int(), tex, 1});
      size_t idx = 0;
      for (const auto& g : d.instance_groups) {
        const auto& mat = tree.wind_instance_info.at(g.instance_idx).matrix;
        std::vector<u32> local;  // strip indices into new world space vertices
        std::unordered_map<u32, u32> remap;
        for (size_t i = idx; i < idx + g.num && i < d.vertex_index_stream.size(); i++) {
          u32 vi = d.vertex_index_stream[i];
          if (vi == UINT32_MAX) {
            local.push_back(UINT32_MAX);
            continue;
          }
          auto it = remap.find(vi);
          if (it == remap.end()) {
            const auto& v = tree.unpacked.vertices.at(vi);
            SrcVertex sv;
            sv.x = mat[0].x() * v.x + mat[1].x() * v.y + mat[2].x() * v.z + mat[3].x();
            sv.y = mat[0].y() * v.x + mat[1].y() * v.y + mat[2].y() * v.z + mat[3].y();
            sv.z = mat[0].z() * v.x + mat[1].z() * v.y + mat[2].z() * v.z + mat[3].z();
            sv.s = v.s;
            sv.t = v.t;
            for (int c = 0; c < 4; c++) {
              sv.rgba[c] = tree.colors.read(v.color_index, opt.palette, c);
            }
            it = remap.emplace(vi, verts.size()).first;
            verts.push_back(sv);
          }
          local.push_back(it->second);
        }
        size_t first_tri = tris.size();
        cur_vis = vis_id(tree.bvh, g.vis_idx);  // (AI-assisted) the instance's BVH node
        add_strips(local, 0, local.size(), 0, key);
        cur_vis = 0xffff;
        wind_tris += tris.size() - first_tri;
        mark_small(first_tri);
        idx += g.num;
      }
    }
    if (opt.tie_winding) {
      orient_ties(tree_first_tri, tris.size());  // (AI-assisted)
    }
  }
};

// ---------------- merc ----------------

struct MercOut {
  std::vector<c3l::MercModel> models;
  std::vector<c3l::MercVertex> verts;
  std::vector<u16> indices;
  std::vector<c3l::MercDraw> draws;
  std::vector<c3l::MercBlercVertex> blerc_verts;
  std::vector<c3l::MercBlercTarget> blerc_targets;
  std::vector<u16> blerc_dests;
  // (AI-assisted) v12: vertices the game rewrites (ripple, texture scroll)
  std::vector<c3l::MercModEffect> mod_effects;
  std::vector<c3l::MercModDest> mod_dests;
  std::vector<u8> mod_masks;
  int skipped_models = 0;
  int envmap_draws = 0;
  int gen_lod_models = 0;  // (AI-assisted)
};

/*!
 * (AI-assisted) The simplified copy of a merc model that has only lod0 (see Options::gen_lod_ratio):
 * a model entry named <name> + kMercGenLodSuffix with the model's own vertices (first_vertex,
 * vertex_count) and draws of its own, appended after the model. Each draw's triangles are
 * simplified on their own with their open edges locked (the palette splits and texture seams are
 * borders: nothing moves there, no cracks), so every vertex keeps its draw's palette. Eye draws
 * keep their triangles; the envmap draws use their base draw's simplified triangles. The runtime
 * (CtrLevel) loads both into one mesh; an older runtime loads the copy as a model nobody draws.
 */
/*!
 * (AI-assisted) The merc strips become triangles without the strip's alternating winding (merc
 * draws aren't culled), so neighbours often wind opposite ways, and the simplifier takes every such
 * shared edge for a border it may not move. Turned consistently here, neighbour by neighbour across
 * the edges they share (by position), which the draw doesn't care about.
 */
void orient_triangles(std::vector<unsigned int>& tri, const std::vector<c3l::MercVertex>& verts) {
  const size_t nt = tri.size() / 3;
  std::map<std::array<s16, 3>, u32> pos_id;
  std::vector<u32> p(tri.size());
  for (size_t i = 0; i < tri.size(); i++) {
    const auto& v = verts[tri[i]];
    p[i] = pos_id.emplace(std::array<s16, 3>{v.pos[0], v.pos[1], v.pos[2]}, (u32)pos_id.size())
               .first->second;
  }
  std::map<std::pair<u32, u32>, std::vector<u32>> by_edge;  // undirected edge -> triangles
  for (u32 t = 0; t < nt; t++) {
    for (int e = 0; e < 3; e++) {
      const u32 a = p[3 * t + e], b = p[3 * t + (e + 1) % 3];
      by_edge[{std::min(a, b), std::max(a, b)}].push_back(t);
    }
  }
  // does triangle t have the directed edge a -> b
  auto has_dir = [&](u32 t, u32 a, u32 b) {
    for (int e = 0; e < 3; e++) {
      if (p[3 * t + e] == a && p[3 * t + (e + 1) % 3] == b) {
        return true;
      }
    }
    return false;
  };
  std::vector<u8> seen(nt, 0);
  std::vector<u32> stack;
  for (u32 s = 0; s < nt; s++) {
    if (seen[s]) {
      continue;
    }
    seen[s] = 1;
    stack.push_back(s);
    while (!stack.empty()) {
      const u32 t = stack.back();
      stack.pop_back();
      for (int e = 0; e < 3; e++) {
        const u32 a = p[3 * t + e], b = p[3 * t + (e + 1) % 3];
        for (u32 n : by_edge[{std::min(a, b), std::max(a, b)}]) {
          if (seen[n]) {
            continue;
          }
          seen[n] = 1;
          if (has_dir(n, a, b)) {  // same direction: n is wound the other way
            std::swap(tri[3 * n + 1], tri[3 * n + 2]);
            std::swap(p[3 * n + 1], p[3 * n + 2]);
          }
          stack.push_back(n);
        }
      }
    }
  }
}

bool gen_merc_lod(const c3l::MercModel& mm,
                  const std::vector<c3l::MercVertex>& verts,
                  const Options& opt,
                  MercOut* out,
                  c3l::MercModel* gen) {
  const size_t first_draw = mm.first_draw, end_draw = out->draws.size();
  const size_t first_new_index = out->indices.size();
  std::vector<float> pos(verts.size() * 3);
  for (size_t i = 0; i < verts.size(); i++) {
    for (int c = 0; c < 3; c++) {
      pos[3 * i + c] = verts[i].pos[c] * mm.scale;
    }
  }
  // the triangle ranges of the model's draws (the envmap draws repeat their base draw's): the
  // ones simplified together, and the eye draws (kept)
  std::map<std::pair<u32, u32>, int> range_id;
  std::vector<std::pair<u32, u32>> ranges;
  std::vector<u8> range_eye;
  for (size_t di = first_draw; di < end_draw; di++) {
    const auto& d = out->draws[di];
    const auto key = std::make_pair(d.first_index, d.index_count);
    if (range_id.emplace(key, (int)ranges.size()).second) {
      ranges.push_back(key);
      range_eye.push_back(0);
    }
    range_eye[range_id[key]] |= d.eye_id != 0xff && d.eye_id != c3l::kMercEnvmapDraw;
  }
  // each vertex belongs to one range (the converter gives every palette split its own vertices):
  // its bone indices only mean something there
  std::vector<int> owner(verts.size(), -1);
  for (size_t r = 0; r < ranges.size(); r++) {
    for (u32 i = ranges[r].first; i < ranges[r].first + ranges[r].second; i++) {
      owner[out->indices[i]] = (int)r;
    }
  }
  // the merc fragments repeat their vertices: the same vertex (every attribute, same range) under
  // several indices leaves the triangles as separate pieces whose edges the simplifier must keep.
  // Each index goes to the first such vertex (the pieces join up)
  std::vector<u32> weld(verts.size());
  {
    std::unordered_map<std::string, u32> first;
    for (size_t i = 0; i < verts.size(); i++) {
      std::string k((const char*)&verts[i], sizeof(c3l::MercVertex));
      k.append((const char*)&owner[i], sizeof(int));
      weld[i] = first.emplace(k, (u32)i).first->second;
    }
  }
  // all ranges but the eyes' simplified together: where two draws meet (same positions, other
  // vertices) the simplifier sees a seam and moves both sides alike, so no cracks open there, and
  // a triangle keeps the vertices of its own range. Vertices at an eye's position stay.
  std::vector<unsigned int> in, res;
  std::set<std::array<s16, 3>> eye_pos;
  size_t before = 0;
  for (size_t r = 0; r < ranges.size(); r++) {
    for (u32 i = ranges[r].first; i < ranges[r].first + ranges[r].second; i++) {
      const auto& v = verts[out->indices[i]];
      if (range_eye[r]) {
        eye_pos.insert({v.pos[0], v.pos[1], v.pos[2]});
      } else {
        in.push_back(weld[out->indices[i]]);
      }
    }
    before += range_eye[r] ? 0 : ranges[r].second;
  }
  if (in.size() < 3 * 16) {
    return false;
  }
  std::vector<u8> lock(verts.size(), 0);
  for (size_t i = 0; i < verts.size(); i++) {
    lock[i] = eye_pos.count({verts[i].pos[0], verts[i].pos[1], verts[i].pos[2]}) != 0;
  }
  orient_triangles(in, verts);
  const size_t target = std::max<size_t>((size_t)(in.size() / 3 * opt.gen_lod_ratio) * 3, 3);
  res.resize(in.size());
  size_t n = meshopt_simplifyWithAttributes(res.data(), in.data(), in.size(), pos.data(),
                                            verts.size(), 3 * sizeof(float), nullptr, 0, nullptr,
                                            0, lock.data(), target, opt.gen_lod_error,
                                            opt.gen_lod_lock_border ? meshopt_SimplifyLockBorder : 0,
                                            nullptr);
  res.resize(n);
  // back to the ranges, each in vertex cache order
  std::vector<std::vector<unsigned int>> per_range(ranges.size());
  size_t after = 0;
  for (size_t t = 0; t + 2 < res.size(); t += 3) {
    const int r = owner[res[t]];
    if (r < 0 || owner[res[t + 1]] != r || owner[res[t + 2]] != r) {
      lg::warn("merc model {}: generated low LOD mixes draws, left out", mm.name);
      return false;
    }
    per_range[r].insert(per_range[r].end(), res.begin() + t, res.begin() + t + 3);
    after += 3;
  }
  std::vector<std::pair<u32, u32>> new_range(ranges.size());
  for (size_t r = 0; r < ranges.size(); r++) {
    std::vector<unsigned int> tri;
    if (range_eye[r]) {
      tri.assign(out->indices.begin() + ranges[r].first,
                 out->indices.begin() + ranges[r].first + ranges[r].second);
    } else if (!per_range[r].empty()) {
      tri.resize(per_range[r].size());
      meshopt_optimizeVertexCache(tri.data(), per_range[r].data(), tri.size(), verts.size());
    }
    new_range[r] = {(u32)out->indices.size(), (u32)tri.size()};
    for (unsigned int v : tri) {
      out->indices.push_back((u16)v);
    }
  }
  std::vector<c3l::MercDraw> gdraws;
  for (size_t di = first_draw; di < end_draw; di++) {
    c3l::MercDraw d = out->draws[di];
    const auto& nr = new_range[range_id[{d.first_index, d.index_count}]];
    if (!nr.second) {
      continue;  // (all its triangles gone)
    }
    d.first_index = nr.first;
    d.index_count = nr.second;
    gdraws.push_back(d);
  }
  if (!before || after * 10 > before * 9) {
    // less than 10% fewer triangles: not worth a second set of draws
    lg::debug("merc model {}: no low LOD, {} -> {} triangles", mm.name, before / 3, after / 3);
    out->indices.resize(first_new_index);
    return false;
  }
  *gen = mm;
  const std::string name = std::string(mm.name) + c3l::kMercGenLodSuffix;
  memset(gen->name, 0, sizeof(gen->name));
  strncpy(gen->name, name.c_str(), sizeof(gen->name) - 1);
  gen->first_draw = out->draws.size();
  gen->draw_count = gdraws.size();
  gen->blerc_first = out->blerc_verts.size();
  gen->blerc_count = 0;
  out->draws.insert(out->draws.end(), gdraws.begin(), gdraws.end());
  lg::info("merc model {}: generated low LOD {}, {} -> {} triangles ({:.0f}%)", mm.name, gen->name,
           before / 3, after / 3, 100.0 * after / before);
  out->gen_lod_models++;
  return true;
}

void convert_merc(const tfrag3::MercModelGroup& group, Converter& cv, MercOut* out) {
  // (AI-assisted) skeleton groups with lower LODs: "<base>-lodN" with N > 0 in this level
  std::unordered_map<std::string, bool> has_lower;
  for (const auto& model : group.models) {
    const auto p = model.name.rfind("-lod");
    if (p != std::string::npos && p + 4 < model.name.size()) {
      has_lower[model.name.substr(0, p)] |= model.name.substr(p + 4) != "0";
    }
  }
  for (const auto& model : group.models) {
    c3l::MercModel mm{};
    strncpy(mm.name, model.name.c_str(), sizeof(mm.name) - 1);
    mm.first_vertex = out->verts.size();
    mm.first_draw = out->draws.size();
    size_t idx_start = out->indices.size();

    // position scale for the whole model
    float max_abs = 1.f;
    for (const auto& eff : model.effects) {
      for (const auto& d : eff.all_draws) {
        for (u32 i = d.first_index; i < d.first_index + d.index_count; i++) {
          u32 vi = group.indices[i];
          if (vi == UINT32_MAX) {
            continue;
          }
          for (float p : group.vertices[vi].pos) {
            max_abs = std::max(max_abs, std::abs(p));
          }
        }
      }
    }
    // blend shapes move vertices away from the base pose: room for that
    // (AI-assisted) and so do ripple waves (the game rewrites the positions, see use_mod)
    bool any_blerc = false;
    for (const auto& eff : model.effects) {
      any_blerc |= !eff.mod.mod_draw.empty();
    }
    mm.scale = max_abs * (any_blerc ? 1.5f : 1.f) / 32767.f;
    mm.blerc_first = out->blerc_verts.size();
    const size_t mod_first = out->mod_effects.size(), mod_dest_first = out->mod_dests.size(),
                 mod_mask_first = out->mod_masks.size();

    // model vertices: (source vertex, draw) -> local index
    std::vector<c3l::MercVertex> local_verts;
    bool too_big = false;
    for (size_t ei = 0; ei < model.effects.size(); ei++) {
      const auto& eff = model.effects[ei];
      // Effects with blend shapes (faces): the PC draws the vertices they move from a copy
      // (mod.vertices, drawn by mod_draw, the rest by fix_draw), and so do we, to know which of
      // our vertices a blend shape vertex writes to.
      const bool use_blerc = !eff.mod.mod_draw.empty() && !eff.mod.blerc.int_data.empty();
      // (AI-assisted) v12: the same split for the vertices the game rewrites in its merc data
      // (ripple, texture scroll), so we know which of our vertices each EE vertex goes to
      const bool use_mod = !eff.mod.mod_draw.empty() && !use_blerc &&
                           eff.mod.vertex_lump4_addr.size() == eff.mod.vertices.size() &&
                           !eff.mod.fragment_mask.empty();
      std::unordered_map<u32, std::vector<u16>> mod_local;  // mod vertex -> model vertices
      std::vector<std::pair<const tfrag3::MercDraw*, bool>> draw_list;  // (draw, from mod.vertices)
      if (use_blerc || use_mod) {
        for (const auto& d : eff.mod.fix_draw) {
          draw_list.push_back({&d, false});
        }
        for (const auto& d : eff.mod.mod_draw) {
          draw_list.push_back({&d, true});
        }
      } else {
        for (const auto& d : eff.all_draws) {
          draw_list.push_back({&d, false});
        }
      }
      const size_t effect_first_draw = out->draws.size();
      for (const auto& [dp, from_mod] : draw_list) {
        const auto& d = *dp;
        const std::vector<tfrag3::MercVertex>& vsrc = from_mod ? eff.mod.vertices : group.vertices;
        // Eye draws (eye_id != 0xff) use the texture CtrEyeRenderer draws every frame from the
        // game's eye sprites (iris, pupil, lids). The level texture is only a gray placeholder,
        // used until the first eye frame.
        // strips (or plain triangles for custom models) -> triangles
        std::vector<std::array<u32, 3>> tris;
        if (d.no_strip) {
          for (u32 i = d.first_index; i + 2 < d.first_index + d.index_count; i += 3) {
            tris.push_back({group.indices[i], group.indices[i + 1], group.indices[i + 2]});
          }
        } else {
          std::vector<u32> strip;
          auto flush = [&]() {
            for (size_t i = 2; i < strip.size(); i++) {
              if (strip[i - 2] != strip[i - 1] && strip[i - 1] != strip[i] &&
                  strip[i - 2] != strip[i]) {
                tris.push_back({strip[i - 2], strip[i - 1], strip[i]});
              }
            }
            strip.clear();
          };
          for (u32 i = d.first_index; i < d.first_index + d.index_count; i++) {
            if (group.indices[i] == UINT32_MAX) {
              flush();
            } else {
              strip.push_back(group.indices[i]);
            }
          }
          flush();
        }

        // split into draws that use at most kMercPaletteSize bones
        size_t t = 0;
        while (t < tris.size()) {
          c3l::MercDraw md{};
          md.mode = d.mode.as_int();
          md.texture = cv.texture_id(d.tree_tex_id);  // for eyes: the gray eye placeholder
          cv.note_use(md.texture, md.mode, true);
          cv.note_merc_kind(md.texture, d.eye_id != 0xff, use_blerc, false);
          md.eye_id = d.eye_id;
          md.effect = ei;
          md.first_index = out->indices.size();
          std::vector<u8> palette;
          std::unordered_map<u32, u16> vmap;
          auto bones_of = [&](u32 vi, std::vector<u8>* bones) {
            const auto& v = vsrc[vi];
            for (int k = 0; k < 3; k++) {
              if (v.weights[k] > 0.f &&
                  std::find(bones->begin(), bones->end(), v.mats[k]) == bones->end()) {
                bones->push_back(v.mats[k]);
              }
            }
          };
          for (; t < tris.size(); t++) {
            std::vector<u8> merged = palette;
            for (u32 vi : tris[t]) {
              bones_of(vi, &merged);
            }
            if ((int)merged.size() > c3l::kMercPaletteSize) {
              break;
            }
            palette = merged;
            for (u32 vi : tris[t]) {
              auto it = vmap.find(vi);
              if (it == vmap.end()) {
                const auto& v = vsrc[vi];
                c3l::MercVertex o{};
                for (int c = 0; c < 3; c++) {
                  o.pos[c] = (s16)std::clamp((int)std::lround(v.pos[c] / mm.scale), -32767, 32767);
                }
                int wsum = 0;
                for (int k = 0; k < 3; k++) {
                  int w = std::clamp((int)std::lround(v.weights[k] * 255.f), 0, 255);
                  if (v.weights[k] <= 0.f) {
                    w = 0;
                  }
                  o.weights[k] = w;
                  wsum += w;
                  // unused bones point at palette entry 0 (weight 0, but must be a valid bone)
                  auto pit = std::find(palette.begin(), palette.end(), v.mats[k]);
                  o.bones[k] = (w > 0 && pit != palette.end()) ? (u8)(pit - palette.begin()) : 0;
                }
                if (wsum != 255 && wsum > 0) {
                  o.weights[0] += 255 - wsum;  // keep the sum exact
                }
                o.st[0] = (s16)std::clamp((int)std::lround(v.st[0] * 1024.f), -32768, 32767);
                o.st[1] = (s16)std::clamp((int)std::lround(v.st[1] * 1024.f), -32768, 32767);
                memcpy(o.rgba, v.rgba, 4);
                for (int c = 0; c < 3; c++) {
                  o.normal[c] = (s8)std::clamp((int)std::lround(v.normal[c] * 127.f), -127, 127);
                }
                if (local_verts.size() >= 65535) {
                  too_big = true;
                  break;
                }
                it = vmap.emplace(vi, (u16)local_verts.size()).first;
                if (from_mod) {  // (AI-assisted) blerc or use_mod
                  mod_local[vi].push_back((u16)local_verts.size());
                }
                local_verts.push_back(o);
              }
              out->indices.push_back(it->second);
            }
            if (too_big) {
              break;
            }
          }
          if (palette.empty()) {
            palette.push_back(0);
          }
          md.palette_count = palette.size();
          memcpy(md.palette, palette.data(), palette.size());
          md.index_count = out->indices.size() - md.first_index;
          if (md.index_count) {
            out->draws.push_back(md);
          }
          if (too_big) {
            break;
          }
          if (t < tris.size() && md.index_count == 0) {
            t++;  // a single triangle with > 24 bones: can't happen (max 9), but don't loop
          }
        }
        if (too_big) {
          break;
        }
      }
      if (too_big) {
        break;
      }
      // envmapped effects (shiny: power cells, precursor metal): the envmap pass draws the same
      // triangles again with the envmap texture and mode (emerc.vert / Merc2 envmap draws)
      if (eff.has_envmap) {
        const size_t effect_end = out->draws.size();
        const u16 env_tex = cv.texture_id((s32)eff.envmap_texture);
        cv.note_use(env_tex, eff.envmap_mode.as_int(), true);
        cv.note_merc_kind(env_tex, false, false, true);
        for (size_t di = effect_first_draw; di < effect_end; di++) {
          c3l::MercDraw ed = out->draws[di];
          ed.mode = eff.envmap_mode.as_int();
          ed.texture = env_tex;
          ed.eye_id = c3l::kMercEnvmapDraw;
          out->draws.push_back(ed);
          out->envmap_draws++;
        }
      }
      if (use_mod) {
        c3l::MercModEffect me{};
        me.model = out->models.size();  // this model's index once pushed
        me.effect = ei;
        me.frag_count = eff.mod.fragment_mask.size();
        me.first_mask = out->mod_masks.size();
        for (u8 m : eff.mod.fragment_mask) {
          out->mod_masks.push_back(m ? 1 : 0);
        }
        me.vidx_end = eff.mod.expect_vidx_end;
        me.first_dest = out->mod_dests.size();
        for (const auto& [mvi, locals] : mod_local) {
          const u32 src = eff.mod.vertex_lump4_addr.at(mvi);
          if (src >= me.vidx_end) {
            continue;  // Merc2 keeps these at their base position too
          }
          for (u16 lv : locals) {
            out->mod_dests.push_back({(u16)src, lv});
          }
        }
        // in vertex order: the runtime writes the model's vertices front to back
        std::sort(out->mod_dests.begin() + me.first_dest, out->mod_dests.end(),
                  [](const c3l::MercModDest& a, const c3l::MercModDest& b) {
                    return a.vertex < b.vertex;
                  });
        me.dest_count = out->mod_dests.size() - me.first_dest;
        me.st_vif_add = model.st_vif_add;
        me.st_magic = model.st_magic;
        me.xyz_scale = model.xyz_scale;
        if (me.dest_count) {
          out->mod_effects.push_back(me);
        }
      }
      if (use_blerc) {
        // per vertex: int [target weight indices..., terminator, dest mod vertex],
        // float [base, target offsets...] (tfrag3::Blerc)
        const auto& bl = eff.mod.blerc;
        size_t ii = 0, fi = 0;
        while (ii < bl.int_data.size() && fi < bl.float_data.size()) {
          c3l::MercBlercVertex bv{};
          for (int c = 0; c < 3; c++) {
            bv.base[c] = bl.float_data[fi].v[c];
          }
          fi++;
          bv.first_target = out->blerc_targets.size();
          while (ii < bl.int_data.size() && bl.int_data[ii] != tfrag3::Blerc::kTargetIdxTerminator &&
                 fi < bl.float_data.size()) {
            c3l::MercBlercTarget t{};
            for (int c = 0; c < 3; c++) {
              t.offset[c] = bl.float_data[fi].v[c];
            }
            t.weight = bl.int_data[ii];
            out->blerc_targets.push_back(t);
            ii++;
            fi++;
          }
          ii++;  // terminator
          if (ii >= bl.int_data.size()) {
            break;
          }
          const u32 dest = bl.int_data[ii++];
          bv.target_count = out->blerc_targets.size() - bv.first_target;
          auto dit = mod_local.find(dest);
          if (dit == mod_local.end() || dit->second.empty()) {
            out->blerc_targets.resize(bv.first_target);  // not drawn
            continue;
          }
          bv.first_dest = out->blerc_dests.size();
          bv.dest_count = dit->second.size();
          out->blerc_dests.insert(out->blerc_dests.end(), dit->second.begin(), dit->second.end());
          out->blerc_verts.push_back(bv);
        }
      }
    }
    if (too_big) {
      lg::warn("merc model {} has more than 65535 vertices, skipped", model.name);
      out->indices.resize(idx_start);
      out->draws.resize(mm.first_draw);
      out->blerc_verts.resize(mm.blerc_first);
      out->mod_effects.resize(mod_first);
      out->mod_dests.resize(mod_dest_first);
      out->mod_masks.resize(mod_mask_first);
      out->skipped_models++;
      continue;
    }
    mm.blerc_count = out->blerc_verts.size() - mm.blerc_first;
    mm.vertex_count = local_verts.size();
    mm.draw_count = out->draws.size() - mm.first_draw;
    out->verts.insert(out->verts.end(), local_verts.begin(), local_verts.end());
    out->models.push_back(mm);
    // (AI-assisted) a generated low LOD for "<base>-lod0" without lower LODs (not with blend
    // shapes or rewritten vertices: those move per frame)
    // (not Jak and Daxter: always close and in view, their look stays the PS2's)
    const auto p = model.name.rfind("-lod0");
    if (cv.opt.gen_lod_ratio > 0.f && p != std::string::npos && p + 5 == model.name.size() &&
        model.name.rfind("eichar-", 0) != 0 && model.name.rfind("sidekick-", 0) != 0 &&
        !has_lower[model.name.substr(0, p)] && !mm.blerc_count &&
        out->mod_effects.size() == mod_first) {
      c3l::MercModel gen;
      lg::debug("merc model {}: only lod0, generating a low LOD", model.name);
      if (gen_merc_lod(mm, local_verts, cv.opt, out, &gen)) {
        out->models.push_back(gen);
      }
    }
  }
}

bool is_drawn_tfrag_kind(tfrag3::TFragmentTreeKind k) {
  using K = tfrag3::TFragmentTreeKind;
  return k == K::NORMAL || k == K::TRANS || k == K::DIRT || k == K::ICE || k == K::WATER;
}

// ---------------- textures ----------------

u32 next_pow2(u32 v) {
  u32 p = 1;
  while (p < v) {
    p <<= 1;
  }
  return p;
}

struct TexStats {
  u64 bytes_16bit = 0;  // what the textures took as single level 16-bit textures (c3l v7)
  u64 bytes = 0;
  int etc1 = 0, etc1a4 = 0, rgb565 = 0, rgba4 = 0;
  double psnr_sum = 0, psnr_min = 99;
  int psnr_count = 0;
  // (AI-assisted) merc textures: all, ETC1 / ETC1A4 by the quality test, tested, kept 16-bit as
  // faces or eyes (of the size to be tested), bytes before (all 16-bit) and after
  int merc = 0, merc_etc1 = 0, merc_tested = 0, merc_face = 0;
  u64 merc_bytes_16 = 0, merc_bytes = 0;
};

// (AI-assisted) the ETC1 quality test of a merc texture (Options::etc1_merc_psnr)
struct MercGate {
  bool tested = false, etc1 = false;
  // against the image: PSNR (dB) and the worst 4x4 block's RMS error, of ETC1 and 16-bit
  double psnr = 0, worst_block = 0, psnr_16 = 0, worst_block_16 = 0;
  u32 bytes_16 = 0;  // the 16-bit version's size (with mips)
};

/*!
 * (AI-assisted) A decoded level against the image it was encoded from: PSNR and the largest RMS
 * error of a 4x4 block, over the color of the texels that can be seen (alpha >= 16)
 */
void compare_level(const ctr_tex::Image& ref,
                   const ctr_tex::Image& dec,
                   double* psnr,
                   double* worst_block) {
  double se = 0;
  size_t n = 0;
  *worst_block = 0;
  for (int by = 0; by < ref.h; by += 4) {
    for (int bx = 0; bx < ref.w; bx += 4) {
      double bse = 0;
      int bn = 0;
      for (int y = by; y < by + 4 && y < ref.h; y++) {
        for (int x = bx; x < bx + 4 && x < ref.w; x++) {
          if (ref.at(x, y)[3] < 16) {
            continue;
          }
          for (int c = 0; c < 3; c++) {
            const double d = (double)ref.at(x, y)[c] - dec.at(x, y)[c];
            bse += d * d;
            bn++;
          }
        }
      }
      se += bse;
      n += bn;
      if (bn) {
        *worst_block = std::max(*worst_block, std::sqrt(bse / bn));
      }
    }
  }
  *psnr = (n == 0 || se == 0) ? 99.0 : 10.0 * std::log10(255.0 * 255.0 * n / se);
}

/*!
 * One level texture -> the 3DS layout (see ctr_texture.h): power of two, at most max_tex, with mip
 * levels, in ETC1 / ETC1A4 / RGB565 / RGBA4.
 */
std::vector<u8> convert_texture(const tfrag3::Texture& tex,
                                const Options& opt,
                                const TexUse& use,
                                c3l::Texture* out_desc,
                                double* psnr_out,
                                MercGate* gate) {
  // source RGBA8888, PS2 alpha (0x80 = opaque)
  u32 sw = tex.w, sh = tex.h;
  u32 dw = std::clamp<u32>(next_pow2(sw), 8, opt.max_tex);
  u32 dh = std::clamp<u32>(next_pow2(sh), 8, opt.max_tex);
  ctr_tex::Image img;
  img.w = dw;
  img.h = dh;
  img.rgba.resize(dw * dh * 4);
  for (u32 y = 0; y < dh; y++) {
    for (u32 x = 0; x < dw; x++) {
      // box filter over the source pixels covered by this texel
      u32 x0 = x * sw / dw, x1 = std::max(x0 + 1, (x + 1) * sw / dw);
      u32 y0 = y * sh / dh, y1 = std::max(y0 + 1, (y + 1) * sh / dh);
      u32 acc[4] = {0, 0, 0, 0}, n = 0;
      for (u32 yy = y0; yy < y1 && yy < sh; yy++) {
        for (u32 xx = x0; xx < x1 && xx < sw; xx++) {
          u32 p = tex.data[xx + yy * sw];
          for (int c = 0; c < 4; c++) {
            acc[c] += (p >> (8 * c)) & 0xff;
          }
          n++;
        }
      }
      for (int c = 0; c < 4; c++) {
        img.rgba[4 * (x + y * dw) + c] = n ? acc[c] / n : 0;
      }
    }
  }
  bool has_alpha = false;
  for (u32 i = 0; i < dw * dh; i++) {
    u32 a = std::min(255u, img.rgba[4 * i + 3] * 2u);
    img.rgba[4 * i + 3] = a;
    if (a < 240) {
      has_alpha = true;
    }
  }
  bool etc1 = opt.etc1 && (!use.merc || opt.etc1_merc);
  const ctr_tex::Format fmt16 = has_alpha ? ctr_tex::Format::RGBA4 : ctr_tex::Format::RGB565;
  const ctr_tex::Format fmt_etc = has_alpha ? ctr_tex::Format::ETC1A4 : ctr_tex::Format::ETC1;
  const int levels = opt.mips ? ctr_tex::level_count(dw, dh, 16) : 1;
  ctr_tex::MipOptions mo;
  mo.preserve_coverage = has_alpha && use.alpha_test;
  mo.alpha_ref = std::clamp(use.aref * 2, 1, 255);
  auto mips = ctr_tex::make_mips(img, levels, mo);
  if (has_alpha) {
    for (auto& m : mips) {
      ctr_tex::fill_transparent_colors(&m);
    }
  }
  const int aref = mo.preserve_coverage ? mo.alpha_ref : 0;
  std::vector<u8> out;
  // (AI-assisted) a merc texture: ETC1 if it is about as close to the image as the 16-bit
  // version (Options::etc1_merc_psnr). The full size level is encoded both ways and compared with
  // the image; the kept one starts the output.
  if (!etc1 && opt.etc1 && use.merc && !opt.full_color && opt.etc1_merc_psnr > 0 && gate) {
    for (const auto& m : mips) {
      gate->bytes_16 += ctr_tex::level_bytes(m.w, m.h, fmt16);
    }
    if (!use.face && !use.eye && (int)dw >= opt.etc1_merc_min && (int)dh >= opt.etc1_merc_min) {
      std::vector<u8> e, s;
      ctr_tex::encode_level(mips[0], fmt_etc, opt.etc1_quality, &e, aref, opt.etc1_rdo);
      ctr_tex::encode_level(mips[0], fmt16, opt.etc1_quality, &s, aref);
      compare_level(mips[0], ctr_tex::decode_level(e.data(), dw, dh, fmt_etc), &gate->psnr,
                    &gate->worst_block);
      compare_level(mips[0], ctr_tex::decode_level(s.data(), dw, dh, fmt16), &gate->psnr_16,
                    &gate->worst_block_16);
      gate->tested = true;
      // RGB565 is close to the image (~48 dB): ETC1 must reach etc1_merc_psnr. RGBA4 is ~34 dB
      // (4-bit color bands): ETC1A4 is often closer than it, and passes when it is.
      gate->etc1 = etc1 =
          (gate->psnr >= opt.etc1_merc_psnr || gate->psnr >= gate->psnr_16) &&
          gate->worst_block <= std::max<double>(opt.etc1_merc_block, gate->worst_block_16);
      out = etc1 ? std::move(e) : std::move(s);
    }
  }
  ctr_tex::Format fmt;
  if (opt.full_color) {
    fmt = ctr_tex::Format::RGBA8;
  } else {
    fmt = etc1 ? fmt_etc : fmt16;
  }
  for (size_t l = out.empty() ? 0 : 1; l < mips.size(); l++) {
    ctr_tex::encode_level(mips[l], fmt, opt.etc1_quality, &out, aref, opt.etc1_rdo);
  }
  if (psnr_out) {
    // quality of the first level (visible texels only)
    const auto dec = ctr_tex::decode_level(out.data(), dw, dh, fmt);
    double se = 0;
    size_t n = 0;
    for (u32 i = 0; i < dw * dh; i++) {
      if (mips[0].rgba[4 * i + 3] < 16) {
        continue;
      }
      for (int c = 0; c < 3; c++) {
        const double d = (double)mips[0].rgba[4 * i + c] - dec.rgba[4 * i + c];
        se += d * d;
        n++;
      }
    }
    *psnr_out = (n == 0 || se == 0) ? 99.0 : 10.0 * std::log10(255.0 * 255.0 * n / se);
  }
  out_desc->w = dw;
  out_desc->h = dh;
  out_desc->format = (u8)fmt;
  out_desc->levels = (u8)mips.size();
  return out;
}

// ---------------- output ----------------

void align16(std::vector<u8>& buf) {
  while (buf.size() % 16) {
    buf.push_back(0);
  }
}

template <typename T>
u32 append(std::vector<u8>& buf, const T* data, size_t count) {
  align16(buf);
  u32 off = buf.size();
  const u8* p = (const u8*)data;
  buf.insert(buf.end(), p, p + sizeof(T) * count);
  return off;
}

// (AI-assisted) The time of day palette to bake when none is given. Outdoor levels go through the
// day (update-mood-palette): palette 1, a daytime one. The interiors' moods (mood.gc) always use
// palette 0 at weight 1 and add the others only as lights or fades on top; their palette 1 can be
// black (jungleb's tie: the whole level was drawn black).
int auto_palette(const std::string& level) {
  static const char* kFixedMood[] = {"jungleb", "maincave", "darkcave",
                                     "robocave", "lavatube", "citadel"};
  for (const char* n : kFixedMood) {
    if (level == n) {
      return 0;
    }
  }
  return 1;
}

// (AI-assisted) Interiors (caves, the sunken city, the citadel): no far views, so no vista tier
// (it would only take linear memory, 50-400 KB per level)
bool is_interior(const std::string& level) {
  static const char* kInterior[] = {"jungleb", "maincave", "darkcave", "robocave",
                                    "sunken",  "sunkenb",  "citadel"};
  for (const char* n : kInterior) {
    if (level == n) {
      return true;
    }
  }
  return false;
}

bool convert(const fs::path& in, const fs::path& out, const Options& opt_in) {
  Options opt = opt_in;
  if (is_interior(in.stem().string())) {
    opt.vista = false;
  }
  if (opt.palette < 0) {
    opt.palette = auto_palette(in.stem().string());
  }
  // (AI-assisted) the title screen's textures at their full size: the logo's "The Precursor
  // Legacy" lettering (256x64) was cut to 128 wide and unreadable. The whole level is ~200 KB.
  if (in.stem().string() == "title") {
    opt.max_tex = std::max(opt.max_tex, 512);
    opt.full_color = true;
  }
  auto data = file_util::read_binary_file(in);
  auto decomp = compression::decompress_zstd(data.data(), data.size());
  Serializer ser(decomp.data(), decomp.size());
  tfrag3::Level level;
  level.serialize(ser);

  Converter cv;
  cv.opt = opt;
  cv.level = &level;
  int tfrag_trees = 0, tie_trees = 0;
  MercOut merc;
  if (!opt.no_merc) {
    convert_merc(level.merc_data, cv, &merc);
  }
  const bool two_tiers = opt.far_tfrag_geo >= 0 && opt.far_tfrag_geo != opt.tfrag_geo;
  cv.cur_tier = two_tiers ? 1 : 0;
  {
    // (AI-assisted) the kinds matter for the 3DS: the game sends the camera for a level's
    // background with its normal tfrag tree only (see CtrTfragRenderer)
    std::string kinds;
    for (auto& tree : level.tfrag_trees[opt.tfrag_geo]) {
      kinds += fmt::format("{}{} ({} draws)", kinds.empty() ? "" : ", ",
                           tfrag3::tfrag_tree_names[(int)tree.kind], tree.draws.size());
    }
    lg::info("{}: tfrag trees: {}", in.stem().string(), kinds.empty() ? "none" : kinds);
    // the baked time of day palette (--palette) against the others: average vertex brightness
    auto palette_line = [&](const char* what, const tfrag3::PackedTimeOfDay& tod) {
      std::string line;
      for (int p = 0; p < 8; p++) {
        double sum = 0;
        for (u32 c = 0; c < tod.color_count; c++) {
          sum += tod.read(c, p, 0) + tod.read(c, p, 1) + tod.read(c, p, 2);
        }
        line += fmt::format(" {}{:.0f}", p == opt.palette ? "*" : "",
                            tod.color_count ? sum / (3.0 * tod.color_count) : 0.0);
      }
      lg::info("{}: {} time of day palettes (avg rgb, * = baked):{}", in.stem().string(), what, line);
    };
    for (auto& tree : level.tfrag_trees[opt.tfrag_geo]) {
      palette_line(tfrag3::tfrag_tree_names[(int)tree.kind], tree.colors);
    }
    for (auto& tree : level.tie_trees[opt.tie_geo]) {
      palette_line("tie", tree.colors);
    }
  }
  for (auto& tree : level.tfrag_trees[opt.tfrag_geo]) {
    if (opt.merc_only) {
      break;
    }
    if (is_drawn_tfrag_kind(tree.kind)) {
      cv.add_tfrag_tree(tree);
      tfrag_trees++;
    }
  }
  size_t far_first = cv.tris.size();
  const char* far_source = "none";
  if (two_tiers && !opt.merc_only) {
    // coarse version of each grid cell: the coarsest tfrag level of detail
    cv.cur_tier = 2;
    for (auto& tree : level.tfrag_trees[opt.far_tfrag_geo]) {
      if (is_drawn_tfrag_kind(tree.kind)) {
        cv.add_tfrag_tree(tree);
      }
    }
    far_source = "lod";
    // the game's low resolution tfrag (drawn by the PS2 when the level is seen from another
    // level; often only the parts visible from there): used when the camera is outside the level
    cv.cur_tier = 3;
    using K = tfrag3::TFragmentTreeKind;
    for (auto& tree : level.tfrag_trees[0]) {
      if (tree.kind == K::LOWRES || tree.kind == K::LOWRES_TRANS) {
        cv.add_tfrag_tree(tree);
        far_source = "lod + lowres";
      }
    }
  }
  const size_t far_tris = cv.tris.size() - far_first;
  cv.cur_tier = 0;
  if (!opt.no_tie && !opt.merc_only) {
    for (auto& tree : level.tie_trees[opt.tie_geo]) {
      cv.add_tie_tree(tree);
      tie_trees++;
    }
  }
  int shrub_trees = 0;
  if (!opt.no_shrub && !opt.merc_only) {
    for (auto& tree : level.shrub_trees) {
      cv.add_shrub_tree(tree);
      shrub_trees++;
    }
  }

  // ---- chunking: grid cells by triangle centroid ----
  // shrubs are small and only drawn up close: smaller cells cull them better
  auto cell_of_detail = [&](int detail) {
    return (detail == 3 ? opt.shrub_cell : opt.cell_meters) * 4096.f;
  };
  // (detail, cell, kind) -> triangle indices. (AI-assisted) The kind (Draw::kind: tfrag, tie,
  // shrub) keeps every chunk to one (Chunk::kind); with the default tiers it splits nothing.
  using CellKey = std::tuple<int, int, int, int, int>;
  std::map<CellKey, std::vector<u32>> cells;
  for (u32 i = 0; i < cv.tris.size(); i++) {
    const auto& t = cv.tris[i];
    float cx = 0, cy = 0, cz = 0;
    for (int k = 0; k < 3; k++) {
      cx += cv.verts[t.v[k]].x;
      cy += cv.verts[t.v[k]].y;
      cz += cv.verts[t.v[k]].z;
    }
    const float cell = cell_of_detail((int)t.detail);
    auto key = std::make_tuple((int)(t.detail + 4 * t.tier), (int)std::floor(cx / 3 / cell),
                               (int)std::floor(cy / 3 / cell), (int)std::floor(cz / 3 / cell),
                               (int)(cv.draw_keys[t.draw_key].kind & 0xff));
    cells[key].push_back(i);
  }

  // (AI-assisted) The chunks' triangles and vertices first: their quantizing grids depend on each
  // other (see below).
  struct ChunkPlan {
    CellKey key;
    std::vector<u32> tris;               // triangle ids, in draw order
    std::vector<u32> src;                // source vertices in local order
    std::unordered_map<u32, u16> local;  // source vertex -> local index
    double grid = 1;                     // quantizing step (game units), a power of two
    double origin[3] = {0, 0, 0};        // a multiple of grid (and a float)
    double old_step = 0;  // the step of the per-chunk bounding box grid used before (stats)
  };
  std::vector<ChunkPlan> plans;
  // (AI-assisted) per vertex: a vista copy (its own quantizing grids, see PosKey)
  std::vector<u8> vista_vertex(cv.verts.size(), 0);
  // (AI-assisted) a cell's triangles -> plans of at most 65535 vertices
  auto make_plans = [&](std::map<CellKey, std::vector<u32>>& cell_map) {
    for (auto& [key, tri_ids] : cell_map) {
      // sort by draw so each chunk has one draw per (mode, texture); (AI-assisted) then by
      // visibility id, so a draw's triangles of one drawable are one run (c3l::VisRun)
      std::stable_sort(tri_ids.begin(), tri_ids.end(), [&](u32 a, u32 b) {
        const auto& ta = cv.tris[a];
        const auto& tb = cv.tris[b];
        return ta.draw_key != tb.draw_key ? ta.draw_key < tb.draw_key : ta.vis < tb.vis;
      });
      size_t pos = 0;
      while (pos < tri_ids.size()) {
        // gather triangles until 65535 unique vertices
        ChunkPlan cp;
        cp.key = key;
        size_t end = pos;
        while (end < tri_ids.size()) {
          const auto& t = cv.tris[tri_ids[end]];
          int new_verts = 0;
          for (int k = 0; k < 3; k++) {
            new_verts += cp.local.count(t.v[k]) ? 0 : 1;
          }
          if (cp.src.size() + new_verts > 65535) {
            break;
          }
          for (int k = 0; k < 3; k++) {
            if (!cp.local.count(t.v[k])) {
              cp.local[t.v[k]] = cp.src.size();
              cp.src.push_back(t.v[k]);
            }
          }
          end++;
        }
        cp.tris.assign(tri_ids.begin() + pos, tri_ids.begin() + end);
        plans.push_back(std::move(cp));
        pos = end;
      }
    }
  };
  make_plans(cells);
  // (AI-assisted) the vista tier (Options::vista): copies of the far content, in large cells
  const size_t normal_plans = plans.size();
  size_t vista_src_tris = 0, vista_tris = 0;
  if (opt.vista && !opt.merc_only) {
    bool has_lowres = false;
    for (const auto& cp : plans) {
      has_lowres |= std::get<0>(cp.key) / 4 == 3;
    }
    const float vcell = opt.vista_cell * 4096.f;
    std::map<CellKey, std::vector<u32>> vcells;
    for (size_t pi = 0; pi < normal_plans && pi < 0xffff; pi++) {
      const auto& cp = plans[pi];
      const int tier = std::get<0>(cp.key) / 4, detail = std::get<0>(cp.key) % 4;
      const int kind = std::get<4>(cp.key);  // Draw::kind: 0 tfrag, 1 tie, 2 shrub
      const bool lowres = tier == 3;
      const bool landmark = tier == 0 && kind == 1 && detail == 0;
      const bool coarse = tier == 2 && kind == 0 && !has_lowres;
      if (!lowres && !landmark && !coarse) {
        continue;
      }
      // by vista cell and draw: simplified on their own (each copy stays one source chunk's)
      std::map<std::tuple<int, int, u32>, std::vector<u32>> groups;
      for (u32 t : cp.tris) {
        const auto& tr = cv.tris[t];
        float cx = 0, cz = 0;
        for (int k = 0; k < 3; k++) {
          cx += cv.verts[tr.v[k]].x / 3;
          cz += cv.verts[tr.v[k]].z / 3;
        }
        groups[{(int)std::floor(cx / vcell), (int)std::floor(cz / vcell), tr.draw_key}].push_back(t);
      }
      for (auto& [gk, ids] : groups) {
        vista_src_tris += ids.size();
        // welded by position (the representative's texture coordinates and color are kept)
        std::unordered_map<u64, u32> weld;
        std::vector<float> pos;
        std::vector<u32> rep_src, idx;
        auto key_of = [](const SrcVertex& v) {
          u32 a, b, c;
          memcpy(&a, &v.x, 4);
          memcpy(&b, &v.y, 4);
          memcpy(&c, &v.z, 4);
          return ((u64)a * 0x9E3779B97F4A7C15ull) ^ ((u64)b * 0xC2B2AE3D27D4EB4Full) ^ c;
        };
        for (u32 t : ids) {
          for (int k = 0; k < 3; k++) {
            const u32 vi = cv.tris[t].v[k];
            const auto& v = cv.verts[vi];
            auto [it, fresh] = weld.emplace(key_of(v), (u32)rep_src.size());
            if (fresh) {
              rep_src.push_back(vi);
              pos.insert(pos.end(), {v.x, v.y, v.z});
            }
            idx.push_back(it->second);
          }
        }
        std::vector<u32> out_idx = idx;
        if (!lowres && opt.vista_error > 0 && idx.size() > 3) {
          out_idx.resize(idx.size());
          const size_t n = meshopt_simplify(out_idx.data(), idx.data(), idx.size(), pos.data(),
                                            rep_src.size(), 12, 0, opt.vista_error * 4096.f,
                                            meshopt_SimplifyErrorAbsolute, nullptr);
          out_idx.resize(n);
          const size_t want = (size_t)(idx.size() * opt.vista_ratio) / 3 * 3;
          if (n > want && want >= 3 && opt.vista_sloppy_error > 0) {
            float ext = 1.f;
            for (int c = 0; c < 3; c++) {
              float lo = 1e30f, hi = -1e30f;
              for (size_t v = 0; v < rep_src.size(); v++) {
                lo = std::min(lo, pos[3 * v + c]);
                hi = std::max(hi, pos[3 * v + c]);
              }
              ext = std::max(ext, hi - lo);
            }
            std::vector<u32> sl(n);
            const size_t m =
                meshopt_simplifySloppy(sl.data(), out_idx.data(), n, pos.data(), rep_src.size(), 12,
                                       nullptr, want, opt.vista_sloppy_error * 4096.f / ext,
                                       nullptr);
            if (m >= 3) {
              sl.resize(m);
              out_idx = sl;
            }
          }
        }
        // new vertices (their quantizing grid is the vista cell's alone)
        std::unordered_map<u32, u32> made;
        for (size_t i = 0; i + 2 < out_idx.size(); i += 3) {
          SrcTri nt = cv.tris[ids[0]];
          for (int k = 0; k < 3; k++) {
            const u32 r = out_idx[i + k];
            auto [it, fresh] = made.emplace(r, (u32)cv.verts.size());
            if (fresh) {
              cv.verts.push_back(cv.verts[rep_src[r]]);
              vista_vertex.resize(cv.verts.size(), 0);
              vista_vertex.back() = 1;
            }
            nt.v[k] = it->second;
          }
          nt.detail = 0;
          nt.tier = c3l::kVistaTier;
          nt.vis = (u16)pi;  // the source chunk
          vcells[std::make_tuple((int)(4 * c3l::kVistaTier), std::get<0>(gk), 0, std::get<1>(gk),
                                 (int)(c3l::CHUNK_KIND_VISTA - c3l::CHUNK_KIND_TFRAG))]
              .push_back((u32)cv.tris.size());
          cv.tris.push_back(nt);
          vista_tris++;
        }
      }
    }
    make_plans(vcells);
  }

  // ---- (AI-assisted) quantizing grids ----
  // A chunk's positions are on a grid of a power of two game units: the finest whose signed
  // 16-bit range around the chunk's centre (a multiple of the grid) holds the chunk. A position
  // that is in several chunks (a surface crossing a cell border, a cell split at 65535 vertices,
  // the two tfrag tiers) is first put on the coarsest of their grids: a multiple of each one's
  // own, so every chunk holding it reconstructs it exactly (origin + pos * scale) and surfaces
  // stay closed. Before, each chunk had its own bounding box grid (minimum + 0..32767 steps of
  // extent / 32767), the same vertex landed 3-9 mm apart in neighbouring chunks, and the
  // background showed through the cracks.
  struct PosKey {
    u32 x, y, z;
    u32 vista;  // (AI-assisted) vista copies don't share grids with the chunks they copy
    bool operator==(const PosKey& o) const {
      return x == o.x && y == o.y && z == o.z && vista == o.vista;
    }
  };
  struct PosKeyHash {
    size_t operator()(const PosKey& k) const {
      return (size_t)k.x * 0x9E3779B97F4A7C15ull ^ (size_t)k.y * 0xC2B2AE3D27D4EB4Full ^
             (size_t)k.z * 0x165667B19E3779F9ull ^ k.vista;
    }
  };
  auto pos_key = [&](u32 s) {
    const auto& v = cv.verts[s];
    auto bits = [](float f) {
      u32 b;
      f = f == 0.f ? 0.f : f;  // -0 and +0: one position
      memcpy(&b, &f, 4);
      return b;
    };
    return PosKey{bits(v.x), bits(v.y), bits(v.z), s < vista_vertex.size() ? vista_vertex[s] : 0u};
  };
  struct Snap {
    double grid = 0;  // the coarsest grid of the chunks holding the position
    u32 last = UINT32_MAX, chunks = 0;
  };
  std::unordered_map<PosKey, Snap, PosKeyHash> snaps;
  auto on_grid = [](float p, double grid) { return std::round((double)p / grid) * grid; };
  for (auto& cp : plans) {
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    for (u32 s : cp.src) {
      const auto& v = cv.verts[s];
      const float p[3] = {v.x, v.y, v.z};
      for (int c = 0; c < 3; c++) {
        mn[c] = std::min(mn[c], p[c]);
        mx[c] = std::max(mx[c], p[c]);
      }
    }
    const float extent = std::max({mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2], 1.f});
    cp.old_step = extent / 32767.f;
    // 65532 steps: room for the centre's rounding to the grid
    cp.grid = std::exp2(std::ceil(std::log2(extent / 65532.0)));
  }
  int grid_rounds = 0;
  for (bool changed = true; changed; grid_rounds++) {
    changed = false;
    snaps.clear();
    for (u32 ci = 0; ci < plans.size(); ci++) {
      for (u32 s : plans[ci].src) {
        auto& sn = snaps[pos_key(s)];
        sn.grid = std::max(sn.grid, plans[ci].grid);
        if (sn.last != ci) {
          sn.last = ci;
          sn.chunks++;
        }
      }
    }
    // the origin: the centre of the snapped positions on the chunk's grid. A coarser snap can
    // push a position out of the 16-bit range: then the chunk's grid doubles, and all again.
    for (auto& cp : plans) {
      double mn[3] = {1e300, 1e300, 1e300}, mx[3] = {-1e300, -1e300, -1e300};
      for (u32 s : cp.src) {
        const auto& v = cv.verts[s];
        const double g = snaps.at(pos_key(s)).grid;
        const double p[3] = {on_grid(v.x, g), on_grid(v.y, g), on_grid(v.z, g)};
        for (int c = 0; c < 3; c++) {
          mn[c] = std::min(mn[c], p[c]);
          mx[c] = std::max(mx[c], p[c]);
        }
      }
      bool fits = true;
      for (int c = 0; c < 3; c++) {
        // as a float (the file's): still a multiple of the grid (a float that isn't one is
        // further than 2^24 grids from 0, where floats are multiples of 2 grids)
        const double o = (double)(float)(std::round((mn[c] + mx[c]) * 0.5 / cp.grid) * cp.grid);
        cp.origin[c] = o;
        fits = fits && (mx[c] - o) / cp.grid <= 32767.0 && (mn[c] - o) / cp.grid >= -32768.0;
      }
      if (!fits) {
        cp.grid *= 2;
        changed = true;
      }
    }
  }
  // stats: steps before / after, positions in several chunks and how they reconstruct
  struct QuantStats {
    std::vector<double> old_steps, new_steps;
    size_t coarser_chunks = 0;  // grid coarser than the old step (none expected)
    size_t shared = 0;          // positions in more than one chunk
    size_t shared_mismatch = 0;  // ... reconstructed differently by two of them (must be 0)
    size_t snapped_coarser = 0;  // vertices on a coarser grid than their chunk's (shared)
    double old_err = 0, new_err = 0;  // largest position error (game units)
  } qs;
  for (const auto& cp : plans) {
    qs.old_steps.push_back(cp.old_step);
    qs.new_steps.push_back(cp.grid);
    qs.coarser_chunks += cp.grid > cp.old_step;
  }
  for (const auto& [k, sn] : snaps) {
    qs.shared += sn.chunks > 1;
  }
  std::unordered_map<PosKey, std::array<double, 3>, PosKeyHash> shared_pos;

  std::vector<c3l::Chunk> chunks;
  std::vector<c3l::Vertex> out_verts;
  std::vector<u16> out_indices;
  std::vector<c3l::Draw> out_draws;
  // (AI-assisted) visibility runs of all chunks (c3l::VisRun), and each chunk's first one
  std::vector<c3l::VisRun> out_runs;
  std::vector<u32> chunk_first_run;
  size_t vis_tris = 0;
  VcacheStats vc_before, vc_after;
  size_t welded_verts = 0;

  for (const auto& cp : plans) {
    {
      const auto& key = cp.key;
      const auto& local = cp.local;
      const auto& local_src = cp.src;

      c3l::Chunk ch{};
      const int detail = std::get<0>(key) % 4;
      ch.lod_tier = std::get<0>(key) / 4;
      const float cell =
          ch.lod_tier == c3l::kVistaTier ? opt.vista_cell * 4096.f : cell_of_detail(detail);
      ch.lod_center[0] = (std::get<1>(key) + 0.5f) * cell;
      ch.lod_center[1] = (std::get<2>(key) + 0.5f) * cell;
      ch.lod_center[2] = (std::get<3>(key) + 0.5f) * cell;
      ch.max_dist = detail == 1   ? opt.detail_dist * 4096.f
                    : detail == 2 ? opt.medium_dist * 4096.f
                    : detail == 3 ? opt.shrub_dist * 4096.f
                                  : 0.f;
      // (AI-assisted) Draw::kind 0 tfrag, 1 tie, 2 shrub
      ch.kind = (u32)std::get<4>(key) + c3l::CHUNK_KIND_TFRAG;
      // (AI-assisted) the positions on their grid (see "quantizing grids"), the old quantizer's
      // error for the stats
      std::vector<std::array<double, 3>> snapped(local_src.size());
      double mn[3] = {1e300, 1e300, 1e300}, mx[3] = {-1e300, -1e300, -1e300};
      double omn[3] = {1e300, 1e300, 1e300};
      for (size_t i = 0; i < local_src.size(); i++) {
        const auto& v = cv.verts[local_src[i]];
        const double g = snaps.at(pos_key(local_src[i])).grid;
        const float p[3] = {v.x, v.y, v.z};
        qs.snapped_coarser += g > cp.grid;
        for (int c = 0; c < 3; c++) {
          snapped[i][c] = on_grid(p[c], g);
          mn[c] = std::min(mn[c], snapped[i][c]);
          mx[c] = std::max(mx[c], snapped[i][c]);
          omn[c] = std::min(omn[c], (double)p[c]);
          qs.new_err = std::max(qs.new_err, std::abs(snapped[i][c] - p[c]));
        }
      }
      for (size_t i = 0; i < local_src.size(); i++) {
        const auto& v = cv.verts[local_src[i]];
        const float p[3] = {v.x, v.y, v.z};
        const float old_scale = (float)cp.old_step;
        for (int c = 0; c < 3; c++) {
          const float o = (float)omn[c];
          const int q = std::clamp((int)std::lround((p[c] - o) / old_scale), 0, 32767);
          qs.old_err = std::max(qs.old_err, std::abs((double)o + q * (double)old_scale - p[c]));
        }
      }
      ch.scale = (float)cp.grid;
      float rad = 0;
      for (int c = 0; c < 3; c++) {
        ch.origin[c] = (float)cp.origin[c];
        ch.bsphere[c] = (float)((mn[c] + mx[c]) / 2);
      }
      for (const auto& p : snapped) {
        const double dx = p[0] - ch.bsphere[0], dy = p[1] - ch.bsphere[1],
                     dz = p[2] - ch.bsphere[2];
        rad = std::max(rad, (float)std::sqrt(dx * dx + dy * dy + dz * dz));
      }
      ch.bsphere[3] = rad;
      ch.first_vertex = out_verts.size();
      ch.vertex_count = local_src.size();
      for (size_t i = 0; i < local_src.size(); i++) {
        const auto& v = cv.verts[local_src[i]];
        c3l::Vertex o{};
        std::array<double, 3> rec;
        for (int c = 0; c < 3; c++) {
          // an integer in the signed 16-bit range (the grids were chosen so)
          const double q = (snapped[i][c] - cp.origin[c]) / cp.grid;
          o.pos[c] = (s16)std::clamp((long)std::lround(q), -32768l, 32767l);
          // what the 3DS reconstructs, exactly: origin + pos * scale
          rec[c] = (double)ch.origin[c] + (double)o.pos[c] * (double)ch.scale;
        }
        if (snaps.at(pos_key(local_src[i])).chunks > 1) {
          auto [it, fresh] = shared_pos.emplace(pos_key(local_src[i]), rec);
          qs.shared_mismatch += !fresh && it->second != rec;
        }
        o.st[0] = (s16)std::clamp((int)std::lround(v.s * 1024.f), -32768, 32767);
        o.st[1] = (s16)std::clamp((int)std::lround(v.t * 1024.f), -32768, 32767);
        memcpy(o.rgba, v.rgba, 4);
        out_verts.push_back(o);
      }
      ch.first_draw = out_draws.size();
      u32 cur_key = UINT32_MAX;
      std::vector<c3l::VisRun> runs;
      bool any_vis = false;
      for (size_t i = 0; i < cp.tris.size(); i++) {
        const auto& t = cv.tris[cp.tris[i]];
        const bool new_draw = t.draw_key != cur_key;
        if (new_draw) {
          cur_key = t.draw_key;
          const auto& dk = cv.draw_keys[cur_key];
          c3l::Draw d{};
          d.mode = dk.mode;
          d.texture = dk.texture;
          d.kind = dk.kind;
          d.first_index = out_indices.size();
          d.index_count = 0;
          out_draws.push_back(d);
        }
        for (int k = 0; k < 3; k++) {
          out_indices.push_back(local.at(t.v[k]));
        }
        out_draws.back().index_count += 3;
        if (new_draw || runs.back().vis != t.vis) {
          runs.push_back(c3l::VisRun{(u16)(out_draws.size() - 1 - ch.first_draw), t.vis, 0});
        }
        runs.back().index_count += 3;
        if (t.vis != 0xffff) {
          any_vis = true;
          vis_tris++;
        }
      }
      ch.draw_count = out_draws.size() - ch.first_draw;
      // (AI-assisted) vertex cache order (meshoptimizer). Only inside a visibility run (the runs
      // are drawn on their own) of a draw without blending that writes depth (blended draws and
      // decals keep the original triangle order). Then the chunk's vertices in first-use order.
      for (u32 di = ch.first_draw; di < ch.first_draw + ch.draw_count; di++) {
        vc_before.add(&out_indices[out_draws[di].first_index], out_draws[di].index_count,
                      ch.vertex_count);
      }
      if (!out_draws.empty() && ch.draw_count && opt.vcache) {
        // the same vertex (after quantizing) from different strips: one vertex, so the cache can
        // reuse it across strips (the PS2 strips share none)
        const u32 i0 = out_draws[ch.first_draw].first_index;
        const u32 i1 = out_draws.back().first_index + out_draws.back().index_count;
        std::unordered_map<std::string, u16> seen;
        std::vector<u16> weld(ch.vertex_count);
        u32 kept = 0;
        for (u32 v = 0; v < ch.vertex_count; v++) {
          const auto& vx = out_verts[ch.first_vertex + v];
          auto [it, fresh] =
              seen.emplace(std::string((const char*)&vx, sizeof(vx)), (u16)kept);
          if (fresh) {
            out_verts[ch.first_vertex + kept++] = vx;
          }
          weld[v] = it->second;
        }
        for (u32 i = i0; i < i1; i++) {
          out_indices[i] = weld[out_indices[i]];
        }
        welded_verts += ch.vertex_count - kept;
        out_verts.resize(ch.first_vertex + kept);
        ch.vertex_count = kept;
      }
      if (!out_draws.empty() && ch.draw_count) {
        const u32 nv = ch.vertex_count;
        u32 ri = 0;
        for (u32 di = ch.first_draw; di < ch.first_draw + ch.draw_count; di++) {
          const auto& d = out_draws[di];
          DrawMode mode;
          mode.as_int() = d.mode;
          const bool any_order = !mode.get_ab_enable() && mode.get_depth_write_enable();
          u32 idx = d.first_index;
          for (; ri < runs.size() && runs[ri].draw == di - ch.first_draw; ri++) {
            const u32 n = runs[ri].index_count;
            if (any_order && opt.vcache) {
              std::vector<unsigned int> in(&out_indices[idx], &out_indices[idx] + n), o(n);
              meshopt_optimizeVertexCache(o.data(), in.data(), n, nv);
              std::copy(o.begin(), o.end(), &out_indices[idx]);
            }
            idx += n;
          }
          vc_after.add(&out_indices[d.first_index], d.index_count, nv);
        }
        if (opt.vcache) {
          const u32 i0 = out_draws[ch.first_draw].first_index;
          const u32 i1 = out_draws.back().first_index + out_draws.back().index_count;
          std::vector<unsigned int> idx(&out_indices[i0], &out_indices[i0] + (i1 - i0)), remap(nv);
          if (meshopt_optimizeVertexFetchRemap(remap.data(), idx.data(), idx.size(), nv) != nv) {
            for (u32 v = 0; v < nv; v++) {
              remap[v] = v;  // unused vertices (never: every vertex comes from a triangle)
            }
          }
          std::vector<c3l::Vertex> moved(nv);
          for (u32 v = 0; v < nv; v++) {
            moved[remap[v]] = out_verts[ch.first_vertex + v];
          }
          std::copy(moved.begin(), moved.end(), out_verts.begin() + ch.first_vertex);
          for (u32 i = i0; i < i1; i++) {
            out_indices[i] = (u16)remap[out_indices[i]];
          }
        }
      }
      // the runs only if some can be hidden (the offset is made absolute at write)
      chunk_first_run.push_back(any_vis ? (u32)out_runs.size() : 0);
      ch.vis_run_count = any_vis ? (u32)runs.size() : 0;
      if (any_vis) {
        out_runs.insert(out_runs.end(), runs.begin(), runs.end());
      }
      chunks.push_back(ch);
    }
  }
  if (qs.shared_mismatch) {
    lg::error("{}: {} positions in several chunks reconstruct differently", in.stem().string(),
              qs.shared_mismatch);
    return false;
  }

  // ---- textures (in parallel: ETC1 encoding is slow) ----
  std::vector<c3l::Texture> tex_descs(cv.tex_order.size());
  std::vector<std::vector<u8>> tex_datas(cv.tex_order.size());
  std::vector<double> tex_psnr(cv.tex_order.size(), 99.0);
  std::vector<MercGate> tex_gate(cv.tex_order.size());
  {
    BS::thread_pool pool(opt.threads > 0 ? opt.threads : std::thread::hardware_concurrency());
    pool.push_loop(cv.tex_order.size(), [&](size_t a, size_t b) {
      for (size_t i = a; i < b; i++) {
        tex_descs[i] = c3l::Texture{};
        tex_datas[i] = convert_texture(level.textures.at(cv.tex_order[i]), opt, cv.tex_use[i],
                                       &tex_descs[i], &tex_psnr[i], &tex_gate[i]);
      }
    });
    pool.wait_for_tasks();
  }
  // (AI-assisted) textures made smaller than their original (max_tex): a blurry detail shows here
  for (size_t i = 0; i < cv.tex_order.size(); i++) {
    const auto& t = level.textures.at(cv.tex_order[i]);
    if ((int)t.w > opt.max_tex || (int)t.h > opt.max_tex) {
      lg::debug("{}: texture {} ({}) {}x{} reduced to at most {}{}", in.stem().string(),
                t.debug_name, t.debug_tpage_name, t.w, t.h, opt.max_tex,
                cv.tex_use[i].merc ? " (merc)" : "");
    }
  }
  TexStats tstats;
  for (size_t i = 0; i < tex_descs.size(); i++) {
    const auto& d = tex_descs[i];
    tstats.bytes_16bit += (u64)d.w * d.h * 2;
    tstats.bytes += tex_datas[i].size();
    switch (d.format) {
      case c3l::TEX_ETC1:
        tstats.etc1++;
        break;
      case c3l::TEX_ETC1A4:
        tstats.etc1a4++;
        break;
      case c3l::TEX_RGB565:
        tstats.rgb565++;
        break;
      default:
        tstats.rgba4++;
        break;
    }
    if (d.format == c3l::TEX_ETC1 || d.format == c3l::TEX_ETC1A4) {
      tstats.psnr_sum += tex_psnr[i];
      tstats.psnr_min = std::min(tstats.psnr_min, tex_psnr[i]);
      tstats.psnr_count++;
    }
    // (AI-assisted) merc textures and their ETC1 quality test
    const auto& u = cv.tex_use[i];
    const auto& g = tex_gate[i];
    if (u.merc && g.bytes_16) {
      const auto& t = level.textures.at(cv.tex_order[i]);
      const bool sized = d.w >= opt.etc1_merc_min && d.h >= opt.etc1_merc_min;
      tstats.merc++;
      tstats.merc_etc1 += g.etc1;
      tstats.merc_tested += g.tested;
      tstats.merc_face += sized && (u.face || u.eye);
      tstats.merc_bytes_16 += g.bytes_16;
      tstats.merc_bytes += tex_datas[i].size();
      lg::debug("{}: merc texture {} ({}) {}x{}{}{}{}{}: {}", in.stem().string(), t.debug_name,
                t.debug_tpage_name, d.w, d.h,
                d.format == c3l::TEX_ETC1A4 || d.format == c3l::TEX_RGBA4 ? " alpha" : "",
                u.face ? " face" : "", u.eye ? " eye" : "", u.envmap ? " envmap" : "",
                g.tested ? fmt::format("ETC1 {:.1f} dB, worst block {:.1f}; 16-bit {:.1f} dB, "
                                       "{:.1f} -> {}",
                                       g.psnr, g.worst_block, g.psnr_16, g.worst_block_16,
                                       g.etc1 ? "ETC1" : "16-bit")
                         : "16-bit");
    }
  }

  // ---- write ----
  std::vector<u8> buf(sizeof(c3l::Header));
  c3l::Header hdr{};
  memcpy(hdr.magic, c3l::kMagic, 4);
  hdr.version = c3l::kVersion;
  strncpy(hdr.level_name, level.level_name.c_str(), sizeof(hdr.level_name) - 1);
  hdr.num_chunks = chunks.size();
  // (AI-assisted) the visibility runs first: the chunks point at them
  {
    const u32 runs_offset = out_runs.empty() ? 0 : append(buf, out_runs.data(), out_runs.size());
    for (size_t i = 0; i < chunks.size(); i++) {
      chunks[i].vis_runs_offset =
          chunks[i].vis_run_count ? runs_offset + chunk_first_run[i] * (u32)sizeof(c3l::VisRun)
                                  : 0;
    }
  }
  hdr.chunks_offset = append(buf, chunks.data(), chunks.size());
  hdr.vertex_data_offset = append(buf, out_verts.data(), out_verts.size());
  hdr.vertex_data_size = out_verts.size() * sizeof(c3l::Vertex);
  hdr.index_data_offset = append(buf, out_indices.data(), out_indices.size());
  hdr.index_data_size = out_indices.size() * 2;
  hdr.draw_data_offset = append(buf, out_draws.data(), out_draws.size());
  hdr.draw_data_size = out_draws.size() * sizeof(c3l::Draw);
  hdr.num_merc_models = merc.models.size();
  hdr.merc_models_offset = append(buf, merc.models.data(), merc.models.size());
  hdr.merc_vertex_offset = append(buf, merc.verts.data(), merc.verts.size());
  hdr.merc_vertex_size = merc.verts.size() * sizeof(c3l::MercVertex);
  hdr.merc_index_offset = append(buf, merc.indices.data(), merc.indices.size());
  hdr.merc_index_size = merc.indices.size() * 2;
  hdr.merc_draw_offset = append(buf, merc.draws.data(), merc.draws.size());
  hdr.merc_draw_size = merc.draws.size() * sizeof(c3l::MercDraw);
  u32 merc_mod_offset = 0;
  if (!merc.blerc_verts.empty() || !merc.mod_effects.empty()) {
    c3l::MercBlercHeader bh{};
    bh.num_vertices = merc.blerc_verts.size();
    bh.num_targets = merc.blerc_targets.size();
    bh.num_dests = merc.blerc_dests.size();
    hdr.merc_blerc_offset = append(buf, &bh, 1);
    buf.insert(buf.end(), (const u8*)merc.blerc_verts.data(),
               (const u8*)(merc.blerc_verts.data() + merc.blerc_verts.size()));
    buf.insert(buf.end(), (const u8*)merc.blerc_targets.data(),
               (const u8*)(merc.blerc_targets.data() + merc.blerc_targets.size()));
    buf.insert(buf.end(), (const u8*)merc.blerc_dests.data(),
               (const u8*)(merc.blerc_dests.data() + merc.blerc_dests.size()));
    if (!merc.mod_effects.empty()) {
      // (AI-assisted) v12: the rewritten vertices, after the blend shapes (MercModHeader)
      c3l::MercModHeader mh{};
      mh.num_effects = merc.mod_effects.size();
      mh.num_dests = merc.mod_dests.size();
      mh.num_mask_bytes = merc.mod_masks.size();
      align16(buf);
      const u32 mod_offset = append(buf, &mh, 1);
      merc_mod_offset = mod_offset;
      buf.insert(buf.end(), (const u8*)merc.mod_effects.data(),
                 (const u8*)(merc.mod_effects.data() + merc.mod_effects.size()));
      buf.insert(buf.end(), (const u8*)merc.mod_dests.data(),
                 (const u8*)(merc.mod_dests.data() + merc.mod_dests.size()));
      buf.insert(buf.end(), merc.mod_masks.begin(), merc.mod_masks.end());
      memcpy(buf.data() + hdr.merc_blerc_offset + offsetof(c3l::MercBlercHeader, mod_offset),
             &mod_offset, 4);
    }
    hdr.merc_blerc_size = buf.size() - hdr.merc_blerc_offset;
  }
  align16(buf);
  hdr.texture_data_offset = buf.size();
  for (size_t i = 0; i < tex_descs.size(); i++) {
    tex_descs[i].data_offset = append(buf, tex_datas[i].data(), tex_datas[i].size());
    tex_descs[i].data_size = tex_datas[i].size();
  }
  align16(buf);
  hdr.texture_data_size = buf.size() - hdr.texture_data_offset;
  hdr.num_textures = tex_descs.size();
  hdr.textures_offset = append(buf, tex_descs.data(), tex_descs.size());
  align16(buf);
  memcpy(buf.data(), &hdr, sizeof(hdr));

  // (AI-assisted) v11: compressed blocks. Sections (and the padding after them) are packed
  // separately; inside them a block starts where the runtime starts reading a piece.
  std::vector<c3l_pack::Section> sections;
  {
    std::vector<std::pair<u32, std::string>> starts = {
        {hdr.chunks_offset, "chunks"},
        {hdr.vertex_data_offset, "vertices"},
        {hdr.index_data_offset, "indices"},
        {hdr.draw_data_offset, "draws"},
        {hdr.merc_models_offset, "merc models"},
        {hdr.merc_vertex_offset, "merc vertices"},
        {hdr.merc_index_offset, "merc indices"},
        {hdr.merc_draw_offset, "merc draws"},
        {hdr.texture_data_offset, "textures"},
        {hdr.textures_offset, "texture table"}};
    if (!out_runs.empty()) {
      starts.push_back({(u32)sizeof(c3l::Header), "vis runs"});
    }
    if (hdr.merc_blerc_offset) {
      starts.push_back({hdr.merc_blerc_offset, "merc blend shapes"});
    }
    if (merc_mod_offset) {
      starts.push_back({merc_mod_offset, "merc rewritten vertices"});
    }
    std::stable_sort(starts.begin(), starts.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    for (size_t i = 0; i < starts.size(); i++) {
      const u32 end = i + 1 < starts.size() ? starts[i + 1].first : (u32)buf.size();
      if (end > starts[i].first) {
        sections.push_back({starts[i].second, starts[i].first, end});
      }
    }
  }
  std::vector<u32> cuts;
  for (const auto& t : tex_descs) {
    cuts.push_back(t.data_offset);
  }
  for (const auto& ch : chunks) {
    cuts.push_back(hdr.vertex_data_offset + ch.first_vertex * (u32)sizeof(c3l::Vertex));
    u32 first = UINT32_MAX;
    for (u32 d = ch.first_draw; d < ch.first_draw + ch.draw_count; d++) {
      first = std::min(first, out_draws[d].first_index);
    }
    if (first != UINT32_MAX) {
      cuts.push_back(hdr.index_data_offset + first * 2);
    }
  }
  for (const auto& m : merc.models) {
    cuts.push_back(hdr.merc_vertex_offset + m.first_vertex * (u32)sizeof(c3l::MercVertex));
  }
  std::vector<c3l_pack::SectionStats> pstats;
  const auto file = c3l_pack::pack(buf, sections, cuts, opt.pack, &pstats);
  file_util::write_binary_file(out, file.data(), file.size());
  if (opt.verify && !c3l_pack::verify(out.string(), buf, cuts)) {
    return false;
  }
  {
    u64 raw = 0, lz4 = 0, zstd = 0, packed = 0;
    double ms = 0, raw_ms = 0;
    for (const auto& st : pstats) {
      if (!st.raw) {
        continue;
      }
      raw += st.raw;
      lz4 += st.lz4;
      zstd += st.zstd;
      packed += st.packed;
      ms += st.load_ms;
      lg::info("{}: c3l {:>17}: {:>6} KB, lz4 {:5.1f}%, zstd {:5.1f}% -> {:5.1f}% (blocks: {} stored, "
               "{} lz4, {} zstd)",
               level.level_name, st.name, st.raw / 1024, 100.0 * st.lz4 / st.raw,
               100.0 * st.zstd / st.raw, 100.0 * st.packed / st.raw, st.blocks[0], st.blocks[1],
               st.blocks[2]);
    }
    raw_ms = raw / (opt.pack.sd_mbps * 1048.576);
    lg::info("{}: c3l {} KB -> {} KB ({:.1f}%; all lz4 {:.1f}%, all zstd {:.1f}%), modeled 3DS load "
             "{:.0f} ms (uncompressed {:.0f} ms){}",
             level.level_name, raw / 1024, file.size() / 1024, 100.0 * file.size() / buf.size(),
             100.0 * lz4 / std::max<u64>(raw, 1), 100.0 * zstd / std::max<u64>(raw, 1), ms, raw_ms,
             opt.verify ? ", read back OK" : "");
  }

  lg::info(
      "{}: {} tfrag + {} tie + {} shrub trees ({} shrub tris) -> {} chunks, {} verts ({} KB), {} "
      "tris, {} draws, {} textures ({} KB), {} merc models ({} verts, {} tris, {} draws, {} envmap), file {} "
      "KB; tie: {} wind tris, {} small/medium-object tris; far tfrag: {} tris ({})",
      level.level_name, tfrag_trees, tie_trees, shrub_trees, cv.shrub_tris, chunks.size(),
      out_verts.size(),
      out_verts.size() * sizeof(c3l::Vertex) / 1024, out_indices.size() / 3, out_draws.size(),
      tex_descs.size(), hdr.texture_data_size / 1024, merc.models.size(), merc.verts.size(),
      merc.indices.size() / 3, merc.draws.size(), merc.envmap_draws, file.size() / 1024, cv.wind_tris, cv.small_tris,
      far_tris, far_source);
  if (!merc.mod_effects.empty()) {
    lg::info("{}: {} merc effects with game-rewritten vertices ({} vertex writes)", level.level_name,
             merc.mod_effects.size(), merc.mod_dests.size());
  }
  lg::info("{}: textures {} KB with mip levels (16-bit, no mips: {} KB): {} ETC1, {} ETC1A4, {} "
           "RGB565, {} RGBA4; ETC1 PSNR avg {:.1f} dB, min {:.1f} dB",
           level.level_name, tstats.bytes / 1024, tstats.bytes_16bit / 1024, tstats.etc1,
           tstats.etc1a4, tstats.rgb565, tstats.rgba4,
           tstats.psnr_count ? tstats.psnr_sum / tstats.psnr_count : 0.0,
           tstats.psnr_count ? tstats.psnr_min : 0.0);
  if (cv.tex_copies) {
    lg::info("{}: {} textures used were copies of others: drawn with the one copy",
             level.level_name, cv.tex_copies);
  }
  if (tstats.merc) {
    lg::info("{}: merc textures: {}, {} of {}x{} and up: {} faces / eyes kept 16-bit, {} "
             "tested, {} ETC1 (PSNR >= {:g} dB or the 16-bit's, blocks <= {:g} or the "
             "16-bit's): {} KB -> {} KB",
             level.level_name, tstats.merc, tstats.merc_tested + tstats.merc_face,
             opt.etc1_merc_min, opt.etc1_merc_min, tstats.merc_face, tstats.merc_tested,
             tstats.merc_etc1, opt.etc1_merc_psnr, opt.etc1_merc_block,
             tstats.merc_bytes_16 / 1024, tstats.merc_bytes / 1024);
  }
  lg::info("{}: post-vertex cache: vertices shaded per triangle (FIFO 8 / 16) {:.3f} / {:.3f}{}",
           level.level_name, vc_before.shaded8 / std::max<u64>(vc_before.tris, 1),
           vc_before.shaded16 / std::max<u64>(vc_before.tris, 1),
           opt.vcache ? fmt::format(" -> {:.3f} / {:.3f} (meshoptimizer)",
                                    vc_after.shaded8 / std::max<u64>(vc_after.tris, 1),
                                    vc_after.shaded16 / std::max<u64>(vc_after.tris, 1))
                      : "");
  if (opt.vcache) {
    lg::info("{}: {} duplicate vertices merged", level.level_name, welded_verts);
  }
  if (!plans.empty()) {
    // (AI-assisted) the quantizing grids against the old per-chunk bounding box steps
    auto median = [](std::vector<double> v) {
      std::sort(v.begin(), v.end());
      return v[v.size() / 2];
    };
    lg::info("{}: quantizing: chunk steps (game units) median {:.3f} max {:.2f} -> powers of two "
             "median {:g} max {:g} ({} chunks coarser, {} rounds); {} positions in several chunks, "
             "all reconstructed identically: {} ({} differ), {} vertices on a coarser grid of "
             "another chunk; largest position error {:.2f} -> {:.2f} units",
             level.level_name, median(qs.old_steps),
             *std::max_element(qs.old_steps.begin(), qs.old_steps.end()), median(qs.new_steps),
             *std::max_element(qs.new_steps.begin(), qs.new_steps.end()), qs.coarser_chunks,
             grid_rounds, qs.shared, qs.shared_mismatch == 0 ? "yes" : "NO", qs.shared_mismatch,
             qs.snapped_coarser, qs.old_err, qs.new_err);
  }
  {
    // (AI-assisted) how well the ETC1 / ETC1A4 texels compress (64 KB pieces), for --etc1-rdo
    std::vector<u8> etc;
    for (size_t i = 0; i < tex_descs.size(); i++) {
      if (tex_descs[i].format == c3l::TEX_ETC1 || tex_descs[i].format == c3l::TEX_ETC1A4) {
        etc.insert(etc.end(), tex_datas[i].begin(), tex_datas[i].end());
      }
    }
    size_t l = 0, z = 0;
    std::vector<u8> tmp(LZ4_compressBound(64 * 1024) + ZSTD_compressBound(64 * 1024));
    for (size_t a = 0; a < etc.size(); a += 64 * 1024) {
      const int n = (int)std::min<size_t>(64 * 1024, etc.size() - a);
      l += LZ4_compress_HC((const char*)etc.data() + a, (char*)tmp.data(), n, (int)tmp.size(), 12);
      z += ZSTD_compress(tmp.data(), tmp.size(), etc.data() + a, n, 19);
    }
    const auto rdo = ctr_tex::rdo_stats();
    lg::info("{}: ETC1 texels {} KB: lz4 {:.1f}%, zstd {:.1f}%{}", level.level_name,
             etc.size() / 1024, 100.0 * l / std::max<size_t>(etc.size(), 1),
             100.0 * z / std::max<size_t>(etc.size(), 1),
             opt.etc1_rdo > 0 ? fmt::format(" (RDO so far: {} of {} blocks copied, {} reuse colors)",
                                            rdo.copies, rdo.blocks, rdo.reused)
                              : "");
  }
  if (opt.vista) {
    size_t vchunks = 0, vdraws = 0;
    for (const auto& ch : chunks) {
      if (ch.kind == c3l::CHUNK_KIND_VISTA) {
        vchunks++;
        vdraws += ch.draw_count;
      }
    }
    lg::info("{}: vista tier: {} far triangles (low resolution tfrag, ties without a distance "
             "limit, coarse tfrag without low resolution tfrag) -> {} ({:.0f}%; simplified to {:g} m, "
             "then to {:.0f}% within {:g} m), {} chunks of {:g} m, {} draws",
             level.level_name, vista_src_tris, vista_tris,
             100.0 * vista_tris / std::max<size_t>(vista_src_tris, 1), opt.vista_error,
             100.0 * opt.vista_ratio, opt.vista_sloppy_error, vchunks,
             opt.vista_cell, vdraws);
  }
  if (opt.tie_winding) {
    lg::info("{}: tie winding: {} of {} cullable tie triangles on closed surfaces, marked for back "
             "face culling ({} turned to face outward)",
             level.level_name, cv.orient_culled, cv.orient_tris, cv.orient_flipped);
  }
  lg::info("{}: visibility: {} of {} tris in {} runs (hidden when the game's visibility data says)",
           level.level_name, vis_tris, out_indices.size() / 3, out_runs.size());
  lg::debug("tie tris by instance radius (<2m, <4, <8, ... >=128m): {} {} {} {} {} {} {} {}",
            cv.radius_hist[0], cv.radius_hist[1], cv.radius_hist[2], cv.radius_hist[3],
            cv.radius_hist[4], cv.radius_hist[5], cv.radius_hist[6], cv.radius_hist[7]);
  return true;
}
}  // namespace

int main(int argc, char** argv) {
  CLI::App app{"fr3 -> c3l (3DS level background) converter (AI-assisted)"};
  Options opt;
  std::string in, out;
  bool all = false;
  app.add_option("input", in, "input .fr3 (or folder with --all)")->required();
  app.add_option("output", out, "output .c3l (or folder with --all)")->required();
  app.add_flag("--all", all, "convert every .fr3 in the input folder");
  app.add_option("--tfrag-geo", opt.tfrag_geo, "tfrag level of detail (0 = most detailed, 2)");
  app.add_option("--tie-geo", opt.tie_geo, "tie level of detail (0 = most detailed, 3)");
  app.add_option("--far-tfrag-geo", opt.far_tfrag_geo,
                 "tfrag level of detail for far away cells (-1: no far version)");
  app.add_option("--palette", opt.palette,
                 "time of day palette to bake (0-7; default: 0 for interiors, else 1)");
  app.add_option("--max-tex", opt.max_tex, "maximum texture size (power of two, <= 1024)");
  app.add_option("--cell", opt.cell_meters, "chunk grid size in meters");
  app.add_option("--detail-radius", opt.detail_radius,
                 "tie instances smaller than this (meters) are small objects");
  app.add_option("--detail-dist", opt.detail_dist,
                 "small objects are drawn up to this distance (meters)");
  app.add_option("--medium-radius", opt.medium_radius, "same for medium objects");
  app.add_option("--medium-dist", opt.medium_dist, "same for medium objects");
  app.add_flag("--no-tie", opt.no_tie, "leave out tie");
  app.add_flag("--no-shrub", opt.no_shrub, "leave out shrubs (grass, bushes, small plants)");
  app.add_option("--shrub-dist", opt.shrub_dist, "shrubs are drawn up to this distance (meters)");
  app.add_option("--shrub-cell", opt.shrub_cell, "chunk grid size for shrubs in meters");
  app.add_flag("--no-merc", opt.no_merc, "leave out merc models");
  app.add_flag("--merc-only", opt.merc_only, "only merc models (for the common GAME.fr3)");
  app.add_option("--gen-lod", opt.gen_lod_ratio,
                 "merc models with only lod0: a low LOD with this fraction of the triangles (0: "
                 "none)");
  app.add_option("--gen-lod-error", opt.gen_lod_error,
                 "largest error of the generated low LOD, relative to the model's size");
  app.add_flag("--gen-lod-lock-border", opt.gen_lod_lock_border,
               "the generated low LOD keeps the model's open edges in place");
  bool no_etc1 = false, no_mips = false;
  app.add_flag("--no-etc1", no_etc1, "16-bit textures (RGB565 / RGBA4) instead of ETC1 / ETC1A4");
  app.add_flag("--etc1-merc", opt.etc1_merc,
               "ETC1 for all textures of merc models (no quality test, faces too)");
  app.add_option("--etc1-merc-psnr", opt.etc1_merc_psnr,
                 "merc textures: ETC1 when its PSNR against the image is at least this or the "
                 "16-bit version's (dB; 0: merc textures stay 16-bit)");
  app.add_option("--etc1-merc-block", opt.etc1_merc_block,
                 "... and no 4x4 block has an RMS error above this or the 16-bit version's worst "
                 "(0..255)");
  app.add_option("--etc1-merc-min", opt.etc1_merc_min,
                 "... for merc textures at least this many texels wide and high");
  app.add_option("--etc1-quality", opt.etc1_quality, "ETC1 encoder quality: 0 low, 1 medium, 2 high");
  app.add_flag("--no-mips", no_mips, "no mip levels");
  app.add_option("--etc1-rdo", opt.etc1_rdo,
                 "ETC1 rate-distortion optimisation for compression (lambda; 0: off)");
  app.add_option("--threads", opt.threads, "texture encoding threads (0: all cores)");
  // (AI-assisted) v11 compression
  std::string compress = "auto";
  app.add_option("--compress", compress,
                 "block compression: auto (per block, the fastest to load on the 3DS), lz4, zstd, "
                 "none");
  app.add_option("--sd-mbps", opt.pack.sd_mbps, "cost model: 3DS SD card read speed (MB/s)");
  app.add_option("--lz4-mbps", opt.pack.lz4_mbps, "cost model: 3DS LZ4 decode speed (MB/s)");
  app.add_option("--zstd-mbps", opt.pack.zstd_mbps, "cost model: 3DS zstd decode speed (MB/s)");
  u32 block_kb = 64;
  app.add_option("--block-kb", block_kb, "largest compressed block (KB, raw)");
  bool no_vcache = false;
  app.add_flag("--no-vcache", no_vcache, "keep the triangle and vertex order (no meshoptimizer)");
  bool no_verify = false;
  app.add_flag("--no-verify", no_verify, "don't read the written files back to check them");
  bool no_tie_winding = false, no_vista = false;
  app.add_flag("--no-tie-winding", no_tie_winding,
               "keep the strips' triangle order (no flipping, no back face culling marks)");
  app.add_flag("--no-vista", no_vista, "no vista tier (far scenery cells)");
  app.add_option("--vista-cell", opt.vista_cell, "vista tier cell size (meters)");
  app.add_option("--vista-error", opt.vista_error,
                 "vista tier: simplification error of ties and coarse tfrag (meters, 0: none)");
  app.add_option("--vista-ratio", opt.vista_ratio,
                 "vista tier: then down to this fraction of the triangles (sloppy)");
  app.add_option("--vista-sloppy-error", opt.vista_sloppy_error,
                 "vista tier: ... with at most this error (meters, 0: no sloppy step)");
  CLI11_PARSE(app, argc, argv);
  opt.verify = !no_verify;
  opt.vcache = !no_vcache;
  opt.tie_winding = !no_tie_winding;
  opt.vista = !no_vista;
  opt.pack.threads = opt.threads;
  opt.pack.max_block = std::max<u32>(block_kb, 1) * 1024;
  opt.pack.min_block = std::min(opt.pack.min_block, opt.pack.max_block);
  if (compress == "none") {
    opt.pack.mode = c3l_pack::Mode::NONE;
  } else if (compress == "lz4") {
    opt.pack.mode = c3l_pack::Mode::LZ4;
  } else if (compress == "zstd") {
    opt.pack.mode = c3l_pack::Mode::ZSTD;
  } else if (compress != "auto") {
    fmt::print(stderr, "--compress: auto, lz4, zstd or none\n");
    return 1;
  }
  opt.etc1 = !no_etc1;
  opt.mips = !no_mips;
  lg::initialize();

  if (all) {
    file_util::create_dir_if_needed(out);
    for (auto& e : fs::directory_iterator(in)) {
      if (e.path().extension() == ".fr3") {
        auto dst = fs::path(out) / (e.path().stem().string() + ".c3l");
        Options o = opt;
        // GAME.fr3 holds the models and textures shared by all levels (Jak, HUD...)
        o.merc_only = opt.merc_only || e.path().stem() == "GAME";
        if (!convert(e.path(), dst, o)) {
          return 1;
        }
      }
    }
    return 0;
  }
  return convert(in, out, opt) ? 0 : 1;
}
