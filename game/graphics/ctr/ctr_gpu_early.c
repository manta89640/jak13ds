/*
 * The early depth test checked on the console itself (ctr_gpu_early.h). What the PICA200 keeps
 * per block and when it updates it is undocumented, and no emulator has the test. So a small
 * frame with every event a real frame has (the clear, near draws that cover a tile, part of a
 * block or a block, mixed depths in one block, sloped draws, alpha tested and blended draws, a
 * render target switch, a command list split, the frame before) is drawn twice into the real top
 * target: with the early test on the draws the game would use it on, and with it off. The color
 * and depth buffers of both must be the same, byte for byte (a 64-bit hash of each). On a
 * mismatch each event is drawn alone, to name it in the log.
 */

#include "game/graphics/ctr/ctr_gpu_early.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

void ctr_linear_lock(void);
void ctr_linear_unlock(void);

/* citro3d's context starts with its GX queue (as ctr_gpu_bench.c reads it) */
extern char __C3D_Context[];
static gxCmdQueue_s* ed_queue(void) {
  return (gxCmdQueue_s*)(void*)__C3D_Context;
}

#define ED_TIMEOUT ((s64)1000 * (s64)(SYSCLOCK_ARM11 / 1000))
#define ED_VERTS 256
#define RT_W 64
#define RT_H 64
#define B32_W 256
#define B32_H 416
#define Z_NEAR 0.9f
#define Z_FAR 0.5f

typedef struct {
  float x, y, z, s, t;
  u8 r, g, b, a;
} EdVtx; /* ctr_basic's layout */

enum {
  F_FRAME = 1 << 0,   /* the frame before left near early depth data everywhere */
  F_TILE8 = 1 << 1,   /* one 8x8 tile covered near */
  F_PARTIAL = 1 << 2, /* a near 4x4 square inside a tile */
  F_MOSAIC = 1 << 3,  /* a 32x32 block of 8x8 squares at two depths (a near one last), a mid quad */
  F_SLOPE = 1 << 4,   /* sloped quads half in front of a covered block */
  F_ATEST = 1 << 5,   /* a near quad half dropped by the alpha test (early test off) */
  F_NOZW = 1 << 6,    /* a near blended quad without depth write (early test off) */
  F_RT = 1 << 7,      /* a near draw into another render target (early test off), and back */
  F_SPLIT = 1 << 8,   /* a covered block, then the command list split (flush, invalidate) */
  F_ZFAR = 1 << 9,    /* a covered block, then depth written farther without the test */
  F_COUNT = 10,
  F_ALL = (1 << F_COUNT) - 1,
};
static const char* const kFeat[F_COUNT] = {"frame", "tile8", "partial", "mosaic", "slope",
                                           "atest", "nozw",  "rt",      "split", "zfar"};

typedef struct {
  C3D_FrameBuf fb;
  u32 clear_color;
  int cbytes, dbytes; /* per pixel */
} EdTarget;

typedef struct {
  u64 chash, dhash; /* of the color and the depth buffer */
  int clear_px;     /* color buffer pixels still at the clear color */
} EdOut;

static struct {
  shaderProgram_s* prog;
  int uloc;
  int timeout; /* a GPU wait timed out: the GPU may still write the test's buffers */
  EdVtx* vb;
  int nv;
  u8* rb;
  u32 rb_bytes;
  C3D_FrameBuf rt;
  void *rt_color, *rt_depth;
  int func;
  u32 clear;
} E;

static void* lin_alloc(size_t n) {
  ctr_linear_lock();
  void* p = linearAlloc(n);
  ctr_linear_unlock();
  return p;
}
static void lin_free(void* p) {
  if (p) {
    ctr_linear_lock();
    linearFree(p);
    ctr_linear_unlock();
  }
}
static void* vram_alloc(size_t n) {
  ctr_linear_lock();
  void* p = vramAlloc(n);
  ctr_linear_unlock();
  return p;
}
static void vram_free(void* p) {
  if (p) {
    ctr_linear_lock();
    vramFree(p);
    ctr_linear_unlock();
  }
}

/* pixel coordinates of a w x h target, z = depth 0..1 (larger = closer) */
static void mtx_px(C3D_Mtx* m, float w, float h) {
  Mtx_Zeros(m);
  m->r[0].x = 2.0f / w;
  m->r[0].w = -1.0f;
  m->r[1].y = 2.0f / h;
  m->r[1].w = -1.0f;
  m->r[2].z = -1.0f;
  m->r[3].w = 1.0f;
}

static void use_target(C3D_FrameBuf* fb, int scissor) {
  C3D_SetFrameBuf(fb);
  C3D_SetViewport(0, 0, fb->width, fb->height);
  if (scissor) {
    C3D_SetScissor(GPU_SCISSOR_NORMAL, 0, 0, fb->width, fb->height);
  } else {
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
  }
  C3D_Mtx m;
  mtx_px(&m, fb->width, fb->height);
  C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, E.uloc, &m);
}

/* program, vertex layout, combiners (vertex color), no texture, culling or stencil */
static void common_state(void) {
  C3D_BindProgram(E.prog);
  C3D_AttrInfo* a = C3D_GetAttrInfo();
  AttrInfo_Init(a);
  AttrInfo_AddLoader(a, 0, GPU_FLOAT, 3);
  AttrInfo_AddLoader(a, 1, GPU_FLOAT, 2);
  AttrInfo_AddLoader(a, 2, GPU_UNSIGNED_BYTE, 4);
  C3D_BufInfo* b = C3D_GetBufInfo();
  BufInfo_Init(b);
  BufInfo_Add(b, E.vb, sizeof(EdVtx), 3, 0x210);
  for (int i = 0; i < 6; i++) {
    C3D_TexEnv* e = C3D_GetTexEnv(i);
    C3D_TexEnvInit(e);
    if (i == 0) {
      C3D_TexEnvSrc(e, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
      C3D_TexEnvFunc(e, C3D_Both, GPU_REPLACE);
    }
  }
  for (int i = 0; i < 3; i++) {
    C3D_TexBind(i, NULL);
  }
  C3D_CullFace(GPU_CULL_NONE);
  C3D_StencilTest(false, GPU_ALWAYS, 0, 0xFF, 0);
  C3D_DepthMap(true, -1.0f, 0.0f);
}

/* the draw state: depth test >= as the level's; early: the early test on (this pass and draw) */
static void state(int early, int zwrite, int atest, int blend) {
  C3D_DepthTest(true, GPU_GEQUAL, zwrite ? GPU_WRITE_ALL : GPU_WRITE_COLOR);
  C3D_EarlyDepthTest(early != 0, (GPU_EARLYDEPTHFUNC)E.func, E.clear);
  C3D_AlphaTest(atest != 0, GPU_GEQUAL, 0x80);
  if (blend) {
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_ONE,
                   GPU_ZERO);
  } else {
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
  }
}

/* a quad (two triangles), depth z0 at x0 to z1 at x1, alpha a0 to a1 the same way */
static void quad(float x0, float y0, float x1, float y1, float z0, float z1, u32 rgb, u8 a0,
                 u8 a1) {
  if (E.nv + 6 > ED_VERTS) {
    return;
  }
  EdVtx* v = E.vb + E.nv;
  const float px[6] = {x0, x1, x0, x0, x1, x1}, py[6] = {y0, y0, y1, y1, y0, y1};
  for (int k = 0; k < 6; k++) {
    const int right = px[k] != x0;
    v[k].x = px[k];
    v[k].y = py[k];
    v[k].z = right ? z1 : z0;
    v[k].s = v[k].t = 0.0f;
    v[k].r = (u8)(rgb >> 16);
    v[k].g = (u8)(rgb >> 8);
    v[k].b = (u8)rgb;
    v[k].a = right ? a1 : a0;
  }
  C3D_DrawArrays(GPU_TRIANGLES, E.nv, 6);
  E.nv += 6;
}

static int frame_begin(void) {
  E.nv = 0;
  return C3D_FrameBegin(0) ? 1 : 0;
}

static int frame_end(void) {
  if (E.nv) {
    GSPGPU_FlushDataCache(E.vb, (u32)E.nv * sizeof(EdVtx));
  }
  C3D_FrameEnd(GX_CMDLIST_FLUSH);
  if (!gxCmdQueueWait(ed_queue(), ED_TIMEOUT)) {
    E.timeout = 1;
    return 0;
  }
  return 1;
}

/* the events of feats, then the far layer and the sky, as the game draws them (after its frame
 * begin: the clear, the target bound, ctr_gpu_early_establish) */
static void draw_events(EdTarget* t, u32 f, int early) {
  const float w = t->fb.width, h = t->fb.height;
  if (f & F_TILE8) {
    state(early, 1, 0, 0);
    quad(8, 8, 16, 16, Z_NEAR, Z_NEAR, 0xff0000, 255, 255);
  }
  if (f & F_PARTIAL) {
    state(early, 1, 0, 0);
    quad(45, 13, 49, 17, Z_NEAR, Z_NEAR, 0xff4000, 255, 255);
  }
  if (f & F_ATEST) {
    state(0, 1, 1, 0);
    quad(96, 0, 128, 32, Z_NEAR, Z_NEAR, 0x00ffff, 0, 255);
  }
  if (f & F_NOZW) {
    state(0, 0, 0, 1);
    quad(160, 0, 192, 32, Z_NEAR, Z_NEAR, 0xffff00, 128, 128);
  }
  if (f & F_MOSAIC) {
    state(early, 1, 0, 0);
    for (int k = 0; k < 16; k++) {
      const int i = k & 3, j = k >> 2;
      const float z = ((i + j) & 1) ? 0.6f : Z_NEAR; /* the last one (3, 3) near */
      quad(8.0f * i, 64.0f + 8.0f * j, 8.0f * i + 8, 72.0f + 8.0f * j, z, z, 0x800000u + 0x20u * k,
           255, 255);
    }
    quad(0, 64, 32, 96, 0.75f, 0.75f, 0x808000, 255, 255);
  }
  if (f & F_SLOPE) {
    state(early, 1, 0, 0);
    quad(64, 64, 128, 96, 0.6f, 0.6f, 0x400080, 255, 255);
    quad(64, 64, 96, 96, 0.4f, 0.8f, 0x8000ff, 255, 255);
    quad(96, 64, 128, 96, 0.8f, 0.4f, 0x0080ff, 255, 255);
  }
  if (f & F_RT) {
    use_target(&E.rt, 1);
    state(0, 1, 0, 0);
    quad(0, 0, RT_W, RT_H, 0.99f, 0.99f, 0xff00ff, 255, 255);
    use_target(&t->fb, 0);
    ctr_gpu_early_establish();
  }
  if (f & F_SPLIT) {
    state(early, 1, 0, 0);
    quad(160, 128, 192, 160, Z_NEAR, Z_NEAR, 0x804020, 255, 255);
    GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_FLUSH, 1);
    GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_INVALIDATE, 1);
    u32* list = NULL;
    u32 words = 0;
    GPUCMD_Split(&list, &words);
    if (words) {
      GX_ProcessCommandList(list, words * 4, GX_CMDLIST_FLUSH);
    }
  }
  if (f & F_ZFAR) {
    state(early, 1, 0, 0);
    quad(192, 64, 224, 96, Z_NEAR, Z_NEAR, 0x206020, 255, 255);
    state(0, 1, 0, 0);
    C3D_DepthTest(true, GPU_ALWAYS, GPU_WRITE_ALL);
    quad(192, 64, 224, 96, 0.3f, 0.3f, 0x602020, 255, 255);
    ctr_gpu_early_establish(); /* as the game's next early draw does after such a write */
  }
  /* the far layer (a strip left for the sky), a hidden one behind it */
  state(early, 1, 0, 0);
  quad(0, 0, w, h - 40, Z_FAR, Z_FAR, 0x00ff00, 255, 255);
  quad(0, 0, w, h - 40, 0.45f, 0.45f, 0x008000, 255, 255);
  /* the sky: depth 0 from the depth map (sky_last), no depth write */
  C3D_DepthMap(true, 0.0f, 0.0f);
  state(early, 0, 0, 0);
  quad(0, 0, w, h, 0.3f, 0.3f, 0x4080ff, 255, 255);
  C3D_DepthMap(true, -1.0f, 0.0f);
}

static int copy_back(const void* src, u32 bytes) {
  if (bytes > E.rb_bytes || !frame_begin()) {
    return 0;
  }
  GX_TextureCopy((u32*)src, 0, (u32*)E.rb, 0, bytes, 8);
  if (!frame_end()) {
    return 0;
  }
  GSPGPU_InvalidateDataCache(E.rb, bytes);
  return 1;
}

static u64 hash_words(const u32* p, u32 n, u64 h) {
  for (u32 i = 0; i < n; i++) {
    h = (h ^ p[i]) * 0x100000001B3ull;
  }
  return h;
}

static int read_out(EdTarget* t, EdOut* o) {
  const u32 px = (u32)t->fb.width * t->fb.height;
  if (!copy_back(t->fb.colorBuf, px * (u32)t->cbytes)) {
    return 0;
  }
  o->chash = hash_words((const u32*)E.rb, px * (u32)t->cbytes / 4, 0xCBF29CE484222325ull);
  int n = 0;
  if (t->cbytes == 4) {
    const u32* c = (const u32*)E.rb;
    for (u32 i = 0; i < px; i++) {
      n += c[i] == t->clear_color;
    }
  } else {
    const u16* c = (const u16*)E.rb;
    for (u32 i = 0; i < px; i++) {
      n += c[i] == (u16)t->clear_color;
    }
  }
  if (!copy_back(t->fb.depthBuf, px * (u32)t->dbytes)) {
    return 0;
  }
  o->dhash = hash_words((const u32*)E.rb, px * (u32)t->dbytes / 4, 0xCBF29CE484222325ull);
  o->clear_px = n;
  return 1;
}

/* one frame of the game's (F_FRAME: and the one before it) with the early test on or off */
static int run(EdTarget* t, u32 f, int early, EdOut* o) {
  if (f & F_FRAME) {
    if (!frame_begin()) {
      return 0;
    }
    use_target(&t->fb, 0);
    common_state();
    ctr_gpu_early_establish();
    state(early, 1, 0, 0);
    quad(0, 0, t->fb.width, t->fb.height, 0.95f, 0.95f, 0xffffff, 255, 255);
    if (!frame_end()) {
      return 0;
    }
  }
  if (!frame_begin()) {
    return 0;
  }
  C3D_FrameBufClear(&t->fb, C3D_CLEAR_ALL, t->clear_color, 0);
  use_target(&t->fb, 0);
  common_state();
  ctr_gpu_early_establish();
  draw_events(t, f, early);
  return frame_end() && read_out(t, o);
}

/* two runs of feats (early test a, b): 0 the same, else 1 the color differs | 2 the depth; -1 the
 * GPU didn't finish. lost: clear color pixels of a minus those of b */
static int differ(EdTarget* t, u32 f, int a, int b, int* lost) {
  EdOut oa, ob;
  if (!run(t, f, a, &oa) || !run(t, f, b, &ob)) {
    return -1;
  }
  *lost = oa.clear_px - ob.clear_px;
  return (oa.chash != ob.chash ? 1 : 0) | (oa.dhash != ob.dhash ? 2 : 0);
}

static const char* const kWhat[4] = {"", "color", "depth", "color+depth"};

/* the whole check of one target: 1 pass, 0 fail, -1 not run; its part of the log line. First
 * the frame twice without the early test: a readback that isn't repeatable proves nothing */
static int check_target(EdTarget* t, const char* tag, char* out, int size) {
  int lost = 0;
  int r = differ(t, F_ALL, 0, 0, &lost);
  if (r < 0) {
    snprintf(out, (size_t)size, "%s: GPU timeout", tag);
    return -1;
  }
  if (r) {
    snprintf(out, (size_t)size, "%s FAIL (not repeatable: %s)", tag, kWhat[r]);
    return 0;
  }
  r = differ(t, F_ALL, 1, 0, &lost);
  if (r < 0) {
    snprintf(out, (size_t)size, "%s: GPU timeout", tag);
    return -1;
  }
  if (!r) {
    snprintf(out, (size_t)size, "%s pass", tag);
    return 1;
  }
  int len = snprintf(out, (size_t)size, "%s FAIL (all %s %d px", tag, kWhat[r], lost);
  for (int k = -1; k < F_COUNT && len < size; k++) {
    const u32 f = k < 0 ? 0u : 1u << k;
    r = differ(t, f, 1, 0, &lost);
    if (r > 0) {
      len += snprintf(out + len, (size_t)(size - len), ", %s %s %d px", k < 0 ? "base" : kFeat[k],
                      kWhat[r], lost);
    }
  }
  if (len < size) {
    snprintf(out + len, (size_t)(size - len), ")");
  }
  return 0;
}

static void target_init(EdTarget* t, C3D_FrameBuf* fb) {
  t->fb = *fb;
  t->cbytes = fb->colorFmt == GPU_RB_RGBA8 ? 4 : 2;
  t->dbytes = fb->depthFmt == GPU_RB_DEPTH16 ? 2 : 4;
  t->clear_color = t->cbytes == 4 ? 0x203040FFu : 0x2104u;
}

int ctr_early_selftest(const CtrEarlyTestIn* in, char* status, int size) {
  memset(&E, 0, sizeof(E));
  E.func = in->func;
  E.clear = in->clear;
  status[0] = 0;
  EdTarget top;
  target_init(&top, in->top);
  E.prog = in->prog;
  E.uloc = in->uloc_projection;
  E.rb_bytes = (u32)B32_W * B32_H * 4;
  E.vb = (EdVtx*)lin_alloc(ED_VERTS * sizeof(EdVtx));
  E.rb = (u8*)lin_alloc(E.rb_bytes);
  E.rt_color = lin_alloc(RT_W * RT_H * 4);
  E.rt_depth = lin_alloc(RT_W * RT_H * 2);
  int ok = 0;
  if (!E.prog || !E.prog->vertexShader || E.uloc < 0 || !E.vb || !E.rb || !E.rt_color ||
      !E.rt_depth) {
    snprintf(status, (size_t)size, "not run (no memory)");
  } else {
    memset(E.rt_color, 0, RT_W * RT_H * 4);
    memset(E.rt_depth, 0, RT_W * RT_H * 2);
    GSPGPU_FlushDataCache(E.rt_color, RT_W * RT_H * 4);
    GSPGPU_FlushDataCache(E.rt_depth, RT_W * RT_H * 2);
    C3D_FrameBufAttrib(&E.rt, RT_W, RT_H, false);
    C3D_FrameBufColor(&E.rt, E.rt_color, GPU_RB_RGBA8);
    C3D_FrameBufDepth(&E.rt, E.rt_depth, GPU_RB_DEPTH16);
    char part32[160], part8[200];
    /* 32x32 block mode first (the log only), the top target last: the GPU ends bound to it */
    EdTarget b32 = top;
    void* c32 = vram_alloc((size_t)B32_W * B32_H * top.cbytes);
    void* d32 = vram_alloc((size_t)B32_W * B32_H * top.dbytes);
    if (c32 && d32) {
      C3D_FrameBufAttrib(&b32.fb, B32_W, B32_H, true);
      C3D_FrameBufColor(&b32.fb, c32, (GPU_COLORBUF)top.fb.colorFmt);
      C3D_FrameBufDepth(&b32.fb, d32, (GPU_DEPTHBUF)top.fb.depthFmt);
      ok |= check_target(&b32, "block32", part32, sizeof(part32)) > 0 ? CTR_EARLY_OK_BLOCK32 : 0;
    } else {
      snprintf(part32, sizeof(part32), "block32 not run (no VRAM)");
    }
    if (!E.timeout) {
      vram_free(c32);
      vram_free(d32);
    }
    ok |= check_target(&top, "block8", part8, sizeof(part8)) > 0 ? CTR_EARLY_OK_BLOCK8 : 0;
    /* the screen shows this buffer next: nothing of the test may be left in it */
    if (frame_begin()) {
      C3D_FrameBufClear(&top.fb, C3D_CLEAR_ALL, top.cbytes == 4 ? 0x000000FFu : 0u, 0);
      frame_end();
    }
    snprintf(status, (size_t)size, "%s, %s (func %d, clear 0x%06lx)%s", part8, part32, E.func,
             (unsigned long)E.clear, in->emulator ? " (emulator: it has no early depth test)" : "");
  }
  C3D_EarlyDepthTest(false, (GPU_EARLYDEPTHFUNC)E.func, E.clear);
  C3D_DepthMap(true, -1.0f, 0.0f);
  if (!E.timeout) {
    lin_free(E.vb);
    lin_free(E.rb);
    lin_free(E.rt_color);
    lin_free(E.rt_depth);
  }
  return ok;
}
