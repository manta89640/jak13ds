#pragma once

/*!
 * @file ctr_texture.h
 * (AI-assisted)
 * Texture encoding for the 3DS GPU (PICA200), used by ctr_level_converter: mip chains and the
 * tiled layouts the GPU samples (RGB565, RGBA4, ETC1, ETC1A4). See docs/3ds-port/c3l_format.md.
 *
 * Layout (all formats): the image is stored bottom row first, in 8x8 tiles (rows of tiles), texels
 * in Morton order inside a tile. ETC1 tiles are four 4x4 blocks (top left, top right, bottom left,
 * bottom right in memory order), each block a little-endian u64 of the standard (big-endian) ETC1
 * block; ETC1A4 puts 4-bit alpha (u64, texel (x, y) of the block at bit 4 * (4 * x + y)) before
 * each block. Mip levels follow each other, down to 8 texels on the short side.
 */

#include <cstdint>
#include <vector>

namespace ctr_tex {

/*! RGBA8, row-major, top row first. Alpha 255 = opaque. */
struct Image {
  int w = 0, h = 0;
  std::vector<uint8_t> rgba;
  uint8_t* at(int x, int y) { return &rgba[4 * (x + y * w)]; }
  const uint8_t* at(int x, int y) const { return &rgba[4 * (x + y * w)]; }
};

enum class Format : uint8_t { RGB565 = 0, RGBA4 = 1, ETC1 = 2, ETC1A4 = 3, RGBA8 = 4 };

/*! Bytes of one level. */
uint32_t level_bytes(int w, int h, Format fmt);

/*! Number of levels down to 8 texels on the short side (the GPU's minimum), capped by max_levels. */
int level_count(int w, int h, int max_levels);

struct MipOptions {
  // Keep the fraction of texels that pass an alpha test (alpha >= alpha_ref, 0..255) the same in
  // every level, so cut-out textures (foliage, fences) don't fade away or fatten with distance.
  bool preserve_coverage = false;
  int alpha_ref = 76;
};

/*!
 * Mip chain of a power of two image: [0] = the image itself. Colors are averaged weighted by
 * alpha (transparent texels don't darken the edges of cut-outs).
 */
std::vector<Image> make_mips(const Image& base, int levels, const MipOptions& opt);

/*!
 * Transparent texels (alpha < 16) get the average color of the visible texels of their 4x4 block:
 * bilinear filtering blends a little of them into the edges, and ETC1 spends no precision on them.
 */
void fill_transparent_colors(Image* img);

/*!
 * quality: 0 low, 1 medium, 2 high (rg_etc1). alpha_ref (1..255, 0: none): the alpha test reference
 * of the draws using the texture; 4-bit alpha is rounded so that every texel passes or fails the
 * test like its 8-bit alpha does (cut-out edges don't move).
 */
void encode_level(const Image& img,
                  Format fmt,
                  int etc1_quality,
                  std::vector<uint8_t>* out,
                  int alpha_ref = 0);

/*! Decoders (for tests and previews): one level in the layout above -> Image. */
Image decode_level(const uint8_t* data, int w, int h, Format fmt);

}  // namespace ctr_tex
