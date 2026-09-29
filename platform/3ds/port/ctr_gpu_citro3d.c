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

static struct {
  int ready;
  C3D_RenderTarget* top;
  DVLB_s* dvlb;
  shaderProgram_s program;
  int uloc_projection;
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
}

static void setup_projection(void) {
  /* x in [-1.25, 1.25] covers the 400 pixel width, so [-1, 1] is a centered 320x240 4:3 area */
  Mtx_OrthoTilt(&g.projection, -1.25f, 1.25f, -1.0f, 1.0f, 0.0f, 1.0f, true);
  /* depth: clip z = -z, so the stored depth (-z / w) is our z (GS: larger = closer) */
  g.projection.r[2].x = 0.0f;
  g.projection.r[2].y = 0.0f;
  g.projection.r[2].z = -1.0f;
  g.projection.r[2].w = 0.0f;
}

int ctr_gpu_init(void) {
  if (g.ready) {
    return 0;
  }
  if (!C3D_Init(C3D_DEFAULT_CMDBUF_SIZE * 2)) {
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

  C3D_AttrInfo* attr = C3D_GetAttrInfo();
  AttrInfo_Init(attr);
  AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 3);          /* position */
  AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 2);          /* texcoord */
  AttrInfo_AddLoader(attr, 2, GPU_UNSIGNED_BYTE, 4);  /* color */

  g.vbuf = (uint8_t*)linearAlloc(VBUF_BYTES);
  if (!g.vbuf) {
    return -3;
  }
  setup_projection();
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
  linearFree(g.vbuf);
  shaderProgramFree(&g.program);
  DVLB_Free(g.dvlb);
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
  C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g.uloc_projection, &g.projection);
  g.vbuf_used = 0;
  memset(&g.cur, 0, sizeof(g.cur));
  g.in_frame = 1;
}

void ctr_gpu_frame_end(void) {
  if (!g.ready || !g.in_frame) {
    return;
  }
  C3D_FrameEnd(0);
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

static void apply_state(const ctr_draw_state* st) {
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
    if (st->decal) {
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

  apply_state(state);
  C3D_BufInfo* buf = C3D_GetBufInfo();
  BufInfo_Init(buf);
  BufInfo_Add(buf, dst, sizeof(ctr_vertex), 3, 0x210);
  C3D_DrawArrays(GPU_TRIANGLES, 0, count);
  g.cur.draws++;
  g.cur.triangles += count / 3;
}

void ctr_gpu_get_stats(ctr_gpu_stats* out) {
  *out = g.last;
}
