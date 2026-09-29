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

#include "fmt/format.h"
#include "game/graphics/ctr/c3l_format.h"
#include "third-party/CLI11.hpp"

namespace {

struct Options {
  int tfrag_geo = 2;  // 0 = most detailed, 2 = least
  int tie_geo = 3;    // 0 = most detailed, 3 = least
  int palette = 1;    // time of day palette to bake (0..7)
  int max_tex = 128;
  float cell_meters = 100.f;
  bool no_tie = false;
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

  void add_tfrag_tree(tfrag3::TfragTree& tree) {
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
    for (auto& d : tree.static_draws) {
      DrawKey k{d.mode.as_int(), texture_id(d.tree_tex_id), 1};
      add_strips(tree.unpacked.indices, d.unpacked.idx_of_first_idx_in_full_buffer,
                 draw_index_count(d), base, key_id(k));
    }
  }
};

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

std::vector<u8> convert_texture(const tfrag3::Texture& tex, int max_size, c3l::Texture* out_desc) {
  // source RGBA8888, PS2 alpha (0x80 = opaque)
  u32 sw = tex.w, sh = tex.h;
  u32 dw = std::clamp<u32>(next_pow2(sw), 8, max_size);
  u32 dh = std::clamp<u32>(next_pow2(sh), 8, max_size);
  std::vector<u8> rgba(dw * dh * 4);
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
        rgba[4 * (x + y * dw) + c] = n ? acc[c] / n : 0;
      }
    }
  }
  bool has_alpha = false;
  for (u32 i = 0; i < dw * dh; i++) {
    u32 a = std::min(255u, rgba[4 * i + 3] * 2u);
    rgba[4 * i + 3] = a;
    if (a < 240) {
      has_alpha = true;
    }
  }
  std::vector<u8> out(dw * dh * 2);
  for (u32 y = 0; y < dh; y++) {
    for (u32 x = 0; x < dw; x++) {
      const u8* p = &rgba[4 * (x + y * dw)];
      u16 v;
      if (has_alpha) {
        v = ((p[0] >> 4) << 12) | ((p[1] >> 4) << 8) | ((p[2] >> 4) << 4) | (p[3] >> 4);
      } else {
        v = ((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3);
      }
      u32 idx = c3l::tiled_index(x, y, dw, dh);
      memcpy(&out[2 * idx], &v, 2);
    }
  }
  out_desc->w = dw;
  out_desc->h = dh;
  out_desc->format = has_alpha ? c3l::TEX_RGBA4 : c3l::TEX_RGB565;
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
  for (auto& tree : level.tfrag_trees[opt.tfrag_geo]) {
    if (is_drawn_tfrag_kind(tree.kind)) {
      cv.add_tfrag_tree(tree);
      tfrag_trees++;
    }
  }
  if (!opt.no_tie) {
    for (auto& tree : level.tie_trees[opt.tie_geo]) {
      cv.add_tie_tree(tree);
      tie_trees++;
    }
  }

  // ---- chunking: grid cells by triangle centroid ----
  const float cell = opt.cell_meters * 4096.f;
  std::map<std::tuple<int, int, int>, std::vector<u32>> cells;  // cell -> triangle indices
  for (u32 i = 0; i < cv.tris.size(); i++) {
    const auto& t = cv.tris[i];
    float cx = 0, cy = 0, cz = 0;
    for (int k = 0; k < 3; k++) {
      cx += cv.verts[t.v[k]].x;
      cy += cv.verts[t.v[k]].y;
      cz += cv.verts[t.v[k]].z;
    }
    auto key = std::make_tuple((int)std::floor(cx / 3 / cell), (int)std::floor(cy / 3 / cell),
                               (int)std::floor(cz / 3 / cell));
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

  // ---- textures ----
  std::vector<c3l::Texture> tex_descs;
  std::vector<std::vector<u8>> tex_datas;
  for (u32 src : cv.tex_order) {
    c3l::Texture d{};
    tex_datas.push_back(convert_texture(level.textures.at(src), opt.max_tex, &d));
    tex_descs.push_back(d);
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
      "{}: {} tfrag + {} tie trees -> {} chunks, {} verts ({} KB), {} tris, {} draws, {} textures "
      "({} KB), file {} KB",
      level.level_name, tfrag_trees, tie_trees, chunks.size(), out_verts.size(),
      out_verts.size() * sizeof(c3l::Vertex) / 1024, out_indices.size() / 3, out_draws.size(),
      tex_descs.size(), hdr.texture_data_size / 1024, buf.size() / 1024);
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
  app.add_option("--palette", opt.palette, "time of day palette to bake (0-7)");
  app.add_option("--max-tex", opt.max_tex, "maximum texture size (power of two, <= 1024)");
  app.add_option("--cell", opt.cell_meters, "chunk grid size in meters");
  app.add_flag("--no-tie", opt.no_tie, "leave out tie");
  CLI11_PARSE(app, argc, argv);
  lg::initialize();

  if (all) {
    file_util::create_dir_if_needed(out);
    for (auto& e : fs::directory_iterator(in)) {
      if (e.path().extension() == ".fr3" && e.path().stem() != "GAME" &&
          e.path().stem() != "common") {
        auto dst = fs::path(out) / (e.path().stem().string() + ".c3l");
        if (!convert(e.path(), dst, opt)) {
          return 1;
        }
      }
    }
    return 0;
  }
  return convert(in, out, opt) ? 0 : 1;
}
