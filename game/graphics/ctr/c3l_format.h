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
#include <string_view>

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
// v18 (AI-assisted): a chunk's vertices grouped by the visibility id of its runs: each id's
// vertices are one range that no other id's indices use (ids in order of their first run, first
// use inside; a vertex of several ids copied, ~0.4%), so the runtime rewrites the time of day
// colors of the drawn runs' ids only. No layout change; the runtime finds the ranges from the
// indices (and rewrites whole chunks where they overlap, as in older files).
// v19 (AI-assisted, streaming): geometry streaming (see StreamChunk). Every tie instance group whose
// shape changes visibly gets its near version (geometry 0), in chunks of their own ("near chunks",
// --stream-cell meters) that the runtime reads from the file while the camera comes near and frees
// when it is far; the group's far version stays in its tie chunk as before (a near chunk that isn't
// there yet draws the far version). The near chunks' vertex and index data come last in their
// sections, in spatial order. The extension header grows (ExtHeader::size 48): each file run's
// bounding sphere (RunSphere), the near chunk table. Draw::kind kDrawAlpha80. Otherwise v18; a v18
// reader would draw both versions (it is refused: version 19).
// (AI-assisted, j2vis) v20: the prototypes the game hides by task state (Jak II), see ProtoVis.
// Otherwise v19 (files without such prototypes are v19's byte for byte but for the version).
// (AI-assisted, j2water2) v21: envmapped tie shine (Jak II), see ExtEnvTie. Otherwise v20 (files
// without envmapped ties are v20's byte for byte but for the version).
// v22: time of day alpha (TodColorA: the alpha of each palette, blended like the rgb); textures up
// to 1024 texels. Otherwise v21.
// v23: alpha mask textures (TEX_A8, TEX_A4): an RGBA8 texture whose alpha needs 8 bits is stored
// as its color (ETC1 or RGB565) and its alpha as a texture of its own, after the others (see
// texture_mask_of). Otherwise v22.
// v24: TEX_RGBA5551 textures. Otherwise v23.
constexpr uint32_t kVersion = 24;
// the first version with RGBA5551 textures
constexpr uint32_t kTex5551Version = 24;
// the first version with alpha mask textures
constexpr uint32_t kTexMaskVersion = 23;
// the first version whose time of day colors have alpha (TodColorA)
constexpr uint32_t kTodAlphaVersion = 22;
// (AI-assisted, j2water2) the first version with the envmapped tie table (ExtEnvTie)
constexpr uint32_t kEnvTieVersion = 21;
// (AI-assisted, j2vis) the first version with the prototype visibility table (ExtProtoVis)
constexpr uint32_t kProtoVisVersion = 20;
// (AI-assisted) the first version with near chunks (StreamChunk), run spheres and kDrawAlpha80
constexpr uint32_t kStreamVersion = 19;
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
  TEX_A8 = 5,      // v23: 8-bit alpha, the alpha mask of another texture (texture_mask_of)
  TEX_A4 = 6,      // v23: 4-bit alpha (two texels per byte, the first in the low bits), as TEX_A8
  TEX_RGBA5551 = 7,  // v24: u16: r5 g5 b5 a1 (r in the high bits, a in bit 0)
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
 * 8..1024. v8: `levels` mip levels one after the other (each half the size of the previous one, the
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

// (AI-assisted, Jak II) Texture::pad: the animated texture slot whose base texture this is
// (jak2_animated_texture_slots() index + 1, little endian; 0: not animated). The runtime draws the
// slot's animation (CtrTexAnimJak2) in its place. Files before it have 0 there.
inline uint16_t texture_anim_slot(const Texture& t) {
  return (uint16_t)(t.pad[0] | (t.pad[1] << 8));
}
inline void set_texture_anim_slot(Texture* t, uint16_t slot) {
  t->pad[0] = (uint8_t)(slot & 0xff);
  t->pad[1] = (uint8_t)(slot >> 8);
}
// v23: a TEX_A8 / TEX_A4 texture is the alpha of the texture whose index Texture::pad holds (same
// size and levels). Draws name the color texture; the runtime samples the alpha from the mask
// (the same texels as the RGBA8 texture had). Never an animated slot's texture.
inline bool texture_is_mask(const Texture& t) {
  return t.format == TEX_A8 || t.format == TEX_A4;
}
inline uint16_t texture_mask_of(const Texture& t) {
  return (uint16_t)(t.pad[0] | (t.pad[1] << 8));
}

/*!
 * A piece of the level: triangle lists sharing one quantized vertex range.
 * world position = origin + pos * scale (game units, 4096 = 1 meter)
 */
struct Chunk {
  float bsphere[4];  // x, y, z, radius (world)
  float origin[3];
  float scale;
  uint32_t first_vertex;  // index into the vertex data (16-byte rows: mesh tails between chunks)
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
// because of its distance (beyond the draw distance, up to the vista distance). (AI-assisted,
// opt-audit F1) In a "spanning" level (low resolution tfrag, coarse tfrag reaching > 1 km past the
// playable area: finalboss, training) the far coarse cells are copied too, and a lowres run's `vis`
// may name the coarse tfrag chunk around it instead (drawn with that cell's copy). No layout change.
constexpr uint32_t CHUNK_KIND_VISTA = 4;
// (AI-assisted) The vista tier untextured (the removed ctr_level_converter --vista-flat): the same
// far content with each texture's average color baked into the vertex colors and no texture, one
// draw per cell and mode, after the CHUNK_KIND_VISTA chunks at the end of the chunk list. No
// shipped file has it and the converter no longer writes it; the kind stays reserved for files
// that do.
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
  // (AI-assisted, title-popin) absolute offset of float[lod_group_count]: each group's shape error
  // (the larger one-sided Hausdorff distance between its two versions, game units), from which
  // the runtime switches by screen-space error; 0: none (files before it: the dist switch)
  uint32_t lod_error_offset;
  // (AI-assisted, streaming) v19 (size >= 48): RunSphere[run_lod_count] (the file's runs from
  // runs_offset on, 0: none), StreamChunk[stream_count], flags (0)
  uint32_t run_sphere_offset;
  uint32_t stream_count;
  uint32_t stream_offset;
  uint32_t stream_flags;
};
static_assert(sizeof(ExtHeader) == 48);
constexpr uint32_t kExtHeaderV17Size = 32;

/*!
 * (AI-assisted, j2vis) v20: prototype visibility. Jak II hides and shows some background prototypes
 * (ties, shrubs) by task state: prototypes-game-visible-set! (goal_src/jak2/engine/draw/drawable.gc,
 * called from task-control.gc) sets the prototype's `visible` flag (= hidden) by name, and the PC
 * port's tie and shrub buckets send the names of the hidden ones every frame (pc-add-tie-vis-mask,
 * pc-add-shrub-vis-mask). The converter keeps the triangles of those prototypes (the names in
 * tools/ctr_level_converter/proto_vis_jak2.inc) in visibility runs of their own and lists them:
 * ExtHeader::size >= kExtHeaderV20Size, then this at ext_offset + sizeof(ExtHeader). The runtime
 * hides the runs of the prototypes named in the game's packets. Older files: nothing hidden.
 */
constexpr uint32_t kExtHeaderV20Size = 64;
struct ExtProtoVis {
  uint32_t proto_count;        // ProtoVis[]
  uint32_t protos_offset;      // absolute
  uint32_t proto_run_count;    // uint32_t[]: file runs (VisRun index from ExtHeader::runs_offset)
  uint32_t proto_runs_offset;  // absolute
};
static_assert(sizeof(ExtProtoVis) == kExtHeaderV20Size - 48);
constexpr uint16_t kProtoTie = 1, kProtoShrub = 2;  // ProtoVis::kind: the game packet it's in
struct ProtoVis {
  char name[52];       // the prototype's name in the game ("sewer-c-connect-door.mb"), NUL ended
  uint16_t kind;       // kProtoTie / kProtoShrub
  uint16_t pad;
  uint32_t first_run;  // its runs: proto runs [first_run, first_run + run_count)
  uint32_t run_count;
};
static_assert(sizeof(ProtoVis) == 64);

/*!
 * (AI-assisted, j2water2) v21: envmapped ties (Jak II ETIE: tie prototypes with an envmap shader,
 * the PC's NORMAL / TRANS / WATER_ENVMAP categories). The PC draws them twice (Tie3: the base draw,
 * then envmap_second_pass_draw: etie.vert's envmap coordinates from the camera space position and
 * normal, color = proto tint * the time of day's env color, added with the destination alpha as its
 * weight). The converter keeps the base draws (as before) and lists them here with the shine's
 * texture, mode and tint; the runtime draws both in one (texture unit 2). Their chunks get each
 * vertex's normal. ExtHeader::size >= kExtHeaderV21Size, then this at ext_offset + 64 (after
 * ExtProtoVis, zero when the level has no toggled prototypes). Older files: no shine.
 */
constexpr uint32_t kExtHeaderV21Size = 80;
struct ExtEnvTie {
  uint32_t draw_count;     // EnvTieDraw[], by draw index
  uint32_t draws_offset;   // absolute
  uint32_t chunk_count;    // EnvTieChunk[], by chunk index
  uint32_t chunks_offset;  // absolute
};
static_assert(sizeof(ExtEnvTie) == kExtHeaderV21Size - kExtHeaderV20Size);
struct EnvTieDraw {
  uint32_t draw;     // index into the file's draws (a tie base draw)
  uint16_t texture;  // the envmap texture (index into the textures)
  uint16_t pad;
  uint32_t mode;     // its tfrag3 DrawMode (clamp, filter; blending Cs * Ad + Cd)
  uint8_t tint[4];   // the prototype's tint (etie.vert proto_tint: 0xff = 1.0)
};
static_assert(sizeof(EnvTieDraw) == 16);
// a chunk with envmapped tie draws: int8_t normal[4] (x, y, z: world space * 127, 0) per vertex of
// the chunk at normals_offset (absolute): its mesh tail, right after its vertices (after its
// MorphVertex records in a morph chunk) in files written since the layout in load order
struct EnvTieChunk {
  uint32_t chunk;
  uint32_t normals_offset;
};
static_assert(sizeof(EnvTieChunk) == 8);

/*!
 * Wind ties (Jak 1 tie trees' instanced_wind_draws: palms, plants): baked at rest in world space
 * as the other ties, and swayed by the runtime as Tie3::render_tree_wind does, an instance at a
 * time: do_wind_math's shear of the instance matrix, x += k.x * (y - y0), z += k.z * (y - y0) in
 * world space (y0: the instance's origin), k from its wind_idx and stiffness and the game's wind
 * (the tie bucket's WindWork). ExtHeader::size >= kExtHeaderV22Size, then this at ext_offset +
 * kExtHeaderV21Size (ExtEnvTie zero when the level has no envmapped ties). Older files: at rest.
 */
constexpr uint32_t kExtHeaderV22Size = 96;
struct ExtWind {
  uint32_t inst_count;     // WindInst[]: every wind instance of the tie trees, tree by tree, in
                           // the order the PC steps them (each step changes its wind_idx's state)
  uint32_t insts_offset;   // absolute
  uint32_t chunk_count;    // WindChunk[]
  uint32_t chunks_offset;  // absolute
};
static_assert(sizeof(ExtWind) == kExtHeaderV22Size - kExtHeaderV21Size);
struct WindInst {
  uint16_t wind_idx;
  uint16_t pad;
  float stiffness;
  float y0;  // world y of the instance's origin (game units)
};
static_assert(sizeof(WindInst) == 12);
// a chunk's wind vertices: uint8_t slot per vertex of the chunk at slots_offset,
// slot 0 none, slot s the instance insts[s - 1] of uint16_t insts[slot_count] at insts_offset,
// and uint8_t[draw_count] at draws_offset: 1 for its draws with wind vertices (absolute; in the
// extension's section, loaded with the level)
constexpr uint32_t kWindSlots = 63;
struct WindChunk {
  uint32_t chunk;
  uint32_t slots_offset;
  uint32_t insts_offset;
  uint32_t slot_count;  // <= kWindSlots
  uint32_t draws_offset;
};
static_assert(sizeof(WindChunk) == 20);

/*!
 * Shrub fade bands (prototype-bucket-shrub dists, tools/ctr_level_converter/shrub_ps2_dists.inc):
 * the PS2 draws a shrub in full up to x, fading out ((w - d) / (w - x)) up to w. Shrub chunks'
 * visibility runs hold one prototype band each (at most 96 triangles). ExtHeader::size >=
 * kExtHeaderV23Size, then this at ext_offset + kExtHeaderV22Size (ExtWind zero when the level has
 * no wind ties). Older files: the chunk's distance class.
 */
constexpr uint32_t kExtHeaderV23Size = 104;
struct ExtShrubFade {
  uint32_t band_count;    // uint32_t[]: one per VisRun of the file from ExtHeader::runs_offset,
                          // x << 16 | w in 1/16 m (0: none)
  uint32_t bands_offset;  // absolute
};
static_assert(sizeof(ExtShrubFade) == kExtHeaderV23Size - kExtHeaderV22Size);

/*!
 * v23: each texture's content key (texture_content_key: its size, format, levels and texels, a
 * color's alpha mask's texels too; 0: never shared, as masks and animated slots), for sharing a
 * texture between the loaded levels that have it (texshare.c3t). ExtHeader::size >=
 * kExtHeaderTexKeySize, then this at ext_offset + kExtHeaderV23Size.
 */
constexpr uint32_t kExtHeaderTexKeySize = 112;
struct ExtTexKey {
  uint32_t count;   // uint64_t[count]: by texture index
  uint32_t offset;  // absolute
};
static_assert(sizeof(ExtTexKey) == kExtHeaderTexKeySize - kExtHeaderV23Size);
inline uint64_t texture_key_add(uint64_t h, const void* data, size_t bytes) {
  const uint8_t* p = (const uint8_t*)data;
  for (size_t i = 0; i < bytes; i++) {
    h = (h ^ p[i]) * 1099511628211ull;
  }
  return h;
}
inline uint64_t texture_content_key(const Texture& t, const void* data, const void* mask,
                                    size_t mask_bytes) {
  const uint32_t d[4] = {t.w, t.h, t.format, t.levels};
  uint64_t h = texture_key_add(1469598103934665603ull, d, sizeof(d));
  h = texture_key_add(h, data, t.data_size);
  h = texture_key_add(h, mask, mask_bytes);
  return h ? h : 1;
}

// a shared texture's descriptor (its size, format, levels and bytes, its mask's too; 0: no mask):
// a level whose texture has the group's key but another descriptor loads its own copy (a key
// collision), and texshare.c3t leaves such keys out
inline uint64_t texture_share_desc(const Texture& t, const Texture* mask) {
  const uint32_t d[10] = {t.w,         t.h,         t.format,         t.levels,         t.data_size,
                          mask ? mask->w : 0u, mask ? mask->h : 0u, mask ? mask->format : 0u,
                          mask ? mask->levels : 0u, mask ? mask->data_size : 0u};
  return texture_key_add(1469598103934665603ull, d, sizeof(d));
}

/*!
 * texshare.c3t (next to the .c3l files, ctr_level_converter --all): the content keys of the
 * textures that more than one level of the game has, each with its group: the textures that the
 * same levels have. A group is loaded once, by the first loaded level of those, into a texture pool
 * of its own, and the levels share it until the last of them unloads (every texture of a group is
 * in every level that uses the group, so none of it is unused while it is loaded).
 */
constexpr char kTexShareMagic[4] = {'C', '3', 'S', 'H'};
struct TexShareHeader {
  char magic[4];
  uint32_t version;  // 1
  uint32_t count;    // TexShareEntry[count], by key
  uint32_t groups;
};
struct TexShareEntry {
  uint64_t key;
  uint32_t group;
  uint32_t pad;
};
static_assert(sizeof(TexShareEntry) == 16);

/*!
 * The opening planes of the kDrawCullCond draws. ExtHeader::size >=
 * kExtHeaderV24Size, then this at ext_offset + kExtHeaderTexKeySize (the sections before it zero when
 * the level has none). A draw's back faces may be culled in a frame where the camera position p
 * (world, game units) has dot(n, p) < d for every one of its planes and is outside each of its
 * surfaces' boxes grown by the near plane's reach (from inside a box the camera may be inside the
 * surface, or see its inside where the near plane cuts its walls). Files without boxes
 * (box_count 0, the field was padding): both sides drawn.
 */
constexpr uint32_t kExtHeaderV24Size = 120;
struct ExtCullCond {
  uint32_t draw_count;     // CullCondDraw[]
  uint32_t draws_offset;   // absolute
};
static_assert(sizeof(ExtCullCond) == kExtHeaderV24Size - kExtHeaderTexKeySize);
struct CullCondDraw {
  uint32_t draw;           // index into the file's draws
  uint32_t plane_count;    // CullCondPlane[]
  uint32_t planes_offset;  // absolute
  uint32_t box_count;      // CullCondBox[], right after the planes
};
static_assert(sizeof(CullCondDraw) == 16);
struct CullCondPlane {
  float n[3];  // the opening's outward unit normal
  float d;
};
static_assert(sizeof(CullCondPlane) == 16);
struct CullCondBox {
  float lo[3];  // world, game units: the surface's vertices wherever its morph moves them
  float pad0;
  float hi[3];
  float pad1;
};
static_assert(sizeof(CullCondBox) == 2 * sizeof(CullCondPlane));

/*!
 * The PS2's tie geomorph (common/custom_data/TieMorph.h). A morph group (TieLodGroup with
 * group_mid > 0) has three versions: RunLod kRunLodNear (geometry 0), kRunLodMid (geometry 2), both
 * in near chunks, and kRunLodFar (geometry 3). Each instance has two morph factors, f_b = clamp((d
 * - start_b) * rlen_b, 0, 1) with d its distance minus its radius: band 1 (from the near-plane)
 * moves geometry 0's own moving vertices onto geometry 2's surface by the mid-plane, band 2 (from
 * the mid-plane) moves geometry 2 onto geometry 3's surface by the far-plane, and geometry 0's
 * vertices with it (with the point of geometry 2 they reached). Every version draws the same
 * surface: a group shows the near version within the mid-plane of its nearest point, the mid
 * version within the far-plane, else the far version.
 * A near chunk with moving vertices (MorphChunk) has a MorphVertex per vertex (at records_offset,
 * the runtime's mesh tail as the envmapped tie normals'); the vertex is at pos + (f_1 * delta + f_2 *
 * delta2) * delta_scale with its row's factors. ExtHeader::size >= kExtHeaderMorphSize, then this
 * at ext_offset +
 * kExtHeaderV24Size.
 */
constexpr uint32_t kExtHeaderMorphSize = 160;
struct ExtMorph {
  uint32_t inst_count;        // MorphInst[]
  uint32_t insts_offset;      // absolute
  uint32_t chunk_count;       // MorphChunk[]
  uint32_t chunks_offset;     // absolute
  uint32_t row_count;         // uint32_t[]: MorphInst index, the chunks' rows
  uint32_t rows_offset;       // absolute
  uint32_t group_mid_offset;  // float[ExtHeader::lod_group_count]: mid-plane (game units), 0: none
  uint32_t draw_rows_size;    // uint8_t[]: per morph chunk draw n, then the n rows its vertices use
  uint32_t draw_rows_offset;  // absolute
  uint32_t pad;
};
static_assert(sizeof(ExtMorph) == kExtHeaderMorphSize - kExtHeaderV24Size);
struct MorphInst {
  float c[3];      // world (game units)
  float r;
  float start[2];  // band 1: near-plane, band 2: mid-plane (game units)
  float rlen[2];   // rlength-near, rlength-mid (1 / game units)
};
static_assert(sizeof(MorphInst) == 32);
// the morph shader's rows: row 0 is zero (vertices that don't move)
constexpr uint32_t kMorphRows = 80;
struct MorphChunk {
  uint32_t chunk;
  uint32_t vertex_count;    // its Chunk::vertex_count
  uint32_t row_first;       // ExtMorph rows [row_first, row_first + row_count): rows 1..row_count
  uint32_t row_count;
  uint32_t draw_rows;       // its draws' lists in ExtMorph's draw rows, from here in draw order
  uint32_t records_offset;  // MorphVertex[vertex_count] (absolute)
  float delta_scale;        // MorphVertex deltas x this = the chunk's quantized units
  uint32_t pad;
};
static_assert(sizeof(MorphChunk) == 32);
struct MorphVertex {
  int8_t delta[3];   // band 1 (x MorphChunk::delta_scale)
  int8_t row;        // 0: doesn't move
  int8_t delta2[3];  // band 2
  int8_t pad;
};
static_assert(sizeof(MorphVertex) == 8);

/*!
 * The PS2's tfrag geomorph (common/custom_data/TfragMorph.h). Each tfrag visibility group of the
 * near tier (lod_tier 1) has three versions as runs of its chunks (RunLod in ExtTfrag's array, not
 * the ExtHeader's): kRunLodNear (geometry 0), kRunLodMid (geometry 1), kRunLodFar (geometry 2),
 * sharing their vertices. A vertex at clip depth w (in the units of dists) is at pos + (f0 *
 * band0 + f1 * band1) * delta_scale with f0 = clamp((w - dists[1]) / (dists[2] - dists[1]), 0, 1),
 * f1 = clamp((w - dists[0]) / (dists[1] - dists[0]), 0, 1), its color c + f1 * color1 * (m - c) +
 * f0 * color0 * (t - m): every version draws the PS2's surface where it is drawn, the mid version
 * where the group's nearest point is beyond TfragGroup::mid, the far one beyond far. m and t are
 * the means of the TfragMorphColor entries' colors (written by the runtime with the time of day).
 * ExtHeader::size >= kExtHeaderTfragSize, then this at ext_offset + kExtHeaderMorphSize.
 */
constexpr uint32_t kExtHeaderTfragSize = 208;
struct ExtTfrag {
  float dists[4];           // m2, m1, m0 (view depth, game units), 0
  uint32_t group_count;     // TfragGroup[]
  uint32_t groups_offset;   // absolute
  uint32_t chunk_count;     // TfragChunk[]
  uint32_t chunks_offset;   // absolute
  uint32_t run_lod_count;   // RunLod[] of the TfragChunks' runs (group: TfragGroup index)
  uint32_t run_lod_offset;  // absolute
  uint32_t color_count;     // TfragMorphColor[]
  uint32_t colors_offset;   // absolute
};
static_assert(sizeof(ExtTfrag) == kExtHeaderTfragSize - kExtHeaderMorphSize);
struct TfragGroup {
  float bsphere[4];  // world: x, y, z, radius (every version, unmoved)
  // its mid version from this view depth of its nearest point on, its far version from far on
  // (game units: ExtTfrag dists[1] and dists[2]; a version the same as the one before is left out:
  // mid = far, or far = FLT_MAX)
  float mid, far;
  float pad[2];
};
static_assert(sizeof(TfragGroup) == 32);
struct TfragChunk {
  uint32_t chunk;
  uint32_t run_first;       // its vis_run_count runs' RunLod in ExtTfrag's array, from here
  uint32_t records_offset;  // TfragMorphVertex[vertex_count] (absolute; the mesh tail), 0: none
  float delta_scale;        // band0 / band1 x this = the chunk's quantized units
  uint32_t color_first;     // TfragMorphColor [color_first, color_first + color_count), by vertex
  uint32_t color_count;
  uint32_t pad[2];
};
static_assert(sizeof(TfragChunk) == 32);
struct TfragMorphVertex {
  int8_t band0[3];
  uint8_t color0;  // 1: the color moves in band 0
  int8_t band1[3];
  uint8_t color1;  // 1: the color moves in band 1
  uint8_t m[4];    // the color at the end of band 1 (rgba)
  uint8_t t[4];    // at the end of band 0
};
static_assert(sizeof(TfragMorphVertex) == 16);
struct TfragMorphColor {
  uint16_t vertex;  // in its chunk
  uint16_t m[2];    // TodColor indices: m is their mean
  uint16_t t[4];    // t is their mean
  uint16_t pad;
};
static_assert(sizeof(TfragMorphColor) == 16);

// ext header bytes kExtHeaderTfragSize..kExtHeaderOccSize: a retired section (occluders), ignored
constexpr uint32_t kExtHeaderOccSize = 224;

// ext header bytes kExtHeaderOccSize..kExtHeaderHorizonSize: a retired section (horizon), ignored
constexpr uint32_t kExtHeaderHorizonSize = 256;

// ext header bytes kExtHeaderHorizonSize..kExtHeaderFarCopySize: a retired section (far copies), ignored
constexpr uint32_t kExtHeaderFarCopySize = 272;

// ext header bytes kExtHeaderFarCopySize..kExtHeaderConeSize: a retired section (normal cones), ignored
constexpr uint32_t kExtHeaderConeSize = 288;

/*!
 * (AI-assisted, streaming) v19: a run's bounding sphere in its chunk's quantized units (world =
 * origin + c * scale, radius r * scale): the center of the box of the run's vertices (rounded down)
 * and the length of the larger half of the box per axis, ceil + 1 (as the runtime made them from the
 * vertices before v19: CtrLevel.cpp make_run_spheres). r = 0xffff: unknown (never hidden by it).
 */
struct RunSphere {
  int16_t c[3];
  uint16_t r;
};
static_assert(sizeof(RunSphere) == 8);

/*!
 * (AI-assisted, streaming) v19: a near chunk (Chunk::kind CHUNK_KIND_TIE, lod_tier 0): the near
 * versions (RunLod kRunLodNear) of the tie LOD groups whose center is in its cell. Every LOD group's
 * near version is in near chunks; its far version in an ordinary tie chunk. cls 1: a group the level's
 * v18 budget took (kTieLodBudgets: loaded with the level when streaming is off), 2: one it left coarse
 * (only streamed). A group of a near chunk is near when the camera is within its shape error x the
 * runtime's factor (tie_lod_px, focal length) + its radius of its center, so the chunk is needed
 * within need_a + need_e x that factor of its bounding sphere's center: need_a the largest distance of
 * a group's center to that center plus the group's radius, need_e the largest group error (game
 * units).
 */
struct StreamChunk {
  uint32_t chunk;
  uint16_t cls;
  uint16_t pad;
  float need_a;
  float need_e;
};
static_assert(sizeof(StreamChunk) == 16);
constexpr uint16_t kStreamCore = 1, kStreamExtra = 2;
struct TieLodGroup {
  float bsphere[4];  // world: x, y, z, radius (both versions)
  float dist;        // the near version up to this distance of the sphere (game units)
};
static_assert(sizeof(TieLodGroup) == 20);
constexpr uint8_t kRunLodAlways = 0, kRunLodNear = 1, kRunLodFar = 2, kRunLodMid = 3;
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
// v22: the same with each palette's alpha (GS units), blended as the PC does: min(128, sum(w[p] *
// a[p]) >> 6) with the palettes' alpha weights. TodMul colors take their color's alpha.
struct TodColorA {
  uint8_t rgba[8][4];
};
static_assert(sizeof(TodColorA) == 32);

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
// (AI-assisted, streaming) v19: every vertex the draw uses has alpha 0x80 (the runtime's fog_alpha,
// CTR_TCC_ALPHA80: before v19 worked out at load from the chunk's vertices, which a near chunk
// doesn't have then)
constexpr uint16_t kDrawAlpha80 = 0x1000;
// kDrawCullCond: closed, consistently wound tie surfaces (as kDrawCullBack) but for
// planar openings (the open back of a rock): their back faces can only be seen through an
// opening, so from the outer side of its plane. The draw's planes (c3l::ExtCullCond) say when its
// back faces may be culled; a reader without them draws both sides.
constexpr uint16_t kDrawCullCond = 0x2000;
// (AI-assisted, terraincull) kDrawCullTerrain: floor-like opaque tfrag (|normal y| >= 0.7) that is
// walkable ground itself (collision within 1 m at its center) with no ground more than 2 m below
// its plane within 20 m (ctr_level_converter --terrain-cull), wound to face up: the camera, always
// above the ground near it, never sees its back. config.ini cull_terrain culls their back faces
// (as kDrawCullBack, with cull_ties' winding). No version change: older readers draw both sides.
constexpr uint16_t kDrawCullTerrain = 0x4000;
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
    case TEX_A8:
      return w * h;
    case TEX_A4:
      return w * h / 2;
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
// (AI-assisted, j1ocean) Merc models always drawn at full detail, never as a generated low LOD:
// the pickups (power cells, scout flies, precursor orbs, orb caches, eco vents), small things the
// player looks for from far away. The converter makes no low LOD for them; the runtime ignores one
// that older files have.
// (AI-assisted, opt-audit M6) Also the far views of other levels (medres-*: seen from kilometers,
// their switch distance is never reached anyway) and Jak II's: the pickups (collectables-*: health,
// the pickup blasts), Jak's gun and its upgrade, his board, Daxter's mech, the Seal of Mars and
// the title logo.
inline bool merc_gen_lod_exempt(std::string_view name) {
  constexpr std::string_view kPrefixes[] = {
      "fuel-cell-",    "fuelcell-naked-", "powercellalt-",  "buzzer-",      "money-",
      "orb-cache-top-", "ecovalve-",      "medres-",        "collectables-", "gun-lod0",
      "gun-upgrade-",  "board-lod0",      "mecha-daxter-",  "seal-of-mar",  "jak-logo-"};
  for (const auto p : kPrefixes) {
    if (name.substr(0, p.size()) == p) {
      return true;
    }
  }
  return false;
}
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
