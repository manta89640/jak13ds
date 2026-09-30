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

#define VBUF_BYTES (1536 * 1024)
#define MAX_STAGING 1024
#define MAX_TEXTURES 2048

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
  int uloc_fog0, uloc_fog1;
  float fog0[4], fog1[4]; /* see ctr_gpu_set_mesh_fog */
  C3D_Tex fog_tex;        /* 64x8: alpha ramp 0..1 along s, fog color */
  uint32_t fog_tex_rgb;
  int fog_tex_valid;
  DVLB_s* skin_dvlb;
  shaderProgram_s skin_program;
  int uloc_skin_clip, uloc_skin_rows[3], uloc_skin_scales, uloc_skin_lights;
  int cur_prog;
  ctr_draw_state last_state;
  int last_state_mesh;
  uint32_t last_tint;
  int last_state_valid;
  /* last ctr_gpu_draw_mesh: mesh and matrix (invalidated by any other program use) */
  int last_mesh_valid;
  int last_mesh;
  float last_clip[16];
  C3D_Mtx gl_to_pica;
  MeshSlot meshes[MAX_MESHES];
  int pending_mesh_delete[MAX_MESHES];
  int pending_mesh_delete_count;
  C3D_Mtx projection;
  uint8_t* vbuf;
  size_t vbuf_used;
  size_t vbuf_flushed; /* vbuf bytes already flushed from the CPU cache (flush_vbuf) */
  TexSlot textures[MAX_TEXTURES];
  ctr_gpu_stats cur, last;
  int in_frame;
  int pending_delete[MAX_TEXTURES];
  int pending_delete_count;
  void* pending_staging[MAX_STAGING]; /* linear buffers of queued VRAM texture copies */
  int pending_staging_count;
  int vram_textures;
  int vram_copy_failures;
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
  for (int i = 0; i < g.pending_staging_count; i++) {
    linearFree(g.pending_staging[i]);
  }
  g.pending_staging_count = 0;
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
  g.last_mesh_valid = 0;
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
    AttrInfo_AddLoader(attr, 5, GPU_BYTE, 3);          /* normal * 127 */
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g.uloc_skin_scales, 1.0f / 1024.0f, 1.0f / 255.0f, 1.0f,
                  1.0f / 127.0f);
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
  /* x in [-1.25, 1.25] covers the 400 pixel width, so [-1, 1] is a centered 320x240 4:3 area */
  Mtx_OrthoTilt(&g.projection, -1.25f, 1.25f, -1.0f, 1.0f, 0.0f, 1.0f, true);
  /* depth: clip z = -z, so the stored depth (-z / w) is our z (GS: larger = closer) */
  g.projection.r[2].x = 0.0f;
  g.projection.r[2].y = 0.0f;
  g.projection.r[2].z = -1.0f;
  g.projection.r[2].w = 0.0f;

  /* OpenGL-style clip space -> PICA: same x/y mapping, z' = -(z + w) / 2 so the depth (-z'/w) is
   * (z/w + 1) / 2, the OpenGL depth. The game's matrices (tfrag3.vert, merc2.vert) put the GS
   * depth (larger = closer) there, and the PC renderer tests it with GL_GEQUAL. */
  Mtx_OrthoTilt(&g.gl_to_pica, -1.25f, 1.25f, -1.0f, 1.0f, 0.0f, 1.0f, true);
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
  C3D_FrameBegin(0); /* waits for the GPU; the game thread already paces to vblank */
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

void ctr_gpu_frame_end(void) {
  if (!g.ready || !g.in_frame) {
    return;
  }
  flush_vbuf();
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

/* Textures go to VRAM while there is room (linear memory is only ~24 MB and holds the level
 * meshes; the GPU also reads VRAM faster), else to linear memory. tex_alloc returns the buffer to
 * write the texels to (a linear staging buffer for VRAM), tex_commit uploads it. */
static int g_rgba4_as_rgba8 = 1;

void ctr_gpu_set_rgba4_as_rgba8(int on) {
  g_rgba4_as_rgba8 = on;
}

static int g_vram_textures = 0;

void ctr_gpu_set_vram_textures(int on) {
  g_vram_textures = on;
}

static void* tex_alloc(C3D_Tex* tex, int w, int h, GPU_TEXCOLOR fmt, int* on_vram) {
  *on_vram = 0;
  /* only outside of a frame: a copy in a frame needs a command list split and a queue entry
   * each, and a level's worth of them overflows the GX queue */
  if (g_vram_textures && !g.in_frame && C3D_TexInitVRAM(tex, (u16)w, (u16)h, fmt)) {
    void* staging = linearAlloc(tex->size);
    if (staging) {
      *on_vram = 1;
      return staging;
    }
    C3D_TexDelete(tex);
  }
  if (!C3D_TexInit(tex, (u16)w, (u16)h, fmt)) {
    return NULL;
  }
  return tex->data;
}

static int tex_commit(C3D_Tex* tex, void* buf, int on_vram) {
  if (!on_vram) {
    C3D_TexFlush(tex);
    return 1;
  }
  GSPGPU_FlushDataCache(buf, tex->size);
  C3D_SyncTextureCopy((u32*)buf, 0, (u32*)tex->data, 0, tex->size, 8);
  /* check that the copy landed (VRAM is readable by the CPU); if not, use linear memory */
  int ok = memcmp(tex->data, buf, tex->size) == 0;
  if (!ok) {
    g.vram_copy_failures++;
    GPU_TEXCOLOR fmt = tex->fmt;
    u16 w = tex->width, h = tex->height;
    C3D_TexDelete(tex);
    if (!C3D_TexInit(tex, w, h, fmt)) {
      linearFree(buf);
      return 0;
    }
    memcpy(tex->data, buf, tex->size);
    C3D_TexFlush(tex);
  } else {
    g.vram_textures++;
  }
  linearFree(buf);
  return 1;
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
  int on_vram;
  uint8_t* dst = (uint8_t*)tex_alloc(tex, w, h, GPU_RGBA8, &on_vram);
  if (!dst) {
    return -1;
  }
  /* tiled: 8x8 tiles in rows, morton order inside a tile */
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
  if (!tex_commit(tex, dst, on_vram)) {
    return -1;
  }
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
    g.cur.cmd_splits++;
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
  } else if (mesh == 1 && g.fog_tex_valid) {
    /* fog: rgb = mix(previous, fog color, fog amount) with the amount from the fog ramp texture
     * (texcoord1 from the mesh shader) */
    C3D_TexBind(1, &g.fog_tex);
    C3D_TexEnvSrc(env1, C3D_RGB, GPU_TEXTURE1, GPU_PREVIOUS, GPU_TEXTURE1);
    C3D_TexEnvOpRgb(env1, GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR,
                    GPU_TEVOP_RGB_SRC_ALPHA);
    C3D_TexEnvFunc(env1, C3D_RGB, GPU_INTERPOLATE);
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
      /* level meshes / merc: texture alpha 0xff = 1, vertex color 0x80 = 1 (merc: the skin shader
       * outputs half the lit color, so x4) */
      C3D_TexEnvSrc(env, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
      C3D_TexEnvFunc(env, C3D_Both, GPU_MODULATE);
      C3D_TexEnvScale(env, C3D_RGB, mesh == 2 ? GPU_TEVSCALE_4 : GPU_TEVSCALE_2);
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
    C3D_TexEnvScale(env, C3D_RGB, mesh == 2 ? GPU_TEVSCALE_4 : GPU_TEVSCALE_2);
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
  memcpy(dst, verts, bytes); /* flushed once at the end of the frame (flush_vbuf) */
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
  int on_vram;
  if (format == 1 && g_rgba4_as_rgba8) {
    /* RGBA4 texels expanded to RGBA8 (same tiled order; GPU_RGBA8 is stored A, B, G, R) */
    uint8_t* dst = (uint8_t*)tex_alloc(tex, w, h, GPU_RGBA8, &on_vram);
    if (!dst) {
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
      return -1;
    }
    g.textures[slot].used = 1;
    return slot;
  }
  void* dst = tex_alloc(tex, w, h, format == 0 ? GPU_RGB565 : GPU_RGBA4, &on_vram);
  if (!dst) {
    return -1;
  }
  memcpy(dst, data, (size_t)size < tex->size ? (size_t)size : tex->size);
  if (!tex_commit(tex, dst, on_vram)) {
    return -1;
  }
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
  /* consecutive draws of a level chunk share the matrix and the vertex buffer */
  if (g.last_mesh_valid && g.last_mesh == mesh && !memcmp(g.last_clip, clip, sizeof(g.last_clip))) {
    apply_state(state, 1);
    C3D_DrawElements(GPU_TRIANGLES, index_count, C3D_UNSIGNED_SHORT,
                     g.meshes[mesh].indices + first_index);
    g.cur.draws++;
    g.cur.triangles += index_count / 3;
    return;
  }
  g.last_mesh_valid = 1;
  g.last_mesh = mesh;
  memcpy(g.last_clip, clip, sizeof(g.last_clip));
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
  size_t vbytes = (size_t)vertex_count * 24; /* c3l::MercVertex */
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
  C3D_FVec* lv = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, g.uloc_skin_lights, 7);
  for (int i = 0; i < 7; i++) {
    lv[i].x = lights[4 * i];
    lv[i].y = lights[4 * i + 1];
    lv[i].z = lights[4 * i + 2];
    lv[i].w = lights[4 * i + 3];
  }
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
  g.last_state_valid = 0;
}
