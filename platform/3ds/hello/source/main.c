// (AI-assisted)
// Minimal libctru + citro3d smoke test for the OpenGOAL 3DS port:
// top screen cleared to a color with citro3d, text console on the bottom screen.
#include <3ds.h>
#include <citro3d.h>
#include <stdint.h>
#include <stdio.h>

#define DISPLAY_TRANSFER_FLAGS                                                           \
  (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |     \
   GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
   GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

int main(void) {
  // boot test for the CIA settings (platform/3ds/cia/boottest.rsf): proves main was reached
  FILE* f = fopen("sdmc:/3ds/jak1/boottest.txt", "w");
  if (f) {
    fputs("main reached\n", f);
    fclose(f);
  }
  gfxInitDefault();
  consoleInit(GFX_BOTTOM, NULL);
  C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);

  C3D_RenderTarget* top = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
  C3D_RenderTargetSetOutput(top, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

  printf("OpenGOAL Jak1 boot test (CIA settings)\n");
  printf("app memory region: %lu KiB\n",
         (unsigned long)(osGetMemRegionSize(MEMREGION_APPLICATION) / 1024));
  printf("sizeof(void*) = %u\n", (unsigned)sizeof(void*));
  printf("linear free: %lu KiB\n", (unsigned long)(linearSpaceFree() / 1024));
  printf("heap: %lu KiB free\n", (unsigned long)(osGetMemRegionFree(MEMREGION_APPLICATION) / 1024));
  bool is_new = false;
  APT_CheckNew3DS(&is_new);
  printf("New 3DS: %s\n", is_new ? "yes" : "no");
  printf("\nPress START to exit.\n");

  u32 frame = 0;
  while (aptMainLoop()) {
    hidScanInput();
    if (hidKeysDown() & KEY_START)
      break;

    // slowly pulse the orange-ish clear color (RGBA8, R in the top byte)
    u8 g = (u8)(0x60 + ((frame >> 1) & 0x3f));
    u32 color = (0xE0u << 24) | ((u32)g << 16) | (0x20u << 8) | 0xFFu;

    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C3D_RenderTargetClear(top, C3D_CLEAR_ALL, color, 0);
    C3D_FrameDrawOn(top);
    C3D_FrameEnd(0);

    printf("\x1b[10;0Hframe %lu", (unsigned long)frame);
    frame++;
  }

  C3D_RenderTargetDelete(top);
  C3D_Fini();
  gfxExit();
  return 0;
}
