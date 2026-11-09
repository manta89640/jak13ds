#pragma once

/*
 * The PICA200's early depth test, checked at boot on the console the game runs on
 * (ctr_gpu_early.c; 3DS only, called by ctr_gpu_citro3d.c's ctr_gpu_early_depth_selftest).
 */

#include <3ds.h>
#include <citro3d.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  C3D_FrameBuf* top;     /* the top render target (its own memory, mode and formats) */
  shaderProgram_s* prog; /* the game's ctr_basic program (it outlives the test) */
  int uloc_projection;   /* its projection uniform */
  int func;              /* GPU_EARLYDEPTHFUNC the game uses */
  u32 clear;             /* EARLYDEPTH_DATA the game uses */
  int emulator;          /* Azahar: no early depth test to check */
} CtrEarlyTestIn;

enum {
  CTR_EARLY_OK_BLOCK8 = 1,  /* the top target as it is */
  CTR_EARLY_OK_BLOCK32 = 2, /* a 256x416 target in 32x32 block mode */
};

/* bits CTR_EARLY_OK_*; status: one line for the log */
int ctr_early_selftest(const CtrEarlyTestIn* in, char* status, int size);

/* the early depth data made valid for the framebuffer citro3d has bound: the bind sent first,
 * then EARLYDEPTH_DATA and EARLYDEPTH_CLEAR (frame begin, after a render target switch) */
void ctr_gpu_early_establish(void);

#ifdef __cplusplus
}
#endif
