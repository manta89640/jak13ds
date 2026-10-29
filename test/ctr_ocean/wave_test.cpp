/*!
 * @file wave_test.cpp
 * Host test of the full ocean's wave texture (CtrOceanFull::update_texture): the texture the GPU
 * draws (update_texture_gpu, here through the soft backend, ctr_gpu_soft.cpp) against the CPU's
 * texels (the reference), from made-up waves and a random env map.
 *
 *   test/ctr_ocean/build.sh && /tmp/ocean_wave_test
 *
 * Both against a reference of the PS2's (the GS's triangles, in floats). Exit status 0 when the
 * GPU's texture is the PS2's: no channel more than 2 apart in 99.9% of the texels (the env map's
 * nearest texel may flip where the interpolated lookup lands on a texel edge). The CPU's (bilinear
 * across each quad, not the GS's two triangles) is only reported.
 */

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "common/dma/dma.h"
#include "common/dma/gs.h"
#include "common/log/log.h"
#include "common/util/FileUtil.h"

#include "game/graphics/ctr/CtrOceanBatch.h"
#include "game/graphics/ctr/CtrOceanVu.h"
#include "game/graphics/ctr/CtrRenderer.h"
#include "game/graphics/ctr/CtrSettings.h"
#include "game/graphics/ctr/CtrVram.h"
#include "game/graphics/ctr/ctr_gpu.h"

#define private public
#include "game/graphics/ctr/CtrOceanFull.cpp"
#undef private

// ---------------------------------------------------------------------------------------------
// stubs: the env map, the settings, the log, file output
// ---------------------------------------------------------------------------------------------

namespace {
constexpr int kEnv = 64;
std::vector<u32> g_env;
CtrTexture g_env_tex;
}  // namespace

CtrSettings g_settings;
namespace ctr_settings_detail {
const CtrSettings* g_settings = &::g_settings;
const CtrSettings& first_use() {
  return ::g_settings;
}
}  // namespace ctr_settings_detail
bool CtrVram::decode_for_cpu(u64, std::vector<u32>* out, int* w, int* h) {
  *out = g_env;
  *w = kEnv;
  *h = kEnv;
  return true;
}
const CtrTexture* CtrVram::get_texture(u64) {
  return &g_env_tex;
}
const void* CtrVram::upload_source(u32, u64*) const {
  return nullptr;
}
namespace file_util {
void write_rgba_png(const fs::path&, void*, int, int) {}
std::string get_file_path(const std::vector<std::string>& path) {
  return path.empty() ? "" : path.back();
}
bool create_dir_if_needed(const fs::path&) {
  return true;
}
}  // namespace file_util
namespace lg::internal {
void log_message(level, LogTime&, const char* message) {
  printf("%s\n", message);
}
void log_print(const char* message) {
  printf("%s", message);
}
}  // namespace lg::internal
void private_assert_failed(const char* expr, const char* file, int line, const char* function) {
  fprintf(stderr, "assert %s %s:%d %s\n", expr, file, line, function);
  abort();
}
void private_assert_failed(const char* expr,
                           const char* file,
                           int line,
                           const char* function,
                           const char* msg) {
  fprintf(stderr, "assert %s %s:%d %s %s\n", expr, file, line, function, msg);
  abort();
}
void private_assert_failed(const char* expr,
                           const char* file,
                           int line,
                           const char* function,
                           const std::string_view& msg) {
  fprintf(stderr, "assert %s %s:%d %s %s\n", expr, file, line, function,
          std::string(msg).c_str());
  abort();
}

int main() {
  srand(7);
  g_env.resize(kEnv * kEnv);
  const bool ids = getenv("WAVE_IDS") != nullptr;  // (env texel = its coordinates, vertex color 1)
  for (int i = 0; ids && i < kEnv * kEnv; i++) {
    g_env[i] = (u32)(i % kEnv) * 4 | (u32)(i / kEnv) * 4 << 8 | 0x80000000u;
  }
  for (auto& e : g_env) {
    if (ids) break;
    // GS colors, alpha 0x80 = 1 (environment-ocean-alphamod's reach up to it)
    e = (u32)(rand() & 0xff) | (u32)(rand() & 0xff) << 8 | (u32)(rand() & 0xff) << 16 |
        (u32)(rand() % 0x90) << 24;
  }
  ctr_gpu_init();
  g_env_tex.handle = ctr_gpu_tex_create(kEnv, kEnv, (const uint8_t*)g_env.data());
  g_env_tex.w = g_env_tex.h = kEnv;

  // the 33 rows of *ocean-verts*: per 4 vertices color 0, nx, color 1, ny, color 2, nz, color 3, -
  alignas(16) static float rows[kGrid + 1][64][4];
  for (int r = 0; r <= kGrid; r++) {
    for (int c = 0; c < kGrid; c++) {
      float(*q)[4] = rows[r] + (c / 4) * 8;
      const int k = c % 4;
      const float u = 6.2831853f * c / kGrid, v = 6.2831853f * r / kGrid;
      float nx = 0.35f * sinf(2 * u + v), ny = 0.3f * cosf(u - 3 * v), nz = 1.f;
      const float len = sqrtf(nx * nx + ny * ny + nz * nz);
      q[1][k] = nx / len;
      q[3][k] = ny / len;
      q[5][k] = nz / len;
      if (ids) {
        q[2 * k][0] = q[2 * k][1] = q[2 * k][2] = 128.f;
        q[2 * k][3] = 1.f + 0.5f * sinf(u * v);
        continue;
      }
      q[2 * k][0] = 70.f + 50.f * sinf(u + 2 * v);
      q[2 * k][1] = 90.f + 30.f * cosf(3 * u);
      q[2 * k][2] = 110.f + 17.f * sinf(v);
      q[2 * k][3] = 1.f + 0.5f * sinf(u * v);
    }
  }
  CtrOceanFull::TextureDma t;
  memset(&t.constants, 0, sizeof(t.constants));
  t.constants.cam_nrm[0] = 0.2f;
  t.constants.cam_nrm[1] = -0.3f;
  t.constants.cam_nrm[2] = sqrtf(1.f - 0.04f - 0.09f);
  t.constants.constants[0] = 0.5f;
  t.constants.constants[1] = 0.5f;
  t.env_tex0 = 1;
  for (int r = 0; r <= kGrid; r++) {
    t.rows[r] = (const u8*)rows[r];
  }
  alignas(CtrVram) static u8 vram_storage[sizeof(CtrVram)];  // (only its stubbed methods are used)
  CtrRenderState rs;
  rs.vram = (CtrVram*)vram_storage;

  CtrOceanFull ocean;
  if (ocean.m_wave_rt < 0) {
    printf("FAIL: no render texture\n");
    return 1;
  }
  ctr_gpu_frame_begin(0, 0, 0);
  ocean.update_texture(t, rs);
  int w = 0, h = 0;
  const uint8_t* gpu = ctr_gpu_soft_texels(ocean.m_wave_rt, &w, &h);
  if (!gpu || w != kTexSize || h != kTexSize) {
    printf("FAIL: no texels\n");
    return 1;
  }
  std::vector<uint8_t> gpu_copy(gpu, gpu + 4 * w * h);

  ocean.m_wave_rt = -1;  // the CPU's texels
  ocean.m_tex = -1;
  ocean.update_texture(t, rs);
  const uint8_t* cpu = (const uint8_t*)ocean.m_texels.data();

  // the PS2's: the program's triangle strips (diagonal from row r + 1 to column c + 1), gouraud
  // color and env map coordinates at the texel centers, the env map's nearest texel, GS modulate
  std::vector<uint8_t> ref(4 * kTexSize * kTexSize);
  {
    struct V {
      float s, t, c[3];
    };
    static V vs[kGrid + 1][kGrid + 1];
    const float cx = t.constants.cam_nrm[0], cy = t.constants.cam_nrm[1],
                cz = t.constants.cam_nrm[2], k0 = t.constants.constants[0],
                k1 = t.constants.constants[1];
    for (int r = 0; r <= kGrid; r++) {
      for (int c = 0; c < kGrid; c++) {
        const float(*q)[4] = rows[r] + (c / 4) * 8;
        const int k = c % 4;
        const float refl = (q[1][k] * cx + q[3][k] * cy) + q[5][k] * cz;
        V& v = vs[r][c];
        v.s = (q[1][k] * refl * q[2 * k][3] + cx) * k0 + k1;
        v.t = (q[3][k] * refl * q[2 * k][3] + cy) * k0 + k1;
        for (int ch = 0; ch < 3; ch++) {
          v.c[ch] = (float)((s32)q[2 * k][ch] & 0xff);
        }
      }
      vs[r][kGrid] = vs[r][0];
    }
    for (int y = 0; y < kTexSize; y++) {
      for (int x = 0; x < kTexSize; x++) {
        const int r = y / 4, c = x / 4;
        const float u = ((x % 4) + 0.5f) / 4, v = ((y % 4) + 0.5f) / 4;
        const V &a = vs[r][c], &b = vs[r][c + 1], &d = vs[r + 1][c], &e = vs[r + 1][c + 1];
        float w[4];  // weights of a, b, d, e
        if (u + v <= 1) {
          w[0] = 1 - u - v, w[1] = u, w[2] = v, w[3] = 0;
        } else {
          w[0] = 0, w[1] = 1 - v, w[2] = 1 - u, w[3] = u + v - 1;
        }
        auto mix = [&](auto f) { return w[0] * f(a) + w[1] * f(b) + w[2] * f(d) + w[3] * f(e); };
        const float s = mix([](const V& p) { return p.s; });
        const float tt = mix([](const V& p) { return p.t; });
        const int tx = std::clamp((int)std::floor(s * kEnv), 0, kEnv - 1);
        const int ty = std::clamp((int)std::floor(tt * kEnv), 0, kEnv - 1);
        const u32 env = g_env[ty * kEnv + tx];
        u8* o = &ref[4 * (y * kTexSize + x)];
        for (int ch = 0; ch < 3; ch++) {
          const float col = mix([ch](const V& p) { return p.c[ch]; });
          o[ch] = (u8)std::min(255.f, ((env >> (8 * ch)) & 0xff) * col / 128.f);
        }
        const u32 ea = env >> 24;
        o[3] = (u8)(ea >= 0x80 ? 0xff : 2 * ea);
      }
    }
  }
  auto compare = [&](const char* what, const uint8_t* p, const uint8_t* q) {
    int hist[3] = {0, 0, 0}, max_d = 0;
    for (int i = 0; i < kTexSize * kTexSize; i++) {
      int d = 0;
      for (int ch = 0; ch < 4; ch++) {
        d = std::max(d, std::abs((int)p[4 * i + ch] - (int)q[4 * i + ch]));
      }
      max_d = std::max(max_d, d);
      hist[d == 0 ? 0 : (d <= 2 ? 1 : 2)]++;
    }
    printf("%s: %d texels equal, %d within 2, %d more (max %d)\n", what, hist[0], hist[1], hist[2],
           max_d);
    return hist[2];
  };
  const int gpu_off = compare("GPU (soft backend) vs the PS2's triangles", gpu_copy.data(), ref.data());
  compare("CPU (bilinear across each quad) vs the PS2's triangles", cpu, ref.data());

  int hist[4] = {0, 0, 0, 0}, max_d = 0;
  double sum = 0;
  for (int i = 0; i < kTexSize * kTexSize; i++) {
    int d = 0;
    for (int ch = 0; ch < 4; ch++) {
      d = std::max(d, std::abs((int)gpu_copy[4 * i + ch] - (int)cpu[4 * i + ch]));
      sum += std::abs((int)gpu_copy[4 * i + ch] - (int)cpu[4 * i + ch]);
    }
    max_d = std::max(max_d, d);
    hist[d == 0 ? 0 : (d <= 2 ? 1 : (d <= 16 ? 2 : 3))]++;
  }
  const int n = kTexSize * kTexSize;
  printf("GPU vs CPU: %d equal, %d within 2, %d within 16, %d more; max %d, mean %.3f per "
         "channel\n",
         hist[0], hist[1], hist[2], hist[3], max_d, sum / (4.0 * n));
  const bool ok = gpu_off <= n / 1000;
  printf("%s\n", ok ? "PASS" : "FAIL");
  return ok ? 0 : 1;
}
