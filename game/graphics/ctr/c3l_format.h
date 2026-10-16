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
// v9: merc envmap draws (MercDraw::eye_id kMercEnvmapDraw)
// v10 (AI-assisted): visibility runs per chunk (Chunk::vis_runs_offset, VisRun)
// v11 (AI-assisted): compressed blocks (BlockDir after the header); the layout is otherwise v10's
// v12 (AI-assisted): merc vertices the game rewrites (ripple water, texture scroll), see
// MercModHeader; otherwise v11
// v13 (AI-assisted): the vista tier (CHUNK_KIND_VISTA chunks, lod_tier 4) and Draw::kind
// kDrawCullBack; otherwise v12. Readers detect both by their values, not by the version.
// v13 files may also hold generated merc LODs (<model>~genlod, kMercGenLodSuffix): no layout
// change, older readers see an extra model (merged Oct 11, AI-assisted).
// v14 (AI-assisted): a shrub chunk's max_dist is its shrubs' PS2 distance (the converter groups
// shrubs by prototype distance class: the middle of the PS2 fade, from the prototype's dists);
// before, every shrub chunk had the converter's --shrub-dist. Otherwise v13.
// v15 (AI-assisted): draw optimisations (ctr_level_converter --vcache-fifo, --atlas, --opaque-alpha):
// Draw::kind kDrawAtlasUV (texture coordinates in 1/kAtlasUVScale, the draw's texture an atlas),
// kDrawAlphaTestDropped and kDrawBlendDropped (the alpha test / blending the converter proved can't
// change a pixel was taken out of Draw::mode). Otherwise v14. Readers detect them by their bits.
// v16 (AI-assisted): time of day at runtime (see TodColor). Vertex::pad is the vertex's time of day
// color (index < Header::tod_color_count) or multiplied color (tod_color_count + TodMul index),
// 0xffff: none. Vertex::rgba keeps the converter's palette (--palette), until the first blend.
// Otherwise v15.
// v17 (AI-assisted): tie levels of detail (see ExtHeader, TieLodGroup, RunLod): Header::ext_offset
// (was tod_pad, 0) points at an ExtHeader. Otherwise v16; an older reader would draw both versions
// of the LOD groups.
constexpr uint32_t kVersion = 17;
// (AI-assisted) the first version with the extension header (tie levels of detail)
constexpr uint32_t kTieLodVersion = 17;
// (AI-assisted) the first version with the time of day table
constexpr uint32_t kTodVersion = 16;
// (AI-assisted) the first version whose shrub chunks carry their PS2 distance
constexpr uint32_t kShrubPs2DistVersion = 14;
// v7 files (16-bit textures without mip levels) still load
constexpr uint32_t kMinVersion = 7;

enum TextureFormat : uint8_t {
  TEX_RGB565 = 0,  // u16: r5 g6 b5 (r in the high bits)
  TEX_RGBA4 = 1,   // u16: r4 g4 b4 a4 (r in the high bits)
  TEX_ETC1 = 2,    // v8: ETC1, 4 bits per texel
  TEX_ETC1A4 = 3,  // v8: ETC1 + 4 bit alpha, 8 bits per texel
  TEX_RGBA8 = 4,   // (AI-assisted) u32 stored a, b, g, r: full color (the title screen's logo)
};

struct Header {
  char magic[4];
  uint32_t version;
  char level_name[16];
  // (AI-assisted) v16: the time of day table: TodColor[tod_color_count], then TodMul[tod_mul_count]
  // (0, 0: none). Zero in older files (the end of their 32 byte level name).
  uint32_t tod_offset;
  uint32_t tod_color_count;
  uint32_t tod_mul_count;
  // (AI-assisted) v17: absolute offset of the ExtHeader (0: none; before v17 always 0)
  uint32_t ext_offset;
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
  // v10 (AI-assisted): the chunk's VisRun list (absolute offset; count 0: never hidden by the
  // game's visibility data). Zero in older files.
  uint32_t vis_runs_offset;
  uint32_t vis_run_count;
  uint32_t kind;  // (AI-assisted) what the chunk holds: CHUNK_KIND_* (0 in older files: unknown)
};
static_assert(sizeof(Chunk) == 80);
// (AI-assisted) Chunk::kind
constexpr uint32_t CHUNK_KIND_UNKNOWN = 0, CHUNK_KIND_TFRAG = 1, CHUNK_KIND_TIE = 2, CHUNK_KIND_SHRUB = 3;
// (AI-assisted) v13: far scenery made by the converter (the vista tier, lod_tier 4): copies of the
// level's far content (low resolution tfrag, the ties without a distance limit, the coarse tfrag of
// levels without low resolution tfrag; the last two simplified), merged into large cells, one draw
// per texture and mode. Its VisRuns' `vis` is the SOURCE CHUNK (index into the level's chunks) the
// run's triangles were copied from: a run stands in for its source chunk where that one isn't drawn
// because of its distance (beyond the draw distance, up to the vista distance).
constexpr uint32_t CHUNK_KIND_VISTA = 4;
// (AI-assisted) The vista tier untextured (ctr_level_converter --vista-flat): the same far content
// with each texture's average color baked into the vertex colors and no texture, one draw per cell
// and mode (draws untextured, alpha test dropped). After the CHUNK_KIND_VISTA chunks, at the end of
// the chunk list. The runtime keeps one of the two sets (config vista_untextured) and drops the
// other at load. No layout change (an older runtime would draw these as ordinary chunks: use them
// with a current runtime).
constexpr uint32_t CHUNK_KIND_VISTA_FLAT = 5;
constexpr uint32_t kVistaTier = 4;

/*!
 * (AI-assisted) v10: the game's visibility data per piece of a draw. A chunk's draws are split in
 * runs of indices from one drawable of the level (a tfrag node of up to 8 tfragments, a tie node
 * of up to 8 instances), in draw order: draw `draw` of the chunk continues with index_count
 * indices from drawable `vis`. The game sends a bit string per level each frame (bit n, MSB first
 * in each byte: drawable n can be seen from the camera's place); hidden runs aren't drawn.
 */
struct VisRun {
  uint16_t draw;         // the draw within the chunk (0 = Chunk::first_draw)
  uint16_t vis;          // drawable id in the level's visibility string, 0xffff = always drawn
  uint32_t index_count;  // the draw's next index_count indices
};
static_assert(sizeof(VisRun) == 8);

/*!
 * (AI-assisted) v17: tie levels of detail. The PS2 draws each tie instance with one of its
 * prototype's four geometries, picked by distance (prototype-bucket-tie dists; geometry 3 beyond
 * the far-plane). A file keeps two versions of some instance groups (a tie prototype in one
 * visibility node, or one wind instance): the near one (geometry 0) and the far one (geometry 3, or
 * nothing), in the same chunks, as visibility runs of their own (RunLod: per VisRun of the file, in
 * file order). The runtime draws a group's near runs while the camera is within its distance of
 * the group's sphere (|camera - center| < dist + radius, with hysteresis), else its far runs. The
 * runs of one draw are ordered near, always, far. Groups not listed (kRunLodAlways runs) have one
 * version (geometry 3, as before v17).
 */
constexpr char kExtMagic[4] = {'C', '3', 'L', 'X'};
struct ExtHeader {
  char magic[4];               // kExtMagic
  uint32_t size;               // sizeof(ExtHeader) (later versions may append fields)
  uint32_t lod_group_count;    // TieLodGroup[]
  uint32_t lod_groups_offset;  // absolute
  uint32_t run_lod_count;      // RunLod[]: one per VisRun of the file, from runs_offset on
  uint32_t run_lod_offset;     // absolute
  uint32_t runs_offset;        // where the file's VisRuns start (RunLod i is VisRun i from here)
  uint32_t pad;
};
static_assert(sizeof(ExtHeader) == 32);
struct TieLodGroup {
  float bsphere[4];  // world: x, y, z, radius (both versions)
  float dist;        // the near version up to this distance of the sphere (game units)
};
static_assert(sizeof(TieLodGroup) == 20);
constexpr uint8_t kRunLodAlways = 0, kRunLodNear = 1, kRunLodFar = 2;
struct RunLod {
  uint16_t group;  // TieLodGroup index (0xffff with kRunLodAlways)
  uint8_t lod;     // kRunLod*
  uint8_t pad;
};
static_assert(sizeof(RunLod) == 4);

/*!
 * (AI-assisted) v16: a time of day color: its rgb in each of the game's 8 palettes, GS units
 * (0x80 = 1.0); the PC's tfrag3 / tie3 / shrub time of day colors. The color at runtime, per
 * channel: min(255, sum(w[p] * rgb[p]) >> 6) with the game's weights w (the mood's itimes, 64 =
 * 1.0), as interp_time_of_day_slow (background_common.cpp). The vertex keeps its own alpha.
 */
struct TodColor {
  uint8_t rgb[8][3];
};
static_assert(sizeof(TodColor) == 24);

/*!
 * (AI-assisted) v16: a time of day color times a vertex color: min(255, color * mul / 128) per
 * channel. Shrubs (shrub.vert: base color * time of day color * 4, in GS units base * tod / 128),
 * the untextured vista (the texture's average color).
 */
struct TodMul {
  uint16_t color;  // TodColor index
  uint8_t mul[3];
  uint8_t pad;
};
static_assert(sizeof(TodMul) == 6);

struct Vertex {
  int16_t pos[3];  // quantized, see Chunk
  int16_t pad;     // (AI-assisted) v16: time of day color / TodMul (uint16_t, see kTodVersion)
  int16_t st[2];      // texture coordinates * 1024
  uint8_t rgba[4];    // baked time of day color, GS units (0x80 = 1.0)
};
static_assert(sizeof(Vertex) == 16);

struct Draw {
  uint32_t mode;        // tfrag3 DrawMode bits (common/dma/gs.h)
  uint16_t texture;     // index into the textures, 0xffff = untextured
  uint16_t kind;        // 0 = tfrag, 1 = tie, 2 = shrub; (AI-assisted) v13: | kDrawCullBack
  uint32_t first_index;  // index into the index data (u16), triangle list
  uint32_t index_count;
};
static_assert(sizeof(Draw) == 16);
// (AI-assisted) v13 Draw::kind flag: the draw's triangles are closed, consistently wound surfaces
// (front faces counter-clockwise seen from outside, right-handed world): back faces may be culled
constexpr uint16_t kDrawCullBack = 0x100;
// (AI-assisted) v15 Draw::kind flags (see kVersion).
// kDrawAtlasUV: the texture is an atlas of small textures and the texture coordinates are in units
// of 1 / kAtlasUVScale (not 1 / 1024): the cells are small parts of it.
constexpr uint16_t kDrawAtlasUV = 0x200;
constexpr float kAtlasUVScale = 32768.f;
// kDrawBlendDropped: the draw mode had alpha blending (Cs * As + Cd * (1 - As)), but every texel of
// every mip level of the texture has alpha 1.0 (or it's untextured) and every vertex alpha is
// >= 0x80: As = 1 everywhere, the blending leaves Cs. The ab bit was cleared; the draw keeps its
// place among the blended draws (the runtime keeps its order). config opaque_alpha = off: blended.
constexpr uint16_t kDrawBlendDropped = 0x400;
// kDrawAlphaTestDropped: the draw mode's alpha test (GEQUAL, reference: the mode's aref field) can
// never fail for these triangles (alpha 1.0 everywhere, as above): its enable bit was cleared
// (draws of the same texture and state may have been merged). config opaque_alpha = off: tested.
constexpr uint16_t kDrawAlphaTestDropped = 0x800;
constexpr uint32_t kAtlasVersion = 15;

/*!
 * (AI-assisted) v11: compression. The file is the v10 layout (the "raw image": every offset in the
 * header, chunks, textures... is an offset in it) cut into blocks, each compressed on its own, so
 * that any piece can be read without the rest: the 128-byte header (raw, version 11), then a
 * BlockDir and its Block[num_blocks], then the packed blocks one after the other. The blocks cover
 * the raw image from byte 128 to raw_size in order. The converter starts a block at the start of
 * every piece the runtime reads (a texture with its mips, a chunk's vertices, ...) so most reads
 * decode whole blocks straight into their destination (linear memory). See c3l_reader.h.
 */
constexpr char kBlockMagic[4] = {'C', '3', 'L', 'Z'};
enum BlockCodec : uint32_t {
  CODEC_STORED = 0,  // the raw bytes
  CODEC_LZ4 = 1,     // an LZ4 block (lz4.h LZ4_decompress_safe, no frame)
  CODEC_ZSTD = 2,    // a zstd frame (with its content size)
};
struct BlockDir {
  char magic[4];          // kBlockMagic
  uint32_t raw_size;      // of the raw image
  uint32_t num_blocks;    // Block[] follows
  uint32_t max_block;     // largest Block::raw_size
};
static_assert(sizeof(BlockDir) == 16);
struct Block {
  uint32_t raw_offset;   // in the raw image
  uint32_t raw_size;
  uint32_t file_offset;  // of the packed bytes
  uint32_t packed;       // packed size (bits 0-27) | codec << 28
  uint32_t packed_size() const { return packed & 0x0fffffff; }
  uint32_t codec() const { return packed >> 28; }
};
static_assert(sizeof(Block) == 16);

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
    case TEX_RGBA8:
      return w * h * 4;
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
  uint8_t eye_id;       // 0xff: not an eye. kMercEnvmapDraw: the envmap pass of an envmapped
                        // effect (after its normal draws). Else the eye texture slot
                        // (CtrEyeRenderer) to use
};
constexpr uint8_t kMercEnvmapDraw = 0xfe;
// (AI-assisted) MercDraw::palette[kMercDrawFlagsByte] (an unused palette byte: zero in files from
// older converters): kMercDrawCullBack = the draw's triangles lie on closed surfaces of the model
// and face outward (the same winding as kDrawCullBack, agreeing with the vertex normals): their
// back faces may be culled (config cull_merc). No layout change; detected by the value.
constexpr int kMercDrawFlagsByte = 30;
constexpr uint8_t kMercDrawCullBack = 0x01;
// (AI-assisted) a model named <name> + this is <name>'s generated low LOD (same vertices: same
// first_vertex and vertex_count; draws and indices of its own, ctr_level_converter --gen-lod). No
// layout change: older readers load it as one more model, which nothing asks for.
constexpr char kMercGenLodSuffix[] = "~genlod";
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
  uint32_t num_vertices, num_targets, num_dests;
  uint32_t mod_offset;  // v12 (AI-assisted): absolute offset of the MercModHeader, 0: none (v11: 0)
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

/*!
 * (AI-assisted) v12: vertices the game rewrites in its own merc data every frame (ripple: water
 * waves, texture scroll: flowing water). The PC renderer (Merc2::model_mod_draws) unpacks them
 * from the game's merc fragments when the game asks (pc-merc-vtx-update); so does CtrMercRenderer.
 * Per effect with such vertices: walking the effect's first frag_count fragments, the fragments
 * whose mask byte is set are unpacked one after the other (vertex numbers 0 .. vidx_end), and
 * unpacked vertex dest.src goes to model vertex dest.vertex (position, normal, texture
 * coordinates). Section: MercModHeader, MercModEffect[], MercModDest[], mask bytes.
 */
struct MercModHeader {
  uint32_t num_effects, num_dests, num_mask_bytes, pad;
};
static_assert(sizeof(MercModHeader) == 16);

struct MercModEffect {
  uint32_t model;  // index into MercModel[]
  uint8_t effect;
  uint8_t pad0;
  uint16_t frag_count;  // mask bytes (tfrag3 fragment_mask)
  uint32_t first_mask;
  uint32_t vidx_end;  // unpacked vertices (tfrag3 expect_vidx_end)
  uint32_t first_dest;
  uint32_t dest_count;
  // the model's unpack constants (tfrag3::MercModel): position = (lump byte as the VIF's
  // 0x4b010000 row add + fragment offset) * xyz_scale, st = (st_vif_add row add) + st_magic
  uint32_t st_vif_add;
  float st_magic;
  float xyz_scale;
  uint32_t pad1[3];
};
static_assert(sizeof(MercModEffect) == 48);

struct MercModDest {
  uint16_t src;     // unpacked vertex
  uint16_t vertex;  // model vertex
};
static_assert(sizeof(MercModDest) == 4);

}  // namespace c3l
