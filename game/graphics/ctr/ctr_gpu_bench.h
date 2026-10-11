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

/* config.ini pica_bench: the GPU microbenchmark (`[bench] ...` lines in gk.log and
 * data/log/bench.txt; platform/3ds/tools/bench_report.py). Call on the thread that owns the GPU,
 * after ctr_gpu_init and before anything else draws (the render thread not started). ~20-60 s.
 * Returns 0, or 1 when it stopped because the GPU didn't finish a test (then the GPU is hung and
 * the game won't draw either). */
int ctr_gpu_bench_run(void);
/* ctr_gpu_citro3d.c: the GPU layer takes citro3d back after the bench (its program bound again,
 * citro3d's state as the game set it up and all of it marked for sending, the raw draws' caches
 * dropped as after the HOME Menu) */
void ctr_gpu_bench_restore(void);

#ifdef __cplusplus
}
#endif
