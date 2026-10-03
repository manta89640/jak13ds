/*!
 * @file CtrOcean.cpp
 * (AI-assisted)
 * See CtrOcean.h.
 */

#include "CtrOcean.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

#include "common/dma/dma.h"
#include "common/goal_constants.h"
#include "common/log/log.h"

#include "game/graphics/ctr/CtrLevel.h"
#include "game/graphics/ctr/CtrOceanNear.h"
#include "game/graphics/ctr/CtrSettings.h"
#include "game/graphics/ctr/c3l_format.h"
#include "game/graphics/ctr/ctr_gpu.h"

// the packet draw-ocean-3ds sends (goal_src/jak1/engine/gfx/ocean/ocean.gc)
struct CtrOceanRenderer::Packet {
  u32 magic;          // "OCN3"
  u32 heights;        // 32 x 32 floats (z major), wave heights of this frame, game units
  u32 colors;         // ocean-colors: 49 rows (z) of 52 rgba (x), one per 96 m grid point
  u32 flags;          // bit 0: sunken / mid off
  u32 mid_indices;    // 36 s16, one per 768 m tile (6 x 6, z major): mask index, -1 = no water
  u32 mid_masks;      // u64 masks, byte = cell row (z), bit = cell column (x), 1 = no water
  u32 trans_indices;  // 48 x 48 {s16 parent, s16 child}: 24 m sub-cell mask of a cell (parent)
  u32 pad;
  float start[4];      // start corner (x, z) and ocean height (y)
  float far_color[4];  // GS units
  CtrBackgroundCamera cam;
};
static_assert(sizeof(CtrOceanRenderer::Packet) == 16 * 27);

namespace {

constexpr u32 kMagic = 0x334e434f;  // "OCN3"
constexpr int kTiles = 6;           // tiles per side
constexpr int kCells = 48;          // 96 m cells per side
constexpr int kSub = 4;             // 24 m sub-cells per cell side
constexpr int kGrid = kCells * kSub;  // 192 sub-cells per side
constexpr float kSubSize = 98304.f;   // 24 m
constexpr float kScale = 768.f;       // mesh quantization: 24 m = 128 steps, the map = 24576
constexpr int kSubSteps = 128;
constexpr int kTexSize = 32;          // one texel per wave height sample (3 m)
// (AI-assisted) ocean = waves: the shimmer layer's repeat, direction and scroll speed (per ms),
// the crests' brightening at the highest wave height (1.33 m), the near grid's camera height limit
// (the PS2 draws its near ocean within 48 m of the water)
constexpr float kShimmerLen = 37.f * 4096.f;
constexpr float kShimmerCos = 0.825f, kShimmerSin = 0.565f;
constexpr float kShimmerSpeedS = 0.00004f, kShimmerSpeedT = 0.000025f;
constexpr float kCrest = 0.25f / (5456.f / ctr_ocean::kYUnit);
constexpr float kNearMaxHeight = 48.f * 4096.f;

// same as make_cam_mat in CtrLevel.cpp (make_new_cam_mat, background_common.cpp)
std::array<math::Vector4f, 4> make_cam_mat(const math::Vector4f cam_T_w[4],
                                           const math::Vector4f persp[4],
                                           float fog_constant,
                                           float hvdf_z) {
  const float pc_pxx = fog_constant * persp[0][0] / 256.f;
  const float pc_pyy = -fog_constant * persp[1][1] / 128.f;
  const float depth_scale = fog_constant * persp[2][2] / 8388608;
  const float game_pzw = persp[2][3];
  const float game_depth_offset = persp[3][2];
  math::Vector3f persp_scale(pc_pxx, pc_pyy, depth_scale);
  persp_scale.z() += (hvdf_z / 8388608.f - 1.f) * game_pzw;
  std::array<math::Vector4f, 4> result;
  for (auto& x : result) {
    x.set_zero();
  }
  for (int row = 0; row < 3; row++) {
    for (int col = 0; col < 3; col++) {
      result[row][col] = cam_T_w[row][col] * persp_scale[col];
    }
  }
  for (int row = 0; row < 3; row++) {
    result[row][3] = cam_T_w[row][2] * game_pzw;
  }
  result[3][2] = fog_constant * game_depth_offset / 8388608;
  return result;
}

bool sphere_in_view(const float* s, const math::Vector4f* planes) {
  math::Vector4f acc = planes[0] * s[0] + planes[1] * s[1] + planes[2] * s[2] - planes[3];
  return acc.x() > -s[3] && acc.y() > -s[3] && acc.z() > -s[3] && acc.w() > -s[3];
}

bool valid_addr(u32 addr, u32 size) {
  return addr >= 16 && addr + size <= (u32)EE_MAIN_MEM_SIZE;
}

template <typename T>
T read(const u8* ee, u32 addr) {
  T v;
  memcpy(&v, ee + addr, sizeof(T));
  return v;
}

}  // namespace

CtrOceanRenderer::CtrOceanRenderer(std::string name, int id)
    : CtrBucketRenderer(std::move(name), id) {}

CtrOceanRenderer::~CtrOceanRenderer() {
  if (m_mesh >= 0) {
    ctr_gpu_mesh_delete(m_mesh);
  }
  if (m_tex >= 0) {
    ctr_gpu_tex_delete(m_tex);
  }
  if (m_shimmer >= 0) {
    ctr_gpu_tex_delete(m_shimmer);
  }
}

bool CtrOceanRenderer::waves() const {
  return ctr_settings().ocean_mode == CtrSettings::OceanMode::Waves;
}

void CtrOceanRenderer::render(DmaFollower& dma, CtrRenderState& rs) {
  Packet p;
  bool have = false;
  while (dma.current_tag_offset() != rs.next_bucket) {
    auto data = dma.read_and_advance();
    if (data.size_bytes == sizeof(Packet) && data.vifcode1().kind == VifCode::Kind::PC_PORT) {
      memcpy(&p, data.data, sizeof(Packet));
      have = p.magic == kMagic;
    }
  }
  if (!have || !ctr_settings().ocean) {
    return;
  }
  const u32 key[6] = {p.colors, p.mid_indices, p.mid_masks, p.trans_indices,
                      (u32)(s32)p.start[0], (u32)(s32)p.start[2]};
  if (!m_have_mesh || memcmp(key, m_key, sizeof(key)) != 0) {
    memcpy(m_key, key, sizeof(key));
    m_have_mesh = true;
    if (!build_mesh(rs.ee_mem, p)) {
      return;
    }
  }
  if (m_mesh < 0) {
    return;
  }
  update_texture(rs.ee_mem, p.heights);
  m_near_index_count = 0;
  if (waves()) {
    build_near(rs.ee_mem, p);
  }
  draw(p, rs);
  m_frames++;
  if (rs.log_now && m_frames) {
    lg::debug("[ctr] ocean: {:.1f} tiles, {:.0f} triangles per frame, height {:.2f} m",
              (float)m_tiles_drawn / m_frames, (float)m_triangles / m_frames, p.start[1] / 4096.f);
    m_frames = m_tiles_drawn = m_triangles = 0;
  }
}

bool CtrOceanRenderer::build_mesh(const u8* ee, const Packet& p) {
  if (m_mesh >= 0) {
    ctr_gpu_mesh_delete(m_mesh);
    m_mesh = -1;
  }
  m_tiles.clear();
  m_water.clear();
  m_trough = 0;
  m_near_frames = 0;
  m_near_ms = 0;
  m_near_logged = false;
  if (!valid_addr(p.colors, 52 * 49 * 4) || !valid_addr(p.mid_indices, 72) ||
      !valid_addr(p.trans_indices, kCells * kCells * 4) || !valid_addr(p.mid_masks, 8)) {
    lg::error("[ctr] ocean: bad map addresses {:x} {:x} {:x} {:x}", p.colors, p.mid_indices,
              p.mid_masks, p.trans_indices);
    return false;
  }
  auto mask_of = [&](int idx) -> u64 {
    const u32 a = p.mid_masks + 8 * (u32)idx;
    return valid_addr(a, 8) ? read<u64>(ee, a) : ~0ull;
  };
  // color at a sub-cell grid point: bilinear from the 96 m color grid
  auto color_at = [&](int gx, int gz, u8* out) {
    const int cx = std::min(gx / kSub, kCells - 1), cz = std::min(gz / kSub, kCells - 1);
    const float fx = (gx - cx * kSub) / (float)kSub, fz = (gz - cz * kSub) / (float)kSub;
    u8 c[4][4];
    for (int k = 0; k < 4; k++) {
      const int x = cx + (k & 1), z = cz + (k >> 1);
      const u32 v = read<u32>(ee, p.colors + 4 * (u32)(z * 52 + x));
      memcpy(c[k], &v, 4);
    }
    for (int ch = 0; ch < 4; ch++) {
      const float a = c[0][ch] + (c[1][ch] - c[0][ch]) * fx;
      const float b = c[2][ch] + (c[3][ch] - c[2][ch]) * fx;
      out[ch] = (u8)std::lround(a + (b - a) * fz);
    }
  };

  std::vector<c3l::Vertex> verts;
  std::vector<u16> indices;
  // (AI-assisted) water per 24 m sub-cell, for the near grid (ocean = waves)
  std::vector<u8> water(kGrid * kGrid, 0);
  std::vector<int> vmap((kGrid / kTiles + 1) * (kGrid / kTiles + 1));
  constexpr int kTileSub = kGrid / kTiles;  // 32 sub-cells per tile side
  int shore_cells = 0, cells = 0;
  for (int tz = 0; tz < kTiles; tz++) {
    for (int tx = 0; tx < kTiles; tx++) {
      const s16 mid_idx = read<s16>(ee, p.mid_indices + 2 * (u32)(tz * kTiles + tx));
      if (mid_idx < 0) {
        continue;
      }
      const u64 mid_mask = mask_of(mid_idx);
      Tile tile;
      tile.first_index = (u32)indices.size();
      std::fill(vmap.begin(), vmap.end(), -1);
      int min_x = kTileSub, min_z = kTileSub, max_x = 0, max_z = 0;
      // vertex at local sub-cell grid point (lx, lz) of this tile
      auto vert = [&](int lx, int lz) -> u16 {
        int& slot = vmap[lz * (kTileSub + 1) + lx];
        if (slot < 0) {
          const int gx = tx * kTileSub + lx, gz = tz * kTileSub + lz;
          c3l::Vertex v;
          memset(&v, 0, sizeof(v));
          v.pos[0] = (s16)(gx * kSubSteps);
          v.pos[1] = 0;
          v.pos[2] = (s16)(gz * kSubSteps);
          // texture: one repeat per 96 m (like the far ocean and the wave height grid)
          v.st[0] = (s16)(lx * 1024 / kSub);
          v.st[1] = (s16)(lz * 1024 / kSub);
          color_at(gx, gz, v.rgba);
          slot = (int)verts.size();
          verts.push_back(v);
        }
        min_x = std::min(min_x, lx);
        max_x = std::max(max_x, lx);
        min_z = std::min(min_z, lz);
        max_z = std::max(max_z, lz);
        return (u16)slot;
      };
      auto quad = [&](int lx0, int lz0, int lx1, int lz1) {
        const u16 a = vert(lx0, lz0), b = vert(lx1, lz0), c = vert(lx1, lz1), d = vert(lx0, lz1);
        indices.insert(indices.end(), {a, b, c, a, c, d});
      };
      for (int r = 0; r < 8; r++) {
        for (int c = 0; c < 8; c++) {
          if ((mid_mask >> (8 * r + c)) & 1) {
            continue;  // no water in this cell
          }
          cells++;
          const int cz = tz * 8 + r, cx = tx * 8 + c;
          const s16 parent = read<s16>(ee, p.trans_indices + 4 * (u32)(cz * kCells + cx));
          const u64 sub_mask = parent >= 0 ? (mask_of(parent) & 0x0f0f0f0full) : 0;
          const int lx = c * kSub, lz = r * kSub;
          for (int sr = 0; sr < kSub; sr++) {
            for (int sc = 0; sc < kSub; sc++) {
              water[(cz * kSub + sr) * kGrid + cx * kSub + sc] = !((sub_mask >> (8 * sr + sc)) & 1);
            }
          }
          if (sub_mask == 0) {
            // open water: 2 x 2 quads of 48 m
            for (int q = 0; q < 4; q++) {
              const int x0 = lx + 2 * (q & 1), z0 = lz + 2 * (q >> 1);
              quad(x0, z0, x0 + 2, z0 + 2);
            }
          } else {
            // shore: the 24 m sub-cells with water
            shore_cells++;
            for (int sr = 0; sr < kSub; sr++) {
              for (int sc = 0; sc < kSub; sc++) {
                if (!((sub_mask >> (8 * sr + sc)) & 1)) {
                  quad(lx + sc, lz + sr, lx + sc + 1, lz + sr + 1);
                }
              }
            }
          }
        }
      }
      tile.index_count = (u32)indices.size() - tile.first_index;
      if (tile.index_count) {
        const float x0 = (tx * kTileSub + min_x) * kSubSize, x1 = (tx * kTileSub + max_x) * kSubSize;
        const float z0 = (tz * kTileSub + min_z) * kSubSize, z1 = (tz * kTileSub + max_z) * kSubSize;
        tile.center[0] = 0.5f * (x0 + x1);
        tile.center[1] = 0;
        tile.center[2] = 0.5f * (z0 + z1);
        tile.radius = 0.5f * std::sqrt((x1 - x0) * (x1 - x0) + (z1 - z0) * (z1 - z0));
        m_tiles.push_back(tile);
      }
    }
  }
  if (verts.empty() || verts.size() > 65535) {
    lg::info("[ctr] ocean: no mesh ({} vertices)", verts.size());
    return false;
  }
  m_mesh = ctr_gpu_mesh_create(verts.data(), (int)verts.size(), indices.data(), (int)indices.size());
  m_water = std::move(water);
  lg::info("[ctr] ocean mesh: {} tiles, {} cells ({} shore), {} vertices, {} triangles{}",
           m_tiles.size(), cells, shore_cells, verts.size(), indices.size() / 3,
           m_mesh < 0 ? " (could not be created)" : "");
  return m_mesh >= 0;
}

void CtrOceanRenderer::update_texture(const u8* ee, u32 heights) {
  constexpr int N = kTexSize;
  float* h = m_heights;  // (AI-assisted) kept for the near grid
  static_assert(sizeof(m_heights) == N * N * 4);
  if (valid_addr(heights, sizeof(m_heights))) {
    memcpy(h, ee + heights, sizeof(m_heights));
  } else {
    memset(h, 0, sizeof(m_heights));
  }
  // Shade the wave heights (3 m apart) like a lit surface: brighter on the slopes facing the
  // light, darker on the others, a little brighter on the crests. The PS2 renders its ocean
  // texture from the same heights (env-mapped normals); this is a cheap stand-in.
  constexpr float kInvStep = 1.f / (2.f * 12288.f);
  constexpr float lx = 0.55f, lz = 0.35f;
  m_texels.resize(N * N * 4);
  for (int z = 0; z < N; z++) {
    const float* row = h + z * N;
    const float* up = h + ((z + N - 1) & (N - 1)) * N;
    const float* down = h + ((z + 1) & (N - 1)) * N;
    for (int x = 0; x < N; x++) {
      const float gx = (row[(x + 1) & (N - 1)] - row[(x + N - 1) & (N - 1)]) * kInvStep;
      const float gz = (down[x] - up[x]) * kInvStep;
      float v = 0.66f + 2.2f * (gx * lx + gz * lz) + row[x] * (0.10f / 4096.f);
      v = std::min(1.f, std::max(0.35f, v));
      const u8 c = (u8)(v * 255.f);
      u8* t = &m_texels[4 * (z * N + x)];
      t[0] = c;
      t[1] = c;
      t[2] = c;
      t[3] = 255;
    }
  }
  // one texture, updated in place (with mip levels: at the horizon a 96 m repeat is a few
  // pixels, and the GPU reads the smallest level there)
  if (m_tex < 0) {
    m_tex = ctr_gpu_tex_create_mipmapped(N, N, m_texels.data());
  } else {
    ctr_gpu_tex_update(m_tex, m_texels.data());
  }
}

// (AI-assisted) ocean = waves: this frame's near grid (CtrOceanNear.h) into the GPU's per-frame
// memory, the shimmer's clock and the deepest trough
void CtrOceanRenderer::build_near(const u8* ee, const Packet& p) {
  using namespace ctr_ocean;
  const float* h = m_heights;
  if (m_shimmer < 0) {
    make_shimmer(h);
  }
  // the shimmer scrolls while the waves move (not in the pause menu)
  const double now = ctr_gpu_time_ms();
  if (h[0] != m_last_h[0] || h[517] != m_last_h[1]) {
    m_time = std::fmod(m_time + (float)std::min(now - m_last_ms, 100.0), 1e6f);
  }
  m_last_ms = now;
  m_last_h[0] = h[0];
  m_last_h[1] = h[517];
  if (!ctr_gpu_has_stencil()) {
    for (int i = 0; i < kWaves * kWaves; i++) {
      m_trough = std::max(m_trough, -h[i]);
    }
  }

  const auto& cam = p.cam;
  if (m_water.empty() || (p.flags & 1) || std::abs(cam.trans[1] - p.start[1]) > kNearMaxHeight) {
    return;
  }
  const double t0 = ctr_gpu_time_ms();
  // the camera's 24 m sub-cell in the middle of the grid
  const float wx = (cam.trans[0] - p.start[0]) * (1.f / kWaveStep);
  const float wz = (cam.trans[2] - p.start[2]) * (1.f / kWaveStep);
  const int ox = ((int)std::floor(wx * 0.125f) - kBlocks / 2) * 8;
  const int oz = ((int)std::floor(wz * 0.125f) - kBlocks / 2) * 8;
  const int blocks = count_water_blocks(m_water.data(), ox, oz);
  if (!blocks) {
    return;
  }
  auto* verts = (Vertex*)ctr_gpu_stream_alloc(kVertexCount * (int)sizeof(Vertex));
  auto* indices = (u32*)ctr_gpu_stream_alloc(blocks * kBlockIndices * 2);
  if (!verts || !indices) {
    return;
  }
  if (!m_have_fade) {
    make_fade(m_fade);
    m_have_fade = true;
  }
  s16 hq[kWaves * kWaves];
  quantize_heights(h, hq);
  build_vertices(hq, m_fade, (const u32*)(ee + p.colors), ox, oz, (int)((wx - ox) * kStep),
                 (int)((wz - oz) * kStep), verts);
  m_near_index_count = build_indices(m_water.data(), ox, oz, indices);
  m_near_verts = verts;
  m_near_indices = (const u16*)indices;
  m_near_ox = ox;
  m_near_oz = oz;

  m_near_ms += ctr_gpu_time_ms() - t0;
  if (!m_near_logged && ++m_near_frames == 60) {
    m_near_logged = true;
    lg::info("[ctr] ocean waves: near grid {} vertices, {} triangles (this frame), {:.3f} ms CPU "
             "per frame (heights, vertices, indices; 60 frames)",
             kVertexCount, m_near_index_count / 3, m_near_ms / 60);
  }
}

// (AI-assisted) the shimmer layer: once, from the wave heights lit from another side, around a
// neutral 0x80 (the combiner doubles it)
void CtrOceanRenderer::make_shimmer(const float* h) {
  constexpr int N = kTexSize;
  constexpr float kInvStep = 1.f / (2.f * 12288.f);
  constexpr float lx = -0.3f, lz = 0.6f;
  std::vector<u8> texels(N * N * 4);
  for (int z = 0; z < N; z++) {
    const float* row = h + z * N;
    const float* up = h + ((z + N - 1) & (N - 1)) * N;
    const float* down = h + ((z + 1) & (N - 1)) * N;
    for (int x = 0; x < N; x++) {
      const float gx = (row[(x + 1) & (N - 1)] - row[(x + N - 1) & (N - 1)]) * kInvStep;
      const float gz = (down[x] - up[x]) * kInvStep;
      const float v = std::clamp(0.5f + 1.5f * (gx * lx + gz * lz), 0.25f, 0.75f);
      u8* t = &texels[4 * (z * N + x)];
      t[0] = t[1] = t[2] = (u8)(v * 255.f);
      t[3] = 255;
    }
  }
  m_shimmer = ctr_gpu_tex_create_mipmapped(N, N, texels.data());
}

void CtrOceanRenderer::draw(const Packet& p, const CtrRenderState& rs) {
  const CtrSettings& settings = ctr_settings();
  const auto& cam = p.cam;
  auto R = make_cam_mat(cam.rot, cam.perspective, cam.fog.x(), cam.hvdf_off.z());
  const float draw_dist = settings.draw_distance * 4096.f;
  {
    // same fog as the level background (CtrTfragRenderer::draw_level)
    float fog0[4] = {0.f, 255.f, 255.f, -1.f / 255.f};
    if (settings.fog) {
      fog0[0] = cam.hvdf_off.w();
      fog0[1] = cam.fog.y();
      fog0[2] = cam.fog.z();
    }
    float fog1[4] = {0.f, 1.f, 0.f, 0.f};
    const float pzw = std::abs(cam.perspective[2][3]);
    // (AI-assisted) no extra fog towards the draw distance where the far ocean ring takes over
    // (below): the near ocean faded into the clear color (black at night) before the ring
    if (draw_dist > 0 && pzw > 0 && (p.flags & 1)) {
      const float start = draw_dist * settings.fog_start;
      fog1[0] = 1.f / pzw;
      fog1[1] = start;
      fog1[2] = 1.f / std::max(draw_dist - start, 1.f);
    }
    ctr_gpu_set_mesh_fog(fog0, fog1, rs.fog_color[0], rs.fog_color[1], rs.fog_color[2]);
  }
  // clip = -(R * (origin + q * scale - cam_trans)), see CtrTfragRenderer::draw_level
  auto make_clip = [&](const double origin[3], const float scale[3], float* m) {
    double d[3];
    for (int i = 0; i < 3; i++) {
      d[i] = origin[i] - (double)cam.trans[i];
    }
    for (int c = 0; c < 4; c++) {
      double t = 0;
      for (int i = 0; i < 3; i++) {
        m[4 * c + i] = -R[i][c] * scale[i];
        t += (double)R[i][c] * d[i];
      }
      if (c < 3) {
        t += R[3][c];
      }
      m[4 * c + 3] = (float)-t;
    }
    constexpr float kYScale = 512.f / 448.f;  // Jak 1 scissor adjust (tfrag3.vert)
    for (int i = 0; i < 4; i++) {
      m[4 + i] *= kYScale;
    }
  };
  // (AI-assisted) ocean = waves: without a stencil buffer, the far mesh and ring go below the
  // deepest trough (else the near grid's troughs would show them)
  const bool waves_on = waves() && m_shimmer >= 0;
  const float drop = waves_on && !ctr_gpu_has_stencil() ? m_trough + 0.1f * 4096.f : 0.f;
  const double origin[3] = {p.start[0], p.start[1] - drop, p.start[2]};
  const float far_scale[3] = {kScale, kScale, kScale};
  float m[16];
  make_clip(origin, far_scale, m);

  // (AI-assisted) ocean = waves: texture rows of ctr_gpu_ocean_draw for positions in steps of
  // `unit` from (x0, z0) (game units from the start corner), reduced near the point (rx, rz)
  auto ocean_rows = [&](ctr_ocean_draw& od, float unit, double x0, double z0, double rx,
                        double rz) {
    auto frac_at = [](double at_origin, double at_ref) { return at_origin - std::floor(at_ref); };
    // the wave texture: one repeat per 96 m, texel centers on the wave heights
    constexpr double kRepeat = 32.0 * 12288.0, kHalfTexel = 0.5 / 32.0;
    const float k = (float)(unit / kRepeat);
    const float r0[4] = {k, 0, 0,
                         (float)frac_at(x0 / kRepeat - kHalfTexel, rx / kRepeat - kHalfTexel)};
    const float r1[4] = {0, 0, k,
                         (float)frac_at(z0 / kRepeat - kHalfTexel, rz / kRepeat - kHalfTexel)};
    // the shimmer: rotated, its own repeat, scrolling
    const float ks = unit / kShimmerLen;
    const double ps = m_time * kShimmerSpeedS, pt = m_time * kShimmerSpeedT;
    auto su = [&](double x, double z) {
      return (kShimmerCos * x + kShimmerSin * z) / kShimmerLen + ps;
    };
    auto tu = [&](double x, double z) {
      return (kShimmerCos * z - kShimmerSin * x) / kShimmerLen + pt;
    };
    const float r2[4] = {kShimmerCos * ks, 0, kShimmerSin * ks,
                         (float)frac_at(su(x0, z0), su(rx, rz))};
    const float r3[4] = {-kShimmerSin * ks, 0, kShimmerCos * ks,
                         (float)frac_at(tu(x0, z0), tu(rx, rz))};
    memcpy(od.tex[0], r0, sizeof(r0));
    memcpy(od.tex[1], r1, sizeof(r1));
    memcpy(od.tex[2], r2, sizeof(r2));
    memcpy(od.tex[3], r3, sizeof(r3));
    memset(od.crest, 0, sizeof(od.crest));
  };

  ctr_draw_state st;
  memset(&st, 0, sizeof(st));
  st.tex = m_tex;
  st.tcc = 1;
  st.filter = 1;
  st.blend = CTR_BLEND_ALPHA;
  st.atest = CTR_TEST_ALWAYS;
  st.ztest = CTR_TEST_GEQUAL;
  st.zwrite = 1;

  // (AI-assisted) ocean = waves: the near grid first, marking its pixels in the stencil buffer
  if (waves_on && m_near_index_count) {
    using namespace ctr_ocean;
    const double gx = m_near_ox * (double)kWaveStep, gz = m_near_oz * (double)kWaveStep;
    const double near_origin[3] = {p.start[0] + gx, p.start[1], p.start[2] + gz};
    const float near_scale[3] = {kXZUnit, kYUnit, kXZUnit};
    float nm[16];
    make_clip(near_origin, near_scale, nm);
    ctr_ocean_draw od;
    ctr_gpu_prepare_mesh_matrix(nm, &od.clip);
    ocean_rows(od, kXZUnit, gx, gz, gx, gz);
    for (int i = 0; i < 3; i++) {
      od.crest[i] = kCrest / 255.f;
    }
    ctr_draw_state ns = st;
    ns.flags = CTR_STATE_STENCIL_SET;
    ctr_gpu_ocean_draw(&ns, m_shimmer, &od, -1, m_near_verts, m_near_indices, 0,
                       m_near_index_count);
    m_triangles += m_near_index_count / 3;
  }

  // (AI-assisted) The far ocean. On the PS2, rings of coarser ocean reach the horizon; the 3DS
  // draws the near ocean only (up to the draw distance), and the gap to the horizon showed the
  // clear color (black at night). An approximation: a flat ring in the ocean's far color, from
  // inside the near ocean's edge out to 8 km (within a pixel of the horizon), a little below the
  // water so the near ocean wins where both are. Depth tested: islands still cover it.
  if (!(p.flags & 1)) {
    const float r0 = draw_dist > 0 ? draw_dist * 0.8f : 400.f * 4096.f;
    const float r1 = 8000.f * 4096.f;
    const float y = p.start[1] - 0.25f * 4096.f - drop;
    auto color = [](float c) { return (u8)std::clamp(c, 0.f, 255.f); };
    const u8 cr = color(p.far_color[0]), cg = color(p.far_color[1]), cb = color(p.far_color[2]);
    // with the game's fog, the outer edge takes the fog color (GS units: half the register's)
    u8 fr = cr, fg = cg, fb = cb;
    if (settings.fog) {
      fr = rs.fog_color[0] / 2;
      fg = rs.fog_color[1] / 2;
      fb = rs.fog_color[2] / 2;
    }
    auto vertex = [&](float angle, float radius, u8 r, u8 g, u8 b) {
      // relative to the camera (the ring is centered on it)
      const double v[3] = {radius * std::cos(angle), (double)y - cam.trans[1],
                           radius * std::sin(angle)};
      float c[4];
      for (int k = 0; k < 4; k++) {
        double t = R[0][k] * v[0] + R[1][k] * v[1] + R[2][k] * v[2];
        if (k < 3) {
          t += R[3][k];
        }
        c[k] = (float)-t;
      }
      ctr_clip_vertex o;
      memset(&o, 0, sizeof(o));
      o.x = c[0];
      o.y = c[1] * (512.f / 448.f);  // Jak 1 scissor adjust, as the mesh matrix below
      o.z = c[2];
      o.w = c[3];
      o.r = r;
      o.g = g;
      o.b = b;
      o.a = 0x80;
      return o;
    };
    constexpr int kSegments = 16;
    ctr_clip_vertex ring[kSegments * 6];
    int n = 0;
    for (int i = 0; i < kSegments; i++) {
      const float a0 = 6.2831853f * i / kSegments, a1 = 6.2831853f * (i + 1) / kSegments;
      const ctr_clip_vertex p0 = vertex(a0, r0, cr, cg, cb), p1 = vertex(a1, r0, cr, cg, cb);
      const ctr_clip_vertex q0 = vertex(a0, r1, fr, fg, fb), q1 = vertex(a1, r1, fr, fg, fb);
      ring[n++] = p0;
      ring[n++] = q0;
      ring[n++] = q1;
      ring[n++] = p0;
      ring[n++] = q1;
      ring[n++] = p1;
    }
    ctr_draw_state far;
    memset(&far, 0, sizeof(far));
    far.tex = -1;
    far.blend = CTR_BLEND_OFF;
    far.atest = CTR_TEST_ALWAYS;
    far.ztest = CTR_TEST_GEQUAL;
    far.zwrite = 1;
    far.flags = waves_on ? CTR_STATE_STENCIL_OUT : 0;
    ctr_gpu_draw_clip(&far, ring, n);
  }

  ctr_ocean_draw fd;
  if (waves_on) {
    st.flags = CTR_STATE_STENCIL_OUT;
    ctr_gpu_prepare_mesh_matrix(m, &fd.clip);
  }
  for (const auto& tile : m_tiles) {
    const float s[4] = {tile.center[0] + p.start[0], p.start[1], tile.center[2] + p.start[2],
                        tile.radius};
    if (!sphere_in_view(s, cam.planes)) {
      continue;
    }
    if (draw_dist > 0) {
      const float dx = s[0] - cam.trans[0], dy = s[1] - cam.trans[1], dz = s[2] - cam.trans[2];
      const float lim = draw_dist + s[3];
      if (dx * dx + dy * dy + dz * dz > lim * lim) {
        continue;
      }
    }
    if (waves_on) {
      ocean_rows(fd, kScale, 0, 0, tile.center[0], tile.center[2]);
      ctr_gpu_ocean_draw(&st, m_shimmer, &fd, m_mesh, nullptr, nullptr, (int)tile.first_index,
                         (int)tile.index_count);
    } else {
      ctr_gpu_draw_mesh(&st, m, m_mesh, (int)tile.first_index, (int)tile.index_count);
    }
    m_tiles_drawn++;
    m_triangles += (int)tile.index_count / 3;
  }
  if (waves_on) {
    ctr_gpu_ocean_end();
  }
}
