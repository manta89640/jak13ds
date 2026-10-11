/*
 * (AI-assisted)
 * GPU timing instrumentation of the 3DS renderer (ctr_gpu_bench.h; 3DS only, next to
 * ctr_gpu_citro3d.c). Everything here is off unless config.ini asks for it, and it only talks to
 * the GPU through the GSP service and citro3d (no system calls of its own, no IO mappings:
 * platform/3ds/tools/check_svc.py, gk.rsf).
 *
 *  - gpu_perflog: the GSP's performance log, one line per stats window.
 */

#include "game/graphics/ctr/ctr_gpu_bench.h"

#include <3ds.h>
#include <citro3d.h>
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

/* raw GSP units per millisecond, from the vblank interval (59.83 Hz) when it is plausible; else
 * microseconds (libctru) */
static double perflog_units_per_ms(const GSPGPU_PerfLog* p) {
  const u32 v = p->entries[PL_VBLANK0].lastDurationUs;
  if (v > 1000u && v < 1000000000u) {
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
