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

/* from ctr_port.c: stop the console's buffer swaps once citro3d owns the screens */
void ctr_port_set_gpu_active(int active);
void ctr_linear_lock(void);
void ctr_linear_unlock(void);

#define DISPLAY_TRANSFER_FLAGS                                                              \
  (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |        \
   GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
   GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

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
  uint8_t levels;
} TexSlot;

typedef struct {
  int used;           /* 0 free, 1 used, 2 pending delete, 3 reserved (being filled) */
  int ready;          /* all texels written (ctr_gpu_pool_ready) */
  uint8_t* linear;    /* the texels (always kept: moving out of VRAM is free) */
  void* vram;         /* the VRAM copy the GPU reads, or NULL */
  unsigned int bytes;
  int priority;
  int no_room_frame;  /* last frame it didn't fit in VRAM (don't try again every frame) */
} TexPool;

#define MAX_MESHES 8192

typedef struct {
  void* verts;
  uint16_t* indices;
  int used; /* 0 free, 1 used, 2 pending delete */
} MeshSlot;

enum { PROG_NONE = 0, PROG_BASIC, PROG_MESH, PROG_SKIN, PROG_SKIN_ENV, PROG_CLIP };

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
  int cur_prog;
  ctr_draw_state last_state;
  int last_state_mesh;
  uint32_t last_tint;
  int last_state_valid;
  /* last ctr_gpu_draw_mesh: mesh and matrix (invalidated by any other program use) */
  int last_mesh_valid;
  int last_mesh;
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

/* The previous frame is still in the render target's color buffer (VRAM, tiled RGBA8, 240x400
 * rotated) at the start of the next frame, after the GPU finished it. Write it as a BMP. */
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
      uint32_t o = (tile * 64 + (((fx & 1)) | ((fy & 1) << 1) | ((fx & 2) << 1) | ((fy & 2) << 2) |
                                 ((fx & 4) << 2) | ((fy & 4) << 3))) *
                   4;
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
static void process_pending_deletes(void) {
  ctr_linear_lock();
  for (int i = 0; i < g.pending_staging_count; i++) {
    linearFree(g.pending_staging[i]);
  }
  g.pending_staging_count = 0;
  for (int i = 0; i < g.pending_delete_count; i++) {
    int h = g.pending_delete[i];
    if (g.textures[h].used == 2) {
      if (g.textures[h].pool < 0) {
        C3D_TexDelete(&g.textures[h].tex);
      }
      g.textures[h].used = 0;
      g.textures[h].pool = -1;
    }
  }
  g.pending_delete_count = 0;
  for (int i = 0; i < MAX_POOLS; i++) {
    TexPool* p = &g.pools[i];
    if (p->used == 2) {
      if (p->vram) {
        vramFree(p->vram);
      }
      linearFree(p->linear);
      memset(p, 0, sizeof(*p));
    }
  }
  for (int i = 0; i < g.pending_mesh_delete_count; i++) {
    MeshSlot* m = &g.meshes[g.pending_mesh_delete[i]];
    if (m->used == 2) {
      linearFree(m->verts);
      linearFree(m->indices);
      m->verts = NULL;
      m->indices = NULL;
      m->used = 0;
    }
  }
  g.pending_mesh_delete_count = 0;
  ctr_linear_unlock();
}

/* Programs have different vertex layouts and uniforms: switch both together. */
static void use_program(int prog) {
  if (g.cur_prog == prog) {
    return;
  }
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
  g.top = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
  if (!g.top) {
    return -2;
  }
  C3D_RenderTargetSetOutput(g.top, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

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
      if (g.textures[i].pool < 0) {
        C3D_TexDelete(&g.textures[i].tex);
      }
      g.textures[i].used = 0;
    }
  }
  for (int i = 0; i < MAX_POOLS; i++) {
    if (g.pools[i].used) {
      if (g.pools[i].vram) {
        vramFree(g.pools[i].vram);
      }
      linearFree(g.pools[i].linear);
      memset(&g.pools[i], 0, sizeof(g.pools[i]));
    }
  }
  linearFree(g.quad_indices);
  for (int i = 0; i < MAX_MESHES; i++) {
    if (g.meshes[i].used) {
      linearFree(g.meshes[i].verts);
      linearFree(g.meshes[i].indices);
      g.meshes[i].used = 0;
    }
  }
  linearFree(g.vbuf);
  ctr_linear_unlock();
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

void ctr_gpu_frame_begin(uint8_t r, uint8_t gr, uint8_t b) {
  if (!g.ready) {
    return;
  }
  C3D_FrameBegin(0); /* waits for the GPU; the game thread already paces to vblank */
  g.cmd_base = gpuCmdBuf;
  g.frame_no++;
  process_pending_deletes();
  /* before any draw: copies to VRAM are queued ahead of this frame's draws */
  update_pool_residency();
  if (g.screenshot_state == 2) {
    write_screenshot();
    g.screenshot_state = 0;
  }
  u32 clear = ((u32)r << 24) | ((u32)gr << 16) | ((u32)b << 8) | 0xff;
  C3D_RenderTargetClear(g.top, C3D_CLEAR_ALL, clear, 0);
  C3D_FrameDrawOn(g.top);
  g.cur_prog = PROG_NONE;
  g.last_state_valid = 0;
  g.bound_tex = NULL;
  use_program(PROG_BASIC);
  /* the fog ramp stays on texture unit 1 for the whole frame (binding a texture clears the GPU's
   * texture cache: once per frame, not at every level draw) */
  if (g.fog_tex_valid) {
    C3D_TexBind(1, &g.fog_tex);
  }
  g.vbuf_used = 0;
  g.vbuf_flushed = 0;
  memset(&g.cur, 0, sizeof(g.cur));
  g.in_frame = 1;
}

/* Immediate draws copy their vertices into g.vbuf. The GPU reads them only when the frame's GX
 * queue runs (C3D_FrameEnd; C3D_FrameBegin stopped the queue, so command list splits wait in it
 * until then), so one cache flush of what this frame wrote is enough. Each flush is an IPC to the
 * GSP service: one per draw cost tens of microseconds each on hardware (free in Azahar). */
static void flush_vbuf(void) {
  if (g.vbuf_used > g.vbuf_flushed) {
    GSPGPU_FlushDataCache(g.vbuf + g.vbuf_flushed, (u32)(g.vbuf_used - g.vbuf_flushed));
    g.vbuf_flushed = g.vbuf_used;
  }
}

/* (AI-assisted) C3D_SyncTextureCopy in a frame splits the command list with C3D_FrameSplit(0): that
 * part of the command list is NOT flushed from the CPU cache. C3D_FrameEnd(0) used to flush the
 * whole linear heap, which covered it; ctr_gpu_frame_end only flushes the last part
 * (GX_CMDLIST_FLUSH), so the GPU could run stale command words of the earlier part (wrong blend
 * and texture state, broken draws) in every frame with a copy (the distorter's screen copy, a
 * texture pool moving to VRAM). Split here with the flush first; C3D_SyncTextureCopy's own split
 * then has nothing left to send. */
static void sync_texture_copy(u32* in, u32 indim, u32* out, u32 outdim, u32 size, u32 flags) {
  if (g.in_frame) {
    C3D_FrameSplit(GX_CMDLIST_FLUSH);
  }
  C3D_SyncTextureCopy(in, indim, out, outdim, size, flags);
}

void ctr_gpu_frame_end(void) {
  if (!g.ready || !g.in_frame) {
    return;
  }
  flush_vbuf();
  /* the command words of the whole frame (splits move gpuCmdBuf on; C3D_FrameBegin restarts it) */
  g.cur.cmd_bytes = (unsigned int)((gpuCmdBuf + gpuCmdBufOffset - g.cmd_base) * 4);
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
    if (g.textures[i].used) {
      count++;
      bytes += g.textures[i].tex.size;
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
        g.cur.pool_vram_bytes += p->bytes;
      }
    }
  }
  g.last = g.cur;
}

void ctr_gpu_wait_vblank(void) {
  /* Like a swap with vsync: only wait if no vertical blank happened since the last call. A frame
   * that took longer than 16.7 ms goes on screen at the next vblank anyway (citro3d swaps the
   * screen buffers there), so waiting for another one only loses time. */
  static u32 last_count;
  if (C3D_FrameCounter(0) == last_count) {
    gspWaitForVBlank();
  }
  last_count = C3D_FrameCounter(0);
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

void ctr_gpu_set_vram_textures(int on) {
  g_vram_textures = on;
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
  GSPGPU_FlushDataCache(p->linear, p->bytes);
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

/* point the pool's textures at its linear memory or its VRAM copy */
static void pool_repoint(int pool) {
  TexPool* p = &g.pools[pool];
  uint8_t* base = p->vram ? (uint8_t*)p->vram : p->linear;
  for (int i = 0; i < MAX_TEXTURES; i++) {
    TexSlot* t = &g.textures[i];
    if (t->used && t->pool == pool) {
      t->tex.data = base + t->offset;
    }
  }
  g.bound_tex = NULL;
}

static void pool_leave_vram(int pool) {
  TexPool* p = &g.pools[pool];
  if (!p->vram) {
    return;
  }
  void* v = p->vram;
  p->vram = NULL;
  pool_repoint(pool);
  /* at frame begin: the GPU finished the frames that read it, and this frame reads the linear
   * copy from now on */
  vramFree(v);
}

/* At frame begin, before any draw: copy the pools that want to be in VRAM there, most wanted
 * first, moving pools with a lower priority out if that makes room. The copies are queued ahead of
 * this frame's draws, which read the VRAM copy. */
static void update_pool_residency(void) {
  if (!g_vram_textures) {
    for (int i = 0; i < MAX_POOLS; i++) {
      if (g.pools[i].used == 1 && g.pools[i].vram) {
        pool_leave_vram(i);
      }
    }
    return;
  }
  int copies = 0;
  int tried[MAX_POOLS] = {0};
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
    while (!v) {
      /* move out the resident pool with the lowest priority below this one's */
      int victim = -1;
      for (int i = 0; i < MAX_POOLS; i++) {
        const TexPool* q = &g.pools[i];
        if (q->used == 1 && q->vram && q->priority < p->priority &&
            (victim < 0 || q->priority < g.pools[victim].priority)) {
          victim = i;
        }
      }
      if (victim < 0) {
        break;
      }
      pool_leave_vram(victim);
      v = vramAlloc(p->bytes);
    }
    ctr_linear_unlock();
    if (!v) {
      p->no_room_frame = g.frame_no;
      continue;
    }
    /* one GPU copy of the whole pool (in a frame: queued, runs before the draws) */
    sync_texture_copy((u32*)p->linear, 0, (u32*)v, 0, p->bytes, 8);
    p->vram = v;
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
  if (!result && C3D_TexInit(tex, (u16)w, (u16)h, fmt)) {
    result = tex->data;
  }
  ctr_linear_unlock();
  return result;
}

static int tex_commit(C3D_Tex* tex, void* buf, int on_vram) {
  if (!on_vram) {
    C3D_TexFlush(tex);
    return 1;
  }
  GSPGPU_FlushDataCache(buf, tex->size);
  sync_texture_copy((u32*)buf, 0, (u32*)tex->data, 0, tex->size, 8);
  /* check that the copy landed (VRAM is readable by the CPU); if not, use linear memory */
  int ok = memcmp(tex->data, buf, tex->size) == 0;
  if (!ok) {
    g.vram_copy_failures++;
    GPU_TEXCOLOR fmt = tex->fmt;
    u16 w = tex->width, h = tex->height;
    ctr_linear_lock();
    C3D_TexDelete(tex);
    if (!C3D_TexInit(tex, w, h, fmt)) {
      linearFree(buf);
      ctr_linear_unlock();
      return 0;
    }
    ctr_linear_unlock();
    memcpy(tex->data, buf, tex->size);
    C3D_TexFlush(tex);
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
  const bool ok = C3D_TexInitMipmap(tex, (u16)w, (u16)h, GPU_RGBA8);
  ctr_linear_unlock();
  if (!ok) {
    release_tex_slot(slot);
    return -1;
  }
  swizzle_rgba8((uint8_t*)tex->data, w, h, rgba);
  C3D_TexGenerateMipmap(tex, GPU_TEXFACE_2D); /* 2x2 averages, on the tiled data */
  C3D_TexFlush(tex);
  g.textures[slot].pool = -1;
  g.textures[slot].levels = (uint8_t)(tex->maxLevel + 1);
  g.textures[slot].used = 1;
  return slot;
}

void ctr_gpu_tex_update(int handle, const uint8_t* rgba) {
  if (handle < 0 || handle >= MAX_TEXTURES || g.textures[handle].used != 1 ||
      g.textures[handle].pool >= 0) {
    return;
  }
  C3D_Tex* tex = &g.textures[handle].tex;
  const u32 addr = (u32)tex->data;
  if (tex->fmt != GPU_RGBA8 || (addr >= OS_VRAM_VADDR && addr < OS_VRAM_VADDR + OS_VRAM_SIZE)) {
    return;
  }
  swizzle_rgba8((uint8_t*)tex->data, tex->width, tex->height, rgba);
  if (tex->maxLevel > 0) {
    C3D_TexGenerateMipmap(tex, GPU_TEXFACE_2D);
  }
  C3D_TexFlush(tex);
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

static void apply_state(const ctr_draw_state* st, int mesh);

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
static void bind_texture(TexSlot* slot, const ctr_draw_state* st) {
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
  C3D_TexBind(0, tex);
  g.bound_tex = tex;
  g.bound_data = tex->data;
  g.bound_param = param;
  g.cur.tex_binds++;
}

/* mesh: 0 = immediate draws, 1 = level mesh, 2 = skinned mesh (tint = constant color stage) */
static void apply_state_tint(const ctr_draw_state* st, int mesh, uint32_t tint) {
  if (g.last_state_valid && g.last_state_mesh == mesh && g.last_tint == tint &&
      !memcmp(&g.last_state, st, sizeof(*st))) {
    return;
  }
  g.last_state = *st;
  g.last_state_mesh = mesh;
  g.last_tint = tint;
  g.last_state_valid = 1;
  /* stage 1: lighting tint for skinned meshes, pass-through otherwise */
  C3D_TexEnv* env1 = C3D_GetTexEnv(1);
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
  }
  C3D_TexEnv* env = C3D_GetTexEnv(0);
  C3D_TexEnvInit(env);
  int textured = st->tex >= 0 && st->tex < MAX_TEXTURES && g.textures[st->tex].used == 1;
  if (textured) {
    bind_texture(&g.textures[st->tex], st);
    if (mesh) {
      /* level meshes / merc: texture alpha 0xff = 1, vertex color 0x80 = 1 (merc: the skin shader
       * outputs half the lit color, so x4) */
      C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
      C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
      C3D_TexEnvScale(env, C3D_RGB, mesh == 2 ? GPU_TEVSCALE_4 : GPU_TEVSCALE_2);
      C3D_TexEnvScale(env, C3D_Alpha, GPU_TEVSCALE_2);
      if (st->decal) {
        /* decal: the texture color alone (alpha as above) */
        C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, 0, 0);
        C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
        C3D_TexEnvScale(env, C3D_RGB, GPU_TEVSCALE_1);
      }
    } else if (st->decal) {
      C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, 0, 0);
      C3D_TexEnvFunc(env, C3D_RGB, GPU_REPLACE);
      C3D_TexEnvScale(env, C3D_RGB, GPU_TEVSCALE_1);
      C3D_TexEnvSrc(env, C3D_Alpha, st->tcc ? GPU_TEXTURE0 : GPU_PRIMARY_COLOR, 0, 0);
      C3D_TexEnvFunc(env, C3D_Alpha, GPU_REPLACE);
      C3D_TexEnvScale(env, C3D_Alpha, GPU_TEVSCALE_2);
    } else {
      /* GS modulate: tex * vertex / 128, i.e. 2x with 0..1 colors */
      C3D_TexEnvSrc(env, C3D_RGB, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
      C3D_TexEnvFunc(env, C3D_RGB, GPU_MODULATE);
      C3D_TexEnvScale(env, C3D_RGB, GPU_TEVSCALE_2);
      if (st->tcc) {
        C3D_TexEnvSrc(env, C3D_Alpha, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
        C3D_TexEnvFunc(env, C3D_Alpha, GPU_MODULATE);
        C3D_TexEnvScale(env, C3D_Alpha, GPU_TEVSCALE_4);
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

  switch (st->blend) {
    case CTR_BLEND_ALPHA:
      C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE,
                     GPU_ZERO);
      break;
    case CTR_BLEND_ADD:
      C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE, GPU_ONE, GPU_ZERO);
      break;
    case CTR_BLEND_SUB:
      C3D_AlphaBlend(GPU_BLEND_REVERSE_SUBTRACT, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE, GPU_ONE,
                     GPU_ZERO);
      break;
    case CTR_BLEND_FIX: {
      u32 fa = st->fix >= 0x80 ? 255 : st->fix * 2;
      C3D_BlendingColor(fa << 24);
      C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_CONSTANT_ALPHA,
                     GPU_ONE_MINUS_CONSTANT_ALPHA, GPU_ONE, GPU_ZERO);
    } break;
    case CTR_BLEND_ADD_DST_A:
      C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_DST_ALPHA, GPU_ONE, GPU_ONE, GPU_ZERO);
      break;
    case CTR_BLEND_ONE_ONE:
      C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ZERO);
      break;
    default:
      C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
      break;
  }

  if (st->atest == CTR_TEST_ALWAYS) {
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
  } else {
    int ref = st->aref * 2;
    C3D_AlphaTest(true, map_test(st->atest), ref > 255 ? 255 : ref);
  }

  if (st->ztest == CTR_TEST_ALWAYS && !st->zwrite) {
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
  } else {
    C3D_DepthTest(true, map_test(st->ztest),
                  st->zwrite ? GPU_WRITE_ALL : GPU_WRITE_COLOR);
  }
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

  use_program(PROG_BASIC);
  apply_state(state, 0);
  C3D_BufInfo* buf = C3D_GetBufInfo();
  BufInfo_Init(buf);
  BufInfo_Add(buf, dst, sizeof(ctr_vertex), 3, 0x210);
  C3D_DrawArrays(GPU_TRIANGLES, 0, count);
  g.cur.draws++;
  g.cur.triangles += count / 3;
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
  use_program(PROG_CLIP);
  apply_state(state, 0);
  C3D_BufInfo* buf = C3D_GetBufInfo();
  BufInfo_Init(buf);
  BufInfo_Add(buf, dst, sizeof(ctr_clip_vertex), 3, 0x210);
  C3D_DrawArrays(GPU_TRIANGLES, 0, count);
  g.cur.draws++;
  g.cur.triangles += count / 3;
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
    apply_state(state, 0);
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, dst, sizeof(ctr_vertex), 3, 0x210);
    /* indices are relative to the buffer: 0 1 3 3 1 2, + 4 per quad */
    C3D_DrawElements(GPU_TRIANGLES, n * 6, C3D_UNSIGNED_SHORT, g.quad_indices);
    g.cur.draws++;
    g.cur.triangles += n * 2;
    verts += 4 * n;
    quad_count -= n;
  }
}

static void apply_state(const ctr_draw_state* st, int mesh) {
  apply_state_tint(st, mesh, 0xffffffff);
}

/* ---------------- the screen as a texture ---------------- */

int ctr_gpu_copy_screen(void) {
  if (!g.ready || !g.in_frame) {
    return -1;
  }
  if (g.screen_tex < 0) {
    /* 256x512 RGBA8 in linear memory (512 KB, made once): the 240x400 color buffer fits, with the
     * same tiled layout (see write_screenshot) */
    const int slot = reserve_tex_slot();
    if (slot < 0) {
      return -1;
    }
    TexSlot* t = &g.textures[slot];
    ctr_linear_lock();
    const bool ok = C3D_TexInit(&t->tex, 256, 512, GPU_RGBA8);
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
   * the framebuffer): 50 rows of 8x8 tiles, 30 tiles (7680 bytes) each in the color buffer, 32 in
   * the texture (a 512 byte gap). In 16 byte units. */
  sync_texture_copy((u32*)g.top->frameBuf.colorBuf, GX_BUFFER_DIM(7680 / 16, 0),
                      (u32*)g.textures[g.screen_tex].tex.data, GX_BUFFER_DIM(7680 / 16, 512 / 16),
                      7680 * 50, 8);
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
  GSPGPU_FlushDataCache(v, vbytes);
  GSPGPU_FlushDataCache(ix, ibytes);
  g.meshes[slot].verts = v;
  g.meshes[slot].indices = ix;
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
  _Static_assert(sizeof(C3D_Mtx) == sizeof(ctr_mesh_matrix), "matrix layout");
  memcpy(out, &m, sizeof(m));
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
  /* consecutive draws of a level chunk share the matrix and the vertex buffer */
  if (!g.last_mesh_valid || memcmp(g.last_clip, matrix->m, sizeof(g.last_clip))) {
    memcpy(g.last_clip, matrix->m, sizeof(g.last_clip));
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g.uloc_clip, (const C3D_Mtx*)matrix);
  }
  if (!g.last_mesh_valid || g.last_mesh != mesh) {
    g.last_mesh = mesh;
    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, g.meshes[mesh].verts, 16, 3, 0x210);
  }
  g.last_mesh_valid = 1;
  apply_state(state, 1);
  C3D_DrawElements(GPU_TRIANGLES, index_count, C3D_UNSIGNED_SHORT,
                   g.meshes[mesh].indices + first_index);
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
  GSPGPU_FlushDataCache(v, vbytes);
  GSPGPU_FlushDataCache(ix, ibytes);
  g.meshes[slot].verts = v;
  g.meshes[slot].indices = ix;
  g.meshes[slot].used = 1;
  return slot;
}

void* ctr_gpu_mesh_vertices(int mesh) {
  if (mesh < 0 || mesh >= MAX_MESHES || g.meshes[mesh].used != 1) {
    return NULL;
  }
  return g.meshes[mesh].verts;
}

void ctr_gpu_mesh_flush(int mesh, int offset, int size) {
  if (mesh < 0 || mesh >= MAX_MESHES || g.meshes[mesh].used != 1 || size <= 0) {
    return;
  }
  GSPGPU_FlushDataCache((uint8_t*)g.meshes[mesh].verts + offset, (u32)size);
}

void ctr_gpu_draw_skinned(const ctr_draw_state* state, const float clip[16], const float* bones,
                          int palette_count, const float lights[28], int mesh, int first_index,
                          int index_count) {
  if (!g.ready || !g.in_frame || mesh < 0 || mesh >= MAX_MESHES || g.meshes[mesh].used != 1 ||
      index_count < 3) {
    return;
  }
  if (!cmd_room()) {
    return;
  }
  use_program(PROG_SKIN);
  /* matrix and lights: uploaded when they change (the draws of a model share them) */
  const int same_clip = g.last_skin_valid && !memcmp(g.last_skin_clip, clip, sizeof(g.last_skin_clip));
  const int same_lights =
      g.last_skin_valid && !memcmp(g.last_skin_lights, lights, sizeof(g.last_skin_lights));
  if (!same_clip) {
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
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g.uloc_skin_clip, &m);
    memcpy(g.last_skin_clip, clip, sizeof(g.last_skin_clip));
  }
  if (palette_count > CTR_MAX_PALETTE) {
    palette_count = CTR_MAX_PALETTE;
  }
  /* bones: the draws of a model with at most CTR_MAX_PALETTE bones all use the same palette */
  const size_t bone_bytes = sizeof(float) * 12 * (size_t)palette_count;
  if (!g.last_skin_valid || g.last_skin_palette_count != palette_count ||
      memcmp(g.last_skin_bones, bones, bone_bytes)) {
    for (int row = 0; row < 3; row++) {
      C3D_FVec* dst = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, g.uloc_skin_rows[row], palette_count);
      for (int p = 0; p < palette_count; p++) {
        const float* src = bones + 12 * p + 4 * row;
        dst[p].x = src[0];
        dst[p].y = src[1];
        dst[p].z = src[2];
        dst[p].w = src[3];
      }
    }
    g.last_skin_palette_count = palette_count;
    memcpy(g.last_skin_bones, bones, bone_bytes);
  }
  if (!same_lights) {
    C3D_FVec* lv = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, g.uloc_skin_lights, 7);
    for (int i = 0; i < 7; i++) {
      lv[i].x = lights[4 * i];
      lv[i].y = lights[4 * i + 1];
      lv[i].z = lights[4 * i + 2];
      lv[i].w = lights[4 * i + 3];
    }
    memcpy(g.last_skin_lights, lights, sizeof(g.last_skin_lights));
  }
  g.last_skin_valid = 1;
  apply_state_tint(state, 2, 0xffffffffu);
  C3D_BufInfo* buf = C3D_GetBufInfo();
  BufInfo_Init(buf);
  BufInfo_Add(buf, g.meshes[mesh].verts, 24, 6, 0x543210);
  C3D_DrawElements(GPU_TRIANGLES, index_count, C3D_UNSIGNED_SHORT,
                   g.meshes[mesh].indices + first_index);
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
  use_program(PROG_SKIN_ENV);
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
  apply_state_tint(state, 2, 0xffffffffu);
  C3D_BufInfo* buf = C3D_GetBufInfo();
  BufInfo_Init(buf);
  BufInfo_Add(buf, g.meshes[mesh].verts, 24, 6, 0x543210);
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
  }
  uint32_t rgb = ((uint32_t)r << 16) | ((uint32_t)gr << 8) | b;
  if (!g.fog_tex_valid || rgb == g.fog_tex_rgb) {
    return;
  }
  /* the GPU may still read the old texture from the previous frame: that frame is finished
   * when this is called (after C3D_FrameBegin), so writing it here is safe */
  g.fog_tex_rgb = rgb;
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
  C3D_TexFlush(&g.fog_tex);
  if (g.in_frame) {
    C3D_TexBind(1, &g.fog_tex); /* clears the texture cache: the GPU must not use the old texels */
  }
  g.last_state_valid = 0;
}
