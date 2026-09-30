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
  lg::info("[ctr] ocean mesh: {} tiles, {} cells ({} shore), {} vertices, {} triangles{}",
           m_tiles.size(), cells, shore_cells, verts.size(), indices.size() / 3,
           m_mesh < 0 ? " (could not be created)" : "");
  return m_mesh >= 0;
}

void CtrOceanRenderer::update_texture(const u8* ee, u32 heights) {
  constexpr int N = kTexSize;
  float h[N * N];
  if (valid_addr(heights, sizeof(h))) {
    memcpy(h, ee + heights, sizeof(h));
  } else {
    memset(h, 0, sizeof(h));
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
  if (m_tex >= 0) {
    ctr_gpu_tex_delete(m_tex);
  }
  m_tex = ctr_gpu_tex_create(N, N, m_texels.data());
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
    if (draw_dist > 0 && pzw > 0) {
      const float start = draw_dist * settings.fog_start;
      fog1[0] = 1.f / pzw;
      fog1[1] = start;
      fog1[2] = 1.f / std::max(draw_dist - start, 1.f);
    }
    ctr_gpu_set_mesh_fog(fog0, fog1, rs.fog_color[0], rs.fog_color[1], rs.fog_color[2]);
  }
  // clip = -(R * (origin + q * scale - cam_trans)), see CtrTfragRenderer::draw_level
  const double origin[3] = {p.start[0], p.start[1], p.start[2]};
  double d[3];
  for (int i = 0; i < 3; i++) {
    d[i] = origin[i] - (double)cam.trans[i];
  }
  float m[16];
  for (int c = 0; c < 4; c++) {
    double t = 0;
    for (int i = 0; i < 3; i++) {
      m[4 * c + i] = -R[i][c] * kScale;
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

  ctr_draw_state st;
  memset(&st, 0, sizeof(st));
  st.tex = m_tex;
  st.tcc = 1;
  st.filter = 1;
  st.blend = CTR_BLEND_ALPHA;
  st.atest = CTR_TEST_ALWAYS;
  st.ztest = CTR_TEST_GEQUAL;
  st.zwrite = 1;
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
    ctr_gpu_draw_mesh(&st, m, m_mesh, (int)tile.first_index, (int)tile.index_count);
    m_tiles_drawn++;
    m_triangles += (int)tile.index_count / 3;
  }
}
