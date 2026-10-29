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

// Write only the changed light vertices of shown groups. A full day rewrite also sets lver,
// so running this after the day pass never writes the same group twice. ranges: per group its
// lo | hi << 16, then per mask the [first, last) of its vertices (made when lo / hi differ).
template <typename Write>
u32 ctr_recolor_light_groups_cached(CtrLightGroup* groups,
                                    u32 count,
                                    const u8* shown,
                                    const u16* run_groups,
                                    u32 runs,
                                    u32 version,
                                    u32 light_last,
                                    u32 light_mask,
                                    const u32* palette_versions,
                                    const std::vector<u16>& vertices,
                                    const std::vector<std::pair<u8, u32>>& masks,
                                    std::vector<u8>& visible,
                                    std::vector<u32>& ranges,
                                    Write write) {
  if (shown) {
    visible.assign(count, 0);
    for (u32 r = 0; r < runs; r++) {
      if (shown[r] && run_groups[r] < count) {
        visible[run_groups[r]] = 1;
      }
    }
  }
  const u32 nm = (u32)masks.size(), stride = 1 + 2 * nm;
  if (ranges.size() != (size_t)count * stride) {
    ranges.assign((size_t)count * stride, 0);
    for (u32 g = 0; g < count; g++) {
      ranges[(size_t)g * stride] = 1;  // (lo 1, hi 0: made at the first use)
    }
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
      u32* const rg = ranges.data() + (size_t)g * stride;
      const u32 key = (u32)group.lo | (u32)group.hi << 16;
      if (rg[0] != key) {
        // Each mask's vertices are in ascending order: the group's are one range of them
        rg[0] = key;
        u32 begin = 0;
        for (u32 k = 0; k < nm; k++) {
          const u32 end = masks[k].second;
          u32 f = begin, l = begin;
          if (end > begin) {
            f = (u32)(std::lower_bound(vertices.begin() + begin, vertices.begin() + end, group.lo) -
                      vertices.begin());
            l = (u32)(std::upper_bound(vertices.begin() + f, vertices.begin() + end, group.hi) -
                      vertices.begin());
          }
          rg[1 + 2 * k] = f;
          rg[2 + 2 * k] = l;
          begin = std::max(begin, end);
        }
      }
      for (u32 k = 0; k < nm; k++) {
        const u32 f = rg[1 + 2 * k], l = rg[2 + 2 * k];
        if ((masks[k].first & moved) && f != l) {
          write(vertices.begin() + f, vertices.begin() + l);
        }
      }
      group.lver = version;
    }
    minimum = std::min(minimum, group.lver);
  }
  return minimum;
}

template <typename Write>
u32 ctr_recolor_light_groups(CtrLightGroup* groups,
                             u32 count,
                             const u8* shown,
                             const u16* run_groups,
                             u32 runs,
                             u32 version,
                             u32 light_last,
                             u32 light_mask,
                             const u32* palette_versions,
                             const std::vector<u16>& vertices,
                             const std::vector<std::pair<u8, u32>>& masks,
                             std::vector<u8>& visible,
                             Write write) {
  std::vector<u32> ranges;
  return ctr_recolor_light_groups_cached(groups, count, shown, run_groups, runs, version,
                                         light_last, light_mask, palette_versions, vertices, masks,
                                         visible, ranges, write);
}
