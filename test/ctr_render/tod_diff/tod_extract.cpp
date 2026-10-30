// (AI-assisted) tod_diff: a c3l level's time of day colors, TodMuls and per chunk its vertices and
// visibility groups (as CtrLevel.cpp tod_make_groups makes them) into one file for tod_diff.
//   tod_extract <level.c3l> <out.tod>
#include <algorithm>
#include <cstdio>
#include <vector>

#include "game/graphics/ctr/c3l_reader.h"

using namespace c3l;
namespace {
void put(FILE* o, const void* p, size_t n) {
  fwrite(p, 1, n, o);
}
void put32(FILE* o, uint32_t v) {
  put(o, &v, 4);
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 3) {
    fprintf(stderr, "usage: tod_extract <level.c3l> <out.tod>\n");
    return 2;
  }
  Reader f(argv[1]);
  Header h{};
  if (!f.ok() || !f.read(0, &h, sizeof(h)) || h.version < kTodAlphaVersion || !h.tod_offset) {
    fprintf(stderr, "%s: no c3l v22 time of day table\n", argv[1]);
    return 1;
  }
  std::vector<TodColorA> cols;
  std::vector<TodMul> muls;
  std::vector<Chunk> ch;
  std::vector<Draw> draws;
  f.read_array(h.tod_offset, h.tod_color_count, &cols);
  f.read_array(h.tod_offset + h.tod_color_count * sizeof(TodColorA), h.tod_mul_count, &muls);
  f.read_array(h.chunks_offset, h.num_chunks, &ch);
  f.read_array(h.draw_data_offset, h.draw_data_size / sizeof(Draw), &draws);
  FILE* o = fopen(argv[2], "wb");
  if (!o) {
    return 1;
  }
  put32(o, h.tod_color_count);
  put32(o, h.tod_mul_count);
  put(o, cols.data(), cols.size() * sizeof(TodColorA));
  put(o, muls.data(), muls.size() * sizeof(TodMul));
  put32(o, h.num_chunks);
  uint64_t verts = 0, groups = 0;
  for (const auto& c : ch) {
    std::vector<Vertex> v;
    f.read_array(h.vertex_data_offset + c.first_vertex * sizeof(Vertex), c.vertex_count, &v);
    std::vector<VisRun> runs;
    if (c.vis_run_count) {
      f.read_array(c.vis_runs_offset, c.vis_run_count, &runs);
    }
    struct G {
      uint16_t lo, hi;
    };
    std::vector<std::pair<uint16_t, uint16_t>> ids;
    std::vector<G> gs;
    bool one = !c.vis_run_count || c.vertex_count < 2;
    size_t r = 0;
    for (uint32_t k = 0; k < c.draw_count && !one; k++) {
      uint32_t idx = draws[c.first_draw + k].first_index;
      for (; r < runs.size() && runs[r].draw == k; r++) {
        std::vector<uint16_t> ind;
        f.read_array(h.index_data_offset + idx * 2, runs[r].index_count, &ind);
        uint16_t g = 0xffff;
        for (const auto& [id, gi] : ids) {
          if (id == runs[r].vis) {
            g = gi;
            break;
          }
        }
        if (g == 0xffff) {
          g = (uint16_t)gs.size();
          ids.emplace_back(runs[r].vis, g);
          gs.push_back({0xffff, 0});
        }
        for (auto x : ind) {
          gs[g].lo = std::min(gs[g].lo, x);
          gs[g].hi = std::max(gs[g].hi, x);
        }
        idx += runs[r].index_count;
      }
    }
    one = one || gs.size() < 2;
    std::sort(gs.begin(), gs.end(), [](G a, G b) { return a.lo < b.lo; });
    for (size_t k = 1; k < gs.size() && !one; k++) {
      one = gs[k].lo <= gs[k - 1].hi;
    }
    if (one) {
      gs.assign(1, G{0, (uint16_t)(std::max<uint32_t>(c.vertex_count, 1) - 1)});
    }
    put32(o, c.vertex_count);
    put32(o, c.lod_tier);
    put32(o, c.kind);
    put(o, c.bsphere, 16);
    put(o, v.data(), v.size() * sizeof(Vertex));
    put32(o, (uint32_t)gs.size());
    for (const auto& g : gs) {
      put(o, &g.lo, 2);
      put(o, &g.hi, 2);
    }
    // its quantization and its draws' indices (tod_diff pixels)
    put(o, c.origin, 12);
    put(o, &c.scale, 4);
    uint32_t i0 = UINT32_MAX, i1 = 0;
    for (uint32_t k = 0; k < c.draw_count; k++) {
      const auto& d = draws[c.first_draw + k];
      i0 = std::min(i0, d.first_index);
      i1 = std::max(i1, d.first_index + d.index_count);
    }
    std::vector<uint16_t> ind;
    if (i1 > i0) {
      f.read_array(h.index_data_offset + i0 * 2, i1 - i0, &ind);
    }
    put32(o, (uint32_t)ind.size());
    put(o, ind.data(), ind.size() * 2);
    verts += c.vertex_count;
    groups += gs.size();
  }
  fclose(o);
  printf("%s: %u colors, %u TodMuls, %u chunks, %llu vertices, %llu groups\n", argv[1],
         h.tod_color_count, h.tod_mul_count, h.num_chunks, (unsigned long long)verts,
         (unsigned long long)groups);
  return 0;
}
