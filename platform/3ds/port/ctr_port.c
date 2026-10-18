/*
 * (AI-assisted)
 * libctru side of the OpenGOAL 3DS platform layer. See ctr_port.h.
 */

#include "ctr_port.h"
#include "ctr_logring.h"

#include "game/graphics/ctr/ctr_config_presets.h"

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
#if defined(OG3DS_GAME_JAK2) && OG3DS_GAME_JAK2
/* (AI-assisted, j2-render-bg) Jak II with the 3DS renderer: 32 MB. Jak II's .text is 8 MB bigger
 * than Jak 1's, so heap + linear is ~102 MB (Jak 1: ~121): 32 MB leaves the heap 70 MB, just over
 * gk's 68 MB check (48 MB of EE memory + 20); the null renderer used 55.6 MB of it in Haven City.
 * (36 MB left 66 MB: refused at boot.) Linear memory holds the level backgrounds (.c3l: the slums
 * ~12.8 MB, a neighbouring district 3-7 MB, ctywide 3.2, lwidea ~2, GAME ~0.8), the near tie
 * stream arena, the vertex and command buffers and the audio buffers; when it runs out, the
 * loader's degrade paths take over (incomplete loads repaired later, far levels' detail freed). */
/* (AI-assisted, j2-mem2) 34 MB: the heap gave up ~6 MB (sparse VRAM copy, no RAMDISK, impostors
 * off, smaller IOP / thread stacks, no Jak 1 code), so 2 MB of it moved here (the market with
 * ctyindb in view had 128 KB of linear memory left); the heap stays at ~69.5 MB, over the 68 MB
 * check. */
u32 __ctru_linear_heap_size = 34 << 20;
#else
u32 __ctru_linear_heap_size = 40 << 20;
#endif

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
static int s_cpu_limit = 0; /* the app got a share of core 1 (APT_SetAppCpuTimeLimit) */
static int s_sound = 0;
static int s_sound_core = 1; /* 2 on New 3DS: see ctr_platform_init */
static u32* s_soc_buffer = NULL;
/* (AI-assisted) the log writer (below) */
static void log_stop(void);
static void log_watchdog(void);

/* Boot progress on the SD card: on hardware a hang before the first frame shows nothing but the
 * launch screen; this file tells how far the boot got. sdmc:/3ds/jak1/boot.txt when started from
 * the Homebrew Launcher, boot_cia.txt otherwise (the installed title; also a .3dsx in the
 * emulator), so running one doesn't overwrite what the other got to. The first lines are written
 * by __appInit below, before main. */
static const char* boot_file(void) {
  return envIsHomebrew() ? CTR_SD_DIR "/boot.txt" : CTR_SD_DIR "/boot_cia.txt";
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

/* (AI-assisted) config.ini, read once (the first call: ctr_platform_init, before any other thread):
 * its key = value lines, in file order */
#define CFG_TEXT_BYTES 16384
#define CFG_MAX_LINES 256
static char s_cfg_text[CFG_TEXT_BYTES];
static const char* s_cfg_key[CFG_MAX_LINES];
static const char* s_cfg_value[CFG_MAX_LINES];
static int s_cfg_lines = -1; /* -1: not read yet */

static void config_load(void) {
  if (s_cfg_lines >= 0) {
    return;
  }
  s_cfg_lines = 0;
  FILE* f = fopen(CTR_SD_DIR "/config.ini", "r");
  if (!f) {
    return;
  }
  char line[512];
  size_t used = 0;
  while (fgets(line, sizeof(line), f)) {
    char* c = strpbrk(line, "#;");
    if (c) {
      *c = 0;
    }
    char* eq = strchr(line, '=');
    if (!eq) {
      continue;
    }
    *eq = 0;
    char* v = eq + 1;
    config_trim(line);
    config_trim(v);
    const size_t kn = strlen(line) + 1, vn = strlen(v) + 1;
    if (s_cfg_lines >= CFG_MAX_LINES || used + kn + vn > sizeof(s_cfg_text)) {
      break;
    }
    memcpy(s_cfg_text + used, line, kn);
    s_cfg_key[s_cfg_lines] = s_cfg_text + used;
    used += kn;
    memcpy(s_cfg_text + used, v, vn);
    s_cfg_value[s_cfg_lines] = s_cfg_text + used;
    used += vn;
    s_cfg_lines++;
  }
  fclose(f);
}

/* (AI-assisted) a key's last line in config.ini (ctr_cfg_get_fn) */
static const char* config_file_get(void* ctx, const char* key) {
  (void)ctx;
  const char* v = NULL;
  for (int i = 0; i < s_cfg_lines; i++) {
    if (strcmp(s_cfg_key[i], key) == 0) {
      v = s_cfg_value[i];
    }
  }
  return v;
}

/* (AI-assisted) the key's line, else a user key's (particles, text_size), else the preset's, else
 * the default (ctr_config_presets.h): the same values CtrSettings.cpp uses */
int ctr_config_get(const char* key, char* out, int size) {
  config_load();
  const char* v = ctr_cfg_resolve(key, config_file_get, NULL, 1);
  if (!v) {
    return 0;
  }
  snprintf(out, (size_t)size, "%s", v);
  return 1;
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
  config_load(); /* (AI-assisted) config.ini, while there is one thread */
  /* (AI-assisted) hardware hang bisect (config.ini, default on): ctr_mem.S's memcpy / memmove /
   * memset (off: ctr_mem_c.c's loops, also used until here) and ctr_clock.cpp's steady_clock */
  {
    extern int ctr_mem_asm, ctr_fast_clock;
    ctr_mem_asm = ctr_config_bool("asm_mem", 1);
    ctr_fast_clock = ctr_config_bool("fast_clock", 1);
  }
  chdir("sdmc:/");
  osSetSpeedupEnable(true); /* 804 MHz + L2 cache on New 3DS, no-op otherwise */
  ctr_boot_mark("3 speedup");
  gfxInitDefault();
  ctr_boot_mark("4 screens");
  if (enable_console) {
    consoleInit(GFX_BOTTOM, &s_stat_con);
    consoleInit(GFX_BOTTOM, &s_log_con);
    /* (AI-assisted) window coordinates start at 1 (0 was taken as 1: the log's first row was the
     * status area's last one) */
    consoleSetWindow(&s_stat_con, 1, 1, 40, STATUS_LINES);
    consoleSetWindow(&s_log_con, 1, STATUS_LINES + 1, 40, 30 - STATUS_LINES);
    consoleSelect(&s_log_con);
    s_console = 1;
  }
  ctr_thread_install_crash_handler();
  /* (AI-assisted) The IO / IOP threads stay on core 0: config.ini io_on_system_core (core 1 for
   * them) was removed on Oct 16 (boot hung at the first IOP file load in Azahar). */
  /* Audio output (config.ini: sound = on, sound_core = N). Default core: 2 on New 3DS, else 1.
   * (AI-assisted) 2026-10-02: core 2 again. It went to core 0 when the render thread on core 2 was
   * the limit (~11 fps); now the game thread on core 0 is (logic 10-30 ms of 33) and the render
   * thread has room (9-16 ms), and the mixer cost the game's core 2-7 ms per frame. Its priority
   * (CTR_PRIO_SOUND) is above the render thread's, so audio isn't starved there.
   * Core 1 is the system core: the system's services (GPU, SD card, DSP, input) run there, so a
   * busy mixer on it slows every service call of the game on real hardware, and the app only gets
   * a share of it (APT_SetAppCpuTimeLimit; the kernel allows one app thread there). */
  /* (AI-assisted) config.ini: sound = on, sound_core = N */
  {
    char v[16];
    if (ctr_config_bool("sound", 1)) { /* (AI-assisted) on by default since Oct 16 */
      s_sound = 1;
#ifdef OG3DS_GAME_JAK2
      /* (AI-assisted, j2-sound) Jak II: core 1 with a share of it (ctr_core1_enable). On core 2
       * the mixer (4.5-7.2 ms per frame in the city, Azahar) pushed Jak II's render thread, which
       * is at its limit there, over 33 ms (slums 29.8 -> 25-29 fps); core 0 has the game logic
       * (~27 ms) and the IOP. */
      s_sound_core = 1;
#else
      s_sound_core = ctr_is_new3ds() ? 2 : 1;
#endif
      if (ctr_config_get("sound_core", v, sizeof(v)) && v[0] >= '0' && v[0] <= '3') {
        s_sound_core = v[0] - '0';
      }
    }
  }
  if (s_sound && s_sound_core == 1) {
    ctr_core1_enable();
  }
  /* C-stick / ZL / ZR on New 3DS (and the Circle Pad Pro) */
  s_irrst = R_SUCCEEDED(irrstInit()) ? 1 : 0;
  ctr_boot_mark("5 console and input");
  return 0;
}

void ctr_platform_exit(void) {
  log_stop();
  ctr_net_exit();
  if (s_irrst) {
    irrstExit();
    s_irrst = 0;
  }
  gfxExit();
}

int ctr_main_loop(void) {
  int running = aptMainLoop() ? 1 : 0;
  log_watchdog();
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
/* the console (bottom screen); without the log writer also stdout.log and the crash tail */
static LightLock s_tee_lock;
static int s_tee_lock_init = 0;
static volatile int s_crashing = 0;
static void console_write_raw(const char* text);

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

/* ---------------- log writer ---------------- */
/* (AI-assisted) config.ini log_async = on (default). Everything logged (lg lines, GOAL output,
 * printf) is copied into a ring buffer (ctr_logring.h); the writer thread writes it to gk.log and
 * stdout.log about once a second (sooner at 16 KB, after an error line, for a background job) and
 * draws the bottom screen's log. Before, every line was written to the SD card (and drawn) by the
 * thread that logged it, under one lock: while the IOP streams from the card a write blocks for
 * 100-400 ms on hardware, which caused most of the long frames in the hardware logs.
 * Core 2 at the lowest priority (0x3F): the 3DS scheduler never runs it while the render thread
 * (0x2F) or the mixer (0x2E) can run, so it only gets core 2's idle time (the render thread waits
 * for the game and the GPU for much of every frame), and it spends most of its time blocked in FS
 * calls anyway. On core 0 the game thread, which spins while it waits for the IOP (RpcSync), would
 * starve it for seconds at boot and in level loads; core 1 needs APT's CPU time share. The ring's
 * lock is held for index updates and one record's memcpy only (the writer copies out without it),
 * so a preempted writer can't hold up a thread that logs. If the writer hasn't had a round for 3 s
 * all the same (core 2 kept busy), the main thread writes the logs (log_watchdog). Crashes and
 * exits write what is left from their own thread (ctr_crash, ctr_log_sync). */
#define LOG_RING_BYTES (256u << 10)
#define LOG_OUT_BYTES (16u << 10) /* per file and batch: >= LOGRING_MAX_RECORD + a dropped line */
#define LOG_WAKE_BYTES (16u << 10)
#define LOG_INTERVAL_MS 1000u
#define LOG_STARVED_MS 3000u

static int s_log_on = 0;           /* the writer thread runs */
static logring s_log_ring;
static LightLock s_log_lock;       /* s_log_ring, the jobs, the status text */
static RecursiveLock s_drain_lock; /* writing the files, the console text, the crash tail */
static LightEvent s_log_event;
static void* s_log_thread = NULL;
static int s_log_core = 2;
static unsigned int s_log_thread_id = 0;
static volatile int s_log_quit = 0;
static volatile int s_log_urgent = 0;
static volatile u32 s_log_alive_ms = 0; /* the writer's last round */
static u32 s_log_written_ms = 0;        /* the ring was last written out */
static FILE* s_gk_file = NULL;          /* the lg log (gk.log), see lg::set_file */
static char s_out_gk[LOG_OUT_BYTES], s_out_so[LOG_OUT_BYTES];

typedef struct {
  void (*fn)(void*);
  void* arg;
} BgJob;
#define BG_JOBS 16
static BgJob s_jobs[BG_JOBS];
static unsigned s_jobs_first = 0, s_jobs_count = 0;

/* The bottom screen's log window as text: the writer draws it row by row, padded to the width, so
 * the libctru console never scrolls (a scroll moves ~130 KB of framebuffer) and never sees a
 * newline (it flushes the framebuffers through GSP at every one). Guarded by s_drain_lock. */
#define CON_COLS 40
#define CON_ROWS (30 - STATUS_LINES)
static char s_con_text[CON_ROWS][CON_COLS];
static unsigned char s_con_len[CON_ROWS];
static int s_con_first = 0, s_con_count = 1; /* rows in use, oldest first; text goes to the last */
static int s_con_esc = 0;                    /* in an escape sequence (colors are left out) */
static volatile int s_con_changed = 0;
static volatile int s_con_paused = 0; /* ctr_message_wait's screen is up */
static char s_status_text[160];       /* ctr_console_status, drawn by the writer */
static volatile int s_status_dirty = 0;

static u32 log_now_ms(void) {
  return (u32)(svcGetSystemTick() / (SYSCLOCK_ARM11 / 1000));
}

static int drain_lock_timed(unsigned ms) {
  for (unsigned i = 0;; i++) {
    if (RecursiveLock_TryLock(&s_drain_lock) == 0) {
      return 1;
    }
    if (i >= ms) {
      return 0;
    }
    svcSleepThread(1000 * 1000LL);
  }
}

/* the ring's lock; when crashing, not forever (the thread that crashed may hold it) */
static int log_ring_lock(int crash) {
  if (!crash) {
    LightLock_Lock(&s_log_lock);
    return 1;
  }
  for (int i = 0; i < 200; i++) {
    if (LightLock_TryLock(&s_log_lock) == 0) {
      return 1;
    }
    svcSleepThread(1000 * 1000LL);
  }
  return 0;
}

static void con_newline(void) {
  if (s_con_count < CON_ROWS) {
    s_con_count++;
  } else {
    s_con_first = (s_con_first + 1) % CON_ROWS;
  }
  s_con_len[(s_con_first + s_con_count - 1) % CON_ROWS] = 0;
}

static void con_putc(char c) {
  int row = (s_con_first + s_con_count - 1) % CON_ROWS;
  if (s_con_len[row] >= CON_COLS) {
    con_newline();
    row = (s_con_first + s_con_count - 1) % CON_ROWS;
  }
  s_con_text[row][s_con_len[row]++] = c;
}

/* stdout text -> the rows, wrapped like the libctru console does it (tabs of 3) */
static void con_feed(const char* p, unsigned n) {
  for (unsigned i = 0; i < n; i++) {
    const unsigned char c = (unsigned char)p[i];
    if (s_con_esc == 1) {
      s_con_esc = c == '[' ? 2 : 0;
    } else if (s_con_esc == 2) {
      s_con_esc = c >= 0x40 && c <= 0x7e ? 0 : 2;
    } else if (c == 0x1b) {
      s_con_esc = 1;
    } else if (c == '\n') {
      con_newline();
    } else if (c == '\t') {
      do {
        con_putc(' ');
      } while (s_con_len[(s_con_first + s_con_count - 1) % CON_ROWS] % 3);
    } else if (c >= 32 && c < 127) {
      con_putc((char)c);
    }
  }
  s_con_changed = 1;
}

/* one row of a console window (y from 1), padded with spaces: no newline, so no scrolling */
static void con_draw_row(PrintConsole* con, int y, const char* text, int len) {
  char line[CON_COLS];
  if (len > CON_COLS) {
    len = CON_COLS;
  }
  memcpy(line, text, (size_t)len);
  memset(line + len, ' ', (size_t)(CON_COLS - len));
  con->cursorX = 1;
  con->cursorY = y;
  s_orig_out->write_r(_REENT, NULL, line, CON_COLS);
}

/* the status lines (at most STATUS_LINES, separated by '\n'), cyan; console lock held */
static void status_render(const char* text) {
  PrintConsole* prev = consoleSelect(&s_stat_con);
  console_write_raw("\x1b[36m");
  const char* p = text;
  for (int y = 1; y <= STATUS_LINES; y++) {
    const char* e = p ? strchr(p, '\n') : NULL;
    const int len = p ? (e ? (int)(e - p) : (int)strlen(p)) : 0;
    con_draw_row(&s_stat_con, y, p ? p : "", len);
    p = e ? e + 1 : NULL;
  }
  console_write_raw("\x1b[0m");
  consoleSelect(prev);
}

/* draw what changed (s_drain_lock held) */
static void console_update(void) {
  if (!s_console || !s_orig_out || !s_orig_out->write_r || s_crashing || s_con_paused) {
    return;
  }
  char status[sizeof(s_status_text)];
  int have_status = 0;
  LightLock_Lock(&s_log_lock);
  if (s_status_dirty) {
    memcpy(status, s_status_text, sizeof(status));
    s_status_dirty = 0;
    have_status = 1;
  }
  LightLock_Unlock(&s_log_lock);
  if (!have_status && !s_con_changed) {
    return;
  }
  tee_lock();
  if (!s_crashing && !s_con_paused) {
    if (s_con_changed) {
      s_con_changed = 0;
      PrintConsole* prev = consoleSelect(&s_log_con);
      for (int r = 0; r < CON_ROWS; r++) {
        const int row = (s_con_first + r) % CON_ROWS;
        con_draw_row(&s_log_con, r + 1, s_con_text[row], r < s_con_count ? s_con_len[row] : 0);
      }
      consoleSelect(prev);
    }
    if (have_status) {
      status_render(status);
    }
    s_console_dirty = 1; /* ctr_main_loop flushes the framebuffer */
  }
  tee_unlock();
}

/* Write what the ring holds to the files (s_drain_lock held). crash: nothing for the console, and
 * returns 0 if the ring's lock stays taken. */
static int log_drain(int crash) {
  for (;;) {
    if (!log_ring_lock(crash)) {
      return 0;
    }
    unsigned dropped = 0;
    const unsigned avail = logring_read_begin(&s_log_ring, &dropped);
    LightLock_Unlock(&s_log_lock);
    logring_out outs[2] = {{s_out_gk, LOG_OUT_BYTES, 0}, {s_out_so, LOG_OUT_BYTES, 0}};
    if (dropped) {
      outs[0].len = logring_dropped_line(s_out_gk, LOG_OUT_BYTES, dropped);
      memcpy(s_out_so, s_out_gk, outs[0].len);
      outs[1].len = outs[0].len;
    }
    const unsigned consumed = logring_take(&s_log_ring, avail, outs, 2);
    if (!log_ring_lock(crash)) {
      return 0;
    }
    logring_read_end(&s_log_ring, consumed);
    LightLock_Unlock(&s_log_lock);
    if (outs[0].len && s_gk_file) {
      fwrite(s_out_gk, 1, outs[0].len, s_gk_file);
    }
    if (outs[1].len) {
      if (s_tee_file) {
        fwrite(s_out_so, 1, outs[1].len, s_tee_file);
      }
      tail_append(s_out_so, outs[1].len);
      if (!crash) {
        con_feed(s_out_so, outs[1].len);
      }
    }
    if (consumed >= avail) {
      return 1;
    }
  }
}

/* the queued background jobs, oldest first (s_drain_lock held) */
static void run_jobs(void) {
  for (;;) {
    LightLock_Lock(&s_log_lock);
    if (!s_jobs_count) {
      LightLock_Unlock(&s_log_lock);
      return;
    }
    const BgJob job = s_jobs[s_jobs_first];
    s_jobs_first = (s_jobs_first + 1) % BG_JOBS;
    s_jobs_count--;
    LightLock_Unlock(&s_log_lock);
    job.fn(job.arg);
  }
}

static void* log_writer_main(void* arg) {
  (void)arg;
  ctr_thread_install_crash_handler();
  s_log_thread_id = ctr_thread_current_id();
  for (;;) {
    LightEvent_WaitTimeout(&s_log_event, (s64)LOG_INTERVAL_MS * 1000000LL);
    const int quit = s_log_quit;
    RecursiveLock_Lock(&s_drain_lock);
    const u32 now = log_now_ms();
    /* woken early by the status line or a job: the files only once a second */
    if (quit || s_log_urgent || now - s_log_written_ms >= LOG_INTERVAL_MS - 100 ||
        s_log_ring.used >= LOG_WAKE_BYTES) {
      s_log_urgent = 0;
      log_drain(0);
      s_log_written_ms = now;
    }
    run_jobs();
    console_update();
    s_log_alive_ms = now;
    RecursiveLock_Unlock(&s_drain_lock);
    if (quit) {
      return NULL;
    }
  }
}

static int log_start(void) {
  void* mem = malloc(LOG_RING_BYTES);
  if (!mem) {
    return 0;
  }
  logring_init(&s_log_ring, mem, LOG_RING_BYTES);
  LightLock_Init(&s_log_lock);
  RecursiveLock_Init(&s_drain_lock);
  LightEvent_Init(&s_log_event, RESET_ONESHOT);
  s_log_alive_ms = s_log_written_ms = log_now_ms();
  s_log_on = 1;
  /* core 2 (New 3DS), else core 0 */
  s_log_core = 2;
  if (ctr_thread_create_pinned(log_writer_main, NULL, 64 * 1024, CTR_PRIO_LOG, 2,
                               &s_log_thread) != 0) {
    s_log_core = CTR_CORE_APP;
    if (ctr_thread_create(log_writer_main, NULL, 64 * 1024, CTR_PRIO_LOG, CTR_CORE_APP,
                          &s_log_thread) != 0) {
      s_log_on = 0;
      free(mem);
      return 0;
    }
  }
  return 1;
}

/* ctr_platform_exit: the writer's last round, then what came after it */
static void log_stop(void) {
  if (!s_log_on) {
    return;
  }
  s_log_quit = 1;
  LightEvent_Signal(&s_log_event);
  ctr_thread_join(s_log_thread);
  RecursiveLock_Lock(&s_drain_lock);
  log_drain(0);
  run_jobs();
  s_log_on = 0;
  RecursiveLock_Unlock(&s_drain_lock);
}

/* main thread (ctr_main_loop, every 16 ms): the writer had no round for LOG_STARVED_MS (its core
 * busy all the time, or the writer stuck in a write): write the logs from here instead */
static void log_watchdog(void) {
  if (!s_log_on || s_crashing || log_now_ms() - s_log_alive_ms < LOG_STARVED_MS) {
    return;
  }
  if (RecursiveLock_TryLock(&s_drain_lock) != 0) {
    return; /* the writer is in a write */
  }
  log_drain(0);
  run_jobs();
  console_update();
  s_log_alive_ms = s_log_written_ms = log_now_ms();
  RecursiveLock_Unlock(&s_drain_lock);
}

int ctr_log_async(void) {
  return s_log_on;
}

void ctr_log_write(int outputs, const char* text, unsigned int len, int urgent) {
  outputs &= CTR_LOG_FILE | CTR_LOG_STDOUT;
  if (!len || !outputs || !s_log_on) {
    return;
  }
  if (outputs & CTR_LOG_STDOUT) {
    /* also to the debugger / emulator log (Azahar: Debug.Emulated); without one it returns */
    svcOutputDebugString(text, (s32)len);
  }
  LightLock_Lock(&s_log_lock);
  const unsigned before = s_log_ring.used;
  logring_put(&s_log_ring, (unsigned)outputs, text, len);
  const unsigned after = s_log_ring.used;
  LightLock_Unlock(&s_log_lock);
  if (urgent) {
    s_log_urgent = 1;
    LightEvent_Signal(&s_log_event);
  } else if (before < LOG_WAKE_BYTES && after >= LOG_WAKE_BYTES) {
    LightEvent_Signal(&s_log_event);
  }
}

void ctr_log_set_file(void* file) {
  if (!s_log_on) {
    return; /* lg writes the file itself */
  }
  RecursiveLock_Lock(&s_drain_lock);
  if (file) {
    setvbuf((FILE*)file, NULL, _IONBF, 0); /* whole batches: one SD write each */
  }
  s_gk_file = (FILE*)file;
  RecursiveLock_Unlock(&s_drain_lock);
}

void ctr_log_sync(void) {
  if (!s_log_on) {
    if (s_tee_file) {
      fflush(s_tee_file);
    }
    return;
  }
  /* the writer may be in the middle of a write (0.4 s at worst on hardware) */
  if (!drain_lock_timed(5000)) {
    return;
  }
  log_drain(0);
  run_jobs();
  s_log_written_ms = log_now_ms();
  RecursiveLock_Unlock(&s_drain_lock);
}

int ctr_bg_run(void (*fn)(void*), void* arg) {
  if (s_log_on && !s_log_quit) {
    LightLock_Lock(&s_log_lock);
    if (s_jobs_count < BG_JOBS) {
      s_jobs[(s_jobs_first + s_jobs_count) % BG_JOBS] = (BgJob){fn, arg};
      s_jobs_count++;
      LightLock_Unlock(&s_log_lock);
      LightEvent_Signal(&s_log_event);
      return 1;
    }
    LightLock_Unlock(&s_log_lock);
  }
  fn(arg);
  return 0;
}

static ssize_t tee_write_out(struct _reent* r, void* fd, const char* ptr, size_t len) {
  if (s_log_on) {
    ctr_log_write(CTR_LOG_STDOUT, ptr, (unsigned int)len, 0);
    return (ssize_t)len;
  }
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
  if (s_log_on) {
    ctr_log_write(CTR_LOG_STDOUT, ptr, (unsigned int)len, 0);
    return (ssize_t)len;
  }
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
  s_tee_file = fopen(path, "a"); /* (AI-assisted) appended: earlier sessions stay (gk main.cpp) */
  if (!s_tee_file) {
    return -1;
  }
  if (!s_tee_lock_init) {
    LightLock_Init(&s_tee_lock);
    s_tee_lock_init = 1;
  }
  /* (AI-assisted) the log writer (config.ini log_async = off: each thread writes its own lines) */
  if (ctr_config_bool("log_async", 1)) {
    setvbuf(s_tee_file, NULL, _IONBF, 0); /* the writer writes whole batches */
    if (!log_start()) {
      setvbuf(s_tee_file, NULL, _IOFBF, BUFSIZ);
    }
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
  if (s_log_on) {
    printf("[ctr] log writer thread on core %d (config.ini log_async = off turns it off)\n",
           s_log_core);
  }
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
  if (s_log_on) {
    /* (AI-assisted) the log writer draws it */
    LightLock_Lock(&s_log_lock);
    snprintf(s_status_text, sizeof(s_status_text), "%s", text);
    s_status_dirty = 1;
    LightLock_Unlock(&s_log_lock);
    LightEvent_Signal(&s_log_event);
    return;
  }
  tee_lock();
  status_render(text);
  s_console_dirty = 1;
  tee_unlock();
}

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
  /* (AI-assisted) the log writer: write out everything logged before the crash, from here. Not if
   * the writer itself crashed, and only once it is between two writes (its lock then stays taken,
   * so it stops); the thread that crashed may hold the ring's lock: log_drain gives up then. */
  int files = 1;
  if (s_log_on) {
    files = ctr_thread_current_id() != s_log_thread_id && drain_lock_timed(2000);
    if (files) {
      log_drain(1);
    }
  }
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
  if (files && s_tee_file) {
    fputs(msg, s_tee_file);
    fflush(s_tee_file);
  }
  if (aptShouldClose()) {
    /* (AI-assisted) the system is closing the app (HOME Menu): nobody sees the crash screen, and
     * the HOME Menu waits until the app tells APT it closed, which _exit does (__appExit ->
     * aptExit); svcExitProcess alone left it on "closing software" forever */
    if (s_log_on && files) {
      run_jobs(); /* a background file write (settings) */
    }
    _exit(1);
  }
  show_screen("\x1b[31m", "*** OpenGOAL crashed ***", title, detail, 1,
              "Logs: " CTR_SD_DIR "/data/log/\nPlease report stdout.log and gk.log\n"
              "(this session is at their end).\n"
              "\x1b[32mPress START to exit.\x1b[0m");
  if (s_log_on && files) {
    run_jobs(); /* (AI-assisted) a background file write (settings), with the screen up */
  }
  wait_for_button();
  svcExitProcess();
  for (;;) {
  }
}

void ctr_message_wait(const char* title, const char* text) {
  s_con_paused = 1; /* (AI-assisted) the log writer leaves the screen alone */
  tee_lock();
  show_screen("\x1b[33m", "*** OpenGOAL ***", title, text, 0,
              "\x1b[32mPress START to continue.\x1b[0m");
  tee_unlock();
  wait_for_button();
  tee_lock();
  consoleSetWindow(&s_log_con, 1, STATUS_LINES + 1, 40, 30 - STATUS_LINES);
  console_write_raw("\x1b[2J");
  s_con_changed = 1; /* drawn again by the writer */
  s_status_dirty = 1;
  tee_unlock();
  s_con_paused = 0;
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
  unsigned int stack_size; /* (AI-assisted, j2-mem2) */
} ThreadStart;

/* (AI-assisted, j2-mem2) Stack high-water marks of the threads made here: at its start a thread
 * fills its stack below the current frame with a pattern (from 512 bytes under it down to 1 KB
 * above the stack's bottom, where libctru keeps the thread's own record); ctr_thread_stacks_check
 * finds the deepest word written. */
#define STK_MAX 24
#define STK_PATTERN 0x5a5a57acu
static struct {
  void* (*fn)(void*);
  const char* name;
  u32* lo;  /* the lowest patterned word */
  u32* hi;  /* one past the highest */
  unsigned int size;
  unsigned int logged;
  volatile int live;
} s_stk[STK_MAX];
static LightLock s_stk_lock = 1; /* free (LightLock_Init's value): threads start concurrently */

static int stk_slot_new(void* (*fn)(void*), unsigned int size, u32* lo, u32* hi) {
  int slot = -1;
  LightLock_Lock(&s_stk_lock);
  for (int i = 0; i < STK_MAX; i++) {
    if (!s_stk[i].live) {
      slot = i;
      s_stk[i].fn = fn;
      s_stk[i].name = NULL;
      s_stk[i].lo = lo;
      s_stk[i].hi = hi;
      s_stk[i].size = size;
      s_stk[i].logged = 0;
      s_stk[i].live = 1;
      break;
    }
  }
  LightLock_Unlock(&s_stk_lock);
  return slot;
}

void ctr_thread_name_current(const char* name) {
  const u32 here = (u32)(uintptr_t)&name;
  LightLock_Lock(&s_stk_lock);
  for (int i = 0; i < STK_MAX; i++) {
    if (s_stk[i].live && here >= (u32)(uintptr_t)s_stk[i].lo &&
        here < (u32)(uintptr_t)s_stk[i].hi + 4096) {
      s_stk[i].name = name;
    }
  }
  LightLock_Unlock(&s_stk_lock);
}

int ctr_thread_stacks_check(char* line, unsigned int line_size) {
  if (line_size < 128) {
    return 0;
  }
  int len = 0, grew = 0, deep = 0;
  line[0] = 0;
  LightLock_Lock(&s_stk_lock);
  for (int i = 0; i < STK_MAX; i++) {
    if (!s_stk[i].live) {
      continue;
    }
    const u32* w = s_stk[i].lo;
    while (w < s_stk[i].hi && *w == STK_PATTERN) {
      w++;
    }
    /* + the 512 bytes above the pattern and the trampoline's frame (roughly) */
    const unsigned int used = (unsigned int)((const u8*)s_stk[i].hi - (const u8*)w) + 640;
    if (!s_stk[i].logged || used >= s_stk[i].logged + 4096) {
      grew = 1;
    }
    if (used > s_stk[i].size / 2) {
      deep = 1;
    }
    if (used > s_stk[i].logged) {
      s_stk[i].logged = used;
    }
    if (len < (int)line_size - 64) {
      char nm[16];
      if (s_stk[i].name) {
        snprintf(nm, sizeof(nm), "%s", s_stk[i].name);
      } else {
        snprintf(nm, sizeof(nm), "%08x", (unsigned)(uintptr_t)s_stk[i].fn);
      }
      len += snprintf(line + len, line_size - len, " %s %.1f/%u", nm, used / 1024.0,
                      s_stk[i].size / 1024);
    }
  }
  LightLock_Unlock(&s_stk_lock);
  return grew ? (deep ? 2 : 1) : 0;
}

static void thread_trampoline(void* p) {
  ThreadStart start = *(ThreadStart*)p;
  free(p);
  /* (AI-assisted, j2-mem2) the pattern below this frame (see s_stk) */
  int slot = -1;
  {
    volatile u32 marker = 0;
    const uintptr_t here = (uintptr_t)&marker;
    u32* hi = (u32*)((here - 512) & ~(uintptr_t)3);
    u32* lo = (u32*)((here - start.stack_size + 1024 + 3) & ~(uintptr_t)3);
    if (start.stack_size > 4096 && lo < hi) {
      for (u32* w = lo; w < hi; w++) {
        *w = STK_PATTERN;
      }
      slot = stk_slot_new(start.fn, start.stack_size, lo, hi);
    }
    (void)marker;
  }
  start.fn(start.arg);
  if (slot >= 0) {
    s_stk[slot].live = 0;
  }
}

int ctr_core1_share(void) {
  return s_cpu_limit;
}

int ctr_core1_enable(void) {
  /* 30% if 80% is refused; without any share, ctr_thread_create_pinned refuses core 1.
   * (AI-assisted, j2-sound) On New 3DS 40% first: only the sound mixer runs there (11-17% of
   * real time in Jak II's city, Azahar), and the rest stays with the system's services (GPU, SD
   * card, DSP), which every service call of the game waits for. The Old 3DS's slower core keeps
   * 80% first. */
  if (!s_cpu_limit) {
    const int first = ctr_is_new3ds() ? 40 : 80;
    s_cpu_limit = R_SUCCEEDED(APT_SetAppCpuTimeLimit(first))  ? first
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
  start->stack_size = stack_size;
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
/* (AI-assisted) buffers queued after the DSP had played everything before them (a gap) */
static unsigned int s_audio_late = 0;
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
  s_audio_late = 0;
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

void ctr_audio_close_for_exit(void) {
  if (!s_audio_on) {
    return;
  }
  /* the mixer thread sees s_audio_on = 0 and stops queueing; its buffers stay allocated (the
   * process exits next) */
  s_audio_on = 0;
  __dsb();
  ndspSetCallback(NULL, NULL);
  ndspChnWaveBufClear(0);
  ndspExit();
}

int ctr_closing(void) {
  return aptShouldClose() ? 1 : 0;
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
  /* (AI-assisted) Late: the buffer queued before this one, the last the DSP had, is done already,
   * so the channel ran dry and this one was due before it came (a gap in the sound). The DSP's
   * dropped frames (ndspGetDroppedFrames) don't count this. At most a DSP frame (~5 ms) of a gap
   * goes unseen: the status is updated once per frame. */
  const ndspWaveBuf* prev = &s_audio_wbuf[(s_audio_next + s_audio_nbufs - 1) % s_audio_nbufs];
  if (prev->status == NDSP_WBUF_DONE) {
    s_audio_late++;
  }
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

unsigned int ctr_audio_late_buffers(void) {
  return s_audio_late;
}
