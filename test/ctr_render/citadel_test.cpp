// (AI-assisted) CPU-only checks for visible light rewrites and perspective-correct sprite trim.
#include "game/graphics/ctr/CtrLight.h"
#include "game/graphics/ctr/CtrSpriteTrim.h"
#include "check.h"

#include <array>
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
  std::vector<uint16_t> vertices;
  std::vector<std::pair<uint8_t, uint32_t>> masks;
  for (size_t i = 0; i < sorted.size(); i++) {
    vertices.push_back(sorted[i].second);
    if (i + 1 == sorted.size() || sorted[i].first != sorted[i + 1].first) {
      masks.emplace_back(sorted[i].first, (uint32_t)i + 1);
    }
  }
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
    const uint32_t minimum = ctr_recolor_light_groups(
        groups.data(), ng, shown.data(), run_groups.data(), (uint32_t)shown.size(),
        frame, frame, 15, versions, vertices, masks, scratch, [&](auto first, auto last) {
          for (; first != last; ++first) {
            const int i = *first, g = i / (nv / ng);
            check(visible[g], "light pass must not touch a hidden group");
            check(!full[g], "full day rewrites must not be repeated by the light pass");
            check(++writes[i] == 1, "duplicate runs must not duplicate light writes");
            colors[i] = expected(i);
          }
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
  ctr_recolor_light_groups(groups.data(), ng, nullptr, nullptr, 0, 1001, 1001, 15,
                           versions, vertices, masks, scratch,
                           [&](auto first, auto last) { touched += (uint32_t)(last - first); });
  check(touched > 0, "chunks without visibility groups must still update");
}

ctr_clip_vertex point(float s, float t) {
  ctr_clip_vertex p{};
  p.x = -2.f + 4.f * s + t;
  p.y = 3.f + s - 5.f * t;
  p.z = 0.5f - s + 0.25f * t;
  p.w = -1.f + 3.f * s + 2.f * t;  // crosses the camera plane
  p.s = s;
  p.t = t;
  return p;
}

void test_clip_trim() {
  std::array<int, 4> order{0, 1, 2, 3};
  const float box[4] = {0.125f, 0.25f, 0.875f, 0.75f};
  do {
    ctr_clip_vertex quad[4]{};
    int select[4][2];
    int k00 = -1, k10 = -1, k01 = -1;
    for (int k = 0; k < 4; k++) {
      const int corner = order[k];
      const float s = (float)(corner & 1), t = (float)(corner >> 1);
      quad[k] = point(s, t);
      quad[k].a = (uint8_t)(20 + k);
      select[k][0] = s ? 2 : 0;
      select[k][1] = t ? 3 : 1;
      if (corner == 0) k00 = k;
      if (corner == 1) k10 = k;
      if (corner == 2) k01 = k;
    }
    check(ctr_trim_clip_quad(quad, k00, k10, k01, select, box, box),
          "affine quad must trim in every UV orientation");
    for (int k = 0; k < 4; k++) {
      const auto expected = point(box[select[k][0]], box[select[k][1]]);
      check(std::abs(quad[k].x - expected.x) < 1e-6f &&
                std::abs(quad[k].y - expected.y) < 1e-6f &&
                std::abs(quad[k].z - expected.z) < 1e-6f &&
                std::abs(quad[k].w - expected.w) < 1e-6f &&
                quad[k].s == expected.s && quad[k].t == expected.t,
            "trim must preserve the original perspective surface and UVs");
      check(quad[k].a == 20 + k, "trim must preserve vertex alpha");
    }
  } while (std::next_permutation(order.begin(), order.end()));

  ctr_clip_vertex quad[4] = {point(0, 0), point(1, 0), point(0, 1), point(1, 1)};
  quad[3].z += 1.f;
  ctr_clip_vertex before[4];
  memcpy(before, quad, sizeof(quad));
  const int select[4][2] = {{0, 1}, {2, 1}, {0, 3}, {2, 3}};
  check(!ctr_trim_clip_quad(quad, 0, 1, 2, select, box, box) &&
            memcmp(before, quad, sizeof(quad)) == 0,
        "non-affine quads must retain their original geometry");
}
}  // namespace

void run_citadel_tests() {
  test_visible_lights();
  test_clip_trim();
}
