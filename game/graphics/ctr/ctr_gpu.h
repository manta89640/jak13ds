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
  /* opaque, depth tested GEQUAL and written, never discarded by the alpha test (the level
   * renderer's draw lists; the GPU layer ignores it) */
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
   * draws only where the stencil isn't 0 and the depth test (ztest, zwrite 0) passes, and sets
   * the stencil back to 0 there; NO_COLOR writes no color (the stencil passes). None of them may
   * use the early depth test. */
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
/* (AI-assisted) with CTR_TCC_CULL_BACK: culled whatever ctr_gpu_set_cull's enable says (the caller
 * decided: merc draws with config cull_merc), with its winding (flip) */
enum { CTR_TCC_CULL_OWN = 8 };
/* (AI-assisted) ctr_draw_state::tcc bit of level mesh draws: every vertex the draw uses has alpha
 * 0x80, so with ctr_gpu_set_fog_alpha the mesh shader may put the fog amount in the color's alpha
 * (the texture combiner then takes the constant 0x80 for the vertex alpha). CtrLevel sets it at
 * load from the chunk's vertices. */
enum { CTR_TCC_ALPHA80 = 4 };
/* (AI-assisted) ctr_draw_state::tcc bit of level mesh draws (c3l v15 kDrawAtlasUV): the vertices'
 * texture coordinates are in 1/32768 (c3l::kAtlasUVScale: a texture atlas's cells), not 1/1024 */
enum { CTR_TCC_ST_ATLAS = 16 };
/* (AI-assisted, far-fog) ctr_draw_state::tcc bit of merc draws (ctr_gpu_draw_skinned): the game's
 * fog per vertex (merc2.vert / merc2.frag), with the values of the last ctr_gpu_set_merc_fog. Only
 * set when ctr_gpu_merc_fog_ready says so. */
enum { CTR_TCC_FOG = 32 };
/* ctr_draw_state::tcc bits of the PS2 ocean's draws (ctr_gpu_ocean_full_draw*): its texture's mip
 * levels blended (the GS's TEX1 MMIN 5, whatever config.ini mipmaps says); MIP_MAX1: only levels
 * 0 and 1 (TEX1 MXL 1, the near ocean's; also its sky reflection's fade) */
enum { CTR_TCC_TRILINEAR = 64, CTR_TCC_MIP_MAX1 = 128 };
/* (AI-assisted) config.ini fog_alpha (CTR_TCC_ALPHA80 draws: fog in the vertex alpha, no fog ramp
 * texture) and tex1_gating (texture unit 1 enabled only for draws whose combiner reads it), each
 * frame (gpu_profile = ab switches them). ctr_gpu_fog_alpha_ready: the program for it is loaded
 * (ctr_mesh_old_fa, or shader_opt's ctr_mesh_opt_fa). */
void ctr_gpu_set_fog_alpha(int on);
void ctr_gpu_set_tex1_gating(int on);
int ctr_gpu_fog_alpha_ready(void);
void ctr_gpu_set_cull(int enable, int flip);
void ctr_gpu_cull_reset(void);
/* (AI-assisted) 1 if the depth buffer has a stencil part (always: D24S8) */
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
/* (AI-assisted, Jak II texture animation) A texture can stand for an animated texture slot (a
 * level's base texture of it: c3l::texture_anim_slot): draws of it use the slot's texture
 * (ctr_gpu_anim_slot_set) instead while the slot has one. Slot -1: none (the default; a deleted
 * handle loses it). ctr_gpu_anim_slot_set takes effect at the next frame begin (draws within a
 * frame all see the same texture); tex -1: none. */
/* c3l v23: the alpha of texture `handle` is texture `mask`'s (CTR_TEX_A8 / A4, the
 * same size and levels): the level mesh (fog_alpha programs), envmapped tie and merc draws of it
 * sample the mask on another texture unit for their alpha. -1: none (a deleted handle or mask
 * loses it). */
void ctr_gpu_tex_set_mask(int handle, int mask);
#define CTR_ANIM_SLOTS 64
void ctr_gpu_tex_set_anim_slot(int handle, int slot);
void ctr_gpu_anim_slot_set(int slot, int tex);
/* (AI-assisted) The slots a draw used in the last finished frame (bit i: slot i; drawn with the
 * base texture or the slot's): their animations are worth updating. */
uint64_t ctr_gpu_anim_slots_drawn(void);
/* (AI-assisted) The same as ctr_gpu_tex_create (GS alpha: 0x80 = 1.0), stored in the smallest
 * 3DS format that keeps it (config.ini compact_textures): gray -> L8 / LA8, opaque -> RGB565,
 * on/off alpha -> RGBA5551, else RGBA8 (as ctr_gpu_tex_create; (AI-assisted) not RGBA4: soft
 * colored glows banded), and
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
/* The same with z already the PICA's: -depth * w. The GPU reads vertex floats as float24 (16 bit
 * mantissa): an OpenGL-style z past a few metres is within 1% of -w, and the shader's (z + w) kept
 * ~2^-16 of w, ~100 GS depth units; this z keeps its own 16 bits. */
void ctr_gpu_draw_clip_pz(const ctr_draw_state* state, const ctr_clip_vertex* verts, int count);
/* (AI-assisted) Indexed triangle lists of ctr_clip_vertex (like ctr_gpu_draw_clip: the GPU clips),
 * written in place into this frame's vertex memory: ctr_gpu_clip_reserve gives room for max_verts
 * vertices and max_indices u16 indices (0 when there is none: nothing reserved). Then
 * ctr_gpu_clip_draw_ranges draws the n index ranges [first[i], first[i] + count[i]) (of the first
 * index_count indices, which index the first vert_count vertices), each with its own state, all of
 * them or none (the shadow volumes' passes leave the stencil at 0 only together), and gives back
 * the rest of the room (n = 0: all of it). Nothing else may be drawn between the two calls. */
int ctr_gpu_clip_reserve(int max_verts, int max_indices, ctr_clip_vertex** verts,
                         uint16_t** indices);
/* The bytes of this frame's vertex memory still free (a reservation rounds its vertices and its
 * indices up to 16 bytes each). */
unsigned int ctr_gpu_vertex_room(void);
void ctr_gpu_clip_draw_ranges(const ctr_draw_state* states, const int* first, const int* count,
                              int n, int vert_count, int index_count);
/* (AI-assisted, j2gpu) ctr_gpu_clip_draw_ranges whose first n_mtx ranges have their vertices'
 * x, y, z (w ignored, taken as 1) in a space that `matrix` (4x4, row major: clip = M (x y z 1))
 * takes to clip space: the vertex shader multiplies (the clip program's 4 dp4 with the matrix
 * folded into its uniform), not the CPU. The ranges after them are in clip space as usual. */
void ctr_gpu_clip_draw_ranges_mtx(const float matrix[16], int n_mtx, const ctr_draw_state* states,
                                  const int* first, const int* count, int n, int vert_count,
                                  int index_count);

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
/* (AI-assisted, j2gaps) frees the screen copy's texture (the next copy makes it again): Jak II's
 * menu keeps a copy of the game's last frame only while the menu shows it */
void ctr_gpu_screen_copy_release(void);
/* A copy of the whole screen that is kept (Jak II's menu background can't be copied again): in
 * linear memory when there is room (VRAM may be gone after the HOME Menu or sleep), and pinned at
 * level 2. -1 if not available.
 * ctr_gpu_screen_copy_pin: 2: the other copy functions return -1 and leave it as it is; 1: they
 * copy into it again; 1 and 2: no texture pool takes its memory; 0: not pinned.
 * ctr_gpu_screen_copy_release unpins. */
int ctr_gpu_copy_screen_keep(void);
void ctr_gpu_screen_copy_pin(int level);
/* (AI-assisted) Render to texture (ocean env map, minimap): an RGBA8 texture the GPU draws into,
 * in VRAM when there is room (else linear memory: make it before the level texture pools fill
 * VRAM), and a D16 depth buffer for
 * tiles of up to max_tile_w x 256 pixels. "Logical" pixels: lw along ctr x (left to right), lh
 * along ctr y (bottom to top), lw and lh powers of two, lh 256 at most. Returns a texture handle
 * (draw it with filter 0, clamped, like any other) or -1; deleted with ctr_gpu_rt_delete.
 * ctr_gpu_rt_begin: the draws from now on (any kind except screen copies and reservations) go to
 * the tile x..x+w, y..y+h of it: their clip / ctr coordinates [-1, 1] cover the tile as they
 * cover the screen (x: tile left to right, y: bottom to top); x a multiple of 16, w + (x & 15)
 * <= max_tile_w; the tile is NOT cleared (draw over all of it first: alpha 0 for empty texels,
 * depth 0 with ztest ALWAYS). No sky hook in between (it resumes after).
 * Another ctr_gpu_rt_begin may follow (the next tile) without an end in between.
 * ctr_gpu_rt_end: the screen again. Only inside a frame. ctr_gpu_rt_uv: the texture coordinates
 * (ctr_vertex s, t) of logical point (lx, ly) of the texture (pixel edges at integers). */
int ctr_gpu_rt_create(int lw, int lh, int max_tile_w);
void ctr_gpu_rt_delete(int tex);
int ctr_gpu_rt_begin(int tex, int x, int y, int w, int h);
void ctr_gpu_rt_end(void);
void ctr_gpu_rt_uv(int tex, float lx, float ly, float* s, float* t);
/* A square size x size render texture with mip levels (down to 8 x 8), drawn into only with
 * ctr_gpu_rt_draw_grid; sampled like a ctr_gpu_tex_create_mipmapped texture.
 * ctr_gpu_rt_draw_grid: the (n + 1) x (n + 1) vertices (row-major) as n x n quads into level 0
 * (ctr coordinates [-1, 1] cover it as ctr_gpu_rt_begin's tile), each further level the 2 x 2
 * averages of the one above; then the screen again. 1 when drawn. Only inside a frame. (The PS2
 * ocean's wave texture: the GS draws its 32 x 32 quads and the mip levels, CtrOceanFull; their
 * alpha fade is ctr_gpu_ocean_full_draw_env's) */
int ctr_gpu_rt_create_mipmapped(int size);
int ctr_gpu_rt_draw_grid(int tex, const ctr_draw_state* state, const ctr_vertex* verts, int n);
/* (AI-assisted) a static mesh's vertices and indices as made (ctr_gpu_mesh_create / _end), to
 * read on the CPU; 0 if there is none */
int ctr_gpu_mesh_peek(int mesh, const void** verts, int* vertex_count, const uint16_t** indices,
                      int* index_count);
/* (AI-assisted) measurement: the CPU time spent in the screen copies since the last call (ms; the
 * draws so far sent to the GPU, then the copy), part of the distorter's build time */
double ctr_gpu_take_screen_copy_ms(void);
void ctr_gpu_screen_uv(float x, float y, float* s, float* t);

/* Draw quads: 4 vertices each, triangles (0, 1, 3) and (3, 1, 2) (a strip in the order 0 1 3 2).
 * A third fewer vertices than ctr_gpu_draw for sprites. */
void ctr_gpu_draw_quads(const ctr_draw_state* state, const ctr_vertex* verts, int quad_count);
/* (AI-assisted) The quads of the last ctr_gpu_draw_quads again with another state (their vertices
 * stay where that call put them: no second copy), right after it. */
void ctr_gpu_draw_quads_again(const ctr_draw_state* state);
/* (AI-assisted) Draw a triangle strip: count vertices, count - 2 triangles (k - 2, k - 1, k).
 * Several strips go in one draw joined by repeating the last vertex of one and the first of the
 * next (the triangles in between have no area). State like ctr_gpu_draw. */
void ctr_gpu_draw_strip(const ctr_draw_state* state, const ctr_vertex* verts, int count);

/* GS screen lines from the centre to the top edge of the game's field, for the PS2 ocean's GS
 * positions: 112 for Jak 1 (512 x 224, the default), 208 for Jak II (512 x 416). */
void ctr_gpu_set_gs_half_height(float lines);
/* (AI-assisted) The PS2 ocean (CtrOceanFull, config.ini ocean = full): vertices as the ocean's VU1
 * programs send them to the GS. x, y: screen position in GS units (12.4 fixed point, the screen
 * centered at 2048 * 16; the game's field, ctr_gpu_set_gs_half_height), z: GS depth (24 bits, larger =
 * closer), q: the GS Q (1 / w, > 0); s, t: the GS S, T (texture coordinates times q: the GPU
 * divides them per pixel, perspective correct like the GS); fog: the amount of fog color, 0..1
 * (the GS's (255 - F) / 255); rgba: GS vertex color. Colors like ctr_gpu_draw_mesh:
 * textured rgb = tex.rgb * v.rgb / 128, then the fog; a = tex.a (0xff = 1) * v.a / 128 when the
 * state's tcc (bit 0) is set, else v.a / 128 (the GS's TEX0.TCC: the near ocean's textured pass).
 * (AI-assisted, j1ocean) es, et: the sky reflection's texture coordinates (env map, clamped; only
 * ctr_gpu_ocean_full_draw_env reads them). 40 bytes. */
typedef struct {
  float x, y, z, q;
  float s, t, fog;
  uint8_t r, g, b, a;
  float es, et;
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
/* (AI-assisted, j1ocean) Like ctr_gpu_ocean_full_draw_clip, with the PS2 ocean's sky reflection
 * (its env map pass, CommonOceanRenderer's ENV_MAP bucket) in the same draw: env_tex (texture
 * unit 2, clamped, at es, et; the env map already times the pass's GS color) is added after the fog,
 * weighted like the PS2's destination alpha:
 *   tcc set (mid ocean, opaque):  rgb = fog(tex * v) + env * (tex.a * v.a / 128), a = tex.a * v.a
 *   tcc clear (near ocean, blended): rgb = fog(tex * v) + env * tex.a, a = v.a
 * (the near ocean's alpha blending then weights both by v.a, as the PS2's alpha-only pass did).
 * The weight also fades with depth as the PS2's did: the GS picks the wave texture's mip level
 * from Q alone, LOD = 2 log2(1 / Q) - 17.1875 (TEX1 L 1, K 0xeed), and its levels' alpha is 1,
 * 0.5, 0, ... (ocean-texture's mip sprites), blended (MMIN 5): weight x clamp(CTR_OCEAN_ENV_FADE_K
 * + log2 Q, 0, 1), at least 0.5 for the near ocean (MXL 1, CTR_TCC_MIP_MAX1 in its state); per
 * pixel, Q interpolated in screen space like the GS's (texture unit 1's t, a table).
 * ctr_gpu_ocean_end after the bucket's draws (texture unit 2 off, combiner stage 2 reset). */
#define CTR_OCEAN_ENV_FADE_K 9.59375f
void ctr_gpu_ocean_full_draw_env(const ctr_draw_state* state, int vert_count, int index_count,
                                 uint8_t fog_r, uint8_t fog_g, uint8_t fog_b, int env_tex);

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
  CTR_TEX_A8 = 5,    /* alpha masks (c3l v23) */
  CTR_TEX_A4 = 6,    /* two texels per byte, the first in the low bits */
  CTR_TEX_RGBA5551 = 7,
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
/* Copies of the texture pools in VRAM while there is room (config.ini vram_textures); linear
 * memory keeps every pool. */
void ctr_gpu_set_vram_textures(int on);
/* Mip levels: 0 = off (always the first level), 1 = nearest level (default), 2 = blend two levels
 * (trilinear, slower) (config.ini mipmaps). */
void ctr_gpu_set_mip_mode(int mode);
/* (AI-assisted) 3DS hardware features (config.ini). proctex: radial textures drawn with the
 * procedural texture unit. compact: ctr_gpu_tex_create_compact picks small formats (off: RGBA8
 * like ctr_gpu_tex_create). */
/* For the shadow self-test (CtrShadow.cpp). ctr_gpu_submit_now: in a frame, what is built so
 * far goes to the GPU as a command list of its own. ctr_gpu_top_histogram: outside a frame, once
 * the GPU has drawn all it was sent, the top screen's pixels by their blue value; 0: can't be
 * read */
void ctr_gpu_submit_now(void);
int ctr_gpu_top_histogram(unsigned int counts[256]);
/* (AI-assisted) config.ini tex_cache_clear: 0 always (each texture unit 0 bind clears the GPU's
 * texture cache, as citro3d does), 1 uploads (a bind that keeps a level pool texture and only
 * changes its sampling settings doesn't clear it unless a pool was filled or moved since the last
 * clear; every switch to another texture and the first bind of a frame clear). Any time. */
void ctr_gpu_set_tex_cache_clear(int mode);
/* (AI-assisted) config.ini mip_bias: the LOD bias of mipmapped textures (levels, + = a smaller
 * level sooner: blurrier, fewer texels read), -4..4 in 1/256 steps. 0 = none. Any time. */
void ctr_gpu_set_mip_bias(float bias);
/* (AI-assisted) config.ini skin_rows_f24: merc bone rows uploaded as float24 (3 words per vector,
 * rounded to nearest; the GPU computes in float24 anyway) instead of float32 (4 words): 216
 * instead of 288 data words per 24-bone palette. Raw draws only. Any time. */
void ctr_gpu_set_skin_rows_f24(int on);
/* (AI-assisted) config.ini skin_clip_f24: each merc model's clip matrix (12 instead of 16 words)
 * and lights (21 instead of 28) as float24 too. Not bit-identical (rounded to nearest where the GPU
 * converts f32 its own way): edge pixels of models can flip. Each frame. */
void ctr_gpu_set_skin_clip_f24(int on);
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
/* (AI-assisted) the game thread hands over a frame: its release vblank (for steady pacing), read
 * with ctr_gpu_release_now when the frame's logic ended. (AI-assisted, gpu-home) Noted only once
 * the render thread is idle (the next frame's begin reads it): noted before, it could overwrite the
 * previous frame's release while that frame was still before its begin. */
unsigned int ctr_gpu_release_now(void);
void ctr_gpu_note_release(unsigned int release);
/* (AI-assisted) since the last call: frames shown for 1, 2, 3, 4, 5+ vblanks, the presentation
 * latency in vblanks (-1: steady pacing off), frames skipped and the render thread's total wait
 * for a free screen buffer; just_in_time: frames the GPU finished in the vblank before their own
 * (no vblank to spare), floor: the latency it doesn't go below for now (a frame was late there) */
void ctr_gpu_pacing_stats(unsigned int hist[5], int* latency, unsigned int* dropped,
                          double* buffer_wait_ms, unsigned int* just_in_time, int* floor);
void ctr_gpu_set_proctex(int on);
/* (AI-assisted, onebone) config.ini merc_one_bone: one-bone palettes' draws with ctr_skin_opt1 */
void ctr_gpu_set_merc_one_bone(int on);
/* (AI-assisted, idx8) config.ini index_u8: quad batches of up to 64 quads with u8 indices */
void ctr_gpu_set_index_u8(int on);
void ctr_gpu_set_compact_textures(int on);
/* (AI-assisted) Overlap: hand the draws so far to the GPU in the middle of a frame (after a
 * renderer bucket, when enough draws piled up), so the GPU draws them while the CPU builds the
 * rest of the frame. Off: the GPU starts at the end of the frame (config.ini overlap). */
void ctr_gpu_set_overlap(int on);
/* (AI-assisted) config.ini raw_cmds: the draws write their GPU commands themselves instead of
 * through citro3d (much less CPU per draw, fewer command words). Can change at any time. */
void ctr_gpu_set_raw_cmds(int on);
/* (AI-assisted) config.ini cmd_dedup (on: register writes that give a register the value it has
 * are left out: texture unit 0's type) and cmd_dedup_draw (the raw draws' PRIMITIVE_CONFIG mode
 * and VERTEX_OFFSET writes too). Each frame (gpu_profile = ab switches them). */
void ctr_gpu_set_cmd_dedup(int on, int draw);
/* (AI-assisted, gpu-rt) config.ini gpu_breakdown (profiling, off): every 4th frame (pipeline on) each
 * part of the frame goes to the GPU alone and is waited for, timed from its submission to its GSP
 * event (P3D, PSC, PPF): the phases below. ctr_gpu_phase: the commands built from now on belong to
 * that phase (the renderer calls it per bucket; the backend for the sky hook, the screen copy and
 * the transfer itself). ctr_gpu_breakdown_line: the window's line, 0 when nothing was measured. */
enum {
  CTR_PH_OTHER,
  CTR_PH_CLEAR,
  CTR_PH_SKY,
  CTR_PH_LEVEL,
  CTR_PH_MERC,
  CTR_PH_TRANS,
  CTR_PH_SHADOW,
  CTR_PH_OCEAN,
  CTR_PH_SPRITE,
  CTR_PH_DISTORT,
  CTR_PH_HUD,
  CTR_PH_TRANSFER,
  CTR_GPU_PHASES
};
void ctr_gpu_set_gpu_breakdown(int on);
void ctr_gpu_phase(int phase);
int ctr_gpu_breakdown_line(char* out, int size, const char* label);
/* (AI-assisted) hardware hang bisect (config.ini asm_tex, CtrSettings.h). asm_tex 0: the old per
 * texel texture tiling loops (no LDM/STM, no 64-bit stores). */
void ctr_gpu_set_asm_tex(int on);
/* (AI-assisted) config.ini defer_flush (hang bisect): 0 = every cache flush right away */
void ctr_gpu_set_defer_flush(int on);
int ctr_gpu_asm_tex(void);
/* (AI-assisted) where the render thread is (a static string) and its frame count; logged once
 * per second for the first 60 s by a watchdog thread (ctr_gpu_diag_start: config.ini
 * render_watchdog), so a hang shows the
 * stage it stopped in */
void ctr_gpu_diag_stage(const char* stage);
/* render_lines 0: only the game thread watchdog (ctr_game_watchdog_tick, once a second) */
void ctr_gpu_diag_start(int render_lines);
void ctr_game_watchdog_tick(void);
void ctr_gpu_submit_partial(void);
/* (AI-assisted) Pipeline: build frame N+1 while the GPU draws frame N (config.ini pipeline).
 * Outside of a frame only. Returns whether it's on (needs ~3.5 MB of linear memory). */
/* (AI-assisted, j2-mem2) the command words one frame has room for (KB): the pipeline's half, else
 * citro3d's whole buffer */
unsigned int ctr_gpu_cmd_buffer_kb(void);
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
/* The same with tail_bytes more after the vertices (at verts + vertex_count * 16; not vertices
 * for ctr_gpu_mesh_peek / _vertices_inplace): data a draw reads beside them (envmapped tie
 * normals), freed with the mesh. */
int ctr_gpu_mesh_begin_tail(int vertex_count, int tail_bytes, int index_count, void** verts,
                            uint16_t** indices);
int ctr_gpu_mesh_end(int mesh, int ok);
/* Between begin and end: the mesh's vertex_count * 16 bytes are written as two attribute buffers,
 * vertex_count records of 12 bytes (s16 pos[3], s16 pad, s16 st[2]) and then the colors (u8
 * rgba[4] each) at ctr_gpu_mesh_color_offset: a rewrite of the colors (the level's time of day)
 * touches only theirs. Drawn like the interleaved layout. */
void ctr_gpu_mesh_split_colors(int mesh);
static inline unsigned int ctr_gpu_mesh_color_offset(int vertex_count) {
  return (unsigned int)vertex_count * 12u;
}
/* (AI-assisted, streaming) The same in memory the caller owns (linear memory: the level stream
 * arena, CtrStream.h): mem holds the vertices (vertex_count * 16 bytes) and then the indices at
 * mem + ctr_gpu_ext_index_offset(vertex_count). Nothing is allocated; once a deleted (or failed)
 * one is no longer used by the GPU, the function set with ctr_gpu_set_ext_free gets mem back
 * (render thread, or the thread of ctr_gpu_free_pending_now; with the linear lock held). */
int ctr_gpu_mesh_begin_ext(void* mem, int vertex_count, int index_count, void** verts,
                           uint16_t** indices);
/* The same with tail_bytes of draw data (envmapped tie normals) at mem + tail_off (after the
 * indices), written at *tail; a draw names it by tail_off from the vertices. */
int ctr_gpu_mesh_begin_ext_tail(void* mem, int vertex_count, int index_count, unsigned int tail_off,
                                unsigned int tail_bytes, void** verts, uint16_t** indices,
                                void** tail);
void ctr_gpu_set_ext_free(void (*fn)(void* mem));
static inline unsigned int ctr_gpu_ext_index_offset(int vertex_count) {
  return ((unsigned int)vertex_count * 16u + 127u) & ~127u;
}
/* (AI-assisted, streaming) a block of linear memory for the stream arena's segments (NULL when
 * there is no room for it and LINEAR_RESERVE), freed with ctr_gpu_linear_block_free (no GPU use
 * left in it); the linear heap's free bytes now (the host: a model, env CTR_SOFT_LINEAR_KB) */
void* ctr_gpu_linear_block(unsigned int bytes);
void ctr_gpu_linear_block_free(void* p);
unsigned int ctr_gpu_linear_free(void);
/* the largest block of linear memory one allocation can get now (probes: not every frame) */
unsigned int ctr_gpu_linear_largest(void);
/* The linear heap by what holds it, in bytes as the heap counts them (the host: its model). The
 * GPU layer's tables, walked under the linear lock: once per statistics line. other: the heap's
 * bytes in use that nothing here holds (the sound's buffers, the malloc heap's last-resort
 * blocks, a texture or mesh being made). */
typedef struct {
  unsigned int heap, free;
  unsigned int before;                    /* in use before ctr_gpu_init (the screens' buffers) */
  unsigned int fixed;                     /* command, vertex, present and probe buffers, ramps */
  unsigned int meshes, mesh_count;        /* static meshes (not the ones in a linear block) */
  unsigned int mesh_copies;               /* second vertex buffers of meshes rewritten per frame */
  unsigned int mesh_pending;              /* deleted, waiting for the GPU to be done with them */
  unsigned int pools, pool_count;         /* texture pools */
  unsigned int pool_pending;
  unsigned int tex_compact, tex_compact_count; /* ctr_gpu_tex_create_compact's own formats */
  unsigned int tex_plain, tex_plain_count;     /* the other textures with memory of their own */
  unsigned int tex_anim;                  /* of tex_plain and tex_copies: the animation slots' */
  unsigned int tex_target;                /* render targets: color, mip levels, depth */
  unsigned int tex_screen;                /* the screen copy */
  unsigned int tex_copies;                /* second copies of textures rewritten per frame */
  unsigned int tex_pending;
  unsigned int staging;                   /* buffers of queued copies to VRAM */
  unsigned int blocks;                    /* ctr_gpu_linear_block */
  int other;
} ctr_gpu_linear_account;
void ctr_gpu_linear_account_get(ctr_gpu_linear_account* out);
/* the linear heap's bytes of a mesh (0: none, or in a caller's block) and of a pool */
unsigned int ctr_gpu_mesh_linear_bytes(int mesh);
unsigned int ctr_gpu_pool_linear_bytes(int pool);
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
/* (AI-assisted) A static mesh's own vertices (ctr_gpu_mesh_create / _end), to rewrite in place:
 * no second buffer (unlike ctr_gpu_mesh_vertices), so a frame the GPU is still drawing may see
 * part of the change. For slow changes (the level's time of day colors). Then ctr_gpu_mesh_flush.
 * NULL if there is none. */
void* ctr_gpu_mesh_vertices_inplace(int mesh, int* vertex_count);
/* The colors of a mesh made with ctr_gpu_mesh_split_colors, in place the same way (NULL for
 * another mesh) */
void* ctr_gpu_mesh_colors_inplace(int mesh);
void ctr_gpu_mesh_flush(int mesh, int offset, int size);
/* (AI-assisted, micro-mg) Bytes the CPU wrote that the GPU reads in this frame (any memory: linear
 * heap ranges are what matter): queued, not flushed now. In a frame (between ctr_gpu_frame_begin
 * and ctr_gpu_frame_end) every queued range is flushed before the next part of the command list
 * goes to the GPU (a partial submit, a screen copy or the frame end), touching and near ranges of
 * the linear heap merged into one GSP request each (gpu_micro: up to 64 KB apart); outside a frame
 * at once. One GSP request costs tens of microseconds of the calling thread (~62 us in Azahar):
 * call this for every dirty range (e.g. each chunk's rewritten vertices) and let the merging do the
 * rest, rather than flushing yourself. ctr_gpu_mesh_flush is this for a part of a mesh's
 * vertices. Any thread may call it outside a frame; in a frame, the render thread. */
void ctr_gpu_flush_range(const void* addr, unsigned int size);
/* (AI-assisted, micro-mg) a test (pad script "set gpu_micro_mask N"): gpu_micro's changes as the
 * bits of N (0 = all off, 1023 = all on; ctr_gpu_citro3d.c MICRO_*) from the next frame on, for
 * old / new measurements of one view in one run */
void ctr_gpu_set_micro_mask(unsigned int mask);

/* bones: palette_count 3x4 row-major matrices, (pos, 1) -> camera space (x, y, z).
 * clip: row-major 4x4 from (camera x, y, z, 1) to OpenGL-style clip space.
 * tint: multiplies the final color (lighting approximation), 0..1. */
/* lights: 7 vec4 like merc2.vert: light direction rows 0-2 (light_dir0/1/2), light colors 0-2,
 * ambient. Vertices are c3l::MercVertex (24 bytes, with a normal).
 * (AI-assisted) bones_id: a number new for every palette the caller makes (never reused for other
 * contents while it may still be the last one drawn), so draws compare it instead of the bones; 0:
 * unknown, uploaded at every draw. bones_f24: NULL, or the bones as ctr_gpu_pack_skin_rows_f24
 * made them (used with skin_rows_f24 instead of converting them again). */
void ctr_gpu_draw_skinned(const ctr_draw_state* state, const float clip[16], const float* bones,
                          int palette_count, const uint32_t* bones_f24, uint32_t bones_id,
                          const float lights[28], int mesh, int first_index, int index_count);
/* (AI-assisted) skin_rows_f24 is on (the draws take bones_f24), and the float24 words of a palette
 * for them: 9 words per bone, row by row (row r of bone b at out[3 * (r * palette_count + b)]),
 * palette_count <= CTR_MAX_PALETTE. */
int ctr_gpu_skin_rows_f24_on(void);
void ctr_gpu_pack_skin_rows_f24(const float* bones, int palette_count, uint32_t* out);
/* (AI-assisted, far-fog) the fog of the next CTR_TCC_FOG skinned draws, as merc2.vert computes it
 * from the merc camera (Merc2 low memory: hvdf_offset.w, fog min and max): amount =
 * (255 - clamp(hvdf_w - clip w, fog_min, fog_max)) / 255, mixed towards (r, g, b) (the frame's fog
 * color, the fog ramp's) like merc2.frag. ctr_gpu_merc_fog_ready: the skin program in use has the
 * fog (shader_opt's ctr_skin_opt; the Oct 2 ctr_skin_old has none) and the fog ramp exists. */
void ctr_gpu_set_merc_fog(float hvdf_w, float fog_min, float fog_max, uint8_t r, uint8_t g,
                          uint8_t b);
/* (AI-assisted, mercsprite) and the port's draw distance / vista haze on them, as the level
 * meshes': amount = max(the game's, k x clip w + c) (k = c = 0: none) */
void ctr_gpu_set_merc_haze(float k, float c);
int ctr_gpu_merc_fog_ready(void);

/* The envmap pass of a skinned mesh draw (emerc.vert): texture coordinates from the camera space
 * normal and position (a sphere map), color = texture * fade * 2 (fade: rgba, GS value / 255 as
 * in Merc2, so 0x80 = texture x 1). */
void ctr_gpu_draw_skinned_env(const ctr_draw_state* state, const float clip[16], const float* bones,
                              int palette_count, const uint32_t* bones_f24, uint32_t bones_id,
                              const float fade[4], int mesh, int first_index, int index_count);

/* The same with the matrix converted once (ctr_gpu_prepare_mesh_matrix) for many draws: the level
 * renderer draws chunks interleaved (sorted by texture), so the conversion would be redone at
 * every chunk change. */
typedef struct {
  float m[16];
  /* (AI-assisted) m as float24 uniform words (3DS raw draws), then shader_opt's two fog rows: 6
   * words the draws make at the first upload with the fog of fog_gen (a cache in the matrix: a
   * prepared matrix is never in const memory) */
  uint32_t f24[18];
  uint32_t id;      /* (AI-assisted) new for every prepare: draws compare it, not m (so a
                     * prepared matrix must not be changed, only prepared again) */
  uint32_t fog_gen; /* (AI-assisted) the fog f24[12..17] were made with (0: not made) */
} ctr_mesh_matrix;
void ctr_gpu_prepare_mesh_matrix(const float clip[16], ctr_mesh_matrix* out);
/* (AI-assisted) config.ini fog_stage_skip: the next ctr_gpu_draw_mesh(_prepared) draws are entirely
 * in front of where both fogs start: their combiner stage passes the color through instead of
 * mixing in the fog ramp, and (raw draws) texture unit 1, the ramp, is off for them (its cache is
 * kept). on: 1. Reset at each frame begin. */
void ctr_gpu_set_mesh_nofog(int on);
void ctr_gpu_draw_mesh_prepared(const ctr_draw_state* state, const ctr_mesh_matrix* matrix,
                                int mesh, int first_index, int index_count);
/* (AI-assisted, gpu-rt) the level renderer hands a pass's draws over in one list (in drawing order;
 * nofog as ctr_gpu_set_mesh_nofog for that draw): drawn one by one */
typedef struct {
  const ctr_draw_state* state;
  const ctr_mesh_matrix* matrix;
  int mesh, first_index, index_count, nofog;
  /* morph_rows: a ctr_gpu_draw_mesh_morph draw (its other arguments here); NULL: none */
  const float* morph_rows;
  const uint8_t* morph_list;
  int morph_list_n, morph_offset;
  uint32_t morph_id;
  /* tmorph: a ctr_gpu_draw_mesh_tmorph draw (its uniforms, morph_id their id, its tail at
   * tmorph_offset); NULL: none */
  const float* tmorph;
  int tmorph_offset;
} ctr_mesh_call;
void ctr_gpu_draw_mesh_calls(const ctr_mesh_call* calls, int n);
/* Jak 1's wind ties (c3l::ExtWind): a level mesh draw (as ctr_gpu_draw_mesh_prepared) whose
 * vertices are sheared by their instance's wind, platform/3ds/shaders/ctr_mesh_wind.v.pica.
 * slots_mesh holds from byte slots_offset (a multiple of 16) a uint8_t slot per vertex of the
 * mesh; rows[4 * s] = kx, kz, cx, cz for slot s (row 0 zero: no wind), at most 64 rows: in
 * the mesh's quantized units x += kx * y + cx, z += kz * y + cz. rows_id: new whenever the rows
 * change (draws with the same id don't upload them again). Without the program (shader_opt off)
 * the draw is at rest. */
void ctr_gpu_draw_mesh_wind(const ctr_draw_state* state, const ctr_mesh_matrix* matrix,
                            const float* rows, int row_count, uint32_t rows_id, int mesh,
                            int slots_mesh, int slots_offset, int first_index, int index_count);
/* 1 when the wind program can be loaded (ctr_gpu_draw_mesh_wind may sway) */
int ctr_gpu_wind_ready(void);
/* The PS2's tie geomorph (c3l::ExtMorph): a level mesh draw (as ctr_gpu_draw_mesh_prepared) whose
 * vertices move by their row's morph factor, platform/3ds/shaders/ctr_mesh_morph.v.pica. The mesh
 * holds from byte morph_offset (a multiple of 16) a c3l::MorphVertex per vertex (8 bytes: int8
 * delta[3], row, delta2[3], pad): the vertex is drawn at pos + f * delta + f2 * delta2, f =
 * rows[4 * row], f2 = rows[4 * row + 1] (row 0: 0; the caller folds the deltas' scale in). list: the rows the draw's vertices use (1 .. CTR_MORPH_ROWS - 1);
 * rows_id: new whenever the rows change (rows the GPU has for this id aren't sent again). Without
 * the program it is drawn at rest. */
#define CTR_MORPH_ROWS 80
void ctr_gpu_draw_mesh_morph(const ctr_draw_state* state, const ctr_mesh_matrix* matrix,
                             const float* rows, const uint8_t* list, int list_n, uint32_t rows_id,
                             int mesh, int morph_offset, int first_index, int index_count);
/* 1 when the morph program can be loaded (ctr_gpu_draw_mesh_morph moves the vertices) */
int ctr_gpu_morph_ready(void);
/* The PS2's tfrag geomorph (c3l::ExtTfrag): a level mesh draw whose vertices move by their own clip
 * w, platform/3ds/shaders/ctr_tfrag_morph.v.pica. The mesh holds from byte tail_offset (a multiple
 * of 16) a c3l::TfragMorphVertex per vertex (16 bytes). unif: f0 = clamp(w * unif[0] + unif[2],
 * 0, 1), f1 = clamp(w * unif[1] + unif[3], 0, 1), unif[4] the records' delta_scale: the vertex is
 * at pos + (f0 * band0 + f1 * band1) * unif[4], its rgb c + f1 * color1 * (m - c) + f0 * color0 *
 * (t - m). unif_id: new whenever unif changes. Without the program it is drawn at rest. */
void ctr_gpu_draw_mesh_tmorph(const ctr_draw_state* state, const ctr_mesh_matrix* matrix,
                              const float* unif, uint32_t unif_id, int mesh, int tail_offset,
                              int first_index, int index_count);
/* (AI-assisted, micro-gpu) the gpu_micro statistics line since the last call ([ctr] gpu micro ...);
 * 0: nothing to say */
int ctr_gpu_micro_line(char* out, int size);

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

/* (AI-assisted, j2water) Jak II's ocean with the sky reflection (the PS2 env map pass in the same
 * draw), platform/3ds/shaders/ctr_ocean_env.v.pica. Vertices in the static mesh layout:
 *   rgb = fog(tex(st0) * color * 2) + env(st2) * a, a = tex.a * color.a * 2 (clamped to 1)
 * with env on texture unit 2 (repeat, linear), st2 = dot(env row.xyz, d) / |d| + env row.w and
 * d = dir rows . (pos, 1) (the vertex relative to the camera). ctr_gpu_ocean_end after the draws. */
typedef struct {
  ctr_mesh_matrix clip; /* (pos, 1) -> clip, prepared */
  float tex[2][4];      /* wave texture s, t = dot(row.xyz, pos) + row.w */
  float dir[3][4];      /* d = dot(row.xyz, pos) + row.w */
  float env[2][4];      /* env s, t (see above) */
} ctr_ocean_env_draw;
void ctr_gpu_ocean_env_draw(const ctr_draw_state* state, int env_tex, const ctr_ocean_env_draw* d,
                            int mesh, int first_index, int index_count);

/* (AI-assisted, j2water2) Jak II's envmapped ties (c3l v21): a level mesh draw (the static mesh
 * layout) with the envmap shine in the same draw, platform/3ds/shaders/ctr_etie.v.pica.
 * normals_mesh: a mesh (ctr_gpu_mesh_create, its vertices as bytes) holding from byte
 * normals_offset (a multiple of 4) int8 x, y, z, 0 per vertex of the mesh (world space * 127). With the envmap on
 * texture unit 2 (clamped, linear) at etie.vert's coordinates:
 *   rgb = fog(tex(st0) * color * 2 + env(st2) * shine * a), a = tex.a * color.a * 2 (at most 1)
 * (decal: tex(st0) instead of tex * color * 2), shine: 0xff = 1 (abgr, the alpha unused).
 * ctr_gpu_etie_end after the draws. */
typedef struct {
  ctr_mesh_matrix clip; /* (pos, 1) -> clip, prepared */
  float cam[3][4];      /* camera space position v = dot(row.xyz, pos) + row.w */
  float nrm[3][4];      /* camera space normal n = dot(row.xyz, normal) */
  float st_scale;       /* 1/1024 (CTR_TCC_ST_ATLAS: 1/32768) */
  uint32_t shine;       /* the shine color (abgr) */
} ctr_etie_draw;
void ctr_gpu_etie_draw(const ctr_draw_state* state, int env_tex, const ctr_etie_draw* d, int mesh,
                       int normals_mesh, int normals_offset, int first_index, int index_count);
void ctr_gpu_etie_end(void);
/* 1 when ctr_gpu_etie_draw can draw (the Jak II build's program is loaded) */
int ctr_gpu_etie_ready(void);

/* Fog for ctr_gpu_draw_mesh (see platform/3ds/shaders/ctr_mesh_old.v.pica), from clip w:
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
   * are in the reserved VRAM, radial ones; draws with the procedural texture */
  int compact_textures;
  unsigned int compact_bytes;
  int compact_in_vram;
  int radial_textures;
  int proctex_draws;
  int nofog_draws;   /* (AI-assisted) level draws without the fog stage (fog_stage_skip) */
  int color_clear_skipped; /* (AI-assisted) skip_clear: this frame cleared only the depth */
  int tex_clears_skipped;  /* (AI-assisted) tex_cache_clear = uploads: binds without the clear */
  int unit1_toggles; /* (AI-assisted) texture unit 1 switched off or on (fog_stage_skip,
                      * tex1_gating) */
  int fog_alpha_draws; /* (AI-assisted) level draws with the fog in the vertex alpha (fog_alpha) */
  unsigned int cmd_dedup_words; /* (AI-assisted) command words cmd_dedup(_draw) left out */
  /* (AI-assisted) vertex program switches, of them with a shader code upload (another shbin), and
   * uniform vectors written at the switches */
  int prog_switches, prog_uploads, prog_unif_vectors;
  /* (AI-assisted) GPU timing that works with config.ini pipeline: gpu_wait_ms is the render
   * thread waiting for the GPU (C3D_FrameBegin) in this frame; gpu_busy_ms the GPU time of the
   * frames it finished since the last frame end (gpu_busy_frames of them; 0 without frame_pacing =
   * steady, whose fence marks a frame done): done(N) - max(first work handed over(N), done(N-1)) */
  float gpu_wait_ms;
  /* (AI-assisted, gpu-home) frame_overlap2 = on: a full running GX queue drained (in gpu_wait_ms) */
  int queue_drains;
  float gpu_busy_ms;
  int gpu_busy_frames;
  /* (AI-assisted) the same frames from the moment their command list was handed to the GPU
   * (pipeline: the frame end, not the frame begin): done(N) - max(list submitted(N), done(N - 1)).
   * busy counts the CPU's build time of a pipelined frame too; this doesn't */
  float gpu_active_ms;
  unsigned int vbuf_used; /* (AI-assisted) immediate vertex buffer bytes this frame (of vbuf_size) */
  unsigned int vbuf_size;
  int vbuf_dropped;       /* draws dropped: the vertex buffer was full */
  int dead_draws;         /* level draws skipped: their mesh is not there (deleted or not made) */
  /* (AI-assisted) filled only in the frame after ctr_gpu_want_detailed_stats (it scans all texture
   * slots and probes the linear heap): textures, tex_bytes, compact_*, radial_textures,
   * linear_free, vram_free and linear_largest (the largest block linearAlloc can give) */
  int detailed;
  unsigned int linear_largest;
} ctr_gpu_stats;
void ctr_gpu_get_stats(ctr_gpu_stats* out);
/* (AI-assisted, depthreuse) config.ini depth_reuse (CtrLevel.cpp, docs/3ds-port/culling.md "4."):
 * the top screen's depth drawn so far, reduced to 8x8 blocks (50 x 30, rows from the top; depth as
 * the soft backend's z: 0 far .. 1 near), each block's farthest (zfar) and nearest (znear) depth,
 * with the 16 floats the caller gave (its camera). ctr_gpu_depth_copy: take it now (3DS: a GPU copy
 * after what is drawn so far, pipeline mode with steady pacing, D24S8, not block32; one at a time).
 * ctr_gpu_depth_blocks: the newest finished one (its sequence number; 0: none yet). */
void ctr_gpu_depth_copy(const float user[16]);
unsigned ctr_gpu_depth_blocks(float* zfar, float* znear, float user[16]);
#ifndef __3DS__
/* (AI-assisted) snow-gpu-audit: the renderer the next draws belong to (soft backend statistics) */
void ctr_gpu_soft_bucket(const char* name);
/* (AI-assisted) debugging: a texture's texels as a PNG (soft backend only) */
void ctr_gpu_soft_dump_tex(int tex, const char* path);
/* host tests: a texture's texels (RGBA8, top row first), NULL if there is none (soft backend) */
const uint8_t* ctr_gpu_soft_texels(int tex, int* w, int* h);
/* (AI-assisted) the name the draws are counted under now */
const char* ctr_gpu_soft_bucket_name(void);
/* the frame's clip w per meter of view depth (soft backend statistics by distance) */
void ctr_gpu_soft_view_depth(float w_per_meter);
/* (AI-assisted, visbake) visibility bakes (CtrLevel.cpp, docs/3ds-port/culling.md "3."): each level
 * mesh triangle drawn gets the tag `tagger(mesh, index of its first corner)` (0: none). A tag counts
 * as seen in a frame when one of its opaque fragments is the last opaque level fragment of a pixel
 * of the screen, or one of its blended fragments passed the depth test there. NULL: off. */
typedef uint32_t (*ctr_soft_tagger)(int mesh, int first_index);
void ctr_gpu_soft_set_tagger(ctr_soft_tagger tagger);
/* the tags seen in the last frame that ended (sorted, unique): how many, the first `max` in out */
int ctr_gpu_soft_seen_tags(uint32_t* out, int max);
#endif
/* (AI-assisted) the next ctr_gpu_frame_end also fills the detailed counts (once per log line) */
void ctr_gpu_want_detailed_stats(void);

/* Monotonic time in milliseconds. */
double ctr_gpu_time_ms(void);
/* (AI-assisted, j2cull) monotonic time in the clock's own ticks (cheap: no conversion), and ticks
 * to milliseconds (for statistics: sums of tick differences converted once per log line) */
unsigned long long ctr_gpu_ticks(void);
double ctr_gpu_ticks_to_ms(unsigned long long ticks);

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
