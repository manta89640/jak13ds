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

#include "game/graphics/ctr/CtrVram.h"

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
  m_acc.resize(kCloudSize * kCloudSize);
  m_acc2.resize(kCloudSize * kCloudSize);
  m_tmp.resize(kCloudSize * kCloudSize / 2);  // upsample2's row pass: 2n x n
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
  // the clouds move slowly: every other frame, unless a layer got new noise
  if ((m_frames++ & 1) && !changed && vram->has_override((u32)in.cloud_dest)) {
    return;
  }
  // the layers are added coarse to fine (each 2x larger one is the sum so far magnified, plus
  // its own): the same sum as magnifying each layer to 128 x 128 on its own, but for the
  // bilinear filter of the 4x and 8x steps. The 8-bit frame buffer clamps every add at 1; the
  // adds are all positive, so one clamp at the end gives the same.
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
    upsample2(acc, dim, m_tmp.data(), next);
    std::swap(acc, next);
    dim *= 2;
    for (int p = 0; p < dim * dim; p++) {
      acc[p] += wn * n.new_tex[p] + wo * n.old_tex[p];
    }
  }
  // tex_anim.frag, enable_tex 2: rgb 0.5, alpha 0.5 * cloud_lookup of the 8-bit sum (0x80 = 1 in
  // the PS2's texture scale, like the PC's texture); stored at half the size (64 x 64, the
  // average of each 2 x 2: 16 KB of linear memory, and the sky is a few hundred pixels wide)
  const int half = dim / 2;
  for (int y = 0; y < half; y++) {
    for (int x = 0; x < half; x++) {
      float a = 0;
      for (int k = 0; k < 4; k++) {
        const float v =
            std::round(std::min(acc[(2 * y + (k >> 1)) * dim + 2 * x + (k & 1)], 1.f) * 255.f) *
            (1.f / 255.f);
        a += cloud_lookup(v, in.cloud_min, in.cloud_max);
      }
      m_out[y * half + x] = 0x808080u | ((u32)std::lround(a * (127.5f / 4.f)) << 24);
    }
  }
  vram->set_override((u32)in.cloud_dest, half, half, m_out.data());
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
