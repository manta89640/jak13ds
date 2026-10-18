/*!
 * @file CtrRendererJak2.cpp
 * (AI-assisted)
 * Jak II's bucket table for the 3DS renderer (see CtrRendererJak2.h): the foreground buckets.
 * Like OpenGLRenderer::init_bucket_renderers_jak2, with the 3DS renderers; buckets without one
 * are skipped.
 */

#include "game/graphics/ctr/CtrRendererJak2.h"

#include <cstdlib>
#include <cstring>

#include "common/dma/dma.h"
#include "common/common_types.h"
#include "common/goal_constants.h"
#include "common/log/log.h"

#include "fmt/format.h"

#include "game/graphics/ctr/CtrDirect.h"
#include "game/graphics/ctr/CtrEye.h"
#include "game/graphics/ctr/CtrLevel.h"
#include "game/graphics/ctr/CtrMerc.h"
#include "game/graphics/ctr/CtrRenderer.h"
#include "game/graphics/ctr/CtrSettings.h"
#include "game/graphics/ctr/CtrShadow.h"
#include "game/graphics/ctr/CtrShadowJak2.h"
#include "game/graphics/ctr/CtrSprite.h"
#include "game/graphics/ctr/CtrTexAnimJak2.h"
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
  kAnimUploadClut = 15,
  kAnimGenericUpload = 16,
  kAnimSetShader = 17,
  kAnimDraw = 18,
  kAnimClouds = 41,
  kAnimCloudsHires = 43,
};

CtrTexAnimJak2& tex_anim() {
  static CtrTexAnimJak2 anim;
  return anim;
}

// the direct renderer's counts at the stats lines (debug level)
void log_stats(CtrDirect& d, CtrRenderState& rs, const std::string& name, int id) {
  if (rs.log_now) {
    const auto& st = d.stats();
    if (st.packets) {
#ifndef __3DS__
      static const bool info = getenv("CTR_DIRECT_STATS") != nullptr;  // host diagnosis
      if (info) {
        lg::info("[ctr] {} {}: {} packets, {} triangles, {} draws, {} skipped prims", name, id,
                 st.packets, st.triangles, st.flushes, st.skipped_prims);
      }
#endif
      lg::debug("[ctr] {} {}: {} packets, {} triangles, {} draws, {} skipped prims", name, id,
                st.packets, st.triangles, st.flushes, st.skipped_prims);
    }
    d.clear_stats();
  }
}

/*!
 * A texture animation array (TextureAnimator::handle_texture_anim_data reads it): the generic
 * CT32 uploads (pc-upload-raw-texture: the boot logos, draw-raw-image) are written to the VRAM
 * copy; the rest (CLUT blends, the sky and cloud textures, scrolling textures) is not done yet and
 * read past.
 */
void handle_anim_array(DmaFollower& dma, CtrRenderState& rs) {
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
      case kAnimUploadClut:
      case kAnimGenericUpload:
        tex_anim().generic_upload(tf.data, (int)tf.size_bytes, rs.ee_mem, rs.vram,
                                  v.immediate == kAnimUploadClut);
        break;
      case kAnimClouds:
      case kAnimCloudsHires:
        tex_anim().clouds(tf.data, (int)tf.size_bytes, rs.vram);
        break;
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

/*!
 * Jak II's texture buckets (TextureUploadHandler): texture page uploads into the emulated VRAM,
 * the texture animation arrays (not done yet: skipped the way TextureAnimator reads them), calls
 * to the default registers, and in some buckets GIF packets drawn directly (add_direct: the map,
 * subtitles, debug text).
 */
class CtrJak2TexRenderer : public CtrBucketRenderer {
 public:
  CtrJak2TexRenderer(std::string name, int id, CtrVram* vram, bool direct, CtrEyeRenderer* eyes)
      : CtrBucketRenderer(std::move(name), id), m_eyes(eyes) {
    if (direct) {
      m_direct = std::make_unique<CtrDirect>(vram);
      m_direct->set_allow_depth(false);
    }
  }

  void render(DmaFollower& dma, CtrRenderState& rs) override {
    if (m_direct) {
      m_direct->reset_state();
#ifndef __3DS__
      static const char* dump = getenv("CTR_DIRECT_DUMP");  // host diagnosis: ",id,frame,"
      if (dump && strstr(dump, fmt::format(",{},{},", m_id, rs.frame_idx).c_str())) {
        g_ctr_direct_dump = getenv("CTR_DIRECT_DUMP_N") ? atoi(getenv("CTR_DIRECT_DUMP_N")) : 60;
        lg::info("[ctr direct] bucket {} ({}) frame {}", m_id, m_name, rs.frame_idx);
      }
#endif
    }
    while (dma.current_tag_offset() != rs.next_bucket) {
      const auto tag = dma.current_tag();
      const VifCode vif0 = dma.current_tag_vifcode0();
      if (vif0.kind == VifCode::Kind::PC_PORT && vif0.immediate == kAnimStart) {
        dma.read_and_advance();
        handle_anim_array(dma, rs);
        continue;
      }
      // the eyes of the characters (TextureUploadHandler: handle_eye_dma2), their GS setup is
      // 8 qw
      if (m_eyes && tag.qwc == 8) {
        const u32 before = dma.current_tag_offset();
        m_eyes->handle_eye_dma(dma, rs);
        if (dma.current_tag_offset() != before) {
          continue;
        }
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
#ifndef __3DS__
        static const bool log_up = getenv("CTR_UPLOAD_LOG") != nullptr;  // host diagnosis
        if (log_up && rs.log_now) {
          lg::info("[ctr] frame {} bucket {}: page upload", rs.frame_idx, m_id);
        }
#endif
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
      log_stats(*m_direct, rs, m_name, m_id);
#ifndef __3DS__
      g_ctr_direct_dump = 0;  // only this bucket's
#endif
    }
  }

 private:
  std::unique_ptr<CtrDirect> m_direct;
  CtrEyeRenderer* m_eyes = nullptr;
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
#ifndef __3DS__
    static const char* dump = getenv("CTR_DIRECT_DUMP");  // host diagnosis: ",id,frame,"
    if (dump && strstr(dump, fmt::format(",{},{},", m_id, rs.frame_idx).c_str())) {
      g_ctr_direct_dump = getenv("CTR_DIRECT_DUMP_N") ? atoi(getenv("CTR_DIRECT_DUMP_N")) : 60;
      lg::info("[ctr direct] bucket {} ({}) frame {}", m_id, m_name, rs.frame_idx);
    }
#endif
    while (dma.current_tag_offset() != rs.next_bucket) {
      // (AI-assisted) draw-raw-image puts its image upload (a texture animation array) here
      const VifCode vif0 = dma.current_tag_vifcode0();
      if (vif0.kind == VifCode::Kind::PC_PORT && vif0.immediate == kAnimStart) {
        dma.read_and_advance();
        handle_anim_array(dma, rs);
        continue;
      }
      auto data = dma.read_and_advance();
      if (data.size_bytes) {
        m_direct->render_vif(data.vif0(), data.vif1(), data.data, data.size_bytes);
      }
    }
    m_direct->flush();
    log_stats(*m_direct, rs, m_name, m_id);
#ifndef __3DS__
    g_ctr_direct_dump = 0;  // only this bucket's
#endif
  }

 private:
  std::unique_ptr<CtrDirect> m_direct;
};

}  // namespace

void CtrRenderer::prepare_textures(const void* ee_mem, u32 chain_offset) {
  if (!m_jak2) {
    return;
  }
  // game thread: the image data of the generic uploads in the animation arrays (the game may
  // rewrite it while the render thread draws the frame)
  auto& anim = tex_anim();
  anim.snapshot_begin();
  const u8* ee = (const u8*)ee_mem;
  for (int id : m_anim_buckets) {
    const u32 end = bucket_tag(chain_offset, id + 1);
    DmaFollower dma(ee_mem, bucket_tag(chain_offset, id));
    bool in_array = false;
    for (int k = 0; k < 100000 && dma.current_tag_offset() != end; k++) {
      auto tf = dma.read_and_advance();
      const VifCode v = tf.vifcode0();
      if (v.kind != VifCode::Kind::PC_PORT) {
        continue;
      }
      if (v.immediate == kAnimStart) {
        in_array = true;
      } else if (v.immediate == kAnimFinish) {
        in_array = false;
      } else if (in_array &&
                 (v.immediate == kAnimGenericUpload || v.immediate == kAnimUploadClut)) {
        anim.snapshot_upload(tf.data, (int)tf.size_bytes, ee);
      }
    }
  }
}

void CtrRenderer::tex_frame_done_jak2() {
  tex_anim().frame_done();
}

void CtrRenderer::init_buckets_jak2() {
  using jak2::BucketId;
  m_buckets.resize((int)BucketId::MAX_BUCKETS);
  CtrJak2BucketSet set = [&](BucketId id, std::unique_ptr<CtrBucketRenderer> r) {
    m_buckets[(int)id] = std::move(r);
  };
  // eyes: drawn to textures from the tex buckets, used by the merc eye draws (no bucket)
  auto eyes = std::make_unique<CtrEyeRenderer>("eyes", -1, m_vram.get());
  CtrEyeRenderer* eye_renderer = eyes.get();
  m_jak2_eyes = std::move(eyes);
  auto tex = [&](BucketId id, bool direct = false) {
    set(id, std::make_unique<CtrJak2TexRenderer>("tex", (int)id, m_vram.get(), direct,
                                                 eye_renderer));
    m_anim_buckets.push_back((int)id);
  };
  auto merc = [&](BucketId id) {
    auto r = std::make_unique<CtrMercRenderer>("merc", (int)id, m_levels.get());
    r->set_eye_renderer(eye_renderer);
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
  // shadow volumes (Shadow2): config.ini shadows = full with a stencil buffer
  set(BucketId::SHADOW, std::make_unique<CtrShadowJak2Renderer>("shadow", (int)BucketId::SHADOW));
  set(BucketId::SHADOW2,
      std::make_unique<CtrShadowJak2Renderer>("shadow", (int)BucketId::SHADOW2));
  tex(BucketId::TEX_LCOM_SKY_POST);
  tex(BucketId::TEX_ALL_SPRITE);
  {
    // particles, world sprites and the HUD's sprites (Sprite3::render_jak2)
    auto sprite = std::make_unique<CtrSpriteRenderer>("sprite", (int)BucketId::PARTICLES,
                                                      m_vram.get());
    m_sprite = sprite.get();
    m_sprite_bucket = (int)BucketId::PARTICLES;
    set(BucketId::PARTICLES, std::move(sprite));
  }
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

  for (auto id : {BucketId::SKY_DRAW, BucketId::PROGRESS, BucketId::SCREEN_FILTER,
                  BucketId::DEBUG2, BucketId::DEBUG_NO_ZBUF2, BucketId::DEBUG3}) {
    m_anim_buckets.push_back((int)id);  // draw-raw-image (the boot logos) may go to any bucket
  }
  for (size_t i = 0; i < m_buckets.size(); i++) {
    if (!m_buckets[i]) {
      m_buckets[i] = std::make_unique<CtrSkipRenderer>("skip", (int)i);
    }
  }
  lg::info("[ctr] Jak II bucket table: {} buckets, {} merc", m_buckets.size(), m_merc.size());
}
