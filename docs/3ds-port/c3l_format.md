# C3L: 3DS level background format

(AI-assisted.)

C3L holds a level's static background (tfrag and tie) in a form the 3DS GPU can draw directly.
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
| `--tfrag-geo N` | 2 | tfrag level of detail. 0 is the most detailed, 2 the least. |
| `--tie-geo N` | 3 | tie level of detail. 0 is the most detailed, 3 the least. |
| `--palette N` | 1 | time of day palette baked into the vertex colors (0..7). |
| `--max-tex N` | 128 | maximum texture size. Larger textures are box-filtered down. |
| `--cell M` | 100 | chunk grid size in meters. Triangles go to the cell of their centroid. |
| `--no-tie` | off | leave tie out. |

What goes in:

- tfrag trees of kind NORMAL, TRANS, DIRT, ICE and WATER. LOWRES trees are left out.
- The static draws of every tie tree, with instances already transformed to world space.
- Not converted (yet):
  - wind tie
  - shrub
  - hfrag
  - merc (characters come from the game at runtime anyway)
  - animated textures: draws that use them become untextured

Sizes for village1 (defaults): 105k triangles (tfrag 12k, tie 93k), 161k vertices
(2.5 MB), 111 textures (1.5 MB), file 4.7 MB. Most of it is tie: a lower tie level of detail, or
dropping small props, is the obvious next size cut.

## Layout

Little endian. All offsets are from the start of the file. Every section is 16-byte aligned.

```
Header (96 bytes)
  char magic[4] = "C3L1"; u32 version = 1; char level_name[32]
  u32 num_textures, textures_offset
  u32 num_chunks, chunks_offset
  u32 vertex_data_offset, vertex_data_size      Vertex[]
  u32 index_data_offset,  index_data_size       u16[]
  u32 draw_data_offset,   draw_data_size        Draw[]
  u32 texture_data_offset, texture_data_size    texels of all textures
  u32 pad[2]

Chunk (48 bytes)
  float bsphere[4]        world-space bounding sphere (x, y, z, r) for frustum culling
  float origin[3], scale  world = origin + pos * scale  (game units: 4096 = 1 m)
  u32 first_vertex, vertex_count   (<= 65536, so indices fit in u16)
  u32 first_draw, draw_count

Vertex (16 bytes)
  s16 pos[3]              quantized position, 0..32767 over the chunk's bounding box
  s16 pad
  s16 st[2]               texture coordinates * 1024
  u8  rgba[4]             baked time of day color, GS units (0x80 = 1.0)

Draw (16 bytes)
  u32 mode                tfrag3 DrawMode bits (common/dma/gs.h): blend, alpha test, clamp,
                          depth write, ...
  u16 texture             texture index, 0xffff = untextured
  u16 kind                0 = tfrag, 1 = tie
  u32 first_index, index_count   triangle list; indices are relative to the chunk's first_vertex

Texture (16 bytes)
  u16 w, h                powers of two, 8..max-tex
  u8  format              0 = RGB565, 1 = RGBA4444 (chosen when any texel is transparent)
  u8  pad[3]
  u32 data_offset, data_size
```

- **Texel layout:** the 3DS GPU layout, ready to copy into a `C3D_Tex`. The image is split into
  8x8 tiles, stored in rows of tiles, with Morton order inside each tile. The bottom row of the
  image comes first, because the GPU samples t = 0 from there. `c3l::tiled_index()` gives the
  texel index for image pixel (x, y).
- **Texture colors:** textures come from the fr3 as RGBA8888 with PS2 alpha (0x80 = opaque).
  Alpha is doubled when converting, so 0xff = opaque.
- **Vertex colors:** vertex colors keep GS units. The renderer applies the same `* 2` that the PC
  tfrag shader does.
