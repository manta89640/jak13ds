/*!
 * @file CtrTexAnimJak2.cpp
 * (AI-assisted)
 * See CtrTexAnimJak2.h.
 */

#include "CtrTexAnimJak2.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "common/goal_constants.h"
#include "common/log/log.h"

#include "fmt/format.h"

#include "game/graphics/ctr/CtrVram.h"
#include "game/graphics/ctr/ctr_gpu.h"

namespace {
// TextureAnimator.cpp kInitialRandomTable
constexpr u8 kInitialRandom[8][16] = {
    {0x20, 0x19, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11, 0x10, 0x89, 0x67, 0x45, 0x23, 0x1},
    {0x37, 0x82, 0x87, 0x23, 0x78, 0x87, 0x4, 0x32, 0x97, 0x91, 0x48, 0x98, 0x30, 0x38, 0x89, 0x87},
    {0x62, 0x47, 0x2, 0x62, 0x78, 0x92, 0x28, 0x90, 0x81, 0x47, 0x72, 0x28, 0x83, 0x29, 0x71, 0x68},
    {0x28, 0x61, 0x17, 0x62, 0x87, 0x74, 0x38, 0x12, 0x83, 0x9, 0x78, 0x12, 0x76, 0x31, 0x72, 0x80},
    {0x39, 0x72, 0x98, 0x34, 0x72, 0x98, 0x69, 0x78, 0x65, 0x71, 0x98, 0x83, 0x97, 0x23, 0x98, 0x1},
    {0x97, 0x38, 0x72, 0x98, 0x23, 0x87, 0x23, 0x98, 0x93, 0x72, 0x98, 0x20, 0x81, 0x29, 0x10, 0x62},
    {0x28, 0x75, 0x38, 0x82, 0x99, 0x30, 0x72, 0x87, 0x83, 0x9, 0x14, 0x98, 0x10, 0x43, 0x87, 0x29},
    {0x87, 0x23, 0x0, 0x87, 0x18, 0x98, 0x12, 0x98, 0x10, 0x98, 0x21, 0x83, 0x90, 0x37, 0x62, 0x71}};

// TextureAnimator.h SkyInput
struct SkyInput {
  float fog_height;
  float cloud_min;
  float cloud_max;
  float times[11];
  float max_times[6];
  float scales[6];
  s32 cloud_dest;
};
static_assert(sizeof(SkyInput) == 108);

// texture-anim-pc-upload
struct PcUpload {
  u32 data;
  u16 width, height;
  u32 dest;
  u8 format, force_to_gpu, pad[2];
};
static_assert(sizeof(PcUpload) == 16);

// bytes of a generic upload's image (0: a format not handled)
u32 image_bytes(const PcUpload& u) {
  if (u.width == 16 && u.height == 16 && u.format != 0x13) {
    return 16 * 16 * 4;  // a CLUT (UPLOAD_CLUT_16_16 records may not say PSMCT32)
  }
  switch (u.format) {
    case 0:  // PSMCT32
      return (u32)u.width * u.height * 4;
    case 0x13:  // PSMT8
      return (u32)u.width * u.height;
    default:
      return 0;
  }
}

bool image_ok(const PcUpload& u, u32 bytes) {
  return bytes && u.width && u.height && u.data &&
         (u64)u.data + bytes <= (u64)EE_MAIN_MEM_SIZE;
}

// TextureAnimator's m_index_to_clut_addr: a PSMT8 index -> its entry in a 16 x 16 CT32 CLUT
// (the GS's CSM1 order)
u8 clut_addr(int i) {
  const int chunk = i / 16;
  int off = i % 16;
  int x = (chunk & 1) ? 8 : 0;
  int y = (chunk >> 1) * 2;
  if (off >= 8) {
    off -= 8;
    y++;
  }
  return (u8)(x + off + y * 16);
}

// GL_LINEAR magnification by 2 with clamp to edge, rows then columns: src n x n -> dst 2n x 2n
void upsample2(const float* src, int n, float* tmp, float* dst) {
  const int m = 2 * n;
  for (int y = 0; y < n; y++) {
    const float* s = src + y * n;
    float* t = tmp + y * m;
    for (int k = 0; k < n; k++) {
      const float a = s[std::max(k - 1, 0)], b = s[k], c = s[std::min(k + 1, n - 1)];
      t[2 * k] = 0.25f * a + 0.75f * b;
      t[2 * k + 1] = 0.75f * b + 0.25f * c;
    }
  }
  for (int k = 0; k < n; k++) {
    const float* a = tmp + std::max(k - 1, 0) * m;
    const float* b = tmp + k * m;
    const float* c = tmp + std::min(k + 1, n - 1) * m;
    float* d0 = dst + (2 * k) * m;
    float* d1 = dst + (2 * k + 1) * m;
    for (int x = 0; x < m; x++) {
      d0[x] = 0.25f * a[x] + 0.75f * b[x];
      d1[x] = 0.75f * b[x] + 0.25f * c[x];
    }
  }
}

// tex_anim.frag cloud_lookup
float cloud_lookup(float v, float minimum, float maximum) {
  maximum = std::max(minimum, maximum);
  if (v <= minimum) {
    return 0.f;
  }
  if (v >= maximum) {
    return 1.f;
  }
  const float alpha = (v - minimum) / (maximum - minimum);
  const float s = std::sin(alpha * 3.1415926f / 2.f);
  return s * s;
}
}  // namespace

CtrTexAnimJak2::CtrTexAnimJak2() {
  memcpy(m_random, kInitialRandom, sizeof(m_random));
  // TextureAnimator::setup_sky: a new and an old texture per layer, 16 x 16 up to 128 x 128
  for (int i = 0, dim = kCloudSize >> (kCloudLayers - 1); i < kCloudLayers; i++, dim *= 2) {
    auto& n = m_noise[i];
    n.dim = dim;
    n.new_tex.resize(dim * dim);
    n.old_tex.resize(dim * dim);
    make_noise(n.new_tex.data(), dim);
    make_noise(n.old_tex.data(), dim);
  }
  m_acc.resize(kCloudSize * kCloudSize / 4);   // 64 x 64
  m_acc2.resize(kCloudSize * kCloudSize / 4);
  m_tmp.resize(kCloudSize * kCloudSize / 8);   // upsample2's row pass: 2n x n (n <= 32)
  m_out.resize(kCloudSize * kCloudSize / 4);
}

void CtrTexAnimJak2::make_noise(u8* dest, int dim) {
  // TextureAnimator.cpp make_noise_texture: each quadword is the sum of three rows of the table,
  // stored in a fourth one (bytes wrap)
  const int qw_per_row = dim / 16;
  for (int qwi = 0; qwi < dim * qw_per_row; qwi++) {
    u8* r0 = m_random[(m_random_index + 0) % 8];
    u8* r1 = m_random[(m_random_index + 3) % 8];
    u8* r2 = m_random[(m_random_index + 5) % 8];
    u8* r3 = m_random[(m_random_index + 7) % 8];
    for (int b = 0; b < 16; b++) {
      r3[b] = (u8)(r0[b] + r1[b] + r2[b]);
    }
    memcpy(dest + 16 * qwi, r3, 16);
    m_random_index = (m_random_index + 1) % 8;
  }
}

void CtrTexAnimJak2::clouds(const u8* data, int bytes, CtrVram* vram) {
  if (bytes < (int)sizeof(SkyInput)) {
    return;
  }
  SkyInput in;
  memcpy(&in, data, sizeof(in));
  // TextureAnimator::run_clouds: each layer adds its new and old noise texture, weighted by how
  // far its time is from one to the other; times[0] belongs to the alpha ramp CLUT
  bool changed = false;
  for (int i = 0; i < kCloudLayers; i++) {
    auto& n = m_noise[i];
    const float t = in.times[1 + i];
    if (t < n.last_time) {
      std::swap(n.new_tex, n.old_tex);
      make_noise(n.new_tex.data(), n.dim);
      changed = true;
    }
    n.last_time = t;
  }
  // the clouds move slowly: every 4th frame, unless a layer got new noise
  if ((m_frames++ & 3) && !changed && vram->has_override((u32)in.cloud_dest)) {
    return;
  }
  const double t0 = ctr_gpu_time_ms();
  // the layers are added coarse to fine (each 2x larger one is the sum so far magnified, plus
  // its own): the same sum as magnifying each layer to the final size on its own, but for the
  // bilinear filter of the 4x and 8x steps. Made at 64 x 64 (the finest layer, 128 x 128, added
  // as the average of each 2 x 2 of its texels): 16 KB of linear memory, a quarter of the work.
  // The 8-bit frame buffer clamps every add at 1; the adds are all positive, so one clamp at the
  // end gives the same.
  float* acc = m_acc.data();
  float* next = m_acc2.data();
  int dim = 0;
  for (int i = 0; i < kCloudLayers; i++) {
    const auto& n = m_noise[i];
    const float interp = in.max_times[i] != 0.f ? n.last_time / in.max_times[i] : 0.f;
    const float wn = interp * in.scales[i] * (1.f / 255.f);
    const float wo = (1.f - interp) * in.scales[i] * (1.f / 255.f);
    if (i == 0) {
      dim = n.dim;
      for (int p = 0; p < dim * dim; p++) {
        acc[p] = wn * n.new_tex[p] + wo * n.old_tex[p];
      }
      continue;
    }
    if (n.dim > kCloudSize / 2) {
      // the finest layer: averaged down to the 64 x 64 result
      const int nd = n.dim;
      const float qn = 0.25f * wn, qo = 0.25f * wo;
      for (int y = 0; y < dim; y++) {
        for (int x = 0; x < dim; x++) {
          const int a = 2 * y * nd + 2 * x;
          acc[y * dim + x] += qn * (n.new_tex[a] + n.new_tex[a + 1] + n.new_tex[a + nd] +
                                    n.new_tex[a + nd + 1]) +
                              qo * (n.old_tex[a] + n.old_tex[a + 1] + n.old_tex[a + nd] +
                                    n.old_tex[a + nd + 1]);
        }
      }
      continue;
    }
    upsample2(acc, dim, m_tmp.data(), next);
    std::swap(acc, next);
    dim *= 2;
    for (int p = 0; p < dim * dim; p++) {
      acc[p] += wn * n.new_tex[p] + wo * n.old_tex[p];
    }
  }
  // tex_anim.frag, enable_tex 2: rgb 0.5, alpha 0.5 * cloud_lookup of the 8-bit sum (0x80 = 1 in
  // the PS2's texture scale, like the PC's texture)
  // (AI-assisted, j2gpu) The sum is an 8-bit value first, so the lookup (a sinf per texel: most of
  // the update's 3 ms in Azahar) is made once per value: the same texels
  const int half = dim;
  u32 ramp[256];
  for (int i = 0; i < 256; i++) {
    const float v = (float)i * (1.f / 255.f);
    ramp[i] = 0x808080u | ((u32)std::lround(cloud_lookup(v, in.cloud_min, in.cloud_max) * 127.5f) << 24);
  }
  for (int p = 0; p < dim * dim; p++) {
    const float q = std::round(std::min(acc[p], 1.f) * 255.f);
    if (q >= 0.f && q <= 255.f) {
      m_out[p] = ramp[(int)q];
    } else {
      // (a negative sum: a layer's time past its end; or not a number) as before
      m_out[p] = 0x808080u |
                 ((u32)std::lround(cloud_lookup(q * (1.f / 255.f), in.cloud_min, in.cloud_max) *
                                   127.5f)
                  << 24);
    }
  }
  vram->set_override((u32)in.cloud_dest, half, half, m_out.data());
  m_ms += ctr_gpu_time_ms() - t0;
  m_runs++;
}

const u8* CtrTexAnimJak2::image_data(u32 addr, u32 bytes, const u8* ee_mem) const {
  if (m_snap_valid) {
    for (const auto& [a, off] : m_snap_index) {
      if (a == addr && off + bytes <= m_snap_bytes.size()) {
        return m_snap_bytes.data() + off;
      }
    }
  }
  return ee_mem + addr;
}

void CtrTexAnimJak2::snapshot_upload(const u8* rec, int bytes, const u8* ee_mem) {
  if (bytes != (int)sizeof(PcUpload)) {
    return;
  }
  PcUpload u;
  memcpy(&u, rec, sizeof(u));
  const u32 n = image_bytes(u);
  if (!image_ok(u, n)) {
    return;
  }
  for (const auto& e : m_snap_index) {
    if (e.first == u.data) {
      return;  // the same image twice in a frame (the logo is drawn more than once)
    }
  }
  m_snap_index.emplace_back(u.data, m_snap_bytes.size());
  m_snap_bytes.insert(m_snap_bytes.end(), ee_mem + u.data, ee_mem + u.data + n);
}

void CtrTexAnimJak2::generic_upload(const u8* rec,
                                    int bytes,
                                    const u8* ee_mem,
                                    CtrVram* vram,
                                    bool clut_upload) {
  if (bytes != (int)sizeof(PcUpload)) {
    return;
  }
  PcUpload u;
  memcpy(&u, rec, sizeof(u));
  const u32 n = image_bytes(u);
  if (!image_ok(u, n)) {
    m_t8_waiting = false;
    return;
  }
  const u8* src = image_data(u.data, n, ee_mem);
#ifndef __3DS__
  static const bool log_up = getenv("CTR_UPLOAD_LOG") != nullptr;  // host diagnosis
  if (log_up) {
    lg::info("[ctr] anim generic upload {}x{} format {} to {} (waiting t8 {})", u.width, u.height,
             u.format, u.dest, m_t8_waiting);
  }
#endif
  if (u.format == 0x13 && !clut_upload) {
    m_t8.assign(src, src + n);
    m_t8_w = u.width;
    m_t8_h = u.height;
    m_t8_dest = u.dest;
    m_t8_waiting = true;
    return;
  }
  // CT32
  if (clut_upload || (m_t8_waiting && u.width == 16 && u.height == 16)) {
    if (!m_t8_waiting || u.width != 16 || u.height != 16) {
      return;  // a CLUT for something not done here (the clouds' alpha ramp)
    }
    // the CLUT of the PSMT8 image before it (TextureAnimator: GENERIC_PSMT8's cbp)
    m_t8_waiting = false;
    const u32* clut = (const u32*)src;
    // the GPU's smallest texture is 8 x 8: rows repeated
    const int w = std::max(m_t8_w, 8), h = std::max(m_t8_h, 8);
    m_t8_rgba.resize((size_t)w * h);
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        const u8 i = m_t8[(size_t)(x * m_t8_w / w) + (size_t)(y * m_t8_h / h) * m_t8_w];
        u32 c;
        memcpy(&c, clut + clut_addr(i), 4);
        m_t8_rgba[(size_t)x + (size_t)y * w] = c;
      }
    }
    vram->set_override(m_t8_dest, w, h, m_t8_rgba.data());
    return;
  }
  if (u.width >= 64) {
    vram->upload_ct32(src, u.dest, u.width, u.height);
  }
}

// ---------------- (AI-assisted) the animated texture slots ----------------

#include "common/texture/texture_slots.h"

#include "game/graphics/ctr/CtrLevel.h"
#include "game/graphics/ctr/CtrTexAnimJak2Defs.h"
#include "game/graphics/opengl_renderer/slime_lut.h"

namespace {
using namespace ctr_texanim_jak2;

// TextureAnimator.h LayerVals
struct LayerVals {
  float color[4];
  float scale[2];
  float offset[2];
  float st_scale[2];
  float st_offset[2];
  float qs[4];
  float rot;
  float st_rot;
  u8 pad[8];
};
static_assert(sizeof(LayerVals) == 80);

int slot_index(const char* name) {
  const auto& slots = jak2_animated_texture_slots();
  for (size_t i = 0; i < slots.size(); i++) {
    if (slots[i] == name) {
      return (int)i;
    }
  }
  return -1;
}

int pow2_at_least_8(int v) {
  int p = 8;
  while (p < v && p < c3t::kMaxSize) {
    p *= 2;
  }
  return p;
}

/*!
 * One layer of a fixed animation (TextureAnimator::run_fixed_animation: set_draw_data_from_
 * interpolated, tex_anim.vert / .frag, the layer's blend) drawn into dst (w x h RGBA8, rows from
 * the top, alpha 0x80 = 1.0). The quad's corners c in [-0.5, 0.5]^2 go to pixels pos = R(c) * scale
 * + offset (times the size) and to texture coordinates st = R_st(c * st_scale) + st_offset: one
 * affine map from pixel centers to texture coordinates. Bilinear (GL_LINEAR), repeat or clamp.
 * Color: the rgb pass blends Cs * As + Cd (add) or Cs * As + Cd * (1 - As) (mix), As = 2 * the
 * alpha (clamped); the alpha pass writes the alpha (tcc: color alpha * texture alpha).
 */
void draw_layer(u32* dst, int w, int h, const LayerVals& v, const u8* tex, int tw, int th, bool clamp,
                bool add) {
  // the color (u32 of color * 128 in the PC's draw data, / 128 in the shader), 1/256 units
  int col[4];
  for (int c = 0; c < 4; c++) {
    col[c] = (int)std::max(0.f, std::floor(v.color[c] * 128.f)) * 2;
  }
  // corners -> pixels: p = M c + o
  float m00 = v.scale[0] * w, m01 = 0, m10 = 0, m11 = v.scale[1] * h;
  if (v.rot != 0.f) {
    const float r = 2.f * 3.14159265f * v.rot / 65536.f;
    const float sn = std::sin(r), cs = std::cos(r);
    // corner' = vx * c.x + vy * c.y, vx = (sin, cos), vy = (cos, -sin); then * scale
    m00 = sn * v.scale[0] * w;
    m01 = cs * v.scale[0] * w;
    m10 = cs * v.scale[1] * h;
    m11 = -sn * v.scale[1] * h;
  }
  const float ox = v.offset[0] * w, oy = v.offset[1] * h;
  const float det = m00 * m11 - m01 * m10;
  if (std::fabs(det) < 1e-6f) {
    return;
  }
  // pixels -> corners: c = Minv (p - o)
  const float i00 = m11 / det, i01 = -m01 / det, i10 = -m10 / det, i11 = m00 / det;
  // corners -> st: st = S c + so
  float s00 = v.st_scale[0], s01 = 0, s10 = 0, s11 = v.st_scale[1];
  if (v.st_rot != 0.f) {
    const float r = 2.f * 3.14159265f * v.st_rot / 65536.f;
    const float sn = std::sin(r), cs = std::cos(r);
    // st = so + vx * (c.x * sx) + vy * (c.y * sy)
    s00 = sn * v.st_scale[0];
    s01 = cs * v.st_scale[1];
    s10 = cs * v.st_scale[0];
    s11 = -sn * v.st_scale[1];
  }
  // texel coordinates (1/256 texel, texel centers at +0.5) per pixel step in x
  const float a00 = (s00 * i00 + s01 * i10) * tw, a10 = (s10 * i00 + s11 * i10) * th;
  const int du = (int)std::lround(a00 * 256.f), dt = (int)std::lround(a10 * 256.f);
  const int wm = tw - 1, hm = th - 1;  // powers of two
  for (int y = 0; y < h; y++) {
    const float py = y + 0.5f - oy, px = 0.5f - ox;
    const float cx0 = i00 * px + i01 * py, cy0 = i10 * px + i11 * py;
    // the pixels of this row inside the quad: -0.5 <= c0 + i * x <= 0.5 for both corner axes
    float lo = 0.f, hi = (float)w - 1.f;
    auto clip = [&](float c0, float d) {
      if (std::fabs(d) < 1e-9f) {
        if (c0 < -0.5001f || c0 > 0.5001f) {
          hi = -1.f;
        }
        return;
      }
      float x0 = (-0.5001f - c0) / d, x1 = (0.5001f - c0) / d;
      if (x0 > x1) {
        std::swap(x0, x1);
      }
      lo = std::max(lo, x0);
      hi = std::min(hi, x1);
    };
    clip(cx0, i00);
    clip(cy0, i10);
    const int xs = std::max(0, (int)std::ceil(lo)), xe = std::min(w - 1, (int)std::floor(hi));
    if (xs > xe) {
      continue;
    }
    const float cx = cx0 + i00 * xs, cy = cy0 + i10 * xs;
    int u = (int)std::floor(((s00 * cx + s01 * cy + v.st_offset[0]) * tw - 0.5f) * 256.f);
    int t = (int)std::floor(((s10 * cx + s11 * cy + v.st_offset[1]) * th - 0.5f) * 256.f);
    u32* row = dst + (size_t)y * w;
    for (int x = xs; x <= xe; x++, u += du, t += dt) {
      // bilinear: four texels, weights summing to 256 (two channels per word)
      int x0 = u >> 8, y0 = t >> 8;
      const int fx = u & 255, fy = t & 255;
      int x1 = x0 + 1, y1 = y0 + 1;
      if (clamp) {
        x0 = std::clamp(x0, 0, wm);
        x1 = std::clamp(x1, 0, wm);
        y0 = std::clamp(y0, 0, hm);
        y1 = std::clamp(y1, 0, hm);
      } else {
        x0 &= wm;
        x1 &= wm;
        y0 &= hm;
        y1 &= hm;
      }
      const u32* tp = (const u32*)tex;
      const u32 p00 = tp[y0 * tw + x0], p01 = tp[y0 * tw + x1];
      const u32 p10 = tp[y1 * tw + x0], p11 = tp[y1 * tw + x1];
      const u32 w11 = (u32)(fx * fy) >> 8, w01 = (u32)fx - w11, w10 = (u32)fy - w11,
                w00 = 256 - w01 - w10 - w11;
      const u32 rb = ((p00 & 0xff00ff) * w00 + (p01 & 0xff00ff) * w01 + (p10 & 0xff00ff) * w10 +
                      (p11 & 0xff00ff) * w11) >> 8;
      const u32 ga = (((p00 >> 8) & 0xff00ff) * w00 + ((p01 >> 8) & 0xff00ff) * w01 +
                      ((p10 >> 8) & 0xff00ff) * w10 + ((p11 >> 8) & 0xff00ff) * w11) >> 8;
      const int sr = (int)(rb & 0xff), sb = (int)((rb >> 16) & 0xff);
      const int sg = (int)(ga & 0xff), sa = (int)((ga >> 16) & 0xff);
      // the alpha pass: color alpha * texture alpha (tcc), 0..255; the rgb pass: As = 2 * that
      const int a = std::min(255, (sa * col[3]) >> 8);
      const int as = std::min(256, 2 * a);  // As * 256
      const u32 d = row[x];
      int dr = d & 0xff, dg = (d >> 8) & 0xff, db = (d >> 16) & 0xff;
      const int cr = (sr * col[0]) >> 8, cg = (sg * col[1]) >> 8, cb = (sb * col[2]) >> 8;
      if (add) {
        dr = std::min(255, dr + ((cr * as) >> 8));
        dg = std::min(255, dg + ((cg * as) >> 8));
        db = std::min(255, db + ((cb * as) >> 8));
      } else {
        dr = std::min(255, (cr * as + dr * (256 - as)) >> 8);
        dg = std::min(255, (cg * as + dg * (256 - as)) >> 8);
        db = std::min(255, (cb * as + db * (256 - as)) >> 8);
      }
      row[x] = (u32)dr | ((u32)dg << 8) | ((u32)db << 16) | ((u32)a << 24);
    }
  }
}

// box filter a w x h RGBA image down to dw x dh (powers of two, dw <= w, dh <= h)
void downsample(const u32* in, int w, int h, u32* out, int dw, int dh) {
  const int fx = w / dw, fy = h / dh;
  for (int y = 0; y < dh; y++) {
    for (int x = 0; x < dw; x++) {
      u32 acc[4] = {0, 0, 0, 0};
      for (int j = 0; j < fy; j++) {
        for (int i = 0; i < fx; i++) {
          const u32 v = in[(size_t)(y * fy + j) * w + x * fx + i];
          for (int c = 0; c < 4; c++) {
            acc[c] += (v >> (8 * c)) & 0xff;
          }
        }
      }
      const u32 n = (u32)(fx * fy);
      out[(size_t)y * dw + x] = (acc[0] / n) | ((acc[1] / n) << 8) | ((acc[2] / n) << 16) |
                                ((acc[3] / n) << 24);
    }
  }
}
}  // namespace

CtrTexAnimJak2::Src* CtrTexAnimJak2::src(const char* name, const char* dgo, bool load) {
  if (!m_c3t_tried) {
    m_c3t_tried = true;
    m_c3t_path = (ctr_level_c3l_dir() / "texanim.c3t").string();
    FILE* f = fopen(m_c3t_path.c_str(), "rb");
    c3t::Header hdr{};
    if (f && fread(&hdr, sizeof(hdr), 1, f) == 1 && !memcmp(hdr.magic, c3t::kMagic, 4) &&
        hdr.version == c3t::kVersion && hdr.count < 4096 &&
        fseek(f, (long)hdr.entries_offset, SEEK_SET) == 0) {
      std::vector<c3t::Entry> es(hdr.count);
      if (fread(es.data(), sizeof(c3t::Entry), es.size(), f) == es.size()) {
        for (const auto& e : es) {
          Src s;
          s.name.assign(e.name, strnlen(e.name, sizeof(e.name)));
          s.dgo.assign(e.dgo, strnlen(e.dgo, sizeof(e.dgo)));
          s.w = e.w;
          s.h = e.h;
          s.kind = e.kind;
          s.offset = e.offset;
          s.size = e.size;
          m_srcs.push_back(std::move(s));
        }
      }
    }
    if (f) {
      fclose(f);
    }
    lg::info("[ctr] jak2 texture animation: {} textures in {}", m_srcs.size(), m_c3t_path);
  }
  for (auto& s : m_srcs) {
    if (s.name != name || s.dgo != (dgo ? dgo : "")) {
      continue;
    }
    if (load && s.data.empty() && s.size) {
      FILE* f = fopen(m_c3t_path.c_str(), "rb");
      if (f) {
        s.data.resize(s.size);
        if (fseek(f, (long)s.offset, SEEK_SET) != 0 ||
            fread(s.data.data(), 1, s.size, f) != s.size) {
          s.data.clear();
        }
        fclose(f);
      }
    }
    if (load && s.data.empty() && s.kind != c3t::kSizeOnly) {
      return nullptr;
    }
    s.used_frame = m_frame;
    return &s;
  }
  return nullptr;
}

bool CtrTexAnimJak2::Out::upload(int slot_index) {
  if (tex < 0) {
    tex = ctr_gpu_tex_create(w, h, (const u8*)rgba.data());
    if (tex < 0) {
      return false;
    }
  } else {
    ctr_gpu_tex_update(tex, (const u8*)rgba.data());
  }
  if (slot != slot_index) {
    slot = slot_index;
    ctr_gpu_anim_slot_set(slot, tex);
  }
  return true;
}

void CtrTexAnimJak2::Out::release() {
  if (slot >= 0) {
    ctr_gpu_anim_slot_set(slot, -1);
    slot = -1;
  }
  if (tex >= 0) {
    ctr_gpu_tex_delete(tex);
    tex = -1;
  }
  std::vector<u32>().swap(rgba);
  std::vector<u8>().swap(input);
  weight = -1.f;
}

CtrTexAnimJak2::Out* CtrTexAnimJak2::out(const char* name, int w, int h) {
  auto& o = m_outs[name];
  if ((int)o.rgba.size() != w * h || o.w != w || o.h != h) {
    o.release();
    o.w = w;
    o.h = h;
    o.rgba.assign((size_t)w * h, 0);
  }
  o.seen_frame = m_frame;
  return &o;
}

void CtrTexAnimJak2::fixed_anim(const void* def_ptr,
                                const u8* in,
                                float time,
                                const u8* vals,
                                int nvals) {
  const auto& def = *(const Anim*)def_ptr;
  const int slot = slot_index(def.dest);
  if (slot < 0 || slot >= CTR_ANIM_SLOTS) {
    return;
  }
  // the destination's size (the base texture's or the override), at most 64 x 64
  int w = def.w, h = def.h;
  if (!w || !h) {
    const Src* d = src(def.dest, nullptr, false);
    if (!d) {
      return;
    }
    w = d->w;
    h = d->h;
  }
  // at most kFixedMax per side (the CPU's cost: ~100 instructions per pixel and layer)
  w = std::min(pow2_at_least_8(w), kFixedMax);
  h = std::min(pow2_at_least_8(h), kFixedMax);
  // only the slots drawn in the last frame (the game sends some, the skull gem and the bomb
  // gradient, in every level whether a model shows them or not); the base texture until then
  if (!(ctr_gpu_anim_slots_drawn() & (1ull << slot))) {
    return;
  }
  auto* o = out(def.dest, w, h);
  // redrawn every kFixedRate frames, if its input changed; at most kFixedPerFrame animations a
  // frame (the one waiting longest first: the others' turn comes in the next frames)
  const size_t n = 4 + (size_t)nvals;
  if (o->tex >= 0 && (m_frame - o->drawn_frame < (u64)kFixedRate ||
                      m_fixed_this_frame >= kFixedPerFrame ||
                      (o->input.size() == n && !memcmp(o->input.data(), &time, 4) &&
                       !memcmp(o->input.data() + 4, vals, nvals)))) {
    return;
  }
  m_fixed_this_frame++;
  (void)in;
  o->input.resize(n);
  memcpy(o->input.data(), &time, 4);
  memcpy(o->input.data() + 4, vals, nvals);
  o->drawn_frame = m_frame;
  // clear: the def's color (0, 0, 0, 0x80) / 128: black, alpha 1.0 (255)
  std::fill(o->rgba.begin(), o->rgba.end(), 0xff000000u);
  for (int l = 0; l < def.layers; l++) {
    const auto& ld = def.layer[l];
    if (time < ld.start || time > ld.end) {
      continue;
    }
    const Src* s = src(ld.tex, nullptr, true);
    if (!s || s->kind != c3t::kRgba) {
      continue;
    }
    LayerVals a, b, iv;
    memcpy(&a, vals + 160 * l, 80);
    memcpy(&b, vals + 160 * l + 80, 80);
    const float f = ld.end > ld.start ? (time - ld.start) / (ld.end - ld.start) : 0.f;
    const float* fa = (const float*)&a;
    const float* fb = (const float*)&b;
    float* fi = (float*)&iv;
    for (int k = 0; k < 18; k++) {
      fi[k] = fa[k] + (fb[k] - fa[k]) * f;
    }
    draw_layer(o->rgba.data(), w, h, iv, s->data.data(), s->w, s->h, ld.clamp, ld.blend == kAdd);
  }
  o->upload(slot);
  m_slot_updates++;
  m_slot_names[def.dest]++;
}

void CtrTexAnimJak2::clut_group(const void* group_ptr, const u8* data, int bytes) {
  const auto& g = *(const ClutGroup*)group_ptr;
  if (bytes < 16 || !g.count) {
    return;
  }
  float f;
  memcpy(&f, data, 4);
#ifndef __3DS__
  // host diagnosis: CTR_TEST_CLUT_WEIGHT=0..1 blends every group by that weight (Dark Jak's colors
  // without playing to him)
  static const char* test_w = getenv("CTR_TEST_CLUT_WEIGHT");
  if (test_w) {
    f = (float)atof(test_w);
  }
#endif
  const float w0 = 1.f - f, w1 = f;
  for (int i = 0; i < g.count; i++) {
    const auto& t = g.tex[i];
    const int slot = slot_index(t.name);
    if (slot < 0 || slot >= CTR_ANIM_SLOTS) {
      continue;
    }
    auto it = m_outs.find(t.name);
    // at weight 0 the level's base texture (baked with the first CLUT) is the same: nothing to do
    // until the weight first moves
    if (f == 0.f && (it == m_outs.end() || it->second.tex < 0)) {
      continue;
    }
    if (it != m_outs.end() && it->second.weight == f && it->second.tex >= 0) {
      it->second.seen_frame = m_frame;
      continue;
    }
    const Src* dst = src(t.name, t.dgo, true);
    const Src* c0 = src((std::string(t.name) + t.suf0).c_str(), t.dgo, true);
    const Src* c1 = src((std::string(t.name) + t.suf1).c_str(), t.dgo, true);
    if (!dst || !c0 || !c1 || dst->kind != c3t::kIndex || c0->kind != c3t::kIndex ||
        c1->kind != c3t::kIndex) {
      continue;
    }
    const int w = pow2_at_least_8(dst->w), h = pow2_at_least_8(dst->h);
    auto* o = out(t.name, w, h);
    o->weight = f;
    // ClutBlender::run: the blended CLUT (cast to u8), then the index lookups
    u32 clut[256];
    const u8* a = c0->data.data() + (size_t)c0->w * c0->h;
    const u8* b = c1->data.data() + (size_t)c1->w * c1->h;
    for (int k = 0; k < 256; k++) {
      u32 v = 0;
      for (int c = 0; c < 4; c++) {
        const float x = a[4 * k + c] * w0 + b[4 * k + c] * w1;
        v |= (u32)(u8)std::clamp((int)x, 0, 255) << (8 * c);
      }
      clut[k] = v;
    }
    const u8* idx = dst->data.data();
    for (int y = 0; y < h; y++) {
      for (int x = 0; x < w; x++) {
        o->rgba[(size_t)y * w + x] = clut[idx[(size_t)(y * dst->h / h) * dst->w + x * dst->w / w]];
      }
    }
    o->upload(slot);
    m_slot_updates++;
    m_slot_names[t.name]++;
  }
}

bool CtrTexAnimJak2::run(u16 code, const u8* data, int bytes) {
  for (const auto& g : kClutGroups) {
    if (g.code == code) {
      const double t0 = ctr_gpu_time_ms();
      clut_group(&g, data, bytes);
      m_slot_ms += ctr_gpu_time_ms() - t0;
      return true;
    }
  }
  for (const auto& arr : kArrays) {
    if (arr.code != code) {
      continue;
    }
    // per animation: 16 bytes (time, tbp), then two LayerVals per layer
    int expect = 0;
    for (int a = 0; a < arr.anims; a++) {
      expect += 16 + 160 * arr.anim[a].layers;
    }
    if (bytes != expect) {
      return true;
    }
    const double t0 = ctr_gpu_time_ms();
    const u8* p = data;
    for (int a = 0; a < arr.anims; a++) {
      float time;
      memcpy(&time, p, 4);
      const int nv = 160 * arr.anim[a].layers;
      fixed_anim(&arr.anim[a], p, time, p + 16, nv);
      p += 16 + nv;
    }
    m_slot_ms += ctr_gpu_time_ms() - t0;
    return true;
  }
  return code == 26;  // KOR_TRANSFORM: an empty group on the PC too
}

void CtrTexAnimJak2::slime(const u8* data, int bytes) {
  // TextureAnimator.h SlimeInput
  struct SlimeInput {
    float times[9];
    s32 dest;
    s32 scroll_dest;
  };
  if (bytes < (int)sizeof(SlimeInput)) {
    return;
  }
  SlimeInput in;
  memcpy(&in, data, sizeof(in));
  const int slot = slot_index(kSlimeDest), slot2 = slot_index(kSlimeScrollDest);
  const double t0 = ctr_gpu_time_ms();
  constexpr int kN = 64;  // TextureAnimator's 128 x 128, at half the size like the clouds
  constexpr float kMaxTimes[4] = {600.f, 300.f, 150.f, 75.f};
  constexpr float kScales[4] = {0.55f, 0.6f, 0.3f, 0.1f};
  if (!m_slime_init) {
    m_slime_init = true;
    for (int i = 0, dim = 16; i < 4; i++, dim *= 2) {
      auto& n = m_slime_noise[i];
      n.dim = dim;
      n.new_tex.resize(dim * dim);
      n.old_tex.resize(dim * dim);
      make_noise(n.new_tex.data(), dim);
      make_noise(n.old_tex.data(), dim);
    }
    m_slime_acc.resize(kN * kN);
  }
  bool changed = false;
  for (int i = 0; i < 4; i++) {
    auto& n = m_slime_noise[i];
    const float t = in.times[1 + i];
    if (t < n.last_time) {
      std::swap(n.new_tex, n.old_tex);
      make_noise(n.new_tex.data(), n.dim);
      changed = true;
    }
    n.last_time = t;
  }
  const u64 drawn = ctr_gpu_anim_slots_drawn();
  if (!((slot >= 0 && (drawn & (1ull << slot))) || (slot2 >= 0 && (drawn & (1ull << slot2))))) {
    m_slot_ms += ctr_gpu_time_ms() - t0;
    return;  // not drawn: the noise above keeps its time
  }
  auto* o1 = out(kSlimeDest, kN, kN);
  auto* o2 = out(kSlimeScrollDest, kN, kN);
  if (o1->tex >= 0 && !changed && m_frame - o1->drawn_frame < (u64)kFixedRate) {
    m_slot_ms += ctr_gpu_time_ms() - t0;
    return;
  }
  o1->drawn_frame = o2->drawn_frame = m_frame;
  // the noise layers added (each magnified to the size; the 128 x 128 one averaged down), the
  // 8-bit target clamping at 1
  float* acc = m_slime_acc.data();
  std::fill(acc, acc + kN * kN, 0.f);
  for (int i = 0; i < 4; i++) {
    const auto& n = m_slime_noise[i];
    const float interp = n.last_time / kMaxTimes[i];
    const float wn = interp * kScales[i] / 255.f, wo = (1.f - interp) * kScales[i] / 255.f;
    for (int y = 0; y < kN; y++) {
      for (int x = 0; x < kN; x++) {
        float v;
        if (n.dim >= kN) {
          const int f = n.dim / kN;
          v = 0;
          for (int j = 0; j < f; j++) {
            for (int k = 0; k < f; k++) {
              const size_t q = (size_t)(y * f + j) * n.dim + x * f + k;
              v += wn * n.new_tex[q] + wo * n.old_tex[q];
            }
          }
          v /= (float)(f * f);
        } else {
          // GL_LINEAR magnification, clamped at the edges
          const float fx = (x + 0.5f) * n.dim / kN - 0.5f, fy = (y + 0.5f) * n.dim / kN - 0.5f;
          const int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
          const float ax = fx - x0, ay = fy - y0;
          auto at = [&](int xx, int yy) {
            xx = std::clamp(xx, 0, n.dim - 1);
            yy = std::clamp(yy, 0, n.dim - 1);
            const size_t q = (size_t)yy * n.dim + xx;
            return wn * n.new_tex[q] + wo * n.old_tex[q];
          };
          v = (at(x0, y0) * (1 - ax) + at(x0 + 1, y0) * ax) * (1 - ay) +
              (at(x0, y0 + 1) * (1 - ax) + at(x0 + 1, y0 + 1) * ax) * ay;
        }
        acc[y * kN + x] += v;
      }
    }
  }
  // the LUT (texelFetch at the 8-bit value), and the scrolled copy (v + times[8] / 1200, nearest,
  // repeat)
  const int scroll = (int)std::floor(in.times[8] / 1200.f * kN);
  for (int y = 0; y < kN; y++) {
    for (int x = 0; x < kN; x++) {
      const int i = std::clamp((int)(std::min(acc[y * kN + x], 1.f) * 255.f), 0, 255);
      u32 c = 0;
      for (int k = 0; k < 4; k++) {
        c |= (u32)std::lround(kSlimeLutData[4 * i + k] * 255.f) << (8 * k);
      }
      o1->rgba[(size_t)y * kN + x] = c;
      const int ys = (((y - scroll) % kN) + kN) % kN;
      o2->rgba[(size_t)ys * kN + x] = c;
    }
  }
  if (slot >= 0) {
    o1->upload(slot);
  }
  if (slot2 >= 0) {
    o2->upload(slot2);
  }
  m_slot_updates++;
  m_slot_ms += ctr_gpu_time_ms() - t0;
}

void CtrTexAnimJak2::frame_done() {
  m_snap_valid = false;
  m_frame++;
  m_fixed_this_frame = 0;
  // idle animations give back their GPU texture and texels; unused sources their data
  if ((m_frame & 63) == 0) {
    for (auto& [name, o] : m_outs) {
      if (o.tex >= 0 && m_frame - o.seen_frame > (u64)kIdleFrames) {
        o.release();
      }
    }
    for (auto& s : m_srcs) {
      if (!s.data.empty() && m_frame - s.used_frame > (u64)kIdleFrames) {
        std::vector<u8>().swap(s.data);
      }
    }
  }
}

void CtrTexAnimJak2::log_stats() {
  if (m_runs) {
    lg::info("[ctr] jak2 texture animation: clouds {:.2f} ms per update, {} updates", m_ms / m_runs,
             m_runs);
    m_ms = 0;
    m_runs = 0;
  }
  if (m_slot_updates || m_slot_ms > 0.05) {
    size_t gpu = 0, src_bytes = 0;
    int live = 0;
    for (const auto& [name, o] : m_outs) {
      if (o.tex >= 0) {
        live++;
        gpu += (size_t)o.w * o.h * 4;
      }
    }
    for (const auto& s : m_srcs) {
      src_bytes += s.data.size();
    }
    std::string names;
    for (const auto& [n, c] : m_slot_names) {
      names += fmt::format(" {} {}", n, c);
    }
    m_slot_names.clear();
    lg::info("[ctr] jak2 texture animation: slots {:.2f} ms, {} updates, {} textures ({} KB GPU, "
             "x2 while updated), sources {} KB; updates:{}",
             m_slot_ms, m_slot_updates, live, gpu / 1024, src_bytes / 1024, names);
    m_slot_ms = 0;
    m_slot_updates = 0;
  }
}
