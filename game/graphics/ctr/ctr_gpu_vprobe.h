#pragma once

/*
 * config.ini vram_probe = on: a hardware diagnostic for textures read wrongly from VRAM
 * (ctr_gpu_vprobe.c). Off by default; off, every hook below is one load and branch.
 *  - GPU-side probes each frame: a fixed pattern texture in VRAM and its control copy in linear
 *    memory drawn 1:1 into linear render targets, a live pool texture drawn from VRAM and from a
 *    GPU copy of it, pool VRAM swept with GPU copies against hashes taken at upload.
 *  - Every GPU write (copies, transfers, fills, render targets) checked against our VRAM
 *    allocations and linear heap; VRAM allocations checked for overlap.
 *  - The system's shared font (memory the HOME Menu draws from) hashed at boot and swept.
 *  - An event ring of GPU memory operations, dumped at the first failure.
 */

#include <3ds.h>
#include <citro3d.h>

#ifdef __cplusplus
extern "C" {
#endif

extern int ctr_vprobe_on;
/* config.ini vram_probe: before the GPU's first allocation */
void ctr_vprobe_enable(int on);

enum {
  VP_TEXCOPY = 1,
  VP_DISPXFER,
  VP_FILL,
  VP_SYNCCOPY,
  VP_SYNCXFER,
  VP_RTCLEAR,
  VP_VALLOC,
  VP_VFREE,
  VP_TEXVRAM,
  VP_TEXDEL,
  VP_RTCREATE,
  VP_RTDELETE,
  VP_POOL_IN,
  VP_POOL_OUT,
  VP_RT_TEX,
  VP_FB,
  VP_MASK,
  VP_APT,
  VP_KINDS
};

typedef struct {
  shaderProgram_s* prog; /* the game's ctr_basic program */
  int uloc_projection;
  int emulator;
  int cpu_vram_read; /* the CPU may read VRAM (mapped for title launches) */
} CtrVprobeIn;

/* a live pool texture to probe: the game's C3D_Tex as bound (data in VRAM) */
typedef struct {
  const C3D_Tex* tex;
  u32 bytes; /* all levels */
  int levels, slot, pool;
} CtrVprobeTex;

/* boot: buffers, orientation calibration, the shared font's hashes; outside a frame */
void ctr_vprobe_init(const CtrVprobeIn* in);
/* frame begin, after the pool copies are queued and before the frame's draws: evaluates finished
 * probes, queues new ones (live may be NULL). The caller resets its GPU state afterwards. */
void ctr_vprobe_frame(u32 frame, const CtrVprobeTex* live);
void ctr_vprobe_exit(void);

/* event rings: memory operations (256) and state (64); repeats of a recent entry are counted */
void ctr_vprobe_ev(int kind, int line, u32 a, u32 b, u32 c, u32 d);
void ctr_vprobe_st(int kind, int line, u32 a, u32 b, u32 c, u32 d);

/* our VRAM allocations (exact chunk sizes from vramGetSize) */
void ctr_vprobe_vram_add(const void* p, int line);
void ctr_vprobe_vram_del(const void* p, int line);
/* a GPU write of [dst, dst + bytes): logged and checked (aux: output dims, flags or control) */
void ctr_vprobe_gpu_write(int kind, int line, const void* dst, u32 bytes, const void* src,
                          u32 src_bytes, u32 aux);
/* a texture the GPU reads: inside a live VRAM allocation (and its pool's VRAM part)? */
void ctr_vprobe_tex_check(const void* data, u32 bytes, int slot, int pool, const void* pool_vram,
                          u32 pool_vram_bytes);
/* pool upload into VRAM (src: the bytes as queued) and its end (eviction, delete) */
void ctr_vprobe_pool_in(int pool, const void* vram, const void* src, u32 bytes);
void ctr_vprobe_pool_out(int pool, const void* vram, int line);

#ifdef __cplusplus
}
#endif

/* Only ctr_gpu_citro3d.c, after its safe_* allocators: every GPU memory operation it makes goes
 * through the checks (the line is the call site's). */
#ifdef CTR_VPROBE_WRAP

static inline u32 vp_copy_extent(u32 dim, u32 size) {
  const u32 w = (dim & 0xFFFF) * 16u, gap = (dim >> 16) * 16u;
  if (!w) {
    return size;
  }
  const u32 rows = (size + w - 1) / w;
  return rows * (w + gap) - gap;
}
static inline u32 vp_xfer_bpp(u32 fmt) {
  return fmt == 0 ? 4u : fmt == 1 ? 3u : 2u;
}
static inline u32 vp_xfer_bytes(u32 dim, u32 fmt) {
  return (dim & 0xFFFF) * (dim >> 16) * vp_xfer_bpp(fmt & 7);
}

static inline Result vp_gx_texcopy(u32* in, u32 indim, u32* out, u32 outdim, u32 size, u32 flags,
                                   int line) {
  if (ctr_vprobe_on) {
    ctr_vprobe_gpu_write(VP_TEXCOPY, line, out, vp_copy_extent(outdim, size), in,
                         vp_copy_extent(indim, size), outdim);
  }
  return (GX_TextureCopy)(in, indim, out, outdim, size, flags);
}
static inline Result vp_gx_xfer(u32* in, u32 indim, u32* out, u32 outdim, u32 flags, int line) {
  if (ctr_vprobe_on) {
    ctr_vprobe_gpu_write(VP_DISPXFER, line, out, vp_xfer_bytes(outdim, flags >> 12), in,
                         vp_xfer_bytes(indim, flags >> 8), flags);
  }
  return (GX_DisplayTransfer)(in, indim, out, outdim, flags);
}
static inline Result vp_gx_fill(u32* b0, u32 v0, u32* e0, u16 c0, u32* b1, u32 v1, u32* e1, u16 c1,
                                int line) {
  if (ctr_vprobe_on) {
    if (b0) {
      ctr_vprobe_gpu_write(VP_FILL, line, b0, (u32)((u8*)e0 - (u8*)b0), NULL, 0, c0);
    }
    if (b1) {
      ctr_vprobe_gpu_write(VP_FILL, line, b1, (u32)((u8*)e1 - (u8*)b1), NULL, 0, c1);
    }
  }
  return (GX_MemoryFill)(b0, v0, e0, c0, b1, v1, e1, c1);
}
static inline void vp_sync_copy(u32* in, u32 indim, u32* out, u32 outdim, u32 size, u32 flags,
                                int line) {
  if (ctr_vprobe_on) {
    ctr_vprobe_gpu_write(VP_SYNCCOPY, line, out, vp_copy_extent(outdim, size), in,
                         vp_copy_extent(indim, size), outdim);
  }
  (C3D_SyncTextureCopy)(in, indim, out, outdim, size, flags);
}
static inline void vp_sync_xfer(u32* in, u32 indim, u32* out, u32 outdim, u32 flags, int line) {
  if (ctr_vprobe_on) {
    ctr_vprobe_gpu_write(VP_SYNCXFER, line, out, vp_xfer_bytes(outdim, flags >> 12), in,
                         vp_xfer_bytes(indim, flags >> 8), flags);
  }
  (C3D_SyncDisplayTransfer)(in, indim, out, outdim, flags);
}
static inline u32 vp_color_bpp(u32 f) {
  return f == GPU_RB_RGBA8 ? 4u : f == GPU_RB_RGB8 ? 3u : 2u;
}
static inline u32 vp_depth_bpp(u32 f) {
  return f == GPU_RB_DEPTH24_STENCIL8 ? 4u : f == GPU_RB_DEPTH24 ? 3u : 2u;
}
static inline void vp_fb_check(const C3D_FrameBuf* fb, int kind, int line) {
  const u32 px = (u32)fb->width * fb->height;
  if (fb->colorBuf) {
    ctr_vprobe_gpu_write(kind, line, fb->colorBuf, px * vp_color_bpp(fb->colorFmt), NULL, 0,
                         ((u32)fb->width << 16) | fb->height);
  }
  if (fb->depthBuf) {
    ctr_vprobe_gpu_write(kind, line, fb->depthBuf, px * vp_depth_bpp(fb->depthFmt), NULL, 0,
                         ((u32)fb->width << 16) | fb->height | 0x8000u);
  }
}
static inline void vp_rt_clear(C3D_RenderTarget* t, C3D_ClearBits bits, u32 color, u32 depth,
                               int line) {
  if (ctr_vprobe_on) {
    vp_fb_check(&t->frameBuf, VP_RTCLEAR, line);
  }
  (C3D_RenderTargetClear)(t, bits, color, depth);
}
static inline void vp_set_fb(C3D_FrameBuf* fb, int line) {
  if (ctr_vprobe_on) {
    vp_fb_check(fb, VP_FB, line);
  }
  (C3D_SetFrameBuf)(fb);
}
static inline int vp_in_vram(const void* p) {
  return (u32)p >= OS_VRAM_VADDR && (u32)p < OS_VRAM_VADDR + OS_VRAM_SIZE;
}
static inline void* vp_vram_alloc_at(size_t size, vramAllocPos pos, int line) {
  void* p = (vramAllocAt)(size, pos);
  if (ctr_vprobe_on && p) {
    ctr_vprobe_vram_add(p, line);
  }
  return p;
}
static inline void vp_vram_free(void* p, int line) {
  if (ctr_vprobe_on && p) {
    ctr_vprobe_vram_del(p, line);
  }
  (vramFree)(p);
}
static inline bool vp_tex_init_params(C3D_Tex* tex, C3D_TexCube* cube, C3D_TexInitParams p,
                                      int line) {
  const bool ok = (C3D_TexInitWithParams)(tex, cube, p);
  if (ctr_vprobe_on && ok && !cube && vp_in_vram(tex->data)) {
    ctr_vprobe_vram_add(tex->data, line);
  }
  return ok;
}
static inline bool vp_tex_init_vram(C3D_Tex* tex, u16 w, u16 h, GPU_TEXCOLOR fmt, int line) {
  const bool ok = (C3D_TexInitVRAM)(tex, w, h, fmt);
  if (ctr_vprobe_on && ok && vp_in_vram(tex->data)) {
    ctr_vprobe_vram_add(tex->data, line);
  }
  return ok;
}
static inline void vp_tex_delete(C3D_Tex* tex, int line) {
  if (ctr_vprobe_on && vp_in_vram(tex->data)) {
    ctr_vprobe_vram_del(tex->data, line);
  }
  (C3D_TexDelete)(tex);
}
static inline C3D_RenderTarget* vp_rt_create(int w, int h, GPU_COLORBUF cf, C3D_DEPTHTYPE df,
                                             int line) {
  C3D_RenderTarget* t = (C3D_RenderTargetCreate)(w, h, cf, df);
  if (ctr_vprobe_on && t) {
    if (vp_in_vram(t->frameBuf.colorBuf)) {
      ctr_vprobe_vram_add(t->frameBuf.colorBuf, line);
    }
    if (vp_in_vram(t->frameBuf.depthBuf)) {
      ctr_vprobe_vram_add(t->frameBuf.depthBuf, line);
    }
  }
  return t;
}
static inline void vp_rt_delete(C3D_RenderTarget* t, int line) {
  if (ctr_vprobe_on && t) {
    if (vp_in_vram(t->frameBuf.colorBuf)) {
      ctr_vprobe_vram_del(t->frameBuf.colorBuf, line);
    }
    if (vp_in_vram(t->frameBuf.depthBuf)) {
      ctr_vprobe_vram_del(t->frameBuf.depthBuf, line);
    }
  }
  (C3D_RenderTargetDelete)(t);
}

#define GX_TextureCopy(a, b, c, d, e, f) vp_gx_texcopy(a, b, c, d, e, f, __LINE__)
#define GX_DisplayTransfer(a, b, c, d, e) vp_gx_xfer(a, b, c, d, e, __LINE__)
#define GX_MemoryFill(a, b, c, d, e, f, g_, h) vp_gx_fill(a, b, c, d, e, f, g_, h, __LINE__)
#define C3D_SyncTextureCopy(a, b, c, d, e, f) vp_sync_copy(a, b, c, d, e, f, __LINE__)
#define C3D_SyncDisplayTransfer(a, b, c, d, e) vp_sync_xfer(a, b, c, d, e, __LINE__)
#define C3D_RenderTargetClear(a, b, c, d) vp_rt_clear(a, b, c, d, __LINE__)
#define C3D_SetFrameBuf(fb) vp_set_fb(fb, __LINE__)
#define vramAllocAt(n, pos) vp_vram_alloc_at(n, pos, __LINE__)
#define vramFree(p) vp_vram_free(p, __LINE__)
#define C3D_TexInitWithParams(...) vp_tex_init_params(__VA_ARGS__, __LINE__)
#define C3D_TexInitVRAM(t, w, h, f) vp_tex_init_vram(t, w, h, f, __LINE__)
#define C3D_TexDelete(t) vp_tex_delete(t, __LINE__)
#define C3D_RenderTargetCreate(w, h, cf, df) vp_rt_create(w, h, cf, df, __LINE__)
#define C3D_RenderTargetDelete(t) vp_rt_delete(t, __LINE__)

#endif
