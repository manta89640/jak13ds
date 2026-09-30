/*!
 * @file CtrLevel.cpp
 * (AI-assisted)
 * See CtrLevel.h. The camera math follows the PC tfrag renderer
 * (make_new_cam_mat and tfrag3.vert in game/graphics/opengl_renderer).
 */

#include "CtrLevel.h"

#include "game/graphics/ctr/CtrSettings.h"

#include <algorithm>
#include <array>
#include <cmath>
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
  if (m_common) {
    unload(*m_common);
  }
}

namespace {
/*!
 * Reads the pieces of a .c3l file as they are needed. A level file is ~10 MB: reading it in one
 * piece needs a free block of memory that big, which the 3DS heap often doesn't have by the time
 * a level loads (std::bad_alloc, and the level has no background).
 */
class C3lReader {
 public:
  explicit C3lReader(const fs::path& path) : m_f(fopen(path.string().c_str(), "rb")) {
    if (m_f) {
      setvbuf(m_f, nullptr, _IOFBF, 64 * 1024);
      fseek(m_f, 0, SEEK_END);
      m_size = ftell(m_f);
      fseek(m_f, 0, SEEK_SET);
    }
  }
  ~C3lReader() {
    if (m_f) {
      fclose(m_f);
    }
  }
  bool ok() const { return m_f && !m_error; }
  long size() const { return m_size; }
  bool read(u32 offset, void* dst, size_t bytes) {
    if (!ok() || (long)offset + (long)bytes > m_size) {
      m_error = true;
      return false;
    }
    if (bytes == 0) {
      return true;
    }
    if (m_pos != (long)offset && fseek(m_f, offset, SEEK_SET) != 0) {
      m_error = true;
      return false;
    }
    if (fread(dst, 1, bytes, m_f) != bytes) {
      m_error = true;
      return false;
    }
    m_pos = (long)offset + (long)bytes;
    return true;
  }
  template <typename T>
  bool read_array(u32 offset, size_t count, std::vector<T>* out) {
    out->resize(count);
    return read(offset, out->data(), count * sizeof(T));
  }

 private:
  FILE* m_f;
  long m_size = 0;
  long m_pos = 0;
  bool m_error = false;
};
}  // namespace

bool CtrLevels::load(const std::string& name, CtrLevelData* out) {
  auto path = file_util::get_jak_project_dir() / "out" / "jak1" / "c3l" / (name + ".c3l");
  if (!fs::exists(path)) {
    return false;
  }
  Timer timer;
  bool ok = false;
  long file_size = 0;
  try {
    ok = load_file(path, name, out, &file_size);
  } catch (std::exception& e) {
    lg::error("[ctr] failed to load {}: {}", path.string(), e.what());
  }
  if (!ok) {
    unload(*out);
    return false;
  }
  int failed = 0;
  for (int t : out->textures) {
    failed += t < 0;
  }
  for (size_t i = 0; i < out->meshes.size(); i++) {
    failed += out->meshes[i] < 0 && out->chunks[i].draw_count > 0;
  }
  for (auto& m : out->merc_models) {
    failed += m.mesh < 0;
  }
  lg::info("[ctr] loaded {} ({} chunks, {} merc models, {} textures, {} KB) in {:.0f} ms", name,
           out->chunks.size(), out->merc_models.size(), out->textures.size(), file_size / 1024,
           timer.getMs());
  if (failed) {
    lg::error("[ctr] {}: {} textures/meshes could not be created (out of GPU memory?)", name,
              failed);
  }
  return true;
}

bool CtrLevels::load_file(const fs::path& path,
                          const std::string& name,
                          CtrLevelData* out,
                          long* file_size) {
  C3lReader f(path);
  c3l::Header hdr;
  if (!f.read(0, &hdr, sizeof(hdr))) {
    lg::error("[ctr] failed to read {}", path.string());
    return false;
  }
  *file_size = f.size();
  if (memcmp(hdr.magic, c3l::kMagic, 4) || hdr.version != c3l::kVersion) {
    lg::error("[ctr] {}: not a C3L v{} file", path.string(), c3l::kVersion);
    return false;
  }
  out->name = name;
  if (!f.read_array(hdr.chunks_offset, hdr.num_chunks, &out->chunks) ||
      !f.read_array(hdr.draw_data_offset, hdr.draw_data_size / sizeof(c3l::Draw), &out->draws)) {
    lg::error("[ctr] {}: truncated file", path.string());
    return false;
  }

  std::vector<c3l::Texture> texs;
  if (!f.read_array(hdr.textures_offset, hdr.num_textures, &texs)) {
    lg::error("[ctr] {}: truncated file", path.string());
    return false;
  }
  std::vector<u8> texels;
  for (auto& t : texs) {
    texels.resize(t.data_size);
    int handle = -1;
    if (f.read(t.data_offset, texels.data(), t.data_size)) {
      handle = ctr_gpu_tex_create_tiled(t.w, t.h, t.format, texels.data(), t.data_size);
    }
    out->textures.push_back(handle);
  }

  // all indices at once (~1 MB): reading them chunk by chunk between the vertex reads would seek
  // back and forth, and every seek throws away the read buffer
  std::vector<u16> indices;
  if (!f.read_array(hdr.index_data_offset, hdr.index_data_size / sizeof(u16), &indices)) {
    lg::error("[ctr] {}: truncated file", path.string());
    return false;
  }
  std::vector<c3l::Vertex> verts;
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
    int mesh = -1;
    if (end <= indices.size() &&
        f.read_array(hdr.vertex_data_offset + ch.first_vertex * sizeof(c3l::Vertex),
                     ch.vertex_count, &verts)) {
      mesh = ctr_gpu_mesh_create(verts.data(), ch.vertex_count, indices.data() + first,
                                 end - first);
    }
    out->meshes.push_back(mesh);
    // make the draws relative to the mesh's index buffer
    for (u32 d = ch.first_draw; d < ch.first_draw + ch.draw_count; d++) {
      out->draws[d].first_index -= first;
    }
  }
  // bounds of the level (for the "seen from another level" mode)
  {
    bool first = true;
    for (auto& ch : out->chunks) {
      if (ch.lod_tier == 3) {
        out->has_lowres = true;
      }
      if (ch.lod_tier > 1) {
        continue;
      }
      for (int c = 0; c < 3; c++) {
        const float lo = ch.bsphere[c] - ch.bsphere[3], hi = ch.bsphere[c] + ch.bsphere[3];
        out->bbox_min[c] = first ? lo : std::min(out->bbox_min[c], lo);
        out->bbox_max[c] = first ? hi : std::max(out->bbox_max[c], hi);
      }
      first = false;
    }
  }
  // GPU state per draw, so drawing doesn't decode the draw modes every frame
  out->draw_states.reserve(out->draws.size());
  for (auto& d : out->draws) {
    DrawMode mode;
    mode.as_int() = d.mode;
    out->draw_states.push_back(ctr_state_from_draw_mode(
        mode, d.texture < out->textures.size() ? out->textures[d.texture] : -1));
  }
  // merc models: one skinned mesh per model
  if (hdr.num_merc_models) {
    std::vector<c3l::MercModel> models;
    std::vector<c3l::MercDraw> mdraws;
    std::vector<u16> mindices;
    if (!f.read_array(hdr.merc_models_offset, hdr.num_merc_models, &models) ||
        !f.read_array(hdr.merc_draw_offset, hdr.merc_draw_size / sizeof(c3l::MercDraw),
                      &mdraws) ||
        !f.read_array(hdr.merc_index_offset, hdr.merc_index_size / sizeof(u16), &mindices)) {
      lg::error("[ctr] {}: truncated file", path.string());
      return false;
    }
    // blend shapes
    if (hdr.merc_blerc_offset && hdr.merc_blerc_size >= sizeof(c3l::MercBlercHeader)) {
      c3l::MercBlercHeader bh;
      u32 off = hdr.merc_blerc_offset;
      if (f.read(off, &bh, sizeof(bh))) {
        off += sizeof(bh);
        if (f.read_array(off, bh.num_vertices, &out->blerc_verts)) {
          off += bh.num_vertices * sizeof(c3l::MercBlercVertex);
          if (f.read_array(off, bh.num_targets, &out->blerc_targets)) {
            off += bh.num_targets * sizeof(c3l::MercBlercTarget);
            f.read_array(off, bh.num_dests, &out->blerc_dests);
          }
        }
      }
      if (out->blerc_dests.size() != bh.num_dests) {
        lg::warn("[ctr] {}: bad blend shape data", path.string());
        out->blerc_verts.clear();
        out->blerc_targets.clear();
        out->blerc_dests.clear();
      }
    }
    std::vector<c3l::MercVertex> mverts;
    for (auto& m : models) {
      CtrMercModelData md;
      md.name = std::string(m.name, strnlen(m.name, sizeof(m.name)));
      md.scale = m.scale;
      if (m.blerc_first + m.blerc_count <= out->blerc_verts.size()) {
        md.blerc_first = m.blerc_first;
        md.blerc_count = m.blerc_count;
      }
      if (m.first_draw + m.draw_count > mdraws.size()) {
        continue;
      }
      md.draws.assign(mdraws.begin() + m.first_draw,
                      mdraws.begin() + m.first_draw + m.draw_count);
      if (md.draws.empty()) {
        continue;
      }
      u32 first = UINT32_MAX, end = 0;
      for (auto& d : md.draws) {
        first = std::min<u32>(first, d.first_index);
        end = std::max<u32>(end, d.first_index + d.index_count);
      }
      for (auto& d : md.draws) {
        d.first_index -= first;
      }
      if (end <= mindices.size() &&
          f.read_array(hdr.merc_vertex_offset + m.first_vertex * sizeof(c3l::MercVertex),
                       m.vertex_count, &mverts)) {
        md.mesh = ctr_gpu_skinned_mesh_create(mverts.data(), m.vertex_count,
                                              mindices.data() + first, end - first);
      }
      out->merc_models.push_back(std::move(md));
    }
  }
  return true;
}

void CtrLevels::process_pending_loads(u64 frame) {
  load_common_now();
  for (const auto& name : m_pending_loads) {
    if (m_levels.count(name)) {
      continue;
    }
    auto lev = std::make_unique<CtrLevelData>();
    if (!load(name, lev.get())) {
      lg::warn("[ctr] no background for level {} (out/jak1/c3l/{}.c3l)", name, name);
      m_missing[name] = true;
      continue;
    }
    lev->last_used_frame = frame;
    m_levels.emplace(name, std::move(lev));
    rebuild_merc_index();
  }
  m_pending_loads.clear();
}

void CtrLevels::unload(CtrLevelData& lev) {
  for (int t : lev.textures) {
    ctr_gpu_tex_delete(t);
  }
  for (int m : lev.meshes) {
    ctr_gpu_mesh_delete(m);
  }
  for (auto& m : lev.merc_models) {
    ctr_gpu_mesh_delete(m.mesh);
  }
  lev.merc_models.clear();
  lev.textures.clear();
  lev.meshes.clear();
}

void CtrLevels::load_common() {
  // loaded with the levels, on the render thread before a frame: GPU transfers from the thread
  // that runs gk's init could overlap the console's last buffer swaps
  m_common_wanted = true;
}

void CtrLevels::load_common_now() {
  if (m_common || !m_common_wanted) {
    return;
  }
  m_common_wanted = false;
  auto lev = std::make_unique<CtrLevelData>();
  if (load("GAME", lev.get())) {
    m_common = std::move(lev);
    rebuild_merc_index();
  } else {
    lg::warn("[ctr] no common models (out/jak1/c3l/GAME.c3l): Jak won't be drawn");
  }
}

void CtrLevels::rebuild_merc_index() {
  m_merc_index.clear();
  if (m_common) {
    for (size_t i = 0; i < m_common->merc_models.size(); i++) {
      m_merc_index[m_common->merc_models[i].name] = {m_common.get(), (int)i};
    }
  }
  for (auto& [name, lev] : m_levels) {
    for (size_t i = 0; i < lev->merc_models.size(); i++) {
      m_merc_index[lev->merc_models[i].name] = {lev.get(), (int)i};
    }
  }
}

const CtrMercModelData* CtrLevels::find_merc_model(const std::string& name,
                                                   const CtrLevelData** lev) {
  auto it = m_merc_index.find(name);
  if (it == m_merc_index.end()) {
    return nullptr;
  }
  *lev = it->second.first;
  return &it->second.first->merc_models[it->second.second];
}

CtrLevelData* CtrLevels::get(const std::string& name, u64 frame) {
  auto it = m_levels.find(name);
  if (it == m_levels.end()) {
    // loaded before the next frame (process_pending_loads), outside of a GPU frame: textures go
    // to VRAM through copies that can't be queued in the middle of a frame
    if (!m_missing.count(name) &&
        std::find(m_pending_loads.begin(), m_pending_loads.end(), name) == m_pending_loads.end()) {
      m_pending_loads.push_back(name);
    }
    return nullptr;
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
      rebuild_merc_index();
    } else {
      ++it;
    }
  }
  // Load the wanted levels before the next frame, not only when their background is drawn: a level
  // can be loaded just for its models. The intro loads the "intro" level (Gol and Maia) next to
  // misty with display mode special, so it never draws a background and was never loaded.
  for (auto& n : names) {
    if (n.empty() || m_levels.count(n) || m_missing.count(n) ||
        std::find(m_pending_loads.begin(), m_pending_loads.end(), n) != m_pending_loads.end()) {
      continue;
    }
    m_pending_loads.push_back(n);
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

}  // namespace

ctr_draw_state ctr_state_from_draw_mode(DrawMode mode, int tex) {
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
    draw_level(*lev, pc_data.camera, rs);
  }
  if (rs.log_now && m_level_draws) {
    lg::debug("[ctr] {}: {} of {} level draws seen from another level", m_name, m_far_levels,
              m_level_draws);
    m_far_levels = m_level_draws = 0;
  }
}

void CtrTfragRenderer::draw_level(CtrLevelData& lev,
                                  const CtrBackgroundCamera& cam,
                                  const CtrRenderState& rs) {
  const CtrSettings& settings = ctr_settings();
  auto R = make_cam_mat(cam.rot, cam.perspective, cam.fog.x(), cam.hvdf_off.z());
  // fog (ctr_gpu_set_mesh_fog): the game's (tfrag3.vert), and towards the draw distance
  const float draw_dist = settings.draw_distance * 4096.f;
  const float lod_dist = settings.lod_distance * 4096.f;
  // camera outside the level's bounds by more than far_level_distance: the level is only seen
  // from a neighbouring one
  bool far_level = false;
  if (settings.far_level_distance > 0) {
    float d2 = 0;
    for (int c = 0; c < 3; c++) {
      const float v = cam.trans[c];
      const float o = v < lev.bbox_min[c] ? lev.bbox_min[c] - v
                                         : (v > lev.bbox_max[c] ? v - lev.bbox_max[c] : 0.f);
      d2 += o * o;
    }
    const float m = settings.far_level_distance * 4096.f;
    far_level = d2 > m * m;
  }
  m_far_levels += far_level;
  m_level_draws++;
  {
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
  // Jak 1 scissor adjust (tfrag3.vert: y *= 512 / 448)
  constexpr float kYScale = 512.f / 448.f;
  int drawn = 0;
  for (size_t ci = 0; ci < lev.chunks.size(); ci++) {
    const auto& ch = lev.chunks[ci];
    if (lev.meshes[ci] < 0 || !sphere_in_view(ch.bsphere, cam.planes)) {
      continue;
    }
    if (far_level) {
      // only the level's low resolution version (or the coarse one), no tie
      if (ch.lod_tier != (lev.has_lowres ? 3u : 2u)) {
        continue;
      }
    } else if (ch.lod_tier == 3) {
      continue;
    } else if (ch.lod_tier) {
      // detailed version up close, coarse version further away
      const float dx = ch.lod_center[0] - cam.trans[0], dy = ch.lod_center[1] - cam.trans[1],
                  dz = ch.lod_center[2] - cam.trans[2];
      const bool near = lod_dist <= 0 || dx * dx + dy * dy + dz * dz < lod_dist * lod_dist;
      if (near != (ch.lod_tier == 1)) {
        continue;
      }
    }
    {
      // draw distance; small objects only up close
      float max_dist = draw_dist;
      if (ch.max_dist > 0) {
        const float d = ch.max_dist * settings.detail_scale;
        max_dist = max_dist > 0 ? std::min(max_dist, d) : d;
      }
      if (max_dist > 0) {
        const float dx = ch.bsphere[0] - cam.trans[0], dy = ch.bsphere[1] - cam.trans[1],
                    dz = ch.bsphere[2] - cam.trans[2];
        const float lim = max_dist + ch.bsphere[3];
        if (dx * dx + dy * dy + dz * dz > lim * lim) {
          continue;
        }
      }
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
      ctr_gpu_draw_mesh(&lev.draw_states[di], m, lev.meshes[ci], dr.first_index,
                        dr.index_count);
    }
    drawn++;
  }
  (void)drawn;
}
