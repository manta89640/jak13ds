/*!
 * @file keira_model.cpp
 * (AI-assisted)
 * See keira_model.h.
 */

#include "keira_model.h"

#include <algorithm>
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
  auto add_vertex = [&](u32 vi) -> u32 {
    auto it = vmap.find(vi);
    if (it != vmap.end()) {
      return it->second;
    }
    tfrag3::MercVertex v = vil.merc_data.vertices.at(vi);
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
    vmap.emplace(vi, (u32)g.vertices.size());
    g.vertices.push_back(v);
    return (u32)g.vertices.size() - 1;
  };
  // her effects match his (0 body, 1 eyes, 2 and 3 shiny parts and hair, with envmaps): the game
  // enables and fades his, so hers keep their numbers
  int eye_draws = 0, torch_tris = 0, tris = 0;
  for (const auto& e : kei->effects) {
    auto& eff = m.effects.emplace_back();
    eff.has_envmap = e.has_envmap;
    eff.envmap_mode = e.envmap_mode;
    eff.envmap_texture = e.envmap_texture;
    for (const auto& d : e.all_draws) {
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
          const auto& vs = vil.merc_data.vertices;
          if (torch(vs.at(a)) || torch(vs.at(b)) || torch(vs.at(c))) {
            torch_tris++;
            continue;
          }
          g.indices.push_back(add_vertex(a));
          g.indices.push_back(add_vertex(b));
          g.indices.push_back(add_vertex(c));
          tris++;
        }
        strip.clear();
      };
      for (u32 i = d.first_index; i < d.first_index + d.index_count; i++) {
        const u32 vi = vil.merc_data.indices.at(i);
        if (d.no_strip) {
          // (not in the game's models) already triangles
          strip.push_back(vi);
          if (strip.size() == 3) {
            const std::vector<u32> t = strip;
            strip = {t[0], t[1], t[2]};
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
        eff.all_draws.push_back(nd);
      }
    }
  }
  g.models.push_back(std::move(m));
  std::string wl;
  for (const auto& [j, w] : weight_of) {
    wl += fmt::format(" {}:{:.0f}", rig.name[j], w);
  }
  size_t draws = 0;
  for (const auto& e : g.models.back().effects) {
    draws += e.all_draws.size();
  }
  lg::info("keira: {}: {} vertices, {} triangles, {} draws ({} eye draws on Jak's eye slots), {} "
           "triangles of her torch left out; weight per rig joint:{}",
           g.models.back().name, g.vertices.size(), tris, draws, eye_draws, torch_tris, wl);
  return true;
}

}  // namespace keira_model
