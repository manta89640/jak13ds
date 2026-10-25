/*
 * (AI-assisted)
 * citro3d implementation of game/graphics/ctr/ctr_gpu.h (see the conventions there).
 *
 * - top screen, 400x240, RGBA8 color + 24/8 depth-stencil
 * - three vertex shaders (platform/3ds/shaders): immediate draws, level meshes, skinned meshes
 * - immediate vertices are copied into a linear-memory buffer that is reset every frame
 *   (C3D_FrameBegin waits until the GPU finished the previous frame first)
 * - level textures live in pools that move to VRAM while there is room (see ctr_gpu_pool_create)
 * - the GPU's texture cache is cleared at every texture change (citro3d), so textures are only
 *   bound again when they really change (bind_texture), and draws are sorted by texture where
 *   the order doesn't matter (CtrTfragRenderer)
 */

#include "game/graphics/ctr/ctr_gpu.h"
#include "game/graphics/ctr/ctr_gpu_bench.h"
#include "game/graphics/ctr/CtrTexConv.h"

#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

extern const uint8_t ctr_basic_shbin[];
extern const size_t ctr_basic_shbin_size;
extern const uint8_t ctr_clip_shbin[];
extern const size_t ctr_clip_shbin_size;
extern const uint8_t ctr_clip2_shbin[];
extern const size_t ctr_clip2_shbin_size;
extern const uint8_t ctr_ocean_shbin[];
extern const size_t ctr_ocean_shbin_size;
extern const uint8_t ctr_ocean_full_shbin[];
extern const size_t ctr_ocean_full_shbin_size;
extern const uint8_t ctr_ocean_full_clip_shbin[];
extern const size_t ctr_ocean_full_clip_shbin_size;
/* (AI-assisted) the Oct 2 shaders (hardware proven; the level meshes and merc without shader_opt,
 * config.ini shader_basic = old). The Oct 3 ctr_mesh / ctr_skin / ctr_skin_env (the new ctr_mesh
 * hung the real GPU), the dph bisect mesh and the shader_groups shbins were removed on Oct 16. */
extern const uint8_t ctr_basic_old_shbin[];
extern const size_t ctr_basic_old_shbin_size;
extern const uint8_t ctr_mesh_old_shbin[];
extern const size_t ctr_mesh_old_shbin_size;
extern const uint8_t ctr_skin_old_shbin[];
extern const size_t ctr_skin_old_shbin_size;
extern const uint8_t ctr_skin_env_old_shbin[];
extern const size_t ctr_skin_env_old_shbin_size;
/* (AI-assisted) config.ini fog_alpha: the mesh shaders with the fog amount in the color's alpha */
extern const uint8_t ctr_mesh_old_fa_shbin[];
extern const size_t ctr_mesh_old_fa_shbin_size;
/* (AI-assisted) config.ini shader_opt: the shorter programs (platform/3ds/shaders/ctr_*_opt.v.pica) */
extern const uint8_t ctr_mesh_opt_shbin[];
extern const size_t ctr_mesh_opt_shbin_size;
extern const uint8_t ctr_mesh_opt_fa_shbin[];
extern const size_t ctr_mesh_opt_fa_shbin_size;
extern const uint8_t ctr_skin_opt_shbin[];
extern const size_t ctr_skin_opt_shbin_size;
extern const uint8_t ctr_skin_env_opt_shbin[];
extern const size_t ctr_skin_env_opt_shbin_size;
extern const uint8_t ctr_ocean_full_opt_shbin[];
extern const size_t ctr_ocean_full_opt_shbin_size;
extern const uint8_t ctr_ocean_full_clip_opt_shbin[];
extern const size_t ctr_ocean_full_clip_opt_shbin_size;
extern const uint8_t ctr_mesh_opt_df_shbin[];
extern const size_t ctr_mesh_opt_df_shbin_size;
extern const uint8_t ctr_mesh_opt_df_fa_shbin[];
extern const size_t ctr_mesh_opt_df_fa_shbin_size;
#define OPT_DVLBS 8
/* (AI-assisted) config.ini shader_basic: 1 ctr_basic (default), 0 the Oct 2 ctr_basic_old */
static int s_basic_new = 1;
void ctr_gpu_set_shader_basic(int basic) {
  s_basic_new = basic ? 1 : 0;
}
void ctr_thread_install_crash_handler(void); /* platform/3ds/port */
static void gl_to_pica_check(void); /* (AI-assisted) */

/* (AI-assisted) hardware hang bisect: where the render thread is (ctr_gpu_diag_stage) and how
 * many frames it finished, logged once per second for the first 60 s by a watchdog thread */
static const char* volatile s_diag_stage = "not started";
static volatile u32 s_diag_frames;
static volatile Thread s_diag_render_thread;
void ctr_gpu_diag_stage(const char* stage) {
  s_diag_stage = stage;
}
/* the GPU code's own markers: render thread only (the loader thread makes textures too) */
static inline void diag_rt(const char* stage) {
  if (!s_diag_render_thread || threadGetCurrent() == s_diag_render_thread) {
    s_diag_stage = stage;
  }
}

/* from ctr_port.c: stop the console's buffer swaps once citro3d owns the screens */
void ctr_port_set_gpu_active(int active);
void ctr_linear_lock(void);
void ctr_linear_unlock(void);

/* (AI-assisted) libctru's linear and VRAM allocators take an empty free list for "not set up yet"
 * (MemPool::Ready() is "first free block != NULL"): an allocation that takes the last free byte
 * makes the next allocation set the heap up again as one free block, and memory that is in use is
 * handed out a second time. That happened when a level didn't fit (Azahar with the pipeline:
 * garbage polygons and textures, "linear free" at nearly the whole heap, then the sound mixer
 * running away on its overwritten buffer), and VRAM is filled with level textures as far as it
 * goes. All allocations here leave some free; textures made by citro3d check the room first. */
#define LINEAR_RESERVE (128u * 1024u)
#define VRAM_RESERVE (16u * 1024u)
static void* safe_linear_alloc(size_t size) {
  if ((size_t)linearSpaceFree() < size + LINEAR_RESERVE) {
    return NULL;
  }
  return linearAlloc(size);
}
static void* safe_vram_alloc(size_t size) {
  if ((size_t)vramSpaceFree() < size + VRAM_RESERVE) {
    return NULL;
  }
  return vramAlloc(size);
}
/* room for a texture citro3d allocates (worst case: 32 bits per texel, mip levels) */
static int linear_room_for_tex(int w, int h, int mipmapped) {
  const size_t bytes = (size_t)w * (size_t)h * 4u * (mipmapped ? 2u : 1u);
  return (size_t)linearSpaceFree() >= bytes + LINEAR_RESERVE;
}
#define linearAlloc safe_linear_alloc
#define vramAlloc safe_vram_alloc

/* (AI-assisted) CPU data cache flushes for memory the GPU reads, through the GSP service. Not
 * svcFlushProcessDataCache directly: it isn't in gk.rsf's SystemCallAccess, and on hardware a
 * system call the exheader doesn't allow raises an exception instead of returning an error (the
 * render thread died at its first flush: black top screen, the game hung after a few seconds).
 * Azahar doesn't check the list. Any thread. */
static u32 s_flush_requests; /* (AI-assisted, micro-gpu) statistics: GSP flush requests */
static void cache_flush_now(const void* addr, u32 size) {
  if (!size) {
    return;
  }
  s_flush_requests++;
  GSPGPU_FlushDataCache(addr, size);
}
/* a texture's texels (all levels; VRAM needs none), like C3D_TexFlush */
static void tex_flush_now(const C3D_Tex* tex) {
  const u32 a = (u32)tex->data;
  if (!(a >= OS_VRAM_VADDR && a < OS_VRAM_VADDR + OS_VRAM_SIZE)) {
    cache_flush_now(tex->data, C3D_TexCalcTotalSize(tex->size, tex->maxLevel));
  }
}

/* (AI-assisted) n words (a multiple of 4) of a and b equal: newlib's memcmp takes ~11
 * instructions per word, this ~2 (the draws compare their matrices, bones and state) */
typedef uint32_t __attribute__((may_alias)) alias_u32;
static inline int words_equal(const void* a, const void* b, unsigned n) {
  const alias_u32* x = (const alias_u32*)a;
  const alias_u32* y = (const alias_u32*)b;
  for (unsigned i = 0; i < n; i += 4) {
    if ((x[i] ^ y[i]) | (x[i + 1] ^ y[i + 1]) | (x[i + 2] ^ y[i + 2]) | (x[i + 3] ^ y[i + 3])) {
      return 0;
    }
  }
  return 1;
}

#define DISPLAY_TRANSFER_FLAGS                                                              \
  (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |        \
   GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
   GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))
/* (AI-assisted) config.ini color16: RGB565 color buffer to an RGB565 top screen (no conversion) */
#define DISPLAY_TRANSFER_FLAGS_565                                                          \
  (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |        \
   GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGB565) |                                          \
   GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB565) | GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

/* (AI-assisted) The frame's immediate vertices (sprites, HUD, sky, shadow volumes, the ocean),
 * two of them with the pipeline. 512 KB since Oct 17 (was 1536 KB): the largest frame measured
 * used ~100 KB (level_pairs.pad, Azahar) and the worst case adds up to ~360 KB (2176 sprites
 * 204 KB, the shadow volumes' 165 KB reservation, an ocean reservation of 152 KB is returned in
 * part); a draw that doesn't fit is dropped and counted (log: "vertex buffer peak N of 512 KB",
 * "N draws DROPPED (vertex buffer full)"), never written past the end. Frees 2 MB of linear
 * memory for the levels (c3l v17 tie levels of detail). */
#define VBUF_BYTES (512 * 1024)
#define MAX_STAGING 1024
#define MAX_TEXTURES 2048
#define MAX_POOLS 16
/* GPU command buffer: a frame's commands must all fit (splitting it doesn't make room). 2 MB of
 * linear memory; a busy frame (3000 level draws) takes ~1 MB. */
#if defined(OG3DS_GAME_JAK2) && OG3DS_GAME_JAK2
/* (AI-assisted, j2-mem2) Jak II: 1.25 MB (the pipeline's halves 640 KB). Haven City frames
 * take ~190 KB of commands (Azahar, baseline C: 835-854 draws, 75k triangles); a frame that
 * doesn't fit switches the pipeline off (CtrRenderer::render_frame) and has all of it. */
#define CMDBUF_BYTES (1280 * 1024)
#else
#define CMDBUF_BYTES (2 * 1024 * 1024)
#endif
/* command words kept free for the end of the frame, and needed for one draw with state changes */
#define CMD_RESERVE_WORDS 512
#define CMD_DRAW_WORDS 384
/* quads per ctr_gpu_draw_quads batch: 4 vertices each, u16 indices */
#define QUAD_MAX 4096
/* texture pools moved to VRAM per frame (each is one GX queue entry; the queue has 32) */
#define POOL_COPIES_PER_FRAME 2

typedef struct {
  C3D_Tex tex;
  int used;
  int pool;          /* -1: own memory, else the pool the texels are in */
  unsigned int offset; /* in the pool */
  unsigned int bytes;  /* in the pool (all mip levels) */
  uint8_t levels;
  /* (AI-assisted) pipeline: a second copy of the texels for textures rewritten every frame (sky,
   * clouds, ocean): written while the GPU still draws the previous frame from the other */
  void* alt;
  int alt_frame;
  /* (AI-assisted) ctr_gpu_tex_create_compact textures */
  uint8_t compact;    /* made by ctr_gpu_tex_create_compact (counted in the stats) */
  uint8_t alpha_full; /* texel alpha 0xff = 1.0 (compact formats store GS alpha doubled) */
  uint8_t want_vram;  /* waiting to move to the reserved VRAM (migrate_small_textures) */
  int16_t radial;     /* index into s_radial (procedural texture color table), -1: not radial */
  int arena_off;      /* >= 0: the texels are in the reserved VRAM at this offset, not citro3d's */
} TexSlot;

typedef struct {
  int used;           /* 0 free, 1 used, 2 pending delete, 3 reserved (being filled), 4 pending
                       * delete held one more frame (process_pending_deletes) */
  int ready;          /* all texels written (ctr_gpu_pool_ready) */
  uint8_t* linear;    /* the pool's bytes [linear_off, bytes) in linear memory (NULL if none) */
  void* vram;         /* the VRAM copy of [0, vram_bytes) the GPU reads, or NULL */
  unsigned int bytes;
  /* (AI-assisted) VRAM residency: the first vram_bytes (whole textures) are in VRAM, maybe not all
   * of the pool when VRAM is short. The part in VRAM isn't kept in linear memory (linear_off):
   * leaving VRAM copies it back (pool_leave_vram). */
  unsigned int vram_bytes;
  unsigned int linear_off;
  int priority;
  int no_room_frame;  /* last frame it didn't fit in VRAM (don't try again every frame) */
} TexPool;

#define MAX_MESHES 8192

typedef struct {
  void* verts;
  uint16_t* indices;
  int used; /* 0 free, 1 used, 2 pending delete, 3 reserved (being filled), 4 pending delete held
             * one more frame (process_pending_deletes) */
  /* (AI-assisted) pipeline: a second vertex buffer for meshes rewritten each frame (blend shapes:
   * faces), swapped once per frame like TexSlot::alt */
  void* verts_alt;
  uint32_t vbytes;
  int alt_frame;
  /* (AI-assisted) physical addresses of verts and indices (the raw draws) */
  u32 verts_pa, indices_pa;
  /* (AI-assisted) the buffer sizes between ctr_gpu_mesh_begin and _end */
  u32 fill_vbytes, fill_ibytes;
  /* (AI-assisted, streaming) ctr_gpu_mesh_begin_ext: verts is the caller's block (indices inside
   * it), given back to s_ext_free instead of freed */
  int ext;
} MeshSlot;
static void (*s_ext_free)(void* mem); /* (AI-assisted, streaming) ctr_gpu_set_ext_free */

enum {
  PROG_NONE = 0,
  PROG_BASIC,
  PROG_MESH,
  PROG_SKIN,
  PROG_SKIN_ENV,
  PROG_CLIP,
  PROG_CLIP2,
  PROG_OCEAN,
  PROG_OCEAN_FULL, /* (AI-assisted) ctr_gpu_ocean_full_draw (ctr_ocean_full.v.pica) */
  PROG_OCEAN_FULL_CLIP, /* (AI-assisted) ctr_gpu_ocean_full_draw_clip (ctr_ocean_full_clip.v.pica) */
  PROG_MESH_FA, /* (AI-assisted) fog_alpha: level meshes with the fog in the color's alpha */
  /* (AI-assisted) shader_opt with the game fog off: PROG_MESH / PROG_MESH_FA with only the draw
   * distance fog (ctr_mesh_opt_df / ctr_mesh_opt_df_fa.v.pica; set 2 only) */
  PROG_MESH_DF,
  PROG_MESH_DF_FA,
};
/* (AI-assisted) the level mesh programs (one vertex layout, the same uniforms) */
static inline int is_mesh_prog(int prog) {
  return prog == PROG_MESH || prog == PROG_MESH_FA || prog == PROG_MESH_DF ||
         prog == PROG_MESH_DF_FA;
}

/* (AI-assisted) The vertex programs and their uniforms' places, one shbin per program. Set 0: the
 * Oct 2 programs (hardware proven); set 1 (config.ini shader_opt): the ctr_*_opt ones. g.ps: the
 * set in use, switched at most once per frame (ctr_gpu_set_shader_opt). */
typedef struct {
  shaderProgram_s basic, mesh, mesh_fa, skin, env, clip, clip2, ocean, ocean_full, ocean_full_clip;
  shaderProgram_s mesh_df, mesh_df_fa; /* (AI-assisted) set 1: PROG_MESH_DF, PROG_MESH_DF_FA */
  int ok;      /* loaded */
  /* (AI-assisted) set 1 (config.ini shader_opt): mesh, mesh_fa, skin, env, ocean_full and
   * ocean_full_clip are the ctr_*_opt programs, whose uniforms carry factors the shaders no longer
   * apply: the mesh matrix has two fog rows after it (mesh_fog_rows), the skin programs' clip has
   * its x, y, z columns / 255 (unscaled weights) and skin's light colors the vertex color scale */
  int opt;
  int df_ok;   /* (AI-assisted) set 1: mesh_df and mesh_df_fa loaded, their uniforms in place */
  int fa_ok;   /* mesh_fa usable (fog_alpha) */
  int uloc_projection, uloc_clip, uloc_scales, uloc_fog0, uloc_fog1;
  int uloc_skin_clip, uloc_skin_rows[3], uloc_skin_scales, uloc_skin_lights;
  int uloc_skin_mfog; /* (AI-assisted, far-fog) the skin program's fog (ctr_skin_opt; -1: none) */
  int uloc_env_clip, uloc_env_rows[3], uloc_env_scales, uloc_env_fade;
  int uloc_clip_glpica, uloc_clip2_glpica;
  int uloc_ocean_clip, uloc_ocean_tex, uloc_ocean_scales, uloc_ocean_fog0, uloc_ocean_fog1;
  int uloc_ocean_full_gs, uloc_ocean_full_clip_gs;
} ProgSet;
static ProgSet s_ps[2];

static struct {
  int ready;
  C3D_RenderTarget* top;
  ProgSet* ps;          /* (AI-assisted) the program set in use (s_ps) */
  DVLB_s* opt_dvlb[OPT_DVLBS]; /* (AI-assisted) set 1's own shbins (shader_opt) */
  /* (AI-assisted) skin_bl_valid: the skin program's bones and lights (last_skin_bones_id /
   * last_skin_lights) are in its registers */
  int skin_bl_valid;
  DVLB_s* dvlb;
  DVLB_s* mesh_dvlb;
  /* (AI-assisted) config.ini fog_alpha: the mesh program with the fog amount in the color's alpha
   * (same uniforms at the same places; ProgSet::fa_ok: loaded and usable) */
  DVLB_s* mesh_fa_dvlb;
  u32 fog_abgr; /* the fog color as a combiner constant (fog_alpha's stage 1), R in the low byte */
  float fog0[4], fog1[4]; /* see ctr_gpu_set_mesh_fog, mesh_fog_uniforms */
  /* (AI-assisted) changes with fog0 (never 0): ctr_mesh_matrix::fog_gen; and the inputs of the
   * last ctr_gpu_set_mesh_fog that changed the uniforms (fog_in_valid: there was one) */
  uint32_t fog_gen;
  int fog_in_valid;
  const void* fog_in_ps;
  float fog_in0[4], fog_in1[4];
  int fog_df; /* (AI-assisted) the game fog is a constant 0: only the draw distance fog is left */
  /* (AI-assisted) the Oct 2 mesh program's fog uniforms: the game's values unchanged (the ocean
   * shader and shader_opt's programs use fog0/fog1) */
  float mfog0[4], mfog1[4];
  C3D_Tex fog_tex;        /* 64x8: alpha ramp 0..1 along s, fog color */
  uint32_t fog_tex_rgb;
  int fog_tex_valid;
  DVLB_s* skin_dvlb;
  DVLB_s* env_dvlb; /* the envmap pass of skinned meshes (ctr_skin_env.v.pica) */
  DVLB_s* clip_dvlb; /* immediate draws in clip space (ctr_clip.v.pica) */
  DVLB_s* clip2_dvlb; /* the same with two texture coordinates (ctr_clip2.v.pica: clouds) */
  /* (AI-assisted) the ocean with waves (ctr_ocean.v.pica, ctr_gpu_ocean_draw) */
  DVLB_s* ocean_dvlb;
  int ocean_tex2_bound; /* texture unit 2 holds the ocean's shimmer texture */
  int ocean_stage2;     /* combiner stage 2 holds the ocean's fog stage */
  /* (AI-assisted) fog_stage_skip: level draws without the fog stage (ctr_gpu_set_mesh_nofog), and
   * texture unit 1 (the fog ramp) switched off for them (raw draws) */
  int mesh_nofog;
  int unit1_off;
  DVLB_s* ocean_full_dvlb; /* (AI-assisted) the PS2 ocean's GS vertices (ctr_ocean_full.v.pica) */
  DVLB_s* ocean_full_clip_dvlb; /* (AI-assisted) the same in clip space (ctr_ocean_full_clip.v.pica) */
  C3D_Mtx ocean_full_gs; /* GS screen units (x, y 12.4, z 24 bits) -> PICA clip space at w = 1 */
  /* the open ctr_gpu_ocean_full_reserve: vertices and indices at these g.vbuf offsets */
  int ocean_full_open;
  size_t ocean_full_voff, ocean_full_ioff;
  int ocean_full_max_verts, ocean_full_max_indices;
  int cur_prog;
  /* (AI-assisted) the frame's clear color as a texture combiner constant (CTR_BLEND_OVER_CLEAR) */
  u32 clear_abgr;
  /* the procedural texture unit's color table in use (-1: the unit is off) */
  int proc_radial;
  ctr_draw_state last_state;
  int last_state_mesh;
  uint32_t last_tint;
  int last_state_valid;
  /* last ctr_gpu_draw_mesh: mesh and matrix (invalidated by any other program use) */
  int last_mesh_valid;
  int mesh_st_atlas; /* (AI-assisted) scales.x is 1/32768 (c3l v15 atlas draws), not 1/1024 */
  uint32_t last_clip_id; /* (AI-assisted) ctr_mesh_matrix::id of the clip uniform */
  /* last ctr_gpu_draw_skinned: its matrix and lights (a model's draws share them) */
  int last_skin_valid;
  float last_skin_clip[16];
  float last_skin_lights[28];
  int last_skin_palette_count;
  uint32_t last_skin_bones_id; /* (AI-assisted) the palette's bones_id (0: none) */
  /* (AI-assisted, far-fog) merc fog (ctr_gpu_set_merc_fog): hvdf.w, the amount's clamps, a number
   * new with every change; the clip w row the skin program's mfog was made from (C3D_FVec order)
   * and the number it was made with (0: not made since the clip upload) */
  float merc_fog_hvdf_w, merc_fog_lo, merc_fog_hi;
  uint32_t merc_fog_gen, skin_mfog_gen;
  float skin_wrow[4];
  /* (AI-assisted) cmd_dedup: the last ctr_gpu_draw_skinned_env's matrix and bones (env_valid: still
   * in the env program's registers) */
  int env_valid;
  int last_env_palette_count;
  float last_env_clip[16];
  float last_env_fade[3];
  uint32_t last_env_bones_id; /* (AI-assisted) as last_skin_bones_id */
  C3D_Mtx gl_to_pica;
  MeshSlot meshes[MAX_MESHES];
  int pending_mesh_delete[MAX_MESHES];
  int pending_mesh_delete_count;
  C3D_Mtx projection;
  uint8_t* vbuf;
  size_t vbuf_used;
  size_t vbuf_flushed; /* vbuf bytes already flushed from the CPU cache (flush_vbuf) */
  TexSlot textures[MAX_TEXTURES];
  TexPool pools[MAX_POOLS];
  uint16_t* quad_indices; /* QUAD_MAX quads: 0 1 3 3 1 2, + 4 per quad */
  /* texture unit 0 as the GPU has it: bind_texture only binds (and clears the texture cache) when
   * the texture, its memory or its parameters change */
  const C3D_Tex* bound_tex;
  const void* bound_data;
  u32* cmd_base; /* the command buffer at C3D_FrameBegin */
  u32 bound_param, bound_lod;
  int frame_no;
  ctr_gpu_stats cur, last;
  int in_frame;
  int pending_delete[MAX_TEXTURES];
  int pending_delete_count;
  void* pending_staging[MAX_STAGING]; /* linear buffers of queued VRAM texture copies */
  int pending_staging_count;
  /* (AI-assisted) frame_overlap2 = on: the staging buffers of the frame before (its copies may
   * still be queued behind the frame the GPU draws), freed one frame begin later */
  void* held_staging[MAX_STAGING];
  int held_staging_count;
  int vram_textures;
  int vram_copy_failures;
  int screen_tex; /* texture slot of ctr_gpu_copy_screen (-1: not made yet), 256x512 RGBA8 */
  int screen_tex_vram; /* (AI-assisted) it is in VRAM (distort_copy = rows) */
  char screenshot_path[256];
  int screenshot_state;  /* 0 none, 1 requested, 2 frame rendered: write at next frame begin */
} g;

/* (AI-assisted, gpu-rt) config.ini static_cmdlists: a stretch of level mesh draws being recorded
 * (ctr_gpu_draw_mesh_calls): its matrix uploads (slots: word offset from p0, then call | words <<
 * 24), and anything that makes the stretch unfit to copy again (bad: a citro3d draw; citro3d
 * updates and sky hook runs are counted) */
static struct {
  int on, bad, call, n, max;
  u32* p0;
  u32* slots;
} s_rec;
static u32 s_ctx_updates, s_hook_runs;

static int g_color16 = 0;
/* (AI-assisted) steady frame pacing, see pres_vblank */
static int g_steady_pacing;
static int pres_init(void);

/* (AI-assisted) config.ini early_depth = block32: the render target in 32x32 block mode
 * (GPUREG_FRAMEBUFFER_BLOCK32), which the early depth test seems to need (OpenLara's 3DS backend
 * left it off for this: "block32 256x416 input -> 240x400 output"). Block mode needs dimensions
 * that are multiples of 32: 256x416, the picture in columns 0..239 and rows 8..407 (the viewport
 * is 8 rows in: the GPU stores y flipped, so the same rows either way). The display transfer
 * untiles 32x32 blocks (flag bit 16) but only to an output that is a multiple of 32 too (3dbrew,
 * GPU/External_Registers), so it goes to s_b32_scratch (256x416 linear), and a texture copy that
 * skips the padding crops it into the 240x400 screen buffer. Needs steady pacing (our own screen
 * buffers); without it, or without the memory, the target stays 240x400 and early depth off. */
#define B32_W 256
#define B32_H 416
#define B32_ROW0 8
#define GX_TRANSFER_BLOCK32 (1u << 16)
static int g_block32 = 0;
static int g_block32_failed = 0;
static uint8_t* s_b32_scratch; /* 256x416 at 32 bits (16 with color16) */
/* (AI-assisted) config.ini depth16: a 16-bit depth buffer (no stencil: only ocean = waves uses it) */
static int g_depth16 = 0;

/* render buffer rows the picture starts at (the viewport's y) */
static int top_row0(void) {
  return g_block32 ? B32_ROW0 : 0;
}

/* the viewport: the whole render target, or with block32 the 240x400 inside the padding */
static void set_top_viewport(void) {
  if (g_block32) {
    C3D_SetViewport(0, B32_ROW0, 240, 400);
  } else {
    C3D_SetViewport(0, 0, g.top->frameBuf.width, g.top->frameBuf.height);
  }
}

/* The previous frame is still in the render target's color buffer (VRAM, tiled RGBA8 or RGB565,
 * 240x400 rotated) at the start of the next frame, after the GPU finished it. Write it as a BMP. */
static void write_screenshot(void) {
  const int fw = 240, fh = 400;  /* render buffer dimensions */
  const uint8_t* src = (const uint8_t*)g.top->frameBuf.colorBuf;
  if (g_block32) {
    return; /* (AI-assisted) the 32x32 block layout isn't decoded here */
  }
  FILE* f = fopen(g.screenshot_path, "wb");
  if (!f) {
    return;
  }
  (void)fh;
  const int W = 400, H = 240;
  const int row = W * 3;
  uint32_t data_size = row * H;
  uint8_t hdr[54] = {'B', 'M'};
  uint32_t file_size = 54 + data_size;
  memcpy(hdr + 2, &file_size, 4);
  uint32_t off = 54, hsz = 40, planes_bpp = 1 | (24 << 16);
  int32_t w = W, h = H;
  memcpy(hdr + 10, &off, 4);
  memcpy(hdr + 14, &hsz, 4);
  memcpy(hdr + 18, &w, 4);
  memcpy(hdr + 22, &h, 4);
  memcpy(hdr + 26, &planes_bpp, 4);
  memcpy(hdr + 34, &data_size, 4);
  fwrite(hdr, 1, 54, f);
  uint8_t* line = (uint8_t*)malloc(row);
  const int tiles_x = fw / 8;
  /* BMP rows are bottom-up */
  for (int y = H - 1; y >= 0; y--) {
    for (int x = 0; x < W; x++) {
      /* screen (x, y) -> render buffer (fx, fy) */
      int fx = (H - 1) - y;
      int fy = x;
      uint32_t tile = (uint32_t)((fy / 8) * tiles_x + (fx / 8));
      uint32_t px = tile * 64 + (((fx & 1)) | ((fy & 1) << 1) | ((fx & 2) << 1) | ((fy & 2) << 2) |
                                 ((fx & 4) << 2) | ((fy & 4) << 3));
      if (g_color16) {
        /* RGB565, R in the high bits */
        const uint32_t v = (uint32_t)src[2 * px] | ((uint32_t)src[2 * px + 1] << 8);
        const uint32_t r = (v >> 11) & 31, gr = (v >> 5) & 63, b = v & 31;
        line[3 * x + 0] = (uint8_t)((b << 3) | (b >> 2));
        line[3 * x + 1] = (uint8_t)((gr << 2) | (gr >> 4));
        line[3 * x + 2] = (uint8_t)((r << 3) | (r >> 2));
        continue;
      }
      uint32_t o = px * 4;
      /* stored A, B, G, R; BMP wants B, G, R */
      line[3 * x + 0] = src[o + 1];
      line[3 * x + 1] = src[o + 2];
      line[3 * x + 2] = src[o + 3];
    }
    fwrite(line, 1, row, f);
  }
  free(line);
  fclose(f);
}

void ctr_gpu_request_screenshot(const char* path) {
  strncpy(g.screenshot_path, path, sizeof(g.screenshot_path) - 1);
  g.screenshot_state = 1;
}

/* (AI-assisted) Reserved VRAM for the small textures made on the fly (sprites, HUD, font:
 * ctr_gpu_tex_create_compact). Taken once before the level texture pools fill VRAM (they move
 * whole pools in, so small textures scattered over VRAM would split it). First fit, in 128-byte
 * units, with the free ranges sorted by offset. Used by the render thread only. */
#define ARENA_BYTES (384 * 1024)
#define ARENA_ALIGN 128
#define ARENA_MAX_FREE 128
static struct {
  uint8_t* base;
  u32 free_off[ARENA_MAX_FREE];
  u32 free_len[ARENA_MAX_FREE];
  int nfree;
} s_arena;
/* (AI-assisted) a compact texture may be waiting to move there (migrate_small_textures) */
static int s_small_waiting;

static u32 arena_round(u32 bytes) {
  return (bytes + ARENA_ALIGN - 1) & ~(u32)(ARENA_ALIGN - 1);
}

static int arena_alloc(u32 bytes) {
  bytes = arena_round(bytes);
  for (int i = 0; i < s_arena.nfree; i++) {
    if (s_arena.free_len[i] >= bytes) {
      const u32 off = s_arena.free_off[i];
      s_arena.free_off[i] += bytes;
      s_arena.free_len[i] -= bytes;
      if (!s_arena.free_len[i]) {
        memmove(&s_arena.free_off[i], &s_arena.free_off[i + 1],
                (size_t)(s_arena.nfree - i - 1) * sizeof(u32));
        memmove(&s_arena.free_len[i], &s_arena.free_len[i + 1],
                (size_t)(s_arena.nfree - i - 1) * sizeof(u32));
        s_arena.nfree--;
      }
      return (int)off;
    }
  }
  return -1;
}

static void arena_free(u32 off, u32 bytes) {
  bytes = arena_round(bytes);
  int i = 0;
  while (i < s_arena.nfree && s_arena.free_off[i] < off) {
    i++;
  }
  /* merge with the range before and / or after */
  const int prev = i > 0 && s_arena.free_off[i - 1] + s_arena.free_len[i - 1] == off;
  const int next = i < s_arena.nfree && off + bytes == s_arena.free_off[i];
  if (prev && next) {
    s_arena.free_len[i - 1] += bytes + s_arena.free_len[i];
    memmove(&s_arena.free_off[i], &s_arena.free_off[i + 1],
            (size_t)(s_arena.nfree - i - 1) * sizeof(u32));
    memmove(&s_arena.free_len[i], &s_arena.free_len[i + 1],
            (size_t)(s_arena.nfree - i - 1) * sizeof(u32));
    s_arena.nfree--;
  } else if (prev) {
    s_arena.free_len[i - 1] += bytes;
  } else if (next) {
    s_arena.free_off[i] = off;
    s_arena.free_len[i] += bytes;
  } else if (s_arena.nfree < ARENA_MAX_FREE) {
    memmove(&s_arena.free_off[i + 1], &s_arena.free_off[i],
            (size_t)(s_arena.nfree - i) * sizeof(u32));
    memmove(&s_arena.free_len[i + 1], &s_arena.free_len[i],
            (size_t)(s_arena.nfree - i) * sizeof(u32));
    s_arena.free_off[i] = off;
    s_arena.free_len[i] = bytes;
    s_arena.nfree++;
  } /* else: too fragmented, the range stays used (lost until exit) */
}

/* (AI-assisted) Radial textures (glows) as procedural texture color tables: the texture's
 * average color by distance from its center (see check_radial). */
#define MAX_RADIAL 64
#define RADIAL_LUT 128 /* entries used: distance 0 (center) .. 1 (edge) */
static C3D_ProcTexColorLut* s_radial[MAX_RADIAL];
static C3D_ProcTex s_proctex;
static C3D_ProcTexLut s_proc_map; /* the identity: distance -> color table position */
/* (AI-assisted, micro-gpu) the tables in the procedural texture unit's LUT memory (what citro3d
 * last uploaded there), valid in apt epoch s_proc_lut_epoch (the HOME Menu and pica_bench use the
 * GPU too). Switching the unit off and on keeps them: a glow after other draws binds the unit
 * again but uploads its tables (128 + 2 x 256 words) only when another table is wanted. */
static const C3D_ProcTexLut* s_proc_lut_map_gpu;
static const C3D_ProcTexColorLut* s_proc_lut_color_gpu;
static u32 s_proc_lut_epoch;
/* what they were at the frame begin (cmd_dump's prologue: state from frames before) */
static const C3D_ProcTexLut* s_proc_lut_map_f0;
static const C3D_ProcTexColorLut* s_proc_lut_color_f0;

static void proc_lut_forget(const C3D_ProcTexColorLut* lut);
/* the texels of a slot: freed (or given back to the reserved VRAM) */
static void tex_slot_free_texels(TexSlot* t) {
  if (t->arena_off >= 0) {
    arena_free((u32)t->arena_off, C3D_TexCalcTotalSize(t->tex.size, t->tex.maxLevel));
    t->arena_off = -1;
    t->tex.data = NULL;
  } else if (t->pool < 0) {
    C3D_TexDelete(&t->tex);
  }
  if (t->radial >= 0) {
    /* (AI-assisted, micro-gpu) neither citro3d's context nor the LUT tracking may keep a freed
     * table (another one could get its address) */
    proc_lut_forget(s_radial[t->radial]);
    free(s_radial[t->radial]);
    s_radial[t->radial] = NULL;
    t->radial = -1;
  }
  t->compact = 0;
  t->alpha_full = 0;
  t->want_vram = 0;
}

/* Texture and mesh slots, the delete lists and linear memory are shared with the level loader
 * thread (CtrLevels creates a level's textures and meshes there): slot reservations, allocations,
 * frees and the delete lists are done under ctr_linear_lock (libctru's linear allocator has no lock
 * of its own). A slot is 3 (reserved) while its creator fills it; the renderer only draws handles
 * it was given, so it never reads a slot the loader is filling. */
static int reserve_tex_slot(void) {
  int slot = -1;
  ctr_linear_lock();
  for (int i = 0; i < MAX_TEXTURES; i++) {
    if (g.textures[i].used == 0) {
      g.textures[i].used = 3;
      slot = i;
      break;
    }
  }
  ctr_linear_unlock();
  return slot;
}

static void release_tex_slot(int slot) {
  ctr_linear_lock();
  g.textures[slot].used = 0;
  ctr_linear_unlock();
}

/* (AI-assisted, Jak II texture animation) ctr_gpu_tex_set_anim_slot / ctr_gpu_anim_slot_set:
 * a texture's slot + 1 (0: none), each slot's texture + 1 for this frame and the next */
static uint8_t s_tex_anim_slot[MAX_TEXTURES];
static int16_t s_anim_slot_tex[CTR_ANIM_SLOTS], s_anim_slot_next[CTR_ANIM_SLOTS];
static int s_anim_any;
static uint64_t s_anim_drawn, s_anim_drawn_last; /* slots drawn this frame / the last one */
/* the texture a draw of t uses */
static inline int tex_res(int t) {
  if (t >= 0 && t < MAX_TEXTURES && s_tex_anim_slot[t]) {
    const int sl = s_tex_anim_slot[t] - 1;
    s_anim_drawn |= 1ull << sl;
    const int a = s_anim_slot_tex[sl] - 1;
    if (a >= 0 && g.textures[a].used == 1) {
      return a;
    }
  }
  return t;
}
uint64_t ctr_gpu_anim_slots_drawn(void) {
  return s_anim_drawn_last | s_anim_drawn;
}
void ctr_gpu_tex_set_anim_slot(int handle, int slot) {
  if (handle >= 0 && handle < MAX_TEXTURES) {
    s_tex_anim_slot[handle] = slot >= 0 && slot < CTR_ANIM_SLOTS ? (uint8_t)(slot + 1) : 0;
  }
}
void ctr_gpu_anim_slot_set(int slot, int tex) {
  if (slot >= 0 && slot < CTR_ANIM_SLOTS) {
    s_anim_slot_next[slot] = (int16_t)(tex >= 0 && tex < MAX_TEXTURES ? tex + 1 : 0);
  }
}
static void anim_slots_frame_begin(void) {
  s_anim_drawn_last = s_anim_drawn;
  s_anim_drawn = 0;
  int any = 0;
  for (int i = 0; i < CTR_ANIM_SLOTS; i++) {
    s_anim_slot_tex[i] = s_anim_slot_next[i];
    any |= s_anim_slot_tex[i];
  }
  s_anim_any = any != 0;
}

static int reserve_mesh_slot(void) {
  int slot = -1;
  ctr_linear_lock();
  for (int i = 0; i < MAX_MESHES; i++) {
    if (g.meshes[i].used == 0) {
      g.meshes[i].used = 3;
      slot = i;
      break;
    }
  }
  ctr_linear_unlock();
  return slot;
}

/* textures deleted during a frame may still be read by the GPU: free them at the start of the
 * next frame, after C3D_FrameBegin(C3D_FRAME_SYNCDRAW) waited for the GPU */
/* hold (render.ini pipeline): the GPU may still draw the frame before the one that asked for a
 * delete, so a slot waits one more frame (2 -> 4 -> freed). Not 3: that is a slot or pool the
 * loader thread is still filling (the pool loop looks at every pool, not a delete list). */
/* (AI-assisted) hold 2 (frame_overlap2 = on, at the frame begin after waiting for frame N - 2
 * only): the staging buffers wait one more frame too. Their copies were queued while frame N - 1
 * was built (its frame begin's pool and small texture moves, the frame's own copies), behind frame
 * N - 2's draws, so they may not have run yet: freed then, a buffer could be reallocated and
 * rewritten before its copy reads it (garbage texels in VRAM). The other modes wait for everything
 * queued so far before they get here. */
static void process_pending_deletes(int hold) {
  ctr_linear_lock();
  for (int i = 0; i < g.held_staging_count; i++) {
    linearFree(g.held_staging[i]);
  }
  g.held_staging_count = 0;
  if (hold == 2) {
    memcpy(g.held_staging, g.pending_staging, sizeof(void*) * (size_t)g.pending_staging_count);
    g.held_staging_count = g.pending_staging_count;
  } else {
    for (int i = 0; i < g.pending_staging_count; i++) {
      linearFree(g.pending_staging[i]);
    }
  }
  g.pending_staging_count = 0;
  int kept = 0;
  for (int i = 0; i < g.pending_delete_count; i++) {
    int h = g.pending_delete[i];
    if (hold && g.textures[h].used == 2) {
      g.textures[h].used = 4;
      g.pending_delete[kept++] = h;
    } else if (g.textures[h].used == 2 || g.textures[h].used == 4) {
      tex_slot_free_texels(&g.textures[h]);
      if (g.textures[h].alt) {
        linearFree(g.textures[h].alt);
        g.textures[h].alt = NULL;
      }
      g.textures[h].used = 0;
      g.textures[h].pool = -1;
    }
  }
  g.pending_delete_count = kept;
  for (int i = 0; i < MAX_POOLS; i++) {
    TexPool* p = &g.pools[i];
    if (hold && p->used == 2) {
      p->used = 4;
    } else if (p->used == 2 || p->used == 4) {
      if (p->vram) {
        vramFree(p->vram);
      }
      if (p->linear) {
        linearFree(p->linear);
      }
      memset(p, 0, sizeof(*p));
    }
  }
  kept = 0;
  for (int i = 0; i < g.pending_mesh_delete_count; i++) {
    MeshSlot* m = &g.meshes[g.pending_mesh_delete[i]];
    if (hold && m->used == 2) {
      m->used = 4;
      g.pending_mesh_delete[kept++] = g.pending_mesh_delete[i];
    } else if (m->used == 2 || m->used == 4) {
      if (m->ext) {
        /* (AI-assisted, streaming) the caller's memory */
        if (s_ext_free) {
          s_ext_free(m->verts);
        }
        m->ext = 0;
      } else {
        linearFree(m->verts);
        linearFree(m->indices);
      }
      if (m->verts_alt) {
        linearFree(m->verts_alt);
      }
      m->verts = NULL;
      m->indices = NULL;
      m->verts_alt = NULL;
      m->used = 0;
    }
  }
  g.pending_mesh_delete_count = kept;
  ctr_linear_unlock();
}

static void raw_note_unif(void);

/* Programs have different vertex layouts and uniforms: switch both together. (AI-assisted) A
 * switch keeps none of the uniforms (another shbin: its uniforms and constants overlap). */
static void use_program(int prog) {
  if (g.cur_prog == prog) {
    return;
  }
  raw_note_unif(); /* (AI-assisted) citro3d sends the program and uniforms at the next draw */
  {
    /* (AI-assisted) statistics: a switch to another shbin uploads its shader code */
    static const void* last_dvlp;
    const shaderProgram_s* sp = prog == PROG_BASIC     ? &g.ps->basic
                                : prog == PROG_CLIP     ? &g.ps->clip
                                : prog == PROG_CLIP2    ? &g.ps->clip2
                                : prog == PROG_OCEAN    ? &g.ps->ocean
                                : prog == PROG_OCEAN_FULL ? &g.ps->ocean_full
                                : prog == PROG_OCEAN_FULL_CLIP ? &g.ps->ocean_full_clip
                                : prog == PROG_SKIN     ? &g.ps->skin
                                : prog == PROG_SKIN_ENV ? &g.ps->env
                                : prog == PROG_MESH_FA  ? &g.ps->mesh_fa
                                : prog == PROG_MESH_DF  ? &g.ps->mesh_df
                                : prog == PROG_MESH_DF_FA ? &g.ps->mesh_df_fa
                                                        : &g.ps->mesh;
    const void* dvlp = sp->vertexShader ? (const void*)sp->vertexShader->dvle->dvlp : NULL;
    g.cur.prog_switches++;
    g.cur.prog_uploads += dvlp != last_dvlp;
    last_dvlp = dvlp;
  }
  g.env_valid = 0; /* (AI-assisted) another program: the env uniforms' registers are reused */
  g.last_mesh_valid = 0;
  g.last_skin_valid = 0;
  g.skin_bl_valid = 0;
  int unif_vectors = 0; /* (AI-assisted) statistics: uniform vectors written here */
  C3D_AttrInfo* attr = C3D_GetAttrInfo();
  AttrInfo_Init(attr);
  if (prog == PROG_BASIC) {
    C3D_BindProgram(&g.ps->basic);
    g.last_state_valid = 0;
    AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 3);         /* position */
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2);         /* texcoord */
    AttrInfo_AddLoader(attr, 2, GPU_UNSIGNED_BYTE, 4); /* color */
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g.ps->uloc_projection, &g.projection);
    unif_vectors += 4;
  } else if (prog == PROG_CLIP || prog == PROG_CLIP2) {
    C3D_BindProgram(prog == PROG_CLIP ? &g.ps->clip : &g.ps->clip2);
    g.last_state_valid = 0;
    AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 4); /* clip space position */
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2); /* texcoord (0) */
    if (prog == PROG_CLIP2) {
      AttrInfo_AddLoader(attr, 2, GPU_FLOAT, 2);         /* texcoord 1 */
      AttrInfo_AddLoader(attr, 3, GPU_UNSIGNED_BYTE, 4); /* color */
    } else {
      AttrInfo_AddLoader(attr, 2, GPU_UNSIGNED_BYTE, 4); /* color */
    }
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,
                     prog == PROG_CLIP ? g.ps->uloc_clip_glpica : g.ps->uloc_clip2_glpica,
                     &g.gl_to_pica);
    unif_vectors += 4;
  } else if (prog == PROG_OCEAN) {
    /* (AI-assisted) the static mesh layout (texcoord unused by the shader) */
    C3D_BindProgram(&g.ps->ocean);
    g.last_state_valid = 0;
    AttrInfo_AddLoader(attr, 0, GPU_SHORT, 4);         /* position (+ pad) */
    AttrInfo_AddLoader(attr, 1, GPU_SHORT, 2);         /* texcoord (unused) */
    AttrInfo_AddLoader(attr, 2, GPU_UNSIGNED_BYTE, 4); /* color */
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g.ps->uloc_ocean_scales, 1.0f / 255.0f, 1.0f / 255.0f,
                  1.0f / 255.0f, 1.0f / 255.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g.ps->uloc_ocean_fog0, g.fog0[0], g.fog0[1], g.fog0[2],
                  g.fog0[3]);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g.ps->uloc_ocean_fog1, g.fog1[0], g.fog1[1], g.fog1[2],
                  g.fog1[3]);
    unif_vectors += 3;
  } else if (prog == PROG_OCEAN_FULL || prog == PROG_OCEAN_FULL_CLIP) {
    /* (AI-assisted) 40 bytes: GS x, y, z, q (or W * (x, y, z, 1)); s, t (times q), fog; color;
     * (j1ocean) the clip program also takes es, et (the sky reflection's texture coordinates) */
    C3D_BindProgram(prog == PROG_OCEAN_FULL ? &g.ps->ocean_full : &g.ps->ocean_full_clip);
    g.last_state_valid = 0;
    AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 4);         /* x, y, z, q / w */
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 3);         /* s, t, fog */
    AttrInfo_AddLoader(attr, 2, GPU_UNSIGNED_BYTE, 4); /* color */
    if (prog == PROG_OCEAN_FULL_CLIP) {
      AttrInfo_AddLoader(attr, 3, GPU_FLOAT, 2); /* es, et */
    }
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,
                     prog == PROG_OCEAN_FULL ? g.ps->uloc_ocean_full_gs
                                             : g.ps->uloc_ocean_full_clip_gs,
                     &g.ocean_full_gs);
    unif_vectors += 4;
  } else if (prog == PROG_SKIN || prog == PROG_SKIN_ENV) {
    C3D_BindProgram(prog == PROG_SKIN ? &g.ps->skin : &g.ps->env);
    g.last_state_valid = 0;
    AttrInfo_AddLoader(attr, 0, GPU_SHORT, 3);         /* position */
    AttrInfo_AddLoader(attr, 1, GPU_UNSIGNED_BYTE, 3); /* bone indices */
    AttrInfo_AddLoader(attr, 2, GPU_UNSIGNED_BYTE, 3); /* weights */
    AttrInfo_AddLoader(attr, 3, GPU_SHORT, 2);         /* texcoord * 1024 */
    AttrInfo_AddLoader(attr, 4, GPU_UNSIGNED_BYTE, 4); /* color */
    AttrInfo_AddLoader(attr, 5, GPU_BYTE, 3);          /* normal * 127 */
    C3D_FVUnifSet(GPU_VERTEX_SHADER,
                  prog == PROG_SKIN ? g.ps->uloc_skin_scales : g.ps->uloc_env_scales,
                  1.0f / 1024.0f, 1.0f / 255.0f, 1.0f, 1.0f / 127.0f);
    unif_vectors += 1;
  } else {
    /* PROG_MESH, (AI-assisted) or PROG_MESH_FA, PROG_MESH_DF(_FA): the same vertex layout and
     * uniforms */
    C3D_BindProgram(prog == PROG_MESH_FA      ? &g.ps->mesh_fa
                    : prog == PROG_MESH_DF    ? &g.ps->mesh_df
                    : prog == PROG_MESH_DF_FA ? &g.ps->mesh_df_fa
                                              : &g.ps->mesh);
    g.last_state_valid = 0;
    AttrInfo_AddLoader(attr, 0, GPU_SHORT, 4);         /* position (+ pad) */
    AttrInfo_AddLoader(attr, 1, GPU_SHORT, 2);         /* texcoord * 1024 */
    AttrInfo_AddLoader(attr, 2, GPU_UNSIGNED_BYTE, 4); /* color */
    /* (AI-assisted) skin's value (w = 1/127: no mesh shader reads w) */
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g.ps->uloc_scales, 1.0f / 1024.0f, 1.0f / 255.0f, 1.0f,
                  1.0f / 127.0f);
    unif_vectors += 1;
    g.mesh_st_atlas = 0; /* (AI-assisted) scales.x is 1/1024 */
    if (g.ps->opt) {
      /* (AI-assisted) shader_opt: the fog clamps (mesh_fog_uniforms' form); the fog factors are
       * in the matrix's fog rows (mesh_fog_rows) */
      C3D_FVUnifSet(GPU_VERTEX_SHADER, g.ps->uloc_fog1, g.fog1[0], g.fog1[1], g.fog1[2],
                    g.fog1[3]);
    } else {
      C3D_FVUnifSet(GPU_VERTEX_SHADER, g.ps->uloc_fog0, g.mfog0[0], g.mfog0[1], g.mfog0[2],
                    g.mfog0[3]);
      C3D_FVUnifSet(GPU_VERTEX_SHADER, g.ps->uloc_fog1, g.mfog1[0], g.mfog1[1], g.mfog1[2],
                    g.mfog1[3]);
      unif_vectors += 2;
    }
  }
  g.cur.prog_unif_vectors += unif_vectors;
  g.cur_prog = prog;
}

/* (AI-assisted) config.ini shader_opt, each frame (gpu_profile = ab switches it): the program set
 * for the rest of the frame. Every cache of what the GPU has is dropped with a change. */
static int s_want_opt;
static void prog_set_select(void) {
  ProgSet* want = s_want_opt && s_ps[1].ok ? &s_ps[1] : &s_ps[0];
  if (!g.ready || want == g.ps) {
    return;
  }
  g.ps = want;
  const int prog = g.cur_prog;
  g.cur_prog = PROG_NONE;
  if (prog != PROG_NONE) {
    use_program(prog);
  }
}

/* (AI-assisted) config.ini shader_opt, each frame (gpu_profile = ab switches it) */
void ctr_gpu_set_shader_opt(int on) {
  s_want_opt = on ? 1 : 0;
  prog_set_select();
}

int ctr_gpu_shader_opt_ready(void) {
  return s_ps[1].ok;
}

static void setup_projection(void) {
  /* x, y in [-1, 1] cover the whole 400x240 screen: the game renders for 5:3 (the kernel reports
   * a 400x240 window, so it widens the view like the PC port's widescreen; HUD and text keep their
   * proportions through its aspect ratio settings) */
  Mtx_OrthoTilt(&g.projection, -1.0f, 1.0f, -1.0f, 1.0f, 0.0f, 1.0f, true);
  /* depth: clip z = -z, so the stored depth (-z / w) is our z (GS: larger = closer) */
  g.projection.r[2].x = 0.0f;
  g.projection.r[2].y = 0.0f;
  g.projection.r[2].z = -1.0f;
  g.projection.r[2].w = 0.0f;

  /* OpenGL-style clip space -> PICA: same x/y mapping, z' = -(z + w) / 2 so the depth (-z'/w) is
   * (z/w + 1) / 2, the OpenGL depth. The game's matrices (tfrag3.vert, merc2.vert) put the GS
   * depth (larger = closer) there, and the PC renderer tests it with GL_GEQUAL. */
  Mtx_OrthoTilt(&g.gl_to_pica, -1.0f, 1.0f, -1.0f, 1.0f, 0.0f, 1.0f, true);
  g.gl_to_pica.r[2].x = 0.0f;
  g.gl_to_pica.r[2].y = 0.0f;
  g.gl_to_pica.r[2].z = -0.5f;
  g.gl_to_pica.r[2].w = -0.5f;
  g.gl_to_pica.r[3].x = 0.0f;
  g.gl_to_pica.r[3].y = 0.0f;
  g.gl_to_pica.r[3].z = 0.0f;
  g.gl_to_pica.r[3].w = 1.0f;
  gl_to_pica_check();

  /* (AI-assisted) the ocean's GS vertices -> g.projection's space like CtrDirect: x = (X / 16 -
   * 2048) / 256, y = (2048 - Y / 16) / 112 (Jak 1's 512 x 224 field), z = Z / (2^24 - 1) */
  C3D_Mtx gs_to_ctr;
  Mtx_Identity(&gs_to_ctr);
  gs_to_ctr.r[0].x = 1.0f / 4096.0f;
  gs_to_ctr.r[0].w = -8.0f;
  gs_to_ctr.r[1].y = -1.0f / 1792.0f;
  gs_to_ctr.r[1].w = 2048.0f / 112.0f;
  gs_to_ctr.r[2].z = 1.0f / 16777215.0f;
  Mtx_Multiply(&g.ocean_full_gs, &g.projection, &gs_to_ctr);
}

/* (AI-assisted) VRAM bank of a VRAM address: 0 = A (0x18000000..0x182FFFFF), 1 = B */
static int vram_bank(const void* p) {
  return osConvertVirtToPhys(p) >= 0x18300000u;
}

/* The top screen's render target: 240x400 (or block32's 256x416), RGBA8 / RGB565 color, D24S8
 * (or depth16's D16) depth. citro3d puts the depth buffer in the other VRAM bank when it fits,
 * else silently in the same one: then color and depth traffic share one bank. (AI-assisted) */
static C3D_RenderTarget* create_top_target(void) {
  const int w = g_block32 ? B32_W : 240, h = g_block32 ? B32_H : 400;
  C3D_RenderTarget* t = C3D_RenderTargetCreate(w, h, g_color16 ? GPU_RB_RGB565 : GPU_RB_RGBA8,
                                               g_depth16 ? GPU_RB_DEPTH16 : GPU_RB_DEPTH24_STENCIL8);
  if (!t) {
    return NULL;
  }
  C3D_FrameBuf* fb = &t->frameBuf;
  if (g_block32) {
    C3D_FrameBufAttrib(fb, (u16)w, (u16)h, true);
  }
  if (fb->depthBuf && vram_bank(fb->colorBuf) == vram_bank(fb->depthBuf)) {
    void* d = vramAllocAt(C3D_CalcDepthBufSize(w, h, fb->depthFmt),
                          vram_bank(fb->colorBuf) ? VRAM_ALLOC_A : VRAM_ALLOC_B);
    if (d) {
      vramFree(fb->depthBuf);
      C3D_FrameBufDepth(fb, d, fb->depthFmt);
    }
  }
  return t;
}

int ctr_gpu_describe_target(char* out, int size) {
  if (!g.ready || size <= 0) {
    return 0;
  }
  const C3D_FrameBuf* fb = &g.top->frameBuf;
  const u32 c = osConvertVirtToPhys(fb->colorBuf), d = osConvertVirtToPhys(fb->depthBuf);
  return snprintf(out, (size_t)size,
                  "%dx%d%s, color %s at 0x%08lx (VRAM %c), depth %s at 0x%08lx (VRAM %c)%s%s",
                  fb->width, fb->height, fb->block32 ? " block32" : "",
                  g_color16 ? "RGB565" : "RGBA8", (unsigned long)c, vram_bank(fb->colorBuf) ? 'B' : 'A',
                  fb->depthFmt == GPU_RB_DEPTH16 ? "D16" : "D24S8", (unsigned long)d,
                  vram_bank(fb->depthBuf) ? 'B' : 'A',
                  vram_bank(fb->colorBuf) == vram_bank(fb->depthBuf) ? " SAME BANK" : "",
                  g_block32_failed ? "; early_depth = block32 FAILED (needs frame_pacing = steady "
                                     "and 0.4 MB linear memory): 240x400, early depth off"
                                   : "");
}

/* (AI-assisted) a program set's uniform places (have_fa: its mesh_fa program is set up) */
static void prog_set_ulocs(ProgSet* p, int have_fa) {
  shaderInstance_s* vs = p->basic.vertexShader;
  p->uloc_projection = shaderInstanceGetUniformLocation(vs, "projection");
  vs = p->mesh.vertexShader;
  p->uloc_clip = shaderInstanceGetUniformLocation(vs, "clip");
  p->uloc_scales = shaderInstanceGetUniformLocation(vs, "scales");
  p->uloc_fog0 = shaderInstanceGetUniformLocation(vs, "fog0");
  p->uloc_fog1 = shaderInstanceGetUniformLocation(vs, "fog1");
  p->fa_ok = 0;
  if (have_fa) {
    vs = p->mesh_fa.vertexShader;
    p->fa_ok = shaderInstanceGetUniformLocation(vs, "clip") == p->uloc_clip &&
               shaderInstanceGetUniformLocation(vs, "scales") == p->uloc_scales &&
               shaderInstanceGetUniformLocation(vs, "fog0") == p->uloc_fog0 &&
               shaderInstanceGetUniformLocation(vs, "fog1") == p->uloc_fog1;
  }
  vs = p->skin.vertexShader;
  p->uloc_skin_clip = shaderInstanceGetUniformLocation(vs, "clip");
  p->uloc_skin_rows[0] = shaderInstanceGetUniformLocation(vs, "row0");
  p->uloc_skin_rows[1] = shaderInstanceGetUniformLocation(vs, "row1");
  p->uloc_skin_rows[2] = shaderInstanceGetUniformLocation(vs, "row2");
  p->uloc_skin_scales = shaderInstanceGetUniformLocation(vs, "scales");
  p->uloc_skin_lights = shaderInstanceGetUniformLocation(vs, "lights");
  p->uloc_skin_mfog = shaderInstanceGetUniformLocation(vs, "mfog"); /* (AI-assisted, far-fog) */
  vs = p->env.vertexShader;
  p->uloc_env_clip = shaderInstanceGetUniformLocation(vs, "clip");
  p->uloc_env_rows[0] = shaderInstanceGetUniformLocation(vs, "row0");
  p->uloc_env_rows[1] = shaderInstanceGetUniformLocation(vs, "row1");
  p->uloc_env_rows[2] = shaderInstanceGetUniformLocation(vs, "row2");
  p->uloc_env_scales = shaderInstanceGetUniformLocation(vs, "scales");
  p->uloc_env_fade = shaderInstanceGetUniformLocation(vs, "fade");
  p->uloc_clip_glpica = shaderInstanceGetUniformLocation(p->clip.vertexShader, "glpica");
  p->uloc_clip2_glpica = shaderInstanceGetUniformLocation(p->clip2.vertexShader, "glpica");
  vs = p->ocean.vertexShader;
  p->uloc_ocean_clip = shaderInstanceGetUniformLocation(vs, "clip");
  p->uloc_ocean_tex = shaderInstanceGetUniformLocation(vs, "tex");
  p->uloc_ocean_scales = shaderInstanceGetUniformLocation(vs, "oscales");
  p->uloc_ocean_fog0 = shaderInstanceGetUniformLocation(vs, "fog0");
  p->uloc_ocean_fog1 = shaderInstanceGetUniformLocation(vs, "fog1");
  p->uloc_ocean_full_gs = shaderInstanceGetUniformLocation(p->ocean_full.vertexShader, "gs");
  p->uloc_ocean_full_clip_gs =
      shaderInstanceGetUniformLocation(p->ocean_full_clip.vertexShader, "gs");
}

static void prog_set_free(ProgSet* p) {
  if (!p->ok) {
    return;
  }
  shaderProgram_s* all[] = {&p->basic, &p->mesh, &p->skin, &p->env, &p->clip,
                            &p->clip2, &p->ocean, &p->ocean_full, &p->ocean_full_clip};
  for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
    shaderProgramFree(all[i]);
  }
  shaderProgram_s* maybe[] = {&p->mesh_fa, &p->mesh_df, &p->mesh_df_fa};
  for (size_t i = 0; i < sizeof(maybe) / sizeof(maybe[0]); i++) {
    if (maybe[i]->vertexShader) {
      shaderProgramFree(maybe[i]);
    }
  }
  memset(p, 0, sizeof(*p));
}

/* (AI-assisted, micro-gpu) config.ini shader_resident (default on): every vertex program of both
 * sets in one code image that stays in the GPU's shader memory. citro3d uploads a program's code
 * and operand descriptors at every switch to a program of another DVLP (C3D_BindProgram compares
 * the DVLPs): with one shbin per program, every switch (~8 per frame) wrote its code again. Here
 * the shbins' code is put one after another (351 of 512 instruction slots) and their operand
 * descriptors into one table without duplicates (49 of 128; the ones a MAD uses first, MAD has 5
 * bits for the index), each instruction's descriptor index rewritten, and each program's DVLE
 * pointed at the image with its main offset moved by where its code went. So the first program
 * bound uploads all of it and a switch writes only the entry point and the output map (citro3d's
 * shaderProgramConfigure without the code). The instructions a draw runs are the same, only at
 * another place (cmd_report.py --kicks resolves them per draw). Only programs without flow control
 * (no addresses to move; check_shaders.py refuses flow control anyway) and with known instruction
 * formats are merged; anything else keeps its own shbins. Uniforms are unchanged: a switch still
 * keeps none (the programs' uniforms overlap). */
static u32 s_res_code[512];
static u32 s_res_desc[128];
static DVLP_s s_res_dvlp;
int ctr_config_bool(const char* key, int def); /* platform/3ds/port */

/* (AI-assisted, micro-gpu) config.ini gpu_micro (default on; off: every micro-gpu change of the
 * command stream and its submission back to the Oct 17 behaviour, for the safe preset):
 * the bits of s_micro. gpu_micro_ident = on (a test): each cmd_dump request dumps a frame with
 * s_micro = 0 and the frame after the next with all on (the same vertex buffer copy), for
 * cmd_report.py --kicks. */
enum {
  MICRO_RESIDENT = 1,   /* shader_resident's one code image (config.ini shader_resident too) */
  MICRO_TEXREGS = 2,    /* texture unit 0: only the registers that change (with cmd_dedup) */
  MICRO_GAS = 4,        /* GAS_DELTAZ_DEPTH only with a new depth function (with cmd_dedup) */
  MICRO_PROCLUT = 8,    /* the procedural texture tables kept in the GPU's LUT memory */
  MICRO_ATLAS = 16,     /* the mesh scales written raw */
  MICRO_FENCE = 32,     /* steady pacing's fence words in the vertex buffer */
  MICRO_FLUSHMERGE = 64, /* cache flush requests of near ranges merged */
  /* (AI-assisted, micro-mg) round 2 */
  MICRO_RAWGLOW = 128, /* the glows (procedural texture) as raw draws, not through citro3d */
  MICRO_TEXSTATE = 256, /* a raw draw state that differs only in its texture: only the bind */
  MICRO_FLUSHGAP = 512  /* deferred flush ranges up to DEFER_GAP2 apart in one request */
};
#define MICRO_ALL 1023u
static u32 s_micro = MICRO_ALL;
static u32 s_micro_cfg = MICRO_ALL; /* config.ini gpu_micro's */
static volatile u32 s_micro_want; /* (AI-assisted, micro-mg) ctr_gpu_set_micro_mask's (bit 31: set) */
static int s_micro_ident;    /* gpu_micro_ident */
static int s_micro_ident_st; /* its steps (ctr_gpu_frame_begin) */
/* the programs' DVLEs as their shbins have them and in the image (res_apply) */
static struct {
  DVLE_s* dvle;
  DVLP_s* own;
  u32 main, endmain, base;
} s_res_prog[24];
static int s_res_n;
static int s_res_on; /* the DVLEs point at the image */

/* the operand descriptor field of a known instruction: 7 bits (1), 5 (MAD, MADI: 2), none (0),
 * or -1: not merged */
static int res_desc_bits(u32 ins) {
  const u32 op = ins >> 26;
  if (op <= 0x0F || op == 0x12 || op == 0x13 || (op >= 0x18 && op <= 0x1B)) {
    return 1;
  }
  if (op == 0x21 || op == 0x22) {
    return 0; /* NOP, END */
  }
  if (op >= 0x30) {
    return 2;
  }
  return -1;
}

static void res_apply(int on);

static void resident_programs(void) {
  if (!ctr_config_bool("shader_resident", 1) || !(s_micro & MICRO_RESIDENT)) {
    printf("[ctr] shader_resident: off\n");
    return;
  }
  ProgSet* sets[2] = {&s_ps[0], &s_ps[1]};
  DVLE_s* dvles[2 * 12];
  int n = 0;
  for (int k = 0; k < 2; k++) {
    ProgSet* p = sets[k];
    if (!p->ok && k) {
      continue;
    }
    shaderProgram_s* all[12] = {&p->basic, &p->mesh, &p->mesh_fa, &p->skin, &p->env, &p->clip,
                                &p->clip2, &p->ocean, &p->ocean_full, &p->ocean_full_clip,
                                &p->mesh_df, &p->mesh_df_fa};
    for (int i = 0; i < 12; i++) {
      if (!all[i]->vertexShader) {
        continue;
      }
      DVLE_s* d = all[i]->vertexShader->dvle;
      int seen = 0;
      for (int j = 0; j < n; j++) {
        seen |= dvles[j] == d;
      }
      if (!seen) {
        dvles[n++] = d;
      }
    }
  }
  /* the distinct DVLPs, their code places, and the descriptor table (MAD's first) */
  DVLP_s* dvlps[2 * 12];
  u32 base[2 * 12];
  int np = 0;
  u32 code = 0, ndesc = 0;
  for (int i = 0; i < n; i++) {
    int seen = 0;
    for (int j = 0; j < np; j++) {
      seen |= dvlps[j] == dvles[i]->dvlp;
    }
    if (!seen) {
      DVLP_s* d = dvles[i]->dvlp;
      if (d->codeSize > 512 - code) {
        printf("[ctr] shader_resident: the programs don't fit in shader memory, off\n");
        return;
      }
      for (u32 k = 0; k < d->codeSize; k++) {
        const int bits = res_desc_bits(d->codeData[k]);
        const u32 di = bits == 2 ? (d->codeData[k] & 0x1F) : (d->codeData[k] & 0x7F);
        if (bits < 0 || (bits && di >= d->opdescSize)) {
          printf("[ctr] shader_resident: instruction %08lx not merged, off\n",
                 (unsigned long)d->codeData[k]);
          return;
        }
      }
      dvlps[np] = d;
      base[np] = code;
      code += d->codeSize;
      np++;
    }
  }
  for (int pass = 2; pass >= 1; pass--) {
    /* pass 2: the descriptors a MAD uses (indices < 32), pass 1: the others */
    for (int j = 0; j < np; j++) {
      const DVLP_s* d = dvlps[j];
      for (u32 k = 0; k < d->codeSize; k++) {
        if (res_desc_bits(d->codeData[k]) != pass) {
          continue;
        }
        const u32 v = d->opcdescData[d->codeData[k] & (pass == 2 ? 0x1F : 0x7F)];
        u32 f = 0;
        while (f < ndesc && s_res_desc[f] != v) {
          f++;
        }
        if (f == ndesc) {
          if (ndesc == 128 || (pass == 2 && ndesc == 32)) {
            printf("[ctr] shader_resident: too many operand descriptors, off\n");
            return;
          }
          s_res_desc[ndesc++] = v;
        }
      }
    }
  }
  for (int j = 0; j < np; j++) {
    const DVLP_s* d = dvlps[j];
    for (u32 k = 0; k < d->codeSize; k++) {
      u32 ins = d->codeData[k];
      const int bits = res_desc_bits(ins);
      if (bits) {
        const u32 m = bits == 2 ? 0x1Fu : 0x7Fu;
        const u32 v = d->opcdescData[ins & m];
        u32 f = 0;
        while (s_res_desc[f] != v) {
          f++;
        }
        ins = (ins & ~m) | f;
      }
      s_res_code[base[j] + k] = ins;
    }
  }
  s_res_dvlp.codeSize = code;
  s_res_dvlp.codeData = s_res_code;
  s_res_dvlp.opdescSize = ndesc;
  s_res_dvlp.opcdescData = s_res_desc;
  s_res_n = 0;
  for (int i = 0; i < n; i++) {
    for (int j = 0; j < np; j++) {
      if (dvles[i]->dvlp == dvlps[j]) {
        s_res_prog[s_res_n].dvle = dvles[i];
        s_res_prog[s_res_n].own = dvlps[j];
        s_res_prog[s_res_n].main = dvles[i]->mainOffset;
        s_res_prog[s_res_n].endmain = dvles[i]->endmainOffset;
        s_res_prog[s_res_n].base = base[j];
        s_res_n++;
        break;
      }
    }
  }
  res_apply(1);
  printf("[ctr] shader_resident: %d programs from %d shbins in one image, %lu instructions, %lu "
         "operand descriptors\n",
         n, np, (unsigned long)code, (unsigned long)ndesc);
}

/* (AI-assisted) config.ini shader_opt: set 1, set 0's basic, clip, clip2 and ocean (the same
 * shbins) with the ctr_*_opt programs for the rest */
static void load_opt_set(void) {
  static const uint8_t* const kBins[OPT_DVLBS] = {
      ctr_mesh_opt_shbin,       ctr_mesh_opt_fa_shbin,         ctr_skin_opt_shbin,
      ctr_skin_env_opt_shbin,   ctr_ocean_full_opt_shbin,      ctr_ocean_full_clip_opt_shbin,
      ctr_mesh_opt_df_shbin,    ctr_mesh_opt_df_fa_shbin};
  const size_t sizes[OPT_DVLBS] = {
      ctr_mesh_opt_shbin_size,       ctr_mesh_opt_fa_shbin_size,
      ctr_skin_opt_shbin_size,       ctr_skin_env_opt_shbin_size,
      ctr_ocean_full_opt_shbin_size, ctr_ocean_full_clip_opt_shbin_size,
      ctr_mesh_opt_df_shbin_size,    ctr_mesh_opt_df_fa_shbin_size};
  for (int i = 0; i < OPT_DVLBS; i++) {
    g.opt_dvlb[i] = DVLB_ParseFile((u32*)kBins[i], (u32)sizes[i]);
    if (!g.opt_dvlb[i]) {
      printf("[ctr] shader_opt: the programs didn't load, off\n");
      for (int k = 0; k < i; k++) {
        DVLB_Free(g.opt_dvlb[k]);
        g.opt_dvlb[k] = NULL;
      }
      return;
    }
  }
  ProgSet* p = &s_ps[1];
  struct {
    shaderProgram_s* prog;
    DVLE_s* dvle;
  } set[] = {
      {&p->basic, &g.dvlb->DVLE[0]},
      {&p->clip, &g.clip_dvlb->DVLE[0]},
      {&p->clip2, &g.clip2_dvlb->DVLE[0]},
      {&p->ocean, &g.ocean_dvlb->DVLE[0]},
      {&p->mesh, &g.opt_dvlb[0]->DVLE[0]},
      {&p->mesh_fa, &g.opt_dvlb[1]->DVLE[0]},
      {&p->skin, &g.opt_dvlb[2]->DVLE[0]},
      {&p->env, &g.opt_dvlb[3]->DVLE[0]},
      {&p->ocean_full, &g.opt_dvlb[4]->DVLE[0]},
      {&p->ocean_full_clip, &g.opt_dvlb[5]->DVLE[0]},
      {&p->mesh_df, &g.opt_dvlb[6]->DVLE[0]},
      {&p->mesh_df_fa, &g.opt_dvlb[7]->DVLE[0]},
  };
  for (size_t i = 0; i < sizeof(set) / sizeof(set[0]); i++) {
    shaderProgramInit(set[i].prog);
    shaderProgramSetVsh(set[i].prog, set[i].dvle);
  }
  prog_set_ulocs(p, 1);
  p->opt = 1;
  p->df_ok = 1;
  const shaderProgram_s* df[2] = {&p->mesh_df, &p->mesh_df_fa};
  for (int i = 0; i < 2; i++) {
    p->df_ok &= shaderInstanceGetUniformLocation(df[i]->vertexShader, "clip") == p->uloc_clip &&
                shaderInstanceGetUniformLocation(df[i]->vertexShader, "scales") == p->uloc_scales;
  }
  p->ok = p->uloc_clip >= 0 && p->uloc_fog1 >= 0 && p->uloc_skin_clip >= 0 &&
          p->uloc_skin_lights >= 0 && p->uloc_env_clip >= 0;
  printf("[ctr] shader_opt: %s (fog_alpha program %s, game fog off programs %s)\n",
         p->ok ? "ready" : "uniforms missing, off", p->fa_ok ? "ready" : "off",
         p->df_ok ? "ready" : "off");
}

int ctr_gpu_init(void) {
  if (g.ready) {
    return 0;
  }
  if (!C3D_Init(CMDBUF_BYTES)) {
    return -1;
  }
  if (g_color16) {
    /* (AI-assisted) config.ini color16: every pixel the GPU writes, blends or clears is half the
     * memory traffic, and so is the display transfer. No destination alpha (CTR_BLEND_ADD_DST_A
     * then adds fully); no dithering (gradients band slightly). */
    gfxSetScreenFormat(GFX_TOP, GSP_RGB565_OES);
  }
  g.top = create_top_target();
  if (!g.top) {
    return -2;
  }
  /* (AI-assisted) with steady pacing the frames go to our own screen buffers (pres_queue_frame) */
  const int pres_on = g_steady_pacing && pres_init();
  if (g_block32) {
    if (pres_on) {
      ctr_linear_lock();
      s_b32_scratch = (uint8_t*)linearAlloc(B32_W * B32_H * (g_color16 ? 2 : 4));
      ctr_linear_unlock();
    }
    if (!s_b32_scratch) {
      /* no block32 without our own screen buffers: the usual 240x400 target, early depth off */
      C3D_RenderTargetDelete(g.top);
      g_block32 = 0;
      g_block32_failed = 1;
      g.top = create_top_target();
      if (!g.top) {
        return -2;
      }
    }
  }
  if (!pres_on) {
    C3D_RenderTargetSetOutput(g.top, GFX_TOP, GFX_LEFT,
                              g_color16 ? DISPLAY_TRANSFER_FLAGS_565 : DISPLAY_TRANSFER_FLAGS);
  }

  printf("[ctr] shaders: basic %s, mesh / skin Oct 2 (shader_opt: the opt programs)\n",
         s_basic_new ? "new" : "old");
  g.ps = &s_ps[0];
  g.dvlb = s_basic_new ? DVLB_ParseFile((u32*)ctr_basic_shbin, (u32)ctr_basic_shbin_size)
                         : DVLB_ParseFile((u32*)ctr_basic_old_shbin, (u32)ctr_basic_old_shbin_size);
  shaderProgramInit(&g.ps->basic);
  shaderProgramSetVsh(&g.ps->basic, &g.dvlb->DVLE[0]);
  C3D_BindProgram(&g.ps->basic);

  g.mesh_dvlb = DVLB_ParseFile((u32*)ctr_mesh_old_shbin, (u32)ctr_mesh_old_shbin_size);
  shaderProgramInit(&g.ps->mesh);
  shaderProgramSetVsh(&g.ps->mesh, &g.mesh_dvlb->DVLE[0]);
  /* (AI-assisted) fog_alpha: its program (ctr_mesh_old_fa) has the mesh program's uniforms at the
   * same places (the draws upload them by g.ps->uloc_*) */
  g.mesh_fa_dvlb = DVLB_ParseFile((u32*)ctr_mesh_old_fa_shbin, (u32)ctr_mesh_old_fa_shbin_size);
  if (g.mesh_fa_dvlb) {
    shaderProgramInit(&g.ps->mesh_fa);
    shaderProgramSetVsh(&g.ps->mesh_fa, &g.mesh_fa_dvlb->DVLE[0]);
  }
  /* no fog until ctr_gpu_set_mesh_fog (AI-assisted: the shader's form, see mesh_fog_uniforms) */
  g.fog0[0] = -1.0f / 255.0f;
  g.fog0[1] = 0.0f;
  g.fog0[2] = -1.0f;
  g.fog0[3] = 0.0f;
  g.fog1[0] = 0.0f;
  g.fog1[1] = 0.0f;
  g.fog1[2] = 0.0f;
  g.fog1[3] = 0.0f;
  memcpy(g.mfog0, g.fog0, sizeof(g.mfog0));
  memcpy(g.mfog1, g.fog1, sizeof(g.mfog1));
  g.fog_gen = 1; /* (AI-assisted) */
  g.merc_fog_gen = 1; /* (AI-assisted, far-fog) no merc fog until ctr_gpu_set_merc_fog */
  g.merc_fog_hvdf_w = 255.0f;
  g.merc_fog_lo = g.merc_fog_hi = 0.0f;
  g.fog_in_valid = 0;
  {
    /* the Oct 2 shader's form: hvdf.w, fog min, fog max, -1/255; w scale, start, 1 / range */
    const float f0[4] = {0.0f, 255.0f, 255.0f, -1.0f / 255.0f}, f1[4] = {0.0f, 1.0f, 0.0f, 0.0f};
    memcpy(g.mfog0, f0, sizeof(f0));
    memcpy(g.mfog1, f1, sizeof(f1));
  }
  if (C3D_TexInit(&g.fog_tex, 64, 8, GPU_RGBA8)) {
    C3D_TexSetFilter(&g.fog_tex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&g.fog_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    g.fog_tex_valid = 1;
    g.fog_tex_rgb = 0xffffffff; /* force the first fill */
  }
  g.skin_dvlb = DVLB_ParseFile((u32*)ctr_skin_old_shbin, (u32)ctr_skin_old_shbin_size);
  shaderProgramInit(&g.ps->skin);
  shaderProgramSetVsh(&g.ps->skin, &g.skin_dvlb->DVLE[0]);
  g.env_dvlb = DVLB_ParseFile((u32*)ctr_skin_env_old_shbin, (u32)ctr_skin_env_old_shbin_size);
  shaderProgramInit(&g.ps->env);
  shaderProgramSetVsh(&g.ps->env, &g.env_dvlb->DVLE[0]);
  g.clip_dvlb = DVLB_ParseFile((u32*)ctr_clip_shbin, (u32)ctr_clip_shbin_size);
  shaderProgramInit(&g.ps->clip);
  shaderProgramSetVsh(&g.ps->clip, &g.clip_dvlb->DVLE[0]);
  g.clip2_dvlb = DVLB_ParseFile((u32*)ctr_clip2_shbin, (u32)ctr_clip2_shbin_size);
  shaderProgramInit(&g.ps->clip2);
  shaderProgramSetVsh(&g.ps->clip2, &g.clip2_dvlb->DVLE[0]);
  g.ocean_dvlb = DVLB_ParseFile((u32*)ctr_ocean_shbin, (u32)ctr_ocean_shbin_size);
  shaderProgramInit(&g.ps->ocean);
  shaderProgramSetVsh(&g.ps->ocean, &g.ocean_dvlb->DVLE[0]);
  g.ocean_full_dvlb = DVLB_ParseFile((u32*)ctr_ocean_full_shbin, (u32)ctr_ocean_full_shbin_size);
  shaderProgramInit(&g.ps->ocean_full);
  shaderProgramSetVsh(&g.ps->ocean_full, &g.ocean_full_dvlb->DVLE[0]);
  g.ocean_full_clip_dvlb =
      DVLB_ParseFile((u32*)ctr_ocean_full_clip_shbin, (u32)ctr_ocean_full_clip_shbin_size);
  shaderProgramInit(&g.ps->ocean_full_clip);
  shaderProgramSetVsh(&g.ps->ocean_full_clip, &g.ocean_full_clip_dvlb->DVLE[0]);
  prog_set_ulocs(&s_ps[0], g.mesh_fa_dvlb != NULL);
  s_ps[0].ok = 1;
  printf("[ctr] fog_alpha program: %s\n",
         s_ps[0].fa_ok ? "ready" : (g.mesh_fa_dvlb ? "uniforms don't match, off" : "none for this mesh shader, off"));
  load_opt_set(); /* (AI-assisted) config.ini shader_opt's programs (set 1) */
  /* (AI-assisted, micro-gpu) config.ini gpu_micro, gpu_micro_ident */
  s_micro = s_micro_cfg = ctr_config_bool("gpu_micro", 1) ? MICRO_ALL : 0;
  s_micro_ident = ctr_config_bool("gpu_micro_ident", 0);
  printf("[ctr] gpu_micro: %s%s\n", s_micro ? "on" : "off",
         s_micro_ident ? " (gpu_micro_ident: dumps off / on pairs)" : "");
  resident_programs(); /* (AI-assisted, micro-gpu) config.ini shader_resident */
  g.cur_prog = PROG_NONE;
  /* (AI-assisted) the procedural texture unit for radial textures (CTR_STATE_PROCTEX): distance
   * from the center (sqrt(u^2 + v^2) of the absolute texture coordinates, clamped to 1) looks up
   * the texture's color table through an identity map */
  {
    float map[129];
    for (int i = 0; i <= 128; i++) {
      map[i] = (float)i / 128.0f;
    }
    ProcTexLut_FromArray(&s_proc_map, map);
    C3D_ProcTexInit(&s_proctex, 0, RADIAL_LUT);
    C3D_ProcTexClamp(&s_proctex, GPU_PT_CLAMP_TO_EDGE, GPU_PT_CLAMP_TO_EDGE);
    C3D_ProcTexCombiner(&s_proctex, false, GPU_PT_SQRT2, GPU_PT_SQRT2);
    C3D_ProcTexNoiseEnable(&s_proctex, false);
    C3D_ProcTexShift(&s_proctex, GPU_PT_NONE, GPU_PT_NONE);
    C3D_ProcTexFilter(&s_proctex, GPU_PT_LINEAR);
    g.proc_radial = -1;
  }

  g.vbuf = (uint8_t*)linearAlloc(VBUF_BYTES);
  if (!g.vbuf) {
    return -3;
  }
  g.quad_indices = (uint16_t*)linearAlloc(QUAD_MAX * 6 * sizeof(uint16_t));
  if (!g.quad_indices) {
    return -4;
  }
  for (int q = 0; q < QUAD_MAX; q++) {
    static const uint16_t kQuad[6] = {0, 1, 3, 3, 1, 2};
    for (int k = 0; k < 6; k++) {
      g.quad_indices[6 * q + k] = (uint16_t)(4 * q + kQuad[k]);
    }
  }
  GSPGPU_FlushDataCache(g.quad_indices, QUAD_MAX * 6 * sizeof(uint16_t));
  for (int i = 0; i < MAX_TEXTURES; i++) {
    g.textures[i].pool = -1;
    g.textures[i].radial = -1;
    g.textures[i].arena_off = -1;
  }
  g.screen_tex = -1;
  setup_projection();
  use_program(PROG_BASIC);
  C3D_CullFace(GPU_CULL_NONE);
  ctr_port_set_gpu_active(1);
  g.ready = 1;
  /* one dark frame right away: until the game draws its first frame (after loading the common
   * files and the title level), the top screen keeps the HOME Menu's launch logo and the boot
   * looks frozen */
  ctr_gpu_frame_begin(12, 12, 20);
  ctr_gpu_frame_end();
  return 0;
}

void ctr_gpu_exit(void) {
  if (!g.ready) {
    return;
  }
  ctr_linear_lock();
  for (int i = 0; i < MAX_TEXTURES; i++) {
    if (g.textures[i].used) {
      tex_slot_free_texels(&g.textures[i]);
      if (g.textures[i].alt) {
        linearFree(g.textures[i].alt);
        g.textures[i].alt = NULL;
      }
      g.textures[i].used = 0;
    }
  }
  for (int i = 0; i < MAX_POOLS; i++) {
    if (g.pools[i].used) {
      if (g.pools[i].vram) {
        vramFree(g.pools[i].vram);
      }
      if (g.pools[i].linear) {
        linearFree(g.pools[i].linear);
      }
      memset(&g.pools[i], 0, sizeof(g.pools[i]));
    }
  }
  linearFree(g.quad_indices);
  for (int i = 0; i < MAX_MESHES; i++) {
    if (g.meshes[i].used) {
      if (!g.meshes[i].ext) { /* (AI-assisted, streaming) the stream arena's: not ours */
        linearFree(g.meshes[i].verts);
        linearFree(g.meshes[i].indices);
      }
      g.meshes[i].ext = 0;
      if (g.meshes[i].verts_alt) {
        linearFree(g.meshes[i].verts_alt);
        g.meshes[i].verts_alt = NULL;
      }
      g.meshes[i].used = 0;
    }
  }
  linearFree(g.vbuf);
  if (s_arena.base) {
    vramFree(s_arena.base);
    memset(&s_arena, 0, sizeof(s_arena));
  }
  ctr_linear_unlock();
  /* (AI-assisted) both program sets, then their shbins */
  prog_set_free(&s_ps[0]);
  prog_set_free(&s_ps[1]);
  g.ps = &s_ps[0];
  for (int i = 0; i < OPT_DVLBS; i++) {
    if (g.opt_dvlb[i]) {
      DVLB_Free(g.opt_dvlb[i]);
      g.opt_dvlb[i] = NULL;
    }
  }
  DVLB_s* dvlbs[] = {g.dvlb,      g.mesh_dvlb,  g.mesh_fa_dvlb, g.skin_dvlb,
                     g.env_dvlb,  g.clip_dvlb,  g.clip2_dvlb,   g.ocean_dvlb,
                     g.ocean_full_dvlb, g.ocean_full_clip_dvlb};
  for (size_t i = 0; i < sizeof(dvlbs) / sizeof(dvlbs[0]); i++) {
    if (dvlbs[i]) {
      DVLB_Free(dvlbs[i]);
    }
  }
  g.mesh_fa_dvlb = NULL;
  C3D_RenderTargetDelete(g.top);
  if (s_b32_scratch) {
    ctr_linear_lock();
    linearFree(s_b32_scratch);
    ctr_linear_unlock();
    s_b32_scratch = NULL;
  }
  C3D_Fini();
  ctr_port_set_gpu_active(0);
  g.ready = 0;
}

static void update_pool_residency(void);
static int g_vram_textures;

/* (AI-assisted) citro3d's internal context (source/internal.h of the citro3d that libcitro3d.a is
 * built from: ~/devkitpro-3ds/src/citro3d). The raw command path below keeps it in step with the
 * GPU (see "raw GPU commands"). The asserts check the layout against the library: its
 * C3Di_UpdateContext reads flags at 0x20, lightEnv at 0x10c, texEnvBuf at 0x19c, fogLut at 0x1a8,
 * and nm -S gives __C3D_Context 0x2d0 bytes. */
typedef struct {
  u32 fragOpMode;
  u32 fragOpShadow;
  u32 zScale, zOffset;
  GPU_CULLMODE cullMode;
  bool zBuffer, earlyDepth;
  GPU_EARLYDEPTHFUNC earlyDepthFunc;
  u32 earlyDepthRef;
  u32 alphaTest;
  u32 stencilMode, stencilOp;
  u32 depthTest;
  u32 blendClr;
  u32 alphaBlend;
  GPU_LOGICOP clrLogicOp;
} C3Di_Effect;
typedef struct {
  gxCmdQueue_s gxQueue;
  u32* cmdBuf;
  size_t cmdBufSize;
  float cmdBufUsage;
  u32 flags;
  shaderProgram_s* program;
  C3D_AttrInfo attrInfo;
  C3D_BufInfo bufInfo;
  C3Di_Effect effect;
  C3D_LightEnv* lightEnv;
  u32 texConfig;
  u32 texShadow;
  C3D_Tex* tex[3];
  C3D_TexEnv texEnv[6];
  u32 texEnvBuf, texEnvBufClr;
  u32 fogClr;
  C3D_FogLut* fogLut;
  u16 gasAttn, gasAccMax;
  u32 gasLightXY, gasLightZ, gasLightZColor;
  u32 gasDeltaZ : 24;
  u32 gasFlags : 8;
  C3D_GasLut* gasLut;
  C3D_ProcTex* procTex;
  C3D_ProcTexLut* procTexLut[3];
  C3D_ProcTexColorLut* procTexColorLut;
  C3D_FrameBuf fb;
  u32 viewport[5];
  u32 scissor[3];
  u16 fixedAttribDirty, fixedAttribEverDirty;
  C3D_FVec fixedAttribs[12];
} C3Di_Context;
_Static_assert(sizeof(C3Di_Context) == 0x2d0, "citro3d context layout");
_Static_assert(offsetof(C3Di_Context, flags) == 0x20, "citro3d context layout");
_Static_assert(offsetof(C3Di_Context, lightEnv) == 0x10c, "citro3d context layout");
_Static_assert(offsetof(C3Di_Context, texEnvBuf) == 0x19c, "citro3d context layout");
_Static_assert(offsetof(C3Di_Context, fogLut) == 0x1a8, "citro3d context layout");
extern C3Di_Context __C3D_Context;
/* (AI-assisted, micro-gpu) shader_resident: */
/* the DVLEs to the image (on) or their own shbins'. A change while a program is bound: citro3d
 * is made to upload the code at the next bind (no program bound: ctx->program), and the next
 * use_program binds (the frame begin's use_program(PROG_BASIC) does) */
static void res_apply(int on) {
  on = on && s_res_n > 0;
  if (on == s_res_on) {
    return;
  }
  for (int i = 0; i < s_res_n; i++) {
    DVLE_s* d = s_res_prog[i].dvle;
    d->dvlp = on ? &s_res_dvlp : s_res_prog[i].own;
    d->mainOffset = s_res_prog[i].main + (on ? s_res_prog[i].base : 0);
    d->endmainOffset = s_res_prog[i].endmain + (on ? s_res_prog[i].base : 0);
  }
  s_res_on = on;
  __C3D_Context.program = NULL;
  g.cur_prog = PROG_NONE;
}

/* (AI-assisted, micro-gpu) a radial texture's color table is freed */
static void proc_lut_forget(const C3D_ProcTexColorLut* lut) {
  if (s_proc_lut_color_gpu == lut) {
    s_proc_lut_color_gpu = NULL;
  }
  if (s_proc_lut_color_f0 == lut) {
    s_proc_lut_color_f0 = NULL;
  }
  if (__C3D_Context.procTexColorLut == lut) {
    C3D_ProcTexColorLutBind(NULL);
  }
}
void C3Di_UpdateContext(void); /* citro3d: sends what's dirty (as its draws do first) */
enum {
  C3DiF_Active = BIT(0),
  C3DiF_DrawUsed = BIT(1),
  C3DiF_BufInfo = BIT(3),
  C3DiF_Effect = BIT(4),
};
#define C3DiF_TexEnv(n) BIT(26 + (n))
#define C3DiF_Tex(n) BIT(23 + (n))
static int s_overlap = 0;
static unsigned s_partial_draws, s_partials;

/* (AI-assisted) render.ini pipeline: the CPU builds frame N+1 while the GPU draws frame N.
 * citro3d's C3D_FrameBegin waits for the GPU before a frame can be built (it reuses one command
 * buffer). With the pipeline the frame is built into one of two command and vertex buffers of
 * our own (citro3d's draw calls write wherever gpuCmdBuf points), the clear is queued at once
 * (it runs after frame N on the GPU), and only ctr_gpu_frame_end waits (C3D_FrameBegin), then
 * submits the frame with citro3d as usual. Deletes wait one more frame (process_pending_deletes).
 * Not done while building: screenshots. Textures updated in place (sky, eyes) may show one frame
 * early in frame N. */
static void flush_vbuf(void);
static void raw_vc_flush(void);
static void raw_frame_reset(void);
static void clip_reserve_reset(void);
static int s_pipeline = 0;
/* (AI-assisted) 1 MB each: the busiest jungle frames on hardware use ~450 KB of commands; a frame
 * that still runs out of room switches the pipeline off (CtrRenderer::render_frame) instead of
 * losing its last draws every frame. 2 MB each cost the linear memory a level needs.
 * (AI-assisted) They are the two halves of citro3d's own command buffer (C3D_Init's
 * CMDBUF_BYTES), which citro3d only writes in frames without the pipeline: 2 MB of linear memory
 * less. Its C3D_FrameBegin in ctr_gpu_frame_end only points gpuCmdBuf there for a moment. */
#define PIPE_CMD_BYTES (CMDBUF_BYTES / 2) /* (AI-assisted, j2-mem2) Jak 1: 1 MB, as before */
_Static_assert(2 * PIPE_CMD_BYTES <= CMDBUF_BYTES, "the pipeline's buffers are halves of citro3d's");
static void defer_linear_free(void* mem);
static u32* s_cmdbufs[2];
static uint8_t* s_vbufs[2];
static int s_k;

unsigned int ctr_gpu_cmd_buffer_kb(void) {
  return (s_pipeline ? PIPE_CMD_BYTES : CMDBUF_BYTES) / 1024;
}

int ctr_gpu_set_pipeline(int on) {
  if (!g.ready || g.in_frame) {
    return 0;
  }
  if (on && !s_pipeline) {
    if (!s_vbufs[1]) {
      ctr_linear_lock();
      s_vbufs[1] = (uint8_t*)linearAlloc(VBUF_BYTES);
      ctr_linear_unlock();
      if (!s_vbufs[1]) {
        return 0;
      }
    }
    s_vbufs[0] = g.vbuf;
    s_cmdbufs[0] = __C3D_Context.cmdBuf;
    s_cmdbufs[1] = __C3D_Context.cmdBuf + PIPE_CMD_BYTES / 4;
    /* the frames before were citro3d's: the GPU may still read their commands from the start of
     * the buffer the pipeline writes into next */
    gxCmdQueueWait(&__C3D_Context.gxQueue, -1);
    s_pipeline = 1;
  } else if (!on && s_pipeline) {
    /* (AI-assisted) citro3d has its buffer back: its next C3D_FrameBegin waits for the GPU before
     * writing to it. The second vertex buffer goes once the GPU is done with the frame that used
     * it (the next frame begin) */
    s_pipeline = 0;
    g.vbuf = s_vbufs[0];
    defer_linear_free(s_vbufs[1]);
    s_vbufs[1] = NULL;
  }
  return s_pipeline;
}

/* ---------------- (AI-assisted) steady frame pacing (config.ini frame_pacing) ----------------
 * citro3d shows a frame at the first vblank after the GPU finished it. The GPU time of a frame
 * varies around the 33 ms of 30 fps, so frames stayed on screen for 1, 2 or 3 vblanks while the
 * average was ~30 fps: choppy. Here each frame is transferred to one of our own screen buffers
 * right after its draws (in the GPU queue, so the render target is free for the next frame), a
 * texture copy after it writes the frame's number to a fence, and the vblank callback shows the
 * frame at release + L vblanks (release: the vblank the game thread handed it over at). Frames
 * then stay on screen exactly as long as the game time they cover. L goes up by one when a frame
 * misses its vblank and back down after a long run of frames with time to spare. */
/* frames in flight at latency L (2 vblanks per frame): L / 2 waiting, one being transferred, one
 * requested, one on screen. 4 buffers were too few at L = 6: the render thread waited for a free
 * one, which made frames late, which raised L (frames on screen for 5+ vblanks on hardware). */
#define PRESENT_BUFS 6
#define PRESENT_MAX_LATENCY 4
#define PRESENT_Q 8
static struct {
  int on;
  uint8_t* buf[PRESENT_BUFS];
  u32* fence; /* the GPU writes the number of the last transferred frame here */
  u32* ids;   /* 16 bytes per queue slot: the frame number the copy to the fence takes */
  u32 stride, mode;
  volatile u32 vblanks;      /* counted by the vblank callback */
  volatile u32 next_release; /* set with each frame by the game thread (ctr_gpu_note_release) */
  u32 cur_release;           /* the frame being built */
  struct {
    u32 id, target;
    int buf;
    volatile int seen; /* (AI-assisted) the callback saw it done (its slack is counted) */
  } q[PRESENT_Q];
  volatile u32 q_head, q_tail;            /* render thread writes q_tail, the callback q_head */
  volatile int buf_state[PRESENT_BUFS];   /* 0 free, 1 queued, 2 requested, 3 on screen */
  volatile int requested, shown, flip;
  volatile u32 last_shown;
  volatile u32 hist[5];                   /* frames on screen for 1, 2, 3, 4, 5+ vblanks */
  volatile u32 late, dropped;
  volatile int min_slack;                 /* vblanks to spare of the frames since the last check */
  /* (AI-assisted) frames first seen done in the vblank before their own (no vblank to spare):
   * a running count; jit_window / jit_stats: where the window and the stats started */
  volatile u32 just_in_time;
  u32 jit_window, jit_stats;
  int floor;          /* (AI-assisted) the latency doesn't go below this for floor_windows more */
  int floor_windows;  /* windows (a late frame was at floor - 1) */
  int latency;
  u32 frames_since_change;
  double buf_wait_ms; /* the render thread waiting for a free screen buffer */
} s_pres;

/* (AI-assisted) GPU time per frame that works with the pipeline. C3D_GetDrawingTime only times
 * from C3D_FrameEnd, and with work handed over in the middle of a frame (pipeline, overlap, the
 * distorter's screen copy, VRAM copies) most of the frame ran before that (it read 1-3 ms in
 * GPU-bound scenes). Here the GSP event thread notes when each frame's steady pacing fence landed
 * (its texture copy ends with a PPF event), and the render thread when it handed the frame's first
 * work over: GPU time(N) = done(N) - max(first(N), done(N - 1)). */
#define GT_RING 16
static struct {
  volatile u32 done_id;            /* the newest frame number the fence holds */
  volatile u64 done_tick[GT_RING]; /* when it landed, by frame number */
  u64 first_tick[GT_RING];         /* render thread: the frame's first work handed to the GPU */
  u32 first_id[GT_RING];
  /* (AI-assisted) render thread: the frame's first command list handed to the GPU (pipeline: at
   * the frame end or a partial submit; gpu_active_ms) */
  u64 submit_tick[GT_RING];
  u32 submit_id[GT_RING];
  u32 reported; /* frames up to this one are counted (gt_collect) */
} s_gt;

/* GSP event thread (core 0, high priority): kept tiny */
static void gt_ppf(void* unused) {
  (void)unused;
  svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32)s_pres.fence, 16);
  const u32 id = *(volatile u32*)s_pres.fence, last = s_gt.done_id;
  if (id == last) {
    return;
  }
  const u64 now = svcGetSystemTick();
  /* fences that landed since the last event: done by now */
  u32 n = (s32)(id - last) > 0 && id - last < GT_RING ? last + 1 : id;
  for (;; n++) {
    s_gt.done_tick[n % GT_RING] = now;
    if (n == id) {
      break;
    }
  }
  __dmb();
  s_gt.done_id = id;
}

/* render thread: the frame's first GPU work is handed over now (if not already) */
static void gt_note_first(void) {
  const u32 n = (u32)g.frame_no;
  if (s_gt.first_id[n % GT_RING] != n) {
    s_gt.first_tick[n % GT_RING] = svcGetSystemTick();
    s_gt.first_id[n % GT_RING] = n;
  }
}

/* (AI-assisted) render thread: the frame's first command list is handed over now (if not already) */
static void gt_note_submit(void) {
  const u32 n = (u32)g.frame_no;
  if (s_gt.submit_id[n % GT_RING] != n) {
    s_gt.submit_tick[n % GT_RING] = svcGetSystemTick();
    s_gt.submit_id[n % GT_RING] = n;
  }
}

/* render thread, at the frame end: the GPU time of the frames whose fence landed since the last
 * call, into g.cur */
static void gt_collect(void) {
  const u32 done = s_gt.done_id;
  __dmb();
  if ((s32)(done - s_gt.reported) <= 0) {
    return;
  }
  u32 n = s_gt.reported + 1;
  if (done - n >= GT_RING) {
    n = done - (GT_RING - 1);
  }
  for (; (s32)(done - n) >= 0; n++) {
    if (s_gt.first_id[n % GT_RING] != n) {
      continue; /* not noted (a frame before steady pacing started) */
    }
    const u64 end = s_gt.done_tick[n % GT_RING];
    u64 start = s_gt.first_tick[n % GT_RING];
    const u64 prev = s_gt.done_tick[(n - 1) % GT_RING]; /* (older than start if not N - 1's) */
    if (prev > start && prev <= end) {
      start = prev;
    }
    if (end > start) {
      g.cur.gpu_busy_ms += (float)((double)(end - start) / (SYSCLOCK_ARM11 / 1000.0));
      g.cur.gpu_busy_frames++;
      /* (AI-assisted) from the frame's command list on (not its clear at the frame begin) */
      u64 sub = s_gt.submit_id[n % GT_RING] == n ? s_gt.submit_tick[n % GT_RING] : start;
      if (sub < start) {
        sub = start;
      }
      if (prev > sub && prev <= end) {
        sub = prev;
      }
      if (end > sub) {
        g.cur.gpu_active_ms += (float)((double)(end - sub) / (SYSCLOCK_ARM11 / 1000.0));
      }
    }
  }
  s_gt.reported = done;
}

/* ---------------- (AI-assisted, gpu-rt) config.ini gpu_breakdown ----------------
 * Where the GPU's time per frame goes, measured on the console. Every 4th frame (pipeline only)
 * each part of the frame is a queue entry of its own that runs alone: the render thread waits for
 * the GPU to be idle, hands the part over and waits for its GSP event (P3D for a command list,
 * PSC0 / PSC1 for the clear's two fills, PPF for the screen copy and the transfer + fence copies);
 * the time is from the submission to the event's tick on the GSP event thread, so it includes the
 * GSP's dispatch (measured each frame with an almost empty list: "overhead"). The command list is
 * split where the phase changes (ctr_gpu_phase: per bucket from the renderer, the sky hook, the
 * screen copy here); each split adds a framebuffer flush (as the pipeline's partial submits). The
 * GPU and the CPU then take turns in those frames: slower while on (a profiling mode). */
static const char* const kPhaseNames[CTR_GPU_PHASES] = {
    "other", "clear", "sky", "level", "merc", "trans", "shadow", "ocean",
    "sprite", "distort", "hud", "transfer"};
enum { BD_P3D, BD_PSC0, BD_PSC1, BD_PPF, BD_EVENTS };
#define BD_EVERY 4
static struct {
  int on, hooked;
  int frame; /* this frame is measured */
  int phase; /* what the commands being built belong to */
  volatile u32 n[BD_EVENTS];
  volatile u64 t[BD_EVENTS];
  u32* probe; /* 4 words in linear memory: a framebuffer flush and the end (overhead) */
  double ms[CTR_GPU_PHASES];
  u32 lists[CTR_GPU_PHASES];
  double overhead_ms;
  u32 overhead_n, frames, timeouts, psc1_seen;
  u64 xfer_t0;
  u32 xfer_n0;
} s_bd;

static void gt_ppf(void* unused);
static void bd_event(int k) {
  s_bd.t[k] = svcGetSystemTick();
  __dmb();
  s_bd.n[k]++;
}
static void bd_p3d(void* u) {
  (void)u;
  bd_event(BD_P3D);
}
static void bd_psc0(void* u) {
  (void)u;
  bd_event(BD_PSC0);
}
static void bd_psc1(void* u) {
  (void)u;
  bd_event(BD_PSC1);
}
static void bd_ppf(void* u) {
  bd_event(BD_PPF);
  if (s_pres.on) {
    gt_ppf(u); /* the steady pacing fence's callback (this event's only other user) */
  }
}

static inline double ticks_ms(u64 t) {
  return (double)t / (SYSCLOCK_ARM11 / 1000.0);
}

void ctr_gpu_set_gpu_breakdown(int on) {
  s_bd.on = on ? 1 : 0;
  if (!s_bd.on || s_bd.hooked || !g.ready) {
    return;
  }
  ctr_linear_lock();
  s_bd.probe = (u32*)linearAlloc(16);
  ctr_linear_unlock();
  if (!s_bd.probe) {
    s_bd.on = 0;
    printf("[ctr] gpu_breakdown: no linear memory, off\n");
    return;
  }
  s_bd.probe[0] = 1;
  s_bd.probe[1] = GPUREG_FRAMEBUFFER_FLUSH | (0xFu << 16);
  s_bd.probe[2] = 0x12345678;
  s_bd.probe[3] = GPUREG_FINALIZE | (0xFu << 16);
  cache_flush_now(s_bd.probe, 16);
  gspSetEventCallback(GSPGPU_EVENT_P3D, bd_p3d, NULL, false);
  gspSetEventCallback(GSPGPU_EVENT_PSC0, bd_psc0, NULL, false);
  gspSetEventCallback(GSPGPU_EVENT_PSC1, bd_psc1, NULL, false);
  gspSetEventCallback(GSPGPU_EVENT_PPF, bd_ppf, NULL, false);
  s_bd.hooked = 1;
  printf("[ctr] gpu_breakdown: on (every %dth frame; the GPU runs each part alone)\n", BD_EVERY);
}

/* the GX queue idle (everything handed over is done), emptied and running */
static void bd_idle(void) {
  gxCmdQueue_s* q = &__C3D_Context.gxQueue;
  if (!gxCmdQueueWait(q, SYSCLOCK_ARM11)) {
    s_bd.timeouts++;
    return;
  }
  gxCmdQueueStop(q);
  gxCmdQueueClear(q);
  gxCmdQueueRun(q);
}

/* the tick of the want-th event k since n0 (the queue is waited for first); now if it never came
 * within 50 ms */
static u64 bd_wait(int k, u32 n0, u32 want) {
  gxCmdQueue_s* q = &__C3D_Context.gxQueue;
  if (!gxCmdQueueWait(q, SYSCLOCK_ARM11)) {
    s_bd.timeouts++;
  }
  const u64 t0 = svcGetSystemTick();
  while ((u32)(s_bd.n[k] - n0) < want) {
    if (svcGetSystemTick() - t0 > SYSCLOCK_ARM11 / 20) {
      s_bd.timeouts++;
      return svcGetSystemTick();
    }
    svcSleepThread(20000);
  }
  __dmb();
  return s_bd.t[k];
}

/* one command list alone on the GPU, timed for phase */
static void bd_list(u32* list, u32 words, int phase) {
  bd_idle();
  cache_flush_now(list, words * 4);
  const u32 n0 = s_bd.n[BD_P3D];
  const u64 t0 = svcGetSystemTick();
  GX_ProcessCommandList(list, words * 4, 0);
  const u64 t1 = bd_wait(BD_P3D, n0, 1);
  s_bd.ms[phase] += ticks_ms(t1 - t0);
  s_bd.lists[phase]++;
}

/* the commands built since the last split, as a part of the current phase */
static void bd_part(void) {
  raw_vc_flush();
  if (!gpuCmdBufOffset) {
    return;
  }
  flush_vbuf();
  GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_FLUSH, 1);
  GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_INVALIDATE, 1);
  u32* list;
  u32 words;
  GPUCMD_Split(&list, &words);
  if (words) {
    gt_note_submit();
    bd_list(list, words, s_bd.phase);
  }
}

void ctr_gpu_phase(int phase) {
  if (phase < 0 || phase >= CTR_GPU_PHASES) {
    phase = CTR_PH_OTHER;
  }
  if (s_bd.frame && phase != s_bd.phase && g.in_frame) {
    bd_part();
  }
  s_bd.phase = phase;
}

/* frame begin (pipeline): is this frame measured; then the previous frame finished */
static void bd_frame_begin(void) {
  s_bd.frame = s_bd.on && s_bd.hooked && s_pipeline && (g.frame_no % BD_EVERY) == 0;
  s_bd.phase = CTR_PH_OTHER;
  if (s_bd.frame) {
    bd_idle();
  }
}

/* the frame's clear (one or two fills), alone: bd_clear_begin before its GX_MemoryFill, then
 * bd_clear_end (two: both fill units) */
static u32 s_bd_c0, s_bd_c1;
static u64 s_bd_ct;
static void bd_clear_begin(void) {
  bd_idle();
  s_bd_c0 = s_bd.n[BD_PSC0];
  s_bd_c1 = s_bd.n[BD_PSC1];
  s_bd_ct = svcGetSystemTick();
}
static void bd_clear_end(int two) {
  u64 t1 = bd_wait(BD_PSC0, s_bd_c0, 1);
  if (two) {
    /* the second fill unit's event: libctru's queue only waits for PSC0, and Azahar sends no
     * PSC1 for a two-buffer fill. Up to 2 ms more (the fills run side by side) */
    const u64 w0 = svcGetSystemTick();
    while (s_bd.n[BD_PSC1] == s_bd_c1 && svcGetSystemTick() - w0 < SYSCLOCK_ARM11 / 500) {
      svcSleepThread(20000);
    }
    __dmb();
    if (s_bd.n[BD_PSC1] != s_bd_c1) {
      t1 = s_bd.t[BD_PSC1] > t1 ? s_bd.t[BD_PSC1] : t1;
      s_bd.psc1_seen++;
    }
  }
  s_bd.ms[CTR_PH_CLEAR] += ticks_ms(t1 - s_bd_ct);
  s_bd.lists[CTR_PH_CLEAR]++;
  /* the dispatch cost of a list that does nothing */
  bd_idle();
  const u32 p0 = s_bd.n[BD_P3D];
  const u64 t3 = svcGetSystemTick();
  GX_ProcessCommandList(s_bd.probe, 16, 0);
  s_bd.overhead_ms += ticks_ms(bd_wait(BD_P3D, p0, 1) - t3);
  s_bd.overhead_n++;
}

/* a texture copy in the frame (the distorter's screen copy, a pool moving to VRAM), alone */
static void bd_copy(u32* in, u32 indim, u32* out, u32 outdim, u32 size, u32 flags) {
  bd_part(); /* the draws before it */
  bd_idle();
  const u32 n0 = s_bd.n[BD_PPF];
  const u64 t0 = svcGetSystemTick();
  GX_TextureCopy(in, indim, out, outdim, size, flags);
  s_bd.ms[CTR_PH_DISTORT] += ticks_ms(bd_wait(BD_PPF, n0, 1) - t0);
  s_bd.lists[CTR_PH_DISTORT]++;
}

/* frame end: before the transfer is queued (its PPF events from here on) / when it starts */
static void bd_transfer_mark(void) {
  s_bd.xfer_t0 = svcGetSystemTick();
  s_bd.xfer_n0 = s_bd.n[BD_PPF];
}
static void bd_frame_end(int ppf_events) {
  if (!s_bd.frame) {
    return;
  }
  s_bd.ms[CTR_PH_TRANSFER] += ticks_ms(bd_wait(BD_PPF, s_bd.xfer_n0, (u32)ppf_events) - s_bd.xfer_t0);
  s_bd.lists[CTR_PH_TRANSFER]++;
  s_bd.frames++;
  s_bd.frame = 0;
}

int ctr_gpu_breakdown_line(char* out, int size, const char* label) {
  if (!s_bd.on) {
    return 0;
  }
  if (!s_bd.frames) {
    return snprintf(out, size, "[ctr] gpu breakdown %s: nothing measured (needs pipeline = on)",
                    label);
  }
  const double n = s_bd.frames;
  double sum = 0;
  int len = snprintf(out, size,
                     "[ctr] gpu breakdown %s over %u frames (every %dth frame, each part alone on "
                     "the GPU: the game runs slower while gpu_breakdown is on); GPU ms per frame "
                     "(lists per frame):",
                     label, s_bd.frames, BD_EVERY);
  for (int i = 0; i < CTR_GPU_PHASES && len < size; i++) {
    sum += s_bd.ms[i];
    len += snprintf(out + len, size - len, " %s %.2f (%.1f)", kPhaseNames[i], s_bd.ms[i] / n,
                    s_bd.lists[i] / n);
  }
  u32 lists = 0;
  for (int i = 0; i < CTR_GPU_PHASES; i++) {
    lists += s_bd.lists[i];
  }
  const double ovh = s_bd.overhead_n ? s_bd.overhead_ms / s_bd.overhead_n : 0.0;
  if (len < size) {
    len += snprintf(out + len, size - len,
                    "; sum %.2f; overhead %.3f ms per list (an empty list's submit to its event, "
                    "included above: %.2f ms per frame); clear: second fill unit's event in %u of "
                    "%u frames%s",
                    sum / n, ovh, ovh * lists / n, s_bd.psc1_seen, s_bd.frames,
                    s_bd.timeouts ? "; TIMEOUTS (an event didn't come in 50 ms)" : "");
  }
  memset(s_bd.ms, 0, sizeof(s_bd.ms));
  memset(s_bd.lists, 0, sizeof(s_bd.lists));
  s_bd.overhead_ms = 0;
  s_bd.overhead_n = s_bd.frames = s_bd.timeouts = s_bd.psc1_seen = 0;
  return len;
}

static void pres_vblank(void* unused) {
  (void)unused;
  const u32 v = ++s_pres.vblanks;
  if (s_pres.requested >= 0) {
    /* the buffer requested at the last vblank is on screen from this one */
    if (s_pres.shown >= 0 && s_pres.shown != s_pres.requested) {
      s_pres.buf_state[s_pres.shown] = 0;
    }
    s_pres.shown = s_pres.requested;
    s_pres.buf_state[s_pres.shown] = 3;
    s_pres.requested = -1;
    if (s_pres.last_shown) {
      const u32 d = v - s_pres.last_shown;
      s_pres.hist[d >= 5 ? 4 : (d ? d - 1 : 0)]++;
    }
    s_pres.last_shown = v;
  }
  if (s_pres.q_head == s_pres.q_tail) {
    return;
  }
  svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32)s_pres.fence, 16);
  const u32 done = *(volatile u32*)s_pres.fence;
  int pick = -1;
  while (s_pres.q_head != s_pres.q_tail) {
    const u32 qi = s_pres.q_head % PRESENT_Q;
    const u32 id = s_pres.q[qi].id;
    const u32 target = s_pres.q[qi].target;
    const int b = s_pres.q[qi].buf;
    if ((s32)(id - done) > 0) {
      break; /* the GPU isn't done with it */
    }
    const s32 slack = (s32)(target - (v + 1));
    /* (AI-assisted) The vblanks a frame had to spare: its slack when it is first seen done. It
     * used to be lowered to 0 when the frame was shown, which every frame on time is, so the
     * latency could only go up (it stayed at 4 for whole sessions on hardware). */
    if (!s_pres.q[qi].seen) {
      s_pres.q[qi].seen = 1;
      if (slack == 0) {
        s_pres.just_in_time++;
      }
      if (slack >= 0 && slack < s_pres.min_slack) {
        s_pres.min_slack = slack;
      }
    }
    if (slack > 0) {
      break; /* too early: its vblank comes later */
    }
    if (slack < 0 && slack >= -2) {
      /* missed its vblank by one or two: the latency is too short. A frame later than that is a
       * stall (a level load, a save), which more latency wouldn't have hidden. */
      s_pres.late++;
      s_pres.min_slack = -1;
    } else if (slack < 0) {
      s_pres.min_slack = -1;
    }
    if (pick >= 0) {
      s_pres.buf_state[pick] = 0; /* a newer frame is due too: skip this one */
      s_pres.dropped++;
    }
    pick = b;
    s_pres.q_head++;
  }
  if (pick >= 0) {
    s_pres.flip ^= 1;
    gspPresentBuffer(GFX_TOP, (unsigned)s_pres.flip, s_pres.buf[pick], s_pres.buf[pick],
                     s_pres.stride, s_pres.mode);
    s_pres.buf_state[pick] = 2;
    s_pres.requested = pick;
  }
}

static int g_steady_pacing = 1;

void ctr_gpu_set_steady_pacing(int on) {
  if (!g.ready) {
    g_steady_pacing = on ? 1 : 0; /* chosen in ctr_gpu_init */
  }
}

/* (AI-assisted) citro3d registers its own vblank callback again when the app comes back from the
 * HOME Menu or sleep (C3Di_RenderQueueEnableVBlank), which replaces ours: then our counter stops,
 * frames aren't shown and the game thread would wait forever. Called from the waits: a counter
 * that hasn't moved for 50 ms gets our callback back. */
static void pres_check_alive(void) {
  static u32 last_v;
  static u64 last_ms;
  if (!s_pres.on) {
    return;
  }
  const u32 v = s_pres.vblanks;
  const u64 now = osGetTime();
  if (v != last_v || last_ms == 0) {
    last_v = v;
    last_ms = now;
    return;
  }
  if (now - last_ms > 50) {
    gspSetEventCallback(GSPGPU_EVENT_VBlank0, pres_vblank, NULL, false);
    last_ms = now;
  }
}

/* the vblank counter the game thread paces with */
static u32 vblank_count(void) {
  return s_pres.on ? s_pres.vblanks : C3D_FrameCounter(0);
}

void ctr_gpu_note_release(void) {
  s_pres.next_release = vblank_count();
}

static int pres_init(void) {
  const GSPGPU_FramebufferFormat fmt = gfxGetScreenFormat(GFX_TOP);
  const u32 bpp = gspGetBytesPerPixel(fmt);
  const u32 bytes = 240 * 400 * bpp;
  /* (AI-assisted) libctru's two top screen framebuffers (gfxInit, in linear memory) are never
   * shown with this on, and each is 240x800 (room for the right eye or wide mode): four of our
   * frames. Only the rest is allocated: 1.1 MB of linear memory less (with 6 buffers of its own,
   * Azahar, where RGBA4 textures are stored as RGBA8, had no room left for misty's last models:
   * no zoomer). gfxGetFramebuffer gives the back buffer; with double buffering off, the front. */
  int borrowed = 0;
  {
    u8* fb[2];
    fb[0] = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    gfxSetDoubleBuffering(GFX_TOP, false);
    fb[1] = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, NULL, NULL);
    gfxSetDoubleBuffering(GFX_TOP, true);
    if (fb[0] && fb[1] && fb[0] != fb[1]) {
      for (int k = 0; k < 2 && borrowed + 2 <= PRESENT_BUFS; k++) {
        s_pres.buf[borrowed++] = fb[k];
        s_pres.buf[borrowed++] = fb[k] + bytes;
      }
    }
  }
  ctr_linear_lock();
  for (int i = borrowed; i < PRESENT_BUFS; i++) {
    s_pres.buf[i] = (uint8_t*)linearAlloc(bytes);
  }
  s_pres.fence = (u32*)linearAlloc(64);
  s_pres.ids = (u32*)linearAlloc(16 * PRESENT_Q);
  ctr_linear_unlock();
  int ok = s_pres.fence != NULL && s_pres.ids != NULL;
  for (int i = 0; i < PRESENT_BUFS; i++) {
    ok = ok && s_pres.buf[i];
  }
  if (!ok) {
    ctr_linear_lock();
    for (int i = 0; i < PRESENT_BUFS; i++) {
      if (i >= borrowed) {
        linearFree(s_pres.buf[i]);
      }
      s_pres.buf[i] = NULL;
    }
    linearFree(s_pres.fence);
    linearFree(s_pres.ids);
    s_pres.fence = NULL;
    s_pres.ids = NULL;
    ctr_linear_unlock();
    return 0;
  }
  for (int i = 0; i < PRESENT_BUFS; i++) {
    memset(s_pres.buf[i], 0, bytes);
    GSPGPU_FlushDataCache(s_pres.buf[i], bytes);
    s_pres.buf_state[i] = 0;
  }
  memset(s_pres.fence, 0, 64);
  GSPGPU_FlushDataCache(s_pres.fence, 64);
  memset(s_pres.ids, 0, 16 * PRESENT_Q);
  s_pres.stride = 240 * bpp;
  s_pres.mode = (u32)fmt | BIT(6) /* 2D */ | (1u << 8) /* in FCRAM */;
  s_pres.requested = s_pres.shown = -1;
  s_pres.q_head = s_pres.q_tail = 0;
  s_pres.latency = 3;
  s_pres.floor = 2;
  s_pres.min_slack = 99;
  s_pres.vblanks = C3D_FrameCounter(0);
  s_pres.on = 1;
  /* replaces citro3d's vblank counter (C3D_FrameCounter isn't used with this on) */
  gspSetEventCallback(GSPGPU_EVENT_VBlank0, pres_vblank, NULL, false);
  /* (AI-assisted) GPU time per frame from the fence (gt_ppf); nothing else uses this event's
   * callback (citro3d's queue gets the event before it) */
  gspSetEventCallback(GSPGPU_EVENT_PPF, gt_ppf, NULL, false);
  return 1;
}

/* after the frame's draws are in the GPU queue: its transfer to a free screen buffer, the
 * fence, and the frame handed to the vblank callback */
static void pres_queue_frame(u32* fence_id) {
  int b = -1;
  const u64 t0 = svcGetSystemTick();
  for (int tries = 0; b < 0; tries++) {
    if (s_pres.q_tail - s_pres.q_head < PRESENT_Q) {
      for (int i = 0; i < PRESENT_BUFS; i++) {
        if (s_pres.buf_state[i] == 0) {
          b = i;
          break;
        }
      }
    }
    if (b >= 0) {
      break;
    }
    if (tries > 300) {
      /* nothing is being shown (the app in the background?): drop the waiting frames */
      s_pres.q_head = s_pres.q_tail;
      for (int i = 0; i < PRESENT_BUFS; i++) {
        if (s_pres.buf_state[i] == 1) {
          s_pres.buf_state[i] = 0;
        }
      }
      continue;
    }
    pres_check_alive();
    svcSleepThread(1000000LL);
  }
  s_pres.buf_state[b] = 1;
  s_pres.buf_wait_ms += (double)(svcGetSystemTick() - t0) / (SYSCLOCK_ARM11 / 1000.0);
  if (s_bd.frame) {
    bd_transfer_mark(); /* (AI-assisted, gpu-rt) frame_overlap2 = on: the queue runs */
  }
  C3D_FrameSplit(GX_CMDLIST_FLUSH); /* the draws first */
  if (g_block32) {
    /* (AI-assisted) untile the 256x416 blocks into the scratch buffer, then copy rows 8..407,
     * 240 pixels of each 256 (in 16 byte units: 720 + 48 bytes in RGB8, 480 + 32 in RGB565) */
    const u32 bpp = g_color16 ? 2 : 3;
    GX_DisplayTransfer((u32*)g.top->frameBuf.colorBuf, GX_BUFFER_DIM(B32_W, B32_H),
                       (u32*)s_b32_scratch, GX_BUFFER_DIM(B32_W, B32_H),
                       (g_color16 ? DISPLAY_TRANSFER_FLAGS_565 : DISPLAY_TRANSFER_FLAGS) |
                           GX_TRANSFER_BLOCK32);
    GX_TextureCopy((u32*)(s_b32_scratch + B32_ROW0 * B32_W * bpp),
                   GX_BUFFER_DIM(240 * bpp / 16, (B32_W - 240) * bpp / 16), (u32*)s_pres.buf[b],
                   GX_BUFFER_DIM(240 * bpp / 16, 0), 240 * 400 * bpp, 8 /* raw copy */);
  } else {
    GX_DisplayTransfer((u32*)g.top->frameBuf.colorBuf, GX_BUFFER_DIM(240, 400),
                       (u32*)s_pres.buf[b], GX_BUFFER_DIM(240, 400),
                       g_color16 ? DISPLAY_TRANSFER_FLAGS_565 : DISPLAY_TRANSFER_FLAGS);
  }
  /* the fence: a 16 byte texture copy of this frame's number, after the transfer. Not a memory
   * fill: on hardware that unit only fills VRAM, and a fill of main memory never finished (the
   * GPU queue stalled: black top screen, frozen game, sound still playing). Azahar fills any
   * memory. */
  {
    /* (AI-assisted, micro-gpu) fence_id: the frame's number already in its vertex buffer
     * (ctr_gpu_frame_end), out of the CPU cache with the vertices: no flush request of its own */
    u32* id = fence_id;
    if (!id) {
      id = s_pres.ids + 4 * (s_pres.q_tail % PRESENT_Q);
      id[0] = id[1] = id[2] = id[3] = g.frame_no;
      cache_flush_now(id, 16);
    }
    GX_TextureCopy(id, 0, s_pres.fence, 0, 16, 8 /* raw copy */);
  }
  /* the latency: up after a late frame. (AI-assisted) Down after a window of 150 frames (~5 s)
   * without a late frame and with at most 2 that had no vblank to spare, but for ~30 s not below
   * the latency that made a frame late (the floor): stepping down into a scene that needs the
   * latency would make a late frame (a stutter) every window */
  s_pres.frames_since_change++;
  if (s_pres.late) {
    s_pres.late = 0;
    if (s_pres.latency < PRESENT_MAX_LATENCY) {
      s_pres.latency++;
    }
    s_pres.floor = s_pres.latency;
    s_pres.floor_windows = 6;
    s_pres.frames_since_change = 0;
    s_pres.jit_window = s_pres.just_in_time;
    s_pres.min_slack = 99;
  } else if (s_pres.frames_since_change >= 150) {
    const u32 jit = s_pres.just_in_time - s_pres.jit_window;
    if (s_pres.floor_windows > 0 && --s_pres.floor_windows == 0) {
      s_pres.floor = 2;
    }
    if (jit <= 2 && s_pres.latency > 2 && s_pres.latency > s_pres.floor) {
      s_pres.latency--;
    }
    s_pres.frames_since_change = 0;
    s_pres.jit_window = s_pres.just_in_time;
    s_pres.min_slack = 99;
  }
  const u32 t = s_pres.q_tail;
  s_pres.q[t % PRESENT_Q].id = g.frame_no;
  s_pres.q[t % PRESENT_Q].target = s_pres.cur_release + (u32)s_pres.latency;
  s_pres.q[t % PRESENT_Q].buf = b;
  s_pres.q[t % PRESENT_Q].seen = 0;
  __dsb();
  s_pres.q_tail = t + 1;
}

void ctr_gpu_pacing_stats(unsigned int hist[5], int* latency, unsigned int* dropped,
                          double* buffer_wait_ms, unsigned int* just_in_time, int* floor) {
  *buffer_wait_ms = s_pres.buf_wait_ms;
  s_pres.buf_wait_ms = 0;
  for (int i = 0; i < 5; i++) {
    hist[i] = s_pres.hist[i];
    s_pres.hist[i] = 0;
  }
  *latency = s_pres.on ? s_pres.latency : -1;
  *dropped = s_pres.dropped;
  s_pres.dropped = 0;
  const u32 jit = s_pres.just_in_time;
  *just_in_time = jit - s_pres.jit_stats;
  s_pres.jit_stats = jit;
  *floor = s_pres.floor;
}

static void migrate_small_textures(void);

/* (AI-assisted) config.ini tex_cache_clear (ctr_gpu_set_tex_cache_clear): 1 = uploads. A bind
 * that keeps unit 0's level pool texture (only its filter, wrap or mip settings change) skips the
 * cache clear unless a pool was filled (ctr_gpu_pool_ready) or moved (pool_repoint) or a new frame
 * began since the last clear (s_tex_dirty). Every switch to another texture clears (see
 * bind_texture). */
static int s_tex_clear_mode = 0;
static volatile int s_tex_dirty = 1;
void ctr_gpu_set_tex_cache_clear(int mode) {
  s_tex_clear_mode = mode ? 1 : 0;
}

/* (AI-assisted) config.ini mip_bias: GPUREG_TEXUNITn_LOD bits 0-12 (fixed 1.4.8) of mipmapped
 * textures, as C3D_TexSetLodBias */
static u16 s_lod_bias = 0;
void ctr_gpu_set_mip_bias(float bias) {
  int b = (int)(bias * 256.0f);
  b = b < -0x1000 ? -0x1000 : (b > 0xFFF ? 0xFFF : b);
  s_lod_bias = (u16)(b & 0x1FFF);
}

/* (AI-assisted) config.ini skin_rows_f24 */
static int s_skin_f24 = 0;
/* (AI-assisted) config.ini skin_clip_f24: merc clip matrix and lights as float24 too */
static int s_skin_clip_f24 = 0;
void ctr_gpu_set_skin_clip_f24(int on) {
  s_skin_clip_f24 = on ? 1 : 0;
}
void ctr_gpu_set_skin_rows_f24(int on) {
  s_skin_f24 = on ? 1 : 0;
}
int ctr_gpu_skin_rows_f24_on(void) {
  return s_skin_f24;
}

/* ---------------- (AI-assisted) config.ini frame_overlap2 (with the pipeline) ----------------
 * The pipeline's frame end used to wait for the GX queue to empty (C3D_FrameBegin: frame N - 1 and
 * the parts of N already sent), stop it, and only then hand N's command list over with
 * GX_CMDLIST_FLUSH: the GSP cleans the whole list (130-260 KB) from the CPU cache before the GPU
 * starts on it, while the GPU sits idle. 1 (clean): the list is closed and cleaned before the
 * wait, and handed over without the flush. 2 (on): the queue keeps running and the list is queued
 * behind frame N - 1; the next frame begin waits for frame N - 1 (whose command and vertex buffers
 * it reuses) with the steady pacing fence instead. libctru's queue only empties its entry list
 * (gxCmdQueueClear, a panic while it runs) when it is idle, and holds 32 entries: an idle queue
 * is emptied before new entries, a full running one waited for (overlap2_room). */
static int s_overlap2 = 1;
static u32 s_buf_frame[2]; /* mode 2: the frame drawn from s_cmdbufs / s_vbufs [k] (0: none) */
static int s_end_mode2;    /* the last pipelined frame end was mode 2 (deletes at the begin) */
static float s_begin_wait_ms;
static u32* s_fin_list;    /* the frame's closed command list (pipeline_close_list) */
static u32 s_fin_words;

void ctr_gpu_set_frame_overlap2(int mode) {
  s_overlap2 = mode < 0 ? 0 : (mode > 2 ? 2 : mode);
}

/* mode 2 needs the fence (frame_pacing = steady) */
static int overlap2_on(void) {
  return s_overlap2 == 2 && s_pres.on;
}

/* before adding n entries to the GX queue (mode 2) */
static void overlap2_room(int n) {
  gxCmdQueue_s* q = &__C3D_Context.gxQueue;
  const int idle = gxCmdQueueWait(q, 0);
  if (idle || q->numEntries + n >= q->maxEntries) {
    if (!idle) {
      gxCmdQueueWait(q, -1);
    }
    gxCmdQueueStop(q);
    gxCmdQueueClear(q);
  }
  gxCmdQueueRun(q); /* active (an added entry starts it); no-op while it runs */
}

/* mode 2: until frame id is done (its fence landed: its draws, transfer and everything before);
 * returns the milliseconds waited */
static float overlap2_wait_frame(u32 id) {
  if (!id) {
    return 0.0f;
  }
  gxCmdQueue_s* q = &__C3D_Context.gxQueue;
  const u64 t0 = svcGetSystemTick();
  for (;;) {
    svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32)s_pres.fence, 16);
    if ((s32)(*(volatile u32*)s_pres.fence - id) >= 0) {
      break;
    }
    if (gxCmdQueueWait(q, 0)) {
      if (q->lastEntry >= q->numEntries) {
        break; /* nothing left to run (the queue was emptied after the frame finished) */
      }
      gxCmdQueueRun(q);
    }
    gspWaitForAnyEvent();
  }
  return (float)((double)(svcGetSystemTick() - t0) / (SYSCLOCK_ARM11 / 1000.0));
}

/* modes 1 and 2: the rest of the frame's command list closed as C3Di_SplitFrame does (the
 * framebuffer flush and the early depth clear after its draws) and cleaned from the CPU cache:
 * s_fin_list, s_fin_words (0: nothing left) */
static void pipeline_close_list(void) {
  s_fin_list = NULL;
  s_fin_words = 0;
  if (!gpuCmdBufOffset) {
    return;
  }
  if (__C3D_Context.flags & C3DiF_DrawUsed) {
    __C3D_Context.flags &= ~(u32)C3DiF_DrawUsed;
    GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_FLUSH, 1);
    GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_INVALIDATE, 1);
    GPUCMD_AddWrite(GPUREG_EARLYDEPTH_CLEAR, 1);
  }
  GPUCMD_Split(&s_fin_list, &s_fin_words);
  cache_flush_now(s_fin_list, s_fin_words * 4);
}

/* ---------------- (AI-assisted) config.ini sky_last / skip_clear ----------------
 * The renderer hands the sky over as a hook (ctr_gpu_set_sky_hook) instead of drawing it first;
 * it runs right before the first draw that needs what is behind it (SKY_HOOK), so every blended
 * pixel still blends over the sky, and the sky draws itself with depth 0 (ctr_gpu_set_depth_far)
 * only where no opaque draw has written depth. Off (no hook set): nothing changes. */
/* (AI-assisted) ctr_gpu_set_early_depth_params: the sky's draws too; the early test's function
 * and the early depth data's clear value (defaults: the old GEQUAL and 0) */
static int g_early_sky = 0;
static int g_early_func = GPU_EARLYDEPTH_GEQUAL;
static u32 g_early_clear = 0;
static ctr_gpu_hook_fn s_sky_hook;
static void* s_sky_hook_arg;
static int g_depth_far;       /* ctr_gpu_set_depth_far */
static int s_zdef_valid;      /* the depth map's own scale and offset below were read */
static u32 s_zscale0, s_zoffset0;
static int s_skip_color_clear; /* ctr_gpu_set_skip_color_clear, for the next frame begin */

void ctr_gpu_set_sky_hook(ctr_gpu_hook_fn fn, void* arg) {
  s_sky_hook = fn;
  s_sky_hook_arg = arg;
}

void ctr_gpu_run_sky_hook(void) {
  const ctr_gpu_hook_fn fn = s_sky_hook;
  if (fn) {
    s_sky_hook = NULL; /* once (its own draws mustn't call it again) */
    s_hook_runs++;     /* (AI-assisted, gpu-rt) static_cmdlists */
    const int phase = s_bd.phase; /* (AI-assisted, gpu-rt) gpu_breakdown: the sky's own part */
    ctr_gpu_phase(CTR_PH_SKY);
    fn(s_sky_hook_arg);
    ctr_gpu_phase(phase);
  }
}

/* the sky may wait for a draw with this state: opaque, writing depth (so the sky's depth test
 * leaves its pixels alone), no stencil or no-color pass */
static inline int keeps_sky(const ctr_draw_state* st) {
  return st->blend == CTR_BLEND_OFF && st->zwrite &&
         !(st->flags & ~(CTR_STATE_EARLY_DEPTH | CTR_STATE_PROCTEX));
}
#define SKY_HOOK(st)                    \
  do {                                  \
    if (s_sky_hook && !keeps_sky(st)) { \
      ctr_gpu_run_sky_hook();           \
    }                                   \
  } while (0)
#define SKY_HOOK_ANY()        \
  do {                        \
    if (s_sky_hook) {         \
      ctr_gpu_run_sky_hook(); \
    }                         \
  } while (0)

void ctr_gpu_set_depth_far(int on) {
  on = on ? 1 : 0;
  if (on != g_depth_far) {
    if (!s_zdef_valid) {
      s_zscale0 = __C3D_Context.effect.zScale;
      s_zoffset0 = __C3D_Context.effect.zOffset;
      s_zdef_valid = 1;
    }
    g_depth_far = on;
    g.last_state_valid = 0;
  }
}

void ctr_gpu_set_skip_color_clear(int on) {
  s_skip_color_clear = on ? 1 : 0;
}

/* the parts of the GPU state a frame starts from (both modes) */
static void frame_state_reset(void) {
  s_pres.cur_release = s_pres.next_release;
  raw_frame_reset();
  g.cur_prog = PROG_NONE;
  g.last_state_valid = 0;
  g.bound_tex = NULL;
  s_tex_dirty = 1; /* (AI-assisted) tex_cache_clear = uploads: the frame's first bind clears */
  g.ocean_full_open = 0; /* (AI-assisted) a ctr_gpu_ocean_full_reserve never drawn */
  g.mesh_nofog = 0;      /* (AI-assisted) and unit 1 on again with the ramp (bound below) */
  g.unit1_off = 0;
  clip_reserve_reset();  /* (AI-assisted) and a ctr_gpu_clip_reserve */
  use_program(PROG_BASIC);
  /* the fog ramp stays on texture unit 1 for the whole frame (binding a texture clears the GPU's
   * texture cache: once per frame, not at every level draw) */
  if (g.fog_tex_valid) {
    C3D_TexBind(1, &g.fog_tex);
  }
  /* (AI-assisted) the early depth buffer starts as "far" like the depth buffer (cleared to 0;
   * larger = closer). citro3d clears it only after frames that drew; the first frame needs it. */
  if (g_block32) {
    /* (AI-assisted) citro3d binds the render target at the first draw, after this clear: set the
     * block mode and the target's place and size first (as C3Di_FrameBufBind), so the early depth
     * data is cleared for this target from the very first frame on */
    const C3D_FrameBuf* fb = &g.top->frameBuf;
    u32 param[3];
    param[0] = osConvertVirtToPhys(fb->depthBuf) >> 3;
    param[1] = osConvertVirtToPhys(fb->colorBuf) >> 3;
    param[2] = 0x01000000 | (((u32)(fb->height - 1) & 0xFFF) << 12) | (fb->width & 0xFFF);
    GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_INVALIDATE, 1);
    GPUCMD_AddIncrementalWrites(GPUREG_DEPTHBUFFER_LOC, param, 3);
    GPUCMD_AddWrite(GPUREG_RENDERBUF_DIM, param[2]);
    GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_BLOCK32, 1);
  }
  /* (AI-assisted) GBATEK: EARLYDEPTH_DATA is the clear value of the early depth data (its own
   * storage, not the depth buffer), EARLYDEPTH_CLEAR fills it (also at the end of every citro3d
   * split): the config's value (early_depth_clear, default 0 = far), and the context keeps it so
   * every later clear uses the same */
  GPUCMD_AddMaskedWrite(GPUREG_EARLYDEPTH_DATA, 0x7, g_early_clear);
  GPUCMD_AddWrite(GPUREG_EARLYDEPTH_CLEAR, 1);
  C3D_EarlyDepthTest(false, (GPU_EARLYDEPTHFUNC)g_early_func, g_early_clear);
  C3D_ProcTexBind(0, NULL);
  g.proc_radial = -1;
  g.vbuf_used = 0;
  g.vbuf_flushed = 0;
  s_partial_draws = 0;
  s_partials = 0;
  memset(&g.cur, 0, sizeof(g.cur));
  g.in_frame = 1;
  migrate_small_textures();
}

/* (AI-assisted) the clear color as the color buffer stores it (memory fill value) */
static u32 color_fill_value(uint8_t r, uint8_t gr, uint8_t b) {
  if (g_color16) {
    return ((u32)(r >> 3) << 11) | ((u32)(gr >> 2) << 5) | (u32)(b >> 3);
  }
  return ((u32)r << 24) | ((u32)gr << 16) | ((u32)b << 8) | 0xff;
}

static void pipeline_frame_begin(u32 clear) {
  if (g.screenshot_state == 2) {
    /* (AI-assisted) the frame to save is complete once the GPU has nothing queued (this frame's
     * clear isn't queued yet): wait for it, only in frames that take a screenshot */
    if (gxCmdQueueWait(&__C3D_Context.gxQueue, 1000000000LL)) {
      write_screenshot();
    }
    g.screenshot_state = 0;
  }
  s_k ^= 1;
  s_begin_wait_ms = 0.0f;
  if (overlap2_on()) {
    /* (AI-assisted) frame_overlap2 = on: the frame before the last one drew from these buffers;
     * then the deletes (as the frame end does in the other modes; not twice after one of those) */
    s_begin_wait_ms = overlap2_wait_frame(s_buf_frame[s_k]);
    s_buf_frame[s_k] = 0;
    if (s_end_mode2) {
      process_pending_deletes(2);
    }
    overlap2_room(8);
  }
  GPUCMD_SetBuffer(s_cmdbufs[s_k], PIPE_CMD_BYTES / 4, 0);
  g.cmd_base = gpuCmdBuf;
  g.vbuf = s_vbufs[s_k];
  g.frame_no++;
  bd_frame_begin(); /* (AI-assisted, gpu-rt) gpu_breakdown: the frame before is done first */
  gt_note_first(); /* (AI-assisted) the clear below starts the GPU on this frame */
  /* copies to VRAM wait for the GPU first (citro3d's safe copy outside of a frame) */
  update_pool_residency();
  if (s_bd.frame) {
    bd_clear_begin();
  }
  C3D_FrameBuf* fb = &g.top->frameBuf;
  u32* color = (u32*)fb->colorBuf;
  u32* depth = (u32*)fb->depthBuf;
  /* D24S8: 4 bytes (D16: 2); color 4 (RGBA8) or 2 (RGB565) */
  const u32 px = (u32)fb->width * fb->height;
  u32* color_end = (u32*)((uint8_t*)color + px * (g_color16 ? 2 : 4));
  if (s_skip_color_clear) {
    /* (AI-assisted) skip_clear: the depth only (VRAM) */
    GX_MemoryFill(depth, 0, (u32*)((uint8_t*)depth + px * (g_depth16 ? 2 : 4)),
                  GX_FILL_TRIGGER | (g_depth16 ? GX_FILL_16BIT_DEPTH : GX_FILL_32BIT_DEPTH), NULL,
                  0, NULL, 0);
  } else {
    GX_MemoryFill(color, clear, color_end,
                  GX_FILL_TRIGGER | (g_color16 ? GX_FILL_16BIT_DEPTH : GX_FILL_32BIT_DEPTH), depth,
                  0, (u32*)((uint8_t*)depth + px * (g_depth16 ? 2 : 4)),
                  GX_FILL_TRIGGER | (g_depth16 ? GX_FILL_16BIT_DEPTH : GX_FILL_32BIT_DEPTH));
  }
  if (s_bd.frame) {
    bd_clear_end(!s_skip_color_clear);
  }
  C3D_SetFrameBuf(fb);
  set_top_viewport();
  frame_state_reset();
  g.cur.color_clear_skipped = s_skip_color_clear;
  g.cur.gpu_wait_ms = s_begin_wait_ms; /* (AI-assisted) frame_overlap2 = on (after the reset) */
}

/* send what's built so far to the GPU (pipeline: our command buffer, citro3d isn't in a frame) */
static void pipeline_submit(void) {
  raw_vc_flush();
  flush_vbuf();
  GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_FLUSH, 1);
  GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_INVALIDATE, 1);
  u32* list;
  u32 words;
  GPUCMD_Split(&list, &words);
  if (overlap2_on()) {
    overlap2_room(4); /* (AI-assisted) this list and a copy or two after it */
  }
  if (words) {
    gt_note_submit();
    GX_ProcessCommandList(list, words * 4, GX_CMDLIST_FLUSH);
  }
}

static volatile int s_cmd_dump_next;
static void proc_lut_snapshot(void);
void ctr_gpu_frame_begin(uint8_t r, uint8_t gr, uint8_t b) {
  if (!g.ready) {
    return;
  }
  anim_slots_frame_begin(); /* (AI-assisted) */
  if (s_micro_ident_st) {
    /* (AI-assisted, micro-gpu) gpu_micro_ident: frame k with everything off (dumped), k + 1 on,
     * k + 2 on (dumped): k and k + 2 draw from the same vertex buffer copy */
    const int st = s_micro_ident_st;
    s_micro = st == 1 ? 0 : s_micro_cfg;
    s_micro_ident_st = st == 3 ? 0 : st + 1;
    if (st != 2) {
      s_cmd_dump_next = 1;
    }
  }
  if (s_micro_want) {
    /* (AI-assisted, micro-mg) ctr_gpu_set_micro_mask (a test): from this frame on */
    s_micro = s_micro_cfg = s_micro_want & MICRO_ALL;
    s_micro_want = 0;
    printf("[ctr] gpu_micro mask 0x%lx\n", (unsigned long)s_micro);
  }
  res_apply((s_micro & MICRO_RESIDENT) != 0); /* (AI-assisted, micro-gpu) */
  proc_lut_snapshot();
  /* texture combiner constant: R in the low byte */
  g.clear_abgr = 0xff000000u | ((u32)b << 16) | ((u32)gr << 8) | (u32)r;
  s_sky_hook = NULL; /* (AI-assisted) sky_last: a hook left from a frame that didn't end */
  if (s_pipeline) {
    pipeline_frame_begin(color_fill_value(r, gr, b));
    return;
  }
  diag_rt("frame_begin: C3D_FrameBegin (wait for GPU)");
  const u64 wait0 = svcGetSystemTick();
  C3D_FrameBegin(0); /* waits for the GPU; the game thread already paces to vblank */
  const float wait_ms = (float)((double)(svcGetSystemTick() - wait0) / (SYSCLOCK_ARM11 / 1000.0));
  diag_rt("frame_begin: deletes, VRAM pool copies");
  g.cmd_base = gpuCmdBuf;
  g.frame_no++;
  process_pending_deletes(0);
  /* before any draw: copies to VRAM are queued ahead of this frame's draws */
  update_pool_residency();
  if (g.screenshot_state == 2) {
    write_screenshot();
    g.screenshot_state = 0;
  }
  /* (AI-assisted) skip_clear: the depth only */
  C3D_RenderTargetClear(g.top, s_skip_color_clear ? C3D_CLEAR_DEPTH : C3D_CLEAR_ALL,
                        color_fill_value(r, gr, b), 0);
  C3D_FrameDrawOn(g.top);
  if (g_block32) {
    set_top_viewport();
  }
  frame_state_reset();
  g.cur.color_clear_skipped = s_skip_color_clear;
  g.cur.gpu_wait_ms = wait_ms; /* (AI-assisted) (after the reset of g.cur) */
  diag_rt("draw build");
}

/* Immediate draws copy their vertices into g.vbuf. The GPU reads them only when the frame's GX
 * queue runs (C3D_FrameEnd; C3D_FrameBegin stopped the queue, so command list splits wait in it
 * until then), so one cache flush of what this frame wrote is enough. Each flush is an IPC to the
 * GSP service: one per draw cost tens of microseconds each on hardware (free in Azahar). */
/* (AI-assisted) The render thread's flushes for this frame's GPU work (texture updates: sky,
 * clouds, ocean, eyes; blend shape vertices; the fog ramp) wait here until the frame's commands
 * go to the GPU (flush_vbuf, at every submission point), merged where the ranges touch. */
#define DEFER_FLUSH_MAX 48
#define DEFER_GAP (8u * 1024u) /* (AI-assisted, micro-gpu) see flush_vbuf */
/* (AI-assisted, micro-mg) with MICRO_FLUSHGAP. Azahar charges ~16.7k ticks (62 us) of the render
 * thread for each GSP flush request (an IPC; tens of microseconds on hardware too), and the level's
 * time of day colors alone flushed 20-35 ranges per frame (Snowy, Misty). Cleaning the cache lines
 * of a 64 KB gap is 2048 line operations: less than one request even if the kernel cleans line by
 * line (if it cleans the whole cache above a size, as citro3d's whole linear heap flush per frame
 * relies on, merging costs nothing). */
#define DEFER_GAP2 (64u * 1024u)
extern u32 __ctru_linear_heap, __ctru_linear_heap_size; /* libctru */
static struct {
  u32 start[DEFER_FLUSH_MAX], end[DEFER_FLUSH_MAX];
  int n;
} s_defer;

static int g_defer_flush = 1;
void ctr_gpu_set_defer_flush(int on) {
  g_defer_flush = on;
}

static void cache_flush_frame(const void* addr, u32 size) {
  if (!g.in_frame || !g_defer_flush) {
    cache_flush_now(addr, size);
    return;
  }
  const u32 a = (u32)addr, e = a + size;
  for (int i = 0; i < s_defer.n; i++) {
    if (a <= s_defer.end[i] && e >= s_defer.start[i]) {
      s_defer.start[i] = a < s_defer.start[i] ? a : s_defer.start[i];
      s_defer.end[i] = e > s_defer.end[i] ? e : s_defer.end[i];
      return;
    }
  }
  if (s_defer.n == DEFER_FLUSH_MAX) {
    /* (AI-assisted, micro-mg) MICRO_FLUSHGAP: into a range of the linear heap less than DEFER_GAP2
     * away, rather than a request of its own now */
    const u32 lin0 = __ctru_linear_heap, lin1 = __ctru_linear_heap + __ctru_linear_heap_size;
    if ((s_micro & MICRO_FLUSHGAP) && a >= lin0 && e <= lin1) {
      for (int i = 0; i < s_defer.n; i++) {
        if (s_defer.start[i] >= lin0 && s_defer.end[i] <= lin1 && a <= s_defer.end[i] + DEFER_GAP2 &&
            e + DEFER_GAP2 >= s_defer.start[i]) {
          s_defer.start[i] = a < s_defer.start[i] ? a : s_defer.start[i];
          s_defer.end[i] = e > s_defer.end[i] ? e : s_defer.end[i];
          return;
        }
      }
    }
    cache_flush_now(addr, size);
    return;
  }
  s_defer.start[s_defer.n] = a;
  s_defer.end[s_defer.n] = e;
  s_defer.n++;
}

static void flush_vbuf(void) {
  if (g.vbuf_used > g.vbuf_flushed) {
    cache_flush_frame(g.vbuf + g.vbuf_flushed, (u32)(g.vbuf_used - g.vbuf_flushed));
    g.vbuf_flushed = g.vbuf_used;
  }
  /* (AI-assisted, micro-gpu) ranges of the linear heap less than DEFER_GAP apart in one request:
   * each request is an IPC to the GSP (tens of microseconds on hardware), cleaning a few more
   * cache lines between them costs far less (the gap's lines: at most 256) */
  if (s_defer.n > 1 && (s_micro & MICRO_FLUSHMERGE)) {
    for (int i = 1; i < s_defer.n; i++) { /* by start (insertion sort, at most 48) */
      const u32 a = s_defer.start[i], e = s_defer.end[i];
      int j = i - 1;
      while (j >= 0 && s_defer.start[j] > a) {
        s_defer.start[j + 1] = s_defer.start[j];
        s_defer.end[j + 1] = s_defer.end[j];
        j--;
      }
      s_defer.start[j + 1] = a;
      s_defer.end[j + 1] = e;
    }
    const u32 lin0 = __ctru_linear_heap, lin1 = __ctru_linear_heap + __ctru_linear_heap_size;
    const u32 gap = (s_micro & MICRO_FLUSHGAP) ? DEFER_GAP2 : DEFER_GAP;
    int k = 0;
    for (int i = 1; i < s_defer.n; i++) {
      const u32 a = s_defer.start[i], e = s_defer.end[i];
      if (a <= s_defer.end[k] + gap && s_defer.start[k] >= lin0 && e <= lin1 && a >= lin0) {
        s_defer.end[k] = e > s_defer.end[k] ? e : s_defer.end[k];
      } else {
        k++;
        s_defer.start[k] = a;
        s_defer.end[k] = e;
      }
    }
    s_defer.n = k + 1;
  }
  for (int i = 0; i < s_defer.n; i++) {
    cache_flush_now((const void*)s_defer.start[i], s_defer.end[i] - s_defer.start[i]);
  }
  s_defer.n = 0;
}

/* (AI-assisted) C3D_SyncTextureCopy in a frame splits the command list with C3D_FrameSplit(0): that
 * part of the command list is NOT flushed from the CPU cache. C3D_FrameEnd(0) used to flush the
 * whole linear heap, which covered it; ctr_gpu_frame_end only flushes the last part
 * (GX_CMDLIST_FLUSH), so the GPU could run stale command words of the earlier part (wrong blend
 * and texture state, broken draws) in every frame with a copy (the distorter's screen copy, a
 * texture pool moving to VRAM). Split here with the flush first; C3D_SyncTextureCopy's own split
 * then has nothing left to send. */
static void sync_texture_copy(u32* in, u32 indim, u32* out, u32 outdim, u32 size, u32 flags) {
  if (g.in_frame && s_pipeline) {
    if (s_bd.frame) {
      bd_copy(in, indim, out, outdim, size, flags); /* (AI-assisted, gpu-rt) gpu_breakdown */
      return;
    }
    pipeline_submit(); /* the draws before the copy, then the copy, in the GPU's queue */
    GX_TextureCopy(in, indim, out, outdim, size, flags);
    return;
  }
  if (g.in_frame) {
    /* (AI-assisted) with overlap the queue may already be running (ctr_gpu_submit_partial): the
     * draws before the copy start right away, so their vertices must be out of the CPU cache */
    raw_vc_flush();
    flush_vbuf();
    C3D_FrameSplit(GX_CMDLIST_FLUSH);
  }
  C3D_SyncTextureCopy(in, indim, out, outdim, size, flags);
}

#define PARTIAL_MIN_DRAWS 24
#define PARTIAL_MAX 6 /* citro3d's queue has 32 entries per frame */

void ctr_gpu_free_pending_now(void) {
  if (!g.ready || g.in_frame) {
    return;
  }
  /* the GX queue holds the last frames' command lists and display transfers; with a timeout in
   * case it isn't running (then the deletes wait for the next frame begin, as before) */
  if (!gxCmdQueueWait(&__C3D_Context.gxQueue, 200000000LL)) {
    return;
  }
  process_pending_deletes(0);
}

void ctr_gpu_set_overlap(int on) {
  s_overlap = on;
}

/* (AI-assisted) The vertices are flushed and the command list split with its cache flush, then
 * the queue runs: the GPU draws this part while the CPU builds the rest (C3D_FrameBegin stops
 * the queue, citro3d only runs it at the frame end otherwise). Draw order is kept: the queue runs
 * its entries in order, and the frame end adds the rest and the display transfer after them. */
void ctr_gpu_submit_partial(void) {
  if (!s_overlap || !g.ready || !g.in_frame || s_partials >= PARTIAL_MAX ||
      g.cur.draws < s_partial_draws + PARTIAL_MIN_DRAWS) {
    return;
  }
  if (s_pipeline) {
    pipeline_submit();
  } else {
    raw_vc_flush();
    flush_vbuf();
    C3D_FrameSplit(GX_CMDLIST_FLUSH);
    gt_note_first(); /* (AI-assisted) the queue (stopped since C3D_FrameBegin) starts here */
    gt_note_submit();
    gxCmdQueueRun(&__C3D_Context.gxQueue);
  }
  s_partial_draws = g.cur.draws;
  s_partials++;
}

/* (AI-assisted) ctr_gpu_want_detailed_stats: the next frame end fills the detailed counts */
static int s_detail = 0;
void ctr_gpu_want_detailed_stats(void) {
  s_detail = 1;
}

/* (AI-assisted) The largest block linearAlloc can give now: the free total can be split in pieces
 * too small for a level's texture pool (chunk meshes take two allocations each). Allocation probes
 * (~10, binary search in 64 KB steps) under the lock, once per log line. safe_linear_alloc keeps
 * LINEAR_RESERVE free, so it reads at most the free total minus that. */
static unsigned int linear_largest_block(void) {
  ctr_linear_lock();
  u32 lo = 0, hi = (u32)linearSpaceFree();
  while (hi - lo > 64 * 1024) {
    const u32 mid = (lo + (hi - lo) / 2) & ~0xFFFu;
    void* p = linearAlloc(mid);
    if (p) {
      linearFree(p);
      lo = mid;
    } else {
      hi = mid;
    }
  }
  ctr_linear_unlock();
  return lo;
}

/* (AI-assisted) config.ini cmd_dump: a frame's whole GPU command list written to
 * sdmc:/3ds/jak1/data/log/cmd_NN.bin every CMD_DUMP_EVERY frames (at most CMD_DUMP_MAX files), for
 * platform/3ds/tools/cmd_report.py (command words per draw kind, register writes that change
 * nothing). Header: "CMDD", frame number, word count; then the words. A diagnostic: the write
 * stalls the render thread for a moment. */
#define CMD_DUMP_EVERY 600
#define CMD_DUMP_MAX 12
static int s_cmd_dump, s_cmd_dumps;
void ctr_gpu_set_cmd_dump(int on) {
  s_cmd_dump = on ? 1 : 0;
}
void ctr_gpu_cmd_dump_next(void) {
  if (s_micro_ident) {
    /* (AI-assisted, micro-gpu) gpu_micro_ident: the off / on pair (ctr_gpu_frame_begin) */
    if (!s_micro_ident_st) {
      s_micro_ident_st = 1;
    }
    return;
  }
  s_cmd_dump_next = 1;
}
static inline u32* raw_w(u32* p, u32 reg, u32 mask, u32 v);
static inline u32* raw_n(u32* p, u32 reg, const u32* v, u32 n, int seq);
static void cmd_dump_frame(void) {
  const int periodic = s_cmd_dump && g.frame_no % CMD_DUMP_EVERY == CMD_DUMP_EVERY / 2;
  if ((!periodic && !s_cmd_dump_next) || s_cmd_dumps >= CMD_DUMP_MAX * 2) {
    return;
  }
  s_cmd_dump_next = 0;
  const u32 words = (u32)(gpuCmdBuf + gpuCmdBufOffset - g.cmd_base);
  char path[64];
  snprintf(path, sizeof(path), "sdmc:/3ds/jak1/data/log/cmd_%02d.bin", s_cmd_dumps++);
  FILE* f = fopen(path, "wb");
  if (!f) {
    return;
  }
  if (s_res_on || s_proc_lut_map_f0 || s_proc_lut_color_f0) {
    /* (AI-assisted, micro-gpu) GPU memory the frame doesn't write but its draws use, from frames
     * before (shader_resident's code image, the procedural texture tables as they were at the
     * frame begin), as the commands that put it there: the file starts with them ("CMDE": a
     * fourth header word, the prologue's word count; cmd_report.py --kicks runs it before the
     * frame) */
    static u32 pro[8 + 512 + 128 + 16 + 3 * 260];
    u32* p = pro;
    if (s_res_on) {
      p = raw_w(p, GPUREG_VSH_CODETRANSFER_CONFIG, 0xF, 0);
      for (u32 i = 0; i < s_res_dvlp.codeSize; i += 128) {
        const u32 left = s_res_dvlp.codeSize - i;
        p = raw_n(p, GPUREG_VSH_CODETRANSFER_DATA, s_res_code + i, left < 128 ? left : 128, 0);
      }
      p = raw_w(p, GPUREG_VSH_OPDESCS_CONFIG, 0xF, 0);
      p = raw_n(p, GPUREG_VSH_OPDESCS_DATA, s_res_desc, s_res_dvlp.opdescSize, 0);
    }
    if (s_proc_lut_map_f0) {
      p = raw_w(p, GPUREG_PROCTEX_LUT, 0xF, (u32)GPU_LUT_RGBMAP << 8);
      p = raw_n(p, GPUREG_PROCTEX_LUT_DATA0, *s_proc_lut_map_f0, 128, 0);
    }
    if (s_proc_lut_color_f0) {
      p = raw_w(p, GPUREG_PROCTEX_LUT, 0xF, (u32)GPU_LUT_COLOR << 8);
      p = raw_n(p, GPUREG_PROCTEX_LUT_DATA0, s_proc_lut_color_f0->color, 256, 0);
      p = raw_w(p, GPUREG_PROCTEX_LUT, 0xF, (u32)GPU_LUT_COLORDIF << 8);
      p = raw_n(p, GPUREG_PROCTEX_LUT_DATA0, s_proc_lut_color_f0->diff, 256, 0);
    }
    const u32 hdr[4] = {0x45444d43u, (u32)g.frame_no, words, (u32)(p - pro)};
    fwrite(hdr, 4, 4, f);
    fwrite(pro, 4, (size_t)(p - pro), f);
  } else {
    const u32 hdr[3] = {0x44444d43u, (u32)g.frame_no, words};
    fwrite(hdr, 4, 3, f);
  }
  fwrite(g.cmd_base, 4, words, f);
  fclose(f);
  printf("[ctr] cmd_dump: frame %d, %lu words -> %s\n", g.frame_no, (unsigned long)words, path);
}

void ctr_gpu_frame_end(void) {
  if (!g.ready || !g.in_frame) {
    return;
  }
  ctr_gpu_rt_end(); /* (AI-assisted) impostors: a render to texture left open */
  ctr_gpu_run_sky_hook(); /* (AI-assisted) sky_last: still pending (nothing blended came) */
  diag_rt("frame_end: flush");
  raw_vc_flush();
  /* (AI-assisted, micro-gpu) steady pacing's fence words (the frame number the fence copy writes)
   * at the end of the frame's vertex buffer, flushed with it: one GSP request less per frame. The
   * buffer is written again two frames later, after this frame's fence landed (overlap2_wait_frame,
   * or C3D_FrameBegin's wait for an idle GPU) */
  u32* fence_id = NULL;
  if ((s_micro & MICRO_FENCE) && s_pres.on && g.vbuf_used + 16 <= VBUF_BYTES) {
    fence_id = (u32*)(g.vbuf + g.vbuf_used);
    fence_id[0] = fence_id[1] = fence_id[2] = fence_id[3] = (u32)g.frame_no;
    g.vbuf_used += 16;
  }
  flush_vbuf();
  const int bd = s_bd.frame;
  if (bd) {
    /* (AI-assisted, gpu-rt) gpu_breakdown: the rest of the frame's commands alone (the frame
     * end below then has no list left to hand over) */
    g.cur.cmd_bytes = (unsigned int)((gpuCmdBuf + gpuCmdBufOffset - g.cmd_base) * 4);
    bd_part();
  }
  /* the command words of the whole frame (splits move gpuCmdBuf on; C3D_FrameBegin restarts it) */
  if (!bd) {
    g.cur.cmd_bytes = (unsigned int)((gpuCmdBuf + gpuCmdBufOffset - g.cmd_base) * 4);
    cmd_dump_frame();
  }
  /* (AI-assisted) frame_overlap2: 0 the old frame end, 1 the list cleaned before the wait, 2 no
   * wait (the list queued behind the frame the GPU draws) */
  const int ov2 = s_pipeline ? (overlap2_on() ? 2 : (s_overlap2 ? 1 : 0)) : 0;
  if (ov2) {
    pipeline_close_list();
  }
  if (ov2 == 2) {
    overlap2_room(5); /* the list, the transfer(s) and the fence */
    if (s_fin_words) {
      gt_note_submit();
      GX_ProcessCommandList(s_fin_list, s_fin_words * 4, 0);
    }
    s_buf_frame[s_k] = (u32)g.frame_no;
    s_end_mode2 = 1;
  } else if (s_pipeline) {
    /* wait for the previous frame (and this one's parts sent so far), then submit the rest of
     * this frame from our buffer with citro3d (display transfer, buffer swap) */
    u32* buf = gpuCmdBuf;
    const u32 size = gpuCmdBufSize, offset = gpuCmdBufOffset;
    const u64 wait0 = svcGetSystemTick();
    C3D_FrameBegin(0);
    g.cur.gpu_wait_ms =
        (float)((double)(svcGetSystemTick() - wait0) / (SYSCLOCK_ARM11 / 1000.0));
    process_pending_deletes(1);
    GPUCMD_SetBuffer(buf, size, offset);
    C3D_FrameDrawOn(g.top);
    gt_note_submit();
    if (ov2 == 1 && s_fin_words) {
      GX_ProcessCommandList(s_fin_list, s_fin_words * 4, 0); /* cleaned before the wait */
    }
    s_buf_frame[s_k] = 0;
    s_end_mode2 = 0;
  }
  if (s_pres.on) {
    diag_rt("frame_end: present queue (wait for a free screen buffer)");
    pres_queue_frame(fence_id);
  }
  /* all our buffers are flushed when written: only flush the command list, not the whole
   * linear heap (C3D_FrameEnd's default) */
  diag_rt("frame_end: C3D_FrameEnd (submit)");
  gt_note_first(); /* (AI-assisted) without the pipeline the queue starts here (or at a partial) */
  gt_note_submit();
  if (bd && (ov2 != 2 || !s_pres.on)) {
    bd_transfer_mark(); /* (AI-assisted, gpu-rt) the queue (stopped) runs the transfer from here */
  }
  if (ov2 != 2) {
    C3D_FrameEnd(GX_CMDLIST_FLUSH);
  }
  if (bd) {
    /* (AI-assisted, gpu-rt) the transfer and the fence (steady pacing), block32's crop copy */
    bd_frame_end(s_pres.on ? (g_block32 ? 3 : 2) : 1);
  }
  diag_rt("frame_end: stats");
  g.in_frame = 0;
  if (g.screenshot_state == 1) {
    g.screenshot_state = 2;
  }
  gt_collect();
  g.cur.vbuf_used = (unsigned int)g.vbuf_used;
  g.cur.vbuf_size = VBUF_BYTES;
  /* (AI-assisted) The counts over all 2048 texture slots and the free memory (the allocators walk
   * their free lists) only when they are logged (ctr_gpu_want_detailed_stats), not every frame */
  if (s_detail) {
    s_detail = 0;
    g.cur.detailed = 1;
    int count = 0;
    unsigned bytes = 0;
    for (int i = 0; i < MAX_TEXTURES; i++) {
      const TexSlot* t = &g.textures[i];
      if (t->used) {
        count++;
        bytes += t->tex.size;
        if (t->compact) {
          g.cur.compact_textures++;
          g.cur.compact_bytes += t->tex.size;
          g.cur.compact_in_vram += t->arena_off >= 0;
          g.cur.radial_textures += t->radial >= 0;
        }
      }
    }
    g.cur.textures = count;
    g.cur.tex_bytes = bytes;
    g.cur.linear_free = (unsigned int)linearSpaceFree();
    g.cur.vram_free = (unsigned int)vramSpaceFree();
    g.cur.linear_largest = linear_largest_block();
  }
  /* times of the last frame the GPU finished (the one before this) */
  g.cur.gpu_ms = C3D_GetProcessingTime();
  g.cur.draw_ms = C3D_GetDrawingTime();
  g.cur.vram_textures = g.vram_textures;
  g.cur.vram_copy_failures = g.vram_copy_failures;
  for (int i = 0; i < MAX_POOLS; i++) {
    const TexPool* p = &g.pools[i];
    if (p->used == 1) {
      g.cur.pools++;
      g.cur.pool_bytes += p->bytes;
      if (p->vram) {
        g.cur.pools_in_vram++;
        g.cur.pool_vram_bytes += p->vram_bytes;
      }
    }
  }
  g.last = g.cur;
}

void ctr_gpu_wait_vblank(int min_vblanks) {
  /* Like a swap with vsync, at most 60 / min_vblanks frames per second (1: 60 fps, 2: 30 fps).
   * (AI-assisted) Frames are released on a schedule of one every min_vblanks vblanks, not
   * min_vblanks after the last one: with "2 since the last frame" every frame that ran a little
   * late (a render spike) cost a whole third vblank and nothing won it back, so a game that ran at
   * 40-45 fps uncapped averaged 26-28 fps with the 30 fps cap. Now the frame after a late one may
   * go one vblank early, and the average stays at the cap. After a long stall (more than one frame
   * behind) the schedule starts again from now instead of rushing frames out. */
  static u32 target;
  static int have_target;
  if (min_vblanks < 1) {
    min_vblanks = 1;
  }
  u32 now = vblank_count();
  /* (AI-assisted) with steady pacing a late frame moves the schedule instead of the next frame
   * going a vblank early: frames are shown for as long as the game time they cover, and a
   * 1 vblank frame after a 3 vblank one looked like a stutter */
  const s32 slip = s_pres.on ? 0 : min_vblanks;
  if (!have_target || (s32)(now - target) > slip) {
    target = now;
    have_target = 1;
  }
  while ((s32)(now - target) < 0) {
    gspWaitForVBlank();
    pres_check_alive();
    now = vblank_count();
  }
  target += (u32)min_vblanks;
}

/* ---------------- textures ---------------- */

/* Settings (render.ini, see CtrSettings). */
static int g_rgba4_as_rgba8 = 1;

void ctr_gpu_set_rgba4_as_rgba8(int on) {
  g_rgba4_as_rgba8 = on;
}

int ctr_gpu_rgba4_as_rgba8(void) {
  return g_rgba4_as_rgba8;
}

/* texture pools in VRAM (update_pool_residency) */
static int g_vram_textures = 0;

static int g_compact;

void ctr_gpu_set_vram_textures(int on) {
  g_vram_textures = on;
  /* (AI-assisted) the reserved VRAM for compact textures: taken before any level pool moves in */
  if (on && g_compact && g.ready && !s_arena.base) {
    ctr_linear_lock();
    s_arena.base = (uint8_t*)vramAlloc(ARENA_BYTES);
    ctr_linear_unlock();
    if (s_arena.base) {
      s_arena.free_off[0] = 0;
      s_arena.free_len[0] = ARENA_BYTES;
      s_arena.nfree = 1;
    }
  }
}

/* Textures made on the fly (ctr_gpu_tex_create) outside of a frame in VRAM: off. Their upload
 * check reads VRAM with the CPU, which isn't mapped for every kind of launch. */
static const int g_vram_dynamic = 0;

static int g_mip_mode = 1;

void ctr_gpu_set_mip_mode(int mode) {
  g_mip_mode = mode < 0 ? 0 : (mode > 2 ? 2 : mode);
}

int ctr_gpu_is_emulator(void) {
  /* Citra and Azahar answer svcGetSystemInfo(0x20000 "emulator information", 0 "emulator id")
   * with their id; the 3DS kernel (and Luma3DS) reject the type */
  static int cached = -1;
  if (cached < 0) {
    s64 out = 0;
    Result r = svcGetSystemInfo(&out, 0x20000, 0);
    cached = (R_SUCCEEDED(r) && out != 0) ? 1 : 0;
  }
  return cached;
}

static GPU_TEXCOLOR gpu_format(int format) {
  switch (format) {
    case CTR_TEX_RGB565:
      return GPU_RGB565;
    case CTR_TEX_RGBA4:
      return GPU_RGBA4;
    case CTR_TEX_ETC1:
      return GPU_ETC1;
    case CTR_TEX_ETC1A4:
      return GPU_ETC1A4;
    default:
      return GPU_RGBA8;
  }
}

static unsigned int format_bits(int format) {
  switch (format) {
    case CTR_TEX_RGB565:
    case CTR_TEX_RGBA4:
      return 16;
    case CTR_TEX_ETC1:
      return 4;
    case CTR_TEX_ETC1A4:
      return 8;
    default:
      return 32;
  }
}

unsigned int ctr_gpu_tex_bytes(int w, int h, int format, int levels) {
  unsigned int total = 0;
  for (int l = 0; l < levels; l++) {
    total += (unsigned int)((w >> l) * (h >> l)) * format_bits(format) / 8;
  }
  return total;
}

/* A C3D_Tex over memory that isn't its own (pools): like C3D_TexInitWithParams. */
static void tex_init_external(C3D_Tex* tex, void* data, int w, int h, int format, int levels) {
  memset(tex, 0, sizeof(*tex));
  const GPU_TEXCOLOR fmt = gpu_format(format);
  tex->data = data;
  tex->width = (u16)w;
  tex->height = (u16)h;
  tex->param = GPU_TEXTURE_MODE(GPU_TEX_2D);
  if (fmt == GPU_ETC1) {
    tex->param |= GPU_TEXTURE_ETC1_PARAM;
  }
  tex->fmt = fmt;
  tex->size = (u32)w * (u32)h * format_bits(format) / 8;
  tex->border = 0;
  tex->lodBias = 0;
  tex->maxLevel = (u8)(levels > 0 ? levels - 1 : 0);
  tex->minLevel = 0;
}

/* ---------------- texture pools ---------------- */

int ctr_gpu_pool_create(unsigned int bytes) {
  if (!g.ready || bytes == 0) {
    return -1;
  }
  int slot = -1;
  ctr_linear_lock();
  for (int i = 0; i < MAX_POOLS; i++) {
    if (g.pools[i].used == 0) {
      slot = i;
      break;
    }
  }
  if (slot >= 0) {
    uint8_t* mem = (uint8_t*)linearAlloc(bytes);
    if (mem) {
      TexPool* p = &g.pools[slot];
      memset(p, 0, sizeof(*p));
      p->used = 3;
      p->linear = mem;
      p->bytes = bytes;
      p->no_room_frame = -1000;
    } else {
      slot = -1;
    }
  }
  ctr_linear_unlock();
  return slot;
}

void* ctr_gpu_pool_data(int pool) {
  if (pool < 0 || pool >= MAX_POOLS || !g.pools[pool].used) {
    return NULL;
  }
  return g.pools[pool].linear;
}

int ctr_gpu_pool_tex(int pool, unsigned int offset, int w, int h, int format, int levels) {
  if (pool < 0 || pool >= MAX_POOLS || !g.pools[pool].used || w < 8 || h < 8 || w > 1024 ||
      h > 1024 || levels < 1 || (offset & 0x7f) ||
      offset + ctr_gpu_tex_bytes(w, h, format, levels) > g.pools[pool].bytes) {
    return -1;
  }
  const int slot = reserve_tex_slot();
  if (slot < 0) {
    return -1;
  }
  TexSlot* t = &g.textures[slot];
  TexPool* p = &g.pools[pool];
  t->bytes = ctr_gpu_tex_bytes(w, h, format, levels);
  /* reads the pool's current place: set before the pool is ready (it stays in linear memory
   * until then) */
  tex_init_external(&t->tex, (p->vram ? (uint8_t*)p->vram : p->linear) + offset, w, h, format,
                    levels);
  t->pool = pool;
  t->offset = offset;
  t->levels = (uint8_t)levels;
  ctr_linear_lock();
  t->used = 1;
  ctr_linear_unlock();
  return slot;
}

void ctr_gpu_pool_ready(int pool) {
  if (pool < 0 || pool >= MAX_POOLS || !g.pools[pool].used) {
    return;
  }
  TexPool* p = &g.pools[pool];
  s_tex_dirty = 1; /* (AI-assisted) tex_cache_clear = uploads (loader thread: a flag) */
  cache_flush_now(p->linear, p->bytes);
  ctr_linear_lock();
  p->ready = 1;
  if (p->used == 3) {
    p->used = 1;
  }
  ctr_linear_unlock();
}

void ctr_gpu_pool_delete(int pool) {
  if (pool < 0 || pool >= MAX_POOLS) {
    return;
  }
  ctr_linear_lock();
  if (g.pools[pool].used == 1 || g.pools[pool].used == 3) {
    g.pools[pool].used = 2; /* freed at the next frame begin, after the GPU is done with it */
  }
  ctr_linear_unlock();
}

void ctr_gpu_pool_set_priority(int pool, int priority) {
  if (pool >= 0 && pool < MAX_POOLS && g.pools[pool].used) {
    g.pools[pool].priority = priority;
  }
}

/* point the pool's textures at its VRAM copy (the ones in [0, vram_bytes)) or linear memory */
static void pool_repoint(int pool) {
  TexPool* p = &g.pools[pool];
  for (int i = 0; i < MAX_TEXTURES; i++) {
    TexSlot* t = &g.textures[i];
    if (t->used && t->pool == pool) {
      if (p->vram && t->offset + t->bytes <= p->vram_bytes) {
        t->tex.data = (uint8_t*)p->vram + t->offset;
      } else {
        t->tex.data = p->linear + (t->offset - p->linear_off);
      }
    }
  }
  g.bound_tex = NULL;
  s_tex_dirty = 1; /* (AI-assisted) tex_cache_clear = uploads */
}

/* a linear buffer the GPU may still read (a queued copy, the frame in flight): freed at the next
 * process_pending_deletes, after the GPU is done */
static void defer_linear_free(void* mem) {
  if (!mem) {
    return;
  }
  if (g.pending_staging_count < MAX_STAGING) {
    g.pending_staging[g.pending_staging_count++] = mem;
  } else {
    /* (AI-assisted) no room in the list: freed once a running GX queue (the pipeline's) has run
     * what is queued (a stopped queue returns at once, as before) */
    gxCmdQueueWait(&__C3D_Context.gxQueue, -1);
    linearFree(mem);
  }
}

/* (AI-assisted) The end of the pool after `part` bytes, without a straddling texture: textures in
 * [0, result) are whole, the others start at result or later. */
static unsigned int pool_split(int pool, unsigned int part) {
  for (int changed = 1; changed;) {
    changed = 0;
    for (int i = 0; i < MAX_TEXTURES; i++) {
      const TexSlot* t = &g.textures[i];
      if (t->used && t->pool == pool && t->offset < part && t->offset + t->bytes > part) {
        part = t->offset;
        changed = 1;
      }
    }
  }
  return part;
}

/* Returns 0 when it can't (no linear memory to copy the VRAM part back to): it stays then. */
static int pool_leave_vram(int pool) {
  TexPool* p = &g.pools[pool];
  if (!p->vram) {
    return 1;
  }
  if (p->linear_off) {
    /* the VRAM part isn't in linear memory: copy the whole pool back (the CPU reads VRAM) */
    uint8_t* full = (uint8_t*)linearAlloc(p->bytes);
    if (!full) {
      return 0;
    }
    memcpy(full, p->vram, p->linear_off);
    if (p->linear && p->bytes > p->linear_off) {
      memcpy(full + p->linear_off, p->linear, p->bytes - p->linear_off);
    }
    cache_flush_now(full, p->bytes);
    defer_linear_free(p->linear);
    p->linear = full;
    p->linear_off = 0;
  }
  void* v = p->vram;
  p->vram = NULL;
  p->vram_bytes = 0;
  pool_repoint(pool);
  /* at frame begin: the GPU finished the frames that read it, and this frame reads the linear
   * copy from now on */
  vramFree(v);
  return 1;
}

/* At frame begin, before any draw: copy the pools that want to be in VRAM there, most wanted
 * first, moving pools with a lower priority out if that makes room. The copies are queued ahead of
 * this frame's draws, which read the VRAM copy. */
static void update_pool_residency(void) {
  if (!g_vram_textures) {
    for (int i = 0; i < MAX_POOLS; i++) {
      if (g.pools[i].used == 1 && g.pools[i].vram) {
        (void)pool_leave_vram(i);
      }
    }
    return;
  }
  /* (AI-assisted) the largest block VRAM can give at all: two separate 3 MB banks, each with a
   * render target buffer. Measured once (only the screens' buffers are there yet). A pool bigger
   * than this never fits whole: evicting the others for it read them back from VRAM with the CPU
   * (slow) and gained nothing, at every level arrival. */
  static unsigned int s_vram_max_block = 0;
  if (!s_vram_max_block) {
    ctr_linear_lock();
    for (unsigned int sz = 6u << 20; sz >= (256u << 10); sz -= 64u << 10) {
      void* probe = vramAlloc(sz);
      if (probe) {
        vramFree(probe);
        s_vram_max_block = sz;
        break;
      }
    }
    ctr_linear_unlock();
    if (!s_vram_max_block) {
      s_vram_max_block = 1;
    }
  }
  int copies = 0;
  int tried[MAX_POOLS] = {0};
  int stuck[MAX_POOLS] = {0}; /* can't leave VRAM (no linear memory to go back to) */
  while (copies < POOL_COPIES_PER_FRAME) {
    int best = -1;
    ctr_linear_lock();
    for (int i = 0; i < MAX_POOLS; i++) {
      const TexPool* p = &g.pools[i];
      if (p->used != 1 || !p->ready || p->vram || tried[i] ||
          g.frame_no - p->no_room_frame < 60) {
        continue;
      }
      if (best < 0 || p->priority > g.pools[best].priority) {
        best = i;
      }
    }
    ctr_linear_unlock();
    if (best < 0) {
      break;
    }
    tried[best] = 1;
    TexPool* p = &g.pools[best];
    ctr_linear_lock();
    void* v = vramAlloc(p->bytes);
    while (!v && p->bytes <= s_vram_max_block) {
      /* move out the resident pool with the lowest priority below this one's */
      int victim = -1;
      for (int i = 0; i < MAX_POOLS; i++) {
        const TexPool* q = &g.pools[i];
        if (q->used == 1 && q->vram && !stuck[i] && q->priority < p->priority &&
            (victim < 0 || q->priority < g.pools[victim].priority)) {
          victim = i;
        }
      }
      if (victim < 0) {
        break;
      }
      if (!pool_leave_vram(victim)) {
        stuck[victim] = 1;
        continue;
      }
      v = vramAlloc(p->bytes);
    }
    unsigned int part = p->bytes;
    if (!v) {
      /* (AI-assisted) not all of it: as much as fits in the VRAM that's left (whole textures from
       * the start), so the GPU reads those fast and they leave linear memory */
      const unsigned int kStep = 128 * 1024, kMinPart = 256 * 1024;
      unsigned int want = (unsigned int)vramSpaceFree();
      want = want > VRAM_RESERVE ? want - VRAM_RESERVE : 0;
      if (want > p->bytes) {
        want = p->bytes;
      }
      want &= ~(kStep - 1);
      while (want >= kMinPart && !(v = vramAlloc(want))) {
        want -= kStep;
      }
      if (v) {
        part = pool_split(best, want);
        if (part < kMinPart) {
          vramFree(v);
          v = NULL;
        }
      }
    }
    ctr_linear_unlock();
    if (!v) {
      p->no_room_frame = g.frame_no;
      continue;
    }
    /* one GPU copy of the part (in a frame: queued, runs before the draws) */
    uint8_t* old = p->linear;
    sync_texture_copy((u32*)old, 0, (u32*)v, 0, part, 8);
    p->vram = v;
    p->vram_bytes = part;
    /* the VRAM part leaves linear memory: the rest (if any) gets a buffer of its own; the old one
     * is freed once the copy has run */
    ctr_linear_lock();
    uint8_t* rest = NULL;
    if (part < p->bytes) {
      rest = (uint8_t*)linearAlloc(p->bytes - part);
    }
    if (part == p->bytes || rest) {
      if (rest) {
        memcpy(rest, old + part, p->bytes - part);
        cache_flush_now(rest, p->bytes - part);
      }
      p->linear = rest;
      p->linear_off = part;
      defer_linear_free(old);
    }
    ctr_linear_unlock();
    pool_repoint(best);
    copies++;
  }
}

/* allow_vram: only from the render thread (the copy to VRAM goes through the GX queue) */
static void* tex_alloc(C3D_Tex* tex, int w, int h, GPU_TEXCOLOR fmt, int* on_vram, int allow_vram) {
  *on_vram = 0;
  void* result = NULL;
  ctr_linear_lock();
  /* only outside of a frame: a copy in a frame needs a command list split and a queue entry
   * each, and a level's worth of them overflows the GX queue */
  if (allow_vram && g_vram_dynamic && !g.in_frame &&
      C3D_TexInitVRAM(tex, (u16)w, (u16)h, fmt)) {
    void* staging = linearAlloc(tex->size);
    if (staging) {
      *on_vram = 1;
      result = staging;
    } else {
      C3D_TexDelete(tex);
    }
  }
  if (!result && linear_room_for_tex(w, h, 0) && C3D_TexInit(tex, (u16)w, (u16)h, fmt)) {
    result = tex->data;
  }
  ctr_linear_unlock();
  return result;
}

static int tex_commit(C3D_Tex* tex, void* buf, int on_vram) {
  if (!on_vram) {
    tex_flush_now(tex);
    return 1;
  }
  const char* const prev_stage = s_diag_stage;
  diag_rt("texture upload: copy to VRAM");
  cache_flush_now(buf, tex->size);
  sync_texture_copy((u32*)buf, 0, (u32*)tex->data, 0, tex->size, 8);
  diag_rt(prev_stage);
  /* check that the copy landed (VRAM is readable by the CPU); if not, use linear memory */
  int ok = memcmp(tex->data, buf, tex->size) == 0;
  if (!ok) {
    g.vram_copy_failures++;
    GPU_TEXCOLOR fmt = tex->fmt;
    u16 w = tex->width, h = tex->height;
    ctr_linear_lock();
    C3D_TexDelete(tex);
    if (!linear_room_for_tex(w, h, 0) || !C3D_TexInit(tex, w, h, fmt)) {
      linearFree(buf);
      ctr_linear_unlock();
      return 0;
    }
    ctr_linear_unlock();
    memcpy(tex->data, buf, tex->size);
    tex_flush_now(tex);
  } else {
    g.vram_textures++;
  }
  ctr_linear_lock();
  linearFree(buf);
  ctr_linear_unlock();
  return 1;
}

/* linear RGBA8 (top row first) -> the GPU's tiled RGBA8 (A, B, G, R; bottom row first): 8x8 tiles
 * in rows, morton order inside a tile (CtrTexConv.h) */
/* (AI-assisted) config.ini asm_tex = off (hardware hang bisect): the Oct 2 per texel loops, byte
 * loads and stores only (no LDM/STM tiling) */
static int s_asm_tex = 1;
void ctr_gpu_set_asm_tex(int on) {
  s_asm_tex = on;
}
int ctr_gpu_asm_tex(void) {
  return s_asm_tex;
}
static inline u32 morton8(u32 x, u32 y) {
  return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) |
         ((y & 4) << 3);
}
/* the texel at image (x, y) (y = 0 at the top) in the tiled layout */
static inline u32 tiled_texel(int x, int y, int w, int h) {
  const int ty = h - 1 - y;
  return (u32)((ty / 8) * (w / 8) + (x / 8)) * 64 + morton8((u32)x & 7, (u32)ty & 7);
}

__attribute__((noinline)) static void swizzle_rgba8(uint8_t* dst, int w, int h,
                                                    const uint8_t* rgba) {
  if (!s_asm_tex) {
    const int tiles_x = w / 8;
    for (int y = 0; y < h; y++) {
      const int ty = h - 1 - y;
      const uint8_t* p = rgba + 4 * y * w;
      for (int x = 0; x < w; x++, p += 4) {
        u32 tile = (u32)((ty / 8) * tiles_x + (x / 8));
        u32 off = (tile * 64 + morton8(x & 7, ty & 7)) * 4;
        dst[off + 0] = p[3];
        dst[off + 1] = p[2];
        dst[off + 2] = p[1];
        dst[off + 3] = p[0];
      }
    }
    return;
  }
  ctr_tile_texels(dst, w, h, rgba, CTR_CONV_RGBA8);
}

int ctr_gpu_tex_create(int w, int h, const uint8_t* rgba) {
  if (!g.ready || w < 8 || h < 8 || w > 1024 || h > 1024) {
    return -1;
  }
  const int slot = reserve_tex_slot();
  if (slot < 0) {
    return -1;
  }
  C3D_Tex* tex = &g.textures[slot].tex;
  int on_vram;
  uint8_t* dst = (uint8_t*)tex_alloc(tex, w, h, GPU_RGBA8, &on_vram, 1);
  if (!dst) {
    release_tex_slot(slot);
    return -1;
  }
  swizzle_rgba8(dst, w, h, rgba);
  if (!tex_commit(tex, dst, on_vram)) {
    release_tex_slot(slot);
    return -1;
  }
  g.textures[slot].pool = -1;
  g.textures[slot].levels = 1;
  g.textures[slot].used = 1;
  return slot;
}

int ctr_gpu_tex_create_mipmapped(int w, int h, const uint8_t* rgba) {
  if (!g.ready || w < 8 || h < 8 || w > 1024 || h > 1024) {
    return -1;
  }
  const int slot = reserve_tex_slot();
  if (slot < 0) {
    return -1;
  }
  C3D_Tex* tex = &g.textures[slot].tex;
  ctr_linear_lock();
  const bool ok = linear_room_for_tex(w, h, 1) && C3D_TexInitMipmap(tex, (u16)w, (u16)h, GPU_RGBA8);
  ctr_linear_unlock();
  if (!ok) {
    release_tex_slot(slot);
    return -1;
  }
  swizzle_rgba8((uint8_t*)tex->data, w, h, rgba);
  C3D_TexGenerateMipmap(tex, GPU_TEXFACE_2D); /* 2x2 averages, on the tiled data */
  tex_flush_now(tex);
  g.textures[slot].pool = -1;
  g.textures[slot].levels = (uint8_t)(tex->maxLevel + 1);
  g.textures[slot].used = 1;
  return slot;
}

/* ---------------- (AI-assisted) 3DS hardware features: settings ---------------- */

static int g_early_depth = 0; /* config.ini early_depth (off by default) */
/* (AI-assisted) cull_ties: which PICA mode culls the c3l back faces (see ctr_gpu_set_cull_flip) */
static int g_cull_flip = 0;
static int g_cull_enable = 0;
/* (AI-assisted) config.ini fog_alpha, tex1_gating (set each frame: gpu_profile = ab switches them) */
static int g_fog_alpha = 0;
static int g_tex1_gating = 0;

void ctr_gpu_set_cull(int enable, int flip) {
  g_cull_enable = enable ? 1 : 0;
  g_cull_flip = flip ? 1 : 0;
}

void ctr_gpu_cull_reset(void) {
  C3Di_Context* ctx = &__C3D_Context;
  if (ctx->effect.cullMode != GPU_CULL_NONE) {
    C3D_CullFace(GPU_CULL_NONE); /* sent with citro3d's next update (or the next raw state) */
    g.last_state_valid = 0;
  }
}
static int g_proctex = 1;
static int g_compact = 1;

void ctr_gpu_set_color16(int on) {
  if (!g.ready) {
    g_color16 = on ? 1 : 0; /* the render target is made in ctr_gpu_init */
  }
}

void ctr_gpu_set_early_depth(int on) {
  /* (AI-assisted) not without the block32 target it was asked for (ctr_gpu_set_framebuffer) */
  g_early_depth = on && !g_block32_failed ? 1 : 0;
}

void ctr_gpu_set_early_depth_params(int sky, int func, unsigned int clear) {
  const int s = sky && !g_block32_failed ? 1 : 0;
  const int f = func & 3;
  const u32 c = clear & 0xFFFFFFu;
  if (s != g_early_sky || f != g_early_func || c != g_early_clear) {
    g_early_sky = s;
    g_early_func = f;
    g_early_clear = c;
    g.last_state_valid = 0;
  }
}

void ctr_gpu_set_framebuffer(int block32, int depth16) {
  if (!g.ready) {
    g_block32 = block32 ? 1 : 0; /* the render target is made in ctr_gpu_init */
    g_depth16 = depth16 ? 1 : 0;
  }
}

void ctr_gpu_set_proctex(int on) {
  g_proctex = on ? 1 : 0;
}

void ctr_gpu_set_compact_textures(int on) {
  g_compact = on ? 1 : 0;
}

/* ---------------- (AI-assisted) compact textures ---------------- */

/* Is a gray texture a radial gradient around its center (glows, light halos)? Its texels must be
 * within a small error of the average at their distance from the center (distances past the edge
 * count as the edge, as the procedural texture unit clamps them). On success fills lut with the
 * averages from the center (0) to the edge (RADIAL_LUT - 1): R in the low byte, alpha doubled
 * like the compact formats. */
static int check_radial(int w, int h, const uint8_t* rgba, u32* lut) {
  enum { BINS = 48 };
  if (w != h || w < 16) {
    return 0;
  }
  float sl[BINS], sa[BINS];
  int cnt[BINS];
  memset(sl, 0, sizeof(sl));
  memset(sa, 0, sizeof(sa));
  memset(cnt, 0, sizeof(cnt));
  const float c = (float)w * 0.5f, inv_c = 1.0f / c;
  float amin = 255.f, amax = 0.f;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      const uint8_t* p = rgba + 4 * (y * w + x);
      const float dx = (float)x + 0.5f - c, dy = (float)y + 0.5f - c;
      float r = sqrtf(dx * dx + dy * dy) * inv_c;
      if (r > 1.0f) {
        r = 1.0f;
      }
      const int b = (int)(r * (BINS - 1) + 0.5f);
      sl[b] += p[1];
      sa[b] += p[3];
      cnt[b]++;
      amin = p[3] < amin ? p[3] : amin;
      amax = p[3] > amax ? p[3] : amax;
    }
  }
  if (amax - amin < 16.f) {
    return 0; /* no alpha gradient: not a glow */
  }
  /* averages, empty bins (small textures) from the nearest filled one before them */
  int last = -1;
  for (int b = 0; b < BINS; b++) {
    if (cnt[b]) {
      sl[b] /= (float)cnt[b];
      sa[b] /= (float)cnt[b];
      last = b;
    } else if (last >= 0) {
      sl[b] = sl[last];
      sa[b] = sa[last];
    }
  }
  if (!cnt[0]) {
    int f = 0;
    while (f < BINS && !cnt[f]) {
      f++;
    }
    if (f == BINS) {
      return 0;
    }
    for (int b = 0; b < f; b++) {
      sl[b] = sl[f];
      sa[b] = sa[f];
    }
  }
  /* how many texels are off their ring's average */
  int bad = 0;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      const uint8_t* p = rgba + 4 * (y * w + x);
      const float dx = (float)x + 0.5f - c, dy = (float)y + 0.5f - c;
      float r = sqrtf(dx * dx + dy * dy) * inv_c;
      if (r > 1.0f) {
        r = 1.0f;
      }
      const float fb = r * (BINS - 1);
      const int b0 = (int)fb, b1 = b0 + 1 < BINS ? b0 + 1 : b0;
      const float t = fb - (float)b0;
      const float el = sl[b0] + (sl[b1] - sl[b0]) * t, ea = sa[b0] + (sa[b1] - sa[b0]) * t;
      if (fabsf((float)p[1] - el) + 2.f * fabsf((float)p[3] - ea) > 28.f) {
        bad++;
      }
    }
  }
  if (bad * 40 > w * h) {
    return 0; /* more than 2.5% of the texels: a pattern, not a gradient */
  }
  for (int i = 0; i < RADIAL_LUT; i++) {
    const float fb = (float)i / (float)(RADIAL_LUT - 1) * (BINS - 1);
    const int b0 = (int)fb, b1 = b0 + 1 < BINS ? b0 + 1 : b0;
    const float t = fb - (float)b0;
    float l = sl[b0] + (sl[b1] - sl[b0]) * t, a = 2.f * (sa[b0] + (sa[b1] - sa[b0]) * t);
    const u32 li = l > 255.f ? 255u : (u32)(l + 0.5f), ai = a > 255.f ? 255u : (u32)(a + 0.5f);
    lut[i] = (ai << 24) | (li << 16) | (li << 8) | li;
  }
  return 1;
}

int ctr_gpu_tex_create_compact(int w, int h, const uint8_t* rgba, int flags) {
  if (!g_compact) {
    return ctr_gpu_tex_create(w, h, rgba);
  }
  if (!g.ready || w < 8 || h < 8 || w > 1024 || h > 1024) {
    return -1;
  }
  const int n = w * h;
  int gray = 1, opaque = 1, binary = 1;
  unsigned amax = 0;
  for (int i = 0; i < n; i++) {
    const uint8_t* p = rgba + 4 * i;
    const int dr = (int)p[0] - (int)p[1], db = (int)p[2] - (int)p[1];
    if (dr > 3 || dr < -3 || db > 3 || db < -3) {
      gray = 0;
    }
    if (p[3] != 0x80) {
      opaque = 0;
      if (p[3] != 0) {
        binary = 0;
      }
    }
    amax = p[3] > amax ? p[3] : amax;
  }
  /* GS alpha above 1.0 (0x80) doesn't survive doubling: keep the texture as it is */
  if (amax > 0x80) {
    return ctr_gpu_tex_create(w, h, rgba);
  }
  GPU_TEXCOLOR fmt;
  if (gray) {
    fmt = opaque ? GPU_L8 : GPU_LA8;
  } else if (opaque) {
    fmt = GPU_RGB565;
  } else if (binary) {
    fmt = GPU_RGBA5551;
  } else {
    /* (AI-assisted) colored with soft alpha (glows, flares, the HUD orb's halo): kept at 8 bits per
     * channel, as on PC. RGBA4 (16 levels, truncated) turned their smooth falloff into rings and
     * dimmed them, on hardware only (Azahar draws RGBA4 as noise, so it already got RGBA8). */
    return ctr_gpu_tex_create(w, h, rgba);
  }
  const int slot = reserve_tex_slot();
  if (slot < 0) {
    return -1;
  }
  TexSlot* t = &g.textures[slot];
  C3D_Tex* tex = &t->tex;
  ctr_linear_lock();
  const bool ok = linear_room_for_tex(w, h, 0) && C3D_TexInit(tex, (u16)w, (u16)h, fmt);
  ctr_linear_unlock();
  if (!ok) {
    release_tex_slot(slot);
    return -1;
  }
  /* (AI-assisted) 4x2 texel groups straight into the tiled layout (CtrTexConv.h) */
  int conv;
  switch (fmt) {
    case GPU_L8:
      conv = CTR_CONV_L8;
      break;
    case GPU_LA8:
      conv = CTR_CONV_LA8;
      break;
    case GPU_RGB565:
      conv = CTR_CONV_RGB565;
      break;
    case GPU_RGBA5551:
      conv = CTR_CONV_RGBA5551;
      break;
    default:
      conv = CTR_CONV_RGBA4;
      break;
  }
  if (s_asm_tex) {
    ctr_tile_texels(tex->data, w, h, rgba, conv);
  } else {
    /* (AI-assisted) asm_tex = off: the Oct 2 per texel loop */
    uint8_t* d8 = (uint8_t*)tex->data;
    uint16_t* d16 = (uint16_t*)tex->data;
    for (int y = 0; y < h; y++) {
      const uint8_t* p = rgba + 4 * y * w;
      for (int x = 0; x < w; x++, p += 4) {
        const u32 o = tiled_texel(x, y, w, h);
        const u32 a2 = p[3] >= 0x80 ? 255u : (u32)p[3] * 2u;
        switch (fmt) {
          case GPU_L8:
            d8[o] = p[1];
            break;
          case GPU_LA8:
            d8[2 * o] = (uint8_t)a2;
            d8[2 * o + 1] = p[1];
            break;
          case GPU_RGB565:
            d16[o] = (uint16_t)(((p[0] >> 3) << 11) | ((p[1] >> 2) << 5) | (p[2] >> 3));
            break;
          case GPU_RGBA5551:
            d16[o] = (uint16_t)(((p[0] >> 3) << 11) | ((p[1] >> 3) << 6) | ((p[2] >> 3) << 1) |
                                (p[3] ? 1 : 0));
            break;
          default: /* GPU_RGBA4 */
            d16[o] = (uint16_t)(((p[0] >> 4) << 12) | ((p[1] >> 4) << 8) |
                                ((p[2] >> 4) << 4) | (a2 >> 4));
            break;
        }
      }
    }
  }
  tex_flush_now(tex);
  t->pool = -1;
  t->levels = 1;
  t->compact = 1;
  t->alpha_full = 1;
  t->radial = -1;
  t->arena_off = -1;
  t->want_vram = s_arena.base != NULL;
  s_small_waiting |= t->want_vram;
  if ((flags & CTR_TEX_CHECK_RADIAL) && gray && !opaque) {
    u32 lut[RADIAL_LUT];
    int k = 0;
    while (k < MAX_RADIAL && s_radial[k]) {
      k++;
    }
    if (k < MAX_RADIAL && check_radial(w, h, rgba, lut)) {
      s_radial[k] = (C3D_ProcTexColorLut*)malloc(sizeof(C3D_ProcTexColorLut));
      if (s_radial[k]) {
        memset(s_radial[k], 0, sizeof(C3D_ProcTexColorLut));
        ProcTexColorLut_Write(s_radial[k], lut, 0, RADIAL_LUT);
        t->radial = (int16_t)k;
      }
    }
  }
  ctr_linear_lock();
  t->used = 1;
  ctr_linear_unlock();
  return slot;
}

int ctr_gpu_tex_radial(int handle) {
  return g_proctex && handle >= 0 && handle < MAX_TEXTURES && g.textures[handle].used == 1 &&
         g.textures[handle].radial >= 0 && s_radial[g.textures[handle].radial] != NULL;
}

/* (AI-assisted) At frame begin, in the frame (the copy is queued before its draws): compact
 * textures made in linear memory move to the reserved VRAM, a batch of them in one GPU copy
 * (each copy is a GX queue entry). The linear copy is freed once the GPU is done with the frames
 * that read it. */
#define SMALL_BATCH_BYTES (64 * 1024)
#define SMALL_BATCH_MAX 32
static void migrate_small_textures(void) {
  /* (AI-assisted) s_small_waiting: a compact texture may wait (set when one is made); the scan of
   * all 2048 slots ran every frame */
  if (!s_arena.base || !s_small_waiting) {
    return;
  }
  static int scan = 0;
  int pick[SMALL_BATCH_MAX];
  u32 sizes[SMALL_BATCH_MAX];
  int npick = 0;
  u32 total = 0;
  int k = 0;
  for (; k < MAX_TEXTURES && npick < SMALL_BATCH_MAX; k++) {
    const int i = (scan + k) % MAX_TEXTURES;
    TexSlot* t = &g.textures[i];
    if (t->used != 1 || !t->want_vram) {
      continue;
    }
    const u32 bytes = arena_round(C3D_TexCalcTotalSize(t->tex.size, t->tex.maxLevel));
    if (bytes > SMALL_BATCH_BYTES) {
      t->want_vram = 0; /* too big for the reserved VRAM */
      continue;
    }
    if (total + bytes > SMALL_BATCH_BYTES) {
      break;
    }
    pick[npick] = i;
    sizes[npick] = bytes;
    npick++;
    total += bytes;
  }
  scan = (scan + k) % MAX_TEXTURES;
  if (!npick) {
    s_small_waiting = 0; /* all slots looked at: none waits */
    return;
  }
  const int off = arena_alloc(total);
  if (off < 0) {
    for (int j = 0; j < npick; j++) {
      g.textures[pick[j]].want_vram = 0; /* full: they stay in linear memory */
    }
    return;
  }
  ctr_linear_lock();
  uint8_t* staging = (uint8_t*)linearAlloc(total);
  ctr_linear_unlock();
  if (!staging) {
    arena_free((u32)off, total);
    return;
  }
  u32 pos = 0;
  for (int j = 0; j < npick; j++) {
    TexSlot* t = &g.textures[pick[j]];
    memcpy(staging + pos, t->tex.data, C3D_TexCalcTotalSize(t->tex.size, t->tex.maxLevel));
    pos += sizes[j];
  }
  cache_flush_now(staging, total);
  sync_texture_copy((u32*)staging, 0, (u32*)(s_arena.base + off), 0, total, 8);
  pos = 0;
  for (int j = 0; j < npick; j++) {
    TexSlot* t = &g.textures[pick[j]];
    defer_linear_free(t->tex.data);
    t->tex.data = s_arena.base + off + pos;
    t->arena_off = off + (int)pos;
    t->want_vram = 0;
    pos += sizes[j];
  }
  defer_linear_free(staging);
  g.bound_tex = NULL;
}

/* (AI-assisted) C3D_TexGenerateMipmap for an RGBA8 texture in linear memory, the same tiled walk
 * (levels down to 8 x 8), each texel the average of its 2 x 2 by ARMv6 byte-wise halving adds
 * (UHADD8: three per texel instead of citro3d's loop per channel; rounded down twice, so at most
 * 1 below citro3d's value) */
static inline u32 avg4_rgba8(u32 a, u32 b, u32 c, u32 d) {
  u32 ab, cd, r;
  __asm__("uhadd8 %0, %1, %2" : "=r"(ab) : "r"(a), "r"(b));
  __asm__("uhadd8 %0, %1, %2" : "=r"(cd) : "r"(c), "r"(d));
  __asm__("uhadd8 %0, %1, %2" : "=r"(r) : "r"(ab), "r"(cd));
  return r;
}

static void tex_mipmaps_rgba8(C3D_Tex* tex) {
  u8* src = (u8*)tex->data;
  u32 level_size = tex->size;
  u32 w = tex->width, h = tex->height;
  for (int level = 0; level < tex->maxLevel; level++) {
    u8* dst = src + level_size;
    const u32 dw = w / 2, dh = h / 2;
    const u32 src_stride = w / 8, dst_stride = dw / 8;
    for (u32 j = 0; j < dh / 8; j++) {
      for (u32 i = 0; i < dst_stride; i++) {
        /* (8 x 8 tiles of 256 bytes; the 4 source tiles' Morton order quadrants) */
        u32* d = (u32*)(dst + 256 * (i + j * dst_stride));
        const u32* q[4] = {
            (const u32*)(src + 256 * (2 * i + 2 * j * src_stride)),
            (const u32*)(src + 256 * (2 * i + 1 + 2 * j * src_stride)),
            (const u32*)(src + 256 * (2 * i + (2 * j + 1) * src_stride)),
            (const u32*)(src + 256 * (2 * i + 1 + (2 * j + 1) * src_stride)),
        };
        for (int k = 0; k < 4; k++) {
          const u32* a = q[k];
          for (int n = 0; n < 16; n++, a += 4) {
            *d++ = avg4_rgba8(a[0], a[1], a[2], a[3]);
          }
        }
      }
    }
    src = dst;
    level_size >>= 2;
    w = dw;
    h = dh;
  }
}

void ctr_gpu_tex_update(int handle, const uint8_t* rgba) {
  if (handle < 0 || handle >= MAX_TEXTURES || g.textures[handle].used != 1 ||
      g.textures[handle].pool >= 0) {
    return;
  }
  TexSlot* slot = &g.textures[handle];
  C3D_Tex* tex = &slot->tex;
  const u32 addr = (u32)tex->data;
  if (tex->fmt != GPU_RGBA8 || (addr >= OS_VRAM_VADDR && addr < OS_VRAM_VADDR + OS_VRAM_SIZE)) {
    return;
  }
  if (s_pipeline) {
    /* (AI-assisted) the frame on the GPU may still read these texels: from the first update in a
     * frame on, write the other copy (the frame before is done) */
    const u32 total = C3D_TexCalcTotalSize(tex->size, tex->maxLevel);
    if (!slot->alt) {
      ctr_linear_lock();
      slot->alt = linearAlloc(total);
      ctr_linear_unlock();
      if (slot->alt) {
        memcpy(slot->alt, tex->data, total);
        slot->alt_frame = -1;
      }
    }
    if (slot->alt && slot->alt_frame != g.frame_no) {
      void* t = tex->data;
      tex->data = slot->alt;
      slot->alt = t;
      slot->alt_frame = g.frame_no;
    }
  }
  swizzle_rgba8((uint8_t*)tex->data, tex->width, tex->height, rgba);
  if (tex->maxLevel > 0) {
    tex_mipmaps_rgba8(tex); /* (AI-assisted) (RGBA8 checked above) */
  }
  cache_flush_frame(tex->data, C3D_TexCalcTotalSize(tex->size, tex->maxLevel));
  /* bind again at the next use: that clears the GPU's texture cache (it may hold old texels).
   * (AI-assisted, j2gaps) The state cache too: a next draw with the same state words returned
   * before the bind, so the unit kept the old address (the other copy in pipeline mode) and the
   * animation showed a frame late */
  if (g.bound_tex == tex) {
    g.bound_tex = NULL;
    g.last_state_valid = 0;
  }
}

void ctr_gpu_tex_delete(int handle) {
  if (handle < 0 || handle >= MAX_TEXTURES) {
    return;
  }
  ctr_linear_lock();
  s_tex_anim_slot[handle] = 0; /* (AI-assisted) */
  if (g.textures[handle].used == 1) {
    g.textures[handle].used = 2; /* pending */
    g.pending_delete[g.pending_delete_count++] = handle;
  }
  ctr_linear_unlock();
}

/* ---------------- drawing ---------------- */

static GPU_TESTFUNC map_test(uint8_t t) {
  switch (t) {
    case CTR_TEST_NEVER:
      return GPU_NEVER;
    case CTR_TEST_ALWAYS:
      return GPU_ALWAYS;
    case CTR_TEST_GEQUAL:
      return GPU_GEQUAL;
    case CTR_TEST_GREATER:
      return GPU_GREATER;
    case CTR_TEST_LESS:
      return GPU_LESS;
    case CTR_TEST_LEQUAL:
      return GPU_LEQUAL;
    case CTR_TEST_EQUAL:
      return GPU_EQUAL;
    case CTR_TEST_NOTEQUAL:
      return GPU_NOTEQUAL;
  }
  return GPU_ALWAYS;
}

/* ---------------- (AI-assisted) raw GPU commands for the hot draws ----------------
 * citro3d spends ~2000 ARM instructions on every draw before the draw itself: C3Di_UpdateContext
 * scans the 96 float uniform dirty flags of the vertex and of the geometry shader
 * (C3D_UpdateUniforms, ~660 instructions each even when nothing is dirty), and each register
 * write is a call to GPUCMD_Add (~45 instructions). With config.ini raw_cmds the draws (level
 * meshes, merc, sprites, sky, HUD) write their command words into the command buffer themselves:
 * state only where it changed, uniforms in one burst, the draw from a fixed template.
 *
 * Invariant (citro3d is never wrong about the GPU): citro3d's context always describes the GPU's
 * state, as far as citro3d tracks it.
 *  - A raw write of texture combiner stages 0-1, the fragment operations (C3D_Effect), texture
 *    unit 0 or attribute buffer 0 stores the same value in the context, without a dirty flag.
 *  - Everything citro3d still has to send (dirty flags, uniforms set with C3D_FVUnif*: noted in
 *    s_raw.unif_pending) is sent with C3Di_UpdateContext before a raw draw's uniforms and the draw
 *    (raw_sync). citro3d sends whole groups from its context, which already holds the raw values.
 *  - Raw state compares against the context (what the GPU has, or gets at the next sync), so it
 *    is right after citro3d's own draws (clouds, envmap, glows) as well.
 * After the HOME Menu citro3d sends everything again (APTHOOK_ONRESTORE), but not the uniforms
 * written raw (matrices, bones, lights): those caches are dropped then (s_raw.apt_epoch).
 * The post-vertex cache: citro3d clears it after every draw. Raw draws do the same, unless
 * config.ini vcache_reuse: then a draw with the same vertex buffer and uniforms as the draw before
 * keeps it (the cache holds vertex shader outputs by index), and it's cleared before any other
 * draw, command list split or the frame end (raw_vc_flush). */
static struct {
  int on;           /* config.ini raw_cmds */
  int vcache_reuse; /* config.ini vcache_reuse */
  int unif_pending; /* citro3d has uniforms to send (C3D_FVUnif* since its last update) */
  volatile u32 apt_epoch;
  u32 seen_epoch;
  int hooked;
  aptHookCookie cookie;
  int vc_dirty;   /* a raw draw left vertices in the post-vertex cache */
  int vs_changed; /* vertex buffer, uniforms or program changed since the last draw */
  const void* vbuf_va; /* g.vbuf and its physical address */
  u32 vbuf_pa;
  u32 quad_pa;    /* physical address of g.quad_indices */
  /* (AI-assisted) config.ini cmd_dedup: register writes that would give a register the value it
   * has are left out (texture unit 0's type); cmd_dedup_draw: the draw template's primitive mode
   * and vertex offset too. prim: PRIMITIVE_CONFIG's mode byte as the GPU has it, voff0: its
   * VERTEX_OFFSET is 0 (-1 / 0: unknown: the frame start, the HOME Menu, a citro3d draw) */
  int dedup, dedup_draw;
  int prim, voff0;
  const C3D_Tex* tex0_ctx; /* cmd_dedup: unit 0's texture after the last raw bind, its type */
  u32 tex0_fmt;
  /* (AI-assisted, micro-gpu) cmd_dedup: unit 0's border, dim, param, lod and address registers
   * as the last raw bind wrote them (valid while tex0_ctx is: see bind_texture) */
  u32 tex0_regs[5];
  int tex0_regs_ok; /* tex0_regs hold what the GPU has (static_cmdlists: not across stretches) */
} s_raw = {.on = 1, .dedup = 1, .prim = -1};

/* (AI-assisted) a citro3d draw writes the draw registers itself (c3d_draw_* below) */
static inline void raw_forget_draw_regs(void) {
  s_raw.prim = -1;
  s_raw.voff0 = 0;
}
static inline void raw_forget_regs(void) {
  raw_forget_draw_regs();
  s_raw.tex0_ctx = NULL;
}
static inline void c3d_draw_arrays_(GPU_Primitive_t prim, int first, int size) {
  raw_forget_draw_regs();
  C3D_DrawArrays(prim, first, size);
}
static inline void c3d_draw_elements_(GPU_Primitive_t prim, int count, int type,
                                      const void* indices) {
  raw_forget_draw_regs();
  C3D_DrawElements(prim, count, type, indices);
}
#define C3D_DrawArrays c3d_draw_arrays_
#define C3D_DrawElements c3d_draw_elements_

#define RAW_BUF_BASE 0x18000000u /* citro3d's attribute buffer base (BufInfo_Init) */
#define RAW_HDR(reg, mask) ((u32)(reg) | ((u32)(mask) << 16))

static void raw_apt_hook(APT_HookType type, void* param) {
  (void)param;
  if (type == APTHOOK_ONRESTORE) {
    s_raw.apt_epoch++;
  }
}

void ctr_gpu_set_raw_cmds(int on) {
  if (on && !s_raw.hooked) {
    aptHook(&s_raw.cookie, raw_apt_hook, NULL);
    s_raw.hooked = 1;
  }
  s_raw.on = on ? 1 : 0;
}

void ctr_gpu_set_vcache_reuse(int on) {
  s_raw.vcache_reuse = on ? 1 : 0;
}

void ctr_gpu_set_cmd_dedup(int on, int draw) {
  s_raw.dedup = on ? 1 : 0;
  s_raw.dedup_draw = draw ? 1 : 0;
}

static inline u32* raw_ptr(void) {
  return gpuCmdBuf + gpuCmdBufOffset;
}
static inline void raw_end(u32* p) {
  gpuCmdBufOffset = (u32)(p - gpuCmdBuf);
}
/* one register (mask: the bytes written) */
static inline u32* raw_w(u32* p, u32 reg, u32 mask, u32 v) {
  p[0] = v;
  p[1] = RAW_HDR(reg, mask);
  return p + 2;
}
/* n >= 1 values: consecutive registers from reg (seq) or all to reg (a data port) */
static inline u32* raw_n(u32* p, u32 reg, const u32* v, u32 n, int seq) {
  p[0] = v[0];
  p[1] = RAW_HDR(reg, 0xF) | ((n - 1) << 20) | (seq ? BIT(31) : 0);
  for (u32 i = 1; i < n; i++) {
    p[1 + i] = v[i];
  }
  p += n + 1;
  if (!(n & 1)) {
    *p++ = 0; /* commands start at 8 byte boundaries */
  }
  return p;
}

/* the post-vertex cache clear (and citro3d's two writes after it) */
static inline u32* raw_vc_clear(u32* p) {
  p = raw_w(p, GPUREG_VTX_FUNC, 0xF, 1);
  p = raw_w(p, GPUREG_PRIMITIVE_CONFIG, 0x8, 0);
  p = raw_w(p, GPUREG_PRIMITIVE_CONFIG, 0x8, 0);
  s_raw.vc_dirty = 0;
  return p;
}
/* before anything that isn't a raw indexed draw: a cache left by vcache_reuse is cleared */
static void raw_vc_flush(void) {
  if (s_raw.vc_dirty) {
    raw_end(raw_vc_clear(raw_ptr()));
  }
}

static void raw_note_unif(void) {
  s_raw.unif_pending = 1;
}

/* a frame starts with citro3d's state (frame_state_reset) and an empty post-vertex cache */
static void raw_frame_reset(void) {
  raw_forget_regs();
  s_raw.vc_dirty = 0;
  s_raw.vs_changed = 1;
  s_raw.unif_pending = 1;
}

/* what the raw draws compare against is gone (the HOME Menu used the GPU) */
static void raw_check_epoch(void) {
  if (s_raw.apt_epoch != s_raw.seen_epoch) {
    s_raw.seen_epoch = s_raw.apt_epoch;
    raw_forget_regs();
    g.env_valid = 0;
    g.last_mesh_valid = 0;
    g.last_skin_valid = 0;
    g.skin_bl_valid = 0; /* (AI-assisted) */
    s_raw.vs_changed = 1;
  }
}

/* (AI-assisted, micro-gpu) the procedural texture tables in the GPU at the frame begin (cmd_dump) */
static void proc_lut_snapshot(void) {
  const int ok = s_proc_lut_epoch == s_raw.apt_epoch;
  s_proc_lut_map_f0 = ok ? s_proc_lut_map_gpu : NULL;
  s_proc_lut_color_f0 = ok ? s_proc_lut_color_gpu : NULL;
}

/* before a raw draw's uniforms and the draw: citro3d sends what it has pending */
static inline void raw_sync(void) {
  raw_check_epoch();
  if (s_raw.unif_pending || (__C3D_Context.flags & ~(u32)(C3DiF_Active | C3DiF_DrawUsed))) {
    s_ctx_updates++; /* (AI-assisted, gpu-rt) static_cmdlists: citro3d wrote words */
    C3Di_UpdateContext();
    s_raw.unif_pending = 0;
    s_raw.vs_changed = 1;
  }
}

/* attribute buffer 0 (the only one the draws use) at physical address pa */
static void raw_vertex_buffer(u32 pa, u32 flags0, u32 flags1) {
  C3D_BufInfo* b = &__C3D_Context.bufInfo;
  const u32 off = pa - RAW_BUF_BASE;
  C3D_BufCfg* c = &b->buffers[0];
  if (b->bufCount != 1 || b->base_paddr != RAW_BUF_BASE) {
    /* not citro3d's single buffer layout yet: citro3d sends all of it (raw_sync) */
    BufInfo_Init(b);
    b->bufCount = 1;
    c->offset = off;
    c->flags[0] = flags0;
    c->flags[1] = flags1;
    __C3D_Context.flags |= C3DiF_BufInfo;
    s_raw.vs_changed = 1;
    return;
  }
  if (c->flags[0] != flags0 || c->flags[1] != flags1) {
    const u32 v[3] = {off, flags0, flags1};
    raw_end(raw_n(raw_ptr(), GPUREG_ATTRIBBUFFER0_OFFSET, v, 3, 1));
  } else if (c->offset != off) {
    raw_end(raw_w(raw_ptr(), GPUREG_ATTRIBBUFFER0_OFFSET, 0xF, off));
  } else {
    return;
  }
  c->offset = off;
  c->flags[0] = flags0;
  c->flags[1] = flags1;
  s_raw.vs_changed = 1;
}
/* attribute buffer 0 config of each program's vertex layout: permutation, stride, attributes */
#define RAW_BUF_FLAGS1(stride, attribs, perm_hi) \
  ((u32)(perm_hi) | ((u32)(stride) << 16) | ((u32)(attribs) << 28))

/* float uniforms from loc: f32 values (4 per vector, in C3D_FVec order w z y x) or float24 words
 * (3 per vector), one config write and one burst to the data port */
static inline u32* raw_unif(u32* p, int loc, const u32* data, u32 words, int f32) {
  p = raw_w(p, GPUREG_VSH_FLOATUNIFORM_CONFIG, 0xF, (u32)loc | (f32 ? BIT(31) : 0));
  s_raw.vs_changed = 1;
  return raw_n(p, GPUREG_VSH_FLOATUNIFORM_DATA, data, words, 0);
}

/* the f24 encoding of libctru's f32tof24, packed 3 words per vector in the order the GPU takes
 * float24 uniforms. (AI-assisted) The mantissa is rounded to nearest, not truncated as f32tof24
 * does: truncating every chunk's matrix translation left sub-pixel gaps between neighbouring level
 * chunks up close (the converter's chunk grids are watertight otherwise). */
/* (AI-assisted, micro-gpu) Without branches: |f| rounded at bit 6 (a carry out of the mantissa
 * raises the exponent), shifted to 7.16 and the exponent rebased from 127 to 63; below the
 * smallest exponent (zeros and denormals too) +-0, above the largest +-(exponent 0x7f, mantissa
 * 0) as before. The same result as the branching form for all 2^32 inputs (checked on the host:
 * docs/3ds-port/performance.md "micro-gpu"); 54 instead of 91 ARM instructions per raw_pack_f24. */
static inline u32 raw_f24(float f) {
  union {
    float f;
    u32 i;
  } u = {f};
  s32 t = (s32)(((u.i & 0x7fffffffu) + 0x40u) >> 7) - (s32)(64u << 16);
  t = t < 0 ? 0 : t;
  t = t >= (s32)(0x80u << 16) ? (s32)(0x7fu << 16) : t;
  return ((u.i >> 8) & 0x800000u) | (u32)t;
}
/* v: C3D_FVec order (w, z, y, x) */
static void raw_pack_f24(const float* v, u32* out) {
  const u32 w = raw_f24(v[0]), z = raw_f24(v[1]), y = raw_f24(v[2]), x = raw_f24(v[3]);
  out[0] = (w << 8) | (z >> 16);
  out[1] = (z << 16) | (y >> 8);
  out[2] = (y << 24) | x;
}

/* The draws: C3D_DrawElements / C3D_DrawArrays' command words (without their context update).
 * Indexed: the index buffer offset and count go in words 4 and 6. */
static const u32 kRawElements[24] = {
    GPU_GEOMETRY_PRIM, RAW_HDR(GPUREG_PRIMITIVE_CONFIG, 0x2),
    1,                 RAW_HDR(GPUREG_RESTART_PRIMITIVE, 0xF),
    0,                 RAW_HDR(GPUREG_INDEXBUFFER_CONFIG, 0xF),
    0,                 RAW_HDR(GPUREG_NUMVERTICES, 0xF),
    0,                 RAW_HDR(GPUREG_VERTEX_OFFSET, 0xF),
    0x100,             RAW_HDR(GPUREG_GEOSTAGE_CONFIG, 0x2),
    0x100,             RAW_HDR(GPUREG_GEOSTAGE_CONFIG2, 0x2),
    0,                 RAW_HDR(GPUREG_START_DRAW_FUNC0, 0x1),
    1,                 RAW_HDR(GPUREG_DRAWELEMENTS, 0xF),
    1,                 RAW_HDR(GPUREG_START_DRAW_FUNC0, 0x1),
    0,                 RAW_HDR(GPUREG_GEOSTAGE_CONFIG, 0x2),
    0,                 RAW_HDR(GPUREG_GEOSTAGE_CONFIG2, 0x2),
};
static const u32 kRawArrays[22] = {
    GPU_TRIANGLES, RAW_HDR(GPUREG_PRIMITIVE_CONFIG, 0x2),
    1,             RAW_HDR(GPUREG_RESTART_PRIMITIVE, 0xF),
    0x80000000u,   RAW_HDR(GPUREG_INDEXBUFFER_CONFIG, 0xF),
    0,             RAW_HDR(GPUREG_NUMVERTICES, 0xF),
    0,             RAW_HDR(GPUREG_VERTEX_OFFSET, 0xF),
    1,             RAW_HDR(GPUREG_GEOSTAGE_CONFIG2, 0x1),
    0,             RAW_HDR(GPUREG_START_DRAW_FUNC0, 0x1),
    1,             RAW_HDR(GPUREG_DRAWARRAYS, 0xF),
    1,             RAW_HDR(GPUREG_START_DRAW_FUNC0, 0x1),
    0,             RAW_HDR(GPUREG_GEOSTAGE_CONFIG2, 0x1),
    1,             RAW_HDR(GPUREG_VTX_FUNC, 0xF),
};

/* an indexed triangle list: index_pa = physical address of the first u16 index */
static void raw_draw_elements(u32 index_pa, u32 count) {
  u32* p = raw_ptr();
  if (s_raw.vc_dirty && s_raw.vs_changed) {
    p = raw_vc_clear(p);
  }
  if (s_raw.dedup_draw && s_raw.prim == GPU_GEOMETRY_PRIM && s_raw.voff0) {
    /* (AI-assisted) cmd_dedup_draw: the GPU has this mode and vertex offset 0 already: the
     * template without its PRIMITIVE_CONFIG and VERTEX_OFFSET writes (20 words) */
    p[0] = 1;
    p[1] = kRawElements[3];
    p[2] = (index_pa - RAW_BUF_BASE) | BIT(31);
    p[3] = kRawElements[5];
    p[4] = count;
    p[5] = kRawElements[7];
    for (int i = 10; i < 24; i++) {
      p[i - 4] = kRawElements[i];
    }
    p += 20;
    g.cur.cmd_dedup_words += 4;
  } else {
    for (int i = 0; i < 24; i += 4) { /* (4 words per ldm / stm) */
      p[i] = kRawElements[i];
      p[i + 1] = kRawElements[i + 1];
      p[i + 2] = kRawElements[i + 2];
      p[i + 3] = kRawElements[i + 3];
    }
    p[4] = (index_pa - RAW_BUF_BASE) | BIT(31); /* u16 indices */
    p[6] = count;
    p += 24;
  }
  s_raw.prim = GPU_GEOMETRY_PRIM;
  s_raw.voff0 = 1;
  if (s_raw.vcache_reuse) {
    s_raw.vc_dirty = 1;
  } else {
    p = raw_vc_clear(p);
  }
  raw_end(p);
  s_raw.vs_changed = 0;
  __C3D_Context.flags |= C3DiF_DrawUsed;
}

/* prim: GPU_TRIANGLES or (AI-assisted) GPU_TRIANGLE_STRIP (ctr_gpu_draw_strip), as C3D_DrawArrays */
static void raw_draw_arrays(u32 count, u32 prim) {
  u32* p = raw_ptr();
  if (s_raw.vc_dirty) {
    p = raw_vc_clear(p);
  }
  for (int i = 0; i < 22; i += 2) {
    p[i] = kRawArrays[i];
    p[i + 1] = kRawArrays[i + 1];
  }
  p[0] = prim;
  p[6] = count;
  raw_end(p + 22);
  s_raw.prim = (int)prim; /* (AI-assisted) cmd_dedup_draw: what the template left */
  s_raw.voff0 = 1;
  s_raw.vs_changed = 0;
  __C3D_Context.flags |= C3DiF_DrawUsed;
}

/* the immediate draws' vertices: physical address of g.vbuf + off */
static inline u32 raw_vbuf_pa(size_t off) {
  if (s_raw.vbuf_va != g.vbuf) {
    s_raw.vbuf_va = g.vbuf;
    s_raw.vbuf_pa = osConvertVirtToPhys(g.vbuf);
  }
  return s_raw.vbuf_pa + (u32)off;
}

/* (AI-assisted) the same through citro3d (its draws): only marked for sending when it changed */
static void c3d_vertex_buffer(u32 pa, const void* va, u32 stride, int attribs, u64 perm) {
  const C3D_BufInfo* b = &__C3D_Context.bufInfo;
  const C3D_BufCfg* c = &b->buffers[0];
  if (b->bufCount == 1 && b->base_paddr == RAW_BUF_BASE && c->offset == pa - RAW_BUF_BASE &&
      c->flags[0] == (u32)perm && c->flags[1] == RAW_BUF_FLAGS1(stride, attribs, perm >> 32)) {
    return;
  }
  C3D_BufInfo* buf = C3D_GetBufInfo();
  BufInfo_Init(buf);
  BufInfo_Add(buf, va, stride, attribs, perm);
}

static void apply_state_tint(const ctr_draw_state* st, int mesh, uint32_t tint, int raw);
static void apply_state(const ctr_draw_state* st, int mesh, int raw);

/* The GPU command buffer is fixed size, and a frame's commands must all fit (libctru panics when
 * it overflows; splitting the frame doesn't make room: the buffer only restarts at the next
 * C3D_FrameBegin). A draw that might not fit is dropped (counted in the stats). */
static int cmd_room(void) {
  if (gpuCmdBufSize - gpuCmdBufOffset < CMD_RESERVE_WORDS + CMD_DRAW_WORDS) {
    g.cur.dropped_draws++;
    return 0;
  }
  return 1;
}

/* Texture unit 0: only bound again (which clears the GPU's texture cache) when the texture, its
 * memory (pools move to VRAM) or its parameters change. */
static void bind_texture(TexSlot* slot, const ctr_draw_state* st, int raw) {
  C3D_Tex* tex = &slot->tex;
  const GPU_TEXTURE_FILTER_PARAM f = st->filter ? GPU_LINEAR : GPU_NEAREST;
  u32 param = tex->param & ~(GPU_TEXTURE_MAG_FILTER(GPU_LINEAR) | GPU_TEXTURE_MIN_FILTER(GPU_LINEAR) |
                             GPU_TEXTURE_WRAP_S(3) | GPU_TEXTURE_WRAP_T(3) |
                             GPU_TEXTURE_MIP_FILTER(GPU_LINEAR));
  param |= GPU_TEXTURE_MAG_FILTER(f) | GPU_TEXTURE_MIN_FILTER(f) |
           GPU_TEXTURE_WRAP_S(st->clamp_s ? GPU_CLAMP_TO_EDGE : GPU_REPEAT) |
           GPU_TEXTURE_WRAP_T(st->clamp_t ? GPU_CLAMP_TO_EDGE : GPU_REPEAT);
  /* mip levels: the nearest one (bilinear inside it), or blending two (trilinear) */
  u8 max_level = 0;
  if (slot->levels > 1 && g_mip_mode > 0) {
    max_level = (u8)(slot->levels - 1);
    if (g_mip_mode == 2) {
      param |= GPU_TEXTURE_MIP_FILTER(GPU_LINEAR);
    }
  }
  /* (AI-assisted) mip_bias: only with mip levels (0 = what every texture has) */
  const u16 lod_bias = max_level ? s_lod_bias : 0;
  if (g.bound_tex == tex && g.bound_data == tex->data && g.bound_param == param &&
      tex->maxLevel == max_level && tex->lodBias == lod_bias) {
    return;
  }
  /* (AI-assisted) the same texels as the unit has (only the filter, wrap or mip settings change):
   * tex_cache_clear = uploads may keep the cache */
  const int same_texels = g.bound_tex == tex && g.bound_data == tex->data;
  tex->param = param;
  tex->maxLevel = max_level;
  tex->lodBias = lod_bias;
  if (raw) {
    /* (AI-assisted) what C3Di_SetTex and citro3d's texture unit update write: the unit's
     * registers, the unit on, then its cache cleared; the context gets the texture (invariant) */
    C3Di_Context* ctx = &__C3D_Context;
    const u32 regs[5] = {tex->border, tex->dim, tex->param, tex->lodParam,
                         osConvertVirtToPhys(tex->data) >> 3};
    /* (AI-assisted) cmd_dedup: the registers the last raw bind wrote are still the GPU's when
     * citro3d hasn't bound or sent a texture on unit 0 since (the context's texture is still the
     * one written then, and not marked for sending; citro3d's own binds here forget them) */
    const int known = s_raw.dedup && s_raw.tex0_ctx && ctx->tex[0] == s_raw.tex0_ctx &&
                      !(ctx->flags & C3DiF_Tex(0));
    u32* p = raw_ptr();
    if (known && s_raw.tex0_regs_ok && (s_micro & MICRO_TEXREGS)) {
      /* (AI-assisted, micro-gpu) only the registers that change (mostly the address): one write
       * each, or one burst over the changed span when that is shorter. The others already hold
       * these values: the same registers at the draw (libctru's GPU_SetTexture writes them one
       * by one too) */
      u32 diff = 0;
      int first = -1, last = -1, n = 0;
      for (int k = 0; k < 5; k++) {
        if (regs[k] != s_raw.tex0_regs[k]) {
          diff |= 1u << k;
          first = first < 0 ? k : first;
          last = k;
          n++;
        }
      }
      u32 w = 0;
      if (n) {
        const u32 span = (u32)(last - first + 1);
        const u32 burst = span + 1 + ((span & 1) ? 0 : 1);
        if (2u * (u32)n <= burst) {
          for (int k = 0; k < 5; k++) {
            if (diff & (1u << k)) {
              p = raw_w(p, GPUREG_TEXUNIT0_BORDER_COLOR + (u32)k, 0xF, regs[k]);
            }
          }
          w = 2u * (u32)n;
        } else {
          p = raw_n(p, GPUREG_TEXUNIT0_BORDER_COLOR + (u32)first, regs + first, span, 1);
          w = burst;
        }
      }
      g.cur.cmd_dedup_words += 6 - w;
    } else {
      p = raw_n(p, GPUREG_TEXUNIT0_BORDER_COLOR, regs, 5, 1);
    }
    memcpy(s_raw.tex0_regs, regs, sizeof(regs));
    s_raw.tex0_regs_ok = 1;
    /* (AI-assisted) cmd_dedup: the type only when it changes (as the registers above) */
    if (known && s_raw.tex0_fmt == (u32)tex->fmt) {
      g.cur.cmd_dedup_words += 2;
    } else {
      p = raw_w(p, GPUREG_TEXUNIT0_TYPE, 0xF, tex->fmt);
      s_raw.tex0_fmt = (u32)tex->fmt;
    }
    ctx->tex[0] = tex;
    s_raw.tex0_ctx = tex;
    if (!(ctx->texConfig & 1)) {
      ctx->texConfig |= 1;
      p = raw_w(p, GPUREG_TEXUNIT_CONFIG, 0xB, ctx->texConfig);
    }
    /* (AI-assisted) tex_cache_clear = uploads: the clear is left out only when the unit keeps
     * its texture (a level pool's, no pool filled or moved since the last clear) and just its
     * sampling settings change. It used to be left out at every switch between level pool
     * textures, on the idea that the cache holds texels by address (never checked). The suspected
     * cause of the red patches on the Volcanic Crater minecarts on hardware (oct12b, the first
     * build with uploads; Azahar has no texture cache): the carts' body is one 8x8 texture (128
     * bytes, less than a texture unit's 256-byte L1 cache) in a level of red lava textures, as if
     * the cache kept the previous texture's texels. citro3d clears at every texture change too. */
    if (!s_tex_clear_mode || slot->pool < 0 || s_tex_dirty || !same_texels) {
      s_tex_dirty = 0;
      p = raw_w(p, GPUREG_TEXUNIT_CONFIG, 0x4, BIT(16));
    } else {
      g.cur.tex_clears_skipped++;
    }
    raw_end(p);
  } else {
    C3D_TexBind(0, tex);
    s_raw.tex0_ctx = NULL; /* (AI-assisted, micro-gpu) citro3d sends unit 0 (cmd_dedup) */
  }
  g.bound_tex = tex;
  g.bound_data = tex->data;
  g.bound_param = param;
  g.cur.tex_binds++;
}

static int early_depth_ok(const ctr_draw_state* st) {
  return (g_early_depth && (st->flags & CTR_STATE_EARLY_DEPTH) && st->ztest == CTR_TEST_GEQUAL &&
          st->zwrite && st->blend == CTR_BLEND_OFF) ||
         /* (AI-assisted) early_depth = sky / sky32 (and on / block32 with sky_last): the sky at
          * depth 0 after the opaque geometry, dropped early where it covered a block */
         (g_early_sky && g_depth_far && st->ztest == CTR_TEST_GEQUAL);
}

/* (AI-assisted) texture combiner stage id (0 or 1) as env: in the context, and to the GPU raw or
 * through citro3d (dirty flag) when it changed */
static void commit_texenv(int id, const C3D_TexEnv* env, int raw) {
  C3Di_Context* ctx = &__C3D_Context;
  const alias_u32* a = (const alias_u32*)&ctx->texEnv[id];
  const alias_u32* b = (const alias_u32*)env;
  _Static_assert(sizeof(*env) == 20, "C3D_TexEnv compared as 5 words");
  if (!((a[0] ^ b[0]) | (a[1] ^ b[1]) | (a[2] ^ b[2]) | (a[3] ^ b[3]) | (a[4] ^ b[4]))) {
    return;
  }
  ctx->texEnv[id] = *env;
  if (raw) {
    raw_end(raw_n(raw_ptr(), GPUREG_TEXENV0_SOURCE + 8 * (u32)id, (const u32*)env,
                  sizeof(*env) / 4, 1));
  } else {
    ctx->flags |= C3DiF_TexEnv(id);
  }
}

/* (AI-assisted, micro-mg) gpu_micro: what the last full raw apply_state_tint worked from and
 * wrote (MICRO_TEXSTATE). A raw state whose other words, mesh, tint, texture kind and inputs
 * (the settings below) are those, while the GPU's (the context's) combiner stages and fragment
 * operations are still what it wrote, gets the same combiner and fragment registers from the
 * full path, so only its texture is bound (bind_texture, as the full path does). */
typedef struct {
  int valid;
  u32 words[3]; /* the state's words 1..3 (word 0 is the texture) */
  int mesh;
  uint32_t tint;
  u32 kind;     /* textured | alpha_full << 1 (proctex draws never take the short path) */
  u32 in[16];   /* the settings the full path reads */
  u32 tex_config_bit1;
  C3D_TexEnv env0, env1;
  /* (AI-assisted, far-fog) stage 2 and g.ocean_stage2 (merc fog draws: the fog stage) */
  C3D_TexEnv env2;
  int stage2;
  C3Di_Effect effect;
} StateFast;
static StateFast s_sf;
static u32 s_state_fast; /* statistics (ctr_gpu_cmdlist_line) */
static inline int words_eq(const void* a, const void* b, unsigned n) {
  const alias_u32* x = (const alias_u32*)a;
  const alias_u32* y = (const alias_u32*)b;
  u32 d = 0;
  for (unsigned i = 0; i < n; i++) {
    d |= x[i] ^ y[i];
  }
  return d == 0;
}
static inline void state_inputs(u32* in) {
  in[0] = (u32)g_depth_far;
  in[1] = (u32)g_cull_enable;
  in[2] = (u32)g_cull_flip;
  in[3] = (u32)g_early_depth;
  in[4] = (u32)g_early_sky;
  in[5] = (u32)g_early_func;
  in[6] = g_early_clear;
  in[7] = (u32)s_zdef_valid;
  in[8] = s_zscale0;
  in[9] = s_zoffset0;
  in[10] = (u32)g_depth16;
  in[11] = (u32)g.fog_tex_valid;
  in[12] = g.fog_abgr;
  in[13] = g.clear_abgr;
  in[14] = (u32)g_tex1_gating;
  in[15] = (u32)g_proctex;
}

/* mesh: 0 = immediate draws, 1 = level mesh, 2 = skinned mesh (tint = constant color stage).
 * (AI-assisted) 3 = the ocean (ctr_gpu_ocean_draw), 4 = level mesh without the fog stage
 * (fog_stage_skip), 5 = level mesh with the fog amount in the vertex alpha (fog_alpha: its vertex
 * alpha is 0x80, the constant 0x80 stands in for it).
 * raw: written as raw commands (see "raw GPU commands"), else through citro3d. Both keep
 * citro3d's context right. */
static void apply_state_tint(const ctr_draw_state* st, int mesh, uint32_t tint, int raw) {
  if (st->flags || g_depth_far) { /* (AI-assisted) the sky's draws (early_depth = sky) too */
    g.cur.early_depth_draws += early_depth_ok(st);
    g.cur.proctex_draws += (st->flags & CTR_STATE_PROCTEX) != 0;
  }
  _Static_assert(sizeof(*st) == 16, "ctr_draw_state compared as 4 words");
  if (g.last_state_valid && g.last_state_mesh == mesh && g.last_tint == tint &&
      words_equal(&g.last_state, st, 4)) {
    return;
  }
  g.last_state = *st;
  g.last_state_mesh = mesh;
  g.last_tint = tint;
  g.last_state_valid = 1;
  /* (AI-assisted, micro-mg) gpu_micro: the same state but for its texture (see StateFast) */
  const int stex0 = tex_res(st->tex); /* (AI-assisted) an animated slot's texture */
  const int textured0 = stex0 >= 0 && stex0 < MAX_TEXTURES && g.textures[stex0].used == 1;
  const alias_u32* stw = (const alias_u32*)st;
  if (raw && s_sf.valid && (s_micro & MICRO_TEXSTATE) && s_sf.mesh == mesh && s_sf.tint == tint &&
      /* (the filter and clamp bytes only matter to the bind) */
      !((((stw[1] ^ s_sf.words[0]) & 0x0000FFFFu) | ((stw[2] ^ s_sf.words[1]) & 0xFFFFFF00u) |
         (stw[3] ^ s_sf.words[2]))) &&
      !(st->flags & CTR_STATE_PROCTEX) && g.proc_radial < 0 && g.ocean_stage2 == s_sf.stage2 &&
      mesh != 3 && mesh != 6) {
    const u32 kind = textured0 ? 1u | ((u32)g.textures[stex0].alpha_full << 1) : 0u;
    u32 in[16];
    state_inputs(in);
    C3Di_Context* fctx = &__C3D_Context;
    if (kind == s_sf.kind && words_eq(in, s_sf.in, 16) &&
        (fctx->texConfig & BIT(1)) == s_sf.tex_config_bit1 &&
        words_eq(&fctx->texEnv[0], &s_sf.env0, sizeof(C3D_TexEnv) / 4) &&
        words_eq(&fctx->texEnv[1], &s_sf.env1, sizeof(C3D_TexEnv) / 4) &&
        words_eq(&fctx->texEnv[2], &s_sf.env2, sizeof(C3D_TexEnv) / 4) &&
        words_eq(&fctx->effect, &s_sf.effect, sizeof(C3Di_Effect) / 4)) {
      if (textured0) {
        bind_texture(&g.textures[stex0], st, 1);
      }
      s_state_fast++;
      return;
    }
  }
  s_sf.valid = 0;
  /* stage 1: lighting tint for skinned meshes, pass-through otherwise */
  C3D_TexEnv e1;
  C3D_TexEnv* env1 = &e1;
  C3D_TexEnvInit(env1);
  if (mesh == 3) {
    /* (AI-assisted) the ocean (ctr_gpu_ocean_draw): rgb times the shimmer layer (texture unit 2,
     * 0x80 = neutral), then the fog in stage 2 (as the meshes' stage 1) */
    C3D_TexEnvSrc(env1, C3D_RGB, GPU_PREVIOUS, GPU_TEXTURE2, 0);
    C3D_TexEnvFunc(env1, C3D_RGB, GPU_MODULATE);
    C3D_TexEnvScale(env1, C3D_RGB, GPU_TEVSCALE_2);
    C3D_TexEnv e2;
    C3D_TexEnvInit(&e2);
    if (g.fog_tex_valid) {
      C3D_TexEnvSrc(&e2, C3D_RGB, GPU_TEXTURE1, GPU_PREVIOUS, GPU_TEXTURE1);
      C3D_TexEnvOpRgb(&e2, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR,
                      GPU_TEVOP_RGB_SRC_ALPHA);
      C3D_TexEnvFunc(&e2, C3D_RGB, GPU_INTERPOLATE);
    }
    commit_texenv(2, &e2, raw);
    g.ocean_stage2 = 1;
  } else if (mesh == 2) {
    C3D_TexEnvSrc(env1, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, 0);
    C3D_TexEnvFunc(env1, C3D_RGB, GPU_MODULATE);
    C3D_TexEnvColor(env1, tint);
  } else if ((mesh == 1 || mesh == 6) && g.fog_tex_valid) {
    /* fog: rgb = mix(previous, fog color, fog amount) with the amount from the fog ramp texture
     * (texcoord1 from the mesh shader; bound on unit 1 at frame begin) */
    C3D_TexEnvSrc(env1, C3D_RGB, GPU_TEXTURE1, GPU_PREVIOUS, GPU_TEXTURE1);
    C3D_TexEnvOpRgb(env1, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR,
                    GPU_TEVOP_RGB_SRC_ALPHA);
    C3D_TexEnvFunc(env1, C3D_RGB, GPU_INTERPOLATE);
  } else if (mesh == 5) {
    /* (AI-assisted) fog_alpha: rgb = mix(previous, fog color, fog amount) with the amount from the
     * vertex shader's color alpha and the fog color as this stage's constant */
    C3D_TexEnvSrc(env1, C3D_RGB, GPU_CONSTANT, GPU_PREVIOUS, GPU_PRIMARY_COLOR);
    C3D_TexEnvOpRgb(env1, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR,
                    GPU_TEVOP_RGB_SRC_ALPHA);
    C3D_TexEnvFunc(env1, C3D_RGB, GPU_INTERPOLATE);
    C3D_TexEnvColor(env1, g.fog_abgr);
  } else if (mesh == 0 && st->blend == CTR_BLEND_OVER_CLEAR) {
    /* (AI-assisted) alpha blending over the clear color, computed here: rgb = mix(clear color,
     * previous, previous alpha), opaque (no framebuffer read) */
    C3D_TexEnvSrc(env1, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_PREVIOUS);
    C3D_TexEnvOpRgb(env1, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR,
                    GPU_TEVOP_RGB_SRC_ALPHA);
    C3D_TexEnvFunc(env1, C3D_RGB, GPU_INTERPOLATE);
    C3D_TexEnvSrc(env1, C3D_Alpha, GPU_CONSTANT, 0, 0);
    C3D_TexEnvFunc(env1, C3D_Alpha, GPU_REPLACE);
    C3D_TexEnvColor(env1, g.clear_abgr);
  }
  C3D_TexEnv e0;
  C3D_TexEnv* env = &e0;
  C3D_TexEnvInit(env);
  const int stex = tex_res(st->tex); /* (AI-assisted) an animated slot's texture */
  int textured = stex >= 0 && stex < MAX_TEXTURES && g.textures[stex].used == 1;
  TexSlot* slot = textured ? &g.textures[stex] : NULL;
  /* (AI-assisted) radial textures from the procedural texture unit (no texels read); compact
   * textures store GS alpha doubled (0xff = 1.0) */
  const int proc = textured && (st->flags & CTR_STATE_PROCTEX) && g_proctex && slot->radial >= 0 &&
                   s_radial[slot->radial];
  const GPU_TEVSRC tsrc = proc ? GPU_TEXTURE3 : GPU_TEXTURE0;
  const int afull = textured && slot->alpha_full;
  if (proc) {
    if (g.proc_radial != slot->radial) {
      C3D_ProcTexBind(0, &s_proctex);
      /* (AI-assisted, micro-gpu) the tables only when the LUT memory has others (see
       * s_proc_lut_epoch); citro3d's context keeps pointing at them */
      const C3D_ProcTexColorLut* lut = s_radial[slot->radial];
      const int known = s_proc_lut_epoch == s_raw.apt_epoch && (s_micro & MICRO_PROCLUT);
      if (!known || s_proc_lut_map_gpu != &s_proc_map || __C3D_Context.procTexLut[1] != &s_proc_map) {
        C3D_ProcTexLutBind(GPU_LUT_RGBMAP, &s_proc_map);
      }
      if (!known || s_proc_lut_color_gpu != lut || __C3D_Context.procTexColorLut != lut) {
        C3D_ProcTexColorLutBind(s_radial[slot->radial]);
      }
      s_proc_lut_map_gpu = &s_proc_map;
      s_proc_lut_color_gpu = lut;
      s_proc_lut_epoch = s_raw.apt_epoch;
      g.proc_radial = slot->radial;
    }
    if (g.bound_tex) {
      C3D_TexBind(0, NULL); /* texture unit 0 off: it would fetch texels nothing uses */
      s_raw.tex0_ctx = NULL; /* (AI-assisted, micro-gpu) */
      g.bound_tex = NULL;
    }
  } else if (g.proc_radial >= 0) {
    C3D_ProcTexBind(0, NULL);
    g.proc_radial = -1;
  }
  if (textured) {
    if (!proc) {
      bind_texture(slot, st, raw);
    }
    if (mesh == 6) {
      /* (AI-assisted, j1ocean) the PS2 ocean (ctr_gpu_ocean_full_draw*): GS modulate (texture
       * alpha 0xff = 1, vertex color 0x80 = 1). Alpha: TCC set = tex.a * v.a; clear = v.a, or with
       * the sky reflection (tint 1) tex.a here, for stage 2's weight (v.a comes in there) */
      C3D_TexEnvSrc(env, C3D_Both, tsrc, GPU_PRIMARY_COLOR, 0);
      C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
      C3D_TexEnvScale(env, C3D_RGB, GPU_TEVSCALE_2);
      C3D_TexEnvScale(env, C3D_Alpha, GPU_TEVSCALE_2);
      if (!(st->tcc & 1)) {
        C3D_TexEnvSrc(env, C3D_Alpha, tint ? tsrc : GPU_PRIMARY_COLOR, 0, 0);
        C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
        C3D_TexEnvScale(env, C3D_Alpha, tint ? GPU_TEVSCALE_1 : GPU_TEVSCALE_2);
      }
    } else if (mesh) {
      /* level meshes / merc: texture alpha 0xff = 1, vertex color 0x80 = 1 (merc: the skin shader
       * outputs half the lit color, so x4) */
      C3D_TexEnvSrc(env, C3D_Both, tsrc, GPU_PRIMARY_COLOR, 0);
      C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
      C3D_TexEnvScale(env, C3D_RGB, mesh == 2 ? GPU_TEVSCALE_4 : GPU_TEVSCALE_2);
      C3D_TexEnvScale(env, C3D_Alpha, GPU_TEVSCALE_2);
      if (st->decal) {
        /* decal: the texture color alone (alpha as above) */
        C3D_TexEnvSrc(env, C3D_RGB, tsrc, 0, 0);
        C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
        C3D_TexEnvScale(env, C3D_RGB, GPU_TEVSCALE_1);
      }
      if (mesh == 5) {
        /* (AI-assisted) fog_alpha: the vertex alpha (0x80) as this stage's constant */
        C3D_TexEnvSrc(env, C3D_Alpha, tsrc, GPU_CONSTANT, 0);
        C3D_TexEnvColor(env, 0x80000000u);
      }
    } else if (st->decal) {
      C3D_TexEnvSrc(env, C3D_RGB, tsrc, 0, 0);
      C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
      C3D_TexEnvScale(env, C3D_RGB, GPU_TEVSCALE_1);
      C3D_TexEnvSrc(env, C3D_Alpha, st->tcc ? tsrc : GPU_PRIMARY_COLOR, 0, 0);
      C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
      C3D_TexEnvScale(env, C3D_Alpha, st->tcc && afull ? GPU_TEVSCALE_1 : GPU_TEVSCALE_2);
    } else {
      /* GS modulate: tex * vertex / 128, i.e. 2x with 0..1 colors */
      C3D_TexEnvSrc(env, C3D_RGB, tsrc, GPU_PRIMARY_COLOR, 0);
      C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
      C3D_TexEnvScale(env, C3D_RGB, GPU_TEVSCALE_2);
      if (st->tcc) {
        C3D_TexEnvSrc(env, C3D_Alpha, tsrc, GPU_PRIMARY_COLOR, 0);
        C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
        C3D_TexEnvScale(env, C3D_Alpha, afull ? GPU_TEVSCALE_2 : GPU_TEVSCALE_4);
      } else {
        C3D_TexEnvSrc(env, C3D_Alpha, GPU_PRIMARY_COLOR, 0, 0);
        C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
        C3D_TexEnvScale(env, C3D_Alpha, GPU_TEVSCALE_2);
      }
    }
  } else {
    /* untextured: the GS outputs the vertex color as it is (direct_basic.vert, tfrag3.frag and
     * merc2.frag without texture), alpha 0x80 = 1. (merc: the skin shader outputs half) */
    C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    C3D_TexEnvScale(env, C3D_RGB, mesh == 2 ? GPU_TEVSCALE_2 : GPU_TEVSCALE_1);
    C3D_TexEnvScale(env, C3D_Alpha, GPU_TEVSCALE_2);
    if (mesh == 5) {
      C3D_TexEnvSrc(env, C3D_Alpha, GPU_CONSTANT, 0, 0); /* (AI-assisted) fog_alpha, as above */
      C3D_TexEnvColor(env, 0x80000000u);
    }
  }
  commit_texenv(1, env1, raw);
  commit_texenv(0, env, raw);
  /* (AI-assisted) fog_stage_skip (mesh 4), and with tex1_gating every draw whose combiner doesn't
   * read texture unit 1 (only the fog ramp stages of mesh 1 and 3 do): unit 1 off while nothing
   * reads it (raw draws: the unit enable bits only, no cache clear; the ramp's texels stay valid,
   * a new color binds it again through citro3d), on again for the draws that read it. The context's
   * texConfig is what the GPU has (or gets: citro3d recomputes it from its bound textures when a
   * bind is pending, which only turns unit 1 on). */
  /* (AI-assisted, far-fog) a merc draw with the game's fog (CTR_TCC_FOG): the fog ramp stage in
   * stage 2 (the ocean's), its amount from texture unit 1 (ctr_skin_opt's texcoord1) */
  const int merc_fog = mesh == 2 && (st->tcc & CTR_TCC_FOG) && g.fog_tex_valid;
  if (g.fog_tex_valid) {
    C3Di_Context* tctx = &__C3D_Context;
    const int need1 = mesh == 1 || mesh == 3 || mesh == 6 || merc_fog;
    const int want_off = g_tex1_gating ? !need1 : mesh == 4;
    const int is_off = !(tctx->texConfig & BIT(1));
    /* (AI-assisted, micro-mg) a glow drawn raw (MICRO_RAWGLOW) leaves unit 1 as the citro3d draw
     * did (it never switched it: the same registers at the draw) */
    const int gate = raw && !(st->flags & CTR_STATE_PROCTEX);
    if (gate && want_off != is_off) {
      if (want_off) {
        tctx->texConfig &= ~BIT(1);
      } else {
        tctx->texConfig |= BIT(1);
      }
      raw_end(raw_w(raw_ptr(), GPUREG_TEXUNIT_CONFIG, 0xB, tctx->texConfig & ~BIT(16)));
      g.cur.unit1_toggles++;
    } else if (!raw && need1 && is_off) {
      tctx->flags |= C3DiF_Tex(1); /* citro3d binds it again (unit on, cache cleared) */
    }
    g.unit1_off = want_off;
  }
  if (mesh == 6) {
    /* (AI-assisted, j1ocean) the PS2 ocean's stage 2: with the sky reflection (tint 1) rgb =
     * env map (unit 2) * previous alpha + previous (the fogged ocean), the PS2's env map pass
     * added with the destination alpha as its weight; TCC clear: alpha = v.a (the near ocean's
     * blend weight). Without: pass-through. ocean_stage2 = 2: stage 2 isn't the fog stage. */
    C3D_TexEnv e2;
    C3D_TexEnvInit(&e2);
    if (tint) {
      C3D_TexEnvSrc(&e2, C3D_RGB, GPU_TEXTURE2, GPU_PREVIOUS, GPU_PREVIOUS);
      C3D_TexEnvOpRgb(&e2, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA,
                      GPU_TEVOP_RGB_SRC_COLOR);
      C3D_TexEnvFunc(&e2, C3D_RGB, GPU_MULTIPLY_ADD);
      if (!(st->tcc & 1)) {
        C3D_TexEnvSrc(&e2, C3D_Alpha, GPU_PRIMARY_COLOR, 0, 0);
        C3D_TexEnvFunc(&e2, C3D_Alpha, GPU_REPLACE);
        C3D_TexEnvScale(&e2, C3D_Alpha, GPU_TEVSCALE_2);
      }
    }
    commit_texenv(2, &e2, raw);
    g.ocean_stage2 = 2;
  } else if (merc_fog && g.ocean_stage2 != 1) {
    /* (AI-assisted, far-fog) rgb = mix(previous, fog color, fog amount), as the ocean's stage 2
     * (ocean_stage2: stage 2 holds that fog stage) */
    C3D_TexEnv e2;
    C3D_TexEnvInit(&e2);
    C3D_TexEnvSrc(&e2, C3D_RGB, GPU_TEXTURE1, GPU_PREVIOUS, GPU_TEXTURE1);
    C3D_TexEnvOpRgb(&e2, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR,
                    GPU_TEVOP_RGB_SRC_ALPHA);
    C3D_TexEnvFunc(&e2, C3D_RGB, GPU_INTERPOLATE);
    commit_texenv(2, &e2, raw);
    g.ocean_stage2 = 1;
  } else if (mesh != 3 && !merc_fog && g.ocean_stage2) {
    /* (AI-assisted, j1ocean) also after the PS2 ocean's stage 2 (ocean_stage2 = 2) */
    /* (AI-assisted) stage 2 back to pass-through after the ocean (the far ring between its
     * draws) or a merc draw with fog */
    C3D_TexEnv e2;
    C3D_TexEnvInit(&e2);
    commit_texenv(2, &e2, raw);
    g.ocean_stage2 = 0;
  }

  /* (AI-assisted) the fragment operations, as citro3d's C3D_AlphaBlend / C3D_BlendingColor /
   * C3D_AlphaTest / C3D_DepthTest / C3D_EarlyDepthTest store them in its context */
  C3Di_Context* ctx = &__C3D_Context;
  C3Di_Effect e = ctx->effect;
  GPU_BLENDEQUATION eq = GPU_BLEND_ADD;
  GPU_BLENDFACTOR src = GPU_ONE, dst = GPU_ZERO;
  GPU_BLENDFACTOR src_a = GPU_ONE, dst_a = GPU_ZERO;
  switch (st->blend) {
    case CTR_BLEND_ALPHA:
      src = GPU_SRC_ALPHA;
      dst = GPU_ONE_MINUS_SRC_ALPHA;
      break;
    case CTR_BLEND_ADD:
      src = GPU_SRC_ALPHA;
      dst = GPU_ONE;
      break;
    case CTR_BLEND_SUB:
      eq = GPU_BLEND_REVERSE_SUBTRACT;
      src = GPU_SRC_ALPHA;
      dst = GPU_ONE;
      break;
    case CTR_BLEND_FIX: {
      u32 fa = st->fix >= 0x80 ? 255 : st->fix * 2;
      e.blendClr = fa << 24;
      src = GPU_CONSTANT_ALPHA;
      dst = GPU_ONE_MINUS_CONSTANT_ALPHA;
    } break;
    case CTR_BLEND_ADD_DST_A:
      src = GPU_DST_ALPHA;
      dst = GPU_ONE;
      break;
    case CTR_BLEND_ONE_ONE:
      dst = GPU_ONE;
      break;
    case CTR_BLEND_MUL:
      /* (AI-assisted) the shadows' darkening (CtrShadow): rgb = Cs * Cd, alpha = Ad */
      src = GPU_DST_COLOR;
      src_a = GPU_ZERO;
      dst_a = GPU_ONE;
      break;
    default:
      break;
  }
  e.alphaBlend = (u32)eq | ((u32)GPU_BLEND_ADD << 8) | ((u32)src << 16) | ((u32)dst << 20) |
                 ((u32)src_a << 24) | ((u32)dst_a << 28);
  e.fragOpMode = (e.fragOpMode & ~0xFF00u) | 0x0100u;
  if (st->atest == CTR_TEST_ALWAYS) {
    e.alphaTest = (u32)GPU_ALWAYS << 4;
  } else {
    int ref = st->aref * 2;
    e.alphaTest = 1u | (((u32)map_test(st->atest) & 7) << 4) | ((u32)(ref > 255 ? 255 : ref) << 8);
  }
  /* (AI-assisted) the shadow volume passes (CTR_STATE_STENCIL_INCR / DECR / CLEAR): the depth test
   * stays enabled (CLEAR: function ALWAYS) so the stencil's depth-pass operation is the one that
   * runs, and writes are controlled by the write mask only (clearing the enable bit doesn't stop
   * depth writes on the hardware): no color for NO_COLOR, no alpha for CTR_BLEND_MUL */
  const int shadow_pass =
      (st->flags & (CTR_STATE_STENCIL_INCR | CTR_STATE_STENCIL_DECR | CTR_STATE_STENCIL_CLEAR)) &&
      !g_depth16;
  u32 wmask = st->zwrite ? GPU_WRITE_ALL : GPU_WRITE_COLOR;
  if (st->flags & CTR_STATE_NO_COLOR) {
    wmask &= ~(u32)GPU_WRITE_COLOR;
  } else if (st->blend == CTR_BLEND_MUL) {
    wmask &= ~(u32)GPU_WRITE_ALPHA;
  }
  if (st->ztest == CTR_TEST_ALWAYS && !st->zwrite && !shadow_pass) {
    e.depthTest = ((u32)GPU_ALWAYS << 4) | (wmask << 8);
  } else {
    e.depthTest = 1u | (((u32)map_test(st->ztest) & 7) << 4) | (wmask << 8);
  }
  /* (AI-assisted) config.ini early_depth: fragments behind what opaque draws already covered are
   * dropped before texturing. Only for draws that write depth with the same test and that nothing
   * discards later (no blending, an alpha test that never fails): the hardware's early depth
   * data can't know about fragments dropped after it. */
  e.earlyDepth = early_depth_ok(st) != 0;
  /* (AI-assisted) sky_last: depth 0 for the sky's draws (ctr_gpu_set_depth_far) */
  if (g_depth_far) {
    e.zScale = 0;
    e.zOffset = 0;
  } else if (s_zdef_valid) {
    e.zScale = s_zscale0;
    e.zOffset = s_zoffset0;
  }
  /* (AI-assisted) cull_ties: back faces of closed tie surfaces (CTR_TCC_CULL_BACK); cull_merc:
   * merc draws the caller marked (CTR_TCC_CULL_OWN) */
  e.cullMode = (st->tcc & CTR_TCC_CULL_BACK) && (g_cull_enable || (st->tcc & CTR_TCC_CULL_OWN))
                   ? (g_cull_flip ? GPU_CULL_FRONT_CCW : GPU_CULL_BACK_CCW)
                   : GPU_CULL_NONE;
  e.earlyDepthFunc = (GPU_EARLYDEPTHFUNC)g_early_func; /* (AI-assisted) defaults: GEQUAL, 0 */
  e.earlyDepthRef = g_early_clear;
  /* (AI-assisted) the stencil marks (CTR_STATE_STENCIL_*): 1 where a STENCIL_SET draw passed the
   * depth test, STENCIL_OUT draws where it isn't 1. Off: only the enable bit changes. */
  if ((st->flags & CTR_STATE_STENCIL_SET) && !g_depth16) {
    e.stencilMode = 1u | ((u32)GPU_ALWAYS << 4) | (0xFFu << 8) | (1u << 16) | (0xFFu << 24);
    e.stencilOp = (u32)GPU_STENCIL_KEEP | ((u32)GPU_STENCIL_KEEP << 4) |
                  ((u32)GPU_STENCIL_REPLACE << 8);
  } else if ((st->flags & CTR_STATE_STENCIL_OUT) && !g_depth16) {
    e.stencilMode = 1u | ((u32)GPU_NOTEQUAL << 4) | (1u << 16) | (0xFFu << 24);
    e.stencilOp = 0;
  } else if ((st->flags & (CTR_STATE_STENCIL_INCR | CTR_STATE_STENCIL_DECR)) && !g_depth16) {
    /* (AI-assisted) shadow volumes: + / - 1 where the face passes the depth test, saturating like
     * the PC renderer's GL_INCR / GL_DECR (all front faces are drawn before the back faces, so a
     * pixel ends at max(0, front - back): where the near plane cut a front face away, no shadow
     * instead of a dark streak) */
    e.stencilMode = 1u | ((u32)GPU_ALWAYS << 4) | (0xFFu << 8) | (0xFFu << 24);
    e.stencilOp = (u32)GPU_STENCIL_KEEP | ((u32)GPU_STENCIL_KEEP << 4) |
                  ((u32)((st->flags & CTR_STATE_STENCIL_INCR) ? GPU_STENCIL_INCR : GPU_STENCIL_DECR)
                   << 8);
  } else if ((st->flags & CTR_STATE_STENCIL_CLEAR) && !g_depth16) {
    /* (AI-assisted) where the stencil isn't 0; set to 0 there (the depth test always passes; the
     * depth-fail operation zeroes too, should it run) */
    e.stencilMode = 1u | ((u32)GPU_NOTEQUAL << 4) | (0xFFu << 8) | (0xFFu << 24);
    e.stencilOp = (u32)GPU_STENCIL_KEEP | ((u32)GPU_STENCIL_ZERO << 4) |
                  ((u32)GPU_STENCIL_ZERO << 8);
  } else {
    e.stencilMode &= ~1u;
  }
  if (!raw) {
    if (memcmp(&e, &ctx->effect, sizeof(e))) {
      ctx->effect = e;
      ctx->flags |= C3DiF_Effect;
    }
    return;
  }
  /* raw: the registers C3Di_EffectBind writes for the fields that changed */
  C3Di_Effect* c = &ctx->effect;
  u32* p = raw_ptr();
  if (e.alphaTest != c->alphaTest) {
    p = raw_w(p, GPUREG_FRAGOP_ALPHA_TEST, 0xF, e.alphaTest);
  }
  if (e.stencilMode != c->stencilMode) {
    p = raw_w(p, GPUREG_STENCIL_TEST, 0xF, e.stencilMode);
  }
  if (e.stencilOp != c->stencilOp) {
    p = raw_w(p, GPUREG_STENCIL_OP, 0xF, e.stencilOp);
  }
  if (e.depthTest != c->depthTest) {
    /* (AI-assisted, micro-gpu) cmd_dedup: GAS_DELTAZ_DEPTH's byte only depends on the depth
     * function (citro3d writes it from the context's, which is the GPU's): left out when only the
     * enable or the write mask changes */
    if (!s_raw.dedup || !(s_micro & MICRO_GAS) || ((e.depthTest ^ c->depthTest) & 0x70u)) {
      p = raw_w(p, GPUREG_GAS_DELTAZ_DEPTH, 0x8,
                (u32)GPU_MAKEGASDEPTHFUNC((e.depthTest >> 4) & 7) << 24);
    } else {
      g.cur.cmd_dedup_words += 2;
    }
    p = raw_w(p, GPUREG_DEPTH_COLOR_MASK, 0xF, e.depthTest);
  }
  if (e.blendClr != c->blendClr) {
    p = raw_w(p, GPUREG_BLEND_COLOR, 0xF, e.blendClr);
  }
  if (e.alphaBlend != c->alphaBlend) {
    p = raw_w(p, GPUREG_BLEND_FUNC, 0xF, e.alphaBlend);
  }
  if (e.fragOpMode != c->fragOpMode) {
    p = raw_w(p, GPUREG_COLOR_OPERATION, 0x7, e.fragOpMode);
  }
  if (e.cullMode != c->cullMode) {
    p = raw_w(p, GPUREG_FACECULLING_CONFIG, 0xF, (u32)e.cullMode & 3);
  }
  if (e.zScale != c->zScale || e.zOffset != c->zOffset) {
    const u32 zm[2] = {e.zScale, e.zOffset}; /* (AI-assisted) sky_last */
    p = raw_n(p, GPUREG_DEPTHMAP_SCALE, zm, 2, 1);
  }
  if (e.earlyDepth != c->earlyDepth || e.earlyDepthFunc != c->earlyDepthFunc ||
      e.earlyDepthRef != c->earlyDepthRef) {
    p = raw_w(p, GPUREG_EARLYDEPTH_TEST1, 0x1, e.earlyDepth ? 1 : 0);
    p = raw_w(p, GPUREG_EARLYDEPTH_TEST2, 0xF, e.earlyDepth ? 1 : 0);
    p = raw_w(p, GPUREG_EARLYDEPTH_FUNC, 0x1, e.earlyDepthFunc);
    p = raw_w(p, GPUREG_EARLYDEPTH_DATA, 0x7, e.earlyDepthRef);
  }
  raw_end(p);
  *c = e;
  /* (AI-assisted, micro-mg) what this full raw state worked from and wrote (StateFast) */
  if (!proc && mesh != 3 && mesh != 6 && g.proc_radial < 0) {
    s_sf.valid = 1;
    s_sf.env2 = ctx->texEnv[2]; /* (AI-assisted, far-fog) */
    s_sf.stage2 = g.ocean_stage2;
    s_sf.words[0] = ((const alias_u32*)st)[1];
    s_sf.words[1] = ((const alias_u32*)st)[2];
    s_sf.words[2] = ((const alias_u32*)st)[3];
    s_sf.mesh = mesh;
    s_sf.tint = tint;
    s_sf.kind = textured ? 1u | ((u32)afull << 1) : 0u;
    state_inputs(s_sf.in);
    s_sf.tex_config_bit1 = ctx->texConfig & BIT(1);
    s_sf.env0 = ctx->texEnv[0];
    s_sf.env1 = ctx->texEnv[1];
    s_sf.effect = *c;
  }
}

/* (AI-assisted) the raw path takes every draw but the glows (procedural texture unit: citro3d) */
static inline int use_raw(const ctr_draw_state* st) {
  /* (AI-assisted, micro-mg) gpu_micro: the glows too. What citro3d sent for them (the whole
   * attribute buffer table, the fragment operations, both combiner stages, every uniform's dirty
   * scan) is what the raw draw writes only where it changes; the procedural texture unit and
   * texture unit 0's switch-off still go through citro3d (raw_sync sends them). */
  return s_raw.on && (!(st->flags & CTR_STATE_PROCTEX) || (s_micro & MICRO_RAWGLOW));
}

/* the immediate draws (triangle lists or strips from g.vbuf): program, state, then the draw */
static void draw_immediate(const ctr_draw_state* state, int prog, uint8_t* dst, u32 stride,
                           int count, GPU_Primitive_t prim) {
  use_program(prog);
  if (use_raw(state)) {
    apply_state(state, 0, 1);
    raw_vertex_buffer(raw_vbuf_pa((size_t)(dst - g.vbuf)), 0x210,
                      RAW_BUF_FLAGS1(stride, 3, 0));
    raw_sync();
    raw_draw_arrays((u32)count, (u32)prim);
  } else {
    raw_vc_flush();
    apply_state(state, 0, 0);
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, dst, stride, 3, 0x210);
    C3D_DrawArrays(prim, 0, count);
  }
  g.cur.draws++;
  g.cur.triangles += prim == GPU_TRIANGLE_STRIP ? count - 2 : count / 3;
}

/* (AI-assisted) room for bytes more in this frame's vertex buffer; a draw that doesn't fit is
 * dropped (the buffer is reset every frame) and counted */
static int vbuf_room(size_t bytes) {
  if (g.vbuf_used + bytes > VBUF_BYTES) {
    g.cur.vbuf_dropped++;
    return 0;
  }
  return 1;
}

void ctr_gpu_draw(const ctr_draw_state* state, const ctr_vertex* verts, int count) {
  SKY_HOOK(state);
  if (!g.ready || !g.in_frame || count < 3) {
    return;
  }
  size_t bytes = (size_t)count * sizeof(ctr_vertex);
  if (!vbuf_room(bytes)) {
    return;
  }
  if (!cmd_room()) {
    return;
  }
  uint8_t* dst = g.vbuf + g.vbuf_used;
  memcpy(dst, verts, bytes); /* flushed once at the end of the frame (flush_vbuf) */
  g.vbuf_used += (bytes + 15) & ~(size_t)15;
  draw_immediate(state, PROG_BASIC, dst, sizeof(ctr_vertex), count, GPU_TRIANGLES);
}

void ctr_gpu_draw_strip(const ctr_draw_state* state, const ctr_vertex* verts, int count) {
  SKY_HOOK(state);
  if (!g.ready || !g.in_frame || count < 3) {
    return;
  }
  size_t bytes = (size_t)count * sizeof(ctr_vertex);
  if (!vbuf_room(bytes) || !cmd_room()) {
    return;
  }
  uint8_t* dst = g.vbuf + g.vbuf_used;
  memcpy(dst, verts, bytes);
  g.vbuf_used += (bytes + 15) & ~(size_t)15;
  draw_immediate(state, PROG_BASIC, dst, sizeof(ctr_vertex), count, GPU_TRIANGLE_STRIP);
}

void ctr_gpu_draw_clip(const ctr_draw_state* state, const ctr_clip_vertex* verts, int count) {
  SKY_HOOK(state);
  if (!g.ready || !g.in_frame || count < 3) {
    return;
  }
  size_t bytes = (size_t)count * sizeof(ctr_clip_vertex);
  if (!vbuf_room(bytes) || !cmd_room()) {
    return;
  }
  uint8_t* dst = g.vbuf + g.vbuf_used;
  memcpy(dst, verts, bytes);
  g.vbuf_used += (bytes + 15) & ~(size_t)15;
  draw_immediate(state, PROG_CLIP, dst, sizeof(ctr_clip_vertex), count, GPU_TRIANGLES);
}

void ctr_gpu_draw_clip2(const ctr_draw_state* state, uint32_t const0, uint32_t const1,
                        const ctr_clip_vertex2* verts, int count) {
  SKY_HOOK(state);
  if (!g.ready || !g.in_frame || count < 3 || state->tex < 0 || state->tex >= MAX_TEXTURES ||
      g.textures[state->tex].used != 1) {
    return;
  }
  size_t bytes = (size_t)count * sizeof(ctr_clip_vertex2);
  if (!vbuf_room(bytes) || !cmd_room()) {
    return;
  }
  uint8_t* dst = g.vbuf + g.vbuf_used;
  memcpy(dst, verts, bytes);
  g.vbuf_used += (bytes + 15) & ~(size_t)15;
  raw_vc_flush();
  use_program(PROG_CLIP2);
  /* the texture on units 0 and 1 (binding unit 1 clears the texture cache like any bind) */
  TexSlot* slot = &g.textures[state->tex];
  if (g.proc_radial >= 0) {
    C3D_ProcTexBind(0, NULL);
    g.proc_radial = -1;
  }
  bind_texture(slot, state, 0);
  C3D_TexBind(1, &slot->tex);
  /* rgb = (tex0 * const0 + tex1 * const1) * vertex alpha (GS units: x2 each); the color is
   * added. The constants doubled here keep 8 bits of precision in the first stages. */
  u32 c2[2];
  for (int i = 0; i < 2; i++) {
    const u32 c = i ? const1 : const0;
    u32 out = 0;
    for (int sh = 0; sh < 32; sh += 8) {
      const u32 v = ((c >> sh) & 0xff) * 2;
      out |= (v > 255 ? 255 : v) << sh;
    }
    c2[i] = out;
  }
  C3D_TexEnv* e0 = C3D_GetTexEnv(0);
  C3D_TexEnvInit(e0);
  C3D_TexEnvSrc(e0, C3D_Both, GPU_TEXTURE0, GPU_CONSTANT, 0);
  C3D_TexEnvFunc(e0, C3D_Both, GPU_MODULATE);
  C3D_TexEnvColor(e0, c2[0]);
  C3D_TexEnv* e1 = C3D_GetTexEnv(1);
  C3D_TexEnvInit(e1);
  C3D_TexEnvSrc(e1, C3D_Both, GPU_TEXTURE1, GPU_CONSTANT, GPU_PREVIOUS);
  C3D_TexEnvFunc(e1, C3D_Both, GPU_MULTIPLY_ADD);
  C3D_TexEnvColor(e1, c2[1]);
  C3D_TexEnv* e2 = C3D_GetTexEnv(2);
  C3D_TexEnvInit(e2);
  C3D_TexEnvSrc(e2, C3D_RGB, GPU_PREVIOUS, GPU_PRIMARY_COLOR, 0);
  C3D_TexEnvOpRgb(e2, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA, 0);
  C3D_TexEnvFunc(e2, C3D_RGB, GPU_MODULATE);
  C3D_TexEnvScale(e2, C3D_RGB, GPU_TEVSCALE_2);
  C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ZERO);
  C3D_AlphaTest(false, GPU_ALWAYS, 0);
  if (state->ztest == CTR_TEST_ALWAYS) {
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
  } else {
    /* (AI-assisted) sky_last: depth tested like the roof (fragment depth 0 with
     * ctr_gpu_set_depth_far: only where no opaque draw wrote depth) */
    C3D_DepthTest(true, map_test(state->ztest), GPU_WRITE_COLOR);
  }
  if (g_depth_far) {
    __C3D_Context.effect.zScale = 0;
    __C3D_Context.effect.zOffset = 0;
  } else if (s_zdef_valid) {
    __C3D_Context.effect.zScale = s_zscale0;
    __C3D_Context.effect.zOffset = s_zoffset0;
  }
  /* (AI-assisted) early_depth = sky: the clouds too; else off, with the configured function and
   * clear value (the defaults GEQUAL, 0 as before: EARLYDEPTH_DATA is the clear value) */
  const int early = g_early_sky && g_depth_far && state->ztest == CTR_TEST_GEQUAL;
  C3D_EarlyDepthTest(early != 0, (GPU_EARLYDEPTHFUNC)g_early_func, g_early_clear);
  g.cur.early_depth_draws += early;
  C3D_BufInfo* buf = C3D_GetBufInfo();
  BufInfo_Init(buf);
  BufInfo_Add(buf, dst, sizeof(ctr_clip_vertex2), 4, 0x3210);
  C3D_DrawArrays(GPU_TRIANGLES, 0, count);
  g.cur.draws++;
  g.cur.triangles += count / 3;
  /* back to what the other draws expect: stage 2 passes through, unit 1 has the fog ramp.
   * (AI-assisted) Through C3D_GetTexEnv: initializing the stage citro3d had just sent (e2) only
   * changed its context, so the GPU kept the clouds' stage 2 for every draw after the sky (rgb
   * times twice the vertex alpha: darker particles), and citro3d's context was wrong about it. */
  C3D_TexEnvInit(C3D_GetTexEnv(2));
  if (g.fog_tex_valid) {
    C3D_TexBind(1, &g.fog_tex);
  }
  g.last_state_valid = 0;
}

/* (AI-assisted) the vertices of the last ctr_gpu_draw_quads in g.vbuf (ctr_gpu_draw_quads_again):
 * its first quad's offset, the quads written (one run: n quads are 64 n bytes, no padding between
 * its batches) and the frame */
static size_t s_lastq_off;
static int s_lastq_quads, s_lastq_frame = -1;

/* the draw of n <= QUAD_MAX quads whose vertices are at dst in g.vbuf */
static void draw_quads_at(const ctr_draw_state* state, uint8_t* dst, int n) {
  use_program(PROG_BASIC);
  if (use_raw(state)) {
    apply_state(state, 0, 1);
    raw_vertex_buffer(raw_vbuf_pa((size_t)(dst - g.vbuf)), 0x210,
                      RAW_BUF_FLAGS1(sizeof(ctr_vertex), 3, 0));
    raw_sync();
    if (!s_raw.quad_pa) {
      s_raw.quad_pa = osConvertVirtToPhys(g.quad_indices);
    }
    raw_draw_elements(s_raw.quad_pa, (u32)n * 6);
  } else {
    raw_vc_flush();
    apply_state(state, 0, 0);
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, dst, sizeof(ctr_vertex), 3, 0x210);
    /* indices are relative to the buffer: 0 1 3 3 1 2, + 4 per quad */
    C3D_DrawElements(GPU_TRIANGLES, n * 6, C3D_UNSIGNED_SHORT, g.quad_indices);
  }
  g.cur.draws++;
  g.cur.triangles += n * 2;
}

void ctr_gpu_draw_quads(const ctr_draw_state* state, const ctr_vertex* verts, int quad_count) {
  SKY_HOOK(state);
  s_lastq_quads = 0;
  if (!g.ready || !g.in_frame) {
    return;
  }
  s_lastq_off = g.vbuf_used;
  s_lastq_frame = g.frame_no;
  while (quad_count > 0) {
    const int n = quad_count > QUAD_MAX ? QUAD_MAX : quad_count;
    const size_t bytes = (size_t)n * 4 * sizeof(ctr_vertex);
    if (!vbuf_room(bytes) || !cmd_room()) {
      return;
    }
    uint8_t* dst = g.vbuf + g.vbuf_used;
    memcpy(dst, verts, bytes);
    g.vbuf_used += (bytes + 15) & ~(size_t)15;
    s_lastq_quads += n;
    draw_quads_at(state, dst, n);
    verts += 4 * n;
    quad_count -= n;
  }
}

void ctr_gpu_draw_quads_again(const ctr_draw_state* state) {
  SKY_HOOK(state);
  if (!g.ready || !g.in_frame || s_lastq_frame != g.frame_no) {
    return;
  }
  uint8_t* dst = g.vbuf + s_lastq_off;
  for (int left = s_lastq_quads; left > 0;) {
    const int n = left > QUAD_MAX ? QUAD_MAX : left;
    if (!cmd_room()) {
      return;
    }
    draw_quads_at(state, dst, n);
    dst += (size_t)n * 4 * sizeof(ctr_vertex);
    left -= n;
  }
}

static void apply_state(const ctr_draw_state* st, int mesh, int raw) {
  apply_state_tint(st, mesh, 0xffffffff, raw);
}

/* ---------------- (AI-assisted) the PS2 ocean (ctr_gpu_ocean_full_reserve / draw) ---------------- */

static void fog_ramp_color(uint8_t r, uint8_t gr, uint8_t b);

int ctr_gpu_ocean_full_reserve(int max_verts, int max_indices, ctr_ocean_full_vertex** verts,
                          uint16_t** indices) {
  SKY_HOOK_ANY(); /* (AI-assisted) sky_last: before the reservation */
  _Static_assert(sizeof(ctr_ocean_full_vertex) == 40, "ctr_ocean_full.v.pica's vertex layout");
  if (!g.ready || !g.in_frame || g.ocean_full_open || max_verts <= 0 || max_indices <= 0) {
    return 0;
  }
  const size_t vbytes = ((size_t)max_verts * sizeof(ctr_ocean_full_vertex) + 15) & ~(size_t)15;
  const size_t ibytes = ((size_t)max_indices * 2 + 15) & ~(size_t)15;
  if (g.vbuf_used + vbytes + ibytes > VBUF_BYTES) {
    return 0;
  }
  g.ocean_full_voff = g.vbuf_used;
  g.ocean_full_ioff = g.vbuf_used + vbytes;
  g.ocean_full_max_verts = max_verts;
  g.ocean_full_max_indices = max_indices;
  g.ocean_full_open = 1;
  *verts = (ctr_ocean_full_vertex*)(g.vbuf + g.ocean_full_voff);
  *indices = (uint16_t*)(g.vbuf + g.ocean_full_ioff);
  return 1;
}

static void ocean_bind_tex2_wrap(int handle, int clamp);

static void ocean_full_draw(const ctr_draw_state* state, int vert_count, int index_count,
                            uint8_t fog_r, uint8_t fog_g, uint8_t fog_b, int prog, int env_tex) {
  if (!g.ocean_full_open) {
    return;
  }
  g.ocean_full_open = 0;
  if (vert_count > g.ocean_full_max_verts || index_count > g.ocean_full_max_indices) {
    return; /* (the caller wrote past the reservation) */
  }
  if (!g.in_frame || vert_count < 3 || index_count < 3) {
    return; /* nothing taken from g.vbuf */
  }
  /* the indices right after the vertices used: the rest of the reservation stays free */
  const size_t voff = g.ocean_full_voff;
  const size_t ioff = voff + (((size_t)vert_count * sizeof(ctr_ocean_full_vertex) + 15) & ~(size_t)15);
  if (ioff != g.ocean_full_ioff) {
    memmove(g.vbuf + ioff, g.vbuf + g.ocean_full_ioff, (size_t)index_count * 2);
  }
  g.vbuf_used = ioff + (((size_t)index_count * 2 + 15) & ~(size_t)15);
  if (!cmd_room()) {
    return;
  }
  fog_ramp_color(fog_r, fog_g, fog_b);
  use_program(prog);
  /* (AI-assisted, j1ocean) mesh = 6: the level meshes' combiners (texture alpha 0xff = 1, fog from
   * texcoord1), the GS's TCC, and with env_tex the sky reflection in stage 2 (texture unit 2, the
   * clip program's texcoord2) */
  const int env = env_tex >= 0 && env_tex < MAX_TEXTURES && g.textures[env_tex].used == 1 &&
                  prog == PROG_OCEAN_FULL_CLIP;
  if (env) {
    ocean_bind_tex2_wrap(env_tex, 1);
  }
  const int attribs = prog == PROG_OCEAN_FULL_CLIP ? 4 : 3;
  const u32 perm = prog == PROG_OCEAN_FULL_CLIP ? 0x3210 : 0x210;
  if (use_raw(state)) {
    apply_state_tint(state, 6, (uint32_t)env, 1);
    raw_vertex_buffer(raw_vbuf_pa(voff), perm,
                      RAW_BUF_FLAGS1(sizeof(ctr_ocean_full_vertex), attribs, 0));
    raw_sync();
    raw_draw_elements(raw_vbuf_pa(ioff), (u32)index_count);
  } else {
    raw_vc_flush();
    apply_state_tint(state, 6, (uint32_t)env, 0);
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, g.vbuf + voff, sizeof(ctr_ocean_full_vertex), attribs, perm);
    C3D_DrawElements(GPU_TRIANGLES, index_count, C3D_UNSIGNED_SHORT, g.vbuf + ioff);
  }
  g.cur.draws++;
  g.cur.triangles += index_count / 3;
}

void ctr_gpu_ocean_full_draw(const ctr_draw_state* state, int vert_count, int index_count,
                             uint8_t fog_r, uint8_t fog_g, uint8_t fog_b) {
  ocean_full_draw(state, vert_count, index_count, fog_r, fog_g, fog_b, PROG_OCEAN_FULL, -1);
}

void ctr_gpu_ocean_full_draw_clip(const ctr_draw_state* state, int vert_count, int index_count,
                                  uint8_t fog_r, uint8_t fog_g, uint8_t fog_b) {
  ocean_full_draw(state, vert_count, index_count, fog_r, fog_g, fog_b, PROG_OCEAN_FULL_CLIP, -1);
}

void ctr_gpu_ocean_full_draw_env(const ctr_draw_state* state, int vert_count, int index_count,
                                 uint8_t fog_r, uint8_t fog_g, uint8_t fog_b, int env_tex) {
  ocean_full_draw(state, vert_count, index_count, fog_r, fog_g, fog_b, PROG_OCEAN_FULL_CLIP,
                  env_tex);
}

/* ---------------- (AI-assisted) indexed clip space draws (the shadow volumes) ----------------
 * ctr_gpu_clip_reserve / ctr_gpu_clip_draw_ranges: the ctr_clip_vertex layout and PROG_CLIP of
 * ctr_gpu_draw_clip (the sky's, hardware proven), indexed like the ocean's reserved draws. */

/* the open reservation: vertices and indices at these g.vbuf offsets */
static struct {
  int open;
  size_t voff, ioff;
  int max_verts, max_indices;
} s_clipres;

static void clip_reserve_reset(void) {
  s_clipres.open = 0; /* a reservation never drawn (frame begin) */
}

int ctr_gpu_clip_reserve(int max_verts, int max_indices, ctr_clip_vertex** verts,
                         uint16_t** indices) {
  SKY_HOOK_ANY(); /* (AI-assisted) sky_last: before the reservation */
  if (!g.ready || !g.in_frame || s_clipres.open || g.ocean_full_open || max_verts <= 0 ||
      max_verts > 65536 || max_indices <= 0) {
    return 0;
  }
  const size_t vbytes = ((size_t)max_verts * sizeof(ctr_clip_vertex) + 15) & ~(size_t)15;
  const size_t ibytes = ((size_t)max_indices * 2 + 15) & ~(size_t)15;
  if (g.vbuf_used + vbytes + ibytes > VBUF_BYTES) {
    g.cur.vbuf_dropped++;
    return 0;
  }
  s_clipres.voff = g.vbuf_used;
  s_clipres.ioff = g.vbuf_used + vbytes; /* indices above the vertices (and the buffer base) */
  s_clipres.max_verts = max_verts;
  s_clipres.max_indices = max_indices;
  s_clipres.open = 1;
  *verts = (ctr_clip_vertex*)(g.vbuf + s_clipres.voff);
  *indices = (uint16_t*)(g.vbuf + s_clipres.ioff);
  return 1;
}

void ctr_gpu_clip_draw_ranges(const ctr_draw_state* states, const int* first, const int* count,
                              int n, int vert_count, int index_count) {
  if (!s_clipres.open) {
    return;
  }
  s_clipres.open = 0;
  if (!g.in_frame || vert_count < 3 || vert_count > s_clipres.max_verts || index_count < 3 ||
      index_count > s_clipres.max_indices) {
    return; /* nothing kept */
  }
  int draws = 0;
  for (int i = 0; i < n; i++) {
    if (first[i] < 0 || count[i] < 0 || first[i] + count[i] > index_count) {
      return; /* (a range outside what was written: none of them) */
    }
    draws += count[i] >= 3;
  }
  if (!draws) {
    return;
  }
  /* the indices right after the vertices used: the rest of the room goes back */
  const size_t voff = s_clipres.voff;
  const size_t ioff = voff + (((size_t)vert_count * sizeof(ctr_clip_vertex) + 15) & ~(size_t)15);
  if (ioff != s_clipres.ioff) {
    memmove(g.vbuf + ioff, g.vbuf + s_clipres.ioff, (size_t)index_count * 2);
  }
  g.vbuf_used = ioff + (((size_t)index_count * 2 + 15) & ~(size_t)15);
  /* all of the draws or none: room for each one's commands (cmd_room's test, times draws) */
  if (gpuCmdBufSize - gpuCmdBufOffset < CMD_RESERVE_WORDS + (u32)draws * CMD_DRAW_WORDS) {
    g.cur.dropped_draws += draws;
    return;
  }
  use_program(PROG_CLIP);
  int stencil = 0;
  for (int i = 0; i < n; i++) {
    if (count[i] < 3) {
      continue;
    }
    const ctr_draw_state* st = &states[i];
    const u32 nidx = (u32)(count[i] - count[i] % 3);
    const size_t at = ioff + 2u * (size_t)first[i];
    stencil |= st->flags & (CTR_STATE_STENCIL_INCR | CTR_STATE_STENCIL_DECR |
                            CTR_STATE_STENCIL_CLEAR);
    if (use_raw(st)) {
      apply_state(st, 0, 1);
      raw_vertex_buffer(raw_vbuf_pa(voff), 0x210, RAW_BUF_FLAGS1(sizeof(ctr_clip_vertex), 3, 0));
      raw_sync();
      raw_draw_elements(raw_vbuf_pa(at), nidx);
    } else {
      raw_vc_flush();
      apply_state(st, 0, 0);
      c3d_vertex_buffer(raw_vbuf_pa(voff), g.vbuf + voff, sizeof(ctr_clip_vertex), 3, 0x210);
      C3D_DrawElements(GPU_TRIANGLES, (int)nidx, C3D_UNSIGNED_SHORT, g.vbuf + at);
    }
    g.cur.draws++;
    g.cur.triangles += (int)nidx / 3;
  }
  if (stencil) {
    /* the stencil test off again (citro3d sends the effect at the next draw), for draws that
     * don't set the fragment operations themselves (the clouds), like ctr_gpu_ocean_end */
    C3Di_Context* ctx = &__C3D_Context;
    if (ctx->effect.stencilMode & 1) {
      ctx->effect.stencilMode &= ~1u;
      ctx->flags |= C3DiF_Effect;
    }
    g.last_state_valid = 0;
  }
}

/* ---------------- the screen as a texture ---------------- */

static int copy_screen_body(float x0, float x1, int part);

/* (AI-assisted) measurement only: the CPU time of the screen copies (the command list sent so
 * far, the copy queued or, without the pipeline, waited for), ctr_gpu_take_screen_copy_ms */
static u64 s_copy_ticks;
double ctr_gpu_take_screen_copy_ms(void) {
  const double ms = (double)s_copy_ticks / (SYSCLOCK_ARM11 / 1000.0);
  s_copy_ticks = 0;
  return ms;
}

static int copy_screen(float x0, float x1, int part) {
  SKY_HOOK_ANY(); /* (AI-assisted) sky_last: the copy shows what's drawn so far */
  const u64 t0 = svcGetSystemTick();
  const int tex = copy_screen_body(x0, x1, part);
  s_copy_ticks += svcGetSystemTick() - t0;
  return tex;
}

int ctr_gpu_copy_screen(void) {
  return copy_screen(-1.0f, 1.0f, 0);
}

int ctr_gpu_copy_screen_part(float x0, float x1) {
  return copy_screen(x0, x1, 1);
}

static int copy_screen_body(float x0, float x1, int part) {
  if (!g.ready || !g.in_frame) {
    return -1;
  }
  if (g.screen_tex < 0) {
    /* 256x512 RGBA8 (or RGB565 with color16) in linear memory (512 / 256 KB, made once): the
     * 240x400 color buffer fits, with the same tiled layout (see write_screenshot) */
    const int slot = reserve_tex_slot();
    if (slot < 0) {
      return -1;
    }
    TexSlot* t = &g.textures[slot];
    const GPU_TEXCOLOR fmt = g_color16 ? GPU_RGB565 : GPU_RGBA8;
    const u32 bytes = 256u * 512u * (g_color16 ? 2u : 4u);
    ctr_linear_lock();
    /* (AI-assisted) distort_copy = rows: in VRAM if there's room now (the copy and the distort
     * draws then stay in VRAM), leaving the reserve free (C3D_TexInitVRAM calls libctru's
     * vramAlloc, not safe_vram_alloc). Not with block32 (its copy goes through the scratch). */
    bool in_vram = part && !g_block32 && (u32)vramSpaceFree() >= bytes + VRAM_RESERVE &&
                   C3D_TexInitVRAM(&t->tex, 256, 512, fmt);
    const bool ok = in_vram || (linear_room_for_tex(256, 512, 0) && C3D_TexInit(&t->tex, 256, 512, fmt));
    ctr_linear_unlock();
    if (!ok) {
      release_tex_slot(slot);
      return -1;
    }
    /* (AI-assisted) cleared and flushed once: the copy never writes columns 240-255 and rows
     * 400-511 (black there instead of old memory: distort sprites at the screen edges sample
     * it), and no dirty CPU cache line left from this memory's previous use can be written back
     * over the GPU's copy later (hardware only: Azahar has no CPU cache). In VRAM: not touched
     * by the CPU (the GPU's copies only); ctr_gpu_screen_uv keeps the sampling half a texel
     * inside the copied 240x400, so what the rest holds is never read. */
    if (!in_vram) {
      memset(t->tex.data, 0, t->tex.size);
      cache_flush_now(t->tex.data, t->tex.size);
    }
    g.screen_tex_vram = in_vram;
    printf("[ctr] distorter screen copy: texture in %s\n", in_vram ? "VRAM" : "linear memory");
    t->pool = -1;
    t->levels = 1;
    ctr_linear_lock();
    t->used = 1;
    ctr_linear_unlock();
    g.screen_tex = slot;
  }
  /* One GX copy after the draws so far (C3D_SyncTextureCopy splits the command list, which flushes
   * the framebuffer): 50 rows of 8x8 tiles, 30 tiles (7680 bytes; 3840 in RGB565) each in the
   * color buffer, 32 in the texture (a 512 / 256 byte gap). In 16 byte units. */
  if (g_block32) {
    /* (AI-assisted) 32x32 blocks: untiled into the scratch buffer, then tiled again in 8x8 tiles
     * into the texture (256 wide like the render target: the same rows, see ctr_gpu_screen_uv) */
    if (s_pipeline) {
      pipeline_submit();
    } else {
      raw_vc_flush(); /* (AI-assisted) as sync_texture_copy: no post-vertex cache left open */
      flush_vbuf();
      C3D_FrameSplit(GX_CMDLIST_FLUSH);
    }
    const u32 fmt = g_color16 ? GX_TRANSFER_FMT_RGB565 : GX_TRANSFER_FMT_RGBA8;
    GX_DisplayTransfer((u32*)g.top->frameBuf.colorBuf, GX_BUFFER_DIM(B32_W, B32_H),
                       (u32*)s_b32_scratch, GX_BUFFER_DIM(B32_W, B32_H),
                       GX_TRANSFER_IN_FORMAT(fmt) | GX_TRANSFER_OUT_FORMAT(fmt) |
                           GX_TRANSFER_BLOCK32);
    GX_DisplayTransfer((u32*)s_b32_scratch, GX_BUFFER_DIM(B32_W, B32_H),
                       (u32*)g.textures[g.screen_tex].tex.data, GX_BUFFER_DIM(B32_W, B32_H),
                       GX_TRANSFER_IN_FORMAT(fmt) | GX_TRANSFER_OUT_FORMAT(fmt) |
                           GX_TRANSFER_OUT_TILED(1));
  } else {
    const u32 row = g_color16 ? 3840 : 7680, gap = g_color16 ? 256 : 512;
    /* (AI-assisted) distort_copy = rows: tile row k holds screen columns 8k..8k+7 (the color
     * buffer is 240 wide, x along its rows); a texel of margin each side for the filter */
    int r0 = 0, r1 = 49;
    if (part) {
      const float c0 = x0 < -1.0f ? -1.0f : (x0 > 1.0f ? 1.0f : x0);
      const float c1 = x1 < -1.0f ? -1.0f : (x1 > 1.0f ? 1.0f : x1);
      int p0 = (int)((c0 + 1.0f) * 0.5f * 399.0f) - 1, p1 = (int)((c1 + 1.0f) * 0.5f * 399.0f) + 2;
      p0 = p0 < 0 ? 0 : (p0 > 399 ? 399 : p0);
      p1 = p1 < 0 ? 0 : (p1 > 399 ? 399 : p1);
      if (p1 < p0) {
        p1 = p0;
      }
      r0 = p0 >> 3;
      r1 = p1 >> 3;
    }
    sync_texture_copy((u32*)((u8*)g.top->frameBuf.colorBuf + (u32)r0 * row),
                      GX_BUFFER_DIM(row / 16, 0),
                      (u32*)((u8*)g.textures[g.screen_tex].tex.data + (u32)r0 * (row + gap)),
                      GX_BUFFER_DIM(row / 16, gap / 16), row * (u32)(r1 - r0 + 1), 8);
  }
  /* the GPU's texture cache may hold the old texels */
  g.bound_tex = NULL;
  return g.screen_tex;
}

void ctr_gpu_screen_copy_release(void) {
  if (g.ready && g.screen_tex >= 0) {
    ctr_gpu_tex_delete(g.screen_tex); /* (freed once the GPU is done with it) */
    if (g.bound_tex == &g.textures[g.screen_tex].tex) {
      g.bound_tex = NULL;
    }
    g.screen_tex = -1;
  }
}

void ctr_gpu_screen_uv(float x, float y, float* s, float* t) {
  /* color buffer column = screen y from the bottom (240), row = screen x from the left (400)
   * (see write_screenshot); texture s along the columns (256), t along the rows (512). The GPU
   * samples t = 0 from the LAST row in memory (as swizzle_rgba8), so memory row r is at
   * t = 1 - r / 512: the copied screen is t = 1 (left) down to 112 / 512 (right); below that are
   * rows the copy never writes (black). */
  /* (clamped half a texel inside the copy: distort sprites at the screen edges reach past it) */
  const float cx = x < -1.0f ? -1.0f : (x > 1.0f ? 1.0f : x);
  const float cy = y < -1.0f ? -1.0f : (y > 1.0f ? 1.0f : y);
  *s = 0.5f / 256.0f + (cy + 1.0f) * 0.5f * (239.0f / 256.0f);
  *t = 1.0f - (0.5f + (float)top_row0()) / 512.0f - (cx + 1.0f) * 0.5f * (399.0f / 512.0f);
}

/* ---------------- (AI-assisted) render to texture (impostors, CtrImpostor) ----------------
 * The texture is an RGBA8 C3D_Tex and is also the color buffer: the GPU's render targets use the
 * textures' tiled layout (8x8 tiles, Morton order inside). Its logical pixels are laid out like
 * the top screen's (the color buffer is 240 wide, the screen's x along its rows): logical x =
 * buffer row, logical y = buffer column, so the draws keep the screen's clip transform and only
 * the viewport changes. A tile is drawn with the framebuffer moved to its band of rows (the band's
 * start at a multiple of 16 rows: 16 KB steps for 256 columns, far beyond the 8-byte alignment the
 * framebuffer address registers need), so the D16 depth buffer needs only max_tile_w rows. The
 * switch goes through citro3d's context (C3D_SetFrameBuf: at the next update citro3d flushes the
 * framebuffer the draws used and binds the new one), which keeps the raw draws' invariant
 * (citro3d's context describes the GPU).
 * Memory (Oct 17, hardware safety): both in VRAM when there is room (made at renderer init, before
 * the level texture pools fill it: C3D_TexInitVRAM, the usual render to texture place on the 3DS;
 * the depth in the other VRAM bank when it fits), else linear memory (the GPU renders to FCRAM as
 * well, slower). Nothing here touches the texels with the CPU or the transfer engine's memory fill
 * (that only fills VRAM on hardware): every tile is cleared by a draw (CtrImpostor::capture's
 * first quad, alpha 0 and depth 0) before it is drawn into or sampled. */
#define MAX_RT 4
static struct {
  int tex1; /* texture slot + 1 (0: free) */
  int lw, lh, max_w;
  void* depth;   /* D16, max_w rows x lh columns */
  int depth_vram; /* (AI-assisted) depth is in VRAM (vramFree) */
  u32 dead_frame; /* ctr_gpu_rt_delete: the depth buffer is freed 3 frames later */
} s_rt[MAX_RT];
static struct {
  int active;
  C3D_FrameBuf fb;
  ctr_gpu_hook_fn hook;
  void* hook_arg;
  int early, early_sky;
} s_rtc;

static void rt_reap(void) {
  for (int i = 0; i < MAX_RT; i++) {
    if (!s_rt[i].tex1 && s_rt[i].depth && (u32)g.frame_no - s_rt[i].dead_frame > 3) {
      ctr_linear_lock();
      if (s_rt[i].depth_vram) {
        vramFree(s_rt[i].depth);
      } else {
        linearFree(s_rt[i].depth);
      }
      ctr_linear_unlock();
      s_rt[i].depth = NULL;
      s_rt[i].depth_vram = 0;
    }
  }
}

int ctr_gpu_rt_create(int lw, int lh, int max_tile_w) {
  rt_reap();
  if (!g.ready || lw < 16 || lh < 16 || lw > 1024 || lh > 1024 || (lw & (lw - 1)) ||
      (lh & (lh - 1)) || max_tile_w < 16 || (max_tile_w & 15) || max_tile_w > lw) {
    return -1;
  }
  int i = 0;
  while (i < MAX_RT && (s_rt[i].tex1 || s_rt[i].depth)) {
    i++;
  }
  if (i == MAX_RT) {
    return -1;
  }
  const int slot = reserve_tex_slot();
  if (slot < 0) {
    return -1;
  }
  TexSlot* t = &g.textures[slot];
  const size_t color_bytes = (size_t)lw * (size_t)lh * 4u;
  const size_t depth_bytes = (size_t)max_tile_w * (size_t)lh * 2u;
  ctr_linear_lock();
  /* the texture: lh wide (buffer columns), lw high (buffer rows). VRAM first (leaving the reserve:
   * C3D_TexInitVRAM calls libctru's vramAlloc, not safe_vram_alloc) */
  int in_vram = (size_t)vramSpaceFree() >= color_bytes + depth_bytes + VRAM_RESERVE &&
                C3D_TexInitVRAM(&t->tex, (u16)lh, (u16)lw, GPU_RGBA8);
  int ok = in_vram ||
           (linear_room_for_tex(lh, lw, 0) && C3D_TexInit(&t->tex, (u16)lh, (u16)lw, GPU_RGBA8));
  void* depth = NULL;
  int depth_vram = 0;
  if (ok && in_vram) {
    /* the other bank than the color (the GPU reads both per pixel) */
    if ((size_t)vramSpaceFree() >= depth_bytes + VRAM_RESERVE) {
      depth = vramAllocAt(depth_bytes, vram_bank(t->tex.data) ? VRAM_ALLOC_A : VRAM_ALLOC_B);
    }
    if (!depth) {
      depth = vramAlloc(depth_bytes);
    }
    depth_vram = depth != NULL;
  }
  if (ok && !depth) {
    depth = linearAlloc(depth_bytes);
  }
  if (ok && !depth) {
    C3D_TexDelete(&t->tex);
    ok = 0;
  }
  ctr_linear_unlock();
  if (!ok) {
    release_tex_slot(slot);
    return -1;
  }
  if (!in_vram) {
    /* no dirty cache line from the memory's last use may land over what the GPU writes */
    cache_flush_now(t->tex.data, t->tex.size);
  }
  if (!depth_vram) {
    cache_flush_now(depth, (u32)depth_bytes);
  }
  t->pool = -1;
  t->levels = 1;
  ctr_linear_lock();
  t->used = 1;
  ctr_linear_unlock();
  s_rt[i].tex1 = slot + 1;
  s_rt[i].lw = lw;
  s_rt[i].lh = lh;
  s_rt[i].max_w = max_tile_w;
  s_rt[i].depth = depth;
  s_rt[i].depth_vram = depth_vram;
  printf("[ctr] render to texture: %dx%d RGBA8 in %s + D16 %dx%d in %s (%u KB)\n", lw, lh,
         in_vram ? (vram_bank(t->tex.data) ? "VRAM B" : "VRAM A") : "linear memory", max_tile_w,
         lh, depth_vram ? (vram_bank(depth) ? "VRAM B" : "VRAM A") : "linear memory",
         (unsigned)((color_bytes + depth_bytes) / 1024u));
  return slot;
}

static int rt_find(int tex) {
  for (int i = 0; tex >= 0 && i < MAX_RT; i++) {
    if (s_rt[i].tex1 == tex + 1) {
      return i;
    }
  }
  return -1;
}

void ctr_gpu_rt_delete(int tex) {
  const int i = rt_find(tex);
  if (i < 0) {
    return;
  }
  if (s_rtc.active) {
    ctr_gpu_rt_end();
  }
  ctr_gpu_tex_delete(tex);
  s_rt[i].tex1 = 0;
  s_rt[i].dead_frame = (u32)g.frame_no;
}

int ctr_gpu_rt_begin(int tex, int x, int y, int w, int h) {
  const int i = rt_find(tex);
  if (i < 0 || !g.ready || !g.in_frame || g.textures[tex].used != 1) {
    return 0;
  }
  const int rows = (w + 7) & ~7;
  if (x < 0 || y < 0 || w < 1 || h < 1 || (x & 15) || rows > s_rt[i].max_w || x + rows > s_rt[i].lw ||
      y + h > s_rt[i].lh) {
    return 0;
  }
  raw_vc_flush();
  if (!s_rtc.active) {
    /* a pending sky_last hook waits for the screen (it never draws into the tile) */
    s_rtc.hook = s_sky_hook;
    s_rtc.hook_arg = s_sky_hook_arg;
    s_sky_hook = NULL;
    s_rtc.early = g_early_depth;
    s_rtc.early_sky = g_early_sky;
    g_early_depth = 0;
    g_early_sky = 0;
  }
  C3D_Tex* t = &g.textures[tex].tex;
  C3D_FrameBuf* fb = &s_rtc.fb;
  memset(fb, 0, sizeof(*fb));
  C3D_FrameBufAttrib(fb, (u16)s_rt[i].lh, (u16)rows, false);
  C3D_FrameBufColor(fb, (u8*)t->data + (size_t)x * (size_t)s_rt[i].lh * 4u, GPU_RB_RGBA8);
  C3D_FrameBufDepth(fb, s_rt[i].depth, GPU_RB_DEPTH16);
  C3D_SetFrameBuf(fb);
  C3D_SetViewport((u32)y, 0, (u32)h, (u32)w);
  C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)y, 0, (u32)(y + h), (u32)w);
  s_rtc.active = 1;
  return 1;
}

void ctr_gpu_rt_end(void) {
  if (!s_rtc.active) {
    return;
  }
  raw_vc_flush();
  C3D_SetFrameBuf(&g.top->frameBuf);
  set_top_viewport();
  C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
  s_sky_hook = s_rtc.hook;
  s_sky_hook_arg = s_rtc.hook_arg;
  g_early_depth = s_rtc.early;
  g_early_sky = s_rtc.early_sky;
  /* the texture cache may hold the tile's old texels: the next bind clears it */
  g.bound_tex = NULL;
  s_tex_dirty = 1;
  s_rtc.active = 0;
}

void ctr_gpu_rt_uv(int tex, float lx, float ly, float* s, float* t) {
  const int i = rt_find(tex);
  if (i < 0) {
    *s = *t = 0.0f;
    return;
  }
  /* logical y = buffer column (s), logical x = buffer row r, sampled at t = 1 - r / rows (the GPU
   * samples t = 0 from the last row in memory, see ctr_gpu_screen_uv) */
  *s = ly / (float)s_rt[i].lh;
  *t = 1.0f - lx / (float)s_rt[i].lw;
}

int ctr_gpu_mesh_peek(int mesh, const void** verts, int* vertex_count, const uint16_t** indices,
                      int* index_count) {
  if (mesh < 0 || mesh >= MAX_MESHES || g.meshes[mesh].used != 1 || !g.meshes[mesh].fill_vbytes) {
    return 0;
  }
  *verts = g.meshes[mesh].verts;
  *vertex_count = (int)(g.meshes[mesh].fill_vbytes / 16u);
  *indices = g.meshes[mesh].indices;
  *index_count = (int)(g.meshes[mesh].fill_ibytes / 2u);
  return 1;
}

/* ---------------- static meshes ---------------- */

/* Also called from the level loader thread: always in linear memory (see tex_alloc). */
int ctr_gpu_tex_create_tiled(int w, int h, int format, const void* data, int size) {
  if (!g.ready || w < 8 || h < 8 || w > 1024 || h > 1024) {
    return -1;
  }
  const int slot = reserve_tex_slot();
  if (slot < 0) {
    return -1;
  }
  C3D_Tex* tex = &g.textures[slot].tex;
  int on_vram;
  if (format == 1 && g_rgba4_as_rgba8) {
    /* RGBA4 texels expanded to RGBA8 (same tiled order; GPU_RGBA8 is stored A, B, G, R) */
    uint8_t* dst = (uint8_t*)tex_alloc(tex, w, h, GPU_RGBA8, &on_vram, 0);
    if (!dst) {
      release_tex_slot(slot);
      return -1;
    }
    int n = size / 2;
    if (n > w * h) {
      n = w * h;
    }
    if (s_asm_tex) {
      ctr_rgba4_to_rgba8((uint32_t*)dst, (const uint8_t*)data, n); /* byte k = nibble k * 17 */
    } else {
      const uint8_t* src = (const uint8_t*)data; /* (AI-assisted) asm_tex = off: bytes */
      for (int i = 0; i < n; i++) {
        const uint16_t v = (uint16_t)(src[2 * i] | (src[2 * i + 1] << 8));
        dst[4 * i + 0] = (uint8_t)((v & 0xf) * 17);
        dst[4 * i + 1] = (uint8_t)(((v >> 4) & 0xf) * 17);
        dst[4 * i + 2] = (uint8_t)(((v >> 8) & 0xf) * 17);
        dst[4 * i + 3] = (uint8_t)(((v >> 12) & 0xf) * 17);
      }
    }
    if (!tex_commit(tex, dst, on_vram)) {
      release_tex_slot(slot);
      return -1;
    }
    g.textures[slot].pool = -1;
    g.textures[slot].levels = 1;
    g.textures[slot].used = 1;
    return slot;
  }
  void* dst = tex_alloc(tex, w, h, format == 0 ? GPU_RGB565 : GPU_RGBA4, &on_vram, 0);
  if (!dst) {
    release_tex_slot(slot);
    return -1;
  }
  memcpy(dst, data, (size_t)size < tex->size ? (size_t)size : tex->size);
  if (!tex_commit(tex, dst, on_vram)) {
    release_tex_slot(slot);
    return -1;
  }
  g.textures[slot].pool = -1;
  g.textures[slot].levels = 1;
  g.textures[slot].used = 1;
  return slot;
}

/* (AI-assisted) begin/end: the slot stays reserved (used = 3) while the loader fills it */
int ctr_gpu_mesh_begin(int vertex_count, int index_count, void** verts, uint16_t** indices) {
  if (!g.ready) {
    return -1;
  }
  const int slot = reserve_mesh_slot();
  if (slot < 0) {
    return -1;
  }
  size_t vbytes = (size_t)vertex_count * 16;
  size_t ibytes = (size_t)index_count * 2;
  ctr_linear_lock();
  void* v = linearAlloc(vbytes);
  uint16_t* ix = (uint16_t*)linearAlloc(ibytes);
  if (!v || !ix) {
    if (v) {
      linearFree(v);
    }
    if (ix) {
      linearFree(ix);
    }
    g.meshes[slot].used = 0;
    ctr_linear_unlock();
    return -1;
  }
  ctr_linear_unlock();
  g.meshes[slot].verts = v;
  g.meshes[slot].ext = 0; /* (AI-assisted, streaming) */
  g.meshes[slot].indices = ix;
  g.meshes[slot].fill_vbytes = (u32)vbytes;
  g.meshes[slot].fill_ibytes = (u32)ibytes;
  *verts = v;
  *indices = ix;
  return slot;
}

/* (AI-assisted, streaming) see ctr_gpu.h */
void ctr_gpu_set_ext_free(void (*fn)(void* mem)) {
  s_ext_free = fn;
}

void* ctr_gpu_linear_block(unsigned int bytes) {
  ctr_linear_lock();
  void* p = linearAlloc(bytes);
  ctr_linear_unlock();
  return p;
}

void ctr_gpu_linear_block_free(void* p) {
  if (!p) {
    return;
  }
  ctr_linear_lock();
  linearFree(p);
  ctr_linear_unlock();
}

unsigned int ctr_gpu_linear_free(void) {
  return (unsigned int)linearSpaceFree();
}

int ctr_gpu_mesh_begin_ext(void* mem, int vertex_count, int index_count, void** verts,
                           uint16_t** indices) {
  if (!g.ready || !mem) {
    return -1;
  }
  const int slot = reserve_mesh_slot();
  if (slot < 0) {
    return -1;
  }
  g.meshes[slot].verts = mem;
  g.meshes[slot].indices = (uint16_t*)((uint8_t*)mem + ctr_gpu_ext_index_offset(vertex_count));
  g.meshes[slot].verts_alt = NULL;
  g.meshes[slot].vbytes = 0;
  g.meshes[slot].ext = 1;
  g.meshes[slot].fill_vbytes = (u32)vertex_count * 16u;
  g.meshes[slot].fill_ibytes = (u32)index_count * 2u;
  *verts = mem;
  *indices = g.meshes[slot].indices;
  return slot;
}

int ctr_gpu_mesh_end(int mesh, int ok) {
  if (mesh < 0 || mesh >= MAX_MESHES || g.meshes[mesh].used != 3) {
    return -1;
  }
  if (!ok && g.meshes[mesh].ext) {
    /* (AI-assisted, streaming) the caller's memory back to it */
    ctr_linear_lock();
    if (s_ext_free) {
      s_ext_free(g.meshes[mesh].verts);
    }
    g.meshes[mesh].ext = 0;
    g.meshes[mesh].verts = NULL;
    g.meshes[mesh].indices = NULL;
    g.meshes[mesh].used = 0;
    ctr_linear_unlock();
    return -1;
  }
  if (!ok) {
    ctr_linear_lock();
    linearFree(g.meshes[mesh].verts);
    linearFree(g.meshes[mesh].indices);
    g.meshes[mesh].verts = NULL;
    g.meshes[mesh].indices = NULL;
    g.meshes[mesh].used = 0;
    ctr_linear_unlock();
    return -1;
  }
  cache_flush_now(g.meshes[mesh].verts, g.meshes[mesh].fill_vbytes);
  cache_flush_now(g.meshes[mesh].indices, g.meshes[mesh].fill_ibytes);
  g.meshes[mesh].verts_pa = osConvertVirtToPhys(g.meshes[mesh].verts);
  g.meshes[mesh].indices_pa = osConvertVirtToPhys(g.meshes[mesh].indices);
  g.meshes[mesh].used = 1;
  return mesh;
}

int ctr_gpu_mesh_create(const void* verts, int vertex_count, const uint16_t* indices,
                        int index_count) {
  void* v;
  uint16_t* ix;
  const int mesh = ctr_gpu_mesh_begin(vertex_count, index_count, &v, &ix);
  if (mesh < 0) {
    return -1;
  }
  memcpy(v, verts, (size_t)vertex_count * 16);
  memcpy(ix, indices, (size_t)index_count * 2);
  return ctr_gpu_mesh_end(mesh, 1);
}

void ctr_gpu_mesh_delete(int mesh) {
  if (mesh < 0 || mesh >= MAX_MESHES) {
    return;
  }
  ctr_linear_lock();
  if (g.meshes[mesh].used == 1) {
    g.meshes[mesh].used = 2;
    g.pending_mesh_delete[g.pending_mesh_delete_count++] = mesh;
  }
  ctr_linear_unlock();
}

/* (AI-assisted) gl_to_pica * clip (clip row-major), in C3D_Mtx layout (components reversed): the
 * level chunks' and merc models' matrices. The generic product sums from +0.0f in k order; with
 * the fixed g.gl_to_pica (rows (0 1 0 0), (-1 0 0 0), (0 0 -1/2 -1/2), (0 0 0 1); zeros of either
 * sign) each element is one or two of clip's with the same rounding: a zero product leaves a
 * nonzero sum as it is and the +0.0f sum at +0 (so the 0.0f + stays: it makes a -0 element +0, and
 * float24 has a sign bit), x * 1 and x * -1 are exact, and the two products of row 2 are added in
 * the same order (VFPv2's VMLA rounds the product and the sum separately: no fused add). Clip
 * matrices with an infinity or NaN take the generic product (0 * inf is NaN there). */
static int s_gl_to_pica_fixed;
static void gl_to_pica_check(void) {
  static const float k[4][4] = {{0, 1, 0, 0}, {-1, 0, 0, 0}, {0, 0, -0.5f, -0.5f}, {0, 0, 0, 1}};
  int ok = 1;
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      ok &= g.gl_to_pica.r[r].c[3 - c] == k[r][c]; /* (== : -0 and +0 alike) */
    }
  }
  s_gl_to_pica_fixed = ok;
  printf("[ctr] gl_to_pica: %s\n", ok ? "the fixed form" : "not the fixed form: generic products");
}
static void gl_to_pica_mul(const float clip[16], C3D_Mtx* m) {
  u32 nonfinite = 0;
  for (int i = 0; i < 16; i++) {
    u32 w;
    memcpy(&w, &clip[i], 4);
    nonfinite |= (w & 0x7f800000u) == 0x7f800000u;
  }
  if (s_gl_to_pica_fixed && !nonfinite) {
    for (int c = 0; c < 4; c++) {
      m->r[0].c[3 - c] = 0.0f + clip[4 + c];
      m->r[1].c[3 - c] = 0.0f - clip[c];
      m->r[2].c[3 - c] = (0.0f + -0.5f * clip[8 + c]) + -0.5f * clip[12 + c];
      m->r[3].c[3 - c] = 0.0f + clip[12 + c];
    }
    return;
  }
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      float acc = 0.0f;
      for (int k = 0; k < 4; k++) {
        acc += g.gl_to_pica.r[r].c[3 - k] * clip[4 * k + c];
      }
      m->r[r].c[3 - c] = acc;
    }
  }
}

void ctr_gpu_prepare_mesh_matrix(const float clip[16], ctr_mesh_matrix* out) {
  /* final = gl_to_pica * clip, in C3D_Mtx layout (components reversed) */
  C3D_Mtx m;
  gl_to_pica_mul(clip, &m);
  _Static_assert(sizeof(C3D_Mtx) == offsetof(ctr_mesh_matrix, f24), "matrix layout");
  memcpy(out->m, &m, sizeof(m));
  /* (AI-assisted) and as float24 uniform words for the raw draws (12 words instead of 16) */
  for (int r = 0; r < 4; r++) {
    raw_pack_f24(out->m + 4 * r, out->f24 + 3 * r);
  }
  static uint32_t s_next_id;
  out->id = ++s_next_id ? s_next_id : ++s_next_id; /* never 0 */
  out->fog_gen = 0;
}

/* (AI-assisted) shader_opt (ctr_mesh_opt.v.pica): the fog rows clip[4], clip[5] of a matrix (C3D_Mtx
 * layout, C3D_FVec order w z y x): both fogs are affine in clip w (g.fog0: -(game fog) =
 * fog0.x * w + fog0.z, draw distance fog = fog0.y * w + fog0.w) and w is dph(position, row 3) */
static void mesh_fog_rows(const float* m, float out[8]) {
  const float* w = m + 12;
  for (int k = 0; k < 4; k++) {
    out[k] = g.fog0[1] * w[k];     /* clip[4]: the draw distance fog */
    out[4 + k] = g.fog0[0] * w[k]; /* clip[5]: -(game fog) */
  }
  out[0] += g.fog0[3];
  out[4] += g.fog0[2];
}

/* (AI-assisted) the mesh programs' scales (x: texture coordinates, 1/1024 or c3l v15 atlases'
 * 1/32768). (micro-gpu) raw: written straight away, the same words C3D_UpdateUniforms writes for
 * it (config, then w z y x as f32), with citro3d's copy of the uniform set and not marked: before,
 * every change made the draw's raw_sync run all of C3Di_UpdateContext (~2000 instructions) for
 * this one vector, and static_cmdlists couldn't record a stretch with one (a citro3d update).
 * When citro3d has uniforms to send anyway (unif_pending), it sends this one with them. */
static void mesh_scales_set(int atlas, int raw) {
  const int loc = g.ps->uloc_scales;
  const float sx = atlas ? 1.0f / 32768.0f : 1.0f / 1024.0f;
  if (!raw || !(s_micro & MICRO_ATLAS) || s_raw.unif_pending || loc < 0 ||
      C3D_FVUnifDirty[GPU_VERTEX_SHADER][loc]) {
    C3D_FVUnifSet(GPU_VERTEX_SHADER, loc, sx, 1.0f / 255.0f, 1.0f, 1.0f / 127.0f);
    raw_note_unif();
    return;
  }
  C3D_FVec* u = &C3D_FVUnif[GPU_VERTEX_SHADER][loc];
  u->x = sx;
  u->y = 1.0f / 255.0f;
  u->z = 1.0f;
  u->w = 1.0f / 127.0f;
  raw_end(raw_unif(raw_ptr(), loc, (const u32*)u, 4, 1)); /* (C3D_FVec: w z y x in memory) */
}

void ctr_gpu_draw_mesh_prepared(const ctr_draw_state* state, const ctr_mesh_matrix* matrix,
                                int mesh, int first_index, int index_count) {
  SKY_HOOK(state);
  if (!g.ready || !g.in_frame || mesh < 0 || mesh >= MAX_MESHES || g.meshes[mesh].used != 1 ||
      index_count < 3) {
    return;
  }
  if (!cmd_room()) {
    return;
  }
  /* (AI-assisted) fog_alpha: draws whose vertex alpha is 0x80 everywhere (CTR_TCC_ALPHA80) take
   * the fog in the color's alpha (mesh 5, no fog ramp and texcoord1); the fog stage's cost is the
   * same with no fog (fog_stage_skip is for the ramp's texture unit) */
  const int fa = g_fog_alpha && g.ps->fa_ok && (state->tcc & CTR_TCC_ALPHA80);
  /* (AI-assisted) shader_opt with the game fog off: the programs with only the draw distance fog */
  const int df = g.ps->df_ok && g.fog_df;
  use_program(fa ? (df ? PROG_MESH_DF_FA : PROG_MESH_FA) : (df ? PROG_MESH_DF : PROG_MESH));
  /* (AI-assisted) c3l v15 atlas draws (CTR_TCC_ST_ATLAS): texture coordinates in 1/32768. The
   * draws are sorted by texture, so an atlas's draws come one after another: two writes per run */
  {
    const int atlas = (state->tcc & CTR_TCC_ST_ATLAS) ? 1 : 0;
    if (atlas != g.mesh_st_atlas) {
      g.mesh_st_atlas = atlas;
      mesh_scales_set(atlas, use_raw(state));
    }
  }
  const MeshSlot* ms = &g.meshes[mesh];
  /* (AI-assisted) fog_stage_skip: mesh 4 = a level mesh without the fog stage */
  const int mesh_mode = fa ? 5 : (g.mesh_nofog ? 4 : 1);
  g.cur.nofog_draws += g.mesh_nofog && !fa;
  g.cur.fog_alpha_draws += fa;
  if (use_raw(state)) {
    /* (AI-assisted) raw: state, vertex buffer, then (after citro3d's pending updates) the matrix
     * as float24 and the draw */
    apply_state(state, mesh_mode, 1);
    raw_vertex_buffer(ms->verts_pa, 0x210, RAW_BUF_FLAGS1(16, 3, 0));
    raw_sync();
    if (!g.last_mesh_valid || g.last_clip_id != matrix->id) {
      g.last_clip_id = matrix->id;
      if (g.ps->opt) {
        /* (AI-assisted) shader_opt: the matrix and its fog rows in one burst (the draw distance
         * fog's only with the game fog off: the df programs don't read the other). The rows are
         * made once per matrix and fog (fog_gen): a chunk's matrix is uploaded again at every
         * switch between chunks (the draws are sorted by texture) */
        ctr_mesh_matrix* mm = (ctr_mesh_matrix*)matrix;
        if (mm->fog_gen != g.fog_gen) {
          float rows[8];
          mesh_fog_rows(matrix->m, rows);
          raw_pack_f24(rows, mm->f24 + 12);
          raw_pack_f24(rows + 4, mm->f24 + 15);
          mm->fog_gen = g.fog_gen;
        }
        if (s_rec.on && s_rec.n < s_rec.max) {
          /* (AI-assisted, gpu-rt) static_cmdlists: where this upload goes when copied again */
          s_rec.slots[2 * s_rec.n] = (u32)(raw_ptr() - s_rec.p0);
          s_rec.slots[2 * s_rec.n + 1] = (u32)s_rec.call | ((u32)(df ? 15 : 18) << 24);
          s_rec.n++;
        } else if (s_rec.on) {
          s_rec.bad = 1;
        }
        raw_end(raw_unif(raw_ptr(), g.ps->uloc_clip, matrix->f24, df ? 15 : 18, 0));
      } else {
        if (s_rec.on && s_rec.n < s_rec.max) {
          s_rec.slots[2 * s_rec.n] = (u32)(raw_ptr() - s_rec.p0);
          s_rec.slots[2 * s_rec.n + 1] = (u32)s_rec.call | (12u << 24);
          s_rec.n++;
        } else if (s_rec.on) {
          s_rec.bad = 1;
        }
        raw_end(raw_unif(raw_ptr(), g.ps->uloc_clip, matrix->f24, 12, 0));
      }
      g.last_mesh_valid = 1;
    }
    raw_draw_elements(ms->indices_pa + 2u * (u32)first_index, (u32)index_count);
  } else {
    s_rec.bad = 1; /* (AI-assisted, gpu-rt) static_cmdlists: citro3d's draw (not while on) */
    raw_vc_flush();
    /* consecutive draws of a level chunk share the matrix and the vertex buffer */
    if (!g.last_mesh_valid || g.last_clip_id != matrix->id) {
      g.last_clip_id = matrix->id;
      C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g.ps->uloc_clip, (const C3D_Mtx*)matrix->m);
      if (g.ps->opt) {
        float r[8];
        mesh_fog_rows(matrix->m, r);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, g.ps->uloc_clip + 4, r[3], r[2], r[1], r[0]);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, g.ps->uloc_clip + 5, r[7], r[6], r[5], r[4]);
      }
    }
    g.last_mesh_valid = 1;
    c3d_vertex_buffer(ms->verts_pa, ms->verts, 16, 3, 0x210);
    apply_state(state, mesh_mode, 0);
    C3D_DrawElements(GPU_TRIANGLES, index_count, C3D_UNSIGNED_SHORT, ms->indices + first_index);
  }
  g.cur.draws++;
  g.cur.triangles += index_count / 3;
}

/* (AI-assisted) impostors: like ctr_gpu_draw_mesh_prepared, with this frame's vertices and indices
 * (ctr_gpu_stream_alloc; c3l::Vertex layout) instead of a mesh's */
void ctr_gpu_draw_mesh_stream(const ctr_draw_state* state, const ctr_mesh_matrix* matrix,
                              const void* verts, const uint16_t* indices, int index_count) {
  SKY_HOOK(state);
  if (!g.ready || !g.in_frame || !verts || !indices || index_count < 3) {
    return;
  }
  if (!cmd_room()) {
    return;
  }
  const int fa = g_fog_alpha && g.ps->fa_ok && (state->tcc & CTR_TCC_ALPHA80);
  const int df = g.ps->df_ok && g.fog_df;
  use_program(fa ? (df ? PROG_MESH_DF_FA : PROG_MESH_FA) : (df ? PROG_MESH_DF : PROG_MESH));
  /* (AI-assisted, titlemove) the texture coordinate scale as ctr_gpu_draw_mesh_prepared sets it:
   * a card drawn after a level draw of an atlas (1/32768) kept that scale, so its texels were
   * sampled from one corner (garbage or alpha-killed: far objects missing until they came back as
   * geometry) */
  {
    const int atlas = (state->tcc & CTR_TCC_ST_ATLAS) ? 1 : 0;
    if (atlas != g.mesh_st_atlas) {
      g.mesh_st_atlas = atlas;
      mesh_scales_set(atlas, use_raw(state));
    }
  }
  const int mesh_mode = fa ? 5 : (g.mesh_nofog ? 4 : 1);
  g.cur.nofog_draws += g.mesh_nofog && !fa;
  g.cur.fog_alpha_draws += fa;
  const u32 vpa = osConvertVirtToPhys(verts), ipa = osConvertVirtToPhys(indices);
  if (use_raw(state)) {
    apply_state(state, mesh_mode, 1);
    raw_vertex_buffer(vpa, 0x210, RAW_BUF_FLAGS1(16, 3, 0));
    raw_sync();
    if (!g.last_mesh_valid || g.last_clip_id != matrix->id) {
      g.last_clip_id = matrix->id;
      if (g.ps->opt) {
        ctr_mesh_matrix* mm = (ctr_mesh_matrix*)matrix;
        if (mm->fog_gen != g.fog_gen) {
          float rows[8];
          mesh_fog_rows(matrix->m, rows);
          raw_pack_f24(rows, mm->f24 + 12);
          raw_pack_f24(rows + 4, mm->f24 + 15);
          mm->fog_gen = g.fog_gen;
        }
        raw_end(raw_unif(raw_ptr(), g.ps->uloc_clip, matrix->f24, df ? 15 : 18, 0));
      } else {
        raw_end(raw_unif(raw_ptr(), g.ps->uloc_clip, matrix->f24, 12, 0));
      }
      g.last_mesh_valid = 1;
    }
    raw_draw_elements(ipa, (u32)index_count);
  } else {
    raw_vc_flush();
    if (!g.last_mesh_valid || g.last_clip_id != matrix->id) {
      g.last_clip_id = matrix->id;
      C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g.ps->uloc_clip, (const C3D_Mtx*)matrix->m);
      if (g.ps->opt) {
        float r[8];
        mesh_fog_rows(matrix->m, r);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, g.ps->uloc_clip + 4, r[3], r[2], r[1], r[0]);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, g.ps->uloc_clip + 5, r[7], r[6], r[5], r[4]);
      }
    }
    g.last_mesh_valid = 1;
    c3d_vertex_buffer(vpa, verts, 16, 3, 0x210);
    apply_state(state, mesh_mode, 0);
    C3D_DrawElements(GPU_TRIANGLES, index_count, C3D_UNSIGNED_SHORT, indices);
  }
  g.cur.draws++;
  g.cur.triangles += index_count / 3;
}

void ctr_gpu_set_mesh_nofog(int on) {
  g.mesh_nofog = on ? 1 : 0;
}

void ctr_gpu_set_fog_alpha(int on) {
  g_fog_alpha = on ? 1 : 0;
}

void ctr_gpu_set_tex1_gating(int on) {
  g_tex1_gating = on ? 1 : 0;
}

int ctr_gpu_fog_alpha_ready(void) {
  return g.ps->fa_ok;
}

void ctr_gpu_draw_mesh(const ctr_draw_state* state, const float clip[16], int mesh,
                       int first_index, int index_count) {
  ctr_mesh_matrix m;
  ctr_gpu_prepare_mesh_matrix(clip, &m);
  ctr_gpu_draw_mesh_prepared(state, &m, mesh, first_index, index_count);
}

/* ---------------- (AI-assisted, gpu-rt) config.ini static_cmdlists ----------------
 * The level renderer's draws of a frame are mostly the same as the frame before's: the same
 * draws in the same order, with only the chunk matrices new (the camera moved). Their raw command
 * words (state changes, vertex buffer, matrix uploads, draw templates) are then the same except
 * the matrices' uniform data. ctr_gpu_draw_mesh_calls takes a pass's draws in one list and splits
 * it into stretches of draws that run one program with nothing but raw words between them (a
 * draw that switches the program, runs the sky hook (sky_last), goes through citro3d or draws
 * nothing is drawn as usual and starts a new stretch). A stretch drawn alike in two frames in a
 * row is recorded: its words, where each matrix upload is, the GPU state the trackers (g, s_raw,
 * citro3d's context) had before and after, the settings, and the statistics it adds. While a later
 * frame's stretch has the same draws (state, texture memory, mesh buffers, counts, which draws
 * share a matrix), the same state before and the same settings, its words are copied in, each
 * matrix upload written again with this frame's matrix (and fog rows), and the trackers set to the
 * state after it: exactly what drawing it one by one writes (static_cmdlists = compare checks every
 * copy against that and counts mismatches).
 * The copies come from one block of linear memory (CL_ARENA_BYTES, taken when the setting is first
 * on: no fragmentation of what level loads need), filled from the start again (every recording
 * dropped) when it is full. The GPU reads the frame's command buffer as always: nothing in the
 * block has a GPU lifetime (a pipelined frame, frame_overlap2, level unloads change nothing).
 * GPUREG_CMDBUF_JUMP / ADDR (3dbrew: "kick" registers; buffers 16-byte aligned) could run the block
 * without the copy, but their call / return use isn't documented and a list the GPU doesn't finish
 * hangs the console: left for a pica_bench test. */
#define CL_ARENA_BYTES (512u * 1024u)
#define CL_MAX_SEGS 8
typedef struct {
  ctr_draw_state last_state;
  int last_state_mesh;
  uint32_t last_tint;
  int last_state_valid;
  const C3D_Tex* bound_tex;
  const void* bound_data;
  u32 bound_param;
  int cur_prog;
  const void* ps;
  int last_mesh_valid;
  int unit1_off, ocean_stage2, proc_radial;
  int env_valid, last_skin_valid, skin_bl_valid;
  C3Di_Effect effect;
  C3D_TexEnv tex_env[3];
  C3D_Tex* tex0;
  u32 tex_config;
  C3D_BufInfo buf;
  u32 ctx_flags;
  const void* program;
  int prim, voff0;
  const C3D_Tex* tex0_ctx;
  u32 tex0_fmt;
  u32 tex0_regs[5]; /* (AI-assisted, micro-gpu) */
  int tex0_regs_ok;
  int mesh_st_atlas; /* (AI-assisted, micro-gpu) the mesh scales the GPU has */
  int vc_dirty, vs_changed, unif_pending;
  int tex_dirty;
  /* the texture unit 0 binding is one no draw of the stretch uses (a texture with its own memory:
   * sky, ocean, eyes; its texels may swap copies every frame): bound_* are 1 / 1 / 0 here, and
   * (out) the stretch didn't bind: the binding before it stays */
  int bound_other;
} ClTrack;
typedef struct {
  int fog_alpha, tex1_gating, dedup, dedup_draw, vcache_reuse, mip_mode, tex_clear_mode;
  int early_depth, early_sky, depth_far, early_func, cull_enable, cull_flip;
  int depth16, fog_tex_valid, fog_df, zdef_valid, proctex;
  u32 lod_bias, early_clear, fog_abgr, zscale0, zoffset0;
  const void* ps;
  u32 micro; /* (AI-assisted, micro-gpu) s_micro */
} ClSet;
typedef struct {
  ctr_draw_state st;
  u32 tex_data, tex_dim, tex_fmt;
  u32 vpa, ipa, count, flags; /* flags: nofog | the matrix of the draw before (uploaded) << 1 */
  u32 pad;
} ClCall;
_Static_assert(sizeof(ClCall) == 48, "ClCall compared as 12 words");
#define CL_STATS(X)                                                                       \
  X(draws) X(triangles) X(tex_binds) X(early_depth_draws) X(proctex_draws) X(nofog_draws) \
  X(tex_clears_skipped) X(unit1_toggles) X(fog_alpha_draws)                                \
  X(cmd_dedup_words)
enum {
#define CL_ENUM(f) CL_S_##f,
  CL_STATS(CL_ENUM) CL_S_COUNT
#undef CL_ENUM
};
typedef struct {
  int valid;
  u32 gen;       /* the arena's generation it was recorded in */
  u32 last_hash; /* the stretch as the last frame drew it (two alike: record) */
  int ncalls, nslots, nwords, last_up;
  ClCall* calls;
  u32* slots;
  u32* words;
  u32 bytes;
  ClTrack in, out;
  ClSet set;
  int stats[CL_S_COUNT];
} ClSeg;
struct ctr_cmd_cache {
  ClSeg seg[CL_MAX_SEGS];
};
static struct {
  int mode;
  uint8_t* arena;
  u32 used, gen;
  u32* scratch; /* compare mode: the copied words; recording: the slots */
  u32 scratch_words;
  /* statistics since the last line */
  u32 segs, replays, records, unlike, normal_calls, replay_calls, words, slots, resets, compares,
      mismatches, frame0;
  int logged_mismatches, logged_why;
  u32 why[5]; /* not alike: arena restarted, draw count, settings, state before, the draws */
  /* (AI-assisted, micro-gpu) draws drawn alone (heads of a stretch), by why: not a raw level
   * pool draw (its texture: own memory / no mesh or count / citro3d), after such a draw, another
   * program, the sky hook pending */
  u32 head_why[6];
  const C3D_Tex* bound0_tex;
  const void* bound0_data;
  u32 bound0_param;
} s_cl = {.mode = 1};

void ctr_gpu_set_static_cmdlists(int mode) {
  s_cl.mode = mode < 0 ? 0 : (mode > 2 ? 2 : mode);
  if (s_cl.mode && !s_cl.arena && g.ready) {
    ctr_linear_lock();
    s_cl.arena = (uint8_t*)linearAlloc(CL_ARENA_BYTES);
    ctr_linear_unlock();
    if (!s_cl.arena) {
      printf("[ctr] static_cmdlists: no linear memory for its %u KB, off\n", CL_ARENA_BYTES / 1024);
    }
  }
  if (!s_cl.arena) {
    s_cl.mode = 0;
  }
}

static u32* cl_scratch(u32 words) {
  if (s_cl.scratch_words < words) {
    free(s_cl.scratch);
    s_cl.scratch_words = words < 4096 ? 4096 : words;
    s_cl.scratch = (u32*)malloc(s_cl.scratch_words * 4);
    if (!s_cl.scratch) {
      s_cl.scratch_words = 0;
    }
  }
  return s_cl.scratch;
}

static void cl_track_save(ClTrack* t) {
  memset(t, 0, sizeof(*t));
  const C3Di_Context* ctx = &__C3D_Context;
  t->last_state = g.last_state;
  t->last_state_mesh = g.last_state_mesh;
  t->last_tint = g.last_tint;
  t->last_state_valid = g.last_state_valid;
  t->bound_tex = g.bound_tex;
  t->bound_data = g.bound_data;
  t->bound_param = g.bound_param;
  t->cur_prog = g.cur_prog;
  t->ps = g.ps;
  t->last_mesh_valid = g.last_mesh_valid;
  t->unit1_off = g.unit1_off;
  t->ocean_stage2 = g.ocean_stage2;
  t->proc_radial = g.proc_radial;
  t->env_valid = g.env_valid;
  t->last_skin_valid = g.last_skin_valid;
  t->skin_bl_valid = g.skin_bl_valid;
  memcpy(&t->effect, &ctx->effect, sizeof(t->effect));
  memcpy(t->tex_env, ctx->texEnv, sizeof(t->tex_env));
  t->tex0 = ctx->tex[0];
  t->tex_config = ctx->texConfig;
  memcpy(&t->buf, &ctx->bufInfo, sizeof(t->buf));
  t->ctx_flags = ctx->flags;
  t->program = ctx->program;
  t->prim = s_raw.prim;
  t->voff0 = s_raw.voff0;
  t->tex0_ctx = s_raw.tex0_ctx;
  t->tex0_fmt = s_raw.tex0_fmt;
  if (s_raw.tex0_ctx && s_raw.tex0_regs_ok) {
    memcpy(t->tex0_regs, s_raw.tex0_regs, sizeof(t->tex0_regs)); /* (AI-assisted, micro-gpu) */
    t->tex0_regs_ok = 1;
  }
  t->mesh_st_atlas = g.mesh_st_atlas;
  t->vc_dirty = s_raw.vc_dirty;
  t->vs_changed = s_raw.vs_changed;
  t->unif_pending = s_raw.unif_pending;
  t->tex_dirty = s_tex_dirty;
  /* (the level draws only bind pool textures: cl_plain) */
  if (g.bound_tex && ((const TexSlot*)g.bound_tex)->pool < 0) {
    t->bound_tex = (const C3D_Tex*)1;
    t->bound_data = (const void*)1;
    t->bound_param = 0;
    t->bound_other = 1;
  }
}

static void cl_track_load(const ClTrack* t) {
  C3Di_Context* ctx = &__C3D_Context;
  g.last_state = t->last_state;
  g.last_state_mesh = t->last_state_mesh;
  g.last_tint = t->last_tint;
  g.last_state_valid = t->last_state_valid;
  if (!t->bound_other) {
    g.bound_tex = t->bound_tex;
    g.bound_data = t->bound_data;
    g.bound_param = t->bound_param;
  }
  g.cur_prog = t->cur_prog;
  g.last_mesh_valid = t->last_mesh_valid;
  g.unit1_off = t->unit1_off;
  g.ocean_stage2 = t->ocean_stage2;
  g.proc_radial = t->proc_radial;
  g.env_valid = t->env_valid;
  g.last_skin_valid = t->last_skin_valid;
  g.skin_bl_valid = t->skin_bl_valid;
  memcpy(&ctx->effect, &t->effect, sizeof(t->effect));
  memcpy(ctx->texEnv, t->tex_env, sizeof(t->tex_env));
  ctx->tex[0] = t->tex0;
  ctx->texConfig = t->tex_config;
  memcpy(&ctx->bufInfo, &t->buf, sizeof(t->buf));
  ctx->flags = t->ctx_flags;
  s_raw.prim = t->prim;
  s_raw.voff0 = t->voff0;
  s_raw.tex0_ctx = t->tex0_ctx;
  s_raw.tex0_fmt = t->tex0_fmt;
  memcpy(s_raw.tex0_regs, t->tex0_regs, sizeof(s_raw.tex0_regs)); /* (AI-assisted, micro-gpu) */
  s_raw.tex0_regs_ok = t->tex0_regs_ok;
  if (g.mesh_st_atlas != t->mesh_st_atlas && is_mesh_prog(g.cur_prog) && g.ps->uloc_scales >= 0) {
    /* (AI-assisted, micro-gpu) the stretch changed the scales (raw): citro3d's copy too */
    C3D_FVUnif[GPU_VERTEX_SHADER][g.ps->uloc_scales].x =
        t->mesh_st_atlas ? 1.0f / 32768.0f : 1.0f / 1024.0f;
  }
  g.mesh_st_atlas = t->mesh_st_atlas;
  s_raw.vc_dirty = t->vc_dirty;
  s_raw.vs_changed = t->vs_changed;
  s_raw.unif_pending = t->unif_pending;
  s_tex_dirty = t->tex_dirty;
}

static void cl_set_save(ClSet* t) {
  memset(t, 0, sizeof(*t));
  t->fog_alpha = g_fog_alpha;
  t->tex1_gating = g_tex1_gating;
  t->dedup = s_raw.dedup;
  t->dedup_draw = s_raw.dedup_draw;
  t->vcache_reuse = s_raw.vcache_reuse;
  t->mip_mode = g_mip_mode;
  t->tex_clear_mode = s_tex_clear_mode;
  t->early_depth = g_early_depth;
  t->early_sky = g_early_sky;
  t->depth_far = g_depth_far;
  t->early_func = g_early_func;
  t->cull_enable = g_cull_enable;
  t->cull_flip = g_cull_flip;
  t->depth16 = g_depth16;
  t->fog_tex_valid = g.fog_tex_valid;
  t->fog_df = g.fog_df;
  t->zdef_valid = s_zdef_valid;
  t->proctex = g_proctex;
  t->lod_bias = s_lod_bias;
  t->early_clear = g_early_clear;
  t->fog_abgr = g.fog_abgr;
  t->zscale0 = s_zscale0;
  t->zoffset0 = s_zoffset0;
  t->ps = g.ps;
  t->micro = s_micro;
}

static inline void cl_stats(int* out) {
#define CL_GET(f) out[CL_S_##f] = (int)g.cur.f;
  CL_STATS(CL_GET)
#undef CL_GET
}

/* the program the draw takes (as ctr_gpu_draw_mesh_prepared) */
static int cl_prog(const ctr_mesh_call* c) {
  const int fa = g_fog_alpha && g.ps->fa_ok && (c->state->tcc & CTR_TCC_ALPHA80);
  const int df = g.ps->df_ok && g.fog_df;
  return fa ? (df ? PROG_MESH_DF_FA : PROG_MESH_FA) : (df ? PROG_MESH_DF : PROG_MESH);
}
/* a draw that writes raw words only (inside a stretch) */
static int cl_plain(const ctr_mesh_call* c) {
  const int t = tex_res(c->state->tex);
  return c->index_count >= 3 && c->mesh >= 0 && c->mesh < MAX_MESHES &&
         g.meshes[c->mesh].used == 1 && use_raw(c->state) &&
         (t < 0 || t >= MAX_TEXTURES || g.textures[t].used != 1 || g.textures[t].pool >= 0);
}

static void cl_rec_call(const ctr_mesh_call* c, const ctr_mesh_call* prev, ClCall* r) {
  memset(r, 0, sizeof(*r));
  r->st = *c->state;
  const int t = tex_res(c->state->tex);
  if (t >= 0 && t < MAX_TEXTURES && g.textures[t].used == 1) {
    const TexSlot* ts = &g.textures[t];
    r->tex_data = (u32)ts->tex.data;
    r->tex_dim = ts->tex.dim;
    r->tex_fmt = (u32)ts->tex.fmt | ((u32)ts->levels << 8) | ((u32)(ts->radial >= 0) << 16) |
                 ((u32)ts->pool << 20);
  }
  const MeshSlot* ms = &g.meshes[c->mesh];
  r->vpa = ms->verts_pa;
  r->ipa = ms->indices_pa + 2u * (u32)c->first_index;
  r->count = (u32)c->index_count;
  const int same = prev ? prev->matrix == c->matrix
                        : (g.last_mesh_valid && g.last_clip_id == c->matrix->id);
  r->flags = (c->nofog ? 1u : 0u) | (same ? 2u : 0u);
}

static inline u32 cl_hash(u32 h, const void* p, u32 bytes) {
  const alias_u32* w = (const alias_u32*)p;
  for (u32 i = 0; i < bytes / 4; i++) {
    h = (h ^ w[i]) * 16777619u;
  }
  return h;
}

static void cl_draw(const ctr_mesh_call* c, int a, int b) {
  for (int i = a; i < b; i++) {
    g.mesh_nofog = c[i].nofog ? 1 : 0;
    ctr_gpu_draw_mesh_prepared(c[i].state, c[i].matrix, c[i].mesh, c[i].first_index,
                               c[i].index_count);
  }
}

/* the stretch's words at p, its matrices this frame's */
static void cl_replay(const ClSeg* sg, const ctr_mesh_call* c, int a, int b) {
  u32* p = raw_ptr();
  memcpy(p, sg->words, (size_t)sg->nwords * 4);
  for (int k = 0; k < sg->nslots; k++) {
    ctr_mesh_matrix* m = (ctr_mesh_matrix*)c[a + (int)(sg->slots[2 * k + 1] & 0xFFFFFF)].matrix;
    const u32 words = sg->slots[2 * k + 1] >> 24;
    if (words > 12 && m->fog_gen != g.fog_gen) {
      float rows[8];
      mesh_fog_rows(m->m, rows);
      raw_pack_f24(rows, m->f24 + 12);
      raw_pack_f24(rows + 4, m->f24 + 15);
      m->fog_gen = g.fog_gen;
    }
    raw_unif(p + sg->slots[2 * k], g.ps->uloc_clip, m->f24, words, 0);
  }
  raw_end(p + sg->nwords);
  const int dirty_now = s_tex_dirty;
  cl_track_load(&sg->out);
  if (dirty_now && !sg->in.tex_dirty) {
    s_tex_dirty = 1; /* set by the loader thread meanwhile: the next bind clears */
  }
  if (sg->last_up >= 0) {
    g.last_clip_id = c[a + sg->last_up].matrix->id;
  }
#define CL_ADD(f) g.cur.f += sg->stats[CL_S_##f];
  CL_STATS(CL_ADD)
#undef CL_ADD
  g.mesh_nofog = c[b - 1].nofog ? 1 : 0;
  __C3D_Context.flags |= C3DiF_DrawUsed;
  s_cl.replays++;
  s_cl.replay_calls += (u32)(b - a);
  s_cl.words += (u32)sg->nwords;
  s_cl.slots += (u32)sg->nslots;
}

/* the arena's room for a recording, from the start again (all recordings dropped) when full */
static uint8_t* cl_alloc(u32 bytes) {
  bytes = (bytes + 15) & ~15u;
  if (bytes > CL_ARENA_BYTES / 2) {
    return NULL;
  }
  if (s_cl.used + bytes > CL_ARENA_BYTES) {
    s_cl.used = 0;
    s_cl.gen++;
    s_cl.resets++;
  }
  uint8_t* p = s_cl.arena + s_cl.used;
  s_cl.used += bytes;
  return p;
}

/* draws c[a..b) one by one, recorded into sg if they qualify */
static void cl_record(ClSeg* sg, const ctr_mesh_call* c, int a, int b, const ClTrack* in,
                      const ClSet* set) {
  const int n = b - a;
  u32* slots = cl_scratch((u32)(2 * n));
  if (!slots) {
    cl_draw(c, a, b);
    return;
  }
  ClCall first;
  cl_rec_call(&c[a], NULL, &first);
  int st0[CL_S_COUNT];
  cl_stats(st0);
  const int dropped0 = g.cur.dropped_draws, prog0 = g.cur.prog_switches;
  const u32 upd0 = s_ctx_updates, hook0 = s_hook_runs;
  u32* const buf0 = gpuCmdBuf;
  u32* const p0 = raw_ptr();
  s_rec.on = 1;
  s_rec.bad = 0;
  s_rec.n = 0;
  s_rec.max = n;
  s_rec.p0 = p0;
  s_rec.slots = slots;
  for (int i = a; i < b; i++) {
    s_rec.call = i - a;
    g.mesh_nofog = c[i].nofog ? 1 : 0;
    ctr_gpu_draw_mesh_prepared(c[i].state, c[i].matrix, c[i].mesh, c[i].first_index,
                               c[i].index_count);
  }
  s_rec.on = 0;
  if (s_rec.bad || upd0 != s_ctx_updates || hook0 != s_hook_runs || gpuCmdBuf != buf0 ||
      g.cur.dropped_draws != dropped0 || g.cur.prog_switches != prog0 || raw_ptr() <= p0) {
    sg->valid = 0;
    return;
  }
  const int nwords = (int)(raw_ptr() - p0);
  const u32 bytes = (u32)n * sizeof(ClCall) + (u32)s_rec.n * 8u + (u32)nwords * 4u;
  uint8_t* mem = cl_alloc(bytes);
  if (!mem) {
    sg->valid = 0;
    return;
  }
  sg->calls = (ClCall*)mem;
  sg->slots = (u32*)(mem + (size_t)n * sizeof(ClCall));
  sg->words = sg->slots + 2 * s_rec.n;
  sg->calls[0] = first;
  for (int i = a + 1; i < b; i++) {
    cl_rec_call(&c[i], &c[i - 1], &sg->calls[i - a]);
  }
  memcpy(sg->slots, slots, (size_t)s_rec.n * 8);
  memcpy(sg->words, p0, (size_t)nwords * 4);
  sg->ncalls = n;
  sg->nslots = s_rec.n;
  sg->nwords = nwords;
  sg->last_up = s_rec.n ? (int)(slots[2 * (s_rec.n - 1) + 1] & 0xFFFFFF) : -1;
  sg->bytes = bytes;
  sg->in = *in;
  sg->set = *set;
  cl_track_save(&sg->out);
  int st1[CL_S_COUNT];
  cl_stats(st1);
  for (int k = 0; k < CL_S_COUNT; k++) {
    sg->stats[k] = st1[k] - st0[k];
  }
  sg->gen = s_cl.gen;
  sg->valid = 1;
  s_cl.records++;
}

/* compare mode: the copy just made at p0 against drawing the stretch one by one; the latter's
 * words and state stay */
static void cl_compare(const ClSeg* sg, const ctr_mesh_call* c, int a, int b, u32* p0,
                       const ClTrack* in, const ctr_gpu_stats* stats0, u32 clip0) {
  const u32 nw = (u32)(raw_ptr() - p0);
  u32* copy = cl_scratch(nw);
  if (!copy) {
    return;
  }
  memcpy(copy, p0, (size_t)nw * 4);
  ClTrack tr;
  cl_track_save(&tr);
  int sr[CL_S_COUNT];
  cl_stats(sr);
  const u32 clip_r = g.last_clip_id;
  /* back to the state before the copy, then the per-frame path */
  cl_track_load(in);
  g.bound_tex = s_cl.bound0_tex;
  g.bound_data = s_cl.bound0_data;
  g.bound_param = s_cl.bound0_param;
  g.cur = *stats0;
  g.last_clip_id = clip0;
  raw_end(p0);
  cl_draw(c, a, b);
  const u32 nn = (u32)(raw_ptr() - p0);
  ClTrack tn;
  cl_track_save(&tn);
  int sn[CL_S_COUNT];
  cl_stats(sn);
  s_cl.compares++;
  int first = -1;
  for (u32 i = 0; i < nw && i < nn; i++) {
    if (copy[i] != p0[i]) {
      first = (int)i;
      break;
    }
  }
  /* the loader thread may have set the texture cache flag meanwhile (not a difference) */
  tn.tex_dirty = tr.tex_dirty;
  const int words_ok = nw == nn && first < 0;
  const int track_ok = !memcmp(&tr, &tn, sizeof(tr)) && clip_r == g.last_clip_id;
  const int stats_ok = !memcmp(sr, sn, sizeof(sr));
  if (!words_ok || !track_ok || !stats_ok) {
    s_cl.mismatches++;
    if (s_cl.logged_mismatches < 8) {
      s_cl.logged_mismatches++;
      printf("[ctr] static_cmdlists compare: MISMATCH frame %d, %d draws: words %lu copied / %lu "
             "drawn (first difference at word %d: %08lx / %08lx), state %s, statistics %s\n",
             g.frame_no, b - a, (unsigned long)nw, (unsigned long)nn, first,
             first >= 0 ? (unsigned long)copy[first] : 0ul,
             first >= 0 ? (unsigned long)p0[first] : 0ul, track_ok ? "same" : "DIFFERS",
             stats_ok ? "same" : "DIFFER");
    }
  }
  (void)sg;
}

static void cl_segment(ClSeg* sg, const ctr_mesh_call* c, int a, int b) {
  s_cl.segs++;
  /* (AI-assisted, micro-gpu) a stretch's first texture bind writes all of unit 0's registers: the
   * ones before it (a sky or ocean texture's other copy every second frame) aren't part of what a
   * recording depends on */
  s_raw.tex0_regs_ok = 0;
  ClSet set;
  cl_set_save(&set);
  ClTrack in;
  cl_track_save(&in);
  const int n = b - a;
  int match = sg->valid && sg->gen == s_cl.gen && sg->ncalls == n &&
              s_raw.apt_epoch == s_raw.seen_epoch && !memcmp(&sg->set, &set, sizeof(set)) &&
              !memcmp(&sg->in, &in, sizeof(in));
  /* the hash of the stretch as drawn now (only made once it differs from the recording: while
   * it matches, it is the recording's) */
  u32 h = 0;
  int hashed = !match;
  if (hashed) {
    h = cl_hash(cl_hash(2166136261u, &set, sizeof(set)), &in, sizeof(in));
  }
  for (int i = a; i < b; i++) {
    ClCall r;
    cl_rec_call(&c[i], i > a ? &c[i - 1] : NULL, &r);
    if (match && !words_equal(&r, &sg->calls[i - a], sizeof(r) / 4)) {
      match = 0;
      hashed = 1;
      h = cl_hash(cl_hash(2166136261u, &set, sizeof(set)), &in, sizeof(in));
      h = cl_hash(h, sg->calls, (u32)(i - a) * sizeof(ClCall));
      if (s_cl.logged_why < 12 && sg->valid && sg->gen == s_cl.gen) {
        s_cl.logged_why++;
        const u32* x = (const u32*)&sg->calls[i - a];
        const u32* y = (const u32*)&r;
        for (u32 w = 0; w < 12; w++) {
          if (x[w] != y[w]) {
            printf("[ctr] static_cmdlists: draw %d of %d differs at word %lu (%08lx / %08lx)\n",
                   i - a, n, (unsigned long)w, (unsigned long)x[w], (unsigned long)y[w]);
            break;
          }
        }
      }
    }
    if (hashed) {
      h = cl_hash(h, &r, sizeof(r));
    }
  }
  if (match) {
    h = sg->last_hash; /* (the recording's: unchanged since) */
  }
  if (match && gpuCmdBufSize - gpuCmdBufOffset >=
                   (u32)sg->nwords + CMD_RESERVE_WORDS + CMD_DRAW_WORDS) {
    u32* const p0 = raw_ptr();
    const ctr_gpu_stats stats0 = g.cur;
    const u32 clip0 = g.last_clip_id;
    s_cl.bound0_tex = g.bound_tex; /* (compare mode puts them back with the state before) */
    s_cl.bound0_data = g.bound_data;
    s_cl.bound0_param = g.bound_param;
    cl_replay(sg, c, a, b);
    if (s_cl.mode == 2) {
      cl_compare(sg, c, a, b, p0, &in, &stats0, clip0);
    }
    sg->last_hash = h;
    return;
  }
  if (sg->valid) {
    s_cl.unlike++;
    /* what differed (the log line's "not alike" split) */
    if (sg->gen != s_cl.gen) {
      s_cl.why[0]++;
    } else if (sg->ncalls != n) {
      s_cl.why[1]++;
    } else if (memcmp(&sg->set, &set, sizeof(set))) {
      s_cl.why[2]++;
    } else if (memcmp(&sg->in, &in, sizeof(in))) {
      s_cl.why[3]++;
      if (s_cl.logged_why < 6) {
        s_cl.logged_why++;
        const u32* a = (const u32*)&sg->in;
        const u32* b2 = (const u32*)&in;
        for (u32 w = 0; w < sizeof(in) / 4; w++) {
          if (a[w] != b2[w]) {
            printf("[ctr] static_cmdlists: state before differs at byte %lu (%08lx / %08lx)\n",
                   (unsigned long)(w * 4), (unsigned long)a[w], (unsigned long)b2[w]);
            break;
          }
        }
      }
    } else {
      s_cl.why[4]++;
    }
  }
  if (s_raw.apt_epoch == s_raw.seen_epoch && h == sg->last_hash) {
    cl_record(sg, c, a, b, &in, &set);
  } else {
    sg->valid = 0;
    cl_draw(c, a, b);
  }
  s_cl.normal_calls += (u32)n;
  sg->last_hash = h;
}

void ctr_gpu_draw_mesh_calls(const ctr_mesh_call* calls, int n, ctr_cmd_cache** cache) {
  if (!g.ready || !g.in_frame || n <= 0) {
    return;
  }
  if (!s_cl.mode || !s_raw.on || !cache) {
    cl_draw(calls, 0, n);
    return;
  }
  if (!*cache) {
    *cache = (ctr_cmd_cache*)calloc(1, sizeof(ctr_cmd_cache));
    if (!*cache) {
      cl_draw(calls, 0, n);
      return;
    }
  }
  ctr_cmd_cache* cc = *cache;
  int seg = 0, start = -1, prev_prog = -1, prev_plain = 0;
  for (int i = 0; i <= n; i++) {
    int head = 1, prog = -1, plain = 0;
    if (i < n) {
      plain = cl_plain(&calls[i]);
      prog = plain ? cl_prog(&calls[i]) : -1;
      head = i == 0 || !plain || !prev_plain || prog != prev_prog ||
             (s_sky_hook && !keeps_sky(calls[i].state));
      if (head && i) {
        const int t = tex_res(calls[i].state->tex);
        const int own_tex = t >= 0 && t < MAX_TEXTURES && g.textures[t].used == 1 &&
                            g.textures[t].pool < 0;
        s_cl.head_why[!plain ? (own_tex ? 0 : 1)
                             : (!prev_plain ? 2 : (prog != prev_prog ? 3 : 4))]++;
      }
    }
    if (head && start >= 0) {
      if (seg < CL_MAX_SEGS) {
        cl_segment(&cc->seg[seg], calls, start, i);
      } else {
        cl_draw(calls, start, i);
      }
      seg++;
      start = -1;
    }
    if (i == n) {
      break;
    }
    if (head) {
      cl_draw(calls, i, i + 1);
      s_cl.normal_calls++;
    } else if (start < 0) {
      start = i;
    }
    prev_prog = prog;
    prev_plain = plain;
  }
}

void ctr_gpu_cmd_cache_free(ctr_cmd_cache** cache) {
  if (cache && *cache) {
    free(*cache); /* (the recordings are in the arena) */
    *cache = NULL;
  }
}

int ctr_gpu_cmdlist_line(char* out, int size) {
  if (!s_cl.mode && !s_cl.segs) {
    return 0;
  }
  const u32 frames = (u32)g.frame_no - s_cl.frame0;
  if (!frames) {
    return 0;
  }
  const double f = frames;
  const int len = snprintf(
      out, size,
      "[ctr] static_cmdlists %s: per frame %.1f stretches, %.1f copied (%.0f draws, %.1f KB of "
      "command words, %.0f matrices written), %.1f draws drawn one by one, %.2f recorded, %.2f "
      "not alike; arena %u of %u KB in linear memory (%u restarts)%s",
      s_cl.mode == 2 ? "compare" : (s_cl.mode ? "on" : "off"), s_cl.segs / f, s_cl.replays / f,
      s_cl.replay_calls / f, s_cl.words * 4.0 / 1024.0 / f, s_cl.slots / f, s_cl.normal_calls / f,
      s_cl.records / f, s_cl.unlike / f, s_cl.used / 1024, CL_ARENA_BYTES / 1024, s_cl.resets,
      s_cl.mode == 2 ? "" : "");
  int l = len;
  if (l < size && s_cl.unlike) {
    l += snprintf(out + l, size - l,
                  "; not alike: %u arena restarted, %u draw count, %u settings, %u state before, "
                  "%u the draws",
                  s_cl.why[0], s_cl.why[1], s_cl.why[2], s_cl.why[3], s_cl.why[4]);
  }
  memset(s_cl.why, 0, sizeof(s_cl.why));
  if (l < size) {
    l += snprintf(out + l, size - l,
                  "; drawn alone: %.1f texture in own memory, %.1f other not plain, %.1f after "
                  "those, %.1f program change, %.1f sky hook",
                  s_cl.head_why[0] / f, s_cl.head_why[1] / f, s_cl.head_why[2] / f,
                  s_cl.head_why[3] / f, s_cl.head_why[4] / f);
  }
  memset(s_cl.head_why, 0, sizeof(s_cl.head_why));
  if (l < size) {
    /* (AI-assisted, micro-gpu) all threads' (textures made by the loader thread too) */
    l += snprintf(out + l, size - l, "; GSP cache flush requests %.1f per frame",
                  s_flush_requests / f);
  }
  s_flush_requests = 0;
  if (l < size) {
    /* (AI-assisted, micro-mg) MICRO_TEXSTATE */
    l += snprintf(out + l, size - l, "; %.1f draw states only bound their texture",
                  s_state_fast / f);
  }
  s_state_fast = 0;
  if (s_cl.mode == 2 && l < size) {
    l += snprintf(out + l, size - l, "; compare: %u copies checked, %u MISMATCHED", s_cl.compares,
                  s_cl.mismatches);
  }
  s_cl.segs = s_cl.replays = s_cl.records = s_cl.unlike = s_cl.normal_calls = s_cl.replay_calls =
      s_cl.words = s_cl.slots = s_cl.resets = s_cl.compares = s_cl.mismatches = 0;
  s_cl.frame0 = (u32)g.frame_no;
  return l;
}

/* ---------------- (AI-assisted) the ocean (config ocean = waves) ---------------- */

int ctr_gpu_has_stencil(void) {
  return !g_depth16;
}

void* ctr_gpu_stream_alloc(int bytes) {
  if (!g.ready || !g.in_frame || bytes <= 0) {
    return NULL;
  }
  const size_t b = ((size_t)bytes + 15) & ~(size_t)15;
  if (g.vbuf_used + b > VBUF_BYTES) {
    return NULL;
  }
  uint8_t* p = g.vbuf + g.vbuf_used; /* flushed with the frame's other immediate vertices */
  g.vbuf_used += b;
  return p;
}

/* the shimmer texture on unit 2 (citro3d sends it at the next update), bound again only when it
 * or its memory (pipeline copies) changed */
static const C3D_Tex* s_ocean_tex2;
static const void* s_ocean_tex2_data;
static u32 s_ocean_tex2_param;
/* (AI-assisted, j1ocean) clamp: the full ocean's sky reflection (env map clamped, no mip levels) */
static void ocean_bind_tex2_wrap(int handle, int clamp) {
  if (handle < 0 || handle >= MAX_TEXTURES || g.textures[handle].used != 1) {
    return;
  }
  TexSlot* slot = &g.textures[handle];
  C3D_Tex* tex = &slot->tex;
  const GPU_TEXTURE_WRAP_PARAM wrap = clamp ? GPU_CLAMP_TO_EDGE : GPU_REPEAT;
  u32 param = tex->param & ~(GPU_TEXTURE_MAG_FILTER(GPU_LINEAR) | GPU_TEXTURE_MIN_FILTER(GPU_LINEAR) |
                             GPU_TEXTURE_WRAP_S(3) | GPU_TEXTURE_WRAP_T(3) |
                             GPU_TEXTURE_MIP_FILTER(GPU_LINEAR));
  param |= GPU_TEXTURE_MAG_FILTER(GPU_LINEAR) | GPU_TEXTURE_MIN_FILTER(GPU_LINEAR) |
           GPU_TEXTURE_WRAP_S(wrap) | GPU_TEXTURE_WRAP_T(wrap);
  if (s_ocean_tex2 == tex && s_ocean_tex2_data == tex->data && s_ocean_tex2_param == param &&
      g.ocean_tex2_bound) {
    return;
  }
  u8 max_level = 0;
  if (slot->levels > 1 && g_mip_mode > 0 && !clamp) {
    max_level = (u8)(slot->levels - 1);
    if (g_mip_mode == 2) {
      param |= GPU_TEXTURE_MIP_FILTER(GPU_LINEAR);
    }
  }
  tex->param = param;
  tex->maxLevel = max_level;
  C3D_TexBind(2, tex);
  s_ocean_tex2 = tex;
  s_ocean_tex2_data = tex->data;
  s_ocean_tex2_param = param;
  g.ocean_tex2_bound = 1;
  g.cur.tex_binds++;
}
static void ocean_bind_tex2(int handle) {
  ocean_bind_tex2_wrap(handle, 0);
}

void ctr_gpu_ocean_draw(const ctr_draw_state* state, int tex2, const ctr_ocean_draw* d, int mesh,
                        const void* verts, const uint16_t* indices, int first_index,
                        int index_count) {
  SKY_HOOK(state);
  if (!g.ready || !g.in_frame || index_count < 3) {
    return;
  }
  u32 vpa, ipa;
  const void* vva;
  const uint16_t* iva;
  if (mesh >= 0) {
    if (mesh >= MAX_MESHES || g.meshes[mesh].used != 1) {
      return;
    }
    const MeshSlot* ms = &g.meshes[mesh];
    vpa = ms->verts_pa;
    vva = ms->verts;
    ipa = ms->indices_pa;
    iva = ms->indices;
  } else {
    if (!verts || !indices) {
      return;
    }
    vva = verts;
    iva = indices;
    vpa = osConvertVirtToPhys(verts);
    ipa = osConvertVirtToPhys(indices);
  }
  ipa += 2u * (u32)first_index;
  iva += first_index;
  if (!cmd_room()) {
    return;
  }
  use_program(PROG_OCEAN);
  ocean_bind_tex2(tex2);
  if (use_raw(state)) {
    apply_state(state, 3, 1);
    raw_vertex_buffer(vpa, 0x210, RAW_BUF_FLAGS1(16, 3, 0));
    raw_sync();
    /* the matrix and the 5 rows (texture coordinates, crest) as float24, two bursts */
    u32 rows[15];
    for (int i = 0; i < 5; i++) {
      const float* r = i < 4 ? d->tex[i] : d->crest;
      const float v[4] = {r[3], r[2], r[1], r[0]};
      raw_pack_f24(v, rows + 3 * i);
    }
    u32* p = raw_unif(raw_ptr(), g.ps->uloc_ocean_clip, d->clip.f24, 12, 0);
    p = raw_unif(p, g.ps->uloc_ocean_tex, rows, 15, 0);
    raw_end(p);
    raw_draw_elements(ipa, (u32)index_count);
  } else {
    raw_vc_flush();
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g.ps->uloc_ocean_clip, (const C3D_Mtx*)d->clip.m);
    for (int i = 0; i < 5; i++) {
      const float* r = i < 4 ? d->tex[i] : d->crest;
      C3D_FVUnifSet(GPU_VERTEX_SHADER, g.ps->uloc_ocean_tex + i, r[0], r[1], r[2], r[3]);
    }
    c3d_vertex_buffer(vpa, vva, 16, 3, 0x210);
    apply_state(state, 3, 0);
    C3D_DrawElements(GPU_TRIANGLES, index_count, C3D_UNSIGNED_SHORT, iva);
  }
  g.cur.draws++;
  g.cur.triangles += index_count / 3;
}

void ctr_gpu_ocean_end(void) {
  if (!g.ready || !g.in_frame) {
    return;
  }
  C3Di_Context* ctx = &__C3D_Context;
  if (g.ocean_tex2_bound) {
    /* unit 2 off (C3D_TexBind can't take NULL for units 1-2) */
    ctx->tex[2] = NULL;
    ctx->flags |= C3DiF_Tex(2);
    g.ocean_tex2_bound = 0;
    s_ocean_tex2 = NULL;
  }
  C3D_TexEnv e;
  C3D_TexEnvInit(&e);
  commit_texenv(2, &e, s_raw.on);
  g.ocean_stage2 = 0;
  if (ctx->effect.stencilMode & 1) {
    ctx->effect.stencilMode &= ~1u;
    ctx->flags |= C3DiF_Effect;
  }
  g.last_state_valid = 0;
}

/* ---------------- skinned meshes ---------------- */

int ctr_gpu_skinned_mesh_create(const void* verts, int vertex_count, const uint16_t* indices,
                                int index_count) {
  if (!g.ready) {
    return -1;
  }
  const int slot = reserve_mesh_slot();
  if (slot < 0) {
    return -1;
  }
  size_t vbytes = (size_t)vertex_count * 24; /* c3l::MercVertex */
  size_t ibytes = (size_t)index_count * 2;
  ctr_linear_lock();
  void* v = linearAlloc(vbytes);
  uint16_t* ix = (uint16_t*)linearAlloc(ibytes);
  if (!v || !ix) {
    if (v) {
      linearFree(v);
    }
    if (ix) {
      linearFree(ix);
    }
    g.meshes[slot].used = 0;
    ctr_linear_unlock();
    return -1;
  }
  ctr_linear_unlock();
  memcpy(v, verts, vbytes);
  memcpy(ix, indices, ibytes);
  cache_flush_now(v, vbytes);
  cache_flush_now(ix, ibytes);
  g.meshes[slot].verts = v;
  g.meshes[slot].ext = 0; /* (AI-assisted, streaming) */
  g.meshes[slot].indices = ix;
  g.meshes[slot].verts_pa = osConvertVirtToPhys(v);
  g.meshes[slot].indices_pa = osConvertVirtToPhys(ix);
  g.meshes[slot].verts_alt = NULL;
  g.meshes[slot].vbytes = (uint32_t)vbytes;
  g.meshes[slot].alt_frame = -1;
  g.meshes[slot].used = 1;
  return slot;
}

void* ctr_gpu_mesh_vertices(int mesh) {
  if (mesh < 0 || mesh >= MAX_MESHES || g.meshes[mesh].used != 1) {
    return NULL;
  }
  MeshSlot* m = &g.meshes[mesh];
  if (s_pipeline && m->vbytes) {
    /* (AI-assisted) the frame on the GPU may still read these vertices: from the first rewrite in
     * a frame on, write the other buffer (the frame before is done). Every rewrite covers all of
     * the moved vertices (CtrMercRenderer::apply_blerc), so either buffer is complete. */
    if (!m->verts_alt) {
      ctr_linear_lock();
      m->verts_alt = linearAlloc(m->vbytes);
      ctr_linear_unlock();
      if (m->verts_alt) {
        memcpy(m->verts_alt, m->verts, m->vbytes);
      }
    }
    if (m->verts_alt && m->alt_frame != g.frame_no) {
      void* t = m->verts;
      m->verts = m->verts_alt;
      m->verts_alt = t;
      m->verts_pa = osConvertVirtToPhys(m->verts);
      m->alt_frame = g.frame_no;
      g.last_mesh_valid = 0;
    }
  }
  return m->verts;
}

void* ctr_gpu_mesh_vertices_inplace(int mesh, int* vertex_count) {
  if (mesh < 0 || mesh >= MAX_MESHES || g.meshes[mesh].used != 1 || !g.meshes[mesh].fill_vbytes ||
      g.meshes[mesh].verts_alt) {
    return NULL;
  }
  *vertex_count = (int)(g.meshes[mesh].fill_vbytes / 16u);
  return g.meshes[mesh].verts;
}

void ctr_gpu_set_micro_mask(unsigned int mask) {
  s_micro_want = (u32)mask | BIT(31);
}

void ctr_gpu_flush_range(const void* addr, unsigned int size) {
  if (!g.ready || !addr || !size) {
    return;
  }
  cache_flush_frame(addr, (u32)size);
}

void ctr_gpu_mesh_flush(int mesh, int offset, int size) {
  if (mesh < 0 || mesh >= MAX_MESHES || g.meshes[mesh].used != 1 || size <= 0) {
    return;
  }
  cache_flush_frame((uint8_t*)g.meshes[mesh].verts + offset, (u32)size);
}

/* (AI-assisted) clip (row-major, GL clip space) -> PICA clip space, in C3D_Mtx layout */
static void skin_clip_matrix(const float clip[16], C3D_Mtx* m) {
  gl_to_pica_mul(clip, m);
}

/* (AI-assisted) shader_opt (ctr_skin_opt / ctr_skin_env_opt.v.pica): the weights go in unscaled
 * (0..255), so the camera space position is 255 times as long: the clip matrix's x, y, z columns
 * (C3D_FVec c[3], c[2], c[1]) / 255 undo it */
static void skin_opt_scale(C3D_Mtx* m) {
  if (!g.ps->opt) {
    return;
  }
  for (int r = 0; r < 4; r++) {
    for (int c = 1; c < 4; c++) {
      m->r[r].c[c] *= 1.0f / 255.0f;
    }
  }
}

/* (AI-assisted) the bone rows as raw f32 uniform bursts: row r of each 3x4 bone matrix in
 * uniform array row<r>, in C3D_FVec order (w z y x), written straight into the command buffer */
static void raw_skin_rows(const int* locs, const float* bones, int palette_count) {
  u32* p = raw_ptr();
  const u32 n = 4u * (u32)palette_count;
  for (int row = 0; row < 3; row++) {
    p = raw_w(p, GPUREG_VSH_FLOATUNIFORM_CONFIG, 0xF, (u32)locs[row] | BIT(31));
    /* the burst: first value, header, the rest, padding (see raw_n) */
    u32* d = p;
    const float* src = bones + 4 * row;
    for (int b = 0; b < palette_count; b++, src += 12) {
      u32* q = d + (b ? 4 * b + 1 : 0);
      const u32* s = (const u32*)src;
      if (b == 0) {
        q[0] = s[3];
        q[1] = RAW_HDR(GPUREG_VSH_FLOATUNIFORM_DATA, 0xF) | ((n - 1) << 20);
        q[2] = s[2];
        q[3] = s[1];
        q[4] = s[0];
      } else {
        q[0] = s[3];
        q[1] = s[2];
        q[2] = s[1];
        q[3] = s[0];
      }
    }
    p += n + 1;
    if (!(n & 1)) {
      *p++ = 0;
    }
  }
  raw_end(p);
  s_raw.vs_changed = 1;
}

/* (AI-assisted) skin_rows_f24: the same as float24 uniforms (3 words per vector, raw_pack_f24),
 * from the words ctr_gpu_pack_skin_rows_f24 made: per row one config write and one burst (first
 * word, header, the rest, padding: see raw_n). The skin and shine programs' bursts differ only in
 * the config word (locs). */
static void raw_skin_rows_f24(const int* locs, const u32* words, int palette_count) {
  u32* p = raw_ptr();
  const u32 n = 3u * (u32)palette_count;
  for (int row = 0; row < 3; row++) {
    p = raw_w(p, GPUREG_VSH_FLOATUNIFORM_CONFIG, 0xF, (u32)locs[row]); /* bit 31 clear: f24 */
    const u32* src = words + n * (u32)row;
    p[0] = src[0];
    p[1] = RAW_HDR(GPUREG_VSH_FLOATUNIFORM_DATA, 0xF) | ((n - 1) << 20);
    memcpy(p + 2, src + 1, 4u * (n - 1));
    p += n + 1;
    if (!(n & 1)) {
      *p++ = 0;
    }
  }
  raw_end(p);
  s_raw.vs_changed = 1;
}

void ctr_gpu_pack_skin_rows_f24(const float* bones, int palette_count, uint32_t* out) {
  for (int row = 0; row < 3; row++) {
    const float* src = bones + 4 * row;
    for (int b = 0; b < palette_count; b++, src += 12, out += 3) {
      const float v[4] = {src[3], src[2], src[1], src[0]}; /* C3D_FVec order: w z y x */
      raw_pack_f24(v, out);
    }
  }
}

/* (AI-assisted) the bones as skin_rows_f24 words: the caller's, else made here */
static const u32* skin_rows_words(const float* bones, const uint32_t* bones_f24, int palette_count,
                                  u32* tmp) {
  if (bones_f24) {
    return bones_f24;
  }
  ctr_gpu_pack_skin_rows_f24(bones, palette_count, tmp);
  return tmp;
}

/* (AI-assisted, far-fog) ctr_skin_opt's mfog uniform: [0] the fog amount as a row for the camera
 * space position (merc2.vert: (255 - (hvdf.w - w)) / 255 = w / 255 + 1 - hvdf.w / 255, w = the clip
 * matrix's w row, skin_wrow), [1] x, y: its clamps (fog max, fog min as amounts). C3D_FVec order
 * (w z y x), f32. */
static void skin_fog_rows(int raw) {
  C3D_FVec v[2];
  for (int k = 0; k < 4; k++) {
    v[0].c[k] = g.skin_wrow[k] * (1.0f / 255.0f);
  }
  v[0].c[0] += 1.0f - g.merc_fog_hvdf_w * (1.0f / 255.0f);
  v[1].c[0] = 0.0f;
  v[1].c[1] = 0.0f;
  v[1].c[2] = g.merc_fog_hi;
  v[1].c[3] = g.merc_fog_lo;
  if (raw) {
    raw_end(raw_unif(raw_ptr(), g.ps->uloc_skin_mfog, (const u32*)v, 8, 1));
  } else {
    C3D_FVec* dst = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, g.ps->uloc_skin_mfog, 2);
    dst[0] = v[0];
    dst[1] = v[1];
  }
  g.skin_mfog_gen = g.merc_fog_gen;
}

void ctr_gpu_set_merc_fog(float hvdf_w, float fog_min, float fog_max, uint8_t r, uint8_t gr,
                          uint8_t b) {
  if (!g.ready) {
    return;
  }
  /* the clamps kept in 0..255 (the amount in 0..1), as ctr_gpu_set_mesh_fog's */
  fog_min = fog_min < 0.0f ? 0.0f : (fog_min > 255.0f ? 255.0f : fog_min);
  fog_max = fog_max < 0.0f ? 0.0f : (fog_max > 255.0f ? 255.0f : fog_max);
  const float lo = 1.0f - fog_max * (1.0f / 255.0f);
  float hi = 1.0f - fog_min * (1.0f / 255.0f);
  hi = hi < lo ? lo : hi;
  if (hvdf_w != g.merc_fog_hvdf_w || lo != g.merc_fog_lo || hi != g.merc_fog_hi) {
    g.merc_fog_hvdf_w = hvdf_w;
    g.merc_fog_lo = lo;
    g.merc_fog_hi = hi;
    g.merc_fog_gen = g.merc_fog_gen + 1 ? g.merc_fog_gen + 1 : 1;
  }
  fog_ramp_color(r, gr, b); /* the frame's fog color (the level renderers' too) */
}

int ctr_gpu_merc_fog_ready(void) {
  return g.ready && g.ps && g.ps->uloc_skin_mfog >= 0 && g.fog_tex_valid;
}

void ctr_gpu_draw_skinned(const ctr_draw_state* state, const float clip[16], const float* bones,
                          int palette_count, const uint32_t* bones_f24, uint32_t bones_id,
                          const float lights[28], int mesh, int first_index, int index_count) {
  SKY_HOOK(state);
  if (!g.ready || !g.in_frame || mesh < 0 || mesh >= MAX_MESHES || g.meshes[mesh].used != 1 ||
      index_count < 3) {
    return;
  }
  /* (AI-assisted) room for the bone rows too (3 x 24 vectors) */
  if (!cmd_room() || gpuCmdBufSize - gpuCmdBufOffset < CMD_RESERVE_WORDS + CMD_DRAW_WORDS + 320) {
    return;
  }
  use_program(PROG_SKIN);
  const MeshSlot* ms = &g.meshes[mesh];
  const int raw = use_raw(state);
  if (raw) {
    apply_state_tint(state, 2, 0xffffffffu, 1);
    raw_vertex_buffer(ms->verts_pa, 0x543210, RAW_BUF_FLAGS1(24, 6, 0));
    raw_sync();
  } else {
    raw_vc_flush();
  }
  /* matrix and lights: uploaded when they change (the draws of a model share them) */
  const int same_clip = g.last_skin_valid && words_equal(g.last_skin_clip, clip, 16);
  /* (AI-assisted) bones and lights: their own registers (skin_bl_valid, while the skin program
   * stays bound) */
  const int same_lights = g.skin_bl_valid && words_equal(g.last_skin_lights, lights, 28);
  if (!same_clip) {
    C3D_Mtx m;
    skin_clip_matrix(clip, &m);
    skin_opt_scale(&m);
    /* (AI-assisted, far-fog) the w row for the fog rows (made again below) */
    memcpy(g.skin_wrow, m.r[3].c, sizeof(g.skin_wrow));
    g.skin_mfog_gen = 0;
    if (raw && s_skin_clip_f24) {
      /* (AI-assisted) skin_clip_f24: the matrix as float24 (12 words instead of 16) */
      u32 c24[12];
      for (int r = 0; r < 4; r++) {
        raw_pack_f24(m.r[r].c, c24 + 3 * r);
      }
      raw_end(raw_unif(raw_ptr(), g.ps->uloc_skin_clip, c24, 12, 0));
    } else if (raw) {
      raw_end(raw_unif(raw_ptr(), g.ps->uloc_skin_clip, (const u32*)&m, 16, 1));
    } else {
      C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g.ps->uloc_skin_clip, &m);
    }
    memcpy(g.last_skin_clip, clip, sizeof(g.last_skin_clip));
  }
  /* (AI-assisted, far-fog) the skin program's fog rows: with every clip upload or fog change */
  if (g.ps->uloc_skin_mfog >= 0 && g.skin_mfog_gen != g.merc_fog_gen) {
    skin_fog_rows(raw);
  }
  if (palette_count > CTR_MAX_PALETTE) {
    palette_count = CTR_MAX_PALETTE;
  }
  /* bones: the draws of a model with at most CTR_MAX_PALETTE bones all use the same palette.
   * (AI-assisted) Compared by the caller's palette number (bones_id), not by the 288 words: a
   * palette is never changed under its number */
  if (!g.skin_bl_valid || g.last_skin_palette_count != palette_count || !bones_id ||
      g.last_skin_bones_id != bones_id) {
    if (raw && palette_count > 0 && s_skin_f24) {
      u32 tmp[9 * CTR_MAX_PALETTE];
      raw_skin_rows_f24(g.ps->uloc_skin_rows,
                        skin_rows_words(bones, bones_f24, palette_count, tmp), palette_count);
    } else if (raw && palette_count > 0) {
      raw_skin_rows(g.ps->uloc_skin_rows, bones, palette_count);
    } else {
      for (int row = 0; row < 3; row++) {
        C3D_FVec* dst =
            C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, g.ps->uloc_skin_rows[row], palette_count);
        for (int p = 0; p < palette_count; p++) {
          const float* src = bones + 12 * p + 4 * row;
          dst[p].x = src[0];
          dst[p].y = src[1];
          dst[p].z = src[2];
          dst[p].w = src[3];
        }
      }
    }
    g.last_skin_palette_count = palette_count;
    g.last_skin_bones_id = bones_id;
  }
  if (!same_lights) {
    /* (AI-assisted) shader_opt: the light colors and ambient times the vertex color scale */
    float lopt[28];
    const float* lights_in = lights;
    if (g.ps->opt) {
      memcpy(lopt, lights, sizeof(lopt));
      for (int i = 12; i < 28; i++) {
        lopt[i] *= (i & 3) == 3 ? 1.0f / 255.0f : 1.0f / 510.0f;
      }
      lights = lopt;
    }
    if (raw && s_skin_clip_f24) {
      /* (AI-assisted) skin_clip_f24: the lights as float24 (21 words instead of 28) */
      u32 l24[21];
      for (int i = 0; i < 7; i++) {
        const float v[4] = {lights[4 * i + 3], lights[4 * i + 2], lights[4 * i + 1],
                            lights[4 * i]};
        raw_pack_f24(v, l24 + 3 * i);
      }
      raw_end(raw_unif(raw_ptr(), g.ps->uloc_skin_lights, l24, 21, 0));
    } else if (raw) {
      u32 lv[28];
      const u32* l = (const u32*)lights;
      for (int i = 0; i < 7; i++) {
        lv[4 * i] = l[4 * i + 3];
        lv[4 * i + 1] = l[4 * i + 2];
        lv[4 * i + 2] = l[4 * i + 1];
        lv[4 * i + 3] = l[4 * i];
      }
      raw_end(raw_unif(raw_ptr(), g.ps->uloc_skin_lights, lv, 28, 1));
    } else {
      C3D_FVec* lv = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, g.ps->uloc_skin_lights, 7);
      for (int i = 0; i < 7; i++) {
        lv[i].x = lights[4 * i];
        lv[i].y = lights[4 * i + 1];
        lv[i].z = lights[4 * i + 2];
        lv[i].w = lights[4 * i + 3];
      }
    }
    memcpy(g.last_skin_lights, lights_in, sizeof(g.last_skin_lights));
    lights = lights_in;
  }
  g.last_skin_valid = 1;
  g.skin_bl_valid = 1;
  if (raw) {
    raw_draw_elements(ms->indices_pa + 2u * (u32)first_index, (u32)index_count);
  } else {
    apply_state_tint(state, 2, 0xffffffffu, 0);
    c3d_vertex_buffer(ms->verts_pa, ms->verts, 24, 6, 0x543210);
    C3D_DrawElements(GPU_TRIANGLES, index_count, C3D_UNSIGNED_SHORT, ms->indices + first_index);
  }
  g.cur.draws++;
  g.cur.triangles += index_count / 3;
}

void ctr_gpu_draw_skinned_env(const ctr_draw_state* state, const float clip[16], const float* bones,
                              int palette_count, const uint32_t* bones_f24, uint32_t bones_id,
                              const float fade[4], int mesh, int first_index, int index_count) {
  SKY_HOOK(state);
  if (!g.ready || !g.in_frame || mesh < 0 || mesh >= MAX_MESHES || g.meshes[mesh].used != 1 ||
      index_count < 3) {
    return;
  }
  /* (AI-assisted) room for the bone rows too (3 x 24 vectors), as ctr_gpu_draw_skinned */
  if (!cmd_room() || gpuCmdBufSize - gpuCmdBufOffset < CMD_RESERVE_WORDS + CMD_DRAW_WORDS + 320) {
    return;
  }
  use_program(PROG_SKIN_ENV);
  if (palette_count > CTR_MAX_PALETTE) {
    palette_count = CTR_MAX_PALETTE;
  }
  /* (AI-assisted) The shine pass tests its depth GEQUAL against its own base pass (no depth write),
   * so its positions must be the same bits as ctr_gpu_draw_skinned's: the same program arithmetic
   * (the skin and skin_env programs compute the position alike) on the same uniform values. The
   * clip matrix and bones therefore go to the GPU exactly as the base pass sends them: raw, as
   * float24 from raw_f24 with skin_clip_f24 / skin_rows_f24, else as f32. They used to go through
   * citro3d as f32 always, which the GPU converts to float24 itself, while the base pass's were
   * rounded by raw_f24: two conversions that disagree in the last bit moved the shine a few hundred
   * depth units in front of or behind the model, and the whole shine of a model passed or failed
   * the depth test frame by frame (the title logo flickering between shiny and dull with
   * skin_clip_f24 / skin_rows_f24 on, in Azahar too). */
  const MeshSlot* ms = &g.meshes[mesh];
  const int raw = use_raw(state);
  if (raw) {
    apply_state_tint(state, 2, 0xffffffffu, 1);
    raw_vertex_buffer(ms->verts_pa, 0x543210, RAW_BUF_FLAGS1(24, 6, 0));
    raw_sync();
  } else {
    raw_vc_flush();
  }
  /* (AI-assisted) cmd_dedup: the shine draws come one after another (envmap_batch), all with the
   * camera's matrix and each model's draws with the same bones: each uploaded again only when it
   * changed or another program used the registers since (use_program clears env_valid) */
  const int dd = s_raw.dedup && g.env_valid;
  if (dd && words_equal(g.last_env_clip, clip, 16)) {
    g.cur.cmd_dedup_words += 18;
  } else {
    C3D_Mtx m;
    skin_clip_matrix(clip, &m);
    skin_opt_scale(&m);
    if (raw && s_skin_clip_f24) {
      u32 c24[12];
      for (int r = 0; r < 4; r++) {
        raw_pack_f24(m.r[r].c, c24 + 3 * r);
      }
      raw_end(raw_unif(raw_ptr(), g.ps->uloc_env_clip, c24, 12, 0));
    } else if (raw) {
      raw_end(raw_unif(raw_ptr(), g.ps->uloc_env_clip, (const u32*)&m, 16, 1));
    } else {
      C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g.ps->uloc_env_clip, &m);
    }
    memcpy(g.last_env_clip, clip, sizeof(g.last_env_clip));
  }
  if (dd && g.last_env_palette_count == palette_count && bones_id &&
      g.last_env_bones_id == bones_id) {
    g.cur.cmd_dedup_words += (u32)(12 * palette_count) + 6;
  } else {
    if (raw && palette_count > 0 && s_skin_f24) {
      u32 tmp[9 * CTR_MAX_PALETTE];
      raw_skin_rows_f24(g.ps->uloc_env_rows,
                        skin_rows_words(bones, bones_f24, palette_count, tmp), palette_count);
    } else if (raw && palette_count > 0) {
      raw_skin_rows(g.ps->uloc_env_rows, bones, palette_count);
    } else {
      for (int row = 0; row < 3; row++) {
        C3D_FVec* dst =
            C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, g.ps->uloc_env_rows[row], palette_count);
        for (int p = 0; p < palette_count; p++) {
          const float* src = bones + 12 * p + 4 * row;
          dst[p].x = src[0];
          dst[p].y = src[1];
          dst[p].z = src[2];
          dst[p].w = src[3];
        }
      }
    }
    g.last_env_bones_id = bones_id;
    g.last_env_palette_count = palette_count;
  }
  /* the texture stage multiplies merc colors by 4: half the fade gives texture * fade * 2 */
  if (dd && words_equal(g.last_env_fade, fade, 3)) {
    g.cur.cmd_dedup_words += 6;
  } else {
    if (raw) {
      /* (C3D_FVec order: w, z, y, x) */
      const float fv[4] = {0.5f, fade[2] * 0.5f, fade[1] * 0.5f, fade[0] * 0.5f};
      raw_end(raw_unif(raw_ptr(), g.ps->uloc_env_fade, (const u32*)fv, 4, 1));
    } else {
      C3D_FVUnifSet(GPU_VERTEX_SHADER, g.ps->uloc_env_fade, fade[0] * 0.5f, fade[1] * 0.5f,
                    fade[2] * 0.5f, 0.5f);
    }
    memcpy(g.last_env_fade, fade, sizeof(g.last_env_fade));
  }
  g.env_valid = 1;
  if (raw) {
    raw_draw_elements(ms->indices_pa + 2u * (u32)first_index, (u32)index_count);
  } else {
    apply_state_tint(state, 2, 0xffffffffu, 0);
    c3d_vertex_buffer(ms->verts_pa, ms->verts, 24, 6, 0x543210);
    C3D_DrawElements(GPU_TRIANGLES, index_count, C3D_UNSIGNED_SHORT, ms->indices + first_index);
  }
  g.cur.draws++;
  g.cur.triangles += index_count / 3;
}

void ctr_gpu_get_stats(ctr_gpu_stats* out) {
  *out = g.last;
}

/* ---------------------------------------------------------------------------------------------
 * timing and the render thread
 * --------------------------------------------------------------------------------------------- */

double ctr_gpu_time_ms(void) {
  return (double)svcGetSystemTick() / (double)(SYSCLOCK_ARM11 / 1000);
}

static struct {
  Thread thread;
  LightEvent start; /* one-shot: a job was submitted (or quit) */
  LightEvent idle;  /* sticky: no job running */
  ctr_gpu_job_fn fn;
  void* arg;
  volatile int quit;
  int running;
} as;

static void render_thread_main(void* unused) {
  (void)unused;
  /* (AI-assisted) a crash here was silent (black top screen, the game waiting forever) */
  ctr_thread_install_crash_handler();
  s_diag_render_thread = threadGetCurrent();
  for (;;) {
    s_diag_stage = "idle (waiting for the game's frame)";
    LightEvent_Wait(&as.start);
    if (as.quit) {
      break;
    }
    s_diag_stage = "job start";
    as.fn(as.arg);
    s_diag_frames++;
    LightEvent_Signal(&as.idle);
  }
  LightEvent_Signal(&as.idle);
}

int ctr_gpu_async_start(ctr_gpu_job_fn fn, void* arg) {
  if (as.running) {
    return 1;
  }
  bool is_new = false;
  APT_CheckNew3DS(&is_new);
  if (!is_new) {
    return 0;
  }
  as.fn = fn;
  as.arg = arg;
  as.quit = 0;
  LightEvent_Init(&as.start, RESET_ONESHOT);
  LightEvent_Init(&as.idle, RESET_STICKY);
  LightEvent_Signal(&as.idle);
  /* core 2 is free for applications on the New 3DS; the priority only matters against other
   * threads on that core */
  as.thread = threadCreate(render_thread_main, NULL, 64 * 1024, 0x2F, 2, false);
  if (!as.thread) {
    return 0;
  }
  as.running = 1;
  return 1;
}

static void diag_thread_main(void* unused) {
  (void)unused;
  u32 last = 0;
  for (int i = 1; i <= 60; i++) {
    svcSleepThread(1000000000LL);
    const u32 f = s_diag_frames;
    printf("[ctr] render alive %ds: frame %lu, last stage %s%s\n", i, (unsigned long)f,
           s_diag_stage, f == last ? " (no frame in the last second)" : "");
    last = f;
  }
}

void ctr_gpu_diag_start(void) {
  /* core 0 at a high priority: it sleeps nearly all the time, and must run while the render
   * thread (core 2) or the game thread spins */
  if (!threadCreate(diag_thread_main, NULL, 16 * 1024, 0x18, 0, true)) {
    printf("[ctr] render watchdog thread not started\n");
  }
}

void ctr_gpu_async_submit(void) {
  LightEvent_Wait(&as.idle);
  LightEvent_Clear(&as.idle);
  LightEvent_Signal(&as.start);
}

double ctr_gpu_async_wait(void) {
  if (!as.running) {
    return 0;
  }
  double t0 = ctr_gpu_time_ms();
  LightEvent_Wait(&as.idle);
  return ctr_gpu_time_ms() - t0;
}

void ctr_gpu_async_stop(void) {
  if (!as.running) {
    return;
  }
  LightEvent_Wait(&as.idle);
  as.quit = 1;
  LightEvent_Signal(&as.start);
  threadJoin(as.thread, U64_MAX);
  threadFree(as.thread);
  as.running = 0;
}

/* ---------------- fog for level meshes ---------------- */

/* (AI-assisted) The game's fog values -> the ocean's and shader_opt's (ctr_mesh_opt.v.pica): both fogs are affine in clip w
 * (game fog 1 + fog0[3] * clamp(fog0[0] - w, fog0[1], fog0[2]), fog0[3] <= 0; draw distance fog
 * (fog1[0] * w - fog1[1]) * fog1[2]), so the shader needs one mul and one add for both. */
static void mesh_fog_uniforms(const float fog0[4], const float fog1[4], float out0[4],
                              float out1[4]) {
  const float k = fog0[3];
  out0[0] = k; /* -(game fog) before its clamps = k * w - 1 - k * hvdf.w */
  out0[1] = fog1[0] * fog1[2];
  out0[2] = -1.0f - k * fog0[0];
  out0[3] = -fog1[1] * fog1[2];
  out1[0] = -(1.0f + k * fog0[1]); /* the clamps, as fog amounts */
  out1[1] = 1.0f + k * fog0[2];
  /* (AI-assisted) the fog is at least the lower clamp: kept >= 0 (ctr_mesh_opt_fa only clamps at 1;
   * the fog ramp gave 0 for anything below) */
  out1[1] = out1[1] < 0.0f ? 0.0f : out1[1];
  out1[2] = 0.0f;
  out1[3] = 0.0f;
}

void ctr_gpu_set_mesh_fog(const float fog0_in[4], const float fog1[4], uint8_t r, uint8_t gr,
                          uint8_t b) {
  /* (AI-assisted) the same inputs as the last call that set the uniforms (the level renderers of
   * one level, each frame): its values are in the registers (or go in at the next use of their
   * program, use_program), so only the ramp color, which ctr_gpu_ocean_full_draw also sets. No
   * new matrix upload for the fog rows, no uniform update. */
  if (g.fog_in_valid && g.fog_in_ps == (const void*)g.ps &&
      words_equal(g.fog_in0, fog0_in, 4) && words_equal(g.fog_in1, fog1, 4)) {
    fog_ramp_color(r, gr, b);
    return;
  }
  g.fog_in_valid = 1;
  g.fog_in_ps = g.ps;
  memcpy(g.fog_in0, fog0_in, sizeof(g.fog_in0));
  memcpy(g.fog_in1, fog1, sizeof(g.fog_in1));
  g.fog_gen = g.fog_gen + 1 ? g.fog_gen + 1 : 1;
  /* (AI-assisted) the game fog's clamps kept in 0..255: its amount stays in 0..1 (fog_alpha's
   * shaders only clamp at 1; the fog ramp gave the same for amounts outside 0..1) */
  float fog0[4];
  memcpy(fog0, fog0_in, sizeof(fog0));
  for (int i = 1; i <= 2; i++) {
    fog0[i] = fog0[i] < 0.0f ? 0.0f : (fog0[i] > 255.0f ? 255.0f : fog0[i]);
  }
  mesh_fog_uniforms(fog0, fog1, g.fog0, g.fog1);
  /* (AI-assisted) game fog = 1 + fog0[3] * clamp(.., fog0[1], fog0[2]): constant when the clamps
   * are equal (config.ini fog = off: 0, 255, 255, -1/255), and no fog when that constant is <= 0
   * (half a step of the fog ramp's 255 for rounding) */
  g.fog_df = fog0[1] == fog0[2] && 1.0f + fog0[3] * fog0[1] <= 0.5f / 255.0f;
  memcpy(g.mfog0, fog0, sizeof(g.mfog0));
  memcpy(g.mfog1, fog1, sizeof(g.mfog1));
  if (g.ps->opt) {
    /* (AI-assisted) shader_opt: new fog rows with the next matrix upload, the clamps now */
    g.last_mesh_valid = 0;
    if (is_mesh_prog(g.cur_prog)) {
      C3D_FVUnifSet(GPU_VERTEX_SHADER, g.ps->uloc_fog1, g.fog1[0], g.fog1[1], g.fog1[2],
                    g.fog1[3]);
      raw_note_unif();
    }
  } else if (is_mesh_prog(g.cur_prog)) {
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g.ps->uloc_fog0, g.mfog0[0], g.mfog0[1], g.mfog0[2], g.mfog0[3]);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g.ps->uloc_fog1, g.mfog1[0], g.mfog1[1], g.mfog1[2], g.mfog1[3]);
    raw_note_unif();
  } else if (g.cur_prog == PROG_OCEAN) {
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g.ps->uloc_ocean_fog0, g.fog0[0], g.fog0[1], g.fog0[2],
                  g.fog0[3]);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g.ps->uloc_ocean_fog1, g.fog1[0], g.fog1[1], g.fog1[2],
                  g.fog1[3]);
    raw_note_unif();
  }
  fog_ramp_color(r, gr, b);
}

/* (AI-assisted) the fog ramp texture's color (the ramp part of ctr_gpu_set_mesh_fog, also used by
 * ctr_gpu_ocean_full_draw). A new color goes into a copy of the texture that no draw the GPU may
 * still do reads: not one bound in this frame (the draws before keep their color; with overlap
 * the GPU is already on them) nor in the frame before (the pipeline: still on the GPU). Before,
 * a second change in a frame wrote over the copy of the first, so every draw of that frame got
 * the last color. A copy that has the color already is bound as it is. */
#define FOG_RAMP_COPIES 4
static struct {
  void* data;     /* NULL: not made yet */
  uint32_t rgb;   /* its color (0xffffffff: not written) */
  int last_frame; /* the last frame that may read it (the current copy: the current frame) */
} s_fog_ramp[FOG_RAMP_COPIES];
static int s_fog_ramp_cur = -1; /* the copy in g.fog_tex.data */
static int s_fog_ramp_full_logged;

static void fog_ramp_color(uint8_t r, uint8_t gr, uint8_t b) {
  uint32_t rgb = ((uint32_t)r << 16) | ((uint32_t)gr << 8) | b;
  /* (AI-assisted) fog_alpha's fog color: a combiner constant (no texture to rewrite or bind) */
  const u32 abgr = 0xff000000u | ((u32)b << 16) | ((u32)gr << 8) | (u32)r;
  if (abgr != g.fog_abgr) {
    g.fog_abgr = abgr;
    g.last_state_valid = 0;
  }
  if (!g.fog_tex_valid || rgb == g.fog_tex_rgb) {
    return;
  }
  if (s_fog_ramp_cur < 0) {
    s_fog_ramp[0].data = g.fog_tex.data; /* (C3D_TexInit's) */
    s_fog_ramp[0].rgb = 0xffffffffu;
    s_fog_ramp[0].last_frame = g.frame_no;
    for (int i = 1; i < FOG_RAMP_COPIES; i++) {
      s_fog_ramp[i].data = NULL;
      s_fog_ramp[i].rgb = 0xffffffffu;
      s_fog_ramp[i].last_frame = -1000;
    }
    s_fog_ramp_cur = 0;
  }
  const u32 total = C3D_TexCalcTotalSize(g.fog_tex.size, g.fog_tex.maxLevel);
  int pick = -1;
  for (int i = 0; i < FOG_RAMP_COPIES && pick < 0; i++) {
    if (s_fog_ramp[i].data && s_fog_ramp[i].rgb == rgb) {
      pick = i;
    }
  }
  int write = 0;
  for (int i = 0; i < FOG_RAMP_COPIES && pick < 0; i++) {
    if (i != s_fog_ramp_cur && s_fog_ramp[i].data && s_fog_ramp[i].last_frame < g.frame_no - 1) {
      pick = i;
      write = 1;
    }
  }
  for (int i = 0; i < FOG_RAMP_COPIES && pick < 0; i++) {
    if (!s_fog_ramp[i].data) {
      ctr_linear_lock();
      s_fog_ramp[i].data = linearAlloc(total);
      ctr_linear_unlock();
      if (s_fog_ramp[i].data) {
        pick = i;
        write = 1;
      }
      break;
    }
  }
  if (pick < 0) {
    /* every copy is read by a draw in flight (more than 3 colors in two frames, or no linear
     * memory): this draw keeps the current color */
    if (!s_fog_ramp_full_logged) {
      s_fog_ramp_full_logged = 1;
      printf("[ctr] fog ramp: no free copy for color %06lx, kept %06lx\n", (unsigned long)rgb,
             (unsigned long)g.fog_tex_rgb);
    }
    return;
  }
  /* the copy given up was read by this frame's draws so far */
  s_fog_ramp[s_fog_ramp_cur].last_frame = g.frame_no;
  s_fog_ramp[pick].last_frame = g.frame_no;
  s_fog_ramp_cur = pick;
  g.fog_tex.data = s_fog_ramp[pick].data;
  g.fog_tex_rgb = rgb;
  if (!write) {
    if (g.in_frame) {
      C3D_TexBind(1, &g.fog_tex);
    }
    g.last_state_valid = 0;
    return;
  }
  s_fog_ramp[pick].rgb = rgb;
  uint8_t* dst = (uint8_t*)g.fog_tex.data;
  for (int y = 0; y < 8; y++) {
    for (int x = 0; x < 64; x++) {
      uint32_t tile = (uint32_t)x / 8; /* one row of 8x8 tiles */
      uint32_t m = (uint32_t)((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) |
                              ((x & 4) << 2) | ((y & 4) << 3));
      uint8_t* p = dst + (tile * 64 + m) * 4;
      p[0] = (uint8_t)(x * 255 / 63); /* A B G R */
      p[1] = b;
      p[2] = gr;
      p[3] = r;
    }
  }
  cache_flush_frame(g.fog_tex.data, C3D_TexCalcTotalSize(g.fog_tex.size, g.fog_tex.maxLevel));
  if (g.in_frame) {
    C3D_TexBind(1, &g.fog_tex); /* clears the texture cache: the GPU must not use the old texels */
  }
  g.last_state_valid = 0;
}

/* ---------------- (AI-assisted) the GPU's own counters (config.ini gpu_profile) ----------------
 * Read through GSPGPU_ReadHWRegs / WriteHWRegs (offsets from 0x1EB00000: the GPU's external
 * registers 0x1EF00000 are 0x400000; the GSP accepts offsets below 0x420000 and up to 0x80 bytes,
 * and does the accesses itself, so gk.rsf needs no IO mapping for them). 3dbrew
 * GPU/External_Registers "Hotspot Profiling": 0x68 = interval length - 1 (high 16 bits) and
 * interval count (low; 0 = until the result FIFO is read, and the 16-bit counts restart once more
 * than 0xFFFF intervals went by), 1 to 0x64 starts it; the FIFO at 0x6C gives four words of two
 * 16-bit stage counts, and reading it ends a count-0 measurement. Each interval adds 1 to one
 * stage: the sum is the interval count. GBATEK "GPU Memory Control/Status Registers": read-only
 * traffic counters 0x70..0xBC (20 words, 80 bytes). Reviewed 2026-10-11 against both: offsets,
 * sizes and the FIFO's four 4-byte reads are right; the window has to stay under 16 s (65535
 * intervals of 65536 cycles), so CtrRenderer stops the sampling after 10 s
 * (ctr_gpu_hwprof_stop_stages) while the traffic counters cover the whole window. */
#define HWREG_HOTSPOT_ENABLE 0x400064u
#define HWREG_HOTSPOT_CONFIG 0x400068u
#define HWREG_HOTSPOT_FIFO 0x40006Cu
#define HWREG_TRAFFIC 0x400070u
static int s_hw_refused;
static u32 s_hw_traffic0[CTR_GPU_HW_TRAFFIC];
static int s_hw_frame0;
static double s_hw_t0;
static int s_hw_stages_done; /* stop_stages read them: s_hw_stage, s_hw_stage_ms */
static unsigned int s_hw_stage[8];
static double s_hw_stage_ms;

int ctr_gpu_hwprof_begin(void) {
  if (s_hw_refused) {
    return 1;
  }
  /* 65536 cycles per interval (0.24 ms at 268 MHz), count 0: until the FIFO is read (16 s at
   * most, see above) */
  const u32 config = 0xFFFFu << 16, off = 0, on = 1;
  Result r = GSPGPU_WriteHWRegs(HWREG_HOTSPOT_ENABLE, &off, 4);
  if (R_SUCCEEDED(r)) {
    r = GSPGPU_WriteHWRegs(HWREG_HOTSPOT_CONFIG, &config, 4);
  }
  if (R_SUCCEEDED(r)) {
    r = GSPGPU_ReadHWRegs(HWREG_TRAFFIC, s_hw_traffic0, sizeof(s_hw_traffic0));
  }
  if (R_SUCCEEDED(r)) {
    r = GSPGPU_WriteHWRegs(HWREG_HOTSPOT_ENABLE, &on, 4);
  }
  if (R_FAILED(r)) {
    s_hw_refused = 1;
    return r ? (int)r : -1;
  }
  s_hw_frame0 = g.frame_no;
  s_hw_t0 = ctr_gpu_time_ms();
  s_hw_stages_done = 0;
  return 0;
}

/* the FIFO: four reads of 4 bytes (it is one register), then the unit off */
static Result hw_read_stages(unsigned int stage[8]) {
  Result r = 0;
  for (int i = 0; i < 4 && R_SUCCEEDED(r); i++) {
    u32 w = 0;
    r = GSPGPU_ReadHWRegs(HWREG_HOTSPOT_FIFO, &w, 4);
    stage[2 * i] = w & 0xFFFFu;
    stage[2 * i + 1] = w >> 16;
  }
  const u32 off = 0;
  if (R_SUCCEEDED(r)) {
    r = GSPGPU_WriteHWRegs(HWREG_HOTSPOT_ENABLE, &off, 4);
  }
  return r;
}

int ctr_gpu_hwprof_stop_stages(void) {
  if (s_hw_refused) {
    return 1;
  }
  if (s_hw_stages_done) {
    return 0;
  }
  const Result r = hw_read_stages(s_hw_stage);
  if (R_FAILED(r)) {
    s_hw_refused = 1;
    return r ? (int)r : -1;
  }
  s_hw_stage_ms = ctr_gpu_time_ms() - s_hw_t0;
  s_hw_stages_done = 1;
  return 0;
}

int ctr_gpu_hwprof_end(ctr_gpu_hwprof* out) {
  memset(out, 0, sizeof(*out));
  if (s_hw_refused) {
    return 1;
  }
  u32 traffic[CTR_GPU_HW_TRAFFIC];
  Result r = GSPGPU_ReadHWRegs(HWREG_TRAFFIC, traffic, sizeof(traffic));
  if (R_SUCCEEDED(r) && !s_hw_stages_done) {
    r = hw_read_stages(s_hw_stage);
    s_hw_stage_ms = ctr_gpu_time_ms() - s_hw_t0;
  }
  if (R_FAILED(r)) {
    s_hw_refused = 1;
    return r ? (int)r : -1;
  }
  s_hw_stages_done = 0;
  memcpy(out->stage, s_hw_stage, sizeof(out->stage));
  for (int i = 0; i < CTR_GPU_HW_TRAFFIC; i++) {
    out->traffic[i] = traffic[i] - s_hw_traffic0[i];
  }
  out->frames = g.frame_no - s_hw_frame0;
  out->ms = ctr_gpu_time_ms() - s_hw_t0;
  out->stage_ms = s_hw_stage_ms;
  return 0;
}

void C3Di_DirtyUniforms(GPU_SHADER_TYPE type); /* citro3d (source/internal.h) */

/* (AI-assisted) config.ini pica_bench (ctr_gpu_bench.c) used citro3d with programs, textures and
 * state of its own: take it back before the bench frees them (ctr_gpu_bench.h) */
void ctr_gpu_bench_restore(void) {
  for (int i = 0; i < 3; i++) {
    C3D_TexBind(i, NULL);
  }
  for (int i = 0; i < 6; i++) {
    C3D_TexEnvInit(C3D_GetTexEnv(i)); /* citro3d's default: what the GPU layer starts from */
  }
  C3D_CullFace(GPU_CULL_NONE);
  C3D_EarlyDepthTest(false, GPU_EARLYDEPTH_GEQUAL, 0);
  C3D_StencilTest(false, GPU_ALWAYS, 0, 0xFF, 0);
  g.cur_prog = PROG_NONE; /* (AI-assisted) the bench's uniforms are in the registers */
  use_program(PROG_BASIC);
  /* everything sent again at the next draw, as citro3d does after the HOME Menu */
  __C3D_Context.flags |= BIT(2) | BIT(3) | BIT(4) | BIT(5) | BIT(6) | BIT(7) | BIT(8) | BIT(9) |
                         BIT(10) | BIT(11) | BIT(12) | (7u << 23) | (0x3Fu << 26);
  C3Di_DirtyUniforms(GPU_VERTEX_SHADER);
  s_raw.apt_epoch++;
  g.last_state_valid = 0;
  g.last_mesh_valid = 0;
  g.last_skin_valid = 0;
  g.skin_bl_valid = 0;
  g.bound_tex = NULL;
  g.proc_radial = -1;
}
