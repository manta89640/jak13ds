/*!
 * @file CtrDirect.cpp
 * (AI-assisted)
 * See CtrDirect.h. The GS behavior follows game/graphics/opengl_renderer/DirectRenderer.cpp.
 */

#include "CtrDirect.h"

#include <cstdlib>
#include <cstring>

#include "common/dma/dma.h"
#include "common/log/log.h"

#include "game/graphics/ctr/CtrVram.h"

namespace {
constexpr int kMaxBatchVerts = 3 * 1024;

u32 get_direct_qwc_or_nop(const VifCode& code, bool* ok) {
  *ok = true;
  switch (code.kind) {
    case VifCode::Kind::NOP:
    case VifCode::Kind::FLUSHA:
    case VifCode::Kind::FLUSH:
    case VifCode::Kind::FLUSHE:
      return 0;
    case VifCode::Kind::DIRECT:
      return code.immediate == 0 ? 65536 : code.immediate;
    default:
      *ok = false;
      return 0;
  }
}

u8 map_ztest(GsTest::ZTest t) {
  switch (t) {
    case GsTest::ZTest::NEVER:
      return CTR_TEST_NEVER;
    case GsTest::ZTest::ALWAYS:
      return CTR_TEST_ALWAYS;
    case GsTest::ZTest::GEQUAL:
      return CTR_TEST_GEQUAL;
    case GsTest::ZTest::GREATER:
      return CTR_TEST_GREATER;
  }
  return CTR_TEST_ALWAYS;
}

u8 map_atest(GsTest::AlphaTest t) {
  switch (t) {
    case GsTest::AlphaTest::NEVER:
      return CTR_TEST_NEVER;
    case GsTest::AlphaTest::ALWAYS:
      return CTR_TEST_ALWAYS;
    case GsTest::AlphaTest::LESS:
      return CTR_TEST_LESS;
    case GsTest::AlphaTest::LEQUAL:
      return CTR_TEST_LEQUAL;
    case GsTest::AlphaTest::EQUAL:
      return CTR_TEST_EQUAL;
    case GsTest::AlphaTest::GEQUAL:
      return CTR_TEST_GEQUAL;
    case GsTest::AlphaTest::GREATER:
      return CTR_TEST_GREATER;
    case GsTest::AlphaTest::NOTEQUAL:
      return CTR_TEST_NOTEQUAL;
  }
  return CTR_TEST_ALWAYS;
}

u8 map_blend(const GsAlpha& a) {
  using B = GsAlpha::BlendMode;
  auto A = a.a_mode(), Bm = a.b_mode(), C = a.c_mode(), D = a.d_mode();
  if (A == B::SOURCE && Bm == B::DEST && C == B::SOURCE && D == B::DEST) {
    return CTR_BLEND_ALPHA;
  }
  if (A == B::SOURCE && Bm == B::ZERO_OR_FIXED && C == B::SOURCE && D == B::DEST) {
    return CTR_BLEND_ADD;
  }
  if (A == B::ZERO_OR_FIXED && Bm == B::SOURCE && C == B::SOURCE && D == B::DEST) {
    return CTR_BLEND_SUB;
  }
  if (A == B::SOURCE && Bm == B::DEST && C == B::ZERO_OR_FIXED && D == B::DEST) {
    return CTR_BLEND_FIX;
  }
  if (A == B::SOURCE && Bm == B::ZERO_OR_FIXED && C == B::DEST && D == B::DEST) {
    return CTR_BLEND_ADD_DST_A;
  }
  if (A == B::SOURCE && Bm == B::SOURCE && C == B::SOURCE && D == B::SOURCE) {
    return CTR_BLEND_OFF;
  }
  return CTR_BLEND_ALPHA;
}

}  // namespace

u8 ctr_blend_from_gs_alpha(u64 alpha) {
  return map_blend(GsAlpha(alpha));
}

namespace {
bool same_state(const ctr_draw_state& a, const ctr_draw_state& b) {
  return memcmp(&a, &b, sizeof(ctr_draw_state)) == 0;
}
}  // namespace

CtrDirect::CtrDirect(CtrVram* vram) : m_vram(vram) {
  m_verts.reserve(kMaxBatchVerts + 6);
  memset(&m_draw_state, 0, sizeof(m_draw_state));
  m_draw_state.tex = -1;
  reset_state();
}

void CtrDirect::reset_state() {
  m_prim = GsPrim();
  m_tex0 = 0;
  m_tex1 = 0;
  m_clamp = 0b101;
  // same defaults as DirectRenderer (the GS state set by the default registers chain):
  // z test GEQUAL, blending (Cs - Cd) * As + Cd
  m_test = GsTest((1ull << 16) | (2ull << 17));
  m_alpha = GsAlpha(0b01'00'01'00);
  m_zbuf_mask = false;
  m_build_idx = 0;
  m_strip_count = 0;
  m_state_dirty = true;
}

void CtrDirect::render_vif(u32 vif0, u32 vif1, const u8* data, u32 size) {
  bool ok0, ok1;
  u32 gif_qwc = get_direct_qwc_or_nop(VifCode(vif0), &ok0);
  if (!gif_qwc) {
    gif_qwc = get_direct_qwc_or_nop(VifCode(vif1), &ok1);
  }
  u32 offset = 0;
  while (offset < size) {
    if (gif_qwc) {
      if (offset & 0xf) {
        offset += 4;  // padding vif codes before the aligned GIF data
      } else {
        u32 bytes = std::min(gif_qwc * 16, size - offset);
        render_gif(data + offset, bytes);
        offset += gif_qwc * 16;
        gif_qwc = 0;
      }
    } else {
      u32 vif;
      memcpy(&vif, data + offset, 4);
      offset += 4;
      bool ok;
      gif_qwc = get_direct_qwc_or_nop(VifCode(vif), &ok);
      if (!ok) {
        // not something we understand (UNPACK etc.): not a direct transfer, give up on this
        // chunk like the GS would never see it.
        return;
      }
    }
  }
}

void CtrDirect::render_gif(const u8* data, u32 size) {
  m_stats.packets++;
  if (getenv("CTR_GIF_DEBUG") && m_stats.packets < 4) {
    GifTag t0(data);
    lg::info("[ctr gif] size {} first tag: {}", size, t0.print());
  }
  u32 offset = 0;
  bool eop = false;
  while (!eop && offset + 16 <= size) {
    GifTag tag(data + offset);
    offset += 16;
    eop = tag.eop();
    u32 nreg = tag.nreg();
    GifTag::RegisterDescriptor regs[16];
    for (u32 i = 0; i < nreg; i++) {
      regs[i] = tag.reg(i);
    }
    auto format = tag.flg();
    if (format == GifTag::Format::PACKED) {
      if (tag.pre()) {
        handle_prim(tag.prim());
      }
      for (u32 loop = 0; loop < tag.nloop(); loop++) {
        for (u32 r = 0; r < nreg; r++) {
          if (offset + 16 > size) {
            return;
          }
          const u8* d = data + offset;
          switch (regs[r]) {
            case GifTag::RegisterDescriptor::AD:
              handle_ad(d);
              break;
            case GifTag::RegisterDescriptor::PRIM: {
              u64 v;
              memcpy(&v, d, 8);
              handle_prim(v);
            } break;
            case GifTag::RegisterDescriptor::RGBAQ:
              // packed: r, g, b, a in the low byte of each 32-bit word
              m_rgba[0] = d[0];
              m_rgba[1] = d[4];
              m_rgba[2] = d[8];
              m_rgba[3] = d[12];
              break;
            case GifTag::RegisterDescriptor::ST: {
              float s, t, q;
              memcpy(&s, d, 4);
              memcpy(&t, d + 4, 4);
              memcpy(&q, d + 8, 4);
              m_s = s;
              m_t = t;
              m_q = q;
            } break;
            case GifTag::RegisterDescriptor::UV: {
              u32 u, v;
              memcpy(&u, d, 4);
              memcpy(&v, d + 4, 4);
              handle_uv(u & 0x3fff, v & 0x3fff);
            } break;
            case GifTag::RegisterDescriptor::XYZF2:
            case GifTag::RegisterDescriptor::XYZ2: {
              u32 x, y;
              u64 upper;
              memcpy(&x, d, 4);
              memcpy(&y, d + 4, 4);
              memcpy(&upper, d + 8, 8);
              bool adc = upper & (1ull << 47);
              u32 z = regs[r] == GifTag::RegisterDescriptor::XYZF2 ? ((upper >> 4) & 0xffffff)
                                                                   : (u32)(upper & 0xffffffff);
              handle_xyz(x & 0xffff, y & 0xffff, z, !adc);
            } break;
            case GifTag::RegisterDescriptor::TEX0_1: {
              u64 v;
              memcpy(&v, d, 8);
              set_tex0(v);
            } break;
            case GifTag::RegisterDescriptor::NOP:
              break;
            default:
              break;
          }
          offset += 16;
        }
      }
    } else if (format == GifTag::Format::REGLIST) {
      for (u32 loop = 0; loop < tag.nloop(); loop++) {
        for (u32 r = 0; r < nreg; r++) {
          if (offset + 8 > size) {
            return;
          }
          u64 v;
          memcpy(&v, data + offset, 8);
          switch (regs[r]) {
            case GifTag::RegisterDescriptor::PRIM:
              handle_prim(v);
              break;
            case GifTag::RegisterDescriptor::RGBAQ:
              handle_rgbaq(v);
              break;
            case GifTag::RegisterDescriptor::XYZF2:
              handle_xyz(v & 0xffff, (v >> 16) & 0xffff, (v >> 32) & 0xffffff, true);
              break;
            case GifTag::RegisterDescriptor::XYZ2:
              handle_xyz(v & 0xffff, (v >> 16) & 0xffff, (u32)(v >> 32), true);
              break;
            case GifTag::RegisterDescriptor::UV:
              handle_uv(v & 0x3fff, (v >> 16) & 0x3fff);
              break;
            case GifTag::RegisterDescriptor::ST: {
              float s, t;
              u32 si = v, ti = v >> 32;
              memcpy(&s, &si, 4);
              memcpy(&t, &ti, 4);
              m_s = s;
              m_t = t;
            } break;
            default:
              break;
          }
          offset += 8;
        }
      }
      if (offset & 15) {
        offset += 8;  // REGLIST data is padded to a quadword
      }
    } else if (format == GifTag::Format::IMAGE) {
      offset += tag.nloop() * 16;  // image transfers are not used by the direct buckets
    } else {
      return;
    }
  }
}

void CtrDirect::handle_ad(const u8* data) {
  u64 value;
  memcpy(&value, data, 8);
  auto addr = (GsRegisterAddress)data[8];
  switch (addr) {
    case GsRegisterAddress::PRIM:
      handle_prim(value);
      break;
    case GsRegisterAddress::RGBAQ:
      handle_rgbaq(value);
      break;
    case GsRegisterAddress::ST: {
      float s, t;
      u32 si = value, ti = value >> 32;
      memcpy(&s, &si, 4);
      memcpy(&t, &ti, 4);
      m_s = s;
      m_t = t;
    } break;
    case GsRegisterAddress::UV:
      handle_uv(value & 0x3fff, (value >> 16) & 0x3fff);
      break;
    case GsRegisterAddress::XYZF2:
      handle_xyz(value & 0xffff, (value >> 16) & 0xffff, (value >> 32) & 0xffffff, true);
      break;
    case GsRegisterAddress::XYZ2:
      handle_xyz(value & 0xffff, (value >> 16) & 0xffff, (u32)(value >> 32), true);
      break;
    case GsRegisterAddress::TEX0_1:
      set_tex0(value);
      break;
    case GsRegisterAddress::TEX1_1:
      if (value != m_tex1) {
        m_tex1 = value;
        set_state_dirty();
      }
      break;
    case GsRegisterAddress::CLAMP_1:
      if (value != m_clamp) {
        m_clamp = value;
        set_state_dirty();
      }
      break;
    case GsRegisterAddress::TEST_1:
      if (value != m_test.data) {
        m_test = GsTest(value);
        set_state_dirty();
      }
      break;
    case GsRegisterAddress::ALPHA_1:
      if (value != m_alpha.data) {
        m_alpha = GsAlpha(value);
        set_state_dirty();
      }
      break;
    case GsRegisterAddress::ZBUF_1: {
      bool mask = value & (1ull << 32);
      if (mask != m_zbuf_mask) {
        m_zbuf_mask = mask;
        set_state_dirty();
      }
    } break;
    default:
      // SCISSOR, FRAME, TEXA, TEXCLUT, FOGCOL, TEXFLUSH, MIPTBP, PABE, COLCLAMP, XYOFFSET...
      break;
  }
}

void CtrDirect::handle_prim(u64 val) {
  GsPrim p(val);
  if (p.data != m_prim.data) {
    m_prim = p;
    set_state_dirty();
  }
  m_build_idx = 0;
  m_strip_count = 0;
}

void CtrDirect::handle_rgbaq(u64 val) {
  m_rgba[0] = val & 0xff;
  m_rgba[1] = (val >> 8) & 0xff;
  m_rgba[2] = (val >> 16) & 0xff;
  m_rgba[3] = (val >> 24) & 0xff;
  u32 qi = val >> 32;
  memcpy(&m_q, &qi, 4);
}

void CtrDirect::handle_uv(u32 u, u32 v) {
  m_u = u;
  m_v = v;
}

void CtrDirect::set_tex0(u64 val) {
  if (val != m_tex0) {
    m_tex0 = val;
    set_state_dirty();
  }
}

void CtrDirect::update_draw_state() {
  ctr_draw_state st;
  memset(&st, 0, sizeof(st));
  st.tex = -1;
  m_tex_w = m_tex_h = 1;
  m_uv_scale_s = m_uv_scale_t = 1.f / 16.f;
  if (m_prim.tme()) {
    const CtrTexture* tex = m_vram->get_texture(m_tex0);
    if (tex && tex->handle >= 0) {
      GsTex0 t(m_tex0);
      st.tex = tex->handle;
      st.tcc = t.tcc();
      st.decal = t.tfx() == GsTex0::TextureFunction::DECAL;
      st.filter = (m_tex1 >> 5) & 1;  // MMAG
      st.clamp_s = (m_clamp & 3) != 0;
      st.clamp_t = ((m_clamp >> 2) & 3) != 0;
      m_tex_w = 1 << t.tw();
      m_tex_h = 1 << t.th();
      m_uv_scale_s = 1.f / (16.f * m_tex_w);
      m_uv_scale_t = 1.f / (16.f * m_tex_h);
    }
  }
  st.blend = m_prim.abe() ? map_blend(m_alpha) : CTR_BLEND_OFF;
  st.fix = m_alpha.fix();
  if (m_test.alpha_test_enable()) {
    st.atest = map_atest(m_test.alpha_test());
    st.aref = m_test.aref();
    if (m_test.afail() != GsTest::AlphaFail::KEEP && st.atest == CTR_TEST_NEVER) {
      // "never pass, but still write rgb/z": used to write only depth or only color. Draw.
      st.atest = CTR_TEST_ALWAYS;
    }
  } else {
    st.atest = CTR_TEST_ALWAYS;
  }
  if (m_allow_depth && m_test.zte()) {
    st.ztest = map_ztest(m_test.ztest());
    st.zwrite = !m_zbuf_mask;
  } else {
    st.ztest = CTR_TEST_ALWAYS;
    st.zwrite = 0;
  }

  if (!same_state(st, m_draw_state)) {
    flush();
    m_draw_state = st;
  }
  m_state_dirty = false;
}

void CtrDirect::batch_mode(bool quads) {
  if (m_quads != quads) {
    flush();
    m_quads = quads;
  }
}

void CtrDirect::push_vertex(int idx) {
  const auto& b = m_build[idx];
  ctr_vertex v;
  v.x = b.x;
  v.y = b.y;
  v.z = b.z;
  v.s = b.s;
  v.t = b.t;
  v.r = b.rgba[0];
  v.g = b.rgba[1];
  v.b = b.rgba[2];
  v.a = b.rgba[3];
  m_verts.push_back(v);
}

void CtrDirect::handle_xyz(u32 x, u32 y, u32 z, bool advance) {
  if (m_state_dirty) {
    update_draw_state();
  }
  auto& b = m_build[m_build_idx];
  // GS coordinates are 12.4 fixed point, centered at 2048. Jak 1 draws a 512 x 224 field.
  float px = x / 16.f;
  float py = y / 16.f;
  b.x = (px - 2048.f) / 256.f;
  b.y = (2048.f - py) * (1.f / 112.f);
  b.z = z * (1.f / 16777215.f);
  if (m_prim.fst()) {
    b.s = m_u * m_uv_scale_s;
    b.t = m_v * m_uv_scale_t;
  } else {
    float q = m_q == 0.f ? 1.f : m_q;
    const float inv_q = 1.f / q;
    b.s = m_s * inv_q;
    b.t = m_t * inv_q;
  }
  memcpy(b.rgba, m_rgba, 4);
  m_build_idx++;

  switch (m_prim.kind()) {
    case GsPrim::Kind::TRI:
      if (m_build_idx == 3) {
        if (advance) {
          batch_mode(false);
          push_vertex(0);
          push_vertex(1);
          push_vertex(2);
          m_stats.triangles++;
        }
        m_build_idx = 0;
      }
      break;
    case GsPrim::Kind::TRI_STRIP:
      if (m_build_idx == 3) {
        m_build_idx = 0;
      }
      m_strip_count++;
      if (m_strip_count >= 3 && advance) {
        batch_mode(false);
        for (int i = 0; i < 3; i++) {
          push_vertex(i);
        }
        m_stats.triangles++;
      }
      break;
    case GsPrim::Kind::TRI_FAN:
      if (m_build_idx == 3) {
        if (advance) {
          batch_mode(false);
          push_vertex(0);
          push_vertex(1);
          push_vertex(2);
          m_stats.triangles++;
        }
        // the next vertex replaces the second one, the first one stays
        m_build[1] = m_build[2];
        m_build_idx = 2;
      }
      break;
    case GsPrim::Kind::SPRITE:
      if (m_build_idx == 2) {
        // two corners: build the other two, flat colored with the second vertex's color
        BuildVert c1 = m_build[0], c2 = m_build[1];
        BuildVert c3 = c1, c4 = c2;
        c3.x = c1.x;
        c3.y = c2.y;
        c3.s = c1.s;
        c3.t = c2.t;
        c4.x = c2.x;
        c4.y = c1.y;
        c4.s = c2.s;
        c4.t = c1.t;
        c1.z = c3.z = c4.z = c2.z;
        memcpy(c1.rgba, c2.rgba, 4);
        memcpy(c3.rgba, c2.rgba, 4);
        memcpy(c4.rgba, c2.rgba, 4);
        // a quad: c1, c4, c2, c3 around (triangles c1 c4 c3, c3 c4 c2)
        batch_mode(true);
        m_build[0] = c1;
        m_build[1] = c4;
        m_build[2] = c2;
        push_vertex(0);
        push_vertex(1);
        push_vertex(2);
        m_build[0] = c3;
        push_vertex(0);
        m_stats.triangles += 2;
        m_build_idx = 0;
      }
      break;
    default:
      // points and lines: not drawn yet (debug lines)
      m_stats.skipped_prims++;
      m_build_idx = 0;
      break;
  }

  if ((int)m_verts.size() >= kMaxBatchVerts) {
    flush();
  }
}

void CtrDirect::flush() {
  if (m_verts.empty()) {
    return;
  }
  static int dbg = 0;
  static const bool debug = getenv("CTR_DRAW_DEBUG") != nullptr;
  if (debug && (dbg++ % 97) == 0) {
    const auto& st = m_draw_state;
    lg::info("[ctr draw] n {} tex {} tcc {} decal {} blend {} atest {} aref {} ztest {} tex0 {:x} "
             "prim {:x} test {:x} alpha {:x}",
             m_verts.size(), st.tex, st.tcc, st.decal, st.blend, st.atest, st.aref, st.ztest,
             m_tex0, m_prim.data, m_test.data, m_alpha.data);
    for (size_t i = 0; i < std::min<size_t>(3, m_verts.size()); i++) {
      const auto& v = m_verts[i];
      lg::info("   v {} {} {} st {} {} rgba {} {} {} {}", v.x, v.y, v.z, v.s, v.t, v.r, v.g, v.b,
               v.a);
    }
  }
  if (m_quads) {
    ctr_gpu_draw_quads(&m_draw_state, m_verts.data(), (int)m_verts.size() / 4);
  } else {
    ctr_gpu_draw(&m_draw_state, m_verts.data(), (int)m_verts.size());
  }
  m_stats.flushes++;
  m_verts.clear();
}
