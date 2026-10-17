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
| `--tfrag-geo N` | 0 | tfrag level of detail near the camera. 0 is the most detailed, 2 the least (2 has holes). (AI-assisted) 0 since v16, as the PS2 near the camera: 1 cut Volcanic Crater's yellow pools into polygons (village3 +1.1% triangles, +50 KB of vertices). |
| `--far-tfrag-geo N` | 2 | tfrag level of detail for far away cells (-1: no far version). |
| `--tie-geo N` | 3 | tie level of detail (the far version with v17 levels of detail). 0 is the most detailed, 3 the least. |
| `--no-tie-lod` | off | (AI-assisted, v17) no near tie versions: every tie at `--tie-geo` (v16's ties). See "Tie levels of detail". |
| `--tie-near-geo N` | 0 | (v17) geometry of the near tie versions. |
| `--tie-lod-budget-kb K` | per level | (v17) extra vertex and index data for near versions per level; default: the level's planned budget (`kTieLodBudgets` in main.cpp), else every group. |
| `--tie-lod-min-angle A`, `--tie-lod-view-m M` | 0.002, 15 | (v17) a group gets a near version when its shape error seen from the larger of its radius and M meters is at least A radians. |
| `--tie-lod-default-m M` | 60 | (v17) near version distance of groups without a PS2 distance (wind instances). |
| `--palette N` | 1 | time of day palette baked into the vertex colors (0..7): what a v16 runtime shows until its first blend, and older runtimes always. |
| `--no-tod` | off | (AI-assisted, v16) no time of day table: the vertex colors stay the baked palette. See "Time of day". |
| `--max-tex N` | 128 | maximum texture size. Larger textures are box-filtered down. |
| `--cell M` | 200 | chunk grid size in meters. Triangles go to the cell of their centroid. |
| `--detail-radius M`, `--detail-dist M` | 4, 100 | tie instances smaller than the radius only drawn up to the distance |
| `--medium-radius M`, `--medium-dist M` | 12, 300 | same for medium tie instances |
| `--shrub-dist M`, `--shrub-cell M` | 100, 50 | shrubs are drawn up to the distance, in chunks of the cell size |
| `--shrub-one-dist` | off | (AI-assisted) v13 shrubs: all up to `--shrub-dist`. By default (v14) each shrub prototype gets a distance class from its PS2 distances (`tools/ctr_level_converter/shrub_ps2_dists.inc`, from the levels' prototype-bucket-shrub dists: full detail up to x, fading or a billboard up to w): the class nearest (x + w) / 2 of 35, 50, 70, 90, 120, 160, 400 m (70 m for the bushes the PS2 shows as billboards beyond w), shrubs of a cell split into one chunk per class, the class in the chunk's `max_dist` (config `shrub_scale`). Prototypes not in the table: `--shrub-dist`, with a warning |
| `--no-tie`, `--no-shrub`, `--no-merc` | off | leave tie / shrubs / merc models out. |
| `--no-tie-winding` | off | (AI-assisted, v13) keep the strips' triangle order. By default every second strip triangle is flipped (consistent winding), and closed tie surfaces are oriented outward and their draws marked `kDrawCullBack` (config `cull_ties`). |
| `--no-vista` | off | (AI-assisted, v13) no vista tier. By default the far content (low resolution tfrag; ties without a distance limit; the coarse tfrag of levels without low resolution tfrag) is copied into `CHUNK_KIND_VISTA` chunks (lod_tier 4) of `--vista-cell` meters, one draw per texture and mode, each run's `vis` = its source chunk. Never for the interiors (jungleb, maincave, darkcave, robocave, sunken, sunkenb, citadel) |
| `--vista-cell M` | 1500 | vista tier cell size |
| `--vista-flat` | off | (AI-assisted) also an untextured copy of the vista tier: `CHUNK_KIND_VISTA_FLAT` (5) chunks after the `CHUNK_KIND_VISTA` ones (the end of the chunk list), each texture's average color (alpha weighted) baked into the vertex colors (texture * vertex / 128), untextured draws with the draw mode's texture bits (clamp, filter, tcc, alpha test) cleared: one draw per cell and mode (Snowy: 127 -> 2 draws, village3: 146 -> 38). The runtime keeps one of the two (config `vista_untextured`) and drops the other at load. No layout change; an older runtime would draw the copies as ordinary chunks. |
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
| `--vcache-fifo N` | -1 | (AI-assisted, v15) the triangle order inside each visibility run of an opaque draw, for the post-vertex cache (see "Vertex order"): N > 0 for a first-in first-out cache of N entries; -1 (default) per run the best of the orders for 4 and 12 entries and meshoptimizer's, by the vertices a FIFO of 4 + 8 + 16 entries shades; 0 the v14 order (meshoptimizer, without the coplanar rule). |
| `--no-opaque-alpha` | off | (AI-assisted, v15) keep alpha tests and blending that can't change a pixel (see "Draw optimisations"). |
| `--no-atlas`, `--atlas-max N`, `--atlas-kb-per-bind K` | off, 32, 2 | (AI-assisted, v15) texture atlases (see "Draw optimisations"): textures up to N texels a side; an atlas only when the memory it adds is at most K KB per texture bind it saves. |
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
- Shrubs (grass, bushes, small plants, ropes), in `--shrub-cell` chunks drawn up to their PS2
  distance class (v14, see `--shrub-one-dist`) or `--shrub-dist`.
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
(AI-assisted) The 3DS loader uses the reader's window mode (`config.ini c3l_read_buffer`, default
256 KB): the file is opened unbuffered and every read from it is one request of a whole window (or
one exact read of a bigger piece), into one of two windows (the one used longest ago is replaced),
stored blocks and uncompressed files included. Each chunk's indices are then read straight into
its index buffer between its vertex reads, each section keeping its own window.

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
put in vertex cache order (blended draws and decals keep their order), then the vertices in
first-use order (`optimizeVertexFetchRemap`). The GPU's post-vertex cache is cleared before every
draw (citro3d, and the raw draws unless `vcache_reuse`), and the runs of a draw may be drawn on
their own, so each run starts with an empty cache. The draws are indexed triangle lists (no strips:
the PICA200 has no restart index, and a list keeps every triangle's own corner order).
Misty: 1.55 -> 1.27 vertices shaded per triangle (FIFO 16).

(AI-assisted) v15 (`--vcache-fifo`): the cache's size and policy aren't documented
(GPUREG_POST_VERTEX_CACHE_NUM 0x22D, "usually 0x4 or 0x84": 3dbrew, GBATEK; citro3d never writes
it; pica_bench B4 measures it). So each run gets the best of three orders by the vertices a
first-in first-out cache of 4, 8 and 16 entries shades together: `c3l_opt::vcache_order` (a
greedy, Forsyth-style order scored on a simulated FIFO: fewest misses, then the oldest entries'
vertices, then vertices with few triangles left) for 4 and for 12 entries, and meshoptimizer's
(only where no order has to be kept). Triangles of a run that lie in one plane (within 1 mm, ~1
degree) and overlap there keep their order (`c3l_opt::coplanar_pairs`; with the GEQUAL depth test
the later of two equal-depth fragments wins): v14's meshoptimizer order didn't keep it (village1:
1207 such pairs in reordered runs, 852 of them the same three corners; Snowy 2092, village3 1884).
Vertices shaded per triangle, FIFO 4 / 8 / 16 / 32 (each vertex once: the least possible), v14 ->
v15: village1 1.411 / 1.258 / 1.245 / 1.241 -> 1.346 / 1.268 / 1.249 / 1.243 (1.236), Snowy
1.410 / 1.253 / 1.233 / 1.227 -> 1.358 / 1.266 / 1.241 / 1.232 (1.218), village3 1.403 / 1.215 /
1.201 / 1.197 -> 1.354 / 1.222 / 1.207 / 1.199 (1.186): -4% for a 4 entry cache, within 1% of the
v14 order for 8 and more (both near the bound there). The converter logs these per level.

## Draw optimisations (v15)

(AI-assisted) `Draw::kind` flags (c3l_format.h), each one pixel for pixel the draw it replaces
(except the atlas gutters, below):

- **Opaque alpha** (`kDrawAlphaTestDropped`, `kDrawBlendDropped`; config `opaque_alpha`): a draw
  with an alpha test (GEQUAL) or alpha blending (Cs * As + Cd * (1 - As)) whose texture has alpha
  1.0 in every texel of every mip level as the GPU reads it (ETC1, RGB565, or every 4-bit alpha
  0xF) and whose every vertex alpha is >= 0x80 (the combiner's tex.a * v.a * 2 saturates at 1;
  fog_alpha puts the constant 0x80 there) gets alpha 1.0 everywhere: the test passes, the
  blending leaves Cs. The bit is cleared from `mode` (the alpha test's aref stays in the mode for
  `opaque_alpha = off`, which puts the test back). Alpha tested draws of one texture and state then
  merge with the opaque ones of that texture in a chunk where those also have alpha 1.0. Not done
  where a draw's triangles overlap another draw's of the same texture in one plane (the runtime
  sorts opaque draws by texture, then state: the state change would reorder them). Blended ones
  keep their place among the blended draws (`kDrawBlendDropped` draws stay in `ordered_draws`):
  moved to the opaque draws they would be drawn before blended draws that were drawn under them.
  Formats: a texture whose alpha is 1.0 everywhere is already ETC1 / RGB565 (every texel >= 240
  at conversion counts as opaque), so nothing to gain there. All levels: 1094 alpha tested draws
  (per level and state) lose the test (village1 85, Snowy 71, village3 119; 18 kept for overlaps),
  28 merges;
  no level has a blended draw with alpha 1.0 everywhere (the ones with an opaque texture have
  vertex alpha below 0x80, and the other way round).
- **Atlases** (`kDrawAtlasUV`): small textures (`--atlas-max`) whose every level draw is opaque
  (any order), each triangle within one copy of the texture (clamped axes: coordinates 0..1;
  repeating axes: after a whole number shift of the triangle, which a repeating texture doesn't
  see), one clamp mode per axis, and no coplanar overlap with another draw (the sort by texture
  would change), are packed per format and mip level count into atlases (shelves, the smallest
  power of two that holds them; at most 256 << (levels - 1) a side). A cell's position is a multiple
  of 4 << (levels - 1) texels, so every level's 4x4 blocks are the texture's own, copied; between
  cells (and at the edges) a gap of the same size: at each level a gutter of 2 << (levels - 1 -
  level) texels around each cell holds what the texture's own addressing reads beyond its edge
  (repeat: the other side, clamp: the edge texel), encoded again (ETC1: approximately; at the
  smallest level a gutter shares a 4x4 block with the neighbour's). The texture coordinates are in
  1/32768 (`c3l::kAtlasUVScale`): 32768 / atlas side units per atlas texel, at most 256 << (levels -
  1) a side, so `kx * (cell x + (st - shift) * w / 1024)` is a whole number: the exact texel
  position of the cell's own coordinates (st in 1/1024). The level of detail is the same (the
  derivatives scale with the atlas, the size the other way). The vertices of an atlased triangle
  are copies (one per shift a vertex needs). The vista tier's copies keep the original texture
  (welded by position there), as do merc models: those textures stay in the file; the others get
  no texels (`data_size` 0: the runtime makes no texture). An atlas is made only when the memory it
  adds (atlas - the textures it replaces + 16 bytes per vertex copy) is at most `--atlas-kb-per-bind`
  KB per texture bind it saves. Draws of a chunk with the same state merge. The level textures are
  mostly 128 x 128 and repeat across their triangles (tfrag, walls), so few fit: all levels 11
  atlases of 40 textures, 5776 triangles (village1 2 atlases, 14 textures, 1006 triangles; Snowy
  none; village3 1, 4).

## Layout

Little endian. All offsets are from the start of the file. Every section is 16-byte aligned.

```
Header (128 bytes)
  char magic[4] = "C3LV"; u32 version = 18 (7..17 still load; v12: merc rewritten vertices; v13: vista
  tier, kDrawCullBack; v14: shrub distances; v15: Draw::kind flags of "Draw optimisations"; v16: time
  of day; v17: tie levels of detail; v18: a chunk's vertices grouped by visibility id, see "Time of
  day"); char level_name[16]; (AI-assisted, v16) u32 tod_offset,
  tod_color_count, tod_mul_count, (v17) ext_offset (ExtHeader, 0: none; the end of the 32 byte level
  name before: zero)
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
  float max_dist          not drawn further than this from the camera (0 = no limit). (AI-assisted)
                          v14 shrub chunks: their shrubs' PS2 distance class (times config
                          shrub_scale); before v14 every shrub chunk had --shrub-dist
  u32 lod_tier            0 always (tie), 1 near tfrag, 2 far tfrag, 3 lowres tfrag
  float lod_center[3]     tiers 1/2: the grid cell center used to pick one of the two
  u32 vis_runs_offset, vis_run_count   (v10) the chunk's VisRun list
  u32 kind                (AI-assisted) what the chunk holds: 1 tfrag, 2 tie, 3 shrub
                          (CHUNK_KIND_*; 0 = unknown, in files written before it; no version
                          change). A chunk holds one kind only.

Vertex (16 bytes)
  s16 pos[3]              quantized position (see "Positions")
  u16 tod                 (v16) time of day color: < tod_color_count a TodColor, then the TodMuls,
                          0xffff none (before v16: 0, "pad")
  s16 st[2]               texture coordinates * 1024
  u8  rgba[4]             baked time of day color, GS units (0x80 = 1.0)

TodColor (24 bytes, v16)  u8 rgb[8][3]: the color in each of the game's 8 palettes, GS units
TodMul (6 bytes, v16)     u16 color (TodColor); u8 mul[3] (128 = 1.0); u8 pad

Draw (16 bytes)
  u32 mode                tfrag3 DrawMode bits (common/dma/gs.h): blend, alpha test, clamp,
                          depth write, ...
  u16 texture             texture index, 0xffff = untextured
  u16 kind                0 = tfrag, 1 = tie, 2 = shrub (low byte); flags: 0x100 kDrawCullBack (v13),
                          0x200 kDrawAtlasUV, 0x400 kDrawBlendDropped, 0x800 kDrawAlphaTestDropped (v15)
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

## Time of day (v16)

(AI-assisted.) The PS2 and the PC light the level background with 8 palettes per vertex color,
blended every frame with weights from the level's mood (time of day: 2-3 of the day's palettes at
a time; lights: lamps and lava glow, which the game moves every frame). Before v16 the converter
baked one palette (`--palette`, daytime), so night, dusk and dawn were drawn lit like day (and noon
darker than the PC: village1's ground 81 instead of 183).

v16 keeps each vertex's color in the 8 palettes: `TodColor`s for the tfrag, tie and shrub time
of day colors (each once per level), and `TodMul`s for a color times a vertex color (shrubs: their
base color, shrub.vert's `base * tod * 4`; the untextured vista: its texture's average color).
Vertex::tod indexes them. Sizes (TodColor + TodMul): village1 10673 + 10567 (312 KB), jungle
9357 + 40645 (457 KB, the largest; shrubs), village3 7238 + 8902 (221 KB); all levels 5.9 MB of
the files.

**v18 vertex groups.** (AI-assisted.) The converter orders each chunk's vertices by the
visibility id of its runs (`group_vertices_by_vis`): each id's vertices are one range that no other
id's indices use (ids in order of their first run, first use inside; a vertex that several ids use
is copied for each but the first: ~0.4% more vertices; a chunk whose copies would pass 65536
vertices is kept as it was). No layout change: the runtime finds the ranges from the indices
(`tod_make_groups`, sorted by vertex) and treats a chunk whose ranges overlap (older files) as one
group. So the runtime rewrites the time of day colors of the groups drawn (one tie instance, one
tfrag visibility group) and leaves hidden ones until they are drawn. Log: `[ctr] <level>: vertices
by visibility id (v18): N chunks, M vertices copied, K chunks kept as they were` (converter).

The runtime (`CtrLevel.cpp`: `load_tod`, `tod_update`, `tod_recolor`, `tod_finish`) keeps the
palettes some color uses (village3: 7 of 8), and rewrites the chunks' vertex colors in place in
their meshes (the alpha stays; a frame the GPU still draws may see part of the change):

- **Table.** Per entry (a color, then the TodMuls; `Vertex::tod`) its rgb, blended as the PC's
  `interp_time_of_day_slow` (per channel `min(255, sum(w * c) >> 6)`, the game's weights from the
  background camera's itimes, 64 = 1.0), only the palettes whose weight isn't 0. Entries are blended
  when a vertex being written needs them: each has a version stamp, and is current when it is at
  least the last move of its palettes (`lut[mask]`). Exact: 0 vertices drawn differ from the PC's
  blend (host check below) when the rewrites keep up.
- **Groups.** A group is rewritten whole when one of its chunk's day palettes moved since it was
  written (or it has the file's colors); a chunk's light vertices (palettes used by a quarter of
  the colors or fewer: lamps, lava glow; the game moves them every frame) all at once when a light
  palette moved (a list per chunk made at load). Within a budget per frame (all levels): the light
  pass first (up to 70% of 1.2 ms, from where the last frame's stopped), then the groups drawn (from
  where the last frame's budget ran out). What the budget doesn't reach is drawn as it is, a day move
  or two behind; a group 3 moves behind, or with the file's colors, is rewritten at once. While the
  day moves fast (the title screen's attract mode: a move nearly every frame) the group pass gets
  the chunks drawn once per 2 moves (their vertices at the measured ms per vertex, spread over the
  frames 2 moves take). The day's own steps (1/64 every ~40 frames in the game) spread over a few
  frames; a group is one visibility id's vertices, so a chunk can show two steps for a frame or two.
  The rewritten ranges are flushed together after the level's pass (sorted, merged within 64 KB:
  a few `ctr_gpu_flush_range` per frame).
- Config `tod_busy_half` (default on, preset safe: off): while day palettes jump (more than 4/64 in
  a frame) every frame for 8 frames (sunken's cycling lights), they are taken every second frame.
- (AI-assisted, citadel) Config `tod_pulse_step` (default 4, preset safe: 0): a day palette that
  pulses (its weight turned around twice within 256 frames: citadel's palette 1, a mood light
  that 71% of its colors use, moving 1/64 every ~2 frames) is taken only when it moved that many
  64ths from the weight last taken, or once it stood still for 8 frames. Without it every move
  rewrote the drawn vertices (the groups were 3 moves behind every ~6 frames): citadel 3.5-8.7 ms
  of time of day per frame, single frames 13-29 ms.
  A palette found pulsing is remembered per level, so a reload counts it as pulsing at once.
- (AI-assisted, citadel) Config `tod_max_ms` (default 2, preset safe: 0): the most ms of rewrites
  per frame (all levels). Groups 3 moves behind or with the file's colors wait beyond it instead
  of being rewritten at once (the next frame's pass starts from them), the group pass's budget
  stays between 0.8 of it and it, and a day move waits (up to 32 frames; a move of 8/64 or more
  doesn't) until the vertices drawn could be rewritten once per move within 0.7 of it (citadel's
  pulse: taken every ~8-12 frames; on its smoothly lit pixels 2% of the 2-frame steps are 5
  levels or more against 0.1% exact, Azahar screenshots). Not exact: a GPU path (palette 1's
  contribution per vertex, 4 bytes, mixed in the vertex shader with a uniform weight) would be,
  for ~1.7 MB of linear memory in citadel (435k vertices). Firecanyon's lava
  (palettes 4-7, 62% of its colors) and swamp's lightning (palette 2, 53%) move as day palettes;
  without the limit single frames took 14-19 ms.
- A level's meshes are colored as they are read when weights are known for it (the weights it was
  last drawn with, or the last level's with the same day palettes); else they show the baked
  palette until drawn (then rewritten at once).

Heap: the colors (palettes used x 3 bytes), the TodMuls, per entry 9 bytes (mask, rgb, stamp), per
chunk 28 bytes, per group 12, per run 2, and the light lists: village1 600 KB, misty 545 KB, snow
458 KB, sunken 309 KB. On the 3DS a level whose table would leave less than 3 MB of heap free keeps
its baked palette (`[ctr] <level>: no heap for the time of day colors`).

Azahar (Oct 17, New 3DS, tod ms of the render thread per frame, average / most in one frame over
25 s): title 7.5 / 16.7 (29.9 fps; the day moves nearly every frame there), village1 hut 1.9 / 2.2,
Snowy 1.2 / 1.8, Volcanic Crater 0.7 / 1.7, sunkenb 5.6 / 9.5 (its cycling lights recolor about half
of the vertices drawn every second frame), Misty 1.4 / 2.1. A group costs ~80 ns per vertex there.

Logs: `[ctr] <level>: time of day: N colors in P palettes (lights M: K colors), ...` at load;
`[ctr] l0-tfrag: time of day N blends, V vertices recolored T ms per level draw (most ...); G
groups rewritten, C chunks drawn behind; groups drawn behind by at most L day moves (...)` and
`[ctr] time of day ms/frame A, most B in one frame, N frames over 2 ms` with the stats. Host: env
`CTR_TOD_LOG=1` (2: every frame) logs the weights; `CTR_TOD_CHECK=1` (2: a line per frame) checks
every vertex drawn against the PC's blend (stale vertices, the largest error, those more than 4
off); `CTR_TOD_BUDGET_MS` sets the budget. A 3DS build with `CTR_TOD_CHECK_BUILD=2` defined in
CtrLevel.cpp runs the check every 16th frame. The pad script command `tod H` sets the game's clock
(pictures at a time of day).

## Tie levels of detail (v17)

(AI-assisted.) The PS2 has four geometries per tie prototype (`prototype-bucket-tie geometry[4]`)
and picks one per instance and frame by its distance (`draw-inline-array-instance-tie`: the view
depth of the instance's sphere minus its radius against the prototype's `dists`): geometry 0 within
8 m (32 x the near clip distance) or where the instance needs clipping, geometry 1 up to the
mid-plane, geometry 2 up to the far-plane (morphing toward geometry 3 on the way), geometry 3
beyond. The far-planes are 45-90 m for most prototypes (`tools/ctr_level_converter/tie_ps2_dists.inc`,
1188 prototypes, from the decompiler's BSP data). Geometry 1 is geometry 0 in every level, geometry 2
~90% of its triangles, geometry 3 ~30-45%. The PC renderer (Tie3) draws one geometry everywhere
(`lod_tie`, 0: the most detailed); v16 files had geometry 3 everywhere (the village3 warp gate ring a
pentagon).

v17 keeps two versions of an instance group (a prototype in one visibility node of the tie tree, up
to 8 instances; or one wind instance): the near version (geometry 0) and the far version (geometry 3,
or nothing: some prototypes have no geometry 3). Both are in one chunk (the group's triangles go to
the cell of the group's center) and share its vertices: geometry 3's corners are geometry 0
vertices (all of them, every level), so the far version costs only its indices. Each version is a
visibility run of its own, the runs of a draw in the order near, one-version, far (so drawing either
version of all of a chunk's groups is one piece per draw). `ExtHeader` (at Header::ext_offset):
`{"C3LX", size 32, lod_group_count, lod_groups_offset, run_lod_count, run_lod_offset, runs_offset,
lod_error_offset}` (lod_error_offset, AI-assisted, title-popin: `float[lod_group_count]`, each group's
shape error below, game units; 0 in files written before it: the runtime then switches at dist); `TieLodGroup {float bsphere[4]; float dist}` (dist: the prototype's PS2 far-plane, game
units); `RunLod {u16 group; u8 lod; u8 pad}` per VisRun of the file from runs_offset on (lod 0: one
version, 1 near, 2 far). The vista tier copies the far versions.

Which groups: a group whose shape error (the larger one-sided Hausdorff distance between the two
versions, sampled at the triangles' corners and centers) seen from the larger of its radius and
15 m is at least 0.002 rad (half a pixel) is a candidate; nearly all are (geometry 3 is coarse).
Their cost is the extra vertex and index data (near vertices the far version doesn't use, near
indices, ~64 bytes of runs); every candidate is taken unless the level has a budget (`kTieLodBudgets`),
then the most error per byte first. Budgets (Oct 17), planned so that every pair of neighbouring
levels keeps >= 3 MB of linear memory free with the 512 KB vertex stream buffers (2 MB more than
before): citadel 5800 KB, firecanyon 6000, jungle 4500, lavatube 5700, misty 4700, ogre 3300,
rolling 2400, swamp 4900, village1 4500, village2 2500; every group in the others. The converter
logs per level the groups, triangles, cost and the prototypes left coarse (`tie levels of detail:`
lines).

Sizes (Oct 17, vertex + index data added): village3 +1954 KB (every group), snow +8230 KB (every
group), ogre +3268 KB (758 of 1210 candidates), lavatube +5504 KB (2874 of 5772), village2 +2461 KB
(792 of 1110); all levels' files 118 -> 162 MB; conversion of all levels ~60 s.

Runtime (`CtrLevel.cpp`, config `tie_lod`): per level draw, each group's version: with the shape
errors (title-popin, Oct 17), near while the camera is within error x focal / tie_lod_px x
detail_scale of the group's sphere (focal: pixels of the 400-wide top screen per unit of x / depth
from this frame's projection, 256 in gameplay, 161 on the title's wide camera): the far version
only where its error is at most tie_lod_px pixels. Without them (older files), near within dist x
detail_scale (x 0.5..1 with auto_detail: half while the GPU is busy; a group in view keeps the factor
it was decided with until it is out of view). 10% hysteresis; every group every frame, so turning
the camera switches nothing. Why: the PS2 morphs geometry 2 into 3 up to the far-plane, the 3DS
switched 0 against 3 there: village1's shape errors at the far-plane are 3.3 px median, 8.3 px for
one group in ten, 19 px at most (the title camera: up to 19.7 px pops, palms and huts).
A chunk with versioned runs is drawn run by run; a run of the other version is hidden and never
drawn along with its neighbours (the small-gap merge), so the two versions are never on top of each
other. Visibility bits, run culling, static_cmdlists (a version change is a new draw list for one
frame), cull_ties (each version oriented on its own), time of day (the chunk's vertices are both
versions') and impostors (cards show the far version) work as before.

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

## Fine visibility (.c3v)

(AI-assisted.) A sidecar per level, `<level>.c3v` next to `<level>.c3l`, layout in
`game/graphics/ctr/c3v_format.h` (v2), read by `CtrFineVis` (`game/graphics/ctr/CtrFineVis.cpp`)
for `CtrLevels` with config `fine_vis = on` (off by default: none are shipped, see "Results"). It is not part of the .c3l: it is made from the
finished .c3l by `ctr_vis_builder` and keyed to it by a fingerprint of the chunk, draw and
visibility run tables (`c3v::fine_vis_level_hash`: chunk bounds, quantization, vertex and draw
counts, LOD fields, kinds, draw index counts, runs; not file offsets, compression, textures, draw
modes, time of day data or the index / vertex order within a run). A .c3l converted again with other
tables makes the runtime ignore its old .c3v (warning `<level>.c3v not used: made from another
conversion`) and draw the level as without one, until the tool is run again. A v1 file (Oct 12) is
refused the same way.

```sh
# every level, the sidecars into their own folder (all cores; the .fr3 files for the collision)
build/tools/ctr_vis_builder --all out/jak1/c3l --fr3 out/jak1/fr3 --out-dir <dir>
# one level / some levels
build/tools/ctr_vis_builder out/jak1/c3l/maincave.c3l <dir>/maincave.c3v --fr3 out/jak1/fr3
build/tools/ctr_vis_builder --all out/jak1/c3l --only maincave,snow --fr3 out/jak1/fr3 --out-dir <dir>
# stage them with the levels
platform/3ds/tools/stage_sd.sh ... --c3v <dir>
```

After a reconvert of the .c3l files that changes any of the tables above (any converter change of
chunking, draws or runs), run `--all` again on the new folder; one that only changes what isn't
in the fingerprint keeps the old files valid (the runtime checks at load). Levels where it finds
nothing to do (no units, floors or occluders: GAME, jungleb) get no file (an old one is removed).
Time: ~16 ms per cell per core (one sample per cell): Precursor Basin (maincave, 328k cells) 10
min on 10 cores; all levels at the defaults ~20.6M cells, ~8-9 h on 11 cores. Size: maincave 37
MB with `--block-kb 32` (13 MB with 8, which cuts a fifth as much); all levels ~2.3 GB / ~0.8 GB.

**Units**: the chunks before the vista chunks (vista tiers are never hidden by it); a chunk's
units are its visibility runs, or (chunks without) its draws in pieces of 288 indices: the pieces
run_culling makes at load. tfrag, ties (both versions of a tie LOD group: each its own runs) and
shrubs; merc (Jak, doors, platforms, anything that moves) is never hidden and never hides.

**Cells**: `--cell` meters (2) where the camera can be: within `--fine-above` (10) meters over /
`--below` (2) under a floor, `--fine-spread` (6) meters sideways; further up (to `--above`, 20)
and out (`--coarse-spread`, 10) cells of twice the size (a 4 m cell with any 2 m cell in it is
cut into 2 m cells). Floors: the level's collision (`--fr3`: its .fr3, or a folder of them; by
default `../fr3/<level>.fr3` from the .c3l folder), triangles steeper than 60 degrees left out; no
.fr3 (or `--draw-floors`): roughly horizontal tfrag / tie triangles (more cells: unreachable
scenery too). One sample per cell, at its center: every point of the cell is within eps = half
its diagonal (1.75 m / 3.48 m). (The 9 samples of a body-centered lattice, eps 0.56 cell, gave
the same sets per cell with twice the samples: their union adds up each sample's uncertainty.)

**Occlusion** (conservative, CPU): from each sample, the level's own opaque static geometry within
`--occ-range` (250) m into a cube of 64x64 depth maps, in four layers (the nearest occluder beyond 1.5, 4, 8, 16
eps). An occluder is a draw that's opaque (no blending), depth tested (GEQUAL) and written, with
no alpha test that can fail (GEQUAL whose reference the texture's smallest alpha passes, at the
GPU's 2 x scale, and vertex alpha >= 0x80), and only from where the runtime draws it for certain
from every point within eps of the sample:
- tfrag: its LOD tier: the detailed one (1) only within 0.9 x `--lod-lo` (180 m) of its
  lod_center, the coarse one (2) only beyond 1.5 x `--lod-hi` (220 m): a detailed cell in view is
  held to 1.5 x lod_distance (title-popin), so the coarse tier isn't certain before;
- every chunk's own distance limit at the smallest factors the sets are for: `--detail-min` (1)
  x detail_scale, ties x `--tie-scale-min` (1) tie_distance_scale, shrubs (v14 PS2 distances) x
  `--shrub-scale-min` (0.8) shrub_scale;
- ties: only nearer than `--tie-occ-max` (120 m, impostor_distance: beyond it a group may be a
  card); the versions of a tie LOD group only where the switch (CtrLevel.cpp: near while the
  camera is within shape error x k + radius, k = detail_scale x focal / tie_lod_px) picks that one
  for every k in `--tie-k-lo` .. `--tie-k-hi` (58 .. 70; gameplay: 1 x 256 / 4 = 64); kRunLodAlways
  runs always;
- closed tie surfaces (kDrawCullBack, which cull_ties may cull): only triangles whose front side
  (counter-clockwise) faces every point within eps.
A camera moved by up to eps sees at most asin(eps / d) around a direction at depth d: a pixel
hides only what is further than the largest depth within asin(eps / dmin) of it (dmin: the
nearest depth there, iterated), +2% + 0.25 m; a window with an empty direction hides nothing.
The windows' pixel rectangles come from a table (per face, 2x2 block, and 64 radius steps, each
rounded up). A unit is hidden from a sample when every pixel its box (then each of its triangles'
boxes) covers hides it; a chunk's box first. A cell's set is what any of its samples may see. The
one case this can't see: a hole in a far wall right behind a near object, seen by none of the
cell's 9 samples.

**Blocks**: the cells in blocks of `--block` (8) cells (16 m), each block on its own in the file
(`BlockEntry` index, then the blocks): the block's distinct sets as a tree, each set the units
where it differs from its parent (the root: from nothing), a spanning tree of the fewest
differences rooted at its center; a cell map (runs of cells with the same set). Sets are merged
(their union: conservative) only in a block over `--block-kb` (32 KB). The tool decodes every
cell of every block again before it writes the file.

**Runtime** (`CtrFineVis`): at the level's load, the header and the block index (8 bytes per block)
and a pool of `fine_vis_kb` (64) KB per level. Each level draw (render thread): the camera's block
and the blocks within 8 m of the camera are asked for; a loader thread of its own (priority 0x3B,
core 2 on New 3DS, a 32 KB stack) reads each with one unbuffered SD card read into the pool, the
blocks wanted least recently making room (never one being read). The cell's set is decoded when
the camera enters a cell with another set (its path in the block's tree), then per covered chunk
whether it hides none / all / some of its units. No block yet (the first frames in a new area),
the camera outside every cell (cutscene and title cameras high up, warps), far mode, an
incomplete load, draw_distance < 250 m + 2 eps, lod_distance x 0.9 < 162 m or x 1.5 > 330 m (so
180 .. 220 m), detail_scale / tie_distance_scale / shrub_scale below the tool's, impostors with
impostor_distance < 120 m, cull_ties = flip, or the tie LOD factor outside the tool's range:
nothing more hidden that frame. Log every stats period: `[ctr] <level>: fine vis: N of M level
draws with a set (W waiting for a block, O outside the cells, U not usable); C cells entered, D
sets decoded (ms); B blocks read (KB, ms each); ... ms per level draw` and `[ctr] l0-tfrag: fine
vis hid N chunks, M tris per level draw with a set`.

Host checks (`--ctr-gfx`): `CTR_FINE_VIS_CHECK=1` draws what fine_vis hid again at the end of each
frame against its depth and counts the pixels that would have been seen (log `[ctr] fine_vis
check:`; any pixel is a culling error); `CTR_FINE_VIS_ORACLE=1` reports how many drawn triangles
were in runs with no pixel seen.

**Results** (Oct 17, finer-vis; c3l v18). The sets are conservative over every view direction and
every point of a cell, which is why they hide little next to what a view hides (the host oracle:
50-85% of the tfrag drawn at the maincave start has no visible pixel):
- triangle weighted share of the level that may be seen, maincave: 93% per 4 m cell, 80% per 2 m
  cell as sampled; in the file 88% (32 KB blocks) / 96% (8 KB);
- per frame, over three maincave checkpoints with camera turns (host): 4 m cells hide nothing,
  2 m cells 2.2k of ~43k triangles (5%) with 32 KB blocks (37 MB), 0.4k (1%) with 8 KB (13 MB);
- open levels lower. So no .c3v files are shipped and `fine_vis` is off by default; the
  streaming runtime costs ~0.003 ms per level draw, a set decode 0.02 ms (host), a block read
  ~0.3 ms of SD card time, 1-8 blocks per second while moving.
`occ_cull` (config_dev.md, CtrOccCull.h) is the exact per-view alternative: it hides far more
(maincave start in Azahar: 16.6k triangles), but its CPU cost on the 3DS is larger than the GPU
time it saves (24.5 ms for that; 6.4 ms for 3.1k with fewer occluders), so it is off too.
