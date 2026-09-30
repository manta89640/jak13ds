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

constexpr char kMagic[4] = {'C', '3', 'L', 'V'};
constexpr uint32_t kVersion = 8;
// v7 files (16-bit textures without mip levels) still load
constexpr uint32_t kMinVersion = 7;

enum TextureFormat : uint8_t {
  TEX_RGB565 = 0,  // u16: r5 g6 b5 (r in the high bits)
  TEX_RGBA4 = 1,   // u16: r4 g4 b4 a4 (r in the high bits)
  TEX_ETC1 = 2,    // v8: ETC1, 4 bits per texel
  TEX_ETC1A4 = 3,  // v8: ETC1 + 4 bit alpha, 8 bits per texel
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
  uint32_t num_merc_models;
  uint32_t merc_models_offset;  // MercModel[num_merc_models]
  uint32_t merc_vertex_offset;  // MercVertex[]
  uint32_t merc_vertex_size;
  uint32_t merc_index_offset;  // u16[], relative to the model's first vertex
  uint32_t merc_index_size;
  uint32_t merc_draw_offset;  // MercDraw[]
  uint32_t merc_draw_size;
  uint32_t merc_blerc_offset;  // MercBlercHeader, then its arrays (0: none)
  uint32_t merc_blerc_size;
};
static_assert(sizeof(Header) == 128);

/*!
 * Texture, already in the 3DS GPU layout: 8x8 tiles (rows of tiles), Morton order inside a tile,
 * bottom row of the image first (ETC1: see tools/ctr_level_converter/ctr_texture.h). Power of two,
 * 8..128. v8: `levels` mip levels one after the other (each half the size of the previous one, the
 * last one at least 8 texels on its short side); v7: always one level (levels = 0).
 */
struct Texture {
  uint16_t w, h;
  uint8_t format;  // TextureFormat
  uint8_t levels;  // v8: number of mip levels in the data (>= 1). v7: 0
  uint8_t pad[2];
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
  float max_dist;  // not drawn if the bounding sphere is further than this from the camera
                   // (game units; 0 = no limit). Small objects are put in separate chunks.
  uint32_t lod_tier;  // 0: always (tie), 1: detailed tfrag, 2: coarse tfrag (see lod_center),
                      // 3: the game's low resolution tfrag, only for views from outside the level
  float lod_center[3];  // tiers 1 and 2: the detailed version is drawn when the camera is closer
                        // to this point than the LOD distance (the same point for both versions
                        // of a grid cell, so exactly one of them is drawn)
  uint32_t pad[3];
};
static_assert(sizeof(Chunk) == 80);

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

/*! Bytes of one mip level of a texture. */
inline uint32_t texture_level_bytes(uint32_t w, uint32_t h, uint8_t format) {
  switch (format) {
    case TEX_ETC1:
      return w * h / 2;
    case TEX_ETC1A4:
      return w * h;
    default:
      return w * h * 2;
  }
}

/*! Texel index in the 3DS tiled layout for image pixel (x, y), y = 0 at the top. */
inline uint32_t tiled_index(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
  uint32_t ty = h - 1 - y;
  return ((ty / 8) * (w / 8) + (x / 8)) * 64 + morton8(x & 7, ty & 7);
}

// ---------------- merc (skinned models) ----------------

constexpr int kMercPaletteSize = 24;  // bones per draw (vertex shader uniform budget)

/*!
 * A merc model (characters, objects). Found by name: the game sends the name with its bones.
 */
struct MercModel {
  char name[64];
  uint32_t first_vertex;  // into the merc vertex data
  uint32_t vertex_count;  // <= 65536
  uint32_t first_draw;    // into the merc draw data
  uint32_t draw_count;
  float scale;  // model position = pos * scale
  uint32_t blerc_first;  // blend shape vertices (MercBlercVertex) of the model: face animation
  uint32_t blerc_count;
  uint32_t pad;
};
static_assert(sizeof(MercModel) == 96);

/*!
 * One draw of a merc model, limited to kMercPaletteSize bones: palette[i] is the model bone used
 * by vertices with bone index i.
 */
struct MercDraw {
  uint32_t mode;         // tfrag3 DrawMode bits
  uint16_t texture;      // 0xffff = untextured
  uint8_t effect;        // merc effect index (the game can disable effects)
  uint8_t palette_count;
  uint32_t first_index;  // triangle list, into the merc index data
  uint32_t index_count;
  uint8_t palette[31];  // kMercPaletteSize used
  uint8_t eye_id;       // 0xff: not an eye. Else the eye texture slot (CtrEyeRenderer) to use
};
static_assert(sizeof(MercDraw) == 48);

struct MercVertex {
  int16_t pos[3];      // * MercModel::scale
  uint8_t bones[3];    // index into the draw's palette
  uint8_t weights[3];  // 0..255
  int16_t st[2];       // * 1024
  uint8_t rgba[4];
  int8_t normal[3];    // * 127 (lighting)
  uint8_t pad;
};
static_assert(sizeof(MercVertex) == 24);

/*!
 * Blend shapes (merc "blerc", faces): each frame the game sends up to kMercBlercWeights weights
 * for a model, and the positions of its blend shape vertices are
 *   base + sum(targets: weight[target.weight] * target.offset)
 * written to the model's vertices dest[first_dest .. first_dest + dest_count) (the same source
 * vertex can be in several draws). The section: MercBlercHeader, vertices, targets, dests (u16).
 */
constexpr int kMercBlercWeights = 40;

struct MercBlercHeader {
  uint32_t num_vertices, num_targets, num_dests, pad;
};
static_assert(sizeof(MercBlercHeader) == 16);

struct MercBlercVertex {
  float base[3];  // model space (MercVertex::pos * MercModel::scale)
  uint32_t first_target;
  uint16_t target_count;
  uint16_t dest_count;
  uint32_t first_dest;
};
static_assert(sizeof(MercBlercVertex) == 24);

struct MercBlercTarget {
  float offset[3];
  uint32_t weight;  // index into the weights
};
static_assert(sizeof(MercBlercTarget) == 16);

}  // namespace c3l
