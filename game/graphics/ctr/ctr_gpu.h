#pragma once

/*!
 * @file ctr_gpu.h
 * (AI-assisted)
 * Minimal GPU interface used by the 3DS renderer (game/graphics/ctr). Plain C types only, so the
 * renderer (C++ with common_types.h) never includes <3ds.h> (whose u32 conflicts with ours).
 *
 * Implementations:
 *  - game/graphics/ctr/ctr_gpu_citro3d.c: citro3d on the 3DS
 *  - game/graphics/ctr/ctr_gpu_soft.cpp: small software rasterizer on PC (for testing the renderer
 *    without an emulator; writes PNG frames)
 *
 * Color conventions follow the PS2 GS (the renderer passes GS values through unchanged):
 *  - vertex color: 0x80 = 1.0 for RGB and alpha
 *  - texture color: RGB 0..255, alpha 0x80 = 1.0
 *  - textured, modulate: rgb = tex.rgb * v.rgb / 128, a = (tcc ? tex.a * v.a / 128 : v.a)
 *  - blending and alpha test use alpha with 0x80 = 1.0
 * The target is the 400x240 top screen. Positions: x, y in [-1, 1] cover the whole screen (the
 * game renders for 5:3, see pc_get_window_size), +y up. z in [0, 1], larger = closer (GS
 * convention).
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  float x, y, z;
  float s, t;  // normalized texture coordinates, t = 0 at the top of the image
  uint8_t r, g, b, a;
} ctr_vertex;

enum ctr_blend {
  CTR_BLEND_OFF = 0,
  CTR_BLEND_ALPHA,       // Cs * As + Cd * (1 - As)
  CTR_BLEND_ADD,         // Cs * As + Cd
  CTR_BLEND_SUB,         // Cd - Cs * As
  CTR_BLEND_FIX,         // Cs * fix + Cd * (1 - fix)
  CTR_BLEND_ADD_DST_A,   // Cs * Ad + Cd
  CTR_BLEND_ONE_ONE,     // Cs + Cd
};

enum ctr_test {
  CTR_TEST_NEVER = 0,
  CTR_TEST_ALWAYS,
  CTR_TEST_GEQUAL,
  CTR_TEST_GREATER,
  CTR_TEST_LESS,
  CTR_TEST_LEQUAL,
  CTR_TEST_EQUAL,
  CTR_TEST_NOTEQUAL,
};

typedef struct {
  int tex;            // texture handle, or -1 for untextured
  uint8_t tcc;        // use texture alpha
  uint8_t decal;      // decal instead of modulate
  uint8_t filter;     // bilinear
  uint8_t clamp_s, clamp_t;
  uint8_t blend;      // enum ctr_blend
  uint8_t fix;        // for CTR_BLEND_FIX, 0x80 = 1.0
  uint8_t atest;      // enum ctr_test (CTR_TEST_ALWAYS = off)
  uint8_t aref;       // 0x80 = 1.0
  uint8_t ztest;      // enum ctr_test (CTR_TEST_ALWAYS = off)
  uint8_t zwrite;
} ctr_draw_state;

/* Lifetime. ctr_gpu_init returns 0 on success. */
int ctr_gpu_init(void);
void ctr_gpu_exit(void);

/* One frame: begin (clears to rgb), draws, end (queues the frame for display). */
void ctr_gpu_frame_begin(uint8_t r, uint8_t g, uint8_t b);
void ctr_gpu_frame_end(void);
/* Wait for the next vertical blank (60 Hz pacing). */
void ctr_gpu_wait_vblank(void);

/* Textures from linear RGBA8 data (w, h: powers of two, 8..1024). Returns a handle or -1. */
int ctr_gpu_tex_create(int w, int h, const uint8_t* rgba);
/* The same with mip levels made from it (for textures seen far away, like the ocean's). */
int ctr_gpu_tex_create_mipmapped(int w, int h, const uint8_t* rgba);
/* New texels for a texture from ctr_gpu_tex_create(_mipmapped) (same size), during a frame: the
 * GPU finished the previous frame, and this frame's draws read the new texels. */
void ctr_gpu_tex_update(int handle, const uint8_t* rgba);
void ctr_gpu_tex_delete(int handle);

/* Draw a triangle list. */
void ctr_gpu_draw(const ctr_draw_state* state, const ctr_vertex* verts, int count);
/* Draw quads: 4 vertices each, triangles (0, 1, 3) and (3, 1, 2) (a strip in the order 0 1 3 2).
 * A third fewer vertices than ctr_gpu_draw for sprites. */
void ctr_gpu_draw_quads(const ctr_draw_state* state, const ctr_vertex* verts, int quad_count);

/* ---------------- static meshes (level background, see c3l_format.h) ----------------
 * Vertex layout = c3l::Vertex (16 bytes): s16 pos[3], s16 pad, s16 st[2] (* 1024), u8 rgba[4]
 * (GS units). Textured draws: rgb = tex.rgb * v.rgb / 128, a = tex.a (0xff = 1) * v.a / 128.
 */

/* Texture formats of textures already in the GPU tiled layout (c3l::TextureFormat, plus RGBA8). */
enum ctr_tex_format {
  CTR_TEX_RGB565 = 0,
  CTR_TEX_RGBA4 = 1,
  CTR_TEX_ETC1 = 2,
  CTR_TEX_ETC1A4 = 3,
  CTR_TEX_RGBA8 = 4, /* stored A, B, G, R */
};
/* Bytes of a tiled texture with `levels` mip levels (each half the size of the previous one). */
unsigned int ctr_gpu_tex_bytes(int w, int h, int format, int levels);

/* Texture from texels already in the GPU tiled layout. format: 0 = RGB565, 1 = RGBA4444. */
int ctr_gpu_tex_create_tiled(int w, int h, int format, const void* data, int size);

/* ---------------- texture pools (a level's textures) ----------------
 * All textures of a level in one block of linear memory, with their mip levels. The GPU reads
 * VRAM much faster than linear (FCRAM) memory: at the start of a frame the render thread copies
 * pools to VRAM while there is room (one GPU copy per pool), and moves a pool back out of VRAM
 * when a pool with a higher priority needs the room (the linear copy stays, so that costs nothing
 * but the copy). Pools are created and filled on the level loader thread:
 *   pool = ctr_gpu_pool_create(bytes); write the texels to ctr_gpu_pool_data(pool) at offsets
 *   aligned to 128 bytes; tex = ctr_gpu_pool_tex(pool, offset, ...) for each; ctr_gpu_pool_ready.
 * Textures from a pool are drawn and deleted like others; ctr_gpu_pool_delete after them. */
int ctr_gpu_pool_create(unsigned int bytes);
void* ctr_gpu_pool_data(int pool);
int ctr_gpu_pool_tex(int pool, unsigned int offset, int w, int h, int format, int levels);
void ctr_gpu_pool_ready(int pool);
void ctr_gpu_pool_delete(int pool);
/* Render thread. Higher = VRAM first: a pool only moves another one out of VRAM if its priority
 * is higher. */
void ctr_gpu_pool_set_priority(int pool, int priority);

/* Store RGBA4 textures as RGBA8 (twice the memory): Azahar (OpenGL and Vulkan) renders RGBA4
 * textures as noise or a solid color (render.ini rgba4_as_rgba8, default: on in the emulator). */
void ctr_gpu_set_rgba4_as_rgba8(int on);
int ctr_gpu_rgba4_as_rgba8(void);
/* Texture pools in VRAM while there is room (render.ini vram_textures, default: off in the
 * emulator, which draws VRAM textures as noise). */
void ctr_gpu_set_vram_textures(int on);
/* Mip levels: 0 = off (always the first level), 1 = nearest level (default), 2 = blend two levels
 * (trilinear, slower) (render.ini mipmaps). */
void ctr_gpu_set_mip_mode(int mode);
/* 1 when running in an emulator (Azahar / Citra: svcGetSystemInfo 0x20000). */
int ctr_gpu_is_emulator(void);

/* Copy a vertex / index (u16, triangle list) buffer to GPU memory. Returns a handle or -1. */
int ctr_gpu_mesh_create(const void* verts, int vertex_count, const uint16_t* indices,
                        int index_count);
void ctr_gpu_mesh_delete(int mesh);

/* Draw part of a mesh. clip = row-major 4x4 matrix from (pos.x, pos.y, pos.z, 1) (quantized) to
 * OpenGL-style clip space (x, y in [-w, w] cover the screen, z in [-w, w], near = -w). */
void ctr_gpu_draw_mesh(const ctr_draw_state* state, const float clip[16], int mesh,
                       int first_index, int index_count);

/* ---------------- skinned meshes (merc, see c3l::MercVertex, 20 bytes) ----------------
 * Vertex: s16 pos[3], u8 bones[3] (palette index), u8 weights[3] (0..255), s16 st[2] (* 1024),
 * u8 rgba[4] (GS units). Deleted with ctr_gpu_mesh_delete.
 */
#define CTR_MAX_PALETTE 24
int ctr_gpu_skinned_mesh_create(const void* verts, int vertex_count, const uint16_t* indices,
                                int index_count);
/* A mesh's vertices, to change them (blend shapes) before its draws of the frame; then flush the
 * changed bytes. The GPU finished the previous frame when a frame begins. */
void* ctr_gpu_mesh_vertices(int mesh);
void ctr_gpu_mesh_flush(int mesh, int offset, int size);

/* bones: palette_count 3x4 row-major matrices, (pos, 1) -> camera space (x, y, z).
 * clip: row-major 4x4 from (camera x, y, z, 1) to OpenGL-style clip space.
 * tint: multiplies the final color (lighting approximation), 0..1. */
/* lights: 7 vec4 like merc2.vert: light direction rows 0-2 (light_dir0/1/2), light colors 0-2,
 * ambient. Vertices are c3l::MercVertex (24 bytes, with a normal). */
void ctr_gpu_draw_skinned(const ctr_draw_state* state, const float clip[16], const float* bones,
                          int palette_count, const float lights[28], int mesh, int first_index,
                          int index_count);

/* Fog for ctr_gpu_draw_mesh (see platform/3ds/shaders/ctr_mesh.v.pica), from clip w:
 *   game fog = (255 - clamp(fog0.x - w, fog0.y, fog0.z)) / 255  (fog0.w = -1/255)
 *   distance fog = (w * fog1.x - fog1.y) * fog1.z
 * the larger of the two mixes the color towards (r, g, b). Call after ctr_gpu_frame_begin. */
void ctr_gpu_set_mesh_fog(const float fog0[4], const float fog1[4], uint8_t r, uint8_t g,
                          uint8_t b);

/* Request a screenshot of the next finished frame (written when it is available: after the next
 * ctr_gpu_frame_begin on the 3DS). PNG on PC, BMP on the 3DS. */
void ctr_gpu_request_screenshot(const char* path);

/* Statistics for the last finished frame. */
typedef struct {
  int draws;
  int triangles;
  int textures;
  unsigned int tex_bytes;
  float gpu_ms;     /* GPU time of the frame: command processing (C3D_GetProcessingTime) */
  float draw_ms;    /* and drawing (C3D_GetDrawingTime) */
  int cmd_splits;   /* command buffer flushes in the middle of the frame */
  int dropped_draws; /* draws skipped because the command buffer was full */
  int tex_binds;     /* texture changes (each clears the GPU's texture cache) */
  unsigned int cmd_bytes; /* GPU commands of the frame */
  unsigned int linear_free; /* free linear memory (meshes, textures, vertex ring), bytes */
  unsigned int vram_free;   /* free VRAM (render target, textures), bytes */
  int vram_textures;        /* textures created in VRAM so far */
  int vram_copy_failures;   /* VRAM uploads that didn't land (then in linear memory) */
  unsigned int pool_bytes;      /* texture pools (all in linear memory; see ctr_gpu_pool_create) */
  unsigned int pool_vram_bytes; /* of which also in VRAM (the copy the GPU reads) */
  int pools, pools_in_vram;
} ctr_gpu_stats;
void ctr_gpu_get_stats(ctr_gpu_stats* out);

/* Monotonic time in milliseconds. */
double ctr_gpu_time_ms(void);

/* Asynchronous rendering (the PS2 overlaps DMA/VU1/GS with the next frame's game logic).
 * ctr_gpu_async_start creates a render thread on another CPU core (New 3DS: core 2) that runs
 * fn(arg) for each ctr_gpu_async_submit. Returns 1 if the thread runs, 0 if rendering has to stay
 * synchronous (Old 3DS, PC). All renderer and GPU work must then happen either on the render
 * thread or on the game thread while the render thread is idle (ctr_gpu_async_wait). */
typedef void (*ctr_gpu_job_fn)(void* arg);
int ctr_gpu_async_start(ctr_gpu_job_fn fn, void* arg);
/* Wait for the previous job, then start fn(arg) on the render thread. */
void ctr_gpu_async_submit(void);
/* Wait until the render thread is idle. Returns the milliseconds waited. */
double ctr_gpu_async_wait(void);
void ctr_gpu_async_stop(void);

#ifdef __cplusplus
}
#endif
