/*!
 * @file CtrMerc.cpp
 * (AI-assisted)
 * See CtrMerc.h. The DMA parsing follows Merc2::handle_all_dma / handle_pc_model, and the
 * projection is merc2.vert's (bones -> camera space -> perspective -> GS screen), folded into
 * one matrix.
 */

#include "CtrMerc.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "common/dma/dma.h"
#include "common/log/log.h"

#include "game/graphics/ctr/CtrEye.h"
#include "game/graphics/ctr/CtrLevel.h"
#include "game/graphics/ctr/CtrSettings.h"
#include "game/graphics/ctr/ctr_gpu.h"

namespace {

struct VuLights {
  math::Vector3f direction0;
  u32 w0;
  math::Vector3f direction1;
  u32 w1;
  math::Vector3f direction2;
  u32 w2;
  math::Vector4f color0;
  math::Vector4f color1;
  math::Vector4f color2;
  math::Vector4f ambient;
};
static_assert(sizeof(VuLights) == 7 * 16);

using MercMat = CtrMercMat;
static_assert(sizeof(MercMat) == 7 * 16);

struct PcMercFlags {
  u64 enable_mask;
  u64 ignore_alpha_mask;
  u8 effect_count;
  u8 bitflags;
};

bool tag_is_nothing_next(const DmaFollower& dma) {
  return dma.current_tag().kind == DmaTag::Kind::NEXT && dma.current_tag().qwc == 0 &&
         dma.current_tag_vif0() == 0 && dma.current_tag_vif1() == 0;
}

// row-major 4x4 multiply: out = a * b
void mul44(float* out, const float* a, const float* b) {
  float r[16];
  for (int i = 0; i < 4; i++) {
    for (int j = 0; j < 4; j++) {
      float acc = 0;
      for (int k = 0; k < 4; k++) {
        acc += a[4 * i + k] * b[4 * k + j];
      }
      r[4 * i + j] = acc;
    }
  }
  memcpy(out, r, sizeof(r));
}
}  // namespace

CtrMercRenderer::CtrMercRenderer(std::string name, int id, CtrLevels* levels)
    : CtrBucketRenderer(std::move(name), id), m_levels(levels) {}

void CtrMercRenderer::handle_setup(const DmaTransfer& setup) {
  // 10 qw: 1 qw of vifcodes, 8 qw of low memory, 1 qw of vifcodes
  if (setup.size_bytes != 10 * 16) {
    m_have_camera = false;
    return;
  }
  const u8* low = setup.data + 16;
  // low memory: tri strip tag, ad gif tag, hvdf offset, perspective[4], fog
  memcpy(&m_hvdf_offset, low + 32, 16);
  memcpy(m_perspective, low + 48, 64);
  memcpy(&m_fog, low + 112, 16);

  // merc2.vert: t = perspective * v, then the GS screen mapping (hvdf, fog constant), as a
  // linear map of t. v = (camera xyz, -1): the bone matrices give -X * p with w = -1.
  const float fx = m_fog.x();
  constexpr float k = 512.f / 448.f;  // Jak 1 scissor adjust
  const float L[16] = {fx / 256.f, 0, 0, (m_hvdf_offset.x() - 2048.f) / 256.f,
                       0, -fx / 128.f * k, 0, -(m_hvdf_offset.y() - 2048.f) / 128.f * k,
                       0, 0, fx / 8388608.f, m_hvdf_offset.z() / 8388608.f - 1.f,
                       0, 0, 0, 1};
  memcpy(m_screen, L, sizeof(L));
  float P[16];
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      P[4 * r + c] = m_perspective[c][r];  // GL mat4: column c = perspective[c]
    }
  }
  // D = diag(1, 1, 1, -1)
  for (int r = 0; r < 4; r++) {
    P[4 * r + 3] = -P[4 * r + 3];
  }
  mul44(m_clip, L, P);
  m_have_camera = true;
}

void CtrMercRenderer::hud_clip(float* out, float y_scale) const {
  // HUD models (draw-bones-hud-merc): generic draws them with *math-camera* isometric instead of
  // the perspective matrix, columns (1 0 0 0) (0 y 0 0) (0 0 -1 0) (0 0 16777215-hvdf.z pfog0).
  // w is then constant: the model's coordinates are GS pixels around the screen center.
  const float iso[4][4] = {{1.f, 0, 0, 0},
                           {0, y_scale, 0, 0},
                           {0, 0, -1.f, 0},
                           {0, 0, 16777215.f - m_hvdf_offset.z(), m_fog.x()}};
  float P[16];
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      P[4 * r + c] = iso[c][r];
    }
    P[4 * r + 3] = -P[4 * r + 3];  // D, as in handle_setup
  }
  mul44(out, m_screen, P);
}

void CtrMercRenderer::snapshot_bones(DmaFollower& dma, CtrRenderState& rs) {
  m_snap.clear();
  m_snapshot_mode = true;
  render(dma, rs);
  m_snapshot_mode = false;
  m_snap_valid = true;
}

void CtrMercRenderer::render(DmaFollower& dma, CtrRenderState& rs) {
  m_snap_pos = 0;
  // same structure as Merc2::handle_all_dma, but without asserts: skip what we don't know
  if (dma.current_tag_offset() == rs.next_bucket) {
    return;
  }
  if (!ctr_settings().merc && !m_snapshot_mode) {
    while (dma.current_tag_offset() != rs.next_bucket) {
      dma.read_and_advance();
    }
    return;
  }
  dma.read_and_advance();  // jump to the merc dma
  if (dma.current_tag().kind == DmaTag::Kind::CALL || dma.current_tag_offset() == rs.next_bucket) {
    while (dma.current_tag_offset() != rs.next_bucket) {
      dma.read_and_advance();
    }
    return;
  }
  handle_setup(dma.read_and_advance());
  // jak 1: test register setup + empty transfer
  if (dma.current_tag_offset() != rs.next_bucket) {
    dma.read_and_advance();
  }
  if (dma.current_tag_offset() != rs.next_bucket) {
    dma.read_and_advance();
  }

  while (dma.current_tag_offset() != rs.next_bucket) {
    while (tag_is_nothing_next(dma) && dma.current_tag_offset() != rs.next_bucket) {
      dma.read_and_advance();
    }
    if (dma.current_tag_offset() == rs.next_bucket) {
      break;
    }
    if (dma.current_tag().kind == DmaTag::Kind::CALL) {
      for (int i = 0; i < 4 && dma.current_tag_offset() != rs.next_bucket; i++) {
        dma.read_and_advance();
      }
      continue;
    }
    auto init = dma.read_and_advance();
    while (init.vifcode1().kind == VifCode::Kind::PC_PORT) {
      handle_model(init, rs);
      for (int i = 0; i < 2 && dma.current_tag_offset() != rs.next_bucket; i++) {
        dma.read_and_advance();
      }
      if (dma.current_tag_offset() == rs.next_bucket) {
        break;
      }
      init = dma.read_and_advance();
    }
  }

  if (m_snapshot_mode) {
    return;
  }
  m_snap_valid = false;  // used up: the next frame needs a new snapshot
  if (rs.log_now && m_stats.models) {
    lg::debug("[ctr] {}: {} models ({} missing, {} with bad bones), {} draws", m_name,
              m_stats.models, m_stats.missing, m_stats.bad_bones, m_stats.draws);
    m_stats = Stats();
  }
}

void CtrMercRenderer::apply_blerc(const CtrLevelData& lev,
                                  const CtrMercModelData& model,
                                  const float* weights) {
  u8* verts = (u8*)ctr_gpu_mesh_vertices(model.mesh);
  if (!verts || model.scale <= 0.f) {
    return;
  }
  const float inv_scale = 1.f / model.scale;
  u32 lo = UINT32_MAX, hi = 0;
  for (u32 k = 0; k < model.blerc_count; k++) {
    const auto& bv = lev.blerc_verts[model.blerc_first + k];
    float p[3] = {bv.base[0], bv.base[1], bv.base[2]};
    for (u32 t = 0; t < bv.target_count && bv.first_target + t < lev.blerc_targets.size(); t++) {
      const auto& tg = lev.blerc_targets[bv.first_target + t];
      const float w = weights[tg.weight % c3l::kMercBlercWeights];
      p[0] += w * tg.offset[0];
      p[1] += w * tg.offset[1];
      p[2] += w * tg.offset[2];
    }
    s16 q[3];
    for (int c = 0; c < 3; c++) {
      q[c] = (s16)std::clamp((int)std::lround(p[c] * inv_scale), -32767, 32767);
    }
    for (u32 d = 0; d < bv.dest_count && bv.first_dest + d < lev.blerc_dests.size(); d++) {
      const u32 vi = lev.blerc_dests[bv.first_dest + d];
      memcpy(verts + sizeof(c3l::MercVertex) * vi, q, sizeof(q));  // pos is the first member
      lo = std::min(lo, vi);
      hi = std::max(hi, vi);
    }
  }
  if (lo <= hi) {
    ctr_gpu_mesh_flush(model.mesh, (int)(lo * sizeof(c3l::MercVertex)),
                       (int)((hi - lo + 1) * sizeof(c3l::MercVertex)));
  }
}

void CtrMercRenderer::handle_model(const DmaTransfer& init, CtrRenderState& rs) {
  const u8* input = init.data;
  // The bone matrices are not in the DMA data (they're in game memory that the next frame's
  // game logic rewrites while the render thread draws this one): snapshot_bones copies them on
  // the game thread, in the order of the models, and drawing reads that copy.
  {
    const u8* sl = input + 128 + sizeof(VuLights) + 16;
    const u32* ma = (const u32*)(sl + 128);
    int n = 0;
    while (n < 128 && sl[n] != 0xff) {
      n++;
    }
    if (m_snapshot_mode) {
      for (int b = 0; b < n; b++) {
        u32 addr;
        memcpy(&addr, &ma[b * 4], 4);
        MercMat m;
        memcpy(&m, rs.ee_mem + addr, sizeof(MercMat));
        m_snap.push_back(m);
      }
      return;
    }
    m_snap_first = m_snap_pos;
    m_snap_pos += n;
  }
  if (!m_have_camera) {
    return;
  }
  char name[128];
  memcpy(name, input, 128);
  name[127] = 0;
  input += 128;

  const CtrLevelData* lev = nullptr;
  const CtrMercModelData* model = m_levels->find_merc_model(name, &lev);
  m_stats.models++;
  if (!model) {
    m_stats.missing++;
    return;
  }

  VuLights lights;
  memcpy(&lights, input, sizeof(lights));
  input += sizeof(VuLights);
  input += 16;  // jak 1: uses-water

  // matrix slot string + GOAL addresses of the bone matrices
  static MercMat bones[128];
  const u8* slots = input;
  const u32* matrix_array = (const u32*)(input + 128);
  int i;
  for (i = 0; i < 128; i++) {
    if (slots[i] == 0xff) {
      break;
    }
    u32 addr;
    memcpy(&addr, &matrix_array[i * 4], 4);
    if (slots[i] < 128) {
      if (m_snap_valid && m_snap_first + i < m_snap.size()) {
        bones[slots[i]] = m_snap[m_snap_first + i];
      } else {
        memcpy(&bones[slots[i]], rs.ee_mem + addr, sizeof(MercMat));
      }
    }
  }
  input += 128 + 16 * i;
  PcMercFlags flags;
  memcpy(&flags, input, sizeof(flags));
  // HUD models (the orb and power cell icons...): isometric projection, see hud_clip
  float hud[16];
  const float* clip = m_clip;
  if (flags.bitflags & 8) {
    float y_scale;
    memcpy(&y_scale, input + 20, sizeof(y_scale));
    hud_clip(hud, y_scale);
    clip = hud;
  }

  // blend shapes (faces): the game's weights follow the flags (Merc2::handle_pc_model)
  if (model->blerc_count) {
    const bool uses_blerc = flags.bitflags & 4;
    if (uses_blerc || model->blerc_moved) {
      float weights[c3l::kMercBlercWeights] = {};
      if (uses_blerc) {
        memcpy(weights, input + 32, sizeof(weights));
      }
      apply_blerc(*lev, *model, weights);
      model->blerc_moved = uses_blerc;
    }
  }

  // Bone matrices that are garbage (seen in cutscenes whose streamed animation is missing) make
  // screen-filling triangles: skip the model instead.
  for (int b = 0; b < i; b++) {
    if (slots[b] >= 128) {
      continue;
    }
    const MercMat& m = bones[slots[b]];
    for (int r = 0; r < 4; r++) {
      for (int c = 0; c < 3; c++) {
        const float v = m.tmat[r][c];
        const float lim = r < 3 ? 64.f : 1e8f;
        if (!(std::abs(v) < lim)) {  // also catches NaN
          m_stats.bad_bones++;
          return;
        }
      }
    }
  }

  // the model's lights, for the skin shader's per vertex lighting (like merc2.vert)
  float light_data[28];
  {
    const float* src[7] = {lights.direction0.data(), lights.direction1.data(),
                           lights.direction2.data(), lights.color0.data(), lights.color1.data(),
                           lights.color2.data(), lights.ambient.data()};
    for (int i = 0; i < 7; i++) {
      for (int c = 0; c < 3; c++) {
        light_data[4 * i + c] = src[i][c];
      }
      light_data[4 * i + 3] = 0.f;
    }
  }

  // bones as 3x4 rows: camera = -(tmat[0] * x + tmat[1] * y + tmat[2] * z + tmat[3]),
  // with the model's quantization scale folded in
  float palette_rows[CTR_MAX_PALETTE * 12];
  for (const auto& draw : model->draws) {
    if (!(flags.enable_mask & (1ull << draw.effect))) {
      continue;
    }
    for (int p = 0; p < draw.palette_count && p < CTR_MAX_PALETTE; p++) {
      const MercMat& m = bones[draw.palette[p]];
      float* rows = &palette_rows[12 * p];
      for (int r = 0; r < 3; r++) {
        rows[4 * r + 0] = -m.tmat[0][r] * model->scale;
        rows[4 * r + 1] = -m.tmat[1][r] * model->scale;
        rows[4 * r + 2] = -m.tmat[2][r] * model->scale;
        rows[4 * r + 3] = -m.tmat[3][r];
      }
    }
    int tex = draw.texture < lev->textures.size() ? lev->textures[draw.texture] : -1;
    if (draw.eye_id != 0xff && m_eyes) {
      const int eye = m_eyes->texture(draw.eye_id);
      if (eye >= 0) {
        tex = eye;
      }
    }
    DrawMode mode;
    mode.as_int() = draw.mode;
    ctr_draw_state st = ctr_state_from_draw_mode(mode, tex);
    // Alpha like merc2.frag, not like the draw mode's alpha test: only (nearly) transparent pixels
    // are dropped (alpha < 0.128), the rest is blended. The draw modes of hair, eyes and many
    // objects ask for alpha >= 0x26, which throws away most of a hair texture (see-through, noisy
    // hair). Effects the game draws with ignore-alpha are opaque.
    if (flags.ignore_alpha_mask & (1ull << draw.effect)) {
      st.atest = CTR_TEST_ALWAYS;
      st.blend = CTR_BLEND_OFF;
    } else {
      st.atest = CTR_TEST_GEQUAL;
      st.aref = 17;  // x2 in the GPU state: 34 / 255
    }
    ctr_gpu_draw_skinned(&st, clip, palette_rows, draw.palette_count, light_data, model->mesh,
                         draw.first_index, draw.index_count);
    m_stats.draws++;
  }
}
