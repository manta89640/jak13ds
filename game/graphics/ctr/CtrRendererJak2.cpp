/*!
 * @file CtrRendererJak2.cpp
 * (AI-assisted)
 * Jak II's bucket table for the 3DS renderer (see CtrRendererJak2.h): the foreground buckets.
 * Like OpenGLRenderer::init_bucket_renderers_jak2, with the 3DS renderers; buckets without one
 * are skipped.
 */

#include "game/graphics/ctr/CtrRendererJak2.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <vector>

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
  kAnimSlime = 42,
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
 * copy; the clouds, (AI-assisted) the slime, the CLUT blends and the fixed animations go to
 * CtrTexAnimJak2; the PS2 drawing codes (erase, set shader, draw) are read past.
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
      case kAnimSlime:
        tex_anim().slime(tf.data, (int)tf.size_bytes);  // (AI-assisted)
        break;
      case kAnimErase:
        extra = 2;
        break;
      case kAnimSetShader:
      case kAnimDraw:
        extra = 1;
        break;
      default:
        // (AI-assisted) the CLUT blends (Dark Jak...) and fixed animations (waterfalls...)
        tex_anim().run(v.immediate, tf.data, (int)tf.size_bytes);
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
    if (id == (int)jak2::BucketId::PROGRESS) {
      m_direct->enable_minimap();
    }
  }
  void render(DmaFollower& dma, CtrRenderState& rs) override {
    m_direct->reset_state();
    // (AI-assisted, gpuopt) sky_last for Jak II's sky (sky-work's GIF packets): its batches are
    // kept and drawn by the sky hook after the opaque level and models, depth tested against them
    // (as Jak 1's CtrSky): the ~3 screens of sky behind the city fail the depth test instead of
    // being blended and written. Not in menu frames (the sky bucket draws the copy of the last
    // frame there, CtrJak2BlitRenderer). Same picture.
    bool defer = m_id == (int)jak2::BucketId::SKY_DRAW && ctr_settings().sky_last &&
                 ctr_direct_menu_screen() < 0;
    if (defer) {
      m_direct->set_record(true);
    }
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
        if (defer) {
          // (AI-assisted, gpuopt) a texture upload after kept batches: they are drawn now, in
          // order (they may read what it changes), and the rest of the bucket as usual
          m_direct->set_record(false);
          m_direct->replay_recorded(false);
          defer = false;
        }
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
    if (defer) {
      m_direct->set_record(false);
      if (m_direct->has_recorded()) {
        ctr_gpu_set_sky_hook(&CtrJak2DirectRenderer::deferred_sky, this);
      }
    }
    m_direct->end_offscreen();
    log_stats(*m_direct, rs, m_name, m_id);
#ifndef __3DS__
    g_ctr_direct_dump = 0;  // only this bucket's
#endif
  }

 private:
  // (AI-assisted, gpuopt) the sky hook (sky_last): the kept batches at depth 0 where nothing
  // opaque wrote depth
  static void deferred_sky(void* arg) {
    auto* self = static_cast<CtrJak2DirectRenderer*>(arg);
#ifndef __3DS__
    const std::string keep = ctr_gpu_soft_bucket_name();
    ctr_gpu_soft_bucket("sky");
#endif
    ctr_gpu_set_depth_far(1);
    self->m_direct->replay_recorded(true);
    ctr_gpu_set_depth_far(0);
#ifndef __3DS__
    ctr_gpu_soft_bucket(keep.c_str());
#endif
  }

  std::unique_ptr<CtrDirect> m_direct;
};

/*!
 * (AI-assisted, j2gaps) Jak II's bucket 3 (BlitDisplays on the PC). When a menu opens
 * (blit-displays, menu-mode) the game stops drawing the world and sends one copy request (pc-port
 * 0x10, tbp 0x3300) here; the sky bucket then draws that copy, tinted, behind the menu every frame
 * (sky-tng.gc). The 3DS kept nothing there: menus showed on black. The frame with the request
 * leaves the color buffer as the last frame left it (CtrRenderer::render_frame clears only the
 * depth then) and nothing is drawn before this bucket, so the copy is the game's last frame, as
 * the PC's copy of its frame buffer before the clear. The copy is given back once the menu stops
 * drawing it (~1 s). Not done: the zoom blur, color filter and slow-time copies (0x13-0x15: Jak II
 * doesn't send them) and the PC's copy-back in the 3 frames after opening / closing (0x11).
 */
class CtrJak2BlitRenderer : public CtrBucketRenderer {
 public:
  static constexpr u32 kCopyTbp = 0x3300;
  CtrJak2BlitRenderer(std::string name, int id) : CtrBucketRenderer(std::move(name), id) {}
  void render(DmaFollower& dma, CtrRenderState& rs) override {
    bool copy = false;
    while (dma.current_tag_offset() != rs.next_bucket) {
      auto data = dma.read_and_advance();
      if (data.vifcode0().kind == VifCode::Kind::PC_PORT && data.vifcode0().immediate == 0x10 &&
          data.vifcode1().immediate == kCopyTbp) {
        copy = true;
      }
    }
    const bool drawn = ctr_direct_take_menu_screen_drawn();
    if (copy) {
      const int h = ctr_gpu_copy_screen_part(-1.f, 1.f);  // (in VRAM when there is room)
      ctr_direct_set_menu_screen(h);
      if (h >= 0) {
        rs.vram->set_override_handle(kCopyTbp, h, 512, 512);
        lg::info("[ctr] menu: the last frame copied for the menu's background");
      }
      m_idle = 0;
    } else if (ctr_direct_menu_screen() >= 0) {
      m_idle = drawn ? 0 : m_idle + 1;
      if (m_idle > 30) {
        rs.vram->clear_override(kCopyTbp);
        ctr_direct_set_menu_screen(-1);
        ctr_gpu_screen_copy_release();
      }
    }
  }

 private:
  int m_idle = 0;  // frames the copy wasn't drawn in
};

/*!
 * (AI-assisted, j2gaps) Jak II's lightning (the EFFECTS bucket: Generic2's LIGHTNING mode on the
 * PC): electric gates' bolts and rings, the guards' and enemies' electric attacks, the prison's
 * and the cutscenes' arcs. The 3DS skipped the bucket, so an electric gate was an invisible wall
 * that still shocked. The DMA as Generic2::process_dma_lightning reads it: per fragment a header
 * (the projection matrix in its first 4 qw, a giftag, two extra GS registers), one adgif and up
 * to 32 vertices of 3 qw (s t as ints, rgba, xyz in camera space; s bit 0 set: no kick). The
 * vertices go to clip space the way generic.vert (not the full-matrix path) and CtrShadowJak2 do
 * it and are drawn as strips with ctr_gpu_draw_clip (the GPU clips them). Generic2's lightning
 * state: no alpha test, depth test GEQUAL without depth writes, blend from the adgif; no fog.
 */
class CtrLightningJak2Renderer : public CtrBucketRenderer {
 public:
  using CtrBucketRenderer::CtrBucketRenderer;
  void render(DmaFollower& dma, CtrRenderState& rs) override {
    auto more = [&]() { return dma.current_tag_offset() != rs.next_bucket; };
    auto skip_rest = [&]() {
      while (more()) {
        dma.read_and_advance();
      }
    };
    if (!more()) {
      return;
    }
    auto first = dma.read_and_advance();
    (void)first;
    if (!more()) {
      return;
    }
    auto direct_setup = dma.read_and_advance();
    if (direct_setup.size_bytes != 32 || !more()) {
      skip_rest();
      return;
    }
    auto constants = dma.read_and_advance();
    if (constants.size_bytes != 128 || constants.vifcode1().kind != VifCode::Kind::UNPACK_V4_32) {
      skip_rest();
      return;
    }
    float fog, hvdf[4];
    memcpy(&fog, constants.data, 4);
    memcpy(hvdf, constants.data + 32, 16);
    if (!(std::abs(fog) > 1e-20f) || !more()) {
      skip_rest();
      return;
    }
    dma.read_and_advance();  // MSCALF STMOD
    if (!more()) {
      return;
    }
    dma.read_and_advance();  // NOP NOP
    bool have_clip = false;
    float clip[16];
    int frags = 0, tris = 0;
    while (more()) {
      auto up1 = dma.read_and_advance();
      if (up1.vifcode1().kind != VifCode::Kind::UNPACK_V4_32 ||
          up1.size_bytes != kHeaderBytes + 5 * 16 || !more()) {
        break;  // the ending (FLUSHA DIRECT ...)
      }
      auto up2 = dma.read_and_advance();
      if (more()) {
        dma.read_and_advance();  // MSCAL
      }
      const u8* header = up1.data;
      float mat_33;
      memcpy(&mat_33, header + 15 * 4, 4);
      if (mat_33 != 0.f) {
        continue;  // a hud matrix (Generic2 keeps them apart; lightning has none)
      }
      if (!have_clip) {
        make_clip(header, fog, hvdf, clip);
        have_clip = true;
      }
      frags++;
      tris += draw_frag(header, up1.data + kHeaderBytes, up2.data, up2.size_bytes / 48, clip, rs);
    }
    skip_rest();
    if (rs.log_now && frags) {
      lg::info("[ctr] lightning: {} fragments, {} triangles", frags, tris);
    }
  }

 private:
  static constexpr u32 kHeaderBytes = 16 * 7;  // Generic2::FRAG_HEADER_SIZE

  // generic.vert: t = -(m * (x, y, z, 1)), screen = t.xyz * fog / t.w + hvdf; as clip space rows
  // (CtrShadowJak2's mapping)
  static void make_clip(const u8* header, float fog, const float* hvdf, float* clip) {
    float persp[4][4];
    memcpy(persp, header, 64);
    const float a = 1.f / fog;
    const float ky = ctr_gs_y_scale() * (1.f / 128.f);
    for (int c = 0; c < 4; c++) {
      const float col[4] = {-persp[c][0], -persp[c][1], -persp[c][2], -persp[c][3]};
      const float w = col[3] * a;
      clip[c] = (col[0] + (hvdf[0] - 2048.f) * w) * (1.f / 256.f);
      clip[4 + c] = -(col[1] + (hvdf[1] - 2048.f) * w) * ky;
      clip[8 + c] = col[2] * (1.f / 8388608.f) + (hvdf[2] * (1.f / 8388608.f) - 1.f) * w;
      clip[12 + c] = w;
    }
  }

  int draw_frag(const u8* header,
                const u8* adgif,
                const u8* vdata,
                u32 nv,
                const float* clip,
                CtrRenderState& rs) {
    if (nv < 3) {
      return 0;
    }
    u64 tex0, tex1, clamp, alpha = 0;
    u8 alpha_addr;
    memcpy(&tex0, adgif, 8);
    memcpy(&tex1, adgif + 16, 8);
    memcpy(&clamp, adgif + 48, 8);
    memcpy(&alpha, adgif + 64, 8);
    alpha_addr = adgif[72];
    bool have_alpha = alpha_addr == (u8)GsRegisterAddress::ALPHA_1;
    // the header's two extra registers (Generic2::determine_draw_modes): an alpha there wins
    u64 bonus[4];
    memcpy(bonus, header + 5 * 16, 32);
    if ((u8)bonus[1] == (u8)GsRegisterAddress::ALPHA_1) {
      alpha = bonus[0];
      have_alpha = true;
    } else if ((u8)bonus[3] == (u8)GsRegisterAddress::ALPHA_1) {
      alpha = bonus[2];
      have_alpha = true;
    }
    ctr_draw_state st;
    memset(&st, 0, sizeof(st));
    st.tex = -1;
    const CtrTexture* tex = rs.vram->get_texture(tex0);
    if (tex && tex->handle >= 0) {
      GsTex0 t(tex0);
      st.tex = tex->handle;
      st.tcc = t.tcc();
      st.decal = t.tfx() == GsTex0::TextureFunction::DECAL;
      st.filter = (tex1 >> 5) & 1;  // MMAG
      st.clamp_s = (clamp & 1) != 0;
      st.clamp_t = (clamp & 4) != 0;
    }
    st.blend = have_alpha ? ctr_blend_from_gs_alpha(alpha) : CTR_BLEND_OFF;
    st.fix = (u8)(alpha >> 32);
    st.atest = CTR_TEST_ALWAYS;
    st.ztest = CTR_TEST_GEQUAL;
    st.zwrite = 0;
    // strips: a vertex with its s bit 0 clear continues the strip, set starts one again with the
    // vertex before it (Generic2::build_index_buffer)
    m_tri.clear();
    ctr_clip_vertex strip[3];
    int in_strip = 0;
    for (u32 i = 0; i < nv; i++) {
      const u8* in = vdata + 48 * i;
      s32 s, t;
      u32 rgba[4];
      float p[3];
      memcpy(&s, in, 4);
      memcpy(&t, in + 4, 4);
      memcpy(rgba, in + 16, 16);
      memcpy(p, in + 32, 12);
      ctr_clip_vertex v;
      v.x = clip[0] * p[0] + clip[1] * p[1] + clip[2] * p[2] + clip[3];
      v.y = clip[4] * p[0] + clip[5] * p[1] + clip[6] * p[2] + clip[7];
      v.z = clip[8] * p[0] + clip[9] * p[1] + clip[10] * p[2] + clip[11];
      v.w = clip[12] * p[0] + clip[13] * p[1] + clip[14] * p[2] + clip[15];
      v.s = (float)(s & ~1) * (1.f / 4096.f);
      v.t = (float)t * (1.f / 4096.f);
      v.r = (u8)std::min<u32>(rgba[0], 255);
      v.g = (u8)std::min<u32>(rgba[1], 255);
      v.b = (u8)std::min<u32>(rgba[2], 255);
      v.a = (u8)std::min<u32>(rgba[3], 255);
      if ((s & 1) == 0) {
        if (in_strip == 3) {
          strip[0] = strip[1];
          strip[1] = strip[2];
          in_strip = 2;
        }
        strip[in_strip++] = v;
      } else {
        if (in_strip > 0) {
          strip[0] = strip[in_strip - 1];
          in_strip = 1;
        }
        strip[in_strip++] = v;
      }
      if (in_strip == 3) {
        m_tri.push_back(strip[0]);
        m_tri.push_back(strip[1]);
        m_tri.push_back(strip[2]);
      }
    }
    if (!m_tri.empty()) {
      ctr_gpu_draw_clip(&st, m_tri.data(), (int)m_tri.size());
    }
    return (int)m_tri.size() / 3;
  }

  std::vector<ctr_clip_vertex> m_tri;
};

}  // namespace

bool ctr_jak2_frame_copies_screen(const void* ee_mem, u32 first_tag, u32 end_tag) {
  DmaFollower dma(ee_mem, first_tag);
  for (int k = 0; k < 16 && dma.current_tag_offset() != end_tag; k++) {
    auto data = dma.read_and_advance();
    if (data.vifcode0().kind == VifCode::Kind::PC_PORT && data.vifcode0().immediate == 0x10) {
      return true;
    }
  }
  return false;
}

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
  auto& anim = tex_anim();
  anim.frame_done();
  if (m_rs.log_now) {
    anim.log_stats();
  }
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
  set(BucketId::BUCKET_3, std::make_unique<CtrJak2BlitRenderer>("blit", (int)BucketId::BUCKET_3));
  set(BucketId::EFFECTS,
      std::make_unique<CtrLightningJak2Renderer>("lightning", (int)BucketId::EFFECTS));

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
