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

#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

extern const uint8_t ctr_basic_shbin[];
extern const size_t ctr_basic_shbin_size;
extern const uint8_t ctr_mesh_shbin[];
extern const size_t ctr_mesh_shbin_size;
extern const uint8_t ctr_skin_shbin[];
extern const size_t ctr_skin_shbin_size;
extern const uint8_t ctr_skin_env_shbin[];
extern const size_t ctr_skin_env_shbin_size;
extern const uint8_t ctr_clip_shbin[];
extern const size_t ctr_clip_shbin_size;
extern const uint8_t ctr_clip2_shbin[];
extern const size_t ctr_clip2_shbin_size;

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

/* (AI-assisted) CPU data cache flushes for memory the GPU reads. GSPGPU_FlushDataCache is an IPC
 * round trip to the GSP service on the system core, which then makes the kernel call; here the
 * kernel call is made directly (svcFlushProcessDataCache: allowed in gk.rsf, like the
 * svcInvalidateProcessDataCache the vblank callback uses), with GSP as the fallback if the kernel
 * refuses it. Any thread. */
static int s_svc_flush = 1;
static void cache_flush_now(const void* addr, u32 size) {
  if (!size) {
    return;
  }
  if (s_svc_flush &&
      R_SUCCEEDED(svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)addr, size))) {
    return;
  }
  s_svc_flush = 0;
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

#define VBUF_BYTES (1536 * 1024)
#define MAX_STAGING 1024
#define MAX_TEXTURES 2048
#define MAX_POOLS 16
/* GPU command buffer: a frame's commands must all fit (splitting it doesn't make room). 2 MB of
 * linear memory; a busy frame (3000 level draws) takes ~1 MB. */
#define CMDBUF_BYTES (2 * 1024 * 1024)
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
} MeshSlot;

enum { PROG_NONE = 0, PROG_BASIC, PROG_MESH, PROG_SKIN, PROG_SKIN_ENV, PROG_CLIP, PROG_CLIP2 };

static struct {
  int ready;
  C3D_RenderTarget* top;
  DVLB_s* dvlb;
  shaderProgram_s program;
  int uloc_projection;
  DVLB_s* mesh_dvlb;
  shaderProgram_s mesh_program;
  int uloc_clip;
  int uloc_scales;
  int uloc_fog0, uloc_fog1;
  float fog0[4], fog1[4]; /* see ctr_gpu_set_mesh_fog */
  C3D_Tex fog_tex;        /* 64x8: alpha ramp 0..1 along s, fog color */
  uint32_t fog_tex_rgb;
  int fog_tex_valid;
  DVLB_s* skin_dvlb;
  shaderProgram_s skin_program;
  int uloc_skin_clip, uloc_skin_rows[3], uloc_skin_scales, uloc_skin_lights;
  DVLB_s* env_dvlb; /* the envmap pass of skinned meshes (ctr_skin_env.v.pica) */
  shaderProgram_s env_program;
  int uloc_env_clip, uloc_env_rows[3], uloc_env_scales, uloc_env_fade;
  DVLB_s* clip_dvlb; /* immediate draws in clip space (ctr_clip.v.pica) */
  shaderProgram_s clip_program;
  int uloc_clip_glpica;
  DVLB_s* clip2_dvlb; /* the same with two texture coordinates (ctr_clip2.v.pica: clouds) */
  shaderProgram_s clip2_program;
  int uloc_clip2_glpica;
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
  float last_clip[16];
  /* last ctr_gpu_draw_skinned: its matrix and lights (a model's draws share them) */
  int last_skin_valid;
  float last_skin_clip[16];
  float last_skin_lights[28];
  int last_skin_palette_count;
  float last_skin_bones[CTR_MAX_PALETTE * 12];
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
  int vram_textures;
  int vram_copy_failures;
  int screen_tex; /* texture slot of ctr_gpu_copy_screen (-1: not made yet), 256x512 RGBA8 */
  char screenshot_path[256];
  int screenshot_state;  /* 0 none, 1 requested, 2 frame rendered: write at next frame begin */
} g;

static int g_color16 = 0;
/* (AI-assisted) steady frame pacing, see pres_vblank */
static int g_steady_pacing;
static int pres_init(void);

/* The previous frame is still in the render target's color buffer (VRAM, tiled RGBA8 or RGB565,
 * 240x400 rotated) at the start of the next frame, after the GPU finished it. Write it as a BMP. */
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
      linearFree(m->verts);
      linearFree(m->indices);
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

/* Programs have different vertex layouts and uniforms: switch both together. */
static void use_program(int prog) {
  if (g.cur_prog == prog) {
    return;
  }
  raw_note_unif(); /* (AI-assisted) citro3d sends the program and uniforms at the next draw */
  g.last_mesh_valid = 0;
  g.last_skin_valid = 0;
  C3D_AttrInfo* attr = C3D_GetAttrInfo();
  AttrInfo_Init(attr);
  if (prog == PROG_BASIC) {
    C3D_BindProgram(&g.program);
    g.last_state_valid = 0;
    AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 3);         /* position */
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2);         /* texcoord */
    AttrInfo_AddLoader(attr, 2, GPU_UNSIGNED_BYTE, 4); /* color */
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g.uloc_projection, &g.projection);
  } else if (prog == PROG_CLIP) {
    C3D_BindProgram(&g.clip_program);
    g.last_state_valid = 0;
    AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 4);         /* clip space position */
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2);         /* texcoord */
    AttrInfo_AddLoader(attr, 2, GPU_UNSIGNED_BYTE, 4); /* color */
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g.uloc_clip_glpica, &g.gl_to_pica);
  } else if (prog == PROG_CLIP2) {
    C3D_BindProgram(&g.clip2_program);
    g.last_state_valid = 0;
    AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 4);         /* clip space position */
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2);         /* texcoord 0 */
    AttrInfo_AddLoader(attr, 2, GPU_FLOAT, 2);         /* texcoord 1 */
    AttrInfo_AddLoader(attr, 3, GPU_UNSIGNED_BYTE, 4); /* color */
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g.uloc_clip2_glpica, &g.gl_to_pica);
  } else if (prog == PROG_SKIN || prog == PROG_SKIN_ENV) {
    C3D_BindProgram(prog == PROG_SKIN ? &g.skin_program : &g.env_program);
    g.last_state_valid = 0;
    AttrInfo_AddLoader(attr, 0, GPU_SHORT, 3);         /* position */
    AttrInfo_AddLoader(attr, 1, GPU_UNSIGNED_BYTE, 3); /* bone indices */
    AttrInfo_AddLoader(attr, 2, GPU_UNSIGNED_BYTE, 3); /* weights */
    AttrInfo_AddLoader(attr, 3, GPU_SHORT, 2);         /* texcoord * 1024 */
    AttrInfo_AddLoader(attr, 4, GPU_UNSIGNED_BYTE, 4); /* color */
    AttrInfo_AddLoader(attr, 5, GPU_BYTE, 3);          /* normal * 127 */
    C3D_FVUnifSet(GPU_VERTEX_SHADER, prog == PROG_SKIN ? g.uloc_skin_scales : g.uloc_env_scales,
                  1.0f / 1024.0f, 1.0f / 255.0f, 1.0f, 1.0f / 127.0f);
  } else {
    C3D_BindProgram(&g.mesh_program);
    g.last_state_valid = 0;
    AttrInfo_AddLoader(attr, 0, GPU_SHORT, 4);         /* position (+ pad) */
    AttrInfo_AddLoader(attr, 1, GPU_SHORT, 2);         /* texcoord * 1024 */
    AttrInfo_AddLoader(attr, 2, GPU_UNSIGNED_BYTE, 4); /* color */
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g.uloc_scales, 1.0f / 1024.0f, 1.0f / 255.0f, 1.0f, 0.0f);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g.uloc_fog0, g.fog0[0], g.fog0[1], g.fog0[2], g.fog0[3]);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g.uloc_fog1, g.fog1[0], g.fog1[1], g.fog1[2], g.fog1[3]);
  }
  g.cur_prog = prog;
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
  g.top = C3D_RenderTargetCreate(240, 400, g_color16 ? GPU_RB_RGB565 : GPU_RB_RGBA8,
                                 GPU_RB_DEPTH24_STENCIL8);
  if (!g.top) {
    return -2;
  }
  /* (AI-assisted) with steady pacing the frames go to our own screen buffers (pres_queue_frame) */
  if (!g_steady_pacing || !pres_init()) {
    C3D_RenderTargetSetOutput(g.top, GFX_TOP, GFX_LEFT,
                              g_color16 ? DISPLAY_TRANSFER_FLAGS_565 : DISPLAY_TRANSFER_FLAGS);
  }

  g.dvlb = DVLB_ParseFile((u32*)ctr_basic_shbin, (u32)ctr_basic_shbin_size);
  shaderProgramInit(&g.program);
  shaderProgramSetVsh(&g.program, &g.dvlb->DVLE[0]);
  C3D_BindProgram(&g.program);
  g.uloc_projection = shaderInstanceGetUniformLocation(g.program.vertexShader, "projection");

  g.mesh_dvlb = DVLB_ParseFile((u32*)ctr_mesh_shbin, (u32)ctr_mesh_shbin_size);
  shaderProgramInit(&g.mesh_program);
  shaderProgramSetVsh(&g.mesh_program, &g.mesh_dvlb->DVLE[0]);
  g.uloc_clip = shaderInstanceGetUniformLocation(g.mesh_program.vertexShader, "clip");
  g.uloc_scales = shaderInstanceGetUniformLocation(g.mesh_program.vertexShader, "scales");
  g.uloc_fog0 = shaderInstanceGetUniformLocation(g.mesh_program.vertexShader, "fog0");
  g.uloc_fog1 = shaderInstanceGetUniformLocation(g.mesh_program.vertexShader, "fog1");
  /* no fog until ctr_gpu_set_mesh_fog */
  g.fog0[0] = 0.0f;
  g.fog0[1] = 255.0f;
  g.fog0[2] = 255.0f;
  g.fog0[3] = -1.0f / 255.0f;
  g.fog1[0] = 0.0f;
  g.fog1[1] = 1.0f;
  g.fog1[2] = 0.0f;
  g.fog1[3] = 0.0f;
  if (C3D_TexInit(&g.fog_tex, 64, 8, GPU_RGBA8)) {
    C3D_TexSetFilter(&g.fog_tex, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetWrap(&g.fog_tex, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    g.fog_tex_valid = 1;
    g.fog_tex_rgb = 0xffffffff; /* force the first fill */
  }
  g.skin_dvlb = DVLB_ParseFile((u32*)ctr_skin_shbin, (u32)ctr_skin_shbin_size);
  shaderProgramInit(&g.skin_program);
  shaderProgramSetVsh(&g.skin_program, &g.skin_dvlb->DVLE[0]);
  g.uloc_skin_clip = shaderInstanceGetUniformLocation(g.skin_program.vertexShader, "clip");
  g.uloc_skin_rows[0] = shaderInstanceGetUniformLocation(g.skin_program.vertexShader, "row0");
  g.uloc_skin_rows[1] = shaderInstanceGetUniformLocation(g.skin_program.vertexShader, "row1");
  g.uloc_skin_rows[2] = shaderInstanceGetUniformLocation(g.skin_program.vertexShader, "row2");
  g.uloc_skin_scales = shaderInstanceGetUniformLocation(g.skin_program.vertexShader, "scales");
  g.uloc_skin_lights = shaderInstanceGetUniformLocation(g.skin_program.vertexShader, "lights");
  g.env_dvlb = DVLB_ParseFile((u32*)ctr_skin_env_shbin, (u32)ctr_skin_env_shbin_size);
  shaderProgramInit(&g.env_program);
  shaderProgramSetVsh(&g.env_program, &g.env_dvlb->DVLE[0]);
  g.uloc_env_clip = shaderInstanceGetUniformLocation(g.env_program.vertexShader, "clip");
  g.uloc_env_rows[0] = shaderInstanceGetUniformLocation(g.env_program.vertexShader, "row0");
  g.uloc_env_rows[1] = shaderInstanceGetUniformLocation(g.env_program.vertexShader, "row1");
  g.uloc_env_rows[2] = shaderInstanceGetUniformLocation(g.env_program.vertexShader, "row2");
  g.uloc_env_scales = shaderInstanceGetUniformLocation(g.env_program.vertexShader, "scales");
  g.uloc_env_fade = shaderInstanceGetUniformLocation(g.env_program.vertexShader, "fade");
  g.clip_dvlb = DVLB_ParseFile((u32*)ctr_clip_shbin, (u32)ctr_clip_shbin_size);
  shaderProgramInit(&g.clip_program);
  shaderProgramSetVsh(&g.clip_program, &g.clip_dvlb->DVLE[0]);
  g.uloc_clip_glpica = shaderInstanceGetUniformLocation(g.clip_program.vertexShader, "glpica");
  g.clip2_dvlb = DVLB_ParseFile((u32*)ctr_clip2_shbin, (u32)ctr_clip2_shbin_size);
  shaderProgramInit(&g.clip2_program);
  shaderProgramSetVsh(&g.clip2_program, &g.clip2_dvlb->DVLE[0]);
  g.uloc_clip2_glpica = shaderInstanceGetUniformLocation(g.clip2_program.vertexShader, "glpica");
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
      linearFree(g.meshes[i].verts);
      linearFree(g.meshes[i].indices);
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
  shaderProgramFree(&g.clip2_program);
  DVLB_Free(g.clip2_dvlb);
  shaderProgramFree(&g.program);
  DVLB_Free(g.dvlb);
  shaderProgramFree(&g.mesh_program);
  DVLB_Free(g.mesh_dvlb);
  shaderProgramFree(&g.skin_program);
  DVLB_Free(g.skin_dvlb);
  shaderProgramFree(&g.env_program);
  DVLB_Free(g.env_dvlb);
  shaderProgramFree(&g.clip_program);
  DVLB_Free(g.clip_dvlb);
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
static int s_pipeline = 0;
/* (AI-assisted) 1 MB each: the busiest jungle frames on hardware use ~450 KB of commands; a frame
 * that still runs out of room switches the pipeline off (CtrRenderer::render_frame) instead of
 * losing its last draws every frame. 2 MB each cost the linear memory a level needs. */
#define PIPE_CMD_BYTES (1024 * 1024)
static u32* s_cmdbufs[2];
static uint8_t* s_vbufs[2];
static int s_k;

int ctr_gpu_set_pipeline(int on) {
  if (!g.ready || g.in_frame) {
    return 0;
  }
  if (on && !s_cmdbufs[1]) {
    ctr_linear_lock();
    s_cmdbufs[0] = (u32*)linearAlloc(PIPE_CMD_BYTES);
    s_cmdbufs[1] = (u32*)linearAlloc(PIPE_CMD_BYTES);
    s_vbufs[1] = (uint8_t*)linearAlloc(VBUF_BYTES);
    ctr_linear_unlock();
    s_vbufs[0] = g.vbuf;
    if (!s_cmdbufs[0] || !s_cmdbufs[1] || !s_vbufs[1]) {
      ctr_linear_lock();
      linearFree(s_cmdbufs[0]);
      linearFree(s_cmdbufs[1]);
      linearFree(s_vbufs[1]);
      ctr_linear_unlock();
      s_cmdbufs[0] = s_cmdbufs[1] = NULL;
      s_vbufs[1] = NULL;
      return 0;
    }
  }
  s_pipeline = on && s_cmdbufs[1];
  if (!s_pipeline && s_vbufs[0]) {
    g.vbuf = s_vbufs[0];
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
  } q[PRESENT_Q];
  volatile u32 q_head, q_tail;            /* render thread writes q_tail, the callback q_head */
  volatile int buf_state[PRESENT_BUFS];   /* 0 free, 1 queued, 2 requested, 3 on screen */
  volatile int requested, shown, flip;
  volatile u32 last_shown;
  volatile u32 hist[5];                   /* frames on screen for 1, 2, 3, 4, 5+ vblanks */
  volatile u32 late, dropped;
  volatile int min_slack;                 /* vblanks to spare of the frames since the last check */
  int latency;
  u32 frames_since_change;
  double buf_wait_ms; /* the render thread waiting for a free screen buffer */
} s_pres;

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
    const u32 id = s_pres.q[s_pres.q_head % PRESENT_Q].id;
    const u32 target = s_pres.q[s_pres.q_head % PRESENT_Q].target;
    const int b = s_pres.q[s_pres.q_head % PRESENT_Q].buf;
    if ((s32)(id - done) > 0) {
      break; /* the GPU isn't done with it */
    }
    const s32 slack = (s32)(target - (v + 1));
    if (slack > 0) {
      if (slack < s_pres.min_slack) {
        s_pres.min_slack = slack;
      }
      break; /* too early: its vblank comes later */
    }
    if (slack < 0 && slack >= -2) {
      /* missed its vblank by one or two: the latency is too short. A frame later than that is a
       * stall (a level load, a save), which more latency wouldn't have hidden. */
      s_pres.late++;
      s_pres.min_slack = -1;
    } else if (slack < 0) {
      s_pres.min_slack = -1;
    } else if (s_pres.min_slack > 0) {
      s_pres.min_slack = 0;
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
  s_pres.min_slack = 99;
  s_pres.vblanks = C3D_FrameCounter(0);
  s_pres.on = 1;
  /* replaces citro3d's vblank counter (C3D_FrameCounter isn't used with this on) */
  gspSetEventCallback(GSPGPU_EVENT_VBlank0, pres_vblank, NULL, false);
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
  C3D_FrameSplit(GX_CMDLIST_FLUSH); /* the draws first */
  GX_DisplayTransfer((u32*)g.top->frameBuf.colorBuf, GX_BUFFER_DIM(240, 400), (u32*)s_pres.buf[b],
                     GX_BUFFER_DIM(240, 400),
                     g_color16 ? DISPLAY_TRANSFER_FLAGS_565 : DISPLAY_TRANSFER_FLAGS);
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
  /* the latency: up after a late frame, down after ~5 s with a vblank to spare every time */
  s_pres.frames_since_change++;
  if (s_pres.late) {
    s_pres.late = 0;
    if (s_pres.latency < PRESENT_MAX_LATENCY) {
      s_pres.latency++;
    }
    s_pres.frames_since_change = 0;
    s_pres.min_slack = 99;
  } else if (s_pres.frames_since_change >= 150) {
    if (s_pres.min_slack >= 1 && s_pres.min_slack != 99 && s_pres.latency > 2) {
      s_pres.latency--;
    }
    s_pres.frames_since_change = 0;
    s_pres.min_slack = 99;
  }
  const u32 t = s_pres.q_tail;
  s_pres.q[t % PRESENT_Q].id = g.frame_no;
  s_pres.q[t % PRESENT_Q].target = s_pres.cur_release + (u32)s_pres.latency;
  s_pres.q[t % PRESENT_Q].buf = b;
  __dsb();
  s_pres.q_tail = t + 1;
}

void ctr_gpu_pacing_stats(unsigned int hist[5], int* latency, unsigned int* dropped,
                          double* buffer_wait_ms) {
  *buffer_wait_ms = s_pres.buf_wait_ms;
  s_pres.buf_wait_ms = 0;
  for (int i = 0; i < 5; i++) {
    hist[i] = s_pres.hist[i];
    s_pres.hist[i] = 0;
  }
  *latency = s_pres.on ? s_pres.latency : -1;
  *dropped = s_pres.dropped;
  s_pres.dropped = 0;
}

static void migrate_small_textures(void);

/* the parts of the GPU state a frame starts from (both modes) */
static void frame_state_reset(void) {
  s_pres.cur_release = s_pres.next_release;
  raw_frame_reset();
  g.cur_prog = PROG_NONE;
  g.last_state_valid = 0;
  g.bound_tex = NULL;
  use_program(PROG_BASIC);
  /* the fog ramp stays on texture unit 1 for the whole frame (binding a texture clears the GPU's
   * texture cache: once per frame, not at every level draw) */
  if (g.fog_tex_valid) {
    C3D_TexBind(1, &g.fog_tex);
  }
  /* (AI-assisted) the early depth buffer starts as "far" like the depth buffer (cleared to 0;
   * larger = closer). citro3d clears it only after frames that drew; the first frame needs it. */
  GPUCMD_AddMaskedWrite(GPUREG_EARLYDEPTH_DATA, 0x7, 0);
  GPUCMD_AddWrite(GPUREG_EARLYDEPTH_CLEAR, 1);
  C3D_EarlyDepthTest(false, GPU_EARLYDEPTH_GEQUAL, 0);
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
  GPUCMD_SetBuffer(s_cmdbufs[s_k], PIPE_CMD_BYTES / 4, 0);
  g.cmd_base = gpuCmdBuf;
  g.vbuf = s_vbufs[s_k];
  g.frame_no++;
  /* copies to VRAM wait for the GPU first (citro3d's safe copy outside of a frame) */
  update_pool_residency();
  C3D_FrameBuf* fb = &g.top->frameBuf;
  u32* color = (u32*)fb->colorBuf;
  u32* depth = (u32*)fb->depthBuf;
  const u32 px = (u32)fb->width * fb->height; /* D24S8: 4 bytes; color 4 (RGBA8) or 2 (RGB565) */
  u32* color_end = (u32*)((uint8_t*)color + px * (g_color16 ? 2 : 4));
  GX_MemoryFill(color, clear, color_end,
                GX_FILL_TRIGGER | (g_color16 ? GX_FILL_16BIT_DEPTH : GX_FILL_32BIT_DEPTH), depth, 0,
                depth + px, GX_FILL_TRIGGER | GX_FILL_32BIT_DEPTH);
  C3D_SetFrameBuf(fb);
  C3D_SetViewport(0, 0, fb->width, fb->height);
  frame_state_reset();
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
  if (words) {
    GX_ProcessCommandList(list, words * 4, GX_CMDLIST_FLUSH);
  }
}

void ctr_gpu_frame_begin(uint8_t r, uint8_t gr, uint8_t b) {
  if (!g.ready) {
    return;
  }
  /* texture combiner constant: R in the low byte */
  g.clear_abgr = 0xff000000u | ((u32)b << 16) | ((u32)gr << 8) | (u32)r;
  if (s_pipeline) {
    pipeline_frame_begin(color_fill_value(r, gr, b));
    return;
  }
  C3D_FrameBegin(0); /* waits for the GPU; the game thread already paces to vblank */
  g.cmd_base = gpuCmdBuf;
  g.frame_no++;
  process_pending_deletes(0);
  /* before any draw: copies to VRAM are queued ahead of this frame's draws */
  update_pool_residency();
  if (g.screenshot_state == 2) {
    write_screenshot();
    g.screenshot_state = 0;
  }
  C3D_RenderTargetClear(g.top, C3D_CLEAR_ALL, color_fill_value(r, gr, b), 0);
  C3D_FrameDrawOn(g.top);
  frame_state_reset();
}

/* Immediate draws copy their vertices into g.vbuf. The GPU reads them only when the frame's GX
 * queue runs (C3D_FrameEnd; C3D_FrameBegin stopped the queue, so command list splits wait in it
 * until then), so one cache flush of what this frame wrote is enough. Each flush is an IPC to the
 * GSP service: one per draw cost tens of microseconds each on hardware (free in Azahar). */
/* (AI-assisted) The render thread's flushes for this frame's GPU work (texture updates: sky,
 * clouds, ocean, eyes; blend shape vertices; the fog ramp) wait here until the frame's commands
 * go to the GPU (flush_vbuf, at every submission point), merged where the ranges touch. */
#define DEFER_FLUSH_MAX 48
static struct {
  u32 start[DEFER_FLUSH_MAX], end[DEFER_FLUSH_MAX];
  int n;
} s_defer;

static void cache_flush_frame(const void* addr, u32 size) {
  if (!g.in_frame) {
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
    gxCmdQueueRun(&__C3D_Context.gxQueue);
  }
  s_partial_draws = g.cur.draws;
  s_partials++;
}

void ctr_gpu_frame_end(void) {
  if (!g.ready || !g.in_frame) {
    return;
  }
  raw_vc_flush();
  flush_vbuf();
  /* the command words of the whole frame (splits move gpuCmdBuf on; C3D_FrameBegin restarts it) */
  g.cur.cmd_bytes = (unsigned int)((gpuCmdBuf + gpuCmdBufOffset - g.cmd_base) * 4);
  if (s_pipeline) {
    /* wait for the previous frame (and this one's parts sent so far), then submit the rest of
     * this frame from our buffer with citro3d (display transfer, buffer swap) */
    u32* buf = gpuCmdBuf;
    const u32 size = gpuCmdBufSize, offset = gpuCmdBufOffset;
    C3D_FrameBegin(0);
    process_pending_deletes(1);
    GPUCMD_SetBuffer(buf, size, offset);
    C3D_FrameDrawOn(g.top);
  }
  if (s_pres.on) {
    pres_queue_frame();
  }
  /* all our buffers are flushed when written: only flush the command list, not the whole
   * linear heap (C3D_FrameEnd's default) */
  C3D_FrameEnd(GX_CMDLIST_FLUSH);
  g.in_frame = 0;
  if (g.screenshot_state == 1) {
    g.screenshot_state = 2;
  }
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
  /* times of the last frame the GPU finished (the one before this) */
  g.cur.gpu_ms = C3D_GetProcessingTime();
  g.cur.draw_ms = C3D_GetDrawingTime();
  g.cur.linear_free = (unsigned int)linearSpaceFree();
  g.cur.vram_free = (unsigned int)vramSpaceFree();
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

static inline u32 morton8(u32 x, u32 y) {
  return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) |
         ((y & 4) << 3);
}

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
  cache_flush_now(buf, tex->size);
  sync_texture_copy((u32*)buf, 0, (u32*)tex->data, 0, tex->size, 8);
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

/* linear RGBA8 (top row first) -> the GPU's tiled RGBA8 (A, B, G, R; bottom row first) */
static void swizzle_rgba8(uint8_t* dst, int w, int h, const uint8_t* rgba) {
  /* tiled: 8x8 tiles in rows, morton order inside a tile */
  const int tiles_x = w / 8;
  for (int y = 0; y < h; y++) {
    /* the GPU samples t = 0 from the last row in memory: store the image bottom row first */
    const int ty = h - 1 - y;
    const uint8_t* p = rgba + 4 * y * w;
    for (int x = 0; x < w; x++, p += 4) {
      u32 tile = (u32)((ty / 8) * tiles_x + (x / 8));
      u32 off = (tile * 64 + morton8(x & 7, ty & 7)) * 4;
      /* GPU_RGBA8 is stored as A, B, G, R */
      dst[off + 0] = p[3];
      dst[off + 1] = p[2];
      dst[off + 2] = p[1];
      dst[off + 3] = p[0];
    }
  }
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
static int g_proctex = 1;
static int g_compact = 1;

void ctr_gpu_set_color16(int on) {
  if (!g.ready) {
    g_color16 = on ? 1 : 0; /* the render target is made in ctr_gpu_init */
  }
}

void ctr_gpu_set_early_depth(int on) {
  g_early_depth = on ? 1 : 0;
}

void ctr_gpu_set_proctex(int on) {
  g_proctex = on ? 1 : 0;
}

void ctr_gpu_set_compact_textures(int on) {
  g_compact = on ? 1 : 0;
}

/* ---------------- (AI-assisted) compact textures ---------------- */

/* the texel at image (x, y) (y = 0 at the top) in the tiled layout (see swizzle_rgba8) */
static inline u32 tiled_texel(int x, int y, int w, int h) {
  const int ty = h - 1 - y;
  return (u32)((ty / 8) * (w / 8) + (x / 8)) * 64 + morton8((u32)x & 7, (u32)ty & 7);
}

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
  } else if (g_rgba4_as_rgba8) {
    return ctr_gpu_tex_create(w, h, rgba); /* Azahar draws RGBA4 as noise */
  } else {
    fmt = GPU_RGBA4;
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
  uint8_t* d8 = (uint8_t*)tex->data;
  uint16_t* d16 = (uint16_t*)tex->data;
  for (int y = 0; y < h; y++) {
    const uint8_t* p = rgba + 4 * y * w;
    for (int x = 0; x < w; x++, p += 4) {
      const u32 o = tiled_texel(x, y, w, h);
      const u32 a2 = p[3] >= 0x80 ? 255u : (u32)p[3] * 2u; /* GS alpha doubled: 0xff = 1.0 */
      switch (fmt) {
        case GPU_L8:
          d8[o] = p[1];
          break;
        case GPU_LA8:
          d8[2 * o] = (uint8_t)a2; /* stored A, L */
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
          d16[o] = (uint16_t)(((p[0] >> 4) << 12) | ((p[1] >> 4) << 8) | ((p[2] >> 4) << 4) |
                              (a2 >> 4));
          break;
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
  if (!s_arena.base) {
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
    C3D_TexGenerateMipmap(tex, GPU_TEXFACE_2D);
  }
  cache_flush_frame(tex->data, C3D_TexCalcTotalSize(tex->size, tex->maxLevel));
  /* bind again at the next use: that clears the GPU's texture cache (it may hold old texels) */
  if (g.bound_tex == tex) {
    g.bound_tex = NULL;
  }
}

void ctr_gpu_tex_delete(int handle) {
  if (handle < 0 || handle >= MAX_TEXTURES) {
    return;
  }
  ctr_linear_lock();
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

void ctr_gpu_set_vcache_reuse(int on) {
  s_raw.vcache_reuse = on ? 1 : 0;
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
  s_raw.vc_dirty = 0;
  s_raw.vs_changed = 1;
  s_raw.unif_pending = 1;
}

/* what the raw draws compare against is gone (the HOME Menu used the GPU) */
static void raw_check_epoch(void) {
  if (s_raw.apt_epoch != s_raw.seen_epoch) {
    s_raw.seen_epoch = s_raw.apt_epoch;
    g.last_mesh_valid = 0;
    g.last_skin_valid = 0;
    s_raw.vs_changed = 1;
  }
}

/* before a raw draw's uniforms and the draw: citro3d sends what it has pending */
static inline void raw_sync(void) {
  raw_check_epoch();
  if (s_raw.unif_pending || (__C3D_Context.flags & ~(u32)(C3DiF_Active | C3DiF_DrawUsed))) {
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

/* the f24 encoding of libctru's f32tof24 (truncated mantissa), packed 3 words per vector in the
 * order the GPU takes float24 uniforms */
static inline u32 raw_f24(float f) {
  union {
    float f;
    u32 i;
  } u = {f};
  const u32 sign = u.i >> 31, mant = (u.i & 0x7fffff) >> 7;
  const s32 e = (s32)((u.i >> 23) & 0xff) - 127 + 63;
  if (e < 0 || !((u.i >> 23) & 0xff)) {
    return sign << 23;
  }
  if (e > 0x7f) {
    return (sign << 23) | (0x7fu << 16);
  }
  return (sign << 23) | ((u32)e << 16) | mant;
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
  for (int i = 0; i < 24; i += 4) { /* (4 words per ldm / stm) */
    p[i] = kRawElements[i];
    p[i + 1] = kRawElements[i + 1];
    p[i + 2] = kRawElements[i + 2];
    p[i + 3] = kRawElements[i + 3];
  }
  p[4] = (index_pa - RAW_BUF_BASE) | BIT(31); /* u16 indices */
  p[6] = count;
  p += 24;
  if (s_raw.vcache_reuse) {
    s_raw.vc_dirty = 1;
  } else {
    p = raw_vc_clear(p);
  }
  raw_end(p);
  s_raw.vs_changed = 0;
  __C3D_Context.flags |= C3DiF_DrawUsed;
}

static void raw_draw_arrays(u32 count) {
  u32* p = raw_ptr();
  if (s_raw.vc_dirty) {
    p = raw_vc_clear(p);
  }
  for (int i = 0; i < 22; i += 2) {
    p[i] = kRawArrays[i];
    p[i + 1] = kRawArrays[i + 1];
  }
  p[6] = count;
  raw_end(p + 22);
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
    u32* p = raw_n(raw_ptr(), GPUREG_TEXUNIT0_BORDER_COLOR, regs, 5, 1);
    p = raw_w(p, GPUREG_TEXUNIT0_TYPE, 0xF, tex->fmt);
    ctx->tex[0] = tex;
    if (!(ctx->texConfig & 1)) {
      ctx->texConfig |= 1;
      p = raw_w(p, GPUREG_TEXUNIT_CONFIG, 0xB, ctx->texConfig);
    }
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

static int early_depth_ok(const ctr_draw_state* st) {
  return g_early_depth && (st->flags & CTR_STATE_EARLY_DEPTH) && st->ztest == CTR_TEST_GEQUAL &&
         st->zwrite && st->blend == CTR_BLEND_OFF;
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
 * raw: written as raw commands (see "raw GPU commands"), else through citro3d. Both keep
 * citro3d's context right. */
static void apply_state_tint(const ctr_draw_state* st, int mesh, uint32_t tint, int raw) {
  if (st->flags) {
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
  /* stage 1: lighting tint for skinned meshes, pass-through otherwise */
  C3D_TexEnv e1;
  C3D_TexEnv* env1 = &e1;
  C3D_TexEnvInit(env1);
  if (mesh == 2) {
    C3D_TexEnvSrc(env1, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, 0);
    C3D_TexEnvFunc(env1, C3D_RGB, GPU_MODULATE);
    C3D_TexEnvColor(env1, tint);
  } else if (mesh == 1 && g.fog_tex_valid) {
    /* fog: rgb = mix(previous, fog color, fog amount) with the amount from the fog ramp texture
     * (texcoord1 from the mesh shader; bound on unit 1 at frame begin) */
    C3D_TexEnvSrc(env1, C3D_RGB, GPU_TEXTURE1, GPU_PREVIOUS, GPU_TEXTURE1);
    C3D_TexEnvOpRgb(env1, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR,
                    GPU_TEVOP_RGB_SRC_ALPHA);
    C3D_TexEnvFunc(env1, C3D_RGB, GPU_INTERPOLATE);
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
  int textured = st->tex >= 0 && st->tex < MAX_TEXTURES && g.textures[st->tex].used == 1;
  TexSlot* slot = textured ? &g.textures[st->tex] : NULL;
  /* (AI-assisted) radial textures from the procedural texture unit (no texels read); compact
   * textures store GS alpha doubled (0xff = 1.0) */
  const int proc = textured && (st->flags & CTR_STATE_PROCTEX) && g_proctex && slot->radial >= 0 &&
                   s_radial[slot->radial];
  const GPU_TEVSRC tsrc = proc ? GPU_TEXTURE3 : GPU_TEXTURE0;
  const int afull = textured && slot->alpha_full;
  if (proc) {
    if (g.proc_radial != slot->radial) {
      C3D_ProcTexBind(0, &s_proctex);
      C3D_ProcTexLutBind(GPU_LUT_RGBMAP, &s_proc_map);
      C3D_ProcTexColorLutBind(s_radial[slot->radial]);
      g.proc_radial = slot->radial;
    }
    if (g.bound_tex) {
      C3D_TexBind(0, NULL); /* texture unit 0 off: it would fetch texels nothing uses */
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
    if (mesh) {
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
  }
  commit_texenv(1, env1, raw);
  commit_texenv(0, env, raw);

  /* (AI-assisted) the fragment operations, as citro3d's C3D_AlphaBlend / C3D_BlendingColor /
   * C3D_AlphaTest / C3D_DepthTest / C3D_EarlyDepthTest store them in its context */
  C3Di_Context* ctx = &__C3D_Context;
  C3Di_Effect e = ctx->effect;
  GPU_BLENDEQUATION eq = GPU_BLEND_ADD;
  GPU_BLENDFACTOR src = GPU_ONE, dst = GPU_ZERO;
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
    default:
      break;
  }
  e.alphaBlend = (u32)eq | ((u32)GPU_BLEND_ADD << 8) | ((u32)src << 16) | ((u32)dst << 20) |
                 ((u32)GPU_ONE << 24) | ((u32)GPU_ZERO << 28);
  e.fragOpMode = (e.fragOpMode & ~0xFF00u) | 0x0100u;
  if (st->atest == CTR_TEST_ALWAYS) {
    e.alphaTest = (u32)GPU_ALWAYS << 4;
  } else {
    int ref = st->aref * 2;
    e.alphaTest = 1u | (((u32)map_test(st->atest) & 7) << 4) | ((u32)(ref > 255 ? 255 : ref) << 8);
  }
  if (st->ztest == CTR_TEST_ALWAYS && !st->zwrite) {
    e.depthTest = ((u32)GPU_ALWAYS << 4) | ((u32)GPU_WRITE_COLOR << 8);
  } else {
    e.depthTest = 1u | (((u32)map_test(st->ztest) & 7) << 4) |
                  ((u32)(st->zwrite ? GPU_WRITE_ALL : GPU_WRITE_COLOR) << 8);
  }
  /* (AI-assisted) config.ini early_depth: fragments behind what opaque draws already covered are
   * dropped before texturing. Only for draws that write depth with the same test and that nothing
   * discards later (no blending, an alpha test that never fails): the hardware's early depth
   * data can't know about fragments dropped after it. */
  e.earlyDepth = early_depth_ok(st) != 0;
  e.earlyDepthFunc = GPU_EARLYDEPTH_GEQUAL;
  e.earlyDepthRef = 0;
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
  if (e.depthTest != c->depthTest) {
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
  if (e.earlyDepth != c->earlyDepth || e.earlyDepthFunc != c->earlyDepthFunc ||
      e.earlyDepthRef != c->earlyDepthRef) {
    p = raw_w(p, GPUREG_EARLYDEPTH_TEST1, 0x1, e.earlyDepth ? 1 : 0);
    p = raw_w(p, GPUREG_EARLYDEPTH_TEST2, 0xF, e.earlyDepth ? 1 : 0);
    p = raw_w(p, GPUREG_EARLYDEPTH_FUNC, 0x1, e.earlyDepthFunc);
    p = raw_w(p, GPUREG_EARLYDEPTH_DATA, 0x7, e.earlyDepthRef);
  }
  raw_end(p);
  *c = e;
}

/* (AI-assisted) the raw path takes every draw but the glows (procedural texture unit: citro3d) */
static inline int use_raw(const ctr_draw_state* st) {
  return s_raw.on && !(st->flags & CTR_STATE_PROCTEX);
}

/* the immediate draws (triangle lists from g.vbuf): program, state, then the draw */
static void draw_immediate(const ctr_draw_state* state, int prog, uint8_t* dst, u32 stride,
                           int count) {
  use_program(prog);
  if (use_raw(state)) {
    apply_state(state, 0, 1);
    raw_vertex_buffer(raw_vbuf_pa((size_t)(dst - g.vbuf)), 0x210,
                      RAW_BUF_FLAGS1(stride, 3, 0));
    raw_sync();
    raw_draw_arrays((u32)count);
  } else {
    raw_vc_flush();
    apply_state(state, 0, 0);
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, dst, stride, 3, 0x210);
    C3D_DrawArrays(GPU_TRIANGLES, 0, count);
  }
  g.cur.draws++;
  g.cur.triangles += count / 3;
}

void ctr_gpu_draw(const ctr_draw_state* state, const ctr_vertex* verts, int count) {
  if (!g.ready || !g.in_frame || count < 3) {
    return;
  }
  size_t bytes = (size_t)count * sizeof(ctr_vertex);
  if (g.vbuf_used + bytes > VBUF_BYTES) {
    /* out of vertex space this frame: drop the draw (the buffer is reset every frame) */
    return;
  }
  if (!cmd_room()) {
    return;
  }
  uint8_t* dst = g.vbuf + g.vbuf_used;
  memcpy(dst, verts, bytes); /* flushed once at the end of the frame (flush_vbuf) */
  g.vbuf_used += (bytes + 15) & ~(size_t)15;
  draw_immediate(state, PROG_BASIC, dst, sizeof(ctr_vertex), count);
}

void ctr_gpu_draw_clip(const ctr_draw_state* state, const ctr_clip_vertex* verts, int count) {
  if (!g.ready || !g.in_frame || count < 3) {
    return;
  }
  size_t bytes = (size_t)count * sizeof(ctr_clip_vertex);
  if (g.vbuf_used + bytes > VBUF_BYTES || !cmd_room()) {
    return;
  }
  uint8_t* dst = g.vbuf + g.vbuf_used;
  memcpy(dst, verts, bytes);
  g.vbuf_used += (bytes + 15) & ~(size_t)15;
  draw_immediate(state, PROG_CLIP, dst, sizeof(ctr_clip_vertex), count);
}

void ctr_gpu_draw_clip2(const ctr_draw_state* state, uint32_t const0, uint32_t const1,
                        const ctr_clip_vertex2* verts, int count) {
  if (!g.ready || !g.in_frame || count < 3 || state->tex < 0 || state->tex >= MAX_TEXTURES ||
      g.textures[state->tex].used != 1) {
    return;
  }
  size_t bytes = (size_t)count * sizeof(ctr_clip_vertex2);
  if (g.vbuf_used + bytes > VBUF_BYTES || !cmd_room()) {
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
  C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
  C3D_EarlyDepthTest(false, GPU_EARLYDEPTH_GEQUAL, 0);
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

void ctr_gpu_draw_quads(const ctr_draw_state* state, const ctr_vertex* verts, int quad_count) {
  if (!g.ready || !g.in_frame) {
    return;
  }
  while (quad_count > 0) {
    const int n = quad_count > QUAD_MAX ? QUAD_MAX : quad_count;
    const size_t bytes = (size_t)n * 4 * sizeof(ctr_vertex);
    if (g.vbuf_used + bytes > VBUF_BYTES || !cmd_room()) {
      return;
    }
    uint8_t* dst = g.vbuf + g.vbuf_used;
    memcpy(dst, verts, bytes);
    g.vbuf_used += (bytes + 15) & ~(size_t)15;
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
    verts += 4 * n;
    quad_count -= n;
  }
}

static void apply_state(const ctr_draw_state* st, int mesh, int raw) {
  apply_state_tint(st, mesh, 0xffffffff, raw);
}

/* ---------------- the screen as a texture ---------------- */

int ctr_gpu_copy_screen(void) {
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
    ctr_linear_lock();
    const bool ok = linear_room_for_tex(256, 512, 0) &&
                    C3D_TexInit(&t->tex, 256, 512, g_color16 ? GPU_RGB565 : GPU_RGBA8);
    ctr_linear_unlock();
    if (!ok) {
      release_tex_slot(slot);
      return -1;
    }
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
  const u32 row = g_color16 ? 3840 : 7680, gap = g_color16 ? 256 : 512;
  sync_texture_copy((u32*)g.top->frameBuf.colorBuf, GX_BUFFER_DIM(row / 16, 0),
                    (u32*)g.textures[g.screen_tex].tex.data, GX_BUFFER_DIM(row / 16, gap / 16),
                    row * 50, 8);
  /* the GPU's texture cache may hold the old texels */
  g.bound_tex = NULL;
  return g.screen_tex;
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
    const uint16_t* src = (const uint16_t*)data;
    int n = size / 2;
    if (n > w * h) {
      n = w * h;
    }
    for (int i = 0; i < n; i++) {
      const uint16_t v = src[i];
      dst[4 * i + 0] = (uint8_t)((v & 0xf) * 17);
      dst[4 * i + 1] = (uint8_t)(((v >> 4) & 0xf) * 17);
      dst[4 * i + 2] = (uint8_t)(((v >> 8) & 0xf) * 17);
      dst[4 * i + 3] = (uint8_t)(((v >> 12) & 0xf) * 17);
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

int ctr_gpu_mesh_create(const void* verts, int vertex_count, const uint16_t* indices,
                        int index_count) {
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
  memcpy(v, verts, vbytes);
  memcpy(ix, indices, ibytes);
  cache_flush_now(v, vbytes);
  cache_flush_now(ix, ibytes);
  g.meshes[slot].verts = v;
  g.meshes[slot].indices = ix;
  g.meshes[slot].verts_pa = osConvertVirtToPhys(v);
  g.meshes[slot].indices_pa = osConvertVirtToPhys(ix);
  g.meshes[slot].used = 1;
  return slot;
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

void ctr_gpu_prepare_mesh_matrix(const float clip[16], ctr_mesh_matrix* out) {
  /* final = gl_to_pica * clip, in C3D_Mtx layout (components reversed) */
  C3D_Mtx m;
  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      float acc = 0.0f;
      for (int k = 0; k < 4; k++) {
        acc += g.gl_to_pica.r[r].c[3 - k] * clip[4 * k + c];
      }
      m.r[r].c[3 - c] = acc;
    }
  }
  _Static_assert(sizeof(C3D_Mtx) == offsetof(ctr_mesh_matrix, f24), "matrix layout");
  memcpy(out->m, &m, sizeof(m));
  /* (AI-assisted) and as float24 uniform words for the raw draws (12 words instead of 16) */
  for (int r = 0; r < 4; r++) {
    raw_pack_f24(out->m + 4 * r, out->f24 + 3 * r);
  }
}

void ctr_gpu_draw_mesh_prepared(const ctr_draw_state* state, const ctr_mesh_matrix* matrix,
                                int mesh, int first_index, int index_count) {
  if (!g.ready || !g.in_frame || mesh < 0 || mesh >= MAX_MESHES || g.meshes[mesh].used != 1 ||
      index_count < 3) {
    return;
  }
  if (!cmd_room()) {
    return;
  }
  use_program(PROG_MESH);
  const MeshSlot* ms = &g.meshes[mesh];
  if (use_raw(state)) {
    /* (AI-assisted) raw: state, vertex buffer, then (after citro3d's pending updates) the matrix
     * as float24 and the draw */
    apply_state(state, 1, 1);
    raw_vertex_buffer(ms->verts_pa, 0x210, RAW_BUF_FLAGS1(16, 3, 0));
    raw_sync();
    if (!g.last_mesh_valid || !words_equal(g.last_clip, matrix->m, 16)) {
      memcpy(g.last_clip, matrix->m, sizeof(g.last_clip));
      raw_end(raw_unif(raw_ptr(), g.uloc_clip, matrix->f24, 12, 0));
      g.last_mesh_valid = 1;
    }
    raw_draw_elements(ms->indices_pa + 2u * (u32)first_index, (u32)index_count);
  } else {
    raw_vc_flush();
    /* consecutive draws of a level chunk share the matrix and the vertex buffer */
    if (!g.last_mesh_valid || !words_equal(g.last_clip, matrix->m, 16)) {
      memcpy(g.last_clip, matrix->m, sizeof(g.last_clip));
      C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g.uloc_clip, (const C3D_Mtx*)matrix->m);
    }
    g.last_mesh_valid = 1;
    c3d_vertex_buffer(ms->verts_pa, ms->verts, 16, 3, 0x210);
    apply_state(state, 1, 0);
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

void ctr_gpu_mesh_flush(int mesh, int offset, int size) {
  if (mesh < 0 || mesh >= MAX_MESHES || g.meshes[mesh].used != 1 || size <= 0) {
    return;
  }
  cache_flush_frame((uint8_t*)g.meshes[mesh].verts + offset, (u32)size);
}

/* (AI-assisted) clip (row-major, GL clip space) -> PICA clip space, in C3D_Mtx layout */
static void skin_clip_matrix(const float clip[16], C3D_Mtx* m) {
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

void ctr_gpu_draw_skinned(const ctr_draw_state* state, const float clip[16], const float* bones,
                          int palette_count, const float lights[28], int mesh, int first_index,
                          int index_count) {
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
  const int same_lights = g.last_skin_valid && words_equal(g.last_skin_lights, lights, 28);
  if (!same_clip) {
    C3D_Mtx m;
    skin_clip_matrix(clip, &m);
    if (raw) {
      raw_end(raw_unif(raw_ptr(), g.uloc_skin_clip, (const u32*)&m, 16, 1));
    } else {
      C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g.uloc_skin_clip, &m);
    }
    memcpy(g.last_skin_clip, clip, sizeof(g.last_skin_clip));
  }
  if (palette_count > CTR_MAX_PALETTE) {
    palette_count = CTR_MAX_PALETTE;
  }
  /* bones: the draws of a model with at most CTR_MAX_PALETTE bones all use the same palette */
  const size_t bone_bytes = sizeof(float) * 12 * (size_t)palette_count;
  if (!g.last_skin_valid || g.last_skin_palette_count != palette_count ||
      !words_equal(g.last_skin_bones, bones, 12u * (unsigned)palette_count)) {
    if (raw && palette_count > 0) {
      raw_skin_rows(g.uloc_skin_rows, bones, palette_count);
    } else {
      for (int row = 0; row < 3; row++) {
        C3D_FVec* dst =
            C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, g.uloc_skin_rows[row], palette_count);
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
    memcpy(g.last_skin_bones, bones, bone_bytes);
  }
  if (!same_lights) {
    if (raw) {
      u32 lv[28];
      const u32* l = (const u32*)lights;
      for (int i = 0; i < 7; i++) {
        lv[4 * i] = l[4 * i + 3];
        lv[4 * i + 1] = l[4 * i + 2];
        lv[4 * i + 2] = l[4 * i + 1];
        lv[4 * i + 3] = l[4 * i];
      }
      raw_end(raw_unif(raw_ptr(), g.uloc_skin_lights, lv, 28, 1));
    } else {
      C3D_FVec* lv = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, g.uloc_skin_lights, 7);
      for (int i = 0; i < 7; i++) {
        lv[i].x = lights[4 * i];
        lv[i].y = lights[4 * i + 1];
        lv[i].z = lights[4 * i + 2];
        lv[i].w = lights[4 * i + 3];
      }
    }
    memcpy(g.last_skin_lights, lights, sizeof(g.last_skin_lights));
  }
  g.last_skin_valid = 1;
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
                              int palette_count, const float fade[4], int mesh, int first_index,
                              int index_count) {
  if (!g.ready || !g.in_frame || mesh < 0 || mesh >= MAX_MESHES || g.meshes[mesh].used != 1 ||
      index_count < 3) {
    return;
  }
  if (!cmd_room()) {
    return;
  }
  /* (rare: envmapped effects only, so no upload caching like ctr_gpu_draw_skinned) */
  raw_vc_flush();
  use_program(PROG_SKIN_ENV);
  C3D_Mtx m;
  skin_clip_matrix(clip, &m);
  C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g.uloc_env_clip, &m);
  if (palette_count > CTR_MAX_PALETTE) {
    palette_count = CTR_MAX_PALETTE;
  }
  for (int row = 0; row < 3; row++) {
    C3D_FVec* dst = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, g.uloc_env_rows[row], palette_count);
    for (int p = 0; p < palette_count; p++) {
      const float* src = bones + 12 * p + 4 * row;
      dst[p].x = src[0];
      dst[p].y = src[1];
      dst[p].z = src[2];
      dst[p].w = src[3];
    }
  }
  /* the texture stage multiplies merc colors by 4: half the fade gives texture * fade * 2 */
  C3D_FVUnifSet(GPU_VERTEX_SHADER, g.uloc_env_fade, fade[0] * 0.5f, fade[1] * 0.5f,
                fade[2] * 0.5f, 0.5f);
  apply_state_tint(state, 2, 0xffffffffu, 0);
  c3d_vertex_buffer(g.meshes[mesh].verts_pa, g.meshes[mesh].verts, 24, 6, 0x543210);
  C3D_DrawElements(GPU_TRIANGLES, index_count, C3D_UNSIGNED_SHORT,
                   g.meshes[mesh].indices + first_index);
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
  for (;;) {
    LightEvent_Wait(&as.start);
    if (as.quit) {
      break;
    }
    as.fn(as.arg);
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

void ctr_gpu_set_mesh_fog(const float fog0[4], const float fog1[4], uint8_t r, uint8_t gr,
                          uint8_t b) {
  memcpy(g.fog0, fog0, sizeof(g.fog0));
  memcpy(g.fog1, fog1, sizeof(g.fog1));
  if (g.cur_prog == PROG_MESH) {
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g.uloc_fog0, g.fog0[0], g.fog0[1], g.fog0[2], g.fog0[3]);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g.uloc_fog1, g.fog1[0], g.fog1[1], g.fog1[2], g.fog1[3]);
    raw_note_unif();
  }
  uint32_t rgb = ((uint32_t)r << 16) | ((uint32_t)gr << 8) | b;
  if (!g.fog_tex_valid || rgb == g.fog_tex_rgb) {
    return;
  }
  /* the GPU may still read the old texture from the previous frame: that frame is finished
   * when this is called (after C3D_FrameBegin), so writing it here is safe. (AI-assisted) Not with
   * the pipeline (the frame before is still on the GPU): write a second copy then, swapped once
   * per frame. */
  g.fog_tex_rgb = rgb;
  if (s_pipeline) {
    static void* fog_alt = NULL;
    static int fog_alt_frame = -1;
    const u32 total = C3D_TexCalcTotalSize(g.fog_tex.size, g.fog_tex.maxLevel);
    if (!fog_alt) {
      ctr_linear_lock();
      fog_alt = linearAlloc(total);
      ctr_linear_unlock();
      if (fog_alt) {
        memcpy(fog_alt, g.fog_tex.data, total);
      }
    }
    if (fog_alt && fog_alt_frame != g.frame_no) {
      void* t = g.fog_tex.data;
      g.fog_tex.data = fog_alt;
      fog_alt = t;
      fog_alt_frame = g.frame_no;
    }
  }
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
