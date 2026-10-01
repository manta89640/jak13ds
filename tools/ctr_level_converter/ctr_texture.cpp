/*!
 * @file ctr_texture.cpp
 * (AI-assisted)
 * See ctr_texture.h.
 */

#include "ctr_texture.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>

#include "third-party/rg_etc1/rg_etc1.h"

namespace ctr_tex {

namespace {

uint32_t morton8(uint32_t x, uint32_t y) {
  return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) |
         ((y & 4) << 3);
}

// texel index of (x, memory row ym) in a tiled 16/32 bit level
uint32_t tiled_texel(int x, int ym, int w) {
  return (uint32_t)(((ym / 8) * (w / 8) + (x / 8)) * 64) + morton8(x & 7, ym & 7);
}

uint32_t to_bits(uint32_t v, uint32_t max) {
  return (v * max + 127) / 255;
}

// like the GPU: bit replication
uint8_t from_bits(uint32_t v, uint32_t bits) {
  return (uint8_t)((v << (8 - bits)) | (v >> (2 * bits - 8)));
}

// 4-bit alpha, rounded to nearest but on the same side of an alpha test reference as the 8-bit value
uint32_t alpha4(uint32_t a, int ref) {
  uint32_t q = to_bits(a, 15);
  if (ref > 0) {
    const int e = from_bits(q, 4);
    if ((int)a >= ref && e < ref && q < 15) {
      q++;
    } else if ((int)a < ref && e >= ref && q > 0) {
      q--;
    }
  }
  return q;
}

void etc1_init_once() {
  static std::once_flag flag;
  std::call_once(flag, [] { rg_etc1::pack_etc1_block_init(); });
}

double coverage(const Image& img, int ref, double scale) {
  if (img.w == 0 || img.h == 0) {
    return 0;
  }
  size_t pass = 0;
  const size_t n = (size_t)img.w * img.h;
  for (size_t i = 0; i < n; i++) {
    if (std::min(255.0, img.rgba[4 * i + 3] * scale) >= ref) {
      pass++;
    }
  }
  return (double)pass / n;
}

// scale alpha so that the fraction of texels with alpha >= ref is `target` (Castano)
void scale_alpha_to_coverage(Image* img, double target, int ref) {
  if (target <= 0.0 || target >= 1.0) {
    return;
  }
  double lo = 0.0, hi = 8.0;
  for (int i = 0; i < 16; i++) {
    const double mid = 0.5 * (lo + hi);
    if (coverage(*img, ref, mid) < target) {
      lo = mid;
    } else {
      hi = mid;
    }
  }
  const double s = 0.5 * (lo + hi);
  const size_t n = (size_t)img->w * img->h;
  for (size_t i = 0; i < n; i++) {
    uint8_t& a = img->rgba[4 * i + 3];
    a = (uint8_t)std::clamp((int)std::lround(a * s), 0, 255);
  }
}

}  // namespace

uint32_t level_bytes(int w, int h, Format fmt) {
  const uint32_t n = (uint32_t)w * (uint32_t)h;
  switch (fmt) {
    case Format::RGB565:
    case Format::RGBA4:
      return n * 2;
    case Format::ETC1:
      return n / 2;
    case Format::ETC1A4:
      return n;
  }
  return 0;
}

int level_count(int w, int h, int max_levels) {
  int n = 1;
  int s = std::min(w, h);
  while (s > 8 && n < max_levels) {
    s >>= 1;
    n++;
  }
  return n;
}

std::vector<Image> make_mips(const Image& base, int levels, const MipOptions& opt) {
  std::vector<Image> out;
  out.reserve(levels);
  out.push_back(base);
  const double base_cov = opt.preserve_coverage ? coverage(base, opt.alpha_ref, 1.0) : 0.0;
  for (int l = 1; l < levels; l++) {
    const Image& src = out.back();
    if (src.w < 16 || src.h < 16) {
      break;
    }
    Image dst;
    dst.w = src.w / 2;
    dst.h = src.h / 2;
    dst.rgba.resize((size_t)dst.w * dst.h * 4);
    for (int y = 0; y < dst.h; y++) {
      for (int x = 0; x < dst.w; x++) {
        double c[3] = {0, 0, 0}, wsum = 0, asum = 0;
        for (int k = 0; k < 4; k++) {
          const uint8_t* p = src.at(2 * x + (k & 1), 2 * y + (k >> 1));
          const double wgt = p[3] / 255.0 + 1.0 / 1024.0;
          for (int ch = 0; ch < 3; ch++) {
            c[ch] += p[ch] * wgt;
          }
          wsum += wgt;
          asum += p[3];
        }
        uint8_t* d = dst.at(x, y);
        for (int ch = 0; ch < 3; ch++) {
          d[ch] = (uint8_t)std::clamp((int)std::lround(c[ch] / wsum), 0, 255);
        }
        d[3] = (uint8_t)std::clamp((int)std::lround(asum / 4.0), 0, 255);
      }
    }
    if (opt.preserve_coverage) {
      scale_alpha_to_coverage(&dst, base_cov, opt.alpha_ref);
    }
    out.push_back(std::move(dst));
  }
  return out;
}

void fill_transparent_colors(Image* img) {
  for (int by = 0; by + 4 <= img->h; by += 4) {
    for (int bx = 0; bx + 4 <= img->w; bx += 4) {
      double c[3] = {0, 0, 0}, wsum = 0;
      bool any_hidden = false;
      for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
          const uint8_t* p = img->at(bx + x, by + y);
          if (p[3] < 16) {
            any_hidden = true;
            continue;
          }
          for (int ch = 0; ch < 3; ch++) {
            c[ch] += p[ch] * (double)p[3];
          }
          wsum += p[3];
        }
      }
      if (!any_hidden || wsum <= 0) {
        continue;
      }
      for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
          uint8_t* p = img->at(bx + x, by + y);
          if (p[3] < 16) {
            for (int ch = 0; ch < 3; ch++) {
              p[ch] = (uint8_t)std::clamp((int)std::lround(c[ch] / wsum), 0, 255);
            }
          }
        }
      }
    }
  }
}

void encode_level(const Image& img,
                  Format fmt,
                  int etc1_quality,
                  std::vector<uint8_t>* out,
                  int alpha_ref) {
  const int w = img.w, h = img.h;
  const size_t start = out->size();
  out->resize(start + level_bytes(w, h, fmt));
  uint8_t* dst = out->data() + start;
  switch (fmt) {
    case Format::RGB565:
    case Format::RGBA4:
      for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
          const uint8_t* p = img.at(x, y);
          uint16_t v;
          if (fmt == Format::RGBA4) {
            v = (uint16_t)((to_bits(p[0], 15) << 12) | (to_bits(p[1], 15) << 8) |
                           (to_bits(p[2], 15) << 4) | alpha4(p[3], alpha_ref));
          } else {
            v = (uint16_t)((to_bits(p[0], 31) << 11) | (to_bits(p[1], 63) << 5) |
                           to_bits(p[2], 31));
          }
          const uint32_t idx = tiled_texel(x, h - 1 - y, w);
          dst[2 * idx] = (uint8_t)(v & 0xff);
          dst[2 * idx + 1] = (uint8_t)(v >> 8);
        }
      }
      break;
    case Format::ETC1:
    case Format::ETC1A4: {
      etc1_init_once();
      rg_etc1::etc1_pack_params params;
      params.m_quality = etc1_quality <= 0   ? rg_etc1::cLowQuality
                         : etc1_quality == 1 ? rg_etc1::cMediumQuality
                                             : rg_etc1::cHighQuality;
      params.m_dithering = false;
      const bool alpha = fmt == Format::ETC1A4;
      uint8_t* o = dst;
      for (int ty = 0; ty < h / 8; ty++) {
        for (int tx = 0; tx < w / 8; tx++) {
          for (int sub = 0; sub < 4; sub++) {
            const int bx = tx * 8 + (sub & 1) * 4;
            const int bym = ty * 8 + (sub >> 1) * 4;  // memory rows (bottom of the image first)
            unsigned int px[16];
            uint64_t alpha_bits = 0;
            for (int y = 0; y < 4; y++) {
              for (int x = 0; x < 4; x++) {
                const uint8_t* p = img.at(bx + x, h - 1 - (bym + y));
                // rg_etc1 wants the bytes R, G, B, A in memory
                uint8_t b[4] = {p[0], p[1], p[2], 255};
                memcpy(&px[y * 4 + x], b, 4);
                alpha_bits |= (uint64_t)alpha4(p[3], alpha_ref) << (4 * (4 * x + y));
              }
            }
            uint8_t blk[8];
            rg_etc1::pack_etc1_block(blk, px, params);
            if (alpha) {
              for (int i = 0; i < 8; i++) {
                o[i] = (uint8_t)(alpha_bits >> (8 * i));
              }
              o += 8;
            }
            // rg_etc1 writes the standard big-endian block; the GPU reads a little-endian u64
            for (int i = 0; i < 8; i++) {
              o[i] = blk[7 - i];
            }
            o += 8;
          }
        }
      }
    } break;
  }
}

Image decode_level(const uint8_t* data, int w, int h, Format fmt) {
  Image img;
  img.w = w;
  img.h = h;
  img.rgba.assign((size_t)w * h * 4, 0);
  switch (fmt) {
    case Format::RGB565:
    case Format::RGBA4:
      for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
          const uint32_t idx = tiled_texel(x, h - 1 - y, w);
          const uint16_t v = (uint16_t)(data[2 * idx] | (data[2 * idx + 1] << 8));
          uint8_t* p = img.at(x, y);
          if (fmt == Format::RGBA4) {
            p[0] = from_bits((v >> 12) & 0xf, 4);
            p[1] = from_bits((v >> 8) & 0xf, 4);
            p[2] = from_bits((v >> 4) & 0xf, 4);
            p[3] = from_bits(v & 0xf, 4);
          } else {
            p[0] = from_bits((v >> 11) & 0x1f, 5);
            p[1] = from_bits((v >> 5) & 0x3f, 6);
            p[2] = from_bits(v & 0x1f, 5);
            p[3] = 255;
          }
        }
      }
      break;
    case Format::ETC1:
    case Format::ETC1A4: {
      const bool alpha = fmt == Format::ETC1A4;
      const uint8_t* o = data;
      for (int ty = 0; ty < h / 8; ty++) {
        for (int tx = 0; tx < w / 8; tx++) {
          for (int sub = 0; sub < 4; sub++) {
            const int bx = tx * 8 + (sub & 1) * 4;
            const int bym = ty * 8 + (sub >> 1) * 4;
            uint64_t alpha_bits = ~0ull;
            if (alpha) {
              alpha_bits = 0;
              for (int i = 0; i < 8; i++) {
                alpha_bits |= (uint64_t)o[i] << (8 * i);
              }
              o += 8;
            }
            uint8_t blk[8];
            for (int i = 0; i < 8; i++) {
              blk[i] = o[7 - i];
            }
            o += 8;
            unsigned int px[16];
            rg_etc1::unpack_etc1_block(blk, px, false);
            for (int y = 0; y < 4; y++) {
              for (int x = 0; x < 4; x++) {
                uint8_t* p = img.at(bx + x, h - 1 - (bym + y));
                memcpy(p, &px[y * 4 + x], 3);
                p[3] = from_bits((uint32_t)(alpha_bits >> (4 * (4 * x + y))) & 0xf, 4);
              }
            }
          }
        }
      }
    } break;
  }
  return img;
}

}  // namespace ctr_tex
