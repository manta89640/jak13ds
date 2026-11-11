// CPU-only checks for visible light rewrites.
#include "game/graphics/ctr/CtrLight.h"
#include "check.h"

#include <array>
#include <cmath>
#include <cstring>
#include <random>

namespace {
void test_visible_lights() {
  constexpr int ng = 10, nv = 240;
  std::array<CtrLightGroup, ng> groups{};
  for (int g = 0; g < ng; g++) {
    groups[g].lo = (uint16_t)(g * (nv / ng));
    groups[g].hi = (uint16_t)((g + 1) * (nv / ng) - 1);
  }
  std::array<uint8_t, nv> vertex_mask{};
  std::vector<std::pair<uint8_t, uint16_t>> sorted;
  for (int i = 0; i < nv; i++) {
    vertex_mask[i] = (uint8_t)(1 + i % 15);
    sorted.emplace_back(vertex_mask[i], (uint16_t)i);
  }
  std::sort(sorted.begin(), sorted.end());
  CtrLightLists lists;
  for (size_t i = 0; i < sorted.size(); i++) {
    lists.verts.push_back(sorted[i].second);
    if (i + 1 == sorted.size() || sorted[i].first != sorted[i + 1].first) {
      lists.masks.emplace_back(sorted[i].first, (uint32_t)i + 1);
    }
  }
  lists.verts.resize(lists.verts.size() + CtrLightLists::kPad, 0);
  auto entry = [](uint32_t i) { return (uint16_t)(i % 7); };
  // each part's vertices, by its runs (every one in its run's entry)
  auto each = [&](uint32_t first, uint32_t rf, uint32_t rl, auto fn) {
    for (uint32_t r = rf; r < rl; r++) {
      for (uint32_t c = 0; c < lists.runs[r] >> 16; c++, first++) {
        check(entry(lists.verts[first]) == (lists.runs[r] & 0xffff), "a run's vertices use its entry");
        fn(lists.verts[first]);
      }
    }
  };
  uint32_t versions[8] = {};
  std::array<uint32_t, nv> colors{};
  std::vector<uint8_t> scratch;
  std::mt19937 rng(0xC17ADE1);
  for (uint32_t frame = 1; frame <= 1000; frame++) {
    versions[rng() % 4] = frame;
    auto expected = [&](int i) {
      uint32_t value = 0;
      for (int p = 0; p < 4; p++) {
        if (vertex_mask[i] & (1u << p)) {
          value += versions[p] * (uint32_t)(p + 1);
        }
      }
      return value;
    };
    std::array<uint8_t, ng * 2> shown{};
    std::array<uint16_t, ng * 2> run_groups{};
    std::array<bool, ng> visible{};
    for (int r = 0; r < ng * 2; r++) {
      run_groups[r] = (uint16_t)(r / 2);  // two runs per group: no double rewrites
      shown[r] = rng() % 3 == 0;
      visible[r / 2] |= shown[r] != 0;
    }
    // Simulate full day rewrites before the light pass: these vertices need no second write.
    std::array<bool, ng> full{};
    for (int g = 0; g < ng; g++) {
      full[g] = visible[g] && rng() % 3 == 0;
      if (full[g]) {
        for (int i = groups[g].lo; i <= groups[g].hi; i++) {
          colors[i] = expected(i);
        }
        groups[g].lver = frame;
      }
    }
    std::array<uint8_t, nv> writes{};
    const uint32_t minimum = ctr_recolor_light_groups_cached(
        groups.data(), ng, shown.data(), run_groups.data(), (uint32_t)shown.size(), frame, frame,
        15, versions, lists, scratch, entry,
        [&](const CtrLightGroup&, uint32_t first, uint32_t rf, uint32_t rl, uint32_t, uint32_t,
            uint32_t&) {
          each(first, rf, rl, [&](int i) {
            const int g = i / (nv / ng);
            check(visible[g], "light pass must not touch a hidden group");
            check(!full[g], "full day rewrites must not be repeated by the light pass");
            check(++writes[i] == 1, "duplicate runs must not duplicate light writes");
            colors[i] = expected(i);
          });
        });
    uint32_t actual_min = frame;
    for (int g = 0; g < ng; g++) {
      actual_min = std::min(actual_min, groups[g].lver);
      if (visible[g]) {
        for (int i = groups[g].lo; i <= groups[g].hi; i++) {
          check(colors[i] == expected(i), "newly visible light vertices must catch up immediately");
        }
      }
    }
    check(minimum == actual_min, "chunk light version must include hidden groups' old versions");
  }
  // No visibility table/older files: update the entire chunk safely.
  versions[0] = 1001;
  uint32_t touched = 0;
  ctr_recolor_light_groups_cached(
      groups.data(), ng, nullptr, nullptr, 0, 1001, 1001, 15, versions, lists, scratch, entry,
      [&](const CtrLightGroup&, uint32_t first, uint32_t rf, uint32_t rl, uint32_t, uint32_t,
            uint32_t&) {
        each(first, rf, rl, [&](int) { touched++; });
      });
  check(touched > 0, "chunks without visibility groups must still update");
}

}  // namespace

void run_citadel_tests() {
  test_visible_lights();
}
