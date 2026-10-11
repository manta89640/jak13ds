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
  // (AI-assisted) Cs * As + clear color * (1 - As), drawn opaque: alpha blending over a part of
  // the screen that still holds the frame's clear color (the sky), without reading the framebuffer
  CTR_BLEND_OVER_CLEAR,
  // (AI-assisted) Cs * Cd (rgb, Cs 0..1: 0xff = 1), the destination alpha kept: darkens (shadows)
  CTR_BLEND_MUL,
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
  uint8_t tcc;        // use texture alpha (bit 0); (AI-assisted) bit 1: CTR_TCC_CULL_BACK
  uint8_t decal;      // decal instead of modulate
  uint8_t filter;     // bilinear
  uint8_t clamp_s, clamp_t;
  uint8_t blend;      // enum ctr_blend
  uint8_t fix;        // for CTR_BLEND_FIX, 0x80 = 1.0
  uint8_t atest;      // enum ctr_test (CTR_TEST_ALWAYS = off)
  uint8_t aref;       // 0x80 = 1.0
  uint8_t ztest;      // enum ctr_test (CTR_TEST_ALWAYS = off)
  uint8_t zwrite;
  uint8_t flags;      // CTR_STATE_* (0 = none)
} ctr_draw_state;

/* (AI-assisted) ctr_draw_state flags: what a draw allows the 3DS GPU to do differently */
enum {
  /* opaque, depth tested GEQUAL and written, never discarded by the alpha test (its texture's
   * alpha passes): may use the early depth test (ctr_gpu_set_early_depth) */
  CTR_STATE_EARLY_DEPTH = 1,
  /* the texture is radial (ctr_gpu_tex_radial) and the texture coordinates go from -1 to 1 across
   * it (center 0): drawn with the procedural texture unit instead of reading texels */
  CTR_STATE_PROCTEX = 2,
  /* (AI-assisted) the stencil buffer (only when ctr_gpu_has_stencil): STENCIL_SET marks the pixels
   * the draw writes (where it passes the depth test), STENCIL_OUT draws only where none is marked.
   * The marks are cleared with the depth buffer at the frame start. The ocean's near grid marks,
   * its far mesh stays out (config ocean = waves). */
  CTR_STATE_STENCIL_SET = 4,
  CTR_STATE_STENCIL_OUT = 8,
  /* (AI-assisted) shadow volumes (CtrShadow, only when ctr_gpu_has_stencil): STENCIL_INCR / DECR
   * add / subtract 1 (saturating at 255 / 0) where the draw passes the depth test; STENCIL_CLEAR
   * draws only where the stencil isn't 0 and sets it back to 0 there (the depth test always
   * passes: set ztest ALWAYS, zwrite 0); NO_COLOR writes no color (the stencil passes). None of
   * them may use the early depth test. */
  CTR_STATE_STENCIL_INCR = 16,
  CTR_STATE_STENCIL_DECR = 32,
  CTR_STATE_STENCIL_CLEAR = 64,
  CTR_STATE_NO_COLOR = 128,
};
/* (AI-assisted) ctr_draw_state::tcc bit: back faces of this level mesh draw may be culled (c3l v13
 * kDrawCullBack). ctr_gpu_set_cull (each frame, config cull_ties): whether they are, and which
 * screen winding is "back" (flip 0: the c3l convention, counter-clockwise front faces seen from
 * outside). ctr_gpu_cull_reset: culling off again (after the level draws; draws that don't go
 * through the state code keep the GPU's last face culling setting). */
enum { CTR_TCC_CULL_BACK = 2 };
void ctr_gpu_set_cull(int enable, int flip);
void ctr_gpu_cull_reset(void);
/* (AI-assisted) 1 if the depth buffer has a stencil part (not config.ini depth16) */
int ctr_gpu_has_stencil(void);

/* Lifetime. ctr_gpu_init returns 0 on success. */
int ctr_gpu_init(void);
void ctr_gpu_exit(void);

/* One frame: begin (clears to rgb), draws, end (queues the frame for display). */
void ctr_gpu_frame_begin(uint8_t r, uint8_t g, uint8_t b);
void ctr_gpu_frame_end(void);
/* Wait for the next vertical blank (60 Hz pacing). */
/* at most 60 / min_vblanks frames per second (render.ini / config.ini fps_cap) */
void ctr_gpu_wait_vblank(int min_vblanks);

/* Textures from linear RGBA8 data (w, h: powers of two, 8..1024). Returns a handle or -1. */
int ctr_gpu_tex_create(int w, int h, const uint8_t* rgba);
/* The same with mip levels made from it (for textures seen far away, like the ocean's). */
int ctr_gpu_tex_create_mipmapped(int w, int h, const uint8_t* rgba);
/* New texels for a texture from ctr_gpu_tex_create(_mipmapped) (same size), during a frame: the
 * GPU finished the previous frame, and this frame's draws read the new texels. */
void ctr_gpu_tex_update(int handle, const uint8_t* rgba);
void ctr_gpu_tex_delete(int handle);
/* (AI-assisted) The same as ctr_gpu_tex_create (GS alpha: 0x80 = 1.0), stored in the smallest
 * 3DS format that keeps it (config.ini compact_textures): gray -> L8 / LA8, opaque -> RGB565,
 * on/off alpha -> RGBA5551, else RGBA4 (RGBA8 when rgba4_as_rgba8 or alpha above 0x80), and
 * moved to a reserved part of VRAM when there's room (vram_textures). flags: CTR_TEX_CHECK_RADIAL
 * also checks whether it is a radial gradient (glows: see CTR_STATE_PROCTEX). */
enum { CTR_TEX_CHECK_RADIAL = 1 };
int ctr_gpu_tex_create_compact(int w, int h, const uint8_t* rgba, int flags);
/* 1 if the texture can be drawn with CTR_STATE_PROCTEX (radial, and config.ini proctex_glows). */
int ctr_gpu_tex_radial(int handle);

/* Draw a triangle list. */
void ctr_gpu_draw(const ctr_draw_state* state, const ctr_vertex* verts, int count);
/* A vertex in OpenGL-style clip space: x, y in [-w, w] cover the screen like ctr_vertex's [-1, 1],
 * depth (z / w + 1) / 2, larger = closer, like ctr_vertex z. */
typedef struct {
  float x, y, z, w;
  float s, t;  // like ctr_vertex (divided by w on the way: perspective correct)
  uint8_t r, g, b, a;
} ctr_clip_vertex;
/* Draw a triangle list in clip space (the sky): the GPU clips it. State like ctr_gpu_draw. */
void ctr_gpu_draw_clip(const ctr_draw_state* state, const ctr_clip_vertex* verts, int count);
/* (AI-assisted) Indexed triangle lists of ctr_clip_vertex (like ctr_gpu_draw_clip: the GPU clips),
 * written in place into this frame's vertex memory: ctr_gpu_clip_reserve gives room for max_verts
 * vertices and max_indices u16 indices (0 when there is none: nothing reserved). Then
 * ctr_gpu_clip_draw_ranges draws the n index ranges [first[i], first[i] + count[i]) (of the first
 * index_count indices, which index the first vert_count vertices), each with its own state, all of
 * them or none (the shadow volumes' passes leave the stencil at 0 only together), and gives back
 * the rest of the room (n = 0: all of it). Nothing else may be drawn between the two calls. */
int ctr_gpu_clip_reserve(int max_verts, int max_indices, ctr_clip_vertex** verts,
                         uint16_t** indices);
void ctr_gpu_clip_draw_ranges(const ctr_draw_state* states, const int* first, const int* count,
                              int n, int vert_count, int index_count);

/* (AI-assisted) Two texture layers in one pass (the sky's two cloud layers): like
 * ctr_gpu_draw_clip, with a second set of texture coordinates for the same texture on texture
 * unit 1. Color added to the screen (state.blend is ignored: Cs + Cd):
 *   (tex(st0).rgb * const0.rgb + tex(st1).rgb * const1.rgb) * vertex alpha
 * (consts and vertex alpha in GS units: 0x80 = 1.0, R in the low byte of the consts; the
 * texture's rgb already multiplied by its alpha). Depth and alpha tests off. */
typedef struct {
  float x, y, z, w;
  float s0, t0, s1, t1;
  uint8_t r, g, b, a;
} ctr_clip_vertex2;
void ctr_gpu_draw_clip2(const ctr_draw_state* state, uint32_t const0, uint32_t const1,
                        const ctr_clip_vertex2* verts, int count);

/* The screen as a texture, for effects that read what is already drawn (the sprite distorter):
 * copies the frame drawn so far into a texture (one GPU copy of the color buffer) and returns its
 * handle, -1 if not available. Valid until the end of the frame; copy again after more draws.
 * ctr_gpu_screen_uv: the texture coordinates of the screen point at ctr_gpu coordinates (x, y)
 * (like ctr_vertex: x, y in [-1, 1], y up). Use it with tcc = 0 (the copy's alpha is meaningless). */
int ctr_gpu_copy_screen(void);
/* (AI-assisted) config distort_copy = rows: copies only the screen columns x0..x1 (ctr_gpu x, -1
 * left .. 1 right; the color buffer's tile rows are 8 screen columns each) with a texel of margin
 * for the filter, and makes the texture in VRAM if there is room then. Texels outside that range
 * keep what an earlier copy left: only read the screen in x0..x1. */
int ctr_gpu_copy_screen_part(float x0, float x1);
void ctr_gpu_screen_uv(float x, float y, float* s, float* t);

/* Draw quads: 4 vertices each, triangles (0, 1, 3) and (3, 1, 2) (a strip in the order 0 1 3 2).
 * A third fewer vertices than ctr_gpu_draw for sprites. */
void ctr_gpu_draw_quads(const ctr_draw_state* state, const ctr_vertex* verts, int quad_count);
/* (AI-assisted) Draw a triangle strip: count vertices, count - 2 triangles (k - 2, k - 1, k).
 * Several strips go in one draw joined by repeating the last vertex of one and the first of the
 * next (the triangles in between have no area). State like ctr_gpu_draw. */
void ctr_gpu_draw_strip(const ctr_draw_state* state, const ctr_vertex* verts, int count);

/* (AI-assisted) The PS2 ocean (CtrOceanFull, config.ini ocean = full): vertices as the ocean's VU1
 * programs send them to the GS. x, y: screen position in GS units (12.4 fixed point, the screen
 * centered at 2048 * 16; Jak 1's 512 x 224 field like CtrDirect), z: GS depth (24 bits, larger =
 * closer), q: the GS Q (1 / w, > 0); s, t: the GS S, T (texture coordinates times q: the GPU
 * divides them per pixel, perspective correct like the GS); fog: the amount of fog color, 0..1
 * (the GS's (255 - F) / 255); rgba: GS vertex color. 32 bytes. Colors like ctr_gpu_draw_mesh:
 * textured rgb = tex.rgb * v.rgb / 128, a = tex.a (0xff = 1) * v.a / 128, then the fog. */
typedef struct {
  float x, y, z, q;
  float s, t, fog;
  uint8_t r, g, b, a;
} ctr_ocean_full_vertex;
/* Room for up to max_verts vertices and max_indices u16 indices (a triangle list) in this frame's
 * vertex memory, written in place (no copy), then drawn with ctr_gpu_ocean_full_draw: one draw, nothing
 * else drawn in between. Returns 0 when there is no room (then nothing is reserved). */
int ctr_gpu_ocean_full_reserve(int max_verts, int max_indices, ctr_ocean_full_vertex** verts,
                          uint16_t** indices);
/* Draw the first vert_count / index_count of the reservation (0 indices: just release it), with
 * the fog towards (fog_r, fog_g, fog_b) (the GS FOGCOL). */
void ctr_gpu_ocean_full_draw(const ctr_draw_state* state, int vert_count, int index_count,
                        uint8_t fog_r, uint8_t fog_g, uint8_t fog_b);
/* (AI-assisted) Like ctr_gpu_ocean_full_draw, for vertices in clip space (CtrOceanFull's native
 * ocean programs, which don't divide by w or clip): (x, y, z, q) = W * (GS x, GS y, GS z, 1) for a
 * W > 0 in front of the camera (the GPU clips), s, t = the texture coordinates (not times Q). */
void ctr_gpu_ocean_full_draw_clip(const ctr_draw_state* state, int vert_count, int index_count,
                                  uint8_t fog_r, uint8_t fog_g, uint8_t fog_b);

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
 * textures as noise or a solid color (config.ini rgba4_as_rgba8, default: on in the emulator). */
void ctr_gpu_set_rgba4_as_rgba8(int on);
int ctr_gpu_rgba4_as_rgba8(void);
/* Texture pools in VRAM while there is room (config.ini vram_textures, default: on; was off in the
 * emulator, which draws VRAM textures as noise). */
void ctr_gpu_set_vram_textures(int on);
/* Mip levels: 0 = off (always the first level), 1 = nearest level (default), 2 = blend two levels
 * (trilinear, slower) (config.ini mipmaps). */
void ctr_gpu_set_mip_mode(int mode);
/* (AI-assisted) 3DS hardware features (config.ini). color16: a 16-bit (RGB565) color buffer and
 * top screen, set before ctr_gpu_init. early_depth: the early depth test for draws with
 * CTR_STATE_EARLY_DEPTH. proctex: radial textures drawn with the procedural texture unit.
 * compact: ctr_gpu_tex_create_compact picks small formats (off: RGBA8 like ctr_gpu_tex_create). */
void ctr_gpu_set_color16(int on);
void ctr_gpu_set_early_depth(int on);
/* (AI-assisted) config.ini early_depth modes (pica_bench B8 decides): sky = the sky's draws
 * (sky_last: depth 0, after the opaque geometry) use the early depth test too; func = the early
 * test's function (GPU_EARLYDEPTHFUNC: 0 >=, 1 >, 2 <=, 3 <; the depth test stays >=); clear =
 * the early depth data's clear value (GPUREG_EARLYDEPTH_DATA, 24 bits; every early depth clear
 * uses it). Defaults 0, 0, 0 = the old state. Any time. */
void ctr_gpu_set_early_depth_params(int sky, int func, unsigned int clear);
/* (AI-assisted) the top render target, set before ctr_gpu_init (config.ini): block32 = 32x32 block
 * mode, 256x416 cropped to the screen (early_depth = block32; needs steady pacing, else the usual
 * target with early depth off), depth16 = a 16-bit depth buffer instead of 24-bit + stencil. */
void ctr_gpu_set_framebuffer(int block32, int depth16);
/* (AI-assisted) one line about the top render target (size, formats, VRAM addresses and banks)
 * for the log; returns its length (0 before ctr_gpu_init) */
int ctr_gpu_describe_target(char* out, int size);
/* (AI-assisted) The GPU's own counters (config.ini gpu_profile, gpu_counters), read through the
 * GSP service: the hotspot sampler (which of 8 pipeline stages is busy, sampled every 65536 GPU
 * cycles; its 16-bit counts restart after 65535 samples = 16 s) and the memory traffic counters
 * at 0x10400070.. (VRAM A/B, main memory, vertex arrays, texture cache misses, depth / color
 * buffer reads and writes, LCD, display transfer, memory fill; the unit is undocumented: the
 * pica_bench B1 lines calibrate it). begin returns 0, or the GSP's error (then never again: it
 * returns 1). stop_stages ends the sampling early (reads its result; the traffic goes on): keep
 * the stage window under 16 s. end fills the counts since begin; returns 0, or the GSP's error. */
#define CTR_GPU_HW_TRAFFIC 20
typedef struct {
  unsigned int stage[8]; /* samples: arrays, vertex shader, primitive setup, rasterizer, texture
                          * reads, lighting, combiners, framebuffer */
  unsigned int traffic[CTR_GPU_HW_TRAFFIC]; /* counter deltas, 0x10400070 + 4 * i */
  int frames;                               /* frames begun since begin */
  double ms;
  double stage_ms; /* the stage samples' window (stop_stages, else ms) */
} ctr_gpu_hwprof;
int ctr_gpu_hwprof_begin(void);
int ctr_gpu_hwprof_stop_stages(void);
int ctr_gpu_hwprof_end(ctr_gpu_hwprof* out);
/* (AI-assisted) config.ini frame_pacing: steady (1, set before ctr_gpu_init) shows every frame
 * a fixed number of vblanks after the game thread handed it over, so frames stay on screen for as
 * long as the game time they cover; fast (0): as soon as the GPU is done (citro3d). */
void ctr_gpu_set_steady_pacing(int on);
/* (AI-assisted) the game thread hands over a frame now (its release vblank, for steady pacing) */
void ctr_gpu_note_release(void);
/* (AI-assisted) since the last call: frames shown for 1, 2, 3, 4, 5+ vblanks, the presentation
 * latency in vblanks (-1: steady pacing off), frames skipped and the render thread's total wait
 * for a free screen buffer; just_in_time: frames the GPU finished in the vblank before their own
 * (no vblank to spare), floor: the latency it doesn't go below for now (a frame was late there) */
void ctr_gpu_pacing_stats(unsigned int hist[5], int* latency, unsigned int* dropped,
                          double* buffer_wait_ms, unsigned int* just_in_time, int* floor);
void ctr_gpu_set_proctex(int on);
void ctr_gpu_set_compact_textures(int on);
/* (AI-assisted) Overlap: hand the draws so far to the GPU in the middle of a frame (after a
 * renderer bucket, when enough draws piled up), so the GPU draws them while the CPU builds the
 * rest of the frame. Off: the GPU starts at the end of the frame (config.ini overlap). */
void ctr_gpu_set_overlap(int on);
/* (AI-assisted) config.ini raw_cmds: the draws write their GPU commands themselves instead of
 * through citro3d (much less CPU per draw, fewer command words). vcache_reuse: consecutive draws
 * of the same vertices and matrices keep the GPU's post-vertex cache (only with raw_cmds). Both
 * can change at any time. */
void ctr_gpu_set_raw_cmds(int on);
void ctr_gpu_set_vcache_reuse(int on);
/* (AI-assisted) hardware hang bisect (config.ini new_shaders / asm_tex, CtrSettings.h).
 * new_shaders 0: the Oct 2 vertex shaders and their uniform conventions; before ctr_gpu_init.
 * asm_tex 0: the old per texel texture tiling loops (no LDM/STM, no 64-bit stores). */
void ctr_gpu_set_new_shaders(int on);
void ctr_gpu_set_asm_tex(int on);
/* (AI-assisted) config.ini defer_flush (hang bisect): 0 = every cache flush right away */
void ctr_gpu_set_defer_flush(int on);
/* (AI-assisted) config.ini shader_basic / shader_mesh (hang bisect), -1 = follow new_shaders */
void ctr_gpu_set_shader_modes(int basic, int mesh);
/* (AI-assisted) config.ini shader_skin: 1 new, 0 the Oct 2 ctr_skin / ctr_skin_env (the same
 * uniforms), -1 = new_shaders; before ctr_gpu_init */
void ctr_gpu_set_shader_skin(int skin);
int ctr_gpu_asm_tex(void);
/* (AI-assisted) where the render thread is (a static string) and its frame count; logged once
 * per second for the first 60 s by a watchdog thread (ctr_gpu_diag_start), so a hang shows the
 * stage it stopped in */
void ctr_gpu_diag_stage(const char* stage);
void ctr_gpu_diag_start(void);
void ctr_gpu_submit_partial(void);
/* (AI-assisted) Pipeline: build frame N+1 while the GPU draws frame N (config.ini pipeline).
 * Outside of a frame only. Returns whether it's on (needs ~3.5 MB of linear memory). */
int ctr_gpu_set_pipeline(int on);
/* (AI-assisted) config.ini sky_last. ctr_gpu_set_sky_hook: fn(arg) is called once, right before
 * the first draw of this frame that needs what is behind it on screen: blending, no depth write,
 * stencil or no-color draws, a screen copy, a clip / ocean reservation (only opaque depth-writing
 * draws let it wait), or at ctr_gpu_run_sky_hook / the frame end, whichever comes first. The
 * renderer draws the sky there (after the opaque level and models). fn = NULL: none. The frame
 * begin drops a hook still set. */
typedef void (*ctr_gpu_hook_fn)(void* arg);
void ctr_gpu_set_sky_hook(ctr_gpu_hook_fn fn, void* arg);
void ctr_gpu_run_sky_hook(void);
/* (AI-assisted) draws from now on get fragment depth 0 (the farthest: the cleared depth) instead
 * of their own (the depth map's scale 0, offset 0): with ztest GEQUAL they draw only where nothing
 * has drawn yet (the sky after the opaque geometry) */
void ctr_gpu_set_depth_far(int on);
/* (AI-assisted) config.ini skip_clear: the next frame begin clears only the depth buffer (the
 * renderer knows every pixel's color gets drawn: the opaque sky roof covers the screen) */
void ctr_gpu_set_skip_color_clear(int on);
/* (AI-assisted) config.ini frame_overlap2 (with the pipeline): 0 off = the frame end waits for
 * the GPU, then hands the frame's command list over with a cache flush (the old way); 1 clean =
 * the list is cleaned from the CPU cache before that wait and handed over without the flush (the
 * GPU's idle gap between two frames is shorter); 2 on = no wait at the frame end: the list is
 * queued behind the frame the GPU is drawing (the next frame begin waits for the frame before
 * that instead, whose buffers it reuses). 2 needs frame_pacing = steady (else 1). Any time. */
void ctr_gpu_set_frame_overlap2(int mode);
/* 1 when running in an emulator (Azahar / Citra: svcGetSystemInfo 0x20000). */
int ctr_gpu_is_emulator(void);

/* Copy a vertex / index (u16, triangle list) buffer to GPU memory. Returns a handle or -1. */
int ctr_gpu_mesh_create(const void* verts, int vertex_count, const uint16_t* indices,
                        int index_count);
void ctr_gpu_mesh_delete(int mesh);
/* (AI-assisted) The same, filled in place (a level loader decodes its vertices straight into GPU
 * memory): begin returns a handle (or -1) and the buffers to write vertex_count * 16 bytes and
 * index_count indices to; end makes it drawable (ok) or frees it (!ok) and returns the handle or
 * -1. Not drawn or deleted between the two. */
int ctr_gpu_mesh_begin(int vertex_count, int index_count, void** verts, uint16_t** indices);
int ctr_gpu_mesh_end(int mesh, int ok);
/* (AI-assisted) Free the deleted textures, pools and meshes now instead of at the next frame
 * begin, once the GPU has finished what it was given (a level was unloaded and the next one loads
 * on another thread: with no frames drawn meanwhile its memory stayed taken). Only while no frame
 * is being built (the render thread idle). */
void ctr_gpu_free_pending_now(void);

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

/* The envmap pass of a skinned mesh draw (emerc.vert): texture coordinates from the camera space
 * normal and position (a sphere map), color = texture * fade * 2 (fade: rgba, 1.0 = GS 0x80). */
void ctr_gpu_draw_skinned_env(const ctr_draw_state* state, const float clip[16], const float* bones,
                              int palette_count, const float fade[4], int mesh, int first_index,
                              int index_count);

/* The same with the matrix converted once (ctr_gpu_prepare_mesh_matrix) for many draws: the level
 * renderer draws chunks interleaved (sorted by texture), so the conversion would be redone at
 * every chunk change. */
typedef struct {
  float m[16];
  uint32_t f24[12]; /* (AI-assisted) m as float24 uniform words (3DS raw draws) */
  uint32_t id;      /* (AI-assisted) new for every prepare: draws compare it, not m (so a
                     * prepared matrix must not be changed, only prepared again) */
} ctr_mesh_matrix;
void ctr_gpu_prepare_mesh_matrix(const float clip[16], ctr_mesh_matrix* out);
/* (AI-assisted) config.ini fog_stage_skip: the next ctr_gpu_draw_mesh(_prepared) draws are entirely
 * in front of where both fogs start: their combiner stage passes the color through instead of
 * mixing in the fog ramp, and (raw draws) texture unit 1, the ramp, is off for them (its cache is
 * kept). on: 1. Reset at each frame begin. */
void ctr_gpu_set_mesh_nofog(int on);
void ctr_gpu_draw_mesh_prepared(const ctr_draw_state* state, const ctr_mesh_matrix* matrix,
                                int mesh, int first_index, int index_count);

/* ---------------- (AI-assisted) the ocean (config ocean = waves, CtrOcean) ----------------
 * Vertices in the static mesh layout (c3l::Vertex, its st unused), drawn by
 * platform/3ds/shaders/ctr_ocean.v.pica: texture coordinates from the position (rows below), the
 * color brightened on crests (position y), fog like ctr_gpu_draw_mesh. Two textures:
 *   rgb = tex(st0) * color * 2 * tex2(st1) * 2, then fog; alpha = tex.a * color.a * 2
 * (tex2 on texture unit 2: a gray around 0x80 is neutral). ctr_gpu_ocean_end after the ocean's
 * draws (texture unit 2 off, combiners as the other draws want them). */
typedef struct {
  ctr_mesh_matrix clip; /* (pos, 1) -> clip, prepared */
  float tex[4][4];      /* s0, t0, s1, t1 = dot(row.xyz, pos) + row.w */
  float crest[4];       /* color factor = 1 + pos.y * crest (rgb; alpha: 0) */
} ctr_ocean_draw;
/* Memory for one frame's vertices / indices (16-byte aligned), read by the GPU during this frame:
 * the immediate draws' buffer (one per frame in flight with the pipeline). NULL when full. */
void* ctr_gpu_stream_alloc(int bytes);
/* mesh >= 0: a range of a static mesh; else verts / indices from ctr_gpu_stream_alloc. */
void ctr_gpu_ocean_draw(const ctr_draw_state* state, int tex2, const ctr_ocean_draw* d, int mesh,
                        const void* verts, const uint16_t* indices, int first_index,
                        int index_count);
void ctr_gpu_ocean_end(void);

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
  /* (AI-assisted) ctr_gpu_tex_create_compact textures: how many, their bytes, how many of them
   * are in the reserved VRAM, radial ones; draws with the early depth test / procedural texture */
  int compact_textures;
  unsigned int compact_bytes;
  int compact_in_vram;
  int radial_textures;
  int early_depth_draws;
  int proctex_draws;
  int nofog_draws;   /* (AI-assisted) level draws without the fog stage (fog_stage_skip) */
  int color_clear_skipped; /* (AI-assisted) skip_clear: this frame cleared only the depth */
  int unit1_toggles; /* (AI-assisted) ... and texture unit 1 switched off or on for them */
  /* (AI-assisted) GPU timing that works with config.ini pipeline: gpu_wait_ms is the render
   * thread waiting for the GPU (C3D_FrameBegin) in this frame; gpu_busy_ms the GPU time of the
   * frames it finished since the last frame end (gpu_busy_frames of them; 0 without frame_pacing =
   * steady, whose fence marks a frame done): done(N) - max(first work handed over(N), done(N-1)) */
  float gpu_wait_ms;
  float gpu_busy_ms;
  int gpu_busy_frames;
  /* (AI-assisted) the same frames from the moment their command list was handed to the GPU
   * (pipeline: the frame end, not the frame begin): done(N) - max(list submitted(N), done(N - 1)).
   * busy counts the CPU's build time of a pipelined frame too; this doesn't */
  float gpu_active_ms;
  unsigned int vbuf_used; /* (AI-assisted) immediate vertex buffer bytes this frame (of vbuf_size) */
  unsigned int vbuf_size;
  int vbuf_dropped;       /* draws dropped: the vertex buffer was full */
  /* (AI-assisted) filled only in the frame after ctr_gpu_want_detailed_stats (it scans all texture
   * slots and probes the linear heap): textures, tex_bytes, compact_*, radial_textures,
   * linear_free, vram_free and linear_largest (the largest block linearAlloc can give) */
  int detailed;
  unsigned int linear_largest;
} ctr_gpu_stats;
void ctr_gpu_get_stats(ctr_gpu_stats* out);
#ifndef __3DS__
/* (AI-assisted) snow-gpu-audit: the renderer the next draws belong to (soft backend statistics) */
void ctr_gpu_soft_bucket(const char* name);
/* (AI-assisted) the name the draws are counted under now */
const char* ctr_gpu_soft_bucket_name(void);
#endif
/* (AI-assisted) the next ctr_gpu_frame_end also fills the detailed counts (once per log line) */
void ctr_gpu_want_detailed_stats(void);

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
