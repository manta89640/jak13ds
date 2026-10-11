/*
 * (AI-assisted)
 * GPU timing instrumentation of the 3DS renderer (ctr_gpu_bench.h; 3DS only, next to
 * ctr_gpu_citro3d.c). Everything here is off unless config.ini asks for it, and it only talks to
 * the GPU through the GSP service and citro3d (no system calls of its own, no IO mappings:
 * platform/3ds/tools/check_svc.py, gk.rsf).
 *
 *  - gpu_perflog: the GSP's performance log, one line per stats window.
 *  - pica_bench: a microbenchmark of the GPU before the game starts (below).
 */

#include "game/graphics/ctr/ctr_gpu_bench.h"

#include <3ds.h>
#include <citro3d.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* citro3d's context starts with its GX queue (source/internal.h; ctr_gpu_citro3d.c checks the
 * whole layout against the library) */
typedef struct {
  gxCmdQueue_s gxQueue;
  u32* cmdBuf;
  size_t cmdBufSize;
  float cmdBufUsage;
  u32 flags;
} BenchC3DHead;
_Static_assert(offsetof(BenchC3DHead, flags) == 0x20, "citro3d context layout");
extern char __C3D_Context[];
static BenchC3DHead* c3d_head(void) {
  return (BenchC3DHead*)(void*)__C3D_Context;
}

/* ---------------- config.ini gpu_perflog ----------------
 * GSPGPU_SetPerfLogMode(1) makes the GSP record, for each GPU engine, the time from starting an
 * operation to its interrupt (libctru's gspgpu.h: lastDurationUs / totalDurationUs; 3dbrew says
 * "delta time" without a unit). P3D = command lists, PPF = display transfers and texture copies,
 * PSC0 / PSC1 = the two memory fill units (a fill of color and depth uses both at once), PDC0 =
 * the time between two top screen vblanks. The vblank interval (16.71 ms) calibrates the unit,
 * which is disputed: the line logs the raw numbers too. Read on the render thread at each stats
 * window (one GSP request). */
enum { PL_PSC0 = 0, PL_PSC1, PL_VBLANK0, PL_VBLANK1, PL_PPF, PL_P3D, PL_DMA, PL_COUNT };
_Static_assert(GSPGPU_EVENT_PSC0 == PL_PSC0 && GSPGPU_EVENT_P3D == PL_P3D &&
                   GSPGPU_EVENT_MAX == PL_COUNT,
               "libctru's GSPGPU_Event order");
static struct {
  int on;      /* started (the GSP accepted the mode) */
  int primed;  /* last holds a starting point */
  GSPGPU_PerfLog last;
  u64 last_tick;
} s_pl;

/* the GPU idle: citro3d's queue finished (at most 1 s) */
static int wait_gpu_idle(s64 timeout_ns) {
  return gxCmdQueueWait(&c3d_head()->gxQueue, timeout_ns) ? 1 : 0;
}

int ctr_gpu_perflog_start(void) {
  if (s_pl.on) {
    return 0;
  }
  /* the GSP assumes no GPU operation in progress when the mode changes (libctru) */
  wait_gpu_idle(1000000000LL);
  const Result r = GSPGPU_SetPerfLogMode(true);
  if (R_FAILED(r)) {
    printf("[ctr] gpu perflog: the GSP refused the performance log (%08lx)\n", (unsigned long)r);
    return r ? (int)r : -1;
  }
  s_pl.on = 1;
  s_pl.primed = R_SUCCEEDED(GSPGPU_GetPerfLog(&s_pl.last)) ? 1 : 0;
  s_pl.last_tick = svcGetSystemTick();
  return 0;
}

/* raw GSP units per millisecond, from the vblank interval (59.83 Hz) when it reads as
 * microseconds (libctru) or nanoseconds (3dbrew's possibility); else microseconds */
static double perflog_units_per_ms(const GSPGPU_PerfLog* p) {
  const u32 v = p->entries[PL_VBLANK0].lastDurationUs;
  if ((v > 12000u && v < 22000u) || (v > 12000000u && v < 22000000u)) {
    return (double)v / 16.7128;
  }
  return 1000.0;
}

int ctr_gpu_perflog_line(char* out, int size, int frames, double draw_ms_sum, double busy_ms_sum,
                         int busy_frames) {
  if (!s_pl.on || size <= 0) {
    return 0;
  }
  GSPGPU_PerfLog now;
  if (R_FAILED(GSPGPU_GetPerfLog(&now))) {
    return 0;
  }
  const u64 tick = svcGetSystemTick();
  if (!s_pl.primed) {
    s_pl.last = now;
    s_pl.last_tick = tick;
    s_pl.primed = 1;
    return 0;
  }
  u32 d[PL_COUNT];
  for (int i = 0; i < PL_COUNT; i++) {
    d[i] = now.entries[i].totalDurationUs - s_pl.last.entries[i].totalDurationUs;
  }
  const double window_ms = (double)(tick - s_pl.last_tick) / (SYSCLOCK_ARM11 / 1000.0);
  s_pl.last = now;
  s_pl.last_tick = tick;
  const double n = frames > 0 ? frames : 1;
  const double upm = perflog_units_per_ms(&now);
  const double p3d = d[PL_P3D] / upm / n, ppf = d[PL_PPF] / upm / n;
  const double psc = (d[PL_PSC0] > d[PL_PSC1] ? d[PL_PSC0] : d[PL_PSC1]) / upm / n;
  const double busy = busy_frames > 0 ? busy_ms_sum / busy_frames : 0.0;
  int len = snprintf(
      out, (size_t)size,
      "[ctr] gpu perflog: %d frames in %.0f ms; raw GSP units per frame: p3d %.0f ppf %.0f "
      "psc0 %.0f psc1 %.0f dma %.0f; vblank0 last %lu raw (1 ms = %.1f raw); ms/frame: p3d %.2f "
      "ppf %.2f psc %.2f, c3d draw %.2f, fence busy %s",
      frames, window_ms, d[PL_P3D] / n, d[PL_PPF] / n, d[PL_PSC0] / n, d[PL_PSC1] / n,
      d[PL_DMA] / n, (unsigned long)now.entries[PL_VBLANK0].lastDurationUs, upm, p3d, ppf, psc,
      draw_ms_sum / n, busy_frames > 0 ? "" : "n/a (needs frame_pacing = steady)");
  if (busy_frames > 0 && len > 0 && len < size) {
    len += snprintf(out + len, (size_t)(size - len),
                    "%.2f -> bubble %.2f (busy - p3d - ppf - max psc)", busy,
                    busy - (p3d + ppf + psc));
  }
  return len < size ? len : size - 1;
}

/* ======================= config.ini pica_bench =======================
 * A boot-time GPU microbenchmark (~20-60 s before the game starts; docs/3ds-port/performance.md
 * "GPU timing in one session"). It runs on the thread that called ctr_init, right after
 * ctr_gpu_init, with citro3d's context and command buffer but its own render targets, buffers,
 * textures and programs, and logs one `[bench] <test> key=value ...` line per test
 * (platform/3ds/tools/bench_report.py turns them into a table). Each line also goes to
 * sdmc:/3ds/jak1/data/log/bench.txt, opened and closed per line like boot.txt, with a
 * `start <test>` line before the test: if the GPU hangs, the last `start` names the test.
 * The tests that could hang the GPU run last (the bench's own shaders, then the unproven new
 * ctr_mesh and ctr_skin). A GPU queue that doesn't finish within 3 s stops the bench.
 *
 * Measurement of one test: the test's command list is built once and queued `reps` times back to
 * back (no CPU gaps), together with any fills / transfers it needs; then
 *  - wall: citro3d's queue timer (start of the queue to its finish callback), per rep, ms
 *  - p3d / ppf / psc0 / psc1: the GSP performance log's total time per engine, per rep, raw units
 *    (the header line's upm = raw units per ms), p3dl: the last command list alone
 *  - hs: the hotspot sampler's 8 stage counts over the run (8192 GPU cycles per sample), hsx: the
 *    samples the elapsed time should give (the counts' sum; the idle tests show where idle goes)
 *  - tr: the traffic counters' deltas per rep (index:count, nonzero only; B1 calibrates them)
 */

#include "platform/3ds/port/ctr_port.h"

extern const uint8_t ctr_basic_shbin[];
extern const size_t ctr_basic_shbin_size;
extern const uint8_t ctr_clip2_shbin[];
extern const size_t ctr_clip2_shbin_size;
extern const uint8_t ctr_mesh_shbin[];
extern const size_t ctr_mesh_shbin_size;
extern const uint8_t ctr_skin_shbin[];
extern const size_t ctr_skin_shbin_size;
extern const uint8_t ctr_bench_shbin[];
extern const size_t ctr_bench_shbin_size;
extern const uint8_t ctr_bench_sep_shbin[];
extern const size_t ctr_bench_sep_shbin_size;
void ctr_linear_lock(void);
void ctr_linear_unlock(void);
int ctr_gpu_is_emulator(void);
/* citro3d internals (exported by libcitro3d.a; source/internal.h) */
bool C3Di_SplitFrame(u32** pBuf, u32* pSize);
void C3Di_UpdateContext(void);
void C3Di_SetTex(int unit, C3D_Tex* tex);

#define BENCH_FILE "sdmc:/3ds/jak1/data/log/bench.txt"
#define BENCH_TIMEOUT_NS 3000000000LL
#define BENCH_QUEUE_MAX 30 /* citro3d's GX queue has 32 entries (more is a panic) */
#define HS_INTERVAL 8192u  /* GPU cycles per hotspot sample: 0xFFFF samples = 2 s */
#define HWREG_HS_ENABLE 0x400064u
#define HWREG_HS_CONFIG 0x400068u
#define HWREG_HS_FIFO 0x40006Cu
#define HWREG_TRAFFIC 0x400070u
#define HWREG_POST_VERTEX_CACHE_NUM (0x401000u + 0x22Du * 4u)
#define NTRAFFIC 20

/* targets: T0 240x400 RGBA8 + D24S8 (the game's), T1 the same with D16, T2 256x416 block32 */
#define TW 240
#define TH 400
#define B32W 256
#define B32H 416

typedef struct {
  float x, y, z, s, t;
  u8 r, g, b, a;
} VtxF; /* ctr_basic's immediate layout */
typedef struct {
  s16 x, y, z, w;
  s16 s, t;
  u8 r, g, b, a;
} VtxS; /* the level mesh layout (c3l::Vertex) */
typedef struct {
  s8 x, y, z, w;
  s8 s, t;
  u8 r, g, b, a;
} VtxB;
typedef struct {
  float x, y, z, w, s0, t0, s1, t1;
  u8 r, g, b, a;
} VtxC2; /* ctr_clip2 */
typedef struct {
  s16 x, y, z;
  u8 idx[3], wgt[3];
  s16 s, t;
  u8 r, g, b, a;
  s8 nx, ny, nz;
  u8 pad;
} VtxSkin; /* c3l::MercVertex */
_Static_assert(sizeof(VtxF) == 24 && sizeof(VtxS) == 16 && sizeof(VtxB) == 10 &&
                   sizeof(VtxC2) == 36 && sizeof(VtxSkin) == 24,
               "vertex layouts");

enum { L_FLOAT, L_SHORT, L_BYTE, L_CLIP2, L_SKIN };

typedef struct {
  GSPGPU_PerfLog pl0, pl1;
  u32 tr0[NTRAFFIC], tr1[NTRAFFIC];
  u32 hs[8];
  u64 t0, t1;
  float wall_ms;
  int pl_ok, tr_ok, hs_ok;
} Meas;

typedef struct {
  DVLB_s* dvlb;
  shaderProgram_s prog;
  int ok;
} Prog;

static struct {
  int dead;      /* the GPU didn't finish: nothing more is run */
  int hw_ok;     /* the GSP accepts the hardware register requests */
  int pl_ok;     /* the performance log is on */
  int queued;    /* GX queue entries this frame */
  u64 start_tick;
  int tests;
  C3D_RenderTarget *t0, *t1, *t2;
  u32* rb;       /* readback: B32W * B32H RGBA8, linear */
  u8* scratch;   /* display transfer output like the screen's: TW * TH RGB8, linear */
  u32* fence_src; /* 16 bytes each: the game's fence copy */
  u32* fence_dst;
  u8* imm;       /* small vertex buffers (quads, single triangles) */
  u32 imm_used;
  VtxS* cull_s;  /* 30000 vertices: 10000 back facing triangles (B3, B5) */
  int cull_mode; /* GPU_CULL_* that culls them all (checked) */
  C3D_Tex tex[6]; /* RGBA8, RGB565, ETC1 at 256x256; the same with all mip levels */
  int tex_ok[6];
  Prog basic, clip2, mesh, skin, bench, sep;
  shaderProgram_s bench_entry[8]; /* the bench DVLB's entry points (shared code) */
  int bench_entry_ok[8];
  C3D_Mtx m_px, m_geo;
  float upm;     /* raw perflog units per ms (from the vblank interval) */
  u32 vblank_raw; /* the vblank interval in raw units */
  char status[96];
} B;

#define IMM_BYTES (256 * 1024)
#define CULL_VERTS 30000
#define CULL_TRIS (CULL_VERTS / 3)
#define BIG_TRIS 100000 /* per vertex test: 10 draws of CULL_TRIS */

/* ---------------- logging ---------------- */

static void bench_file(const char* line) {
  FILE* f = fopen(BENCH_FILE, "a");
  if (f) {
    fputs(line, f);
    fputc('\n', f);
    fclose(f);
  }
}

static void bench_emit(const char* line) {
  bench_file(line);
  char buf[1024];
  const int n = snprintf(buf, sizeof(buf), "%s\n", line);
  if (n <= 0) {
    return;
  }
  if (ctr_log_async()) {
    ctr_log_write(CTR_LOG_FILE | CTR_LOG_STDOUT, buf,
                  (unsigned)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1), 0);
  } else {
    fputs(buf, stdout);
  }
}

static void bench_emitf(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
static void bench_emitf(const char* fmt, ...) {
  char line[900];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  bench_emit(line);
}

/* before a test: its name in bench.txt (on the card before the GPU sees anything) and on the
 * bottom screen */
static void bench_start(const char* id) {
  char line[96];
  snprintf(line, sizeof(line), "start %s", id);
  bench_file(line);
  snprintf(B.status, sizeof(B.status), "pica_bench %d tests, %.0f s: %s", B.tests,
           (double)(svcGetSystemTick() - B.start_tick) / SYSCLOCK_ARM11, id);
  ctr_console_status(B.status);
  B.tests++;
}

/* ---------------- GPU counters ---------------- */

static void hs_start(void) {
  if (!B.hw_ok) {
    return;
  }
  const u32 off = 0, on = 1, config = (HS_INTERVAL - 1u) << 16; /* count 0: until read */
  if (R_FAILED(GSPGPU_WriteHWRegs(HWREG_HS_ENABLE, &off, 4)) ||
      R_FAILED(GSPGPU_WriteHWRegs(HWREG_HS_CONFIG, &config, 4)) ||
      R_FAILED(GSPGPU_WriteHWRegs(HWREG_HS_ENABLE, &on, 4))) {
    B.hw_ok = 0;
  }
}

static int hs_read(u32 hs[8]) {
  memset(hs, 0, 8 * sizeof(u32));
  if (!B.hw_ok) {
    return 0;
  }
  for (int i = 0; i < 4; i++) {
    u32 w = 0;
    if (R_FAILED(GSPGPU_ReadHWRegs(HWREG_HS_FIFO, &w, 4))) {
      B.hw_ok = 0;
      return 0;
    }
    hs[2 * i] = w & 0xFFFFu;
    hs[2 * i + 1] = w >> 16;
  }
  const u32 off = 0;
  GSPGPU_WriteHWRegs(HWREG_HS_ENABLE, &off, 4);
  return 1;
}

static void meas_begin(Meas* m) {
  memset(m, 0, sizeof(*m));
  m->pl_ok = B.pl_ok && R_SUCCEEDED(GSPGPU_GetPerfLog(&m->pl0));
  m->tr_ok = B.hw_ok && R_SUCCEEDED(GSPGPU_ReadHWRegs(HWREG_TRAFFIC, m->tr0, sizeof(m->tr0)));
  hs_start();
  m->t0 = svcGetSystemTick();
}

static void meas_end(Meas* m) {
  m->hs_ok = hs_read(m->hs);
  m->t1 = svcGetSystemTick();
  m->tr_ok = m->tr_ok && R_SUCCEEDED(GSPGPU_ReadHWRegs(HWREG_TRAFFIC, m->tr1, sizeof(m->tr1)));
  m->pl_ok = m->pl_ok && R_SUCCEEDED(GSPGPU_GetPerfLog(&m->pl1));
}

/* ---------------- frames and the GX queue ---------------- */

/* a frame of citro3d's (the queue stopped: entries wait for bench_run) */
static void fb_begin(C3D_RenderTarget* t) {
  C3D_FrameBegin(0);
  B.queued = 0;
  if (t) {
    C3D_FrameDrawOn(t);
  }
}

static int queue_room(int n) {
  return B.queued + n <= BENCH_QUEUE_MAX;
}

/* the commands built so far as one list (NULL: none) */
static u32* split_list(u32* words) {
  u32* list = NULL;
  *words = 0;
  if (!C3Di_SplitFrame(&list, words) || !*words) {
    return NULL;
  }
  return list;
}

static void queue_list(u32* list, u32 words, int reps) {
  for (int r = 0; list && r < reps && queue_room(1); r++) {
    GX_ProcessCommandList(list, words * 4, r == 0 ? GX_CMDLIST_FLUSH : 0);
    B.queued++;
  }
}

static void queue_clear(C3D_RenderTarget* t, u32 color, u32 depth) {
  if (queue_room(1)) {
    C3D_RenderTargetClear(t, C3D_CLEAR_ALL, color, depth);
    B.queued++;
  }
}

/* the frame's queue runs now; waits for it (3 s at most: else the bench stops) */
static int bench_run(Meas* m) {
  Meas dummy;
  if (!m) {
    m = &dummy;
  }
  meas_begin(m);
  C3D_FrameEnd(GX_CMDLIST_FLUSH);
  const int ok = gxCmdQueueWait(&c3d_head()->gxQueue, BENCH_TIMEOUT_NS) ? 1 : 0;
  meas_end(m);
  m->wall_ms = C3D_GetDrawingTime();
  if (!ok) {
    B.dead = 1;
    bench_emit("[bench] HANG: the GPU queue didn't finish within 3 s; the bench stops here "
               "(the test is the last `start` line in bench.txt)");
    ctr_log_sync();
  }
  return ok;
}

/* the build of the current frame's commands as one list, queued reps times, then run */
static int run_list(int reps, Meas* m) {
  u32 words;
  u32* list = split_list(&words);
  queue_list(list, words, reps);
  return bench_run(m);
}

/* one line: id, items, reps, the measurement, extra key=value text */
static void put_meas(const char* id, int n, int reps, const Meas* m, const char* extra) {
  char line[900];
  const double r = reps > 0 ? reps : 1;
  int len = snprintf(line, sizeof(line), "[bench] %s n=%d reps=%d wall=%.4f", id, n, reps,
                     m->wall_ms / r);
  if (m->pl_ok) {
    u32 d[PL_COUNT];
    for (int i = 0; i < PL_COUNT; i++) {
      d[i] = m->pl1.entries[i].totalDurationUs - m->pl0.entries[i].totalDurationUs;
    }
    len += snprintf(line + len, sizeof(line) - (size_t)len,
                    " p3d=%.0f ppf=%.0f psc0=%.0f psc1=%.0f p3dl=%lu", d[PL_P3D] / r,
                    d[PL_PPF] / r, d[PL_PSC0] / r, d[PL_PSC1] / r,
                    (unsigned long)m->pl1.entries[PL_P3D].lastDurationUs);
  }
  if (m->hs_ok) {
    len += snprintf(line + len, sizeof(line) - (size_t)len,
                    " hs=%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu hsx=%lu", (unsigned long)m->hs[0],
                    (unsigned long)m->hs[1], (unsigned long)m->hs[2], (unsigned long)m->hs[3],
                    (unsigned long)m->hs[4], (unsigned long)m->hs[5], (unsigned long)m->hs[6],
                    (unsigned long)m->hs[7], (unsigned long)((m->t1 - m->t0) / HS_INTERVAL));
  }
  if (m->tr_ok) {
    len += snprintf(line + len, sizeof(line) - (size_t)len, " tr=");
    int first = 1;
    for (int i = 0; i < NTRAFFIC && len < (int)sizeof(line) - 24; i++) {
      const u32 d = m->tr1[i] - m->tr0[i];
      if (d) {
        len += snprintf(line + len, sizeof(line) - (size_t)len, "%s%d:%.0f", first ? "" : ",", i,
                        d / r);
        first = 0;
      }
    }
    if (first) {
      len += snprintf(line + len, sizeof(line) - (size_t)len, "-");
    }
  }
  if (extra && *extra && len < (int)sizeof(line) - 2) {
    snprintf(line + len, sizeof(line) - (size_t)len, " %s", extra);
  }
  bench_emit(line);
}

/* ---------------- resources ---------------- */

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

/* a raw copy of n bytes (multiple of 16) through the GX queue, timed */
static int gx_copy(const void* src, void* dst, u32 n) {
  fb_begin(NULL);
  GSPGPU_FlushDataCache(src, n);
  GX_TextureCopy((u32*)src, 0, (u32*)dst, 0, n, 8);
  B.queued++;
  return bench_run(NULL);
}

static int prog_load(Prog* p, const uint8_t* shbin, size_t size, int entry) {
  p->dvlb = DVLB_ParseFile((u32*)shbin, (u32)size);
  if (!p->dvlb || entry >= (int)p->dvlb->numDVLE) {
    return 0;
  }
  shaderProgramInit(&p->prog);
  shaderProgramSetVsh(&p->prog, &p->dvlb->DVLE[entry]);
  p->ok = 1;
  return 1;
}

static void prog_free(Prog* p) {
  if (p->ok) {
    shaderProgramFree(&p->prog);
  }
  if (p->dvlb) {
    DVLB_Free(p->dvlb);
  }
  memset(p, 0, sizeof(*p));
}

static int uloc(shaderProgram_s* p, const char* name) {
  return shaderInstanceGetUniformLocation(p->vertexShader, name);
}

static void attrs(int layout) {
  C3D_AttrInfo* a = C3D_GetAttrInfo();
  AttrInfo_Init(a);
  switch (layout) {
    case L_FLOAT:
      AttrInfo_AddLoader(a, 0, GPU_FLOAT, 3);
      AttrInfo_AddLoader(a, 1, GPU_FLOAT, 2);
      AttrInfo_AddLoader(a, 2, GPU_UNSIGNED_BYTE, 4);
      break;
    case L_SHORT:
      AttrInfo_AddLoader(a, 0, GPU_SHORT, 4);
      AttrInfo_AddLoader(a, 1, GPU_SHORT, 2);
      AttrInfo_AddLoader(a, 2, GPU_UNSIGNED_BYTE, 4);
      break;
    case L_BYTE:
      AttrInfo_AddLoader(a, 0, GPU_BYTE, 4);
      AttrInfo_AddLoader(a, 1, GPU_BYTE, 2);
      AttrInfo_AddLoader(a, 2, GPU_UNSIGNED_BYTE, 4);
      break;
    case L_CLIP2:
      AttrInfo_AddLoader(a, 0, GPU_FLOAT, 4);
      AttrInfo_AddLoader(a, 1, GPU_FLOAT, 2);
      AttrInfo_AddLoader(a, 2, GPU_FLOAT, 2);
      AttrInfo_AddLoader(a, 3, GPU_UNSIGNED_BYTE, 4);
      break;
    case L_SKIN:
      AttrInfo_AddLoader(a, 0, GPU_SHORT, 3);
      AttrInfo_AddLoader(a, 1, GPU_UNSIGNED_BYTE, 3);
      AttrInfo_AddLoader(a, 2, GPU_UNSIGNED_BYTE, 3);
      AttrInfo_AddLoader(a, 3, GPU_SHORT, 2);
      AttrInfo_AddLoader(a, 4, GPU_UNSIGNED_BYTE, 4);
      AttrInfo_AddLoader(a, 5, GPU_BYTE, 3);
      break;
  }
}

static void vbuf(const void* data, int layout) {
  static const int stride[] = {sizeof(VtxF), sizeof(VtxS), sizeof(VtxB), sizeof(VtxC2),
                               sizeof(VtxSkin)};
  static const int count[] = {3, 3, 3, 4, 6};
  static const u64 perm[] = {0x210, 0x210, 0x210, 0x3210, 0x543210};
  C3D_BufInfo* b = C3D_GetBufInfo();
  BufInfo_Init(b);
  BufInfo_Add(b, data, stride[layout], count[layout], perm[layout]);
}

/* pixel coordinates of a W x H target (x, y; z = depth 0..1, larger = closer) -> PICA clip */
static void mtx_px(C3D_Mtx* m, float w, float h) {
  Mtx_Zeros(m);
  m->r[0].x = 2.0f / w;
  m->r[0].w = -1.0f;
  m->r[1].y = 2.0f / h;
  m->r[1].w = -1.0f;
  m->r[2].z = -1.0f;
  m->r[3].w = 1.0f;
}

/* the vertex tests' space: x, y in -128..128 cover the target, depth 0.5 */
static void mtx_geo(C3D_Mtx* m) {
  Mtx_Zeros(m);
  m->r[0].x = 1.0f / 128.0f;
  m->r[1].y = 1.0f / 128.0f;
  m->r[2].w = -0.5f;
  m->r[3].w = 1.0f;
}

/* ctr_basic with a matrix */
static void use_basic(const C3D_Mtx* m, int layout) {
  C3D_BindProgram(&B.basic.prog);
  attrs(layout);
  C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uloc(&B.basic.prog, "projection"), m);
}

/* a bench entry point (shared DVLB) or the separate DVLB's (entry -1) */
static void use_bench(int entry, const C3D_Mtx* m) {
  shaderProgram_s* p = entry < 0 ? &B.sep.prog : &B.bench_entry[entry];
  C3D_BindProgram(p);
  attrs(L_SHORT);
  C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uloc(p, "bproj"), m);
  C3D_FVUnifSet(GPU_VERTEX_SHADER, uloc(p, "bscales"), 1.0f / 1024.0f, 1.0f / 255.0f, 1.0f, 1.0f);
}

/* texture combiners: stage 0 = vertex color (tex 0: texture 0 x vertex color), `stages` stages
 * in use (the others pass the previous one through, as citro3d's default), tex1: stage 1
 * multiplies by texture 1 */
static void combiners(int tex0, int stages, int tex1) {
  for (int i = 0; i < 6; i++) {
    C3D_TexEnv* e = C3D_GetTexEnv(i);
    C3D_TexEnvInit(e);
    if (i == 0) {
      if (tex0) {
        C3D_TexEnvSrc(e, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
        C3D_TexEnvFunc(e, C3D_Both, GPU_MODULATE);
      } else {
        C3D_TexEnvSrc(e, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
        C3D_TexEnvFunc(e, C3D_Both, GPU_REPLACE);
      }
    } else if (i == 1 && tex1) {
      C3D_TexEnvSrc(e, C3D_Both, GPU_PREVIOUS, GPU_TEXTURE1, 0);
      C3D_TexEnvFunc(e, C3D_Both, GPU_MODULATE);
    } else if (i < stages) {
      C3D_TexEnvSrc(e, C3D_Both, GPU_PREVIOUS, GPU_CONSTANT, 0);
      C3D_TexEnvFunc(e, C3D_Both, GPU_MODULATE);
      C3D_TexEnvColor(e, 0xFFFFFFFF);
    }
  }
}

/* opaque, no depth test or write, no culling, no early depth */
static void plain_state(void) {
  C3D_CullFace(GPU_CULL_NONE);
  C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
  C3D_AlphaTest(false, GPU_ALWAYS, 0);
  C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
  C3D_EarlyDepthTest(false, GPU_EARLYDEPTH_GEQUAL, 0);
  C3D_StencilTest(false, GPU_ALWAYS, 0, 0xFF, 0);
  for (int i = 0; i < 3; i++) {
    C3D_TexBind(i, NULL);
  }
  combiners(0, 1, 0);
}

/* room in the small vertex buffer (reset per test) */
static void* imm_take(u32 bytes) {
  bytes = (bytes + 15u) & ~15u;
  if (B.imm_used + bytes > IMM_BYTES) {
    B.imm_used = 0; /* never with the tests' sizes (each test starts from 0): wrap */
  }
  void* p = B.imm + B.imm_used;
  B.imm_used += bytes;
  return p;
}

static void imm_flush(void) {
  GSPGPU_FlushDataCache(B.imm, B.imm_used);
}

/* quads (two triangles each) in pixel space for ctr_basic's float layout */
static VtxF* quads_f(int n, float x0, float y0, float x1, float y1, float z, float s0, float t0,
                     float s1, float t1, u32 rgba) {
  VtxF* v = (VtxF*)imm_take((u32)n * 6 * sizeof(VtxF));
  const float px[6] = {x0, x1, x0, x0, x1, x1}, py[6] = {y0, y0, y1, y1, y0, y1};
  const float ps[6] = {s0, s1, s0, s0, s1, s1}, pt[6] = {t0, t0, t1, t1, t0, t1};
  for (int q = 0; q < n; q++) {
    for (int k = 0; k < 6; k++) {
      VtxF* o = &v[q * 6 + k];
      o->x = px[k];
      o->y = py[k];
      o->z = z;
      o->s = ps[k];
      o->t = pt[k];
      o->r = (u8)(rgba >> 24);
      o->g = (u8)(rgba >> 16);
      o->b = (u8)(rgba >> 8);
      o->a = (u8)rgba;
    }
  }
  return v;
}

/* the RGBA8 color buffer word of a color (memory fill value; R in the top byte) */
#define RGBA(r, g, b, a) (((u32)(r) << 24) | ((u32)(g) << 16) | ((u32)(b) << 8) | (u32)(a))
#define COL_A RGBA(255, 0, 0, 255) /* the near / small quad */
#define COL_B RGBA(0, 255, 0, 255) /* the full screen quad */
#define COL_C RGBA(0, 0, 255, 255) /* the clear color */

/* the color buffer into B.rb (linear, the target's width x height) */
static void queue_readback(C3D_RenderTarget* t) {
  if (!queue_room(1)) {
    return;
  }
  const C3D_FrameBuf* fb = &t->frameBuf;
  const u32 flags = GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) |
                    GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                    GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                    GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO) | (fb->block32 ? BIT(16) : 0);
  GX_DisplayTransfer((u32*)fb->colorBuf, GX_BUFFER_DIM(fb->width, fb->height), B.rb,
                     GX_BUFFER_DIM(fb->width, fb->height), flags);
  B.queued++;
}

typedef struct {
  int a, b, c, other;            /* pixels of each color */
  int cx0, cy0, cx1, cy1;        /* bounding box of the clear color (x1 < x0: none) */
  int ax0, ay0, ax1, ay1;        /* of color A */
} Census;

static int classify(u32 p) {
  const int r = (int)(p >> 24), g = (int)((p >> 16) & 0xFF), b = (int)((p >> 8) & 0xFF);
  if (r > 128 && g < 128 && b < 128) {
    return 0;
  }
  if (g > 128 && r < 128 && b < 128) {
    return 1;
  }
  if (b > 128 && r < 128 && g < 128) {
    return 2;
  }
  return 3;
}

static void census(const C3D_RenderTarget* t, Census* c) {
  const int w = t->frameBuf.width, h = t->frameBuf.height;
  GSPGPU_InvalidateDataCache(B.rb, (u32)w * h * 4);
  memset(c, 0, sizeof(*c));
  c->cx0 = c->ax0 = w;
  c->cy0 = c->ay0 = h;
  c->cx1 = c->cy1 = c->ax1 = c->ay1 = -1;
  for (int y = 0; y < h; y++) {
    const u32* row = B.rb + (size_t)y * w;
    for (int x = 0; x < w; x++) {
      const int k = classify(row[x]);
      if (k == 0) {
        c->a++;
        c->ax0 = x < c->ax0 ? x : c->ax0;
        c->ax1 = x > c->ax1 ? x : c->ax1;
        c->ay0 = y < c->ay0 ? y : c->ay0;
        c->ay1 = y > c->ay1 ? y : c->ay1;
      } else if (k == 1) {
        c->b++;
      } else if (k == 2) {
        c->c++;
        c->cx0 = x < c->cx0 ? x : c->cx0;
        c->cx1 = x > c->cx1 ? x : c->cx1;
        c->cy0 = y < c->cy0 ? y : c->cy0;
        c->cy1 = y > c->cy1 ? y : c->cy1;
      } else {
        c->other++;
      }
    }
  }
}

static int census_text(const Census* c, char* out, int size) {
  return snprintf(out, (size_t)size, "a=%d b=%d c=%d other=%d cbox=%d,%d,%d,%d abox=%d,%d,%d,%d",
                  c->a, c->b, c->c, c->other, c->cx0, c->cy0, c->cx1, c->cy1, c->ax0, c->ay0,
                  c->ax1, c->ay1);
}

/* ---------------- B1: the floor ---------------- */

/* one triangle outside the screen (clipped away), ctr_basic's float layout, pixel space */
static VtxF* offscreen_tri_f(void) {
  VtxF* v = (VtxF*)imm_take(3 * sizeof(VtxF));
  memset(v, 0, 3 * sizeof(VtxF));
  for (int k = 0; k < 3; k++) {
    v[k].x = 10.0f * TW + (k == 1 ? 8.0f : 0.0f);
    v[k].y = 10.0f + (k == 2 ? 8.0f : 0.0f);
    v[k].z = 0.5f;
    v[k].r = v[k].g = v[k].b = v[k].a = 255;
  }
  return v;
}

/* a minimal frame's command list: the state and one clipped triangle */
static void build_min_list(void) {
  B.imm_used = 0;
  plain_state();
  use_basic(&B.m_px, L_FLOAT);
  VtxF* v = offscreen_tri_f();
  imm_flush();
  vbuf(v, L_FLOAT);
  C3D_DrawArrays(GPU_TRIANGLES, 0, 3);
}

static void queue_screen_transfer(void) {
  if (!queue_room(1)) {
    return;
  }
  const C3D_FrameBuf* fb = &B.t0->frameBuf;
  GX_DisplayTransfer((u32*)fb->colorBuf, GX_BUFFER_DIM(TW, TH), (u32*)B.scratch,
                     GX_BUFFER_DIM(TW, TH),
                     GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) |
                         GX_TRANSFER_RAW_COPY(0) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) |
                         GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) |
                         GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO));
  B.queued++;
}

static void queue_fence(void) {
  if (queue_room(1)) {
    GX_TextureCopy(B.fence_src, 0, B.fence_dst, 0, 16, 8);
    B.queued++;
  }
}

static void b1(void) {
  Meas m;
  static const int idle_ms[2] = {50, 100};
  for (int i = 0; i < 2 && !B.dead; i++) {
    char id[32];
    snprintf(id, sizeof(id), "B1.idle%d", idle_ms[i]);
    bench_start(id);
    meas_begin(&m);
    svcSleepThread((s64)idle_ms[i] * 1000000LL);
    meas_end(&m);
    put_meas(id, 0, 1, &m, "");
  }
  if (B.dead) {
    return;
  }
  bench_start("B1.list");
  fb_begin(B.t0);
  build_min_list();
  run_list(8, &m);
  put_meas("B1.list", 1, 8, &m, "");
  if (B.dead) {
    return;
  }
  bench_start("B1.clear");
  fb_begin(NULL);
  for (int r = 0; r < 4; r++) {
    queue_clear(B.t0, COL_C, 0);
  }
  bench_run(&m);
  put_meas("B1.clear", TW * TH, 4, &m, "bytes_fill0=384000 bytes_fill1=384000");
  if (B.dead) {
    return;
  }
  bench_start("B1.transfer");
  fb_begin(NULL);
  for (int r = 0; r < 4; r++) {
    queue_screen_transfer();
  }
  bench_run(&m);
  put_meas("B1.transfer", TW * TH, 4, &m, "bytes_r=384000 bytes_w=288000");
  if (B.dead) {
    return;
  }
  bench_start("B1.fence");
  fb_begin(NULL);
  for (int r = 0; r < 4; r++) {
    queue_fence();
  }
  bench_run(&m);
  put_meas("B1.fence", 16, 4, &m, "");
  if (B.dead) {
    return;
  }
  /* the game's empty frame: clear, the minimal list, display transfer, fence */
  bench_start("B1.frame");
  fb_begin(B.t0);
  build_min_list();
  u32 words;
  u32* list = split_list(&words);
  for (int r = 0; r < 4; r++) {
    queue_clear(B.t0, COL_C, 0);
    queue_list(list, words, 1);
    queue_screen_transfer();
    queue_fence();
  }
  bench_run(&m);
  put_meas("B1.frame", 1, 4, &m, "");
}

/* ---------------- B2: per draw costs (2000 one-triangle draws, clipped away) ---------------- */

enum {
  B2_NOSTATE,
  B2_REBIND_CLR,
  B2_REBIND,
  B2_ALT2_CLR,
  B2_ALT2,
  B2_TEXENV,
  B2_UNIF_F32,
  B2_UNIF_F24,
  B2_PROG_ENTRY,
  B2_PROG_SEP,
};
#define B2_DRAWS 2000

/* libctru's f32tof24 rounding as ctr_gpu_citro3d.c's raw_f24 */
static u32 f24(float f) {
  union {
    float f;
    u32 i;
  } u = {f};
  const u32 m23 = (u.i & 0x7fffff) + 0x40;
  const u32 sign = u.i >> 31, mant = (m23 >> 7) & 0xffff;
  const s32 e = (s32)((u.i >> 23) & 0xff) - 127 + 63 + (s32)(m23 >> 23);
  if (e < 0 || !((u.i >> 23) & 0xff)) {
    return sign << 23;
  }
  if (e > 0x7f) {
    return (sign << 23) | (0x7fu << 16);
  }
  return (sign << 23) | ((u32)e << 16) | mant;
}

static void b2(int variant, const char* id) {
  if (B.dead) {
    return;
  }
  if (!B.tex_ok[0] || !B.tex_ok[1]) {
    bench_emitf("[bench] %s skipped=no_texture", id);
    return;
  }
  const int prog = variant == B2_PROG_ENTRY || variant == B2_PROG_SEP;
  if (prog && (!B.bench_entry_ok[0] || !B.bench_entry_ok[1] || !B.sep.ok)) {
    bench_emitf("[bench] %s skipped=no_program", id);
    return;
  }
  bench_start(id);
  fb_begin(B.t0);
  B.imm_used = 0;
  plain_state();
  combiners(1, 1, 0);
  C3D_TexBind(0, &B.tex[0]);
  const void* tri;
  if (prog) {
    VtxS* v = (VtxS*)imm_take(3 * sizeof(VtxS));
    memset(v, 0, 3 * sizeof(VtxS));
    for (int k = 0; k < 3; k++) {
      v[k].x = (s16)(3000 + (k == 1 ? 8 : 0));
      v[k].y = (s16)(k == 2 ? 8 : 0);
      v[k].w = 1;
      v[k].r = v[k].g = v[k].b = v[k].a = 255;
    }
    tri = v;
    use_bench(-1, &B.m_geo); /* the separate DVLB's uniforms, then the shared one's */
    use_bench(variant == B2_PROG_SEP ? 0 : 1, &B.m_geo);
    use_bench(0, &B.m_geo);
  } else {
    tri = offscreen_tri_f();
    use_basic(&B.m_px, L_FLOAT);
  }
  imm_flush();
  vbuf(tri, prog ? L_SHORT : L_FLOAT);
  C3Di_UpdateContext();
  const int loc = uloc(&B.basic.prog, "projection");
  u32 m24[12];
  for (int r = 0; r < 4; r++) {
    const float* v = (const float*)&B.m_px.r[r]; /* C3D_FVec: w, z, y, x */
    const u32 w = f24(v[0]), z = f24(v[1]), y = f24(v[2]), x = f24(v[3]);
    m24[3 * r] = (w << 8) | (z >> 16);
    m24[3 * r + 1] = (z << 16) | (y >> 8);
    m24[3 * r + 2] = (y << 24) | x;
  }
  for (int i = 0; i < B2_DRAWS; i++) {
    switch (variant) {
      case B2_REBIND_CLR:
      case B2_REBIND:
      case B2_ALT2_CLR:
      case B2_ALT2: {
        const int alt = variant == B2_ALT2_CLR || variant == B2_ALT2;
        C3Di_SetTex(0, &B.tex[alt && (i & 1) ? 1 : 0]);
        GPUCMD_AddMaskedWrite(GPUREG_TEXUNIT_CONFIG, 0xB, BIT(12) | 1);
        if (variant == B2_REBIND_CLR || variant == B2_ALT2_CLR) {
          GPUCMD_AddMaskedWrite(GPUREG_TEXUNIT_CONFIG, 0x4, BIT(16));
        }
        break;
      }
      case B2_TEXENV: {
        C3D_TexEnv* e = C3D_GetTexEnv(0);
        C3D_TexEnvInit(e);
        C3D_TexEnvSrc(e, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
        C3D_TexEnvFunc(e, C3D_Both, (i & 1) ? GPU_ADD : GPU_MODULATE);
        break;
      }
      case B2_UNIF_F32:
        GPUCMD_AddWrite(GPUREG_VSH_FLOATUNIFORM_CONFIG, BIT(31) | (u32)loc);
        GPUCMD_AddWrites(GPUREG_VSH_FLOATUNIFORM_DATA, (u32*)&B.m_px, 16);
        break;
      case B2_UNIF_F24:
        GPUCMD_AddWrite(GPUREG_VSH_FLOATUNIFORM_CONFIG, (u32)loc);
        GPUCMD_AddWrites(GPUREG_VSH_FLOATUNIFORM_DATA, m24, 12);
        break;
      case B2_PROG_ENTRY:
        C3D_BindProgram(&B.bench_entry[i & 1]);
        break;
      case B2_PROG_SEP:
        C3D_BindProgram((i & 1) ? &B.sep.prog : &B.bench_entry[0]);
        break;
      default:
        break;
    }
    C3D_DrawArrays(GPU_TRIANGLES, 0, 3);
  }
  const u32 cmd_words = gpuCmdBufOffset;
  Meas m;
  run_list(4, &m);
  char extra[48];
  snprintf(extra, sizeof(extra), "cmd_words=%lu", (unsigned long)cmd_words);
  put_meas(id, B2_DRAWS, 4, &m, extra);
}

/* ---------------- the back facing triangles (B3, B5) ---------------- */

static void cull_pos(int i, int k, int* x, int* y) {
  const int cx = -96 + (i % 96) * 2, cy = -96 + ((i / 96) % 96) * 2;
  *x = cx + (k == 2 ? 2 : 0);
  *y = cy + (k == 1 ? 2 : 0);
}

static void make_cull_buffer(void) {
  for (int i = 0; i < CULL_TRIS; i++) {
    for (int k = 0; k < 3; k++) {
      VtxS* v = &B.cull_s[3 * i + k];
      int x, y;
      cull_pos(i, k, &x, &y);
      memset(v, 0, sizeof(*v));
      v->x = (s16)x;
      v->y = (s16)y;
      v->w = 1;
      v->s = (s16)(k * 512);
      v->r = v->g = v->b = v->a = 255;
    }
  }
  GSPGPU_FlushDataCache(B.cull_s, CULL_VERTS * sizeof(VtxS));
}

/* the pixels the cull buffer covers with a cull mode */
static int cull_drawn(GPU_CULLMODE mode) {
  fb_begin(B.t0);
  queue_clear(B.t0, COL_C, 0);
  plain_state();
  C3D_CullFace(mode);
  use_basic(&B.m_geo, L_SHORT);
  vbuf(B.cull_s, L_SHORT);
  C3D_DrawArrays(GPU_TRIANGLES, 0, CULL_VERTS);
  u32 words;
  u32* list = split_list(&words);
  queue_list(list, words, 1);
  queue_readback(B.t0);
  if (!bench_run(NULL)) {
    return -1;
  }
  Census c;
  census(B.t0, &c);
  return TW * TH - c.c;
}

static void cull_check(void) {
  bench_start("B3.cullcheck");
  const int none = cull_drawn(GPU_CULL_NONE);
  const int back = B.dead ? -1 : cull_drawn(GPU_CULL_BACK_CCW);
  const int front = B.dead ? -1 : cull_drawn(GPU_CULL_FRONT_CCW);
  B.cull_mode = back == 0 ? GPU_CULL_BACK_CCW : (front == 0 ? GPU_CULL_FRONT_CCW : GPU_CULL_BACK_CCW);
  bench_emitf("[bench] B3.cullcheck drawn_none=%d drawn_back=%d drawn_front=%d use=%s ok=%d", none,
              back, front, B.cull_mode == GPU_CULL_BACK_CCW ? "back_ccw" : "front_ccw",
              back == 0 || front == 0);
}

/* 100000 culled triangles: 10 draws of the buffer (the current program and layout) */
static void draw_big(void) {
  for (int d = 0; d < BIG_TRIS / CULL_TRIS; d++) {
    C3D_DrawArrays(GPU_TRIANGLES, 0, CULL_VERTS);
  }
}

/* ---------------- B4: the post-transform vertex cache ---------------- */

#define B4_INDICES 300000
#define B4_VERTS 4096

/* experiments: 0 = the cache setting as it is, 1 = written at 0x4 and 0x84 */
static void b4(int experiments) {
  if (B.dead) {
    return;
  }
  VtxS* verts = (VtxS*)lin_alloc(B4_VERTS * sizeof(VtxS));
  u16* idx = (u16*)lin_alloc(B4_INDICES * sizeof(u16));
  if (!verts || !idx) {
    bench_emit("[bench] B4 skipped=no_memory");
    lin_free(verts);
    lin_free(idx);
    return;
  }
  /* every vertex at one point: every triangle has no area (set up, never drawn) */
  for (int i = 0; i < B4_VERTS; i++) {
    memset(&verts[i], 0, sizeof(VtxS));
    verts[i].w = 1;
    verts[i].r = verts[i].g = verts[i].b = verts[i].a = 255;
  }
  GSPGPU_FlushDataCache(verts, B4_VERTS * sizeof(VtxS));
  /* GPUREG_POST_VERTEX_CACHE_NUM (0x22D): "number of entries, usually 0x4 or 0x84" (3dbrew,
   * GBATEK); citro3d never writes it. Read once (GSP), tried at both documented values, then
   * written back. */
  u32 orig = 0;
  const int have = B.hw_ok && R_SUCCEEDED(GSPGPU_ReadHWRegs(HWREG_POST_VERTEX_CACHE_NUM, &orig, 4));
  if (!experiments) {
    bench_emitf("[bench] B4.cachenum read=%d value=0x%08lx", have, (unsigned long)orig);
  }
  static const int ks[] = {0, 1, 2, 3, 4, 5, 6, 8, 10, 12, 16, 20};
  const int settings[3] = {-1, 0x04, 0x84};
  for (int si = 0; si < 3 && !B.dead; si++) {
    const int s = settings[si];
    if ((s >= 0) != (experiments != 0)) {
      continue;
    }
    if (s >= 0 && !have) {
      bench_emitf("[bench] B4.c%02x skipped=cachenum_unread", s);
      continue;
    }
    for (unsigned ki = 0; ki < sizeof(ks) / sizeof(ks[0]) && !B.dead; ki++) {
      const int k = ks[ki];
      for (int i = 0; i < B4_INDICES; i++) {
        int v;
        if (k == 0) {
          v = i; /* no reuse */
        } else {
          const int block = i / (2 * k), within = i % (2 * k);
          v = block * k + within % k;
        }
        idx[i] = (u16)(v % B4_VERTS);
      }
      GSPGPU_FlushDataCache(idx, B4_INDICES * sizeof(u16));
      char id[40];
      snprintf(id, sizeof(id), "B4.%s.k%d", s < 0 ? "cdef" : (s == 0x04 ? "c04" : "c84"), k);
      bench_start(id);
      fb_begin(B.t0);
      plain_state();
      use_basic(&B.m_geo, L_SHORT);
      vbuf(verts, L_SHORT);
      C3Di_UpdateContext();
      if (s >= 0) {
        GPUCMD_AddMaskedWrite(GPUREG_POST_VERTEX_CACHE_NUM, 0x1, (u32)s);
      }
      for (int d = 0; d < B4_INDICES / 30000; d++) {
        C3D_DrawElements(GPU_TRIANGLES, 30000, C3D_UNSIGNED_SHORT, idx + d * 30000);
      }
      Meas m;
      run_list(3, &m);
      put_meas(id, B4_INDICES, 3, &m, "");
    }
  }
  if (experiments && have && !B.dead) {
    fb_begin(B.t0);
    GPUCMD_AddMaskedWrite(GPUREG_POST_VERTEX_CACHE_NUM, 0x1, orig & 0xFF);
    run_list(1, NULL);
  }
  lin_free(verts);
  lin_free(idx);
}

/* ---------------- B5: attribute fetch ---------------- */

static void b5_one(const char* id, const void* buf, int layout) {
  if (B.dead) {
    return;
  }
  bench_start(id);
  fb_begin(B.t0);
  plain_state();
  C3D_CullFace((GPU_CULLMODE)B.cull_mode);
  use_basic(&B.m_geo, layout);
  vbuf(buf, layout);
  draw_big();
  Meas m;
  run_list(3, &m);
  put_meas(id, BIG_TRIS, 3, &m, "");
}

static void b5(void) {
  b5_one("B5.short", B.cull_s, L_SHORT);
  VtxF* f = (VtxF*)lin_alloc(CULL_VERTS * sizeof(VtxF));
  if (f) {
    for (int i = 0; i < CULL_VERTS; i++) {
      const VtxS* s = &B.cull_s[i];
      f[i] = (VtxF){s->x, s->y, s->z, s->s / 1024.0f, s->t / 1024.0f, s->r, s->g, s->b, s->a};
    }
    GSPGPU_FlushDataCache(f, CULL_VERTS * sizeof(VtxF));
    b5_one("B5.float", f, L_FLOAT);
    lin_free(f);
  }
  VtxB* b = (VtxB*)lin_alloc(CULL_VERTS * sizeof(VtxB));
  if (b) {
    for (int i = 0; i < CULL_VERTS; i++) {
      const VtxS* s = &B.cull_s[i];
      b[i] = (VtxB){(s8)s->x, (s8)s->y, (s8)s->z, 1, 0, 0, s->r, s->g, s->b, s->a};
    }
    GSPGPU_FlushDataCache(b, CULL_VERTS * sizeof(VtxB));
    b5_one("B5.byte", b, L_BYTE);
    lin_free(b);
  }
  void* v = vram_alloc(CULL_VERTS * sizeof(VtxS));
  if (v && !B.dead && gx_copy(B.cull_s, v, CULL_VERTS * sizeof(VtxS))) {
    b5_one("B5.short_vram", v, L_SHORT);
  } else if (!v) {
    bench_emit("[bench] B5.short_vram skipped=no_vram");
  }
  vram_free(v);
}

/* ---------------- B6: fill (20 full screen quads per variant) ---------------- */

#define B6_QUADS 20
enum { TEX_RGBA8, TEX_RGB565, TEX_ETC1, TEX_RGBA8_MIP, TEX_RGB565_MIP, TEX_ETC1_MIP };

typedef struct {
  int tex;        /* -1: untextured */
  float scale;    /* texels per pixel along each axis */
  int stages;     /* combiner stages in use */
  int blend;
  int depth;      /* 0 off, 1: T0's D24S8 tested and written, 2: T1's D16 */
  int cbread0;    /* GPUREG_COLORBUFFER_READ = 0 */
  float bias;     /* LOD bias */
} Fill;

static void b6_fill(const char* id, Fill f) {
  if (B.dead) {
    return;
  }
  if (f.tex >= 0 && !B.tex_ok[f.tex]) {
    bench_emitf("[bench] %s skipped=no_texture", id);
    return;
  }
  C3D_RenderTarget* t = f.depth == 2 ? B.t1 : B.t0;
  if (!t) {
    bench_emitf("[bench] %s skipped=no_target", id);
    return;
  }
  bench_start(id);
  fb_begin(t);
  B.imm_used = 0;
  if (f.depth) {
    queue_clear(t, COL_C, 0);
  }
  plain_state();
  use_basic(&B.m_px, L_FLOAT);
  const float s1 = f.scale * TW / 256.0f, t1 = f.scale * TH / 256.0f;
  VtxF* q = quads_f(B6_QUADS, 0, 0, TW, TH, 0.5f, 0, 0, s1, t1, RGBA(255, 255, 255, 128));
  imm_flush();
  vbuf(q, L_FLOAT);
  if (f.tex >= 0) {
    C3D_TexSetLodBias(&B.tex[f.tex], f.bias);
    C3D_TexBind(0, &B.tex[f.tex]);
  }
  combiners(f.tex >= 0, f.stages, 0);
  if (f.blend) {
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA,
                   GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
  }
  if (f.depth) {
    C3D_DepthTest(true, GPU_GEQUAL, GPU_WRITE_ALL);
  }
  if (f.cbread0) {
    C3Di_UpdateContext();
    GPUCMD_AddWrite(GPUREG_COLORBUFFER_READ, 0);
  }
  C3D_DrawArrays(GPU_TRIANGLES, 0, B6_QUADS * 6);
  if (f.cbread0) {
    GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_FLUSH, 1);
    GPUCMD_AddWrite(GPUREG_COLORBUFFER_READ, 0xF);
  }
  Meas m;
  run_list(3, &m);
  if (f.tex >= 0) {
    C3D_TexSetLodBias(&B.tex[f.tex], 0.0f);
  }
  put_meas(id, B6_QUADS * TW * TH, 3, &m, "");
}

/* GPUREG_COLORBUFFER_READ = 0 on an opaque quad over part of the screen (its edge not on an
 * 8 pixel block boundary): does the cleared background next to it survive? */
static void b6_cbread_probe(void) {
  if (B.dead) {
    return;
  }
  bench_start("B6.cbread0.probe");
  fb_begin(B.t0);
  B.imm_used = 0;
  queue_clear(B.t0, COL_C, 0);
  plain_state();
  use_basic(&B.m_px, L_FLOAT);
  VtxF* q = quads_f(1, 0, 0, 101, TH, 0.5f, 0, 0, 0, 0, COL_A);
  imm_flush();
  vbuf(q, L_FLOAT);
  C3Di_UpdateContext();
  GPUCMD_AddWrite(GPUREG_COLORBUFFER_READ, 0);
  C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
  GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_FLUSH, 1);
  GPUCMD_AddWrite(GPUREG_COLORBUFFER_READ, 0xF);
  u32 words;
  u32* list = split_list(&words);
  queue_list(list, words, 1);
  queue_readback(B.t0);
  Meas m;
  if (!bench_run(&m)) {
    return;
  }
  Census c;
  census(B.t0, &c);
  char text[160];
  census_text(&c, text, sizeof(text));
  bench_emitf("[bench] B6.cbread0.probe %s exp_a=%d exp_c=%d bg_ok=%d", text, 101 * TH,
              TW * TH - 101 * TH, c.c == TW * TH - 101 * TH && c.other == 0);
}

/* texture unit 1 (ctr_clip2: texture coordinates 1): mode 1 bound but unread, 2 off, 3 read */
static void b6_unit1(const char* id, int mode) {
  if (B.dead || !B.clip2.ok || !B.tex_ok[TEX_RGBA8]) {
    return;
  }
  bench_start(id);
  fb_begin(B.t0);
  B.imm_used = 0;
  plain_state();
  C3D_BindProgram(&B.clip2.prog);
  attrs(L_CLIP2);
  C3D_Mtx id4;
  Mtx_Identity(&id4);
  id4.r[2].z = -0.5f; /* depth 0.5 (z' = -0.5 for z = 1) */
  C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uloc(&B.clip2.prog, "glpica"), &id4);
  VtxC2* v = (VtxC2*)imm_take(B6_QUADS * 6 * sizeof(VtxC2));
  const float px[6] = {-1, 1, -1, -1, 1, 1}, py[6] = {-1, -1, 1, 1, -1, 1};
  for (int i = 0; i < B6_QUADS * 6; i++) {
    const int k = i % 6;
    v[i] = (VtxC2){px[k], py[k], 1.0f, 1.0f, (px[k] + 1) * TW / 512.0f,
                   (py[k] + 1) * TH / 512.0f, (px[k] + 1) * TW / 512.0f,
                   (py[k] + 1) * TH / 512.0f, 255, 255, 255, 255};
  }
  imm_flush();
  vbuf(v, L_CLIP2);
  C3D_TexBind(0, &B.tex[TEX_RGBA8]);
  if (mode != 2) {
    C3D_TexBind(1, &B.tex[TEX_RGBA8]);
  }
  combiners(1, 1, mode == 3);
  C3D_DrawArrays(GPU_TRIANGLES, 0, B6_QUADS * 6);
  Meas m;
  run_list(3, &m);
  put_meas(id, B6_QUADS * TW * TH, 3, &m, "");
}

static void b6(void) {
  b6_fill("B6.untex", (Fill){-1, 1, 1, 0, 0, 0, 0});
  static const char* const fmt[3] = {"rgba8", "rgb565", "etc1"};
  for (int i = 0; i < 3; i++) {
    char id[40];
    snprintf(id, sizeof(id), "B6.%s.1to1", fmt[i]);
    b6_fill(id, (Fill){i, 1, 1, 0, 0, 0, 0});
    snprintf(id, sizeof(id), "B6.%s.mag4", fmt[i]);
    b6_fill(id, (Fill){i, 0.25f, 1, 0, 0, 0, 0});
    snprintf(id, sizeof(id), "B6.%s.min4", fmt[i]);
    b6_fill(id, (Fill){i, 4, 1, 0, 0, 0, 0});
    snprintf(id, sizeof(id), "B6.%s.min4mip", fmt[i]);
    b6_fill(id, (Fill){i + 3, 4, 1, 0, 0, 0, 0});
  }
  b6_fill("B6.rgba8.min4mip.bias1", (Fill){TEX_RGBA8_MIP, 4, 1, 0, 0, 0, 1.0f});
  b6_fill("B6.stages2", (Fill){TEX_RGBA8, 1, 2, 0, 0, 0, 0});
  b6_fill("B6.stages6", (Fill){TEX_RGBA8, 1, 6, 0, 0, 0, 0});
  b6_fill("B6.blend", (Fill){TEX_RGBA8, 1, 1, 1, 0, 0, 0});
  b6_fill("B6.d24s8", (Fill){-1, 1, 1, 0, 1, 0, 0});
  b6_fill("B6.d16", (Fill){-1, 1, 1, 0, 2, 0, 0});
  b6_unit1("B6.unit1.unread", 1);
  b6_unit1("B6.unit1.off", 2);
  b6_unit1("B6.unit1.read", 3);
}

/* ---------------- B7: triangle setup ---------------- */

static void b7_one(const char* id, int zero_area) {
  if (B.dead) {
    return;
  }
  VtxF* v = (VtxF*)lin_alloc(CULL_VERTS * sizeof(VtxF));
  if (!v) {
    bench_emitf("[bench] %s skipped=no_memory", id);
    return;
  }
  for (int i = 0; i < CULL_TRIS; i++) {
    const float px = (float)(20 + i % 200), py = (float)(50 + ((i / 200) % 150) * 2);
    for (int k = 0; k < 3; k++) {
      VtxF* o = &v[3 * i + k];
      memset(o, 0, sizeof(*o));
      if (zero_area) {
        o->x = px + 0.5f;
        o->y = py + 0.5f;
      } else {
        o->x = px + (k == 1 ? 1.2f : 0.0f);
        o->y = py + (k == 2 ? 1.2f : 0.0f);
      }
      o->z = 0.5f;
      o->r = o->g = o->b = o->a = 255;
    }
  }
  GSPGPU_FlushDataCache(v, CULL_VERTS * sizeof(VtxF));
  bench_start(id);
  fb_begin(B.t0);
  plain_state();
  use_basic(&B.m_px, L_FLOAT);
  vbuf(v, L_FLOAT);
  draw_big();
  Meas m;
  run_list(3, &m);
  put_meas(id, BIG_TRIS, 3, &m, "");
  lin_free(v);
}

/* ---------------- B8: early depth semantics ---------------- */

typedef struct {
  float x0, y0, x1, y1, z;
  u32 col;
  int early;          /* early depth test on */
  int func;           /* GPU_EARLYDEPTHFUNC */
  u32 data;           /* EARLYDEPTH_DATA (the clear value) */
  int clear_early;    /* EARLYDEPTH_CLEAR before this draw */
  int zwrite;         /* depth write */
  int ztest;          /* GPU_TESTFUNC of the depth test (enabled) */
  int atest_fail;     /* alpha test NEVER */
} Draw8;

/* draws into t, one after another in one command list after a clear (color C, depth 0); keep:
 * leave out citro3d's early depth clear at the end of the list (case 5's first frame);
 * readback: census of the result */
static int b8_draws(C3D_RenderTarget* t, const Draw8* d, int n, int clear_target, int keep,
                    int readback, Census* c) {
  const C3D_FrameBuf* fb = &t->frameBuf;
  C3D_Mtx px;
  mtx_px(&px, fb->width, fb->height);
  fb_begin(t);
  B.imm_used = 0;
  if (clear_target) {
    queue_clear(t, COL_C, 0);
  }
  plain_state();
  use_basic(&px, L_FLOAT);
  VtxF* v = (VtxF*)imm_take((u32)n * 6 * sizeof(VtxF));
  for (int i = 0; i < n; i++) {
    VtxF* q = quads_f(1, d[i].x0, d[i].y0, d[i].x1, d[i].y1, d[i].z, 0, 0, 0, 0, d[i].col);
    memcpy(v + 6 * i, q, 6 * sizeof(VtxF));
  }
  imm_flush();
  vbuf(v, L_FLOAT);
  for (int i = 0; i < n; i++) {
    C3D_EarlyDepthTest(d[i].early != 0, (GPU_EARLYDEPTHFUNC)d[i].func, d[i].data);
    C3D_DepthTest(true, (GPU_TESTFUNC)d[i].ztest, d[i].zwrite ? GPU_WRITE_ALL : GPU_WRITE_COLOR);
    C3D_AlphaTest(d[i].atest_fail != 0, GPU_NEVER, 0);
    if (d[i].clear_early) {
      C3Di_UpdateContext(); /* EARLYDEPTH_DATA first (citro3d writes it with the effect) */
      GPUCMD_AddWrite(GPUREG_EARLYDEPTH_CLEAR, 1);
    }
    C3D_DrawArrays(GPU_TRIANGLES, 6 * i, 6);
  }
  if (keep) {
    GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_FLUSH, 1);
    GPUCMD_AddWrite(GPUREG_FRAMEBUFFER_INVALIDATE, 1);
    c3d_head()->flags &= ~2u; /* C3DiF_DrawUsed: no flush + early depth clear from citro3d */
  }
  u32 words;
  u32* list = split_list(&words);
  queue_list(list, words, 1);
  if (readback) {
    queue_readback(t);
  }
  if (!bench_run(NULL)) {
    return 0;
  }
  if (readback) {
    census(t, c);
  }
  return 1;
}

static void b8_emit(const char* tag, const char* name, const Census* c, const char* expect) {
  char text[200];
  census_text(c, text, sizeof(text));
  bench_emitf("[bench] B8.%s.%s %s %s", tag, name, text, expect);
}

static void b8(C3D_RenderTarget* t, const char* tag) {
  if (B.dead || !t) {
    return;
  }
  const float w = t->frameBuf.width, h = t->frameBuf.height;
  /* the near 4x4 quad at an unaligned spot, the full screen quad between the depths */
  const Draw8 small = {13, 21, 17, 25, 0.8f, COL_A, 1, GPU_EARLYDEPTH_GEQUAL, 0, 1, 1, GPU_GEQUAL, 0};
  const Draw8 full = {0, 0, w, h, 0.5f, COL_B, 1, GPU_EARLYDEPTH_GEQUAL, 0, 0, 1, GPU_GEQUAL, 0};
  const int all = (int)(w * h);
  char id[48], exp[96];
  Census c;
  /* ref: where the small quad lands in the readback (no early depth) */
  snprintf(id, sizeof(id), "B8.%s.ref", tag);
  bench_start(id);
  {
    Draw8 d = small;
    d.early = 0;
    d.ztest = GPU_ALWAYS;
    if (b8_draws(t, &d, 1, 1, 0, 1, &c)) {
      b8_emit(tag, "ref", &c, "exp_a=16");
    }
  }
  /* (1) clear + the full quad with early depth: all drawn? */
  snprintf(id, sizeof(id), "B8.%s.c1", tag);
  bench_start(id);
  {
    Draw8 d = full;
    d.clear_early = 1;
    if (!B.dead && b8_draws(t, &d, 1, 1, 0, 1, &c)) {
      snprintf(exp, sizeof(exp), "exp_b=%d", all);
      b8_emit(tag, "c1", &c, exp);
    }
  }
  /* (2) the small near quad, then the full quad: the clear color left = wrongly rejected */
  snprintf(id, sizeof(id), "B8.%s.c2", tag);
  bench_start(id);
  {
    const Draw8 d[2] = {small, full};
    if (!B.dead && b8_draws(t, d, 2, 1, 0, 1, &c)) {
      snprintf(exp, sizeof(exp), "exp_a=16 exp_b=%d exp_c=0", all - 16);
      b8_emit(tag, "c2", &c, exp);
    }
  }
  /* (3) each function against a clear value of 0 and of 1 (0xFFFFFF), the normal depth test
   * passing everything */
  static const char* const fname[4] = {"ge", "gt", "le", "lt"};
  for (int f = 0; f < 4 && !B.dead; f++) {
    for (int dv = 0; dv < 2 && !B.dead; dv++) {
      char name[24];
      snprintf(name, sizeof(name), "c3.%s.d%s", fname[f], dv ? "ff" : "0");
      snprintf(id, sizeof(id), "B8.%s.%s", tag, name);
      bench_start(id);
      Draw8 d = full;
      d.func = f;
      d.data = dv ? 0xFFFFFF : 0;
      d.clear_early = 1;
      d.ztest = GPU_ALWAYS;
      if (b8_draws(t, &d, 1, 1, 0, 1, &c)) {
        b8_emit(tag, name, &c, "exp=b_count_is_the_early_test_result");
      }
    }
  }
  /* (4) the small quad (a) without depth write, (b) failing the alpha test, (c) with early
   * depth off, then the full quad with early depth: does the small quad still update the early
   * depth data (the clear color shows in a block around it)? */
  static const char* const c4name[3] = {"c4a.nozwrite", "c4b.alphafail", "c4c.earlyoff"};
  for (int i = 0; i < 3 && !B.dead; i++) {
    snprintf(id, sizeof(id), "B8.%s.%s", tag, c4name[i]);
    bench_start(id);
    Draw8 d[2] = {small, full};
    if (i == 0) {
      d[0].zwrite = 0;
    } else if (i == 1) {
      d[0].atest_fail = 1;
    } else {
      d[0].early = 0;
      d[1].clear_early = 0;
    }
    if (b8_draws(t, d, 2, 1, 0, 1, &c)) {
      snprintf(exp, sizeof(exp), "exp_a=%d exp_c=0", i == 2 ? 16 : 0);
      b8_emit(tag, c4name[i], &c, exp);
    }
  }
  /* (5) two frames without an early depth clear between them: a near full quad, then (after a
   * memory fill of color and depth) the mid full quad; c5b: the same with the clear */
  for (int i = 0; i < 2 && !B.dead; i++) {
    const char* name = i ? "c5b.clear" : "c5a.noclear";
    snprintf(id, sizeof(id), "B8.%s.%s", tag, name);
    bench_start(id);
    Draw8 dnear = full;
    dnear.z = 0.8f;
    dnear.col = COL_A;
    dnear.clear_early = 1;
    Draw8 mid = full;
    mid.clear_early = i;
    if (b8_draws(t, &dnear, 1, 1, 1, 0, &c) && b8_draws(t, &mid, 1, 1, 0, 1, &c)) {
      snprintf(exp, sizeof(exp), "exp_b=%d (fewer: early depth data kept across frames)", all);
      b8_emit(tag, name, &c, exp);
    }
  }
  C3D_EarlyDepthTest(false, GPU_EARLYDEPTH_GEQUAL, 0);
}

/* ---------------- B3: vertex shader cost (last: the bench's own shaders) ---------------- */

static void b3_entry(const char* id, int entry) {
  if (B.dead) {
    return;
  }
  if (!B.bench_entry_ok[entry]) {
    bench_emitf("[bench] %s skipped=no_program", id);
    return;
  }
  bench_start(id);
  fb_begin(B.t0);
  plain_state();
  C3D_CullFace((GPU_CULLMODE)B.cull_mode);
  use_bench(entry, &B.m_geo);
  vbuf(B.cull_s, L_SHORT);
  draw_big();
  Meas m;
  run_list(3, &m);
  put_meas(id, BIG_TRIS, 3, &m, "");
}

/* the game's new ctr_mesh (shader_mesh = new) on the same triangles */
static void b3_mesh_new(void) {
  if (B.dead || !B.mesh.ok) {
    return;
  }
  bench_start("B3.mesh_new");
  fb_begin(B.t0);
  plain_state();
  C3D_CullFace((GPU_CULLMODE)B.cull_mode);
  shaderProgram_s* p = &B.mesh.prog;
  C3D_BindProgram(p);
  attrs(L_SHORT);
  C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uloc(p, "clip"), &B.m_geo);
  C3D_FVUnifSet(GPU_VERTEX_SHADER, uloc(p, "scales"), 1.0f / 1024.0f, 1.0f / 255.0f, 1.0f, 0.0f);
  C3D_FVUnifSet(GPU_VERTEX_SHADER, uloc(p, "fog0"), -1.0f / 255.0f, 0.0f, -1.0f, 0.0f);
  C3D_FVUnifSet(GPU_VERTEX_SHADER, uloc(p, "fog1"), 0.0f, 0.0f, 0.0f, 0.0f);
  vbuf(B.cull_s, L_SHORT);
  draw_big();
  Meas m;
  run_list(3, &m);
  put_meas("B3.mesh_new", BIG_TRIS, 3, &m, "");
}

/* the game's new ctr_skin (shader_skin = new): one bone (the identity) per vertex */
static void b3_skin_new(void) {
  if (B.dead || !B.skin.ok) {
    return;
  }
  VtxSkin* v = (VtxSkin*)lin_alloc(CULL_VERTS * sizeof(VtxSkin));
  if (!v) {
    bench_emit("[bench] B3.skin_new skipped=no_memory");
    return;
  }
  for (int i = 0; i < CULL_VERTS; i++) {
    const VtxS* s = &B.cull_s[i];
    memset(&v[i], 0, sizeof(VtxSkin));
    v[i].x = s->x;
    v[i].y = s->y;
    v[i].wgt[0] = 255;
    v[i].r = v[i].g = v[i].b = v[i].a = 255;
    v[i].nz = 127;
  }
  GSPGPU_FlushDataCache(v, CULL_VERTS * sizeof(VtxSkin));
  bench_start("B3.skin_new");
  fb_begin(B.t0);
  plain_state();
  C3D_CullFace((GPU_CULLMODE)B.cull_mode);
  shaderProgram_s* p = &B.skin.prog;
  C3D_BindProgram(p);
  attrs(L_SKIN);
  C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, uloc(p, "clip"), &B.m_geo);
  const int r0 = uloc(p, "row0"), r1 = uloc(p, "row1"), r2 = uloc(p, "row2");
  C3D_FVUnifSet(GPU_VERTEX_SHADER, r0, 1.0f, 0.0f, 0.0f, 0.0f);
  C3D_FVUnifSet(GPU_VERTEX_SHADER, r1, 0.0f, 1.0f, 0.0f, 0.0f);
  C3D_FVUnifSet(GPU_VERTEX_SHADER, r2, 0.0f, 0.0f, 1.0f, 0.0f);
  C3D_FVUnifSet(GPU_VERTEX_SHADER, uloc(p, "scales"), 1.0f / 1024.0f, 1.0f / 255.0f, 1.0f,
                1.0f / 127.0f);
  const int lights = uloc(p, "lights");
  for (int i = 0; i < 7; i++) {
    C3D_FVUnifSet(GPU_VERTEX_SHADER, lights + i, 0.0f, 0.0f, 0.0f, i == 6 ? 1.0f : 0.0f);
  }
  vbuf(v, L_SKIN);
  draw_big();
  Meas m;
  run_list(3, &m);
  put_meas("B3.skin_new", BIG_TRIS, 3, &m, "");
  lin_free(v);
}

/* ---------------- setup, teardown, the run ---------------- */

static int tex_make(int i, int w, int h, GPU_TEXCOLOR fmt, int mips) {
  C3D_Tex* t = &B.tex[i];
  const C3D_TexInitParams p = {(u16)w, (u16)h, (u8)(mips ? C3D_TexCalcMaxLevel(w, h) : 0), fmt,
                               GPU_TEX_2D, true};
  ctr_linear_lock();
  const bool ok = C3D_TexInitWithParams(t, NULL, p);
  ctr_linear_unlock();
  if (!ok) {
    return 0;
  }
  const u32 total = (C3D_TexCalcTotalSize(t->size, t->maxLevel) + 15u) & ~15u;
  u8* staging = (u8*)lin_alloc(total);
  if (!staging) {
    ctr_linear_lock();
    C3D_TexDelete(t);
    ctr_linear_unlock();
    return 0;
  }
  for (u32 k = 0; k < total; k++) {
    staging[k] = (u8)((k * 37u) ^ (k >> 7));
  }
  const int copied = gx_copy(staging, t->data, total);
  lin_free(staging);
  C3D_TexSetFilter(t, GPU_LINEAR, GPU_LINEAR);
  C3D_TexSetWrap(t, GPU_REPEAT, GPU_REPEAT);
  if (mips) {
    C3D_TexSetFilterMipmap(t, GPU_NEAREST);
  }
  B.tex_ok[i] = copied;
  return copied;
}

static C3D_RenderTarget* target_make(int w, int h, GPU_DEPTHBUF depth, int block32) {
  ctr_linear_lock();
  C3D_RenderTarget* t = C3D_RenderTargetCreate(w, h, GPU_RB_RGBA8, depth);
  ctr_linear_unlock();
  if (t && block32) {
    C3D_FrameBufAttrib(&t->frameBuf, (u16)w, (u16)h, true);
  }
  return t;
}

static int setup(void) {
  B.hw_ok = 1;
  B.t0 = target_make(TW, TH, GPU_RB_DEPTH24_STENCIL8, 0);
  B.t1 = target_make(TW, TH, GPU_RB_DEPTH16, 0);
  B.t2 = target_make(B32W, B32H, GPU_RB_DEPTH24_STENCIL8, 1);
  B.rb = (u32*)lin_alloc(B32W * B32H * 4);
  B.scratch = (u8*)lin_alloc(TW * TH * 3);
  B.fence_src = (u32*)lin_alloc(16);
  B.fence_dst = (u32*)lin_alloc(16);
  B.imm = (u8*)lin_alloc(IMM_BYTES);
  B.cull_s = (VtxS*)lin_alloc(CULL_VERTS * sizeof(VtxS));
  if (!B.t0 || !B.rb || !B.scratch || !B.fence_src || !B.fence_dst || !B.imm || !B.cull_s) {
    return 0;
  }
  memset(B.fence_src, 0x5A, 16);
  GSPGPU_FlushDataCache(B.fence_src, 16);
  mtx_px(&B.m_px, TW, TH);
  mtx_geo(&B.m_geo);
  prog_load(&B.basic, ctr_basic_shbin, ctr_basic_shbin_size, 0);
  prog_load(&B.clip2, ctr_clip2_shbin, ctr_clip2_shbin_size, 0);
  prog_load(&B.mesh, ctr_mesh_shbin, ctr_mesh_shbin_size, 0);
  prog_load(&B.skin, ctr_skin_shbin, ctr_skin_shbin_size, 0);
  prog_load(&B.sep, ctr_bench_sep_shbin, ctr_bench_sep_shbin_size, 0);
  if (prog_load(&B.bench, ctr_bench_shbin, ctr_bench_shbin_size, 0)) {
    for (int i = 0; i < 8 && i < (int)B.bench.dvlb->numDVLE; i++) {
      shaderProgramInit(&B.bench_entry[i]);
      B.bench_entry_ok[i] = shaderProgramSetVsh(&B.bench_entry[i], &B.bench.dvlb->DVLE[i]) == 0;
    }
  }
  if (!B.basic.ok) {
    return 0;
  }
  make_cull_buffer();
  static const GPU_TEXCOLOR fmts[3] = {GPU_RGBA8, GPU_RGB565, GPU_ETC1};
  for (int i = 0; i < 6 && !B.dead; i++) {
    tex_make(i, 256, 256, fmts[i % 3], i >= 3);
  }
  return !B.dead;
}

static void teardown(void) {
  ctr_gpu_bench_restore(); /* the game's program and state; nothing points at ours any more */
  for (int i = 0; i < 8; i++) {
    if (B.bench_entry_ok[i]) {
      shaderProgramFree(&B.bench_entry[i]);
    }
  }
  prog_free(&B.basic);
  prog_free(&B.clip2);
  prog_free(&B.mesh);
  prog_free(&B.skin);
  prog_free(&B.sep);
  prog_free(&B.bench);
  ctr_linear_lock();
  for (int i = 0; i < 6; i++) {
    if (B.tex_ok[i]) {
      C3D_TexDelete(&B.tex[i]);
    }
  }
  if (!B.dead) {
    /* (C3D_RenderTargetDelete waits for the GPU: not after a hang) */
    if (B.t0) {
      C3D_RenderTargetDelete(B.t0);
    }
    if (B.t1) {
      C3D_RenderTargetDelete(B.t1);
    }
    if (B.t2) {
      C3D_RenderTargetDelete(B.t2);
    }
  }
  ctr_linear_unlock();
  lin_free(B.rb);
  lin_free(B.scratch);
  lin_free(B.fence_src);
  lin_free(B.fence_dst);
  lin_free(B.imm);
  lin_free(B.cull_s);
}

int ctr_gpu_bench_run(void) {
  memset(&B, 0, sizeof(B));
  B.start_tick = svcGetSystemTick();
  bench_file("===== pica_bench session =====");
  wait_gpu_idle(1000000000LL);
  /* the performance log: on for the bench (back off after it unless gpu_perflog turns it on) */
  const int pl_was_on = s_pl.on;
  B.pl_ok = pl_was_on || R_SUCCEEDED(GSPGPU_SetPerfLogMode(true));
  if (B.pl_ok) {
    svcSleepThread(50000000LL); /* a few vblanks for the vblank interval (the unit) */
    GSPGPU_PerfLog p;
    if (R_SUCCEEDED(GSPGPU_GetPerfLog(&p))) {
      B.upm = (float)perflog_units_per_ms(&p);
      B.vblank_raw = p.entries[PL_VBLANK0].lastDurationUs;
    }
  }
  bool is_new = false;
  APT_CheckNew3DS(&is_new);
  const int ok = setup();
  bench_emitf("[bench] begin v=1 new3ds=%d emulator=%d perflog=%d upm=%.1f vblank_raw=%lu hw=%d "
              "hs_interval=%u setup=%d t1=%d t2=%d textures=%d%d%d%d%d%d programs=basic%d,clip2%d,mesh%d,"
              "skin%d,bench%d,sep%d",
              is_new ? 1 : 0, ctr_gpu_is_emulator(), B.pl_ok, B.upm, (unsigned long)B.vblank_raw,
              B.hw_ok, HS_INTERVAL, ok,
              B.t1 != NULL, B.t2 != NULL, B.tex_ok[0], B.tex_ok[1], B.tex_ok[2], B.tex_ok[3],
              B.tex_ok[4], B.tex_ok[5], B.basic.ok, B.clip2.ok, B.mesh.ok, B.skin.ok,
              B.bench.ok, B.sep.ok);
  if (ok) {
    b1();
    b2(B2_NOSTATE, "B2.nostate");
    b2(B2_REBIND_CLR, "B2.rebind_clr");
    b2(B2_REBIND, "B2.rebind");
    b2(B2_ALT2_CLR, "B2.alt2_clr");
    b2(B2_ALT2, "B2.alt2");
    b2(B2_TEXENV, "B2.texenv");
    b2(B2_UNIF_F32, "B2.unif_f32");
    b2(B2_UNIF_F24, "B2.unif_f24");
    b4(0);
    if (!B.dead) {
      cull_check();
    }
    b5();
    b6();
    b7_one("B7.px1", 0);
    b7_one("B7.zero", 1);
    b8(B.t0, "n");
    /* register settings the game never used on hardware: colour buffer reads off, the
     * post-vertex cache size, the 32x32 block mode (and its display transfer) */
    ctr_log_sync();
    b6_fill("B6.cbread0", (Fill){-1, 1, 1, 0, 0, 1, 0});
    b6_cbread_probe();
    b4(1);
    b8(B.t2, "b32");
    /* everything so far is on the card before the tests that could hang the GPU: the bench's
     * own shaders, then the game's unproven new ctr_mesh and ctr_skin */
    ctr_log_sync();
    b2(B2_PROG_ENTRY, "B2.prog_entry");
    b2(B2_PROG_SEP, "B2.prog_sep");
    static const char* const ids[8] = {"B3.v7", "B3.v7b", "B3.v14", "B3.v21",
                                       "B3.v35", "B3.v53", "B3.o3", "B3.o4"};
    for (int i = 0; i < 8; i++) {
      if (i != 1) {
        b3_entry(ids[i], i);
      }
    }
    b3_mesh_new();
    b3_skin_new();
  }
  const double secs = (double)(svcGetSystemTick() - B.start_tick) / SYSCLOCK_ARM11;
  bench_emitf("[bench] end tests=%d seconds=%.1f stopped=%d", B.tests, secs, B.dead);
  teardown();
  if (!pl_was_on && B.pl_ok) {
    GSPGPU_SetPerfLogMode(false);
  }
  ctr_console_status("");
  ctr_log_sync();
  return B.dead;
}
