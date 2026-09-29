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

/* Free application memory in bytes (heap), and linear memory. */
unsigned int ctr_app_mem_free(void);
unsigned int ctr_linear_mem_free(void);

#ifdef __cplusplus
}
#endif
