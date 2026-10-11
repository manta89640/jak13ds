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
| `--no-tie-winding` | off | (AI-assisted, v13) keep the strips' triangle order. By default every second strip triangle is flipped (consistent winding), and closed tie surfaces are oriented outward and their draws marked `kDrawCullBack` (config `cull_ties`). |
| `--no-vista` | off | (AI-assisted, v13) no vista tier. By default the far content (low resolution tfrag; ties without a distance limit; the coarse tfrag of levels without low resolution tfrag) is copied into `CHUNK_KIND_VISTA` chunks (lod_tier 4) of `--vista-cell` meters, one draw per texture and mode, each run's `vis` = its source chunk. Never for the interiors (jungleb, maincave, darkcave, robocave, sunken, sunkenb, citadel) |
| `--vista-cell M` | 1500 | vista tier cell size |
| `--vista-error M`, `--vista-ratio R`, `--vista-sloppy-error M` | 3, 0.25, 10 | vista ties / coarse tfrag: simplified (topology kept) to the error, then (sloppy) towards the ratio of the triangles within the second error |
| `--merc-only` | off | only merc models (used for the common GAME.fr3 with `--all`). |
| `--gen-lod R`, `--gen-lod-error E`, `--gen-lod-lock-border` | 0.3, 0.02, off | (AI-assisted) merc models with only `-lod0` in the level (not Jak / Daxter, no blend shapes or rewritten vertices): a simplified copy named `<model>~genlod` (`c3l::kMercGenLodSuffix`) with the model's own vertices (same `first_vertex` / `vertex_count`) and draws of its own, aiming at R of the triangles with an error under E of the model's size (meshoptimizer; all draws together, so their seams move alike; eye draws kept, vertices at the eyes locked). No layout change: older runtimes load it as a model nobody asks for. `--gen-lod 0`: none. |
| `--merc-cull-agree R`, `--merc-cull-cos C`, `--merc-cull-open O` | 0.99, 0.2, 0.02 | (AI-assisted) merc back face culling marks (`kMercDrawCullBack` in `palette[30]`, config `cull_merc`): an opaque, depth written draw whose texture never fails the runtime's alpha test is marked when at least R of its triangles have all three corners' normals on one side (cos >= C; each is turned to face that side) and at most O of them have an open edge (the model's draws welded by position: single sheets and holes stay unculled). Envmap passes take their draw's mark; generated LODs never have it. `--merc-cull-agree 0`: no marks. |
| `--no-etc1` | off | 16-bit textures (RGB565 / RGBA4444) instead of ETC1 / ETC1A4. |
| `--etc1-merc` | off | ETC1 for all textures of merc models, without the quality test (faces too). |
| `--etc1-merc-psnr DB` | 34 | (AI-assisted) merc textures become ETC1 / ETC1A4 when they pass the quality test (see "Formats"); 0: they stay 16-bit. |
| `--etc1-merc-block E` | 16 | ... and no 4x4 block has an RMS error above E (0..255) or the 16-bit version's worst. |
| `--etc1-merc-min N` | 64 | ... for merc textures at least N texels wide and high (smaller ones stay 16-bit). |
| `--etc1-quality N` | 1 | ETC1 encoder: 0 fast, 1 medium, 2 high (slow). |
| `--no-mips` | off | no mip levels. |
| `--threads N` | 0 | texture encoding threads (0: all cores). |
| `--etc1-rdo L` | 10 | (v11) ETC1 rate-distortion optimisation strength (lambda), 0: off. See "Compression". |
| `--compress M` | auto | (v11) block compression: `auto` (per block, the fastest to load by the cost model), `lz4`, `zstd`, `none`. |
| `--sd-mbps`, `--lz4-mbps`, `--zstd-mbps` | 5.5, 100, 16 | the cost model: New 3DS SD read speed and decode speeds (MB/s). |
| `--block-kb N` | 64 | largest compressed block (raw KB). |
| `--no-vcache` | off | keep the triangle and vertex order (no vertex welding, no meshoptimizer). |
| `--no-verify` | off | don't read every written file back with the runtime's reader to compare it. |

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

Sizes (defaults, AI-assisted, Oct 3): village1 254k triangles in 626 chunks, 296 textures
(1.2 MB), file 4.9 MB; misty 5.1 MB, citadel 4.2 MB (241 textures, 1.4 MB; before textures were
shared, 1017 and 17.6 MB). All 26 files 98 MB (textures 21 MB).

## Compression (v11)

(AI-assisted.) Level files are read from the SD card (about 5 MB/s on the New 3DS: misty, 16 MB
uncompressed, loaded in 3.1 s), so they are compressed. A v11 file is the v10 layout (the "raw
image"; every offset in it stays an offset in the raw image) cut into blocks of up to 64 KB, each
packed on its own: the 128-byte header (raw), then `BlockDir {"C3LZ", raw_size, num_blocks,
max_block}` and `Block {raw_offset, raw_size, file_offset, packed_size | codec << 28}[]`, then the
packed blocks. Codecs: 0 stored, 1 LZ4 block (`LZ4_decompress_safe`), 2 zstd frame.
`game/graphics/ctr/c3l_reader.cpp` reads any offset range: it reads packed bytes in 128 KB windows
and decodes block by block as they come in, a block the read covers completely straight into the
destination (texture pool, mesh vertex buffers: linear memory, `ctr_gpu_mesh_begin`), a partly
covered one through a one-block cache. The converter starts a block at every piece the loader
reads (each texture, each chunk's vertices and index range, each merc model's vertices) unless the
block is still under 16 KB, and never lets a block span two sections. v7..v10 files still load.

The codec is picked per block by a cost model, the load time on the New 3DS:
`packed / sd_mbps + raw / decode_mbps`. The decode speeds come from instruction counts of the
devkitARM -O2 armv6k decoders (lz4 1.10, zstd 1.5.0) on misty's blocks, run under unicorn:
LZ4 5.1 instructions per output byte, zstd 33.5, at an assumed 1.5 cycles per instruction and
804 MHz: about 100 and 16 MB/s. zstd packs 10-15 % of the raw size smaller than LZ4 HC, which at
5.5 MB/s saves less time than its decode costs, except for index data (LZ4 69 %, zstd 36 %): with
the defaults, indices are mostly zstd, everything else LZ4. The converter logs, per section, the
LZ4 and zstd ratios and what it chose.

`--etc1-rdo`: ETC1 blocks are chosen for compression as well as quality. For each block the
encoder's best block competes with copies of the 64 blocks written before it and with their
colors under new selectors; the one with the lowest squared error + lambda * (estimated LZ bits)
wins. lambda 10: ETC1 texels of misty LZ4 81 % -> 75 %, PSNR 37.3 -> 36.8 dB.

Vertex order: within each chunk identical vertices (after quantizing) are merged (the PS2 strips
share none), the triangles of each visibility run of a draw without blending that writes depth are
put in vertex cache order (meshoptimizer `optimizeVertexCache`; blended draws and decals keep their
order), then the vertices in first-use order (`optimizeVertexFetchRemap`). The GPU's post-vertex
cache is cleared before every draw. Misty: 1.55 -> 1.27 vertices shaded per triangle (FIFO 16).

## Layout

Little endian. All offsets are from the start of the file. Every section is 16-byte aligned.

```
Header (128 bytes)
  char magic[4] = "C3LV"; u32 version = 12 (7..11 still load; v12: merc rewritten vertices); char level_name[32]
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
  float origin[3], scale  world = origin + pos * scale  (game units: 4096 = 1 m), see "Positions"
  u32 first_vertex, vertex_count   (<= 65536, so indices fit in u16)
  u32 first_draw, draw_count
  float max_dist          not drawn further than this from the camera (0 = no limit)
  u32 lod_tier            0 always (tie), 1 near tfrag, 2 far tfrag, 3 lowres tfrag
  float lod_center[3]     tiers 1/2: the grid cell center used to pick one of the two
  u32 vis_runs_offset, vis_run_count   (v10) the chunk's VisRun list
  u32 kind                (AI-assisted) what the chunk holds: 1 tfrag, 2 tie, 3 shrub
                          (CHUNK_KIND_*; 0 = unknown, in files written before it; no version
                          change). A chunk holds one kind only.

Vertex (16 bytes)
  s16 pos[3]              quantized position (see "Positions")
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

- **Positions:** (AI-assisted) `scale` is a power of two (game units) and `origin` a multiple of
  it near the chunk's centre; `pos` uses the signed range -32768..32767. The converter gives each
  chunk the finest power of two that holds it, and puts a position that is in several chunks
  (a surface crossing a cell border, a cell split at 65535 vertices, the two tfrag tiers) on the
  coarsest of their grids first. That grid is a multiple of each chunk's own, so every chunk
  reconstructs the position exactly the same and surfaces stay closed across chunks (checked at
  conversion; the log's `quantizing:` line). Files from before Oct 3 2026 quantized each chunk on
  its own bounding box grid (`pos` 0..32767 from the minimum, scale = extent / 32767): a vertex
  shared by two chunks landed 3-9 mm apart in each, and the background showed through the cracks.
  No chunk's step is coarser than with the old grid (typically 6.3 -> 4 units, 1 mm). The 3DS
  still adds float24 rounding: the runtime folds origin and scale into each chunk's clip matrix,
  truncated to float24 on upload. Simulated for firecanyon's and village1's shared vertices, two
  chunks put them on screen within 0.1 px (median; at most 0.85) with the camera 1 m away,
  0.03 px (at most 0.2) at 3 m and 0.01 px at 10 m, against 0.7 / 0.25 / 0.07 px before; what is
  left are sub-pixel differences up close.
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
  `ctr_gpu_pool_create`). The converter logs the ETC1 PSNR per level file.
  (AI-assisted) Merc textures (characters, objects) of 64x64 and up are ETC1 / ETC1A4 when that
  is about as good as their 16-bit version, measured on the full size level against the image:
  a PSNR of at least 34 dB (about RGBA4's own error) or at least the 16-bit version's, and no 4x4
  block with an RMS error above 16 or the 16-bit version's worst. RGB565 is close to the image
  (~48 dB), so opaque ones need the 34 dB; RGBA4's 4-bit colors band (~34.5 dB), and ETC1A4 is the
  closer one for most textures with alpha. The others stay 16-bit, and so do the textures of
  characters with face animation (effects with blend shapes: the sages, the farmer, Jak...) and
  the eye placeholders. Oct 3 2026, all levels: 695 merc textures tested, 526 ETC1 (16-bit
  equivalent 22.3 MB -> 13.5 MB). The log lists every merc texture with its numbers.
- **Shared textures:** (AI-assisted) the fr3 has a copy of a texture for each merc model that
  uses it (citadel: 1017 textures, 776 of them copies; pal-environment-front 90 times). Identical
  textures (size and texels) are written once and the draws share it: citadel's textures 17.6 MB
  -> 2.1 MB (1.4 MB with the merc ETC1 above), all levels 82 MB -> 21 MB.
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
  u8  palette[31]         palette slot -> skeleton bone (24 used). (AI-assisted) palette[30]:
                          flags, 0 from older converters: 0x01 kMercDrawCullBack (back faces may
                          be culled: wound like the vertex normals, see --merc-cull-agree)
  u8  eye_id              0xff; 0xfe (v9): the envmap pass of an envmapped effect, after its
                          normal draws (same triangles, envmap texture and mode); else the eye
                          texture slot that CtrEyeRenderer draws each frame

MercVertex (24 bytes)
  s16 pos[3]; u8 bones[3] (palette slots); u8 weights[3] (sum 255); s16 st[2] (* 1024);
  u8 rgba[4]; s8 normal[3] (* 127, lighting); u8 pad

Blend shapes: MercBlercHeader {u32 num_vertices, num_targets, num_dests, mod_offset}, then
MercBlercVertex[] {float base[3]; u32 first_target; u16 target_count, dest_count; u32 first_dest},
MercBlercTarget[] {float offset[3]; u32 weight}, u16 dests[] (model vertex indices).

v12 rewritten vertices, at mod_offset (0: none; the blend shape counts may be 0):
MercModHeader {u32 num_effects, num_dests, num_mask_bytes, pad}, then
MercModEffect[] (48 bytes) {u32 model; u8 effect, pad; u16 frag_count; u32 first_mask; u32 vidx_end;
  u32 first_dest, dest_count; u32 st_vif_add; float st_magic, xyz_scale; u32 pad[3]},
MercModDest[] {u16 src (unpacked vertex), u16 vertex (model vertex)}, sorted by vertex,
u8 masks[] (one per fragment: 1 = unpacked).
```

- Draws are split so that each one uses at most 24 bones (the vertex shader's uniform space).
- Blend shapes (faces): each frame the positions of the blend shape vertices are
  `base + sum(weight[target.weight] * target.offset)`, written to their `dest` vertices.
- Envmapped effects (shiny: power cells, precursor metal) get envmap pass draws (v9). The runtime
  draws them with `ctr_skin_env.v.pica` (emerc.vert's sphere map) and the effect's fade color from
  the game, when it isn't 0 (the game fades the shine out with distance).
- Models with more than 65535 vertices are skipped (logged).
- (AI-assisted) v12: vertices the game rewrites in its own merc data every frame (ripple: water
  waves such as the dark eco pools and the jungle river; texture scroll) come with the tfrag3
  `mod` data that Merc2 uses: the effects are split into fix and mod draws like blend shapes, and
  MercModEffect says which fragments to unpack (`fragment_mask`), how many vertices that gives
  (`expect_vidx_end`) and which model vertices each unpacked vertex goes to
  (`vertex_lump4_addr`). Each frame `CtrMercRenderer` copies the fragments' vertex bytes with the
  bone snapshot (game thread) and unpacks positions, normals and texture coordinates into the
  model's vertices on the render thread (`apply_mod`, the mesh's second vertex buffer with the
  pipeline). The model's position scale gets the blend shapes' 1.5x room for the waves. v11 files
  have none: those models stay at their base pose, as before.
