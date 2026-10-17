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

#if defined(__ARM_FEATURE_SAT)
#include <arm_acle.h>
#endif

#include "common/dma/dma.h"
#include "common/log/log.h"

#include "game/graphics/ctr/CtrEye.h"
#include "game/graphics/ctr/CtrMicro.h"
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
static_assert(sizeof(MercMat) == 4 * 16);

// (AI-assisted) The bone table handle_model reads a model's palettes from: per slot the matrix the
// game last sent for it, a pointer into the bone snapshot (m_snap) or game memory instead of a
// copy. Shared by all merc renderers, like the copies were (one static table), so a palette naming
// a slot its model didn't send still reads the matrix an earlier model left there (Merc2 does the
// same with its own leftovers; "a draw uses bone N, which the game didn't send" in the log).
// What the slots point at is kept (copied) at the end of each render(): the snapshot and game
// memory change before the next frame. Slots 128-255 (no slot string sets them) read zeros.
struct BoneTable {
  const MercMat* slot[256];
  MercMat kept[128];  // zeros at first (as the copies' static table)
  MercMat zero;
  u64 touched[2] = {0, 0};
  BoneTable() {
    memset(kept, 0, sizeof(kept));
    memset(&zero, 0, sizeof(zero));
    for (int i = 0; i < 256; i++) {
      slot[i] = i < 128 ? &kept[i] : &zero;
    }
  }
  void set(int s, const MercMat* m) {
    slot[s] = m;
    touched[s >> 6] |= 1ull << (s & 63);
  }
  // copy what the slots set since the last keep() point at
  void keep() {
    for (int h = 0; h < 2; h++) {
      for (u64 t = touched[h]; t; t &= t - 1) {
        const int s = 64 * h + __builtin_ctzll(t);
        if (slot[s] != &kept[s]) {
          memcpy(&kept[s], slot[s], sizeof(MercMat));
          slot[s] = &kept[s];
        }
      }
      touched[h] = 0;
    }
  }
};
BoneTable g_bones;

struct PcMercFlags {
  u64 enable_mask;
  u64 ignore_alpha_mask;
  u8 effect_count;
  u8 bitflags;
};

// (AI-assisted) a new number for each bone palette made (all merc renderers: the GPU keeps the last
// one drawn), never 0. The GPU compares these instead of the bones: a palette's rows are never
// changed under its number.
u32 next_palette_id() {
  static u32 s_id;
  return ++s_id ? s_id : ++s_id;
}

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
  m_mod_snaps.clear();
  m_mod_bytes.clear();
  m_snapshot_mode = true;
  render(dma, rs);
  m_snapshot_mode = false;
  m_snap_valid = true;
}

void CtrMercRenderer::render(DmaFollower& dma, CtrRenderState& rs) {
  m_snap_pos = 0;
  m_model_ordinal = 0;
  m_mod_cursor = 0;
  m_mod_frame_counted = false;
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
  g_bones.keep();  // (AI-assisted) the snapshot and game memory change before the next frame
  flush_queues();
  // (AI-assisted) merc_gen_lod: forget instances not drawn for 10 s
  if (++m_frame % 300 == 0) {
    for (auto it = m_gen_lod.begin(); it != m_gen_lod.end();) {
      it = m_frame - it->second.frame > 300 ? m_gen_lod.erase(it) : std::next(it);
    }
  }
  m_snap_valid = false;  // used up: the next frame needs a new snapshot
  // (AI-assisted) the cost of the rewritten vertices, once per level (the first one drawn with
  // them) after a few seconds of updates
  if (m_mod_frames >= 150) {
    const CtrLevelData* lev = nullptr;
    m_levels->find_merc_model(m_mod_last_model, &lev);
    const std::string level = lev ? lev->name : std::string("?");
    if (level != m_mod_logged_for) {
      m_mod_logged_for = level;
      lg::info("[ctr] {}: merc vertex updates in {}: {:.1f} effects, {:.0f} vertices per frame, "
               "{:.3f} ms/frame unpacking (render thread) + {:.3f} ms/frame copying (game "
               "thread), {} bad",
               m_name, level, (double)m_mod_effects / m_mod_frames,
               (double)m_mod_verts / m_mod_frames, m_mod_ms / m_mod_frames,
               m_mod_gather_ms / m_mod_frames, m_mod_bad);
    }
    m_mod_frames = m_mod_verts = m_mod_effects = m_mod_bad = 0;
    m_mod_ms = m_mod_gather_ms = 0;
  }
  if (rs.log_now && m_stats.models) {
    lg::debug("[ctr] {}: {} models ({} missing, {} with bad bones, {} with the generated low LOD), "
              "{} draws, {} envmap batches, {} draws sorted by texture, {} with back faces culled "
              "({} not: mirroring bone), {} palette bones not sent",
              m_name, m_stats.models, m_stats.missing, m_stats.bad_bones, m_stats.gen_lod,
              m_stats.draws, m_stats.env_batches, m_stats.sorted_draws, m_stats.culled,
              m_stats.mirrored, m_stats.unsent_bones);
    m_stats = Stats();
  }
}

bool CtrMercRenderer::bad_bones(const CtrMercMat* const* bones, const u8* slots, int n) {
  static_assert(sizeof(CtrMercMat) == 64);
  u32 bad = 0;
  for (int b = 0; b < n; b++) {
    if (slots[b] < 128) {
      bad |= ctr_micro::bad_bone_bits(bones[slots[b]]->tmat[0].data());
    }
  }
  return bad & 0x80000000u;
}

u32 CtrMercRenderer::queue_floats(const float* src, int n) {
  const u32 at = (u32)m_qdata.size();
  m_qdata.insert(m_qdata.end(), src, src + n);
  return at;
}

void CtrMercRenderer::flush_env() {
  if (!m_q_env.empty()) {
    const float* d = m_qdata.data();
    for (const auto& q : m_q_env) {
      ctr_gpu_draw_skinned_env(&q.st, d + q.clip, d + q.rows, q.palette_count,
                               q.rows_f24 == ~0u ? nullptr : m_qf24.data() + q.rows_f24,
                               q.bones_id, q.fade, q.mesh, q.first_index, q.index_count);
    }
    m_stats.env_batches++;
    m_q_env.clear();
  }
}

void CtrMercRenderer::flush_opaque() {
  if (m_q_opaque.empty()) {
    return;
  }
  const u32 n = (u32)m_q_opaque.size();
  m_q_order.resize(n);
  for (u32 i = 0; i < n; i++) {
    m_q_order[i] = i;
  }
  // by model (in the order they came), then draw, then the instances in the order they came (the
  // queue index: the order std::stable_sort gave, without its buffer from the heap every frame)
  std::sort(m_q_order.begin(), m_q_order.end(), [&](u32 a, u32 b) {
    const auto& qa = m_q_opaque[a];
    const auto& qb = m_q_opaque[b];
    if (qa.model_rank != qb.model_rank) {
      return qa.model_rank < qb.model_rank;
    }
    if (qa.draw_index != qb.draw_index) {
      return qa.draw_index < qb.draw_index;
    }
    return a < b;
  });
  const float* d = m_qdata.data();
  for (u32 i : m_q_order) {
    const auto& q = m_q_opaque[i];
    ctr_gpu_draw_skinned(&q.st, d + q.clip, d + q.rows, q.palette_count,
                         q.rows_f24 == ~0u ? nullptr : m_qf24.data() + q.rows_f24, q.bones_id,
                         d + q.lights, q.mesh, q.first_index, q.index_count);
  }
  m_stats.sorted_draws += (int)n;
  m_q_opaque.clear();
  m_q_models.clear();
}

void CtrMercRenderer::flush_queues() {
  flush_opaque();  // (the bases before their shine)
  flush_env();
  m_qdata.clear();
  m_qf24.clear();
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

namespace {
// (AI-assisted) lump byte -> MercVertex::normal: Merc2's float bits (0x47800000 + byte) - 65537
// (= byte / 128 - 1), * 127 like the converter
struct NormalLut {
  s8 v[256];
  NormalLut() {
    for (int b = 0; b < 256; b++) {
      v[b] = (s8)std::clamp((int)std::lround((b / 128.f - 1.f) * 127.f), -127, 127);
    }
  }
};
const NormalLut g_normal_lut;

inline s16 sat16(s32 v) {
#if defined(__ARM_FEATURE_SAT)
  return (s16)__ssat(v, 16);
#else
  return (s16)std::clamp(v, -32768, 32767);
#endif
}

void put_u32(std::vector<u8>& out, u32 v) {
  const size_t at = out.size();
  out.resize(at + 4);
  memcpy(out.data() + at, &v, 4);
}
}  // namespace

void CtrMercRenderer::gather_mod(const u8* ee_mem,
                                 const u8* effect_ptrs,
                                 int effect_count,
                                 const CtrMercModelData& model) {
  // like Merc2::model_mod_draws: walk the effect's fragments and their controls
  for (const auto& e : model.mod) {
    const size_t count_at = m_mod_bytes.size();
    put_u32(m_mod_bytes, 0);
    if (e.effect >= effect_count) {
      continue;
    }
    u32 ea;
    memcpy(&ea, effect_ptrs + 4 * e.effect, 4);
    if (!ea) {
      continue;
    }
    const u8* merc_effect = ee_mem + ea;
    u16 frag_cnt;
    memcpy(&frag_cnt, merc_effect + 18, 2);
    if (frag_cnt < e.mask.size()) {
      continue;  // not the model we think
    }
    u32 frag_goal, ctrl_goal;
    memcpy(&frag_goal, merc_effect, 4);
    memcpy(&ctrl_goal, merc_effect + 4, 4);
    const u8* frag = ee_mem + frag_goal;
    const u8* ctrl = ee_mem + ctrl_goal;
    u32 total = 0;
    for (size_t fi = 0; fi < e.mask.size(); fi++) {
      if (e.mask[fi]) {
        const u32 u4 = ((ctrl[0] + 3) / 4) * 16;
        const u32 l4 = ((ctrl[1] + 3) / 4) * 16;
        const u32 nv = l4 / 12;  // 3 words per vertex
        put_u32(m_mod_bytes, nv);
        const size_t at = m_mod_bytes.size();
        m_mod_bytes.resize(at + 12 + ((12 * nv + 3) & ~3u));
        memcpy(m_mod_bytes.data() + at, frag + frag[10] * 16, 12);  // position offsets
        memcpy(m_mod_bytes.data() + at + 12, frag + u4, 12 * nv);
        total += nv;
      }
      ctrl += 4 + 2 * ctrl[3];
      frag += frag[11] * 16;
    }
    memcpy(m_mod_bytes.data() + count_at, &total, 4);
  }
}

void CtrMercRenderer::apply_mod(const CtrMercModelData& model, size_t pos) {
  const double t0 = ctr_gpu_time_ms();
  u8* verts = (u8*)ctr_gpu_mesh_vertices(model.mesh);
  if (!verts) {
    return;
  }
  const u8* p = m_mod_bytes.data() + pos;
  const u8* end = m_mod_bytes.data() + m_mod_bytes.size();
  u32 lo = UINT32_MAX, hi = 0;
  for (const auto& e : model.mod) {
    if (p + 4 > end) {
      return;
    }
    u32 total;
    memcpy(&total, p, 4);
    p += 4;
    if (total != e.vidx_end) {
      // not unpacked (or not the fragments we expect): the vertices stay as they are, and the
      // rest of the copy can't be followed
      m_mod_bad++;
      if (total) {
        return;
      }
      continue;
    }
    if (m_mod_tmp.size() < total) {
      m_mod_tmp.resize(total);
    }
    ModV* out = m_mod_tmp.data();
    const s16* st_lut = e.st_lut;
    const s8* nrm_lut = g_normal_lut.v;
    // position: (float bits (0x4b010000 + byte) + fragment offset) * xyz_scale (Merc2), in
    // MercVertex units (/ model scale), rounded by adding 1.5 * 2^23: the float's low mantissa
    // bits are then the integer (no float -> int conversion or compare in the loop)
    const float k = e.pos_k;
    constexpr float kRound = 12582912.f;
    u32 done = 0;
    while (done < total) {
      u32 nv;
      memcpy(&nv, p, 4);
      float off[3];
      memcpy(off, p + 4, 12);
      const u8* v = p + 16;
      p += 16 + ((12 * nv + 3) & ~3u);
      if (nv > total - done) {
        m_mod_bad++;
        return;
      }
      for (u32 i = 0; i < nv; i++, v += 12, out++) {
        // x, y, z as three independent chains (VFP11 latency)
        const u32 bx = 0x4b010000u + v[3], by = 0x4b010000u + v[7], bz = 0x4b010000u + v[11];
        float fx, fy, fz;
        memcpy(&fx, &bx, 4);
        memcpy(&fy, &by, 4);
        memcpy(&fz, &bz, 4);
        fx = (fx + off[0]) * k + kRound;
        fy = (fy + off[1]) * k + kRound;
        fz = (fz + off[2]) * k + kRound;
        s32 ix, iy, iz;
        memcpy(&ix, &fx, 4);
        memcpy(&iy, &fy, 4);
        memcpy(&iz, &fz, 4);
        out->pos[0] = sat16(ix - 0x4b400000);
        out->pos[1] = sat16(iy - 0x4b400000);
        out->pos[2] = sat16(iz - 0x4b400000);
        out->nrm[0] = nrm_lut[v[2]];
        out->nrm[1] = nrm_lut[v[6]];
        out->nrm[2] = nrm_lut[v[10]];
        out->pad = 0;
        out->st[0] = st_lut[v[8]];
        out->st[1] = st_lut[v[9]];
      }
      done += nv;
    }
    // to the model's vertices (in vertex order)
    const ModV* src = m_mod_tmp.data();
    if (!model.mod_checked) {
      // once per model: how far the game's vertices are from the file's (the same before the
      // game changes them, but for rounding)
      int dxz = 0, dy = 0, dst = 0, dn = 0;
      for (const auto& d : e.dests) {
        c3l::MercVertex fv;
        memcpy(&fv, verts + sizeof(c3l::MercVertex) * d.vertex, sizeof(fv));
        const ModV& s = src[d.src];
        dxz = std::max({dxz, std::abs(fv.pos[0] - s.pos[0]), std::abs(fv.pos[2] - s.pos[2])});
        dy = std::max(dy, std::abs(fv.pos[1] - s.pos[1]));
        dst = std::max({dst, std::abs(fv.st[0] - s.st[0]), std::abs(fv.st[1] - s.st[1])});
        for (int c = 0; c < 3; c++) {
          dn = std::max(dn, std::abs(fv.normal[c] - s.nrm[c]));
        }
      }
      lg::info("[ctr] merc vertex updates for {} effect {}: {} vertices -> {} model vertices; "
               "largest difference to the file: x/z {}, y {} (scale {}), st {} (1/1024), "
               "normal {} (1/127)",
               model.name, e.effect, total, e.dests.size(), dxz, dy, model.scale, dst, dn);
    }
    for (const auto& d : e.dests) {
      u8* dst = verts + sizeof(c3l::MercVertex) * d.vertex;
      const ModV& s = src[d.src];
      memcpy(dst, s.pos, 6);
      memcpy(dst + 12, s.st, 4);
      memcpy(dst + 20, s.nrm, 4);  // normal + pad
    }
    if (!e.dests.empty()) {
      lo = std::min<u32>(lo, e.dests.front().vertex);
      hi = std::max<u32>(hi, e.dests.back().vertex);
    }
    m_mod_effects++;
    m_mod_verts += total;
  }
  if (lo <= hi) {
    ctr_gpu_mesh_flush(model.mesh, (int)(lo * sizeof(c3l::MercVertex)),
                       (int)((hi - lo + 1) * sizeof(c3l::MercVertex)));
  }
  model.mod_checked = true;
  m_mod_last_model = model.name;
  if (!m_mod_frame_counted) {
    m_mod_frame_counted = true;
    m_mod_frames++;
  }
  m_mod_ms += ctr_gpu_time_ms() - t0;
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
        MercMat& m = m_snap.emplace_back();
        memcpy(&m, rs.ee_mem + addr, sizeof(MercMat));  // (AI-assisted) tmat only
      }
      // (AI-assisted) vertices the game rewrites (ripple, texture scroll): the game changes them
      // during the next frame's logic, so they're copied now too. Only models with an effect
      // that may have them (effect-bits: texture scroll 1, ripple 4) are looked up by name.
      // Not only pc-merc-vtx-update models: water with a ripple query (the dark eco pools) was
      // meant for generic merc and comes without it.
      const u8* fl = sl + 128 + 16 * n;
      PcMercFlags flags;
      memcpy(&flags, fl, sizeof(flags));
      const int ec = std::min<int>(flags.effect_count, 64);
      const u8* ptrs = fl + 32 + ((flags.bitflags & 4) ? 4 * c3l::kMercBlercWeights : 0) +
                       ((ec * 4 + 15) / 16) * 16;
      bool maybe = flags.bitflags & 1;
      for (int e = 0; e < ec && !maybe; e++) {
        u32 ea;
        memcpy(&ea, ptrs + 4 * e, 4);
        maybe = ea && (rs.ee_mem[ea + 17] & 5);
      }
      if (maybe) {
        const double t0 = ctr_gpu_time_ms();
        char name[128];
        memcpy(name, input, 128);
        name[127] = 0;
        const CtrLevelData* lev = nullptr;
        const CtrMercModelData* model = m_levels->find_merc_model(name, &lev);
        if (model && !model->mod.empty()) {
          m_mod_snaps.push_back({m_model_ordinal, m_mod_bytes.size()});
          gather_mod(rs.ee_mem, ptrs, ec, *model);
        }
        m_mod_gather_ms += ctr_gpu_time_ms() - t0;
      }
      m_model_ordinal++;
      return;
    }
    m_model_ordinal++;
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

#ifndef __3DS__
  {
    // (AI-assisted) snow-gpu-audit: statistics per model (env CTR_SOFT_MERC_MODELS)
    static const bool per_model = getenv("CTR_SOFT_MERC_MODELS") != nullptr;
    if (per_model) {
      ctr_gpu_soft_bucket((std::string("m:") + name).c_str());
    }
  }
#endif
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
  // (AI-assisted) jak 1: uses-water (Merc2's jak1_water_mode: blended, no depth writes; the
  // jungle river and other water-anim models were drawn opaque)
  u64 uses_water = 0;
  memcpy(&uses_water, input, 8);
  input += 16;

  // matrix slot string + GOAL addresses of the bone matrices
  const MercMat* const* bones = g_bones.slot;  // (AI-assisted) see BoneTable
  const u8* slots = input;
  const u32* matrix_array = (const u32*)(input + 128);
  u64 sent[2] = {0, 0};  // (AI-assisted) the bones the game sent for this model
  int i;
  for (i = 0; i < 128; i++) {
    if (slots[i] == 0xff) {
      break;
    }
    u32 addr;
    memcpy(&addr, &matrix_array[i * 4], 4);
    if (slots[i] < 128) {
      sent[slots[i] >> 6] |= 1ull << (slots[i] & 63);
      if (m_snap_valid && m_snap_first + i < m_snap.size()) {
        g_bones.set(slots[i], &m_snap[m_snap_first + i]);
      } else if (!(addr & 3)) {
        g_bones.set(slots[i], (const MercMat*)(rs.ee_mem + addr));
      } else {
        // (AI-assisted) not a float's alignment: copied, as before
        memcpy(&g_bones.kept[slots[i]], rs.ee_mem + addr, sizeof(MercMat));
        g_bones.set(slots[i], &g_bones.kept[slots[i]]);
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

  // (AI-assisted) vertices the game rewrites: from the snapshot of this model (async), else
  // straight from game memory (synchronous rendering: the game waits)
  if (!model->mod.empty()) {
    const u32 ordinal = m_model_ordinal - 1;
    while (m_mod_cursor < m_mod_snaps.size() && m_mod_snaps[m_mod_cursor].ordinal < ordinal) {
      m_mod_cursor++;
    }
    if (m_snap_valid) {
      if (m_mod_cursor < m_mod_snaps.size() && m_mod_snaps[m_mod_cursor].ordinal == ordinal) {
        apply_mod(*model, m_mod_snaps[m_mod_cursor].pos);
      }
    } else {
      const int ec = std::min<int>(flags.effect_count, 64);
      const u8* ptrs = input + 32 + ((flags.bitflags & 4) ? 4 * c3l::kMercBlercWeights : 0) +
                       ((ec * 4 + 15) / 16) * 16;
      m_mod_bytes.clear();
      gather_mod(rs.ee_mem, ptrs, ec, *model);
      apply_mod(*model, 0);
    }
  }

  // Bone matrices that are garbage (seen in cutscenes whose streamed animation is missing) make
  // screen-filling triangles: skip the model instead. (AI-assisted) On the bits (CtrMicro.h
  // not_below), OR-ed over the bones: no VFP compare and branch per value
  if (bad_bones(bones, slots, i)) {
    m_stats.bad_bones++;
    return;
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
      // (AI-assisted) the colors' and the ambient's alpha (the shader multiplies the vertex alpha
      // by it, like merc2.vert): color-mult's alpha, e.g. 0.75 for the jungle river's water
      light_data[4 * i + 3] = i >= 3 ? src[i][3] : 0.f;
    }
  }

  // (AI-assisted) HUD models are scaled unevenly (the orb icon: x and z 0.0076, y -0.011). The skin
  // shaders turn the normals with the bone rows themselves (M n), right for evenly scaled bones; the
  // game's normal matrix (the inverse transpose, which Merc2 and generic use) differs from it by
  // D = (M M^T)^-1. For HUD models, D goes into the rows and its inverse into the clip matrix:
  // the same positions, normals D M n = M^-T n, for the lighting and the envmap's texture
  // coordinates. (The envmap's view vector gets D too: small next to the normal's change, HUD
  // models being far along z from the HUD's origin.)
  float hud_d[9];
  bool hud_nfix = false;
  float hud_fixed_clip[16];
  if ((flags.bitflags & 8) && i > 0 && slots[0] < 128) {
    const MercMat& m0 = *bones[slots[0]];
    double g[3][3];
    for (int r = 0; r < 3; r++) {
      for (int t = 0; t < 3; t++) {
        g[r][t] = 0;
        for (int c = 0; c < 3; c++) {
          g[r][t] += (double)m0.tmat[c][r] * m0.tmat[c][t];
        }
      }
    }
    const double det = g[0][0] * (g[1][1] * g[2][2] - g[1][2] * g[2][1]) -
                       g[0][1] * (g[1][0] * g[2][2] - g[1][2] * g[2][0]) +
                       g[0][2] * (g[1][0] * g[2][1] - g[1][1] * g[2][0]);
    if (std::abs(det) > 1e-30) {
      double d[3][3];
      for (int r = 0; r < 3; r++) {
        for (int t = 0; t < 3; t++) {
          const int r1 = (t + 1) % 3, r2 = (t + 2) % 3, c1 = (r + 1) % 3, c2 = (r + 2) % 3;
          d[r][t] = (g[r1][c1] * g[r2][c2] - g[r1][c2] * g[r2][c1]) / det;
        }
      }
      // scaled to about 1 (only its direction matters for the normals): D / k, D^-1 = k G
      const double k = (d[0][0] + d[1][1] + d[2][2]) / 3.0;
      float e[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
      for (int r = 0; r < 3; r++) {
        for (int t = 0; t < 3; t++) {
          hud_d[3 * r + t] = (float)(d[r][t] / k);
          e[4 * r + t] = (float)(g[r][t] * k);
        }
      }
      mul44(hud_fixed_clip, clip, e);
      clip = hud_fixed_clip;
      hud_nfix = true;
    }
  }

  // envmap strength and color per effect (pc-merc-draw-request "fades", after the flags and the
  // blend shape weights): 0 when the effect's envmap is off or faded out by distance
  const u8* fades = input + 32 + ((flags.bitflags & 4) ? 4 * c3l::kMercBlercWeights : 0);
  const int effect_count = flags.effect_count;

  // bones as 3x4 rows: camera = -(tmat[0] * x + tmat[1] * y + tmat[2] * z + tmat[3]),
  // with the model's quantization scale folded in
  float palette_rows[CTR_MAX_PALETTE * 12];
  const c3l::MercDraw* rows_of = nullptr;  // the draw whose palette palette_rows holds
  // (AI-assisted) cull_merc: a bone of the palette mirrors (its matrix' determinant isn't positive):
  // that draw's winding is flipped on screen, so it isn't culled. Not for HUD models (their own
  // projection)
  const bool cull_merc = ctr_settings().cull_merc && !(flags.bitflags & 8);
  bool palette_mirrors = false;
  // (AI-assisted) envmap_batch: this model's clip matrix and current palette in m_qdata (~0u: not
  // there yet)
  // merc_tex_sort: not for models whose vertices change per instance (blend shapes, rewritten
  // vertices: one mesh); their shine is queued too then (its base may be in the queue)
  const bool tex_sort = ctr_settings().merc_tex_sort && !model->blerc_count && model->mod.empty();
  const bool env_batch = ctr_settings().envmap_batch || tex_sort;
  u32 q_clip = ~0u, q_rows = ~0u, q_lights = ~0u;
  u32 model_rank = ~0u;
  // (AI-assisted) the palette's number for the GPU (new with every palette made) and its float24
  // words for skin_rows_f24, made once per palette (not at every upload: the shine pass and the
  // texture sorted draws upload a palette again after other models' palettes)
  u32 palette_id = 0;
  const bool rows_f24 = ctr_gpu_skin_rows_f24_on() != 0;
  uint32_t palette_f24[9 * CTR_MAX_PALETTE];
  bool palette_f24_made = false;
  u32 q_f24 = ~0u;
  auto direct_f24 = [&](int count) -> const uint32_t* {
    if (!rows_f24) {
      return nullptr;
    }
    if (!palette_f24_made) {
      // (AI-assisted) ctr_gpu_pack_skin_rows_f24's words in fewer instructions (CtrMicro.h)
      ctr_micro::pack_skin_rows_f24(palette_rows, std::min<int>(count, CTR_MAX_PALETTE), palette_f24);
      palette_f24_made = true;
    }
    return palette_f24;
  };
  auto queue_f24 = [&](int count) -> u32 {
    if (!rows_f24) {
      return ~0u;
    }
    if (q_f24 == ~0u) {
      const uint32_t* w = direct_f24(count);
      const int n = 9 * std::min<int>(count, CTR_MAX_PALETTE);
      q_f24 = (u32)m_qf24.size();
      m_qf24.insert(m_qf24.end(), w, w + n);
    }
    return q_f24;
  };
  // (AI-assisted) config merc_gen_lod: the converter's generated low LOD of a model with only lod0
  // (the yeti) when its first bone is further from the camera than max(25 m, 12.5 x the model's
  // size) x merc_lod_scale; 10% either side of that before it switches (per instance: the
  // address of its bone matrices)
  const std::vector<c3l::MercDraw>* draws = &model->draws;
  if (!model->gen_draws.empty() && ctr_settings().merc_gen_lod && !(flags.bitflags & 8) && i > 0 &&
      slots[0] < 128) {
    u32 key;
    memcpy(&key, &matrix_array[0], 4);
    const math::Vector4f& t = bones[slots[0]]->tmat[3];
    const float d = std::sqrt(t.x() * t.x() + t.y() * t.y() + t.z() * t.z()) * (1.f / 4096.f);
    const float size_m = model->scale * 32767.f * (1.f / 4096.f);
    const float at = std::max(25.f, 12.5f * size_m) * ctr_settings().merc_lod_scale;
    auto& g = m_gen_lod[key];
    g.frame = m_frame;
    g.low = g.low ? d > 0.9f * at : d > 1.1f * at;
    if (g.low) {
      draws = &model->gen_draws;
      m_stats.gen_lod++;
#ifndef __3DS__
      // (AI-assisted) per model statistics: the generated low LOD on its own line
      static const bool per_model = getenv("CTR_SOFT_MERC_MODELS") != nullptr;
      if (per_model) {
        ctr_gpu_soft_bucket((std::string("m:") + name + c3l::kMercGenLodSuffix).c_str());
      }
#endif
    }
  }
  for (const auto& draw : *draws) {
    if (!(flags.enable_mask & (1ull << draw.effect))) {
      continue;
    }
    const bool envmap = draw.eye_id == c3l::kMercEnvmapDraw;
    float fade[4] = {0, 0, 0, 0};
    if (envmap) {
      if (draw.effect >= effect_count || !ctr_settings().envmap) {
        continue;
      }
      const u8* f = fades + 4 * draw.effect;
      if (!f[0] && !f[1] && !f[2]) {
        continue;
      }
      // (AI-assisted) as Merc2: fade / 255, which emerc.frag doubles (texture * fade * 2), so the
      // game's 0x80 adds the envmap texture once (GS modulate: texture * 0x80 / 128). This was
      // fade / 128, a shine twice as bright as on PC/PS2 (the HUD orb and power cell icons, whose
      // envmap fade draw-bones-hud-merc sets to 0x80, and every envmapped model in the world).
      for (int c = 0; c < 4; c++) {
        fade[c] = f[c] / 255.f;
      }
    }
    // the draws of a model with at most 24 bones share one palette: made once
    const bool same_palette = rows_of && rows_of->palette_count == draw.palette_count &&
                              !memcmp(rows_of->palette, draw.palette, draw.palette_count);
    if (!same_palette) {
      q_rows = ~0u;
      q_f24 = ~0u;
      palette_id = next_palette_id();
      palette_f24_made = false;
    }
    if (!same_palette) {
      palette_mirrors = false;
    }
    for (int p = 0; !same_palette && p < draw.palette_count && p < CTR_MAX_PALETTE; p++) {
      // (AI-assisted) a bone the game didn't send this frame: the table still has the matrix of
      // an earlier model (Merc2 reads its own leftover the same way). Counted, logged once per
      // model: it would put that part of the model somewhere else
      const u8 bi = draw.palette[p];
      if (bi < 128 && !(sent[bi >> 6] & (1ull << (bi & 63)))) {
        m_stats.unsent_bones++;
        if (std::find(m_unsent_bone_logged.begin(), m_unsent_bone_logged.end(), model->name) ==
            m_unsent_bone_logged.end()) {
          m_unsent_bone_logged.push_back(model->name);
          lg::info("[ctr] merc {}: a draw uses bone {}, which the game didn't send ({} sent)",
                   model->name, bi, i);
        }
      }
      const MercMat& m = *bones[bi];
      float* rows = &palette_rows[12 * p];
      if (cull_merc) {
        const math::Vector4f& a = m.tmat[0];
        const math::Vector4f& b = m.tmat[1];
        const math::Vector4f& c = m.tmat[2];
        const float det = a.x() * (b.y() * c.z() - b.z() * c.y()) -
                          a.y() * (b.x() * c.z() - b.z() * c.x()) +
                          a.z() * (b.x() * c.y() - b.y() * c.x());
        palette_mirrors = palette_mirrors || !(det > 0.f);
      }
      for (int r = 0; r < 3; r++) {
        rows[4 * r + 0] = -m.tmat[0][r] * model->scale;
        rows[4 * r + 1] = -m.tmat[1][r] * model->scale;
        rows[4 * r + 2] = -m.tmat[2][r] * model->scale;
        rows[4 * r + 3] = -m.tmat[3][r];
      }
    }
    if (hud_nfix && !same_palette) {
      for (int p = 0; p < draw.palette_count && p < CTR_MAX_PALETTE; p++) {
        float* rows = &palette_rows[12 * p];
        float o[12];
        for (int r = 0; r < 3; r++) {
          for (int c = 0; c < 4; c++) {
            o[4 * r + c] = hud_d[3 * r] * rows[c] + hud_d[3 * r + 1] * rows[4 + c] +
                           hud_d[3 * r + 2] * rows[8 + c];
          }
        }
        memcpy(rows, o, sizeof(o));
      }
    }
    rows_of = &draw;
    int tex = draw.texture < lev->textures.size() ? lev->textures[draw.texture] : -1;
    if (!envmap && draw.eye_id != 0xff && m_eyes) {
      const int eye = m_eyes->texture(draw.eye_id);
      if (eye >= 0) {
        tex = eye;
      }
    }
    DrawMode mode;
    mode.as_int() = draw.mode;
    if (uses_water) {
      mode.set_ab(true);
      mode.disable_depth_write();
    }
    ctr_draw_state st = ctr_state_from_draw_mode(mode, tex);
    // (AI-assisted) cull_merc: draws the converter marked (closed enough, wound like the vertex
    // normals: c3l::kMercDrawCullBack) lose their back faces
    if (cull_merc && (draw.palette[c3l::kMercDrawFlagsByte] & c3l::kMercDrawCullBack)) {
      if (palette_mirrors) {
        m_stats.mirrored++;
      } else {
        st.tcc |= CTR_TCC_CULL_BACK;
        st.tcc |= CTR_TCC_CULL_OWN;
        m_stats.culled++;
      }
    }
    if (envmap) {
      // emerc.frag: texture * fade * 2, no alpha test, the envmap mode's blending (adds a shine)
      st.atest = CTR_TEST_ALWAYS;
      if (env_batch) {
        if (q_clip == ~0u) {
          q_clip = queue_floats(clip, 16);
        }
        if (q_rows == ~0u) {
          q_rows = queue_floats(palette_rows, 12 * std::min<int>(draw.palette_count, CTR_MAX_PALETTE));
        }
        QueuedDraw q;
        q.st = st;
        q.clip = q_clip;
        q.rows = q_rows;
        q.rows_f24 = queue_f24(draw.palette_count);
        q.bones_id = palette_id;
        q.lights = 0;
        q.palette_count = draw.palette_count;
        memcpy(q.fade, fade, sizeof(fade));
        q.mesh = model->mesh;
        q.first_index = (int)draw.first_index;
        q.index_count = (int)draw.index_count;
        m_q_env.push_back(q);
      } else {
        ctr_gpu_draw_skinned_env(&st, clip, palette_rows, draw.palette_count,
                                 direct_f24(draw.palette_count), palette_id, fade, model->mesh,
                                 draw.first_index, draw.index_count);
      }
      m_stats.draws++;
      continue;
    }
    // Alpha like merc2.frag, not like the draw mode's alpha test: only (nearly) transparent pixels
    // are dropped (alpha < 0.128), the rest is blended. The draw modes of hair, eyes and many
    // objects ask for alpha >= 0x26, which throws away most of a hair texture (see-through, noisy
    // hair). Effects the game draws with ignore-alpha keep all their pixels (merc2.frag skips the
    // discard), but their blending is still the draw mode's, as on PC (AI-assisted: turning it
    // off drew the jungle river's water opaque).
    if (flags.ignore_alpha_mask & (1ull << draw.effect)) {
      st.atest = CTR_TEST_ALWAYS;
    } else {
      st.atest = CTR_TEST_GEQUAL;
      st.aref = 17;  // x2 in the GPU state: 34 / 255
    }
    // (AI-assisted) opaque parts whose texture passes the alpha test everywhere: the early depth
    // test drops what's hidden behind the level before texturing (CtrTfragRenderer draws first)
    if (st.blend == CTR_BLEND_OFF && st.zwrite && st.ztest == CTR_TEST_GEQUAL &&
        draw.eye_id == 0xff &&
        (st.atest == CTR_TEST_ALWAYS || (draw.texture < lev->tex_min_alpha.size() &&
                                         lev->tex_min_alpha[draw.texture] >= st.aref * 2))) {
      st.flags |= CTR_STATE_EARLY_DEPTH;
    }
    const bool opaque = st.blend == CTR_BLEND_OFF && st.zwrite && st.ztest == CTR_TEST_GEQUAL;
    if (tex_sort && opaque && draw.eye_id == 0xff) {
      if (q_clip == ~0u) {
        q_clip = queue_floats(clip, 16);
      }
      if (q_rows == ~0u) {
        q_rows = queue_floats(palette_rows, 12 * std::min<int>(draw.palette_count, CTR_MAX_PALETTE));
      }
      if (q_lights == ~0u) {
        q_lights = queue_floats(light_data, 28);
      }
      if (model_rank == ~0u) {
        auto it = std::find(m_q_models.begin(), m_q_models.end(), (const void*)draws);
        model_rank = (u32)(it - m_q_models.begin());
        if (it == m_q_models.end()) {
          m_q_models.push_back(draws);
        }
      }
      QueuedDraw q;
      q.st = st;
      q.clip = q_clip;
      q.rows = q_rows;
      q.rows_f24 = queue_f24(draw.palette_count);
      q.bones_id = palette_id;
      q.lights = q_lights;
      q.palette_count = draw.palette_count;
      memset(q.fade, 0, sizeof(q.fade));
      q.mesh = model->mesh;
      q.first_index = (int)draw.first_index;
      q.index_count = (int)draw.index_count;
      q.model_rank = model_rank;
      q.draw_index = (u32)(&draw - draws->data());
      m_q_opaque.push_back(q);
      m_stats.draws++;
      continue;
    }
    // (AI-assisted) envmap_batch / merc_tex_sort: the queues before a draw that blends or doesn't
    // write depth (it would change what the shine adds to, or be covered by it), and before any
    // draw of a sorted model that isn't queued (an eye: after its face, as in the model)
    if ((!m_q_env.empty() || !m_q_opaque.empty()) && (!opaque || tex_sort)) {
      flush_queues();
      q_clip = q_rows = q_lights = q_f24 = ~0u;
      model_rank = ~0u;
    }
    ctr_gpu_draw_skinned(&st, clip, palette_rows, draw.palette_count,
                         direct_f24(draw.palette_count), palette_id, light_data, model->mesh,
                         draw.first_index, draw.index_count);
    m_stats.draws++;
  }
}
