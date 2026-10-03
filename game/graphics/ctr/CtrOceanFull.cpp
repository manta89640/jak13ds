/*!
 * @file CtrOceanFull.cpp
 * (AI-assisted)
 * See CtrOceanFull.h.
 */

#include "CtrOceanFull.h"

#include <algorithm>
#include <atomic>
#include <cstring>

#include "common/dma/dma.h"
#include "common/dma/gs.h"
#include "common/log/log.h"

#include "game/graphics/ctr/CtrOceanVu.h"
#include "game/graphics/ctr/CtrVram.h"
#include "game/graphics/ctr/ctr_gpu.h"

namespace {

std::atomic<u32> g_levels_gen{1};
std::string g_levels;  // written while the render thread is idle (ctr_set_levels)

constexpr int kTexSize = 64;   // the wave texture (the PS2's is 128: 32 x 32 quads of 4 x 4)
constexpr int kGrid = 32;      // wave vertices per side (the 33rd row / column repeats the first)
constexpr int kChunkVerts = 4096;  // vertices per reservation (one draw)

//! GS fog value F -> fog amount (255 - F) / 255 (CommonOceanRenderer: fog = 255 - F)
struct FogTable {
  float amount[256];
  FogTable() {
    for (int i = 0; i < 256; i++) {
      amount[i] = (255 - i) / 255.f;
    }
  }
};
const FogTable kFog;

bool is_vu_end_tag(const DmaTag& tag, const VifCode& v0, const VifCode& v1) {
  return tag.qwc == 2 && tag.kind == DmaTag::Kind::CNT && v0.kind == VifCode::Kind::NOP &&
         v1.kind == VifCode::Kind::DIRECT;
}

void drain(DmaFollower& dma, const CtrRenderState& rs) {
  while (dma.current_tag_offset() != rs.next_bucket) {
    dma.read_and_advance();
  }
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// GS packets -> ctr_gpu_ocean_draw
// ---------------------------------------------------------------------------------------------

/*!
 * The GS packets the ocean's VU programs (and the far ocean) send, as one indexed triangle list
 * written into the frame's vertex memory (CommonOceanRenderer, without its OpenGL buffers). Only
 * the textured pass is kept: the near ocean's alpha and env map passes and the mid ocean's env map
 * pass are skipped while reading (their vertices aren't converted).
 */
class CtrOceanBatch {
 public:
  void begin(const ctr_draw_state& st, const u8 fog[3]) {
    m_state = st;
    memcpy(m_fog, fog, 3);
    m_failed = false;
  }
  void end() { flush(); }

  //! xgkick of the ocean-near program (CommonOceanRenderer::kick_from_near)
  void kick_near(const u8* data, u32 bytes) {
    u32 offset = 0;
    for (int tags = 0; tags < 64 && offset + 16 <= bytes; tags++) {
      GifTag tag(data + offset);
      offset += 16;
      const u32 nloop = tag.nloop();
      if (tag.nreg() == 3) {
        if (offset + 48 * nloop > bytes) {
          return;
        }
        if (m_near_bucket == kTextured) {
          prim(data + offset, nloop, GsPrim(tag.prim()).kind() != GsPrim::Kind::TRI_STRIP);
        }
        offset += 48 * nloop;
      } else if (tag.nreg() == 1) {
        if (offset + 16 * nloop > bytes) {
          return;
        }
        near_adgif(data + offset, nloop);
        offset += 16 * nloop;
      } else {
        return;
      }
      if (tag.eop()) {
        return;
      }
    }
  }

  //! xgkick of the ocean-mid program (CommonOceanRenderer::kick_from_mid): drawn without
  //! blending = the textured pass; with = the env map pass
  void kick_mid(const u8* data, u32 bytes) {
    u32 offset = 0;
    for (int tags = 0; tags < 64 && offset + 16 <= bytes; tags++) {
      GifTag tag(data + offset);
      offset += 16;
      if (tag.flg() != GifTag::Format::PACKED) {
        return;
      }
      const u32 nloop = tag.nloop();
      if (tag.nreg() == 1) {
        offset += 16 * nloop;  // the adgif: the env map texture (not drawn)
      } else if (tag.nreg() == 3) {
        if (offset + 48 * nloop > bytes) {
          return;
        }
        const GsPrim prim_reg(tag.prim());
        if (!prim_reg.abe()) {
          prim(data + offset, nloop, prim_reg.kind() != GsPrim::Kind::TRI_STRIP);
        }
        offset += 48 * nloop;
      } else {
        return;
      }
      if (tag.eop()) {
        return;
      }
    }
  }

  //! the far ocean: GIF packets of draw-large-polygon-ocean (fans of ST, RGBAQ, XYZF2) after the
  //! A+D register setup of ocean-init-buffer
  void far_gif(const u8* data, u32 bytes) {
    u32 offset = 0;
    while (offset + 16 <= bytes) {
      GifTag tag(data + offset);
      offset += 16;
      const u32 nloop = tag.nloop();
      const u32 nreg = tag.nreg() ? tag.nreg() : 16;
      if (tag.flg() != GifTag::Format::PACKED) {
        return;  // (not in this bucket)
      }
      if (nreg == 3 && tag.pre()) {
        if (offset + 48 * nloop > bytes) {
          return;
        }
        prim(data + offset, nloop, GsPrim(tag.prim()).kind() != GsPrim::Kind::TRI_STRIP);
      }
      offset += 16 * nreg * nloop;
    }
  }

  // statistics
  int vertices = 0, triangles = 0, draws = 0, dropped = 0;

 private:
  enum : u32 { kTextured = 0, kAlpha = 1, kEnvMap = 2 };

  //! CommonOceanRenderer::handle_near_adgif: which of the 3 passes the next packets are
  void near_adgif(const u8* data, u32 count) {
    for (u32 i = 0; i < count; i++) {
      u64 value;
      memcpy(&value, data + 16 * i, 8);
      const auto addr = (GsRegisterAddress)data[16 * i + 8];
      switch (addr) {
        case GsRegisterAddress::CLAMP_1:
          if (value & 0b001) {
            m_near_bucket = kEnvMap;
          }
          break;
        case GsRegisterAddress::TEX0_1:
          if (!GsTex0(value).tcc()) {
            m_near_bucket = kTextured;
          }
          break;
        case GsRegisterAddress::ALPHA_1: {
          GsAlpha reg(value);
          if (reg.a_mode() == GsAlpha::BlendMode::SOURCE &&
              reg.b_mode() == GsAlpha::BlendMode::ZERO_OR_FIXED &&
              reg.c_mode() == GsAlpha::BlendMode::DEST &&
              reg.d_mode() == GsAlpha::BlendMode::DEST) {
            m_near_bucket = kEnvMap;
          }
        } break;
        case GsRegisterAddress::FRAME_1:
          if (value >> 32) {
            m_near_bucket = kAlpha;
          }
          break;
        default:
          break;
      }
    }
  }

  /*!
   * nloop vertices of ST, RGBAQ, XYZF2 (packed): a strip or a fan. Like the GS, a vertex with the
   * ADC bit set joins the primitive without drawing a triangle.
   */
  void prim(const u8* data, u32 nloop, bool fan) {
    if (m_failed || nloop < 3) {
      return;
    }
    if (!room((int)nloop)) {
      return;
    }
    const int first = m_nv;
    ctr_ocean_vertex* v = m_v + m_nv;
    u16* idx = m_i + m_ni;
    for (u32 i = 0; i < nloop; i++, v++) {
      const u8* p = data + 48 * i;
      u32 w[12];
      memcpy(w, p, 48);
      // ST (with Q): floats; Q > 0 for the GPU's 1 / Q (a far vertex near the horizon has a tiny
      // Q): as integers, so no float compare (positive floats order like their bits)
      memcpy(&v->s, &w[0], 8);
      const u32 q_bits = (s32)w[2] < 0x2B8CBCCC ? 0x2B8CBCCCu : w[2];  // 1e-12
      memcpy(&v->q, &q_bits, 4);
      v->r = (u8)w[4];
      v->g = (u8)w[5];
      v->b = (u8)w[6];
      v->a = (u8)w[7];
      // XYZF2: x, y 12.4 in the low halves of words 8 and 9, z bits 4..27 of word 10, F bits
      // 4..11 of word 11, ADC bit 15 of word 11
      v->x = (float)(w[8] & 0xffff);
      v->y = (float)(w[9] & 0xffff);
      v->z = (float)((w[10] >> 4) & 0xffffff);
      v->fog = kFog.amount[(w[11] >> 4) & 0xff];
      if (i >= 2 && !(w[11] & 0x8000)) {
        const int k = first + (int)i;
        idx[0] = (u16)(fan ? first : k - 2);
        idx[1] = (u16)(k - 1);
        idx[2] = (u16)k;
        idx += 3;
      }
    }
    m_nv += (int)nloop;
    m_ni = (int)(idx - m_i);
    vertices += (int)nloop;
  }

  bool room(int n) {
    if (m_open && m_nv + n <= m_max_v && m_ni + 3 * n <= m_max_i) {
      return true;
    }
    flush();
    // the next chunk (smaller when the frame's vertex memory is short)
    for (int size = std::max(kChunkVerts, n); size >= n; size /= 4) {
      if (ctr_gpu_ocean_reserve(size, 3 * size, &m_v, &m_i)) {
        m_open = true;
        m_max_v = size;
        m_max_i = 3 * size;
        m_nv = m_ni = 0;
        return true;
      }
    }
    m_failed = true;  // no room this frame: the rest of the bucket isn't drawn
    dropped++;
    return false;
  }

  void flush() {
    if (!m_open) {
      return;
    }
    m_open = false;
    ctr_gpu_ocean_draw(&m_state, m_nv, m_ni, m_fog[0], m_fog[1], m_fog[2]);
    if (m_ni >= 3) {
      draws++;
      triangles += m_ni / 3;
    }
    m_nv = m_ni = 0;
  }

  ctr_draw_state m_state;
  u8 m_fog[3] = {0, 0, 0};
  bool m_open = false, m_failed = false;
  ctr_ocean_vertex* m_v = nullptr;
  u16* m_i = nullptr;
  int m_nv = 0, m_ni = 0, m_max_v = 0, m_max_i = 0;
  u32 m_near_bucket = kTextured;  // CommonOceanRenderer::m_current_bucket (kept between kicks)
};

// ---------------------------------------------------------------------------------------------
// the VU programs' DMA (OceanMid::run, OceanNear::render_jak1), without the asserts
// ---------------------------------------------------------------------------------------------

CtrOceanMidVu::CtrOceanMidVu() {
  for (auto& x : m_vu_data) {
    x.fill(999.);
  }
  vu.vf25 = Vf(1, 1, 1, 1);
}

void CtrOceanMidVu::run_call0() {
  run_call0_vu2c();
}

void CtrOceanMidVu::xgkick(u16 addr) {
  if (addr < 1024) {
    m_batch->kick_mid((const u8*)&m_vu_data[addr], (1024 - addr) * 16);
  }
}

bool CtrOceanMidVu::run(DmaFollower& dma, CtrOceanBatch* batch) {
  m_batch = batch;
  {
    auto base_offset = dma.read_and_advance();
    if (base_offset.size_bytes != 0 || base_offset.vifcode0().kind != VifCode::Kind::BASE ||
        base_offset.vifcode1().kind != VifCode::Kind::OFFSET ||
        base_offset.vifcode0().immediate != VU1_INPUT_BUFFER_BASE ||
        base_offset.vifcode1().immediate != VU1_INPUT_BUFFER_OFFSET) {
      return false;
    }
  }
  {
    auto constants = dma.read_and_advance();
    if (constants.size_bytes != sizeof(Constants) ||
        VifCodeUnpack(constants.vifcode1()).addr_qw != Vu1Data::CONSTANTS) {
      return false;
    }
    memcpy(&m_constants, constants.data, sizeof(Constants));
    memcpy(m_vu_data + Vu1Data::CONSTANTS, &m_constants, sizeof(Constants));
  }
  {
    auto call0 = dma.read_and_advance();
    if (call0.vifcode1().kind != VifCode::Kind::MSCALF || call0.vifcode1().immediate != 0) {
      return false;
    }
    run_call0();
  }
  for (int guard = 0; guard < 100000; guard++) {
    if (is_vu_end_tag(dma.current_tag(), dma.current_tag_vif0(), dma.current_tag_vif1())) {
      return true;
    }
    auto data = dma.read_and_advance();
    auto v0 = data.vifcode0();
    auto v1 = data.vifcode1();
    if (v0.kind == VifCode::Kind::STCYCL && v0.immediate == 0x404 &&
        v1.kind == VifCode::Kind::UNPACK_V4_32) {
      auto up = VifCodeUnpack(v1);
      const u32 addr = up.addr_qw + (up.use_tops_flag ? get_upload_buffer() : 0);
      if (addr + v1.num > 1024 || 16 * v1.num > data.size_bytes) {
        return false;
      }
      memcpy(m_vu_data + addr, data.data, 16 * v1.num);
    } else if (v0.kind == VifCode::Kind::STCYCL && v0.immediate == 0x404 &&
               v1.kind == VifCode::Kind::UNPACK_V4_8) {
      auto up = VifCodeUnpack(v1);
      const u32 addr = up.addr_qw + get_upload_buffer();
      if (addr + v1.num > 1024 || 4 * v1.num > data.size_bytes) {
        return false;
      }
      for (u32 i = 0; i < v1.num; i++) {
        const u32 temp[4] = {data.data[4 * i], data.data[4 * i + 1], data.data[4 * i + 2],
                             data.data[4 * i + 3]};
        memcpy(m_vu_data + addr + i, temp, 16);
      }
    } else if (v0.kind == VifCode::Kind::STCYCL && v0.immediate == 0x204 &&
               v1.kind == VifCode::Kind::UNPACK_V4_8) {
      auto up = VifCodeUnpack(v1);
      const u32 addr = up.addr_qw + get_upload_buffer();
      // cl = 4, wl = 2: two of every four quadwords written
      if (addr + 4 * ((v1.num + 1) / 2) > 1024 || 4 * v1.num > data.size_bytes) {
        return false;
      }
      for (u32 i = 0; i < v1.num; i++) {
        const u32 temp[4] = {data.data[4 * i], data.data[4 * i + 1], data.data[4 * i + 2],
                             data.data[4 * i + 3]};
        memcpy(m_vu_data + addr + 4 * (i / 2) + i % 2, temp, 16);
      }
    } else if (v0.kind == VifCode::Kind::STCYCL && v0.immediate == 0x404 &&
               v1.kind == VifCode::Kind::MSCALF) {
      switch (v1.immediate) {
        case 46:
          run_call46_vu2c();
          break;
        case 73:
          run_call73_vu2c();
          break;
        case 107:
          run_call107_vu2c();
          break;
        case 275:
          run_call275_vu2c();
          break;
        default:
          return false;
      }
    } else if (v0.kind == VifCode::Kind::MSCALF && v1.kind == VifCode::Kind::FLUSHA) {
      switch (v0.immediate) {
        case 41:
          run_call41_vu2c();
          break;
        case 43:
          run_call43_vu2c();
          break;
        default:
          return false;
      }
    } else {
      return false;
    }
  }
  return false;
}

CtrOceanNearVu::CtrOceanNearVu() {
  for (auto& a : m_vu_data) {
    a.fill(0);
  }
}

void CtrOceanNearVu::xgkick(u16 addr) {
  if (addr < 1024) {
    m_batch->kick_near((const u8*)&m_vu_data[addr], (1024 - addr) * 16);
  }
}

bool CtrOceanNearVu::run(DmaFollower& dma, CtrOceanBatch* batch) {
  m_batch = batch;
  if (dma.current_tag().qwc != 2) {
    return false;  // (OceanNear: "abort OceanNear::render!")
  }
  {
    auto setup = dma.read_and_advance();  // the GS TEST register
    if (setup.vifcode1().kind != VifCode::Kind::DIRECT || setup.size_bytes != 32) {
      return false;
    }
  }
  {
    auto ob = dma.read_and_advance();
    if (ob.size_bytes != 0 || ob.vifcode0().kind != VifCode::Kind::BASE ||
        ob.vifcode1().kind != VifCode::Kind::OFFSET ||
        ob.vifcode0().immediate != VU1_INPUT_BUFFER_BASE ||
        ob.vifcode1().immediate != VU1_INPUT_BUFFER_OFFSET) {
      return false;
    }
  }
  for (int guard = 0; guard < 100000; guard++) {
    if (is_vu_end_tag(dma.current_tag(), dma.current_tag_vif0(), dma.current_tag_vif1())) {
      return true;
    }
    auto data = dma.read_and_advance();
    auto v0 = data.vifcode0();
    auto v1 = data.vifcode1();
    if (v0.kind == VifCode::Kind::STCYCL && v1.kind == VifCode::Kind::UNPACK_V4_32) {
      auto up = VifCodeUnpack(v1);
      const u32 addr = up.addr_qw + (up.use_tops_flag ? get_upload_buffer() : 0);
      if (v0.immediate != 0x404 || addr + v1.num > 1024 || 16 * v1.num > data.size_bytes) {
        return false;
      }
      memcpy(m_vu_data + addr, data.data, 16 * v1.num);
    } else if (v0.kind == VifCode::Kind::MSCALF && v1.kind == VifCode::Kind::STMOD) {
      switch (v0.immediate) {
        case 0:
          run_call0_vu2c();
          break;
        case 39:
          run_call39_vu2c();
          break;
        default:
          return false;
      }
    }
  }
  return false;
}

// ---------------------------------------------------------------------------------------------
// the wave texture (OceanTexture: the ocean-texture VU program, run_L3_PC)
// ---------------------------------------------------------------------------------------------

struct CtrOceanFull::TextureDma {
  // (deftype ocean-texture-constants), see OceanTexture.h
  struct Constants {
    u8 giftag[16];
    u32 buffers[4];
    u32 dests[4];
    float start[4];
    float offsets[4];
    float constants[4];
    float cam_nrm[4];
  } constants;
  static_assert(sizeof(Constants) == 112);
  u64 env_tex0 = 0;
  // the 33 rows of *ocean-verts* the program reads (32 + the first again), 64 quadwords each:
  // per 4 vertices: color 0, normal x of the 4, color 1, normal y, color 2, normal z, color 3, -
  const u8* rows[kGrid + 1];
};

/*!
 * The ocean-texture DMA (OceanTexture::handle_ocean_texture_jak1): constants, the env map adgif,
 * the vertex uploads and the program calls. Both buckets start with it.
 */
bool CtrOceanFull::read_texture_dma(DmaFollower& dma, TextureDma* out) {
  dma.read_and_advance();  // set-display-gs-state
  {
    auto data = dma.read_and_advance();
    if (data.size_bytes != 0 || data.vifcode0().kind != VifCode::Kind::BASE ||
        data.vifcode1().kind != VifCode::Kind::OFFSET) {
      return false;
    }
  }
  {
    auto data = dma.read_and_advance();
    if (data.size_bytes != sizeof(TextureDma::Constants) ||
        data.vifcode1().kind != VifCode::Kind::UNPACK_V4_32) {
      return false;
    }
    memcpy(&out->constants, data.data, sizeof(TextureDma::Constants));
  }
  {
    auto data = dma.read_and_advance();
    if (data.size_bytes != sizeof(AdGifData) + 16 || data.vifcode1().kind != VifCode::Kind::DIRECT) {
      return false;
    }
    AdGifData ad;
    memcpy(&ad, data.data + 16, sizeof(AdGifData));
    out->env_tex0 = ad.tex0_data;
  }
  // uploads (192, then 9 x 192 after the calls, then 128 + 64 quadwords) and the 12 calls
  int rows = 0;
  for (int guard = 0; guard < 32; guard++) {
    auto data = dma.read_and_advance();
    const auto v0 = data.vifcode0(), v1 = data.vifcode1();
    if (v1.kind == VifCode::Kind::UNPACK_V4_32) {
      if (data.size_bytes % (64 * 16) != 0 || rows + (int)(data.size_bytes / (64 * 16)) > kGrid + 1) {
        return false;
      }
      for (u32 r = 0; r < data.size_bytes / (64 * 16); r++) {
        out->rows[rows++] = data.data + r * 64 * 16;
      }
    } else if (v0.kind == VifCode::Kind::MSCALF) {
      if (v0.immediate == 4) {  // the last call (TexVu1Prog::DONE)
        return rows == kGrid + 1;
      }
    } else {
      return false;
    }
  }
  return false;
}

/*!
 * The env map (environment-ocean-alphamod) as RGBA8, decoded from CtrVram again when its texture
 * page changes (and every 300 frames).
 */
bool CtrOceanFull::update_env(u64 tex0, CtrRenderState& rs) {
  if (!rs.vram) {
    return false;
  }
  GsTex0 t(tex0);
  u64 key = tex0;
  key = (key ^ (u64)(uintptr_t)rs.vram->upload_source(t.tbp0())) * 1099511628211ull;
  key = (key ^ (u64)(uintptr_t)rs.vram->upload_source(t.cbp())) * 1099511628211ull;
  if (key == m_env_key && rs.frame_idx - m_env_frame < 300) {
    return m_env_ok;
  }
  m_env_key = key;
  m_env_frame = rs.frame_idx;
  int w = 0, h = 0;
  m_env_ok = rs.vram->decode_for_cpu(tex0, &m_env, &w, &h) && w > 0 && h > 0 &&
             m_env.size() >= (size_t)w * h;
  m_env_w = w;
  m_env_h = h;
  if (std::find(m_env_logged.begin(), m_env_logged.end(), tex0) == m_env_logged.end() &&
      m_env_logged.size() < 16) {
    m_env_logged.push_back(tex0);
    lg::info("[ctr] ocean full: env map tex0 {:x} {}x{} {}", tex0, w, h,
             m_env_ok ? "ok" : "NOT DECODED (plain colors)");
  }
  return m_env_ok;
}

/*!
 * The wave texture: run_L3_PC's math for the 33 x 33 vertices, then 64 x 64 texels between them
 * (texel centers at 1/4 and 3/4 of the PS2's 4 x 4 texel quads: weights 3:1), each the env map
 * texel at the interpolated lookup times the interpolated color (GS modulate). Integers after the
 * vertices: no float compares or divides in the texel loop.
 */
void CtrOceanFull::update_texture(const TextureDma& t, CtrRenderState& rs) {
  const bool env_ok = update_env(t.env_tex0, rs);
  const int ew = env_ok ? m_env_w : 1, eh = env_ok ? m_env_h : 1;
  const u32 gray = 0xff808080;  // no env map: the vertex colors alone
  const u32* env = env_ok ? m_env.data() : &gray;

  // per vertex: env map texel coordinates with 8 fraction bits, color (GS: the low byte)
  struct V {
    s32 s, t, r, g, b;
  };
  static V verts[kGrid + 1][kGrid + 1];
  const float cx = t.constants.cam_nrm[0], cy = t.constants.cam_nrm[1],
              cz = t.constants.cam_nrm[2];
  const float k0 = t.constants.constants[0], k1 = t.constants.constants[1];
  const float sscale = 256.f * ew, tscale = 256.f * eh;
  constexpr s32 kLim = 1 << 22;  // keeps the weighted sums below in range
  for (int r = 0; r <= kGrid; r++) {
    for (int g = 0; g < kGrid / 4; g++) {
      float q[8][4];
      memcpy(q, t.rows[r] + g * 8 * 16, sizeof(q));
      const float* nx = q[1];
      const float* ny = q[3];
      const float* nz = q[5];
      // four independent vertices (the VU's 4 lanes): the VFP pipelines them
      float refl[4], s[4], tt[4];
      for (int k = 0; k < 4; k++) {
        refl[k] = (nx[k] * cx + ny[k] * cy) + nz[k] * cz;
      }
      for (int k = 0; k < 4; k++) {
        const float w = q[2 * k][3];
        s[k] = (nx[k] * refl[k] * w + cx) * k0 + k1;
        tt[k] = (ny[k] * refl[k] * w + cy) * k0 + k1;
      }
      for (int k = 0; k < 4; k++) {
        V& v = verts[r][4 * g + k];
        v.s = std::clamp((s32)(s[k] * sscale), -kLim, kLim);
        v.t = std::clamp((s32)(tt[k] * tscale), -kLim, kLim);
        v.r = (s32)q[2 * k][0] & 0xff;  // ftoi0, then the GS's 8 bits
        v.g = (s32)q[2 * k][1] & 0xff;
        v.b = (s32)q[2 * k][2] & 0xff;
      }
    }
    verts[r][kGrid] = verts[r][0];
  }

  // across: 64 samples per vertex row (3a + b, a + 3b: 4x)
  static V across[kGrid + 1][kTexSize];
  for (int r = 0; r <= kGrid; r++) {
    for (int c = 0; c < kGrid; c++) {
      const V& a = verts[r][c];
      const V& b = verts[r][c + 1];
      V& o0 = across[r][2 * c];
      V& o1 = across[r][2 * c + 1];
      o0.s = 3 * a.s + b.s;
      o1.s = a.s + 3 * b.s;
      o0.t = 3 * a.t + b.t;
      o1.t = a.t + 3 * b.t;
      o0.r = 3 * a.r + b.r;
      o1.r = a.r + 3 * b.r;
      o0.g = 3 * a.g + b.g;
      o1.g = a.g + 3 * b.g;
      o0.b = 3 * a.b + b.b;
      o1.b = a.b + 3 * b.b;
    }
  }

  // down, the env map lookup and the GS modulate ((tex * color) >> 7, at most 255)
  m_texels.resize(kTexSize * kTexSize);
  u32* out = m_texels.data();
  const u32 umax_s = (u32)ew - 1, umax_t = (u32)eh - 1;
  for (int y = 0; y < kTexSize; y++) {
    const V* a = across[y >> 1];
    const V* b = across[(y >> 1) + 1];
    const s32 wa = (y & 1) ? 1 : 3, wb = 4 - wa;
    for (int x = 0; x < kTexSize; x++) {
      // 16x the value; s, t with 8 fraction bits
      s32 tx = (wa * a[x].s + wb * b[x].s) >> 12;
      s32 ty = (wa * a[x].t + wb * b[x].t) >> 12;
      tx = (u32)tx > umax_s ? (tx < 0 ? 0 : (s32)umax_s) : tx;  // clamp to the edge
      ty = (u32)ty > umax_t ? (ty < 0 ? 0 : (s32)umax_t) : ty;
      const u32 e = env[ty * ew + tx];
      const u32 cr = (u32)(wa * a[x].r + wb * b[x].r) >> 4;
      const u32 cg = (u32)(wa * a[x].g + wb * b[x].g) >> 4;
      const u32 cb = (u32)(wa * a[x].b + wb * b[x].b) >> 4;
      const u32 r = std::min<u32>(((e & 0xff) * cr) >> 7, 255);
      const u32 g = std::min<u32>((((e >> 8) & 0xff) * cg) >> 7, 255);
      const u32 bl = std::min<u32>((((e >> 16) & 0xff) * cb) >> 7, 255);
      // alpha 0xff: the PS2's texture alpha only weights the env map pass (left out)
      *out++ = r | (g << 8) | (bl << 16) | 0xff000000u;
    }
  }
  const u8* bytes = (const u8*)m_texels.data();  // RGBA in memory (little endian)
  if (m_tex < 0) {
    m_tex = ctr_gpu_tex_create_mipmapped(kTexSize, kTexSize, bytes);
  } else {
    ctr_gpu_tex_update(m_tex, bytes);  // (a second copy with the pipeline)
  }
}

// ---------------------------------------------------------------------------------------------
// the buckets
// ---------------------------------------------------------------------------------------------

CtrOceanFull::CtrOceanFull()
    : m_mid(std::make_unique<CtrOceanMidVu>()),
      m_near(std::make_unique<CtrOceanNearVu>()),
      m_batch(std::make_unique<CtrOceanBatch>()) {}

CtrOceanFull::~CtrOceanFull() {
  if (m_tex >= 0) {
    ctr_gpu_tex_delete(m_tex);
  }
}

void CtrOceanFull::note_levels(const std::vector<std::string>& levels) {
  std::string s;
  for (const auto& l : levels) {
    s += (s.empty() ? "" : " ") + l;
  }
  if (s != g_levels) {
    g_levels = s;
    g_levels_gen++;
  }
}

/*!
 * Per frame: the frame's ms so far into the window's sums; once per level (set of levels), after
 * 150 frames with the ocean, one log line with the averages.
 */
void CtrOceanFull::frame_stats(const CtrRenderState& rs) {
  if (rs.frame_idx == m_stat_frame) {
    return;
  }
  m_max_frame_ms = std::max(m_max_frame_ms, m_cur_frame_ms);
  m_cur_frame_ms = 0;
  m_stat_frame = rs.frame_idx;
  const u32 gen = g_levels_gen.load();
  if (gen != m_window_gen) {
    m_window_gen = gen;
    m_frames = m_mid_runs = m_near_runs = m_failed = 0;
    m_tex_ms = m_mid_ms = m_near_ms = m_max_frame_ms = 0;
    m_batch->vertices = m_batch->triangles = m_batch->draws = m_batch->dropped = 0;
  }
  m_frames++;
  if (m_frames == 150 && m_logged_gen != gen) {
    m_logged_gen = gen;
    const double n = m_frames - 1;  // (this frame hasn't run yet)
    lg::info(
        "[ctr] ocean full ({}): render thread {:.2f} ms/frame (texture {:.2f}, far+mid {:.2f}, "
        "near {:.2f}; max {:.2f}), {:.0f} vertices, {:.0f} triangles, {:.1f} draws per frame; "
        "mid in {:.0f}%, near in {:.0f}% of frames{}{}",
        g_levels, (m_tex_ms + m_mid_ms + m_near_ms) / n, m_tex_ms / n, m_mid_ms / n,
        m_near_ms / n, m_max_frame_ms, m_batch->vertices / n, m_batch->triangles / n,
        m_batch->draws / n, 100.0 * m_mid_runs / n, 100.0 * m_near_runs / n,
        m_failed ? fmt::format(", {} unexpected DMA", m_failed) : "",
        m_batch->dropped ? fmt::format(", {} dropped (no vertex memory)", m_batch->dropped) : "");
  }
}

void CtrOceanFull::render_mid_and_far(DmaFollower& dma, CtrRenderState& rs) {
  frame_stats(rs);
  const double t0 = ctr_gpu_time_ms();
  // OceanMidAndFar::render_jak1: the bucket's first transfer, then a CALL if the game drew none
  dma.read_and_advance();
  if (dma.current_tag().kind == DmaTag::Kind::CALL) {
    drain(dma, rs);
    return;
  }
  TextureDma tex;
  if (!read_texture_dma(dma, &tex)) {
    m_failed++;
    drain(dma, rs);
    return;
  }
  update_texture(tex, rs);
  m_tex_frame = rs.frame_idx;
  const double t1 = ctr_gpu_time_ms();
  m_tex_ms += t1 - t0;

  // far and mid: no blending, depth test off, depth written (CommonOceanRenderer::flush_mid,
  // the far ocean's GS TEST)
  ctr_draw_state st;
  memset(&st, 0, sizeof(st));
  st.tex = m_tex;
  st.tcc = 1;
  st.filter = 1;
  st.blend = CTR_BLEND_OFF;
  st.atest = CTR_TEST_ALWAYS;
  st.ztest = CTR_TEST_ALWAYS;
  st.zwrite = 1;
  m_batch->begin(st, rs.fog_color);
  read_far(dma);
  bool ok = true;
  if (dma.current_tag_vifcode0().kind == VifCode::Kind::BASE) {
    ok = m_mid->run(dma, m_batch.get());
    m_mid_runs++;
  }
  m_batch->end();
  if (!ok) {
    m_failed++;
  }
  drain(dma, rs);
  const double dt = ctr_gpu_time_ms() - t1;
  m_mid_ms += dt;
  m_cur_frame_ms += ctr_gpu_time_ms() - t0;
}

/*!
 * The far ocean (OceanMidAndFar::handle_ocean_far): the GS register setup (ocean-init-buffer),
 * then direct GIF transfers.
 */
void CtrOceanFull::read_far(DmaFollower& dma) {
  dma.read_and_advance();  // ocean-init-buffer's A+D registers (the state is fixed above)
  while (dma.current_tag().kind == DmaTag::Kind::CNT &&
         dma.current_tag_vifcode0().kind == VifCode::Kind::NOP) {
    auto data = dma.read_and_advance();
    if (data.vifcode1().kind == VifCode::Kind::DIRECT) {
      m_batch->far_gif(data.data, data.size_bytes);
    }
  }
}

void CtrOceanFull::render_near(DmaFollower& dma, CtrRenderState& rs) {
  frame_stats(rs);
  const double t0 = ctr_gpu_time_ms();
  // OceanNear::render_jak1
  dma.read_and_advance();
  if (dma.current_tag().kind == DmaTag::Kind::CALL) {
    drain(dma, rs);
    return;
  }
  TextureDma tex;
  if (!read_texture_dma(dma, &tex)) {
    m_failed++;
    drain(dma, rs);
    return;
  }
  if (m_tex_frame != rs.frame_idx) {
    // (the mid-and-far bucket made it already: the same vertices and camera)
    update_texture(tex, rs);
    m_tex_frame = rs.frame_idx;
  }
  const double t1 = ctr_gpu_time_ms();
  m_tex_ms += t1 - t0;

  // CommonOceanRenderer::flush_near's textured pass: alpha blended, depth tested, not written
  ctr_draw_state st;
  memset(&st, 0, sizeof(st));
  st.tex = m_tex;
  st.tcc = 1;
  st.filter = 1;
  st.blend = CTR_BLEND_ALPHA;
  st.atest = CTR_TEST_ALWAYS;
  st.ztest = CTR_TEST_GEQUAL;
  st.zwrite = 0;
  m_batch->begin(st, rs.fog_color);
  const bool ok = m_near->run(dma, m_batch.get());
  m_batch->end();
  m_near_runs++;
  if (!ok) {
    m_failed++;
  }
  drain(dma, rs);
  const double dt = ctr_gpu_time_ms() - t1;
  m_near_ms += dt;
  m_cur_frame_ms += ctr_gpu_time_ms() - t0;
}
