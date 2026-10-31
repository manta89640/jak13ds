/*!
 * c3l_mem: the linear memory .c3l levels take on the 3DS, as the runtime counts it (CtrLevels:
 * level_mem, load_textures), per level and per set of levels loaded together.
 *
 *   c3l_mem DIR [--sets FILE] [--vram KB] [--total KB] [--arena KB] [--reserve KB] [--formats]
 *   c3l_mem --compare-alpha OLD.c3l NEW.c3l
 *
 * Per level: texture pools (each texture 128-byte aligned, its levels down to 8 texels), resident
 * chunk meshes (every chunk but the near chunks, which live in the stream arena), merc meshes, the
 * envmapped ties' normals and the wind slots. A set (one line of FILE: level names, GAME added)
 * needs its levels' meshes and the part of its textures VRAM can't hold (--vram); what is left of
 * --total after it and the stream arena's base (--arena) is the set's free linear memory, and that
 * less --reserve its margin. Per set also: the bytes of textures in more than one of its levels
 * (the same size and texels) and the most a far level's detail-only textures take. --formats: each
 * level's texture bytes by format.
 *
 * --compare-alpha: every alpha mask texture (TEX_A8 / TEX_A4) of NEW against the texture of the
 * same index in OLD (RGBA8): every texel of every level must have the same alpha.
 */

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "game/graphics/ctr/c3l_format.h"
#include "game/graphics/ctr/c3l_reader.h"
#include "texshare.h"

namespace fs = std::filesystem;
using u8 = uint8_t;
using u32 = uint32_t;
using u64 = uint64_t;

namespace {

struct LevelMem {
  u64 tex = 0, tex_masks = 0, meshes = 0, near = 0, merc = 0, normals = 0, wind = 0;
  // the tfrag geomorph (c3l::ExtTfrag): the records in the meshes' tails, the color lists (heap)
  u64 tmorph = 0, tmorph_heap = 0;
  // the meshes Jak II's distance freeing can leave out (shrubs and ties with a distance limit)
  u64 dist_free = 0;
  // textures only drawn by the chunks a far level frees (CtrLevels::unload_detail)
  u64 tex_detail = 0;
  u64 by_format[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  // each texture's (size, texels) hash and pool bytes: the same texture in several levels
  std::vector<std::pair<u64, u32>> tex_ids;
  std::vector<u64> keys;  // the content keys (ExtTexKey)
  bool ok = false;
  u64 linear() const { return meshes + merc + normals + wind + tmorph; }
};

u32 mesh_bytes(u32 vertex_count, u32 index_count) {
  const u32 v = (vertex_count * 16u + 127u) & ~127u;
  return (v + index_count * 2u + 127u) & ~127u;
}

LevelMem level_mem(const fs::path& path) {
  LevelMem m;
  c3l::Reader f(path.string().c_str());
  c3l::Header hdr;
  if (!f.ok() || !f.read(0, &hdr, sizeof(hdr)) || memcmp(hdr.magic, c3l::kMagic, 4)) {
    return m;
  }
  std::vector<c3l::Texture> texs;
  std::vector<c3l::Chunk> chunks;
  std::vector<c3l::Draw> draws;
  std::vector<c3l::MercModel> models;
  std::vector<c3l::MercDraw> mdraws;
  if (!f.read_array(hdr.textures_offset, hdr.num_textures, &texs) ||
      !f.read_array(hdr.chunks_offset, hdr.num_chunks, &chunks) ||
      !f.read_array(hdr.draw_data_offset, hdr.draw_data_size / sizeof(c3l::Draw), &draws) ||
      !f.read_array(hdr.merc_models_offset, hdr.num_merc_models, &models) ||
      !f.read_array(hdr.merc_draw_offset, hdr.merc_draw_size / sizeof(c3l::MercDraw), &mdraws)) {
    return m;
  }
  // the textures only the chunks far mode doesn't draw use (far_chunk: the lowest tfrag tier, ties
  // without a distance limit, the vista tier), not merc's; a mask goes with its texture
  bool lowres = false;
  for (const auto& ch : chunks) {
    lowres |= ch.lod_tier == 3;
  }
  std::vector<u8> far_use(texs.size(), 0), near_use(texs.size(), 0);
  for (const auto& ch : chunks) {
    const bool far = ch.lod_tier == (lowres ? 3u : 2u) ||
                     (ch.lod_tier == 0 && ch.kind == c3l::CHUNK_KIND_TIE && ch.max_dist <= 0.f) ||
                     ch.kind == c3l::CHUNK_KIND_VISTA;
    for (u32 d = ch.first_draw; d < ch.first_draw + ch.draw_count && d < draws.size(); d++) {
      if (draws[d].texture < texs.size()) {
        (far ? far_use : near_use)[draws[d].texture] = 1;
      }
    }
  }
  for (const auto& d : mdraws) {
    if (d.texture < texs.size()) {
      far_use[d.texture] = 1;
    }
  }
  std::vector<u8> data;
  for (size_t ti = 0; ti < texs.size(); ti++) {
    const auto& t = texs[ti];
    if (!t.data_size) {
      continue;
    }
    const size_t owner = c3l::texture_is_mask(t) ? c3l::texture_mask_of(t) : ti;
    u32 b = 0;
    for (int l = 0; l < std::max<int>(1, t.levels); l++) {
      const u32 w = t.w >> l, h = t.h >> l, lb = c3l::texture_level_bytes(w, h, t.format);
      if (w < 8 || h < 8 || b + lb > t.data_size) {
        break;
      }
      b += lb;
    }
    m.tex += (b + 127) & ~127u;
    m.by_format[t.format & 7] += (b + 127) & ~127u;
    if (c3l::texture_is_mask(t)) {
      m.tex_masks += (b + 127) & ~127u;
    }
    if (owner < texs.size() && near_use[owner] && !far_use[owner]) {
      m.tex_detail += (b + 127) & ~127u;
    }
    data.resize(t.data_size);
    u64 h = 1469598103934665603ull ^ ((u64)t.w << 32 | (u64)t.h << 16 | t.format << 8 | t.levels);
    if (f.read(t.data_offset, data.data(), data.size())) {
      for (u8 c : data) {
        h = (h ^ c) * 1099511628211ull;
      }
    }
    m.tex_ids.emplace_back(h, (b + 127) & ~127u);
  }
  std::vector<u8> near(chunks.size(), 0);
  c3l::ExtHeader ext{};
  if (hdr.ext_offset && f.read(hdr.ext_offset, &ext, sizeof(ext))) {
    if (ext.size >= 48 && ext.stream_count) {
      std::vector<c3l::StreamChunk> sc;
      if (f.read_array(ext.stream_offset, ext.stream_count, &sc)) {
        for (const auto& s : sc) {
          if (s.chunk < near.size()) {
            near[s.chunk] = 1;
          }
        }
      }
    }
    if (ext.size >= c3l::kExtHeaderV21Size) {
      c3l::ExtEnvTie et{};
      std::vector<c3l::EnvTieChunk> ecs;
      if (f.read(hdr.ext_offset + c3l::kExtHeaderV20Size, &et, sizeof(et)) && et.chunk_count &&
          f.read_array(et.chunks_offset, et.chunk_count, &ecs)) {
        u64 n = 0;
        for (const auto& e : ecs) {
          n += e.chunk < chunks.size() ? chunks[e.chunk].vertex_count * 4u : 0;
        }
        m.normals = mesh_bytes((u32)((n + 15) / 16), 2);
      }
    }
    if (ext.size >= c3l::kExtHeaderTfragSize) {
      c3l::ExtTfrag et{};
      std::vector<c3l::TfragChunk> tcs;
      if (f.read(hdr.ext_offset + c3l::kExtHeaderMorphSize, &et, sizeof(et)) && et.chunk_count &&
          f.read_array(et.chunks_offset, et.chunk_count, &tcs)) {
        // (near chunks' records live in the stream arena with them)
        for (const auto& t : tcs) {
          m.tmorph += t.records_offset && t.chunk < chunks.size() && !near[t.chunk]
                          ? chunks[t.chunk].vertex_count * (u64)sizeof(c3l::TfragMorphVertex)
                          : 0;
        }
        m.tmorph_heap = (u64)et.color_count * sizeof(c3l::TfragMorphColor) +
                        (u64)et.group_count * sizeof(c3l::TfragGroup) +
                        (u64)et.run_lod_count * sizeof(c3l::RunLod);
      }
    }
    if (ext.size >= c3l::kExtHeaderV22Size) {
      c3l::ExtWind ew{};
      std::vector<c3l::WindChunk> wcs;
      if (f.read(hdr.ext_offset + c3l::kExtHeaderV21Size, &ew, sizeof(ew)) && ew.chunk_count &&
          f.read_array(ew.chunks_offset, ew.chunk_count, &wcs)) {
        for (const auto& w : wcs) {
          m.wind += w.chunk < chunks.size() ? (chunks[w.chunk].vertex_count + 127u) & ~127u : 0;
        }
      }
    }
  }
  for (size_t ci = 0; ci < chunks.size(); ci++) {
    const auto& ch = chunks[ci];
    u32 lo = UINT32_MAX, hi = 0;
    for (u32 d = ch.first_draw; d < ch.first_draw + ch.draw_count && d < draws.size(); d++) {
      lo = std::min(lo, draws[d].first_index);
      hi = std::max(hi, draws[d].first_index + draws[d].index_count);
    }
    if (hi <= lo) {
      continue;
    }
    (near[ci] ? m.near : m.meshes) += mesh_bytes(ch.vertex_count, hi - lo);
    if (!near[ci] && ch.lod_tier == 0 && ch.max_dist > 0.f &&
        (ch.kind == c3l::CHUNK_KIND_SHRUB || ch.kind == c3l::CHUNK_KIND_TIE)) {
      m.dist_free += mesh_bytes(ch.vertex_count, hi - lo);
    }
  }
  // one mesh per vertex range (a generated LOD shares its model's)
  std::map<u32, std::pair<u32, std::pair<u32, u32>>> ranges;
  for (const auto& mm : models) {
    auto& r = ranges.emplace(mm.first_vertex, std::make_pair(mm.vertex_count,
                                                              std::make_pair(UINT32_MAX, 0u)))
                  .first->second;
    for (u32 d = mm.first_draw; d < mm.first_draw + mm.draw_count && d < mdraws.size(); d++) {
      r.second.first = std::min(r.second.first, mdraws[d].first_index);
      r.second.second = std::max(r.second.second, mdraws[d].first_index + mdraws[d].index_count);
    }
  }
  for (const auto& [first, r] : ranges) {
    const u32 lo = r.second.first, hi = r.second.second;
    m.merc += (u64)r.first * sizeof(c3l::MercVertex) + (hi > lo ? (hi - lo) * 2u : 0);
  }
  texshare::LevelKeys lk;
  if (texshare::read_level_keys(path, &lk)) {
    m.keys = std::move(lk.keys);
  }
  m.ok = true;
  return m;
}

int compare_alpha(const char* old_path, const char* new_path) {
  c3l::Reader a(old_path), b(new_path);
  c3l::Header ha, hb;
  std::vector<c3l::Texture> ta, tb;
  if (!a.ok() || !b.ok() || !a.read(0, &ha, sizeof(ha)) || !b.read(0, &hb, sizeof(hb)) ||
      !a.read_array(ha.textures_offset, ha.num_textures, &ta) ||
      !b.read_array(hb.textures_offset, hb.num_textures, &tb)) {
    fprintf(stderr, "can't read the files\n");
    return 2;
  }
  int masks = 0, bad = 0;
  for (const auto& m : tb) {
    if (!c3l::texture_is_mask(m)) {
      continue;
    }
    masks++;
    const u32 i = c3l::texture_mask_of(m);
    if (i >= ta.size() || ta[i].format != c3l::TEX_RGBA8 || ta[i].w != m.w || ta[i].h != m.h ||
        ta[i].levels != m.levels) {
      bad++;
      continue;
    }
    std::vector<u8> pa(ta[i].data_size), pb(m.data_size);
    if (!a.read(ta[i].data_offset, pa.data(), pa.size()) ||
        !b.read(m.data_offset, pb.data(), pb.size())) {
      bad++;
      continue;
    }
    u32 oa = 0, ob = 0;
    bool same = true;
    for (int l = 0; l < std::max<int>(1, m.levels); l++) {
      const u32 w = m.w >> l, h = m.h >> l;
      for (u32 k = 0; k < w * h; k++) {
        const u8 av = pa[oa + 4 * k];  // stored A, B, G, R
        const u8 bv = m.format == c3l::TEX_A8
                          ? pb[ob + k]
                          : (u8)(((pb[ob + k / 2] >> (4 * (k & 1))) & 0xf) * 17);
        same = same && av == bv;
      }
      oa += c3l::texture_level_bytes(w, h, c3l::TEX_RGBA8);
      ob += c3l::texture_level_bytes(w, h, m.format);
    }
    bad += !same;
  }
  printf("%s: %d alpha masks, %d differ from the RGBA8 alpha\n", new_path, masks, bad);
  return bad ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 4 && !strcmp(argv[1], "--compare-alpha")) {
    return compare_alpha(argv[2], argv[3]);
  }
  if (argc < 2) {
    fprintf(stderr, "c3l_mem DIR [--sets FILE] [--vram KB] [--total KB] [--arena KB] "
                    "[--reserve KB] [--formats] | --compare-alpha OLD NEW\n");
    return 2;
  }
  const fs::path dir = argv[1];
  std::string sets_file;
  u64 vram = 4650, total = 0, arena = 4096, reserve = 3072;
  bool formats = false, share = false, pools = false;
  for (int i = 2; i < argc; i += 2) {
    const std::string k = argv[i];
    if (k == "--formats" || k == "--share" || k == "--pools") {
      if (k == "--pools") {
        pools = true;
        i--;
        continue;
      }
      (k == "--formats" ? formats : share) = true;
      i--;
      continue;
    }
    if (i + 1 >= argc) {
      break;
    }
    const u64 v = strtoull(argv[i + 1], nullptr, 10);
    if (k == "--sets") {
      sets_file = argv[i + 1];
    } else if (k == "--vram") {
      vram = v;
    } else if (k == "--total") {
      total = v;
    } else if (k == "--arena") {
      arena = v;
    } else if (k == "--reserve") {
      reserve = v;
    }
  }
  std::map<std::string, LevelMem> levels;
  for (const auto& e : fs::directory_iterator(dir)) {
    if (e.path().extension() == ".c3l") {
      levels[e.path().stem().string()] = level_mem(e.path());
    }
  }
  printf("level              textures (masks) (detail)  meshes   near   merc normals wind tmorph "
         "linear KB (tmorph heap)\n");
  for (const auto& [name, m] : levels) {
    if (!m.ok) {
      printf("%-16s unreadable\n", name.c_str());
      continue;
    }
    printf("%-16s %8llu (%5llu) (%6llu) %7llu %6llu %6llu %7llu %4llu %6llu %8llu (%llu)\n",
           name.c_str(), (unsigned long long)m.tex / 1024, (unsigned long long)m.tex_masks / 1024,
           (unsigned long long)m.tex_detail / 1024,
           (unsigned long long)m.meshes / 1024, (unsigned long long)m.near / 1024,
           (unsigned long long)m.merc / 1024, (unsigned long long)m.normals / 1024,
           (unsigned long long)m.wind / 1024, (unsigned long long)m.tmorph / 1024,
           (unsigned long long)(m.linear() + m.tex) / 1024,
           (unsigned long long)m.tmorph_heap / 1024);
  }
  if (formats) {
    printf("\nlevel            RGB565 RGBA4  ETC1 ETC1A4 RGBA8    A8    A4 KB\n");
    for (const auto& [name, m] : levels) {
      printf("%-16s", name.c_str());
      for (int k = 0; k < 7; k++) {
        printf(" %5llu", (unsigned long long)m.by_format[k] / 1024);
      }
      printf("\n");
    }
  }
  if (sets_file.empty()) {
    return 0;
  }
  // texshare.c3t: the share groups (--pools: the texture pools a set's levels make)
  std::map<u64, u32> group_of;
  {
    std::ifstream ts(dir / "texshare.c3t", std::ios::binary);
    c3l::TexShareHeader h{};
    if (ts.read((char*)&h, sizeof(h)) && !memcmp(h.magic, c3l::kTexShareMagic, 4)) {
      std::vector<c3l::TexShareEntry> e(h.count);
      ts.read((char*)e.data(), (std::streamsize)(e.size() * sizeof(e[0])));
      for (const auto& x : e) {
        group_of[x.key] = x.group;
      }
    }
  }
  std::ifstream in(sets_file);
  std::string line;
  printf("\nset                                   textures  in linear  meshes+  free KB  "
         "margin KB  shared  detail  floor\n");
  while (std::getline(in, line)) {
    std::istringstream ss(line);
    std::vector<std::string> names{"GAME"};
    std::string n;
    while (ss >> n) {
      if (n != "GAME") {
        names.push_back(n);
      }
    }
    if (names.size() < 2) {
      continue;
    }
    u64 tex = 0, lin = 0, shared = 0, detail = 0, dfree = 0;
    std::string label, missing;
    std::map<u64, int> seen;
    for (const auto& nm : names) {
      auto it = levels.find(nm);
      if (it == levels.end() || !it->second.ok) {
        missing += " " + nm;
        continue;
      }
      tex += it->second.tex;
      lin += it->second.linear();
      dfree += it->second.dist_free;
      // the most a far level gives back: the largest one's (one level is the camera's)
      if (nm != "GAME") {
        detail = std::max(detail, it->second.tex_detail);
      }
      for (const auto& [h, b] : it->second.tex_ids) {
        if (seen[h]++) {
          shared += b;
        }
      }
      if (nm != "GAME") {
        label += (label.empty() ? "" : "+") + nm;
      }
    }
    if (pools) {
      std::set<u32> groups;
      u64 own = 0;
      for (const auto& nm : names) {
        auto it = levels.find(nm);
        if (it == levels.end()) {
          continue;
        }
        for (size_t t = 0; t < it->second.keys.size(); t++) {
          auto g = group_of.find(it->second.keys[t]);
          if (g != group_of.end()) {
            groups.insert(g->second);
          }
        }
        own += it->second.tex / (256 * 1024) + 1;
      }
      printf("  pools: %llu own pieces (at most), %zu share groups\n", (unsigned long long)own,
             groups.size());
    }
    // with --share: a texture several of the set's levels have is loaded once
    if (share) {
      tex -= shared;
    }
    const u64 tex_linear = tex / 1024 > vram ? tex / 1024 - vram : 0;
    const long long free_kb = (long long)total - (long long)(lin / 1024 + tex_linear + arena);
    // the least linear memory the set needs (Jak II: all distance-limited meshes left out)
    const unsigned long long floor_kb = (lin - dfree) / 1024 + tex_linear + arena;
    printf("%-36s %9llu %10llu %8llu %8lld %10lld %7llu %7llu %6llu%s\n", label.c_str(),
           (unsigned long long)tex / 1024, (unsigned long long)tex_linear,
           (unsigned long long)lin / 1024, free_kb, free_kb - (long long)reserve,
           (unsigned long long)shared / 1024, (unsigned long long)detail / 1024, floor_kb,
           missing.empty() ? "" : (" (missing:" + missing + ")").c_str());
  }
  return 0;
}
