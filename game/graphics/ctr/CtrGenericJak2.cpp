/*!
 * @file CtrGenericJak2.cpp
 * Jak II's generic merc on the 3DS, as Generic2 draws it on the PC (Generic2_DMA.cpp
 * process_dma_jak2, Generic2_Build.cpp, Generic2_OpenGL.cpp, generic.vert / generic.frag):
 *  - the DMA: per fragment a 7 qw header (projection matrix, giftag, two extra GS registers),
 *    adgifs, then positions (V3_32), colors (V4_8) and texture coordinates (V2_16, s bit 0 set:
 *    no kick); a fragment may continue in the next transfer (its positions come there);
 *  - the draw modes from the adgifs and the header registers, with the GS state carried from one
 *    adgif to the next, and the draws grouped into Generic2's buckets, drawn in its alpha order,
 *    the HUD matrix draws last;
 *  - generic.vert's projection (the diagonal of the matrix, mat_23, mat_32, mat_33), its doubled
 *    vertex alpha, and its fog: an opaque fogged draw gets a second pass over the depth it wrote
 *    (fog color, alpha = the fog amount), which is mix(color, fog color, amount) as generic.frag;
 *  - the warp texture (tbp 1216): the screen drawn so far, read at the GS frame coordinates
 *    (generic.vert warp_sample_mode).
 * Fog of blended or depth-masked fogged draws is not drawn.
 */

#include "game/graphics/ctr/CtrGenericJak2.h"

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "common/dma/dma.h"
#include "common/dma/dma_chain_read.h"
#include "common/dma/gs.h"
#include "common/log/log.h"

#include "game/graphics/ctr/CtrVram.h"
#include "game/graphics/ctr/ctr_gpu.h"

namespace {

constexpr u32 kHeaderBytes = 16 * 7;
constexpr u32 kWarpTbp = 1216;
constexpr int kMaxBatchVerts = 65535;

struct Vertex {
  float xyz[3];
  u8 rgba[4];
  float s, t;
  bool adc;
};

struct Fragment {
  const u8* header;
  u32 adgif_idx = 0, adgif_count = 0;
  u32 vtx_idx = 0, vtx_count = 0;
  bool hud = false;
};

struct Adgif {
  AdGifData data;
  DrawMode mode;
  u32 tbp = 0;
  u32 fix = 0;
  bool fog = false;
  u32 frag = 0;
  u32 vtx_idx = 0, vtx_count = 0;
  bool hud = false;
  u32 next = UINT32_MAX;

  u64 key() const {
    u64 result = mode.as_int();
    result |= ((u64)tbp) << 32;
    result |= ((u64)fix) << 48;
    result |= (hud ? 1ull : 0ull) << 62;
    return result;
  }
};

struct Bucket {
  u32 start = UINT32_MAX, last = UINT32_MAX;
};

// generic.vert's projection: scale, mat_23, mat_32, mat_33
struct Proj {
  float scale[3] = {0, 0, 0};
  float m23 = 0, m32 = 0, m33 = 0;
};

// the render thread's scratch, shared by all generic buckets
struct Work {
  std::vector<Vertex> verts;
  std::vector<Fragment> frags;
  std::vector<Adgif> ads;
  std::vector<Bucket> buckets;
  std::unordered_map<u64, u32> key_to_bucket;
  std::vector<u32> order;
  std::vector<int> tex;
  std::vector<u8> warp;
  std::vector<int> nverts, nidx;  // per bucket: what draw_batch writes (0: left out)
  std::vector<u8> fog;            // per bucket: has a fog pass
  std::vector<ctr_draw_state> states;
  std::vector<int> first, count;
  void reset() {
    verts.clear();
    frags.clear();
    ads.clear();
    buckets.clear();
    key_to_bucket.clear();
    order.clear();
  }
};
Work g_work;

int g_copy = -1;
u64 g_copy_frame = ~0ull;

// Generic2::build_index_buffer: a kicked vertex continues the strip, an unkicked one starts it
// again with the vertex before it. put(vertex index) -> output index, tri(a, b, c)
template <typename Put, typename Tri>
void walk_strip(const Adgif& ad, const std::vector<Vertex>& verts, Put&& put, Tri&& tri) {
  int a = -1, b = -1, last = -1;
  for (u32 i = 0; i < ad.vtx_count; i++) {
    const u32 vi = ad.vtx_idx + i;
    const int c = put(vi);
    if (verts[vi].adc) {
      if (a >= 0) {
        tri(a, b, c);
      }
      a = b;
    } else if (i > 0) {
      a = last;
    } else if (vi > 0) {
      a = put(vi - 1);
    } else {
      a = -1;
    }
    b = c;
    last = c;
  }
}

void count_strip(const Adgif& ad, const std::vector<Vertex>& verts, int& nv, int& ni) {
  walk_strip(ad, verts, [&](u32) { return nv++; }, [&](int, int, int) { ni += 3; });
}

VifCode vif_at(const u8* data, u32 off) {
  u32 v;
  memcpy(&v, data + off, 4);
  return VifCode(v);
}

bool is_nop_at(const u8* data, u32 off) {
  return vif_at(data, off).kind == VifCode::Kind::NOP;
}

bool is_nop_zero(const DmaTransfer& xf) {
  return xf.size_bytes == 0 && xf.vifcode0().kind == VifCode::Kind::NOP &&
         xf.vifcode1().kind == VifCode::Kind::NOP;
}

bool is_jak2_end(const DmaTransfer& xf) {
  return xf.size_bytes == 160 && xf.vifcode0().kind == VifCode::Kind::FLUSHA &&
         xf.vifcode1().kind == VifCode::Kind::DIRECT;
}

void unpack_positions(Vertex* v, const u8* data, u32 n) {
  for (u32 i = 0; i < n; i++) {
    memcpy(v[i].xyz, data + 12 * i, 12);
  }
}

void unpack_colors(Vertex* v, const u8* data, u32 n) {
  for (u32 i = 0; i < n; i++) {
    memcpy(v[i].rgba, data + 4 * i, 4);
  }
}

void unpack_tcs(Vertex* v, const u8* data, u32 n) {
  for (u32 i = 0; i < n; i++) {
    s16 s, t;
    memcpy(&s, data + 4 * i, 2);
    memcpy(&t, data + 4 * i + 2, 2);
    const s16 sm = s & (s16)0xfffe;
    v[i].s = sm;
    v[i].t = t;
    v[i].adc = sm == s;
  }
}

/*!
 * Generic2::handle_fragments_after_unpack_v4_32: the header, adgifs and vertex data of one
 * fragment from off in data (end: the transfer's size). False if it isn't laid out like that.
 */
bool read_fragment(Work& w,
                   const u8* data,
                   u32 off,
                   u32 first_bytes,
                   u32 end,
                   Fragment* frag,
                   bool loop,
                   u32* off_out) {
  const u32 off_aligned = (off + 15) & ~15u;
  if (first_bytes < kHeaderBytes + sizeof(AdGifData) || off_aligned + first_bytes > end) {
    return false;
  }
  frag->header = data + off_aligned;
  const u32 adgif_bytes = first_bytes - kHeaderBytes;
  const u32 adgifs = adgif_bytes / sizeof(AdGifData);
  if (adgifs * sizeof(AdGifData) != adgif_bytes) {
    return false;
  }
  frag->adgif_idx = (u32)w.ads.size();
  frag->adgif_count = adgifs;
  for (u32 i = 0; i < adgifs; i++) {
    Adgif& ad = w.ads.emplace_back();
    memcpy(&ad.data, data + off_aligned + kHeaderBytes + i * sizeof(AdGifData),
           sizeof(AdGifData));
  }
  off += first_bytes;
  auto skip_nops = [&]() {
    while (off + 4 <= end && is_nop_at(data, off)) {
      off += 4;
    }
  };
  auto tag = [&](VifCode* code) {
    if (off + 4 > end) {
      return false;
    }
    *code = vif_at(data, off);
    return true;
  };
  VifCode code(0);
  skip_nops();
  if (!tag(&code) || code.kind != VifCode::Kind::STCYCL || code.immediate != 0x103) {
    return false;
  }
  off += 4;
  if (!tag(&code)) {
    return false;
  }
  if (code.kind == VifCode::Kind::UNPACK_V4_8) {
    if (!loop) {
      return false;
    }
  } else {
    if (loop || code.kind != VifCode::Kind::UNPACK_V3_32) {
      return false;
    }
    off += 4;
    frag->vtx_idx = (u32)w.verts.size();
    frag->vtx_count = code.num;
    if (off + code.num * 12 > end) {
      return false;
    }
    w.verts.resize(w.verts.size() + code.num);
    unpack_positions(&w.verts[frag->vtx_idx], data + off, code.num);
    off += code.num * 12;
    skip_nops();
  }
  if (!tag(&code) || code.kind != VifCode::Kind::UNPACK_V4_8) {
    return false;
  }
  off += 4;
  if (loop) {
    frag->vtx_idx = (u32)w.verts.size();
    frag->vtx_count = code.num;
    w.verts.resize(w.verts.size() + code.num);
  } else if (code.num != frag->vtx_count) {
    return false;
  }
  if (off + frag->vtx_count * 4 > end) {
    return false;
  }
  unpack_colors(&w.verts[frag->vtx_idx], data + off, frag->vtx_count);
  off += frag->vtx_count * 4;
  skip_nops();
  if (!tag(&code) || code.kind != VifCode::Kind::UNPACK_V2_16 || code.num != frag->vtx_count) {
    return false;
  }
  off += 4;
  if (off + frag->vtx_count * 4 > end) {
    return false;
  }
  unpack_tcs(&w.verts[frag->vtx_idx], data + off, frag->vtx_count);
  off += frag->vtx_count * 4;
  if (off == end) {
    *off_out = off;
    return true;
  }
  skip_nops();
  if (!tag(&code)) {
    return false;
  }
  off += 4;
  if (code.kind == VifCode::Kind::STCYCL) {
    skip_nops();
    if (!tag(&code) || code.kind != VifCode::Kind::MSCAL) {
      return false;
    }
    off += 4;
  } else {
    if (code.kind != VifCode::Kind::MSCAL) {
      return false;
    }
    skip_nops();
    if (!tag(&code) || code.kind != VifCode::Kind::STCYCL) {
      return false;
    }
    off += 4;
  }
  while (off + 4 <= end && (vif_at(data, off).kind == VifCode::Kind::NOP ||
                            vif_at(data, off).kind == VifCode::Kind::FLUSHE)) {
    off += 4;
  }
  *off_out = off;
  return true;
}

// Generic2::check_for_end_of_generic_data: true at the bucket's end (its GS reset call)
bool at_end(DmaFollower& dma, u32 next_bucket) {
  while (dma.current_tag_offset() != next_bucket && dma.current_tag().qwc == 0 &&
         dma.current_tag_vifcode0().kind == VifCode::Kind::NOP &&
         dma.current_tag_vifcode1().kind == VifCode::Kind::NOP) {
    if (dma.current_tag().kind == DmaTag::Kind::CALL) {
      return true;
    }
    dma.read_and_advance();
  }
  return dma.current_tag_offset() == next_bucket;
}

/*!
 * generic.vert (no full matrix) and the 3DS's clip space, row major: clip = M (x, y, z, 1).
 * t = -(x sx, y sy, z sz + m32, z m23 + m33), screen = t.xyz pfog0 / t.w + hvdf, then the GS
 * screen to clip space times t.w (as CtrLightningJak2Renderer::make_clip).
 */
void make_clip(const Proj& p, float pfog0, const float* hvdf, float* m) {
  const float ky = ctr_gs_y_scale() * (1.f / 128.f);
  const float kz = 1.f / 8388608.f;
  const float hx = hvdf[0] - 2048.f, hy = hvdf[1] - 2048.f, hz = hvdf[2];
  // row x: (t.x pfog0 + hx t.w) / 256
  m[0] = -p.scale[0] * pfog0 * (1.f / 256.f);
  m[1] = 0;
  m[2] = -hx * p.m23 * (1.f / 256.f);
  m[3] = -hx * p.m33 * (1.f / 256.f);
  // row y: -(t.y pfog0 + hy t.w) ky
  m[4] = 0;
  m[5] = p.scale[1] * pfog0 * ky;
  m[6] = hy * p.m23 * ky;
  m[7] = hy * p.m33 * ky;
  // row z: (t.z pfog0 + hz t.w) / 2^23 - t.w
  m[8] = 0;
  m[9] = 0;
  m[10] = -p.scale[2] * pfog0 * kz - hz * p.m23 * kz + p.m23;
  m[11] = -p.m32 * pfog0 * kz - hz * p.m33 * kz + p.m33;
  // row w: t.w
  m[12] = 0;
  m[13] = 0;
  m[14] = -p.m23;
  m[15] = -p.m33;
}

u8 blend_of(DrawMode::AlphaBlend b) {
  switch (b) {
    case DrawMode::AlphaBlend::SRC_DST_SRC_DST:
      return CTR_BLEND_ALPHA;
    case DrawMode::AlphaBlend::SRC_0_SRC_DST:
      return CTR_BLEND_ADD;
    case DrawMode::AlphaBlend::ZERO_SRC_SRC_DST:
      return CTR_BLEND_SUB;
    case DrawMode::AlphaBlend::SRC_DST_FIX_DST:
      return CTR_BLEND_FIX;
    case DrawMode::AlphaBlend::SRC_0_DST_DST:
      return CTR_BLEND_ADD_DST_A;
    case DrawMode::AlphaBlend::SRC_0_FIX_DST:
      return CTR_BLEND_ONE_ONE;
    default:
      return CTR_BLEND_OFF;
  }
}

u8 ztest_of(GsTest::ZTest z) {
  switch (z) {
    case GsTest::ZTest::NEVER:
      return CTR_TEST_NEVER;
    case GsTest::ZTest::ALWAYS:
      return CTR_TEST_ALWAYS;
    case GsTest::ZTest::GREATER:
      return CTR_TEST_GREATER;
    default:
      return CTR_TEST_GEQUAL;
  }
}

class CtrGenericJak2Renderer : public CtrBucketRenderer {
 public:
  CtrGenericJak2Renderer(std::string name, int id, bool warp)
      : CtrBucketRenderer(std::move(name), id), m_warp(warp) {}

  void render(DmaFollower& dma, CtrRenderState& rs) override {
    Work& w = g_work;
    w.reset();
    const bool ok = read_dma(dma, rs, w);
    while (dma.current_tag_offset() != rs.next_bucket) {
      dma.read_and_advance();
    }
    if (!ok) {
      m_errors++;
    } else if (!w.frags.empty()) {
      draw(rs, w);
    }
    if (rs.log_now && (m_frags || m_errors)) {
      lg::info("[ctr] {}: {} fragments, {} triangles, {} draws, {} fog passes, {} dropped, {} bad "
               "buckets (since the last stats)",
               m_name, m_frags, m_tris, m_draws, m_fog_passes, m_dropped, m_errors);
      m_frags = m_tris = m_draws = m_fog_passes = m_dropped = m_errors = 0;
    }
  }

 private:
  bool read_dma(DmaFollower& dma, CtrRenderState& rs, Work& w) {
    auto more = [&]() { return dma.current_tag_offset() != rs.next_bucket; };
    if (!more()) {
      return true;
    }
    auto first = dma.read_and_advance();
    if (is_nop_zero(first) && !more()) {
      return true;
    }
    const auto v0k = first.vifcode0().kind;
    if ((v0k != VifCode::Kind::MARK && v0k != VifCode::Kind::NOP) ||
        first.vifcode1().kind != VifCode::Kind::NOP || !more()) {
      return false;
    }
    auto direct_setup = dma.read_and_advance();
    if (direct_setup.size_bytes != 32 || direct_setup.vifcode1().kind != VifCode::Kind::DIRECT ||
        !more()) {
      return false;
    }
    u64 zbuf;
    memcpy(&zbuf, direct_setup.data + 16, 8);
    m_zmsk = (zbuf >> 32) & 1;
    auto constants = dma.read_and_advance();
    if (constants.size_bytes != 128 || constants.vifcode0().kind != VifCode::Kind::STCYCL ||
        constants.vifcode1().kind != VifCode::Kind::UNPACK_V4_32 || !more()) {
      return false;
    }
    memcpy(&m_pfog0, constants.data + 0, 4);
    memcpy(&m_fog_min, constants.data + 4, 4);
    memcpy(&m_fog_max, constants.data + 8, 4);
    memcpy(m_hvdf, constants.data + 32, 16);
    auto vu_setup = dma.read_and_advance();
    if (vu_setup.size_bytes != 32) {
      return false;
    }
    if (is_nop_zero(first) && !more()) {
      return true;
    }
    int continued = -1;
    while (more()) {
      if (at_end(dma, rs.next_bucket)) {
        break;
      }
      auto xf = dma.read_and_advance();
      if (is_nop_zero(xf)) {
        continue;
      }
      if (is_jak2_end(xf)) {
        break;
      }
      if (continued >= 0) {
        const Fragment& cf = w.frags[continued];
        const auto up = xf.vifcode1();
        if (xf.vifcode0().kind != VifCode::Kind::NOP || up.kind != VifCode::Kind::UNPACK_V3_32 ||
            up.num != cf.vtx_count || xf.size_bytes < up.num * 12 || !more()) {
          return false;
        }
        unpack_positions(&w.verts[cf.vtx_idx], xf.data, cf.vtx_count);
        continued = -1;
        auto call = dma.read_and_advance();
        if (call.size_bytes != 0 || call.vifcode1().kind != VifCode::Kind::MSCAL) {
          return false;
        }
        if (at_end(dma, rs.next_bucket)) {
          return true;
        }
        continue;
      }
      const auto v1 = xf.vifcode1();
      if (xf.vifcode0().kind != VifCode::Kind::STCYCL || v1.kind != VifCode::Kind::UNPACK_V4_32) {
        return false;
      }
      w.frags.emplace_back();
      u32 off = 0;
      if (!read_fragment(w, xf.data, 0, v1.num * 16, xf.size_bytes, &w.frags.back(), false,
                         &off)) {
        return false;
      }
      if (at_end(dma, rs.next_bucket)) {
        return true;
      }
      if (off < xf.size_bytes) {
        if (off + 8 > xf.size_bytes || vif_at(xf.data, off).kind != VifCode::Kind::STCYCL) {
          return false;
        }
        off += 4;
        const VifCode next = vif_at(xf.data, off);
        if (next.kind != VifCode::Kind::UNPACK_V4_32) {
          return false;
        }
        w.frags.emplace_back();
        if (!read_fragment(w, xf.data, off, next.num * 16, xf.size_bytes, &w.frags.back(), true,
                           &off) ||
            off != xf.size_bytes) {
          return false;
        }
        continued = (int)w.frags.size() - 1;
      }
    }
    return continued < 0;
  }

  // Generic2::link_adgifs_back_to_frags, process_matrices and determine_draw_modes
  bool setup(Work& w) {
    for (u32 i = 0; i < w.frags.size(); i++) {
      const Fragment& f = w.frags[i];
      for (u32 j = 0; j < f.adgif_count; j++) {
        Adgif& ad = w.ads[f.adgif_idx + j];
        ad.vtx_count = (ad.data.tex1_addr >> 32) & 0xfff;
        ad.vtx_idx = f.vtx_idx + ((ad.data.tex0_addr >> 32) & 0xffff) / 3;
        if (ad.vtx_idx + ad.vtx_count > f.vtx_idx + f.vtx_count) {
          return false;
        }
        ad.frag = i;
      }
    }
    bool found_proj = false, found_hud = false;
    m_proj = Proj();
    m_hud = Proj();
    for (Fragment& f : w.frags) {
      float m[16];
      memcpy(m, f.header, 64);
      f.hud = m[15] != 0.f;
      Proj& p = f.hud ? m_hud : m_proj;
      bool& found = f.hud ? found_hud : found_proj;
      if (!found) {
        found = true;
        p.scale[0] = m[0];
        p.scale[1] = m[5];
        p.scale[2] = m[10];
        p.m23 = m[11];
        p.m32 = m[14];
        p.m33 = f.hud ? m[15] : 0.f;
      }
    }

    DrawMode mode;
    mode.set_at(true);
    mode.set_alpha_test(DrawMode::AlphaTest::GEQUAL);
    mode.set_aref(0x26);
    mode.set_alpha_fail(GsTest::AlphaFail::FB_ONLY);
    mode.set_zt(true);
    mode.set_depth_test(GsTest::ZTest::GEQUAL);
    mode.set_depth_write_enable(!m_zmsk);
    mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_SRC_SRC_SRC);
    bool fog = !m_warp;
    u32 tbp = UINT32_MAX;
    u64 tex0 = UINT64_MAX;
    GsAlpha gs_alpha;
    for (Adgif& ad : w.ads) {
      const Fragment& f = w.frags[ad.frag];
      ad.hud = f.hud;
      GifTag tag(f.header + 4 * 16);
      if (tag.pre()) {
        fog = GsPrim(tag.prim()).fge();
      }
      const AdGifData& d = ad.data;
      if ((u8)d.tex0_addr != (u8)GsRegisterAddress::TEX0_1 ||
          (u8)d.tex1_addr != (u8)GsRegisterAddress::TEX1_1 ||
          (u8)d.clamp_addr != (u8)GsRegisterAddress::CLAMP_1) {
        return false;
      }
      if (d.tex0_data != tex0) {
        tex0 = d.tex0_data;
        GsTex0 reg(d.tex0_data);
        tbp = reg.tbp0();
        if (reg.psm() == GsTex0::PSM::PSMT4HH) {
          tbp |= 0x8000;
        }
        mode.set_tcc(reg.tcc());
        mode.set_decal(reg.tfx() == GsTex0::TextureFunction::DECAL);
      }
      mode.set_filt_enable(GsTex1(d.tex1_data).mmag());
      mode.set_clamp_s_enable(d.clamp_data & 0b001);
      mode.set_clamp_t_enable(d.clamp_data & 0b100);

      bool have_alpha = false, have_test = false;
      u64 alpha = 0, test = 0;
      if ((u8)d.alpha_addr == (u8)GsRegisterAddress::ALPHA_1) {
        alpha = d.alpha_data;
        have_alpha = true;
      }
      u64 bonus[4];
      memcpy(bonus, f.header + 5 * 16, sizeof(bonus));
      if ((u8)bonus[1] == (u8)GsRegisterAddress::ALPHA_1) {
        alpha = bonus[0];
        have_alpha = true;
        test = bonus[2];
        have_test = true;
      } else {
        if ((u8)bonus[1] == (u8)GsRegisterAddress::TEST_1) {
          test = bonus[0];
          have_test = true;
        }
        if ((u8)bonus[3] == (u8)GsRegisterAddress::ALPHA_1) {
          alpha = bonus[2];
          have_alpha = true;
        } else if ((u8)bonus[3] == (u8)GsRegisterAddress::TEST_1) {
          test = bonus[2];
          have_test = true;
        }
      }
      if (have_alpha && GsAlpha(alpha) != gs_alpha) {
        gs_alpha = GsAlpha(alpha);
        using BM = GsAlpha::BlendMode;
        const BM a = gs_alpha.a_mode(), b = gs_alpha.b_mode(), c = gs_alpha.c_mode(),
                 dd = gs_alpha.d_mode();
        if (a == BM::SOURCE && b == BM::DEST && c == BM::SOURCE && dd == BM::DEST) {
          mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_DST_SRC_DST);
        } else if (a == BM::SOURCE && b == BM::ZERO_OR_FIXED && c == BM::SOURCE && dd == BM::DEST) {
          mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_0_SRC_DST);
        } else if (a == BM::ZERO_OR_FIXED && b == BM::SOURCE && c == BM::SOURCE && dd == BM::DEST) {
          mode.set_alpha_blend(DrawMode::AlphaBlend::ZERO_SRC_SRC_DST);
        } else if (a == BM::SOURCE && b == BM::DEST && c == BM::ZERO_OR_FIXED && dd == BM::DEST) {
          mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_DST_FIX_DST);
        } else if (a == BM::SOURCE && b == BM::SOURCE && c == BM::SOURCE && dd == BM::SOURCE) {
          mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_SRC_SRC_SRC);
        } else if (a == BM::SOURCE && b == BM::ZERO_OR_FIXED && c == BM::DEST && dd == BM::DEST) {
          mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_0_DST_DST);
        } else if (a == BM::SOURCE && b == BM::ZERO_OR_FIXED && c == BM::ZERO_OR_FIXED &&
                   dd == BM::DEST) {
          mode.set_alpha_blend(DrawMode::AlphaBlend::SRC_0_FIX_DST);
        }
      }
      if (have_test) {
        GsTest reg(test);
        mode.set_at(reg.alpha_test_enable());
        if (reg.alpha_test_enable()) {
          switch (reg.alpha_test()) {
            case GsTest::AlphaTest::NEVER:
              mode.set_alpha_test(DrawMode::AlphaTest::NEVER);
              break;
            case GsTest::AlphaTest::ALWAYS:
              mode.set_alpha_test(DrawMode::AlphaTest::ALWAYS);
              break;
            case GsTest::AlphaTest::GEQUAL:
              mode.set_alpha_test(DrawMode::AlphaTest::GEQUAL);
              break;
            default:
              break;
          }
        }
        mode.set_aref(reg.aref());
        mode.set_alpha_fail(reg.afail());
        mode.set_zt(reg.zte());
        mode.set_depth_test(reg.ztest());
        if (mode.get_alpha_fail() == GsTest::AlphaFail::FB_ONLY && mode.get_aref() == 0x80 &&
            mode.get_alpha_test() == DrawMode::AlphaTest::GEQUAL) {
          mode.set_alpha_test(DrawMode::AlphaTest::ALWAYS);
          mode.disable_depth_write();
        }
        if (mode.get_alpha_test() == DrawMode::AlphaTest::NEVER &&
            mode.get_alpha_fail() == GsTest::AlphaFail::FB_ONLY) {
          mode.set_alpha_test(DrawMode::AlphaTest::ALWAYS);
          mode.disable_depth_write();
        }
      }
      ad.mode = mode;
      ad.fog = fog;
      ad.tbp = tbp;
      ad.fix = gs_alpha.fix();
    }

    // Generic2::draws_to_buckets
    for (u32 i = 0; i < w.ads.size(); i++) {
      Adgif& ad = w.ads[i];
      ad.next = UINT32_MAX;
      if (!ad.hud) {
        auto it = w.key_to_bucket.find(ad.key());
        if (it != w.key_to_bucket.end()) {
          Bucket& b = w.buckets[it->second];
          w.ads[b.last].next = i;
          b.last = i;
          continue;
        }
        w.key_to_bucket[ad.key()] = (u32)w.buckets.size();
      }
      w.buckets.push_back({i, i});
    }
    // Generic2::do_draws: by alpha mode, then the HUD draws
    constexpr DrawMode::AlphaBlend kOrder[] = {
        DrawMode::AlphaBlend::SRC_0_FIX_DST,    DrawMode::AlphaBlend::SRC_SRC_SRC_SRC,
        DrawMode::AlphaBlend::SRC_DST_SRC_DST,  DrawMode::AlphaBlend::SRC_0_SRC_DST,
        DrawMode::AlphaBlend::ZERO_SRC_SRC_DST, DrawMode::AlphaBlend::SRC_DST_FIX_DST,
        DrawMode::AlphaBlend::SRC_0_DST_DST,
    };
    for (auto blend : kOrder) {
      for (u32 i = 0; i < w.buckets.size(); i++) {
        const Adgif& first = w.ads[w.buckets[i].start];
        if (!first.hud && first.mode.get_alpha_blend() == blend) {
          w.order.push_back(i);
        }
      }
    }
    m_first_hud = (u32)w.order.size();
    for (u32 i = 0; i < w.buckets.size(); i++) {
      if (w.ads[w.buckets[i].start].hud) {
        w.order.push_back(i);
      }
    }
    return true;
  }

  // the draw's texture: the warp texture is the screen so far (Warp::render's copy)
  void bucket_textures(CtrRenderState& rs, Work& w) {
    w.tex.assign(w.buckets.size(), -1);
    w.warp.assign(w.buckets.size(), 0);
    if (m_warp) {
      g_copy_frame = ~0ull;
    }
    for (u32 i = 0; i < w.buckets.size(); i++) {
      const Adgif& first = w.ads[w.buckets[i].start];
      if ((first.tbp & 0x7fff) == kWarpTbp) {
        if (g_copy_frame != rs.frame_idx) {
          g_copy = ctr_gpu_copy_screen();
          g_copy_frame = rs.frame_idx;
        }
        w.tex[i] = g_copy;  // (-1: no copy, the bucket is left out)
        w.warp[i] = 1;
      } else {
        const CtrTexture* t = rs.vram->get_texture(first.data.tex0_data);
        w.tex[i] = t ? t->handle : -1;
      }
    }
  }

  ctr_draw_state state_of(const Adgif& first, int tex, bool warp) const {
    const DrawMode& m = first.mode;
    ctr_draw_state st;
    memset(&st, 0, sizeof(st));
    st.tex = tex;
    st.tcc = m.get_tcc_enable() ? 1 : 0;
    st.decal = m.get_decal() ? 1 : 0;
    st.filter = m.get_filt_enable() ? 1 : 0;
    st.clamp_s = warp || m.get_clamp_s_enable();
    st.clamp_t = warp || m.get_clamp_t_enable();
    st.blend = blend_of(m.get_alpha_blend());
    st.fix = (u8)first.fix;
    st.atest = CTR_TEST_ALWAYS;
    if (m.get_at_enable() && m.get_alpha_test() == DrawMode::AlphaTest::GEQUAL) {
      st.atest = CTR_TEST_GEQUAL;
      st.aref = m.get_aref();
    }
    st.ztest = m.get_zt_enable() ? ztest_of(m.get_depth_test()) : CTR_TEST_ALWAYS;
    st.zwrite = m.get_depth_write_enable() ? 1 : 0;
    return st;
  }

  // fog amount 0..128 (generic.vert: 255 - clamp(-t.w + hvdf.w, fog_min, fog_max), / 255)
  u8 fog_of(const Proj& p, float z) const {
    const float d = std::clamp(p.m23 * z + p.m33 + m_hvdf[3], m_fog_min, m_fog_max);
    const float f = std::clamp((255.f - d) * (1.f / 255.f), 0.f, 1.f);
    return (u8)(f * 128.f + 0.5f);
  }

  // an opaque draw that writes depth: its fog is a second pass over the depth it wrote
  static bool fog_pass_ok(const Adgif& first) {
    return first.mode.get_alpha_blend() == DrawMode::AlphaBlend::SRC_SRC_SRC_SRC &&
           first.mode.get_depth_write_enable() && first.mode.get_zt_enable();
  }

  void draw(CtrRenderState& rs, Work& w) {
    if (!setup(w)) {
      m_errors++;
      return;
    }
    m_frags += (int)w.frags.size();
    bucket_textures(rs, w);
    bucket_sizes(w);
    float proj_clip[16], hud_clip[16];
    make_clip(m_proj, m_pfog0, m_hvdf, proj_clip);
    make_clip(m_hud, m_pfog0, m_hvdf, hud_clip);
    // batches of whole buckets: at most kMaxBatchVerts vertices, HUD draws apart
    size_t k = 0;
    while (k < w.order.size()) {
      const bool hud = k >= m_first_hud;
      int nv = 0;
      size_t end = k;
      while (end < w.order.size() && (end >= m_first_hud) == hud) {
        const int bv = w.nverts[w.order[end]];
        if (end > k && nv + bv > kMaxBatchVerts) {
          break;
        }
        nv += bv;
        end++;
      }
      draw_range(rs, w, k, end, hud, hud ? hud_clip : proj_clip);
      k = end;
    }
  }

  // the vertices and indices draw_batch writes per bucket, and whether it has a fog pass
  void bucket_sizes(Work& w) const {
    const size_t n = w.buckets.size();
    w.nverts.assign(n, 0);
    w.nidx.assign(n, 0);
    w.fog.assign(n, 0);
    for (u32 i = 0; i < n; i++) {
      if (w.warp[i] && w.tex[i] < 0) {
        continue;
      }
      const Adgif& first = w.ads[w.buckets[i].start];
      const Proj& proj = first.hud ? m_hud : m_proj;
      int nv = 0, ni = 0;
      for (u32 a = w.buckets[i].start; a != UINT32_MAX; a = w.ads[a].next) {
        count_strip(w.ads[a], w.verts, nv, ni);
      }
      if (fog_pass_ok(first)) {
        bool any = false;
        for (u32 a = w.buckets[i].start; a != UINT32_MAX && !any; a = w.ads[a].next) {
          const Adgif& ad = w.ads[a];
          for (u32 j = 0; ad.fog && j < ad.vtx_count && !any; j++) {
            any = fog_of(proj, w.verts[ad.vtx_idx + j].xyz[2]) != 0;
          }
        }
        if (any) {
          w.fog[i] = 1;
          for (u32 a = w.buckets[i].start; a != UINT32_MAX; a = w.ads[a].next) {
            if (w.ads[a].fog) {
              count_strip(w.ads[a], w.verts, nv, ni);
            }
          }
        }
      }
      w.nverts[i] = nv;
      w.nidx[i] = ni;
    }
  }

  // buckets [from, to) in one reservation, or in halves when it doesn't fit
  void draw_range(CtrRenderState& rs,
                  Work& w,
                  size_t from,
                  size_t to,
                  bool hud,
                  const float* clip) {
    int nv = 0, ni = 0;
    for (size_t k = from; k < to; k++) {
      nv += w.nverts[w.order[k]];
      ni += w.nidx[w.order[k]];
    }
    if (ni < 3) {
      return;
    }
    if (nv <= kMaxBatchVerts && draw_batch(rs, w, from, to, hud, clip, nv, ni)) {
      return;
    }
    if (to - from > 1) {
      const size_t mid = from + (to - from) / 2;
      draw_range(rs, w, from, mid, hud, clip);
      draw_range(rs, w, mid, to, hud, clip);
      return;
    }
    m_dropped++;
  }

  // false: no room for the reservation
  bool draw_batch(CtrRenderState& rs,
                  Work& w,
                  size_t from,
                  size_t to,
                  bool hud,
                  const float* clip,
                  int max_verts,
                  int max_indices) {
    ctr_clip_vertex* out;
    u16* idx;
    if (!ctr_gpu_clip_reserve(max_verts, max_indices, &out, &idx)) {
      return false;
    }
    const Proj& proj = hud ? m_hud : m_proj;
    int nv = 0, ni = 0;
    w.states.clear();
    w.first.clear();
    w.count.clear();
    auto put = [&](const Vertex& v, bool warp, u8 fog_a, bool fog_vertex) {
      ctr_clip_vertex& o = out[nv];
      if (hud) {
        const float* p = v.xyz;
        o.x = clip[0] * p[0] + clip[1] * p[1] + clip[2] * p[2] + clip[3];
        o.y = clip[4] * p[0] + clip[5] * p[1] + clip[6] * p[2] + clip[7];
        o.z = clip[8] * p[0] + clip[9] * p[1] + clip[10] * p[2] + clip[11];
        o.w = clip[12] * p[0] + clip[13] * p[1] + clip[14] * p[2] + clip[15];
      } else {
        o.x = v.xyz[0];
        o.y = v.xyz[1];
        o.z = v.xyz[2];
        o.w = 1.f;
      }
      if (fog_vertex) {
        o.s = o.t = 0;
        o.r = rs.fog_color[0];
        o.g = rs.fog_color[1];
        o.b = rs.fog_color[2];
        o.a = fog_a;
      } else {
        const float s = v.s * (1.f / 4096.f), t = v.t * (1.f / 4096.f);
        if (warp) {
          // the GS frame: 512 columns from x 1792, rows from the top of the 416 line screen
          ctr_gpu_screen_uv(2.f * s - 1.f, (208.f - 512.f * t) * ctr_gs_y_scale() * (1.f / 128.f),
                            &o.s, &o.t);
        } else {
          o.s = s;
          o.t = t;
        }
        o.r = v.rgba[0];
        o.g = v.rgba[1];
        o.b = v.rgba[2];
        o.a = v.rgba[3];  // GS units: the texture stage scales it (0x80 = 1)
      }
      return nv++;
    };
    auto strips = [&](const Adgif& ad, bool warp, bool fog_vertex) {
      auto fog_a = [&](const Vertex& v) {
        return fog_vertex && ad.fog ? fog_of(proj, v.xyz[2]) : (u8)0;
      };
      walk_strip(
          ad, w.verts,
          [&](u32 vi) {
            const Vertex& v = w.verts[vi];
            return put(v, warp, fog_a(v), fog_vertex);
          },
          [&](int a, int b, int c) {
            idx[ni++] = (u16)a;
            idx[ni++] = (u16)b;
            idx[ni++] = (u16)c;
            m_tris += !fog_vertex;
          });
    };
    for (size_t k = from; k < to; k++) {
      const u32 bi = w.order[k];
      const Adgif& first = w.ads[w.buckets[bi].start];
      const bool warp = w.warp[bi];
      if (warp && w.tex[bi] < 0) {
        continue;  // no screen copy this frame
      }
      const int range_start = ni;
      for (u32 a = w.buckets[bi].start; a != UINT32_MAX; a = w.ads[a].next) {
        strips(w.ads[a], warp, false);
      }
      if (ni > range_start) {
        w.states.push_back(state_of(first, w.tex[bi], warp));
        w.first.push_back(range_start);
        w.count.push_back(ni - range_start);
        m_draws++;
      }
      if (!w.fog[bi]) {
        continue;
      }
      const int fog_start = ni;
      for (u32 a = w.buckets[bi].start; a != UINT32_MAX; a = w.ads[a].next) {
        if (w.ads[a].fog) {
          strips(w.ads[a], warp, true);
        }
      }
      if (ni > fog_start) {
        ctr_draw_state st;
        memset(&st, 0, sizeof(st));
        st.tex = -1;
        st.blend = CTR_BLEND_ALPHA;
        st.atest = CTR_TEST_ALWAYS;
        st.ztest = CTR_TEST_EQUAL;
        st.zwrite = 0;
        w.states.push_back(st);
        w.first.push_back(fog_start);
        w.count.push_back(ni - fog_start);
        m_fog_passes++;
      }
    }
    const int n = (int)w.states.size();
    if (hud) {
      ctr_gpu_clip_draw_ranges(w.states.data(), w.first.data(), w.count.data(), n, nv, ni);
    } else {
      ctr_gpu_clip_draw_ranges_mtx(clip, n, w.states.data(), w.first.data(), w.count.data(), n,
                                   nv, ni);
    }
    return true;
  }

  bool m_warp;
  bool m_zmsk = false;
  float m_pfog0 = 0, m_fog_min = 0, m_fog_max = 0;
  float m_hvdf[4] = {0, 0, 0, 0};
  Proj m_proj, m_hud;
  u32 m_first_hud = 0;
  int m_frags = 0, m_tris = 0, m_draws = 0, m_fog_passes = 0, m_dropped = 0, m_errors = 0;
};

}  // namespace

std::unique_ptr<CtrBucketRenderer> ctr_make_generic_jak2(const char* name, int id, bool warp) {
  return std::make_unique<CtrGenericJak2Renderer>(name, id, warp);
}
