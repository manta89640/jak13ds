/*
 * config.ini vram_probe = on (ctr_gpu_vprobe.h). Each probe kind answers one question:
 *  - static S: a 32x32 RGBA8 pattern (each texel names its own address) in VRAM, read by the
 *    texture unit (drawn 1:1), by the transfer engine (copied) and by the CPU; its control C is the
 *    same pattern in linear memory, drawn the same way. Texture unit wrong, copy and CPU right: the
 *    GPU fetches VRAM wrongly. All three wrong: something wrote over it. CPU right, copy wrong: the
 *    CPU's view of VRAM can't be trusted (the old texture check read it).
 *  - live: a pool texture drawn from VRAM while the copy engine copies it to linear memory; once
 *    the CPU has rewritten that copy (an emulator keeps the GPU's view of copied memory apart
 *    from the bytes), it is drawn from there the same way. Any difference is the texture unit
 *    reading VRAM differently from the copy engine.
 *  - sweep: pool VRAM copied out piece by piece and hashed against the bytes as uploaded.
 *  - guard: every GPU write must land inside one of our VRAM allocations or the linear heap, no
 *    VRAM allocation may overlap another, no write may land in a resident pool.
 *  - font: the system's shared font (the HOME Menu's glyphs) must never change.
 * All results are read back after a stamp drawn last has landed.
 */

#include "game/graphics/ctr/ctr_gpu_vprobe.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ctr_linear_lock(void);
void ctr_linear_unlock(void);
void ctr_log_sync(void);
extern u32 __ctru_linear_heap, __ctru_linear_heap_size;

int ctr_vprobe_on = 0;
extern char __C3D_Context[]; /* starts with its GX queue (as ctr_gpu_early.c reads it) */

#define VP_W 32
#define VP_TEX (VP_W * VP_W * 4)
#define VP_SWEEP (16 * 1024)
#define VP_LIVE (32 * 1024)
#define VP_CHUNK 4096
#define VP_SETS 2
#define VP_ORIENT 8
#define VP_STAMP_W 8
#define VP_FONT_STEP (16 * 1024)
#define VP_POOLS 256
#define VP_VMAX 1024
#define VP_RING 256
#define VP_SRING 64
#define VP_SET_BYTES (4 * VP_TEX + VP_TEX + VP_SWEEP + 256)

static const char* const kKind[VP_KINDS] = {
    "?",      "texcopy", "xfer",    "fill",     "synccopy", "syncxfer", "rtclear",
    "valloc", "vfree",   "texvram", "texdel",   "rtcreate", "rtdelete", "pool_in",
    "pool_out", "rt_tex", "fb",     "mask",     "apt"};

enum {
  F_WRITE,     /* GPU write outside our memory */
  F_OVERLAP,   /* VRAM allocation overlapping a live one */
  F_FREE,      /* vramFree of an address we don't hold */
  F_TEXPTR,    /* texture reading outside our VRAM allocations / its pool */
  F_POOLWRITE, /* GPU write into a resident pool's VRAM */
  F_S_TEX,
  F_S_DMA,
  F_S_CPU,
  F_C_TEX,
  F_SWEEP_DMA,
  F_SWEEP_CPU,
  F_LIVE,
  F_FONT,
  F_LOST, /* a probe never finished */
  F_N
};
static const char* const kFail[F_N] = {
    "GPU write outside our memory", "VRAM allocation overlaps a live one",
    "vramFree of an address not held", "texture outside our VRAM / its pool",
    "GPU write into a resident pool", "static VRAM texture drawn wrong",
    "static VRAM texture copied wrong", "static VRAM texture read wrong by the CPU",
    "control linear texture drawn wrong", "pool VRAM copied differs from upload",
    "pool VRAM read by the CPU differs from upload", "live texture: VRAM draw != linear draw",
    "shared font changed", "probe never finished"};

typedef struct {
  u32 first, last, count;
  u16 kind, line;
  u32 a, b, c, d;
} VpEv;

typedef struct {
  u32 va, bytes;
  u16 line;
} VpAlloc;

typedef struct {
  const u8* vram;
  u32 bytes, nchunks, frame;
  u32* hash;
} VpPool;

typedef struct {
  float x, y, z, s, t;
  u8 r, g, b, a;
} VpVtx;

typedef struct {
  int state; /* 0 free, 1 queued */
  int dma_s;
  u32 frame, stamp;
  u8* mem;
  u32 *t_s, *t_c, *t_v, *t_l, *r_s, *t_stamp;
  u8* r_sw;
  C3D_FrameBuf fb_s, fb_c, fb_v, fb_l, fb_stamp;
  int sw_pool, sw_n;
  u32 sw_off, sw_hash[VP_SWEEP / VP_CHUNK], sw_len[VP_SWEEP / VP_CHUNK];
  const u8* sw_vram;
  u32 sw_frame_in;
  int live; /* 1: the VRAM draw and the copy, 2: the draw from the copy */
} VpSet;

static LightLock s_lock;
static volatile u32 s_frame;
static VpEv s_ring[VP_RING], s_sring[VP_SRING];
static u32 s_ring_n, s_sring_n;
static VpAlloc s_va[VP_VMAX];
static int s_nva, s_va_full;
static VpPool s_pool[VP_POOLS];

static struct {
  int ready, orient, cpu_vram, dumps;
  shaderProgram_s* prog;
  int uloc;
  u32* pattern;
  void* s_vram;
  VpVtx* vb;
  C3D_Tex tex_s, tex_c, tex_v, tex_l;
  /* the live probe: 0 idle, 1 VRAM draw queued, 2 copy rewritten, 3 copy draw queued */
  int live_state, live_slot, live_pool, live_mips;
  u32 live_bytes, live_fmt, live_dim, live_frame;
  const void* live_vram;
  u8* r_live;
  u32 live_tv[VP_W * VP_W];
  VpSet set[VP_SETS];
  u32 sw_pool, sw_off; /* sweep cursor */
  /* the shared font */
  const u8* font;
  u32 font_bytes, font_chunks, font_cursor, font_sweeps;
  u32* font_hash;
  /* results */
  u32 fails[F_N];
  int first_fail;
  u32 first_frame;
  char pending[F_N][320];
  u8 pending_on[F_N];
  u32 probes, dropped, s_ok, live_ok, sw_chunks, sw_bytes, writes, allocs, texchecks;
  u64 ticks, ticks_n, t_check, t_queue, t_font;
  u64 last_line;
} P;

/* ---------------- helpers ---------------- */

static u32 hash_bytes(const u8* p, u32 n) {
  u32 h = 0x811C9DC5u;
  const u32* w = (const u32*)p;
  u32 i = 0;
  for (; i + 4 <= n; i += 4) {
    h = (h ^ *w++) * 0x01000193u;
  }
  for (; i < n; i++) {
    h = (h ^ p[i]) * 0x01000193u;
  }
  return h;
}

static void cache_invalidate(const void* p, u32 n) {
  if (R_FAILED(svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32)p, n))) {
    GSPGPU_InvalidateDataCache(p, n);
  }
}

static int in_vram(u32 va) {
  return va >= OS_VRAM_VADDR && va < OS_VRAM_VADDR + OS_VRAM_SIZE;
}

/* where a VA is: V A / V B (VRAM banks), lin (our linear heap), or what else */
static const char* where(u32 va) {
  if (in_vram(va)) {
    return va < OS_VRAM_VADDR + OS_VRAM_SIZE / 2 ? "VRAM A" : "VRAM B";
  }
  if (va >= __ctru_linear_heap && va < __ctru_linear_heap + __ctru_linear_heap_size) {
    return "linear";
  }
  return osConvertVirtToPhys((const void*)va) ? "FOREIGN" : "unmapped";
}

static u32 pattern_word(u32 i) {
  return 0xA5000000u | ((~i & 0x3FFu) << 12) | (i & 0x3FFu);
}
static int pattern_decode(u32 w) {
  if ((w & 0xFF000000u) != 0xA5000000u || (w & 0x00C00C00u) ||
      ((w >> 12) & 0x3FFu) != (~w & 0x3FFu)) {
    return -1;
  }
  return (int)(w & 0x3FFu);
}

void ctr_vprobe_enable(int on) {
  if (on && !ctr_vprobe_on) {
    LightLock_Init(&s_lock);
    ctr_vprobe_on = 1;
  }
}

/* ---------------- event rings ---------------- */

static void ring_push(VpEv* r, u32 cap, u32* n, int kind, int line, u32 a, u32 b, u32 c, u32 d) {
  LightLock_Lock(&s_lock);
  const u32 look = *n < 8 ? *n : 8;
  for (u32 k = 1; k <= look; k++) {
    VpEv* e = &r[(*n - k) % cap];
    if (e->kind == kind && e->line == line && e->a == a && e->b == b && e->c == c &&
        e->d == d) {
      e->count++;
      e->last = s_frame;
      LightLock_Unlock(&s_lock);
      return;
    }
  }
  VpEv* e = &r[*n % cap];
  e->first = e->last = s_frame;
  e->count = 1;
  e->kind = (u16)kind;
  e->line = (u16)line;
  e->a = a;
  e->b = b;
  e->c = c;
  e->d = d;
  (*n)++;
  LightLock_Unlock(&s_lock);
}

void ctr_vprobe_ev(int kind, int line, u32 a, u32 b, u32 c, u32 d) {
  if (ctr_vprobe_on) {
    ring_push(s_ring, VP_RING, &s_ring_n, kind, line, a, b, c, d);
  }
}

void ctr_vprobe_st(int kind, int line, u32 a, u32 b, u32 c, u32 d) {
  if (ctr_vprobe_on) {
    ring_push(s_sring, VP_SRING, &s_sring_n, kind, line, a, b, c, d);
  }
}

static void addr_txt(char* out, int size, u32 va) {
  if (in_vram(va) || (va >= 0x14000000u && va < 0x40000000u)) {
    snprintf(out, (size_t)size, "%08lx(%s pa %08lx)", (unsigned long)va, where(va),
             (unsigned long)osConvertVirtToPhys((const void*)va));
  } else {
    snprintf(out, (size_t)size, "%lx", (unsigned long)va);
  }
}

static void dump_ring(const char* name, const VpEv* r, u32 cap, u32 n) {
  const u32 count = n < cap ? n : cap;
  printf("[vprobe] %s ring: %lu entries (of %lu), oldest first; a = destination or allocation, "
         "b = bytes, c = source, d = dims/flags; L = line in ctr_gpu_citro3d.c\n",
         name, (unsigned long)count, (unsigned long)n);
  for (u32 k = 0; k < count; k++) {
    const VpEv* e = &r[(n - count + k) % cap];
    char a[64], c[64];
    addr_txt(a, sizeof(a), e->a);
    addr_txt(c, sizeof(c), e->c);
    printf("[vprobe]  f%lu-%lu x%lu %s L%u a=%s b=%lx c=%s d=%lx\n", (unsigned long)e->first,
           (unsigned long)e->last, (unsigned long)e->count,
           e->kind < VP_KINDS ? kKind[e->kind] : "?", e->line, a, (unsigned long)e->b, c,
           (unsigned long)e->d);
    if ((k & 31) == 31) {
      ctr_log_sync();
    }
  }
}

static void dump_allocs(void) {
  LightLock_Lock(&s_lock);
  static VpAlloc copy[VP_VMAX];
  const int n = s_nva;
  memcpy(copy, s_va, sizeof(VpAlloc) * (size_t)n);
  LightLock_Unlock(&s_lock);
  printf("[vprobe] VRAM allocations held: %d (vramSpaceFree %lu)\n", n,
         (unsigned long)vramSpaceFree());
  for (int i = 0; i < n; i++) {
    printf("[vprobe]  %08lx-%08lx %6lu L%u\n", (unsigned long)copy[i].va,
           (unsigned long)(copy[i].va + copy[i].bytes), (unsigned long)copy[i].bytes,
           copy[i].line);
    if ((i & 31) == 31) {
      ctr_log_sync();
    }
  }
}

/* any thread: noted, printed at the next frame begin (no lock or log wait here) */
static void fail(int cat, const char* fmt, ...) {
  LightLock_Lock(&s_lock);
  const u32 n = P.fails[cat]++;
  if (n == 0 && !P.pending_on[cat]) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(P.pending[cat], sizeof(P.pending[cat]), fmt, ap);
    va_end(ap);
    P.pending_on[cat] = 1;
  }
  LightLock_Unlock(&s_lock);
}

static void report_pending(void) {
  for (int c = 0; c < F_N; c++) {
    if (!P.pending_on[c]) {
      continue;
    }
    const int first = !P.first_fail;
    if (first) {
      P.first_fail = 1;
      P.first_frame = s_frame;
    }
    ctr_log_sync();
    printf("[vprobe] %s frame %lu: %s: %s\n", first ? "FIRST FAILURE" : "first failure of a kind",
           (unsigned long)s_frame, kFail[c], P.pending[c]);
    P.pending_on[c] = 0;
    if (P.dumps < 3 && c != F_LOST) {
      P.dumps++;
      ctr_log_sync();
      dump_ring("memory", s_ring, VP_RING, s_ring_n);
      ctr_log_sync();
      dump_ring("state", s_sring, VP_SRING, s_sring_n);
      ctr_log_sync();
      dump_allocs();
    }
    ctr_log_sync();
  }
}

/* ---------------- VRAM allocations and GPU writes ---------------- */

static int alloc_find(u32 va, u32 bytes) {
  for (int i = 0; i < s_nva; i++) {
    if (va >= s_va[i].va && va + bytes <= s_va[i].va + s_va[i].bytes) {
      return i;
    }
  }
  return -1;
}

void ctr_vprobe_vram_add(const void* p, int line) {
  if (!ctr_vprobe_on || !p) {
    return;
  }
  u32 bytes = (u32)vramGetSize((void*)p);
  if (!bytes) {
    bytes = 128;
  }
  const u32 va = (u32)p;
  ring_push(s_ring, VP_RING, &s_ring_n, VP_VALLOC, line, va, bytes, 0, vramSpaceFree());
  LightLock_Lock(&s_lock);
  int hit = -1;
  for (int i = 0; i < s_nva; i++) {
    if (va < s_va[i].va + s_va[i].bytes && s_va[i].va < va + bytes) {
      hit = i;
      break;
    }
  }
  VpAlloc old = hit >= 0 ? s_va[hit] : (VpAlloc){0, 0, 0};
  if (s_nva < VP_VMAX) {
    s_va[s_nva++] = (VpAlloc){va, bytes, (u16)line};
  } else {
    s_va_full = 1;
  }
  LightLock_Unlock(&s_lock);
  if (hit >= 0) {
    fail(F_OVERLAP, "new %08lx+%lu (L%d) over held %08lx+%lu (L%u), vramSpaceFree %lu",
         (unsigned long)va, (unsigned long)bytes, line, (unsigned long)old.va,
         (unsigned long)old.bytes, old.line, (unsigned long)vramSpaceFree());
  }
}

void ctr_vprobe_vram_del(const void* p, int line) {
  if (!ctr_vprobe_on || !p) {
    return;
  }
  const u32 va = (u32)p;
  LightLock_Lock(&s_lock);
  int i = 0;
  while (i < s_nva && s_va[i].va != va) {
    i++;
  }
  const int found = i < s_nva;
  u32 bytes = 0;
  if (found) {
    bytes = s_va[i].bytes;
    s_va[i] = s_va[--s_nva];
  }
  LightLock_Unlock(&s_lock);
  ring_push(s_ring, VP_RING, &s_ring_n, VP_VFREE, line, va, bytes, 0, 0);
  if (!found && !s_va_full) {
    fail(F_FREE, "%08lx at L%d (libctru's chunk size there now %lu)", (unsigned long)va, line,
         (unsigned long)vramGetSize((void*)p));
  }
}

void ctr_vprobe_gpu_write(int kind, int line, const void* dst, u32 bytes, const void* src,
                          u32 src_bytes, u32 aux) {
  if (!ctr_vprobe_on) {
    return;
  }
  const u32 va = (u32)dst;
  if (kind == VP_FB) {
    ring_push(s_sring, VP_SRING, &s_sring_n, kind, line, va, bytes, 0, aux);
  } else {
    ring_push(s_ring, VP_RING, &s_ring_n, kind, line, va, bytes, (u32)src, aux);
  }
  P.writes++;
  if (!bytes || bytes > 8u * 1024u * 1024u) {
    fail(F_WRITE, "%s L%d dst %08lx: %lu bytes", kKind[kind], line, (unsigned long)va,
         (unsigned long)bytes);
    return;
  }
  if (in_vram(va)) {
    LightLock_Lock(&s_lock);
    const int a = alloc_find(va, bytes);
    LightLock_Unlock(&s_lock);
    if (a < 0 && !s_va_full) {
      fail(F_WRITE, "%s L%d dst %08lx+%lu (pa %08lx, src %08lx+%lu, d %lx): no VRAM allocation "
           "of ours holds it", kKind[kind], line, (unsigned long)va, (unsigned long)bytes,
           (unsigned long)osConvertVirtToPhys(dst), (unsigned long)(u32)src,
           (unsigned long)src_bytes, (unsigned long)aux);
    }
    for (int p = 0; p < VP_POOLS; p++) {
      const VpPool* q = &s_pool[p];
      if (q->vram && va < (u32)q->vram + q->bytes && (u32)q->vram < va + bytes) {
        fail(F_POOLWRITE, "%s L%d dst %08lx+%lu over pool %d's VRAM %08lx+%lu (in since frame "
             "%lu)", kKind[kind], line, (unsigned long)va, (unsigned long)bytes, p,
             (unsigned long)(u32)q->vram, (unsigned long)q->bytes, (unsigned long)q->frame);
        if (q->hash) {
          memset(q->hash, 0, sizeof(u32) * q->nchunks); /* no longer the upload */
        }
      }
    }
  } else if (!(va >= __ctru_linear_heap &&
               va + bytes <= __ctru_linear_heap + __ctru_linear_heap_size)) {
    fail(F_WRITE, "%s L%d dst %08lx+%lu (%s, pa %08lx; linear heap %08lx+%lu)", kKind[kind], line,
         (unsigned long)va, (unsigned long)bytes, where(va),
         (unsigned long)osConvertVirtToPhys(dst), (unsigned long)__ctru_linear_heap,
         (unsigned long)__ctru_linear_heap_size);
  }
}

void ctr_vprobe_tex_check(const void* data, u32 bytes, int slot, int pool, const void* pool_vram,
                          u32 pool_vram_bytes) {
  if (!ctr_vprobe_on) {
    return;
  }
  const u32 va = (u32)data;
  P.texchecks++;
  if (!data) {
    fail(F_TEXPTR, "texture slot %d (pool %d): data NULL while in use", slot, pool);
    return;
  }
  if (!in_vram(va)) {
    return;
  }
  LightLock_Lock(&s_lock);
  const int a = alloc_find(va, bytes);
  LightLock_Unlock(&s_lock);
  if (a < 0 && !s_va_full) {
    fail(F_TEXPTR, "texture slot %d (pool %d) at %08lx+%lu: no VRAM allocation of ours holds it",
         slot, pool, (unsigned long)va, (unsigned long)bytes);
  } else if (pool >= 0 && (!pool_vram || va < (u32)pool_vram ||
                           va + bytes > (u32)pool_vram + pool_vram_bytes)) {
    fail(F_TEXPTR, "texture slot %d at %08lx+%lu outside pool %d's VRAM %08lx+%lu", slot,
         (unsigned long)va, (unsigned long)bytes, pool, (unsigned long)(u32)pool_vram,
         (unsigned long)pool_vram_bytes);
  }
}

void ctr_vprobe_pool_in(int pool, const void* vram, const void* src, u32 bytes) {
  if (!ctr_vprobe_on || pool < 0 || pool >= VP_POOLS) {
    return;
  }
  ring_push(s_ring, VP_RING, &s_ring_n, VP_POOL_IN, 0, (u32)vram, bytes, (u32)src, (u32)pool);
  VpPool* q = &s_pool[pool];
  free(q->hash);
  const u32 n = (bytes + VP_CHUNK - 1) / VP_CHUNK;
  u32* h = (u32*)malloc(sizeof(u32) * (n ? n : 1));
  if (h) {
    for (u32 i = 0; i < n; i++) {
      const u32 len = bytes - i * VP_CHUNK < VP_CHUNK ? bytes - i * VP_CHUNK : VP_CHUNK;
      h[i] = hash_bytes((const u8*)src + i * VP_CHUNK, len) | 1u; /* 0: unknown */
    }
  }
  LightLock_Lock(&s_lock);
  q->hash = h;
  q->nchunks = h ? n : 0;
  q->bytes = bytes;
  q->frame = s_frame;
  q->vram = (const u8*)vram;
  LightLock_Unlock(&s_lock);
}

void ctr_vprobe_pool_out(int pool, const void* vram, int line) {
  if (!ctr_vprobe_on || pool < 0 || pool >= VP_POOLS) {
    return;
  }
  ring_push(s_ring, VP_RING, &s_ring_n, VP_POOL_OUT, line, (u32)vram, s_pool[pool].bytes, 0,
            (u32)pool);
  VpPool* q = &s_pool[pool];
  LightLock_Lock(&s_lock);
  u32* h = q->hash;
  memset(q, 0, sizeof(*q));
  LightLock_Unlock(&s_lock);
  free(h);
}

/* ---------------- GPU probes ---------------- */

static void tex_setup(C3D_Tex* t, void* data, u32 dim, GPU_TEXCOLOR fmt, u32 size, u32 etc1,
                      int max_level) {
  memset(t, 0, sizeof(*t));
  t->data = data;
  t->fmt = fmt;
  t->size = size;
  t->dim = dim;
  t->param = GPU_TEXTURE_MODE(GPU_TEX_2D) | etc1 | GPU_TEXTURE_MAG_FILTER(GPU_NEAREST) |
             GPU_TEXTURE_MIN_FILTER(GPU_NEAREST) | GPU_TEXTURE_MIP_FILTER(GPU_NEAREST) |
             GPU_TEXTURE_WRAP_S(GPU_CLAMP_TO_EDGE) | GPU_TEXTURE_WRAP_T(GPU_CLAMP_TO_EDGE);
  t->maxLevel = (u8)max_level;
}

static void fb_setup(C3D_FrameBuf* fb, void* color, int w) {
  memset(fb, 0, sizeof(*fb));
  C3D_FrameBufAttrib(fb, (u16)w, (u16)w, false);
  C3D_FrameBufColor(fb, color, GPU_RB_RGBA8);
}

/* program, layout, one combiner stage (texture 0 or the constant), no tests or blending */
static void common_state(int constant, u32 color) {
  C3D_BindProgram(P.prog);
  C3D_AttrInfo* a = C3D_GetAttrInfo();
  AttrInfo_Init(a);
  AttrInfo_AddLoader(a, 0, GPU_FLOAT, 3);
  AttrInfo_AddLoader(a, 1, GPU_FLOAT, 2);
  AttrInfo_AddLoader(a, 2, GPU_UNSIGNED_BYTE, 4);
  C3D_BufInfo* b = C3D_GetBufInfo();
  BufInfo_Init(b);
  BufInfo_Add(b, P.vb, sizeof(VpVtx), 3, 0x210);
  for (int i = 0; i < 6; i++) {
    C3D_TexEnv* e = C3D_GetTexEnv(i);
    C3D_TexEnvInit(e);
    if (i == 0) {
      C3D_TexEnvSrc(e, C3D_Both, constant ? GPU_CONSTANT : GPU_TEXTURE0, 0, 0);
      C3D_TexEnvFunc(e, C3D_Both, GPU_REPLACE);
      C3D_TexEnvColor(e, color);
    }
  }
  C3D_CullFace(GPU_CULL_NONE);
  C3D_StencilTest(false, GPU_ALWAYS, 0, 0xFF, 0);
  C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
  C3D_EarlyDepthTest(false, GPU_EARLYDEPTH_GEQUAL, 0);
  C3D_AlphaTest(false, GPU_ALWAYS, 0);
  C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
  C3D_Mtx m;
  Mtx_Zeros(&m);
  m.r[0].x = 1.0f;
  m.r[1].y = 1.0f;
  m.r[2].w = -0.5f;
  m.r[3].w = 1.0f;
  C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, P.uloc, &m);
}

static void draw_into(C3D_FrameBuf* fb, int w, C3D_Tex* tex, int orient) {
  C3D_SetFrameBuf(fb);
  C3D_SetViewport(0, 0, (u32)w, (u32)w);
  C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
  if (tex) {
    C3D_TexBind(0, tex);
  }
  C3D_DrawArrays(GPU_TRIANGLE_STRIP, orient * 4, 4);
}

static void stamp_draw(VpSet* s) {
  common_state(1, s->stamp);
  draw_into(&s->fb_stamp, VP_STAMP_W, NULL, 0);
}

static int stamp_landed(const VpSet* s) {
  cache_invalidate(s->t_stamp, VP_STAMP_W * VP_STAMP_W * 4);
  const u32 want = __builtin_bswap32(s->stamp);
  for (int i = 0; i < VP_STAMP_W * VP_STAMP_W; i++) {
    const u32 v = ((volatile const u32*)s->t_stamp)[i];
    if (v != want && v != s->stamp) {
      return 0;
    }
  }
  return 1;
}

/* wrong texels of a 32x32 result against the pattern, and per 256-byte tile: '.' right, the hex
 * digit of the tile its texels came from, '?' texels that are no pattern word */
static u32 pattern_diff(const u32* got, char* map, char* first, int first_size) {
  u32 wrong = 0;
  int shown = 0;
  first[0] = 0;
  for (int t = 0; t < 16; t++) {
    int votes[16] = {0}, bad = 0;
    for (int k = 0; k < 64; k++) {
      const u32 i = (u32)(t * 64 + k);
      if (got[i] == P.pattern[i]) {
        continue;
      }
      bad++;
      const int src = pattern_decode(got[i]);
      if (src >= 0) {
        votes[src >> 6]++;
      }
      if (shown < 4) {
        const size_t len = strlen(first);
        snprintf(first + len, (size_t)first_size - len, "%s[%lu]=%08lx(src %d, xor %lx)",
                 shown ? " " : "", (unsigned long)i, (unsigned long)got[i], src,
                 src >= 0 ? (unsigned long)(i ^ (u32)src) : 0ul);
        shown++;
      }
    }
    wrong += (u32)bad;
    int best = 0;
    for (int k = 1; k < 16; k++) {
      best = votes[k] > votes[best] ? k : best;
    }
    map[t] = !bad ? '.' : (votes[best] * 2 >= bad ? "0123456789abcdef"[best] : '?');
  }
  map[16] = 0;
  return wrong;
}

/* two 32x32 results against each other: per tile '.', the tile of b it equals, or '?' */
static u32 result_diff(const u32* a, const u32* b, char* map) {
  u32 wrong = 0;
  for (int t = 0; t < 16; t++) {
    int bad = 0;
    for (int k = 0; k < 64; k++) {
      bad += a[t * 64 + k] != b[t * 64 + k];
    }
    wrong += (u32)bad;
    char c = '.';
    if (bad) {
      c = '?';
      for (int u = 0; u < 16; u++) {
        if (!memcmp(a + t * 64, b + u * 64, 256)) {
          c = "0123456789abcdef"[u];
          break;
        }
      }
    }
    map[t] = c;
  }
  map[16] = 0;
  return wrong;
}

static void set_layout(VpSet* s) {
  u8* m = s->mem;
  s->t_s = (u32*)m;
  s->t_c = (u32*)(m + VP_TEX);
  s->t_v = (u32*)(m + 2 * VP_TEX);
  s->t_l = (u32*)(m + 3 * VP_TEX);
  s->r_s = (u32*)(m + 4 * VP_TEX);
  s->r_sw = m + 5 * VP_TEX;
  s->t_stamp = (u32*)(s->r_sw + VP_SWEEP);
  fb_setup(&s->fb_s, s->t_s, VP_W);
  fb_setup(&s->fb_c, s->t_c, VP_W);
  fb_setup(&s->fb_v, s->t_v, VP_W);
  fb_setup(&s->fb_l, s->t_l, VP_W);
  fb_setup(&s->fb_stamp, s->t_stamp, VP_STAMP_W);
}

/* the next resident pool piece to sweep (up to VP_SWEEP bytes) */
static void sweep_pick(VpSet* s) {
  s->sw_pool = -1;
  LightLock_Lock(&s_lock);
  for (int tries = 0; tries < VP_POOLS; tries++) {
    VpPool* q = &s_pool[P.sw_pool % VP_POOLS];
    if (!q->vram || !q->hash || P.sw_off >= q->bytes) {
      P.sw_pool = (P.sw_pool + 1) % VP_POOLS;
      P.sw_off = 0;
      continue;
    }
    s->sw_pool = (int)(P.sw_pool % VP_POOLS);
    s->sw_off = P.sw_off;
    s->sw_vram = q->vram + P.sw_off;
    s->sw_frame_in = q->frame;
    for (u32 off = P.sw_off; off < q->bytes && s->sw_n < VP_SWEEP / VP_CHUNK; off += VP_CHUNK) {
      const u32 len = q->bytes - off < VP_CHUNK ? q->bytes - off : VP_CHUNK;
      s->sw_hash[s->sw_n] = q->hash[off / VP_CHUNK];
      s->sw_len[s->sw_n] = len;
      s->sw_n++;
    }
    P.sw_off += VP_SWEEP;
    break;
  }
  LightLock_Unlock(&s_lock);
}

static void probe_queue(VpSet* s, u32 frame, const CtrVprobeTex* live) {
  const u32 n = P.probes;
  s->frame = frame;
  s->stamp = (frame * 2654435761u) ^ 0x5bd1e995u;
  /* the copies run ahead of this frame's command list: the static texture every 4th probe, a
   * sweep piece every other one, the live texture's copy in between (one GX copy per probe) */
  s->dma_s = (n & 3) == 0;
  if (s->dma_s) {
    GX_TextureCopy((u32*)P.s_vram, 0, s->r_s, 0, VP_TEX, 8);
  }
  s->sw_n = 0;
  if (!(n & 1)) {
    sweep_pick(s);
  }
  if (s->sw_n) {
    u32 bytes = 0;
    for (int i = 0; i < s->sw_n; i++) {
      bytes += s->sw_len[i];
    }
    GX_TextureCopy((u32*)s->sw_vram, 0, (u32*)s->r_sw, 0, (bytes + 15) & ~15u, 8);
  }
  s->live = 0;
  if (P.live_state == 2) {
    s->live = 2;
    P.live_state = 3;
  } else if (P.live_state == 0 && (n & 1) && P.r_live && live && live->tex && live->bytes &&
             live->bytes <= VP_LIVE) {
    const C3D_Tex* g = live->tex;
    s->live = 1;
    P.live_state = 1;
    P.live_frame = frame;
    P.live_slot = live->slot;
    P.live_pool = live->pool;
    P.live_bytes = live->bytes;
    P.live_fmt = g->fmt;
    P.live_dim = g->dim;
    P.live_vram = g->data;
    P.live_mips = (n & 2) && live->levels > 1 ? live->levels - 1 : 0;
    GX_TextureCopy((u32*)g->data, 0, (u32*)P.r_live, 0, (live->bytes + 15) & ~15u, 8);
    const u32 etc1 = g->param & GPU_TEXTURE_ETC1_PARAM;
    tex_setup(&P.tex_v, g->data, g->dim, g->fmt, g->size, etc1, P.live_mips);
    tex_setup(&P.tex_l, P.r_live, g->dim, g->fmt, g->size, etc1, P.live_mips);
  }
  common_state(0, 0);
  draw_into(&s->fb_s, VP_W, &P.tex_s, P.orient);
  draw_into(&s->fb_c, VP_W, &P.tex_c, P.orient);
  if (s->live == 1) {
    draw_into(&s->fb_v, VP_W, &P.tex_v, P.orient);
  } else if (s->live == 2) {
    draw_into(&s->fb_l, VP_W, &P.tex_l, P.orient);
  }
  stamp_draw(s);
  s->state = 1;
  P.probes++;
}

static void probe_check(VpSet* s) {
  cache_invalidate(s->mem, VP_SET_BYTES);
  char map[17], first[200];
  u32 w;
  /* static: texture unit, copy engine, CPU; the control */
  int s_bad = 0;
  if ((w = pattern_diff(s->t_c, map, first, sizeof(first)))) {
    s_bad = 1;
    fail(F_C_TEX, "frame %lu: %lu of 1024 texels wrong; tiles %s; %s", (unsigned long)s->frame,
         (unsigned long)w, map, first);
  }
  if ((w = pattern_diff(s->t_s, map, first, sizeof(first)))) {
    s_bad = 1;
    fail(F_S_TEX, "frame %lu: %lu of 1024 texels wrong; tiles %s; %s; VRAM %08lx (pa %08lx)",
         (unsigned long)s->frame, (unsigned long)w, map, first, (unsigned long)(u32)P.s_vram,
         (unsigned long)osConvertVirtToPhys(P.s_vram));
  }
  if (s->dma_s && (w = pattern_diff(s->r_s, map, first, sizeof(first)))) {
    s_bad = 1;
    fail(F_S_DMA, "frame %lu: %lu of 1024 words wrong; tiles %s; %s", (unsigned long)s->frame,
         (unsigned long)w, map, first);
  }
  if (P.cpu_vram && s->dma_s && (s->frame & 0x1E) == 0) {
    static u32 cpu[VP_W * VP_W];
    for (int i = 0; i < VP_W * VP_W; i++) {
      cpu[i] = ((volatile const u32*)P.s_vram)[i];
    }
    if ((w = pattern_diff(cpu, map, first, sizeof(first)))) {
      s_bad = 1;
      fail(F_S_CPU, "frame %lu: %lu of 1024 words wrong; tiles %s; %s", (unsigned long)s->frame,
           (unsigned long)w, map, first);
    }
  }
  P.s_ok += !s_bad;
  /* sweep: copied pool VRAM against the upload */
  const u8* p = s->r_sw;
  for (int i = 0; i < s->sw_n; i++) {
    const u32 len = s->sw_len[i];
    const u32 want = s->sw_hash[i];
    if (want) {
      const u32 got = hash_bytes(p, len) | 1u;
      P.sw_chunks++;
      P.sw_bytes += len;
      if (got != want) {
        const u8* v = s->sw_vram + (u32)i * VP_CHUNK;
        u32 cpu = 0;
        if (P.cpu_vram) {
          static u8 tmp[VP_CHUNK];
          for (u32 k = 0; k < len / 4; k++) {
            ((u32*)tmp)[k] = ((volatile const u32*)v)[k];
          }
          cpu = hash_bytes(tmp, len) | 1u;
        }
        fail(F_SWEEP_DMA, "frame %lu: pool %d +%lu (%08lx, %s pa %08lx, %lu bytes, in since frame "
             "%lu): copy hash %08lx, CPU %08lx, upload %08lx; copy starts %08lx %08lx %08lx "
             "%08lx", (unsigned long)s->frame, s->sw_pool,
             (unsigned long)(s->sw_off + (u32)i * VP_CHUNK), (unsigned long)(u32)v,
             where((u32)v), (unsigned long)osConvertVirtToPhys(v), (unsigned long)len,
             (unsigned long)s->sw_frame_in, (unsigned long)got, (unsigned long)cpu,
             (unsigned long)want, (unsigned long)((const u32*)p)[0],
             (unsigned long)((const u32*)p)[1], (unsigned long)((const u32*)p)[2],
             (unsigned long)((const u32*)p)[3]);
      } else if (P.cpu_vram && i == 0 && (s->frame & 0x1E) == 0x10) {
        static u8 tmp[VP_CHUNK];
        const u8* v = s->sw_vram;
        for (u32 k = 0; k < len / 4; k++) {
          ((u32*)tmp)[k] = ((volatile const u32*)v)[k];
        }
        const u32 cpu = hash_bytes(tmp, len - (len & 3)) | 1u;
        const u32 dma = hash_bytes(p, len - (len & 3)) | 1u;
        if (cpu != dma) {
          fail(F_SWEEP_CPU, "frame %lu: pool %d +%lu (%08lx): CPU %08lx, copy %08lx = upload",
               (unsigned long)s->frame, s->sw_pool, (unsigned long)s->sw_off,
               (unsigned long)(u32)v, (unsigned long)cpu, (unsigned long)dma);
        }
      }
    }
    p += len;
  }
  /* live: the VRAM draw kept and the copy rewritten by the CPU; then the copy's draw */
  if (s->live == 1) {
    cache_invalidate(P.r_live, VP_LIVE);
    volatile u32* r = (volatile u32*)P.r_live;
    for (u32 k = 0; k < (P.live_bytes + 3) / 4; k++) {
      r[k] = r[k];
    }
    GSPGPU_FlushDataCache(P.r_live, VP_LIVE);
    memcpy(P.live_tv, s->t_v, sizeof(P.live_tv));
    P.live_state = 2;
  } else if (s->live == 2) {
    if ((w = result_diff(P.live_tv, s->t_l, map))) {
      fail(F_LIVE, "frame %lu: texture slot %d (pool %d) %ux%u fmt %lu %s, %08lx (%s pa %08lx) "
           "%lu bytes: %lu of 1024 pixels differ; tiles %s (the copy draw's tile they equal); "
           "VRAM draw %08lx %08lx, copy draw %08lx %08lx", (unsigned long)P.live_frame,
           P.live_slot, P.live_pool, (unsigned)(P.live_dim >> 16), (unsigned)(P.live_dim & 0xFFFF),
           (unsigned long)P.live_fmt, P.live_mips ? "mip levels" : "level 0",
           (unsigned long)(u32)P.live_vram, where((u32)P.live_vram),
           (unsigned long)osConvertVirtToPhys(P.live_vram), (unsigned long)P.live_bytes,
           (unsigned long)w, map, (unsigned long)P.live_tv[0], (unsigned long)P.live_tv[1],
           (unsigned long)s->t_l[0], (unsigned long)s->t_l[1]);
    } else {
      P.live_ok++;
    }
    P.live_state = 0;
  }
  s->state = 0;
}

/* ---------------- the shared font ---------------- */

static void font_init(void) {
  if (R_FAILED(fontEnsureMapped())) {
    printf("[vprobe] shared font: not mapped, no font check\n");
    return;
  }
  const CFNT_s* f = fontGetSystemFont();
  if (!f || (f->signature != 0x554E4643u && f->signature != 0x544E4643u) || f->fileSize < 4096 ||
      f->fileSize > 0x400000u) {
    printf("[vprobe] shared font: no CFNT header, no font check\n");
    return;
  }
  P.font = (const u8*)f;
  P.font_bytes = f->fileSize & ~3u;
  P.font_chunks = (P.font_bytes + VP_CHUNK - 1) / VP_CHUNK;
  P.font_hash = (u32*)malloc(sizeof(u32) * P.font_chunks);
  if (!P.font_hash) {
    P.font = NULL;
    return;
  }
  cache_invalidate(P.font, P.font_bytes);
  for (u32 i = 0; i < P.font_chunks; i++) {
    const u32 len = P.font_bytes - i * VP_CHUNK < VP_CHUNK ? P.font_bytes - i * VP_CHUNK : VP_CHUNK;
    P.font_hash[i] = hash_bytes(P.font + i * VP_CHUNK, len);
  }
  printf("[vprobe] shared font: %08lx (pa %08lx) %lu KB hashed\n", (unsigned long)(u32)P.font,
         (unsigned long)osConvertVirtToPhys(P.font), (unsigned long)(P.font_bytes / 1024));
}

static void font_step(void) {
  if (!P.font) {
    return;
  }
  const u32 per = VP_FONT_STEP / VP_CHUNK;
  const u32 i0 = P.font_cursor;
  const u32 i1 = i0 + per < P.font_chunks ? i0 + per : P.font_chunks;
  const u32 end = i1 * VP_CHUNK < P.font_bytes ? i1 * VP_CHUNK : P.font_bytes;
  cache_invalidate(P.font + i0 * VP_CHUNK, end - i0 * VP_CHUNK);
  for (u32 i = i0; i < i1; i++) {
    const u32 len = P.font_bytes - i * VP_CHUNK < VP_CHUNK ? P.font_bytes - i * VP_CHUNK : VP_CHUNK;
    const u32 h = hash_bytes(P.font + i * VP_CHUNK, len);
    if (h != P.font_hash[i]) {
      const u32* w = (const u32*)(P.font + i * VP_CHUNK);
      fail(F_FONT, "frame %lu: font +%lu (pa %08lx) changed; now starts %08lx %08lx %08lx %08lx",
           (unsigned long)s_frame, (unsigned long)(i * VP_CHUNK),
           (unsigned long)osConvertVirtToPhys(P.font + i * VP_CHUNK), (unsigned long)w[0],
           (unsigned long)w[1], (unsigned long)w[2], (unsigned long)w[3]);
      P.font_hash[i] = h;
    }
  }
  P.font_cursor = i1 >= P.font_chunks ? 0 : i1;
  P.font_sweeps += i1 >= P.font_chunks;
}

/* ---------------- boot, frames, exit ---------------- */

static int frame_sync(void) {
  C3D_FrameEnd(GX_CMDLIST_FLUSH);
  gxCmdQueue_s* q = (gxCmdQueue_s*)(void*)__C3D_Context;
  return gxCmdQueueWait(q, (s64)1000 * (s64)(SYSCLOCK_ARM11 / 1000)) ? 1 : 0;
}

void ctr_vprobe_init(const CtrVprobeIn* in) {
  if (!ctr_vprobe_on || P.ready) {
    return;
  }
  P.prog = in->prog;
  P.uloc = in->uloc_projection;
  P.cpu_vram = in->cpu_vram_read;
  const u32 need = VP_TEX + VP_ORIENT * 4 * sizeof(VpVtx) + VP_SETS * VP_SET_BYTES + VP_LIVE;
  ctr_linear_lock();
  int ok = linearSpaceFree() >= need + 512u * 1024u;
  if (ok) {
    P.pattern = (u32*)linearAlloc(VP_TEX);
    P.vb = (VpVtx*)linearAlloc(VP_ORIENT * 4 * sizeof(VpVtx));
    P.r_live = (u8*)linearAlloc(VP_LIVE);
    for (int k = 0; k < VP_SETS; k++) {
      P.set[k].mem = (u8*)linearAlloc(VP_SET_BYTES);
      ok = ok && P.set[k].mem;
    }
    ok = ok && P.pattern && P.vb && P.r_live;
  }
  if (ok && vramSpaceFree() >= VP_TEX + 64u * 1024u) {
    P.s_vram = vramAlloc(VP_TEX);
  }
  ctr_linear_unlock();
  if (!ok || !P.s_vram) {
    printf("[vprobe] no memory for the probes (linear %lu, VRAM %lu free): guard and font only\n",
           (unsigned long)linearSpaceFree(), (unsigned long)vramSpaceFree());
    ctr_vprobe_vram_add(P.s_vram, 0);
    font_init();
    return;
  }
  ctr_vprobe_vram_add(P.s_vram, 0);
  for (u32 i = 0; i < VP_W * VP_W; i++) {
    P.pattern[i] = pattern_word(i);
  }
  /* corners for a strip, texture coordinates per orientation: o&1 swaps s and t, o&2 / o&4 flip */
  static const float cx[4] = {-1.0f, 1.0f, -1.0f, 1.0f}, cy[4] = {-1.0f, -1.0f, 1.0f, 1.0f};
  for (int o = 0; o < VP_ORIENT; o++) {
    for (int k = 0; k < 4; k++) {
      VpVtx* v = &P.vb[o * 4 + k];
      float s = (cx[k] + 1.0f) * 0.5f, t = (cy[k] + 1.0f) * 0.5f;
      if (o & 1) {
        const float x = s;
        s = t;
        t = x;
      }
      if (o & 2) {
        s = 1.0f - s;
      }
      if (o & 4) {
        t = 1.0f - t;
      }
      v->x = cx[k];
      v->y = cy[k];
      v->z = 0.0f;
      v->s = s;
      v->t = t;
      v->r = v->g = v->b = v->a = 255;
    }
  }
  for (int k = 0; k < VP_SETS; k++) {
    memset(P.set[k].mem, 0, VP_SET_BYTES);
    set_layout(&P.set[k]);
  }
  GSPGPU_FlushDataCache(P.pattern, VP_TEX);
  GSPGPU_FlushDataCache(P.vb, VP_ORIENT * 4 * sizeof(VpVtx));
  GSPGPU_FlushDataCache(P.r_live, VP_LIVE);
  for (int k = 0; k < VP_SETS; k++) {
    GSPGPU_FlushDataCache(P.set[k].mem, VP_SET_BYTES);
  }
  const u32 dim = ((u32)VP_W << 16) | VP_W;
  tex_setup(&P.tex_s, P.s_vram, dim, GPU_RGBA8, VP_TEX, 0, 0);
  tex_setup(&P.tex_c, P.pattern, dim, GPU_RGBA8, VP_TEX, 0, 0);
  /* calibration: the control drawn in each orientation, the one that gives the pattern back */
  C3D_FrameBuf* fbs[VP_ORIENT] = {&P.set[0].fb_s, &P.set[0].fb_c, &P.set[0].fb_v,
                                  &P.set[0].fb_l, &P.set[1].fb_s, &P.set[1].fb_c,
                                  &P.set[1].fb_v, &P.set[1].fb_l};
  u32* outs[VP_ORIENT] = {P.set[0].t_s, P.set[0].t_c, P.set[0].t_v, P.set[0].t_l,
                          P.set[1].t_s, P.set[1].t_c, P.set[1].t_v, P.set[1].t_l};
  int synced = 0;
  if (C3D_FrameBegin(0)) {
    GX_TextureCopy(P.pattern, 0, (u32*)P.s_vram, 0, VP_TEX, 8);
    common_state(0, 0);
    for (int o = 0; o < VP_ORIENT; o++) {
      draw_into(fbs[o], VP_W, &P.tex_c, o);
    }
    synced = frame_sync();
  }
  P.orient = -1;
  char txt[200] = "";
  for (int o = 0; o < VP_ORIENT; o++) {
    cache_invalidate(outs[o], VP_TEX);
    char map[17], first[160];
    const u32 w = pattern_diff(outs[o], map, first, sizeof(first));
    const size_t len = strlen(txt);
    snprintf(txt + len, sizeof(txt) - len, " %d:%lu", o, (unsigned long)w);
    if (!w && P.orient < 0) {
      P.orient = o;
    }
  }
  const int calibrated = P.orient >= 0;
  if (!calibrated) {
    P.orient = 0;
  }
  for (int k = 0; k < VP_SETS; k++) {
    memset(P.set[k].mem, 0, VP_SET_BYTES);
    GSPGPU_FlushDataCache(P.set[k].mem, VP_SET_BYTES);
  }
  /* the first probe, waited for */
  int boot_ok = 0;
  if (synced && C3D_FrameBegin(0)) {
    probe_queue(&P.set[0], 0, NULL);
    if (frame_sync() && stamp_landed(&P.set[0])) {
      const u32 before = P.s_ok;
      probe_check(&P.set[0]);
      boot_ok = P.s_ok > before;
    }
    P.set[0].state = 0;
  }
  font_init();
  P.ready = calibrated;
  P.last_line = svcGetSystemTick();
  printf("[vprobe] on: static texture at %08lx (%s pa %08lx), control at %08lx; orientation %d "
         "(%s; wrong texels per orientation:%s); boot probe %s; CPU reads VRAM: %s\n",
         (unsigned long)(u32)P.s_vram, where((u32)P.s_vram),
         (unsigned long)osConvertVirtToPhys(P.s_vram), (unsigned long)(u32)P.pattern, P.orient,
         calibrated ? "calibrated" : "NOT calibrated: probes off, guard and font only", txt,
         boot_ok ? "passed" : "FAILED", P.cpu_vram ? "yes" : "no");
  report_pending();
}

void ctr_vprobe_frame(u32 frame, const CtrVprobeTex* live) {
  if (!ctr_vprobe_on) {
    return;
  }
  const u64 t0 = svcGetSystemTick();
  s_frame = frame;
  if (P.ready && (frame & 1)) {
    VpSet* freeset = NULL;
    for (int k = 0; k < VP_SETS; k++) {
      VpSet* s = &P.set[k];
      if (s->state == 1) {
        const u64 c0 = svcGetSystemTick();
        if (stamp_landed(s)) {
          probe_check(s);
          P.t_check += svcGetSystemTick() - c0;
        } else if (frame - s->frame > 30) {
          /* the frame's command list was dropped (a frame begun and never ended) */
          P.dropped++;
          s->state = 0;
          if (s->live) {
            P.live_state = 0;
          }
        }
      }
      if (s->state == 0 && !freeset) {
        freeset = s;
      }
    }
    if (freeset) {
      const u64 q0 = svcGetSystemTick();
      probe_queue(freeset, frame, live);
      P.t_queue += svcGetSystemTick() - q0;
    }
  }
  if (!(frame & 1)) {
    const u64 f0 = svcGetSystemTick();
    font_step();
    P.t_font += svcGetSystemTick() - f0;
  }
  report_pending();
  const u64 now = svcGetSystemTick();
  P.ticks += now - t0;
  P.ticks_n++;
  if (now - P.last_line >= 30ull * SYSCLOCK_ARM11) {
    P.last_line = now;
    u32 total = 0;
    for (int c = 0; c < F_N; c++) {
      total += P.fails[c];
    }
    printf("[vprobe] %s frame %lu: %lu probes (%lu dropped, static ok %lu, live ok %lu), swept %lu chunks "
           "(%lu KB), font sweeps %lu, GPU writes checked %lu, VRAM allocations %d, texture "
           "checks %lu; failures %lu; CPU %.0f us/frame (check %.0f, queue %.0f, font %.0f)\n",
           total ? "FAILING" : "passing", (unsigned long)frame, (unsigned long)P.probes,
           (unsigned long)P.dropped,
           (unsigned long)P.s_ok, (unsigned long)P.live_ok, (unsigned long)P.sw_chunks,
           (unsigned long)(P.sw_bytes / 1024), (unsigned long)P.font_sweeps,
           (unsigned long)P.writes, s_nva, (unsigned long)P.texchecks, (unsigned long)total,
           P.ticks_n ? (double)P.ticks / (double)P.ticks_n * 1e6 / SYSCLOCK_ARM11 : 0.0,
           P.ticks_n ? (double)P.t_check / (double)P.ticks_n * 1e6 / SYSCLOCK_ARM11 : 0.0,
           P.ticks_n ? (double)P.t_queue / (double)P.ticks_n * 1e6 / SYSCLOCK_ARM11 : 0.0,
           P.ticks_n ? (double)P.t_font / (double)P.ticks_n * 1e6 / SYSCLOCK_ARM11 : 0.0);
  }
}

void ctr_vprobe_exit(void) {
  if (!ctr_vprobe_on) {
    return;
  }
  char txt[400] = "";
  for (int c = 0; c < F_N; c++) {
    if (P.fails[c]) {
      const size_t len = strlen(txt);
      snprintf(txt + len, sizeof(txt) - len, "%s%s x%lu", len ? ", " : "", kFail[c],
               (unsigned long)P.fails[c]);
    }
  }
  printf("[vprobe] exit: %lu probes (static ok %lu, live ok %lu), %lu KB swept, %lu font sweeps, "
         "%lu GPU writes checked; %s%s; first failure at frame %lu; CPU %.0f us/frame\n",
         (unsigned long)P.probes, (unsigned long)P.s_ok, (unsigned long)P.live_ok,
         (unsigned long)(P.sw_bytes / 1024), (unsigned long)P.font_sweeps, (unsigned long)P.writes,
         txt[0] ? "FAILURES: " : "no failures", txt, (unsigned long)P.first_frame,
         P.ticks_n ? (double)P.ticks / (double)P.ticks_n * 1e6 / SYSCLOCK_ARM11 : 0.0);
}
