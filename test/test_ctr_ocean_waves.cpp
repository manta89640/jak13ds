// (AI-assisted) The 3DS near ocean grid (game/graphics/ctr/CtrOceanNear.h, config ocean = waves)
// against ocean-get-height (goal_src/jak1/engine/gfx/ocean/ocean.gc), the height the game's
// physics uses: the vertices must sit on the same waves, at random camera positions over the map.

#include <cmath>
#include <random>
#include <vector>

#include "game/graphics/ctr/CtrOceanNear.h"
#include "gtest/gtest.h"

using namespace ctr_ocean;

namespace {
struct Fixture {
  std::mt19937 rng{1234};
  float heights[32 * 32];
  u32 colors[kColorRow * 49];
  std::vector<u8> water = std::vector<u8>(kMapSub * kMapSub, 0);
  u32 fade[256];
  float start_x = -1234567.f, start_z = 2345678.f;

  Fixture() {
    // like ocean-interp-wave: s8 heights * 512 * 0.333 (both frames' weights)
    std::uniform_int_distribution<int> s8(-32, 31);
    for (float& h : heights) {
      h = (float)(s8(rng) * 512) * 0.333f;
    }
    std::uniform_int_distribution<u32> byte(0, 255);
    for (u32& c : colors) {
      c = byte(rng) | (byte(rng) << 8) | (byte(rng) << 16) | (0x80u << 24);
    }
    std::bernoulli_distribution wet(0.7);
    for (u8& w : water) {
      w = wet(rng);
    }
    make_fade(fade);
  }

  // a grid for a camera at (x, z) game units from the start corner, like CtrOcean::build_near
  void build(float x, float z, int* ox, int* oz, std::vector<Vertex>* verts) {
    const float wx = x / kWaveStep, wz = z / kWaveStep;
    *ox = ((int)std::floor(wx * 0.125f) - kBlocks / 2) * 8;
    *oz = ((int)std::floor(wz * 0.125f) - kBlocks / 2) * 8;
    s16 hq[32 * 32];
    quantize_heights(heights, hq);
    verts->assign(kVertexCount, Vertex{});
    build_vertices(hq, fade, colors, *ox, *oz, (int)((wx - *ox) * kStep),
                   (int)((wz - *oz) * kStep), verts->data());
  }
};
}  // namespace

TEST(CtrOceanWaves, VerticesOnOceanGetHeight) {
  Fixture f;
  // over the map, away from its edges (ocean-get-height extrapolates left of the start corner)
  std::uniform_real_distribution<float> pos(30.f * 4096.f, 4578.f * 4096.f);
  int full = 0, faded = 0;
  for (int trial = 0; trial < 200; trial++) {
    const float cx = pos(f.rng), cz = pos(f.rng);
    int ox, oz;
    std::vector<Vertex> v;
    f.build(cx, cz, &ox, &oz, &v);
    for (int r = 0; r < kVerts; r++) {
      for (int c = 0; c < kVerts; c++) {
        const Vertex& vx = v[r * kVerts + c];
        const s16 x = (s16)(vx.xy & 0xffff), y = (s16)(vx.xy >> 16), z = (s16)(vx.z & 0xffff);
        ASSERT_EQ(x, c * kStep);
        ASSERT_EQ(z, r * kStep);
        // the vertex in the world, and the game's height there
        const float wx = f.start_x + (float)(ox + c) * kWaveStep;
        const float wz = f.start_z + (float)(oz + r) * kWaveStep;
        const float ref = ocean_get_height_ref(f.heights, f.start_x, f.start_z, wx, wz);
        const float dx = (float)(ox + c) * kWaveStep - cx, dz = (float)(oz + r) * kWaveStep - cz;
        const float dist = std::sqrt(dx * dx + dz * dz);
        const float got = y * kYUnit;
        if (dist < kFadeStart - kWaveStep) {
          // full waves: the height itself (y steps of 3 mm)
          EXPECT_NEAR(got, ref, kYUnit) << "trial " << trial << " vertex " << c << "," << r;
          full++;
        } else if (dist > kFadeEnd + kWaveStep) {
          EXPECT_EQ(y, 0);  // flat at the edge: meets the far ocean
        } else {
          EXPECT_LE(std::abs(got), std::abs(ref) + kYUnit);
          faded++;
        }
      }
    }
  }
  EXPECT_GT(full, 200 * 100);
  EXPECT_GT(faded, 200 * 100);
}

TEST(CtrOceanWaves, VertexColorsBilinear) {
  Fixture f;
  std::uniform_real_distribution<float> pos(-30.f * 4096.f, 4650.f * 4096.f);  // also off the map
  for (int trial = 0; trial < 100; trial++) {
    int ox, oz;
    std::vector<Vertex> v;
    f.build(pos(f.rng), pos(f.rng), &ox, &oz, &v);
    for (int r = 0; r < kVerts; r++) {
      for (int c = 0; c < kVerts; c++) {
        // the far mesh's color_at, in wave steps (32 per color point)
        const float gx = std::clamp(ox + c, 0, kMapCells * 32) / 32.f;
        const float gz = std::clamp(oz + r, 0, kMapCells * 32) / 32.f;
        const int cx = std::min((int)gx, kMapCells - 1), cz = std::min((int)gz, kMapCells - 1);
        const float fx = gx - cx, fz = gz - cz;
        for (int ch = 0; ch < 4; ch++) {
          auto at = [&](int x, int z) {
            return (float)((f.colors[z * kColorRow + x] >> (8 * ch)) & 0xff);
          };
          const float a = at(cx, cz) + (at(cx + 1, cz) - at(cx, cz)) * fx;
          const float b = at(cx, cz + 1) + (at(cx + 1, cz + 1) - at(cx, cz + 1)) * fx;
          const float want = a + (b - a) * fz;
          const float got = (float)((v[r * kVerts + c].rgba >> (8 * ch)) & 0xff);
          ASSERT_NEAR(got, want, 1.01f) << "trial " << trial << " vertex " << c << "," << r;
        }
      }
    }
  }
}

TEST(CtrOceanWaves, IndicesOnlyWhereWater) {
  Fixture f;
  std::uniform_int_distribution<int> sub(-4, kMapSub + 4);
  std::vector<u32> idx(kMaxIndices / 2);
  for (int trial = 0; trial < 500; trial++) {
    const int ox = sub(f.rng) * 8, oz = sub(f.rng) * 8;
    const int n = build_indices(f.water.data(), ox, oz, idx.data());
    ASSERT_EQ(n, count_water_blocks(f.water.data(), ox, oz) * kBlockIndices);
    const u16* ix = (const u16*)idx.data();
    for (int i = 0; i < n; i += 3) {
      // each triangle in one sub-cell with water
      int min_c = kVerts, min_r = kVerts;
      for (int k = 0; k < 3; k++) {
        ASSERT_LT(ix[i + k], kVertexCount);
        min_c = std::min(min_c, ix[i + k] % kVerts);
        min_r = std::min(min_r, ix[i + k] / kVerts);
      }
      EXPECT_TRUE(sub_has_water(f.water.data(), (ox >> 3) + min_c / 8, (oz >> 3) + min_r / 8));
    }
  }
}
