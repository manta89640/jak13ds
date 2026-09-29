#pragma once

/*!
 * @file c3l_format.h
 * (AI-assisted)
 * "C3L": compact level background format for the 3DS renderer, written on PC by
 * ctr_level_converter (tools/ctr_level_converter) from the PC port's .fr3 files, and read by
 * game/graphics/ctr/CtrLevel.cpp. See docs/3ds-port/c3l_format.md.
 *
 * Little endian, every section 16-byte aligned, offsets are from the start of the file.
 */

#include <cstdint>

namespace c3l {

constexpr char kMagic[4] = {'C', '3', 'L', '1'};
constexpr uint32_t kVersion = 1;

enum TextureFormat : uint8_t {
  TEX_RGB565 = 0,  // u16: r5 g6 b5 (r in the high bits)
  TEX_RGBA4 = 1,   // u16: r4 g4 b4 a4 (r in the high bits)
};

struct Header {
  char magic[4];
  uint32_t version;
  char level_name[32];
  uint32_t num_textures;
  uint32_t textures_offset;  // Texture[num_textures]
  uint32_t num_chunks;
  uint32_t chunks_offset;  // Chunk[num_chunks]
  uint32_t vertex_data_offset;  // all vertices (Vertex)
  uint32_t vertex_data_size;
  uint32_t index_data_offset;  // all indices (u16, relative to the chunk's first vertex)
  uint32_t index_data_size;
  uint32_t draw_data_offset;  // all draws (Draw)
  uint32_t draw_data_size;
  uint32_t texture_data_offset;  // all texture texels
  uint32_t texture_data_size;
  uint32_t pad[2];
};
static_assert(sizeof(Header) == 96);

/*!
 * Texture, already in the 3DS GPU layout: 8x8 tiles (rows of tiles), Morton order inside a tile,
 * bottom row of the image first. Power of two, 8..128.
 */
struct Texture {
  uint16_t w, h;
  uint8_t format;  // TextureFormat
  uint8_t pad[3];
  uint32_t data_offset;  // absolute
  uint32_t data_size;
};
static_assert(sizeof(Texture) == 16);

/*!
 * A piece of the level: triangle lists sharing one quantized vertex range.
 * world position = origin + pos * scale (game units, 4096 = 1 meter)
 */
struct Chunk {
  float bsphere[4];  // x, y, z, radius (world)
  float origin[3];
  float scale;
  uint32_t first_vertex;  // index into the vertex data
  uint32_t vertex_count;  // <= 65536
  uint32_t first_draw;    // index into the draw data
  uint32_t draw_count;
};
static_assert(sizeof(Chunk) == 48);

struct Vertex {
  int16_t pos[3];  // quantized, see Chunk
  int16_t pad;
  int16_t st[2];      // texture coordinates * 1024
  uint8_t rgba[4];    // baked time of day color, GS units (0x80 = 1.0)
};
static_assert(sizeof(Vertex) == 16);

struct Draw {
  uint32_t mode;        // tfrag3 DrawMode bits (common/dma/gs.h)
  uint16_t texture;     // index into the textures, 0xffff = untextured
  uint16_t kind;        // 0 = tfrag, 1 = tie, 2 = shrub
  uint32_t first_index;  // index into the index data (u16), triangle list
  uint32_t index_count;
};
static_assert(sizeof(Draw) == 16);

inline uint32_t morton8(uint32_t x, uint32_t y) {
  return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) |
         ((y & 4) << 3);
}

/*! Texel index in the 3DS tiled layout for image pixel (x, y), y = 0 at the top. */
inline uint32_t tiled_index(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
  uint32_t ty = h - 1 - y;
  return ((ty / 8) * (w / 8) + (x / 8)) * 64 + morton8(x & 7, ty & 7);
}

}  // namespace c3l
