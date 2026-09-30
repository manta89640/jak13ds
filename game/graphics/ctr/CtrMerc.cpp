/*!
 * @file CtrMerc.cpp
 * (AI-assisted)
 * See CtrMerc.h. The DMA parsing follows Merc2::handle_all_dma / handle_pc_model, and the
 * projection is merc2.vert's (bones -> camera space -> perspective -> GS screen), folded into
 * one matrix.
 */

#include "CtrMerc.h"

#include <cstring>

#include "common/dma/dma.h"
#include "common/log/log.h"

#include "game/graphics/ctr/CtrLevel.h"
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

struct MercMat {
  math::Vector4f tmat[4];
  math::Vector4f nmat[3];
};
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
  float L[16] = {fx / 256.f, 0, 0, (m_hvdf_offset.x() - 2048.f) / 256.f,
                 0, -fx / 128.f * k, 0, -(m_hvdf_offset.y() - 2048.f) / 128.f * k,
                 0, 0, fx / 8388608.f, m_hvdf_offset.z() / 8388608.f - 1.f,
                 0, 0, 0, 1};
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

void CtrMercRenderer::render(DmaFollower& dma, CtrRenderState& rs) {
  // same structure as Merc2::handle_all_dma, but without asserts: skip what we don't know
  if (dma.current_tag_offset() == rs.next_bucket) {
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

  if (rs.frame_idx % 300 == 0 && m_stats.models) {
    lg::debug("[ctr] {}: {} models ({} missing), {} draws", m_name, m_stats.models,
              m_stats.missing, m_stats.draws);
    m_stats = Stats();
  }
}

void CtrMercRenderer::handle_model(const DmaTransfer& init, CtrRenderState& rs) {
  if (!m_have_camera) {
    return;
  }
  const u8* input = init.data;
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
      memcpy(&bones[slots[i]], rs.ee_mem + addr, sizeof(MercMat));
    }
  }
  input += 128 + 16 * i;
  PcMercFlags flags;
  memcpy(&flags, input, sizeof(flags));

  // a crude light: ambient + half of the first light, applied to the whole model
  float tint[3];
  for (int c = 0; c < 3; c++) {
    float v = lights.ambient[c] + 0.5f * lights.color0[c];
    tint[c] = v < 0.f ? 0.f : (v > 1.f ? 1.f : v);
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
    int tex = draw.texture == 0xffff ? -1 : lev->textures[draw.texture];
    DrawMode mode;
    mode.as_int() = draw.mode;
    ctr_draw_state st = ctr_state_from_draw_mode(mode, tex);
    ctr_gpu_draw_skinned(&st, m_clip, palette_rows, draw.palette_count, tint, model->mesh,
                         draw.first_index, draw.index_count);
    m_stats.draws++;
  }
}
