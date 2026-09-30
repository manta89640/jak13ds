/*!
 * @file CtrLevel.cpp
 * (AI-assisted)
 * See CtrLevel.h. The camera math follows the PC tfrag renderer
 * (make_new_cam_mat and tfrag3.vert in game/graphics/opengl_renderer).
 */

#include "CtrLevel.h"

#include <array>
#include <cstring>

#include "common/dma/dma.h"
#include "common/dma/gs.h"
#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/util/Timer.h"

#include "game/graphics/ctr/ctr_gpu.h"

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

CtrLevels::~CtrLevels() {
  for (auto& [name, lev] : m_levels) {
    unload(*lev);
  }
}

bool CtrLevels::load(const std::string& name, CtrLevelData* out) {
  auto path = file_util::get_jak_project_dir() / "out" / "jak1" / "c3l" / (name + ".c3l");
  if (!fs::exists(path)) {
    return false;
  }
  Timer timer;
  std::vector<u8> file;
  try {
    file = file_util::read_binary_file(path);
  } catch (std::exception& e) {
    lg::error("[ctr] failed to read {}: {}", path.string(), e.what());
    return false;
  }
  if (file.size() < sizeof(c3l::Header)) {
    return false;
  }
  c3l::Header hdr;
  memcpy(&hdr, file.data(), sizeof(hdr));
  if (memcmp(hdr.magic, c3l::kMagic, 4) || hdr.version != c3l::kVersion) {
    lg::error("[ctr] {}: not a C3L v{} file", path.string(), c3l::kVersion);
    return false;
  }
  out->name = name;
  out->chunks.resize(hdr.num_chunks);
  memcpy(out->chunks.data(), file.data() + hdr.chunks_offset,
         hdr.num_chunks * sizeof(c3l::Chunk));
  out->draws.resize(hdr.draw_data_size / sizeof(c3l::Draw));
  memcpy(out->draws.data(), file.data() + hdr.draw_data_offset, hdr.draw_data_size);

  std::vector<c3l::Texture> texs(hdr.num_textures);
  memcpy(texs.data(), file.data() + hdr.textures_offset, hdr.num_textures * sizeof(c3l::Texture));
  for (auto& t : texs) {
    out->textures.push_back(ctr_gpu_tex_create_tiled(t.w, t.h, t.format,
                                                     file.data() + t.data_offset, t.data_size));
  }

  const auto* verts = (const c3l::Vertex*)(file.data() + hdr.vertex_data_offset);
  const auto* indices = (const u16*)(file.data() + hdr.index_data_offset);
  for (auto& ch : out->chunks) {
    // the chunk's draws cover a contiguous range of the index data
    u32 first = UINT32_MAX, end = 0;
    for (u32 d = ch.first_draw; d < ch.first_draw + ch.draw_count; d++) {
      first = std::min<u32>(first, out->draws[d].first_index);
      end = std::max<u32>(end, out->draws[d].first_index + out->draws[d].index_count);
    }
    if (first == UINT32_MAX) {
      out->meshes.push_back(-1);
      continue;
    }
    int mesh = ctr_gpu_mesh_create(verts + ch.first_vertex, ch.vertex_count, indices + first,
                                   end - first);
    out->meshes.push_back(mesh);
    // make the draws relative to the mesh's index buffer
    for (u32 d = ch.first_draw; d < ch.first_draw + ch.draw_count; d++) {
      out->draws[d].first_index -= first;
    }
  }
  lg::info("[ctr] loaded {} ({} chunks, {} textures, {} KB) in {:.0f} ms", name,
           out->chunks.size(), out->textures.size(), file.size() / 1024, timer.getMs());
  return true;
}

void CtrLevels::unload(CtrLevelData& lev) {
  for (int t : lev.textures) {
    ctr_gpu_tex_delete(t);
  }
  for (int m : lev.meshes) {
    ctr_gpu_mesh_delete(m);
  }
  lev.textures.clear();
  lev.meshes.clear();
}

CtrLevelData* CtrLevels::get(const std::string& name, u64 frame) {
  auto it = m_levels.find(name);
  if (it == m_levels.end()) {
    if (m_missing.count(name)) {
      return nullptr;
    }
    auto lev = std::make_unique<CtrLevelData>();
    if (!load(name, lev.get())) {
      lg::warn("[ctr] no background for level {} (out/jak1/c3l/{}.c3l)", name, name);
      m_missing[name] = true;
      return nullptr;
    }
    it = m_levels.emplace(name, std::move(lev)).first;
  }
  it->second->last_used_frame = frame;
  return it->second.get();
}

void CtrLevels::set_wanted(const std::vector<std::string>& names) {
  for (auto it = m_levels.begin(); it != m_levels.end();) {
    bool wanted = false;
    for (auto& n : names) {
      wanted |= n == it->first;
    }
    if (!wanted) {
      lg::info("[ctr] unloading {}", it->first);
      unload(*it->second);
      it = m_levels.erase(it);
    } else {
      ++it;
    }
  }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

namespace {

struct TfragPcPortData {
  CtrBackgroundCamera camera;
  char level_name[32];
};
static_assert(sizeof(TfragPcPortData) == 16 * 25);

bool looks_like_tfragment_dma(const DmaFollower& follow) {
  return follow.current_tag_vifcode0().kind == VifCode::Kind::STCYCL;
}

bool looks_like_tfrag_init(const DmaFollower& follow) {
  return follow.current_tag_vifcode0().kind == VifCode::Kind::NOP &&
         follow.current_tag_vifcode1().kind == VifCode::Kind::DIRECT &&
         follow.current_tag_vifcode1().immediate == 2;
}

// same as make_new_cam_mat (background_common.cpp)
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

ctr_draw_state state_from_draw_mode(DrawMode mode, int tex) {
  ctr_draw_state st;
  memset(&st, 0, sizeof(st));
  st.tex = tex;
  st.tcc = 1;
  st.filter = mode.get_filt_enable();
  st.clamp_s = mode.get_clamp_s_enable();
  st.clamp_t = mode.get_clamp_t_enable();
  st.blend = CTR_BLEND_OFF;
  if (mode.get_ab_enable()) {
    switch (mode.get_alpha_blend()) {
      case DrawMode::AlphaBlend::SRC_DST_SRC_DST:
        st.blend = CTR_BLEND_ALPHA;
        break;
      case DrawMode::AlphaBlend::SRC_0_SRC_DST:
        st.blend = CTR_BLEND_ADD;
        break;
      case DrawMode::AlphaBlend::SRC_0_FIX_DST:
        st.blend = CTR_BLEND_ONE_ONE;
        break;
      case DrawMode::AlphaBlend::SRC_DST_FIX_DST:
        st.blend = CTR_BLEND_FIX;
        st.fix = 0x40;
        break;
      case DrawMode::AlphaBlend::ZERO_SRC_SRC_DST:
        st.blend = CTR_BLEND_SUB;
        break;
      case DrawMode::AlphaBlend::SRC_0_DST_DST:
        st.blend = CTR_BLEND_ADD_DST_A;
        break;
      default:
        break;
    }
  }
  if (mode.get_at_enable() && mode.get_alpha_test() == DrawMode::AlphaTest::GEQUAL) {
    st.atest = CTR_TEST_GEQUAL;
    st.aref = mode.get_aref();
  } else if (mode.get_at_enable() && mode.get_alpha_test() == DrawMode::AlphaTest::NEVER) {
    st.atest = CTR_TEST_ALWAYS;  // afail tricks: just draw
  } else {
    st.atest = CTR_TEST_ALWAYS;
  }
  if (mode.get_zt_enable()) {
    switch (mode.get_depth_test()) {
      case GsTest::ZTest::NEVER:
        st.ztest = CTR_TEST_NEVER;
        break;
      case GsTest::ZTest::ALWAYS:
        st.ztest = CTR_TEST_ALWAYS;
        break;
      case GsTest::ZTest::GEQUAL:
        st.ztest = CTR_TEST_GEQUAL;
        break;
      case GsTest::ZTest::GREATER:
        st.ztest = CTR_TEST_GREATER;
        break;
    }
  } else {
    st.ztest = CTR_TEST_ALWAYS;
  }
  st.zwrite = mode.get_depth_write_enable();
  return st;
}
}  // namespace

CtrTfragRenderer::CtrTfragRenderer(std::string name, int id, CtrLevels* levels)
    : CtrBucketRenderer(std::move(name), id), m_levels(levels) {}

void CtrTfragRenderer::render(DmaFollower& dma, CtrRenderState& rs) {
  TfragPcPortData pc_data;
  bool have_data = false;
  while (dma.current_tag_offset() != rs.next_bucket) {
    if (looks_like_tfrag_init(dma)) {
      // setup test, matrix 0, matrix 1, data, mscal (see TFragment::handle_initialization)
      for (int i = 0; i < 5; i++) {
        dma.read_and_advance();
      }
      auto pc = dma.read_and_advance();
      if (pc.size_bytes == sizeof(TfragPcPortData)) {
        memcpy(&pc_data, pc.data, sizeof(pc_data));
        pc_data.level_name[11] = '\0';
        have_data = true;
      }
      dma.read_and_advance();  // double buffer setup
      while (looks_like_tfragment_dma(dma) && dma.current_tag_offset() != rs.next_bucket) {
        dma.read_and_advance();
      }
    } else {
      dma.read_and_advance();
    }
  }
  if (!have_data) {
    return;
  }
  CtrLevelData* lev = m_levels->get(pc_data.level_name, rs.frame_idx);
  if (lev) {
    draw_level(*lev, pc_data.camera);
  }
}

void CtrTfragRenderer::draw_level(CtrLevelData& lev, const CtrBackgroundCamera& cam) {
  auto R = make_cam_mat(cam.rot, cam.perspective, cam.fog.x(), cam.hvdf_off.z());
  // Jak 1 scissor adjust (tfrag3.vert: y *= 512 / 448)
  constexpr float kYScale = 512.f / 448.f;
  int drawn = 0;
  for (size_t ci = 0; ci < lev.chunks.size(); ci++) {
    const auto& ch = lev.chunks[ci];
    if (lev.meshes[ci] < 0 || !sphere_in_view(ch.bsphere, cam.planes)) {
      continue;
    }
    // clip = -(R * (origin + q * scale - cam_trans)) (tfrag3.vert), as a matrix on (q, 1).
    // The translation is done in double: world coordinates are large.
    double d[3];
    for (int i = 0; i < 3; i++) {
      d[i] = (double)ch.origin[i] - (double)cam.trans[i];
    }
    float m[16];
    for (int c = 0; c < 4; c++) {
      double t = 0;
      for (int i = 0; i < 3; i++) {
        m[4 * c + i] = -R[i][c] * ch.scale;
        t += (double)R[i][c] * d[i];
      }
      if (c < 3) {
        t += R[3][c];
      }
      m[4 * c + 3] = (float)-t;
    }
    for (int i = 0; i < 4; i++) {
      m[4 + i] *= kYScale;
    }
    for (u32 di = ch.first_draw; di < ch.first_draw + ch.draw_count; di++) {
      const auto& dr = lev.draws[di];
      int tex = dr.texture == 0xffff ? -1 : lev.textures[dr.texture];
      DrawMode mode;
      mode.as_int() = dr.mode;
      ctr_draw_state st = state_from_draw_mode(mode, tex);
      ctr_gpu_draw_mesh(&st, m, lev.meshes[ci], dr.first_index, dr.index_count);
    }
    drawn++;
  }
  (void)drawn;
}
