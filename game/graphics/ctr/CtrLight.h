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
  u32 dver;
  u32 lver = 0;  // light palettes last applied to this group (hidden groups stay stale)
};

// Write only the changed light vertices of shown groups. A full day rewrite also sets lver,
// so running this after the day pass never writes the same group twice.
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
  if (shown) {
    visible.assign(count, 0);
    for (u32 r = 0; r < runs; r++) {
      if (shown[r] && run_groups[r] < count) {
        visible[run_groups[r]] = 1;
      }
    }
  }
  u32 minimum = version;
  for (u32 g = 0; g < count; g++) {
    auto& group = groups[g];
    if ((!shown || visible[g]) && group.lver < light_last) {
      u32 moved = 0;
      for (int p = 0; p < 8; p++) {
        if ((light_mask & (1u << p)) && palette_versions[p] > group.lver) {
          moved |= 1u << p;
        }
      }
      u32 begin = 0;
      for (const auto& [mask, end] : masks) {
        if ((mask & moved) && end > begin) {
          // Each mask's vertices are in ascending order. Skip hidden ranges without reading
          // their vertices or touching their cache lines.
          auto first = std::lower_bound(vertices.begin() + begin, vertices.begin() + end, group.lo);
          auto last = std::upper_bound(first, vertices.begin() + end, group.hi);
          if (first != last) {
            write(first, last);
          }
        }
        begin = end;
      }
      group.lver = version;
    }
    minimum = std::min(minimum, group.lver);
  }
  return minimum;
}
