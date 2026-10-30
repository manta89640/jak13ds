// (AI-assisted) tod_diff pixels: chunks drawn through the host soft renderer (ctr_gpu_soft.cpp)
// from a mesh layout, and what that renderer needs from the rest of gk (its screenshots land in
// a buffer instead of a file)
#include <algorithm>
#include <cstring>
#include <vector>

#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "game/graphics/ctr/ctr_gpu.h"

#include "tod_pixels.h"

static std::vector<unsigned char> g_shot;

namespace lg::internal {
void log_message(level, LogTime&, const char*) {}
}  // namespace lg::internal
namespace file_util {
void write_rgba_png(const fs::path&, void* data, int w, int h) {
  g_shot.assign((const unsigned char*)data, (const unsigned char*)data + (size_t)w * h * 4);
}
bool create_dir_if_needed(const fs::path&) {
  return true;
}
}  // namespace file_util

// textured with a checker (the texture coordinates count) times the vertex colors, depth tested,
// seen from above (orthographic over the chunks' spheres)
std::vector<unsigned char> tod_draw_pixels(const TodPxChunk* chunks, size_t count, bool split) {
  static int tex = -1;
  if (tex < 0) {
    ctr_gpu_init();
    std::vector<uint8_t> t(64 * 64 * 4);
    for (int i = 0; i < 64 * 64; i++) {
      const uint8_t v = ((i % 64) / 8 + (i / 64) / 8) % 2 ? 0xff : 0x60;
      t[4 * i] = v;
      t[4 * i + 1] = (uint8_t)(i * 7);
      t[4 * i + 2] = 0xff - v;
      t[4 * i + 3] = 0xff;
    }
    tex = ctr_gpu_tex_create(64, 64, t.data());
  }
  float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
  for (size_t k = 0; k < count; k++) {
    for (int a = 0; a < 3; a++) {
      lo[a] = std::min(lo[a], chunks[k].bsphere[a] - chunks[k].bsphere[3]);
      hi[a] = std::max(hi[a], chunks[k].bsphere[a] + chunks[k].bsphere[3]);
    }
  }
  const float cx = (lo[0] + hi[0]) / 2, cy = (lo[1] + hi[1]) / 2, cz = (lo[2] + hi[2]) / 2;
  const float h = std::max(hi[0] - lo[0], hi[2] - lo[2]) / 2 + 1, hy = (hi[1] - lo[1]) / 2 + 1;
  ctr_draw_state st{};
  st.tex = tex;
  st.tcc = 1;
  st.blend = CTR_BLEND_OFF;
  st.atest = CTR_TEST_ALWAYS;
  st.ztest = CTR_TEST_GEQUAL;
  st.zwrite = 1;
  ctr_gpu_frame_begin(0, 0, 0);
  for (size_t k = 0; k < count; k++) {
    const auto& c = chunks[k];
    if (!c.index_count || !c.vertex_count) {
      continue;
    }
    void* v;
    uint16_t* ix;
    const int m = ctr_gpu_mesh_begin((int)c.vertex_count, (int)c.index_count, &v, &ix);
    if (split) {
      ctr_gpu_mesh_split_colors(m);
    }
    memcpy(v, c.verts, 16 * (size_t)c.vertex_count);
    memcpy(ix, c.indices, 2 * (size_t)c.index_count);
    ctr_gpu_mesh_end(m, 1);
    const float s = c.scale;
    const float clip[16] = {s / h, 0, 0, (c.origin[0] - cx) / h,
                            0, 0, s / h, (c.origin[2] - cz) / h,
                            0, s / hy, 0, (c.origin[1] - cy) / hy,
                            0, 0, 0, 1};
    ctr_gpu_draw_mesh(&st, clip, m, 0, (int)c.index_count);
    ctr_gpu_mesh_delete(m);
  }
  ctr_gpu_request_screenshot("pixels");
  ctr_gpu_frame_end();
  return g_shot;
}
