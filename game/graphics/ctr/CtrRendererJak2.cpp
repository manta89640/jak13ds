/*!
 * @file CtrRendererJak2.cpp
 * (AI-assisted)
 * Jak II's bucket table for the 3DS renderer (see CtrRendererJak2.h): the foreground buckets.
 * Like OpenGLRenderer::init_bucket_renderers_jak2, with the 3DS renderers; buckets without one
 * are skipped.
 */

#include "game/graphics/ctr/CtrRendererJak2.h"

#include <cstring>

#include "common/dma/dma.h"
#include "common/log/log.h"

#include "game/graphics/ctr/CtrDirect.h"
#include "game/graphics/ctr/CtrEye.h"
#include "game/graphics/ctr/CtrLevel.h"
#include "game/graphics/ctr/CtrMerc.h"
#include "game/graphics/ctr/CtrRenderer.h"
#include "game/graphics/ctr/CtrSettings.h"
#include "game/graphics/ctr/CtrShadow.h"
#include "game/graphics/ctr/CtrSprite.h"
#include "game/graphics/ctr/CtrVram.h"
#include "game/graphics/ctr/ctr_gpu.h"

namespace {

constexpr int kLevels = 6;  // jak2 LEVEL_MAX

// the bucket of level i in a list of per-level buckets (GET_BUCKET_ID_FOR_LIST)
jak2::BucketId level_bucket(jak2::BucketId l0, jak2::BucketId l1, int i) {
  return (jak2::BucketId)((int)l0 + ((int)l1 - (int)l0) * i);
}

// Jak II's texture animation codes (TextureAnimator.cpp PcTextureAnimCodesJak2)
enum : u16 {
  kAnimStart = 12,  // the tag that starts an animation array (TextureUploadHandler)
  kAnimFinish = 13,
  kAnimErase = 14,
  kAnimSetShader = 17,
  kAnimDraw = 18,
};

/*!
 * Jak II's texture buckets (TextureUploadHandler): texture page uploads into the emulated VRAM,
 * the texture animation arrays (not done yet: skipped the way TextureAnimator reads them), calls
 * to the default registers, and in some buckets GIF packets drawn directly (add_direct: the map,
 * subtitles, debug text).
 */
class CtrJak2TexRenderer : public CtrBucketRenderer {
 public:
  CtrJak2TexRenderer(std::string name, int id, CtrVram* vram, bool direct)
      : CtrBucketRenderer(std::move(name), id) {
    if (direct) {
      m_direct = std::make_unique<CtrDirect>(vram);
      m_direct->set_allow_depth(false);
    }
  }

  void render(DmaFollower& dma, CtrRenderState& rs) override {
    if (m_direct) {
      m_direct->reset_state();
    }
    while (dma.current_tag_offset() != rs.next_bucket) {
      const auto tag = dma.current_tag();
      const VifCode vif0 = dma.current_tag_vifcode0();
      if (vif0.kind == VifCode::Kind::PC_PORT && vif0.immediate == kAnimStart) {
        dma.read_and_advance();
        skip_anim_array(dma, rs);
        continue;
      }
      auto data = dma.read_and_advance();
      if (data.size_bytes == 0 && data.vif0() == 0 && data.vif1() == 0) {
        continue;
      }
      if (data.size_bytes == 16 && data.vifcode0().kind == VifCode::Kind::PC_PORT &&
          data.vif1() == 3) {
        u64 page;
        s64 mode;
        memcpy(&page, data.data, 8);
        memcpy(&mode, data.data + 8, 8);
        rs.vram->upload_texture_page(rs.ee_mem + page, (int)mode, rs.ee_mem, rs.offset_of_s7);
        continue;
      }
      if (tag.kind == DmaTag::Kind::CALL) {
        // the default registers: call, cnt, ret
        for (int i = 0; i < 2 && dma.current_tag_offset() != rs.next_bucket; i++) {
          dma.read_and_advance();
        }
        continue;
      }
      if (m_direct && (data.vifcode0().kind == VifCode::Kind::DIRECT ||
                       data.vifcode1().kind == VifCode::Kind::DIRECT)) {
        m_direct->render_vif(data.vif0(), data.vif1(), data.data, data.size_bytes);
      }
    }
    if (m_direct) {
      m_direct->flush();
    }
  }

 private:
  // TextureAnimator::handle_texture_anim_data's reads, without drawing
  void skip_anim_array(DmaFollower& dma, CtrRenderState& rs) {
    while (dma.current_tag_offset() != rs.next_bucket) {
      auto tf = dma.read_and_advance();
      const VifCode v = tf.vifcode0();
      if (v.kind != VifCode::Kind::PC_PORT) {
        continue;
      }
      int extra = 0;
      switch (v.immediate) {
        case kAnimFinish:
          return;
        case kAnimErase:
          extra = 2;
          break;
        case kAnimSetShader:
        case kAnimDraw:
          extra = 1;
          break;
        default:
          break;
      }
      for (int i = 0; i < extra && dma.current_tag_offset() != rs.next_bucket; i++) {
        dma.read_and_advance();
      }
    }
  }

  std::unique_ptr<CtrDirect> m_direct;
};

/*! Jak II's direct buckets: GIF packets, with calls to the default registers in between. */
class CtrJak2DirectRenderer : public CtrBucketRenderer {
 public:
  CtrJak2DirectRenderer(std::string name, int id, CtrVram* vram, bool depth)
      : CtrBucketRenderer(std::move(name), id), m_direct(std::make_unique<CtrDirect>(vram)) {
    m_direct->set_allow_depth(depth);
  }
  void render(DmaFollower& dma, CtrRenderState& rs) override {
    m_direct->reset_state();
    while (dma.current_tag_offset() != rs.next_bucket) {
      auto data = dma.read_and_advance();
      if (data.size_bytes) {
        m_direct->render_vif(data.vif0(), data.vif1(), data.data, data.size_bytes);
      }
    }
    m_direct->flush();
  }

 private:
  std::unique_ptr<CtrDirect> m_direct;
};

}  // namespace

void CtrRenderer::init_buckets_jak2() {
  using jak2::BucketId;
  m_buckets.resize((int)BucketId::MAX_BUCKETS);
  CtrJak2BucketSet set = [&](BucketId id, std::unique_ptr<CtrBucketRenderer> r) {
    m_buckets[(int)id] = std::move(r);
  };
  auto tex = [&](BucketId id, bool direct = false) {
    set(id, std::make_unique<CtrJak2TexRenderer>("tex", (int)id, m_vram.get(), direct));
  };
  auto merc = [&](BucketId id) {
    auto r = std::make_unique<CtrMercRenderer>("merc", (int)id, m_levels.get());
    m_merc.emplace_back((int)id, r.get());
    set(id, std::move(r));
  };

  // the background first: its renderers may be replaced below only where both name one
  ctr_jak2_background_buckets(set, m_levels.get(), m_vram.get());

  tex(BucketId::TEX_LCOM_SKY_PRE);
  set(BucketId::SKY_DRAW,
      std::make_unique<CtrJak2DirectRenderer>("sky", (int)BucketId::SKY_DRAW, m_vram.get(), false));
  for (int i = 0; i < kLevels; i++) {
    tex(level_bucket(BucketId::TEX_L0_TFRAG, BucketId::TEX_L1_TFRAG, i));
    merc(level_bucket(BucketId::MERC_L0_TFRAG, BucketId::MERC_L1_TFRAG, i));
    tex(level_bucket(BucketId::TEX_L0_SHRUB, BucketId::TEX_L1_SHRUB, i));
    merc(level_bucket(BucketId::MERC_L0_SHRUB, BucketId::MERC_L1_SHRUB, i));
    tex(level_bucket(BucketId::TEX_L0_ALPHA, BucketId::TEX_L1_ALPHA, i));
    merc(level_bucket(BucketId::MERC_L0_ALPHA, BucketId::MERC_L1_ALPHA, i));
    tex(level_bucket(BucketId::TEX_L0_PRIS, BucketId::TEX_L1_PRIS, i));
    merc(level_bucket(BucketId::MERC_L0_PRIS, BucketId::MERC_L1_PRIS, i));
    tex(level_bucket(BucketId::TEX_L0_PRIS2, BucketId::TEX_L1_PRIS2, i));
    merc(level_bucket(BucketId::MERC_L0_PRIS2, BucketId::MERC_L1_PRIS2, i));
    tex(level_bucket(BucketId::TEX_L0_WATER, BucketId::TEX_L1_WATER, i));
    merc(level_bucket(BucketId::MERC_L0_WATER, BucketId::MERC_L1_WATER, i));
  }
  tex(BucketId::TEX_LCOM_TFRAG);
  merc(BucketId::MERC_LCOM_TFRAG);
  tex(BucketId::TEX_LCOM_SHRUB);
  merc(BucketId::MERC_LCOM_SHRUB);
  tex(BucketId::TEX_LCOM_PRIS);
  merc(BucketId::MERC_LCOM_PRIS);
  tex(BucketId::TEX_LCOM_PRIS2);
  merc(BucketId::MERC_LCOM_PRIS2);
  tex(BucketId::TEX_LCOM_WATER);
  merc(BucketId::MERC_LCOM_WATER);
  tex(BucketId::TEX_LCOM_SKY_POST);
  tex(BucketId::TEX_ALL_SPRITE);
  tex(BucketId::TEX_ALL_WARP);
  tex(BucketId::DEBUG_NO_ZBUF1, true);
  tex(BucketId::TEX_ALL_MAP, true);
  set(BucketId::PROGRESS, std::make_unique<CtrJak2DirectRenderer>("debug", (int)BucketId::PROGRESS,
                                                                  m_vram.get(), false));
  set(BucketId::SCREEN_FILTER, std::make_unique<CtrJak2DirectRenderer>(
                                   "debug", (int)BucketId::SCREEN_FILTER, m_vram.get(), false));
  tex(BucketId::SUBTITLE, true);
  set(BucketId::DEBUG2, std::make_unique<CtrJak2DirectRenderer>("debug", (int)BucketId::DEBUG2,
                                                                m_vram.get(), true));
  set(BucketId::DEBUG_NO_ZBUF2, std::make_unique<CtrJak2DirectRenderer>(
                                    "debug-no-zbuf", (int)BucketId::DEBUG_NO_ZBUF2, m_vram.get(),
                                    false));
  set(BucketId::DEBUG3, std::make_unique<CtrJak2DirectRenderer>("debug", (int)BucketId::DEBUG3,
                                                                m_vram.get(), true));

  for (size_t i = 0; i < m_buckets.size(); i++) {
    if (!m_buckets[i]) {
      m_buckets[i] = std::make_unique<CtrSkipRenderer>("skip", (int)i);
    }
  }
  lg::info("[ctr] Jak II bucket table: {} buckets, {} merc", m_buckets.size(), m_merc.size());
}
