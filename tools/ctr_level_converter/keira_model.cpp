/*!
 * @file keira_model.cpp
 * (AI-assisted)
 * See keira_model.h.
 */

#include "keira_model.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <set>
#include <unordered_map>

#include "common/log/log.h"

#include "game/graphics/ctr/CtrKeiraRig.h"

namespace keira_model {
namespace {

constexpr char kJakModel[] = "eichar-lod0";
constexpr char kKeiraModel[] = "assistant-lod0";

const tfrag3::MercModel* find_model(const tfrag3::Level& lev, const char* name) {
  for (const auto& m : lev.merc_data.models) {
    if (m.name == name) {
      return &m;
    }
  }
  return nullptr;
}

// the vertices a model's draws use (all of them: base and blend shape draws)
template <typename F>
void for_each_vertex(const tfrag3::MercModelGroup& g, const tfrag3::MercModel& m, F&& f) {
  for (const auto& e : m.effects) {
    for (const auto& d : e.all_draws) {
      for (u32 i = d.first_index; i < d.first_index + d.index_count && i < g.indices.size(); i++) {
        if (g.indices[i] != UINT32_MAX) {
          f(g.vertices.at(g.indices[i]));
        }
      }
    }
  }
}

void log_model(const tfrag3::Level& lev, const tfrag3::MercModel& m) {
  for (size_t ei = 0; ei < m.effects.size(); ei++) {
    const auto& e = m.effects[ei];
    std::string eyes;
    std::set<std::string> tex;
    for (const auto& d : e.all_draws) {
      if (d.eye_id != 0xff) {
        eyes += fmt::format(" {}", d.eye_id);
      }
      if (d.tree_tex_id >= 0 && d.tree_tex_id < (s32)lev.textures.size()) {
        tex.insert(lev.textures[d.tree_tex_id].debug_name);
      } else {
        tex.insert(fmt::format("#{}", d.tree_tex_id));
      }
    }
    std::string tl;
    for (const auto& t : tex) {
      tl += " " + t;
    }
    // the blend shape weights its targets use (Blerc int data: per vertex the targets' weight
    // indices, a terminator, the vertex)
    std::set<int> wset;
    {
      const auto& bl = e.mod.blerc;
      size_t ii = 0;
      while (ii < bl.int_data.size()) {
        while (ii < bl.int_data.size() && bl.int_data[ii] != tfrag3::Blerc::kTargetIdxTerminator) {
          wset.insert(bl.int_data[ii++]);
        }
        ii += 2;
      }
    }
    for (int w : wset) {
      tl += fmt::format(" w{}", w);
    }
    // (AI-assisted) what each weight moves: where (offset-weighted center of its vertices, m) and
    // which way (mean offset, mm per full weight)
    {
      const auto& bl = e.mod.blerc;
      std::map<int, std::array<double, 7>> acc;  // cx cy cz dx dy dz sum|d|
      size_t ii = 0, fi = 0;
      while (ii < bl.int_data.size() && fi < bl.float_data.size()) {
        const float* base = bl.float_data[fi++].v;
        while (ii < bl.int_data.size() && bl.int_data[ii] != tfrag3::Blerc::kTargetIdxTerminator &&
               fi < bl.float_data.size()) {
          const float* o = bl.float_data[fi++].v;
          const double l = std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
          auto& a = acc[bl.int_data[ii++]];
          for (int c = 0; c < 3; c++) {
            a[c] += base[c] * l;
            a[3 + c] += o[c];
          }
          a[6] += l;
        }
        ii += 2;
      }
      for (auto& [w, a] : acc) {
        if (a[6] <= 0) {
          continue;
        }
        lg::info("keira: {} effect {} weight {}: at ({:.3f} {:.3f} {:.3f}) m, moves ({:.1f} {:.1f} "
                 "{:.1f}) sum {:.1f}",
                 m.name, ei, w, a[0] / a[6] / 4096, a[1] / a[6] / 4096, a[2] / a[6] / 4096, a[3],
                 a[4], a[5], a[6]);
      }
    }
    lg::info("keira: {} effect {}: {} draws, envmap {}, mod draws {}+{} (blend shapes {}), eyes{}; "
             "textures:{}",
             m.name, ei, e.all_draws.size(), e.has_envmap, e.mod.fix_draw.size(),
             e.mod.mod_draw.size(), e.mod.blerc.float_data.size(), eyes.empty() ? " none" : eyes,
             tl);
  }
}

}  // namespace

bool make_level(const tfrag3::Level& game, const tfrag3::Level& vil, tfrag3::Level* out) {
  const tfrag3::MercModel* jak = find_model(game, kJakModel);
  const tfrag3::MercModel* kei = find_model(vil, kKeiraModel);
  if (!jak || !kei) {
    lg::error("keira: no {} in the common file or no {} in village1", kJakModel, kKeiraModel);
    return false;
  }
  log_model(game, *jak);
  log_model(vil, *kei);
  const auto& rig = ctr_keira::rig();

  // Jak's eye slots (left, right): his eye draws' (slot << 1 | right)
  int jak_eye[2] = {-1, -1};
  for (const auto& e : jak->effects) {
    for (const auto& d : e.all_draws) {
      if (d.eye_id != 0xff) {
        jak_eye[d.eye_id & 1] = d.eye_id;
      }
    }
  }
  // the joints his mesh uses (the slots the game sends for him)
  std::set<int> jak_used;
  for_each_vertex(game.merc_data, *jak, [&](const tfrag3::MercVertex& v) {
    for (int k = 0; k < 3; k++) {
      if (v.weights[k] > 0) {
        jak_used.insert(v.mats[k] - 1);
      }
    }
  });
  std::string used;
  for (int j : jak_used) {
    used += fmt::format(" {}", j);
  }
  lg::info("keira: Jak's mesh uses {} joints:{}; eye slots {} {}", jak_used.size(), used,
           jak_eye[0], jak_eye[1]);
  for (int k = 0; k < rig.count; k++) {
    int j = rig.jak[k];
    while (j >= 0 && !jak_used.count(j)) {
      j = rig.jak_parent[j];
    }
    if (j != rig.jak[k]) {
      lg::info("keira: rig joint {} follows Jak joint {}, which the game doesn't send: {} instead",
               rig.name[k], rig.jak[k], j);
    }
  }

  // her weights per joint, before and after the upper chest split
  std::map<int, double> weight_of;
  out->level_name = ctr_keira::kFileName;
  out->textures = vil.textures;  // the converter writes the ones the draws use
  tfrag3::MercModel m;
  m.name = std::string(ctr_keira::kModelPrefix) + kJakModel;
  m.max_draws = kei->max_draws;
  m.max_bones = kei->max_bones;
  m.st_vif_add = kei->st_vif_add;
  m.xyz_scale = kei->xyz_scale;
  m.st_magic = kei->st_magic;
  auto& g = out->merc_data;
  std::unordered_map<u32, u32> vmap;  // her vertex -> ours
  const int chest = ctr_keira::keira_joint("chest");
  // her welding torch (joints torch, torchTIP): placed in her hand by her own animations, at her
  // feet in the bind pose: left out
  auto torch = [&](const tfrag3::MercVertex& v) {
    for (int k = 0; k < 3; k++) {
      if (v.weights[k] > 0) {
        const char* n = rig.name[v.mats[k] - 1];
        if (!strncmp(n, "torch", 5) || !strncmp(n, "camera", 6)) {
          return true;
        }
      }
    }
    return false;
  };
  auto remap = [&](tfrag3::MercVertex& v) {
    // influences: (rig joint, weight); her chest's upper part to the upper chest joint
    std::vector<std::pair<int, float>> inf;
    for (int k = 0; k < 3; k++) {
      if (v.weights[k] > 0) {
        const int j = v.mats[k] - 1;
        if (j == chest) {
          const float s = ctr_keira::chest2_share(v.pos[1]);
          if (s > 0) {
            inf.push_back({rig.chest2, v.weights[k] * s});
          }
          if (s < 1) {
            inf.push_back({j, v.weights[k] * (1 - s)});
          }
        } else {
          inf.push_back({j, v.weights[k]});
        }
      }
    }
    std::sort(inf.begin(), inf.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });
    if (inf.size() > 3) {
      inf.resize(3);
    }
    float sum = 0;
    for (const auto& p : inf) {
      sum += p.second;
    }
    for (int k = 0; k < 3; k++) {
      if (k < (int)inf.size() && sum > 0) {
        v.mats[k] = inf[k].first + 1;
        v.weights[k] = inf[k].second / sum;
        weight_of[inf[k].first] += v.weights[k];
      } else {
        v.mats[k] = 0;
        v.weights[k] = 0;
      }
    }
  };
  auto add_vertex = [&](u32 vi) -> u32 {
    auto it = vmap.find(vi);
    if (it != vmap.end()) {
      return it->second;
    }
    tfrag3::MercVertex v = vil.merc_data.vertices.at(vi);
    remap(v);
    vmap.emplace(vi, (u32)g.vertices.size());
    g.vertices.push_back(v);
    return (u32)g.vertices.size() - 1;
  };
  // her effects match his (0 body, 1 eyes, 2 and 3 shiny parts and hair, with envmaps): the game
  // enables and fades his, so hers keep their numbers
  int eye_draws = 0, torch_tris = 0, tris = 0, blend_effects = 0;
  for (const auto& e : kei->effects) {
    auto& eff = m.effects.emplace_back();
    eff.has_envmap = e.has_envmap;
    eff.envmap_mode = e.envmap_mode;
    eff.envmap_texture = e.envmap_texture;
    // (AI-assisted) a draw list: from_mod: the blend shape draws, their indices into the effect's
    // own vertices (mod.vertices, kept whole: the blend shapes name them), else hers (compacted)
    auto copy_draws = [&](const std::vector<tfrag3::MercDraw>& src, bool from_mod,
                          std::vector<tfrag3::MercDraw>* dst) {
      const auto& vs = from_mod ? e.mod.vertices : vil.merc_data.vertices;
      for (const auto& d : src) {
        tfrag3::MercDraw nd = d;
        nd.first_index = g.indices.size();
        nd.no_strip = true;  // plain triangles (the torch's taken out of the strips)
        if (d.eye_id != 0xff) {
          eye_draws++;
          nd.eye_id = jak_eye[d.eye_id & 1] >= 0 ? jak_eye[d.eye_id & 1] : 0xff;
        }
        // the strips as the converter reads them (restarts at UINT32_MAX, degenerates dropped)
        std::vector<u32> strip;
        auto flush = [&]() {
          for (size_t i = 2; i < strip.size(); i++) {
            const u32 a = strip[i - 2], b = strip[i - 1], c = strip[i];
            if (a == b || b == c || a == c) {
              continue;
            }
            if (torch(vs.at(a)) || torch(vs.at(b)) || torch(vs.at(c))) {
              torch_tris++;
              continue;
            }
            for (u32 x : {a, b, c}) {
              g.indices.push_back(from_mod ? x : add_vertex(x));
            }
            tris++;
          }
          strip.clear();
        };
        for (u32 i = d.first_index; i < d.first_index + d.index_count; i++) {
          const u32 vi = vil.merc_data.indices.at(i);
          if (d.no_strip) {
            strip.push_back(vi);
            if (strip.size() == 3) {
              flush();
            }
          } else if (vi == UINT32_MAX) {
            flush();
          } else {
            strip.push_back(vi);
          }
        }
        flush();
        nd.index_count = g.indices.size() - nd.first_index;
        nd.num_triangles = nd.index_count / 3;
        if (nd.index_count) {
          dst->push_back(nd);
        }
      }
    };
    copy_draws(e.all_draws, false, &eff.all_draws);
    // her face: the blend shapes (as Jak's: the game sends his weights, by number)
    if (!e.mod.mod_draw.empty() && !e.mod.blerc.int_data.empty()) {
      eff.mod.vertices = e.mod.vertices;
      for (auto& v : eff.mod.vertices) {
        if (!torch(v)) {
          remap(v);
        }
      }
      eff.mod.blerc = e.mod.blerc;
      eff.has_mod_draw = e.has_mod_draw;
      copy_draws(e.mod.fix_draw, false, &eff.mod.fix_draw);
      copy_draws(e.mod.mod_draw, true, &eff.mod.mod_draw);
      blend_effects++;
    }
  }
  // (AI-assisted) her shadow: a convex hull per body part (each rigid on its joint), see
  // CtrKeiraRig.h ShadowMesh
  tfrag3::MercModel sm;
  sm.name = m.name + ctr_keira::kShadowSuffix;
  sm.max_draws = 1;
  sm.max_bones = 24;
  sm.st_vif_add = m.st_vif_add;
  sm.xyz_scale = m.xyz_scale;
  sm.st_magic = m.st_magic;
  int hulls = 0, hull_verts = 0, hull_tris = 0;
  {
    static const char* kParts[] = {"head",      "backHair1", "neck",   "chest",  "chest2",
                                   "hips",      "Lshoulder", "Lelbow", "Lhand",  "Rshoulder",
                                   "Relbow",    "Rhand",     "Lthigh", "Lknee",  "Lankle",
                                   "Rthigh",    "Rknee",     "Rankle"};
    auto part_of = [&](int k) {
      for (int j = k; j >= 0; j = rig.parent[j]) {
        if (!strcmp(rig.name[j], "main")) {
          return (int)ctr_keira::keira_joint("hips");
        }
        for (const char* n : kParts) {
          if (!strcmp(rig.name[j], n)) {
            return j;
          }
        }
      }
      return -1;
    };
    std::map<int, std::vector<math::Vector3f>> pts;
    for (const auto& v : g.vertices) {
      if (v.weights[0] > 0 && v.mats[0] > 0) {
        const int part = part_of(v.mats[0] - 1);
        if (part >= 0) {
          pts[part].push_back(v.pos_vec);
        }
      }
    }
    auto& seff = sm.effects.emplace_back();
    tfrag3::MercDraw sd;
    sd.tree_tex_id = -1;
    sd.eye_id = 0xff;
    sd.no_strip = true;
    sd.first_index = g.indices.size();
    for (auto& [part, p] : pts) {
      if (p.size() < 8) {
        continue;
      }
      // the extreme points along 26 directions, nudged off exact coplanarity (deterministic)
      std::vector<math::Vector3f> h;
      for (int dx = -1; dx <= 1; dx++) {
        for (int dy = -1; dy <= 1; dy++) {
          for (int dz = -1; dz <= 1; dz++) {
            if (!dx && !dy && !dz) {
              continue;
            }
            const math::Vector3f dir((float)dx, (float)dy, (float)dz);
            size_t best = 0;
            for (size_t i = 1; i < p.size(); i++) {
              if (p[i].dot(dir) > p[best].dot(dir)) {
                best = i;
              }
            }
            bool dup = false;
            for (const auto& q : h) {
              dup |= (q - p[best]).length() < 8.f;
            }
            if (!dup) {
              h.push_back(p[best]);
            }
          }
        }
      }
      for (size_t i = 0; i < h.size(); i++) {
        h[i] += math::Vector3f(0.37f * ((i * 7) % 5), 0.29f * ((i * 3) % 7), 0.41f * ((i * 5) % 3));
      }
      const int n = (int)h.size();
      if (n < 4) {
        continue;
      }
      // brute force hull: a triangle is a face when every other point is behind it
      std::vector<std::array<int, 3>> faces;
      for (int a = 0; a < n; a++) {
        for (int b = a + 1; b < n; b++) {
          for (int c = b + 1; c < n; c++) {
            const math::Vector3f nrm = (h[b] - h[a]).cross(h[c] - h[a]);
            if (nrm.length() < 1e-3f) {
              continue;
            }
            int pos = 0, neg = 0;
            for (int o = 0; o < n; o++) {
              if (o == a || o == b || o == c) {
                continue;
              }
              const float d = nrm.dot(h[o] - h[a]);
              pos += d > 0;
              neg += d < 0;
            }
            if (!pos) {
              faces.push_back({a, b, c});
            } else if (!neg) {
              faces.push_back({a, c, b});
            }
          }
        }
      }
      std::map<int, u32> used;
      const u32 v0 = g.vertices.size();
      for (const auto& f : faces) {
        for (int i : f) {
          if (!used.count(i)) {
            used[i] = 0;
          }
        }
      }
      for (auto& [i, out_i] : used) {
        tfrag3::MercVertex v{};
        v.pos_vec = h[i];
        v.weights[0] = 1.f;
        v.mats[0] = part + 1;
        out_i = g.vertices.size();
        g.vertices.push_back(v);
      }
      for (const auto& f : faces) {
        for (int i : f) {
          g.indices.push_back(used[i]);
        }
      }
      hulls++;
      hull_verts += g.vertices.size() - v0;
      hull_tris += faces.size();
    }
    sd.index_count = g.indices.size() - sd.first_index;
    sd.num_triangles = sd.index_count / 3;
    seff.all_draws.push_back(sd);
  }
  lg::info("keira: shadow: {} hulls, {} vertices, {} triangles", hulls, hull_verts, hull_tris);
  g.models.push_back(std::move(m));
  if (hulls) {
    g.models.push_back(std::move(sm));
  }
  std::string wl;
  for (const auto& [j, w] : weight_of) {
    wl += fmt::format(" {}:{:.0f}", rig.name[j], w);
  }
  size_t draws = 0;
  for (const auto& e : g.models.front().effects) {
    draws += e.all_draws.size();
  }
  lg::info("keira: {}: {} vertices, {} triangles, {} draws ({} eye draws on Jak's eye slots), {} "
           "triangles of her torch left out, {} effects with blend shapes; weight per rig joint:{}",
           g.models.front().name, g.vertices.size(), tris, draws, eye_draws, torch_tris,
           blend_effects, wl);
  return true;
}

}  // namespace keira_model
