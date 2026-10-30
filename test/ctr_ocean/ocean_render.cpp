/*!
 * @file ocean_render.cpp
 * Host picture of a dumped 3DS ocean frame (CtrOceanFull's ocean_dump.bin, see CtrOceanFull.cpp)
 * through the soft backend: the ocean alone, as the 3DS draws it (the GPU's wave texture).
 *
 *   test/ctr_ocean/build.sh && /tmp/ocean_render ocean_dump.bin out.ppm [all|mid|near] [noenv]
 *
 * mid / near: only that bucket's draws. The fog color is the far ocean's at the screen's top row of
 * ocean (the dump has none); the sky reflection uses the dumped env map.
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
#include "test/ctr_ocean/jak2_stubs.h"

// ---------------------------------------------------------------------------------------------
// stubs: the env map, the settings, the log, file output
// ---------------------------------------------------------------------------------------------

namespace {
int g_env_w = 1, g_env_h = 1;
std::vector<u32> g_env;
CtrTexture g_env_tex;
const char* g_out = nullptr;
bool g_no_refl = false;  // "noenv": no sky reflection texture
u64 g_wave_env_tex0 = 0;
}  // namespace

CtrSettings g_settings;
namespace ctr_settings_detail {
const CtrSettings* g_settings = &::g_settings;
const CtrSettings& first_use() {
  return ::g_settings;
}
}  // namespace ctr_settings_detail
bool CtrVram::decode_for_cpu(u64 tex0, std::vector<u32>* out, int* w, int* h) {
  if (g_no_refl && tex0 != g_wave_env_tex0) {
    return false;
  }
  *out = g_env;
  *w = g_env_w;
  *h = g_env_h;
  return true;
}
const CtrTexture* CtrVram::get_texture(u64) {
  return &g_env_tex;
}
const void* CtrVram::upload_source(u32, u64*) const {
  return nullptr;
}
namespace file_util {
void write_rgba_png(const fs::path&, void* data, int w, int h) {
  FILE* f = fopen(g_out, "wb");
  fprintf(f, "P6 %d %d 255\n", w, h);
  for (int i = 0; i < w * h; i++) {
    fwrite((const u8*)data + 4 * i, 1, 3, f);
  }
  fclose(f);
}
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

namespace {
struct Bucket {
  std::vector<u8> mem;  // tags and data, then an END tag at `end`
  u32 end = 0;
};

bool load(const char* path, Bucket* mid, Bucket* near) {
  FILE* f = fopen(path, "rb");
  if (!f) {
    return false;
  }
  std::vector<u8> d;
  u8 buf[65536];
  size_t n;
  while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
    d.insert(d.end(), buf, buf + n);
  }
  fclose(f);
  size_t p = 0;
  auto get32 = [&]() {
    u32 v;
    memcpy(&v, &d[p], 4);
    p += 4;
    return v;
  };
  while (p + 4 <= d.size()) {
    const std::string tag((const char*)&d[p], 4);
    p += 4;
    if (tag == "OCNB") {
      const u32 is_near = get32(), count = get32();
      Bucket& b = is_near ? *near : *mid;
      b.mem.clear();
      for (u32 i = 0; i < count; i++) {
        const u32 qwc = get32();
        u64 vifs;
        memcpy(&vifs, &d[p], 8);
        p += 8;
        const u64 dma_tag = (u64)qwc | (1ull << 28);  // CNT
        const size_t at = b.mem.size();
        b.mem.resize(at + 16 + 16 * qwc);
        memcpy(&b.mem[at], &dma_tag, 8);
        memcpy(&b.mem[at + 8], &vifs, 8);
        memcpy(&b.mem[at + 16], &d[p], 16 * qwc);
        p += 16 * qwc;
      }
      b.end = (u32)b.mem.size();
      const u64 end_tag = 7ull << 28;
      b.mem.resize(b.end + 16);
      memset(&b.mem[b.end], 0, 16);
      memcpy(&b.mem[b.end], &end_tag, 8);
    } else if (tag == "OCNE") {
      g_env_w = (int)get32();
      g_env_h = (int)get32();
      g_env.resize((size_t)g_env_w * g_env_h);
      memcpy(g_env.data(), &d[p], 4 * g_env.size());
      p += 4 * g_env.size();
    } else {
      return false;
    }
  }
  return !mid->mem.empty() && !near->mem.empty();
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    printf("usage: ocean_render ocean_dump.bin out.ppm [all|mid|near] [noenv]\n");
    return 1;
  }
  Bucket mid, near;
  if (!load(argv[1], &mid, &near)) {
    printf("can't read %s\n", argv[1]);
    return 1;
  }
  g_out = argv[2];
  const std::string only = argc > 3 ? argv[3] : "";
  g_no_refl = argc > 4 && !strcmp(argv[4], "noenv");
  g_wave_env_tex0 = 0x2004e7c59930a71aull;
  ctr_gpu_init();
  g_env_tex.handle = ctr_gpu_tex_create(g_env_w, g_env_h, (const uint8_t*)g_env.data());
  g_env_tex.w = g_env_w;
  g_env_tex.h = g_env_h;
  alignas(CtrVram) static u8 vram_storage[sizeof(CtrVram)];
  CtrOceanFull ocean;
  for (int frame = 1; frame <= 2; frame++) {
    CtrRenderState rs;
    rs.vram = (CtrVram*)vram_storage;
    rs.frame_idx = frame;
    rs.fog_color[0] = 133, rs.fog_color[1] = 157, rs.fog_color[2] = 209;
    ctr_gpu_frame_begin(0, 0, 0);
    rs.next_bucket = mid.end;
    DmaFollower dm(mid.mem.data(), 0);
    if (only != "near") {
      ocean.render_mid_and_far(dm, rs);
    }
    rs.next_bucket = near.end;
    DmaFollower dn(near.mem.data(), 0);
    if (only != "mid") {
      ocean.render_near(dn, rs);
    }
    if (frame == 2) {
      ctr_gpu_request_screenshot(argv[2]);
    }
    ctr_gpu_frame_end();
  }
  printf("mid runs %d, near runs %d, unexpected DMA %d, vertices %d\n", ocean.m_mid_runs,
         ocean.m_near_runs, ocean.m_failed, ocean.m_batch->vertices);
  return 0;
}
