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
/* (AI-assisted) Envmapped ties in both games. */
extern const uint8_t ctr_etie_shbin[];
extern const size_t ctr_etie_shbin_size;
/* the draws whose texture has an alpha mask (c3l v23): ctr_skin_opt and ctr_etie with the mask's
 * texture coordinates */
extern const uint8_t ctr_skin_opt_mask_shbin[];
extern const size_t ctr_skin_opt_mask_shbin_size;
extern const uint8_t ctr_etie_fa_shbin[];
extern const size_t ctr_etie_fa_shbin_size;
/* Jak 1's wind ties (ctr_gpu_draw_mesh_wind), with shader_opt's set */
extern const uint8_t ctr_mesh_wind_shbin[];
extern const size_t ctr_mesh_wind_shbin_size;
/* the PS2's tie geomorph (ctr_gpu_draw_mesh_morph), with shader_opt's set */
extern const uint8_t ctr_mesh_morph_shbin[];
extern const size_t ctr_mesh_morph_shbin_size;
extern const uint8_t ctr_mesh_morph_fa_shbin[];
extern const size_t ctr_mesh_morph_fa_shbin_size;
/* the PS2's tfrag geomorph (ctr_gpu_draw_mesh_tmorph), with shader_opt's set */
extern const uint8_t ctr_tfrag_morph_shbin[];
extern const size_t ctr_tfrag_morph_shbin_size;
extern const uint8_t ctr_tfrag_morph_fa_shbin[];
extern const size_t ctr_tfrag_morph_fa_shbin_size;
/* the level mesh, merc and PS2 ocean programs (platform/3ds/shaders/ctr_*_opt.v.pica) */
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
void ctr_thread_install_crash_handler(void); /* platform/3ds/port */
int ctr_config_bool(const char* key, int def); /* platform/3ds/port */
static void gl_to_pica_check(void); /* (AI-assisted) */

/* (AI-assisted) hardware hang bisect: where the render thread is (ctr_gpu_diag_stage) and how
 * many frames it finished, logged once per second for the first 60 s by a watchdog thread, and
 * whenever it has no frame for 3 s (diag_stall_report) */
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
/* (AI-assisted, gpu-home) fn runs on the main thread after each aptMainLoop (ctr_main_loop) */
void ctr_port_set_apt_after(void (*fn)(void));
/* (AI-assisted, gpu-home) the HOME Menu / sleep gate (see apt_gate_hook) */
static void apt_gate_install(void);
static void apt_gate_remove(void);
static void apt_gate_enter(void);
static void apt_gate_leave(void);
static int gx_begin(void);
static void gx_end(void);
static int gx_lost(void);
static void raw_gas_flags_clear(void);
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
static int vram_room(size_t size) {
  return (size_t)vramSpaceFree() >= size + VRAM_RESERVE;
}
/* room for a texture citro3d allocates (worst case: 32 bits per texel, mip levels) */
static int linear_room_for_tex(int w, int h, int mipmapped) {
  const size_t bytes = (size_t)w * (size_t)h * 4u * (mipmapped ? 2u : 1u);
  return (size_t)linearSpaceFree() >= bytes + LINEAR_RESERVE;
}
#define linearAlloc safe_linear_alloc
#define vramAlloc safe_vram_alloc

/* (AI-assisted) gxCmdQueueWait's timeout is in system ticks (268 MHz), not nanoseconds */
#define GX_WAIT_MS(ms) ((s64)(ms) * (s64)(SYSCLOCK_ARM11 / 1000))

/* (AI-assisted) CPU data cache flushes for memory the GPU reads. Started as a title, the kernel
 * call GSP's FlushDataCache would make for us, without the IPC (gk.rsf allows it). From the
 * Homebrew Launcher the exheader isn't ours (a svc it doesn't allow raises an exception): GSP.
 * Any thread. */
static u32 s_flush_requests; /* statistics: GSP flush requests */
static u32 s_flush_svcs;     /* statistics: kernel flushes */
static int s_kernel_flush = -1;
int ctr_kernel_flush_cfg = 1; /* config.ini kernel_flush */
static inline int kernel_flush_on(void) {
  if (s_kernel_flush < 0) {
    s_kernel_flush = envIsHomebrew() || !ctr_kernel_flush_cfg ? 0 : 1;
  }
  return s_kernel_flush;
}
static void cache_flush_now(const void* addr, u32 size) {
  if (!size) {
    return;
  }
  if (kernel_flush_on()) {
    if (R_SUCCEEDED(svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)addr, size))) {
      s_flush_svcs++;
      return;
    }
    s_kernel_flush = 0;
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
#define MAX_POOLS 256
/* GPU command buffer: 2 MB of linear memory; a busy frame (3000 level draws) takes ~1 MB. A frame
 * that doesn't fit is flushed and waited for in the middle (cmd_room). */
#if defined(OG3DS_GAME_JAK2) && OG3DS_GAME_JAK2
/* (AI-assisted, j2-mem2) Jak II: 1.25 MB (the pipeline's halves 640 KB). Haven City frames
 * take ~190 KB of commands (Azahar, baseline C: 835-854 draws, 75k triangles). */
#define CMDBUF_BYTES (1280 * 1024)
#else
#define CMDBUF_BYTES (2 * 1024 * 1024)
#endif
/* command words kept free for the end of the frame, and needed for one draw with state changes */
#define CMD_RESERVE_WORDS 512
#define CMD_DRAW_WORDS 384
/* quads per ctr_gpu_draw_quads batch: 4 vertices each, u16 indices */
#define QUAD_MAX 4096
/* texture pool bytes copied to VRAM per frame (a bigger pool alone), and how long a pool stays
 * before one with a higher priority may take its place */
#define POOL_COPY_BYTES (256u * 1024u)
#define POOL_STAY_FRAMES 60

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
  int arena_off;      /* >= 0: the texels are in the reserved VRAM at this offset, not citro3d's */
  void* home;         /* arena_off >= 0: the texels in linear memory (the copy that is kept) */
} TexSlot;

typedef struct {
  int used;           /* 0 free, 1 used, 2 pending delete, 3 reserved (being filled), 4 pending
                       * delete held one more frame (process_pending_deletes) */
  int ready;          /* all texels written (ctr_gpu_pool_ready) */
  uint8_t* linear;    /* the pool's bytes, all of them, always */
  void* vram;         /* a copy of [0, vram_bytes) the GPU reads instead, or NULL */
  unsigned int bytes;
  unsigned int vram_bytes; /* whole textures from the start; less than bytes when VRAM is short */
  int priority;
  int no_room_frame;  /* last frame it didn't fit in VRAM (don't try again every frame) */
  int vram_frame;     /* the frame its copy to VRAM was queued */
  unsigned int vram_serial; /* which move into VRAM this copy is from (0: none) */
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
  u32 tail_off, tail_bytes; /* ctr_gpu_mesh_begin_tail / _ext_tail: at verts + tail_off */
  u32 col_off; /* ctr_gpu_mesh_split_colors: the colors' buffer at verts + col_off (0: none) */
  u32 lin_bytes; /* the linear heap's bytes of verts and indices (0: ext) */
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
  PROG_OCEAN_FULL, /* (AI-assisted) ctr_gpu_ocean_full_draw (ctr_ocean_full.v.pica) */
  PROG_OCEAN_FULL_CLIP, /* (AI-assisted) ctr_gpu_ocean_full_draw_clip (ctr_ocean_full_clip.v.pica) */
  PROG_MESH_FA, /* (AI-assisted) fog_alpha: level meshes with the fog in the color's alpha */
  /* (AI-assisted) shader_opt with the game fog off: PROG_MESH / PROG_MESH_FA with only the draw
   * distance fog (ctr_mesh_opt_df / ctr_mesh_opt_df_fa.v.pica; set 2 only) */
  PROG_MESH_DF,
  PROG_MESH_DF_FA,
  PROG_ETIE,      /* (AI-assisted, j2water2) ctr_gpu_etie_draw (ctr_etie.v.pica) */
  PROG_MESH_WIND, /* ctr_gpu_draw_mesh_wind (ctr_mesh_wind.v.pica, set 1 only) */
  PROG_SKIN_MASK, /* PROG_SKIN for textures with an alpha mask (on unit 2) */
  PROG_ETIE_FA,   /* PROG_ETIE for textures with an alpha mask (on unit 1) */
  PROG_MESH_MORPH,    /* ctr_gpu_draw_mesh_morph (ctr_mesh_morph.v.pica, set 1 only) */
  PROG_MESH_MORPH_FA, /* the same with the fog in the alpha (ctr_mesh_morph_fa.v.pica) */
  PROG_TFRAG_MORPH,    /* ctr_gpu_draw_mesh_tmorph (ctr_tfrag_morph.v.pica, set 1 only) */
  PROG_TFRAG_MORPH_FA, /* the same with the fog in the alpha (ctr_tfrag_morph_fa.v.pica) */
  PROG_CLIP_PZ, /* PROG_CLIP whose vertices' z is already the PICA's (ctr_gpu_draw_clip_pz) */
};
/* (AI-assisted) the level mesh programs (one vertex layout, the same uniforms) */
static inline int is_mesh_prog(int prog) {
  return prog == PROG_MESH || prog == PROG_MESH_FA || prog == PROG_MESH_DF ||
         prog == PROG_MESH_DF_FA || prog == PROG_MESH_WIND || prog == PROG_MESH_MORPH ||
         prog == PROG_MESH_MORPH_FA || prog == PROG_TFRAG_MORPH || prog == PROG_TFRAG_MORPH_FA;
}

/* (AI-assisted) The vertex programs and their uniforms' places, one shbin per program. */
typedef struct {
  shaderProgram_s basic, mesh, mesh_fa, skin, env, clip, clip2, ocean_full, ocean_full_clip;
  shaderProgram_s mesh_df, mesh_df_fa; /* (AI-assisted) set 1: PROG_MESH_DF, PROG_MESH_DF_FA */
  int ok;      /* loaded */
  /* mesh, mesh_fa, skin, env, ocean_full and ocean_full_clip are the ctr_*_opt programs, whose
   * uniforms carry factors the shaders don't apply: the mesh matrix has two fog rows after it
   * (mesh_fog_rows), the skin programs' clip has its x, y, z columns / 255 (unscaled weights) and
   * skin's light colors the vertex color scale */
  int df_ok;   /* (AI-assisted) set 1: mesh_df and mesh_df_fa loaded, their uniforms in place */
  int fa_ok;   /* mesh_fa usable (fog_alpha) */
  int uloc_projection, uloc_clip, uloc_scales, uloc_fog0, uloc_fog1;
  int uloc_skin_clip, uloc_skin_rows[3], uloc_skin_scales, uloc_skin_lights;
  int uloc_skin_mfog; /* (AI-assisted, far-fog) the skin program's fog (ctr_skin_opt; -1: none) */
  int uloc_env_clip, uloc_env_rows[3], uloc_env_scales, uloc_env_fade;
  int uloc_clip_glpica, uloc_clip2_glpica;
  int uloc_ocean_full_gs, uloc_ocean_full_clip_gs;
  /* (AI-assisted, j2water2) Jak II's envmapped ties (no vertexShader: not loaded) */
  shaderProgram_s etie;
  int uloc_etie_clip, uloc_etie_cam, uloc_etie_nrm, uloc_etie_scales, uloc_etie_fog0,
      uloc_etie_fog1;
  /* wind ties (set 1; no vertexShader: not loaded): the mesh uniforms at the mesh program's
   * places (wind_ok), then the slot rows */
  shaderProgram_s mesh_wind;
  int uloc_wind, wind_ok;
  /* the tie geomorph (set 1; no vertexShader: not loaded): the mesh uniforms at the mesh program's
   * places (morph_ok), then the rows */
  shaderProgram_s mesh_morph, mesh_morph_fa;
  int uloc_morph, morph_ok;
  /* the tfrag geomorph (set 1; no vertexShader: not loaded): the mesh uniforms at the mesh
   * program's places (tmorph_ok), then tband, tscale */
  shaderProgram_s tfrag_morph, tfrag_morph_fa;
  int uloc_tband, tmorph_ok;
  /* the alpha mask programs (c3l v23), their uniforms at skin's / etie's places */
  shaderProgram_s skin_mask, etie_fa;
  int mask_ok, etie_fa_ok;
} ProgSet;
static ProgSet s_ps;

static struct {
  int ready;
  C3D_RenderTarget* top;
  ProgSet* ps;          /* (AI-assisted) the programs (s_ps) */
  DVLB_s* opt_dvlb[OPT_DVLBS]; /* (AI-assisted) the ctr_*_opt shbins */
  /* (AI-assisted) skin_bl_valid: the skin program's bones and lights (last_skin_bones_id /
   * last_skin_lights) are in its registers */
  int skin_bl_valid;
  DVLB_s* dvlb;
  u32 fog_abgr; /* the fog color as a combiner constant (fog_alpha's stage 1), R in the low byte */
  float fog0[4], fog1[4]; /* see ctr_gpu_set_mesh_fog, mesh_fog_uniforms */
  /* (AI-assisted) changes with fog0 (never 0): ctr_mesh_matrix::fog_gen; and the inputs of the
   * last ctr_gpu_set_mesh_fog that changed the uniforms (fog_in_valid: there was one) */
  uint32_t fog_gen;
  int fog_in_valid;
  const void* fog_in_ps;
  float fog_in0[4], fog_in1[4];
  int fog_df; /* (AI-assisted) the game fog is a constant 0: only the draw distance fog is left */
  C3D_Tex fog_tex;        /* 64x8: alpha ramp 0..1 along s, fog color */
  uint32_t fog_tex_rgb;
  int fog_tex_valid;
  DVLB_s* clip_dvlb; /* immediate draws in clip space (ctr_clip.v.pica) */
  DVLB_s* clip2_dvlb; /* the same with two texture coordinates (ctr_clip2.v.pica: clouds) */
  DVLB_s* etie_dvlb;      /* (AI-assisted, j2water2) ctr_etie.v.pica (Jak II) */
  DVLB_s* wind_dvlb;      /* ctr_mesh_wind.v.pica */
  DVLB_s* morph_dvlb[2];  /* ctr_mesh_morph(_fa).v.pica */
  DVLB_s* tmorph_dvlb[2]; /* ctr_tfrag_morph(_fa).v.pica */
  u32 tmorph_unif_id;     /* the tfrag geomorph uniforms the GPU has (ctr_gpu_draw_mesh_tmorph) */
  /* ctr_skin_opt_mask, ctr_etie_fa.v.pica */
  DVLB_s* skin_mask_dvlb;
  DVLB_s* etie_fa_dvlb;
  u32 wind_rows_id;       /* the slot rows the GPU has (ctr_gpu_draw_mesh_wind), 0: none */
  u32 morph_slot_id[CTR_MORPH_ROWS]; /* each row's rows_id on the GPU (ctr_gpu_draw_mesh_morph) */
  /* the etie program's uniforms are those of this matrix, texture coordinate scale and fog */
  int etie_valid;
  u32 etie_clip_id, etie_fog_gen;
  float etie_st_scale;
  int ocean_tex2_bound; /* texture unit 2 holds the ocean's shimmer texture */
  /* combiner stage 2 isn't pass-through: 1 a fog stage (ocean, merc fog) or Jak II's ocean env /
   * etie stage, 2 the PS2 ocean's. (AI-assisted) Only says "reset it": the merc fog draws commit
   * their stage every time (commit_texenv compares) */
  int ocean_stage2;
  int etie_stage3;      /* (AI-assisted, j2water2) combiner stage 3 holds the envmapped ties' fog */
  C3D_Mtx ocean_full_gs; /* GS screen units (x, y 12.4, z 24 bits) -> PICA clip space at w = 1 */
  /* the open ctr_gpu_ocean_full_reserve: vertices and indices at these g.vbuf offsets */
  int ocean_full_open;
  size_t ocean_full_voff, ocean_full_ioff;
  int ocean_full_max_verts, ocean_full_max_indices;
  int cur_prog;
  /* (AI-assisted) the frame's clear color as a texture combiner constant (CTR_BLEND_OVER_CLEAR) */
  u32 clear_abgr;
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
  float merc_haze_k, merc_haze_c; /* (AI-assisted, mercsprite) ctr_gpu_set_merc_haze */
  uint32_t merc_fog_gen, skin_mfog_gen;
  float skin_wrow[4];
  /* (AI-assisted) the last ctr_gpu_draw_skinned_env's matrix and bones (env_valid: still in the
   * env program's registers) */
  int env_valid;
  int last_env_palette_count;
  float last_env_clip[16];
  float last_env_fade[3];
  uint32_t last_env_bones_id; /* (AI-assisted) as last_skin_bones_id */
  C3D_Mtx gl_to_pica;
  C3D_Mtx gl_to_pica_pz; /* gl_to_pica with z passed through (PROG_CLIP_PZ) */
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
  u32* cmd_base; /* the command buffer at C3D_FrameBegin (or at the last cmd_room flush) */
  u32 cmd_flushed_words; /* the frame's command words before the last cmd_room flush */
  u32 bound_param, bound_lod;
  int frame_no;
  ctr_gpu_stats cur, last;
  int in_frame;
  int pending_delete[MAX_TEXTURES];
  int pending_delete_count;
  void* pending_staging[MAX_STAGING]; /* linear buffers of queued VRAM texture copies */
  int pending_staging_count;
  int vram_textures;
  int vram_copy_failures;
  int screen_tex; /* texture slot of ctr_gpu_copy_screen (-1: not made yet), 256x512 RGBA8 */
  int screen_tex_vram; /* (AI-assisted) it is in VRAM (a kept copy that found no linear memory) */
  int screen_tex_pinned; /* ctr_gpu_screen_copy_pin */
  char screenshot_path[256];
  int screenshot_state;  /* 0 none, 1 requested, 2 frame rendered: write at next frame begin */
} g;

/* (AI-assisted) steady frame pacing, see pres_vblank */
static int g_steady_pacing;
static int pres_init(void);


#define TOP_W 240
#define TOP_H 400

/* the viewport: the screen's 240x400 (the whole render target in 8x8 mode) */
static void set_top_viewport(void) {
  C3D_SetViewport(0, 0, TOP_W, TOP_H);
}

/* The previous frame is still in the render target's color buffer (VRAM, tiled RGBA8 or RGB565,
 * 240x400 rotated) at the start of the next frame, after the GPU finished it. Write it as a BMP.
 */
static void write_screenshot(void) {
  const int fw = 240, fh = 400;  /* render buffer dimensions */
  const uint8_t* src = (const uint8_t*)g.top->frameBuf.colorBuf;
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
/* (AI-assisted, streammem) as many free ranges as the arena can have (every other granule free):
 * with 128 a fragmented arena lost each freed range that had no free neighbour, for good (compact
 * textures then went to linear memory) */
#define ARENA_MAX_FREE (ARENA_BYTES / ARENA_ALIGN / 2 + 1)
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

/* the texels of a slot: freed (or given back to the reserved VRAM) */
static void tex_slot_free_texels(TexSlot* t) {
  if (t->arena_off >= 0) {
    arena_free((u32)t->arena_off, C3D_TexCalcTotalSize(t->tex.size, t->tex.maxLevel));
    t->arena_off = -1;
    linearFree(t->home);
    t->home = NULL;
    t->tex.data = NULL;
  } else if (t->pool < 0) {
    C3D_TexDelete(&t->tex);
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
/* c3l v23 alpha masks (ctr_gpu_tex_set_mask): a texture's mask + 1 and a mask's
 * texture + 1 (0: none) */
static int16_t s_tex_mask[MAX_TEXTURES], s_mask_of[MAX_TEXTURES];
/* the masks units 1 and 2 hold (unit 1: NULL while it has the fog ramp) */
static const C3D_Tex* s_tex1_mask;
static const C3D_Tex* s_tex2_mask;
/* every unit-1 bind but bind_mask's: unit 1 no longer holds a mask */
static void tex1_bind(C3D_Tex* t) {
  C3D_TexBind(1, t);
  s_tex1_mask = NULL;
}
/* units 1 and 2 emptied outside the draws (self-test, bench): no cached binding holds */
static void tex_units_forget(void) {
  s_tex1_mask = NULL;
  s_tex2_mask = NULL;
  g.ocean_tex2_bound = 0;
}
void ctr_gpu_tex_set_mask(int handle, int mask) {
  if (handle < 0 || handle >= MAX_TEXTURES) {
    return;
  }
  ctr_linear_lock();
  if (s_tex_mask[handle]) {
    s_mask_of[s_tex_mask[handle] - 1] = 0;
  }
  s_tex_mask[handle] = 0;
  if (mask >= 0 && mask < MAX_TEXTURES && mask != handle) {
    s_tex_mask[handle] = (int16_t)(mask + 1);
    s_mask_of[mask] = (int16_t)(handle + 1);
  }
  ctr_linear_unlock();
}
/* the mask of texture t when it is ready (-1: none) */
static inline int tex_mask(int t) {
  const int m = t >= 0 && t < MAX_TEXTURES ? s_tex_mask[t] - 1 : -1;
  return m >= 0 && g.textures[m].used == 1 ? m : -1;
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
      g.meshes[i].col_off = 0;
      g.meshes[i].lin_bytes = 0;
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
static void process_pending_deletes(int hold) {
  ctr_linear_lock();
  for (int i = 0; i < g.pending_staging_count; i++) {
    linearFree(g.pending_staging[i]);
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
                                : prog == PROG_CLIP_PZ  ? &g.ps->clip
                                : prog == PROG_CLIP2    ? &g.ps->clip2
                                : prog == PROG_OCEAN_FULL ? &g.ps->ocean_full
                                : prog == PROG_OCEAN_FULL_CLIP ? &g.ps->ocean_full_clip
                                : prog == PROG_SKIN     ? &g.ps->skin
                                : prog == PROG_SKIN_ENV ? &g.ps->env
                                : prog == PROG_MESH_FA  ? &g.ps->mesh_fa
                                : prog == PROG_MESH_DF  ? &g.ps->mesh_df
                                : prog == PROG_MESH_DF_FA ? &g.ps->mesh_df_fa
                                : prog == PROG_ETIE ? &g.ps->etie
                                : prog == PROG_MESH_WIND ? &g.ps->mesh_wind
                                : prog == PROG_MESH_MORPH ? &g.ps->mesh_morph
                                : prog == PROG_MESH_MORPH_FA ? &g.ps->mesh_morph_fa
                                : prog == PROG_TFRAG_MORPH ? &g.ps->tfrag_morph
                                : prog == PROG_TFRAG_MORPH_FA ? &g.ps->tfrag_morph_fa
                                : prog == PROG_SKIN_MASK ? &g.ps->skin_mask
                                : prog == PROG_ETIE_FA ? &g.ps->etie_fa
                                                        : &g.ps->mesh;
    const void* dvlp = sp->vertexShader ? (const void*)sp->vertexShader->dvle->dvlp : NULL;
    g.cur.prog_switches++;
    g.cur.prog_uploads += dvlp != last_dvlp;
    last_dvlp = dvlp;
  }
  g.env_valid = 0; /* (AI-assisted) another program: the env uniforms' registers are reused */
  g.etie_valid = 0;
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
  } else if (prog == PROG_CLIP || prog == PROG_CLIP2 || prog == PROG_CLIP_PZ) {
    C3D_BindProgram(prog == PROG_CLIP2 ? &g.ps->clip2 : &g.ps->clip);
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
                     prog == PROG_CLIP2 ? g.ps->uloc_clip2_glpica : g.ps->uloc_clip_glpica,
                     prog == PROG_CLIP_PZ ? &g.gl_to_pica_pz : &g.gl_to_pica);
    unif_vectors += 4;
  } else if (prog == PROG_ETIE || prog == PROG_ETIE_FA) {
    /* (AI-assisted, j2water2) the static mesh layout + the normals (attribute buffer 1) */
    C3D_BindProgram(prog == PROG_ETIE ? &g.ps->etie : &g.ps->etie_fa);
    g.last_state_valid = 0;
    AttrInfo_AddLoader(attr, 0, GPU_SHORT, 4);         /* position (+ pad) */
    AttrInfo_AddLoader(attr, 1, GPU_SHORT, 2);         /* texcoord */
    AttrInfo_AddLoader(attr, 2, GPU_UNSIGNED_BYTE, 4); /* color */
    AttrInfo_AddLoader(attr, 3, GPU_BYTE, 4);          /* normal (+ 0) */
  } else if (prog == PROG_OCEAN_FULL || prog == PROG_OCEAN_FULL_CLIP) {
    /* (AI-assisted) 40 bytes: GS x, y, z, q (or W * (x, y, z, 1)); s, t (times q), fog; color;
     * (j1ocean) the clip program also takes es, et (the sky reflection's texture coordinates) */
    C3D_BindProgram(prog == PROG_OCEAN_FULL ? &g.ps->ocean_full : &g.ps->ocean_full_clip);
    g.last_state_valid = 0;
    AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 4);         /* x, y, z, q / w */
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 3);         /* s, t, fog */
    AttrInfo_AddLoader(attr, 2, GPU_UNSIGNED_BYTE, 4); /* color */
    /* (AI-assisted, j1hang) es, et for both programs (ctr_ocean_full.v.pica doesn't read them):
     * the attributes loaded add up to the 40 byte stride. With 3 (32 bytes in a 40 byte stride)
     * the GS ocean draws hung the real 3DS's GPU at the title (Azahar takes any stride) */
    AttrInfo_AddLoader(attr, 3, GPU_FLOAT, 2); /* es, et */
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER,
                     prog == PROG_OCEAN_FULL ? g.ps->uloc_ocean_full_gs
                                             : g.ps->uloc_ocean_full_clip_gs,
                     &g.ocean_full_gs);
    unif_vectors += 4;
  } else if (prog == PROG_SKIN || prog == PROG_SKIN_ENV || prog == PROG_SKIN_MASK) {
    C3D_BindProgram(prog == PROG_SKIN        ? &g.ps->skin
                    : prog == PROG_SKIN_MASK ? &g.ps->skin_mask
                                             : &g.ps->env);
    g.last_state_valid = 0;
    AttrInfo_AddLoader(attr, 0, GPU_SHORT, 3);         /* position */
    AttrInfo_AddLoader(attr, 1, GPU_UNSIGNED_BYTE, 3); /* bone indices */
    AttrInfo_AddLoader(attr, 2, GPU_UNSIGNED_BYTE, 3); /* weights */
    AttrInfo_AddLoader(attr, 3, GPU_SHORT, 2);         /* texcoord * 1024 */
    AttrInfo_AddLoader(attr, 4, GPU_UNSIGNED_BYTE, 4); /* color */
    AttrInfo_AddLoader(attr, 5, GPU_BYTE, 3);          /* normal * 127 */
    C3D_FVUnifSet(GPU_VERTEX_SHADER,
                  prog == PROG_SKIN_ENV ? g.ps->uloc_env_scales : g.ps->uloc_skin_scales,
                  1.0f / 1024.0f, 1.0f / 255.0f, 1.0f, 1.0f / 127.0f);
    unif_vectors += 1;
  } else {
    /* PROG_MESH, (AI-assisted) or PROG_MESH_FA, PROG_MESH_DF(_FA): the same vertex layout and
     * uniforms */
    C3D_BindProgram(prog == PROG_MESH_FA      ? &g.ps->mesh_fa
                    : prog == PROG_MESH_DF    ? &g.ps->mesh_df
                    : prog == PROG_MESH_DF_FA ? &g.ps->mesh_df_fa
                    : prog == PROG_MESH_WIND  ? &g.ps->mesh_wind
                    : prog == PROG_MESH_MORPH ? &g.ps->mesh_morph
                    : prog == PROG_MESH_MORPH_FA ? &g.ps->mesh_morph_fa
                    : prog == PROG_TFRAG_MORPH ? &g.ps->tfrag_morph
                    : prog == PROG_TFRAG_MORPH_FA ? &g.ps->tfrag_morph_fa
                                              : &g.ps->mesh);
    g.last_state_valid = 0;
    AttrInfo_AddLoader(attr, 0, GPU_SHORT, 4);         /* position (+ pad) */
    AttrInfo_AddLoader(attr, 1, GPU_SHORT, 2);         /* texcoord * 1024 */
    AttrInfo_AddLoader(attr, 2, GPU_UNSIGNED_BYTE, 4); /* color */
    if (prog == PROG_MESH_WIND) {
      AttrInfo_AddLoader(attr, 3, GPU_UNSIGNED_BYTE, 1); /* wind slot */
      g.wind_rows_id = 0; /* (a switch keeps no uniforms) */
    }
    if (prog == PROG_MESH_MORPH || prog == PROG_MESH_MORPH_FA) {
      AttrInfo_AddLoader(attr, 3, GPU_BYTE, 4); /* c3l::MorphVertex delta, row */
      AttrInfo_AddLoader(attr, 4, GPU_BYTE, 4); /* delta2, pad */
      memset(g.morph_slot_id, 0, sizeof(g.morph_slot_id));
    }
    if (prog == PROG_TFRAG_MORPH || prog == PROG_TFRAG_MORPH_FA) {
      AttrInfo_AddLoader(attr, 3, GPU_BYTE, 4);          /* c3l::TfragMorphVertex band0, color0 */
      AttrInfo_AddLoader(attr, 4, GPU_BYTE, 4);          /* band1, color1 */
      AttrInfo_AddLoader(attr, 5, GPU_UNSIGNED_BYTE, 4); /* m */
      AttrInfo_AddLoader(attr, 6, GPU_UNSIGNED_BYTE, 4); /* t */
      g.tmorph_unif_id = 0;
    }
    /* (AI-assisted) skin's value (w = 1/127: no mesh shader reads w) */
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g.ps->uloc_scales, 1.0f / 1024.0f, 1.0f / 255.0f, 1.0f,
                  1.0f / 127.0f);
    unif_vectors += 1;
    g.mesh_st_atlas = 0; /* (AI-assisted) scales.x is 1/1024 */
    /* (AI-assisted) the fog clamps (mesh_fog_uniforms' form); the fog factors are in the
     * matrix's fog rows (mesh_fog_rows) */
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g.ps->uloc_fog1, g.fog1[0], g.fog1[1], g.fog1[2], g.fog1[3]);
  }
  g.cur.prog_unif_vectors += unif_vectors;
  g.cur_prog = prog;
}

static float s_gs_half_height = 112.0f; /* ctr_gpu_set_gs_half_height */

static void setup_projection(void);

void ctr_gpu_set_gs_half_height(float lines) {
  if (lines > 0.0f && lines != s_gs_half_height) {
    s_gs_half_height = lines;
    if (g.ready) {
      setup_projection();
    }
  }
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
  g.gl_to_pica_pz = g.gl_to_pica;
  g.gl_to_pica_pz.r[2].z = 1.0f;
  g.gl_to_pica_pz.r[2].w = 0.0f;

  /* (AI-assisted) the ocean's GS vertices -> g.projection's space like CtrDirect: x = (X / 16 -
   * 2048) / 256, y = (2048 - Y / 16) / h (h: the field's half height, 112 for Jak 1's 512 x 224,
   * 208 for Jak II's 512 x 416), z = Z / (2^24 - 1) */
  C3D_Mtx gs_to_ctr;
  Mtx_Identity(&gs_to_ctr);
  gs_to_ctr.r[0].x = 1.0f / 4096.0f;
  gs_to_ctr.r[0].w = -8.0f;
  gs_to_ctr.r[1].y = -1.0f / (16.0f * s_gs_half_height);
  gs_to_ctr.r[1].w = 2048.0f / s_gs_half_height;
  gs_to_ctr.r[2].z = 1.0f / 16777215.0f;
  Mtx_Multiply(&g.ocean_full_gs, &g.projection, &gs_to_ctr);
}

/* (AI-assisted) VRAM bank of a VRAM address: 0 = A (0x18000000..0x182FFFFF), 1 = B */
static int vram_bank(const void* p) {
  return osConvertVirtToPhys(p) >= 0x18300000u;
}

/* The top screen's render target: 240x400, RGBA8 / RGB565 color, D24S8
 * depth. citro3d puts the depth buffer in the other VRAM bank when it fits,
 * else silently in the same one: then color and depth traffic share one bank. (AI-assisted) */
static C3D_RenderTarget* create_top_target(void) {
  const int w = TOP_W, h = TOP_H;
  C3D_RenderTarget* t = C3D_RenderTargetCreate(w, h, GPU_RB_RGBA8,
                                               GPU_RB_DEPTH24_STENCIL8);
  if (!t) {
    return NULL;
  }
  C3D_FrameBuf* fb = &t->frameBuf;
  C3D_FrameBufAttrib(fb, (u16)w, (u16)h, false);
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
                  "%dx%d, color %s at 0x%08lx (VRAM %c), depth %s at 0x%08lx (VRAM %c)%s",
                  fb->width, fb->height,
                  "RGBA8", (unsigned long)c, vram_bank(fb->colorBuf) ? 'B' : 'A',
                  fb->depthFmt == GPU_RB_DEPTH16 ? "D16" : "D24S8", (unsigned long)d,
                  vram_bank(fb->depthBuf) ? 'B' : 'A',
                  vram_bank(fb->colorBuf) == vram_bank(fb->depthBuf) ? " SAME BANK" : "");
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
  p->uloc_ocean_full_gs = shaderInstanceGetUniformLocation(p->ocean_full.vertexShader, "gs");
  p->uloc_ocean_full_clip_gs =
      shaderInstanceGetUniformLocation(p->ocean_full_clip.vertexShader, "gs");
  if (p->etie.vertexShader) {
    /* (AI-assisted, j2water2) */
    vs = p->etie.vertexShader;
    p->uloc_etie_clip = shaderInstanceGetUniformLocation(vs, "clip");
    p->uloc_etie_cam = shaderInstanceGetUniformLocation(vs, "cam");
    p->uloc_etie_nrm = shaderInstanceGetUniformLocation(vs, "nrm");
    p->uloc_etie_scales = shaderInstanceGetUniformLocation(vs, "scales");
    p->uloc_etie_fog0 = shaderInstanceGetUniformLocation(vs, "fog0");
    p->uloc_etie_fog1 = shaderInstanceGetUniformLocation(vs, "fog1");
  }
  /* the alpha mask programs: usable when their uniforms are at the same places */
  p->mask_ok = 0;
  if (p->skin_mask.vertexShader) {
    vs = p->skin_mask.vertexShader;
    p->mask_ok = shaderInstanceGetUniformLocation(vs, "clip") == p->uloc_skin_clip &&
                 shaderInstanceGetUniformLocation(vs, "row0") == p->uloc_skin_rows[0] &&
                 shaderInstanceGetUniformLocation(vs, "row1") == p->uloc_skin_rows[1] &&
                 shaderInstanceGetUniformLocation(vs, "row2") == p->uloc_skin_rows[2] &&
                 shaderInstanceGetUniformLocation(vs, "scales") == p->uloc_skin_scales &&
                 shaderInstanceGetUniformLocation(vs, "lights") == p->uloc_skin_lights &&
                 shaderInstanceGetUniformLocation(vs, "mfog") == p->uloc_skin_mfog;
  }
  p->etie_fa_ok = 0;
  if (p->etie_fa.vertexShader && p->etie.vertexShader) {
    vs = p->etie_fa.vertexShader;
    p->etie_fa_ok = shaderInstanceGetUniformLocation(vs, "clip") == p->uloc_etie_clip &&
                    shaderInstanceGetUniformLocation(vs, "cam") == p->uloc_etie_cam &&
                    shaderInstanceGetUniformLocation(vs, "nrm") == p->uloc_etie_nrm &&
                    shaderInstanceGetUniformLocation(vs, "scales") == p->uloc_etie_scales &&
                    shaderInstanceGetUniformLocation(vs, "fog0") == p->uloc_etie_fog0 &&
                    shaderInstanceGetUniformLocation(vs, "fog1") == p->uloc_etie_fog1;
  }
}

static void prog_set_free(ProgSet* p) {
  if (!p->ok) {
    return;
  }
  shaderProgram_s* all[] = {&p->basic, &p->mesh, &p->skin, &p->env, &p->clip,
                            &p->clip2, &p->ocean_full, &p->ocean_full_clip};
  for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
    shaderProgramFree(all[i]);
  }
  shaderProgram_s* maybe[] = {&p->mesh_fa,   &p->mesh_df,       &p->mesh_df_fa,
                              &p->etie,      &p->mesh_wind,     &p->skin_mask,  &p->etie_fa,
                              &p->mesh_morph, &p->mesh_morph_fa, &p->tfrag_morph,
                              &p->tfrag_morph_fa};
  for (size_t i = 0; i < sizeof(maybe) / sizeof(maybe[0]); i++) {
    if (maybe[i]->vertexShader) {
      shaderProgramFree(maybe[i]);
    }
  }
  memset(p, 0, sizeof(*p));
}

/* (AI-assisted) the ctr_*_opt programs and the ones that build on them, after basic, clip, clip2,
 * ocean and the ties' programs; 0: they didn't load */
static int load_opt_set(void) {
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
      printf("[ctr] shaders: the ctr_*_opt programs didn't load\n");
      for (int k = 0; k < i; k++) {
        DVLB_Free(g.opt_dvlb[k]);
        g.opt_dvlb[k] = NULL;
      }
      return 0;
    }
  }
  ProgSet* p = &s_ps;
  struct {
    shaderProgram_s* prog;
    DVLE_s* dvle;
  } set[] = {
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
  g.skin_mask_dvlb =
      DVLB_ParseFile((u32*)ctr_skin_opt_mask_shbin, (u32)ctr_skin_opt_mask_shbin_size);
  if (g.skin_mask_dvlb) {
    shaderProgramInit(&p->skin_mask);
    shaderProgramSetVsh(&p->skin_mask, &g.skin_mask_dvlb->DVLE[0]);
  }
  g.wind_dvlb = DVLB_ParseFile((u32*)ctr_mesh_wind_shbin, (u32)ctr_mesh_wind_shbin_size);
  if (g.wind_dvlb) {
    shaderProgramInit(&p->mesh_wind);
    shaderProgramSetVsh(&p->mesh_wind, &g.wind_dvlb->DVLE[0]);
  }
  g.morph_dvlb[0] = DVLB_ParseFile((u32*)ctr_mesh_morph_shbin, (u32)ctr_mesh_morph_shbin_size);
  g.morph_dvlb[1] =
      DVLB_ParseFile((u32*)ctr_mesh_morph_fa_shbin, (u32)ctr_mesh_morph_fa_shbin_size);
  if (g.morph_dvlb[0] && g.morph_dvlb[1]) {
    shaderProgramInit(&p->mesh_morph);
    shaderProgramSetVsh(&p->mesh_morph, &g.morph_dvlb[0]->DVLE[0]);
    shaderProgramInit(&p->mesh_morph_fa);
    shaderProgramSetVsh(&p->mesh_morph_fa, &g.morph_dvlb[1]->DVLE[0]);
  }
  g.tmorph_dvlb[0] = DVLB_ParseFile((u32*)ctr_tfrag_morph_shbin, (u32)ctr_tfrag_morph_shbin_size);
  g.tmorph_dvlb[1] =
      DVLB_ParseFile((u32*)ctr_tfrag_morph_fa_shbin, (u32)ctr_tfrag_morph_fa_shbin_size);
  if (g.tmorph_dvlb[0] && g.tmorph_dvlb[1]) {
    shaderProgramInit(&p->tfrag_morph);
    shaderProgramSetVsh(&p->tfrag_morph, &g.tmorph_dvlb[0]->DVLE[0]);
    shaderProgramInit(&p->tfrag_morph_fa);
    shaderProgramSetVsh(&p->tfrag_morph_fa, &g.tmorph_dvlb[1]->DVLE[0]);
  }
  prog_set_ulocs(p, 1);
  if (p->tfrag_morph.vertexShader && p->tfrag_morph_fa.vertexShader) {
    p->tmorph_ok = 1;
    const shaderProgram_s* tp[2] = {&p->tfrag_morph, &p->tfrag_morph_fa};
    p->uloc_tband = shaderInstanceGetUniformLocation(tp[0]->vertexShader, "tband");
    for (int i = 0; i < 2; i++) {
      shaderInstance_s* vs = tp[i]->vertexShader;
      p->tmorph_ok &= p->uloc_tband >= 0 &&
                      shaderInstanceGetUniformLocation(vs, "tband") == p->uloc_tband &&
                      shaderInstanceGetUniformLocation(vs, "tscale") == p->uloc_tband + 1 &&
                      shaderInstanceGetUniformLocation(vs, "clip") == p->uloc_clip &&
                      shaderInstanceGetUniformLocation(vs, "scales") == p->uloc_scales &&
                      shaderInstanceGetUniformLocation(vs, "fog1") == p->uloc_fog1;
    }
  }
  if (p->mesh_morph.vertexShader && p->mesh_morph_fa.vertexShader) {
    p->morph_ok = 1;
    const shaderProgram_s* mp[2] = {&p->mesh_morph, &p->mesh_morph_fa};
    p->uloc_morph = shaderInstanceGetUniformLocation(mp[0]->vertexShader, "morph");
    for (int i = 0; i < 2; i++) {
      shaderInstance_s* vs = mp[i]->vertexShader;
      p->morph_ok &= p->uloc_morph >= 0 &&
                     shaderInstanceGetUniformLocation(vs, "morph") == p->uloc_morph &&
                     shaderInstanceGetUniformLocation(vs, "clip") == p->uloc_clip &&
                     shaderInstanceGetUniformLocation(vs, "scales") == p->uloc_scales &&
                     shaderInstanceGetUniformLocation(vs, "fog1") == p->uloc_fog1;
    }
  }
  if (p->mesh_wind.vertexShader) {
    shaderInstance_s* vs = p->mesh_wind.vertexShader;
    p->uloc_wind = shaderInstanceGetUniformLocation(vs, "wind");
    p->wind_ok = p->uloc_wind >= 0 &&
                 shaderInstanceGetUniformLocation(vs, "clip") == p->uloc_clip &&
                 shaderInstanceGetUniformLocation(vs, "scales") == p->uloc_scales &&
                 shaderInstanceGetUniformLocation(vs, "fog1") == p->uloc_fog1;
  }
  p->df_ok = 1;
  const shaderProgram_s* df[2] = {&p->mesh_df, &p->mesh_df_fa};
  for (int i = 0; i < 2; i++) {
    p->df_ok &= shaderInstanceGetUniformLocation(df[i]->vertexShader, "clip") == p->uloc_clip &&
                shaderInstanceGetUniformLocation(df[i]->vertexShader, "scales") == p->uloc_scales;
  }
  p->ok = p->uloc_clip >= 0 && p->uloc_fog1 >= 0 && p->uloc_skin_clip >= 0 &&
          p->uloc_skin_lights >= 0 && p->uloc_env_clip >= 0;
  printf("[ctr] shaders: %s (alpha mask mesh program %s, game fog off programs %s)\n",
         p->ok ? "ready" : "uniforms missing", p->fa_ok ? "ready" : "off",
         p->df_ok ? "ready" : "off");
  return p->ok;
}

/* Boot check of the CPU cache cleaning the GPU depends on: data written on another core,
 * cleaned from this one by the method under test (0 kernel, 1 GSP), copied by the GPU, read back.
 * The writer spins on its core meanwhile: its lines are still in that core's cache.
 * Returns the words that differ (all: the test could not run). */
typedef struct {
  u32* buf;
  u32 words, seed;
  volatile int spin, written, release;
} FlushFill;
static void flush_fill(void* arg) {
  FlushFill* f = (FlushFill*)arg;
  for (u32 i = 0; i < f->words; i++) {
    f->buf[i] = (i * 2654435761u) ^ f->seed;
  }
  f->written = 1;
  while (f->spin && !f->release) {
  }
}
static u32 flush_check(int method, int other_core, u32 bytes, u32 seed) {
  const u32 words = bytes / 4;
  u32* src = (u32*)linearAlloc(bytes);
  u32* dst = (u32*)linearAlloc(bytes);
  u32 bad = words;
  if (src && dst) {
    memset(dst, 0, bytes);
    GSPGPU_FlushDataCache(dst, bytes);
    FlushFill f = {src, words, seed, other_core, 0, 0};
    s32 me = svcGetProcessorID();
    Thread t = other_core ? threadCreate(flush_fill, &f, 4096, 0x30, me == 0 ? 2 : 0, false) : NULL;
    if (t) {
      while (!f.written) {
        svcSleepThread(100000LL);
      }
    } else {
      f.spin = 0;
      flush_fill(&f);
    }
    if (method == 0) {
      svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)src, bytes);
    } else {
      GSPGPU_FlushDataCache(src, bytes);
    }
    C3D_SyncTextureCopy(src, 0, dst, 0, bytes, 8);
    svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32)dst, bytes);
    bad = 0;
    for (u32 i = 0; i < words; i++) {
      bad += dst[i] != ((i * 2654435761u) ^ seed);
    }
    if (t) {
      f.release = 1;
      threadJoin(t, U64_MAX);
      threadFree(t);
    }
  }
  linearFree(src);
  linearFree(dst);
  return bad;
}
static void flush_selftest(void) {
  static const char* const kName[2] = {"kernel", "GSP"};
  static const u32 kBytes[2] = {4096, 256 * 1024};
  u32 bad[2] = {0, 0};
  char txt[2][96];
  u32 seed = 0x9e3779b9u;
  for (int m = 0; m < 2; m++) {
    u32 r[4];
    for (int c = 0; c < 2; c++) {
      for (int z = 0; z < 2; z++) {
        r[c * 2 + z] = flush_check(m, c, kBytes[z], seed += 0x6d2b79f5u);
        bad[m] += r[c * 2 + z];
      }
    }
    snprintf(txt[m], sizeof(txt[m]), "%s: same core %lu/%lu, other core %lu/%lu words wrong",
             kName[m], (unsigned long)r[0], (unsigned long)r[1], (unsigned long)r[2],
             (unsigned long)r[3]);
  }
  const int was = kernel_flush_on();
  s_kernel_flush = was && bad[0] == 0;
  printf("[ctr] cache flush self-test (4 KB/256 KB, the writer still on its core): %s; %s -> %s\n",
         txt[0], txt[1],
         s_kernel_flush ? "kernel" : "GSP");
}

static int top_target_setup(void);
extern u32 __ctru_linear_heap_size;
static u32 s_lin_before; /* the linear heap's bytes in use when ctr_gpu_init began */
int ctr_gpu_init(void) {
  if (g.ready) {
    return 0;
  }
  ctr_linear_lock();
  s_lin_before = __ctru_linear_heap_size - (u32)linearSpaceFree();
  ctr_linear_unlock();
  if (!C3D_Init(CMDBUF_BYTES)) {
    return -1;
  }
  apt_gate_install(); /* (AI-assisted, gpu-home) after C3D_Init: its hook runs before citro3d's */
  g.top = create_top_target();
  if (!g.top) {
    return -2;
  }

  g.ps = &s_ps;
  g.dvlb = DVLB_ParseFile((u32*)ctr_basic_shbin, (u32)ctr_basic_shbin_size);
  shaderProgramInit(&g.ps->basic);
  shaderProgramSetVsh(&g.ps->basic, &g.dvlb->DVLE[0]);
  C3D_BindProgram(&g.ps->basic);

  /* no fog until ctr_gpu_set_mesh_fog (AI-assisted: the shader's form, see mesh_fog_uniforms) */
  g.fog0[0] = -1.0f / 255.0f;
  g.fog0[1] = 0.0f;
  g.fog0[2] = -1.0f;
  g.fog0[3] = 0.0f;
  g.fog1[0] = 0.0f;
  g.fog1[1] = 0.0f;
  g.fog1[2] = 0.0f;
  g.fog1[3] = 0.0f;
  g.fog_gen = 1; /* (AI-assisted) */
  g.merc_fog_gen = 1; /* (AI-assisted, far-fog) no merc fog until ctr_gpu_set_merc_fog */
  g.merc_fog_hvdf_w = 255.0f;
  g.merc_fog_lo = g.merc_fog_hi = 0.0f;
  g.merc_haze_k = g.merc_haze_c = 0.0f;
  g.fog_in_valid = 0;
  if (C3D_TexInit(&g.fog_tex, 64, 8, GPU_RGBA8)) {
    C3D_TexSetFilter(&g.fog_tex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&g.fog_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    g.fog_tex_valid = 1;
    g.fog_tex_rgb = 0xffffffff; /* force the first fill */
  }
  g.clip_dvlb = DVLB_ParseFile((u32*)ctr_clip_shbin, (u32)ctr_clip_shbin_size);
  shaderProgramInit(&g.ps->clip);
  shaderProgramSetVsh(&g.ps->clip, &g.clip_dvlb->DVLE[0]);
  g.clip2_dvlb = DVLB_ParseFile((u32*)ctr_clip2_shbin, (u32)ctr_clip2_shbin_size);
  shaderProgramInit(&g.ps->clip2);
  shaderProgramSetVsh(&g.ps->clip2, &g.clip2_dvlb->DVLE[0]);
  /* (AI-assisted) Both games' converted ties can carry v21 reflection data. */
  g.etie_dvlb = DVLB_ParseFile((u32*)ctr_etie_shbin, (u32)ctr_etie_shbin_size);
  if (g.etie_dvlb) {
    shaderProgramInit(&g.ps->etie);
    shaderProgramSetVsh(&g.ps->etie, &g.etie_dvlb->DVLE[0]);
  }
  /* the alpha mask programs (c3l v23) */
  g.etie_fa_dvlb = DVLB_ParseFile((u32*)ctr_etie_fa_shbin, (u32)ctr_etie_fa_shbin_size);
  if (g.etie_fa_dvlb) {
    shaderProgramInit(&g.ps->etie_fa);
    shaderProgramSetVsh(&g.ps->etie_fa, &g.etie_fa_dvlb->DVLE[0]);
  }
  if (!load_opt_set()) {
    return -5;
  }
  g.cur_prog = PROG_NONE;

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
    g.textures[i].arena_off = -1;
  }
  g.screen_tex = -1;
  setup_projection();
  use_program(PROG_BASIC);
  C3D_CullFace(GPU_CULL_NONE);
  ctr_port_set_gpu_active(1);
  flush_selftest();
  g.ready = 1;
  if (top_target_setup() != 0) {
    g.ready = 0;
    return -2;
  }
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
  apt_gate_remove(); /* (AI-assisted, gpu-home) */
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
  /* (AI-assisted) the programs, then their shbins */
  prog_set_free(&s_ps);
  for (int i = 0; i < OPT_DVLBS; i++) {
    if (g.opt_dvlb[i]) {
      DVLB_Free(g.opt_dvlb[i]);
      g.opt_dvlb[i] = NULL;
    }
  }
  DVLB_s* dvlbs[] = {g.dvlb,      g.clip_dvlb,  g.clip2_dvlb,   g.etie_dvlb,
                     g.wind_dvlb, g.skin_mask_dvlb, g.etie_fa_dvlb,
                     g.morph_dvlb[0], g.morph_dvlb[1], g.tmorph_dvlb[0], g.tmorph_dvlb[1]};
  for (size_t i = 0; i < sizeof(dvlbs) / sizeof(dvlbs[0]); i++) {
    if (dvlbs[i]) {
      DVLB_Free(dvlbs[i]);
    }
  }
  C3D_RenderTargetDelete(g.top);
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
static void raw_frame_reset(void);
static void clip_reserve_reset(void);
static int s_pipeline = 0;
/* (AI-assisted) 1 MB each: the busiest jungle frames on hardware use ~450 KB of commands; a frame
 * that still runs out of room is flushed and waited for in the middle (cmd_room). 2 MB each cost
 * the linear memory a level needs.
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
  u32 bytes;    /* one screen buffer (the render target's size) */
  u32 show_off; /* where the LCD starts reading in a buffer */
  int last_buf; /* the newest frame's buffer (-1 none) */
  volatile u32 vblanks;      /* counted by the vblank callback */
  volatile u32 next_release; /* set with each frame by the game thread (ctr_gpu_note_release) */
  u32 cur_release;           /* the frame being built */
  struct {
    u32 id, target;
    int buf;
    volatile int seen; /* (AI-assisted) the callback saw it done (its slack is counted) */
  } q[PRESENT_Q];
  volatile u32 q_head, q_tail;            /* render thread writes q_tail, the callback q_head */
  /* (AI-assisted, gpu-home) the render thread asks the callback to drop the waiting frames
   * (pres_queue_frame): the callback alone moves q_head (it walked stale slots for ~2^32 rounds
   * when the render thread set q_head in the middle of its loop) */
  volatile int drop_req;
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
  /* what each part drew, and the part's list run again with a one pixel scissor (its geometry:
   * vertices, clipping, setup; almost no fragments): fill = ms - geo_ms */
  double tris[CTR_GPU_PHASES], draws[CTR_GPU_PHASES], binds[CTR_GPU_PHASES];
  double geo_ms[CTR_GPU_PHASES];
  u32 geo_lists[CTR_GPU_PHASES], geo_skipped;
  int c_tris, c_draws, c_binds; /* g.cur's counts when the part began */
  u32* scis;                    /* 16 words in linear memory: the scissor lists (set, restore) */
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
  s_bd.scis = (u32*)linearAlloc(64);
  ctr_linear_unlock();
  if (!s_bd.probe || !s_bd.scis) {
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

static void bd_resend(void);

/* a list of 8 words at w: a framebuffer flush, the 3 scissor registers, the end */
static void bd_scissor_list(u32* w, u32 mode, u32 pos, u32 end) {
  w[0] = 1;
  w[1] = GPUREG_FRAMEBUFFER_FLUSH | (0xFu << 16);
  w[2] = mode;
  w[3] = GPUREG_SCISSORTEST_MODE | (0xFu << 16) | (2u << 20) | (1u << 31);
  w[4] = pos;
  w[5] = end;
  w[6] = 0x12345678;
  w[7] = GPUREG_FINALIZE | (0xFu << 16);
}

/* does the list write the scissor registers or the framebuffer's place (then it can't be run
 * again under another scissor) */
static int bd_list_sets_target(const u32* list, u32 words) {
  for (u32 i = 0; i + 1 < words;) {
    const u32 h = list[i + 1];
    const u32 id = h & 0xFFFF, extra = (h >> 20) & 0xFF, seq = h >> 31;
    const u32 last = seq ? id + extra : id;
    if ((id <= GPUREG_SCISSORTEST_DIM && last >= GPUREG_SCISSORTEST_MODE) ||
        (id <= GPUREG_FRAMEBUFFER_DIM && last >= GPUREG_DEPTHBUFFER_LOC)) {
      return 1;
    }
    i += (2 + extra + 1) & ~1u;
  }
  return 0;
}

static void bd_run(u32* list, u32 bytes) {
  const u32 n0 = s_bd.n[BD_P3D];
  GX_ProcessCommandList(list, bytes, 0);
  bd_wait(BD_P3D, n0, 1);
}

/* one command list alone on the GPU, timed for phase; then again with only the top left pixel
 * in the scissor (opaque draws write the same values there again) for its geometry's time */
static void bd_list(u32* list, u32 words, int phase) {
  bd_idle();
  cache_flush_now(list, words * 4);
  const u32 n0 = s_bd.n[BD_P3D];
  const u64 t0 = svcGetSystemTick();
  GX_ProcessCommandList(list, words * 4, 0);
  const u64 t1 = bd_wait(BD_P3D, n0, 1);
  s_bd.ms[phase] += ticks_ms(t1 - t0);
  s_bd.lists[phase]++;
  if (bd_list_sets_target(list, words)) {
    s_bd.geo_skipped++;
    return;
  }
  const C3Di_Context* ctx = &__C3D_Context;
  bd_scissor_list(s_bd.scis, GPU_SCISSOR_NORMAL, 0, 0);
  bd_scissor_list(s_bd.scis + 8, ctx->scissor[0], ctx->scissor[1], ctx->scissor[2]);
  cache_flush_now(s_bd.scis, 64);
  bd_run(s_bd.scis, 32);
  const u32 n1 = s_bd.n[BD_P3D];
  const u64 t2 = svcGetSystemTick();
  GX_ProcessCommandList(list, words * 4, 0);
  const u64 t3 = bd_wait(BD_P3D, n1, 1);
  bd_run(s_bd.scis + 8, 32);
  s_bd.geo_ms[phase] += ticks_ms(t3 - t2);
  s_bd.geo_lists[phase]++;
}

/* the draws counted since the last call belong to the current part */
static void bd_count(void) {
  const int ph = s_bd.phase;
  s_bd.tris[ph] += g.cur.triangles - s_bd.c_tris;
  s_bd.draws[ph] += g.cur.draws - s_bd.c_draws;
  s_bd.binds[ph] += g.cur.tex_binds - s_bd.c_binds;
  s_bd.c_tris = g.cur.triangles;
  s_bd.c_draws = g.cur.draws;
  s_bd.c_binds = g.cur.tex_binds;
}

/* the commands built since the last split, as a part of the current phase */
static void bd_part(void) {
  bd_count();
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
  bd_resend();
}

void ctr_gpu_phase(int phase) {
  if (phase < 0 || phase >= CTR_GPU_PHASES) {
    phase = CTR_PH_OTHER;
  }
  if (s_bd.frame && phase != s_bd.phase && g.in_frame) {
    bd_part();
  } else if (s_bd.frame) {
    bd_count();
  }
  s_bd.phase = phase;
}

/* frame begin (pipeline): is this frame measured; then the previous frame finished */
static void bd_frame_begin(void) {
  s_bd.frame = s_bd.on && s_bd.hooked && s_pipeline && (g.frame_no % BD_EVERY) == 0;
  s_bd.phase = CTR_PH_OTHER;
  s_bd.c_tris = s_bd.c_draws = s_bd.c_binds = 0; /* (g.cur restarts after this) */
  if (s_bd.frame) {
    bd_idle();
    bd_resend();
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
  if (size) {
    GX_TextureCopy(in, indim, out, outdim, size, flags);
  } else {
    GX_DisplayTransfer(in, indim, out, outdim, flags); /* (size 0: a display transfer) */
  }
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
  /* per part: its geometry alone (the list again under a one pixel scissor) and what it drew */
  if (len < size) {
    len += snprintf(out + len, size - len, "; geometry ms, triangles, draws, texture binds:");
  }
  for (int i = 0; i < CTR_GPU_PHASES && len < size; i++) {
    if (!s_bd.lists[i] || !s_bd.geo_lists[i]) {
      continue;
    }
    len += snprintf(out + len, size - len, " %s %.2f %.0f %.0f %.0f", kPhaseNames[i],
                    s_bd.geo_ms[i] / s_bd.geo_lists[i] * (s_bd.lists[i] / n), s_bd.tris[i] / n,
                    s_bd.draws[i] / n, s_bd.binds[i] / n);
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
                    "%u frames; %u lists not run again (they set the scissor or the target)%s",
                    sum / n, ovh, ovh * lists / n, s_bd.psc1_seen, s_bd.frames, (unsigned)s_bd.geo_skipped,
                    s_bd.timeouts ? "; TIMEOUTS (an event didn't come in 50 ms)" : "");
  }
  memset(s_bd.ms, 0, sizeof(s_bd.ms));
  memset(s_bd.lists, 0, sizeof(s_bd.lists));
  memset(s_bd.tris, 0, sizeof(s_bd.tris));
  memset(s_bd.draws, 0, sizeof(s_bd.draws));
  memset(s_bd.binds, 0, sizeof(s_bd.binds));
  memset(s_bd.geo_ms, 0, sizeof(s_bd.geo_ms));
  memset(s_bd.geo_lists, 0, sizeof(s_bd.geo_lists));
  s_bd.geo_skipped = 0;
  s_bd.overhead_ms = 0;
  s_bd.overhead_n = s_bd.frames = s_bd.timeouts = s_bd.psc1_seen = 0;
  return len;
}

static int apt_gate_closed(void);
static void pres_vblank(void* unused) {
  (void)unused;
  const u32 v = ++s_pres.vblanks;
  if (apt_gate_closed()) {
    return;
  }
  if (s_pres.drop_req || (s32)(s_pres.q_tail - s_pres.q_head) < 0) {
    /* (AI-assisted, gpu-home) the render thread's drop (no free screen buffer for 300 ms), or
     * indices that went wrong: nothing waits any more */
    s_pres.q_head = s_pres.q_tail;
    for (int i = 0; i < PRESENT_BUFS; i++) {
      if (s_pres.buf_state[i] == 1) {
        s_pres.buf_state[i] = 0;
      }
    }
    s_pres.drop_req = 0;
  }
  if (s_pres.requested >= 0) {
    if (gspIsPresentPending(GFX_TOP)) {
      /* (AI-assisted, gpu-home) the GSP hasn't taken the last request yet (this callback ran
       * after its vblank handling): the old buffer is still scanned out. Nothing freed or
       * requested until it has: the next vblank. */
      return;
    }
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
  if ((s32)(s_pres.q_tail - s_pres.q_head) <= 0) {
    return;
  }
  svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32)s_pres.fence, 16);
  const u32 done = *(volatile u32*)s_pres.fence;
  int pick = -1;
  while ((s32)(s_pres.q_tail - s_pres.q_head) > 0) {
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
    u8* const fb = s_pres.buf[pick] + s_pres.show_off;
    gspPresentBuffer(GFX_TOP, (unsigned)s_pres.flip, fb, fb, s_pres.stride, s_pres.mode);
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

unsigned int ctr_gpu_release_now(void) {
  return vblank_count();
}

void ctr_gpu_note_release(unsigned int release) {
  s_pres.next_release = release;
}

static int pres_init(void) {
  const GSPGPU_FramebufferFormat fmt = gfxGetScreenFormat(GFX_TOP);
  const u32 bpp = gspGetBytesPerPixel(fmt);
  const u32 fw = g.top->frameBuf.width, fh = g.top->frameBuf.height;
  const u32 bytes = fw * fh * bpp;
  const u32 region = 240 * 800 * bpp; /* one of libctru's top framebuffers */
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
      u8* at[2] = {fb[0], fb[1]};
      u32 size[2] = {region, region};
      const int regions = 2;
      for (int k = 0; k < regions; k++) {
        for (u32 off = 0; off + bytes <= size[k] && borrowed < PRESENT_BUFS; off += bytes) {
          s_pres.buf[borrowed++] = at[k] + off;
        }
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
  s_pres.stride = fw * bpp;
  s_pres.bytes = bytes;
  s_pres.show_off = (fh - TOP_H) * s_pres.stride;
  s_pres.last_buf = -1;
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
static void pres_queue_frame(void) {
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
      /* nothing is being shown (the app in the background?): drop the waiting frames. (AI-assisted,
       * gpu-home) The vblank callback does it (it alone moves q_head); only when it doesn't run
       * at all (100 ms more) here, as before */
      if (!s_pres.drop_req && tries == 301) {
        s_pres.drop_req = 1;
      } else if (tries > 400 && s_pres.drop_req) {
        s_pres.drop_req = 0;
        s_pres.q_head = s_pres.q_tail;
        for (int i = 0; i < PRESENT_BUFS; i++) {
          if (s_pres.buf_state[i] == 1) {
            s_pres.buf_state[i] = 0;
          }
        }
        tries = 0;
        continue;
      }
      if (!s_pres.drop_req) {
        tries = 0; /* (the callback dropped them: wait for a buffer again) */
        continue;
      }
    }
    pres_check_alive();
    svcSleepThread(1000000LL);
  }
  s_pres.buf_state[b] = 1;
  s_pres.buf_wait_ms += (double)(svcGetSystemTick() - t0) / (SYSCLOCK_ARM11 / 1000.0);
  C3D_FrameSplit(GX_CMDLIST_FLUSH); /* the draws first */
  {
    const C3D_FrameBuf* fb = &g.top->frameBuf;
    const u32 dim = GX_BUFFER_DIM(fb->width, fb->height);
    GX_DisplayTransfer((u32*)fb->colorBuf, dim, (u32*)s_pres.buf[b], dim,
                       DISPLAY_TRANSFER_FLAGS);
    s_pres.last_buf = b;
  }
  /* the fence: a 16 byte texture copy of this frame's number, after the transfer. Not a memory
   * fill: on hardware that unit only fills VRAM, and a fill of main memory never finished (the
   * GPU queue stalled: black top screen, frozen game, sound still playing). Azahar fills any
   * memory. */
  {
    u32* id = s_pres.ids + 4 * (s_pres.q_tail % PRESENT_Q);
    id[0] = id[1] = id[2] = id[3] = g.frame_no;
    cache_flush_now(id, 16);
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


/* (AI-assisted) Jak II's menu: the next frame begin clears only the depth */
static int s_skip_color_clear; /* ctr_gpu_set_skip_color_clear, for the next frame begin */

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
  g.ocean_full_open = 0; /* (AI-assisted) a ctr_gpu_ocean_full_reserve never drawn */
  clip_reserve_reset();  /* (AI-assisted) and a ctr_gpu_clip_reserve */
  use_program(PROG_BASIC);
  /* the fog ramp stays on texture unit 1 for the whole frame (binding a texture clears the GPU's
   * texture cache: once per frame, not at every level draw) */
  if (g.fog_tex_valid) {
    tex1_bind(&g.fog_tex);
  }
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
  return ((u32)r << 24) | ((u32)gr << 16) | ((u32)b << 8) | 0xff;
}

/* a C3D_FrameBegin whose frame was abandoned: ended with nothing to send, the gate open again */
static int s_c3d_open;
static void c3d_frame_abort(void) {
  if (!s_c3d_open) {
    return;
  }
  s_c3d_open = 0;
  GPUCMD_SetBuffer(gpuCmdBuf, gpuCmdBufSize, 0);
  __C3D_Context.flags &= ~(u32)C3DiF_DrawUsed;
  g.top->used = false;
  C3D_FrameEnd(GX_CMDLIST_FLUSH);
}

static void pipeline_frame_begin(u32 clear) {
  if (g.screenshot_state == 2) {
    /* (AI-assisted) the frame to save is complete once the GPU has nothing queued (this frame's
     * clear isn't queued yet): wait for it, only in frames that take a screenshot */
    if (gxCmdQueueWait(&__C3D_Context.gxQueue, GX_WAIT_MS(1000))) {
      write_screenshot();
    }
    g.screenshot_state = 0;
  }
  s_k ^= 1;
  /* the frame's first GPU work in one piece: all of it queued, or the frame abandoned */
  if (gx_lost() || !gx_begin()) {
    return;
  }
  c3d_frame_abort();
  GPUCMD_SetBuffer(s_cmdbufs[s_k], PIPE_CMD_BYTES / 4, 0);
  g.cmd_base = gpuCmdBuf;
  g.cmd_flushed_words = 0;
  g.vbuf = s_vbufs[s_k];
  g.frame_no++;
  bd_frame_begin(); /* (AI-assisted, gpu-rt) gpu_breakdown: the frame before is done first */
  gt_note_first(); /* (AI-assisted) the clear below starts the GPU on this frame */
  update_pool_residency();
  if (s_bd.frame) {
    bd_clear_begin();
  }
  C3D_FrameBuf* fb = &g.top->frameBuf;
  u32* color = (u32*)fb->colorBuf;
  u32* depth = (u32*)fb->depthBuf;
  /* D24S8: 4 bytes; RGBA8: 4 */
  const u32 px = (u32)fb->width * fb->height;
  u32* color_end = (u32*)((uint8_t*)color + px * 4);
  if (s_skip_color_clear) {
    /* (AI-assisted) the depth only (VRAM): Jak II's menu copies the last frame */
    GX_MemoryFill(depth, 0, (u32*)((uint8_t*)depth + px * 4),
                  GX_FILL_TRIGGER | GX_FILL_32BIT_DEPTH, NULL,
                  0, NULL, 0);
  } else {
    GX_MemoryFill(color, clear, color_end,
                  GX_FILL_TRIGGER | GX_FILL_32BIT_DEPTH, depth,
                  0, (u32*)((uint8_t*)depth + px * 4), GX_FILL_TRIGGER | GX_FILL_32BIT_DEPTH);
  }
  if (s_bd.frame) {
    bd_clear_end(!s_skip_color_clear);
  }
  C3D_SetFrameBuf(fb);
  set_top_viewport();
  frame_state_reset();
  g.cur.color_clear_skipped = s_skip_color_clear;
  gx_end();
}

/* send what's built so far to the GPU (pipeline: our command buffer, citro3d isn't in a frame) */
static void pipeline_submit(void) {
  flush_vbuf();
  if (!gx_begin()) {
    return;
  }
  GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_FLUSH, 1);
  GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_INVALIDATE, 1);
  u32* list;
  u32 words;
  GPUCMD_Split(&list, &words);
  if (words) {
    gt_note_submit();
    GX_ProcessCommandList(list, words * 4, GX_CMDLIST_FLUSH);
  }
  gx_end();
}

void ctr_gpu_frame_begin(uint8_t r, uint8_t gr, uint8_t b) {
  if (!g.ready) {
    return;
  }
  apt_gate_enter(); /* (AI-assisted, gpu-home) waits while the HOME Menu or sleep has the GPU */
  anim_slots_frame_begin(); /* (AI-assisted) */
  /* texture combiner constant: R in the low byte */
  g.clear_abgr = 0xff000000u | ((u32)b << 16) | ((u32)gr << 8) | (u32)r;
  if (s_pipeline) {
    pipeline_frame_begin(color_fill_value(r, gr, b));
    return;
  }
  if (!gx_begin()) {
    return;
  }
  c3d_frame_abort();
  diag_rt("frame_begin: C3D_FrameBegin (wait for GPU)");
  const u64 wait0 = svcGetSystemTick();
  C3D_FrameBegin(0); /* waits for the GPU; the game thread already paces to vblank */
  s_c3d_open = 1;
  const float wait_ms = (float)((double)(svcGetSystemTick() - wait0) / (SYSCLOCK_ARM11 / 1000.0));
  diag_rt("frame_begin: deletes, VRAM pool copies");
  g.cmd_base = gpuCmdBuf;
  g.cmd_flushed_words = 0;
  g.frame_no++;
  process_pending_deletes(0);
  /* before any draw: copies to VRAM are queued ahead of this frame's draws */
  update_pool_residency();
  if (g.screenshot_state == 2) {
    write_screenshot();
    g.screenshot_state = 0;
  }
  /* (AI-assisted) Jak II's menu: the depth only */
  C3D_RenderTargetClear(g.top, s_skip_color_clear ? C3D_CLEAR_DEPTH : C3D_CLEAR_ALL,
                        color_fill_value(r, gr, b), 0);
  C3D_FrameDrawOn(g.top);
  set_top_viewport(); /* (FrameDrawOn's viewport is the whole target) */
  frame_state_reset();
  g.cur.color_clear_skipped = s_skip_color_clear;
  g.cur.gpu_wait_ms = wait_ms; /* (AI-assisted) (after the reset of g.cur) */
  gx_end();
  diag_rt("draw build");
}

/* Immediate draws copy their vertices into g.vbuf. The GPU reads them only when the frame's GX
 * queue runs (C3D_FrameEnd; C3D_FrameBegin stopped the queue, so command list splits wait in it
 * until then), so one cache flush of what this frame wrote is enough. */
/* (AI-assisted) The render thread's flushes for this frame's GPU work (texture updates: sky,
 * clouds, ocean, eyes; blend shape vertices; the fog ramp) wait here until the frame's commands
 * go to the GPU (flush_vbuf, at every submission point), merged where the ranges touch. */
#define DEFER_FLUSH_MAX 48
static struct {
  u32 start[DEFER_FLUSH_MAX], end[DEFER_FLUSH_MAX];
  int n;
} s_defer;

static int g_defer_flush = 1;
void ctr_gpu_set_defer_flush(int on) {
  g_defer_flush = on;
}

static u32 s_defer_added, s_defer_flushed; /* statistics: ranges deferred, flushed at submits */
static void cache_flush_frame(const void* addr, u32 size) {
  if (!g.in_frame || !g_defer_flush) {
    cache_flush_now(addr, size);
    return;
  }
  s_defer_added++;
  const u32 a = (u32)addr, e = a + size;
  for (int i = 0; i < s_defer.n; i++) {
    if (a <= s_defer.end[i] && e >= s_defer.start[i]) {
      s_defer.start[i] = a < s_defer.start[i] ? a : s_defer.start[i];
      s_defer.end[i] = e > s_defer.end[i] ? e : s_defer.end[i];
      return;
    }
  }
  if (s_defer.n == DEFER_FLUSH_MAX) {
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
  for (int i = 0; i < s_defer.n; i++) {
    cache_flush_now((const void*)s_defer.start[i], s_defer.end[i] - s_defer.start[i]);
  }
  s_defer_flushed += (u32)s_defer.n;
  s_defer.n = 0;
}

/* (AI-assisted) C3D_SyncTextureCopy in a frame splits the command list with C3D_FrameSplit(0): that
 * part of the command list is NOT flushed from the CPU cache. C3D_FrameEnd(0) used to flush the
 * whole linear heap, which covered it; ctr_gpu_frame_end only flushes the last part
 * (GX_CMDLIST_FLUSH), so the GPU could run stale command words of the earlier part (wrong blend
 * and texture state, broken draws) in every frame with a copy (the distorter's screen copy, a
 * texture pool moving to VRAM). Split here with the flush first; C3D_SyncTextureCopy's own split
 * then has nothing left to send. */
/* Returns 0 when the copy was not queued (the HOME Menu or sleep has the GPU). */
static int sync_texture_copy(u32* in, u32 indim, u32* out, u32 outdim, u32 size, u32 flags) {
  if (!gx_begin()) {
    return 0;
  }
  if (g.in_frame && s_pipeline) {
    if (s_bd.frame) {
      bd_copy(in, indim, out, outdim, size, flags); /* (AI-assisted, gpu-rt) gpu_breakdown */
      gx_end();
      return 1;
    }
    pipeline_submit(); /* the draws before the copy, then the copy, in the GPU's queue */
    GX_TextureCopy(in, indim, out, outdim, size, flags);
    gx_end();
    return 1;
  }
  if (g.in_frame) {
    /* (AI-assisted) with overlap the queue may already be running (ctr_gpu_submit_partial): the
     * draws before the copy start right away, so their vertices must be out of the CPU cache */
    flush_vbuf();
    C3D_FrameSplit(GX_CMDLIST_FLUSH);
    C3D_SyncTextureCopy(in, indim, out, outdim, size, flags);
    gx_end();
    return 1;
  }
  C3D_SyncTextureCopy(in, indim, out, outdim, size, flags);
  gx_end();
  return 1;
  /* (AI-assisted) Outside a frame citro3d waits for "the" PPF event (gspWaitForPPF), which steady
   * pacing's transfers and fences signal with nobody clearing it, so this can return while the copy
   * still runs (audit FS-10). Waiting for the GX queue here instead cost ~28 ms per frame in lava
   * tube (Azahar, Oct 29: the queue also held the frames in flight), so it is not done: no caller
   * reads the copy's result on the CPU. */
}

#define PARTIAL_MIN_DRAWS 24
#define PARTIAL_MAX 6 /* citro3d's queue has 32 entries per frame */
/* (AI-assisted, gpuopt) with the pipeline the GPU draws the frame before while this one is built,
 * so splitting this one finely buys little: fewer, bigger parts. Each part costs the render
 * thread a GSP cache flush request for its vertices (and, while the queue is idle, the submit:
 * Azahar charges ~62 us for each request) and the GPU a framebuffer flush and invalidate
 * (Haven City slums: 5-6 parts per frame with the limits above) */
#define PIPE_PARTIAL_MIN_DRAWS 200
#define PIPE_PARTIAL_MAX 2

void ctr_gpu_free_pending_now(void) {
  if (!g.ready || g.in_frame) {
    return;
  }
  /* the GX queue holds the last frames' command lists and display transfers; with a timeout in
   * case it isn't running (then the deletes wait for the next frame begin, as before) */
  if (!gxCmdQueueWait(&__C3D_Context.gxQueue, GX_WAIT_MS(200))) {
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
  if (!s_overlap || !g.ready || !g.in_frame || s_bd.frame ||
      s_partials >= (s_pipeline ? PIPE_PARTIAL_MAX : PARTIAL_MAX) ||
      g.cur.draws < s_partial_draws + (s_pipeline ? PIPE_PARTIAL_MIN_DRAWS : PARTIAL_MIN_DRAWS)) {
    return;
  }
  if (s_pipeline) {
    pipeline_submit();
  } else if (gx_begin()) {
    flush_vbuf();
    C3D_FrameSplit(GX_CMDLIST_FLUSH);
    gt_note_first(); /* (AI-assisted) the queue (stopped since C3D_FrameBegin) starts here */
    gt_note_submit();
    gxCmdQueueRun(&__C3D_Context.gxQueue);
    gx_end();
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

void ctr_gpu_frame_end(void) {
  if (!g.ready || !g.in_frame) {
    if (g.ready) {
      ctr_gpu_rt_end();
    }
    apt_gate_leave(); /* (AI-assisted, gpu-home) */
    return;
  }
  ctr_gpu_rt_end(); /* (AI-assisted) impostors: a render to texture left open */
  diag_rt("frame_end: flush");
  flush_vbuf();
  /* the rest of the frame's GPU work in one piece: all of it queued, or none */
  if (!gx_begin()) {
    return;
  }
  const int bd = s_bd.frame;
  if (bd) {
    /* (AI-assisted, gpu-rt) gpu_breakdown: the rest of the frame's commands alone (the frame
     * end below then has no list left to hand over) */
    g.cur.cmd_bytes = (unsigned int)((gpuCmdBuf + gpuCmdBufOffset - g.cmd_base + g.cmd_flushed_words) * 4);
    bd_part();
  }
  /* the command words of the whole frame (splits move gpuCmdBuf on; C3D_FrameBegin restarts it) */
  if (!bd) {
    g.cur.cmd_bytes = (unsigned int)((gpuCmdBuf + gpuCmdBufOffset - g.cmd_base + g.cmd_flushed_words) * 4);
  }
  if (s_pipeline) {
    /* wait for the previous frame (and this one's parts sent so far), then submit the rest of
     * this frame from our buffer with citro3d (display transfer, buffer swap) */
    u32* buf = gpuCmdBuf;
    const u32 size = gpuCmdBufSize, offset = gpuCmdBufOffset;
    const u64 wait0 = svcGetSystemTick();
    C3D_FrameBegin(0);
    s_c3d_open = 1;
    g.cur.gpu_wait_ms =
        (float)((double)(svcGetSystemTick() - wait0) / (SYSCLOCK_ARM11 / 1000.0));
    process_pending_deletes(1);
    GPUCMD_SetBuffer(buf, size, offset);
    C3D_FrameDrawOn(g.top);
    set_top_viewport();
    gt_note_submit();
  }
  if (s_pres.on) {
    diag_rt("frame_end: present queue (wait for a free screen buffer)");
    pres_queue_frame();
  }
  /* all our buffers are flushed when written: only flush the command list, not the whole
   * linear heap (C3D_FrameEnd's default) */
  diag_rt("frame_end: C3D_FrameEnd (submit)");
  gt_note_first(); /* (AI-assisted) without the pipeline the queue starts here (or at a partial) */
  gt_note_submit();
  if (bd) {
    bd_transfer_mark(); /* (AI-assisted, gpu-rt) the queue (stopped) runs the transfer from here */
  }
  C3D_FrameEnd(GX_CMDLIST_FLUSH);
  s_c3d_open = 0;
  if (bd) {
    /* (AI-assisted, gpu-rt) the transfer and the fence (steady pacing) */
    bd_frame_end(s_pres.on ? 2 : 1);
  }
  gx_end();
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
        }
      }
    }
    g.cur.textures = count;
    g.cur.tex_bytes = bytes;
    g.cur.linear_free = ctr_gpu_linear_free(); /* (AI-assisted, streammem) locked */
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
  apt_gate_leave(); /* (AI-assisted, gpu-home) the frame's GPU work is all queued */
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
    case CTR_TEX_A8:
      return GPU_A8;
    case CTR_TEX_A4:
      return GPU_A4;
    case CTR_TEX_RGBA5551:
      return GPU_RGBA5551;
    default:
      return GPU_RGBA8;
  }
}

static unsigned int format_bits(int format) {
  switch (format) {
    case CTR_TEX_RGB565:
    case CTR_TEX_RGBA4:
    case CTR_TEX_RGBA5551:
      return 16;
    case CTR_TEX_ETC1:
    case CTR_TEX_A4:
      return 4;
    case CTR_TEX_ETC1A4:
    case CTR_TEX_A8:
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

/* VRAM holds copies of what linear memory holds: the GPU reads them faster. The CPU never reads
 * VRAM (its mapping is cached: a read can return what was there before the GPU's last write), and
 * what is there may be gone after the HOME Menu or sleep. */
/* a copy at frame begin, in the queue's order behind the frames in flight (not waited for);
 * 0: not queued */
static int queue_texture_copy(void* in, void* out, u32 size) {
  return sync_texture_copy((u32*)in, 0, (u32*)out, 0, size, 8);
}

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
        t->tex.data = p->linear + t->offset;
      }
    }
  }
  g.bound_tex = NULL;
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

/* the GPU reads linear memory again (frames in flight keep the VRAM copy: whatever takes its
 * place is queued behind them) */
static void pool_leave_vram(int pool) {
  TexPool* p = &g.pools[pool];
  if (!p->vram) {
    return;
  }
  void* v = p->vram;
  p->vram = NULL;
  p->vram_bytes = 0;
  p->vram_serial = 0;
  pool_repoint(pool);
  vramFree(v);
}

static void arena_leave_vram(int back);

/* nothing in VRAM is what was copied there any more (after the HOME Menu or sleep): everything is read from linear memory; back: and copied to VRAM again from there */
static void vram_copies_lost(int back) {
  ctr_linear_lock();
  for (int i = 0; i < MAX_POOLS; i++) {
    if (g.pools[i].used == 1 && g.pools[i].vram) {
      pool_leave_vram(i);
      g.pools[i].no_room_frame = -1000;
    }
  }
  ctr_linear_unlock();
  arena_leave_vram(back);
}

/* At frame begin, before any draw: copy the pools that want to be in VRAM there, most wanted
 * first, moving pools with a lower priority out if that makes room. The copies are queued ahead of
 * this frame's draws, which read the VRAM copy. */
static void update_pool_residency(void) {
  if (!g_vram_textures) {
    int any = s_arena.base && !(s_arena.nfree == 1 && s_arena.free_len[0] == ARENA_BYTES);
    for (int i = 0; i < MAX_POOLS && !any; i++) {
      any = g.pools[i].used == 1 && g.pools[i].vram != NULL;
    }
    if (any) {
      vram_copies_lost(0);
    }
    return;
  }
  /* (AI-assisted) the largest block VRAM can give at all: two separate 3 MB banks, each with a
   * render target buffer. Measured once (only the screens' buffers are there yet). A pool bigger
   * than this never fits whole. */
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
  static int s_copy_released_frame = -1000; /* the distorter's screen copy gave up its VRAM */
  unsigned int copied = 0;
  int tried[MAX_POOLS] = {0};
  int evictions = 0;
  while (copied < POOL_COPY_BYTES) {
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
    if (copied && copied + p->bytes > POOL_COPY_BYTES) {
      break; /* the next frame's */
    }
    ctr_linear_lock();
    void* v = safe_vram_alloc(p->bytes);
    while (!v && p->bytes <= s_vram_max_block && evictions < 4) {
      /* move out the resident pool with the lowest priority below this one's */
      int victim = -1;
      for (int i = 0; i < MAX_POOLS; i++) {
        const TexPool* q = &g.pools[i];
        if (q->used == 1 && q->vram && q->priority < p->priority &&
            g.frame_no - q->vram_frame >= POOL_STAY_FRAMES &&
            (victim < 0 || q->priority < g.pools[victim].priority)) {
          victim = i;
        }
      }
      if (victim < 0) {
        /* (AI-assisted, gpu-home) last, the distorter's screen copy: level textures are read for
         * most of the screen, the copy for a few distort pixels. Only for a pool at least as big
         * (its linear memory freed pays for the copy's, made there at the next distort sprite);
         * the copy's VRAM is free after the next frame begin's deletes (this pool's retry). */
        const u32 screen_bytes = 256u * 512u * 4u;
        if (g.screen_tex >= 0 && g.screen_tex_vram && !g.screen_tex_pinned &&
            p->bytes >= screen_bytes) {
          ctr_linear_unlock();
          ctr_gpu_screen_copy_release();
          ctr_linear_lock();
          s_copy_released_frame = g.frame_no;
          printf("[ctr] distorter screen copy: VRAM given to a texture pool (%u KB)\n",
                 p->bytes / 1024);
        }
        break;
      }
      pool_leave_vram(victim);
      evictions++;
      v = safe_vram_alloc(p->bytes);
    }
    unsigned int part = p->bytes & ~15u;
    if (part < p->bytes) {
      part = pool_split(best, part);
    }
    if (!v) {
      /* (AI-assisted) not all of it: as much as fits in the VRAM that's left (whole textures from
       * the start), so the GPU reads those fast */
      /* (config.ini texshare = off: the steps from before texture sharing) */
      static int s_texshare = -1;
      if (s_texshare < 0) {
        s_texshare = ctr_config_bool("texshare", 1);
      }
      const unsigned int kStep = s_texshare ? 32 * 1024 : 128 * 1024,
                         kMinPart = s_texshare ? 64 * 1024 : 256 * 1024;
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
    if (!v || !part) {
      if (v) {
        ctr_linear_lock();
        vramFree(v);
        ctr_linear_unlock();
      }
      /* (not while the screen copy's VRAM waits in the deferred deletes: retried each frame) */
      if (g.frame_no - s_copy_released_frame > 3) {
        p->no_room_frame = g.frame_no;
      }
      continue;
    }
    if (!queue_texture_copy(p->linear, v, part)) {
      ctr_linear_lock();
      vramFree(v);
      ctr_linear_unlock();
      break;
    }
    p->vram_frame = g.frame_no;
    p->vram = v;
    p->vram_bytes = part;
    static unsigned int s_serial;
    p->vram_serial = ++s_serial ? s_serial : ++s_serial;
    pool_repoint(best);
    copied += part;
  }
}

/* allow_vram: only from the render thread (the copy to VRAM goes through the GX queue) */
static void* tex_alloc(C3D_Tex* tex, int w, int h, GPU_TEXCOLOR fmt, int* on_vram, int allow_vram) {
  *on_vram = 0;
  void* result = NULL;
  ctr_linear_lock();
  /* only outside of a frame: a copy in a frame needs a command list split and a queue entry
   * each, and a level's worth of them overflows the GX queue */
  if (allow_vram && g_vram_dynamic && !g.in_frame && vram_room((size_t)w * h * 4u) &&
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

/* (AI-assisted) cull_ties: which PICA mode culls the c3l back faces (see ctr_gpu_set_cull_flip) */
static int g_cull_flip = 0;
static int g_cull_enable = 0;

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
static int g_compact = 1;

/* the GPU's queue run to its end (nothing left waiting for a run) */
static void gpu_drain(void) {
  gxCmdQueue_s* q = &__C3D_Context.gxQueue;
  const u64 t0 = svcGetSystemTick();
  while ((s64)(svcGetSystemTick() - t0) < GX_WAIT_MS(1000)) {
    if (gxCmdQueueWait(q, 0)) {
      if (q->lastEntry >= q->numEntries) {
        return;
      }
      gxCmdQueueRun(q);
    }
    gspWaitForAnyEvent();
  }
}

/* ctr_gpu_init: the screen output (with steady pacing the frames go to our own screen buffers,
 * pres_queue_frame) */
static int top_target_setup(void) {
  const int pres_on = g_steady_pacing && pres_init();
  if (!pres_on) {
    C3D_RenderTargetSetOutput(g.top, GFX_TOP, GFX_LEFT,
                              DISPLAY_TRANSFER_FLAGS);
  }
  return 0;
}

void ctr_gpu_submit_now(void) {
  if (!g.ready || !g.in_frame || s_bd.frame) {
    return;
  }
  if (s_pipeline) {
    pipeline_submit();
  } else if (gx_begin()) {
    flush_vbuf();
    C3D_FrameSplit(GX_CMDLIST_FLUSH);
    gxCmdQueueRun(&__C3D_Context.gxQueue);
    gx_end();
  }
}

int ctr_gpu_top_histogram(unsigned int counts[256]) {
  memset(counts, 0, 256 * sizeof(counts[0]));
  if (!g.ready || g.in_frame || envIsHomebrew()) {
    return 0;
  }
  apt_gate_enter();
  gpu_drain();
  const int idle = gxCmdQueueWait(&__C3D_Context.gxQueue, 0);
  if (idle) {
    /* RGBA8: R in the high byte */
    const u32* c = (const u32*)g.top->frameBuf.colorBuf;
    const u32 px = (u32)g.top->frameBuf.width * g.top->frameBuf.height;
    svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32)c, px * 4);
    for (u32 i = 0; i < px; i++) {
      counts[(c[i] >> 8) & 0xff]++;
    }
  }
  apt_gate_leave();
  return idle;
}



void ctr_gpu_set_compact_textures(int on) {
  g_compact = on ? 1 : 0;
}

/* ---------------- (AI-assisted) compact textures ---------------- */

/* the GPU expands 5 / 6 bit channels by bit replication */
static inline int exact5(uint8_t v) {
  const uint8_t c = ctr_q5[v];
  return (uint8_t)((c << 3) | (c >> 2)) == v;
}
static inline int exact6(uint8_t v) {
  const uint8_t c = ctr_q6[v];
  return (uint8_t)((c << 2) | (c >> 4)) == v;
}

int ctr_gpu_tex_create_compact(int w, int h, const uint8_t* rgba) {
  if (!g_compact) {
    return ctr_gpu_tex_create(w, h, rgba);
  }
  if (!g.ready || w < 8 || h < 8 || w > 1024 || h > 1024) {
    return -1;
  }
  const int n = w * h;
  /* only formats that keep every texel exactly */
  int gray = 1, opaque = 1, binary = 1, fit565 = 1, fit5551 = 1;
  unsigned amax = 0;
  for (int i = 0; i < n; i++) {
    const uint8_t* p = rgba + 4 * i;
    if (p[0] != p[1] || p[2] != p[1]) {
      gray = 0;
    }
    if (!exact5(p[0]) || !exact6(p[1]) || !exact5(p[2])) {
      fit565 = 0;
    }
    if (!exact5(p[0]) || !exact5(p[1]) || !exact5(p[2])) {
      fit5551 = 0;
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
  } else if (opaque && fit565) {
    fmt = GPU_RGB565;
  } else if (binary && fit5551) {
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
  /* room for the format's own size (linear_room_for_tex asks for 32 bits per texel) */
  const size_t fmt_bytes = (size_t)n * (fmt == GPU_L8 ? 1u : 2u);
  ctr_linear_lock();
  const bool ok = (size_t)linearSpaceFree() >= fmt_bytes + LINEAR_RESERVE &&
                  C3D_TexInit(tex, (u16)w, (u16)h, fmt);
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
          default: /* (AI-assisted, gpu-home) RGB565, RGBA5551, RGBA4: rounded (CtrTexConv.h) */
            d16[o] = (uint16_t)ctr_pack16(ctr_ld32(p), conv);
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
  t->arena_off = -1;
  t->want_vram = s_arena.base != NULL;
  s_small_waiting |= t->want_vram;
  ctr_linear_lock();
  t->used = 1;
  ctr_linear_unlock();
  return slot;
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
  if (!sync_texture_copy((u32*)staging, 0, (u32*)(s_arena.base + off), 0, total, 8)) {
    arena_free((u32)off, total);
    defer_linear_free(staging);
    return;
  }
  pos = 0;
  for (int j = 0; j < npick; j++) {
    TexSlot* t = &g.textures[pick[j]];
    t->home = t->tex.data;
    t->tex.data = s_arena.base + off + pos;
    t->arena_off = off + (int)pos;
    t->want_vram = 0;
    pos += sizes[j];
  }
  defer_linear_free(staging);
  g.bound_tex = NULL;
}

/* the reserved VRAM's textures are read from linear memory again; back: and move there again */
static void arena_leave_vram(int back) {
  if (!s_arena.base) {
    return;
  }
  ctr_linear_lock();
  for (int i = 0; i < MAX_TEXTURES; i++) {
    TexSlot* t = &g.textures[i];
    if (t->arena_off >= 0 && t->home) {
      t->tex.data = t->home;
      t->home = NULL;
      t->arena_off = -1;
      t->want_vram = back && t->used == 1;
    }
  }
  ctr_linear_unlock();
  s_arena.free_off[0] = 0;
  s_arena.free_len[0] = ARENA_BYTES;
  s_arena.nfree = 1;
  s_small_waiting = back;
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
  if (s_tex_mask[handle]) {
    s_mask_of[s_tex_mask[handle] - 1] = 0;
    s_tex_mask[handle] = 0;
  }
  if (s_mask_of[handle]) {
    s_tex_mask[s_mask_of[handle] - 1] = 0;
    s_mask_of[handle] = 0;
  }
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
 * The post-vertex cache: citro3d clears it after every draw, and so do the raw draws (raw_vc_clear
 * in their templates). (AI-assisted, hudshard) Never keep it for the next draw, not even one of the
 * same vertices with the same uniforms: vcache_reuse (Oct 3, on by default from Oct 16 to Oct 25)
 * did, and on the real 3DS the HUD's faint second pass (ctr_gpu_draw_quads_again: the first pass's
 * vertices again) got a wrong first vertex in single-quad HUD buckets: the red / white HUD shards
 * (one triangle of an icon stretched off to one side; the button icons, the progress menu's frame,
 * the orb glow). Azahar keeps no vertices across draws, so it never showed. See
 * docs/3ds-port/performance.md "The red HUD shard". */
static struct {
  int on;           /* config.ini raw_cmds */
  int unif_pending; /* citro3d has uniforms to send (C3D_FVUnif* since its last update) */
  volatile u32 apt_epoch;
  u32 seen_epoch;
  int hooked;
  aptHookCookie cookie;
  const void* vbuf_va; /* g.vbuf and its physical address */
  u32 vbuf_pa;
  u32 quad_pa;    /* physical address of g.quad_indices */
} s_raw = {.on = 1};

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

/* ---------------- (AI-assisted, gpu-home) the HOME Menu and sleep ----------------
 * libctru runs the APT hooks on the thread that calls aptMainLoop: the main thread (gfx.cpp,
 * ctr_main_loop). citro3d's hook waits for its GX queue, stops and clears it (ONSUSPEND) and marks
 * its whole context dirty (ONRESTORE), while the render thread (or, without it, the game thread)
 * kept building and submitting frames: Stop / Clear / Run of the two threads interleaved on the
 * unprotected queue (gxCmdQueueClear panics on a running queue: a crash at HOME, or lost entries),
 * GPU writes landed between GSPGPU_SaveVramSysArea and the release of the GPU (undone by the
 * restore), and the two threads' read-modify-writes of the context's dirty flags could lose bits.
 * This hook, registered after C3D_Init (aptHook puts the newest first), runs before citro3d's:
 * on ONSUSPEND / ONSLEEP it closes the gate, waits for the frame being built (ctr_gpu_frame_begin
 * .. ctr_gpu_frame_end) to end and for the GPU to finish it; citro3d's hook then finds the queue
 * idle and nobody adding to it. The gate opens on the main thread once aptMainLoop has returned
 * (every path: back, woken up, or closing), after citro3d's ONRESTORE; the next frame begin sees
 * the new epoch and sends the whole GPU state again (apt_resume_state). */
static struct {
  LightLock lock;
  CondVar cv;
  volatile int gate;   /* closed: no frame may begin */
  volatile int busy;   /* a frame is being built (between frame begin and end) */
  volatile u32 epoch;  /* + 1 at every opening */
  u32 seen;            /* the epoch the frames last sent the state for */
  int hooked;
  aptHookCookie cookie;
  u32 suspends, timeouts;
  /* gx_begin: the thread holding the lock and its depth; lost: the frame met a closed gate */
  Thread owner, frame_thread;
  int depth;
  volatile int lost;
  u32 abandoned;
} s_apt;

static int apt_gate_closed(void) {
  return s_apt.gate;
}

static void apt_gate_hook(APT_HookType type, void* param) {
  (void)param;
  if (type != APTHOOK_ONSUSPEND && type != APTHOOK_ONSLEEP) {
    return;
  }
  const u64 t0 = svcGetSystemTick();
  LightLock_Lock(&s_apt.lock);
  s_apt.gate = 1;
  s_apt.suspends++;
  /* the frame being built: bounded (a frame stuck on the GPU must not keep the HOME Menu out) */
  while (s_apt.busy && svcGetSystemTick() - t0 < (u64)GX_WAIT_MS(1000)) {
    CondVar_WaitTimeout(&s_apt.cv, &s_apt.lock, 50000000LL);
  }
  const int timed_out = s_apt.busy;
  s_apt.timeouts += (u32)timed_out;
  LightLock_Unlock(&s_apt.lock);
  /* what is queued */
  gxCmdQueueWait(&__C3D_Context.gxQueue, GX_WAIT_MS(1000));
  printf("[ctr] %s: frames stopped (%.1f ms%s; %lu frames abandoned unsent so far)\n",
         type == APTHOOK_ONSLEEP ? "sleep" : "HOME Menu",
         (double)(svcGetSystemTick() - t0) / (SYSCLOCK_ARM11 / 1000.0),
         timed_out ? ", the frame being built did NOT end: nothing more of it is sent" : "",
         (unsigned long)s_apt.abandoned);
}

/* main thread, after every aptMainLoop (ctr_port.c ctr_main_loop): open a closed gate. Not when
 * the HOME Menu closes the app: the GPU stays the HOME Menu's (no ONRESTORE), and a frame or
 * texture copy sent now would run on top of it and hang the console; the process ends with the
 * render thread still waiting at the gate. */
static void apt_gate_after(void) {
  if (!s_apt.gate || aptShouldClose()) {
    return;
  }
  LightLock_Lock(&s_apt.lock);
  s_apt.epoch++;
  s_apt.gate = 0;
  CondVar_Broadcast(&s_apt.cv);
  LightLock_Unlock(&s_apt.lock);
}

static void apt_gate_install(void) {
  if (s_apt.hooked) {
    return;
  }
  LightLock_Init(&s_apt.lock);
  CondVar_Init(&s_apt.cv);
  s_apt.gate = s_apt.busy = 0;
  aptHook(&s_apt.cookie, apt_gate_hook, NULL);
  ctr_port_set_apt_after(apt_gate_after);
  s_apt.hooked = 1;
}

static void apt_gate_remove(void) {
  if (!s_apt.hooked) {
    return;
  }
  ctr_port_set_apt_after(NULL);
  aptUnhook(&s_apt.cookie);
  s_apt.hooked = 0;
  apt_gate_after(); /* (a thread waiting at the gate goes on) */
}

void C3Di_DirtyUniforms(GPU_SHADER_TYPE type); /* citro3d (source/internal.h) */
void C3Di_ProcTexDirty(C3Di_Context* ctx);       /* citro3d (source/proctex.c) */

/* the render path, at the first frame begin after an opening: what the GPU holds may be the HOME
 * Menu's (or unknown after sleep). citro3d's ONRESTORE marked its context dirty already (main
 * thread, while no frame ran); marked again here (sleep has no ONRESTORE), our own caches of GPU
 * state dropped, and steady pacing's vblank callback back at once (citro3d's ONRESTORE registered
 * its own over it; pres_check_alive would take 50 ms). The frames queued before the suspend are
 * all due at the first vblank: the callback shows the newest and skips the rest. */
static void apt_resume_state(void) {
  C3Di_Context* ctx = &__C3D_Context;
  /* AttrInfo .. GshCode, TexAll, TexEnvAll (as citro3d's ONRESTORE, but not its gas bit) */
  ctx->flags |= BIT(2) | BIT(3) | BIT(4) | BIT(5) | BIT(6) | BIT(7) | BIT(8) | BIT(9) | BIT(10) |
                BIT(11) | BIT(12) | (7u << 23) | (0x3Fu << 26);
  raw_gas_flags_clear(); /* (GC-1: ONRESTORE's C3DiF_Gas) */
  ctx->fixedAttribDirty |= ctx->fixedAttribEverDirty;
  C3Di_DirtyUniforms(GPU_VERTEX_SHADER);
  C3Di_DirtyUniforms(GPU_GEOMETRY_SHADER);
  s_raw.apt_epoch++; /* raw caches, the procedural texture tables (raw_check_epoch) */
  g.last_state_valid = 0;
  g.last_mesh_valid = 0;
  g.etie_valid = 0;
  g.last_skin_valid = 0;
  g.skin_bl_valid = 0;
  g.bound_tex = NULL;
  if (g_vram_textures) {
    vram_copies_lost(1);
  }
  if (s_pres.on) {
    gspSetEventCallback(GSPGPU_EVENT_VBlank0, pres_vblank, NULL, false);
  }
  printf("[ctr] back from the HOME Menu / sleep (%lu): the GPU state is sent again\n",
         (unsigned long)s_apt.suspends);
}

/* gpu_breakdown: each part's list begins by sending the GPU state its
 * draws use (as after the HOME Menu, but not the framebuffer, viewport and scissor), so that it
 * can run a second time from the same state */
static void bd_resend(void) {
  if (!s_bd.frame) {
    return;
  }
  C3Di_Context* ctx = &__C3D_Context;
  /* AttrInfo, BufInfo, Effect, Program, TexEnvBuf, LightEnv, VshCode, TexStatus, TexAll,
   * TexEnvAll */
  ctx->flags |= BIT(2) | BIT(3) | BIT(4) | BIT(8) | BIT(9) | BIT(10) | BIT(11) | BIT(14) |
                (7u << 23) | (0x3Fu << 26);
  C3Di_ProcTexDirty(ctx);
  ctx->fixedAttribDirty |= ctx->fixedAttribEverDirty;
  C3Di_DirtyUniforms(GPU_VERTEX_SHADER);
  s_raw.apt_epoch++;
  g.last_state_valid = 0;
  g.last_mesh_valid = 0;
  g.etie_valid = 0;
  g.last_skin_valid = 0;
  g.skin_bl_valid = 0;
  g.bound_tex = NULL;
}

static void apt_gate_enter(void) {
  if (!s_apt.hooked) {
    return;
  }
  LightLock_Lock(&s_apt.lock);
  while (s_apt.gate) {
    CondVar_Wait(&s_apt.cv, &s_apt.lock);
  }
  s_apt.busy = 1;
  s_apt.lost = 0;
  s_apt.frame_thread = threadGetCurrent();
  LightLock_Unlock(&s_apt.lock);
  if (s_apt.seen != s_apt.epoch) {
    s_apt.seen = s_apt.epoch;
    apt_resume_state();
  }
}

static void apt_gate_leave(void) {
  if (!s_apt.busy) {
    return;
  }
  LightLock_Lock(&s_apt.lock);
  s_apt.busy = 0;
  CondVar_Broadcast(&s_apt.cv);
  LightLock_Unlock(&s_apt.lock);
}

/* GX queue work only between gx_begin and gx_end (nests); 0: gate closed, frame abandoned */
static int gx_begin(void) {
  if (!s_apt.hooked) {
    return 1;
  }
  const Thread me = threadGetCurrent();
  if (s_apt.depth && s_apt.owner == me) {
    s_apt.depth++;
    return 1;
  }
  LightLock_Lock(&s_apt.lock);
  if (!s_apt.gate) {
    s_apt.owner = me;
    s_apt.depth = 1;
    return 1;
  }
  if (s_apt.busy && s_apt.frame_thread == me) {
    s_apt.busy = 0;
    s_apt.lost = 1;
    s_apt.abandoned++;
    g.in_frame = 0;
    CondVar_Broadcast(&s_apt.cv);
  }
  LightLock_Unlock(&s_apt.lock);
  return 0;
}

static void gx_end(void) {
  if (s_apt.hooked && --s_apt.depth == 0) {
    LightLock_Unlock(&s_apt.lock);
  }
}

static int gx_lost(void) {
  return s_apt.lost;
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
#pragma GCC unroll 4
  for (u32 i = 1; i < n; i++) {
    p[1 + i] = v[i];
  }
  p += n + 1;
  if (!(n & 1)) {
    *p++ = 0; /* commands start at 8 byte boundaries */
  }
  return p;
}

/* the post-vertex cache clear (and citro3d's two writes after it), after every indexed draw */
static inline u32* raw_vc_clear(u32* p) {
  p = raw_w(p, GPUREG_VTX_FUNC, 0xF, 1);
  p = raw_w(p, GPUREG_PRIMITIVE_CONFIG, 0x8, 0);
  p = raw_w(p, GPUREG_PRIMITIVE_CONFIG, 0x8, 0);
  return p;
}

static void raw_note_unif(void) {
  s_raw.unif_pending = 1;
}

/* a frame starts with citro3d's state (frame_state_reset) */
static void raw_frame_reset(void) {
  s_raw.unif_pending = 1;
}

/* (AI-assisted, gpu-home) citro3d's C3DiF_Gas / C3DiF_GasLut (BIT 18 / 19). Its APTHOOK_ONRESTORE
 * sets C3DiF_Gas, and only C3Di_GasUpdate clears it, which C3Di_UpdateContext runs in gas mode
 * only (texEnvBuf & 7 == GPU_GAS: never here). Left set, every raw_sync after a trip to the HOME
 * Menu ran the whole context update (~1.4-2k instructions per draw, 1.5-3 ms per frame). */
#define RAW_GAS_FLAGS ((u32)(BIT(18) | BIT(19)))
static void raw_gas_flags_clear(void) {
  if ((__C3D_Context.texEnvBuf & 7) != GPU_GAS) {
    __C3D_Context.flags &= ~RAW_GAS_FLAGS;
  }
}

/* what the raw draws compare against is gone (the HOME Menu used the GPU) */
static void raw_check_epoch(void) {
  if (s_raw.apt_epoch != s_raw.seen_epoch) {
    s_raw.seen_epoch = s_raw.apt_epoch;
    raw_gas_flags_clear();
    g.env_valid = 0;
    g.last_mesh_valid = 0;
    g.etie_valid = 0;
    g.last_skin_valid = 0;
    g.skin_bl_valid = 0; /* (AI-assisted) */
  }
}

/* before a raw draw's uniforms and the draw: citro3d sends what it has pending. (AI-assisted,
 * gpu-home) Not for the gas bits alone (RAW_GAS_FLAGS: never cleared outside gas mode) */
static inline void raw_sync(void) {
  raw_check_epoch();
  if (s_raw.unif_pending ||
      (__C3D_Context.flags & ~(u32)(C3DiF_Active | C3DiF_DrawUsed | RAW_GAS_FLAGS))) {
    C3Di_UpdateContext();
    s_raw.unif_pending = 0;
  }
}

/* attribute buffers 0..n-1 of the draws (n <= 3; the others up to 2 zero, as BufInfo_Init leaves
 * them): want[k] = physical address, flags 0, flags 1 */
static void raw_vertex_buffers_n(int n, const u32 (*want_pa)[3]) {
  C3D_BufInfo* b = &__C3D_Context.bufInfo;
  if (b->bufCount == n && b->base_paddr == RAW_BUF_BASE) {
    /* the same layout (the configs, and zero beyond n): only the offsets that change */
    u32 d = 0;
    for (int k = 0; k < 3; k++) {
      const C3D_BufCfg* c = &b->buffers[k];
      d |= k < n ? (c->flags[0] ^ want_pa[k][1]) | (c->flags[1] ^ want_pa[k][2])
                 : c->offset | c->flags[0] | c->flags[1];
    }
    if (!d) {
      u32* p = raw_ptr();
      for (int k = 0; k < n; k++) {
        const u32 off = want_pa[k][0] - RAW_BUF_BASE;
        if (b->buffers[k].offset != off) {
          p = raw_w(p, GPUREG_ATTRIBBUFFER0_OFFSET + 3 * (u32)k, 0xF, off);
          b->buffers[k].offset = off;
        }
      }
      raw_end(p);
      return;
    }
  }
  u32 want[3][3];
  for (int k = 0; k < 3; k++) {
    want[k][0] = k < n ? want_pa[k][0] - RAW_BUF_BASE : 0;
    want[k][1] = k < n ? want_pa[k][1] : 0;
    want[k][2] = k < n ? want_pa[k][2] : 0;
  }
  if (b->bufCount < 1 || b->bufCount > 3 || b->base_paddr != RAW_BUF_BASE) {
    /* not citro3d's layout of one to three buffers yet: citro3d sends all of it (raw_sync) */
    BufInfo_Init(b);
    b->bufCount = n;
    for (int k = 0; k < n; k++) {
      b->buffers[k].offset = want[k][0];
      b->buffers[k].flags[0] = want[k][1];
      b->buffers[k].flags[1] = want[k][2];
    }
    __C3D_Context.flags |= C3DiF_BufInfo;
    return;
  }
  for (int k = 0; k < 3; k++) {
    C3D_BufCfg* c = &b->buffers[k];
    if (c->flags[0] != want[k][1] || c->flags[1] != want[k][2]) {
      raw_end(raw_n(raw_ptr(), GPUREG_ATTRIBBUFFER0_OFFSET + 3 * k, want[k], 3, 1));
    } else if (c->offset != want[k][0]) {
      raw_end(raw_w(raw_ptr(), GPUREG_ATTRIBBUFFER0_OFFSET + 3 * k, 0xF, want[k][0]));
    } else {
      continue;
    }
    c->offset = want[k][0];
    c->flags[0] = want[k][1];
    c->flags[1] = want[k][2];
  }
  b->bufCount = n;
}
static void raw_vertex_buffers(int n, u32 pa0, u32 f00, u32 f01, u32 pa1, u32 f10, u32 f11) {
  const u32 w[2][3] = {{pa0, f00, f01}, {pa1, f10, f11}};
  raw_vertex_buffers_n(n, w);
}

/* attribute buffer 0 (the only one most draws use) at physical address pa */
static void raw_vertex_buffer(u32 pa, u32 flags0, u32 flags1) {
  raw_vertex_buffers(1, pa, flags0, flags1, 0, 0, 0);
}
/* attribute buffer 0 config of each program's vertex layout: permutation, stride, attributes */
#define RAW_BUF_FLAGS1(stride, attribs, perm_hi) \
  ((u32)(perm_hi) | ((u32)(stride) << 16) | ((u32)(attribs) << 28))

/* float uniforms from loc: f32 values (4 per vector, in C3D_FVec order w z y x) or float24 words
 * (3 per vector), one config write and one burst to the data port */
static inline u32* raw_unif(u32* p, int loc, const u32* data, u32 words, int f32) {
  p = raw_w(p, GPUREG_VSH_FLOATUNIFORM_CONFIG, 0xF, (u32)loc | (f32 ? BIT(31) : 0));
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
  {
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
  raw_end(raw_vc_clear(p));
  __C3D_Context.flags |= C3DiF_DrawUsed;
}

/* prim: GPU_TRIANGLES or (AI-assisted) GPU_TRIANGLE_STRIP (ctr_gpu_draw_strip), as C3D_DrawArrays */
static void raw_draw_arrays(u32 count, u32 prim) {
  u32* p = raw_ptr();
  for (int i = 0; i < 22; i += 2) {
    p[i] = kRawArrays[i];
    p[i + 1] = kRawArrays[i + 1];
  }
  p[0] = prim;
  p[6] = count;
  raw_end(p + 22);
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

/* a static mesh's attribute buffers for the programs of its layout (position, texcoord, color):
 * one, or with ctr_gpu_mesh_split_colors two (12-byte records, the colors) */
static void raw_mesh_buffers(const MeshSlot* ms) {
  C3D_BufInfo* b = &__C3D_Context.bufInfo;
  if (ms->col_off && b->bufCount == 2 && b->base_paddr == RAW_BUF_BASE &&
      b->buffers[0].flags[0] == 0x10 && b->buffers[0].flags[1] == RAW_BUF_FLAGS1(12, 2, 0) &&
      b->buffers[1].flags[0] == 0x2 && b->buffers[1].flags[1] == RAW_BUF_FLAGS1(4, 1, 0) &&
      !(b->buffers[2].offset | b->buffers[2].flags[0] | b->buffers[2].flags[1])) {
    /* the split layout in place: only its offsets */
    const u32 off0 = ms->verts_pa - RAW_BUF_BASE, off1 = off0 + ms->col_off;
    u32* p = raw_ptr();
    if (b->buffers[0].offset != off0) {
      p = raw_w(p, GPUREG_ATTRIBBUFFER0_OFFSET, 0xF, off0);
      b->buffers[0].offset = off0;
    }
    if (b->buffers[1].offset != off1) {
      p = raw_w(p, GPUREG_ATTRIBBUFFER0_OFFSET + 3, 0xF, off1);
      b->buffers[1].offset = off1;
    }
    raw_end(p);
    return;
  }
  if (!ms->col_off) {
    raw_vertex_buffer(ms->verts_pa, 0x210, RAW_BUF_FLAGS1(16, 3, 0));
  } else {
    raw_vertex_buffers(2, ms->verts_pa, 0x10, RAW_BUF_FLAGS1(12, 2, 0), ms->verts_pa + ms->col_off,
                       0x2, RAW_BUF_FLAGS1(4, 1, 0));
  }
}
/* through citro3d; extra: a further buffer of one attribute (3: wind slots, normals), or NULL */
static void c3d_mesh_buffers(const MeshSlot* ms, const void* extra, u32 extra_stride) {
  if (!ms->col_off && !extra) {
    c3d_vertex_buffer(ms->verts_pa, ms->verts, 16, 3, 0x210);
    return;
  }
  if (!extra) {
    const C3D_BufInfo* b = &__C3D_Context.bufInfo;
    const u32 off0 = ms->verts_pa - RAW_BUF_BASE;
    if (b->bufCount == 2 && b->base_paddr == RAW_BUF_BASE && b->buffers[0].offset == off0 &&
        b->buffers[1].offset == off0 + ms->col_off && b->buffers[0].flags[0] == 0x10 &&
        b->buffers[0].flags[1] == RAW_BUF_FLAGS1(12, 2, 0) && b->buffers[1].flags[0] == 0x2 &&
        b->buffers[1].flags[1] == RAW_BUF_FLAGS1(4, 1, 0)) {
      return;
    }
  }
  if (extra) {
    /* what BufInfo_Init and BufInfo_Add below make, compared first: a run of wind or envmapped
     * tie draws of one chunk keeps its buffers */
    const C3D_BufInfo* b = &__C3D_Context.bufInfo;
    const u32 off0 = ms->verts_pa - RAW_BUF_BASE;
    u32 want[3][3];
    int n = 0;
    if (ms->col_off) {
      want[n][0] = off0;
      want[n][1] = 0x10;
      want[n++][2] = RAW_BUF_FLAGS1(12, 2, 0);
      want[n][0] = off0 + ms->col_off;
      want[n][1] = 0x2;
      want[n++][2] = RAW_BUF_FLAGS1(4, 1, 0);
    } else {
      want[n][0] = off0;
      want[n][1] = 0x210;
      want[n++][2] = RAW_BUF_FLAGS1(16, 3, 0);
    }
    want[n][0] = osConvertVirtToPhys(extra) - RAW_BUF_BASE;
    want[n][1] = 0x3;
    want[n++][2] = RAW_BUF_FLAGS1(extra_stride, 1, 0);
    int same = b->bufCount == n && b->base_paddr == RAW_BUF_BASE;
    for (int k = 0; k < n && same; k++) {
      same = b->buffers[k].offset == want[k][0] && b->buffers[k].flags[0] == want[k][1] &&
             b->buffers[k].flags[1] == want[k][2];
    }
    if (same) {
      return;
    }
  }
  C3D_BufInfo* buf = C3D_GetBufInfo();
  BufInfo_Init(buf);
  if (ms->col_off) {
    BufInfo_Add(buf, ms->verts, 12, 2, 0x10);
    BufInfo_Add(buf, (const u8*)ms->verts + ms->col_off, 4, 1, 0x2);
  } else {
    BufInfo_Add(buf, ms->verts, 16, 3, 0x210);
  }
  if (extra) {
    BufInfo_Add(buf, extra, extra_stride, 1, 0x3);
  }
}

static void apply_state_tint(const ctr_draw_state* st, int mesh, uint32_t tint, int raw);
static void apply_state(const ctr_draw_state* st, int mesh, int raw);
static int s_ocean_fade_ok; /* (the PS2 ocean's reflection fade tables, ocean_fade_init) */

/* The command buffer is fixed size (libctru panics when it overflows). When the next draw might
 * not fit, what is built so far goes to the GPU, the render thread waits until the GPU has run
 * everything queued, and the buffer starts over: one stall, the frame continues (no draw dropped,
 * no mode switch). 0: no room (the HOME Menu took the GPU, or the GPU didn't finish in 2 s). */
static int cmd_flush_wait(void) {
  if (!g.in_frame) {
    return 0;
  }
  const u64 t0 = svcGetSystemTick();
  u32* base;
  u32 size;
  if (s_pipeline) {
    if (s_bd.frame) {
      bd_part();
    } else {
      pipeline_submit();
    }
    base = s_cmdbufs[s_k];
    size = PIPE_CMD_BYTES / 4;
  } else {
    if (!gx_begin()) {
      return 0;
    }
    flush_vbuf();
    C3D_FrameSplit(GX_CMDLIST_FLUSH);
    gxCmdQueueRun(&__C3D_Context.gxQueue);
    gx_end();
    base = __C3D_Context.cmdBuf;
    size = (u32)__C3D_Context.cmdBufSize;
  }
  if (gx_lost() || !g.in_frame) {
    return 0;
  }
  diag_rt("command buffer full: waiting for the GPU");
  if (!gxCmdQueueWait(&__C3D_Context.gxQueue, GX_WAIT_MS(2000))) {
    printf("[ctr] command buffer full and the GPU didn't finish in 2 s: draws dropped\n");
    return 0;
  }
  diag_rt("draw build");
  g.cmd_flushed_words += (u32)(gpuCmdBuf + gpuCmdBufOffset - g.cmd_base);
  GPUCMD_SetBuffer(base, size, 0);
  g.cmd_base = gpuCmdBuf;
  g.cur.cmd_splits++;
  g.cur.gpu_wait_ms += (float)((double)(svcGetSystemTick() - t0) / (SYSCLOCK_ARM11 / 1000.0));
  return 1;
}

/* room for words more command words (and the frame end's reserve): flushed and waited for when
 * there isn't */
static int cmd_room_for(u32 words) {
  if (gpuCmdBufSize - gpuCmdBufOffset >= CMD_RESERVE_WORDS + words) {
    return 1;
  }
  if (cmd_flush_wait() && gpuCmdBufSize - gpuCmdBufOffset >= CMD_RESERVE_WORDS + words) {
    return 1;
  }
  g.cur.dropped_draws++;
  return 0;
}
static int cmd_room(void) {
  return cmd_room_for(CMD_DRAW_WORDS);
}

/* c3l v23: the alpha mask on texture unit 1 (instead of the fog ramp) or 2, bound
 * with its texture's sampling (filter, wrap, mip levels: the same texels' alpha as the RGBA8
 * texture had) */
static const C3D_Tex* s_ocean_tex2;
static const void* s_etie_tex2;
static const void* s_mask_data[3];
static u32 s_mask_param[3], s_mask_lod[3];
static void bind_mask(TexSlot* m, const C3D_Tex* color, int unit) {
  C3D_Tex* t = &m->tex;
  t->param = color->param & ~(u32)GPU_TEXTURE_ETC1_PARAM;
  /* the mask's own levels: a colour texture can have more */
  const u8 mask_max = m->levels > 0 ? (u8)(m->levels - 1) : 0;
  t->maxLevel = color->maxLevel < mask_max ? color->maxLevel : mask_max;
  t->minLevel = color->minLevel < t->maxLevel ? color->minLevel : t->maxLevel;
  t->lodBias = color->lodBias;
  const C3D_Tex** cur = unit == 1 ? &s_tex1_mask : &s_tex2_mask;
  if (*cur == t && s_mask_data[unit] == t->data && s_mask_param[unit] == t->param &&
      s_mask_lod[unit] == t->lodParam) {
    return;
  }
  C3D_TexBind(unit, t);
  *cur = t;
  s_mask_data[unit] = t->data;
  s_mask_param[unit] = t->param;
  s_mask_lod[unit] = t->lodParam;
  if (unit == 2) {
    g.ocean_tex2_bound = 1; /* the ocean and the envmapped ties bind theirs again */
    s_ocean_tex2 = NULL;
    s_etie_tex2 = NULL;
  }
  g.cur.tex_binds++;
}
static void unit2_mask_release(void) {
  C3Di_Context* ctx = &__C3D_Context;
  ctx->tex[2] = NULL;
  ctx->flags |= C3DiF_Tex(2);
  g.ocean_tex2_bound = 0;
  s_ocean_tex2 = NULL;
  s_etie_tex2 = NULL;
  s_tex2_mask = NULL;
}

/* Texture unit 0: only bound again (which clears the GPU's texture cache) when the texture, its
 * memory (pools move to VRAM) or its parameters change. */
static void bind_texture(TexSlot* slot, const ctr_draw_state* st, int raw) {
  C3D_Tex* tex = &slot->tex;
  const GPU_TEXTURE_FILTER_PARAM f = st->filter ? GPU_LINEAR : GPU_NEAREST;
  u32 param = tex->param & ~(GPU_TEXTURE_MAG_FILTER(GPU_LINEAR) | GPU_TEXTURE_MIN_FILTER(GPU_LINEAR) |
                             GPU_TEXTURE_WRAP_S(3) | GPU_TEXTURE_WRAP_T(3) |
                             GPU_TEXTURE_MIP_FILTER(GPU_LINEAR) | GPU_TEXTURE_MODE(7));
  /* (AI-assisted) STQ is affine on the GS; divide by interpolated Q in the texture sampler.
   * Position W stays one so primary color, fog and reflection coordinates stay screen-affine. */
  if (g.cur_prog == PROG_OCEAN_FULL || g.cur_prog == PROG_OCEAN_FULL_CLIP) {
    param |= GPU_TEXTURE_MODE(GPU_TEX_PROJECTION);
  }
  param |= GPU_TEXTURE_MAG_FILTER(f) | GPU_TEXTURE_MIN_FILTER(f) |
           GPU_TEXTURE_WRAP_S(st->clamp_s ? GPU_CLAMP_TO_EDGE : GPU_REPEAT) |
           GPU_TEXTURE_WRAP_T(st->clamp_t ? GPU_CLAMP_TO_EDGE : GPU_REPEAT);
  /* mip levels: the nearest one (bilinear inside it), or blending two (trilinear); unfiltered
   * draws sample the top level only, as the PC (GL_NEAREST) */
  u8 max_level = 0;
  const int tri = (st->tcc & CTR_TCC_TRILINEAR) != 0;
  if (slot->levels > 1 && st->filter && (g_mip_mode > 0 || tri)) {
    max_level = (u8)(slot->levels - 1);
    if ((st->tcc & CTR_TCC_MIP_MAX1) && max_level > 1) {
      max_level = 1;
    }
    if (g_mip_mode == 2 || tri) {
      param |= GPU_TEXTURE_MIP_FILTER(GPU_LINEAR);
    }
  }
  if (g.bound_tex == tex && g.bound_data == tex->data && g.bound_param == param &&
      tex->maxLevel == max_level) {
    return;
  }
  tex->param = param;
  tex->maxLevel = max_level;
  if (raw) {
    /* (AI-assisted) what C3Di_SetTex and citro3d's texture unit update write: the unit's
     * registers, the unit on, then its cache cleared; the context gets the texture (invariant) */
    C3Di_Context* ctx = &__C3D_Context;
    const u32 regs[5] = {tex->border, tex->dim, tex->param, tex->lodParam,
                         osConvertVirtToPhys(tex->data) >> 3};
    u32* p = raw_ptr();
    p = raw_n(p, GPUREG_TEXUNIT0_BORDER_COLOR, regs, 5, 1);
    p = raw_w(p, GPUREG_TEXUNIT0_TYPE, 0xF, tex->fmt);
    ctx->tex[0] = tex;
    if (!(ctx->texConfig & 1)) {
      ctx->texConfig |= 1;
      p = raw_w(p, GPUREG_TEXUNIT_CONFIG, 0xB, ctx->texConfig);
    }
    /* every bind clears the texture cache, as citro3d does (skipping it left stale texels:
     * the red patches on the Volcanic Crater minecarts) */
    p = raw_w(p, GPUREG_TEXUNIT_CONFIG, 0x4, BIT(16));
    raw_end(p);
  } else {
    C3D_TexBind(0, tex);
  }
  g.bound_tex = tex;
  g.bound_data = tex->data;
  g.bound_param = param;
  g.cur.tex_binds++;
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

/* mesh: 0 = immediate draws, 1 = level mesh, 2 = skinned mesh (tint = constant color stage).
 * (AI-assisted) 5 = level mesh with the fog amount in the vertex alpha (fog_alpha: its vertex
 * alpha is 0x80, the constant 0x80 stands in for it).
 * raw: written as raw commands (see "raw GPU commands"), else through citro3d. Both keep
 * citro3d's context right. */
static void apply_state_tint(const ctr_draw_state* st, int mesh, uint32_t tint, int raw) {
  _Static_assert(sizeof(*st) == 16, "ctr_draw_state compared as 4 words");
  if (g.last_state_valid && g.last_state_mesh == mesh && g.last_tint == tint &&
      words_equal(&g.last_state, st, 4)) {
    return;
  }
  g.last_state = *st;
  g.last_state_mesh = mesh;
  g.last_tint = tint;
  g.last_state_valid = 1;
  const int stex0 = tex_res(st->tex); /* (AI-assisted) an animated slot's texture */
  const int textured0 = stex0 >= 0 && stex0 < MAX_TEXTURES && g.textures[stex0].used == 1;
  /* c3l v23: the texture's alpha mask, for the programs that give it coordinates
   * (unit 2 for merc, unit 1 for the fog_alpha level meshes and envmapped ties) */
  const int mask_prog =
      g.cur_prog == PROG_SKIN_MASK || g.cur_prog == PROG_ETIE_FA ||
      (mesh == 5 && (g.cur_prog == PROG_MESH_FA || g.cur_prog == PROG_MESH_DF_FA ||
                     g.cur_prog == PROG_MESH_MORPH_FA || g.cur_prog == PROG_TFRAG_MORPH_FA));
  const int smask = textured0 && mask_prog ? tex_mask(stex0) : -1;
  /* stage 1: lighting tint for skinned meshes, pass-through otherwise */
  C3D_TexEnv e1;
  C3D_TexEnv* env1 = &e1;
  C3D_TexEnvInit(env1);
  if (mesh == 2) {
    C3D_TexEnvSrc(env1, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, 0);
    C3D_TexEnvFunc(env1, C3D_RGB, GPU_MODULATE);
    C3D_TexEnvColor(env1, tint);
  } else if (mesh == 6 && tint && s_ocean_fade_ok > 0) {
    /* (AI-assisted) the PS2 ocean with the sky reflection: the fog with the fog color constant
     * (unit 1 holds the depth fade table, ocean_fade_init), alpha = the reflection's weight times
     * the fade */
    C3D_TexEnvSrc(env1, C3D_RGB, GPU_CONSTANT, GPU_PREVIOUS, GPU_TEXTURE1);
    C3D_TexEnvOpRgb(env1, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR,
                    GPU_TEVOP_RGB_SRC_ALPHA);
    C3D_TexEnvFunc(env1, C3D_RGB, GPU_INTERPOLATE);
    C3D_TexEnvColor(env1, g.fog_abgr);
    C3D_TexEnvSrc(env1, C3D_Alpha, GPU_PREVIOUS, GPU_TEXTURE1, 0);
    C3D_TexEnvOpAlpha(env1, GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_R, GPU_TEVOP_A_SRC_ALPHA);
    C3D_TexEnvFunc(env1, C3D_Alpha, GPU_MODULATE);
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
  /* (AI-assisted) compact textures store GS alpha doubled (0xff = 1.0) */
  const GPU_TEVSRC tsrc = GPU_TEXTURE0;
  const int afull = textured && slot->alpha_full;
  if (textured) {
    bind_texture(slot, st, raw);
    if (smask >= 0) {
      bind_mask(&g.textures[smask], &slot->tex, mesh == 2 ? 2 : 1);
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
        C3D_TexEnvSrc(env, C3D_Alpha, smask >= 0 ? GPU_TEXTURE1 : tsrc, GPU_CONSTANT, 0);
        C3D_TexEnvColor(env, 0x80000000u);
      } else if (smask >= 0) {
        C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE2, GPU_PRIMARY_COLOR, 0); /* merc */
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
  if (mesh == 7) {
    /* (AI-assisted, j2water2; mesh 7, as 6 is j1ocean's ocean) Jak II's envmapped ties (ctr_gpu_etie_draw), tint = the shine
     * (abgr, halved unless decal): stage 0 rgb = env (unit 2) * shine, alpha = the base alpha (as
     * the level meshes'); stage 1 rgb * alpha (the PC's destination alpha weight); stage 2 + the
     * base color (tex * color, x2: hence the halved shine; decal: + tex); stage 3 the fog (as the
     * level meshes' stage 1). The PC (Tie3 envmap_second_pass_draw) adds the shine unfogged. */
    C3D_TexEnvInit(env);
    C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE2, GPU_CONSTANT, 0);
    C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
    C3D_TexEnvColor(env, tint);
    C3D_TexEnvSrc(env, C3D_Alpha, textured ? tsrc : GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, 0);
    if (smask >= 0) {
      /* ctr_etie_fa: the mask times the vertex alpha 0x80 (the color's alpha is the
       * fog amount) */
      C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE1, GPU_CONSTANT, 0);
      C3D_TexEnvColor(env, (tint & 0x00FFFFFFu) | 0x80000000u);
    }
    C3D_TexEnvFunc(env, C3D_Alpha, textured ? GPU_MODULATE : GPU_REPLACE);
    C3D_TexEnvScale(env, C3D_Alpha, GPU_TEVSCALE_2);
    C3D_TexEnvInit(env1);
    C3D_TexEnvSrc(env1, C3D_RGB, GPU_PREVIOUS, GPU_PREVIOUS, 0);
    C3D_TexEnvOpRgb(env1, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_ALPHA, 0);
    C3D_TexEnvFunc(env1, C3D_RGB, GPU_MODULATE);
    C3D_TexEnv e2, e3;
    C3D_TexEnvInit(&e2);
    if (textured && st->decal) {
      C3D_TexEnvSrc(&e2, C3D_RGB, tsrc, GPU_PREVIOUS, 0);
      C3D_TexEnvFunc(&e2, C3D_RGB, GPU_ADD);
    } else if (textured) {
      C3D_TexEnvSrc(&e2, C3D_RGB, tsrc, GPU_PRIMARY_COLOR, GPU_PREVIOUS);
      C3D_TexEnvFunc(&e2, C3D_RGB, GPU_MULTIPLY_ADD);
      C3D_TexEnvScale(&e2, C3D_RGB, GPU_TEVSCALE_2);
    } else {
      C3D_TexEnvSrc(&e2, C3D_RGB, GPU_PRIMARY_COLOR, GPU_PREVIOUS, 0);
      C3D_TexEnvFunc(&e2, C3D_RGB, GPU_ADD);
    }
    C3D_TexEnvInit(&e3);
    if (smask >= 0) {
      /* ctr_etie_fa: the fog as the fog_alpha level meshes' (mesh 5's stage 1) */
      C3D_TexEnvSrc(&e3, C3D_RGB, GPU_CONSTANT, GPU_PREVIOUS, GPU_PRIMARY_COLOR);
      C3D_TexEnvOpRgb(&e3, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR,
                      GPU_TEVOP_RGB_SRC_ALPHA);
      C3D_TexEnvFunc(&e3, C3D_RGB, GPU_INTERPOLATE);
      C3D_TexEnvColor(&e3, g.fog_abgr);
    } else if (g.fog_tex_valid) {
      C3D_TexEnvSrc(&e3, C3D_RGB, GPU_TEXTURE1, GPU_PREVIOUS, GPU_TEXTURE1);
      C3D_TexEnvOpRgb(&e3, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR,
                      GPU_TEVOP_RGB_SRC_ALPHA);
      C3D_TexEnvFunc(&e3, C3D_RGB, GPU_INTERPOLATE);
    }
    commit_texenv(2, &e2, raw);
    commit_texenv(3, &e3, raw);
    g.ocean_stage2 = 1; /* stage 2 back to pass-through at the next other draw */
    g.etie_stage3 = 1;
  }
  commit_texenv(1, env1, raw);
  commit_texenv(0, env, raw);
  /* (AI-assisted) unit 1 on (the context's texConfig is what the GPU has, or gets: citro3d
   * recomputes it from its bound textures when a bind is pending) */
  /* (AI-assisted, far-fog) a merc draw with the game's fog (CTR_TCC_FOG): the fog ramp stage in
   * stage 2 (the ocean's), its amount from texture unit 1 (ctr_skin_opt's texcoord1) */
  const int merc_fog = mesh == 2 && (st->tcc & CTR_TCC_FOG) && g.fog_tex_valid;
  /* unit 1 back to the fog ramp after an alpha mask, for the draws that read it */
  const int need_ramp = mesh == 1 || mesh == 6 || (mesh == 7 && smask < 0) || merc_fog;
  if (need_ramp && s_tex1_mask && g.fog_tex_valid) {
    tex1_bind(&g.fog_tex);
  }
  if (s_tex2_mask && mesh != 6 && mesh != 7 && !(smask >= 0 && mesh == 2)) {
    unit2_mask_release();
  }
  if (g.fog_tex_valid) {
    C3Di_Context* tctx = &__C3D_Context;
    const int need1 = need_ramp || (smask >= 0 && mesh != 2);
    const int is_off = !(tctx->texConfig & BIT(1));
    if (raw && is_off) {
      tctx->texConfig |= BIT(1);
      raw_end(raw_w(raw_ptr(), GPUREG_TEXUNIT_CONFIG, 0xB, tctx->texConfig & ~BIT(16)));
      g.cur.unit1_toggles++;
    } else if (!raw && need1 && is_off) {
      tctx->flags |= C3DiF_Tex(1); /* citro3d binds it again (unit on, cache cleared) */
    }
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
  } else if (merc_fog) {
    /* (AI-assisted, far-fog) rgb = mix(previous, fog color, fog amount), as the ocean's stage 2.
     * (AI-assisted) Built every time: commit_texenv sends it only when stage 2 differs. A flag
     * shortcut here lost the fog after the clouds (ctr_gpu_draw_clip2) or Jak II's ocean env /
     * etie had changed stage 2 behind it. */
    C3D_TexEnv e2;
    C3D_TexEnvInit(&e2);
    C3D_TexEnvSrc(&e2, C3D_RGB, GPU_TEXTURE1, GPU_PREVIOUS, GPU_TEXTURE1);
    C3D_TexEnvOpRgb(&e2, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR,
                    GPU_TEVOP_RGB_SRC_ALPHA);
    C3D_TexEnvFunc(&e2, C3D_RGB, GPU_INTERPOLATE);
    commit_texenv(2, &e2, raw);
    g.ocean_stage2 = 1;
  } else if (mesh != 7 && !merc_fog && g.ocean_stage2) {
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
   * stays enabled so the stencil's depth-pass operation is the one that runs, and writes are
   * controlled by the write mask only (clearing the enable bit doesn't stop depth writes on the
   * hardware): no color for NO_COLOR, no alpha for CTR_BLEND_MUL */
  const int shadow_pass =
      (st->flags & (CTR_STATE_STENCIL_INCR | CTR_STATE_STENCIL_DECR | CTR_STATE_STENCIL_CLEAR));
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
  /* (AI-assisted) cull_ties: back faces of closed tie surfaces (CTR_TCC_CULL_BACK); cull_merc:
   * merc draws the caller marked (CTR_TCC_CULL_OWN) */
  e.cullMode = (st->tcc & CTR_TCC_CULL_BACK) && (g_cull_enable || (st->tcc & CTR_TCC_CULL_OWN))
                   ? (g_cull_flip ? GPU_CULL_FRONT_CCW : GPU_CULL_BACK_CCW)
                   : GPU_CULL_NONE;
  /* (AI-assisted) the stencil marks (CTR_STATE_STENCIL_*): 1 where a STENCIL_SET draw passed the
   * depth test, STENCIL_OUT draws where it isn't 1. Off: only the enable bit changes. */
  if ((st->flags & CTR_STATE_STENCIL_SET)) {
    e.stencilMode = 1u | ((u32)GPU_ALWAYS << 4) | (0xFFu << 8) | (1u << 16) | (0xFFu << 24);
    e.stencilOp = (u32)GPU_STENCIL_KEEP | ((u32)GPU_STENCIL_KEEP << 4) |
                  ((u32)GPU_STENCIL_REPLACE << 8);
  } else if ((st->flags & CTR_STATE_STENCIL_OUT)) {
    e.stencilMode = 1u | ((u32)GPU_NOTEQUAL << 4) | (1u << 16) | (0xFFu << 24);
    e.stencilOp = 0;
  } else if ((st->flags & (CTR_STATE_STENCIL_INCR | CTR_STATE_STENCIL_DECR))) {
    /* (AI-assisted) shadow volumes: + / - 1 where the face passes the depth test, saturating like
     * the PC renderer's GL_INCR / GL_DECR (all front faces are drawn before the back faces, so a
     * pixel ends at max(0, front - back): where the near plane cut a front face away, no shadow
     * instead of a dark streak) */
    e.stencilMode = 1u | ((u32)GPU_ALWAYS << 4) | (0xFFu << 8) | (0xFFu << 24);
    e.stencilOp = (u32)GPU_STENCIL_KEEP | ((u32)GPU_STENCIL_KEEP << 4) |
                  ((u32)((st->flags & CTR_STATE_STENCIL_INCR) ? GPU_STENCIL_INCR : GPU_STENCIL_DECR)
                   << 8);
  } else if ((st->flags & CTR_STATE_STENCIL_CLEAR)) {
    /* (AI-assisted) where the stencil isn't 0 and the depth test passes; set to 0 there (kept
     * where the depth test fails: another face of the draw may pass it there) */
    e.stencilMode = 1u | ((u32)GPU_NOTEQUAL << 4) | (0xFFu << 8) | (0xFFu << 24);
    e.stencilOp = (u32)GPU_STENCIL_KEEP | ((u32)GPU_STENCIL_KEEP << 4) |
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
  /* a stencil change: alpha test, stencil test, stencil op and depth/color mask in one block, as
   * citro3d writes them (the shadows' INCR -> DECR change was the only lone STENCIL_OP write) */
  const int stencil_block = e.stencilMode != c->stencilMode || e.stencilOp != c->stencilOp;
  if (stencil_block) {
    p = raw_n(p, GPUREG_FRAGOP_ALPHA_TEST, &e.alphaTest, 4, 1);
    if ((e.depthTest ^ c->depthTest) & 0x70u) {
      p = raw_w(p, GPUREG_GAS_DELTAZ_DEPTH, 0x8,
                (u32)GPU_MAKEGASDEPTHFUNC((e.depthTest >> 4) & 7) << 24);
    }
  } else if (e.alphaTest != c->alphaTest) {
    p = raw_w(p, GPUREG_FRAGOP_ALPHA_TEST, 0xF, e.alphaTest);
  }
  if (e.depthTest != c->depthTest && !stencil_block) {
    p = raw_w(p, GPUREG_GAS_DELTAZ_DEPTH, 0x8,
              (u32)GPU_MAKEGASDEPTHFUNC((e.depthTest >> 4) & 7) << 24);
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
  raw_end(p);
  *c = e;
}

/* (AI-assisted) raw_cmds: the draws write their commands themselves */
static inline int use_raw(const ctr_draw_state* st) {
  (void)st;
  return s_raw.on;
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

static void draw_clip_prog(const ctr_draw_state* state, const ctr_clip_vertex* verts, int count,
                           int prog) {
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
  draw_immediate(state, prog, dst, sizeof(ctr_clip_vertex), count, GPU_TRIANGLES);
}

void ctr_gpu_draw_clip(const ctr_draw_state* state, const ctr_clip_vertex* verts, int count) {
  draw_clip_prog(state, verts, count, PROG_CLIP);
}

void ctr_gpu_draw_clip_pz(const ctr_draw_state* state, const ctr_clip_vertex* verts, int count) {
  draw_clip_prog(state, verts, count, PROG_CLIP_PZ);
}

void ctr_gpu_draw_clip2(const ctr_draw_state* state, uint32_t const0, uint32_t const1,
                        const ctr_clip_vertex2* verts, int count) {
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
  use_program(PROG_CLIP2);
  /* the texture on units 0 and 1 (binding unit 1 clears the texture cache like any bind) */
  TexSlot* slot = &g.textures[state->tex];
  bind_texture(slot, state, 0);
  tex1_bind(&slot->tex);
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
    C3D_DepthTest(true, map_test(state->ztest), GPU_WRITE_COLOR);
  }
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
  g.ocean_stage2 = 0; /* (AI-assisted) stage 2 is pass-through again */
  if (g.fog_tex_valid) {
    tex1_bind(&g.fog_tex);
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

/* The sky reflection's depth fade (ctr_gpu_ocean_full_draw_env): texture unit 1 holds a table
 * instead of the fog ramp for those draws, s = the fog amount (alpha, the fog ramp's), t = Q * 2^
 * (CTR_OCEAN_ENV_FADE_K - 1.5) (the clip program's texcoord1.y): rgb = clamp(1.5 + log2 t, lo, 1)
 * = the fade, lo 0 (mid ocean) or 0.5 (near ocean, [1]); the fog color is a combiner constant. */
static C3D_Tex s_ocean_fade[2];
static int s_ocean_fade_ok = -1; /* -1 not made yet, 0 no memory */

static void ocean_fade_init(void) {
  s_ocean_fade_ok = 0;
  enum { N = 64 };
  for (int k = 0; k < 2; k++) {
    C3D_Tex* t = &s_ocean_fade[k];
    ctr_linear_lock();
    const int ok = linear_room_for_tex(N, N, 0) && C3D_TexInit(t, N, N, GPU_RGBA8);
    ctr_linear_unlock();
    if (!ok) {
      if (k) {
        ctr_linear_lock();
        C3D_TexDelete(&s_ocean_fade[0]);
        ctr_linear_unlock();
      }
      return;
    }
    C3D_TexSetFilter(t, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(t, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    u8* d = (u8*)t->data;
    for (int y = 0; y < N; y++) {
      const float tc = ((float)y + 0.5f) / (float)N; /* (image row y from the top: t = 0 there) */
      float f = 1.5f + log2f(tc);
      const float lo = k ? 0.5f : 0.0f;
      f = f < lo ? lo : (f > 1.0f ? 1.0f : f);
      const u8 v = (u8)(f * 255.0f + 0.5f);
      for (int x = 0; x < N; x++) {
        u8* o = d + 4 * tiled_texel(x, y, N, N);
        o[0] = (u8)(x * 255 / (N - 1)); /* A B G R: alpha the fog ramp's */
        o[1] = o[2] = o[3] = v;
      }
    }
    cache_flush_now(t->data, t->size);
  }
  s_ocean_fade_ok = 1;
}

/* texture unit 1: the fog ramp again after the fade tables */
static void ocean_fade_unbind(void) {
  const C3D_Tex* t1 = __C3D_Context.tex[1];
  if (g.fog_tex_valid && (t1 == &s_ocean_fade[0] || t1 == &s_ocean_fade[1])) {
    tex1_bind(&g.fog_tex);
  }
}

int ctr_gpu_ocean_full_reserve(int max_verts, int max_indices, ctr_ocean_full_vertex** verts,
                          uint16_t** indices) {
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
static const C3D_Tex* s_ocean_tex2; /* (AI-assisted, j1hang) defined with ocean_bind_tex2_wrap */

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
    if (s_ocean_fade_ok < 0) {
      ocean_fade_init();
    }
    C3D_Tex* fade = &s_ocean_fade[(state->tcc & CTR_TCC_MIP_MAX1) ? 1 : 0];
    if (s_ocean_fade_ok > 0 && __C3D_Context.tex[1] != fade) {
      tex1_bind(fade);
    }
  } else {
    ocean_fade_unbind();
  }
  /* (AI-assisted, j1hang) a draw without the reflection: texture unit 2 off (no unit on whose
   * coordinates the program may not output: ctr_ocean_full.v.pica has no texcoord2) */
  if (!env && g.ocean_tex2_bound) {
    __C3D_Context.tex[2] = NULL;
    __C3D_Context.flags |= C3DiF_Tex(2);
    g.ocean_tex2_bound = 0;
    s_tex2_mask = NULL;
    s_ocean_tex2 = NULL;
  }
  /* (AI-assisted, j1hang) all 4 attributes (40 bytes, the whole stride) for both programs */
  const int attribs = 4;
  const u32 perm = 0x3210;
  if (use_raw(state)) {
    apply_state_tint(state, 6, (uint32_t)env, 1);
    raw_vertex_buffer(raw_vbuf_pa(voff), perm,
                      RAW_BUF_FLAGS1(sizeof(ctr_ocean_full_vertex), attribs, 0));
    raw_sync();
    raw_draw_elements(raw_vbuf_pa(ioff), (u32)index_count);
  } else {
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

unsigned int ctr_gpu_vertex_room(void) {
  return g.ready && g.in_frame && g.vbuf_used < VBUF_BYTES ? (unsigned int)(VBUF_BYTES - g.vbuf_used)
                                                           : 0;
}

static void gl_to_pica_mul(const float clip[16], C3D_Mtx* m);

/* (AI-assisted, j2gpu) matrix (row major) != NULL: the first n_mtx ranges' vertices go through it
 * in the vertex shader (the clip program's glpica uniform = gl_to_pica * matrix for them, then
 * gl_to_pica again) */
static void clip_draw_ranges(const float* matrix, int n_mtx, const ctr_draw_state* states,
                             const int* first, const int* count, int n, int vert_count,
                             int index_count) {
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
  /* all of the draws or none (a shadow volume's passes leave the stencil at 0 only together): room
   * for each one's commands, the buffer flushed first when short */
  if (!cmd_room_for((u32)draws * CMD_DRAW_WORDS)) {
    g.cur.dropped_draws += draws - 1;
    return;
  }
  use_program(PROG_CLIP);
  int stencil = 0;
  int mtx_on = 0;
  for (int i = 0; i < n; i++) {
    /* (AI-assisted, j2gpu) the matrix in the uniform for ranges [0, n_mtx), gl_to_pica after */
    const int want = matrix && i < n_mtx;
    if (want != mtx_on) {
      C3D_Mtx m;
      if (want) {
        gl_to_pica_mul(matrix, &m);
      }
      C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g.ps->uloc_clip_glpica, want ? &m : &g.gl_to_pica);
      raw_note_unif();
      mtx_on = want;
    }
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
      apply_state(st, 0, 0);
      c3d_vertex_buffer(raw_vbuf_pa(voff), g.vbuf + voff, sizeof(ctr_clip_vertex), 3, 0x210);
      C3D_DrawElements(GPU_TRIANGLES, (int)nidx, C3D_UNSIGNED_SHORT, g.vbuf + at);
    }
    g.cur.draws++;
    g.cur.triangles += (int)nidx / 3;
  }
  if (mtx_on) {
    /* (AI-assisted, j2gpu) the other clip draws' matrix again (sent at the next draw) */
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g.ps->uloc_clip_glpica, &g.gl_to_pica);
    raw_note_unif();
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

void ctr_gpu_clip_draw_ranges(const ctr_draw_state* states, const int* first, const int* count,
                              int n, int vert_count, int index_count) {
  clip_draw_ranges(NULL, 0, states, first, count, n, vert_count, index_count);
}

void ctr_gpu_clip_draw_ranges_mtx(const float matrix[16], int n_mtx, const ctr_draw_state* states,
                                  const int* first, const int* count, int n, int vert_count,
                                  int index_count) {
  clip_draw_ranges(matrix, n_mtx, states, first, count, n, vert_count, index_count);
}

/* ---------------- the screen as a texture ---------------- */

static int copy_screen_body(int keep);

/* (AI-assisted) measurement only: the CPU time of the screen copies (the command list sent so
 * far, the copy queued or, without the pipeline, waited for), ctr_gpu_take_screen_copy_ms */
static u64 s_copy_ticks;
double ctr_gpu_take_screen_copy_ms(void) {
  const double ms = (double)s_copy_ticks / (SYSCLOCK_ARM11 / 1000.0);
  s_copy_ticks = 0;
  return ms;
}

static int copy_screen(int keep) {
  if (g.screen_tex_pinned == 2) {
    return -1;
  }
  const u64 t0 = svcGetSystemTick();
  const int tex = copy_screen_body(keep);
  s_copy_ticks += svcGetSystemTick() - t0;
  return tex;
}

int ctr_gpu_copy_screen(void) {
  return copy_screen(0);
}

static int copy_screen_body(int keep) {
  if (!g.ready || !g.in_frame) {
    return -1;
  }
  if (g.screen_tex < 0) {
    /* 256x512 RGBA8 in linear memory (512 KB, made once): the
     * 240x400 color buffer fits, with the same tiled layout (see write_screenshot) */
    const int slot = reserve_tex_slot();
    if (slot < 0) {
      return -1;
    }
    TexSlot* t = &g.textures[slot];
    const GPU_TEXCOLOR fmt = GPU_RGBA8;
    const u32 bytes = 256u * 512u * 4u;
    ctr_linear_lock();
    bool in_vram = false;
    bool ok = linear_room_for_tex(256, 512, 0) && C3D_TexInit(&t->tex, 256, 512, fmt);
    if (!ok && keep) {
      /* a kept copy: linear memory first, VRAM rather than none */
      in_vram = (u32)vramSpaceFree() >= bytes + VRAM_RESERVE &&
                C3D_TexInitVRAM(&t->tex, 256, 512, fmt);
      ok = in_vram;
    }
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
  {
    const u32 row = 7680, gap = 512;
    sync_texture_copy((u32*)g.top->frameBuf.colorBuf, GX_BUFFER_DIM(row / 16, 0),
                      (u32*)g.textures[g.screen_tex].tex.data, GX_BUFFER_DIM(row / 16, gap / 16),
                      row * 50u, 8);
  }
  /* the GPU's texture cache may hold the old texels */
  g.bound_tex = NULL;
  return g.screen_tex;
}

void ctr_gpu_screen_copy_pin(int level) {
  g.screen_tex_pinned = g.ready && g.screen_tex >= 0 ? level : 0;
}

void ctr_gpu_screen_copy_release(void) {
  g.screen_tex_pinned = 0;
  if (g.ready && g.screen_tex >= 0) {
    g.screen_tex_vram = 0;
    ctr_gpu_tex_delete(g.screen_tex); /* (freed once the GPU is done with it) */
    if (g.bound_tex == &g.textures[g.screen_tex].tex) {
      g.bound_tex = NULL;
    }
    g.screen_tex = -1;
  }
}

int ctr_gpu_copy_screen_keep(void) {
  g.screen_tex_pinned = 0;
  if (g.screen_tex >= 0 && g.screen_tex_vram) {
    ctr_gpu_screen_copy_release();
  }
  const int tex = copy_screen(1);
  g.screen_tex_pinned = tex >= 0 ? 2 : 0;
  return tex;
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
  *t = 1.0f - 0.5f / 512.0f - (cx + 1.0f) * 0.5f * (399.0f / 512.0f);
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
#define MAX_RT 6 /* Jak II has 4 at once: wave, env, env warp, minimap */
#define RT_MIP_MAX 7
static struct {
  int tex1; /* texture slot + 1 (0: free) */
  int lw, lh, max_w;
  /* ctr_gpu_rt_create_mipmapped: the texture's mip levels, and the textures levels 1 and on are
   * drawn into before they are copied in. Azahar's rasterizer cache can't take draws into a mip
   * level (FramebufferHelper's invalidation takes the level's row offset from level 0's height:
   * an interval past the texture, an assert); draws into level 0 and copies into a level are fine */
  int levels;
  int mip[RT_MIP_MAX];
  void* depth;   /* D16, max_w rows x lh columns */
  int depth_vram; /* (AI-assisted) depth is in VRAM (vramFree) */
  u32 dead_frame; /* ctr_gpu_rt_delete: the depth buffer is freed 3 frames later */
} s_rt[MAX_RT];
static struct {
  int active;
  C3D_FrameBuf fb;
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

/* a w x h RGBA8 texture a mip level is drawn into (ctr_gpu_rt_create_mipmapped), in VRAM when
 * there is room */
static int rt_tex_create(int w, int h) {
  const int slot = reserve_tex_slot();
  if (slot < 0) {
    return -1;
  }
  TexSlot* t = &g.textures[slot];
  ctr_linear_lock();
  const int in_vram = (size_t)vramSpaceFree() >= (size_t)w * h * 4u + VRAM_RESERVE &&
                      C3D_TexInitVRAM(&t->tex, (u16)w, (u16)h, GPU_RGBA8);
  const int ok = in_vram ||
                 (linear_room_for_tex(w, h, 0) && C3D_TexInit(&t->tex, (u16)w, (u16)h, GPU_RGBA8));
  ctr_linear_unlock();
  if (!ok) {
    release_tex_slot(slot);
    return -1;
  }
  if (!in_vram) {
    cache_flush_now(t->tex.data, t->tex.size);
  }
  t->pool = -1;
  t->levels = 1;
  ctr_linear_lock();
  t->used = 1;
  ctr_linear_unlock();
  return slot;
}

/* depth: a D16 buffer (the mipmapped wave texture's draws neither test nor write depth) */
static int rt_create(int lw, int lh, int max_tile_w, int max_level, int depth_on) {
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
  const size_t color_bytes = C3D_TexCalcTotalSize((size_t)lw * (size_t)lh * 4u, max_level);
  const size_t depth_bytes = depth_on ? (size_t)max_tile_w * (size_t)lh * 2u : 0;
  ctr_linear_lock();
  /* the texture: lh wide (buffer columns), lw high (buffer rows). VRAM first (leaving the reserve:
   * C3D_TexInitWithParams calls libctru's vramAlloc, not safe_vram_alloc) */
  int in_vram = (size_t)vramSpaceFree() >= color_bytes + depth_bytes + VRAM_RESERVE &&
                C3D_TexInitWithParams(&t->tex, NULL,
                                      (C3D_TexInitParams){(u16)lh, (u16)lw, (u8)max_level,
                                                          GPU_RGBA8, GPU_TEX_2D, true});
  int ok = in_vram ||
           (linear_room_for_tex(lh, lw, max_level > 0) &&
            C3D_TexInitWithParams(&t->tex, NULL,
                                  (C3D_TexInitParams){(u16)lh, (u16)lw, (u8)max_level, GPU_RGBA8,
                                                      GPU_TEX_2D, false}));
  void* depth = NULL;
  int depth_vram = 0;
  if (ok && in_vram && depth_on) {
    /* the other bank than the color (the GPU reads both per pixel) */
    if ((size_t)vramSpaceFree() >= depth_bytes + VRAM_RESERVE) {
      depth = vramAllocAt(depth_bytes, vram_bank(t->tex.data) ? VRAM_ALLOC_A : VRAM_ALLOC_B);
    }
    if (!depth && vram_room(depth_bytes)) {
      depth = vramAlloc(depth_bytes);
    }
    depth_vram = depth != NULL;
  }
  if (ok && !depth && depth_on) {
    depth = linearAlloc(depth_bytes);
  }
  if (ok && !depth && depth_on) {
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
    cache_flush_now(t->tex.data, (u32)color_bytes);
  }
  if (depth && !depth_vram) {
    cache_flush_now(depth, (u32)depth_bytes);
  }
  t->pool = -1;
  t->levels = (uint8_t)(max_level + 1);
  ctr_linear_lock();
  t->used = 1;
  ctr_linear_unlock();
  s_rt[i].tex1 = slot + 1;
  s_rt[i].levels = max_level + 1;
  for (int l = 0; l < RT_MIP_MAX; l++) {
    s_rt[i].mip[l] = -1;
  }
  s_rt[i].lw = lw;
  s_rt[i].lh = lh;
  s_rt[i].max_w = max_tile_w;
  s_rt[i].depth = depth;
  s_rt[i].depth_vram = depth_vram;
  char dtxt[48] = "no depth";
  if (depth) {
    snprintf(dtxt, sizeof(dtxt), "D16 %dx%d in %s", max_tile_w, lh,
             depth_vram ? (vram_bank(depth) ? "VRAM B" : "VRAM A") : "linear memory");
  }
  printf("[ctr] render to texture: %dx%d RGBA8 (%d levels) in %s + %s (%u KB)\n", lw, lh,
         max_level + 1, in_vram ? (vram_bank(t->tex.data) ? "VRAM B" : "VRAM A") : "linear memory",
         dtxt, (unsigned)((color_bytes + depth_bytes) / 1024u));
  return slot;
}

int ctr_gpu_rt_create(int lw, int lh, int max_tile_w) {
  return rt_create(lw, lh, max_tile_w, 0, 1);
}

static int rt_find(int tex) {
  for (int i = 0; tex >= 0 && i < MAX_RT; i++) {
    if (s_rt[i].tex1 == tex + 1) {
      return i;
    }
  }
  return -1;
}

int ctr_gpu_rt_create_mipmapped(int size) {
  /* (down to 8 x 8, as ctr_gpu_tex_create_mipmapped) */
  const int max_level = C3D_TexCalcMaxLevel((u32)size, (u32)size);
  const int tex = rt_create(size, size, size, max_level, 0);
  const int i = rt_find(tex);
  if (i < 0) {
    return -1;
  }
  int ok = 1;
  for (int l = 1; ok && l <= max_level; l++) {
    ok = (s_rt[i].mip[l - 1] = rt_tex_create(size >> l, size >> l)) >= 0;
  }
  if (!ok) {
    ctr_gpu_rt_delete(tex);
    return -1;
  }
  /* the texels' alpha is 0xff = 1 (the decal copies of the mip levels keep it) */
  g.textures[tex].alpha_full = 1;
  for (int l = 1; l <= max_level; l++) {
    g.textures[s_rt[i].mip[l - 1]].alpha_full = 1;
  }
  return tex;
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
  for (int l = 0; l < RT_MIP_MAX; l++) {
    ctr_gpu_tex_delete(s_rt[i].mip[l]);
    s_rt[i].mip[l] = -1;
  }
  s_rt[i].tex1 = 0;
  s_rt[i].dead_frame = (u32)g.frame_no;
}

static void rt_target(int i, void* color, int fb_w, int fb_h, int vx, int vw, int vh);

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
  rt_target(i, (u8*)g.textures[tex].tex.data + (size_t)x * (size_t)s_rt[i].lh * 4u, s_rt[i].lh,
            rows, y, h, w);
  return 1;
}

/* ctr_gpu_rt_begin's switch to the render texture: color at `color` (fb_w
 * buffer columns, fb_h rows), the viewport and scissor (vx, 0, vw, vh) */
static void rt_target(int i, void* color, int fb_w, int fb_h, int vx, int vw, int vh) {
  C3D_FrameBuf* fb = &s_rtc.fb;
  memset(fb, 0, sizeof(*fb));
  C3D_FrameBufAttrib(fb, (u16)fb_w, (u16)fb_h, false);
  C3D_FrameBufColor(fb, color, GPU_RB_RGBA8);
  C3D_FrameBufDepth(fb, s_rt[i].depth, GPU_RB_DEPTH16);
  C3D_SetFrameBuf(fb);
  C3D_SetViewport((u32)vx, 0, (u32)vw, (u32)vh);
  C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)vx, 0, (u32)(vx + vw), (u32)vh);
  s_rtc.active = 1;
}

void ctr_gpu_rt_end(void) {
  if (!s_rtc.active) {
    return;
  }
  C3D_SetFrameBuf(&g.top->frameBuf);
  set_top_viewport();
  C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
  /* the texture cache may hold the tile's old texels: the next bind clears it */
  g.bound_tex = NULL;
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

/* ctr_gpu_rt_draw_grid's indices: n x n quads of a row-major (n + 1)^2 grid
 * (made once, in linear memory; the GPU reads them in every frame's lists) */
static uint16_t* s_grid_idx;
static int s_grid_n;

int ctr_gpu_rt_draw_grid(int tex, const ctr_draw_state* state, const ctr_vertex* verts, int n) {
  const int i = rt_find(tex);
  if (i < 0 || !g.ready || !g.in_frame || g.textures[tex].used != 1 || n < 1 || n > 64 ||
      s_rt[i].lw != s_rt[i].lh || s_rt[i].max_w != s_rt[i].lw) {
    return 0;
  }
  const int levels = s_rt[i].levels;
  if (levels < 2 || s_rt[i].mip[levels - 2] < 0) {
    return 0;
  }
  const int nv = (n + 1) * (n + 1), ni = n * n * 6;
  if (s_grid_n != n) {
    ctr_linear_lock();
    uint16_t* idx = (uint16_t*)linearAlloc((size_t)ni * 2);
    ctr_linear_unlock();
    if (!idx) {
      return 0;
    }
    uint16_t* o = idx;
    for (int r = 0; r < n; r++) {
      for (int c = 0; c < n; c++) {
        const uint16_t a = (uint16_t)(r * (n + 1) + c), b = (uint16_t)(a + 1),
                       d = (uint16_t)(a + n + 1), e = (uint16_t)(d + 1);
        *o++ = a, *o++ = b, *o++ = d, *o++ = d, *o++ = b, *o++ = e;
      }
    }
    cache_flush_now(idx, (u32)ni * 2);
    /* (an earlier grid's indices stay: a list in flight may read them; n never changes) */
    s_grid_idx = idx;
    s_grid_n = n;
  }
  const size_t bytes = (size_t)nv * sizeof(ctr_vertex);
  const size_t grid_bytes = (bytes + 15) & ~(size_t)15;
  const size_t quad_bytes = 6 * sizeof(ctr_vertex);
  if (!vbuf_room(grid_bytes + quad_bytes) || !cmd_room_for((u32)levels * CMD_DRAW_WORDS)) {
    return 0;
  }
  uint8_t* dst = g.vbuf + g.vbuf_used;
  memcpy(dst, verts, bytes);
  /* the mip levels' quad: the source's texel corners at the target's pixel centers (2 x 2
   * averages); s, t of ctr_gpu_rt_uv: s = logical y, t = 1 - logical x */
  ctr_vertex* quad = (ctr_vertex*)(dst + grid_bytes);
  static const float kQuad[6][2] = {{-1, -1}, {1, -1}, {-1, 1}, {-1, 1}, {1, -1}, {1, 1}};
  for (int k = 0; k < 6; k++) {
    ctr_vertex* v = &quad[k];
    v->x = kQuad[k][0];
    v->y = kQuad[k][1];
    v->z = 0.5f;
    v->s = 0.5f * (v->y + 1.0f);
    v->t = 0.5f * (1.0f - v->x);
    v->r = v->g = v->b = v->a = 0x80;
  }
  g.vbuf_used += grid_bytes + quad_bytes;

  const int dim = s_rt[i].lw;
  rt_target(i, g.textures[tex].tex.data, dim, dim, 0, dim, dim);
  use_program(PROG_BASIC);
  if (use_raw(state)) {
    apply_state(state, 0, 1);
    raw_vertex_buffer(raw_vbuf_pa((size_t)(dst - g.vbuf)), 0x210,
                      RAW_BUF_FLAGS1(sizeof(ctr_vertex), 3, 0));
    raw_sync();
    raw_draw_elements(osConvertVirtToPhys(s_grid_idx), (u32)ni);
  } else {
    apply_state(state, 0, 0);
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, dst, sizeof(ctr_vertex), 3, 0x210);
    C3D_DrawElements(GPU_TRIANGLES, ni, C3D_UNSIGNED_SHORT, s_grid_idx);
  }
  g.cur.draws++;
  g.cur.triangles += n * n * 2;

  /* the mip levels: 2 x 2 averages (the PS2's bilinear half size sprites) */
  ctr_draw_state q;
  memset(&q, 0, sizeof(q));
  q.tcc = 1;
  q.decal = 1;
  q.filter = 1;
  q.clamp_s = q.clamp_t = 1;
  q.blend = CTR_BLEND_OFF;
  q.atest = CTR_TEST_ALWAYS;
  q.ztest = CTR_TEST_ALWAYS;
  for (int l = 1; l < levels; l++) {
    const int d = dim >> l;
    q.tex = l == 1 ? tex : s_rt[i].mip[l - 2];
    rt_target(i, g.textures[s_rt[i].mip[l - 1]].tex.data, d, d, 0, d, d);
    g.textures[tex].levels = 1; /* (level 1 samples level 0 alone) */
    draw_immediate(&q, PROG_BASIC, (uint8_t*)quad, sizeof(ctr_vertex), 6, GPU_TRIANGLES);
    g.textures[tex].levels = (uint8_t)levels;
  }
  ctr_gpu_rt_end();

  /* after the draws, in the GPU's queue: the levels into place */
  const int bd = s_pipeline && s_bd.frame;
  u8* level = (u8*)g.textures[tex].tex.data + (size_t)dim * (size_t)dim * 4u;
  for (int l = 1; l < levels; l++) {
    const int d = dim >> l;
    const u32 size = (u32)d * (u32)d * 4u;
    u32* src = (u32*)g.textures[s_rt[i].mip[l - 1]].tex.data;
    if (l == 1 || bd) {
      if (!sync_texture_copy(src, 0, (u32*)level, 0, size, 8)) {
        return 0;
      }
    } else if (gx_begin()) {
      GX_TextureCopy(src, 0, (u32*)level, 0, size, 8);
      gx_end();
    } else {
      return 0;
    }
    level += size;
  }
  return 1;
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
  return ctr_gpu_mesh_begin_tail(vertex_count, 0, index_count, verts, indices);
}

int ctr_gpu_mesh_begin_tail(int vertex_count, int tail_bytes, int index_count, void** verts,
                            uint16_t** indices) {
  if (!g.ready || tail_bytes < 0) {
    return -1;
  }
  const int slot = reserve_mesh_slot();
  if (slot < 0) {
    return -1;
  }
  size_t vbytes = (size_t)vertex_count * 16;
  size_t ibytes = (size_t)index_count * 2;
  ctr_linear_lock();
  void* v = linearAlloc(vbytes + (size_t)tail_bytes);
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
  g.meshes[slot].lin_bytes = linearGetSize(v) + linearGetSize(ix);
  ctr_linear_unlock();
  g.meshes[slot].verts = v;
  g.meshes[slot].ext = 0; /* (AI-assisted, streaming) */
  g.meshes[slot].indices = ix;
  g.meshes[slot].fill_vbytes = (u32)vbytes;
  g.meshes[slot].fill_ibytes = (u32)ibytes;
  g.meshes[slot].tail_off = (u32)vbytes;
  g.meshes[slot].tail_bytes = (u32)tail_bytes;
  *verts = v;
  *indices = ix;
  return slot;
}

/* (AI-assisted, streaming) see ctr_gpu.h */
void ctr_gpu_set_ext_free(void (*fn)(void* mem)) {
  s_ext_free = fn;
}

static u32 s_block_bytes; /* ctr_gpu_linear_block's, as the linear heap counts them */

void* ctr_gpu_linear_block(unsigned int bytes) {
  ctr_linear_lock();
  void* p = linearAlloc(bytes);
  if (p) {
    s_block_bytes += linearGetSize(p);
  }
  ctr_linear_unlock();
  return p;
}

void ctr_gpu_linear_block_free(void* p) {
  if (!p) {
    return;
  }
  ctr_linear_lock();
  s_block_bytes -= linearGetSize(p);
  linearFree(p);
  ctr_linear_unlock();
}

unsigned int ctr_gpu_linear_free(void) {
  /* (AI-assisted, streammem) under the linear lock (recursive): libctru walks its free list, whose
   * nodes another thread's allocation or free unlinks and free()s (render thread every frame, the
   * loader during loads) */
  ctr_linear_lock();
  const unsigned int n = (unsigned int)linearSpaceFree();
  ctr_linear_unlock();
  return n;
}

unsigned int ctr_gpu_linear_largest(void) {
  return linear_largest_block();
}

int ctr_gpu_mesh_begin_ext(void* mem, int vertex_count, int index_count, void** verts,
                           uint16_t** indices) {
  return ctr_gpu_mesh_begin_ext_tail(mem, vertex_count, index_count, 0, 0, verts, indices, NULL);
}

int ctr_gpu_mesh_begin_ext_tail(void* mem, int vertex_count, int index_count, unsigned int tail_off,
                                unsigned int tail_bytes, void** verts, uint16_t** indices,
                                void** tail) {
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
  g.meshes[slot].tail_off = tail_off;
  g.meshes[slot].tail_bytes = tail_bytes;
  *verts = mem;
  *indices = g.meshes[slot].indices;
  if (tail) {
    *tail = (uint8_t*)mem + tail_off;
  }
  return slot;
}

void ctr_gpu_mesh_split_colors(int mesh) {
  if (mesh >= 0 && mesh < MAX_MESHES && g.meshes[mesh].used == 3 && g.meshes[mesh].fill_vbytes) {
    g.meshes[mesh].col_off = ctr_gpu_mesh_color_offset((int)(g.meshes[mesh].fill_vbytes / 16u));
  }
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
  if (g.meshes[mesh].tail_bytes) {
    cache_flush_now((uint8_t*)g.meshes[mesh].verts + g.meshes[mesh].tail_off,
                    g.meshes[mesh].tail_bytes);
  }
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
 * 1/32768), sent by citro3d with the draw's sync */
static void mesh_scales_set(int atlas, int raw) {
  (void)raw;
  const float sx = atlas ? 1.0f / 32768.0f : 1.0f / 1024.0f;
  C3D_FVUnifSet(GPU_VERTEX_SHADER, g.ps->uloc_scales, sx, 1.0f / 255.0f, 1.0f, 1.0f / 127.0f);
  raw_note_unif();
}

void ctr_gpu_draw_mesh_prepared(const ctr_draw_state* state, const ctr_mesh_matrix* matrix,
                                int mesh, int first_index, int index_count) {
  if (!g.ready || !g.in_frame || index_count < 3) {
    return;
  }
  if (mesh < 0 || mesh >= MAX_MESHES || g.meshes[mesh].used != 1) {
    g.cur.dead_draws++;
    return;
  }
  if (!cmd_room()) {
    return;
  }
  /* (AI-assisted) a texture with an alpha mask (c3l v23; vertex alpha 0x80 everywhere,
   * CTR_TCC_ALPHA80): the mesh_fa programs, with the fog in the color's alpha (mesh 5) and the
   * mask on unit 1 */
  const int fa = g.ps->fa_ok && (state->tcc & CTR_TCC_ALPHA80) &&
                 tex_mask(tex_res(state->tex)) >= 0;
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
  const int mesh_mode = fa ? 5 : 1;
  g.cur.fog_alpha_draws += fa;
  if (use_raw(state)) {
    /* (AI-assisted) raw: state, vertex buffer, then (after citro3d's pending updates) the matrix
     * as float24 and the draw */
    apply_state(state, mesh_mode, 1);
    raw_mesh_buffers(ms);
    raw_sync();
    if (!g.last_mesh_valid || g.last_clip_id != matrix->id) {
      g.last_clip_id = matrix->id;
      {
        /* (AI-assisted) the matrix and its fog rows in one burst (the draw distance
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
        raw_end(raw_unif(raw_ptr(), g.ps->uloc_clip, matrix->f24, df ? 15 : 18, 0));
      }
      g.last_mesh_valid = 1;
    }
    raw_draw_elements(ms->indices_pa + 2u * (u32)first_index, (u32)index_count);
  } else {
    /* consecutive draws of a level chunk share the matrix and the vertex buffer */
    if (!g.last_mesh_valid || g.last_clip_id != matrix->id) {
      g.last_clip_id = matrix->id;
      C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g.ps->uloc_clip, (const C3D_Mtx*)matrix->m);
      float r[8];
      mesh_fog_rows(matrix->m, r);
      C3D_FVUnifSet(GPU_VERTEX_SHADER, g.ps->uloc_clip + 4, r[3], r[2], r[1], r[0]);
      C3D_FVUnifSet(GPU_VERTEX_SHADER, g.ps->uloc_clip + 5, r[7], r[6], r[5], r[4]);
    }
    g.last_mesh_valid = 1;
    c3d_mesh_buffers(ms, NULL, 0);
    apply_state(state, mesh_mode, 0);
    C3D_DrawElements(GPU_TRIANGLES, index_count, C3D_UNSIGNED_SHORT, ms->indices + first_index);
  }
  g.cur.draws++;
  g.cur.triangles += index_count / 3;
}

void ctr_gpu_draw_mesh(const ctr_draw_state* state, const float clip[16], int mesh,
                       int first_index, int index_count) {
  ctr_mesh_matrix m;
  ctr_gpu_prepare_mesh_matrix(clip, &m);
  ctr_gpu_draw_mesh_prepared(state, &m, mesh, first_index, index_count);
}

/* (AI-assisted, gpu-rt) a pass's level draws in one list (the level renderer's), one by one */
void ctr_gpu_draw_mesh_calls(const ctr_mesh_call* calls, int n) {
  if (!g.ready || !g.in_frame || n <= 0) {
    return;
  }
  for (int i = 0; i < n; i++) {
    const ctr_mesh_call* c = &calls[i];
    if (c->morph_rows) {
      ctr_gpu_draw_mesh_morph(c->state, c->matrix, c->morph_rows, c->morph_list, c->morph_list_n,
                              c->morph_id, c->mesh, c->morph_offset, c->first_index,
                              c->index_count);
    } else if (c->tmorph) {
      ctr_gpu_draw_mesh_tmorph(c->state, c->matrix, c->tmorph, c->morph_id, c->mesh,
                               c->tmorph_offset, c->first_index, c->index_count);
    } else {
      ctr_gpu_draw_mesh_prepared(c->state, c->matrix, c->mesh, c->first_index, c->index_count);
    }
  }
}

/* Jak 1's wind ties (see ctr_gpu.h): ctr_gpu_draw_mesh_prepared's citro3d path with the wind
 * program, a second attribute buffer (the slots) and the slot rows, uploaded when they change. Its
 * combiner is the level meshes' with the fog ramp (the program has no fog_alpha variant). */
void ctr_gpu_draw_mesh_wind(const ctr_draw_state* state, const ctr_mesh_matrix* matrix,
                            const float* rows, int row_count, uint32_t rows_id, int mesh,
                            int slots_mesh, int slots_offset, int first_index, int index_count) {
  if (!g.ready || !g.in_frame || !g.ps->wind_ok || row_count <= 0 || row_count > 64 ||
      slots_mesh < 0 || slots_mesh >= MAX_MESHES || g.meshes[slots_mesh].used != 1 ||
      mesh < 0 || mesh >= MAX_MESHES || g.meshes[mesh].used != 1) {
    ctr_gpu_draw_mesh_prepared(state, matrix, mesh, first_index, index_count);
    return;
  }
  if (index_count < 3 || !cmd_room()) {
    return;
  }
  use_program(PROG_MESH_WIND);
  {
    const int atlas = (state->tcc & CTR_TCC_ST_ATLAS) ? 1 : 0;
    if (atlas != g.mesh_st_atlas) {
      g.mesh_st_atlas = atlas;
      mesh_scales_set(atlas, 0);
    }
  }
  const MeshSlot* ms = &g.meshes[mesh];
  const int mesh_mode = 1;
  if (!g.last_mesh_valid || g.last_clip_id != matrix->id) {
    g.last_clip_id = matrix->id;
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g.ps->uloc_clip, (const C3D_Mtx*)matrix->m);
    float r[8];
    mesh_fog_rows(matrix->m, r);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g.ps->uloc_clip + 4, r[3], r[2], r[1], r[0]);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g.ps->uloc_clip + 5, r[7], r[6], r[5], r[4]);
  }
  g.last_mesh_valid = 1;
  if (g.wind_rows_id != rows_id || !rows_id) {
    g.wind_rows_id = rows_id;
    for (int i = 0; i < row_count; i++) {
      const float* w = rows + 4 * i;
      C3D_FVUnifSet(GPU_VERTEX_SHADER, g.ps->uloc_wind + i, w[0], w[1], w[2], w[3]);
    }
  }
  raw_note_unif();
  c3d_mesh_buffers(ms, (const u8*)g.meshes[slots_mesh].verts + slots_offset, 1);
  apply_state(state, mesh_mode, 0);
  C3D_DrawElements(GPU_TRIANGLES, index_count, C3D_UNSIGNED_SHORT, ms->indices + first_index);
  g.cur.draws++;
  g.cur.triangles += index_count / 3;
}

int ctr_gpu_wind_ready(void) {
  return g.ready && s_ps.wind_ok;
}

/* the PS2's tie geomorph (see ctr_gpu.h): ctr_gpu_draw_mesh_prepared's citro3d path with the morph
 * program, a second attribute buffer (the mesh's MorphVertex records) and the rows, uploaded when
 * they change. Without the program: at rest. */
void ctr_gpu_draw_mesh_morph(const ctr_draw_state* state, const ctr_mesh_matrix* matrix,
                             const float* rows, const uint8_t* list, int list_n, uint32_t rows_id,
                             int mesh, int morph_offset, int first_index, int index_count) {
  if (!g.ready || !g.in_frame || !g.ps->morph_ok || !rows || list_n < 0 || !rows_id ||
      morph_offset < 0 || (morph_offset & 15) || mesh < 0 || mesh >= MAX_MESHES ||
      g.meshes[mesh].used != 1) {
    ctr_gpu_draw_mesh_prepared(state, matrix, mesh, first_index, index_count);
    return;
  }
  if (index_count < 3 || !cmd_room()) {
    return;
  }
  const int fa = g.ps->fa_ok && (state->tcc & CTR_TCC_ALPHA80) &&
                 tex_mask(tex_res(state->tex)) >= 0;
  use_program(fa ? PROG_MESH_MORPH_FA : PROG_MESH_MORPH);
  {
    const int atlas = (state->tcc & CTR_TCC_ST_ATLAS) ? 1 : 0;
    if (atlas != g.mesh_st_atlas) {
      g.mesh_st_atlas = atlas;
      mesh_scales_set(atlas, use_raw(state));
    }
  }
  const MeshSlot* ms = &g.meshes[mesh];
  const int mesh_mode = fa ? 5 : 1;
  g.cur.fog_alpha_draws += fa;
  if (use_raw(state)) {
    /* as ctr_gpu_draw_mesh_prepared's raw path, with the records' buffer and the rows the draw
     * uses (sent after raw_sync: a program switch makes citro3d send its own copies first) */
    apply_state(state, mesh_mode, 1);
    const u32 rec_pa = ms->verts_pa + (u32)morph_offset;
    if (!ms->col_off) {
      const u32 w[2][3] = {{ms->verts_pa, 0x210, RAW_BUF_FLAGS1(16, 3, 0)},
                           {rec_pa, 0x43, RAW_BUF_FLAGS1(8, 2, 0)}};
      raw_vertex_buffers_n(2, w);
    } else {
      const u32 w[3][3] = {{ms->verts_pa, 0x10, RAW_BUF_FLAGS1(12, 2, 0)},
                           {ms->verts_pa + ms->col_off, 0x2, RAW_BUF_FLAGS1(4, 1, 0)},
                           {rec_pa, 0x43, RAW_BUF_FLAGS1(8, 2, 0)}};
      raw_vertex_buffers_n(3, w);
    }
    raw_sync();
    if (!g.last_mesh_valid || g.last_clip_id != matrix->id) {
      g.last_clip_id = matrix->id;
      ctr_mesh_matrix* mm = (ctr_mesh_matrix*)matrix;
      if (mm->fog_gen != g.fog_gen) {
        float fr[8];
        mesh_fog_rows(matrix->m, fr);
        raw_pack_f24(fr, mm->f24 + 12);
        raw_pack_f24(fr + 4, mm->f24 + 15);
        mm->fog_gen = g.fog_gen;
      }
      raw_end(raw_unif(raw_ptr(), g.ps->uloc_clip, matrix->f24, 18, 0));
      g.last_mesh_valid = 1;
    }
    u32* p = raw_ptr();
    if (!g.morph_slot_id[0]) {
      g.morph_slot_id[0] = 1;
      const u32 z[4] = {0, 0, 0, 0};
      p = raw_unif(p, g.ps->uloc_morph, z, 4, 1);
    }
    for (int i = 0; i < list_n; i++) {
      const int r = list[i];
      if (r > 0 && r < CTR_MORPH_ROWS && g.morph_slot_id[r] != rows_id) {
        g.morph_slot_id[r] = rows_id;
        u32 v[4] = {0, 0, 0, 0}; /* w z y x */
        memcpy(&v[2], &rows[4 * r + 1], 4);
        memcpy(&v[3], &rows[4 * r], 4);
        p = raw_unif(p, g.ps->uloc_morph + r, v, 4, 1);
      }
    }
    raw_end(p);
    raw_draw_elements(ms->indices_pa + 2u * (u32)first_index, (u32)index_count);
    g.cur.draws++;
    g.cur.triangles += index_count / 3;
    return;
  }
  if (!g.last_mesh_valid || g.last_clip_id != matrix->id) {
    g.last_clip_id = matrix->id;
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g.ps->uloc_clip, (const C3D_Mtx*)matrix->m);
    float r[8];
    mesh_fog_rows(matrix->m, r);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g.ps->uloc_clip + 4, r[3], r[2], r[1], r[0]);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g.ps->uloc_clip + 5, r[7], r[6], r[5], r[4]);
  }
  g.last_mesh_valid = 1;
  if (!g.morph_slot_id[0]) {
    g.morph_slot_id[0] = 1;
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g.ps->uloc_morph, 0.f, 0.f, 0.f, 0.f);
  }
  for (int i = 0; i < list_n; i++) {
    const int r = list[i];
    if (r > 0 && r < CTR_MORPH_ROWS && g.morph_slot_id[r] != rows_id) {
      g.morph_slot_id[r] = rows_id;
      C3D_FVUnifSet(GPU_VERTEX_SHADER, g.ps->uloc_morph + r, rows[4 * r], rows[4 * r + 1], 0.f,
                    0.f);
    }
  }
  raw_note_unif();
  {
    /* each buffer's attributes add up to its stride (j1hang: the real GPU hangs else) */
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    if (ms->col_off) {
      BufInfo_Add(buf, ms->verts, 12, 2, 0x10);
      BufInfo_Add(buf, (const u8*)ms->verts + ms->col_off, 4, 1, 0x2);
    } else {
      BufInfo_Add(buf, ms->verts, 16, 3, 0x210);
    }
    BufInfo_Add(buf, (const u8*)ms->verts + morph_offset, 8, 2, 0x43);
  }
  apply_state(state, mesh_mode, 0);
  C3D_DrawElements(GPU_TRIANGLES, index_count, C3D_UNSIGNED_SHORT, ms->indices + first_index);
  BufInfo_Init(C3D_GetBufInfo());
  g.cur.draws++;
  g.cur.triangles += index_count / 3;
}

int ctr_gpu_morph_ready(void) {
  return g.ready && s_ps.morph_ok;
}

/* the PS2's tfrag geomorph (see ctr_gpu.h): ctr_gpu_draw_mesh_morph's paths with the tfrag morph
 * program, the records' buffer (4 attributes) and its two uniforms, sent when they change. Without
 * the program: at rest. */
void ctr_gpu_draw_mesh_tmorph(const ctr_draw_state* state, const ctr_mesh_matrix* matrix,
                              const float* unif, uint32_t unif_id, int mesh, int tail_offset,
                              int first_index, int index_count) {
  if (!g.ready || !g.in_frame || !g.ps->tmorph_ok || !unif || !unif_id || tail_offset < 0 ||
      (tail_offset & 15) || mesh < 0 || mesh >= MAX_MESHES || g.meshes[mesh].used != 1) {
    ctr_gpu_draw_mesh_prepared(state, matrix, mesh, first_index, index_count);
    return;
  }
  if (index_count < 3 || !cmd_room()) {
    return;
  }
  const int fa = g.ps->fa_ok && (state->tcc & CTR_TCC_ALPHA80) &&
                 tex_mask(tex_res(state->tex)) >= 0;
  use_program(fa ? PROG_TFRAG_MORPH_FA : PROG_TFRAG_MORPH);
  {
    const int atlas = (state->tcc & CTR_TCC_ST_ATLAS) ? 1 : 0;
    if (atlas != g.mesh_st_atlas) {
      g.mesh_st_atlas = atlas;
      mesh_scales_set(atlas, use_raw(state));
    }
  }
  const MeshSlot* ms = &g.meshes[mesh];
  const int mesh_mode = fa ? 5 : 1;
  g.cur.fog_alpha_draws += fa;
  if (use_raw(state)) {
    apply_state(state, mesh_mode, 1);
    const u32 rec_pa = ms->verts_pa + (u32)tail_offset;
    if (!ms->col_off) {
      const u32 w[2][3] = {{ms->verts_pa, 0x210, RAW_BUF_FLAGS1(16, 3, 0)},
                           {rec_pa, 0x6543, RAW_BUF_FLAGS1(16, 4, 0)}};
      raw_vertex_buffers_n(2, w);
    } else {
      const u32 w[3][3] = {{ms->verts_pa, 0x10, RAW_BUF_FLAGS1(12, 2, 0)},
                           {ms->verts_pa + ms->col_off, 0x2, RAW_BUF_FLAGS1(4, 1, 0)},
                           {rec_pa, 0x6543, RAW_BUF_FLAGS1(16, 4, 0)}};
      raw_vertex_buffers_n(3, w);
    }
    raw_sync();
    if (!g.last_mesh_valid || g.last_clip_id != matrix->id) {
      g.last_clip_id = matrix->id;
      ctr_mesh_matrix* mm = (ctr_mesh_matrix*)matrix;
      if (mm->fog_gen != g.fog_gen) {
        float fr[8];
        mesh_fog_rows(matrix->m, fr);
        raw_pack_f24(fr, mm->f24 + 12);
        raw_pack_f24(fr + 4, mm->f24 + 15);
        mm->fog_gen = g.fog_gen;
      }
      raw_end(raw_unif(raw_ptr(), g.ps->uloc_clip, matrix->f24, 18, 0));
      g.last_mesh_valid = 1;
    }
    if (g.tmorph_unif_id != unif_id) {
      g.tmorph_unif_id = unif_id;
      u32 v[8]; /* w z y x, per vector */
      for (int k = 0; k < 8; k++) {
        memcpy(&v[k], &unif[(k & ~3) + 3 - (k & 3)], 4);
      }
      raw_end(raw_unif(raw_ptr(), g.ps->uloc_tband, v, 8, 1));
    }
    raw_draw_elements(ms->indices_pa + 2u * (u32)first_index, (u32)index_count);
    g.cur.draws++;
    g.cur.triangles += index_count / 3;
    return;
  }
  if (!g.last_mesh_valid || g.last_clip_id != matrix->id) {
    g.last_clip_id = matrix->id;
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g.ps->uloc_clip, (const C3D_Mtx*)matrix->m);
    float r[8];
    mesh_fog_rows(matrix->m, r);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g.ps->uloc_clip + 4, r[3], r[2], r[1], r[0]);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g.ps->uloc_clip + 5, r[7], r[6], r[5], r[4]);
  }
  g.last_mesh_valid = 1;
  if (g.tmorph_unif_id != unif_id) {
    g.tmorph_unif_id = unif_id;
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g.ps->uloc_tband, unif[0], unif[1], unif[2], unif[3]);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g.ps->uloc_tband + 1, unif[4], unif[5], unif[6], unif[7]);
  }
  raw_note_unif();
  {
    /* each buffer's attributes add up to its stride (j1hang: the real GPU hangs else) */
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    if (ms->col_off) {
      BufInfo_Add(buf, ms->verts, 12, 2, 0x10);
      BufInfo_Add(buf, (const u8*)ms->verts + ms->col_off, 4, 1, 0x2);
    } else {
      BufInfo_Add(buf, ms->verts, 16, 3, 0x210);
    }
    BufInfo_Add(buf, (const u8*)ms->verts + tail_offset, 16, 4, 0x6543);
  }
  apply_state(state, mesh_mode, 0);
  C3D_DrawElements(GPU_TRIANGLES, index_count, C3D_UNSIGNED_SHORT, ms->indices + first_index);
  BufInfo_Init(C3D_GetBufInfo());
  g.cur.draws++;
  g.cur.triangles += index_count / 3;
}

/* (AI-assisted) the cache flush statistics line since the last call */
int ctr_gpu_micro_line(char* out, int size) {
  static u32 frame0;
  const u32 frames = (u32)g.frame_no - frame0;
  if (!frames || size <= 0) {
    return 0;
  }
  const double f = frames;
  /* all threads' flushes (textures made by the loader thread too) */
  const int l = snprintf(out, size,
                         "[ctr] gpu flushes: cache flushes %.1f per frame by the kernel, %.1f GSP "
                         "requests (%.1f ranges at submits of %.1f deferred)",
                         s_flush_svcs / f, s_flush_requests / f, s_defer_flushed / f,
                         s_defer_added / f);
  s_flush_requests = 0;
  s_flush_svcs = 0;
  s_defer_added = 0;
  s_defer_flushed = 0;
  frame0 = (u32)g.frame_no;
  return l < size ? l : size - 1;
}

int ctr_gpu_has_stencil(void) {
  return 1;
}

/* texture unit 2 (citro3d sends it at the next update), bound again only when it or its memory
 * (pipeline copies) changed */
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
  s_tex2_mask = NULL;
  g.cur.tex_binds++;
}
/* (AI-assisted, j2water2) Jak II's envmapped ties (see ctr_gpu.h). Through citro3d (their draws
 * are a few per frame): the uniforms with every draw, two attribute buffers (the mesh's vertices,
 * the normals). The envmap on texture unit 2, clamped and linear (process_envmap_draw_mode). */
static const void* s_etie_tex2;
void ctr_gpu_etie_draw(const ctr_draw_state* state, int env_tex, const ctr_etie_draw* d, int mesh,
                       int normals_mesh, int normals_offset, int first_index, int index_count) {
  if (!g.ready || !g.in_frame || index_count <= 0 || !g.ps->etie.vertexShader ||
      normals_mesh < 0 || normals_mesh >= MAX_MESHES || g.meshes[normals_mesh].used != 1 ||
      mesh < 0 || mesh >= MAX_MESHES || g.meshes[mesh].used != 1 || env_tex < 0 ||
      env_tex >= MAX_TEXTURES || g.textures[env_tex].used != 1) {
    return;
  }
  const MeshSlot* ms = &g.meshes[mesh];
  if (!cmd_room()) {
    return;
  }
  /* c3l v23: a texture with an alpha mask (on unit 1; the vertex alpha is 0x80) */
  use_program(g.ps->etie_fa_ok && tex_mask(tex_res(state->tex)) >= 0 ? PROG_ETIE_FA : PROG_ETIE);
  {
    TexSlot* slot = &g.textures[env_tex];
    C3D_Tex* tex = &slot->tex;
    if (s_etie_tex2 != tex || !g.ocean_tex2_bound || s_ocean_tex2 != NULL) {
      u32 param = tex->param & ~(GPU_TEXTURE_MAG_FILTER(GPU_LINEAR) |
                                 GPU_TEXTURE_MIN_FILTER(GPU_LINEAR) | GPU_TEXTURE_WRAP_S(3) |
                                 GPU_TEXTURE_WRAP_T(3) | GPU_TEXTURE_MIP_FILTER(GPU_LINEAR));
      param |= GPU_TEXTURE_MAG_FILTER(GPU_LINEAR) | GPU_TEXTURE_MIN_FILTER(GPU_LINEAR) |
               GPU_TEXTURE_WRAP_S(GPU_CLAMP_TO_EDGE) | GPU_TEXTURE_WRAP_T(GPU_CLAMP_TO_EDGE);
      tex->param = param;
      tex->maxLevel = 0;
      C3D_TexBind(2, tex);
      s_etie_tex2 = tex;
      s_ocean_tex2 = NULL; /* the ocean binds its own again */
      g.ocean_tex2_bound = 1;
      s_tex2_mask = NULL;
      g.cur.tex_binds++;
    }
  }
  const ProgSet* p = g.ps;
  /* a chunk's draws share the matrix, camera and normal rows (prepared once per frame: its id) */
  if (!g.etie_valid || g.etie_clip_id != d->clip.id || g.etie_fog_gen != g.fog_gen ||
      g.etie_st_scale != d->st_scale) {
    g.etie_valid = 1;
    g.etie_clip_id = d->clip.id;
    g.etie_fog_gen = g.fog_gen;
    g.etie_st_scale = d->st_scale;
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, p->uloc_etie_clip, (const C3D_Mtx*)d->clip.m);
    for (int i = 0; i < 3; i++) {
      const float* r = d->cam[i];
      C3D_FVUnifSet(GPU_VERTEX_SHADER, p->uloc_etie_cam + i, r[0], r[1], r[2], r[3]);
      r = d->nrm[i];
      C3D_FVUnifSet(GPU_VERTEX_SHADER, p->uloc_etie_nrm + i, r[0], r[1], r[2], r[3]);
    }
    C3D_FVUnifSet(GPU_VERTEX_SHADER, p->uloc_etie_scales, d->st_scale, 1.0f / 255.0f, 1.0f, 0.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, p->uloc_etie_fog0, g.fog0[0], g.fog0[1], g.fog0[2], g.fog0[3]);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, p->uloc_etie_fog1, g.fog1[0], g.fog1[1], g.fog1[2], g.fog1[3]);
    raw_note_unif();
  }
  c3d_mesh_buffers(ms, (const u8*)g.meshes[normals_mesh].verts + normals_offset, 4);
  apply_state_tint(state, 7, d->shine, 0);
  C3D_DrawElements(GPU_TRIANGLES, index_count, C3D_UNSIGNED_SHORT, ms->indices + first_index);
  g.cur.draws++;
  g.cur.triangles += index_count / 3;
}

int ctr_gpu_etie_ready(void) {
  return g.ready && g.ps->etie.vertexShader != NULL;
}

void ctr_gpu_etie_end(void) {
  if (!g.ready || !g.in_frame) {
    return;
  }
  C3Di_Context* ctx = &__C3D_Context;
  if (g.ocean_tex2_bound) {
    ctx->tex[2] = NULL;
    ctx->flags |= C3DiF_Tex(2);
    g.ocean_tex2_bound = 0;
    s_tex2_mask = NULL;
    s_ocean_tex2 = NULL;
  }
  s_etie_tex2 = NULL;
  C3D_TexEnv e;
  C3D_TexEnvInit(&e);
  commit_texenv(2, &e, s_raw.on);
  commit_texenv(3, &e, s_raw.on);
  g.ocean_stage2 = 0;
  g.etie_stage3 = 0;
  /* the next draws' single attribute buffer (raw_vertex_buffer / c3d_vertex_buffer: bufCount) */
  BufInfo_Init(C3D_GetBufInfo());
  g.last_state_valid = 0;
}

void ctr_gpu_ocean_end(void) {
  if (!g.ready || !g.in_frame) {
    return;
  }
  ocean_fade_unbind();
  C3Di_Context* ctx = &__C3D_Context;
  if (g.ocean_tex2_bound) {
    /* unit 2 off (C3D_TexBind can't take NULL for units 1-2) */
    ctx->tex[2] = NULL;
    ctx->flags |= C3DiF_Tex(2);
    g.ocean_tex2_bound = 0;
    s_tex2_mask = NULL;
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
  g.meshes[slot].lin_bytes = linearGetSize(v) + linearGetSize(ix);
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
        cache_flush_frame(m->verts_alt, m->vbytes); /* (the caller flushes only what it moves) */
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

void* ctr_gpu_mesh_colors_inplace(int mesh) {
  if (mesh < 0 || mesh >= MAX_MESHES || g.meshes[mesh].used != 1 || !g.meshes[mesh].col_off ||
      g.meshes[mesh].verts_alt) {
    return NULL;
  }
  return (uint8_t*)g.meshes[mesh].verts + g.meshes[mesh].col_off;
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

/* (AI-assisted) ctr_skin_opt / ctr_skin_env_opt.v.pica: the weights go in unscaled
 * (0..255), so the camera space position is 255 times as long: the clip matrix's x, y, z columns
 * (C3D_FVec c[3], c[2], c[1]) / 255 undo it */
static void skin_opt_scale(C3D_Mtx* m) {
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
}

/* (AI-assisted, far-fog) ctr_skin_opt's mfog uniform: [0] the fog amount as a row for the camera
 * space position (merc2.vert: (255 - (hvdf.w - w)) / 255 = w / 255 + 1 - hvdf.w / 255, w = the clip
 * matrix's w row, skin_wrow), [1] x, y: its clamps (fog max, fog min as amounts), [2]
 * (AI-assisted, mercsprite) the haze amount as a row (k w + c, ctr_gpu_set_merc_haze). C3D_FVec
 * order (w z y x), f32. */
static void skin_fog_rows(int raw) {
  C3D_FVec v[3];
  for (int k = 0; k < 4; k++) {
    v[0].c[k] = g.skin_wrow[k] * (1.0f / 255.0f);
    v[2].c[k] = g.skin_wrow[k] * g.merc_haze_k;
  }
  v[0].c[0] += 1.0f - g.merc_fog_hvdf_w * (1.0f / 255.0f);
  v[2].c[0] += g.merc_haze_c;
  v[1].c[0] = 0.0f;
  v[1].c[1] = 0.0f;
  v[1].c[2] = g.merc_fog_hi;
  v[1].c[3] = g.merc_fog_lo;
  if (raw) {
    raw_end(raw_unif(raw_ptr(), g.ps->uloc_skin_mfog, (const u32*)v, 12, 1));
  } else {
    C3D_FVec* dst = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, g.ps->uloc_skin_mfog, 3);
    dst[0] = v[0];
    dst[1] = v[1];
    dst[2] = v[2];
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

void ctr_gpu_set_merc_haze(float k, float c) {
  if (!g.ready) {
    return;
  }
  if (k != g.merc_haze_k || c != g.merc_haze_c) {
    g.merc_haze_k = k;
    g.merc_haze_c = c;
    g.merc_fog_gen = g.merc_fog_gen + 1 ? g.merc_fog_gen + 1 : 1;
  }
}

int ctr_gpu_merc_fog_ready(void) {
  return g.ready && g.ps && g.ps->uloc_skin_mfog >= 0 && g.fog_tex_valid;
}

/* (AI-assisted, gpuopt) The merc draws come sorted by model and draw, then instance
 * (merc_tex_sort: each texture once for all instances), so the clip matrix and the lights change
 * at almost every draw and were converted again each time (gl_to_pica_mul, the 1/255 scaling, the
 * float24 packing of 4 + 7 vectors, the light scaling): ~750 instructions per draw for the same
 * few instances. Their upload words are kept here per instance (the caller's array, checked by its
 * contents) and only copied into the command list; the words are the same. mode: what the words
 * depend on (raw, the opt program set). */
#define SKIN_UCACHE 16
typedef struct {
  const float* key;
  u32 mode;
  float src[16];
  u32 words[16]; /* f32, C3D_FVec order */
  float wrow[4]; /* the w row for skin_fog_rows */
} SkinClipEnt;
typedef struct {
  const float* key;
  u32 mode;
  float src[28];
  u32 words[28]; /* f32, C3D_FVec order */
} SkinLightEnt;
static SkinClipEnt s_skin_cc[SKIN_UCACHE];
static SkinLightEnt s_skin_lc[SKIN_UCACHE];
static u32 s_skin_cc_next, s_skin_lc_next;

static const SkinClipEnt* skin_clip_words(const float clip[16], u32 mode) {
  for (int i = 0; i < SKIN_UCACHE; i++) {
    SkinClipEnt* e = &s_skin_cc[i];
    if (e->key == clip && e->mode == mode && words_equal(e->src, clip, 16)) {
      return e;
    }
  }
  SkinClipEnt* e = &s_skin_cc[s_skin_cc_next++ % SKIN_UCACHE];
  C3D_Mtx m;
  skin_clip_matrix(clip, &m);
  skin_opt_scale(&m);
  memcpy(e->wrow, m.r[3].c, sizeof(e->wrow));
  memcpy(e->words, &m, sizeof(m));
  e->key = clip;
  e->mode = mode;
  memcpy(e->src, clip, sizeof(e->src));
  return e;
}

static const SkinLightEnt* skin_light_words(const float lights_in[28], u32 mode) {
  for (int i = 0; i < SKIN_UCACHE; i++) {
    SkinLightEnt* e = &s_skin_lc[i];
    if (e->key == lights_in && e->mode == mode && words_equal(e->src, lights_in, 28)) {
      return e;
    }
  }
  SkinLightEnt* e = &s_skin_lc[s_skin_lc_next++ % SKIN_UCACHE];
  /* shader_opt: the light colors and ambient times the vertex color scale */
  float lopt[28];
  const float* lights = lights_in;
  if (mode & 4) {
    memcpy(lopt, lights_in, sizeof(lopt));
    for (int i = 12; i < 28; i++) {
      lopt[i] *= (i & 3) == 3 ? 1.0f / 255.0f : 1.0f / 510.0f;
    }
    lights = lopt;
  }
  const u32* l = (const u32*)lights;
  for (int i = 0; i < 7; i++) {
    e->words[4 * i] = l[4 * i + 3];
    e->words[4 * i + 1] = l[4 * i + 2];
    e->words[4 * i + 2] = l[4 * i + 1];
    e->words[4 * i + 3] = l[4 * i];
  }
  e->key = lights_in;
  e->mode = mode;
  memcpy(e->src, lights_in, sizeof(e->src));
  return e;
}

void ctr_gpu_draw_skinned(const ctr_draw_state* state, const float clip[16], const float* bones,
                          int palette_count, const uint32_t* bones_f24, uint32_t bones_id,
                          const float lights[28], int mesh, int first_index, int index_count) {
  (void)bones_f24; /* the rows go as float32 */
  if (!g.ready || !g.in_frame || mesh < 0 || mesh >= MAX_MESHES || g.meshes[mesh].used != 1 ||
      index_count < 3) {
    return;
  }
  /* (AI-assisted) room for the bone rows too (3 x 24 vectors) */
  if (!cmd_room_for(CMD_DRAW_WORDS + 320)) {
    return;
  }
  /* c3l v23: a texture with an alpha mask (on unit 2) */
  use_program(g.ps->mask_ok && tex_mask(tex_res(state->tex)) >= 0 ? PROG_SKIN_MASK : PROG_SKIN);
  const MeshSlot* ms = &g.meshes[mesh];
  const int raw = use_raw(state);
  if (raw) {
    apply_state_tint(state, 2, 0xffffffffu, 1);
    raw_vertex_buffer(ms->verts_pa, 0x543210, RAW_BUF_FLAGS1(24, 6, 0));
    raw_sync();
  }
  /* matrix and lights: uploaded when they change (the draws of a model share them) */
  const int same_clip = g.last_skin_valid && words_equal(g.last_skin_clip, clip, 16);
  /* (AI-assisted) bones and lights: their own registers (skin_bl_valid, while the skin program
   * stays bound) */
  const int same_lights = g.skin_bl_valid && words_equal(g.last_skin_lights, lights, 28);
  if (!same_clip && raw) {
    /* (AI-assisted, gpuopt) the words made once per instance (skin_clip_words) */
    const SkinClipEnt* ce = skin_clip_words(clip, 5u);
    /* (AI-assisted, far-fog) the w row for the fog rows (made again below) */
    memcpy(g.skin_wrow, ce->wrow, sizeof(g.skin_wrow));
    g.skin_mfog_gen = 0;
    raw_end(raw_unif(raw_ptr(), g.ps->uloc_skin_clip, ce->words, 16, 1));
    memcpy(g.last_skin_clip, clip, sizeof(g.last_skin_clip));
  } else if (!same_clip) {
    C3D_Mtx m;
    skin_clip_matrix(clip, &m);
    skin_opt_scale(&m);
    /* (AI-assisted, far-fog) the w row for the fog rows (made again below) */
    memcpy(g.skin_wrow, m.r[3].c, sizeof(g.skin_wrow));
    g.skin_mfog_gen = 0;
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g.ps->uloc_skin_clip, &m);
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
    if (raw && palette_count > 0) {
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
  if (!same_lights && raw) {
    /* (AI-assisted, gpuopt) the words made once per instance (skin_light_words): shader_opt's
     * scaled light colors */
    const SkinLightEnt* le = skin_light_words(lights, 5u);
    raw_end(raw_unif(raw_ptr(), g.ps->uloc_skin_lights, le->words, 28, 1));
    memcpy(g.last_skin_lights, lights, sizeof(g.last_skin_lights));
  } else if (!same_lights) {
    /* (AI-assisted) the light colors and ambient times the vertex color scale */
    float lopt[28];
    const float* lights_in = lights;
    memcpy(lopt, lights, sizeof(lopt));
    for (int i = 12; i < 28; i++) {
      lopt[i] *= (i & 3) == 3 ? 1.0f / 255.0f : 1.0f / 510.0f;
    }
    lights = lopt;
    if (raw) {
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
  (void)bones_f24; /* the rows go as float32 */
  if (!g.ready || !g.in_frame || mesh < 0 || mesh >= MAX_MESHES || g.meshes[mesh].used != 1 ||
      index_count < 3) {
    return;
  }
  /* (AI-assisted) room for the bone rows too (3 x 24 vectors), as ctr_gpu_draw_skinned */
  if (!cmd_room_for(CMD_DRAW_WORDS + 320)) {
    return;
  }
  use_program(PROG_SKIN_ENV);
  if (palette_count > CTR_MAX_PALETTE) {
    palette_count = CTR_MAX_PALETTE;
  }
  /* (AI-assisted) The shine pass tests its depth GEQUAL against its own base pass (no depth write),
   * so its positions must be the same bits as ctr_gpu_draw_skinned's: the same program arithmetic
   * (the skin and skin_env programs compute the position alike) on the same uniform values. The
   * clip matrix and bones therefore go to the GPU exactly as the base pass sends them: f32 words
   * (skin_clip_words), converted by the GPU. They used to go through
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
  }
  if (raw) {
    /* (AI-assisted, gpuopt) the base pass's words for this instance (skin_clip_words): the same
     * bits, made once */
    const SkinClipEnt* ce = skin_clip_words(clip, 5u);
    raw_end(raw_unif(raw_ptr(), g.ps->uloc_env_clip, ce->words, 16, 1));
    memcpy(g.last_env_clip, clip, sizeof(g.last_env_clip));
  } else {
    C3D_Mtx m;
    skin_clip_matrix(clip, &m);
    skin_opt_scale(&m);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g.ps->uloc_env_clip, &m);
    memcpy(g.last_env_clip, clip, sizeof(g.last_env_clip));
  }
  {
    if (raw && palette_count > 0) {
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
  {
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
  /* (AI-assisted, j2cull) the tick count to double as two 32-bit halves (VFP conversions) instead
   * of libgcc's __floatundidf (~3% of the render thread's samples, Oct 25: a call per bucket and
   * phase): exact while below 2^53 ticks (~1000 years), so the same value */
  const u64 t = svcGetSystemTick();
  const double d = (double)(u32)(t >> 32) * 4294967296.0 + (double)(u32)t;
  return d / (double)(SYSCLOCK_ARM11 / 1000);
}

unsigned long long ctr_gpu_ticks(void) {
  return svcGetSystemTick();
}

double ctr_gpu_ticks_to_ms(unsigned long long ticks) {
  return (double)ticks / (double)(SYSCLOCK_ARM11 / 1000);
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

/* render_watchdog: the render thread without a frame for 3 s: where it is and the GX queue */
static void diag_stall_report(int sec) {
  gxCmdQueue_s* q = &__C3D_Context.gxQueue;
  printf("[ctr] render stalled %ds: stage %s, frame %lu, GX queue %u entries, %u sent, %u done, "
         "idle %d; fence %lu\n",
         sec, s_diag_stage, (unsigned long)g.frame_no, q->numEntries, q->curEntry, q->lastEntry,
         (int)gxCmdQueueWait(q, 0),
         s_pres.fence ? (unsigned long)*(volatile u32*)s_pres.fence : 0ul);
  for (int e = q->lastEntry; e < q->numEntries && e < q->lastEntry + 4; e++) {
    const u32* d = q->entries[e].data;
    printf("[ctr]   entry %d (%s): %08lx %08lx %08lx %08lx\n", e,
           e < q->curEntry ? "sent" : "waiting", (unsigned long)d[0], (unsigned long)d[1],
           (unsigned long)d[2], (unsigned long)d[3]);
  }
}
static int s_diag_render_lines;
static void diag_thread_main(void* unused) {
  (void)unused;
  u32 last = 0;
  int still = 0;
  for (int i = 1;; i++) {
    svcSleepThread(1000000000LL);
    ctr_game_watchdog_tick();
    if (!s_diag_render_lines) {
      continue;
    }
    const u32 f = s_diag_frames;
    if (i <= 60) {
      printf("[ctr] render alive %ds: frame %lu, last stage %s%s\n", i, (unsigned long)f,
             s_diag_stage, f == last ? " (no frame in the last second)" : "");
    }
    still = f == last ? still + 1 : 0;
    last = f;
    if (still == 3) {
      diag_stall_report(i);
    }
  }
}

void ctr_gpu_diag_start(int render_lines) {
  s_diag_render_lines = render_lines;
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
  /* (AI-assisted) new fog rows with the next matrix upload, the clamps now */
  g.last_mesh_valid = 0;
  if (is_mesh_prog(g.cur_prog)) {
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g.ps->uloc_fog1, g.fog1[0], g.fog1[1], g.fog1[2], g.fog1[3]);
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
      tex1_bind(&g.fog_tex);
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
    tex1_bind(&g.fog_tex); /* clears the texture cache: the GPU must not use the old texels */
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

/* (AI-assisted) config.ini pica_bench (ctr_gpu_bench.c) used citro3d with programs, textures and
 * state of its own: take it back before the bench frees them (ctr_gpu_bench.h) */
void ctr_gpu_bench_gate(int enter) {
  if (enter) {
    apt_gate_enter();
  } else {
    apt_gate_leave();
  }
}

void ctr_gpu_bench_restore(void) {
  /* units 1 and 2 can't take NULL (citro3d reads its type): the fog ramp, which lives as long as
   * the GPU layer, replaces the bench's textures there */
  C3D_TexBind(0, NULL);
  if (g.fog_tex_valid) {
    C3D_TexBind(1, &g.fog_tex);
    C3D_TexBind(2, &g.fog_tex);
  }
  tex_units_forget();
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
  g.etie_valid = 0;
  g.last_skin_valid = 0;
  g.skin_bl_valid = 0;
  g.bound_tex = NULL;
  C3D_ProcTexBind(0, NULL); /* (the bench's procedural texture tests) */
}

/* ---------------- the linear heap's account ---------------- */

unsigned int ctr_gpu_mesh_linear_bytes(int mesh) {
  return mesh >= 0 && mesh < MAX_MESHES && g.meshes[mesh].used ? g.meshes[mesh].lin_bytes : 0;
}

unsigned int ctr_gpu_pool_linear_bytes(int pool) {
  unsigned int n = 0;
  if (pool >= 0 && pool < MAX_POOLS) {
    ctr_linear_lock();
    if (g.pools[pool].used && g.pools[pool].linear) {
      n = linearGetSize(g.pools[pool].linear);
    }
    ctr_linear_unlock();
  }
  return n;
}

static u32 lin_size(const void* p) {
  return p ? linearGetSize((void*)p) : 0;
}

void ctr_gpu_linear_account_get(ctr_gpu_linear_account* out) {
  /* a texture slot is 1 for a render target's texture, 2 for an animation slot's */
  static uint8_t mark[MAX_TEXTURES];
  memset(out, 0, sizeof(*out));
  if (!g.ready) {
    return;
  }
  ctr_linear_lock();
  out->heap = __ctru_linear_heap_size;
  out->free = (unsigned int)linearSpaceFree();
  out->before = s_lin_before;
  out->blocks = s_block_bytes;
  out->fixed = lin_size(__C3D_Context.cmdBuf) + lin_size(s_vbufs[0] ? s_vbufs[0] : g.vbuf) +
               lin_size(s_vbufs[1]) + lin_size(g.quad_indices) + lin_size(s_grid_idx) +
               lin_size(s_pres.fence) + lin_size(s_pres.ids) + lin_size(s_bd.probe) +
               lin_size(s_bd.scis);
  for (int i = 0; i < PRESENT_BUFS; i++) {
    out->fixed += lin_size(s_pres.buf[i]);
  }
  if (g.fog_tex_valid) {
    out->fixed += lin_size(g.fog_tex.data);
    for (int i = 0; i < FOG_RAMP_COPIES; i++) {
      if (s_fog_ramp_cur >= 0 && s_fog_ramp[i].data != g.fog_tex.data) {
        out->fixed += lin_size(s_fog_ramp[i].data);
      }
    }
  }
  if (s_ocean_fade_ok > 0) {
    out->fixed += lin_size(s_ocean_fade[0].data) + lin_size(s_ocean_fade[1].data);
  }
  for (int i = 0; i < g.pending_staging_count; i++) {
    out->staging += lin_size(g.pending_staging[i]);
  }
  for (int i = 0; i < MAX_MESHES; i++) {
    const MeshSlot* m = &g.meshes[i];
    if (!m->used) {
      continue;
    }
    /* (lin_bytes is 0 for a mesh in a caller's block; 3: being made, verts_alt not set yet) */
    const u32 alt = m->used == 3 ? 0 : lin_size(m->verts_alt);
    if (m->used == 2 || m->used == 4) {
      out->mesh_pending += m->lin_bytes + alt;
    } else {
      out->meshes += m->lin_bytes;
      out->mesh_count += m->lin_bytes != 0;
      out->mesh_copies += alt;
    }
  }
  for (int i = 0; i < MAX_POOLS; i++) {
    const TexPool* p = &g.pools[i];
    if (p->used == 2 || p->used == 4) {
      out->pool_pending += lin_size(p->linear);
    } else if (p->used) {
      out->pools += lin_size(p->linear);
      out->pool_count++;
    }
  }
  memset(mark, 0, sizeof(mark));
  for (int i = 0; i < CTR_ANIM_SLOTS; i++) {
    if (s_anim_slot_next[i] > 0 && s_anim_slot_next[i] <= MAX_TEXTURES) {
      mark[s_anim_slot_next[i] - 1] = 2;
    }
  }
  for (int i = 0; i < MAX_RT; i++) {
    if (s_rt[i].depth && !s_rt[i].depth_vram) {
      out->tex_target += lin_size(s_rt[i].depth);
    }
    if (!s_rt[i].tex1) {
      continue;
    }
    mark[s_rt[i].tex1 - 1] = 1;
    for (int l = 0; l < RT_MIP_MAX; l++) {
      if (s_rt[i].mip[l] >= 0 && s_rt[i].mip[l] < MAX_TEXTURES) {
        mark[s_rt[i].mip[l]] = 1;
      }
    }
  }
  for (int i = 0; i < MAX_TEXTURES; i++) {
    const TexSlot* t = &g.textures[i];
    /* (3: being made, its fields not all set; a pool's texture has no memory of its own) */
    if ((t->used != 1 && t->used != 2 && t->used != 4) || t->pool >= 0) {
      continue;
    }
    const u32 own = lin_size(t->arena_off >= 0 ? t->home : t->tex.data);
    const u32 alt = lin_size(t->alt);
    if (t->used != 1) {
      out->tex_pending += own + alt;
    } else if (i == g.screen_tex) {
      out->tex_screen += own;
    } else if (mark[i] == 1) {
      out->tex_target += own;
    } else {
      if (t->compact) {
        out->tex_compact += own;
        out->tex_compact_count++;
      } else {
        out->tex_plain += own;
        out->tex_plain_count += own != 0;
      }
      out->tex_copies += alt;
      if (mark[i] == 2) {
        out->tex_anim += own + alt;
      }
    }
  }
  ctr_linear_unlock();
  out->other = (int)(out->heap - out->free) -
               (int)(out->before + out->fixed + out->meshes + out->mesh_copies + out->mesh_pending +
                     out->pools + out->pool_pending + out->tex_compact + out->tex_plain +
                     out->tex_target + out->tex_screen + out->tex_copies + out->tex_pending +
                     out->staging + out->blocks);
}

/* ---------------- (AI-assisted, depthreuse) config.ini depth_reuse ----------------
 * The top target's depth after what the frame drew so far, copied by the GPU (GX_TextureCopy,
 * queued after a partial submit of the frame's commands, so after them in the GX queue) into one
 * linear buffer (240 x 400 x 4 bytes). It is complete once the frame's fence has landed (steady
 * pacing: the fence copy is queued at the frame end, after it), then reduced on the CPU to 8x8
 * blocks: the buffer's tile (tx, ty) (8x8 pixels, 64 words) is the screen's block
 * (ty, 29 - tx) (write_screenshot's mapping: screen (x, y) = buffer (239 - y, x)); a D24S8 word's
 * depth is its low 24 bits (0 far, the cleared value; larger = closer). Not
 * without the pipeline and steady pacing. One copy at a time. */
static struct {
  u32* buf;
  u32 bytes;
  int failed;
  int busy;
  u32 frame;
  unsigned seq;
  float user_q[16], user[16];
  float zfar[50 * 30], znear[50 * 30];
} s_dr;

void ctr_gpu_depth_copy(const float user[16]) {
  if (!g.ready || !g.in_frame || s_dr.failed || s_dr.busy || !s_pipeline || !s_pres.on ||
      !g.top || s_bd.frame) {
    return; /* (a gpu_breakdown frame submits its own parts: no copy of ours in between) */
  }
  const C3D_FrameBuf* fb = &g.top->frameBuf;
  if (fb->width != 240 || fb->height != 400 || fb->depthFmt != GPU_RB_DEPTH24_STENCIL8) {
    s_dr.failed = 1;
    printf("[ctr] depth_reuse: the top target is %ux%u, depth format %d: off\n",
           (unsigned)fb->width, (unsigned)fb->height, (int)fb->depthFmt);
    return;
  }
  if (!s_dr.buf) {
    s_dr.bytes = 240u * 400u * 4u;
    s_dr.buf = (u32*)safe_linear_alloc(s_dr.bytes);
    if (!s_dr.buf) {
      s_dr.failed = 1;
      printf("[ctr] depth_reuse: no linear memory for the copy (%u KB): off\n",
             (unsigned)(s_dr.bytes / 1024));
      return;
    }
    printf("[ctr] depth_reuse: copy buffer %u KB of linear memory\n", (unsigned)(s_dr.bytes / 1024));
  }
  pipeline_submit(); /* what is drawn so far, queued before the copy */
  if (!gx_begin()) {
    return;
  }
  GX_TextureCopy((u32*)fb->depthBuf, 0, s_dr.buf, 0, s_dr.bytes, 8);
  gx_end();
  s_dr.busy = 1;
  s_dr.frame = (u32)g.frame_no;
  memcpy(s_dr.user_q, user, sizeof(s_dr.user_q));
}

unsigned ctr_gpu_depth_blocks(float* zfar, float* znear, float user[16]) {
  if (s_dr.busy && (s32)(s_gt.done_id - s_dr.frame) >= 0) {
    GSPGPU_InvalidateDataCache(s_dr.buf, s_dr.bytes);
    const float k = 1.0f / 16777215.0f;
    for (u32 t = 0; t < 30u * 50u; t++) {
      const u32* w = s_dr.buf + t * 64u;
      u32 lo = 0xffffffu, hi = 0;
      for (int i = 0; i < 64; i++) {
        const u32 d = w[i] & 0xffffffu;
        lo = d < lo ? d : lo;
        hi = d > hi ? d : hi;
      }
      const u32 tx = t % 30u, ty = t / 30u;
      const u32 b = (29u - tx) * 50u + ty;
      s_dr.zfar[b] = (float)lo * k;
      s_dr.znear[b] = (float)hi * k;
    }
    memcpy(s_dr.user, s_dr.user_q, sizeof(s_dr.user));
    s_dr.busy = 0;
    s_dr.seq++;
  }
  if (!s_dr.seq) {
    return 0;
  }
  memcpy(zfar, s_dr.zfar, sizeof(s_dr.zfar));
  memcpy(znear, s_dr.znear, sizeof(s_dr.znear));
  memcpy(user, s_dr.user, sizeof(s_dr.user));
  return s_dr.seq;
}
