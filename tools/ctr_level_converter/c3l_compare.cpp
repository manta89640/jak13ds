/*!
 * @file c3l_compare.cpp
 * Compares two .c3l files by content, whatever their section order: every table and every piece
 * the runtime reads (chunk vertices and mesh tails, indices, textures, the extension's tables).
 */

#include "c3l_compare.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <vector>

#include "common/log/log.h"

#include "game/graphics/ctr/c3l_reader.h"

namespace c3l_compare {

using namespace c3l;

namespace {
int g_diff = 0;

struct F {
  Reader r;
  Header h{};
  ExtHeader eh{};
  std::vector<uint8_t> ext;  // the extension header bytes
  explicit F(const char* p) : r(p) {
    r.read(0, &h, sizeof(h));
    if (h.ext_offset) {
      uint32_t ms[2];
      r.read(h.ext_offset, ms, 8);
      ext.resize(ms[1]);
      r.read(h.ext_offset, ext.data(), ext.size());
      memcpy(&eh, ext.data(), std::min(sizeof(eh), ext.size()));
    }
  }
  std::vector<uint8_t> get(uint32_t off, size_t n) {
    std::vector<uint8_t> v(n);
    if (n && !r.read(off, v.data(), n)) {
      lg::error("compare: read of {} bytes at {} failed", n, off);
      g_diff++;
    }
    return v;
  }
  template <typename T>
  std::vector<T> arr(uint32_t off, size_t n) {
    std::vector<T> v(n);
    if (n && !r.read(off, v.data(), n * sizeof(T))) {
      lg::error("compare: read of {} bytes at {} failed", n, off);
      g_diff++;
    }
    return v;
  }
  template <typename T>
  T sub(uint32_t at) {
    T t{};
    if (at + sizeof(T) <= ext.size()) {
      memcpy(&t, ext.data() + at, sizeof(T));
    }
    return t;
  }
};

void same(const char* what, const void* a, size_t na, const void* b, size_t nb) {
  if (na != nb || (na && memcmp(a, b, na))) {
    if (g_diff < 20) {
      lg::error("compare: {} differ ({} vs {} bytes)", what, na, nb);
    }
    g_diff++;
  }
}
template <typename T>
void same_v(const char* what, const std::vector<T>& a, const std::vector<T>& b) {
  same(what, a.data(), a.size() * sizeof(T), b.data(), b.size() * sizeof(T));
}

}  // namespace

namespace {
bool compare_file(const std::string& pa, const std::string& pb) {
  g_diff = 0;
  F a(pa.c_str()), b(pb.c_str());
  if (!a.r.ok() || !b.r.ok()) {
    lg::error("compare: can't read {} or {}", pa, pb);
    return false;
  }
  Header ha = a.h, hb = b.h;
  // offsets differ; counts and sizes must not (vertex_data_size holds the tails)
  for (Header* h : {&ha, &hb}) {
    h->tod_offset = h->ext_offset = h->textures_offset = h->chunks_offset = 0;
    h->vertex_data_offset = h->vertex_data_size = h->index_data_offset = h->draw_data_offset = 0;
    h->texture_data_offset = h->merc_models_offset = h->merc_vertex_offset = 0;
    h->merc_index_offset = h->merc_draw_offset = h->merc_blerc_offset = 0;
  }
  same("header", &ha, sizeof(ha), &hb, sizeof(hb));
  auto ca = a.arr<Chunk>(a.h.chunks_offset, a.h.num_chunks);
  auto cb = b.arr<Chunk>(b.h.chunks_offset, b.h.num_chunks);
  same_v("draws", a.arr<Draw>(a.h.draw_data_offset, a.h.draw_data_size / sizeof(Draw)),
         b.arr<Draw>(b.h.draw_data_offset, b.h.draw_data_size / sizeof(Draw)));
  same_v("indices", a.arr<uint16_t>(a.h.index_data_offset, a.h.index_data_size / 2),
         b.arr<uint16_t>(b.h.index_data_offset, b.h.index_data_size / 2));
  uint64_t verts = 0;
  for (size_t i = 0; i < ca.size() && i < cb.size(); i++) {
    Chunk x = ca[i], y = cb[i];
    auto va = a.arr<Vertex>(a.h.vertex_data_offset + x.first_vertex * 16, x.vertex_count);
    auto vb = b.arr<Vertex>(b.h.vertex_data_offset + y.first_vertex * 16, y.vertex_count);
    same_v("chunk vertices", va, vb);
    verts += va.size();
    same_v("chunk vis runs", a.arr<VisRun>(x.vis_runs_offset, x.vis_run_count),
           b.arr<VisRun>(y.vis_runs_offset, y.vis_run_count));
    x.first_vertex = y.first_vertex = x.vis_runs_offset = y.vis_runs_offset = 0;
    same("chunk", &x, sizeof(x), &y, sizeof(y));
  }
  // time of day
  const uint32_t csize = a.h.version >= kTodAlphaVersion ? sizeof(TodColorA) : sizeof(TodColor);
  same_v("tod", a.get(a.h.tod_offset, a.h.tod_color_count * csize + a.h.tod_mul_count * 6),
         b.get(b.h.tod_offset, b.h.tod_color_count * csize + b.h.tod_mul_count * 6));
  // textures
  auto ta = a.arr<Texture>(a.h.textures_offset, a.h.num_textures);
  auto tb = b.arr<Texture>(b.h.textures_offset, b.h.num_textures);
  for (size_t i = 0; i < ta.size() && i < tb.size(); i++) {
    same_v("texels", a.get(ta[i].data_offset, ta[i].data_size),
           b.get(tb[i].data_offset, tb[i].data_size));
    ta[i].data_offset = tb[i].data_offset = 0;
  }
  same_v("texture table", ta, tb);
  // merc
  same_v("merc models", a.arr<MercModel>(a.h.merc_models_offset, a.h.num_merc_models),
         b.arr<MercModel>(b.h.merc_models_offset, b.h.num_merc_models));
  same_v("merc verts", a.get(a.h.merc_vertex_offset, a.h.merc_vertex_size),
         b.get(b.h.merc_vertex_offset, b.h.merc_vertex_size));
  same_v("merc indices", a.get(a.h.merc_index_offset, a.h.merc_index_size),
         b.get(b.h.merc_index_offset, b.h.merc_index_size));
  same_v("merc draws", a.get(a.h.merc_draw_offset, a.h.merc_draw_size),
         b.get(b.h.merc_draw_offset, b.h.merc_draw_size));
  if (a.h.merc_blerc_offset) {
    auto ba = a.get(a.h.merc_blerc_offset, a.h.merc_blerc_size);
    auto bb = b.get(b.h.merc_blerc_offset, b.h.merc_blerc_size);
    MercBlercHeader xa, xb;
    memcpy(&xa, ba.data(), sizeof(xa));
    memcpy(&xb, bb.data(), sizeof(xb));
    // mod_offset is absolute: compare it relative to the blend shapes
    uint32_t ra = xa.mod_offset ? xa.mod_offset - a.h.merc_blerc_offset : 0;
    uint32_t rb = xb.mod_offset ? xb.mod_offset - b.h.merc_blerc_offset : 0;
    same("merc blend mod offset", &ra, 4, &rb, 4);
    xa.mod_offset = xb.mod_offset = 0;
    memcpy(ba.data(), &xa, sizeof(xa));
    memcpy(bb.data(), &xb, sizeof(xb));
    same_v("merc blend", ba, bb);
  }
  // extension
  ExtHeader ea = a.eh, eb = b.eh;
  same("ext size", &ea.size, 4, &eb.size, 4);
  same_v("lod groups", a.arr<TieLodGroup>(ea.lod_groups_offset, ea.lod_group_count),
         b.arr<TieLodGroup>(eb.lod_groups_offset, eb.lod_group_count));
  same_v("run lods", a.arr<RunLod>(ea.run_lod_offset, ea.run_lod_count),
         b.arr<RunLod>(eb.run_lod_offset, eb.run_lod_count));
  const uint32_t ra = ea.runs_offset ? 1 : 0, rb = eb.runs_offset ? 1 : 0;
  same("runs offset set", &ra, 4, &rb, 4);
  same_v("lod error",
         a.arr<float>(ea.lod_error_offset, ea.lod_error_offset ? ea.lod_group_count : 0),
         b.arr<float>(eb.lod_error_offset, eb.lod_error_offset ? eb.lod_group_count : 0));
  same_v("run spheres",
         a.arr<RunSphere>(ea.run_sphere_offset, ea.run_sphere_offset ? ea.run_lod_count : 0),
         b.arr<RunSphere>(eb.run_sphere_offset, eb.run_sphere_offset ? eb.run_lod_count : 0));
  same_v("stream", a.arr<StreamChunk>(ea.stream_offset, ea.stream_count),
         b.arr<StreamChunk>(eb.stream_offset, eb.stream_count));
  same("stream flags", &ea.stream_flags, 4, &eb.stream_flags, 4);
  {
    auto xa = a.sub<ExtProtoVis>(sizeof(ExtHeader)), xb = b.sub<ExtProtoVis>(sizeof(ExtHeader));
    same_v("protos", a.arr<ProtoVis>(xa.protos_offset, xa.proto_count),
           b.arr<ProtoVis>(xb.protos_offset, xb.proto_count));
    same_v("proto runs", a.arr<uint32_t>(xa.proto_runs_offset, xa.proto_run_count),
           b.arr<uint32_t>(xb.proto_runs_offset, xb.proto_run_count));
  }
  {
    auto xa = a.sub<ExtEnvTie>(kExtHeaderV20Size), xb = b.sub<ExtEnvTie>(kExtHeaderV20Size);
    same_v("env draws", a.arr<EnvTieDraw>(xa.draws_offset, xa.draw_count),
           b.arr<EnvTieDraw>(xb.draws_offset, xb.draw_count));
    auto ka = a.arr<EnvTieChunk>(xa.chunks_offset, xa.chunk_count);
    auto kb = b.arr<EnvTieChunk>(xb.chunks_offset, xb.chunk_count);
    for (size_t i = 0; i < ka.size() && i < kb.size(); i++) {
      same("env chunk", &ka[i].chunk, 4, &kb[i].chunk, 4);
      const uint32_t n = ka[i].chunk < ca.size() ? ca[ka[i].chunk].vertex_count * 4 : 0;
      same_v("env normals", a.get(ka[i].normals_offset, n), b.get(kb[i].normals_offset, n));
    }
    same("env chunk count", &xa.chunk_count, 4, &xb.chunk_count, 4);
  }
  {
    auto xa = a.sub<ExtWind>(kExtHeaderV21Size), xb = b.sub<ExtWind>(kExtHeaderV21Size);
    same_v("wind insts", a.arr<WindInst>(xa.insts_offset, xa.inst_count),
           b.arr<WindInst>(xb.insts_offset, xb.inst_count));
    auto ka = a.arr<WindChunk>(xa.chunks_offset, xa.chunk_count);
    auto kb = b.arr<WindChunk>(xb.chunks_offset, xb.chunk_count);
    same("wind chunk count", &xa.chunk_count, 4, &xb.chunk_count, 4);
    for (size_t i = 0; i < ka.size() && i < kb.size(); i++) {
      same("wind chunk", &ka[i].chunk, 4, &kb[i].chunk, 4);
      same("wind slot count", &ka[i].slot_count, 4, &kb[i].slot_count, 4);
      const auto& ch = ca[ka[i].chunk];
      same_v("wind slots", a.get(ka[i].slots_offset, ch.vertex_count),
             b.get(kb[i].slots_offset, ch.vertex_count));
      same_v("wind slot insts", a.get(ka[i].insts_offset, ka[i].slot_count * 2),
             b.get(kb[i].insts_offset, kb[i].slot_count * 2));
      same_v("wind draws", a.get(ka[i].draws_offset, ch.draw_count),
             b.get(kb[i].draws_offset, ch.draw_count));
    }
  }
  {
    auto xa = a.sub<ExtShrubFade>(kExtHeaderV22Size), xb = b.sub<ExtShrubFade>(kExtHeaderV22Size);
    same_v("shrub bands", a.arr<uint32_t>(xa.bands_offset, xa.band_count),
           b.arr<uint32_t>(xb.bands_offset, xb.band_count));
  }
  {
    auto xa = a.sub<ExtTexKey>(kExtHeaderV23Size), xb = b.sub<ExtTexKey>(kExtHeaderV23Size);
    same_v("tex keys", a.arr<uint64_t>(xa.offset, xa.count), b.arr<uint64_t>(xb.offset, xb.count));
  }
  {
    auto xa = a.sub<ExtCullCond>(kExtHeaderTexKeySize);
    auto xb = b.sub<ExtCullCond>(kExtHeaderTexKeySize);
    auto ka = a.arr<CullCondDraw>(xa.draws_offset, xa.draw_count);
    auto kb = b.arr<CullCondDraw>(xb.draws_offset, xb.draw_count);
    same("cond count", &xa.draw_count, 4, &xb.draw_count, 4);
    for (size_t i = 0; i < ka.size() && i < kb.size(); i++) {
      // (the planes, then the boxes: two plane records each)
      same_v("cond planes",
             a.arr<CullCondPlane>(ka[i].planes_offset, ka[i].plane_count + 2 * ka[i].box_count),
             b.arr<CullCondPlane>(kb[i].planes_offset, kb[i].plane_count + 2 * kb[i].box_count));
      ka[i].planes_offset = kb[i].planes_offset = 0;
    }
    same_v("cond draws", ka, kb);
  }
  {
    auto xa = a.sub<ExtMorph>(kExtHeaderV24Size), xb = b.sub<ExtMorph>(kExtHeaderV24Size);
    same_v("morph insts", a.arr<MorphInst>(xa.insts_offset, xa.inst_count),
           b.arr<MorphInst>(xb.insts_offset, xb.inst_count));
    same_v("morph rows", a.arr<uint32_t>(xa.rows_offset, xa.row_count),
           b.arr<uint32_t>(xb.rows_offset, xb.row_count));
    same_v("morph draw rows", a.get(xa.draw_rows_offset, xa.draw_rows_size),
           b.get(xb.draw_rows_offset, xb.draw_rows_size));
    same_v("group mid",
           a.arr<float>(xa.group_mid_offset, xa.group_mid_offset ? ea.lod_group_count : 0),
           b.arr<float>(xb.group_mid_offset, xb.group_mid_offset ? eb.lod_group_count : 0));
    auto ka = a.arr<MorphChunk>(xa.chunks_offset, xa.chunk_count);
    auto kb = b.arr<MorphChunk>(xb.chunks_offset, xb.chunk_count);
    for (size_t i = 0; i < ka.size() && i < kb.size(); i++) {
      same_v("morph records", a.arr<MorphVertex>(ka[i].records_offset, ka[i].vertex_count),
             b.arr<MorphVertex>(kb[i].records_offset, kb[i].vertex_count));
      ka[i].records_offset = kb[i].records_offset = 0;
    }
    same_v("morph chunks", ka, kb);
  }
  {
    auto xa = a.sub<ExtTfrag>(kExtHeaderMorphSize), xb = b.sub<ExtTfrag>(kExtHeaderMorphSize);
    same("tfrag dists", xa.dists, sizeof(xa.dists), xb.dists, sizeof(xb.dists));
    same_v("tfrag groups", a.arr<TfragGroup>(xa.groups_offset, xa.group_count),
           b.arr<TfragGroup>(xb.groups_offset, xb.group_count));
    same_v("tfrag run lods", a.arr<RunLod>(xa.run_lod_offset, xa.run_lod_count),
           b.arr<RunLod>(xb.run_lod_offset, xb.run_lod_count));
    same_v("tfrag colors", a.arr<TfragMorphColor>(xa.colors_offset, xa.color_count),
           b.arr<TfragMorphColor>(xb.colors_offset, xb.color_count));
    auto ka = a.arr<TfragChunk>(xa.chunks_offset, xa.chunk_count);
    auto kb = b.arr<TfragChunk>(xb.chunks_offset, xb.chunk_count);
    for (size_t i = 0; i < ka.size() && i < kb.size(); i++) {
      const uint32_t na = ka[i].records_offset && ka[i].chunk < ca.size()
                              ? ca[ka[i].chunk].vertex_count
                              : 0;
      const uint32_t nb = kb[i].records_offset && kb[i].chunk < cb.size()
                              ? cb[kb[i].chunk].vertex_count
                              : 0;
      same_v("tfrag morph records", a.arr<TfragMorphVertex>(ka[i].records_offset, na),
             b.arr<TfragMorphVertex>(kb[i].records_offset, nb));
      const uint32_t sa = ka[i].records_offset ? 1 : 0, sb = kb[i].records_offset ? 1 : 0;
      same("tfrag records set", &sa, 4, &sb, 4);
      ka[i].records_offset = kb[i].records_offset = 0;
    }
    same_v("tfrag chunks", ka, kb);
  }
  if (g_diff) {
    lg::error("{} vs {}: {} differences", pa, pb, g_diff);
  } else {
    lg::info("{} vs {}: the same ({} chunk vertices)", pa, pb, verts);
  }
  return g_diff == 0;
}
}  // namespace

bool compare(const std::string& a, const std::string& b) {
  namespace fs = std::filesystem;
  if (!fs::is_directory(a)) {
    return compare_file(a, b);
  }
  bool ok = true;
  size_t n = 0;
  for (const auto& e : fs::directory_iterator(a)) {
    if (e.path().extension() == ".c3l") {
      ok = compare_file(e.path().string(), (fs::path(b) / e.path().filename()).string()) && ok;
      n++;
    }
  }
  lg::info("compare: {} files, {}", n, ok ? "all the same" : "some differ");
  return ok;
}

}  // namespace c3l_compare
