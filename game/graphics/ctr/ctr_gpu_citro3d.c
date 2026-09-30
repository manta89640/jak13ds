/*
 * (AI-assisted)
 * citro3d implementation of game/graphics/ctr/ctr_gpu.h (see the conventions there).
 *
 * - top screen, 400x240, RGBA8 color + 24/8 depth-stencil, black letterbox bars
 * - one vertex shader (platform/3ds/shaders/ctr_basic.v.pica), one TEV stage per draw
 * - vertices are copied into a linear-memory ring buffer that is reset every frame
 *   (C3D_FRAME_SYNCDRAW makes sure the GPU finished the previous frame first)
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

/* from ctr_port.c: stop the console's buffer swaps once citro3d owns the screens */
void ctr_port_set_gpu_active(int active);

#define DISPLAY_TRANSFER_FLAGS                                                              \
  (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |        \
   GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
   GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

#define VBUF_BYTES (4 * 1024 * 1024)
#define MAX_TEXTURES 1024

typedef struct {
  C3D_Tex tex;
  int used;
} TexSlot;

#define MAX_MESHES 8192

typedef struct {
  void* verts;
  uint16_t* indices;
  int used; /* 0 free, 1 used, 2 pending delete */
} MeshSlot;

enum { PROG_NONE = 0, PROG_BASIC, PROG_MESH, PROG_SKIN };

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
  DVLB_s* skin_dvlb;
  shaderProgram_s skin_program;
  int uloc_skin_clip, uloc_skin_rows[3], uloc_skin_scales;
  int cur_prog;
  ctr_draw_state last_state;
  int last_state_mesh;
  uint32_t last_tint;
  int last_state_valid;
  C3D_Mtx gl_to_pica;
  MeshSlot meshes[MAX_MESHES];
  int pending_mesh_delete[MAX_MESHES];
  int pending_mesh_delete_count;
  C3D_Mtx projection;
  uint8_t* vbuf;
  size_t vbuf_used;
  TexSlot textures[MAX_TEXTURES];
  ctr_gpu_stats cur, last;
  int in_frame;
  int pending_delete[MAX_TEXTURES];
  int pending_delete_count;
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

/* textures deleted during a frame may still be read by the GPU: free them at the start of the
 * next frame, after C3D_FrameBegin(C3D_FRAME_SYNCDRAW) waited for the GPU */
static void process_pending_deletes(void) {
  for (int i = 0; i < g.pending_delete_count; i++) {
    int h = g.pending_delete[i];
    if (g.textures[h].used == 2) {
      C3D_TexDelete(&g.textures[h].tex);
      g.textures[h].used = 0;
    }
  }
  g.pending_delete_count = 0;
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
}

/* Programs have different vertex layouts and uniforms: switch both together. */
static void use_program(int prog) {
  if (g.cur_prog == prog) {
    return;
  }
  C3D_AttrInfo* attr = C3D_GetAttrInfo();
  AttrInfo_Init(attr);
  if (prog == PROG_BASIC) {
    C3D_BindProgram(&g.program);
    g.last_state_valid = 0;
    AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 3);         /* position */
    AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2);         /* texcoord */
    AttrInfo_AddLoader(attr, 2, GPU_UNSIGNED_BYTE, 4); /* color */
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g.uloc_projection, &g.projection);
  } else if (prog == PROG_SKIN) {
    C3D_BindProgram(&g.skin_program);
    g.last_state_valid = 0;
    AttrInfo_AddLoader(attr, 0, GPU_SHORT, 3);         /* position */
    AttrInfo_AddLoader(attr, 1, GPU_UNSIGNED_BYTE, 3); /* bone indices */
    AttrInfo_AddLoader(attr, 2, GPU_UNSIGNED_BYTE, 3); /* weights */
    AttrInfo_AddLoader(attr, 3, GPU_SHORT, 2);         /* texcoord * 1024 */
    AttrInfo_AddLoader(attr, 4, GPU_UNSIGNED_BYTE, 4); /* color */
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g.uloc_skin_scales, 1.0f / 1024.0f, 1.0f / 255.0f, 1.0f,
                  0.0f);
  } else {
    C3D_BindProgram(&g.mesh_program);
    g.last_state_valid = 0;
    AttrInfo_AddLoader(attr, 0, GPU_SHORT, 4);         /* position (+ pad) */
    AttrInfo_AddLoader(attr, 1, GPU_SHORT, 2);         /* texcoord * 1024 */
    AttrInfo_AddLoader(attr, 2, GPU_UNSIGNED_BYTE, 4); /* color */
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g.uloc_scales, 1.0f / 1024.0f, 1.0f / 255.0f, 1.0f, 0.0f);
  }
  g.cur_prog = prog;
}

static void setup_projection(void) {
  /* x in [-1.25, 1.25] covers the 400 pixel width, so [-1, 1] is a centered 320x240 4:3 area */
  Mtx_OrthoTilt(&g.projection, -1.25f, 1.25f, -1.0f, 1.0f, 0.0f, 1.0f, true);
  /* depth: clip z = -z, so the stored depth (-z / w) is our z (GS: larger = closer) */
  g.projection.r[2].x = 0.0f;
  g.projection.r[2].y = 0.0f;
  g.projection.r[2].z = -1.0f;
  g.projection.r[2].w = 0.0f;

  /* OpenGL-style clip space -> PICA: same x/y mapping, z' = (z - w) / 2 so the depth (-z'/w) is 1 at
   * the near plane and 0 at the far plane (larger = closer, like the GS) */
  Mtx_OrthoTilt(&g.gl_to_pica, -1.25f, 1.25f, -1.0f, 1.0f, 0.0f, 1.0f, true);
  g.gl_to_pica.r[2].x = 0.0f;
  g.gl_to_pica.r[2].y = 0.0f;
  g.gl_to_pica.r[2].z = 0.5f;
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
  if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 4)) {
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
  g.skin_dvlb = DVLB_ParseFile((u32*)ctr_skin_shbin, (u32)ctr_skin_shbin_size);
  shaderProgramInit(&g.skin_program);
  shaderProgramSetVsh(&g.skin_program, &g.skin_dvlb->DVLE[0]);
  g.uloc_skin_clip = shaderInstanceGetUniformLocation(g.skin_program.vertexShader, "clip");
  g.uloc_skin_rows[0] = shaderInstanceGetUniformLocation(g.skin_program.vertexShader, "row0");
  g.uloc_skin_rows[1] = shaderInstanceGetUniformLocation(g.skin_program.vertexShader, "row1");
  g.uloc_skin_rows[2] = shaderInstanceGetUniformLocation(g.skin_program.vertexShader, "row2");
  g.uloc_skin_scales = shaderInstanceGetUniformLocation(g.skin_program.vertexShader, "scales");
  g.cur_prog = PROG_NONE;

  g.vbuf = (uint8_t*)linearAlloc(VBUF_BYTES);
  if (!g.vbuf) {
    return -3;
  }
  setup_projection();
  use_program(PROG_BASIC);
  C3D_CullFace(GPU_CULL_NONE);
  ctr_port_set_gpu_active(1);
  g.ready = 1;
  return 0;
}

void ctr_gpu_exit(void) {
  if (!g.ready) {
    return;
  }
  for (int i = 0; i < MAX_TEXTURES; i++) {
    if (g.textures[i].used) {
      C3D_TexDelete(&g.textures[i].tex);
      g.textures[i].used = 0;
    }
  }
  for (int i = 0; i < MAX_MESHES; i++) {
    if (g.meshes[i].used) {
      linearFree(g.meshes[i].verts);
      linearFree(g.meshes[i].indices);
      g.meshes[i].used = 0;
    }
  }
  linearFree(g.vbuf);
  shaderProgramFree(&g.program);
  DVLB_Free(g.dvlb);
  shaderProgramFree(&g.mesh_program);
  DVLB_Free(g.mesh_dvlb);
  shaderProgramFree(&g.skin_program);
  DVLB_Free(g.skin_dvlb);
  C3D_RenderTargetDelete(g.top);
  C3D_Fini();
  ctr_port_set_gpu_active(0);
  g.ready = 0;
}

void ctr_gpu_frame_begin(uint8_t r, uint8_t gr, uint8_t b) {
  if (!g.ready) {
    return;
  }
  C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
  process_pending_deletes();
  if (g.screenshot_state == 2) {
    write_screenshot();
    g.screenshot_state = 0;
  }
  u32 clear = ((u32)r << 24) | ((u32)gr << 16) | ((u32)b << 8) | 0xff;
  C3D_RenderTargetClear(g.top, C3D_CLEAR_ALL, clear, 0);
  C3D_FrameDrawOn(g.top);
  g.cur_prog = PROG_NONE;
  g.last_state_valid = 0;
  use_program(PROG_BASIC);
  g.vbuf_used = 0;
  memset(&g.cur, 0, sizeof(g.cur));
  g.in_frame = 1;
}

void ctr_gpu_frame_end(void) {
  if (!g.ready || !g.in_frame) {
    return;
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
    if (g.textures[i].used) {
      count++;
      bytes += g.textures[i].tex.size;
    }
  }
  g.cur.textures = count;
  g.cur.tex_bytes = bytes;
  g.last = g.cur;
}

void ctr_gpu_wait_vblank(void) {
  gspWaitForVBlank();
}

/* ---------------- textures ---------------- */

static inline u32 morton8(u32 x, u32 y) {
  return (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) |
         ((y & 4) << 3);
}

int ctr_gpu_tex_create(int w, int h, const uint8_t* rgba) {
  if (!g.ready || w < 8 || h < 8 || w > 1024 || h > 1024) {
    return -1;
  }
  int slot = -1;
  for (int i = 0; i < MAX_TEXTURES; i++) {
    if (g.textures[i].used == 0) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    return -1;
  }
  C3D_Tex* tex = &g.textures[slot].tex;
  if (!C3D_TexInit(tex, (u16)w, (u16)h, GPU_RGBA8)) {
    return -1;
  }
  /* tiled: 8x8 tiles in rows, morton order inside a tile */
  uint8_t* dst = (uint8_t*)tex->data;
  const int tiles_x = w / 8;
  for (int y = 0; y < h; y++) {
    for (int x = 0; x < w; x++) {
      const uint8_t* p = rgba + 4 * (x + y * w);
      /* the GPU samples t = 0 from the last row in memory: store the image bottom row first */
      int ty = h - 1 - y;
      u32 tile = (u32)((ty / 8) * tiles_x + (x / 8));
      u32 off = (tile * 64 + morton8(x & 7, ty & 7)) * 4;
      /* GPU_RGBA8 is stored as A, B, G, R */
      dst[off + 0] = p[3];
      dst[off + 1] = p[2];
      dst[off + 2] = p[1];
      dst[off + 3] = p[0];
    }
  }
  C3D_TexFlush(tex);
  g.textures[slot].used = 1;
  return slot;
}

void ctr_gpu_tex_delete(int handle) {
  if (handle < 0 || handle >= MAX_TEXTURES || !g.textures[handle].used) {
    return;
  }
  g.textures[handle].used = 2; /* pending */
  g.pending_delete[g.pending_delete_count++] = handle;
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

/* The GPU command buffer is fixed size: submit what we have when it gets full (a frame with many
 * draws would overflow it, and libctru panics then). */
static void check_cmdbuf(void) {
  if (C3D_GetCmdBufUsage() > 0.85f) {
    C3D_FrameSplit(GX_CMDLIST_FLUSH);
  }
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
  }
  C3D_TexEnv* env = C3D_GetTexEnv(0);
  C3D_TexEnvInit(env);
  int textured = st->tex >= 0 && st->tex < MAX_TEXTURES && g.textures[st->tex].used == 1;
  if (textured) {
    C3D_Tex* tex = &g.textures[st->tex].tex;
    GPU_TEXTURE_FILTER_PARAM f = st->filter ? GPU_LINEAR : GPU_NEAREST;
    C3D_TexSetFilter(tex, f, f);
    C3D_TexSetWrap(tex, st->clamp_s ? GPU_CLAMP_TO_EDGE : GPU_REPEAT,
                   st->clamp_t ? GPU_CLAMP_TO_EDGE : GPU_REPEAT);
    C3D_TexBind(0, tex);
    if (mesh) {
      /* level meshes / merc: texture alpha 0xff = 1, vertex color 0x80 = 1 */
      C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
      C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
      C3D_TexEnvScale(env, C3D_RGB, GPU_TEVSCALE_2);
      C3D_TexEnvScale(env, C3D_Alpha, GPU_TEVSCALE_2);
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
    C3D_TexEnvSrc(env, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
    C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
    C3D_TexEnvScale(env, C3D_RGB, GPU_TEVSCALE_2);
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
  uint8_t* dst = g.vbuf + g.vbuf_used;
  memcpy(dst, verts, bytes);
  GSPGPU_FlushDataCache(dst, bytes);
  g.vbuf_used += (bytes + 15) & ~(size_t)15;

  check_cmdbuf();
  use_program(PROG_BASIC);
  apply_state(state, 0);
  C3D_BufInfo* buf = C3D_GetBufInfo();
  BufInfo_Init(buf);
  BufInfo_Add(buf, dst, sizeof(ctr_vertex), 3, 0x210);
  C3D_DrawArrays(GPU_TRIANGLES, 0, count);
  g.cur.draws++;
  g.cur.triangles += count / 3;
}

static void apply_state(const ctr_draw_state* st, int mesh) {
  apply_state_tint(st, mesh, 0xffffffff);
}

/* ---------------- static meshes ---------------- */

int ctr_gpu_tex_create_tiled(int w, int h, int format, const void* data, int size) {
  if (!g.ready || w < 8 || h < 8 || w > 1024 || h > 1024) {
    return -1;
  }
  int slot = -1;
  for (int i = 0; i < MAX_TEXTURES; i++) {
    if (g.textures[i].used == 0) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    return -1;
  }
  C3D_Tex* tex = &g.textures[slot].tex;
  if (!C3D_TexInit(tex, (u16)w, (u16)h, format == 0 ? GPU_RGB565 : GPU_RGBA4)) {
    return -1;
  }
  memcpy(tex->data, data, (size_t)size < tex->size ? (size_t)size : tex->size);
  C3D_TexFlush(tex);
  g.textures[slot].used = 1;
  return slot;
}

int ctr_gpu_mesh_create(const void* verts, int vertex_count, const uint16_t* indices,
                        int index_count) {
  if (!g.ready) {
    return -1;
  }
  int slot = -1;
  for (int i = 0; i < MAX_MESHES; i++) {
    if (g.meshes[i].used == 0) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    return -1;
  }
  size_t vbytes = (size_t)vertex_count * 16;
  size_t ibytes = (size_t)index_count * 2;
  void* v = linearAlloc(vbytes);
  uint16_t* ix = (uint16_t*)linearAlloc(ibytes);
  if (!v || !ix) {
    if (v) {
      linearFree(v);
    }
    if (ix) {
      linearFree(ix);
    }
    return -1;
  }
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
  if (mesh < 0 || mesh >= MAX_MESHES || g.meshes[mesh].used != 1) {
    return;
  }
  g.meshes[mesh].used = 2;
  g.pending_mesh_delete[g.pending_mesh_delete_count++] = mesh;
}

void ctr_gpu_draw_mesh(const ctr_draw_state* state, const float clip[16], int mesh,
                       int first_index, int index_count) {
  if (!g.ready || !g.in_frame || mesh < 0 || mesh >= MAX_MESHES || g.meshes[mesh].used != 1 ||
      index_count < 3) {
    return;
  }
  check_cmdbuf();
  use_program(PROG_MESH);
  /* final = gl_to_pica * clip */
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
  C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g.uloc_clip, &m);
  apply_state(state, 1);
  C3D_BufInfo* buf = C3D_GetBufInfo();
  BufInfo_Init(buf);
  BufInfo_Add(buf, g.meshes[mesh].verts, 16, 3, 0x210);
  C3D_DrawElements(GPU_TRIANGLES, index_count, C3D_UNSIGNED_SHORT,
                   g.meshes[mesh].indices + first_index);
  g.cur.draws++;
  g.cur.triangles += index_count / 3;
}

/* ---------------- skinned meshes ---------------- */

int ctr_gpu_skinned_mesh_create(const void* verts, int vertex_count, const uint16_t* indices,
                                int index_count) {
  if (!g.ready) {
    return -1;
  }
  int slot = -1;
  for (int i = 0; i < MAX_MESHES; i++) {
    if (g.meshes[i].used == 0) {
      slot = i;
      break;
    }
  }
  if (slot < 0) {
    return -1;
  }
  size_t vbytes = (size_t)vertex_count * 20;
  size_t ibytes = (size_t)index_count * 2;
  void* v = linearAlloc(vbytes);
  uint16_t* ix = (uint16_t*)linearAlloc(ibytes);
  if (!v || !ix) {
    if (v) {
      linearFree(v);
    }
    if (ix) {
      linearFree(ix);
    }
    return -1;
  }
  memcpy(v, verts, vbytes);
  memcpy(ix, indices, ibytes);
  GSPGPU_FlushDataCache(v, vbytes);
  GSPGPU_FlushDataCache(ix, ibytes);
  g.meshes[slot].verts = v;
  g.meshes[slot].indices = ix;
  g.meshes[slot].used = 1;
  return slot;
}

void ctr_gpu_draw_skinned(const ctr_draw_state* state, const float clip[16], const float* bones,
                          int palette_count, const float tint[3], int mesh, int first_index,
                          int index_count) {
  if (!g.ready || !g.in_frame || mesh < 0 || mesh >= MAX_MESHES || g.meshes[mesh].used != 1 ||
      index_count < 3) {
    return;
  }
  check_cmdbuf();
  use_program(PROG_SKIN);
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
  if (palette_count > CTR_MAX_PALETTE) {
    palette_count = CTR_MAX_PALETTE;
  }
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
  uint32_t t = 0xff000000u;
  for (int c = 0; c < 3; c++) {
    float v = tint[c] < 0.f ? 0.f : (tint[c] > 1.f ? 1.f : tint[c]);
    t |= (uint32_t)(v * 255.f) << (8 * c);
  }
  apply_state_tint(state, 2, t);
  C3D_BufInfo* buf = C3D_GetBufInfo();
  BufInfo_Init(buf);
  BufInfo_Add(buf, g.meshes[mesh].verts, 20, 5, 0x43210);
  C3D_DrawElements(GPU_TRIANGLES, index_count, C3D_UNSIGNED_SHORT,
                   g.meshes[mesh].indices + first_index);
  g.cur.draws++;
  g.cur.triangles += index_count / 3;
}

void ctr_gpu_get_stats(ctr_gpu_stats* out) {
  *out = g.last;
}
