# C3L: 3DS level background format

(AI-assisted.)

C3L holds a level's static background (tfrag, tie, shrub) and its merc models in a form the 3DS
GPU can draw directly.
`tools/ctr_level_converter` writes it from the PC port's `.fr3` files, and
`game/graphics/ctr/CtrLevel.cpp` loads it. The binary layout is defined in
`game/graphics/ctr/c3l_format.h`.

## Converting

```sh
build/tools/ctr_level_converter out/jak1/fr3/village1.fr3 village1.c3l
build/tools/ctr_level_converter --all out/jak1/fr3 out/jak1/c3l   # every level
```

| option | default | meaning |
|---|---|---|
| `--tfrag-geo N` | 1 | tfrag level of detail near the camera. 0 is the most detailed, 2 the least (2 has holes). |
| `--far-tfrag-geo N` | 2 | tfrag level of detail for far away cells (-1: no far version). |
| `--tie-geo N` | 3 | tie level of detail. 0 is the most detailed, 3 the least. |
| `--palette N` | 1 | time of day palette baked into the vertex colors (0..7). |
| `--max-tex N` | 128 | maximum texture size. Larger textures are box-filtered down. |
| `--cell M` | 200 | chunk grid size in meters. Triangles go to the cell of their centroid. |
| `--detail-radius M`, `--detail-dist M` | 4, 100 | tie instances smaller than the radius only drawn up to the distance |
| `--medium-radius M`, `--medium-dist M` | 12, 300 | same for medium tie instances |
| `--shrub-dist M`, `--shrub-cell M` | 100, 50 | shrubs are drawn up to the distance, in chunks of the cell size |
| `--no-tie`, `--no-shrub`, `--no-merc` | off | leave tie / shrubs / merc models out. |
| `--merc-only` | off | only merc models (used for the common GAME.fr3 with `--all`). |
| `--no-etc1` | off | 16-bit textures (RGB565 / RGBA4444) instead of ETC1 / ETC1A4. |
| `--etc1-merc` | off | ETC1 also for the textures of merc models (they stay 16-bit by default). |
| `--etc1-quality N` | 1 | ETC1 encoder: 0 fast, 1 medium, 2 high (slow). |
| `--no-mips` | off | no mip levels. |
| `--threads N` | 0 | texture encoding threads (0: all cores). |

What goes in:

- tfrag trees of kind NORMAL, TRANS, DIRT, ICE and WATER, twice: the near version
  (`--tfrag-geo`, chunk tier 1) and a coarse one (`--far-tfrag-geo`, tier 2). The runtime draws
  one of the two per grid cell, by the camera's distance to the cell center (`lod_distance`).
- LOWRES / LOWRES_TRANS tfrag (the PS2's version for views from a neighbouring level; often only
  the parts visible from there) as tier 3. It is drawn only when the camera is outside the level.
- The static draws of every tie tree (categories normal, trans, water, and the base pass of
  envmapped ties; not the envmap shine passes), with instances already in world space. Small and
  medium instances go to their own chunks with a draw distance.
- Wind tie (palm trees, plants), baked at rest with their instance matrix.
- Shrubs (grass, bushes, small plants), in `--shrub-cell` chunks drawn up to `--shrub-dist`.
  Their vertex colors are the base color times the time of day color (shrub.vert).
- Merc models (characters, objects), see "Merc" below.
- Not converted: hfrag, animated textures (those draws become untextured).

Sizes (defaults): village1 148k triangles (tfrag 42k near + 12k far + 2k lowres, tie 94k),
345 chunks, 419 textures, file 8.7 MB. misty 11.2 MB, citadel 18.5 MB (876 textures).

## Layout

Little endian. All offsets are from the start of the file. Every section is 16-byte aligned.

```
Header (128 bytes)
  char magic[4] = "C3LV"; u32 version = 8 (7 still loads); char level_name[32]
  u32 num_textures, textures_offset
  u32 num_chunks, chunks_offset
  u32 vertex_data_offset, vertex_data_size      Vertex[]
  u32 index_data_offset,  index_data_size       u16[]
  u32 draw_data_offset,   draw_data_size        Draw[]
  u32 texture_data_offset, texture_data_size    texels of all textures
  u32 num_merc_models, merc_models_offset       MercModel[]
  u32 merc_vertex_offset, merc_vertex_size      MercVertex[]
  u32 merc_index_offset,  merc_index_size       u16[]
  u32 merc_draw_offset,   merc_draw_size        MercDraw[]
  u32 merc_blerc_offset,  merc_blerc_size       blend shapes (0: none), see "Merc"

Chunk (80 bytes)
  float bsphere[4]        world-space bounding sphere (x, y, z, r) for frustum culling
  float origin[3], scale  world = origin + pos * scale  (game units: 4096 = 1 m)
  u32 first_vertex, vertex_count   (<= 65536, so indices fit in u16)
  u32 first_draw, draw_count
  float max_dist          not drawn further than this from the camera (0 = no limit)
  u32 lod_tier            0 always (tie), 1 near tfrag, 2 far tfrag, 3 lowres tfrag
  float lod_center[3]     tiers 1/2: the grid cell center used to pick one of the two
  u32 pad[3]

Vertex (16 bytes)
  s16 pos[3]              quantized position, 0..32767 over the chunk's bounding box
  s16 pad
  s16 st[2]               texture coordinates * 1024
  u8  rgba[4]             baked time of day color, GS units (0x80 = 1.0)

Draw (16 bytes)
  u32 mode                tfrag3 DrawMode bits (common/dma/gs.h): blend, alpha test, clamp,
                          depth write, ...
  u16 texture             texture index, 0xffff = untextured
  u16 kind                0 = tfrag, 1 = tie, 2 = shrub
  u32 first_index, index_count   triangle list; indices are relative to the chunk's first_vertex

Texture (16 bytes)
  u16 w, h                powers of two, 8..max-tex
  u8  format              0 = RGB565, 1 = RGBA4444, 2 = ETC1, 3 = ETC1A4 (with alpha)
  u8  levels              mip levels in the data (v7: 0, one level)
  u8  pad[2]
  u32 data_offset, data_size
```

- **Texel layout:** the 3DS GPU layout, ready to copy into a `C3D_Tex`. The image is split into
  8x8 tiles, stored in rows of tiles, with Morton order inside each tile. The bottom row of the
  image comes first, because the GPU samples t = 0 from there. `c3l::tiled_index()` gives the
  texel index for image pixel (x, y) of the 16-bit formats. An ETC1 tile is four 4x4 blocks (top
  left, top right, bottom left, bottom right in memory order), each a little-endian u64 of the
  standard big-endian ETC1 block; ETC1A4 puts the block's 4-bit alpha before it (a u64, texel
  (x, y) of the block at bit 4 * (4 * x + y)). See `tools/ctr_level_converter/ctr_texture.h`.
- **Mip levels:** `levels` images one after the other, each half the size of the previous one,
  down to 8 texels on the short side. Colors are averaged weighted by alpha; for alpha tested
  textures the fraction of texels that pass the test is kept the same in every level, so cut-outs
  (fences, leaves) don't fade away with distance. Without mips the GPU reads the full size texture
  for far away surfaces, which is slow on the 3DS (texture cache misses) and shimmers.
- **Formats:** ETC1 (4 bits per texel) for opaque textures, ETC1A4 (8 bits) when any texel is
  transparent: 1/4 and 1/2 of the 16-bit sizes, so the textures of a level fit in VRAM. The
  runtime copies all textures of a level into one linear memory block and from there to VRAM (see
  `ctr_gpu_pool_create`). Merc textures stay 16-bit unless `--etc1-merc` (faces show ETC1's 4x4
  blocks up close). The converter logs the ETC1 PSNR per level file.
- **Texture colors:** textures come from the fr3 as RGBA8888 with PS2 alpha (0x80 = opaque).
  Alpha is doubled when converting, so 0xff = opaque.
- **Vertex colors:** vertex colors keep GS units. The renderer applies the same `* 2` that the PC
  tfrag shader does.

## Merc

Merc models (skinned characters and objects) come from the fr3's merc data, drawn by
`CtrMercRenderer` with GPU skinning (`platform/3ds/shaders/ctr_skin.v.pica`).

```
MercModel (96 bytes)
  char name[64]           the name the game sends in its merc DMA
  u32 first_vertex, vertex_count, first_draw, draw_count
  float scale             position = pos * scale (model space)
  u32 blerc_first, blerc_count   the model's blend shape vertices
  u32 pad

MercDraw (48 bytes)
  u32 mode                DrawMode bits
  u16 texture             0xffff = untextured
  u8  effect              merc effect index (the game can disable effects)
  u8  palette_count       bones used by this draw (<= 24)
  u32 first_index, index_count   triangle list, indices relative to the model's first_vertex
  u8  palette[31]         palette slot -> skeleton bone
  u8  eye_id              0xff, or the eye texture slot that CtrEyeRenderer draws each frame

MercVertex (24 bytes)
  s16 pos[3]; u8 bones[3] (palette slots); u8 weights[3] (sum 255); s16 st[2] (* 1024);
  u8 rgba[4]; s8 normal[3] (* 127, lighting); u8 pad

Blend shapes: MercBlercHeader {u32 num_vertices, num_targets, num_dests, pad}, then
MercBlercVertex[] {float base[3]; u32 first_target; u16 target_count, dest_count; u32 first_dest},
MercBlercTarget[] {float offset[3]; u32 weight}, u16 dests[] (model vertex indices).
```

- Draws are split so that each one uses at most 24 bones (the vertex shader's uniform space).
- Blend shapes (faces): each frame the positions of the blend shape vertices are
  `base + sum(weight[target.weight] * target.offset)`, written to their `dest` vertices.
- Models with more than 65535 vertices are skipped (logged). Envmap and ripple are not converted.
