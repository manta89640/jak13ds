/*
 * (AI-assisted)
 * libctru side of the OpenGOAL 3DS platform layer. See ctr_port.h.
 */

#include "ctr_port.h"

#include <3ds.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/iosupport.h>

/* libctru splits free memory between the regular heap (malloc) and the linear heap (GPU/DSP
 * buffers) at startup, capping the regular heap at 24 MB by default. The runtime needs one big
 * malloc for the EE memory (48 MB in the small layout), so keep the linear heap small; the
 * regular heap gets the rest. Raise this when the renderer needs more linear memory. */
u32 __ctru_linear_heap_size = 8 << 20;

static int s_console = 0;
static volatile int s_gpu_active = 0;

void ctr_port_set_gpu_active(int active) {
  s_gpu_active = active;
}
static int s_irrst = 0;
static u32* s_soc_buffer = NULL;

int ctr_platform_init(int enable_console) {
  /* paths without a device name ("/3ds/jak1/...") then refer to the SD card, and
   * std::filesystem treats them as absolute ("sdmc:/..." would be a relative path to it) */
  chdir("sdmc:/");
  osSetSpeedupEnable(true); /* 804 MHz + L2 cache on New 3DS, no-op otherwise */
  gfxInitDefault();
  if (enable_console) {
    consoleInit(GFX_BOTTOM, NULL);
    s_console = 1;
  }
  /* C-stick / ZL / ZR on New 3DS (and the Circle Pad Pro) */
  s_irrst = R_SUCCEEDED(irrstInit()) ? 1 : 0;
  return 0;
}

void ctr_platform_exit(void) {
  ctr_net_exit();
  if (s_irrst) {
    irrstExit();
    s_irrst = 0;
  }
  gfxExit();
}

int ctr_main_loop(void) {
  int running = aptMainLoop() ? 1 : 0;
  if (s_console) {
    if (s_gpu_active) {
      /* citro3d swaps the screens; the console screen is single buffered, flush it only */
      gfxFlushBuffers();
    } else {
      gfxFlushBuffers();
      gfxSwapBuffers();
    }
  }
  return running;
}

int ctr_is_new3ds(void) {
  bool is_new = false;
  APT_CheckNew3DS(&is_new);
  return is_new ? 1 : 0;
}

static unsigned char stick_axis(int v, int invert) {
  /* circle pad reports about +-156, c-stick about +-146 */
  int s = (v * 128) / 150;
  if (invert) {
    s = -s;
  }
  s += 128;
  if (s < 0) {
    s = 0;
  }
  if (s > 255) {
    s = 255;
  }
  return (unsigned char)s;
}

enum {
  B_SELECT = 0,
  B_L3,
  B_R3,
  B_START,
  B_UP,
  B_RIGHT,
  B_DOWN,
  B_LEFT,
  B_L2,
  B_R2,
  B_L1,
  B_R1,
  B_TRIANGLE,
  B_CIRCLE,
  B_CROSS,
  B_SQUARE
};

void ctr_pad_read(ctr_pad_state* out) {
  hidScanInput();
  if (s_irrst) {
    irrstScanInput();
  }
  u32 held = hidKeysHeld();
  unsigned short b = 0;
  /* face buttons by position: A right, B bottom, Y left, X top */
  if (held & KEY_A) b |= 1 << B_CIRCLE;
  if (held & KEY_B) b |= 1 << B_CROSS;
  if (held & KEY_Y) b |= 1 << B_SQUARE;
  if (held & KEY_X) b |= 1 << B_TRIANGLE;
  if (held & KEY_L) b |= 1 << B_L1;
  if (held & KEY_R) b |= 1 << B_R1;
  if (held & KEY_ZL) b |= 1 << B_L2;
  if (held & KEY_ZR) b |= 1 << B_R2;
  if (held & KEY_START) b |= 1 << B_START;
  if (held & KEY_SELECT) b |= 1 << B_SELECT;
  if (held & KEY_DUP) b |= 1 << B_UP;
  if (held & KEY_DDOWN) b |= 1 << B_DOWN;
  if (held & KEY_DLEFT) b |= 1 << B_LEFT;
  if (held & KEY_DRIGHT) b |= 1 << B_RIGHT;
  /* no stick clicks on the 3DS: touching the lower/upper half of the touch screen */
  if (held & KEY_TOUCH) {
    touchPosition t;
    hidTouchRead(&t);
    b |= 1 << (t.py < 120 ? B_L3 : B_R3);
  }
  out->buttons = b;

  circlePosition cp;
  hidCircleRead(&cp);
  out->lx = stick_axis(cp.dx, 0);
  out->ly = stick_axis(cp.dy, 1);

  circlePosition cs = {0, 0};
  if (s_irrst) {
    irrstCstickRead(&cs);
  }
  out->rx = stick_axis(cs.dx, 0);
  out->ry = stick_axis(cs.dy, 1);
}

int ctr_net_init(unsigned int buffer_size) {
  if (s_soc_buffer) {
    return 0;
  }
  s_soc_buffer = (u32*)memalign(0x1000, buffer_size);
  if (!s_soc_buffer) {
    return -1;
  }
  if (R_FAILED(socInit(s_soc_buffer, buffer_size))) {
    free(s_soc_buffer);
    s_soc_buffer = NULL;
    return -2;
  }
  return 0;
}

void ctr_net_exit(void) {
  if (s_soc_buffer) {
    socExit();
    free(s_soc_buffer);
    s_soc_buffer = NULL;
  }
}

extern u32 __ctru_heap_size;
unsigned int ctr_app_mem_free(void) {
  /* the whole application region is committed to the heaps at startup: report the heap size */
  return (unsigned int)__ctru_heap_size;
}

unsigned int ctr_linear_mem_free(void) {
  return (unsigned int)linearSpaceFree();
}

void ctr_thread_set_priority(int prio) {
  svcSetThreadPriority(CUR_THREAD_HANDLE, prio);
}

void ctr_thread_sleep_us(unsigned int us) {
  svcSleepThread((s64)us * 1000);
}

/* abort() (failed asserts, lg::die, std::terminate): flush the log and stop with svcBreak, which
 * the emulator reports ("svcBreak") and a debugger catches. The newlib default ends in exit(),
 * which without the Homebrew Launcher jumps to address 0. */
static FILE* s_tee_file;
void abort(void) {
  static const char msg[] = "gk: abort() called";
  svcOutputDebugString(msg, sizeof(msg) - 1);
  if (s_tee_file) {
    fputs("\ngk: abort() called\n", s_tee_file);
    fflush(s_tee_file);
  }
  svcBreak(USERBREAK_PANIC);
  for (;;) {
  }
}

/* ---------------- stdout/stderr tee ---------------- */

static devoptab_t s_tee_out, s_tee_err;
static const devoptab_t* s_orig_out = NULL;
static const devoptab_t* s_orig_err = NULL;
static LightLock s_tee_lock;

static void tee_to_file(const char* ptr, size_t len) {
  /* also to the debugger / emulator log (Azahar: Debug.Emulated) */
  svcOutputDebugString(ptr, (s32)len);
  if (!s_tee_file) {
    return;
  }
  LightLock_Lock(&s_tee_lock);
  fwrite(ptr, 1, len, s_tee_file);
  if (memchr(ptr, '\n', len)) {
    fflush(s_tee_file);
  }
  LightLock_Unlock(&s_tee_lock);
}

static ssize_t tee_write_out(struct _reent* r, void* fd, const char* ptr, size_t len) {
  tee_to_file(ptr, len);
  if (s_orig_out && s_orig_out->write_r) {
    return s_orig_out->write_r(r, fd, ptr, len);
  }
  return (ssize_t)len;
}

static ssize_t tee_write_err(struct _reent* r, void* fd, const char* ptr, size_t len) {
  tee_to_file(ptr, len);
  if (s_orig_err && s_orig_err->write_r) {
    return s_orig_err->write_r(r, fd, ptr, len);
  }
  return (ssize_t)len;
}

int ctr_stdio_tee(const char* path) {
  if (s_tee_file) {
    return 0;
  }
  s_tee_file = fopen(path, "w");
  if (!s_tee_file) {
    return -1;
  }
  LightLock_Init(&s_tee_lock);
  s_orig_out = devoptab_list[STD_OUT];
  s_orig_err = devoptab_list[STD_ERR];
  memset(&s_tee_out, 0, sizeof(s_tee_out));
  memset(&s_tee_err, 0, sizeof(s_tee_err));
  if (s_orig_out) {
    s_tee_out = *s_orig_out;
  }
  if (s_orig_err) {
    s_tee_err = *s_orig_err;
  }
  s_tee_out.name = "tee_out";
  s_tee_out.write_r = tee_write_out;
  s_tee_err.name = "tee_err";
  s_tee_err.write_r = tee_write_err;
  devoptab_list[STD_OUT] = &s_tee_out;
  devoptab_list[STD_ERR] = &s_tee_err;
  /* line buffering on stdout so the file sees lines promptly */
  setvbuf(stdout, NULL, _IOLBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  return 0;
}
