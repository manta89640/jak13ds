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

// Boot test for the CIA settings (make_cia.sh --boottest): each line goes to sdmc:/boottest.txt and
// sdmc:/3ds/jak1/boottest.txt (the SD root too, in case that folder is the problem), with the
// title's program id (make_cia.sh builds several variants of the settings, one title each). The
// first line is written before the HOME Menu handshake (aptInit), as in gk
// (platform/3ds/port/ctr_port.c), so the file tells whether the title started at all, stopped in
// the handshake, or reached main. main also shows the results on the bottom screen: if the screens
// come up but no file appears, it is the SD card access that fails.
static unsigned long long s_program_id;
static Result s_rc_fs, s_rc_sdmc, s_rc_pid;
static int s_ok_root, s_ok_jak1;

static int mark_to(const char* path, const char* mode, const char* step) {
  FILE* f = fopen(path, mode);
  if (!f) {
    return 0;
  }
  fprintf(f, "%016llx %s\n", s_program_id, step);
  fclose(f);
  return 1;
}

static void mark(const char* mode, const char* step) {
  s_ok_root = mark_to("sdmc:/boottest.txt", mode, step);
  s_ok_jak1 = mark_to("sdmc:/3ds/jak1/boottest.txt", mode, step);
}

void __appInit(void) {
  srvInit();
  s_rc_fs = fsInit();
  s_rc_sdmc = archiveMountSdmc();
  {
    u32 pid = 0;
    FS_ProgramInfo info;
    s_rc_pid = svcGetProcessId(&pid, CUR_PROCESS_HANDLE);
    if (R_SUCCEEDED(s_rc_pid)) {
      s_rc_pid = FSUSER_GetProgramLaunchInfo(&info, pid);
      if (R_SUCCEEDED(s_rc_pid)) {
        s_program_id = info.programId;
      }
    }
  }
  // append: several variants (titles) write to the same files, one after the other
  mark("a", "0 started: services, SD card");
  aptInit();
  mark("a", "1 APT (HOME Menu handshake)");
  hidInit();
}

int main(void) {
  mark("a", "2 main reached");
  gfxInitDefault();
  consoleInit(GFX_BOTTOM, NULL);
  C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);

  C3D_RenderTarget* top = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
  C3D_RenderTargetSetOutput(top, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);

  printf("OpenGOAL Jak1 boot test (CIA settings)\n");
  printf("title %016llx\n", s_program_id);
  printf("fsInit %08lx sdmc %08lx id %08lx\n", (unsigned long)s_rc_fs, (unsigned long)s_rc_sdmc,
         (unsigned long)s_rc_pid);
  printf("app memory region: %lu KiB\n",
         (unsigned long)(osGetMemRegionSize(MEMREGION_APPLICATION) / 1024));
  printf("sizeof(void*) = %u\n", (unsigned)sizeof(void*));
  printf("linear free: %lu KiB\n", (unsigned long)(linearSpaceFree() / 1024));
  printf("heap: %lu KiB free\n", (unsigned long)(osGetMemRegionFree(MEMREGION_APPLICATION) / 1024));
  bool is_new = false;
  APT_CheckNew3DS(&is_new);
  printf("New 3DS: %s\n", is_new ? "yes" : "no");
  printf("\nPress START to exit.\n");
  {
    char line[128];
    snprintf(line, sizeof(line), "3 screens up: app memory %lu KiB, New 3DS %s",
             (unsigned long)(osGetMemRegionSize(MEMREGION_APPLICATION) / 1024), is_new ? "yes" : "no");
    mark("a", line);
  }
  printf("boottest.txt written: SD root %s, 3ds/jak1 %s\n", s_ok_root ? "yes" : "NO",
         s_ok_jak1 ? "yes" : "NO");

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
