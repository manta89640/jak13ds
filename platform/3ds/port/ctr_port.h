#pragma once

/*
 * (AI-assisted)
 * Thin C interface to libctru for the OpenGOAL runtime.
 *
 * <3ds.h> defines u8/u16/u32/s32... as typedefs of the newlib <stdint.h> types
 * (u32 = uint32_t = unsigned long), which conflicts with common/common_types.h on the 3DS
 * (u32 = unsigned int). So only ctr_port.c includes <3ds.h>; runtime code talks to libctru through
 * these functions, which use plain C types.
 */

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize services and the bottom-screen console. Returns 0 on success. */
int ctr_platform_init(int enable_console);
/* Append a line to sdmc:/3ds/jak1/boot.txt (boot progress, for hangs on hardware). */
void ctr_boot_mark(const char* step);
void ctr_platform_exit(void);

/* Call regularly from the main thread. Returns 0 when the app should quit (HOME -> close,
 * power off...). */
int ctr_main_loop(void);

/* 1 on New 3DS / New 2DS XL */
int ctr_is_new3ds(void);

/* Pad state in PS2 DualShock terms.
 * buttons: bit i = PadData::ButtonIndex i (SELECT=0, L3, R3, START, UP, RIGHT, DOWN, LEFT,
 *          L2, R2, L1, R1, TRIANGLE, CIRCLE, CROSS, SQUARE=15), 1 = pressed.
 * sticks: 0..255, 127/128 = neutral, y grows downwards like on the PS2. */
typedef struct {
  unsigned short buttons;
  unsigned char lx, ly, rx, ry;
} ctr_pad_state;

/* Scan HID and return the current state. Call from a single thread only. */
void ctr_pad_read(ctr_pad_state* out);

/* Start the soc:U service (BSD sockets) for the REPL listener. Uses `buffer_size` bytes of
 * memory (must be a multiple of 0x1000). Returns 0 on success. */
int ctr_net_init(unsigned int buffer_size);
void ctr_net_exit(void);

/* Copy everything written to stdout/stderr (console) into a file as well, flushed at every
 * newline, so logs survive a crash and can be read from the SD card. Returns 0 on success. */
int ctr_stdio_tee(const char* path);

/* Thread priorities (0x18 = highest for apps, 0x3F = lowest). The 3DS scheduler never time-slices
 * threads of the same priority, so a thread that polls (the EE, the IOP kernel) must run at a
 * lower priority than the threads it waits for. See docs/3ds-port/3ds_build.md. */
enum {
  CTR_PRIO_SOUND = 0x2E,    /* audio mixer: fills the DSP's buffers, must never be starved */
  CTR_PRIO_MAIN = 0x30,     /* main thread (libctru default): APT + gfx loop, mostly sleeping */
  CTR_PRIO_IO = 0x31,       /* short blocking helpers: fake ISO file reads, sound tick */
  CTR_PRIO_IOP = 0x34,      /* IOP kernel (polls during overlord init) */
  CTR_PRIO_DECI = 0x35,     /* listener */
  CTR_PRIO_WORKER = 0x36,   /* EE background worker */
  CTR_PRIO_EE = 0x3A,       /* the game (polls on RPC / DMA) */
};
/* Cores. Old 3DS: 0 (application), 1 (system core, a share of it with APT_SetAppCpuTimeLimit).
 * New 3DS: also 2 (fully available to the application) and 3. Threads can't move between cores
 * after they are created, and libctru's pthreads (std::thread) always use core 0. */
enum {
  CTR_CORE_APP = 0,
  CTR_CORE_SYS = 1, /* usable when ctr_platform_init got a time limit (ctr_syscore_available) */
};
/* Create a thread on a core (falls back to core 0 if that core isn't usable). The handle is for
 * ctr_thread_join. Returns 0 on success. */
int ctr_thread_create(void* (*fn)(void*), void* arg, unsigned int stack_size, int prio, int core,
                      void** handle);
/* Same, but fails (-2: no share of core 1, -1: thread not created) instead of using core 0. */
int ctr_thread_create_pinned(void* (*fn)(void*), void* arg, unsigned int stack_size, int prio,
                             int core, void** handle);
void ctr_thread_join(void* handle);
/* 1 if the IOP / IO threads should run on the system core (core 1): the use_syscore flag file,
 * and the app got a share of that core. */
int ctr_syscore_available(void);
/* The app's share of core 1 in percent (APT_SetAppCpuTimeLimit: 80, 30 if refused, 0: none). */
int ctr_core1_share(void);
/* Ask for a share of core 1 (if not done yet); returns the share in percent (0: refused). */
int ctr_core1_enable(void);

/* Audio output through the DSP (libctru ndsp): one stereo PCM16 channel that a software mixer
 * feeds, `nbufs` buffers of `frames` stereo frames each, resampled by the DSP from `rate` Hz.
 * Needs the DSP firmware (sdmc:/3ds/dspfirm.cdc on real hardware; Azahar's HLE accepts any file).
 * ctr_audio_init returns 0 on success, or a negative error (nothing was started). */
int ctr_audio_init(unsigned int rate, unsigned int frames, unsigned int nbufs);
void ctr_audio_exit(void);
/* The next buffer to fill (`frames` * 2 s16 samples, interleaved L/R), or NULL if all are still
 * queued to the DSP. Buffers are returned in order; fill it, then ctr_audio_submit it. */
short* ctr_audio_get_buffer(void);
void ctr_audio_submit(short* buffer);
/* Wait until the DSP finished a frame (about every 5 ms), or `us` microseconds. */
void ctr_audio_wait(unsigned int us);
/* Frames the DSP had to skip because the mixer was late (total since init). */
unsigned int ctr_audio_dropped_frames(void);

/* Sound settings from the flag file sdmc:/3ds/jak1/sound: returns 1 if it exists (audio output
 * on); *core = the core for the mixer thread (the file's content, default 1). */
int ctr_sound_config(int* core);

/* Rough CPU clock and memory load latencies (L1, L2, RAM), one line for the log: shows whether the
 * New 3DS speedup (804 MHz, L2 cache) is on. Takes ~50 ms and 16 MB of heap for a moment. */
void ctr_hw_probe(char* out, int size);

/* Set the priority of the calling thread. */
void ctr_thread_set_priority(int prio);
/* libctru's linear heap (linearAlloc/linearFree, C3D_TexInit, ndsp) has no lock of its own: code
 * that allocates or frees linear or VRAM memory while other threads can do the same (the level
 * loader thread, the render thread, sound init on the IOP thread) holds this (recursive). */
void ctr_linear_lock(void);
void ctr_linear_unlock(void);
/* The calling thread's id (kernel thread id). */
unsigned int ctr_thread_current_id(void);
/* Sleep for at least `us` microseconds (0 = yield). */
void ctr_thread_sleep_us(unsigned int us);

/* Free application memory in bytes (heap), and linear memory. */
unsigned int ctr_app_mem_free(void);
unsigned int ctr_linear_mem_free(void);

typedef struct {
  unsigned int app_region_total; /* APPLICATION memory region: 64 MB (Old 3DS), 124 MB (New 3DS
                                    extended mode), less when started as an applet */
  unsigned int app_region_used;
  unsigned int heap_size;        /* malloc heap */
  unsigned int linear_size;      /* linear heap (GPU buffers, textures) */
  unsigned int linear_free;
  int is_new3ds;
  int is_hbl;                    /* started from the Homebrew Launcher (3dsx) */
  const char* model;             /* "New 3DS XL", "Old 2DS"... */
} ctr_mem_info;
void ctr_get_mem_info(ctr_mem_info* out);

/* Bottom screen: the top 3 lines are a status area (performance stats), the rest is the log.
 * Replaces the status area with `text` (may contain newlines, at most 3 lines). */
void ctr_console_status(const char* text);

/* Show a crash / error screen on the bottom screen: title, detail, the last lines of the log and
 * where the logs are, then wait for START (or A) and exit the app. Never returns. Safe to call
 * from any thread; only the first caller shows its screen. */
void ctr_crash(const char* title, const char* detail) __attribute__((noreturn));

/* Show a message and wait for START / A, without exiting (for example: not enough memory). */
void ctr_message_wait(const char* title, const char* text);

/* Install the CPU exception handler (data abort, prefetch abort, undefined instruction) for the
 * calling thread; it shows the crash screen. Call at the start of every thread. */
void ctr_thread_install_crash_handler(void);

#ifdef __cplusplus
}
#endif
