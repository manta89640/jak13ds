/*!
 * @file CtrOceanFull.cpp
 * (AI-assisted)
 * See CtrOceanFull.h.
 */

#include "CtrOceanFull.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>


#include "common/dma/dma.h"
#include "common/dma/gs.h"
#include "common/log/log.h"

#include "game/graphics/ctr/CtrOceanBatch.h"
#include "game/graphics/ctr/CtrOceanVu.h"
#include "game/graphics/ctr/CtrSettings.h"
#include "game/graphics/ctr/CtrVram.h"
#include "game/graphics/ctr/ctr_gpu.h"

namespace {

std::atomic<u32> g_levels_gen{1};
std::string g_levels;  // written while the render thread is idle (ctr_set_levels)

constexpr int kTexSize = 64;   // the wave texture (the PS2's is 128: 32 x 32 quads of 4 x 4)
constexpr int kGrid = 32;      // wave vertices per side (the 33rd row / column repeats the first)


bool is_vu_end_tag(const DmaTag& tag, const VifCode& v0, const VifCode& v1) {
  return tag.qwc == 2 && tag.kind == DmaTag::Kind::CNT && v0.kind == VifCode::Kind::NOP &&
         v1.kind == VifCode::Kind::DIRECT;
}

void drain(DmaFollower& dma, const CtrRenderState& rs) {
  while (dma.current_tag_offset() != rs.next_bucket) {
    dma.read_and_advance();
  }
}

/*!
 * Debug: with the flag file sdmc:/3ds/jak1/ocean_dump, the 200th frame with both ocean buckets
 * (and its env map) goes to sdmc:/3ds/jak1/ocean_dump.bin, for the host replay test
 * (test/ctr_ocean). Format: per bucket "OCNB", u32 near, u32 transfers, then per transfer u32 qwc,
 * u64 vif codes, the data; then "OCNE", u32 w, h, the env map's RGBA8 texels.
 */
struct OceanDump {
  int state = -1;  // -1 unknown, 0 off, 1 armed, 2 done
  int frames = 0;
  std::vector<u8> mid;  // this frame's mid-and-far bucket
  bool armed() {
    if (state < 0) {
      FILE* flag = fopen("sdmc:/3ds/jak1/ocean_dump", "rb");
      state = flag ? 1 : 0;
      if (flag) {
        fclose(flag);
      }
    }
    return state == 1;
  }
  static void put(std::vector<u8>& out, const void* p, size_t n) {
    out.insert(out.end(), (const u8*)p, (const u8*)p + n);
  }
  static void bucket(std::vector<u8>& out, DmaFollower dma, const CtrRenderState& rs, u32 near) {
    std::vector<DmaTransfer> list;
    while (dma.current_tag_offset() != rs.next_bucket) {
      list.push_back(dma.read_and_advance());
    }
    const u32 n = list.size();
    put(out, "OCNB", 4);
    put(out, &near, 4);
    put(out, &n, 4);
    for (auto& t : list) {
      const u32 qwc = t.size_bytes / 16;
      put(out, &qwc, 4);
      put(out, &t.transferred_tag, 8);
      put(out, t.data, t.size_bytes);
    }
  }
  void write(const std::vector<u8>& near, const std::vector<u32>& env, int w, int h) {
    state = 2;
    FILE* f = fopen("sdmc:/3ds/jak1/ocean_dump.bin", "wb");
    if (!f) {
      return;
    }
    fwrite(mid.data(), 1, mid.size(), f);
    fwrite(near.data(), 1, near.size(), f);
    fwrite("OCNE", 1, 4, f);
    fwrite(&w, 4, 1, f);
    fwrite(&h, 4, 1, f);
    fwrite(env.data(), 4, (size_t)w * h, f);
    fclose(f);
    lg::info("[ctr] ocean full: wrote sdmc:/3ds/jak1/ocean_dump.bin");
  }
};
OceanDump g_dump;

}  // namespace

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
    m_native_ok = -1;
    if (m_env_hook && m_native) {
      // (AI-assisted, j1ocean) the env map pass's texture and color (ocean-mid-constants)
      const float col[4] = {m_constants.env_color.x(), m_constants.env_color.y(),
                            m_constants.env_color.z(), m_constants.env_color.w()};
      m_env_hook->ocean_env(m_constants.env_texture.tex0_data, col);
    }
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
      const bool native = m_native && native_ready();
      switch (v1.immediate) {
        case 46:
          native ? native_tile(false) : run_call46_vu2c();
          break;
        case 73:
          native ? native_tile(true) : run_call73_vu2c();
          break;
        case 107:
          native ? native_transition(false) : run_call107_vu2c();
          break;
        case 275:
          native ? native_transition(true) : run_call275_vu2c();
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
  m_env_sent = false;
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
          if (m_native && m_env_hook && !m_env_sent) {
            // (AI-assisted, j1ocean) the env map pass's texture and color, from the constants
            // (ocean-near-constants: env-texture's TEX0 at 970, env-color at 976)
            m_env_sent = true;
            u64 tex0;
            memcpy(&tex0, &m_vu_data[970], 8);
            const float col[4] = {m_vu_data[976].x(), m_vu_data[976].y(), m_vu_data[976].z(),
                                  m_vu_data[976].w()};
            m_env_hook->ocean_env(tex0, col);
          }
          m_native ? native_patch() : run_call39_vu2c();
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

namespace {
//! a row of the wave texture's across samples (update_texture)
struct TexAcross {
  s32 s[kTexSize], t[kTexSize];  // env map texel coordinates (8 fraction bits), times 4
  u32 rb[kTexSize];              // red | blue << 16, times 4
  u32 g[kTexSize];               // green, times 4
};

inline u32 sat8(s32 v) {
  return v < 0 ? 0 : (v > 255 ? 255 : (u32)v);  // (usat)
}

/*!
 * One texel row: rows a and b of the across samples weighted WA : 4 - WA (16x the value), the env
 * map texel at the clamped coordinates times the color, (tex * color) >> 7 at most 255 (GS
 * modulate). ENV64: a 64 x 64 env map (shifts; the clamps are usat).
 */
template <int WA, bool ENV64>
void texel_row(const TexAcross& a, const TexAcross& b, u32* out, const u32* env, int ew, int eh) {
  constexpr int WB = 4 - WA;
  const s32 smax = ENV64 ? 63 : ew - 1, tmax = ENV64 ? 63 : eh - 1;
  for (int x = 0; x < kTexSize; x++) {
    s32 tx = (WA * a.s[x] + WB * b.s[x]) >> 12;
    s32 ty = (WA * a.t[x] + WB * b.t[x]) >> 12;
    tx = tx < 0 ? 0 : (tx > smax ? smax : tx);  // clamp to the edge
    ty = ty < 0 ? 0 : (ty > tmax ? tmax : ty);
    const u32 e = env[ENV64 ? (ty << 6) + tx : ty * ew + tx];
    const u32 crb = ((WA * a.rb[x] + WB * b.rb[x]) >> 4) & 0x00ff00ffu;
    const u32 cg = (WA * a.g[x] + WB * b.g[x]) >> 4;
    const u32 erb = e & 0x00ff00ffu;
    const u32 r = sat8(((s32)(s16)erb * (s32)(s16)crb) >> 7);  // (smulbb)
#if defined(__ARM_FEATURE_DSP)
    // (AI-assisted) the top halves' product in one instruction (both are 0..255: signed halves
    // give the same), and the compiler then saturates with usat
    s32 tt;
    asm("smultt %0, %1, %2" : "=r"(tt) : "r"(erb), "r"(crb));
    const u32 bl = sat8(tt >> 7);
#else
    const u32 bl = sat8(((s32)erb >> 16) * ((s32)crb >> 16) >> 7);  // (smultt)
#endif
    const u32 g = sat8((s32)(((e >> 8) & 0xff) * cg) >> 7);
    // (AI-assisted, j1ocean) alpha: the env map's (GS modulate with the program's alpha 0x80), 0xff
    // = 1 (doubled, as the GPU's combiner takes it): the sky reflection's weight
    const u32 a = e >> 24;
    out[x] = r | (g << 8) | (bl << 16) | ((a >= 0x80 ? 0xffu : 2 * a) << 24);
  }
}
}  // namespace

/*!
 * The wave texture: run_L3_PC's math for the 33 x 33 vertices, then 64 x 64 texels between them
 * (texel centers at 1/4 and 3/4 of the PS2's 4 x 4 texel quads: weights 3:1), each the env map
 * texel at the interpolated lookup times the interpolated color (GS modulate). Integers after the
 * vertices: no float compares or divides in the texel loop.
 */
void CtrOceanFull::update_texture(const TextureDma& t, CtrRenderState& rs) {
  const bool env_ok = update_env(t.env_tex0, rs);
  const int ew = env_ok ? m_env_w : 1, eh = env_ok ? m_env_h : 1;
  const u32 gray = 0x80808080;  // no env map: the vertex colors alone (alpha 1)
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
      // (AI-assisted) read in place (the DMA data, 16-byte aligned), not copied to the stack first
      const float(*q)[4] = (const float(*)[4])(t.rows[r] + g * 8 * 16);
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

  // across: 64 samples per vertex row (3a + b, a + 3b: 4x); red and blue in the 16 bit halves
  // of one word (at most 1020, then 4080 after the down weights: no carries between them)
  // (AI-assisted) 3a + b and a + 3b as (a + b) + 2a and (a + b) + 2b (integers: the same), and
  // vertex c + 1's values kept for the next square instead of read again
  static TexAcross across[kGrid + 1];
  for (int r = 0; r <= kGrid; r++) {
    TexAcross& o = across[r];
    const V* row = verts[r];
    s32 as = row[0].s, at = row[0].t, ag = row[0].g;
    u32 arb = (u32)row[0].r | (u32)row[0].b << 16;
    for (int c = 0; c < kGrid; c++) {
      const V& b = row[c + 1];
      const s32 bs = b.s, bt = b.t, bg = b.g;
      const u32 brb = (u32)b.r | (u32)b.b << 16;
      const s32 ss = as + bs, st = at + bt, sg = ag + bg;
      const u32 srb = arb + brb;
      o.s[2 * c] = ss + 2 * as;
      o.s[2 * c + 1] = ss + 2 * bs;
      o.t[2 * c] = st + 2 * at;
      o.t[2 * c + 1] = st + 2 * bt;
      o.rb[2 * c] = srb + 2 * arb;
      o.rb[2 * c + 1] = srb + 2 * brb;
      o.g[2 * c] = sg + 2 * ag;
      o.g[2 * c + 1] = sg + 2 * bg;
      as = bs;
      at = bt;
      ag = bg;
      arb = brb;
    }
  }

  // down, the env map lookup and the GS modulate
  m_texels.resize(kTexSize * kTexSize);
  u32* out = m_texels.data();
  const bool env64 = ew == 64 && eh == 64;  // (the env map: shifts and saturations)
  for (int y = 0; y < kTexSize; y++, out += kTexSize) {
    const TexAcross& a = across[y >> 1];
    const TexAcross& b = across[(y >> 1) + 1];
    if (env64) {
      (y & 1) ? texel_row<1, true>(a, b, out, env, ew, eh)
              : texel_row<3, true>(a, b, out, env, ew, eh);
    } else {
      (y & 1) ? texel_row<1, false>(a, b, out, env, ew, eh)
              : texel_row<3, false>(a, b, out, env, ew, eh);
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
      m_batch(std::make_unique<CtrOceanBatch>()) {
  // (AI-assisted) config.ini ocean_texture_rate, ocean_near_alpha (CtrSettings.h)
  const CtrSettings& s = ctr_settings();
  m_tex_rate = std::clamp(s.ocean_texture_rate, 1, 8);
  const int floor = (int)(std::clamp(s.ocean_near_alpha, 0.f, 1.f) * 128.f + 0.5f);
  m_near->set_alpha_floor(floor);
  m_mid->set_env_hook(this);  // (AI-assisted, j1ocean) the sky reflection
  m_near->set_env_hook(this);
  lg::info("[ctr] ocean full: wave texture every {} frame(s), near ocean alpha at least {:.2f}",
           m_tex_rate, floor / 128.0);
}

CtrOceanFull::~CtrOceanFull() {
  if (m_tex >= 0) {
    ctr_gpu_tex_delete(m_tex);
  }
  if (m_refl >= 0) {
    ctr_gpu_tex_delete(m_refl);
  }
}

/*!
 * (AI-assisted, j1ocean) The sky reflection's texture: the env map pass's texture (environment-ocean,
 * decoded from CtrVram again when its texture page changes and every 300 frames) times its GS
 * color (env-color, the time of day's: GS modulate, (texel * color) >> 7 at most 255), made again
 * when either changes (a color change alone: at most every ocean_texture_rate frames, like the
 * wave texture). Given to the batch for the native programs' env draws.
 */
void CtrOceanFull::ocean_env(u64 tex0, const float color[4]) {
  if (!m_rs || !m_rs->vram) {
    m_batch->set_env(-1);
    return;
  }
  CtrRenderState& rs = *m_rs;
  GsTex0 t(tex0);
  u64 key = tex0;
  key = (key ^ (u64)(uintptr_t)rs.vram->upload_source(t.tbp0())) * 1099511628211ull;
  key = (key ^ (u64)(uintptr_t)rs.vram->upload_source(t.cbp())) * 1099511628211ull;
  bool src_new = false;
  if (key != m_refl_key || rs.frame_idx < m_refl_frame || rs.frame_idx - m_refl_frame >= 300) {
    int w = 0, h = 0;
    m_refl_ok = rs.vram->decode_for_cpu(tex0, &m_refl_src, &w, &h) && w >= 8 && h >= 8 &&
                w <= 1024 && h <= 1024 && m_refl_src.size() >= (size_t)w * h;
    if (std::find(m_refl_logged.begin(), m_refl_logged.end(), tex0) == m_refl_logged.end() &&
        m_refl_logged.size() < 16) {
      m_refl_logged.push_back(tex0);
      lg::info("[ctr] ocean full: sky reflection tex0 {:x} {}x{} {}", tex0, w, h,
               m_refl_ok ? "ok" : "NOT DECODED (no reflection)");
    }
    if (m_refl_ok && (w != m_refl_w || h != m_refl_h) && m_refl >= 0) {
      ctr_gpu_tex_delete(m_refl);
      m_refl = -1;
    }
    m_refl_w = w;
    m_refl_h = h;
    m_refl_key = key;
    m_refl_frame = rs.frame_idx;
    src_new = true;
  }
  if (!m_refl_ok) {
    m_batch->set_env(-1);
    return;
  }
  // ftoi0, then the GS's 8 bits
  u32 c[3];
  for (int i = 0; i < 3; i++) {
    c[i] = (u32)(s32)color[i] & 0xff;
  }
  const u32 col = c[0] | (c[1] << 8) | (c[2] << 16);
  const bool due = m_refl_made == ~0ull || rs.frame_idx < m_refl_made ||
                   rs.frame_idx - m_refl_made >= (u64)m_tex_rate;
  if (m_refl < 0 || src_new || (col != m_refl_color && due)) {
    const size_t n = (size_t)m_refl_w * m_refl_h;
    m_refl_texels.resize(n);
    const u32* src = m_refl_src.data();
    u32* dst = m_refl_texels.data();
    for (size_t i = 0; i < n; i++) {
      const u32 e = src[i];
      const u32 r = std::min<u32>(((e & 0xff) * c[0]) >> 7, 255);
      const u32 g = std::min<u32>((((e >> 8) & 0xff) * c[1]) >> 7, 255);
      const u32 b = std::min<u32>((((e >> 16) & 0xff) * c[2]) >> 7, 255);
      dst[i] = r | (g << 8) | (b << 16) | 0xff000000u;
    }
    const u8* bytes = (const u8*)m_refl_texels.data();
    if (m_refl < 0) {
      m_refl = ctr_gpu_tex_create_mipmapped(m_refl_w, m_refl_h, bytes);
    } else {
      ctr_gpu_tex_update(m_refl, bytes);
    }
    m_refl_color = col;
    m_refl_made = rs.frame_idx;
    m_refl_updates++;
  }
  m_batch->set_env(m_refl);
}

void CtrOceanFull::set_native(bool on) {
  m_mid->set_native(on);
  m_near->set_native(on);
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
 * 150 frames with the ocean, one log line with the averages. (AI-assisted) Whenever parts of the
 * ocean weren't drawn (unexpected DMA, no vertex memory, refused transition pieces): a warning
 * with the counts since the last one, at most every 300 frames.
 */
void CtrOceanFull::frame_stats(const CtrRenderState& rs) {
  if (rs.frame_idx == m_stat_frame) {
    return;
  }
  m_max_frame_ms = std::max(m_max_frame_ms, m_cur_frame_ms);
  m_cur_frame_ms = 0;
  m_stat_frame = rs.frame_idx;
  if ((m_failed_unlogged || m_batch->dropped_unlogged || m_batch->rejected_unlogged) &&
      (m_warn_frame == ~0ull || rs.frame_idx - m_warn_frame >= 300)) {
    m_warn_frame = rs.frame_idx;
    lg::warn("[ctr] ocean full ({}): parts not drawn since the last warning: {} buckets with "
             "unexpected DMA, {} out of vertex memory, {} transition pieces refused",
             g_levels, m_failed_unlogged, m_batch->dropped_unlogged, m_batch->rejected_unlogged);
    m_failed_unlogged = m_batch->dropped_unlogged = m_batch->rejected_unlogged = 0;
  }
  const u32 gen = g_levels_gen.load();
  if (gen != m_window_gen) {
    m_window_gen = gen;
    m_frames = m_mid_runs = m_near_runs = m_failed = m_tex_updates = m_refl_updates = 0;
    m_batch->env_draws = 0;
    m_tex_ms = m_mid_ms = m_near_ms = m_max_frame_ms = 0;
    m_batch->vertices = m_batch->triangles = m_batch->draws = m_batch->dropped = 0;
    m_batch->rejected = 0;
  }
  m_frames++;
  if (m_frames == 150 && m_logged_gen != gen) {
    m_logged_gen = gen;
    const double n = m_frames - 1;  // (this frame hasn't run yet)
    lg::info(
        "[ctr] ocean full ({}): render thread {:.2f} ms/frame (texture {:.2f} in {:.0f}% of "
        "frames, far+mid {:.2f}, near {:.2f}; max {:.2f}), {:.0f} vertices, {:.0f} triangles, "
        "{:.1f} draws per frame ({:.1f} with the sky reflection, its texture made in {:.0f}% of "
        "frames); mid in {:.0f}%, near in {:.0f}% of frames{}{}{}",
        g_levels, (m_tex_ms + m_mid_ms + m_near_ms) / n, m_tex_ms / n,
        100.0 * m_tex_updates / n, m_mid_ms / n, m_near_ms / n, m_max_frame_ms,
        m_batch->vertices / n, m_batch->triangles / n, m_batch->draws / n,
        m_batch->env_draws / n, 100.0 * m_refl_updates / n, 100.0 * m_mid_runs / n, 100.0 * m_near_runs / n,
        m_failed ? fmt::format(", {} unexpected DMA", m_failed) : "",
        m_batch->dropped ? fmt::format(", {} dropped (no vertex memory)", m_batch->dropped) : "",
        m_batch->rejected ? fmt::format(", {} transition pieces refused", m_batch->rejected)
                          : "");
  }
}

/*!
 * (AI-assisted) config.ini ocean_texture_rate: the wave texture is made again every nth frame
 * (the first time and after a pause right away). Between, the GPU keeps drawing the last one:
 * ctr_gpu_tex_update writes the copy the frame on the GPU doesn't read (pipeline).
 */
bool CtrOceanFull::texture_due(const CtrRenderState& rs) const {
  if (m_tex < 0 || m_tex_frame == ~0ull || rs.frame_idx < m_tex_frame) {
    return true;
  }
  return rs.frame_idx - m_tex_frame >= (u64)m_tex_rate;
}

void CtrOceanFull::render_mid_and_far(DmaFollower& dma, CtrRenderState& rs) {
  frame_stats(rs);
  if (g_dump.armed()) {
    g_dump.mid.clear();
    OceanDump::bucket(g_dump.mid, dma, rs, 0);
  }
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
    m_failed_unlogged++;
    drain(dma, rs);
    return;
  }
  if (texture_due(rs)) {
    update_texture(tex, rs);
    m_tex_frame = rs.frame_idx;
    m_tex_updates++;
  }
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
  m_batch->set_env(-1);  // (AI-assisted, j1ocean) until the constants name it (ocean_env)
  m_rs = &rs;
  const int env_draws0 = m_batch->env_draws;
  read_far(dma);
  bool ok = true;
  if (dma.current_tag_vifcode0().kind == VifCode::Kind::BASE) {
    ok = m_mid->run(dma, m_batch.get());
    m_mid_runs++;
  }
  m_batch->end();
  m_rs = nullptr;
  if (m_batch->env_draws != env_draws0) {
    ctr_gpu_ocean_end();  // (AI-assisted, j1ocean) texture unit 2 off, combiner stage 2 reset
  }
  if (!ok) {
    m_failed++;
    m_failed_unlogged++;
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
  std::vector<u8> dump_near;
  if (g_dump.armed()) {
    OceanDump::bucket(dump_near, dma, rs, 1);
  }
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
    m_failed_unlogged++;
    drain(dma, rs);
    return;
  }
  if (texture_due(rs)) {
    // (not when the mid-and-far bucket made it this frame: the same vertices and camera)
    update_texture(tex, rs);
    m_tex_frame = rs.frame_idx;
    m_tex_updates++;
  }
  const double t1 = ctr_gpu_time_ms();
  m_tex_ms += t1 - t0;

  // CommonOceanRenderer::flush_near's textured pass: alpha blended, depth tested, not written.
  // (AI-assisted, j1ocean) TCC off (the PS2's drw-texture: the alpha is the vertex's; the texture's
  // weights the sky reflection)
  ctr_draw_state st;
  memset(&st, 0, sizeof(st));
  st.tex = m_tex;
  st.tcc = 0;
  st.filter = 1;
  st.blend = CTR_BLEND_ALPHA;
  st.atest = CTR_TEST_ALWAYS;
  st.ztest = CTR_TEST_GEQUAL;
  st.zwrite = 0;
  m_batch->begin(st, rs.fog_color);
  m_batch->set_env(-1);
  m_rs = &rs;
  const int env_draws0 = m_batch->env_draws;
  const bool ok = m_near->run(dma, m_batch.get());
  m_batch->end();
  m_rs = nullptr;
  if (m_batch->env_draws != env_draws0) {
    ctr_gpu_ocean_end();
  }
  m_near_runs++;
  if (!ok) {
    m_failed++;
    m_failed_unlogged++;
  }
  drain(dma, rs);
  if (!dump_near.empty() && ++g_dump.frames == 200) {
    g_dump.write(dump_near, m_env_ok ? m_env : std::vector<u32>(1, 0xff808080u),
                 m_env_ok ? m_env_w : 1, m_env_ok ? m_env_h : 1);
  }
  const double dt = ctr_gpu_time_ms() - t1;
  m_near_ms += dt;
  m_cur_frame_ms += ctr_gpu_time_ms() - t0;
}
