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
#include "common/texture/texture_slots.h"
#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/util/Timer.h"
#include "common/util/Serializer.h"
#include "common/util/compress.h"

#include "c3l_opt.h"
#include "c3l_pack.h"
#include "ctr_texture.h"
#include "keira_model.h"
#include "tie_lod.h"
#include "fmt/format.h"
#include "game/graphics/ctr/c3l_format.h"
#include "game/graphics/ctr/CtrTexAnimJak2Defs.h"
#include "third-party/BS_thread_pool.hpp"
#include "third-party/CLI11.hpp"
#include "third-party/lz4/lz4hc.h"
#include "third-party/meshoptimizer/meshoptimizer.h"
#include "third-party/zstd/lib/zstd.h"

namespace {

struct Options {
  // (AI-assisted) the game of the .fr3: "jak1" or "jak2" ("": from the input path, out/jak2/...).
  // Jak II looks up its own PS2 distances (tie_ps2_dists_jak2.inc, shrub_ps2_dists_jak2.inc).
  std::string game;
  // (AI-assisted) 0 as the PS2 near the camera: 1 cut the yellow pools of Volcanic Crater into
  // polygons (all levels: +1% triangles)
  int tfrag_geo = 0;  // 0 = most detailed, 2 = least (2 has large holes)
  int far_tfrag_geo = 2;  // coarse version for far away cells (-1: none)
  int tie_geo = 3;    // 0 = most detailed, 3 = least
  // (AI-assisted) v17 tie levels of detail (tie_lod.h): the instance groups whose shape changes
  // most also get the tie_near_geo version (geo 0), drawn up to their PS2 far-plane (beyond it the
  // PS2 too draws geometry 3), within tie_lod_budget_kb of extra vertex and index data per level
  // (< 0: the level's budget from kTieLodBudgets, else all groups)
  bool tie_lod = true;
  int tie_near_geo = 0;
  // (AI-assisted, lod-light) the far versions' vertex colors fitted to the near ones (the PC draws
  // geometry 0 only): no change of light on a surface at the switch (Converter::far_colors_from_near).
  // false: each geometry's own colors (the PS2's)
  bool tie_far_near_colors = true;
  float tie_lod_budget_kb = -1.f;
  // smallest shape error worth a near version, as an angle (radians) at the larger of the group's
  // radius and tie_lod_view_m meters (0.002: half a pixel on the top screen)
  float tie_lod_min_angle = 0.002f;
  float tie_lod_view_m = 15.f;
  // the switch distance of groups without a PS2 distance (wind instances), meters
  float tie_lod_default_m = 60.f;
  // (AI-assisted, streaming) v19: every candidate group gets its near version, in near chunks of
  // stream_cell meters (c3l::StreamChunk) that the runtime streams in around the camera; the level's
  // v18 budget (kTieLodBudgets) only marks which groups a runtime without streaming loads (cls 1).
  // Off: v18 files (the near versions in the far versions' chunks, within the budget)
  bool stream = true;
  // (AI-assisted, j2vis) v20: the prototypes the game hides by task state in runs of their own
  // (c3l::ProtoVis; Jak II with streaming, set in convert; --no-proto-vis: off)
  bool proto_vis = false;
  bool no_proto_vis = false;
  bool no_env_ties = false;  // (AI-assisted, j2water2) --no-env-ties: v20's base draws only
  float stream_cell = 100.f;
  // (AI-assisted, streaming) near chunks by need class too: a group's near version is needed within
  // error x kStreamNeedK + radius of its center; the groups of one class (bounds in meters) in near
  // chunks of their own, of the class's cell size (stream_class_cells; else stream_cell), so a chunk
  // isn't needed from far away for one large group in it
  std::vector<float> stream_classes = {30.f, 60.f, 120.f, 240.f};
  std::vector<float> stream_class_cells = {50.f, 50.f, 100.f, 100.f, 200.f};
  int palette = -1;   // time of day palette to bake (0..7; -1: auto_palette)
  // (AI-assisted) v16: every vertex gets its time of day color (c3l::TodColor / TodMul, Vertex::pad), the
  // runtime blends the palettes with the game's weights (off: the baked palette only)
  bool tod = true;
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
  // (AI-assisted) v14: each shrub prototype is drawn up to its PS2 distance (shrub_ps2_dists.inc:
  // the middle of its PS2 fade, (x + w) / 2), shrubs grouped into chunks by distance class
  // (kShrubClassMeters); off: all up to shrub_dist (v13)
  bool shrub_ps2_dist = true;
  bool no_merc = false;
  bool merc_only = false;  // common file (GAME.fr3): merc models + their textures only
  // (AI-assisted) the "play as Keira" easter egg's file (KEIRA.c3l, keira_model.h): the input is
  // the common GAME.fr3, this the village1.fr3 her model comes from
  std::string keira_from;
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
  bool vis_vertex_groups = true;  // (AI-assisted) v18, see group_vertices_by_vis
  // (AI-assisted) v15: the triangle order for a first-in first-out post-vertex cache of this many
  // entries (c3l_opt::vcache_order), coplanar overlapping triangles kept in their order. -1 (the
  // default): per run the best of the orders for 4 and 12 entries and meshoptimizer's (where no
  // order has to be kept), by the vertices shaded with 4 + 8 + 16 entries (the cache's size is
  // unknown). 0: the v14 order (meshoptimizer, without the coplanar rule)
  int vcache_fifo = -1;
  // (AI-assisted) v15 texture atlases (c3l::kDrawAtlasUV): the small textures (at most atlas_max
  // texels on each side) whose every level draw is opaque and has each triangle within one copy of
  // the texture go into atlases with gutters (one per format and level count, up to 256 x 256), so
  // a chunk's draws of the same state merge and the level binds fewer textures
  bool atlas = true;
  int atlas_max = 32;
  // an atlas is made only if the memory it adds (atlas - its textures) is at most this many KB per
  // texture bind it saves (cells - 1)
  float atlas_kb_per_bind = 2.f;
  // (AI-assisted) v15 opaque alpha (c3l::kDrawAlphaTestDropped, kDrawBlendDropped): the alpha test
  // or alpha blending of draws whose texture alpha is 1.0 in every texel of every level and whose
  // vertex alpha is >= 0x80 is taken out (it can't change a pixel); then draws of the same texture
  // and state merge
  bool opaque_alpha = true;
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
  // (AI-assisted) also an untextured copy of the vista tier (c3l::CHUNK_KIND_VISTA_FLAT): each
  // texture's average color (alpha weighted) baked into the vertex colors, one draw per cell and
  // mode; config vista_untextured picks it at load
  bool vista_flat = false;
  // (AI-assisted) merc back face culling marks (c3l::kMercDrawCullBack, config cull_merc), see
  // mark_merc_cull: at least merc_cull_agree of a draw's triangles agree clearly with their vertex
  // normals (each corner's normal within acos(merc_cull_cos) of the face's side), at most
  // merc_cull_open of them have an open edge. merc_cull_agree 0: no marks
  float merc_cull_agree = 0.99f;
  float merc_cull_cos = 0.2f;
  float merc_cull_open = 0.02f;
};

// (AI-assisted) post-vertex cache statistics (the GPU's cache is cleared before each draw):
// vertices shaded per triangle, for a FIFO cache of 8 and 16 entries
struct VcacheStats {
  double shaded8 = 0, shaded16 = 0;
  // (AI-assisted) a plain FIFO of 4, 8, 16 and 32 entries (c3l_opt::fifo_misses)
  u64 fifo[4] = {0, 0, 0, 0};
  u64 tris = 0;
  u64 unique = 0;  // (AI-assisted) vertices used: the least any order shades
  void add(const u16* idx, size_t count, size_t vertex_count) {
    std::vector<unsigned int> v(idx, idx + count);
    {
      std::vector<unsigned int> u(v);
      std::sort(u.begin(), u.end());
      unique += std::unique(u.begin(), u.end()) - u.begin();
    }
    shaded8 += meshopt_analyzeVertexCache(v.data(), count, vertex_count, 8, 0, 0).vertices_transformed;
    shaded16 +=
        meshopt_analyzeVertexCache(v.data(), count, vertex_count, 16, 0, 0).vertices_transformed;
    for (int k = 0; k < 4; k++) {
      fifo[k] += c3l_opt::fifo_misses(v.data(), count, 4 << k);
    }
    tris += count / 3;
  }
  std::string fifo_line() const {
    const double t = (double)std::max<u64>(tris, 1);
    return fmt::format("{:.3f} / {:.3f} / {:.3f} / {:.3f} (each vertex once: {:.3f})", fifo[0] / t,
                       fifo[1] / t, fifo[2] / t, fifo[3] / t, unique / t);
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
  // (AI-assisted) Jak II: the animated texture slot (jak2_animated_texture_slots index + 1) whose
  // base texture this is (0: none). Written to c3l::Texture::pad (c3l::texture_anim_slot); kept out
  // of the content sharing, the atlases and the alpha optimisations (the runtime replaces it)
  u16 anim_slot = 0;
};

// A triangle soup vertex before chunking
struct SrcVertex {
  float x, y, z;
  float s, t;
  u8 rgba[4];
  // (AI-assisted) v16: Converter::tod_colors index, or kTodMulBit | tod_muls index (kTodNone: none)
  u32 tod = 0xffffffffu;
  // (AI-assisted) v15 atlas copies (Options::atlas): the vertex this one copies (UINT32_MAX: not a
  // copy) and its texture coordinates in the atlas (1/32768, c3l::kDrawAtlasUV)
  u32 orig = UINT32_MAX;
  s16 ast[2] = {0, 0};
  // (AI-assisted, j2water2) c3l v21: an envmapped tie vertex's normal (world, unit length * 127)
  s8 nrm[3] = {0, 0, 0};
  u8 has_nrm = 0;
};

struct SrcTri {
  u32 v[3];         // into the global SrcVertex list
  u32 draw_key;     // index into draw_keys
  u32 detail = 0;   // 1, 2: part of a small/medium object (drawn up to detail/medium_dist),
                    // 3: shrub (drawn up to shrub_dist)
  u32 tier = 0;     // c3l::Chunk::lod_tier
  u32 sclass = 0;   // (AI-assisted) shrubs: distance class (kShrubClassMeters), 0: shrub_dist
  u16 vis = 0xffff;  // (AI-assisted) drawable id in the level's visibility string (c3l::VisRun)
                     // (vista tier: the source chunk)
  // (AI-assisted) v15: the draw key before the atlas (the vista tier copies with it; UINT32_MAX:
  // draw_key)
  u32 orig_key = UINT32_MAX;
  // (AI-assisted) v17 tie levels of detail: c3l::kRunLodNear / kRunLodFar version of LOD group
  // lod_group (Converter::lod_groups), or kRunLodAlways
  u8 lod = 0;
  u32 lod_group = UINT32_MAX;
  // (AI-assisted, streaming) v19: a near version in a near chunk: c3l::kStreamCore (a group the
  // budget took) or kStreamExtra; 0: an ordinary chunk's
  u8 stream = 0;
  // (AI-assisted, j2vis) v20: a prototype the game hides by task state (Converter::proto_vis
  // index), 0xffff: none. Its triangles are visibility runs of their own (c3l::ProtoVis)
  u16 proto = 0xffff;
};

struct DrawKey {
  u32 mode;
  u16 texture;
  u16 kind;
  // (AI-assisted, j2water2) c3l v21: the envmapped tie base draw's shine (Converter::env_ties
  // index), 0xffff: none. Kept by every key made from this one (copy it, then change fields).
  u16 env = 0xffff;
  bool operator<(const DrawKey& o) const {
    return std::tie(mode, texture, kind, env) < std::tie(o.mode, o.texture, o.kind, o.env);
  }
};
// (AI-assisted, j2water2) a key made from dk: its env kept
inline DrawKey key_like(const DrawKey& dk, u32 mode, u16 texture, u16 kind) {
  DrawKey k = dk;
  k.mode = mode;
  k.texture = texture;
  k.kind = kind;
  return k;
}

// (AI-assisted) the PS2 shrub distances, see shrub_ps2_dists.inc
struct ShrubPs2Dist {
  const char* level;
  const char* proto;
  float x, w;  // meters: full detail up to x, fading out (or a billboard) up to w
};
const ShrubPs2Dist kShrubPs2Dists[] = {
#include "shrub_ps2_dists.inc"
#include "shrub_ps2_dists_jak2.inc"
};
// (AI-assisted) the PS2 distance tables' level prefix of the game being converted: "" for Jak 1,
// "jak2/" for Jak II (its levels' names are in the tables so: Jak II has a village1 too)
std::string g_dist_prefix;
// shrub distance classes (meters, index = SrcTri::sclass; 0 unused): a chunk holds one class
constexpr float kShrubClassMeters[] = {0.f, 35.f, 50.f, 70.f, 90.f, 120.f, 160.f, 400.f};
constexpr int kShrubClasses = sizeof(kShrubClassMeters) / sizeof(kShrubClassMeters[0]);
// prototypes the PS2 draws as billboards beyond w (big bushes, w = 10 m): no billboards here, the
// whole shrub up to this distance (v13's shrub distance plus its cells' margin)
constexpr float kShrubBillboardMeters = 70.f;

// (AI-assisted, j2vis) v20: the prototypes Jak II hides and shows by task state (c3l::ProtoVis)
const char* const kToggledProtosJak2[] = {
#include "proto_vis_jak2.inc"
};
bool toggled_proto(const std::string& name) {
  for (const char* n : kToggledProtosJak2) {
    if (name == n) {
      return true;
    }
  }
  return false;
}

// distance class of a shrub prototype of a level (0: not in the table)
int shrub_class(const std::string& level, const std::string& proto, float* meters) {
  for (const auto& e : kShrubPs2Dists) {
    if (level == e.level && proto == e.proto) {
      const float d = e.w <= e.x ? kShrubBillboardMeters : 0.5f * (e.x + e.w);
      int best = 1;
      for (int c = 2; c < kShrubClasses; c++) {
        if (std::abs(std::log(kShrubClassMeters[c] / d)) <
            std::abs(std::log(kShrubClassMeters[best] / d))) {
          best = c;
        }
      }
      *meters = d;
      return best;
    }
  }
  return 0;
}

struct Converter {
  Options opt;
  const tfrag3::Level* level = nullptr;
  std::vector<SrcVertex> verts;
  // (AI-assisted) v16: the level's time of day colors and multiplied colors (each once)
  static constexpr u32 kTodNone = 0xffffffffu, kTodMulBit = 0x80000000u;
  std::vector<c3l::TodColor> tod_colors;
  std::vector<c3l::TodMul> tod_muls;
  std::unordered_map<std::string, u32> tod_color_ids, tod_mul_ids;
  // a tfrag / tie / shrub time of day color: its 8 palettes
  u32 tod_color(const tfrag3::PackedTimeOfDay& c, u32 color) {
    if (!opt.tod) {
      return kTodNone;
    }
    c3l::TodColor e;
    for (int p = 0; p < 8; p++) {
      for (int ch = 0; ch < 3; ch++) {
        e.rgb[p][ch] = c.read(color, p, ch);
      }
    }
    auto [it, fresh] =
        tod_color_ids.emplace(std::string((const char*)&e, sizeof(e)), (u32)tod_colors.size());
    if (fresh) {
      tod_colors.push_back(e);
    }
    return it->second;
  }
  // a time of day color times mul / 128 (u8 per channel)
  u32 tod_mul(u32 id, const u8 mul[3]) {
    if (id == kTodNone) {
      return kTodNone;
    }
    if (id & kTodMulBit) {
      // already a multiplied one: the product of the two
      const auto& m = tod_muls[id & ~kTodMulBit];
      u8 k[3];
      for (int c = 0; c < 3; c++) {
        k[c] = (u8)std::min(255, (m.mul[c] * mul[c] + 64) / 128);
      }
      return tod_mul(m.color, k);
    }
    if (mul[0] == 128 && mul[1] == 128 && mul[2] == 128) {
      return id;
    }
    c3l::TodMul m{};
    m.color = (u16)id;
    memcpy(m.mul, mul, 3);
    auto [it, fresh] =
        tod_mul_ids.emplace(std::string((const char*)&m, sizeof(m)), (u32)tod_muls.size());
    if (fresh) {
      tod_muls.push_back(m);
    }
    return it->second | kTodMulBit;
  }
  // the vertex's Vertex::pad (colors first, then the multiplied ones)
  u16 tod_pad(u32 id) const {
    if (id == kTodNone) {
      return 0xffff;
    }
    const u32 v = id & kTodMulBit ? (u32)tod_colors.size() + (id & ~kTodMulBit) : id;
    return v < 0xffff ? (u16)v : (u16)0xffff;
  }
  std::vector<SrcTri> tris;
  // (AI-assisted, j2water2) c3l v21: the envmap shine of envmapped tie base draws (DrawKey::env):
  // the envmap texture (output index), its draw mode and the prototype's tint
  bool env_ties_on = false;
  struct EnvTie {
    u16 texture;
    u32 mode;
    u8 tint[4];
  };
  std::vector<EnvTie> env_ties;
  std::map<std::tuple<u16, u32, u32>, u16> env_tie_ids;
  size_t env_tie_tris = 0, env_tie_groups_missing = 0;
  u16 env_tie_id(u16 tex, u32 mode, const u8 tint[4]) {
    u32 t;
    memcpy(&t, tint, 4);
    auto [it, fresh] = env_tie_ids.emplace(std::make_tuple(tex, mode, t), (u16)env_ties.size());
    if (fresh) {
      EnvTie e{tex, mode, {tint[0], tint[1], tint[2], tint[3]}};
      env_ties.push_back(e);
    }
    return it->second;
  }
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

  // (AI-assisted) v15: where a key's triangles go in a chunk's draw order (make_plans): a key made
  // by the draw optimisations takes the place of the first key merged into it, so the chunks keep
  // their layout (which keys end up in which chunk split, the quantizing grids)
  std::vector<u32> key_rank;
  u32 rank_of(u32 k) const { return k < key_rank.size() ? key_rank[k] : k; }
  void rank_merge(u32 to, u32 from) {
    while (key_rank.size() < draw_keys.size()) {
      key_rank.push_back((u32)key_rank.size());
    }
    key_rank[to] = std::min(key_rank[to], rank_of(from));
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

  // (AI-assisted) Jak II's animated texture slots (merc draws with tree_tex_id -(slot + 1)): the
  // level texture made for the slot by add_anim_slot_textures, if any
  std::unordered_map<s32, s32> anim_slot_tex;
  std::unordered_map<u16, u16> slot_tex_map;  // (AI-assisted, j2gaps) slot + 1 -> output texture
  std::unordered_map<s32, u16> base_tex_slot;  // (AI-assisted, j2gaps) base texture -> slot + 1
  int slot_tex_first_plain = 0;  // slot textures first used by a plain draw (unmarked before)

  u16 texture_id(s32 tree_tex_id) {
    const s32 orig_tex_id = tree_tex_id;
    u16 slot = 0;  // (AI-assisted) animated slot + 1
    if (tree_tex_id < 0) {
      auto a = anim_slot_tex.find(-(tree_tex_id + 1));
      if (a != anim_slot_tex.end()) {
        slot = (u16)(-tree_tex_id);
        tree_tex_id = a->second;
      }
    } else if (!anim_slot_tex.empty()) {
      // (AI-assisted, j2gaps) a plain draw of a slot's base texture (the background's draws of a
      // fixed animation's texture: waterfalls, security walls) is a draw of the slot too
      if (base_tex_slot.empty()) {
        for (const auto& [s, t] : anim_slot_tex) {
          base_tex_slot.emplace(t, (u16)(s + 1));
        }
      }
      auto b = base_tex_slot.find(tree_tex_id);
      if (b != base_tex_slot.end()) {
        slot = b->second;
      }
    }
    if (tree_tex_id < 0 || tree_tex_id >= (s32)level->textures.size()) {
      return 0xffff;  // animated / missing
    }
    if (slot) {
      // (AI-assisted, j2gaps) a slot's draws (and its base texture's) get one texture, marked
      // with the slot, whatever came first: a plain draw of the base texture, or of a copy with the
      // same content, used to create the shared entry unmarked; the slot's draws then found it and
      // never animated
      auto sit = slot_tex_map.find(slot);
      if (sit != slot_tex_map.end()) {
        return sit->second;
      }
      u16 id = tex_order.size();
      tex_order.push_back(tree_tex_id);
      tex_use.emplace_back();
      tex_use.back().anim_slot = slot;
      slot_tex_map[slot] = id;
      slot_tex_first_plain += orig_tex_id >= 0;
      return id;
    }
    auto it = tex_map.find(tree_tex_id);
    if (it != tex_map.end()) {
      return it->second;
    }
    const tfrag3::Texture* t = &level->textures[tree_tex_id];
    if (!slot) {
      auto cit = tex_by_content.find(t);
      if (cit != tex_by_content.end() && !tex_use[cit->second].anim_slot) {
        tex_map[tree_tex_id] = cit->second;
        tex_copies++;
        return cit->second;
      }
    }
    u16 id = tex_order.size();
    tex_order.push_back(tree_tex_id);
    tex_use.emplace_back();
    tex_use.back().anim_slot = slot;
    tex_map[tree_tex_id] = id;
    if (!slot) {
      tex_by_content.emplace(t, id);
    }
    return id;
  }

  // (AI-assisted) the visibility id of the triangles being added (see c3l::VisRun): a BVH node's
  // id in the level's visibility string, 0xffff = none
  u16 cur_vis = 0xffff;
  // (AI-assisted, j2vis) v20: the toggled prototype of the triangles being added (proto_vis), 0xffff
  u16 cur_proto = 0xffff;
  struct ProtoOut {
    std::string name;
    u16 kind;  // c3l::kProtoTie / kProtoShrub
  };
  std::vector<ProtoOut> proto_vis;
  // the prototype's id in proto_vis if the game toggles it (Jak II, proto_vis_jak2.inc), else 0xffff
  u16 proto_vis_id(const std::vector<std::string>& names, u32 idx, u16 kind) {
    if (!opt.proto_vis || idx >= names.size()) {
      return 0xffff;
    }
    const std::string& name = names[idx];
    if (!toggled_proto(name)) {
      return 0xffff;
    }
    for (size_t i = 0; i < proto_vis.size(); i++) {
      if (proto_vis[i].name == name && proto_vis[i].kind == kind) {
        return (u16)i;
      }
    }
    if (name.size() >= sizeof(c3l::ProtoVis::name)) {
      lg::warn("{}: prototype name {} too long for the visibility table", level->level_name, name);
      return 0xffff;
    }
    proto_vis.push_back(ProtoOut{name, kind});
    return (u16)(proto_vis.size() - 1);
  }
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
        tris.back().proto = cur_proto;
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
      sv.tod = tod_color(tree.colors, v.color_index);
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
  size_t shrub_class_tris[kShrubClasses] = {};  // (AI-assisted)
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
      {
        // (AI-assisted) v16: the time of day color times the base color (/ 128, as above)
        u8 mul[3];
        for (int c = 0; c < 3; c++) {
          mul[c] = (u8)std::min(255, (v.rgba_base[c] * 256 + 127) / 255);
        }
        sv.tod = tod_mul(tod_color(tree.time_of_day_colors, v.color_index), mul);
      }
      verts.push_back(sv);
    }
    for (const auto& d : tree.static_draws) {
      const u16 tex = texture_id((s32)d.tree_tex_id);
      note_use(tex, d.mode.as_int(), false);
      const u32 key = key_id(DrawKey{d.mode.as_int(), tex, 2});
      const size_t first_tri = tris.size();
      // (AI-assisted, j2vis) v20: a prototype the game hides by task state (Shrub: per draw)
      cur_proto = tree.has_per_proto_visibility_toggle
                      ? proto_vis_id(tree.proto_names, d.proto_idx, c3l::kProtoShrub)
                      : (u16)0xffff;
      add_strips(tree.indices, d.first_index_index, d.num_indices, base, key);
      cur_proto = 0xffff;
      // (AI-assisted) shrub_ps2_dist: the prototype's distance class
      int sclass = 0;
      if (opt.shrub_ps2_dist && d.proto_idx < tree.proto_names.size()) {
        float m = 0;
        sclass = shrub_class(g_dist_prefix + level->level_name, tree.proto_names[d.proto_idx], &m);
        if (!sclass) {
          lg::warn("{}: shrub {} has no PS2 distance, drawn up to {} m", level->level_name,
                   tree.proto_names[d.proto_idx], opt.shrub_dist);
        }
      }
      for (size_t i = first_tri; i < tris.size(); i++) {
        tris[i].detail = 3;
        tris[i].sclass = sclass;
      }
      shrub_class_tris[sclass] += tris.size() - first_tri;
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
          tri.draw_key = key_id(key_like(dk, dk.mode, dk.texture, (u16)(dk.kind | c3l::kDrawCullBack)));
          orient_culled++;
        }
      }
    }
  }

  // (AI-assisted) v17 tie levels of detail (tie_lod.h): the selected instance groups (group key ->
  // index into lod_groups), and per group its detail class (mark_small, from both versions)
  std::unordered_map<u64, u32> lod_group_of;
  std::vector<c3l::TieLodGroup> lod_groups;
  std::vector<u32> lod_group_detail;
  std::vector<float> lod_group_error;  // (AI-assisted, title-popin) shape error per group
  // (AI-assisted, streaming) v19: per group, whether the level's v18 budget took it
  std::vector<u8> lod_group_core;
  // the tree being added (add_tie_tree): its index in the level's tree list, and which version:
  // -1 without levels of detail (every group, as before v17), 0 the near geometry (only the
  // selected groups), 1 the far geometry (every group; the selected ones as their far version)
  u32 cur_tree = 0;
  int cur_lod_slot = -1;
  size_t lod_tris[2] = {0, 0};
  // a group's triangles [first, end) were added: their LOD version and detail class (selected
  // groups), else mark_small. Returns false for a group the near tree doesn't add.
  bool lod_wanted(u64 key) const {
    return cur_lod_slot != 0 || lod_group_of.count(key);
  }
  void lod_mark(u64 key, size_t first) {
    auto it = cur_lod_slot >= 0 ? lod_group_of.find(key) : lod_group_of.end();
    if (it == lod_group_of.end()) {
      mark_small(first);
      return;
    }
    const u32 detail = lod_group_detail[it->second];
    // (AI-assisted, streaming) v19: the near versions go to near chunks
    const u8 stream = cur_lod_slot == 0 && opt.stream
                          ? (lod_group_core[it->second] ? (u8)c3l::kStreamCore : (u8)c3l::kStreamExtra)
                          : (u8)0;
    for (size_t i = first; i < tris.size(); i++) {
      tris[i].lod = cur_lod_slot == 0 ? c3l::kRunLodNear : c3l::kRunLodFar;
      tris[i].lod_group = it->second;
      tris[i].detail = detail;
      tris[i].stream = stream;
    }
    small_tris += detail ? tris.size() - first : 0;
    lod_tris[cur_lod_slot] += tris.size() - first;
  }

  // (AI-assisted, lod-light) far_colors_from_near's statistics: the far surface's color against
  // the near surface's under it (area weighted, GS units, mean over the 8 palettes' rgb): with
  // geometry 3's own colors (the PS2's), with the fit, with the colors picked
  struct FarColorStats {
    size_t far_verts = 0, changed = 0;
    double area = 0;
    double abs_own = 0, abs_fit = 0, abs_pick = 0;
    double sgn_own = 0, sgn_fit = 0, sgn_pick = 0;
  };

  /*!
   * (AI-assisted, lod-light) The far versions' vertex colors fitted to their near versions. The
   * PS2 and the PC light ties by baked vertex colors. The PS2 bakes each geometry's own (an
   * instance's color indices per geometry) and its far versions lose geometry 0's extra vertices:
   * on rocks, the dark crevices and lit ridges between the far corners. The far corners
   * sat mostly on ridges, so a far rock was lighter as a whole (beach: +5 levels on 16x16 pixel
   * blocks), and the near/far switch showed as the light on the surface changing when the camera
   * came near (the PC draws geometry 0 only: nothing changes there).
   *
   * Each far vertex gets the near surface's mean color around it: the near surface is sampled
   * (each near triangle's corners and center, by area), each sample goes to the nearest point of
   * the far surface, and a far vertex's color is the mean of the samples weighted by its
   * barycentric weight there (the far surface then has the near surface's mean color as a whole,
   * and nearly so in each part). The table of colors stays as it is: the vertex takes the color
   * of the near vertex (among those whose samples it got) nearest to that mean, in all 8 palettes.
   */
  void far_colors_from_near(FarColorStats* st) {
    struct GroupTris {
      std::vector<u32> near_t, far_t;
    };
    std::unordered_map<u32, GroupTris> groups;
    for (u32 i = 0; i < tris.size(); i++) {
      const auto& t = tris[i];
      if (t.lod_group == UINT32_MAX) {
        continue;
      }
      if (t.lod == c3l::kRunLodNear) {
        groups[t.lod_group].near_t.push_back(i);
      } else if (t.lod == c3l::kRunLodFar) {
        groups[t.lod_group].far_t.push_back(i);
      }
    }
    constexpr int kC = 24;  // 8 palettes x rgb
    // the pick's weight of a difference in the mean of the 24 values (beach, per far triangle:
    // 0 -> 2.94 GS units off, the far version 0.62 darker; 16 -> 2.60, 0.09 darker; the fit 2.48)
    constexpr float kMeanWeight = 16.f;
    using Col = std::array<float, kC>;
    auto color_of = [&](u32 vi) {
      Col c;
      const auto& v = verts[vi];
      if (v.tod != kTodNone && !(v.tod & kTodMulBit) && v.tod < tod_colors.size()) {
        for (int k = 0; k < kC; k++) {
          c[k] = tod_colors[v.tod].rgb[k / 3][k % 3];
        }
      } else {
        for (int k = 0; k < kC; k++) {
          c[k] = v.rgba[k % 3];
        }
      }
      return c;
    };
    auto pos_of = [&](u32 vi, float* p) {
      p[0] = verts[vi].x;
      p[1] = verts[vi].y;
      p[2] = verts[vi].z;
    };
    std::vector<u32> keys;
    for (const auto& [g, gt] : groups) {
      if (!gt.near_t.empty() && !gt.far_t.empty()) {
        keys.push_back(g);
      }
    }
    std::sort(keys.begin(), keys.end());
    struct Sample {
      u32 ftri;
      float w[3];
      float weight;
      Col col;
    };
    struct Result {
      std::vector<std::pair<u32, u32>> pick;  // far vertex, near vertex whose colors it takes
      double area = 0, abs_own = 0, abs_fit = 0, abs_pick = 0, sgn_own = 0, sgn_fit = 0,
             sgn_pick = 0;
    };
    std::vector<Result> results(keys.size());
    BS::thread_pool pool(opt.threads > 0 ? opt.threads : std::thread::hardware_concurrency());
    pool.push_loop(keys.size(), [&](size_t a, size_t b) {
      for (size_t gi = a; gi < b; gi++) {
        const auto& gt = groups.at(keys[gi]);
        auto& res = results[gi];
        // the far surface
        std::vector<tie_lod::Tri> ft(gt.far_t.size());
        std::vector<std::array<float, 6>> boxes(gt.far_t.size());
        std::unordered_map<u32, u32> slot;  // far vertex -> local index
        std::vector<u32> fverts;
        for (size_t i = 0; i < gt.far_t.size(); i++) {
          const auto& t = tris[gt.far_t[i]];
          for (int k = 0; k < 3; k++) {
            pos_of(t.v[k], &ft[i][3 * k]);
            if (slot.emplace(t.v[k], (u32)fverts.size()).second) {
              fverts.push_back(t.v[k]);
            }
          }
          for (int c = 0; c < 3; c++) {
            boxes[i][c] = std::min({ft[i][c], ft[i][3 + c], ft[i][6 + c]});
            boxes[i][3 + c] = std::max({ft[i][c], ft[i][3 + c], ft[i][6 + c]});
          }
        }
        std::vector<Col> own(fverts.size());
        for (size_t i = 0; i < fverts.size(); i++) {
          own[i] = color_of(fverts[i]);
        }
        // the near surface's samples on it
        std::vector<Col> acc(fverts.size());
        for (auto& c : acc) {
          c.fill(0.f);
        }
        std::vector<double> wacc(fverts.size(), 0.0);
        std::vector<Sample> samples;
        size_t hint = 0;
        auto nearest = [&](const float* p, float* w) {
          float best = tie_lod::closest_point_bary(p, ft[hint], w);
          size_t bi = hint;
          for (size_t i = 0; i < ft.size(); i++) {
            float d2 = 0;
            for (int c = 0; c < 3; c++) {
              const float o = p[c] < boxes[i][c]       ? boxes[i][c] - p[c]
                              : p[c] > boxes[i][3 + c] ? p[c] - boxes[i][3 + c]
                                                       : 0.f;
              d2 += o * o;
            }
            if (i == hint || d2 >= best) {
              continue;
            }
            float ww[3];
            const float d = tie_lod::closest_point_bary(p, ft[i], ww);
            if (d < best) {
              best = d;
              bi = i;
              memcpy(w, ww, sizeof(ww));
            }
          }
          hint = bi;
          return (u32)bi;
        };
        for (u32 ti : gt.near_t) {
          const auto& t = tris[ti];
          float p[3][3];
          Col c[3];
          for (int k = 0; k < 3; k++) {
            pos_of(t.v[k], p[k]);
            c[k] = color_of(t.v[k]);
          }
          float e1[3], e2[3];
          for (int k = 0; k < 3; k++) {
            e1[k] = p[1][k] - p[0][k];
            e2[k] = p[2][k] - p[0][k];
          }
          const float cx = e1[1] * e2[2] - e1[2] * e2[1], cy = e1[2] * e2[0] - e1[0] * e2[2],
                      cz = e1[0] * e2[1] - e1[1] * e2[0];
          const float area = 0.5f * std::sqrt(cx * cx + cy * cy + cz * cz);
          if (!(area > 0)) {
            continue;
          }
          // the corners (half of the area) and the center (the other half)
          for (int k = 0; k < 4; k++) {
            Sample s;
            float q[3];
            if (k < 3) {
              memcpy(q, p[k], sizeof(q));
              s.col = c[k];
              s.weight = area / 6.f;
            } else {
              for (int j = 0; j < 3; j++) {
                q[j] = (p[0][j] + p[1][j] + p[2][j]) / 3.f;
              }
              for (int j = 0; j < kC; j++) {
                s.col[j] = (c[0][j] + c[1][j] + c[2][j]) / 3.f;
              }
              s.weight = area / 2.f;
            }
            s.ftri = nearest(q, s.w);
            const auto& f = tris[gt.far_t[s.ftri]];
            for (int j = 0; j < 3; j++) {
              const u32 sl = slot.at(f.v[j]);
              const float ww = s.w[j] * s.weight;
              for (int ch = 0; ch < kC; ch++) {
                acc[sl][ch] += ww * s.col[ch];
              }
              wacc[sl] += ww;
            }
            samples.push_back(s);
          }
        }
        // the fit, then the group's near color nearest to it (in all 8 palettes; a difference in
        // the mean, the overall light, counts more than one between channels or palettes)
        std::vector<std::pair<u32, Col>> gcols;  // a near vertex of each of the group's colors
        {
          std::set<std::pair<u32, u32>> seen;  // (tod, rgba)
          for (u32 ti : gt.near_t) {
            for (u32 nv : tris[ti].v) {
              u32 rgba;
              memcpy(&rgba, verts[nv].rgba, 4);
              if (seen.emplace(verts[nv].tod, rgba).second) {
                gcols.push_back({nv, color_of(nv)});
              }
            }
          }
        }
        std::vector<Col> fit(fverts.size()), pick(fverts.size());
        for (size_t i = 0; i < fverts.size(); i++) {
          if (wacc[i] <= 0) {
            fit[i] = pick[i] = own[i];
            continue;
          }
          for (int ch = 0; ch < kC; ch++) {
            fit[i][ch] = (float)(acc[i][ch] / wacc[i]);
          }
          float best = 1e30f;
          u32 bv = UINT32_MAX;
          for (const auto& [nv, cc] : gcols) {
            float d = 0, m = 0;
            for (int ch = 0; ch < kC; ch++) {
              const float x = cc[ch] - fit[i][ch];
              d += x * x;
              m += x;
            }
            d += kMeanWeight * m * m / kC;
            if (d < best) {
              best = d;
              bv = nv;
            }
          }
          if (bv == UINT32_MAX) {
            pick[i] = own[i];
            continue;
          }
          pick[i] = color_of(bv);
          res.pick.push_back({fverts[i], bv});
        }
        // statistics: per far triangle, the mean difference over the near surface under it (what
        // the far version's light is off by there; the detail within a triangle it can't have)
        std::vector<std::array<double, 4>> per(ft.size(), {0, 0, 0, 0});  // weight, own, fit, pick
        for (const auto& s : samples) {
          const auto& f = tris[gt.far_t[s.ftri]];
          double d_own = 0, d_fit = 0, d_pick = 0;
          for (int ch = 0; ch < kC; ch++) {
            float o = 0, ft_ = 0, pk = 0;
            for (int j = 0; j < 3; j++) {
              const u32 sl = slot.at(f.v[j]);
              o += s.w[j] * own[sl][ch];
              ft_ += s.w[j] * fit[sl][ch];
              pk += s.w[j] * pick[sl][ch];
            }
            d_own += o - s.col[ch];
            d_fit += ft_ - s.col[ch];
            d_pick += pk - s.col[ch];
          }
          auto& pt = per[s.ftri];
          pt[0] += s.weight;
          pt[1] += s.weight * d_own / kC;
          pt[2] += s.weight * d_fit / kC;
          pt[3] += s.weight * d_pick / kC;
        }
        for (const auto& pt : per) {
          if (pt[0] <= 0) {
            continue;
          }
          res.area += pt[0];
          res.abs_own += std::abs(pt[1]);
          res.abs_fit += std::abs(pt[2]);
          res.abs_pick += std::abs(pt[3]);
          res.sgn_own += pt[1];
          res.sgn_fit += pt[2];
          res.sgn_pick += pt[3];
        }
      }
    });
    pool.wait_for_tasks();
    for (const auto& r : results) {
      for (const auto& [fv, nv] : r.pick) {
        auto& v = verts[fv];
        const auto& n = verts[nv];
        st->far_verts++;
        if (memcmp(v.rgba, n.rgba, 4) != 0 || v.tod != n.tod) {
          memcpy(v.rgba, n.rgba, 4);
          v.tod = n.tod;
          st->changed++;
        }
      }
      st->area += r.area;
      st->abs_own += r.abs_own;
      st->abs_fit += r.abs_fit;
      st->abs_pick += r.abs_pick;
      st->sgn_own += r.sgn_own;
      st->sgn_fit += r.sgn_fit;
      st->sgn_pick += r.sgn_pick;
    }
  }

  void add_tie_tree(tfrag3::TieTree& tree) {
    const size_t tree_first_tri = tris.size();
    if (tree.unpacked.indices.empty()) {
      tree.unpack();  // (AI-assisted) once: unpack() appends the indices again
    }
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
      sv.tod = tod_color(tree.colors, v.color_index);
      verts.push_back(sv);
    }
    // static instances (already in world space), one vis group per instance
    // categories: normal, trans, water, the base draws of envmapped ties (drawn like the others:
    // same colors as tfrag3, see etie_base.vert), then the envmap shine draws (not supported)
    u32 last_draw = tree.category_draw_indices[(int)tfrag3::TieCategory::WATER_ENVMAP + 1];
    if (last_draw == 0) {
      last_draw = tree.static_draws.size();
    }
    // (AI-assisted, j2water2) c3l v21 (Jak II): the envmap shine draws (the second draw
    // categories, Tie3::envmap_second_pass_draw) give each envmapped prototype its envmap texture
    // and mode; its base draws carry them (DrawKey::env), with the prototype's tint (the vertices'
    // rgba, etie.vert proto_tint) and the vertices' normals: one draw instead of two
    const u32 env_first = tree.category_draw_indices[(int)tfrag3::TieCategory::NORMAL_ENVMAP];
    std::unordered_map<u32, std::pair<s32, u32>> proto_env;  // proto -> (tree texture, mode)
    if (env_ties_on && last_draw) {
      const u32 e0 =
          tree.category_draw_indices[(int)tfrag3::TieCategory::NORMAL_ENVMAP_SECOND_DRAW];
      const u32 e1 =
          tree.category_draw_indices[(int)tfrag3::TieCategory::WATER_ENVMAP_SECOND_DRAW + 1];
      for (u32 di = e0; di < e1 && di < tree.static_draws.size(); di++) {
        const auto& d = tree.static_draws[di];
        for (const auto& g : d.vis_groups) {
          proto_env.emplace(g.tie_proto_idx, std::make_pair(d.tree_tex_id, d.mode.as_int()));
        }
      }
    }
    for (u32 di = 0; di < tree.static_draws.size() && di < last_draw; di++) {
      const auto& d = tree.static_draws[di];
      const u16 tex = texture_id(d.tree_tex_id);
      note_use(tex, d.mode.as_int(), false);
      u32 key = key_id(DrawKey{d.mode.as_int(), tex, 1});
      const u32 plain_key = key;
      size_t idx = d.unpacked.idx_of_first_idx_in_full_buffer;
      for (auto& g : d.vis_groups) {
        size_t first_tri = tris.size();
        // (AI-assisted) v17: the group's levels of detail
        const u64 lkey = tie_lod::static_key(cur_tree, g.vis_idx_in_pc_bvh, g.tie_proto_idx);
        if (!lod_wanted(lkey)) {
          idx += g.num_inds;
          continue;
        }
        key = plain_key;
        if (!proto_env.empty() && di >= env_first) {
          // (AI-assisted, j2water2) an envmapped prototype's base draw: its shine
          auto pe = proto_env.find(g.tie_proto_idx);
          u32 v0 = UINT32_MAX;
          for (size_t i = idx; i < idx + g.num_inds && i < tree.unpacked.indices.size(); i++) {
            if (tree.unpacked.indices[i] != UINT32_MAX) {
              v0 = tree.unpacked.indices[i];
              break;
            }
          }
          const u16 etex = pe == proto_env.end() ? (u16)0xffff : texture_id(pe->second.first);
          if (etex != 0xffff && v0 != UINT32_MAX) {
            const auto& pv = tree.unpacked.vertices.at(v0);
            const u8 tint[4] = {pv.r, pv.g, pv.b, pv.a};
            note_use(etex, pe->second.second, false);
            DrawKey ek{d.mode.as_int(), tex, 1};
            ek.env = env_tie_id(etex, pe->second.second, tint);
            key = key_id(ek);
            // the normals (Tfrag3Data unpack: 10-bit signed fields, world space)
            for (size_t i = idx; i < idx + g.num_inds && i < tree.unpacked.indices.size(); i++) {
              const u32 vi = tree.unpacked.indices[i];
              if (vi == UINT32_MAX) {
                continue;
              }
              auto& sv = verts[base + vi];
              if (sv.has_nrm) {
                continue;
              }
              const u32 nor = tree.unpacked.vertices[vi].nor;
              float n[3];
              for (int c = 0; c < 3; c++) {
                int f = (int)((nor >> (10 * c)) & 0x3ff);
                n[c] = (float)(f >= 512 ? f - 1024 : f);
              }
              const float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
              for (int c = 0; c < 3; c++) {
                sv.nrm[c] = len > 0 ? (s8)std::lround(127.f * n[c] / len) : 0;
              }
              sv.has_nrm = 1;
            }
          } else {
            env_tie_groups_missing++;
          }
        }
        cur_vis = vis_id(tree.bvh, g.vis_idx_in_pc_bvh);  // (AI-assisted) up to 8 instances
        // (AI-assisted, j2vis) v20: a prototype the game hides by task state (Tie3: per group)
        cur_proto = tree.has_per_proto_visibility_toggle
                        ? proto_vis_id(tree.proto_names, g.tie_proto_idx, c3l::kProtoTie)
                        : (u16)0xffff;
        add_strips(tree.unpacked.indices, idx, g.num_inds, base, key);
        if (key != plain_key) {
          env_tie_tris += tris.size() - first_tri;  // (AI-assisted, j2water2)
        }
        lod_mark(lkey, first_tri);
        idx += g.num_inds;
      }
      cur_vis = 0xffff;
      cur_proto = 0xffff;
    }
    // instances moved by the wind (plants, trees): baked at rest with their instance matrix
    for (const auto& d : tree.instanced_wind_draws) {
      const u16 tex = texture_id(d.tree_tex_id);
      note_use(tex, d.mode.as_int(), false);
      u32 key = key_id(DrawKey{d.mode.as_int(), tex, 1});
      size_t idx = 0;
      for (const auto& g : d.instance_groups) {
        // (AI-assisted) v17: the instance's levels of detail
        const u64 lkey = tie_lod::wind_key(cur_tree, g.instance_idx);
        if (!lod_wanted(lkey)) {
          idx += g.num;
          continue;
        }
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
            sv.tod = tod_color(tree.colors, v.color_index);
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
        lod_mark(lkey, first_tri);
        idx += g.num;
      }
    }
    if (opt.tie_winding) {
      orient_ties(tree_first_tri, tris.size());  // (AI-assisted)
    }
  }
};

/*!
 * (AI-assisted) Jak II: merc draws of animated textures (Jak's face, hair, fingers and eyebrows,
 * whose colors the texture animator blends between a normal and a dark CLUT; the jakb cutscene
 * models) name a slot of jak2_animated_texture_slots() instead of a texture. The 3DS has no
 * texture animator for them yet: the slot gets a fixed texture, the index texture of the slot's
 * name with its first CLUT (name + "-norm", else "-start", else its own), as TextureAnimator's
 * ClutBlender shows it at weight 0. Added to the level's textures; slots without an index texture
 * in this level stay untextured. Nothing changes for files without such textures (Jak 1).
 */
void add_anim_slot_textures(tfrag3::Level* level, std::unordered_map<s32, s32>* out) {
  if (level->index_textures.empty()) {
    return;
  }
  auto find = [&](const std::string& name) -> const tfrag3::IndexTexture* {
    for (const auto& t : level->index_textures) {
      if (t.name == name) {
        return &t;
      }
    }
    return nullptr;
  };
  const auto& slots = jak2_animated_texture_slots();
  for (size_t i = 0; i < slots.size(); i++) {
    const tfrag3::IndexTexture* dest = find(slots[i]);
    if (!dest) {
      continue;
    }
    const tfrag3::IndexTexture* clut = find(slots[i] + "-norm");
    if (!clut) {
      clut = find(slots[i] + "-start");
    }
    if (!clut) {
      clut = dest;
    }
    tfrag3::Texture t;
    t.w = dest->w;
    t.h = dest->h;
    t.debug_name = slots[i];
    t.debug_tpage_name = dest->tpage_name;
    t.data.resize(dest->index_data.size());
    for (size_t p = 0; p < dest->index_data.size(); p++) {
      const auto& c = clut->color_table[dest->index_data[p]];
      t.data[p] = (u32)c.x() | ((u32)c.y() << 8) | ((u32)c.z() << 16) | ((u32)c.w() << 24);
    }
    (*out)[(s32)i] = (s32)level->textures.size();
    level->textures.push_back(std::move(t));
    lg::info("animated texture slot {}: {}x{} with the CLUT of {}", slots[i], dest->w, dest->h,
             clut->name);
  }
}

/*!
 * (AI-assisted) Jak II: the slots of the fixed texture animations (waterfalls, lava, security walls,
 * the slime...: TextureAnimator's FixedAnimDef destinations, no index texture) have a level texture
 * of their name, the base texture (as fr3_to_gltf's anim_slot_to_base_tex): the draws get it, and
 * the runtime draws the animation into its place (CtrTexAnimJak2).
 */
void add_anim_slot_base_textures(const tfrag3::Level& level, std::unordered_map<s32, s32>* out) {
  const auto& slots = jak2_animated_texture_slots();
  for (size_t i = 0; i < slots.size(); i++) {
    if (out->count((s32)i)) {
      continue;
    }
    for (size_t t = 0; t < level.textures.size(); t++) {
      if (level.textures[t].debug_name == slots[i]) {
        (*out)[(s32)i] = (s32)t;
        lg::info("animated texture slot {}: base texture {}x{}", slots[i], level.textures[t].w,
                 level.textures[t].h);
        break;
      }
    }
  }
}

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
  // (AI-assisted) merc_cull statistics: triangles of the drawn (not envmap) draws, and of those
  // marked kMercDrawCullBack; draws marked; triangles turned to face outward
  size_t cull_tris = 0, cull_marked_tris = 0, cull_marked_draws = 0, cull_draws = 0,
         cull_flipped = 0;
  // why draws weren't marked (blended / cut-out texture, open edges (sheets), normals)
  size_t cull_no_mode = 0, cull_no_surface = 0, cull_no_normals = 0;
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
    // (AI-assisted) simplified triangles: wound consistently, not necessarily outward: no culling
    d.palette[c3l::kMercDrawFlagsByte] &= (u8)~c3l::kMercDrawCullBack;
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

/*!
 * (AI-assisted) Back face culling marks of a model's draws (Options::merc_cull_agree): the merc
 * strips became triangles without the strips' alternating winding, so a triangle's winding says
 * nothing yet; the vertex normals say which side is the outside. A triangle is clear when all
 * three corners' normals are on one side of it (cos >= merc_cull_cos); it is turned to face that
 * side. A draw is marked c3l::kMercDrawCullBack when it is opaque and depth written with a texture
 * whose alpha never fails the runtime's alpha test (a cut-out shows its back through the holes),
 * at least merc_cull_agree of its triangles are clear, and at most merc_cull_open of them have an
 * open edge (the model's draws welded by position: an edge of only one triangle). That last test
 * keeps single sheets (capes, cloth hems, flags, leaves: seen from both sides on the PS2, which
 * doesn't cull) and the parts around holes unculled. Envmap draws repeat a draw's triangles and
 * take its mark. Was a closed surface test like the ties' (orient_ties) first: merc models are
 * rarely watertight (2% of Jak's and 13% of Snowy's merc triangles passed).
 */
void mark_merc_cull(const c3l::MercModel& mm,
                    const std::vector<c3l::MercVertex>& verts,
                    Converter& cv,
                    MercOut* out) {
  (void)mm;
  const Options& opt = cv.opt;
  const size_t d0 = mm.first_draw, d1 = out->draws.size();
  if (opt.merc_cull_agree <= 0.f || d1 <= d0) {
    return;
  }
  // texture alpha against the runtime's merc alpha test (CtrMercRenderer: GS alpha < 17 dropped)
  auto tex_ok = [&](u16 tex) {
    if (tex == 0xffff) {
      return true;
    }
    const auto& t = cv.level->textures.at(cv.tex_order.at(tex));
    for (u32 p : t.data) {
      if (((p >> 24) & 0xff) < 17) {
        return false;
      }
    }
    return true;
  };
  std::map<u16, bool> tex_cache;
  // welded edges of all the model's (non-envmap) draws: open = used by one triangle
  std::map<std::array<s16, 3>, u32> weld;
  auto corner = [&](u32 index_pos) {
    const auto& v = verts.at(out->indices[index_pos]);
    return weld.emplace(std::array<s16, 3>{v.pos[0], v.pos[1], v.pos[2]}, (u32)weld.size())
        .first->second;
  };
  std::unordered_map<u64, u32> edge_use;
  auto edge_key = [](u32 a, u32 b) { return ((u64)std::min(a, b) << 32) | std::max(a, b); };
  for (size_t di = d0; di < d1; di++) {
    const auto& d = out->draws[di];
    if (d.eye_id == c3l::kMercEnvmapDraw) {
      continue;
    }
    for (u32 i = d.first_index; i + 2 < d.first_index + d.index_count; i += 3) {
      const u32 c[3] = {corner(i), corner(i + 1), corner(i + 2)};
      for (int e = 0; e < 3; e++) {
        if (c[e] != c[(e + 1) % 3]) {
          edge_use[edge_key(c[e], c[(e + 1) % 3])]++;
        }
      }
    }
  }
  std::map<std::pair<u32, u32>, size_t> range_draw;  // a non-envmap draw by its index range
  for (size_t di = d0; di < d1; di++) {
    auto& d = out->draws[di];
    if (d.eye_id == c3l::kMercEnvmapDraw) {
      continue;
    }
    range_draw.emplace(std::make_pair(d.first_index, d.index_count), di);
    const size_t n = d.index_count / 3;
    out->cull_draws++;
    out->cull_tris += n;
    DrawMode mode;
    mode.as_int() = d.mode;
    auto it = tex_cache.find(d.texture);
    if (it == tex_cache.end()) {
      it = tex_cache.emplace(d.texture, tex_ok(d.texture)).first;
    }
    if (mode.get_ab_enable() || !mode.get_depth_write_enable() || !it->second || !n) {
      out->cull_no_mode++;
      continue;
    }
    size_t clear = 0, open = 0;
    std::vector<u8> turn(n, 0);
    for (size_t t = 0; t < n; t++) {
      const u32 i = d.first_index + 3 * (u32)t;
      const u32 c[3] = {corner(i), corner(i + 1), corner(i + 2)};
      bool has_open = false;
      for (int e = 0; e < 3; e++) {
        has_open |= c[e] != c[(e + 1) % 3] && edge_use[edge_key(c[e], c[(e + 1) % 3])] == 1;
      }
      open += has_open;
      double p[3][3], vn[3][3];
      for (int k = 0; k < 3; k++) {
        const auto& v = verts.at(out->indices[i + k]);
        for (int q = 0; q < 3; q++) {
          p[k][q] = v.pos[q];
          vn[k][q] = v.normal[q];
        }
      }
      const double e1[3] = {p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
      const double e2[3] = {p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
      const double fn[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                            e1[0] * e2[1] - e1[1] * e2[0]};
      const double fl = std::sqrt(fn[0] * fn[0] + fn[1] * fn[1] + fn[2] * fn[2]);
      int front = 0, back = 0;
      double sum = 0;
      for (int k = 0; k < 3; k++) {
        const double vl =
            std::sqrt(vn[k][0] * vn[k][0] + vn[k][1] * vn[k][1] + vn[k][2] * vn[k][2]);
        const double cs =
            fl > 0 && vl > 0
                ? (fn[0] * vn[k][0] + fn[1] * vn[k][1] + fn[2] * vn[k][2]) / (fl * vl)
                : 0.0;
        sum += cs;
        front += cs >= opt.merc_cull_cos;
        back += cs <= -opt.merc_cull_cos;
      }
      clear += front == 3 || back == 3;
      turn[t] = sum < 0;  // (not clear: the corners' majority)
    }
    if ((double)open > opt.merc_cull_open * (double)n) {
      out->cull_no_surface++;
      continue;
    }
    if ((double)clear < opt.merc_cull_agree * (double)n) {
      out->cull_no_normals++;
      continue;
    }
    for (size_t t = 0; t < n; t++) {
      if (turn[t]) {
        const u32 i = d.first_index + 3 * (u32)t;
        std::swap(out->indices[i + 1], out->indices[i + 2]);
        out->cull_flipped++;
      }
    }
    d.palette[c3l::kMercDrawFlagsByte] |= c3l::kMercDrawCullBack;
    out->cull_marked_draws++;
    out->cull_marked_tris += n;
  }
  // the envmap passes share their draw's (turned) indices: and its mark
  for (size_t di = d0; di < d1; di++) {
    auto& d = out->draws[di];
    if (d.eye_id != c3l::kMercEnvmapDraw) {
      continue;
    }
    auto it = range_draw.find({d.first_index, d.index_count});
    if (it != range_draw.end()) {
      d.palette[c3l::kMercDrawFlagsByte] =
          out->draws[it->second].palette[c3l::kMercDrawFlagsByte];
    }
  }
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
    mark_merc_cull(mm, local_verts, cv, out);  // (AI-assisted) before the generated LOD copies
    out->verts.insert(out->verts.end(), local_verts.begin(), local_verts.end());
    out->models.push_back(mm);
    // (AI-assisted) a generated low LOD for "<base>-lod0" without lower LODs (not with blend
    // shapes or rewritten vertices: those move per frame)
    // (not Jak and Daxter: always close and in view, their look stays the PS2's; (AI-assisted,
    // j1ocean) not the pickups: power cells, scout flies, orbs always full detail)
    const auto p = model.name.rfind("-lod0");
    if (cv.opt.gen_lod_ratio > 0.f && p != std::string::npos && p + 5 == model.name.size() &&
        model.name.rfind("eichar-", 0) != 0 && model.name.rfind("sidekick-", 0) != 0 &&
        model.name.rfind("keira-as-", 0) != 0 && !c3l::merc_gen_lod_exempt(model.name) &&
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

// (AI-assisted) every texel of every level of the texture has alpha 1.0 (as the GPU reads it)
bool texture_alpha_full(const c3l::Texture& d, const std::vector<u8>& data) {
  if (d.format == c3l::TEX_ETC1 || d.format == c3l::TEX_RGB565) {
    return true;
  }
  u32 off = 0;
  for (int l = 0; l < std::max<int>(1, d.levels); l++) {
    const int w = d.w >> l, h = d.h >> l;
    const auto img = ctr_tex::decode_level(data.data() + off, w, h, (ctr_tex::Format)d.format);
    for (int i = 0; i < w * h; i++) {
      if (img.rgba[4 * i + 3] != 255) {
        return false;
      }
    }
    off += c3l::texture_level_bytes(w, h, d.format);
  }
  return true;
}

// (AI-assisted) bytes of a texture with `levels` mip levels
u64 ctr_gpu_level_bytes(int w, int h, int format, int levels) {
  u64 b = 0;
  for (int l = 0; l < levels; l++) {
    b += c3l::texture_level_bytes(w >> l, h >> l, (u8)format);
  }
  return b;
}

// (AI-assisted) v15 draw optimisations (Options::opaque_alpha, Options::atlas), on the triangles
// before chunking. Statistics for the log.
struct DrawOptStats {
  size_t keys_at = 0, tris_at = 0, keys_blend = 0, tris_blend = 0, merged_keys = 0;
  size_t at_candidates = 0, blend_candidates = 0, blocked_coplanar = 0;
  size_t coplanar_pairs = 0, cross_key_pairs = 0;
  int atlases = 0, atlas_cells = 0, atlas_textures_fit = 0, atlas_blocked_coplanar = 0;
  int atlas_elsewhere = 0, atlas_costly = 0;
  size_t atlas_tris = 0, atlas_verts = 0;
  u64 atlas_bytes = 0;
  std::vector<u16> atlased;  // textures moved into atlases (output indices)
  c3l_opt::AtlasStats ast;
};

void optimize_draws(Converter& cv,
                    const Options& opt,
                    std::vector<c3l::Texture>& tex_descs,
                    std::vector<std::vector<u8>>& tex_datas,
                    std::vector<TexUse>& tex_use,
                    DrawOptStats* st) {
  const size_t nk = cv.draw_keys.size();
  auto mode_of = [&](u32 k) {
    DrawMode m;
    m.as_int() = cv.draw_keys[k].mode;
    return m;
  };
  auto any_order = [&](u32 k) {
    const DrawMode m = mode_of(k);
    return !m.get_ab_enable() && m.get_depth_write_enable();
  };
  // textures with alpha 1.0 in every texel of every level (as the GPU reads them)
  std::vector<int> full(tex_descs.size(), -1);
  auto tex_full = [&](u16 t) {
    if (t == 0xffff) {
      return true;
    }
    if (t >= tex_descs.size() || (t < tex_use.size() && tex_use[t].anim_slot)) {
      return false;  // (AI-assisted) an animated slot's texels change at runtime
    }
    if (full[t] < 0) {
      full[t] = texture_alpha_full(tex_descs[t], tex_datas[t]);
    }
    return full[t] == 1;
  };
  // per key: triangles, vertex alpha >= 0x80 everywhere
  std::vector<size_t> key_tris(nk, 0);
  std::vector<u8> key_va(nk, 1);
  for (const auto& t : cv.tris) {
    key_tris[t.draw_key]++;
    for (u32 v : t.v) {
      if (cv.verts[v].rgba[3] < 0x80) {
        key_va[t.draw_key] = 0;
      }
    }
  }
  // coplanar overlaps between different keys (opaque ones: their order is the runtime's sort, by
  // texture, then state): a key whose triangles overlap another key's of the same texture keeps its
  // state (the sort among the texture's draws would change); a texture with any such overlap stays
  // out of the atlases (the sort by texture would change)
  std::vector<u8> key_blocked(nk, 0), tex_blocked(tex_descs.size(), 0);
  {
    std::vector<c3l_opt::Tri> wt;
    std::vector<u32> ids;
    for (u32 i = 0; i < cv.tris.size(); i++) {
      const auto& t = cv.tris[i];
      if (!any_order(t.draw_key)) {
        continue;
      }
      c3l_opt::Tri w;
      for (int k = 0; k < 3; k++) {
        const auto& v = cv.verts[t.v[k]];
        w[k] = {v.x, v.y, v.z};
      }
      wt.push_back(w);
      ids.push_back(i);
    }
    const auto pairs = c3l_opt::coplanar_pairs(wt, {}, c3l_opt::CoplanarOptions{});
    st->coplanar_pairs = pairs.size();
    for (auto [a, b] : pairs) {
      const auto& ta = cv.tris[ids[a]];
      const auto& tb = cv.tris[ids[b]];
      // the near and coarse tfrag versions of a cell are never drawn together
      if (ta.tier != tb.tier && ta.tier && tb.tier && ta.tier + tb.tier == 3) {
        continue;
      }
      // (AI-assisted) v17: nor the near and far versions of a tie group
      if (ta.lod_group != UINT32_MAX && ta.lod_group == tb.lod_group && ta.lod != tb.lod) {
        continue;
      }
      if (ta.draw_key == tb.draw_key) {
        continue;
      }
      st->cross_key_pairs++;
      const auto& ka = cv.draw_keys[ta.draw_key];
      const auto& kb = cv.draw_keys[tb.draw_key];
      if (ka.texture == kb.texture) {
        key_blocked[ta.draw_key] = key_blocked[tb.draw_key] = 1;
      }
      for (u16 t : {ka.texture, kb.texture}) {
        if (t < tex_blocked.size()) {
          tex_blocked[t] = 1;
        }
      }
    }
  }
  std::vector<u32> remap(nk);
  for (u32 k = 0; k < nk; k++) {
    remap[k] = k;
  }
  if (opt.opaque_alpha) {
    constexpr u32 kAtBits = 0xFF00 | (3u << 16) | (1u << 18) | (3u << 21);
    // groups of opaque keys that differ only in their alpha test: the ones whose alpha is 1.0
    // everywhere become one key without the test (the largest aref kept for config opaque_alpha
    // = off); blended ones (alpha blending of Cs * As + Cd * (1 - As)) lose the blending in place
    std::map<std::tuple<u32, u16, u16, u16>, std::vector<u32>> groups;  // (+ env, j2water2)
    for (u32 k = 0; k < nk; k++) {
      if (!key_tris[k]) {
        continue;
      }
      const auto dk = cv.draw_keys[k];
      const DrawMode m = mode_of(k);
      const bool alpha1 = tex_full(dk.texture) && key_va[k];
      const bool at = m.get_at_enable() && m.get_alpha_test() == DrawMode::AlphaTest::GEQUAL;
      const bool blend =
          m.get_ab_enable() && m.get_alpha_blend() == DrawMode::AlphaBlend::SRC_DST_SRC_DST;
      st->at_candidates += at && alpha1;
      st->blend_candidates += blend && alpha1;
      if (!alpha1) {
        continue;
      }
      if (blend) {
        // in place: the same key number (the chunk's draw order: the blended draws keep theirs)
        DrawMode nm = m;
        nm.disable_ab();
        u16 kind = (u16)(dk.kind | c3l::kDrawBlendDropped);
        if (at) {
          nm.disable_at();
          kind |= c3l::kDrawAlphaTestDropped;
        }
        cv.draw_key_ids.erase(dk);
        cv.draw_keys[k] = key_like(dk, nm.as_int(), dk.texture, kind);
        cv.draw_key_ids.emplace(cv.draw_keys[k], k);
        st->keys_blend++;
        st->tris_blend += key_tris[k];
        continue;
      }
      if (!any_order(k) || key_blocked[k] || (m.get_at_enable() && !at)) {
        st->blocked_coplanar += at && key_blocked[k];
        continue;
      }
      groups[{dk.mode & ~kAtBits, dk.texture, dk.kind, dk.env}].push_back(k);
    }
    for (auto& [g, keys] : groups) {
      int aref = -1;
      for (u32 k : keys) {
        const DrawMode m = mode_of(k);
        if (m.get_at_enable()) {
          aref = std::max<int>(aref, m.get_aref());
        }
      }
      if (aref < 0) {
        continue;  // no alpha test among them
      }
      DrawMode nm;
      nm.as_int() = std::get<0>(g);
      nm.set_aref((u8)aref);
      nm.set_alpha_test(DrawMode::AlphaTest::GEQUAL);
      DrawKey tk{nm.as_int(), std::get<1>(g), (u16)(std::get<2>(g) | c3l::kDrawAlphaTestDropped)};
      tk.env = std::get<3>(g);  // (AI-assisted, j2water2)
      const u32 to = cv.key_id(tk);
      for (u32 k : keys) {
        remap[k] = to;
        if (mode_of(k).get_at_enable()) {
          st->keys_at++;
          st->tris_at += key_tris[k];
        }
      }
      st->merged_keys += keys.size() - 1;
    }
    for (auto& t : cv.tris) {
      if (remap[t.draw_key] != t.draw_key) {
        if (t.orig_key == UINT32_MAX) {
          t.orig_key = t.draw_key;  // the vista tier copies with the key from before
        }
        cv.rank_merge(remap[t.draw_key], t.draw_key);
        t.draw_key = remap[t.draw_key];
      }
    }
  }
  if (!opt.atlas) {
    return;
  }
  // ---- atlases ----
  // the textures that fit: every key using it is opaque (any order) and each triangle is within
  // one copy of the texture (clamped axes: 0..1; repeating ones: after a whole number shift), one
  // clamp mode per axis
  const size_t nk2 = cv.draw_keys.size();
  std::vector<u8> key_used(nk2, 0);
  for (const auto& t : cv.tris) {
    key_used[t.draw_key] = 1;
  }
  struct TexFit {
    bool used = false, ok = true;
    int clamp_s = -1, clamp_t = -1;
  };
  std::vector<TexFit> fit(tex_descs.size());
  for (u32 k = 0; k < nk2; k++) {
    const auto& dk = cv.draw_keys[k];
    if (!key_used[k] || dk.texture >= fit.size()) {
      continue;
    }
    auto& f = fit[dk.texture];
    f.used = true;
    const DrawMode m = mode_of(k);
    const int cs = m.get_clamp_s_enable(), ct = m.get_clamp_t_enable();
    if (!any_order(k) || (f.clamp_s >= 0 && f.clamp_s != cs) || (f.clamp_t >= 0 && f.clamp_t != ct)) {
      f.ok = false;
    }
    f.clamp_s = cs;
    f.clamp_t = ct;
  }
  auto st_of = [](float v) { return (int)std::lround(v * 1024.f); };
  // per triangle: its whole number shift (in 1024ths), or none
  auto tri_shift = [&](const SrcTri& t, int* su, int* sv) {
    const auto& f = fit[cv.draw_keys[t.draw_key].texture];
    for (int ax = 0; ax < 2; ax++) {
      int lo = INT32_MAX, hi = INT32_MIN;
      for (u32 v : t.v) {
        const int q = st_of(ax ? cv.verts[v].t : cv.verts[v].s);
        lo = std::min(lo, q);
        hi = std::max(hi, q);
      }
      const bool clamp = ax ? f.clamp_t : f.clamp_s;
      int base = 0;
      if (clamp) {
        if (lo < 0 || hi > 1024) {
          return false;
        }
      } else {
        base = (int)std::floor(lo / 1024.0) * 1024;
        if (hi - base > 1024) {
          return false;
        }
      }
      (ax ? *sv : *su) = base;
    }
    return true;
  };
  for (const auto& t : cv.tris) {
    const u16 tex = cv.draw_keys[t.draw_key].texture;
    if (tex < fit.size() && fit[tex].ok) {
      int su, sv;
      if (!tri_shift(t, &su, &sv)) {
        fit[tex].ok = false;
      }
    }
  }
  // vertices an atlas would add per texture: a vertex whose triangles need different whole
  // number shifts gets a copy for each
  std::vector<u32> extra_verts(tex_descs.size(), 0);
  {
    std::set<std::tuple<u16, u32, int, int>> with;
    std::set<std::pair<u16, u32>> plain;
    for (const auto& t : cv.tris) {
      const u16 tex = cv.draw_keys[t.draw_key].texture;
      if (tex >= fit.size() || !fit[tex].ok) {
        continue;
      }
      int su = 0, sv = 0;
      tri_shift(t, &su, &sv);
      for (u32 v : t.v) {
        with.insert({tex, v, su, sv});
        plain.insert({tex, v});
      }
    }
    for (const auto& w : with) {
      extra_verts[std::get<0>(w)]++;
    }
    for (const auto& p0 : plain) {
      extra_verts[p0.first]--;
    }
  }
  // textures the vista tier will draw (its copies keep the original texture: they are welded by
  // position, and the atlas copies' shifted coordinates would mix there) or merc models draw: they
  // stay in the file next to their atlas copy (counted in the atlas's cost)
  std::vector<u8> elsewhere(tex_descs.size(), 0);
  {
    bool has_lowres = false;
    for (const auto& t : cv.tris) {
      has_lowres |= t.tier == 3;
    }
    for (const auto& t : cv.tris) {
      const auto& dk = cv.draw_keys[t.draw_key];
      const int kind = dk.kind & 0xff;
      const bool vista = opt.vista && (t.tier == 3 || (t.tier == 0 && kind == 1 && t.detail == 0) ||
                                       (t.tier == 2 && kind == 0 && !has_lowres));
      if (vista && dk.texture < elsewhere.size()) {
        elsewhere[dk.texture] = 1;
      }
    }
    for (u16 t = 0; t < tex_use.size() && t < elsewhere.size(); t++) {
      elsewhere[t] |= tex_use[t].merc;
    }
  }
  // by format and levels
  std::map<std::pair<int, int>, std::vector<u16>> groups;
  for (u16 t = 0; t < fit.size(); t++) {
    const auto& d = tex_descs[t];
    if (!fit[t].used || !fit[t].ok || d.w > opt.atlas_max || d.h > opt.atlas_max ||
        d.format == c3l::TEX_RGBA8 || (t < tex_use.size() && tex_use[t].anim_slot)) {
      continue;
    }
    st->atlas_textures_fit++;
    st->atlas_elsewhere += elsewhere[t];
    if (tex_blocked[t]) {
      st->atlas_blocked_coplanar++;
      continue;
    }
    groups[{d.format, std::max<int>(1, d.levels)}].push_back(t);
  }
  // shelf packing into the smallest power of two atlases (by bytes) that hold them, at most
  // 256 << (levels - 1) on a side: 1/32768 texture coordinates then land exactly on the cells'
  // own 1/1024 ones (32768 / atlas side units per atlas texel, a multiple of 1024 / the cells'
  // shortest side 8 << (levels - 1))
  struct Cell {
    u16 tex;
    int atlas, x, y;
  };
  std::vector<int> cell_of(tex_descs.size(), -1);
  std::vector<Cell> cells;
  struct Atlas {
    int format, levels, w = 0, h = 0;
    u16 tex;  // output index
  };
  std::vector<Atlas> atlases;
  // place list[from..] into w x h: how many fit (in order), and where
  auto pack = [&](const std::vector<u16>& list, size_t from, int w, int h, int sp,
                  std::vector<std::pair<int, int>>* pos) {
    pos->clear();
    int x = sp, y = sp, shelf_h = 0;
    for (size_t j = from; j < list.size(); j++) {
      const auto& d = tex_descs[list[j]];
      if (x + d.w + sp > w) {
        x = sp;
        y += shelf_h + sp;
        shelf_h = 0;
      }
      if (y + d.h + sp > h || x + d.w + sp > w) {
        break;
      }
      pos->push_back({x, y});
      x += d.w + sp;
      shelf_h = std::max<int>(shelf_h, d.h);
    }
    return pos->size();
  };
  for (auto& [g, list] : groups) {
    if (list.size() < 2) {
      continue;
    }
    const int levels = g.second;
    const int sp = 4 << (levels - 1);  // the gap between cells (and the margin)
    const int max_side = std::min(1024, 256 << (levels - 1));
    std::stable_sort(list.begin(), list.end(), [&](u16 a, u16 b) {
      return std::make_pair(tex_descs[a].h, tex_descs[a].w) >
             std::make_pair(tex_descs[b].h, tex_descs[b].w);
    });
    size_t i = 0;
    while (i + 1 < list.size()) {
      // the smallest atlas that holds the rest; if none does, the largest one (as many as fit)
      int best_w = 0, best_h = 0;
      size_t best_n = 0;
      u64 best_area = UINT64_MAX;
      std::vector<std::pair<int, int>> pos;
      for (int w = 16; w <= max_side; w *= 2) {
        for (int h = 16; h <= max_side; h *= 2) {
          const size_t n = pack(list, i, w, h, sp, &pos);
          const u64 area = (u64)w * h;
          if (n == list.size() - i ? (best_n < n || area < best_area) : (n > best_n && best_n < list.size() - i)) {
            if (n == list.size() - i || n > best_n) {
              best_n = n;
              best_w = w;
              best_h = h;
              best_area = area;
            }
          }
        }
      }
      if (best_n < 2) {
        break;
      }
      {
        // worth it: the memory it adds (texels, and the vertex copies) per bind it saves at most
        // atlas_kb_per_bind
        // the textures the atlas replaces (not the vista's or merc's), less the vertex copies
        double saved = 0;
        for (size_t k = 0; k < best_n; k++) {
          saved += elsewhere[list[i + k]] ? 0.0 : (double)tex_datas[list[i + k]].size();
          saved -= 16.0 * extra_verts[list[i + k]];
        }
        const u64 atlas_bytes = ctr_gpu_level_bytes(best_w, best_h, g.first, levels);
        if ((double)atlas_bytes - saved > opt.atlas_kb_per_bind * 1024.0 * (double)(best_n - 1)) {
          st->atlas_costly += (int)best_n;
          i += best_n;
          continue;
        }
      }
      pack(list, i, best_w, best_h, sp, &pos);
      Atlas a;
      a.format = g.first;
      a.levels = levels;
      a.w = best_w;
      a.h = best_h;
      a.tex = (u16)tex_descs.size();
      std::vector<c3l_opt::AtlasCell> ac;
      for (size_t k = 0; k < best_n; k++) {
        const u16 t = list[i + k];
        Cell c{t, (int)atlases.size(), pos[k].first, pos[k].second};
        cell_of[t] = (int)cells.size();
        cells.push_back(c);
        c3l_opt::AtlasCell x0;
        x0.data = tex_datas[t].data();
        x0.w = tex_descs[t].w;
        x0.h = tex_descs[t].h;
        x0.x = c.x;
        x0.y = c.y;
        x0.clamp_s = fit[t].clamp_s == 1;
        x0.clamp_t = fit[t].clamp_t == 1;
        ac.push_back(x0);
        st->atlased.push_back(t);
      }
      i += best_n;
      c3l::Texture desc{};
      desc.w = (u16)a.w;
      desc.h = (u16)a.h;
      desc.format = (u8)a.format;
      desc.levels = (u8)a.levels;
      tex_datas.push_back(
          c3l_opt::build_atlas(a.w, a.h, a.format, a.levels, ac, opt.etc1_quality, &st->ast));
      tex_descs.push_back(desc);
      tex_use.emplace_back();
      st->atlas_bytes += tex_datas.back().size();
      {
        u64 cb = 0;
        std::string sizes;
        for (size_t k = 0; k < best_n; k++) {
          cb += tex_datas[list[i - best_n + k]].size();
          sizes += fmt::format(" {}x{}", tex_descs[list[i - best_n + k]].w,
                               tex_descs[list[i - best_n + k]].h);
        }
        lg::debug("atlas {}x{} format {} levels {}: {} cells ({} bytes) -> {} bytes:{}", a.w, a.h,
                  a.format, a.levels, best_n, cb, tex_datas.back().size(), sizes);
      }
      atlases.push_back(a);
      st->atlases++;
      st->atlas_cells += (int)best_n;
    }
  }
  if (cells.empty()) {
    return;
  }
  // the triangles: the atlas's key, copies of their vertices with the atlas texture coordinates
  std::map<std::tuple<u32, int, int, int>, u32> copies;  // (vertex, cell, shift s, shift t)
  std::vector<u32> atlas_key(nk2, UINT32_MAX);
  for (auto& t : cv.tris) {
    const auto& dk = cv.draw_keys[t.draw_key];
    if (dk.texture >= cell_of.size() || cell_of[dk.texture] < 0) {
      continue;
    }
    const int ci = cell_of[dk.texture];
    const Cell& c = cells[ci];
    const Atlas& a = atlases[c.atlas];
    const auto& d = tex_descs[c.tex];
    int su = 0, sv = 0;
    tri_shift(t, &su, &sv);
    if (atlas_key[t.draw_key] == UINT32_MAX) {
      atlas_key[t.draw_key] =
          cv.key_id(key_like(dk, dk.mode, a.tex, (u16)(dk.kind | c3l::kDrawAtlasUV)));
    }
    const u32 from = t.draw_key;
    if (t.orig_key == UINT32_MAX) {
      t.orig_key = t.draw_key;
    }
    t.draw_key = atlas_key[from];
    cv.rank_merge(t.draw_key, from);
    // 1/32768 units per atlas texel: kx = 32768 / atlas width; a cell texel is 1024 / w of its
    // coordinates: st_atlas = kx * (x + (st - shift) * w / 1024), exact (kx * w / 1024 is whole)
    const int kx = (int)c3l::kAtlasUVScale / a.w, ky = (int)c3l::kAtlasUVScale / a.h;
    for (u32& vi : t.v) {
      auto [it, fresh] = copies.emplace(std::make_tuple(vi, ci, su, sv), (u32)cv.verts.size());
      if (fresh) {
        SrcVertex v = cv.verts[vi];
        v.orig = vi;
        const int s0 = st_of(v.s) - su, t0 = st_of(v.t) - sv;
        v.ast[0] = (s16)(kx * c.x + s0 * (kx * d.w / 1024));
        v.ast[1] = (s16)(ky * c.y + t0 * (ky * d.h / 1024));
        cv.verts.push_back(v);
        st->atlas_verts++;
      }
      vi = it->second;
    }
    st->atlas_tris++;
  }
}


// (AI-assisted) v17 tie levels of detail: the levels whose near versions for every group don't fit
// in linear memory next to their neighbours (KB of extra vertex and index data, estimated as in
// select_tie_lods; the others get every group). Planned Oct 17 from Azahar's linear free memory
// with level pairs loaded (level_pairs.pad, 3ds-sdcard v16 levels; worst village3 + ogre 9.1 MB)
// plus the 2 MB the smaller vertex stream buffers freed, the rule: every pair of neighbouring levels
// keeps >= 3 MB (+ 256 KB margin). Pairs the pad doesn't load were estimated from the levels' sizes
// (village2 + rolling ~6.5 MB, village2 + ogre ~7.3 MB before: the tightest). The groups left
// coarse are logged ("left coarse").
struct TieLodBudget {
  const char* level;
  float kb;
};
constexpr TieLodBudget kTieLodBudgets[] = {
    {"citadel", 5800},  {"firecanyon", 6000}, {"jungle", 4500}, {"lavatube", 5700},
    {"misty", 4700},    {"ogre", 3300},       {"rolling", 2400}, {"swamp", 4900},
    {"village1", 4500}, {"village2", 2500},
};

// (AI-assisted) v17 tie levels of detail: statistics for the log
struct TieLodStats {
  size_t groups = 0, candidates = 0, selected = 0, no_dist = 0;
  size_t core = 0;  // (AI-assisted, streaming) selected groups the budget took
  size_t near_tris = 0, far_tris = 0;          // all groups: near / far geometry
  size_t sel_near_tris = 0, sel_far_tris = 0;  // the selected groups
  double cost_kb = 0, cost_all_kb = 0;         // estimated extra vertex + index data
  double budget_kb = 0;
  double ms = 0;
  // per prototype: groups selected / candidates, near triangles of the candidates left coarse
  struct Proto {
    u32 selected = 0, candidates = 0;
    size_t left_tris = 0;
    float worst_left = 0;  // largest shape error left coarse (meters)
  };
  std::map<std::string, Proto> protos;
};

/*!
 * (AI-assisted) v17: picks the tie instance groups that get a near version (geometry
 * opt.tie_near_geo, drawn up to the prototype's PS2 far-plane; the far version is geometry
 * opt.tie_geo). Every group whose shape changes visibly is a candidate: its shape error
 * (tie_lod::shape_error) seen from the larger of its radius and tie_lod_view_m is at least
 * tie_lod_min_angle. The candidates with the most error per byte of extra data (near vertices not
 * in the far version, near indices) are taken until tie_lod_budget_kb is used.
 */
void select_tie_lods(tfrag3::Level& level, const Options& opt, Converter* cv, TieLodStats* st) {
  Timer timer;
  std::unordered_map<u64, tie_lod::Group> groups;
  for (int slot = 0; slot < 2; slot++) {
    auto& trees = level.tie_trees[slot ? opt.tie_geo : opt.tie_near_geo];
    for (u32 t = 0; t < trees.size(); t++) {
      if (trees[t].unpacked.indices.empty()) {
        trees[t].unpack();
      }
      tie_lod::collect(trees[t], t, slot, &groups);
    }
  }
  std::vector<std::pair<u64, const tie_lod::Group*>> list;
  for (const auto& [k, g] : groups) {
    list.push_back({k, &g});
  }
  std::sort(list.begin(), list.end(),
            [](const auto& a, const auto& b) { return a.first < b.first; });
  std::vector<float> err(list.size());
  {
    BS::thread_pool pool(opt.threads > 0 ? opt.threads : std::thread::hardware_concurrency());
    pool.push_loop(list.size(), [&](size_t a, size_t b) {
      for (size_t i = a; i < b; i++) {
        err[i] = tie_lod::shape_error(*list[i].second);
      }
    });
    pool.wait_for_tasks();
  }
  struct Cand {
    size_t i;
    double value;
    u64 cost;
  };
  std::vector<Cand> cands;
  std::vector<std::string> names(list.size());
  std::vector<float> dists(list.size());
  for (size_t i = 0; i < list.size(); i++) {
    const auto& g = *list[i].second;
    const u32 tree = (u32)((list[i].first >> 40) & 0x7fffff);
    const auto& far_trees = level.tie_trees[opt.tie_geo];
    names[i] = g.proto != 0xffff && tree < far_trees.size() &&
                       g.proto < far_trees[tree].proto_names.size()
                   ? far_trees[tree].proto_names[g.proto]
                   : std::string("(wind)");
    float mid = 0, far = 0;
    if (!tie_lod::ps2_dists(g_dist_prefix + level.level_name, names[i], &mid, &far)) {
      far = opt.tie_lod_default_m;
      st->no_dist += g.proto != 0xffff;
    }
    dists[i] = far * 4096.f;
    st->groups++;
    st->near_tris += g.tris[0].size();
    st->far_tris += g.tris[1].size();
    if (g.tris[0].empty()) {
      continue;
    }
    const auto bs = tie_lod::bounds(g);
    const double benefit = err[i] / std::max(bs[3], opt.tie_lod_view_m * 4096.f);
    if (benefit < opt.tie_lod_min_angle) {
      continue;
    }
    // extra data: the near vertices the far version doesn't have (its corners are near ones),
    // the near indices, a visibility run per draw and the group
    // (vertices: position and texture coordinates; colors are the same at both)
    std::set<std::array<float, 5>> p0(g.corners[0].begin(), g.corners[0].end());
    size_t shared = 0;
    for (const auto& c : std::set<std::array<float, 5>>(g.corners[1].begin(), g.corners[1].end())) {
      shared += p0.count(c);
    }
    const u64 cost = (p0.size() - shared) * sizeof(c3l::Vertex) +
                     g.tris[0].size() * 3 * sizeof(u16) + 64;
    cands.push_back({i, benefit / cost, cost});
    st->cost_all_kb += cost / 1024.0;
    auto& pr = st->protos[names[i]];
    pr.candidates++;
  }
  st->candidates = cands.size();
  std::sort(cands.begin(), cands.end(), [&](const Cand& a, const Cand& b) {
    return a.value != b.value ? a.value > b.value : list[a.i].first < list[b.i].first;
  });
  double budget_kb = opt.tie_lod_budget_kb;
  if (budget_kb < 0) {
    budget_kb = 1e12;  // every group
    for (const auto& b : kTieLodBudgets) {
      if (g_dist_prefix + level.level_name == b.level) {  // (AI-assisted) Jak 1's levels
        budget_kb = b.kb;
      }
    }
  }
  st->budget_kb = budget_kb;
  const u64 budget = (u64)(budget_kb * 1024.0);
  u64 used = 0;
  std::vector<u8> picked(list.size(), 0), core(list.size(), 0);
  for (const auto& c : cands) {
    if (used + c.cost <= budget) {
      used += c.cost;
      picked[c.i] = 1;
      core[c.i] = 1;
    } else {
      auto& pr = st->protos[names[c.i]];
      pr.left_tris += list[c.i].second->tris[0].size();
      pr.worst_left = std::max(pr.worst_left, err[c.i] / 4096.f);
      // (AI-assisted, streaming) v19: every candidate; the budget's are the core set
      picked[c.i] = opt.stream ? 1 : 0;
    }
  }
  st->cost_kb = used / 1024.0;
  // the groups in key order (deterministic indices)
  for (size_t i = 0; i < list.size(); i++) {
    if (!picked[i]) {
      continue;
    }
    const auto& g = *list[i].second;
    const auto bs = tie_lod::bounds(g);
    c3l::TieLodGroup lg{};
    for (int c = 0; c < 4; c++) {
      lg.bsphere[c] = bs[c];
    }
    lg.dist = dists[i];
    cv->lod_group_error.push_back(err[i]);
    cv->lod_group_core.push_back(core[i]);  // (AI-assisted, streaming)
    st->core += core[i];
    cv->lod_group_of[list[i].first] = (u32)cv->lod_groups.size();
    cv->lod_groups.push_back(lg);
    cv->lod_group_detail.push_back(bs[3] < opt.detail_radius * 4096.f   ? 1
                                   : bs[3] < opt.medium_radius * 4096.f ? 2
                                                                        : 0);
    st->selected++;
    st->sel_near_tris += g.tris[0].size();
    st->sel_far_tris += g.tris[1].size();
    st->protos[names[i]].selected++;
  }
  if (cv->lod_groups.size() > 0xfffe) {
    // (RunLod::group is 16 bits)
    lg::warn("{}: {} tie LOD groups, only levels of detail for the first 65534",
             level.level_name, cv->lod_groups.size());
    cv->lod_groups.resize(0xfffe);
    cv->lod_group_detail.resize(0xfffe);
    cv->lod_group_error.resize(0xfffe);
    cv->lod_group_core.resize(0xfffe);
    for (auto it = cv->lod_group_of.begin(); it != cv->lod_group_of.end();) {
      it = it->second >= 0xfffe ? cv->lod_group_of.erase(it) : std::next(it);
    }
  }
  st->ms = timer.getMs();
}

// (AI-assisted, streaming) a group's near version is needed within this many x its shape error (+
// its radius) of its center: gameplay's focal length 256 / tie_lod_px 4, x 1.1 (the hysteresis)
constexpr float kStreamNeedK = 256.f / 4.f * 1.1f;
int stream_need_class(const Converter& cv, const Options& opt, u32 g) {
  const float e = g < cv.lod_group_error.size() ? cv.lod_group_error[g] : cv.lod_groups[g].dist;
  const float m = (e * kStreamNeedK + cv.lod_groups[g].bsphere[3]) / 4096.f;
  int c = 0;
  while (c < (int)opt.stream_classes.size() && m >= opt.stream_classes[c]) {
    c++;
  }
  return c;
}
float stream_class_cell(const Options& opt, int nc) {
  return nc < (int)opt.stream_class_cells.size() ? opt.stream_class_cells[nc] : opt.stream_cell;
}

bool convert(const fs::path& in, const fs::path& out, const Options& opt_in) {
  Options opt = opt_in;
  if (opt.game.empty()) {
    // (AI-assisted) out/jak2/fr3/<level>.fr3: Jak II
    opt.game = in.parent_path().parent_path().filename() == "jak2" ? "jak2" : "jak1";
  }
  const bool jak2 = opt.game == "jak2";
  g_dist_prefix = jak2 ? "jak2/" : "";
  // (AI-assisted, j2vis) v20 prototype visibility: Jak II's task-toggled prototypes (v20 files only:
  // with near chunks)
  opt.proto_vis = jak2 && opt.stream && opt.shrub_ps2_dist && !opt.no_proto_vis;
  // (the interior and fixed mood lists are Jak 1's levels)
  if (!jak2 && is_interior(in.stem().string())) {
    opt.vista = false;
  }
  if (opt.palette < 0) {
    opt.palette = jak2 ? 1 : auto_palette(in.stem().string());
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
  if (!opt.keira_from.empty()) {
    // (AI-assisted) KEIRA.c3l: only her model, from village1, bound to the rig (keira_model.h)
    auto vdata = file_util::read_binary_file(opt.keira_from);
    auto vdecomp = compression::decompress_zstd(vdata.data(), vdata.size());
    Serializer vser(vdecomp.data(), vdecomp.size());
    tfrag3::Level village1;
    village1.serialize(vser);
    tfrag3::Level keira;
    if (!keira_model::make_level(level, village1, &keira)) {
      return false;
    }
    level = std::move(keira);
    opt.merc_only = true;
  }

  Converter cv;
  cv.opt = opt;
  add_anim_slot_textures(&level, &cv.anim_slot_tex);
  if (jak2) {
    add_anim_slot_base_textures(level, &cv.anim_slot_tex);
  }
  cv.level = &level;
  // (AI-assisted, j2water2) c3l v21 envmapped tie shine: Jak II (in the extension header after
  // v20's, so with near chunks)
  cv.env_ties_on = jak2 && opt.stream && !opt.no_env_ties;
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
  TieLodStats lstats;
  if (!opt.no_tie && !opt.merc_only) {
    // (AI-assisted) v17: which instance groups get a near version (select_tie_lods)
    const bool lod = opt.tie_lod && opt.tie_near_geo >= 0 && opt.tie_near_geo < opt.tie_geo &&
                     level.tie_trees[opt.tie_near_geo].size() == level.tie_trees[opt.tie_geo].size();
    if (lod) {
      select_tie_lods(level, opt, &cv, &lstats);
    }
    for (u32 t = 0; t < level.tie_trees[opt.tie_geo].size(); t++) {
      cv.cur_tree = t;
      cv.cur_lod_slot = lod ? 1 : -1;
      cv.add_tie_tree(level.tie_trees[opt.tie_geo][t]);
      tie_trees++;
      if (lod && !cv.lod_groups.empty()) {
        cv.cur_lod_slot = 0;
        cv.add_tie_tree(level.tie_trees[opt.tie_near_geo][t]);
      }
    }
    cv.cur_lod_slot = -1;
    if (lod && opt.tie_far_near_colors) {
      // (AI-assisted, lod-light)
      Converter::FarColorStats fs;
      cv.far_colors_from_near(&fs);
      const double a = std::max(fs.area, 1e-9);
      lg::info("{}: tie levels of detail: far version colors fitted to the near versions: {} of {} "
               "vertices changed; far - near surface color per far triangle (GS units, area "
               "weighted, mean |d| / mean d): geometry 3's own {:.2f} / {:+.2f}, fit {:.2f} / "
               "{:+.2f}, picked {:.2f} / {:+.2f}",
               in.stem().string(), fs.changed, fs.far_verts, fs.abs_own / a, fs.sgn_own / a,
               fs.abs_fit / a, fs.sgn_fit / a, fs.abs_pick / a, fs.sgn_pick / a);
    }
  }
  int shrub_trees = 0;
  if (!opt.no_shrub && !opt.merc_only) {
    for (auto& tree : level.shrub_trees) {
      cv.add_shrub_tree(tree);
      shrub_trees++;
    }
  }

  // ---- textures (in parallel: ETC1 encoding is slow) ----
  // (AI-assisted) before the chunks: the v15 draw optimisations look at the encoded texels
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
        c3l::set_texture_anim_slot(&tex_descs[i], cv.tex_use[i].anim_slot);  // (AI-assisted)
      }
    });
    pool.wait_for_tasks();
  }
  // (AI-assisted) v15 draw optimisations: alpha tests / blending that can't change a pixel, atlases
  DrawOptStats dstats;
  if (!opt.merc_only) {
    optimize_draws(cv, opt, tex_descs, tex_datas, cv.tex_use, &dstats);
    tex_psnr.resize(tex_descs.size(), 99.0);
    tex_gate.resize(tex_descs.size());
  }
  // ---- chunking: grid cells by triangle centroid ----
  // shrubs are small and only drawn up close: smaller cells cull them better
  auto cell_of_detail = [&](int detail) {
    return (detail == 3 ? opt.shrub_cell : opt.cell_meters) * 4096.f;
  };
  // (AI-assisted, j2water2) v21: an envmap shine with fewer than kMinEnvTris triangles in the
  // level isn't worth its own draws (each shine kind splits draws off the plain ones): plain
  {
    constexpr size_t kMinEnvTris = 256;
    std::vector<size_t> n(cv.draw_keys.size(), 0);
    for (const auto& t : cv.tris) {
      n[t.draw_key]++;
    }
    std::map<u16, size_t> per_env;
    for (size_t k = 0; k < n.size(); k++) {
      if (cv.draw_keys[k].env != 0xffff) {
        per_env[cv.draw_keys[k].env] += n[k];
      }
    }
    size_t dropped = 0;
    for (auto& t : cv.tris) {
      const DrawKey dk = cv.draw_keys[t.draw_key];
      if (dk.env != 0xffff && per_env[dk.env] < kMinEnvTris) {
        DrawKey pk = dk;
        pk.env = 0xffff;
        t.draw_key = cv.key_id(pk);
        dropped++;
      }
    }
    if (dropped) {
      lg::info("{}: envmapped ties: {} tris of small shines drawn plain", level.level_name,
               dropped);
      cv.env_tie_tris -= std::min(cv.env_tie_tris, dropped);
    }
  }
  // (detail, cell, kind) -> triangle indices. (AI-assisted) The kind (Draw::kind: tfrag, tie,
  // shrub) keeps every chunk to one (Chunk::kind); with the default tiers it splits nothing.
  using CellKey = std::tuple<int, int, int, int, int>;
  constexpr int kEnvCellBit = 0x40;  // (AI-assisted, j2water2) in the kind
  std::map<CellKey, std::vector<u32>> cells;
  for (u32 i = 0; i < cv.tris.size(); i++) {
    const auto& t = cv.tris[i];
    float cx = 0, cy = 0, cz = 0;
    if (t.lod_group != UINT32_MAX) {
      // (AI-assisted) v17: a LOD group's triangles (both versions) in its center's cell: one chunk
      // holds both, and they share its vertices (the far corners are near ones)
      const auto& g = cv.lod_groups[t.lod_group];
      cx = 3 * g.bsphere[0];
      cy = 3 * g.bsphere[1];
      cz = 3 * g.bsphere[2];
    } else {
      for (int k = 0; k < 3; k++) {
        cx += cv.verts[t.v[k]].x;
        cy += cv.verts[t.v[k]].y;
        cz += cv.verts[t.v[k]].z;
      }
    }
    // (AI-assisted, streaming) v19 near versions: near chunks of stream_cell meters (or their need
    // class's cell size)
    int nc = 0;
    if (t.stream && t.lod_group < cv.lod_groups.size()) {
      nc = stream_need_class(cv, opt, t.lod_group);
    }
    const float cell = t.stream ? stream_class_cell(opt, nc) * 4096.f : cell_of_detail((int)t.detail);
    // (AI-assisted) detail + 4 * tier + 64 * shrub distance class (+ 1024 * (stream class + 3 *
    // need class): the near chunks after the others)
    auto key = std::make_tuple(
        (int)(t.detail + 4 * t.tier + 64 * t.sclass + (t.stream ? 1024 * (t.stream + 3 * nc) : 0)),
                               (int)std::floor(cx / 3 / cell),
                               (int)std::floor(cy / 3 / cell), (int)std::floor(cz / 3 / cell),
                               (int)(cv.draw_keys[t.draw_key].kind & 0xff) |
                                   // (AI-assisted, j2water2) v21: envmapped tie triangles in
                                   // chunks of their own (only those get normals)
                                   (cv.draw_keys[t.draw_key].env != 0xffff ? kEnvCellBit : 0));
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
        // (AI-assisted) v15: by key rank (Converter::key_rank), then key
        const u32 ra = cv.rank_of(ta.draw_key), rb = cv.rank_of(tb.draw_key);
        if (ra != rb) {
          return ra < rb;
        }
        if (ta.draw_key != tb.draw_key) {
          return ta.draw_key < tb.draw_key;
        }
        // (AI-assisted) v17: a draw's near versions, then the triangles of one version, then the
        // far versions (c3l::RunLod), each by visibility id (BVH order: near runs drawn together
        // are mostly neighbours), then LOD group
        constexpr u8 kLodRank[3] = {1, 0, 2};
        if (ta.lod != tb.lod) {
          return kLodRank[ta.lod] < kLodRank[tb.lod];
        }
        if (ta.vis != tb.vis) {
          return ta.vis < tb.vis;
        }
        // (AI-assisted, j2vis) v20: then the toggled prototype (0xffff, every other triangle,
        // last: the order of levels without such prototypes doesn't change)
        return ta.lod_group != tb.lod_group ? ta.lod_group < tb.lod_group : ta.proto < tb.proto;
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
  // (AI-assisted, streaming) v19: the near chunks (the last of these plans: their keys sort last) in
  // spatial order, so that the ones the runtime reads together lie together in the file: by the
  // Morton order of their cells (x, z), then height, then key
  size_t first_stream_plan = plans.size();
  for (size_t pi = 0; pi < plans.size(); pi++) {
    if (std::get<0>(plans[pi].key) >= 1024) {
      first_stream_plan = pi;
      break;
    }
  }
  {
    int mx = INT32_MAX, mz = INT32_MAX;
    for (size_t pi = first_stream_plan; pi < plans.size(); pi++) {
      mx = std::min(mx, std::get<1>(plans[pi].key));
      mz = std::min(mz, std::get<3>(plans[pi].key));
    }
    auto morton = [&](const CellKey& k) {
      const u32 x = (u32)(std::get<1>(k) - mx) & 0xffff, z = (u32)(std::get<3>(k) - mz) & 0xffff;
      u64 m = 0;
      for (int b = 0; b < 16; b++) {
        m |= (u64)((x >> b) & 1) << (2 * b) | (u64)((z >> b) & 1) << (2 * b + 1);
      }
      return m;
    };
    std::stable_sort(plans.begin() + first_stream_plan, plans.end(),
                     [&](const ChunkPlan& a, const ChunkPlan& b) {
                       const u64 ma = morton(a.key), mb = morton(b.key);
                       if (ma != mb) {
                         return ma < mb;
                       }
                       if (std::get<2>(a.key) != std::get<2>(b.key)) {
                         return std::get<2>(a.key) < std::get<2>(b.key);
                       }
                       return a.key < b.key;
                     });
  }
  // (AI-assisted) the vista tier (Options::vista): copies of the far content, in large cells
  const size_t normal_plans = plans.size();
  size_t vista_src_tris = 0, vista_tris = 0, vista_flat_tris = 0;
  if (opt.vista && !opt.merc_only) {
    bool has_lowres = false;
    for (const auto& cp : plans) {
      has_lowres |= std::get<0>(cp.key) / 4 % 16 == 3;
    }
    const float vcell = opt.vista_cell * 4096.f;
    std::map<CellKey, std::vector<u32>> vcells;
    // (AI-assisted) vista_flat: the untextured copies' cells, and the textures' average colors
    // (alpha weighted, 0..255; untextured: 128 = the vertex color unchanged)
    std::map<CellKey, std::vector<u32>> fcells;
    std::map<u16, std::array<float, 3>> tex_avg;
    auto avg_color = [&](u16 tex) {
      auto it = tex_avg.find(tex);
      if (it != tex_avg.end()) {
        return it->second;
      }
      std::array<float, 3> c = {128.f, 128.f, 128.f};
      if (tex != 0xffff) {
        const auto& t = cv.level->textures.at(cv.tex_order.at(tex));
        double sum[3] = {0, 0, 0}, w = 0, plain[3] = {0, 0, 0};
        for (u32 p : t.data) {
          const double a = (p >> 24) & 0xff;
          for (int k = 0; k < 3; k++) {
            const double v = (p >> (8 * k)) & 0xff;
            sum[k] += v * a;
            plain[k] += v;
          }
          w += a;
        }
        for (int k = 0; k < 3; k++) {
          c[k] = (float)(w > 0 ? sum[k] / w : (t.data.empty() ? 128.0 : plain[k] / t.data.size()));
        }
      }
      tex_avg[tex] = c;
      return c;
    };
    for (size_t pi = 0; pi < normal_plans && pi < 0xffff; pi++) {
      const auto& cp = plans[pi];
      const int tier = std::get<0>(cp.key) / 4 % 16, detail = std::get<0>(cp.key) % 4;
      const int kind = std::get<4>(cp.key) & ~kEnvCellBit;  // Draw::kind: 0 tfrag, 1 tie, 2 shrub
      const bool lowres = tier == 3;
      // (AI-assisted, streaming) not the near chunks (near versions only)
      const bool landmark = tier == 0 && kind == 1 && detail == 0 && std::get<0>(cp.key) < 1024;
      const bool coarse = tier == 2 && kind == 0 && !has_lowres;
      if (!lowres && !landmark && !coarse) {
        continue;
      }
      // by vista cell and draw: simplified on their own (each copy stays one source chunk's)
      // (AI-assisted, j2vis) and by toggled prototype (v20: its copies are runs of their own too;
      // 0xffff for all the others, so their groups don't change)
      std::map<std::tuple<int, int, u32, u16>, std::vector<u32>> groups;
      for (u32 t : cp.tris) {
        const auto& tr = cv.tris[t];
        if (tr.lod == c3l::kRunLodNear) {
          continue;  // (AI-assisted) v17: the vista copies the far versions
        }
        float cx = 0, cz = 0;
        for (int k = 0; k < 3; k++) {
          cx += cv.verts[tr.v[k]].x / 3;
          cz += cv.verts[tr.v[k]].z / 3;
        }
        // (AI-assisted) v15: with the draw key and vertices from before the atlas (welded by
        // position below: the atlas copies' shifted texture coordinates would mix)
        groups[{(int)std::floor(cx / vcell), (int)std::floor(cz / vcell),
                tr.orig_key == UINT32_MAX ? tr.draw_key : tr.orig_key, tr.proto}]
            .push_back(t);
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
            u32 vi = cv.tris[t].v[k];
            if (cv.verts[vi].orig != UINT32_MAX) {
              vi = cv.verts[vi].orig;  // (AI-assisted) not the atlas copy
            }
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
        std::unordered_map<u32, u32> made, made_flat;
        // (AI-assisted) vista_flat: the copies' draw key (untextured, the draw mode without its
        // texture parts: clamp, filter, tcc, alpha test) and color
        u32 src_key_id = cv.tris[ids[0]].orig_key == UINT32_MAX ? cv.tris[ids[0]].draw_key
                                                                   : cv.tris[ids[0]].orig_key;
        if (cv.draw_keys[src_key_id].env != 0xffff) {
          // (AI-assisted, j2water2) the vista draws no envmap shine (it needs the normals)
          DrawKey nk = cv.draw_keys[src_key_id];
          nk.env = 0xffff;
          src_key_id = cv.key_id(nk);
        }
        const DrawKey src_key = cv.draw_keys[src_key_id];
        constexpr u32 kTextureModeBits = 0x00E7FFE0;  // bits 5-18, 21-23
        const u32 flat_key =
            opt.vista_flat
                ? cv.key_id(DrawKey{src_key.mode & ~kTextureModeBits, 0xffff,
                                    (u16)(c3l::CHUNK_KIND_VISTA_FLAT - c3l::CHUNK_KIND_TFRAG)})
                : 0;
        const std::array<float, 3> flat_rgb =
            opt.vista_flat ? avg_color(src_key.texture) : std::array<float, 3>{128.f, 128.f, 128.f};
        for (size_t i = 0; i + 2 < out_idx.size(); i += 3) {
          SrcTri nt = cv.tris[ids[0]];
          nt.draw_key = src_key_id;  // (AI-assisted) not the atlas's
          nt.orig_key = UINT32_MAX;
          nt.lod = c3l::kRunLodAlways;  // (AI-assisted) v17: one version
          nt.lod_group = UINT32_MAX;
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
          if (opt.vista_flat) {
            // (AI-assisted) the untextured copy: textured output = texture * vertex / 128
            SrcTri ft = nt;
            for (int k = 0; k < 3; k++) {
              const u32 r = out_idx[i + k];
              auto [it, fresh] = made_flat.emplace(r, (u32)cv.verts.size());
              if (fresh) {
                SrcVertex v = cv.verts[rep_src[r]];
                for (int c = 0; c < 3; c++) {
                  v.rgba[c] = (u8)std::clamp((int)std::lround(v.rgba[c] * flat_rgb[c] / 128.f), 0,
                                             255);
                }
                {
                  // (AI-assisted) v16: the time of day color times the texture's color
                  u8 mul[3];
                  for (int c = 0; c < 3; c++) {
                    mul[c] = (u8)std::clamp((int)std::lround(flat_rgb[c]), 0, 255);
                  }
                  v.tod = cv.tod_mul(v.tod, mul);
                }
                cv.verts.push_back(v);
                vista_vertex.resize(cv.verts.size(), 0);
                vista_vertex.back() = 1;
              }
              ft.v[k] = it->second;
            }
            ft.draw_key = flat_key;
            fcells[std::make_tuple((int)(4 * c3l::kVistaTier), std::get<0>(gk), 0,
                                   std::get<1>(gk),
                                   (int)(c3l::CHUNK_KIND_VISTA_FLAT - c3l::CHUNK_KIND_TFRAG))]
                .push_back((u32)cv.tris.size());
            cv.tris.push_back(ft);
            vista_flat_tris++;
          }
        }
      }
    }
    make_plans(vcells);
    make_plans(fcells);  // (AI-assisted) after the textured vista: the end of the chunk list
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
  std::vector<c3l::RunLod> out_run_lods;  // (AI-assisted) v17: one per out_runs
  std::vector<u16> out_run_protos;  // (AI-assisted, j2vis) v20: one per out_runs (SrcTri::proto)
  size_t lod_runs = 0, lod_chunks = 0;
  std::vector<u32> chunk_first_run;
  std::vector<u8> chunk_stream_cls;  // (AI-assisted, streaming) v19: per chunk, 0 or its class
  size_t vis_tris = 0;
  VcacheStats vc_before, vc_after;
  // (AI-assisted) v18 (Options::vis_vertex_groups): the chunk's vertices by visibility id of its
  // runs: each id's vertices one range of their own, the ids in order of their first run, first
  // use inside; a vertex of several ids copied for each but the first. Kept as it was when the
  // copies would pass 65536 vertices.
  size_t vg_chunks = 0, vg_copies = 0, vg_skipped = 0;
  auto group_vertices_by_vis = [&](c3l::Chunk& ch, const std::vector<c3l::VisRun>& runs) {
    const u32 nv = ch.vertex_count;
    // the index position of each run, in run order (runs are in draw order)
    std::vector<u32> run_idx(runs.size());
    {
      u32 ri = 0;
      for (u32 k = 0; k < ch.draw_count; k++) {
        u32 idx = out_draws[ch.first_draw + k].first_index;
        for (; ri < runs.size() && runs[ri].draw == k; ri++) {
          run_idx[ri] = idx;
          idx += runs[ri].index_count;
        }
      }
      if (ri != runs.size()) {
        vg_skipped++;
        return;
      }
    }
    // ids in order of their first run
    std::vector<u16> ids;
    std::unordered_map<u16, u32> id_slot;
    for (const auto& r : runs) {
      if (id_slot.emplace(r.vis, (u32)ids.size()).second) {
        ids.push_back(r.vis);
      }
    }
    if (ids.size() < 2) {
      return;
    }
    // owner id of each vertex (its first id); copies for the others
    std::vector<u32> owner(nv, UINT32_MAX);
    std::map<std::pair<u32, u32>, u32> copy;  // (vertex, id slot) -> new vertex
    std::vector<c3l::Vertex> extra;
    std::vector<u32> extra_slot;
    for (size_t r = 0; r < runs.size(); r++) {
      const u32 slot = id_slot[runs[r].vis];
      for (u32 i = run_idx[r]; i < run_idx[r] + runs[r].index_count; i++) {
        const u32 v = out_indices[i];
        if (v >= nv) {
          continue;  // (a copy made for this id already)
        }
        if (owner[v] == UINT32_MAX) {
          owner[v] = slot;
        }
      }
    }
    for (size_t r = 0; r < runs.size(); r++) {
      const u32 slot = id_slot[runs[r].vis];
      for (u32 i = run_idx[r]; i < run_idx[r] + runs[r].index_count; i++) {
        const u32 v = out_indices[i];
        if (owner[v] == slot) {
          continue;
        }
        auto [it, fresh] = copy.emplace(std::make_pair(v, slot), nv + (u32)extra.size());
        if (fresh) {
          extra.push_back(out_verts[ch.first_vertex + v]);
          extra_slot.push_back(slot);
        }
      }
    }
    if (nv + extra.size() > 65536) {
      vg_skipped++;
      return;
    }
    // the indices onto the copies
    for (size_t r = 0; r < runs.size(); r++) {
      const u32 slot = id_slot[runs[r].vis];
      for (u32 i = run_idx[r]; i < run_idx[r] + runs[r].index_count; i++) {
        const u32 v = out_indices[i];
        if (owner[v] != slot) {
          out_indices[i] = (u16)copy.at({v, slot});
        }
      }
    }
    // the new order: per id slot, its vertices in first use order
    const u32 total = nv + (u32)extra.size();
    std::vector<u32> remap(total, UINT32_MAX);
    u32 next = 0;
    for (u32 slot = 0; slot < ids.size(); slot++) {
      for (size_t r = 0; r < runs.size(); r++) {
        if (id_slot[runs[r].vis] != slot) {
          continue;
        }
        for (u32 i = run_idx[r]; i < run_idx[r] + runs[r].index_count; i++) {
          const u32 v = out_indices[i];
          if (remap[v] == UINT32_MAX) {
            remap[v] = next++;
          }
        }
      }
    }
    for (u32 v = 0; v < total; v++) {
      if (remap[v] == UINT32_MAX) {
        remap[v] = next++;  // (unused vertices: never)
      }
    }
    std::vector<c3l::Vertex> moved(total);
    for (u32 v = 0; v < total; v++) {
      moved[remap[v]] = v < nv ? out_verts[ch.first_vertex + v] : extra[v - nv];
    }
    out_verts.resize(ch.first_vertex);
    out_verts.insert(out_verts.end(), moved.begin(), moved.end());
    const u32 i0 = out_draws[ch.first_draw].first_index;
    const u32 i1 = out_draws[ch.first_draw + ch.draw_count - 1].first_index +
                   out_draws[ch.first_draw + ch.draw_count - 1].index_count;
    for (u32 i = i0; i < i1; i++) {
      out_indices[i] = (u16)remap[out_indices[i]];
    }
    ch.vertex_count = total;
    vg_chunks++;
    vg_copies += extra.size();
  };
  size_t vc_kept_pairs = 0;  // (AI-assisted) coplanar overlaps kept in order by vcache_order
  size_t welded_verts = 0;
  // (AI-assisted, j2water2) c3l v21: the envmapped tie base draws (c3l::EnvTieDraw), and per chunk
  // with any the normal of each vertex by its bytes (no two vertices of such a chunk have the same
  // bytes and different normals: the later one's s moved by 1/1024 until it differs), so welding,
  // the vertex orders and copies after this keep them (looked up again at the end)
  std::vector<c3l::EnvTieDraw> out_env_draws;
  std::map<u32, std::unordered_map<std::string, std::array<s8, 3>>> env_chunk_normals;
  size_t env_st_moved = 0;

  for (const auto& cp : plans) {
    {
      const auto& key = cp.key;
      const auto& local = cp.local;
      const auto& local_src = cp.src;

      c3l::Chunk ch{};
      const int detail = std::get<0>(key) % 4;
      ch.lod_tier = std::get<0>(key) / 4 % 16;
      const int sclass = std::get<0>(key) / 64 % 16;  // (AI-assisted)
      // (AI-assisted, streaming) v19 near chunk: its class (core / extra), need class
      const int stream_cls = std::get<0>(key) / 1024 % 3;
      const int need_cls = std::get<0>(key) / 1024 / 3;
      const float cell = ch.lod_tier == c3l::kVistaTier ? opt.vista_cell * 4096.f
                         : stream_cls ? stream_class_cell(opt, need_cls) * 4096.f
                                      : cell_of_detail(detail);
      ch.lod_center[0] = (std::get<1>(key) + 0.5f) * cell;
      ch.lod_center[1] = (std::get<2>(key) + 0.5f) * cell;
      ch.lod_center[2] = (std::get<3>(key) + 0.5f) * cell;
      ch.max_dist = detail == 1   ? opt.detail_dist * 4096.f
                    : detail == 2 ? opt.medium_dist * 4096.f
                    : detail == 3 ? (sclass ? kShrubClassMeters[sclass] : opt.shrub_dist) * 4096.f
                                  : 0.f;
      // (AI-assisted) Draw::kind 0 tfrag, 1 tie, 2 shrub
      ch.kind = (u32)(std::get<4>(key) & ~kEnvCellBit) + c3l::CHUNK_KIND_TFRAG;
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
      std::unordered_map<std::string, std::array<s8, 3>>* env_normals = nullptr;
      for (u32 ti : cp.tris) {
        if (cv.draw_keys[cv.tris[ti].draw_key].env != 0xffff) {
          env_normals = &env_chunk_normals[(u32)chunks.size()];
          break;
        }
      }
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
        if (v.orig != UINT32_MAX) {
          // (AI-assisted) an atlas copy: the atlas texture coordinates (c3l::kDrawAtlasUV)
          o.st[0] = v.ast[0];
          o.st[1] = v.ast[1];
        }
        memcpy(o.rgba, v.rgba, 4);
        o.pad = (s16)cv.tod_pad(v.tod);  // (AI-assisted) v16: the time of day color
        if (env_normals) {
          // (AI-assisted, j2water2) v21
          const std::array<s8, 3> n = {v.nrm[0], v.nrm[1], v.nrm[2]};
          for (int tries = 0; tries < 64; tries++) {
            auto [it, fresh] =
                env_normals->emplace(std::string((const char*)&o, sizeof(o)), n);
            if (fresh || it->second == n || !v.has_nrm) {
              break;
            }
            if (!it->second[0] && !it->second[1] && !it->second[2]) {
              it->second = n;  // a vertex without a normal came first
              break;
            }
            o.st[0]++;
            env_st_moved++;
          }
        }
        out_verts.push_back(o);
      }
      ch.first_draw = out_draws.size();
      u32 cur_key = UINT32_MAX;
      std::vector<c3l::VisRun> runs;
      std::vector<c3l::RunLod> run_lods;  // (AI-assisted) v17
      std::vector<u16> run_protos;        // (AI-assisted, j2vis) v20
      bool any_vis = false, any_lod = false, any_proto = false;
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
          if (dk.env != 0xffff) {
            // (AI-assisted, j2water2) v21
            const auto& e = cv.env_ties.at(dk.env);
            c3l::EnvTieDraw ed{};
            ed.draw = (u32)out_draws.size() - 1;
            ed.texture = e.texture;
            ed.mode = e.mode;
            memcpy(ed.tint, e.tint, 4);
            out_env_draws.push_back(ed);
          }
        }
        for (int k = 0; k < 3; k++) {
          out_indices.push_back(local.at(t.v[k]));
        }
        out_draws.back().index_count += 3;
        // (AI-assisted) v17: a run per LOD group version too (run_lods: c3l::RunLod)
        const u16 lg = t.lod ? (u16)t.lod_group : (u16)0xffff;
        // (AI-assisted, j2vis) v20: and per toggled prototype
        if (new_draw || runs.back().vis != t.vis || run_lods.back().group != lg ||
            run_lods.back().lod != t.lod || run_protos.back() != t.proto) {
          runs.push_back(c3l::VisRun{(u16)(out_draws.size() - 1 - ch.first_draw), t.vis, 0});
          run_lods.push_back(c3l::RunLod{lg, t.lod, 0});
          run_protos.push_back(t.proto);
        }
        any_proto |= t.proto != 0xffff;
        runs.back().index_count += 3;
        if (t.vis != 0xffff) {
          any_vis = true;
          vis_tris++;
        }
        any_lod |= t.lod != c3l::kRunLodAlways;
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
            if (any_order && opt.vcache && opt.vcache_fifo == 0) {
              std::vector<unsigned int> in(&out_indices[idx], &out_indices[idx] + n), o(n);
              meshopt_optimizeVertexCache(o.data(), in.data(), n, nv);
              std::copy(o.begin(), o.end(), &out_indices[idx]);
            } else if (any_order && opt.vcache && n > 3) {
              // (AI-assisted) v15: for a FIFO cache; triangles that lie on top of each other in one
              // plane keep their order (with the depth test's GEQUAL the later one wins)
              std::vector<u32> in(&out_indices[idx], &out_indices[idx] + n);
              std::vector<c3l_opt::Tri> wt(n / 3);
              for (u32 t = 0; t < n / 3; t++) {
                for (int k = 0; k < 3; k++) {
                  const auto& v = out_verts[ch.first_vertex + in[3 * t + k]];
                  for (int c = 0; c < 3; c++) {
                    wt[t][k][c] = ch.origin[c] + v.pos[c] * ch.scale;
                  }
                }
              }
              const auto pairs = c3l_opt::coplanar_pairs(wt, {}, c3l_opt::CoplanarOptions{});
              std::vector<std::vector<u32>> before;
              if (!pairs.empty()) {
                before.resize(n / 3);
                for (auto [a, b] : pairs) {
                  before[b].push_back(a);
                }
                vc_kept_pairs += pairs.size();
              }
              std::vector<u32> best;
              if (opt.vcache_fifo > 0) {
                const auto order = c3l_opt::vcache_order(in, nv, opt.vcache_fifo, before);
                best.resize(n);
                for (u32 t = 0; t < n / 3; t++) {
                  for (int k = 0; k < 3; k++) {
                    best[3 * t + k] = in[3 * order[t] + k];
                  }
                }
              } else {
                auto cost = [](const std::vector<u32>& v) {
                  return c3l_opt::fifo_misses(v.data(), v.size(), 4) +
                         c3l_opt::fifo_misses(v.data(), v.size(), 8) +
                         c3l_opt::fifo_misses(v.data(), v.size(), 16);
                };
                u64 best_cost = UINT64_MAX;
                for (int fifo : {4, 12}) {
                  const auto order = c3l_opt::vcache_order(in, nv, fifo, before);
                  std::vector<u32> o(n);
                  for (u32 t = 0; t < n / 3; t++) {
                    for (int k = 0; k < 3; k++) {
                      o[3 * t + k] = in[3 * order[t] + k];
                    }
                  }
                  const u64 c = cost(o);
                  if (c < best_cost) {
                    best_cost = c;
                    best = std::move(o);
                  }
                }
                if (pairs.empty()) {
                  std::vector<u32> o(n);
                  meshopt_optimizeVertexCache(o.data(), in.data(), n, nv);
                  const u64 c = cost(o);
                  if (c < best_cost) {
                    best_cost = c;
                    best = std::move(o);
                  }
                }
              }
              for (u32 i = 0; i < n; i++) {
                out_indices[idx + i] = (u16)best[i];
              }
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
        // (AI-assisted) v18: the vertices of each visibility id together (ids in order of their
        // first run, first use inside), a vertex used by several ids copied for each: the runtime
        // rewrites the time of day colors of the drawn runs' ids only (CtrLevel.cpp tod_recolor)
        if (opt.vis_vertex_groups && runs.size() > 1) {
          group_vertices_by_vis(ch, runs);
        }
      }
      // the runs only if some can be hidden (the offset is made absolute at write); (AI-assisted)
      // v17: or there are levels of detail
      // (AI-assisted, j2vis) v20: or a prototype the game hides (a shrub chunk: runs per draw and
      // prototype, vis 0xffff)
      const bool keep_runs = any_vis || any_lod || any_proto;
      chunk_first_run.push_back(keep_runs ? (u32)out_runs.size() : 0);
      ch.vis_run_count = keep_runs ? (u32)runs.size() : 0;
      if (keep_runs) {
        out_runs.insert(out_runs.end(), runs.begin(), runs.end());
        out_run_lods.insert(out_run_lods.end(), run_lods.begin(), run_lods.end());
        out_run_protos.insert(out_run_protos.end(), run_protos.begin(), run_protos.end());
        lod_runs += any_lod ? runs.size() : 0;
        lod_chunks += any_lod;
      }
      chunks.push_back(ch);
      chunk_stream_cls.push_back((u8)stream_cls);
    }
  }
  // (AI-assisted, streaming) v19: what the runtime worked out from a chunk's vertices at load and
  // can't for a near chunk that isn't there yet: each draw's kDrawAlpha80 (fog_alpha: every vertex
  // the draw uses has alpha 0x80, as CtrLevels::load_file), each run's bounding sphere (as
  // make_run_spheres), and the near chunk table (c3l::StreamChunk)
  std::vector<c3l::RunSphere> run_spheres;
  std::vector<c3l::StreamChunk> stream_chunks;
  struct StreamStats {
    size_t chunks[3] = {0, 0, 0};
    u64 bytes[3] = {0, 0, 0};  // vertex + index data
    u64 tris[3] = {0, 0, 0};
    u64 other_bytes = 0;  // every other chunk's
  } sst;
  if (opt.stream) {
    run_spheres.assign(out_runs.size(), c3l::RunSphere{{0, 0, 0}, 0xffff});
    for (size_t ci = 0; ci < chunks.size(); ci++) {
      const auto& ch = chunks[ci];
      bool all = true;
      for (u32 v = 0; v < ch.vertex_count && all; v++) {
        all = out_verts[ch.first_vertex + v].rgba[3] == 0x80;
      }
      u32 i_lo = UINT32_MAX, i_hi = 0;
      for (u32 d = ch.first_draw; d < ch.first_draw + ch.draw_count; d++) {
        auto& dr = out_draws[d];
        i_lo = std::min(i_lo, dr.first_index);
        i_hi = std::max(i_hi, dr.first_index + dr.index_count);
        bool ok = all;
        if (!ok) {
          ok = true;
          for (u32 i = dr.first_index; i < dr.first_index + dr.index_count && ok; i++) {
            const u16 ix = out_indices[i];
            ok = ix < ch.vertex_count && out_verts[ch.first_vertex + ix].rgba[3] == 0x80;
          }
        }
        if (ok) {
          dr.kind |= c3l::kDrawAlpha80;
        }
      }
      const u64 bytes = (u64)ch.vertex_count * sizeof(c3l::Vertex) +
                        (i_hi > i_lo ? (u64)(i_hi - i_lo) * 2 : 0);
      const u8 cls = chunk_stream_cls[ci];
      if (cls) {
        sst.chunks[cls]++;
        sst.bytes[cls] += bytes;
        sst.tris[cls] += i_hi > i_lo ? (i_hi - i_lo) / 3 : 0;
      } else {
        sst.other_bytes += bytes;
      }
      if (!ch.vis_run_count) {
        continue;
      }
      // the runs' spheres (runs are in draw order, each draw's from its first index on)
      const u32 r0 = chunk_first_run[ci];
      u32 idx = 0, last_draw = UINT32_MAX;
      float need_a = 0, need_e = 0;
      for (u32 r = r0; r < r0 + ch.vis_run_count; r++) {
        const auto& run = out_runs[r];
        if (run.draw != last_draw) {
          last_draw = run.draw;
          idx = out_draws[ch.first_draw + run.draw].first_index;
        }
        const u32 n = run.index_count;
        int mn[3] = {32767, 32767, 32767}, mx[3] = {-32768, -32768, -32768};
        bool ok = n > 0 && idx >= i_lo && idx + n <= i_hi;
        for (u32 i = idx; ok && i < idx + n; i++) {
          const u16 v = out_indices[i];
          ok = v < ch.vertex_count;
          for (int c = 0; ok && c < 3; c++) {
            mn[c] = std::min<int>(mn[c], out_verts[ch.first_vertex + v].pos[c]);
            mx[c] = std::max<int>(mx[c], out_verts[ch.first_vertex + v].pos[c]);
          }
        }
        idx += n;
        if (ok) {
          auto& sp = run_spheres[r];
          float d2 = 0;
          for (int c = 0; c < 3; c++) {
            sp.c[c] = (s16)((mn[c] + mx[c]) >> 1);
            const float h = (float)std::max(sp.c[c] - mn[c], mx[c] - sp.c[c]);
            d2 += h * h;
          }
          sp.r = (u16)std::min(65534.f, std::ceil(std::sqrt(d2)) + 1.f);
        }
        // a near chunk's groups: where it is needed
        const auto& rl = out_run_lods[r];
        if (cls && rl.lod == c3l::kRunLodNear && rl.group < cv.lod_groups.size()) {
          const auto& g = cv.lod_groups[rl.group];
          float dd = 0;
          for (int c = 0; c < 3; c++) {
            dd += (g.bsphere[c] - ch.bsphere[c]) * (g.bsphere[c] - ch.bsphere[c]);
          }
          need_a = std::max(need_a, std::sqrt(dd) + g.bsphere[3]);
          need_e = std::max(need_e, rl.group < cv.lod_group_error.size()
                                        ? cv.lod_group_error[rl.group]
                                        : g.dist);
        }
      }
      if (cls) {
        stream_chunks.push_back(c3l::StreamChunk{(u32)ci, cls, 0, need_a, need_e});
      }
    }
  }
  // (AI-assisted, streaming) the near chunk data needed at once (the runtime's rule: a group's
  // near version within error x kNeedK + its radius of its center; kNeedK: gameplay focal 256 /
  // tie_lod_px 4 x 1.1), the camera at each group's center (a rough sample of where it goes)
  if (opt.stream && !stream_chunks.empty()) {
    constexpr float kNeedK = kStreamNeedK;
    struct UG {
      u64 bytes;
      std::vector<u32> groups;
    };
    std::vector<UG> ug;
    for (const auto& sc : stream_chunks) {
      const auto& ch = chunks[sc.chunk];
      UG u;
      u32 lo = UINT32_MAX, hi = 0;
      for (u32 d = ch.first_draw; d < ch.first_draw + ch.draw_count; d++) {
        lo = std::min(lo, out_draws[d].first_index);
        hi = std::max(hi, out_draws[d].first_index + out_draws[d].index_count);
      }
      u.bytes = (u64)ch.vertex_count * 16 + (hi > lo ? (u64)(hi - lo) * 2 : 0);
      for (u32 r = chunk_first_run[sc.chunk]; r < chunk_first_run[sc.chunk] + ch.vis_run_count; r++) {
        if (out_run_lods[r].lod == c3l::kRunLodNear) {
          u.groups.push_back(out_run_lods[r].group);
        }
      }
      std::sort(u.groups.begin(), u.groups.end());
      u.groups.erase(std::unique(u.groups.begin(), u.groups.end()), u.groups.end());
      ug.push_back(std::move(u));
    }
    std::vector<float> lim(cv.lod_groups.size());
    for (size_t g = 0; g < cv.lod_groups.size(); g++) {
      lim[g] = (g < cv.lod_group_error.size() ? cv.lod_group_error[g] : cv.lod_groups[g].dist) *
                   kNeedK +
               cv.lod_groups[g].bsphere[3];
    }
    u64 mx = 0;
    double sum = 0;
    const size_t step = std::max<size_t>(1, cv.lod_groups.size() / 2000);
    size_t samples = 0;
    for (size_t sgi = 0; sgi < cv.lod_groups.size(); sgi += step) {
      const float* cam = cv.lod_groups[sgi].bsphere;
      u64 need = 0;
      for (const auto& u : ug) {
        for (u32 g : u.groups) {
          const float* c = cv.lod_groups[g].bsphere;
          const float dx = c[0] - cam[0], dy = c[1] - cam[1], dz = c[2] - cam[2];
          if (dx * dx + dy * dy + dz * dz < lim[g] * lim[g]) {
            need += u.bytes;
            break;
          }
        }
      }
      mx = std::max(mx, need);
      sum += need;
      samples++;
    }
    lg::info("{}: streaming: near chunk data needed at once, the camera at the groups' centers: "
             "{:.0f} KB on average, at most {} KB",
             level.level_name, sum / std::max<size_t>(1, samples) / 1024, mx / 1024);
  }
  if (qs.shared_mismatch) {
    lg::error("{}: {} positions in several chunks reconstruct differently", in.stem().string(),
              qs.shared_mismatch);
    return false;
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

  // (AI-assisted) v15: textures that went into atlases and nothing draws any more (the vista tier
  // and merc models keep theirs): no texels (the runtime makes no texture of them)
  size_t atlas_dropped = 0;
  u64 atlas_dropped_bytes = 0;
  if (!dstats.atlased.empty()) {
    std::vector<u8> drawn(tex_descs.size(), 0);
    for (const auto& d : out_draws) {
      if (d.texture < drawn.size()) {
        drawn[d.texture] = 1;
      }
    }
    for (const auto& d : merc.draws) {
      if (d.texture < drawn.size()) {
        drawn[d.texture] = 1;
      }
    }
    for (u16 t : dstats.atlased) {
      if (!drawn[t]) {
        atlas_dropped++;
        atlas_dropped_bytes += tex_datas[t].size();
        tex_datas[t].clear();
      }
    }
  }
  // ---- write ----
  std::vector<u8> buf(sizeof(c3l::Header));
  c3l::Header hdr{};
  memcpy(hdr.magic, c3l::kMagic, 4);
  // (AI-assisted) --shrub-one-dist: v13 (the shrub chunks don't carry PS2 distances)
  // (AI-assisted, streaming) --no-stream: v18
  hdr.version = !opt.shrub_ps2_dist ? c3l::kShrubPs2DistVersion - 1
                : opt.stream        ? c3l::kVersion
                                    : c3l::kStreamVersion - 1;
  strncpy(hdr.level_name, level.level_name.c_str(), sizeof(hdr.level_name) - 1);
  hdr.num_chunks = chunks.size();
  // (AI-assisted) the visibility runs first: the chunks point at them
  u32 runs_offset = 0;
  {
    runs_offset = out_runs.empty() ? 0 : append(buf, out_runs.data(), out_runs.size());
    for (size_t i = 0; i < chunks.size(); i++) {
      chunks[i].vis_runs_offset =
          chunks[i].vis_run_count ? runs_offset + chunk_first_run[i] * (u32)sizeof(c3l::VisRun)
                                  : 0;
    }
  }
  hdr.chunks_offset = append(buf, chunks.data(), chunks.size());
  // (AI-assisted) v16: the time of day table
  if (!cv.tod_colors.empty() && cv.tod_colors.size() + cv.tod_muls.size() < 0xffff) {
    hdr.tod_color_count = cv.tod_colors.size();
    hdr.tod_mul_count = cv.tod_muls.size();
    hdr.tod_offset = append(buf, cv.tod_colors.data(), cv.tod_colors.size());
    buf.insert(buf.end(), (const u8*)cv.tod_muls.data(),
               (const u8*)(cv.tod_muls.data() + cv.tod_muls.size()));
    align16(buf);
  }
  // (AI-assisted) v17: tie levels of detail (c3l::ExtHeader)
  // (AI-assisted, streaming) v19: always with runs (the run spheres), 48 bytes; v18: 32
  if ((!cv.lod_groups.empty() || opt.stream) && !out_runs.empty() &&
      out_run_lods.size() == out_runs.size()) {
    c3l::ExtHeader eh{};
    memcpy(eh.magic, c3l::kExtMagic, 4);
    // (AI-assisted, j2vis) v20: the prototype visibility table (c3l::ExtProtoVis) after it, when the
    // level has toggled prototypes (else v19's 48 bytes)
    std::vector<c3l::ProtoVis> pv_protos;
    std::vector<u32> pv_runs;
    if (opt.stream && opt.proto_vis && out_run_protos.size() == out_runs.size()) {
      for (size_t p = 0; p < cv.proto_vis.size(); p++) {
        c3l::ProtoVis pv{};
        strncpy(pv.name, cv.proto_vis[p].name.c_str(), sizeof(pv.name) - 1);
        pv.kind = cv.proto_vis[p].kind;
        pv.first_run = (u32)pv_runs.size();
        u32 tris = 0;
        for (u32 r = 0; r < out_run_protos.size(); r++) {
          if (out_run_protos[r] == p) {
            pv_runs.push_back(r);
            tris += out_runs[r].index_count / 3;
          }
        }
        pv.run_count = (u32)pv_runs.size() - pv.first_run;
        if (pv.run_count) {
          pv_protos.push_back(pv);
          // where it is (meters; the box of its triangles, vista copies left out)
          float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
          for (const auto& t : cv.tris) {
            if (t.proto != p || t.tier == c3l::kVistaTier) {
              continue;
            }
            for (u32 vi : t.v) {
              const float q[3] = {cv.verts[vi].x, cv.verts[vi].y, cv.verts[vi].z};
              for (int c = 0; c < 3; c++) {
                mn[c] = std::min(mn[c], q[c] / 4096.f);
                mx[c] = std::max(mx[c], q[c] / 4096.f);
              }
            }
          }
          lg::info("{}: prototype visibility (v20): {} {} in {} runs, {} triangles, box ({:.1f} "
                   "{:.1f} {:.1f}) .. ({:.1f} {:.1f} {:.1f}) m",
                   level.level_name, pv.kind == c3l::kProtoTie ? "tie" : "shrub", pv.name,
                   pv.run_count, tris, mn[0], mn[1], mn[2], mx[0], mx[1], mx[2]);
        }
      }
    }
    const u32 eh_size = !out_env_draws.empty() ? c3l::kExtHeaderV21Size  // (AI-assisted, j2water2)
                        : !pv_protos.empty()    ? c3l::kExtHeaderV20Size
                        : opt.stream            ? (u32)sizeof(eh)
                                                : c3l::kExtHeaderV17Size;
    eh.size = eh_size;
    align16(buf);
    hdr.ext_offset = (u32)buf.size();
    buf.insert(buf.end(), (const u8*)&eh, (const u8*)&eh + std::min<u32>(eh_size, sizeof(eh)));
    buf.resize(hdr.ext_offset + eh_size, 0);  // (AI-assisted, j2vis) v20: ExtProtoVis, below
    eh.lod_group_count = cv.lod_groups.size();
    eh.lod_groups_offset =
        cv.lod_groups.empty() ? 0 : append(buf, cv.lod_groups.data(), cv.lod_groups.size());
    eh.run_lod_count = out_run_lods.size();
    eh.run_lod_offset = append(buf, out_run_lods.data(), out_run_lods.size());
    eh.runs_offset = runs_offset;
    if (!cv.lod_groups.empty() && cv.lod_group_error.size() == cv.lod_groups.size()) {
      eh.lod_error_offset = append(buf, cv.lod_group_error.data(), cv.lod_group_error.size());
    }
    if (opt.stream) {
      eh.run_sphere_offset = append(buf, run_spheres.data(), run_spheres.size());
      eh.stream_count = stream_chunks.size();
      eh.stream_offset =
          stream_chunks.empty() ? 0 : append(buf, stream_chunks.data(), stream_chunks.size());
    }
    memcpy(buf.data() + hdr.ext_offset, &eh, std::min<u32>(eh_size, sizeof(eh)));
    if (!pv_protos.empty()) {
      c3l::ExtProtoVis ep{};
      ep.proto_count = (u32)pv_protos.size();
      ep.protos_offset = append(buf, pv_protos.data(), pv_protos.size());
      ep.proto_run_count = (u32)pv_runs.size();
      ep.proto_runs_offset = append(buf, pv_runs.data(), pv_runs.size());
      memcpy(buf.data() + hdr.ext_offset + sizeof(eh), &ep, sizeof(ep));
    }
    if (!out_env_draws.empty()) {
      // (AI-assisted, j2water2) v21: the envmapped tie draws, their chunks' normals
      c3l::ExtEnvTie et{};
      et.draw_count = (u32)out_env_draws.size();
      et.draws_offset = append(buf, out_env_draws.data(), out_env_draws.size());
      std::vector<c3l::EnvTieChunk> ecs;
      size_t missing = 0, nverts = 0, nv_dist = 0, nv_near = 0;
      for (const auto& [ci, nm] : env_chunk_normals) {
        const auto& ch = chunks.at(ci);
        nv_dist += ch.max_dist > 0 ? ch.vertex_count : 0;
        nv_near += chunk_stream_cls[ci] ? ch.vertex_count : 0;
        std::vector<std::array<s8, 4>> n(ch.vertex_count, {0, 0, 0, 0});
        for (u32 v = 0; v < ch.vertex_count; v++) {
          const auto& o = out_verts[ch.first_vertex + v];
          auto it = nm.find(std::string((const char*)&o, sizeof(o)));
          if (it == nm.end()) {
            missing++;
            continue;
          }
          n[v] = {it->second[0], it->second[1], it->second[2], 0};
        }
        nverts += n.size();
        ecs.push_back(c3l::EnvTieChunk{ci, append(buf, n.data(), n.size())});
      }
      et.chunk_count = (u32)ecs.size();
      et.chunks_offset = append(buf, ecs.data(), ecs.size());
      memcpy(buf.data() + hdr.ext_offset + c3l::kExtHeaderV20Size, &et, sizeof(et));
      lg::info("{}: envmapped ties (v21): {} draws ({} shine kinds), {} tris, {} chunks with "
               "{} normals ({} KB; {} KB in distance-limited chunks, {} KB in near chunks), {} "
               "groups without a shine, {} s moved, {} vertices unmatched",
               level.level_name, out_env_draws.size(), cv.env_ties.size(), cv.env_tie_tris,
               ecs.size(), nverts, nverts * 4 / 1024, nv_dist * 4 / 1024, nv_near * 4 / 1024,
               cv.env_tie_groups_missing, env_st_moved, missing);
    }
  }
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
    if (hdr.tod_offset) {
      starts.push_back({hdr.tod_offset, "time of day"});  // (AI-assisted) v16
    }
    if (hdr.ext_offset) {
      starts.push_back({hdr.ext_offset, "tie lod"});  // (AI-assisted) v17
    }
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
  if (lstats.groups) {
    // (AI-assisted) v17 tie levels of detail
    lg::info("{}: tie levels of detail: {} of {} instance groups ({} candidates, {} without a PS2 "
             "distance) have a near version (geo {}): {} near + {} far tris (all groups: {} near, {} "
             "far), estimated +{:.0f} KB (budget {}; all candidates {:.0f} KB); {} LOD runs "
             "in {} chunks; {} near / {} far tris added; {:.0f} ms",
             level.level_name, lstats.selected, lstats.groups, lstats.candidates, lstats.no_dist,
             opt.tie_near_geo, lstats.sel_near_tris, lstats.sel_far_tris, lstats.near_tris,
             lstats.far_tris, lstats.cost_kb,
             lstats.budget_kb >= 1e11 ? std::string("all groups") : fmt::format("{:.0f} KB", lstats.budget_kb),
             lstats.cost_all_kb, lod_runs,
             lod_chunks, cv.lod_tris[0], cv.lod_tris[1], lstats.ms);
    std::vector<std::pair<std::string, TieLodStats::Proto>> left(lstats.protos.begin(),
                                                                 lstats.protos.end());
    std::sort(left.begin(), left.end(),
              [](const auto& a, const auto& b) { return a.second.left_tris > b.second.left_tris; });
    std::string l;
    for (size_t i = 0; i < left.size() && i < 12 && left[i].second.left_tris; i++) {
      l += fmt::format("{}{} ({} of {} groups coarse, {} tris, error up to {:.2f} m)",
                       i ? ", " : "", left[i].first,
                       left[i].second.candidates - left[i].second.selected,
                       left[i].second.candidates, left[i].second.left_tris,
                       left[i].second.worst_left);
    }
    lg::info("{}: tie levels of detail: {} (over budget): {}", level.level_name,
             opt.stream ? "streamed only, not in the budget's core set" : "left coarse",
             l.empty() ? "none" : l);
  }
  if (opt.stream) {
    // (AI-assisted, streaming) v19 near chunks
    std::string cls;
    for (size_t c = 0; c <= opt.stream_classes.size(); c++) {
      cls += fmt::format("{}{}{:g} m", c ? ", " : "",
                         c < opt.stream_classes.size() ? fmt::format("< {:g} m: ", opt.stream_classes[c])
                                                       : std::string("beyond: "),
                         stream_class_cell(opt, (int)c));
    }
    lg::info("{}: streaming (v19): {} near chunks (cells by need distance: {}): core {} ({} KB, {} "
             "tris), extra {} ({} KB, {} tris); {} groups in the budget's core set of {}; other "
             "chunks {} KB",
             level.level_name, sst.chunks[1] + sst.chunks[2], cls, sst.chunks[1],
             sst.bytes[1] / 1024, sst.tris[1], sst.chunks[2], sst.bytes[2] / 1024, sst.tris[2],
             lstats.core, lstats.selected, sst.other_bytes / 1024);
  }
  if (cv.shrub_tris && opt.shrub_ps2_dist) {
    // (AI-assisted) shrub triangles by distance class
    std::string cl;
    for (int c = 0; c < kShrubClasses; c++) {
      if (cv.shrub_class_tris[c]) {
        cl += fmt::format(" {}: {}", c ? fmt::format("{} m", kShrubClassMeters[c])
                                       : fmt::format("{} m (no PS2 distance)", opt.shrub_dist),
                          cv.shrub_class_tris[c]);
      }
    }
    lg::info("{}: shrub triangles by PS2 distance class:{}", level.level_name, cl);
  }
  if (merc.cull_draws) {
    // (AI-assisted) merc back face culling marks (mark_merc_cull)
    lg::info("{}: merc culling: {} of {} draws marked ({} of {} triangles, {:.0f}%; {} turned to "
             "face outward); not marked: {} blended / cut-out, {} with open edges (sheets), {} "
             "unclear normals",
             level.level_name, merc.cull_marked_draws, merc.cull_draws, merc.cull_marked_tris,
             merc.cull_tris, 100.0 * merc.cull_marked_tris / std::max<size_t>(merc.cull_tris, 1),
             merc.cull_flipped, merc.cull_no_mode, merc.cull_no_surface, merc.cull_no_normals);
  }
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
  if (!cv.slot_tex_map.empty()) {
    lg::info("{}: {} animated slot textures, {} of them first drawn by a plain draw of the base "
             "texture",
             level.level_name, cv.slot_tex_map.size(), cv.slot_tex_first_plain);
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
  lg::info("{}: post-vertex cache: vertices shaded per triangle (meshoptimizer's model 8 / 16) "
           "{:.3f} / {:.3f}{}; FIFO 4 / 8 / 16 / 32: {}{}",
           level.level_name, vc_before.shaded8 / std::max<u64>(vc_before.tris, 1),
           vc_before.shaded16 / std::max<u64>(vc_before.tris, 1),
           opt.vcache ? fmt::format(" -> {:.3f} / {:.3f}",
                                    vc_after.shaded8 / std::max<u64>(vc_after.tris, 1),
                                    vc_after.shaded16 / std::max<u64>(vc_after.tris, 1))
                      : "",
           vc_before.fifo_line(),
           opt.vcache ? fmt::format(" -> {} ({}; {} coplanar overlaps kept in order)",
                                    vc_after.fifo_line(),
                                    opt.vcache_fifo > 0
                                        ? fmt::format("FIFO {} order", opt.vcache_fifo)
                                    : opt.vcache_fifo < 0 ? std::string("best of FIFO 4 / 12 / meshoptimizer orders")
                                        : std::string("meshoptimizer"),
                                    vc_kept_pairs)
                      : "");
  // (AI-assisted) v16
  {
    const size_t n = cv.tod_colors.size() + cv.tod_muls.size();
    lg::info("{}: time of day: {} colors, {} multiplied ({} KB){}", level.level_name,
             cv.tod_colors.size(), cv.tod_muls.size(),
             (cv.tod_colors.size() * sizeof(c3l::TodColor) + cv.tod_muls.size() * sizeof(c3l::TodMul)) /
                 1024,
             n >= 0xffff ? " TOO MANY: left out (the baked palette only)" : "");
  }
  if (opt.vcache) {
    lg::info("{}: {} duplicate vertices merged", level.level_name, welded_verts);
    lg::info("{}: vertices by visibility id (v18): {} chunks, {} vertices copied, {} chunks kept "
             "as they were",
             level.level_name, vg_chunks, vg_copies, vg_skipped);
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
    if (opt.vista_flat) {
      // (AI-assisted)
      size_t fchunks = 0, fdraws = 0;
      for (const auto& ch : chunks) {
        if (ch.kind == c3l::CHUNK_KIND_VISTA_FLAT) {
          fchunks++;
          fdraws += ch.draw_count;
        }
      }
      lg::info("{}: untextured vista tier: {} triangles, {} chunks, {} draws", level.level_name,
               vista_flat_tris, fchunks, fdraws);
    }
  }
  if (opt.tie_winding) {
    lg::info("{}: tie winding: {} of {} cullable tie triangles on closed surfaces, marked for back "
             "face culling ({} turned to face outward)",
             level.level_name, cv.orient_culled, cv.orient_tris, cv.orient_flipped);
  }
  if (!opt.merc_only) {
    // (AI-assisted) v15 draw optimisations
    lg::info("{}: opaque alpha: {} draws ({} triangles) lose their alpha test, {} blended ones ({} "
             "triangles) their blending (texture alpha 1.0 in every level, vertex alpha >= 0x80; "
             "of {} alpha tested, {} blended such draws: {} kept for coplanar overlaps with other "
             "draws of their texture); {} draws merged into others",
             level.level_name, dstats.keys_at, dstats.tris_at, dstats.keys_blend, dstats.tris_blend,
             dstats.at_candidates, dstats.blend_candidates, dstats.blocked_coplanar,
             dstats.merged_keys);
    lg::info("{}: atlases: {} of {} textures that fit ({} left out: coplanar overlaps with other "
             "draws; {} also drawn by the vista tier or merc; {} too costly) in {} atlases ({} KB), {} "
             "triangles, {} vertex copies; {} textures no longer "
             "drawn ({} KB); gutters vs the textures' own edges: RMS {:.2f}, max {} (of {} texels), "
             "cells exact: {}; coplanar overlaps of different draws: {} (of {})",
             level.level_name, dstats.atlas_cells, dstats.atlas_textures_fit,
             dstats.atlas_blocked_coplanar, dstats.atlas_elsewhere, dstats.atlas_costly,
             dstats.atlases, dstats.atlas_bytes / 1024,
             dstats.atlas_tris, dstats.atlas_verts, atlas_dropped, atlas_dropped_bytes / 1024,
             dstats.ast.gutter_texels
                 ? std::sqrt(dstats.ast.gutter_sq / (4.0 * dstats.ast.gutter_texels))
                 : 0.0,
             dstats.ast.gutter_max, dstats.ast.gutter_texels, dstats.ast.cells_exact ? "yes" : "NO",
             dstats.cross_key_pairs, dstats.coplanar_pairs);
  }
  lg::info("{}: visibility: {} of {} tris in {} runs (hidden when the game's visibility data says)",
           level.level_name, vis_tris, out_indices.size() / 3, out_runs.size());
  lg::debug("tie tris by instance radius (<2m, <4, <8, ... >=128m): {} {} {} {} {} {} {} {}",
            cv.radius_hist[0], cv.radius_hist[1], cv.radius_hist[2], cv.radius_hist[3],
            cv.radius_hist[4], cv.radius_hist[5], cv.radius_hist[6], cv.radius_hist[7]);
  return true;
}
}  // namespace

/*!
 * (AI-assisted) Jak II: texanim.c3t (see CtrTexAnimJak2Defs.h), the source and destination
 * textures of the texture animations, from GAME.fr3.
 */
bool write_texanim(const fs::path& in, const fs::path& out) {
  using namespace ctr_texanim_jak2;
  auto data = file_util::read_binary_file(in);
  auto decomp = compression::decompress_zstd(data.data(), data.size());
  Serializer ser(decomp.data(), decomp.size());
  tfrag3::Level level;
  level.serialize(ser);

  std::vector<c3t::Entry> entries;
  std::vector<u8> blob;
  std::set<std::pair<std::string, std::string>> done;
  auto set_name = [](char* dst, size_t n, const std::string& s) {
    memset(dst, 0, n);
    memcpy(dst, s.data(), std::min(s.size(), n - 1));
  };
  auto find_rgba = [&](const std::string& name) -> const tfrag3::Texture* {
    for (const auto& t : level.textures) {
      if (t.debug_name == name) {
        return &t;
      }
    }
    return nullptr;
  };
  auto add_rgba = [&](const std::string& name, bool size_only, int ow, int oh) {
    if (!done.insert({name, ""}).second) {
      return true;
    }
    const auto* t = find_rgba(name);
    if (!t) {
      lg::error("texanim: no texture {}", name);
      return false;
    }
    c3t::Entry e{};
    set_name(e.name, sizeof(e.name), name);
    int w = ow ? ow : t->w, h = oh ? oh : t->h;
    if (size_only) {
      while (w > c3t::kMaxSize || h > c3t::kMaxSize) {
        w = std::max(1, w / 2);
        h = std::max(1, h / 2);
      }
      e.w = (u16)w;
      e.h = (u16)h;
      e.kind = c3t::kSizeOnly;
      entries.push_back(e);
      return true;
    }
    // halved (box filter) until it fits
    std::vector<u32> img(t->data.begin(), t->data.end());
    w = t->w;
    h = t->h;
    while ((w > c3t::kMaxSize || h > c3t::kMaxSize) && w > 1 && h > 1) {
      const int nw = w / 2, nh = h / 2;
      std::vector<u32> o((size_t)nw * nh);
      for (int y = 0; y < nh; y++) {
        for (int x = 0; x < nw; x++) {
          u32 acc[4] = {0, 0, 0, 0};
          for (int k = 0; k < 4; k++) {
            const u32 v = img[(size_t)(2 * y + (k >> 1)) * w + 2 * x + (k & 1)];
            for (int c = 0; c < 4; c++) {
              acc[c] += (v >> (8 * c)) & 0xff;
            }
          }
          u32 r = 0;
          for (int c = 0; c < 4; c++) {
            r |= ((acc[c] + 2) / 4) << (8 * c);
          }
          o[(size_t)y * nw + x] = r;
        }
      }
      img.swap(o);
      w = nw;
      h = nh;
    }
    e.w = (u16)w;
    e.h = (u16)h;
    e.kind = c3t::kRgba;
    e.offset = blob.size();
    e.size = (u32)img.size() * 4;
    const u8* p = (const u8*)img.data();
    blob.insert(blob.end(), p, p + e.size);
    entries.push_back(e);
    return true;
  };
  auto find_index = [&](const std::string& name, const char* dgo) -> const tfrag3::IndexTexture* {
    for (const auto& t : level.index_textures) {
      if (t.name != name) {
        continue;
      }
      if (dgo && std::find(t.level_names.begin(), t.level_names.end(), std::string(dgo)) ==
                     t.level_names.end()) {
        continue;
      }
      return &t;
    }
    return nullptr;
  };
  auto add_index = [&](const std::string& name, const char* dgo) {
    const std::string d = dgo ? dgo : "";
    if (!done.insert({name, d}).second) {
      return true;
    }
    const auto* t = find_index(name, dgo);
    if (!t) {
      lg::error("texanim: no index texture {} ({})", name, d);
      return false;
    }
    c3t::Entry e{};
    set_name(e.name, sizeof(e.name), name);
    set_name(e.dgo, sizeof(e.dgo), d);
    int step = 1;
    while (t->w / step > c3t::kMaxSize || t->h / step > c3t::kMaxSize) {
      step *= 2;
    }
    const int w = std::max(1, t->w / step), h = std::max(1, t->h / step);
    e.w = (u16)w;
    e.h = (u16)h;
    e.kind = c3t::kIndex;
    e.offset = blob.size();
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        blob.push_back(t->index_data[(size_t)y * step * t->w + (size_t)x * step]);
      }
    }
    for (const auto& c : t->color_table) {
      blob.push_back(c.x());
      blob.push_back(c.y());
      blob.push_back(c.z());
      blob.push_back(c.w());
    }
    e.size = blob.size() - e.offset;
    entries.push_back(e);
    return true;
  };
  int missing = 0;
  for (const auto& arr : kArrays) {
    for (int a = 0; a < arr.anims; a++) {
      const auto& an = arr.anim[a];
      missing += !add_rgba(an.dest, true, an.w, an.h);
      for (int l = 0; l < an.layers; l++) {
        missing += !add_rgba(an.layer[l].tex, false, 0, 0);
      }
    }
  }
  for (const auto& g : kClutGroups) {
    for (int i = 0; i < g.count; i++) {
      const auto& t = g.tex[i];
      missing += !add_index(t.name, t.dgo);
      missing += !add_index(std::string(t.name) + t.suf0, t.dgo);
      missing += !add_index(std::string(t.name) + t.suf1, t.dgo);
    }
  }
  c3t::Header hdr{};
  memcpy(hdr.magic, c3t::kMagic, 4);
  hdr.version = c3t::kVersion;
  hdr.count = (u32)entries.size();
  hdr.entries_offset = sizeof(hdr);
  const u32 data_start = sizeof(hdr) + (u32)(entries.size() * sizeof(c3t::Entry));
  for (auto& e : entries) {
    if (e.kind != c3t::kSizeOnly) {
      e.offset += data_start;
    }
  }
  std::vector<u8> file(sizeof(hdr));
  memcpy(file.data(), &hdr, sizeof(hdr));
  const u8* ep = (const u8*)entries.data();
  file.insert(file.end(), ep, ep + entries.size() * sizeof(c3t::Entry));
  file.insert(file.end(), blob.begin(), blob.end());
  file_util::write_binary_file(out, file.data(), file.size());
  lg::info("texanim: {} textures, {} KB ({} missing) -> {}", entries.size(), file.size() / 1024,
           missing, out.string());
  return true;
}

int main(int argc, char** argv) {
  CLI::App app{"fr3 -> c3l (3DS level background) converter (AI-assisted)"};
  Options opt;
  std::string in, out;
  bool all = false;
  app.add_option("input", in, "input .fr3 (or folder with --all)")->required();
  app.add_option("output", out, "output .c3l (or folder with --all)")->required();
  app.add_flag("--all", all, "convert every .fr3 in the input folder");
  bool texanim = false;
  app.add_flag("--texanim", texanim,
               "(AI-assisted, Jak II) write the texture animations' textures from GAME.fr3 "
               "(input) to a .c3t file (output); --all writes <output>/texanim.c3t too");
  app.add_option("--game", opt.game,
                 "(AI-assisted) jak1 | jak2 (default: from the input path, out/jak2/fr3: jak2)");
  app.add_option("--tfrag-geo", opt.tfrag_geo, "tfrag level of detail (0 = most detailed, 2)");
  app.add_option("--tie-geo", opt.tie_geo, "tie level of detail (0 = most detailed, 3)");
  bool no_tie_lod = false;
  app.add_flag("--no-tie-lod", no_tie_lod,
               "(AI-assisted, v17) no near versions of ties: all at --tie-geo, as v16");
  app.add_option("--tie-near-geo", opt.tie_near_geo,
                 "(v17) geometry of the ties' near versions (0 = most detailed)");
  app.add_option("--tie-lod-budget-kb", opt.tie_lod_budget_kb,
                 "(v17) extra vertex and index data for near tie versions per level (KB; default: "
                 "the level's planned budget, else all groups)");
  app.add_option("--tie-lod-min-angle", opt.tie_lod_min_angle,
                 "(v17) smallest shape error worth a near version (radians, at the larger of the "
                 "group's radius and --tie-lod-view-m)");
  app.add_option("--tie-lod-view-m", opt.tie_lod_view_m,
                 "(v17) viewing distance for the shape error (meters)");
  app.add_option("--tie-lod-default-m", opt.tie_lod_default_m,
                 "(v17) near version distance of ties without a PS2 distance (wind; meters)");
  bool no_stream = false;
  app.add_flag("--no-stream", no_stream,
               "(AI-assisted, v19) no near chunks for streaming: v18 files (near versions within "
               "the level's budget, in the far versions' chunks)");
  app.add_flag("--no-env-ties", opt.no_env_ties,
               "(AI-assisted) Jak II: no envmapped tie shine (c3l v21 ExtEnvTie)");
  app.add_flag("--no-proto-vis", opt.no_proto_vis,
               "(AI-assisted, v20) Jak II: no runs of their own for the prototypes the game hides "
               "by task state (drawn always, as v19)");
  app.add_option("--stream-cell", opt.stream_cell,
                 "(AI-assisted, v19) near chunk cell size (meters)");
  app.add_option("--stream-classes", opt.stream_classes,
                 "(AI-assisted, v19) near chunks per need class: the bounds (meters) of the "
                 "distance a group's near version is needed within")
      ->delimiter(',');
  app.add_option("--stream-class-cells", opt.stream_class_cells,
                 "(AI-assisted, v19) near chunk cell size per need class (meters)")
      ->delimiter(',');
  app.add_option("--far-tfrag-geo", opt.far_tfrag_geo,
                 "tfrag level of detail for far away cells (-1: no far version)");
  app.add_option("--palette", opt.palette,
                 "time of day palette to bake (0-7; default: 0 for interiors, else 1)");
  bool no_tod = false;
  app.add_flag("--no-tod", no_tod,
               "(v16) no time of day table: the vertex colors stay the baked palette");
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
  app.add_flag("!--shrub-one-dist", opt.shrub_ps2_dist,
               "(AI-assisted) all shrubs up to --shrub-dist (v13), not each up to its PS2 distance");
  app.add_flag("--no-merc", opt.no_merc, "leave out merc models");
  app.add_flag("--merc-only", opt.merc_only, "only merc models (for the common GAME.fr3)");
  app.add_option("--keira-from", opt.keira_from,
                 "(AI-assisted) input GAME.fr3: write KEIRA.c3l, Keira's model from this "
                 "village1.fr3 for the play-as-Keira easter egg (--all makes it too)");
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
  bool no_vis_groups = false;
  app.add_flag("--no-vis-vertex-groups", no_vis_groups,
               "(AI-assisted, v18) keep the vertices in first use order (not grouped by visibility "
               "id: the runtime then rewrites a chunk's time of day colors all at once)");
  app.add_option("--vcache-fifo", opt.vcache_fifo,
                 "(AI-assisted) triangle order for a FIFO post-vertex cache of N entries, coplanar "
                 "overlaps kept in order (0: the v14 meshoptimizer order)");
  bool no_atlas = false, no_opaque_alpha = false;
  app.add_flag("--no-atlas", no_atlas, "(AI-assisted) no texture atlases (v15)");
  app.add_option("--atlas-max", opt.atlas_max,
                 "(AI-assisted) textures up to this size (texels per side) may go into atlases");
  app.add_option("--atlas-kb-per-bind", opt.atlas_kb_per_bind,
                 "(AI-assisted) make an atlas only if it adds at most this many KB per texture bind "
                 "it saves");
  app.add_flag("--no-opaque-alpha", no_opaque_alpha,
               "(AI-assisted) keep alpha tests / blending that can't change a pixel (v15)");
  bool no_verify = false;
  app.add_flag("--no-verify", no_verify, "don't read the written files back to check them");
  bool no_tie_winding = false, no_vista = false;
  bool tie_far_own_colors = false;  // (AI-assisted, lod-light)
  app.add_flag("--tie-far-own-colors", tie_far_own_colors,
               "(AI-assisted) tie far versions keep geometry 3's own vertex colors (the PS2's) instead "
               "of their near version's (the PC's: the light on a surface doesn't change at the switch)");
  app.add_flag("--no-tie-winding", no_tie_winding,
               "keep the strips' triangle order (no flipping, no back face culling marks)");
  app.add_flag("--no-vista", no_vista, "no vista tier (far scenery cells)");
  app.add_flag("--vista-flat", opt.vista_flat,
               "also an untextured vista tier (average texture colors in the vertex colors, one "
               "draw per cell and mode; config vista_untextured)");
  app.add_option("--vista-cell", opt.vista_cell, "vista tier cell size (meters)");
  app.add_option("--merc-cull-agree", opt.merc_cull_agree,
                 "merc back face culling marks: share of a draw's triangles agreeing with their "
                 "vertex normals (0: no marks)");
  app.add_option("--merc-cull-cos", opt.merc_cull_cos,
                 "merc culling: a corner's normal agrees when cos(angle to the face's side) >= this");
  app.add_option("--merc-cull-open", opt.merc_cull_open,
                 "merc culling: share of a draw's triangles that may have an open edge");
  app.add_option("--vista-error", opt.vista_error,
                 "vista tier: simplification error of ties and coarse tfrag (meters, 0: none)");
  app.add_option("--vista-ratio", opt.vista_ratio,
                 "vista tier: then down to this fraction of the triangles (sloppy)");
  app.add_option("--vista-sloppy-error", opt.vista_sloppy_error,
                 "vista tier: ... with at most this error (meters, 0: no sloppy step)");
  CLI11_PARSE(app, argc, argv);
  opt.verify = !no_verify;
  opt.vcache = !no_vcache;
  opt.vis_vertex_groups = !no_vis_groups;
  opt.atlas = !no_atlas;
  opt.opaque_alpha = !no_opaque_alpha;
  opt.tie_winding = !no_tie_winding;
  opt.tie_far_near_colors = !tie_far_own_colors;
  opt.tie_lod = !no_tie_lod;
  opt.stream = !no_stream;
  opt.vista = !no_vista;
  opt.tod = !no_tod;
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

  if (texanim) {
    return write_texanim(in, out) ? 0 : 1;
  }
  if (all) {
    file_util::create_dir_if_needed(out);
    // (AI-assisted) Jak II: the texture animations' textures (CtrTexAnimJak2)
    if (fs::exists(fs::path(in) / "GAME.fr3") &&
        (opt.game == "jak2" || fs::path(in).string().find("jak2") != std::string::npos)) {
      write_texanim(fs::path(in) / "GAME.fr3", fs::path(out) / "texanim.c3t");
    }
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
    // (AI-assisted) the "play as Keira" easter egg's KEIRA.c3l (from GAME.fr3 and village1.fr3)
    const fs::path game_fr3 = fs::path(in) / "GAME.fr3", village1_fr3 = fs::path(in) / "village1.fr3";
    if (fs::exists(game_fr3) && fs::exists(village1_fr3)) {
      Options o = opt;
      o.keira_from = village1_fr3.string();
      if (!convert(game_fr3, fs::path(out) / "KEIRA.c3l", o)) {
        return 1;
      }
    }
    return 0;
  }
  return convert(in, out, opt) ? 0 : 1;
}
