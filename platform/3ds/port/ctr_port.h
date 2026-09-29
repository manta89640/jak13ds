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
  CTR_PRIO_MAIN = 0x30,     /* main thread (libctru default): APT + gfx loop, mostly sleeping */
  CTR_PRIO_IO = 0x31,       /* short blocking helpers: fake ISO file reads, sound tick */
  CTR_PRIO_IOP = 0x34,      /* IOP kernel (polls during overlord init) */
  CTR_PRIO_DECI = 0x35,     /* listener */
  CTR_PRIO_WORKER = 0x36,   /* EE background worker */
  CTR_PRIO_EE = 0x3A,       /* the game (polls on RPC / DMA) */
};
/* Set the priority of the calling thread. */
void ctr_thread_set_priority(int prio);
/* Sleep for at least `us` microseconds (0 = yield). */
void ctr_thread_sleep_us(unsigned int us);

/* Free application memory in bytes (heap), and linear memory. */
unsigned int ctr_app_mem_free(void);
unsigned int ctr_linear_mem_free(void);

#ifdef __cplusplus
}
#endif
