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
#include <string>
#include <unordered_map>
#include <vector>

#include "common/custom_data/Tfrag3Data.h"
#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/util/Serializer.h"
#include "common/util/compress.h"

#include "ctr_texture.h"
#include "fmt/format.h"
#include "game/graphics/ctr/c3l_format.h"
#include "third-party/BS_thread_pool.hpp"
#include "third-party/CLI11.hpp"

namespace {

struct Options {
  int tfrag_geo = 1;  // 0 = most detailed, 2 = least (2 has large holes)
  int far_tfrag_geo = 2;  // coarse version for far away cells (-1: none)
  int tie_geo = 3;    // 0 = most detailed, 3 = least
  int palette = 1;    // time of day palette to bake (0..7)
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
  // Textures: ETC1 (opaque) / ETC1A4 (with alpha) instead of RGB565 / RGBA4: 1/4 and 1/2 of the
  // memory, so a level's textures fit in VRAM. Textures of merc models (characters) stay 16-bit
  // unless etc1_merc: faces and small details show ETC1's 4x4 blocks up close.
  bool etc1 = true;
  bool etc1_merc = false;
  int etc1_quality = 1;  // 0 low, 1 medium, 2 high (slow)
  // Mip levels: the GPU reads textures much faster when it can use a smaller version for surfaces
  // further away (and they don't shimmer).
  bool mips = true;
  int threads = 0;  // texture encoding threads (0: all cores)
};

// how a texture is used by the level's draws
struct TexUse {
  bool merc = false;        // drawn by a merc model
  bool alpha_test = false;  // drawn with an alpha test (cut-outs: keep their coverage in the mips)
  int aref = 0x26;          // the alpha test reference (GS units, 0x80 = 1.0)
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

  u16 texture_id(s32 tree_tex_id) {
    if (tree_tex_id < 0 || tree_tex_id >= (s32)level->textures.size()) {
      return 0xffff;  // animated / missing
    }
    auto it = tex_map.find(tree_tex_id);
    if (it != tex_map.end()) {
      return it->second;
    }
    u16 id = tex_order.size();
    tex_order.push_back(tree_tex_id);
    tex_use.emplace_back();
    tex_map[tree_tex_id] = id;
    return id;
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
        tris.push_back(SrcTri{{vertex_base + a, vertex_base + b, vertex_base + c}, key});
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
      if (tree.use_strips) {
        add_strips(tree.unpacked.indices, d.unpacked.idx_of_first_idx_in_full_buffer,
                   draw_index_count(d), base, id);
      } else {
        size_t first = d.unpacked.idx_of_first_idx_in_full_buffer;
        size_t n = draw_index_count(d);
        for (size_t i = first; i + 2 < first + n; i += 3) {
          tris.push_back(SrcTri{{base + tree.unpacked.indices[i], base + tree.unpacked.indices[i + 1],
                                 base + tree.unpacked.indices[i + 2]},
                                id});
        }
      }
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

  void add_tie_tree(tfrag3::TieTree& tree) {
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
        add_strips(tree.unpacked.indices, idx, g.num_inds, base, key);
        mark_small(first_tri);
        idx += g.num_inds;
      }
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
        add_strips(local, 0, local.size(), 0, key);
        wind_tris += tris.size() - first_tri;
        mark_small(first_tri);
        idx += g.num;
      }
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
  int skipped_models = 0;
};

void convert_merc(const tfrag3::MercModelGroup& group, Converter& cv, MercOut* out) {
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
    bool any_blerc = false;
    for (const auto& eff : model.effects) {
      any_blerc |= !eff.mod.mod_draw.empty() && !eff.mod.blerc.int_data.empty();
    }
    mm.scale = max_abs * (any_blerc ? 1.5f : 1.f) / 32767.f;
    mm.blerc_first = out->blerc_verts.size();

    // model vertices: (source vertex, draw) -> local index
    std::vector<c3l::MercVertex> local_verts;
    bool too_big = false;
    for (size_t ei = 0; ei < model.effects.size(); ei++) {
      const auto& eff = model.effects[ei];
      // Effects with blend shapes (faces): the PC draws the vertices they move from a copy
      // (mod.vertices, drawn by mod_draw, the rest by fix_draw), and so do we, to know which of
      // our vertices a blend shape vertex writes to.
      const bool use_blerc = !eff.mod.mod_draw.empty() && !eff.mod.blerc.int_data.empty();
      std::unordered_map<u32, std::vector<u16>> mod_local;  // mod vertex -> model vertices
      std::vector<std::pair<const tfrag3::MercDraw*, bool>> draw_list;  // (draw, from mod.vertices)
      if (use_blerc) {
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
                if (from_mod) {
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
      out->skipped_models++;
      continue;
    }
    mm.blerc_count = out->blerc_verts.size() - mm.blerc_first;
    mm.vertex_count = local_verts.size();
    mm.draw_count = out->draws.size() - mm.first_draw;
    out->verts.insert(out->verts.end(), local_verts.begin(), local_verts.end());
    out->models.push_back(mm);
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
};

/*!
 * One level texture -> the 3DS layout (see ctr_texture.h): power of two, at most max_tex, with mip
 * levels, in ETC1 / ETC1A4 / RGB565 / RGBA4.
 */
std::vector<u8> convert_texture(const tfrag3::Texture& tex,
                                const Options& opt,
                                const TexUse& use,
                                c3l::Texture* out_desc,
                                double* psnr_out) {
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
  const bool etc1 = opt.etc1 && (!use.merc || opt.etc1_merc);
  ctr_tex::Format fmt;
  if (has_alpha) {
    fmt = etc1 ? ctr_tex::Format::ETC1A4 : ctr_tex::Format::RGBA4;
  } else {
    fmt = etc1 ? ctr_tex::Format::ETC1 : ctr_tex::Format::RGB565;
  }
  const int levels = opt.mips ? ctr_tex::level_count(dw, dh, 16) : 1;
  ctr_tex::MipOptions mo;
  mo.preserve_coverage = has_alpha && use.alpha_test;
  mo.alpha_ref = std::clamp(use.aref * 2, 1, 255);
  auto mips = ctr_tex::make_mips(img, levels, mo);
  std::vector<u8> out;
  for (auto& m : mips) {
    if (has_alpha) {
      ctr_tex::fill_transparent_colors(&m);
    }
    ctr_tex::encode_level(m, fmt, opt.etc1_quality, &out,
                          mo.preserve_coverage ? mo.alpha_ref : 0);
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

bool convert(const fs::path& in, const fs::path& out, const Options& opt) {
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
  // (detail, cell) -> triangle indices
  std::map<std::tuple<int, int, int, int>, std::vector<u32>> cells;
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
                               (int)std::floor(cy / 3 / cell), (int)std::floor(cz / 3 / cell));
    cells[key].push_back(i);
  }

  std::vector<c3l::Chunk> chunks;
  std::vector<c3l::Vertex> out_verts;
  std::vector<u16> out_indices;
  std::vector<c3l::Draw> out_draws;

  for (auto& [key, tri_ids] : cells) {
    // sort by draw so each chunk has one draw per (mode, texture)
    std::stable_sort(tri_ids.begin(), tri_ids.end(),
                     [&](u32 a, u32 b) { return cv.tris[a].draw_key < cv.tris[b].draw_key; });
    size_t pos = 0;
    while (pos < tri_ids.size()) {
      // gather triangles until 65535 unique vertices
      std::unordered_map<u32, u16> local;
      std::vector<u32> local_src;
      size_t end = pos;
      while (end < tri_ids.size()) {
        const auto& t = cv.tris[tri_ids[end]];
        int new_verts = 0;
        for (int k = 0; k < 3; k++) {
          new_verts += local.count(t.v[k]) ? 0 : 1;
        }
        if (local_src.size() + new_verts > 65535) {
          break;
        }
        for (int k = 0; k < 3; k++) {
          if (!local.count(t.v[k])) {
            local[t.v[k]] = local_src.size();
            local_src.push_back(t.v[k]);
          }
        }
        end++;
      }

      c3l::Chunk ch{};
      const int detail = std::get<0>(key) % 4;
      const float cell = cell_of_detail(detail);
      ch.lod_tier = std::get<0>(key) / 4;
      ch.lod_center[0] = (std::get<1>(key) + 0.5f) * cell;
      ch.lod_center[1] = (std::get<2>(key) + 0.5f) * cell;
      ch.lod_center[2] = (std::get<3>(key) + 0.5f) * cell;
      ch.max_dist = detail == 1   ? opt.detail_dist * 4096.f
                    : detail == 2 ? opt.medium_dist * 4096.f
                    : detail == 3 ? opt.shrub_dist * 4096.f
                                  : 0.f;
      float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
      for (u32 s : local_src) {
        const auto& v = cv.verts[s];
        float p[3] = {v.x, v.y, v.z};
        for (int c = 0; c < 3; c++) {
          mn[c] = std::min(mn[c], p[c]);
          mx[c] = std::max(mx[c], p[c]);
        }
      }
      float extent = std::max({mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2], 1.f});
      ch.scale = extent / 32767.f;
      float rad = 0;
      for (int c = 0; c < 3; c++) {
        ch.origin[c] = mn[c];
        ch.bsphere[c] = (mn[c] + mx[c]) / 2;
      }
      for (u32 s : local_src) {
        const auto& v = cv.verts[s];
        float dx = v.x - ch.bsphere[0], dy = v.y - ch.bsphere[1], dz = v.z - ch.bsphere[2];
        rad = std::max(rad, std::sqrt(dx * dx + dy * dy + dz * dz));
      }
      ch.bsphere[3] = rad;
      ch.first_vertex = out_verts.size();
      ch.vertex_count = local_src.size();
      for (u32 s : local_src) {
        const auto& v = cv.verts[s];
        c3l::Vertex o{};
        float p[3] = {v.x, v.y, v.z};
        for (int c = 0; c < 3; c++) {
          o.pos[c] = (s16)std::clamp((int)std::lround((p[c] - ch.origin[c]) / ch.scale), 0, 32767);
        }
        o.st[0] = (s16)std::clamp((int)std::lround(v.s * 1024.f), -32768, 32767);
        o.st[1] = (s16)std::clamp((int)std::lround(v.t * 1024.f), -32768, 32767);
        memcpy(o.rgba, v.rgba, 4);
        out_verts.push_back(o);
      }
      ch.first_draw = out_draws.size();
      u32 cur_key = UINT32_MAX;
      for (size_t i = pos; i < end; i++) {
        const auto& t = cv.tris[tri_ids[i]];
        if (t.draw_key != cur_key) {
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
      }
      ch.draw_count = out_draws.size() - ch.first_draw;
      chunks.push_back(ch);
      pos = end;
    }
  }

  // ---- textures (in parallel: ETC1 encoding is slow) ----
  std::vector<c3l::Texture> tex_descs(cv.tex_order.size());
  std::vector<std::vector<u8>> tex_datas(cv.tex_order.size());
  std::vector<double> tex_psnr(cv.tex_order.size(), 99.0);
  {
    BS::thread_pool pool(opt.threads > 0 ? opt.threads : std::thread::hardware_concurrency());
    pool.push_loop(cv.tex_order.size(), [&](size_t a, size_t b) {
      for (size_t i = a; i < b; i++) {
        tex_descs[i] = c3l::Texture{};
        tex_datas[i] = convert_texture(level.textures.at(cv.tex_order[i]), opt, cv.tex_use[i],
                                       &tex_descs[i], &tex_psnr[i]);
      }
    });
    pool.wait_for_tasks();
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
  }

  // ---- write ----
  std::vector<u8> buf(sizeof(c3l::Header));
  c3l::Header hdr{};
  memcpy(hdr.magic, c3l::kMagic, 4);
  hdr.version = c3l::kVersion;
  strncpy(hdr.level_name, level.level_name.c_str(), sizeof(hdr.level_name) - 1);
  hdr.num_chunks = chunks.size();
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
  if (!merc.blerc_verts.empty()) {
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
  file_util::write_binary_file(out, buf.data(), buf.size());

  lg::info(
      "{}: {} tfrag + {} tie + {} shrub trees ({} shrub tris) -> {} chunks, {} verts ({} KB), {} "
      "tris, {} draws, {} textures ({} KB), {} merc models ({} verts, {} tris, {} draws), file {} "
      "KB; tie: {} wind tris, {} small/medium-object tris; far tfrag: {} tris ({})",
      level.level_name, tfrag_trees, tie_trees, shrub_trees, cv.shrub_tris, chunks.size(),
      out_verts.size(),
      out_verts.size() * sizeof(c3l::Vertex) / 1024, out_indices.size() / 3, out_draws.size(),
      tex_descs.size(), hdr.texture_data_size / 1024, merc.models.size(), merc.verts.size(),
      merc.indices.size() / 3, merc.draws.size(), buf.size() / 1024, cv.wind_tris, cv.small_tris,
      far_tris, far_source);
  lg::info("{}: textures {} KB with mip levels (16-bit, no mips: {} KB): {} ETC1, {} ETC1A4, {} "
           "RGB565, {} RGBA4; ETC1 PSNR avg {:.1f} dB, min {:.1f} dB",
           level.level_name, tstats.bytes / 1024, tstats.bytes_16bit / 1024, tstats.etc1,
           tstats.etc1a4, tstats.rgb565, tstats.rgba4,
           tstats.psnr_count ? tstats.psnr_sum / tstats.psnr_count : 0.0,
           tstats.psnr_count ? tstats.psnr_min : 0.0);
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
  app.add_option("--palette", opt.palette, "time of day palette to bake (0-7)");
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
  bool no_etc1 = false, no_mips = false;
  app.add_flag("--no-etc1", no_etc1, "16-bit textures (RGB565 / RGBA4) instead of ETC1 / ETC1A4");
  app.add_flag("--etc1-merc", opt.etc1_merc, "ETC1 also for the textures of merc models");
  app.add_option("--etc1-quality", opt.etc1_quality, "ETC1 encoder quality: 0 low, 1 medium, 2 high");
  app.add_flag("--no-mips", no_mips, "no mip levels");
  app.add_option("--threads", opt.threads, "texture encoding threads (0: all cores)");
  CLI11_PARSE(app, argc, argv);
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
