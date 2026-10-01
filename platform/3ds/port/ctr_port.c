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
/* (AI-assisted) 40 MB: the GPU reads level data only from linear memory or VRAM. With 32 MB,
 * village1 + jungle (28 MB of .c3l) left 0 KB free and 92 textures/meshes missing; the heap used
 * ~64-66 MB of its 81 MB on hardware. */
u32 __ctru_linear_heap_size = 40 << 20;

extern char* fake_heap_start;
extern char* fake_heap_end;
extern u32 __ctru_heap;
extern u32 __ctru_linear_heap;
extern u32 __ctru_heap_size;

/* libctru's __system_allocateHeaps, except that it doesn't stop the process when the 32 MB linear
 * heap doesn't fit (svcBreak before main: the screen just stays on the launch screen, e.g. for a
 * .3dsx started from the Homebrew Launcher, which only gets the memory of the app it runs in).
 * With less memory the heaps are smaller and main() explains what to do instead. */
void __system_allocateHeaps(void) {
  Handle reslimit = 0;
  if (R_FAILED(svcGetResourceLimit(&reslimit, CUR_PROCESS_HANDLE))) {
    svcBreak(USERBREAK_PANIC);
  }
  s64 max_commit = 0, cur_commit = 0;
  ResourceLimitType type = RESLIMIT_COMMIT;
  svcGetResourceLimitLimitValues(&max_commit, reslimit, &type, 1);
  svcGetResourceLimitCurrentValues(&cur_commit, reslimit, &type, 1);
  svcCloseHandle(reslimit);
  const u32 remaining = (u32)(max_commit - cur_commit) & ~0xFFFu;
  if (__ctru_linear_heap_size > remaining / 2) {
    __ctru_linear_heap_size = (remaining / 2) & ~0xFFFu;
  }
  __ctru_heap_size = remaining - __ctru_linear_heap_size;
  if (R_FAILED(svcControlMemory(&__ctru_heap, OS_HEAP_AREA_BEGIN, 0x0, __ctru_heap_size,
                                MEMOP_ALLOC, MEMPERM_READ | MEMPERM_WRITE))) {
    svcBreak(USERBREAK_PANIC);
  }
  if (R_FAILED(svcControlMemory(&__ctru_linear_heap, 0x0, 0x0, __ctru_linear_heap_size,
                                MEMOP_ALLOC_LINEAR, MEMPERM_READ | MEMPERM_WRITE))) {
    svcBreak(USERBREAK_PANIC);
  }
  mappableInit(OS_MAP_AREA_BEGIN, OS_MAP_AREA_END);
  fake_heap_start = (char*)__ctru_heap;
  fake_heap_end = fake_heap_start + __ctru_heap_size;
}

static int s_console = 0;
static volatile int s_console_dirty = 1; /* bottom screen console written since the last flush */
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
static int s_syscore = 0;   /* IOP / IO threads on core 1 (config.ini io_on_system_core) */
static int s_cpu_limit = 0; /* the app got a share of core 1 (APT_SetAppCpuTimeLimit) */
static int s_sound = 0;
static int s_sound_core = 1; /* 2 on New 3DS: see ctr_platform_init */
static u32* s_soc_buffer = NULL;

/* Boot progress on the SD card: on hardware a hang before the first frame shows nothing but the
 * launch screen; this file tells how far the boot got. sdmc:/3ds/jak1/boot.txt when started from
 * the Homebrew Launcher, boot_cia.txt otherwise (the installed title; also a .3dsx in the
 * emulator), so running one doesn't overwrite what the other got to. The first lines are written
 * by __appInit below, before main. */
static const char* boot_file(void) {
  return envIsHomebrew() ? "sdmc:/3ds/jak1/boot.txt" : "sdmc:/3ds/jak1/boot_cia.txt";
}

/* libctru's __appInit (services, before the constructors and main), except that the SD card is
 * mounted before APT so the boot file can show whether the installed title stops in aptInit,
 * which waits for the HOME Menu to wake the application up. */
void __appInit(void) {
  srvInit();
  fsInit();
  archiveMountSdmc();
  FILE* f = fopen(boot_file(), "w");
  if (f) {
    fprintf(f, "0 services, SD card (%s)\n", envIsHomebrew() ? "Homebrew Launcher" : "title");
    fclose(f);
  }
  aptInit();
  ctr_boot_mark("0 APT (HOME Menu handshake)");
  hidInit();
}

__attribute__((constructor(101))) static void ctr_boot_mark_first(void) {
  ctr_boot_mark("1 libctru started (heaps, services, SD card)");
}

void ctr_boot_mark(const char* step) {
  FILE* f = fopen(boot_file(), "a");
  if (f) {
    fprintf(f, "%s\n", step);
    fclose(f);
  }
}

static void config_trim(char* s) {
  char* a = s;
  while (*a == ' ' || *a == '\t') {
    a++;
  }
  memmove(s, a, strlen(a) + 1);
  size_t n = strlen(s);
  while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) {
    s[--n] = 0;
  }
}

int ctr_config_get(const char* key, char* out, int size) {
  {
    FILE* f = fopen("sdmc:/3ds/jak1/config.ini", "r");
    if (!f) {
      return 0;
    }
    char line[512];
    int found = 0;
    while (!found && fgets(line, sizeof(line), f)) {
      char* c = strpbrk(line, "#;");
      if (c) {
        *c = 0;
      }
      char* eq = strchr(line, '=');
      if (!eq) {
        continue;
      }
      *eq = 0;
      config_trim(line);
      if (strcmp(line, key) == 0) {
        char* v = eq + 1;
        config_trim(v);
        snprintf(out, (size_t)size, "%s", v);
        found = 1;
      }
    }
    fclose(f);
    return found;
  }
  return 0;
}

int ctr_config_bool(const char* key, int def) {
  char v[32];
  if (!ctr_config_get(key, v, sizeof(v))) {
    return def;
  }
  return strcmp(v, "1") == 0 || strcmp(v, "on") == 0 || strcmp(v, "true") == 0 ||
         strcmp(v, "yes") == 0;
}

int ctr_platform_init(int enable_console) {
  ctr_linear_lock(); /* creates the lock while there is one thread */
  ctr_linear_unlock();
  /* paths without a device name ("/3ds/jak1/...") then refer to the SD card, and
   * std::filesystem treats them as absolute ("sdmc:/..." would be a relative path to it) */
  ctr_boot_mark("2 main");
  chdir("sdmc:/");
  osSetSpeedupEnable(true); /* 804 MHz + L2 cache on New 3DS, no-op otherwise */
  ctr_boot_mark("3 speedup");
  gfxInitDefault();
  ctr_boot_mark("4 screens");
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
   * config.ini io_on_system_core = on. */
  /* Audio output (config.ini: sound = on, sound_core = N). Default core: 0 on New 3DS (core 2
   * runs the render thread), else core 1.
   * Core 1 is the system core: the system's services (GPU, SD card, DSP, input) run there, so a
   * busy mixer on it slows every service call of the game on real hardware, and the app only gets
   * a share of it (APT_SetAppCpuTimeLimit; the kernel allows one app thread there). */
  /* (AI-assisted) config.ini: sound = on, sound_core = N */
  {
    char v[16];
    if (ctr_config_bool("sound", 0)) {
      s_sound = 1;
      s_sound_core = ctr_is_new3ds() ? 0 : 1;
      if (ctr_config_get("sound_core", v, sizeof(v)) && v[0] >= '0' && v[0] <= '3') {
        s_sound_core = v[0] - '0';
      }
    }
  }
  int want_syscore = ctr_config_bool("io_on_system_core", 0);
  if (want_syscore || (s_sound && s_sound_core == 1)) {
    ctr_core1_enable();
  }
  s_syscore = (want_syscore && s_cpu_limit) ? 1 : 0;
  /* C-stick / ZL / ZR on New 3DS (and the Circle Pad Pro) */
  s_irrst = R_SUCCEEDED(irrstInit()) ? 1 : 0;
  ctr_boot_mark("5 console and input");
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
      /* citro3d swaps the top screen; the console screen is single buffered: flush it only, and
       * only after something was printed (a flush is a GSP IPC plus a cache clean, and this runs
       * every 16 ms on the game's core) */
      if (s_console_dirty) {
        s_console_dirty = 0;
        u16 w = 0, h = 0;
        u8* fb = gfxGetFramebuffer(GFX_BOTTOM, GFX_LEFT, &w, &h);
        GSPGPU_FlushDataCache(fb, (u32)w * h * gspGetBytesPerPixel(gfxGetScreenFormat(GFX_BOTTOM)));
      }
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

static RecursiveLock s_linear_lock;
static volatile int s_linear_lock_ready = 0;

void ctr_linear_lock(void) {
  if (!s_linear_lock_ready) { /* first use is at startup, before other threads exist */
    RecursiveLock_Init(&s_linear_lock);
    s_linear_lock_ready = 1;
  }
  RecursiveLock_Lock(&s_linear_lock);
}

void ctr_linear_unlock(void) {
  RecursiveLock_Unlock(&s_linear_lock);
}

unsigned int ctr_thread_current_id(void) {
  u32 id = 0;
  svcGetThreadId(&id, CUR_THREAD_HANDLE);
  return id;
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
  /* at most twice a second: each flush is a write to the SD card, slow on hardware (crashes flush
   * what is left, see ctr_crash) */
  static u64 s_last_flush;
  if (memchr(ptr, '\n', len)) {
    const u64 now = svcGetSystemTick();
    if (now - s_last_flush > SYSCLOCK_ARM11 / 2) {
      fflush(s_tee_file);
      s_last_flush = now;
    }
  }
}

static ssize_t tee_write_out(struct _reent* r, void* fd, const char* ptr, size_t len) {
  ssize_t rv = (ssize_t)len;
  tee_lock();
  tee_to_file(ptr, len);
  if (s_orig_out && s_orig_out->write_r) {
    rv = s_orig_out->write_r(r, fd, ptr, len);
    s_console_dirty = 1;
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
    s_console_dirty = 1;
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

/* ---------------- hardware probe ---------------- */

/* A chain of dependent adds: one cycle each on the ARM11, whatever the memory does. */
static __attribute__((noinline)) u32 probe_add_chain(u32 iterations) {
  u32 a = 0;
  for (u32 i = 0; i < iterations; i++) {
    __asm__ volatile(".rept 64\n\tadd %0, %0, #1\n\t.endr" : "+r"(a));
  }
  return a;
}

/* ns per load for a pointer chase through `bytes` of memory (a random cycle of 64-byte steps) */
static double probe_chase_ns(u32* buf, u32 bytes, u32 loads) {
  const u32 n = bytes / 64;
  u32* order = (u32*)malloc(n * sizeof(u32));
  if (!order || n < 2) {
    free(order);
    return 0.0;
  }
  for (u32 i = 0; i < n; i++) {
    order[i] = i;
  }
  u32 seed = 12345;
  for (u32 i = n - 1; i > 0; i--) {
    seed = seed * 1664525u + 1013904223u;
    u32 j = seed % (i + 1);
    u32 t = order[i];
    order[i] = order[j];
    order[j] = t;
  }
  for (u32 i = 0; i < n; i++) {
    buf[order[i] * 16] = order[(i + 1) % n] * 16;
  }
  free(order);
  volatile u32 sink;
  u32 p = 0;
  for (u32 i = 0; i < 1000; i++) {
    p = buf[p];
  }
  const u64 t0 = svcGetSystemTick();
  for (u32 i = 0; i < loads; i++) {
    p = buf[p];
  }
  const u64 t1 = svcGetSystemTick();
  sink = p;
  (void)sink;
  return (double)(t1 - t0) * 1e9 / SYSCLOCK_ARM11 / loads;
}

void ctr_hw_probe(char* out, int size) {
  const u32 iters = 100000; /* 6.4 M dependent adds + the loop, ~9 ms at 804 MHz */
  const u64 t0 = svcGetSystemTick();
  volatile u32 sink = probe_add_chain(iters);
  const u64 t1 = svcGetSystemTick();
  (void)sink;
  /* the loop adds ~3 cycles per 64 adds (increment, compare, predicted branch) */
  const double mhz = (double)iters * 67.0 * SYSCLOCK_ARM11 / (double)(t1 - t0) / 1e6;
  u32* buf = (u32*)malloc(16u << 20);
  double l1 = 0, l2 = 0, ram = 0;
  if (buf) {
    l1 = probe_chase_ns(buf, 8u << 10, 200000);
    l2 = probe_chase_ns(buf, 512u << 10, 100000);
    ram = probe_chase_ns(buf, 16u << 20, 50000);
    free(buf);
  }
  snprintf(out, size,
           "cpu ~%.0f MHz (804 = New 3DS speedup on); load latency 8 KB %.0f ns, 512 KB %.0f ns "
           "(L2 on if well below 16 MB), 16 MB %.0f ns",
           mhz, l1, l2, ram);
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
  s_console_dirty = 1;
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

int ctr_core1_share(void) {
  return s_cpu_limit;
}

int ctr_core1_enable(void) {
  /* 30% if 80% is refused; without any share, ctr_thread_create_pinned refuses core 1 */
  if (!s_cpu_limit) {
    s_cpu_limit = R_SUCCEEDED(APT_SetAppCpuTimeLimit(80))   ? 80
                  : R_SUCCEEDED(APT_SetAppCpuTimeLimit(30)) ? 30
                                                             : 0;
  }
  return s_cpu_limit;
}

static int thread_create(void* (*fn)(void*), void* arg, unsigned int stack_size, int prio, int core,
                         int pinned, void** handle) {
  if (core == CTR_CORE_SYS && !s_cpu_limit) {
    if (pinned) {
      return -2;
    }
    core = CTR_CORE_APP;
  }
  ThreadStart* start = (ThreadStart*)malloc(sizeof(ThreadStart));
  if (!start) {
    return -1;
  }
  start->fn = fn;
  start->arg = arg;
  Thread t = threadCreate(thread_trampoline, start, stack_size, prio, core, false);
  if (!t && core != CTR_CORE_APP && !pinned) {
    t = threadCreate(thread_trampoline, start, stack_size, prio, CTR_CORE_APP, false);
  }
  if (!t) {
    free(start);
    return -1;
  }
  *handle = t;
  return 0;
}

int ctr_thread_create(void* (*fn)(void*), void* arg, unsigned int stack_size, int prio, int core,
                      void** handle) {
  return thread_create(fn, arg, stack_size, prio, core, 0, handle);
}

int ctr_thread_create_pinned(void* (*fn)(void*), void* arg, unsigned int stack_size, int prio,
                             int core, void** handle) {
  return thread_create(fn, arg, stack_size, prio, core, 1, handle);
}

void ctr_thread_join(void* handle) {
  Thread t = (Thread)handle;
  threadJoin(t, U64_MAX);
  threadFree(t);
}

/* ---------------- audio output (ndsp) ---------------- */

int ctr_sound_config(int* core) {
  if (core) {
    *core = s_sound_core;
  }
  return s_sound;
}

#define AUDIO_MAX_BUFS 4
static int s_audio_on = 0;
static short* s_audio_mem = NULL; /* linear memory: nbufs * frames * 2 samples */
static ndspWaveBuf s_audio_wbuf[AUDIO_MAX_BUFS];
static unsigned int s_audio_frames = 0;
static unsigned int s_audio_nbufs = 0;
static unsigned int s_audio_next = 0; /* the next buffer to hand out (round robin) */
static LightEvent s_audio_event;

static void audio_frame_callback(void* data) {
  (void)data;
  LightEvent_Signal(&s_audio_event);
}

int ctr_audio_init(unsigned int rate, unsigned int frames, unsigned int nbufs) {
  if (s_audio_on) {
    return 0;
  }
  if (nbufs < 2) {
    nbufs = 2;
  }
  if (nbufs > AUDIO_MAX_BUFS) {
    nbufs = AUDIO_MAX_BUFS;
  }
  /* ndspInit allocates linear memory too (the IOP thread runs this while the renderer loads) */
  ctr_linear_lock();
  Result rc = ndspInit();
  if (R_FAILED(rc)) {
    ctr_linear_unlock();
    printf("[ctr] ndspInit failed: 0x%08lx (DSP firmware sdmc:/3ds/dspfirm.cdc missing?)\n",
           (unsigned long)rc);
    return -1;
  }
  const size_t bytes = (size_t)nbufs * frames * 2 * sizeof(short);
  s_audio_mem = (short*)linearAlloc(bytes);
  if (!s_audio_mem) {
    ndspExit();
    ctr_linear_unlock();
    printf("[ctr] audio: linearAlloc(%u) failed\n", (unsigned int)bytes);
    return -2;
  }
  ctr_linear_unlock();
  memset(s_audio_mem, 0, bytes);
  DSP_FlushDataCache(s_audio_mem, bytes);

  ndspSetOutputMode(NDSP_OUTPUT_STEREO);
  ndspChnReset(0);
  ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
  ndspChnSetRate(0, (float)rate);
  ndspChnSetFormat(0, NDSP_FORMAT_STEREO_PCM16);
  float mix[12];
  memset(mix, 0, sizeof(mix));
  mix[0] = 1.0f; /* front left */
  mix[1] = 1.0f; /* front right */
  ndspChnSetMix(0, mix);

  memset(s_audio_wbuf, 0, sizeof(s_audio_wbuf));
  for (unsigned int i = 0; i < nbufs; i++) {
    s_audio_wbuf[i].data_vaddr = s_audio_mem + (size_t)i * frames * 2;
    s_audio_wbuf[i].nsamples = frames;
    s_audio_wbuf[i].status = NDSP_WBUF_FREE;
  }
  s_audio_frames = frames;
  s_audio_nbufs = nbufs;
  s_audio_next = 0;
  LightEvent_Init(&s_audio_event, RESET_ONESHOT);
  ndspSetCallback(audio_frame_callback, NULL);
  s_audio_on = 1;
  printf("[ctr] audio: ndsp on, %u Hz, %u buffers of %u frames (%.1f ms each)\n", rate, nbufs,
         frames, 1000.0 * frames / rate);
  return 0;
}

void ctr_audio_exit(void) {
  if (!s_audio_on) {
    return;
  }
  ndspSetCallback(NULL, NULL);
  ndspChnWaveBufClear(0);
  ctr_linear_lock();
  ndspExit();
  linearFree(s_audio_mem);
  ctr_linear_unlock();
  s_audio_mem = NULL;
  s_audio_on = 0;
}

short* ctr_audio_get_buffer(void) {
  if (!s_audio_on) {
    return NULL;
  }
  ndspWaveBuf* wb = &s_audio_wbuf[s_audio_next];
  if (wb->status == NDSP_WBUF_FREE || wb->status == NDSP_WBUF_DONE) {
    return (short*)wb->data_vaddr;
  }
  return NULL;
}

void ctr_audio_submit(short* buffer) {
  if (!s_audio_on) {
    return;
  }
  ndspWaveBuf* wb = &s_audio_wbuf[s_audio_next];
  if ((short*)wb->data_vaddr != buffer) {
    printf("[ctr] audio: submit out of order\n");
    return;
  }
  DSP_FlushDataCache(buffer, (size_t)s_audio_frames * 2 * sizeof(short));
  ndspChnWaveBufAdd(0, wb);
  s_audio_next = (s_audio_next + 1) % s_audio_nbufs;
}

void ctr_audio_wait(unsigned int us) {
  if (!s_audio_on) {
    svcSleepThread((s64)us * 1000);
    return;
  }
  LightEvent_WaitTimeout(&s_audio_event, (s64)us * 1000);
}

unsigned int ctr_audio_dropped_frames(void) {
  return s_audio_on ? ndspGetDroppedFrames() : 0;
}
