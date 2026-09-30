/*!
 * @file CtrEye.cpp
 * (AI-assisted)
 * See CtrEye.h. The DMA follows the PC port's EyeRenderer::get_draws (Jak 1), without asserts:
 * anything unexpected ends the bucket and keeps the eyes of the last frame.
 */

#include "CtrEye.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "common/dma/dma.h"
#include "common/dma/gs.h"
#include "common/log/log.h"
#ifndef __3DS__
#include "common/util/FileUtil.h"
#include "fmt/format.h"
#endif

#include "game/graphics/ctr/CtrVram.h"
#include "game/graphics/ctr/ctr_gpu.h"

namespace {

// an adgif transfer: gif tag + 5 A+D register writes (tex0, tex1, mip, clamp, alpha)
bool adgif_tex0(const DmaTransfer& t, u64* tex0) {
  if (t.size_bytes != 96) {
    return false;
  }
  for (int i = 0; i < 5; i++) {
    const u8* ad = t.data + 16 + 16 * i;
    if (ad[8] == (u8)GsRegisterAddress::TEX0_1) {
      memcpy(tex0, ad, 8);
      return true;
    }
  }
  return false;
}

}  // namespace

CtrEyeRenderer::CtrEyeRenderer(std::string name, int id, CtrVram* vram)
    : CtrBucketRenderer(std::move(name), id), m_vram(vram) {
  for (int i = 0; i < kSlots; i++) {
    m_tex[i] = -1;
    m_hash[i] = 0;
    m_key[i] = 0;
    m_age[i] = 0;
  }
}

CtrEyeRenderer::~CtrEyeRenderer() {
  for (int t : m_tex) {
    if (t >= 0) {
      ctr_gpu_tex_delete(t);
    }
  }
}

int CtrEyeRenderer::texture(int eye_id) const {
  if (eye_id < 0) {
    return -1;
  }
  return m_tex[eye_id % kSlots];
}

void CtrEyeRenderer::render(DmaFollower& dma, CtrRenderState& rs) {
  auto skip_rest = [&]() {
    while (dma.current_tag_offset() != rs.next_bucket) {
      dma.read_and_advance();
    }
  };
  if (dma.current_tag_offset() == rs.next_bucket) {
    return;
  }
  dma.read_and_advance();  // jump to the bucket's data
  if (dma.current_tag_offset() != rs.next_bucket &&
      dma.current_tag().kind != DmaTag::Kind::CALL) {  // (CALL: no eyes this frame)
    handle_eye_dma(dma, rs);
  }
  skip_rest();
  if (rs.log_now && (m_stats.eyes || m_stats.bad_dma)) {
    lg::debug("[ctr] eyes: {} drawn, {} texture uploads, {} bad dma", m_stats.eyes,
              m_stats.uploads, m_stats.bad_dma);
    m_stats = Stats();
  }
}

void CtrEyeRenderer::handle_eye_dma(DmaFollower& dma, CtrRenderState& rs) {
  auto more = [&]() { return dma.current_tag_offset() != rs.next_bucket; };
  // GS setup for drawing to the eye texture area (128 bytes), alpha setup (32 bytes)
  if (!more() || dma.current_tag().qwc != 8) {
    return;
  }
  dma.read_and_advance();
  if (!more() || dma.current_tag().qwc != 2) {
    m_stats.bad_dma++;
    return;
  }
  dma.read_and_advance();
  // jak 1: the empty transfer of the add to bucket
  if (more() && dma.current_tag().qwc == 0) {
    dma.read_and_advance();
  }

  std::vector<Eye> eyes;
  // pairs of eyes until the 8 qw transfer that restores the GS
  while (more() && dma.current_tag().qwc != 8) {
    if (!read_pair(dma, rs, &eyes)) {
      m_stats.bad_dma++;
      break;
    }
  }

  // the textures of this bucket's level (the pris buckets upload a level's page just before)
  m_sources.clear();
  for (const auto& e : eyes) {
    composite(e);
  }
  m_stats.eyes += (int)eyes.size();
}

bool CtrEyeRenderer::read_pair(DmaFollower& dma, CtrRenderState& rs, std::vector<Eye>* eyes) {
  auto more = [&]() { return dma.current_tag_offset() != rs.next_bucket; };
  // scissor (32 bytes) then the sprite (96 bytes: gif tag, rgbaq, uv, xyz2, uv, xyz2)
  auto read_draw = [&](Sprite* s, int* scissor_h) {
    if (!more()) {
      return false;
    }
    auto sc = dma.read_and_advance();
    if (sc.size_bytes != 32 || !more()) {
      return false;
    }
    u64 reg;
    memcpy(&reg, sc.data + 16, 8);
    if (scissor_h) {
      GsScissor scissor(reg);
      *scissor_h = (int)scissor.y1() - (int)scissor.y0();
    }
    auto sp = dma.read_and_advance();
    if (sp.size_bytes != 96) {
      return false;
    }
    memcpy(s->xyz0, sp.data + 48, 8);
    memcpy(s->xyz1, sp.data + 80, 8);
    s->valid = true;
    return true;
  };
  // a texture change for the right eye: an adgif (6 qw) instead of the next scissor (2 qw)
  auto right_tex = [&](u64* tex0) {
    if (more() && dma.current_tag().qwc == 6) {
      return adgif_tex0(dma.read_and_advance(), tex0);
    }
    return true;
  };

  Eye l, r;
  l.lr = 0;
  r.lr = 1;

  // iris texture, then the background: the whole eye in the iris texture's first texel. Its
  // position tells which pair of eyes this is.
  u64 tex0;
  if (!more() || !adgif_tex0(dma.read_and_advance(), &tex0)) {
    return false;
  }
  l.iris_tex0 = r.iris_tex0 = tex0;
  Sprite bg;
  int scissor_h = 0;
  if (!read_draw(&bg, &scissor_h)) {
    return false;
  }
  const bool using_64 = scissor_h == 63;
  u32 y0 = (bg.xyz0[1] - 512) >> 4;
  if (using_64) {
    y0 = ((bg.xyz0[1] - 1024) >> 5) * 4;
  }
  l.pair = r.pair = (int)(y0 / kSize);
  l.using_64 = r.using_64 = using_64;

  // iris
  if (!read_draw(&l.iris, nullptr) || !right_tex(&r.iris_tex0) || !read_draw(&r.iris, nullptr)) {
    return false;
  }
  // pupil (after a test register write)
  if (!more()) {
    return false;
  }
  dma.read_and_advance();
  if (!more() || !adgif_tex0(dma.read_and_advance(), &tex0)) {
    return false;
  }
  l.pupil_tex0 = r.pupil_tex0 = tex0;
  if (!read_draw(&l.pupil, nullptr) || !right_tex(&r.pupil_tex0) ||
      !read_draw(&r.pupil, nullptr)) {
    return false;
  }
  // eyelid
  if (!more()) {
    return false;
  }
  dma.read_and_advance();
  if (!more() || !adgif_tex0(dma.read_and_advance(), &tex0)) {
    return false;
  }
  l.lid_tex0 = r.lid_tex0 = tex0;
  if (!read_draw(&l.lid, nullptr) || !right_tex(&r.lid_tex0) || !read_draw(&r.lid, nullptr)) {
    return false;
  }
  // jak 1: an empty transfer ends the pair
  if (more() && dma.current_tag().qwc == 0) {
    dma.read_and_advance();
  }
  eyes->push_back(l);
  eyes->push_back(r);
  return true;
}

const CtrEyeRenderer::Source* CtrEyeRenderer::source(u64 tex0) {
  for (const auto& s : m_sources) {
    if (s.tex0 == tex0) {
      return s.ok ? &s : nullptr;
    }
  }
  Source s;
  s.tex0 = tex0;
  s.ok = m_vram && m_vram->decode_for_cpu(tex0, &s.rgba, &s.w, &s.h) && s.w > 0 && s.h > 0 &&
         (int)s.rgba.size() >= s.w * s.h;
  m_sources.push_back(std::move(s));
  return m_sources.back().ok ? &m_sources.back() : nullptr;
}

namespace {
// GS alpha (0x80 = 1.0) -> 0..255
inline u32 alpha255(u32 c) {
  return std::min(255u, ((c >> 24) & 0xff) * 2);
}
}  // namespace

void CtrEyeRenderer::draw_sprite(const Eye& e,
                                 const Sprite& s,
                                 const Source* src,
                                 bool blend,
                                 bool keep_alpha) {
  if (!s.valid || !src) {
    return;
  }
  // sprite corners -> pixels of this eye's texture, as EyeRenderer::add_draw_to_buffer_32 / _64
  // and eye.vert map them to the eye's framebuffer
  auto to_px = [&](u32 v, int axis) {
    float c;
    if (e.using_64) {
      const float off = axis == 0 ? e.lr * kSize * 32.f : (e.pair / 4) * kSize * 32.f;
      c = ((float)v - off) / 2.f;
    } else {
      const float off = axis == 0 ? e.lr * kSize * 16.f : e.pair * kSize * 16.f;
      c = (float)v - off;
    }
    return (c - 512.f) / 16.f;
  };
  const float x0 = to_px(s.xyz0[0], 0), x1 = to_px(s.xyz1[0], 0);
  const float y0 = to_px(s.xyz0[1], 1), y1 = to_px(s.xyz1[1], 1);
  if (x0 == x1 || y0 == y1) {
    return;
  }
  const int px0 = std::max(0, (int)std::floor(std::min(x0, x1)));
  const int px1 = std::min(kSize, (int)std::ceil(std::max(x0, x1)));
  const int py0 = std::max(0, (int)std::floor(std::min(y0, y1)));
  const int py1 = std::min(kSize, (int)std::ceil(std::max(y0, y1)));
  for (int py = py0; py < py1; py++) {
    // the texture is stretched over the sprite: (0, 0) at xyz0, (1, 1) at xyz1
    const float t = ((float)py + 0.5f - y0) / (y1 - y0);
    if (t < 0.f || t >= 1.f) {
      continue;
    }
    const int ty = std::clamp((int)(t * src->h), 0, src->h - 1);
    for (int px = px0; px < px1; px++) {
      const float u = ((float)px + 0.5f - x0) / (x1 - x0);
      if (u < 0.f || u >= 1.f) {
        continue;
      }
      const int tx = std::clamp((int)(u * src->w), 0, src->w - 1);
      const u32 c = src->rgba[ty * src->w + tx];
      const u32 a = alpha255(c);
      u32& d = m_pixels[py * kSize + px];
      if (!blend) {
        // the lid: the game draws it with alpha test NEVER and AFAIL RGB only, so only the color
        // changes and the eye keeps the alpha of the iris / background
        d = keep_alpha ? ((c & 0xffffffu) | (d & 0xff000000u)) : ((c & 0xffffffu) | (a << 24));
      } else {
        // src alpha, 1 - src alpha (the pupil over the iris), alpha kept (as above)
        u32 out = d & 0xff000000u;
        for (int ch = 0; ch < 24; ch += 8) {
          const u32 sc = (c >> ch) & 0xff, dc = (d >> ch) & 0xff;
          out |= ((sc * a + dc * (255 - a)) / 255) << ch;
        }
        d = out;
      }
    }
  }
}

void CtrEyeRenderer::composite(const Eye& e) {
  const int slot = e.pair * 2 + e.lr;
  if (slot < 0 || slot >= kSlots) {
    return;
  }
  // Same sprites and textures as last time: the eye didn't move (most frames, between blinks and
  // glances). Redone every 30 frames anyway, in case the texture data changed under it.
  u64 key = 1469598103934665603ull;
  auto mix = [&](const void* p, size_t n) {
    const u8* b = (const u8*)p;
    for (size_t i = 0; i < n; i++) {
      key = (key ^ b[i]) * 1099511628211ull;
    }
  };
  const Sprite* sprites[3] = {&e.iris, &e.pupil, &e.lid};
  for (const Sprite* sp : sprites) {
    mix(sp->xyz0, sizeof(sp->xyz0));
    mix(sp->xyz1, sizeof(sp->xyz1));
    mix(&sp->valid, sizeof(sp->valid));
  }
  mix(&e.iris_tex0, 8);
  mix(&e.pupil_tex0, 8);
  mix(&e.lid_tex0, 8);
  mix(&e.using_64, sizeof(e.using_64));
  if (m_tex[slot] >= 0 && m_key[slot] == key && ++m_age[slot] < 30) {
    return;
  }
  m_key[slot] = key;
  m_age[slot] = 0;
  const Source* iris = source(e.iris_tex0);
  const Source* pupil = source(e.pupil_tex0);
  const Source* lid = source(e.lid_tex0);
  // background: the iris texture's first texel everywhere
  const u32 bg = iris ? ((iris->rgba[0] & 0xffffffu) | (alpha255(iris->rgba[0]) << 24))
                      : 0xff000000u;
  m_pixels.assign(kSize * kSize, bg);
  draw_sprite(e, e.iris, iris, false, false);
  draw_sprite(e, e.pupil, pupil, true, true);
  draw_sprite(e, e.lid, lid, false, true);

  // upload when it changed (eyes are still most of the time)
  u64 h = 1469598103934665603ull;
  for (u32 p : m_pixels) {
    h = (h ^ p) * 1099511628211ull;
  }
  if (m_tex[slot] >= 0 && m_hash[slot] == h) {
    return;
  }
#ifndef __3DS__
  // debugging on PC: OPENGOAL_EYE_DUMP=<dir> writes the eye textures (and their sources)
  static const char* dump_dir = getenv("OPENGOAL_EYE_DUMP");
  static int dumps = 0;
  if (dump_dir && dumps < 40) {
    dumps++;
    file_util::write_rgba_png(fs::path(dump_dir) / fmt::format("eye_{:02d}_slot{}.png", dumps, slot),
                              m_pixels.data(), kSize, kSize);
    const Source* srcs[3] = {iris, pupil, lid};
    const char* names[3] = {"iris", "pupil", "lid"};
    for (int i = 0; i < 3; i++) {
      if (srcs[i]) {
        lg::info("[eye dump {}] {} tex0 {:x}: {}x{}, {} texels", dumps, names[i], srcs[i]->tex0,
                 srcs[i]->w, srcs[i]->h, srcs[i]->rgba.size());
        try {
          file_util::write_rgba_png(
            fs::path(dump_dir) / fmt::format("eye_{:02d}_{}.png", dumps, names[i]),
            (void*)srcs[i]->rgba.data(), srcs[i]->w, srcs[i]->h);
        } catch (std::exception&) {
        }
      }
    }
    lg::info("[eye dump {}] slot {} pair {} lr {} 64:{} iris {} {},{}-{},{} pupil {},{}-{},{} lid "
             "{},{}-{},{}",
             dumps, slot, e.pair, e.lr, e.using_64, e.iris.valid, e.iris.xyz0[0], e.iris.xyz0[1],
             e.iris.xyz1[0], e.iris.xyz1[1], e.pupil.xyz0[0], e.pupil.xyz0[1], e.pupil.xyz1[0],
             e.pupil.xyz1[1], e.lid.xyz0[0], e.lid.xyz0[1], e.lid.xyz1[0], e.lid.xyz1[1]);
  }
#endif
  const int tex = ctr_gpu_tex_create(kSize, kSize, (const uint8_t*)m_pixels.data());
  if (tex < 0) {
    return;
  }
  if (m_tex[slot] >= 0) {
    ctr_gpu_tex_delete(m_tex[slot]);  // freed after this frame: earlier buckets may use it
  }
  m_tex[slot] = tex;
  m_hash[slot] = h;
  m_stats.uploads++;
}
