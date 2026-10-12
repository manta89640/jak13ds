/*!
 * @file c3l_opt.cpp
 * (AI-assisted) See c3l_opt.h.
 */

#include "c3l_opt.h"

#include "ctr_texture.h"
#include "game/graphics/ctr/c3l_format.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace c3l_opt {

namespace {

struct Plane {
  double n[3];
  double d;
  double mn[3], mx[3];
  bool ok;
};

Plane make_plane(const Tri& t) {
  Plane p{};
  const double e1[3] = {t[1][0] - t[0][0], t[1][1] - t[0][1], t[1][2] - t[0][2]};
  const double e2[3] = {t[2][0] - t[0][0], t[2][1] - t[0][1], t[2][2] - t[0][2]};
  double n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                 e1[0] * e2[1] - e1[1] * e2[0]};
  const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
  // less than ~0.5 square units: no area to speak of
  p.ok = len > 1.0;
  for (int c = 0; c < 3; c++) {
    p.n[c] = p.ok ? n[c] / len : 0;
    p.mn[c] = std::min({t[0][c], t[1][c], t[2][c]});
    p.mx[c] = std::max({t[0][c], t[1][c], t[2][c]});
  }
  p.d = p.n[0] * t[0][0] + p.n[1] * t[0][1] + p.n[2] * t[0][2];
  return p;
}

double plane_dist(const Plane& p, const std::array<float, 3>& v) {
  return std::abs(p.n[0] * v[0] + p.n[1] * v[1] + p.n[2] * v[2] - p.d);
}

// the two triangles, projected onto plane a's axes, overlap by more than eps (separating axes: the
// edge normals of both)
bool overlap_in_plane(const Plane& a, const Tri& ta, const Tri& tb, double eps) {
  // axes u, v perpendicular to a.n
  double u[3];
  if (std::abs(a.n[0]) < 0.9) {
    u[0] = 0;
    u[1] = a.n[2];
    u[2] = -a.n[1];
  } else {
    u[0] = -a.n[2];
    u[1] = 0;
    u[2] = a.n[0];
  }
  const double ul = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
  for (double& c : u) {
    c /= ul;
  }
  const double v[3] = {a.n[1] * u[2] - a.n[2] * u[1], a.n[2] * u[0] - a.n[0] * u[2],
                       a.n[0] * u[1] - a.n[1] * u[0]};
  double pa[3][2], pb[3][2];
  for (int k = 0; k < 3; k++) {
    pa[k][0] = u[0] * ta[k][0] + u[1] * ta[k][1] + u[2] * ta[k][2];
    pa[k][1] = v[0] * ta[k][0] + v[1] * ta[k][1] + v[2] * ta[k][2];
    pb[k][0] = u[0] * tb[k][0] + u[1] * tb[k][1] + u[2] * tb[k][2];
    pb[k][1] = v[0] * tb[k][0] + v[1] * tb[k][1] + v[2] * tb[k][2];
  }
  auto separated = [&](const double (*p)[2]) {
    for (int e = 0; e < 3; e++) {
      const double ax = -(p[(e + 1) % 3][1] - p[e][1]), ay = p[(e + 1) % 3][0] - p[e][0];
      const double l = std::sqrt(ax * ax + ay * ay);
      if (l <= 0) {
        continue;
      }
      double amin = 1e300, amax = -1e300, bmin = 1e300, bmax = -1e300;
      for (int k = 0; k < 3; k++) {
        const double sa = (pa[k][0] * ax + pa[k][1] * ay) / l;
        const double sb = (pb[k][0] * ax + pb[k][1] * ay) / l;
        amin = std::min(amin, sa);
        amax = std::max(amax, sa);
        bmin = std::min(bmin, sb);
        bmax = std::max(bmax, sb);
      }
      // the overlap of the two intervals is at most eps: a separating (or touching) axis
      if (std::min(amax, bmax) - std::max(amin, bmin) <= eps) {
        return true;
      }
    }
    return false;
  };
  return !separated(pa) && !separated(pb);
}

}  // namespace

std::vector<std::pair<uint32_t, uint32_t>> coplanar_pairs(const std::vector<Tri>& tris,
                                                          const std::vector<uint32_t>& groups,
                                                          const CoplanarOptions& opt) {
  std::vector<std::pair<uint32_t, uint32_t>> out;
  const size_t n = tris.size();
  std::vector<Plane> planes(n);
  for (size_t i = 0; i < n; i++) {
    planes[i] = make_plane(tris[i]);
  }
  // grids of 4 m, 8 m, 16 m... cells: a triangle goes into the level whose cells are at least its
  // (expanded) size, so it is in at most 8 cells; each triangle looks for partners in its own level
  // and the coarser ones (a pair is found from its smaller triangle, stamps against repeats)
  constexpr double kCell = 4.0 * 4096.0;
  constexpr int kLevels = 24;
  struct Key {
    int64_t x, y, z;
    uint32_t g, level;
    bool operator==(const Key& o) const {
      return x == o.x && y == o.y && z == o.z && g == o.g && level == o.level;
    }
  };
  struct KeyHash {
    size_t operator()(const Key& k) const {
      return (size_t)k.x * 0x9E3779B97F4A7C15ull ^ (size_t)k.y * 0xC2B2AE3D27D4EB4Full ^
             (size_t)k.z * 0x165667B19E3779F9ull ^ (size_t)k.g * 0x27D4EB2F165667C5ull ^
             (size_t)k.level * 0x94D049BB133111EBull;
    }
  };
  std::unordered_map<Key, std::vector<uint32_t>, KeyHash> cells;
  std::vector<uint8_t> level_of(n, 0);
  bool used_level[kLevels] = {};
  auto cell_of = [&](double v, int level) {
    return (int64_t)std::floor(v / (kCell * (double)(1ull << level)));
  };
  for (size_t i = 0; i < n; i++) {
    const auto& p = planes[i];
    if (!p.ok) {
      continue;
    }
    const double ext = std::max({p.mx[0] - p.mn[0], p.mx[1] - p.mn[1], p.mx[2] - p.mn[2]}) +
                       2.0 * opt.dist;
    int level = 0;
    while (level + 1 < kLevels && kCell * (double)(1ull << level) < ext) {
      level++;
    }
    level_of[i] = (uint8_t)level;
    used_level[level] = true;
    const uint32_t g = groups.empty() ? 0 : groups[i];
    for (int64_t x = cell_of(p.mn[0] - opt.dist, level); x <= cell_of(p.mx[0] + opt.dist, level);
         x++) {
      for (int64_t y = cell_of(p.mn[1] - opt.dist, level);
           y <= cell_of(p.mx[1] + opt.dist, level); y++) {
        for (int64_t z = cell_of(p.mn[2] - opt.dist, level);
             z <= cell_of(p.mx[2] + opt.dist, level); z++) {
          cells[Key{x, y, z, g, (uint32_t)level}].push_back((uint32_t)i);
        }
      }
    }
  }
  std::vector<uint32_t> stamp(n, UINT32_MAX);
  for (size_t i = 0; i < n; i++) {
    const Plane& pi = planes[i];
    if (!pi.ok) {
      continue;
    }
    const uint32_t g = groups.empty() ? 0 : groups[i];
    for (int level = level_of[i]; level < kLevels; level++) {
      if (!used_level[level]) {
        continue;
      }
      for (int64_t x = cell_of(pi.mn[0] - opt.dist, level);
           x <= cell_of(pi.mx[0] + opt.dist, level); x++) {
        for (int64_t y = cell_of(pi.mn[1] - opt.dist, level);
             y <= cell_of(pi.mx[1] + opt.dist, level); y++) {
          for (int64_t z = cell_of(pi.mn[2] - opt.dist, level);
               z <= cell_of(pi.mx[2] + opt.dist, level); z++) {
            auto it = cells.find(Key{x, y, z, g, (uint32_t)level});
            if (it == cells.end()) {
              continue;
            }
            for (uint32_t j : it->second) {
              if (j == i || stamp[j] == (uint32_t)i ||
                  (level_of[j] == level_of[i] && j < i)) {
                continue;
              }
              stamp[j] = (uint32_t)i;
              const Plane& pj = planes[j];
              if (std::abs(pi.n[0] * pj.n[0] + pi.n[1] * pj.n[1] + pi.n[2] * pj.n[2]) < opt.cos) {
                continue;
              }
              bool boxes = true;
              for (int c = 0; c < 3 && boxes; c++) {
                boxes = std::max(pi.mn[c], pj.mn[c]) - opt.dist <=
                        std::min(pi.mx[c], pj.mx[c]) + opt.dist;
              }
              if (!boxes) {
                continue;
              }
              bool near = true;
              for (int k = 0; k < 3 && near; k++) {
                near = plane_dist(pi, tris[j][k]) <= opt.dist &&
                       plane_dist(pj, tris[i][k]) <= opt.dist;
              }
              if (near && overlap_in_plane(pi, tris[i], tris[j], opt.overlap)) {
                out.push_back({(uint32_t)std::min<size_t>(i, j), (uint32_t)std::max<size_t>(i, j)});
              }
            }
          }
        }
      }
    }
  }
  std::sort(out.begin(), out.end());
  out.erase(std::unique(out.begin(), out.end()), out.end());
  return out;
}

uint64_t fifo_misses(const uint32_t* indices, size_t count, int fifo) {
  std::vector<uint32_t> cache(std::max(fifo, 1), UINT32_MAX);
  size_t head = 0;
  uint64_t misses = 0;
  for (size_t i = 0; i < count; i++) {
    const uint32_t v = indices[i];
    if (std::find(cache.begin(), cache.end(), v) == cache.end()) {
      misses++;
      cache[head] = v;
      head = (head + 1) % cache.size();
    }
  }
  return misses;
}

std::vector<uint32_t> vcache_order(const std::vector<uint32_t>& in_indices,
                                   uint32_t,
                                   int fifo,
                                   const std::vector<std::vector<uint32_t>>& before) {
  const uint32_t nt = (uint32_t)(in_indices.size() / 3);
  // the vertices numbered 0.. in this piece (a run uses few of its chunk's vertices)
  std::vector<uint32_t> uniq(in_indices);
  std::sort(uniq.begin(), uniq.end());
  uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
  const uint32_t vertex_count = (uint32_t)uniq.size();
  std::vector<uint32_t> indices(in_indices.size());
  for (size_t i = 0; i < indices.size(); i++) {
    indices[i] = (uint32_t)(std::lower_bound(uniq.begin(), uniq.end(), in_indices[i]) - uniq.begin());
  }
  fifo = std::max(fifo, 3);
  // the triangles of each vertex
  std::vector<uint32_t> first(vertex_count + 1, 0), adj(3 * (size_t)nt);
  for (uint32_t i = 0; i < 3 * nt; i++) {
    first[indices[i] + 1]++;
  }
  for (uint32_t v = 0; v < vertex_count; v++) {
    first[v + 1] += first[v];
  }
  {
    std::vector<uint32_t> fill(first.begin(), first.end() - 1);
    for (uint32_t i = 0; i < 3 * nt; i++) {
      adj[fill[indices[i]]++] = i / 3;
    }
  }
  std::vector<uint32_t> valence(vertex_count, 0);
  for (uint32_t i = 0; i < 3 * nt; i++) {
    valence[indices[i]]++;
  }
  // the order constraints: pending[t] predecessors not out yet
  std::vector<uint32_t> pending(nt, 0);
  std::vector<std::vector<uint32_t>> after(before.empty() ? 0 : nt);
  for (uint32_t t = 0; t < nt && t < before.size(); t++) {
    pending[t] = (uint32_t)before[t].size();
    for (uint32_t p : before[t]) {
      after[p].push_back(t);
    }
  }
  std::vector<uint8_t> done(nt, 0);
  // the FIFO: slots, the next slot to replace, each vertex's slot
  std::vector<uint32_t> slot(fifo, UINT32_MAX);
  std::vector<int32_t> where(vertex_count, -1);
  int head = 0;
  // misses of triangle t now (a short simulation: an insert can push out a later corner)
  auto misses = [&](uint32_t t) {
    int m = 0;
    int h = head;
    uint32_t pushed[3];
    int np = 0;
    for (int k = 0; k < 3; k++) {
      const uint32_t v = indices[3 * t + k];
      bool hit = where[v] >= 0;
      if (hit) {
        // pushed out by one of this triangle's own inserts?
        const int s = where[v];
        for (int q = 0; q < np && hit; q++) {
          hit = (int)pushed[q] != s;
        }
      } else {
        for (int q = 0; q < np && !hit; q++) {
          hit = indices[3 * t + q] == v;  // never (no repeated corners)
        }
      }
      if (!hit) {
        m++;
        pushed[np++] = (uint32_t)h;
        h = (h + 1) % fifo;
      }
    }
    return m;
  };
  auto score = [&](uint32_t t) {
    double s = -4.0 * misses(t);
    for (int k = 0; k < 3; k++) {
      const uint32_t v = indices[3 * t + k];
      if (where[v] >= 0) {
        // older entries go out sooner: use them first
        const int age = (where[v] - head + fifo) % fifo;  // 0 = the next one out
        s += 0.75 * (1.0 - (double)age / fifo);
      }
      // vertices with few triangles left: finish them (no lonely vertices later)
      s += 2.0 / std::sqrt((double)valence[v]);
    }
    return s;
  };
  std::vector<uint32_t> order;
  order.reserve(nt);
  uint32_t scan = 0;
  std::vector<uint32_t> seen_mark(nt, UINT32_MAX);
  while (order.size() < nt) {
    int64_t best = -1;
    double best_s = -1e300;
    const uint32_t stamp = (uint32_t)order.size();
    for (int s = 0; s < fifo; s++) {
      const uint32_t v = slot[s];
      if (v == UINT32_MAX) {
        continue;
      }
      for (uint32_t a = first[v]; a < first[v + 1]; a++) {
        const uint32_t t = adj[a];
        if (done[t] || pending[t] || seen_mark[t] == stamp) {
          continue;
        }
        seen_mark[t] = stamp;
        const double sc = score(t);
        if (sc > best_s || (sc == best_s && t < best)) {
          best_s = sc;
          best = t;
        }
      }
    }
    if (best < 0) {
      while (scan < nt && (done[scan] || pending[scan])) {
        scan++;
      }
      if (scan >= nt) {
        // only triangles still waiting for others (never: the earliest one left can always go)
        for (scan = 0; scan < nt && done[scan]; scan++) {
        }
      }
      best = scan;
    }
    const uint32_t t = (uint32_t)best;
    done[t] = 1;
    order.push_back(t);
    for (int k = 0; k < 3; k++) {
      const uint32_t v = indices[3 * t + k];
      valence[v]--;
      if (where[v] < 0) {
        if (slot[head] != UINT32_MAX) {
          where[slot[head]] = -1;
        }
        slot[head] = v;
        where[v] = head;
        head = (head + 1) % fifo;
      }
    }
    if (!after.empty()) {
      for (uint32_t n : after[t]) {
        pending[n]--;
      }
    }
  }
  return order;
}

namespace {
// byte offset of 4x4 block (bx, bym) (bym: block rows from the bottom of the image) of a w wide
// ETC1 / ETC1A4 level (ctr_texture.h: 8x8 tiles of four blocks, rows of tiles bottom first)
size_t block_offset(int bx, int bym, int w, int block_bytes) {
  return ((size_t)((bym >> 1) * (w / 8) + (bx >> 1)) * 4 + (bx & 1) + 2 * (bym & 1)) *
         (size_t)block_bytes;
}
}  // namespace

std::vector<uint8_t> build_atlas(int aw,
                                 int ah,
                                 int format,
                                 int levels,
                                 const std::vector<AtlasCell>& cells,
                                 int etc1_quality,
                                 AtlasStats* stats) {
  const auto fmt = (ctr_tex::Format)format;
  const bool etc = fmt == ctr_tex::Format::ETC1 || fmt == ctr_tex::Format::ETC1A4;
  const int block_bytes = fmt == ctr_tex::Format::ETC1A4 ? 16 : 8;
  std::vector<uint8_t> out;
  for (int l = 0; l < levels; l++) {
    const int lw = aw >> l, lh = ah >> l;
    const int g = (2 << (levels - 1)) >> l;  // gutter texels on each side of a cell
    ctr_tex::Image img;
    img.w = lw;
    img.h = lh;
    img.rgba.assign((size_t)lw * lh * 4, 0);
    // what each gutter texel should be, for the statistics
    std::vector<int> want_cell((size_t)lw * lh, -1);
    std::vector<ctr_tex::Image> dec(cells.size());
    for (size_t ci = 0; ci < cells.size(); ci++) {
      const auto& c = cells[ci];
      const int cw = c.w >> l, ch = c.h >> l, cx = c.x >> l, cy = c.y >> l;
      uint32_t off = 0;
      for (int k = 0; k < l; k++) {
        off += c3l::texture_level_bytes(c.w >> k, c.h >> k, (uint8_t)format);
      }
      dec[ci] = ctr_tex::decode_level(c.data + off, cw, ch, fmt);
      for (int y = -g; y < ch + g; y++) {
        for (int x = -g; x < cw + g; x++) {
          const int ax = cx + x, ay = cy + y;
          if (ax < 0 || ay < 0 || ax >= lw || ay >= lh) {
            continue;
          }
          const int sx = c.clamp_s ? std::clamp(x, 0, cw - 1) : ((x % cw) + cw) % cw;
          const int sy = c.clamp_t ? std::clamp(y, 0, ch - 1) : ((y % ch) + ch) % ch;
          memcpy(img.at(ax, ay), dec[ci].at(sx, sy), 4);
          want_cell[(size_t)ay * lw + ax] = (int)ci;
        }
      }
    }
    std::vector<uint8_t> level;
    ctr_tex::encode_level(img, fmt, etc1_quality, &level, 0, 0);
    if (etc) {
      // the cells' own blocks, as they are
      for (size_t ci = 0; ci < cells.size(); ci++) {
        const auto& c = cells[ci];
        const int cw = c.w >> l, ch = c.h >> l, cx = c.x >> l, cy = c.y >> l;
        uint32_t off = 0;
        for (int k = 0; k < l; k++) {
          off += c3l::texture_level_bytes(c.w >> k, c.h >> k, (uint8_t)format);
        }
        const int base_bym = (lh - cy - ch) / 4;
        for (int bym = 0; bym < ch / 4; bym++) {
          for (int bx = 0; bx < cw / 4; bx++) {
            memcpy(level.data() + block_offset(cx / 4 + bx, base_bym + bym, lw, block_bytes),
                   c.data + off + block_offset(bx, bym, cw, block_bytes), (size_t)block_bytes);
          }
        }
      }
    }
    if (stats) {
      const auto got = ctr_tex::decode_level(level.data(), lw, lh, fmt);
      for (size_t ci = 0; ci < cells.size(); ci++) {
        const auto& c = cells[ci];
        const int cw = c.w >> l, ch = c.h >> l, cx = c.x >> l, cy = c.y >> l;
        for (int y = 0; y < ch; y++) {
          for (int x = 0; x < cw; x++) {
            stats->cells_exact =
                stats->cells_exact && !memcmp(got.at(cx + x, cy + y), dec[ci].at(x, y), 4);
          }
        }
      }
      for (int y = 0; y < lh; y++) {
        for (int x = 0; x < lw; x++) {
          const int ci = want_cell[(size_t)y * lw + x];
          if (ci < 0) {
            continue;
          }
          const auto& c = cells[ci];
          const int cw = c.w >> l, ch = c.h >> l, cx = c.x >> l, cy = c.y >> l;
          if (x >= cx && x < cx + cw && y >= cy && y < cy + ch) {
            continue;
          }
          for (int k = 0; k < 4; k++) {
            const int d = (int)got.at(x, y)[k] - (int)img.at(x, y)[k];
            stats->gutter_sq += (double)d * d;
            stats->gutter_max = std::max(stats->gutter_max, std::abs(d));
          }
          stats->gutter_texels++;
        }
      }
    }
    out.insert(out.end(), level.begin(), level.end());
  }
  return out;
}

}  // namespace c3l_opt
