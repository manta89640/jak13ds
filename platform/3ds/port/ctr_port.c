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
 * malloc for the EE memory (48 MB in the small layout), so set the linear heap explicitly; the
 * regular heap gets the rest. Linear memory holds the renderer's vertex ring buffer, textures and
 * the loaded level backgrounds (two big levels can need ~20 MB). 32 MB leaves ~17 MB of the regular
 * heap free in the 124 MB mode (about 63 MB of it is used, mostly by the 48 MB of EE memory). */
u32 __ctru_linear_heap_size = 32 << 20;

static int s_console = 0;
static FILE* s_tee_file;
/* bottom screen: status lines (perf stats) at the top, the log below */
static PrintConsole s_stat_con;
static PrintConsole s_log_con;
#define STATUS_LINES 3
static volatile int s_gpu_active = 0;

void ctr_port_set_gpu_active(int active) {
  s_gpu_active = active;
}
static int s_irrst = 0;
static int s_syscore = 0;
static u32* s_soc_buffer = NULL;

int ctr_platform_init(int enable_console) {
  /* paths without a device name ("/3ds/jak1/...") then refer to the SD card, and
   * std::filesystem treats them as absolute ("sdmc:/..." would be a relative path to it) */
  chdir("sdmc:/");
  osSetSpeedupEnable(true); /* 804 MHz + L2 cache on New 3DS, no-op otherwise */
  gfxInitDefault();
  if (enable_console) {
    consoleInit(GFX_BOTTOM, &s_stat_con);
    consoleInit(GFX_BOTTOM, &s_log_con);
    consoleSetWindow(&s_stat_con, 0, 0, 40, STATUS_LINES);
    consoleSetWindow(&s_log_con, 0, STATUS_LINES, 40, 30 - STATUS_LINES);
    consoleSelect(&s_log_con);
    s_console = 1;
  }
  ctr_thread_install_crash_handler();
  /* Let the app use part of the system core (core 1) for the IO / IOP threads, so that core 0 is
   * left to the game logic (EE thread). 80% of core 1; the system keeps the rest. */
  /* Experimental, off by default: in Azahar, boot hangs at the first IOP file load when the IOP
   * thread runs on core 1 (not investigated further; untested on hardware). Turned on by the file
   * sdmc:/3ds/jak1/use_syscore. */
  if (access("/3ds/jak1/use_syscore", F_OK) == 0) {
    s_syscore = R_SUCCEEDED(APT_SetAppCpuTimeLimit(80)) ? 1 : 0;
  } else {
    s_syscore = 0;
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

/* abort() (failed asserts, lg::die, std::terminate): show the crash screen. The message (assert
 * text, lg::die message) was printed just before, so it's in the "last log lines". The newlib
 * default ends in exit(), which without the Homebrew Launcher jumps to address 0. */
void abort(void) {
  char msg[96];
  snprintf(msg, sizeof(msg), "abort() called from %p\n(arm-none-eabi-addr2line -e gk.elf)",
           __builtin_return_address(0));
  ctr_crash("The game stopped (abort)", msg);
}

/* ---------------- stdout/stderr tee ---------------- */

static devoptab_t s_tee_out, s_tee_err;
static const devoptab_t* s_orig_out = NULL;
static const devoptab_t* s_orig_err = NULL;
static LightLock s_tee_lock;
static int s_tee_lock_init = 0;

/* the last output, for the crash screen */
#define TAIL_SIZE 4096
static char s_tail[TAIL_SIZE];
static unsigned s_tail_pos = 0; /* total bytes written */

static void tail_append(const char* ptr, size_t len) {
  for (size_t i = 0; i < len; i++) {
    s_tail[s_tail_pos % TAIL_SIZE] = ptr[i];
    s_tail_pos++;
  }
}

static void tee_lock(void) {
  if (!s_tee_lock_init) {
    LightLock_Init(&s_tee_lock);
    s_tee_lock_init = 1;
  }
  LightLock_Lock(&s_tee_lock);
}

static void tee_unlock(void) {
  LightLock_Unlock(&s_tee_lock);
}

static void tee_to_file(const char* ptr, size_t len) {
  /* also to the debugger / emulator log (Azahar: Debug.Emulated) */
  svcOutputDebugString(ptr, (s32)len);
  tail_append(ptr, len);
  if (!s_tee_file) {
    return;
  }
  fwrite(ptr, 1, len, s_tee_file);
  if (memchr(ptr, '\n', len)) {
    fflush(s_tee_file);
  }
}

static ssize_t tee_write_out(struct _reent* r, void* fd, const char* ptr, size_t len) {
  ssize_t rv = (ssize_t)len;
  tee_lock();
  tee_to_file(ptr, len);
  if (s_orig_out && s_orig_out->write_r) {
    rv = s_orig_out->write_r(r, fd, ptr, len);
  }
  tee_unlock();
  return rv;
}

static ssize_t tee_write_err(struct _reent* r, void* fd, const char* ptr, size_t len) {
  ssize_t rv = (ssize_t)len;
  tee_lock();
  tee_to_file(ptr, len);
  if (s_orig_err && s_orig_err->write_r) {
    rv = s_orig_err->write_r(r, fd, ptr, len);
  }
  tee_unlock();
  return rv;
}

int ctr_stdio_tee(const char* path) {
  if (s_tee_file) {
    return 0;
  }
  s_tee_file = fopen(path, "w");
  if (!s_tee_file) {
    return -1;
  }
  if (!s_tee_lock_init) {
    LightLock_Init(&s_tee_lock);
    s_tee_lock_init = 1;
  }
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

/* ---------------- memory info ---------------- */

extern u32 __ctru_linear_heap_size;

void ctr_get_mem_info(ctr_mem_info* out) {
  memset(out, 0, sizeof(*out));
  out->app_region_total = (unsigned int)osGetMemRegionSize(MEMREGION_APPLICATION);
  out->app_region_used = (unsigned int)osGetMemRegionUsed(MEMREGION_APPLICATION);
  out->heap_size = (unsigned int)__ctru_heap_size;
  out->linear_size = (unsigned int)__ctru_linear_heap_size;
  out->linear_free = (unsigned int)linearSpaceFree();
  out->is_new3ds = ctr_is_new3ds();
  out->is_hbl = envIsHomebrew() ? 1 : 0;
  out->model = "unknown model";
  u8 model = 0;
  if (R_SUCCEEDED(cfguInit())) {
    if (R_SUCCEEDED(CFGU_GetSystemModel(&model))) {
      static const char* names[] = {"Old 3DS", "Old 3DS XL", "New 3DS", "Old 2DS", "New 3DS XL",
                                    "New 2DS XL"};
      if (model < sizeof(names) / sizeof(names[0])) {
        out->model = names[model];
      }
    }
    cfguExit();
  }
}

/* ---------------- status lines and crash screen ---------------- */

static void console_write_raw(const char* text) {
  if (s_orig_out && s_orig_out->write_r) {
    s_orig_out->write_r(_REENT, NULL, text, strlen(text));
  } else {
    /* no tee (yet): the default devoptab is the console */
    fputs(text, stdout);
  }
}

void ctr_console_status(const char* text) {
  if (!s_console || !s_orig_out) {
    return;
  }
  tee_lock();
  PrintConsole* prev = consoleSelect(&s_stat_con);
  console_write_raw("\x1b[2J\x1b[36m");
  console_write_raw(text);
  console_write_raw("\x1b[0m");
  consoleSelect(prev);
  tee_unlock();
}

static volatile int s_crashing = 0;

static void screen_refresh(void) {
  gfxFlushBuffers();
  if (!s_gpu_active) {
    gfxSwapBuffers();
  }
}

static void wait_for_button(void) {
  /* wait for the buttons to be released first, then pressed */
  for (int released = 0;;) {
    hidScanInput();
    u32 held = hidKeysHeld();
    if (!(held & (KEY_START | KEY_A))) {
      released = 1;
    } else if (released) {
      break;
    }
    screen_refresh();
    svcSleepThread(16 * 1000 * 1000LL);
  }
}

/* print the last `max_lines` lines of the output, each cut to the screen width */
static void print_tail(int max_lines) {
  unsigned count = s_tail_pos < TAIL_SIZE ? s_tail_pos : TAIL_SIZE;
  unsigned start = s_tail_pos - count;
  /* find the start of the last max_lines lines */
  int lines = 0;
  unsigned i = s_tail_pos;
  while (i > start) {
    char c = s_tail[(i - 1) % TAIL_SIZE];
    if (c == '\n' && i != s_tail_pos) {
      if (++lines >= max_lines) {
        break;
      }
    }
    i--;
  }
  char line[41];
  int col = 0;
  for (; i < s_tail_pos; i++) {
    char c = s_tail[i % TAIL_SIZE];
    if (c == '\n' || col == 39) {
      line[col] = 0;
      console_write_raw(line);
      console_write_raw("\n");
      col = 0;
      if (c != '\n') {
        /* skip the rest of a long line */
        while (i + 1 < s_tail_pos && s_tail[(i + 1) % TAIL_SIZE] != '\n') {
          i++;
        }
      }
    } else if (c >= 32 && c < 127) {
      line[col++] = c;
    }
  }
  if (col) {
    line[col] = 0;
    console_write_raw(line);
    console_write_raw("\n");
  }
}

static void show_screen(const char* color, const char* header, const char* title,
                        const char* detail, int with_tail, const char* footer) {
  if (!s_console) {
    return;
  }
  consoleSetWindow(&s_log_con, 0, 0, 40, 30);
  consoleSelect(&s_log_con);
  console_write_raw("\x1b[2J");
  console_write_raw(color);
  console_write_raw(header);
  console_write_raw("\x1b[0m\n");
  console_write_raw(title);
  console_write_raw("\n");
  if (detail && detail[0]) {
    console_write_raw(detail);
    console_write_raw("\n");
  }
  if (with_tail) {
    console_write_raw("\x1b[33mLast log lines:\x1b[0m\n");
    print_tail(14);
  }
  console_write_raw("\n");
  console_write_raw(footer);
  screen_refresh();
}

void ctr_crash(const char* title, const char* detail) {
  if (__atomic_exchange_n(&s_crashing, 1, __ATOMIC_SEQ_CST)) {
    /* another thread is already showing its crash: stop here */
    for (;;) {
      svcSleepThread(1000 * 1000 * 1000LL);
    }
  }
  char msg[512];
  int len = snprintf(msg, sizeof(msg), "\n*** CRASH: %s\n%s\n", title, detail ? detail : "");
  svcOutputDebugString(msg, len);
  /* don't wait forever for a lock held by a thread that crashed in the middle of a print */
  int locked = 0;
  if (s_tee_lock_init) {
    for (int i = 0; i < 100 && !locked; i++) {
      locked = LightLock_TryLock(&s_tee_lock) == 0;
      if (!locked) {
        svcSleepThread(1000 * 1000LL);
      }
    }
  }
  if (s_tee_file) {
    fputs(msg, s_tee_file);
    fflush(s_tee_file);
  }
  show_screen("\x1b[31m", "*** OpenGOAL crashed ***", title, detail, 1,
              "Logs: sdmc:/3ds/jak1/data/log/\nPlease report stdout.log and gk.log.\n"
              "\x1b[32mPress START to exit.\x1b[0m");
  wait_for_button();
  svcExitProcess();
  for (;;) {
  }
}

void ctr_message_wait(const char* title, const char* text) {
  tee_lock();
  show_screen("\x1b[33m", "*** OpenGOAL ***", title, text, 0,
              "\x1b[32mPress START to continue.\x1b[0m");
  tee_unlock();
  wait_for_button();
  tee_lock();
  consoleSetWindow(&s_log_con, 0, STATUS_LINES, 40, 30 - STATUS_LINES);
  console_write_raw("\x1b[2J");
  tee_unlock();
}

/* ---------------- CPU exceptions ---------------- */

static u8 s_exc_stack[32 * 1024] __attribute__((aligned(8)));

static void exception_handler(ERRF_ExceptionInfo* excep, CpuRegisters* regs) {
  const char* type = "exception";
  switch (excep->type) {
    case ERRF_EXCEPTION_PREFETCH_ABORT:
      type = "prefetch abort";
      break;
    case ERRF_EXCEPTION_DATA_ABORT:
      type = "data abort";
      break;
    case ERRF_EXCEPTION_UNDEFINED:
      type = "undefined instruction";
      break;
    case ERRF_EXCEPTION_VFP:
      type = "VFP exception";
      break;
  }
  char detail[256];
  snprintf(detail, sizeof(detail),
           "%s at pc %08lx\nlr %08lx  sp %08lx\naddress %08lx  fsr %08lx\n"
           "(arm-none-eabi-addr2line -f -e gk.elf <pc> <lr>)",
           type, (unsigned long)regs->pc, (unsigned long)regs->lr, (unsigned long)regs->sp,
           (unsigned long)excep->far, (unsigned long)excep->fsr);
  ctr_crash("CPU exception", detail);
}

void ctr_thread_install_crash_handler(void) {
  threadOnException(exception_handler, s_exc_stack + sizeof(s_exc_stack),
                    WRITE_DATA_TO_HANDLER_STACK);
}

/* ---------------- threads on other cores ---------------- */

typedef struct {
  void* (*fn)(void*);
  void* arg;
} ThreadStart;

static void thread_trampoline(void* p) {
  ThreadStart start = *(ThreadStart*)p;
  free(p);
  start.fn(start.arg);
}

int ctr_syscore_available(void) {
  return s_syscore;
}

int ctr_thread_create(void* (*fn)(void*), void* arg, unsigned int stack_size, int prio, int core,
                      void** handle) {
  if (core == CTR_CORE_SYS && !s_syscore) {
    core = CTR_CORE_APP;
  }
  ThreadStart* start = (ThreadStart*)malloc(sizeof(ThreadStart));
  if (!start) {
    return -1;
  }
  start->fn = fn;
  start->arg = arg;
  Thread t = threadCreate(thread_trampoline, start, stack_size, prio, core, false);
  if (!t && core != CTR_CORE_APP) {
    t = threadCreate(thread_trampoline, start, stack_size, prio, CTR_CORE_APP, false);
  }
  if (!t) {
    free(start);
    return -1;
  }
  *handle = t;
  return 0;
}

void ctr_thread_join(void* handle) {
  Thread t = (Thread)handle;
  threadJoin(t, U64_MAX);
  threadFree(t);
}
