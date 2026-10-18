/*!
 * @file tie_lod.cpp
 * (AI-assisted) c3l v17 tie levels of detail: instance groups and their shape error (tie_lod.h).
 */

#include "tie_lod.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace tie_lod {

namespace {

struct TiePs2Dist {
  const char* level;
  const char* proto;
  float near, mid, far;
};
const TiePs2Dist kTiePs2Dists[] = {
#include "tie_ps2_dists.inc"
#include "tie_ps2_dists_jak2.inc"  // (AI-assisted) level names "jak2/<level>"
};

// strips with UINT32_MAX restarts -> triangles (corner order doesn't matter here)
void strips_to_tris(const u32* idx,
                    size_t count,
                    const std::vector<tfrag3::PreloadedVertex>& verts,
                    const std::array<math::Vector4f, 4>* mat,
                    std::vector<Tri>* out,
                    std::vector<std::array<float, 5>>* corners) {
  auto pos = [&](u32 vi, float* p) {
    const auto& v = verts.at(vi);
    if (mat) {
      const auto& m = *mat;
      for (int c = 0; c < 3; c++) {
        p[c] = m[0][c] * v.x + m[1][c] * v.y + m[2][c] * v.z + m[3][c];
      }
    } else {
      p[0] = v.x;
      p[1] = v.y;
      p[2] = v.z;
    }
  };
  size_t run = 0;
  for (size_t i = 0; i < count; i++) {
    if (idx[i] == UINT32_MAX) {
      run = 0;
      continue;
    }
    run++;
    if (run >= 3) {
      const u32 a = idx[i - 2], b = idx[i - 1], c = idx[i];
      if (a == b || b == c || a == c) {
        continue;
      }
      Tri t;
      pos(a, &t[0]);
      pos(b, &t[3]);
      pos(c, &t[6]);
      out->push_back(t);
      const u32 vi[3] = {a, b, c};
      for (int k = 0; k < 3; k++) {
        const auto& v = verts.at(vi[k]);
        corners->push_back({t[3 * k], t[3 * k + 1], t[3 * k + 2], v.s, v.t});
      }
    }
  }
}

// squared distance from p to triangle t (Ericson, Real-Time Collision Detection 5.1.5)
float dist2_point_tri(const float* p, const Tri& t) {
  const float* a = &t[0];
  const float* b = &t[3];
  const float* c = &t[6];
  auto sub = [](const float* x, const float* y, float* o) {
    o[0] = x[0] - y[0];
    o[1] = x[1] - y[1];
    o[2] = x[2] - y[2];
  };
  auto dot = [](const float* x, const float* y) { return x[0] * y[0] + x[1] * y[1] + x[2] * y[2]; };
  float ab[3], ac[3], ap[3];
  sub(b, a, ab);
  sub(c, a, ac);
  sub(p, a, ap);
  float q[3];
  auto at = [&](float u, float v) {  // a + u * ab + v * ac
    for (int k = 0; k < 3; k++) {
      q[k] = a[k] + u * ab[k] + v * ac[k];
    }
  };
  const float d1 = dot(ab, ap), d2 = dot(ac, ap);
  if (d1 <= 0 && d2 <= 0) {
    at(0, 0);
  } else {
    float bp[3];
    sub(p, b, bp);
    const float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) {
      at(1, 0);
    } else {
      const float vc = d1 * d4 - d3 * d2;
      if (vc <= 0 && d1 >= 0 && d3 <= 0) {
        at(d1 / (d1 - d3), 0);
      } else {
        float cp[3];
        sub(p, c, cp);
        const float d5 = dot(ab, cp), d6 = dot(ac, cp);
        if (d6 >= 0 && d5 <= d6) {
          at(0, 1);
        } else {
          const float vb = d5 * d2 - d1 * d6;
          if (vb <= 0 && d2 >= 0 && d6 <= 0) {
            at(0, d2 / (d2 - d6));
          } else {
            const float va = d3 * d6 - d5 * d4;
            if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
              const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
              // b + w * (c - b)
              for (int k = 0; k < 3; k++) {
                q[k] = b[k] + w * (c[k] - b[k]);
              }
            } else {
              const float denom = va + vb + vc;
              if (std::abs(denom) < 1e-20f) {
                at(0, 0);  // degenerate
              } else {
                at(vb / denom, vc / denom);
              }
            }
          }
        }
      }
    }
  }
  float d[3];
  sub(p, q, d);
  return dot(d, d);
}

struct Box {
  float mn[3], mx[3];
};

Box tri_box(const Tri& t) {
  Box b;
  for (int c = 0; c < 3; c++) {
    b.mn[c] = std::min({t[c], t[3 + c], t[6 + c]});
    b.mx[c] = std::max({t[c], t[3 + c], t[6 + c]});
  }
  return b;
}

float box_dist2(const float* p, const Box& b) {
  float d2 = 0;
  for (int c = 0; c < 3; c++) {
    const float o = p[c] < b.mn[c] ? b.mn[c] - p[c] : (p[c] > b.mx[c] ? p[c] - b.mx[c] : 0.f);
    d2 += o * o;
  }
  return d2;
}

// one-sided Hausdorff distance (squared) from the samples of a to the surface of b, given that the
// answer is at least floor2 (early outs: a sample within that of b can't raise it)
float one_sided2(const std::vector<Tri>& a, const std::vector<Tri>& b, float floor2) {
  std::vector<Box> boxes(b.size());
  for (size_t i = 0; i < b.size(); i++) {
    boxes[i] = tri_box(b[i]);
  }
  float worst = floor2;
  size_t hint = 0;  // the triangle that was nearest to the last sample (neighbours are close)
  auto sample = [&](const float* p) {
    // nearest first guess
    float best = b.empty() ? 1e30f : dist2_point_tri(p, b[hint]);
    if (best <= worst) {
      return;
    }
    for (size_t i = 0; i < b.size(); i++) {
      if (box_dist2(p, boxes[i]) >= best) {
        continue;
      }
      const float d = dist2_point_tri(p, b[i]);
      if (d < best) {
        best = d;
        hint = i;
        if (best <= worst) {
          return;
        }
      }
    }
    worst = std::max(worst, best);
  };
  for (const auto& t : a) {
    sample(&t[0]);
    sample(&t[3]);
    sample(&t[6]);
    const float cen[3] = {(t[0] + t[3] + t[6]) / 3, (t[1] + t[4] + t[7]) / 3,
                          (t[2] + t[5] + t[8]) / 3};
    sample(cen);
  }
  return worst;
}

}  // namespace

void collect(const tfrag3::TieTree& tree,
             u32 tree_idx,
             int slot,
             std::unordered_map<u64, Group>* out) {
  for (const auto& d : tree.static_draws) {
    size_t idx = d.unpacked.idx_of_first_idx_in_full_buffer;
    for (const auto& g : d.vis_groups) {
      auto& grp = (*out)[static_key(tree_idx, g.vis_idx_in_pc_bvh, g.tie_proto_idx)];
      grp.proto = g.tie_proto_idx;
      const size_t n = std::min<size_t>(g.num_inds, tree.unpacked.indices.size() - idx);
      strips_to_tris(tree.unpacked.indices.data() + idx, n, tree.unpacked.vertices, nullptr,
                     &grp.tris[slot], &grp.corners[slot]);
      idx += g.num_inds;
    }
  }
  for (const auto& d : tree.instanced_wind_draws) {
    size_t idx = 0;
    for (const auto& g : d.instance_groups) {
      if (g.instance_idx < tree.wind_instance_info.size()) {
        auto& grp = (*out)[wind_key(tree_idx, g.instance_idx)];
        const size_t n = std::min<size_t>(g.num, d.vertex_index_stream.size() - idx);
        strips_to_tris(d.vertex_index_stream.data() + idx, n, tree.unpacked.vertices,
                       &tree.wind_instance_info[g.instance_idx].matrix, &grp.tris[slot],
                       &grp.corners[slot]);
      }
      idx += g.num;
    }
  }
}

std::array<float, 4> bounds(const Group& g) {
  float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
  bool any = false;
  for (const auto& v : g.tris) {
    for (const auto& t : v) {
      for (int k = 0; k < 3; k++) {
        for (int c = 0; c < 3; c++) {
          mn[c] = std::min(mn[c], t[3 * k + c]);
          mx[c] = std::max(mx[c], t[3 * k + c]);
        }
      }
      any = true;
    }
  }
  if (!any) {
    return {0, 0, 0, 0};
  }
  std::array<float, 4> s;
  for (int c = 0; c < 3; c++) {
    s[c] = 0.5f * (mn[c] + mx[c]);
  }
  const float dx = mx[0] - mn[0], dy = mx[1] - mn[1], dz = mx[2] - mn[2];
  s[3] = 0.5f * std::sqrt(dx * dx + dy * dy + dz * dz);
  return s;
}

float shape_error(const Group& g) {
  if (g.tris[0].empty()) {
    return 0.f;
  }
  if (g.tris[1].empty()) {
    Group near_only;
    near_only.tris[0] = g.tris[0];
    return bounds(near_only)[3];
  }
  float e2 = one_sided2(g.tris[0], g.tris[1], 0.f);
  e2 = one_sided2(g.tris[1], g.tris[0], e2);
  return std::sqrt(e2);
}

float closest_point_bary(const float* p, const Tri& t, float* w) {
  // Ericson, Real-Time Collision Detection 5.1.5, as dist2_point_tri, keeping the weights
  const float* a = &t[0];
  const float* b = &t[3];
  const float* c = &t[6];
  float ab[3], ac[3], ap[3], bp[3], cp[3];
  for (int k = 0; k < 3; k++) {
    ab[k] = b[k] - a[k];
    ac[k] = c[k] - a[k];
    ap[k] = p[k] - a[k];
    bp[k] = p[k] - b[k];
    cp[k] = p[k] - c[k];
  }
  auto dot = [](const float* x, const float* y) { return x[0] * y[0] + x[1] * y[1] + x[2] * y[2]; };
  const float d1 = dot(ab, ap), d2 = dot(ac, ap), d3 = dot(ab, bp), d4 = dot(ac, bp),
              d5 = dot(ab, cp), d6 = dot(ac, cp);
  const float vc = d1 * d4 - d3 * d2, vb = d5 * d2 - d1 * d6, va = d3 * d6 - d5 * d4;
  float u = 0, v = 0;  // the point: a + u * ab + v * ac
  if (d1 <= 0 && d2 <= 0) {
    // a
  } else if (d3 >= 0 && d4 <= d3) {
    u = 1;
  } else if (vc <= 0 && d1 >= 0 && d3 <= 0) {
    u = d1 / (d1 - d3);
  } else if (d6 >= 0 && d5 <= d6) {
    v = 1;
  } else if (vb <= 0 && d2 >= 0 && d6 <= 0) {
    v = d2 / (d2 - d6);
  } else if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
    v = (d4 - d3) / ((d4 - d3) + (d5 - d6));
    u = 1 - v;
  } else {
    const float den = va + vb + vc;
    if (std::abs(den) > 1e-20f) {
      u = vb / den;
      v = vc / den;
    }
  }
  if (!std::isfinite(u) || !std::isfinite(v)) {
    u = v = 0;  // (a degenerate triangle: its first corner)
  }
  w[0] = 1 - u - v;
  w[1] = u;
  w[2] = v;
  float d2s = 0;
  for (int k = 0; k < 3; k++) {
    const float q = a[k] + u * ab[k] + v * ac[k] - p[k];
    d2s += q * q;
  }
  return d2s;
}

bool ps2_dists(const std::string& level, const std::string& proto, float* mid, float* far) {
  for (const auto& e : kTiePs2Dists) {
    if (level == e.level && proto == e.proto) {
      *mid = e.mid;
      *far = e.far;
      return true;
    }
  }
  return false;
}

}  // namespace tie_lod
