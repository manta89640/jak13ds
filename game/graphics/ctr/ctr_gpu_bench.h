#pragma once

/*!
 * @file ctr_gpu_bench.h
 * (AI-assisted)
 * GPU timing instrumentation of the 3DS renderer (game/graphics/ctr/ctr_gpu_bench.c, 3DS only;
 * docs/3ds-port/performance.md "GPU timing in one session"). Plain C types, like ctr_gpu.h.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* config.ini gpu_perflog: the GSP's performance log (GSPGPU_SetPerfLogMode / GetPerfLog: how
 * long each GPU engine was busy, recorded by the GSP between an operation's start and its
 * interrupt). start: once, with the GPU idle (after ctr_gpu_init); returns 0 or the GSP's error.
 * line: one log line for a stats window (the deltas since the previous call), given the window's
 * frames and the renderer's own GPU times summed over them (C3D draw time, fence busy time and
 * the frames that had one); returns its length, 0 when there's nothing to log (the GSP refused). */
int ctr_gpu_perflog_start(void);
int ctr_gpu_perflog_line(char* out, int size, int frames, double draw_ms_sum, double busy_ms_sum,
                         int busy_frames);

#ifdef __cplusplus
}
#endif
