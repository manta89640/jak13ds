#pragma once

// (AI-assisted) CPU-only visibility and freshness tracking for time-of-day light rewrites.
#include <algorithm>
#include <utility>
#include <vector>

#include "common/common_types.h"

struct CtrLightGroup {
  u16 lo, hi;  // vertices [lo, hi]
  u32 ver : 31;
  u32 plain : 1;  // every vertex has a time-of-day color
  u32 dver : 24;
  u32 bright : 8;  // the palettes its vertices' colors are bright in (Tod::color_bright)
  u32 lver = 0;  // light palettes last applied to this group (hidden groups stay stale)
};

// A chunk's vertices with a light color. verts: per mask (the light palettes of their color; masks:
// each one's end) in vertex order, then (once its groups are known) each group's part by table
// entry; runs: per group and mask with vertices the version they were all last written at (0: not
// since the layout), then their entries, entry | count << 16; ranges: per group its lo | hi << 16,
// its light vertices' lowest | highest << 16, then per mask its verts first, runs first and runs
// end.
struct CtrLightLists {
  std::vector<u16> verts;  // (kPad more at the end: read ahead by the writes, never written)
  std::vector<std::pair<u8, u32>> masks;
  std::vector<u32> runs;
  std::vector<u32> ranges;
  static constexpr u32 kPad = 8;
  bool empty() const { return verts.size() <= kPad; }
  u32 count() const { return empty() ? 0 : (u32)verts.size() - kPad; }
};

// The parts and runs of every group made again from the vertex order (when a group's lo / hi
// changed). entry(vertex): its table entry.
template <typename Entry>
void ctr_light_layout(const CtrLightGroup* groups, u32 count, CtrLightLists& L, Entry entry) {
  const u32 nm = (u32)L.masks.size(), stride = 2 + 3 * nm;
  L.ranges.assign((size_t)count * stride, 0);
  L.runs.clear();
  std::vector<std::pair<u32, u32>> parts;  // per group and mask [first, last)
  parts.reserve((size_t)count * nm);
  u32 begin = 0;
  for (u32 k = 0; k < nm; k++) {
    const u32 end = L.masks[k].second;
    if (end > begin) {
      std::sort(L.verts.begin() + begin, L.verts.begin() + end);
    }
    begin = std::max(begin, end);
  }
  for (u32 g = 0; g < count; g++) {
    begin = 0;
    for (u32 k = 0; k < nm; k++) {
      const u32 end = L.masks[k].second;
      u32 f = begin, l = begin;
      if (end > begin) {
        f = (u32)(std::lower_bound(L.verts.begin() + begin, L.verts.begin() + end, groups[g].lo) -
                  L.verts.begin());
        l = (u32)(std::upper_bound(L.verts.begin() + f, L.verts.begin() + end, groups[g].hi) -
                  L.verts.begin());
      }
      parts.emplace_back(f, l);
      begin = std::max(begin, end);
    }
  }
  // (groups whose vertices overlap keep the vertex order: no vertex leaves its group's part)
  std::vector<std::pair<u32, u32>> sorted;
  for (const auto& p : parts) {
    if (p.second > p.first) {
      sorted.push_back(p);
    }
  }
  std::sort(sorted.begin(), sorted.end());
  bool apart = true;
  for (size_t i = 1; i < sorted.size(); i++) {
    apart = apart && sorted[i].first >= sorted[i - 1].second;
  }
  std::vector<u32> keyed;
  for (u32 g = 0; g < count; g++) {
    u32* const rg = L.ranges.data() + (size_t)g * stride;
    rg[0] = (u32)groups[g].lo | (u32)groups[g].hi << 16;
    u32 lowest = 0xffff, highest = 0;
    for (u32 k = 0; k < nm; k++) {
      const auto [f, l] = parts[(size_t)g * nm + k];
      const u32 rf = (u32)L.runs.size();
      if (l > f) {
        lowest = std::min<u32>(lowest, L.verts[f]);
        highest = std::max<u32>(highest, L.verts[l - 1]);
        L.runs.push_back(0);
        keyed.clear();
        for (u32 i = f; i < l; i++) {
          keyed.push_back((apart ? (u32)entry(L.verts[i]) << 16 : 0) | L.verts[i]);
        }
        if (apart) {
          std::sort(keyed.begin(), keyed.end());
        }
        for (u32 i = 0; i < l - f; i++) {
          const u32 v = keyed[i] & 0xffff, e = entry(v);
          L.verts[f + i] = (u16)v;
          if (L.runs.size() > rf + 1 && (L.runs.back() & 0xffff) == e &&
              (L.runs.back() >> 16) < 0xffff) {
            L.runs.back() += 1u << 16;
          } else {
            L.runs.push_back(e | 1u << 16);
          }
        }
      }
      rg[2 + 3 * k] = f;
      rg[3 + 3 * k] = rf;
      rg[4 + 3 * k] = (u32)L.runs.size();
    }
    rg[1] = lowest | highest << 16;
  }
  L.runs.shrink_to_fit();
}

// Write only the changed light vertices of shown groups. A full day rewrite also sets lver,
// so running this after the day pass never writes the same group twice.
// write(group, verts first, runs first, runs last, lowest, highest, written): one group's light
// vertices of one mask (lowest, highest: of all of the group's; written: the version they were all
// last written at, set by write).
// entry(vertex): its table entry (for the layout).
template <typename Entry, typename Write>
u32 ctr_recolor_light_groups_cached(CtrLightGroup* groups,
                                    u32 count,
                                    const u8* shown,
                                    const u16* run_groups,
                                    u32 runs,
                                    u32 version,
                                    u32 light_last,
                                    u32 light_mask,
                                    const u32* palette_versions,
                                    CtrLightLists& L,
                                    std::vector<u8>& visible,
                                    Entry entry,
                                    Write write) {
  if (shown) {
    visible.assign(count, 0);
    for (u32 r = 0; r < runs; r++) {
      if (shown[r] && run_groups[r] < count) {
        visible[run_groups[r]] = 1;
      }
    }
  }
  const u32 nm = (u32)L.masks.size(), stride = 2 + 3 * nm;
  bool layout = L.ranges.size() != (size_t)count * stride;
  for (u32 g = 0; g < count && !layout; g++) {
    layout = L.ranges[(size_t)g * stride] != ((u32)groups[g].lo | (u32)groups[g].hi << 16);
  }
  if (layout) {
    ctr_light_layout(groups, count, L, entry);
  }
  u32 minimum = version;
  for (u32 g = 0; g < count; g++) {
    auto& group = groups[g];
    if ((!shown || visible[g]) && group.lver < light_last) {
      u32 moved = 0;
      for (u32 m = light_mask & 0xff; m; m &= m - 1) {
        const int p = __builtin_ctz(m);
        moved |= palette_versions[p] > group.lver ? 1u << p : 0;
      }
      u32* const rg = L.ranges.data() + (size_t)g * stride;
      for (u32 k = 0; k < nm; k++) {
        const u32 rf = rg[3 + 3 * k], rl = rg[4 + 3 * k];
        if ((L.masks[k].first & moved) && rl != rf) {
          write((const CtrLightGroup&)group, rg[2 + 3 * k], rf + 1, rl, rg[1] & 0xffff, rg[1] >> 16,
                L.runs[rf]);
        }
      }
      group.lver = version;
    }
    minimum = std::min(minimum, group.lver);
  }
  return minimum;
}
