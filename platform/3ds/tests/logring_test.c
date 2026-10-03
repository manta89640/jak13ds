/* (AI-assisted) Host test for platform/3ds/port/ctr_logring.h, the 3DS log writer's ring buffer:
 * wrap-around against a simple model, dropping the oldest records (and the "[log] N bytes dropped"
 * count), dropping the newest while the reader copies, draining, long writes split into records,
 * producer threads racing a reader that copies without the lock, and the cost of one log line on
 * the calling thread. Build from the repo root:
 *   cc -O2 -I. platform/3ds/tests/logring_test.c -o /tmp/logring_test -lpthread && /tmp/logring_test
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "platform/3ds/port/ctr_logring.h"

static int g_fail = 0;
#define CHECK(c)                                                    \
  do {                                                              \
    if (!(c)) {                                                     \
      if (g_fail++ < 20) {                                          \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);         \
      }                                                             \
    }                                                               \
  } while (0)

static unsigned rnd_state = 12345;
static unsigned rnd(void) {
  rnd_state = rnd_state * 1664525u + 1013904223u;
  return rnd_state >> 8;
}

/* ---- a model of the ring: the records it holds, in order ---- */
#define MODEL_MAX 4096
typedef struct {
  unsigned len, mask;
  char data[64];
} mrec;
typedef struct {
  mrec q[MODEL_MAX];
  unsigned head, n;  /* circular queue of records */
  unsigned cap, used, reading, dropped;
} model;

static void model_put1(model* m, unsigned mask, const char* d, unsigned len) {
  const unsigned need = LOGRING_HDR + len;
  if (!len) {
    return; /* logring_put stores nothing */
  }
  if (need > m->cap) {
    m->dropped += len;
    return;
  }
  while (m->cap - m->used < need) {
    if (m->reading) {
      m->dropped += len;
      return;
    }
    mrec* o = &m->q[m->head];
    m->used -= LOGRING_HDR + o->len;
    m->dropped += o->len;
    m->head = (m->head + 1) % MODEL_MAX;
    m->n--;
  }
  mrec* r = &m->q[(m->head + m->n) % MODEL_MAX];
  r->len = len;
  r->mask = mask;
  memcpy(r->data, d, len);
  m->n++;
  m->used += need;
}

/* 1. random puts / partial reads with small buffers, compared with the model byte for byte */
static void test_random_vs_model(void) {
  static model m;
  for (unsigned cap = 23; cap < 200; cap += 7) {
    unsigned char mem[256];
    logring r;
    logring_init(&r, mem, cap);
    memset(&m, 0, sizeof(m));
    m.cap = cap;
    for (int step = 0; step < 20000; step++) {
      if (rnd() % 3) {
        char d[64];
        const unsigned len = rnd() % 40;
        for (unsigned i = 0; i < len; i++) {
          d[i] = (char)('a' + rnd() % 26);
        }
        const unsigned mask = 1 + rnd() % 3;
        logring_put(&r, mask, d, len);
        model_put1(&m, mask, d, len);
        CHECK(r.used == m.used);
      } else {
        unsigned dropped = 0;
        const unsigned avail = logring_read_begin(&r, &dropped);
        CHECK(dropped == m.dropped);
        m.dropped = 0;
        m.reading = avail;
        CHECK(avail == m.used);
        /* puts while the reader copies: may only drop the new records */
        for (unsigned k = rnd() % 3; k; k--) {
          char d[8] = "xyzxyzx";
          const unsigned len = 1 + rnd() % 7;
          logring_put(&r, 3, d, len);
          model_put1(&m, 3, d, len);
        }
        char b0[256], b1[256];
        logring_out outs[2] = {{b0, 40 + rnd() % 60, 0}, {b1, 40 + rnd() % 60, 0}};
        const unsigned consumed = logring_take(&r, avail, outs, 2);
        /* the model's version of the same take */
        char e0[256], e1[256];
        unsigned l0 = 0, l1 = 0, off = 0, k = 0;
        while (off < avail) {
          const mrec* x = &m.q[(m.head + k) % MODEL_MAX];
          const int f0 = !(x->mask & 1) || l0 + x->len <= outs[0].cap;
          const int f1 = !(x->mask & 2) || l1 + x->len <= outs[1].cap;
          if (!(f0 && f1)) {
            break;
          }
          if (x->mask & 1) {
            memcpy(e0 + l0, x->data, x->len);
            l0 += x->len;
          }
          if (x->mask & 2) {
            memcpy(e1 + l1, x->data, x->len);
            l1 += x->len;
          }
          off += LOGRING_HDR + x->len;
          k++;
        }
        CHECK(consumed == off);
        CHECK(outs[0].len == l0 && memcmp(b0, e0, l0) == 0);
        CHECK(outs[1].len == l1 && memcmp(b1, e1, l1) == 0);
        logring_read_end(&r, consumed);
        m.head = (m.head + k) % MODEL_MAX;
        m.n -= k;
        m.used -= consumed;
        m.reading = 0;
        CHECK(r.used == m.used && r.reading == 0);
      }
    }
  }
}

/* 2. full: the oldest records go, the count is reported once */
static void test_overflow_marker(void) {
  unsigned char mem[64];
  logring r;
  logring_init(&r, mem, sizeof(mem));
  char line[16];
  for (int i = 0; i < 10; i++) {
    snprintf(line, sizeof(line), "line %d...\n", i); /* 10 bytes */
    logring_put(&r, 3, line, 10);
  }
  /* 14 bytes per record: 4 fit, the first 6 were dropped (60 bytes of text) */
  unsigned dropped = 0;
  const unsigned avail = logring_read_begin(&r, &dropped);
  CHECK(dropped == 60);
  CHECK(avail == 56);
  char b0[128], b1[128];
  logring_out outs[2] = {{b0, sizeof(b0), 0}, {b1, sizeof(b1), 0}};
  outs[0].len = logring_dropped_line(b0, sizeof(b0), dropped);
  CHECK(strcmp(b0, "[log] 60 bytes dropped\n") == 0);
  const unsigned consumed = logring_take(&r, avail, outs, 2);
  logring_read_end(&r, consumed);
  CHECK(consumed == avail && r.used == 0);
  CHECK(memcmp(b0 + 23, "line 6...\nline 7...\nline 8...\nline 9...\n", 40) == 0);
  CHECK(outs[1].len == 40 && memcmp(b1, "line 6...\n", 10) == 0);
  logring_read_begin(&r, &dropped);
  CHECK(dropped == 0);
  logring_read_end(&r, 0);
}

/* 3. while the reader copies, the new record is dropped, not the ones being copied */
static void test_drop_newest_while_reading(void) {
  unsigned char mem[64];
  logring r;
  logring_init(&r, mem, sizeof(mem));
  for (int i = 0; i < 4; i++) {
    logring_put(&r, 1, "0123456789", 10);
  }
  unsigned dropped = 0;
  const unsigned avail = logring_read_begin(&r, &dropped);
  CHECK(avail == 56 && dropped == 0);
  CHECK(logring_put(&r, 1, "abcdefghij", 10) == 0); /* 8 bytes free */
  CHECK(logring_put(&r, 1, "ab", 2) == 2);          /* 6 bytes: fits */
  char b0[128];
  logring_out out = {b0, sizeof(b0), 0};
  const unsigned consumed = logring_take(&r, avail, &out, 1);
  CHECK(consumed == 56 && out.len == 40);
  logring_read_end(&r, consumed);
  CHECK(r.used == 6);
  logring_read_begin(&r, &dropped);
  CHECK(dropped == 10);
  out.len = 0;
  logring_read_end(&r, logring_take(&r, r.used, &out, 1));
  CHECK(out.len == 2 && memcmp(b0, "ab", 2) == 0 && r.used == 0);
}

/* 4. a write longer than a record is split; small outputs drain it over several reads */
static void test_split_and_drain(void) {
  static unsigned char mem[64 * 1024];
  static char text[20000], got[20000];
  logring r;
  logring_init(&r, mem, sizeof(mem));
  for (unsigned i = 0; i < sizeof(text); i++) {
    text[i] = (char)('A' + i % 23);
  }
  CHECK(logring_put(&r, 2, text, sizeof(text)) == sizeof(text));
  CHECK(r.used == sizeof(text) + 3 * LOGRING_HDR); /* 8192 + 8192 + 3616 */
  unsigned total = 0, reads = 0;
  while (r.used) {
    unsigned dropped;
    const unsigned avail = logring_read_begin(&r, &dropped);
    static char b[LOGRING_MAX_RECORD];
    logring_out outs[2] = {{NULL, 0, 0}, {b, sizeof(b), 0}};
    const unsigned consumed = logring_take(&r, avail, outs, 2);
    logring_read_end(&r, consumed);
    memcpy(got + total, b, outs[1].len);
    total += outs[1].len;
    reads++;
  }
  CHECK(total == sizeof(text) && memcmp(got, text, sizeof(text)) == 0 && reads == 3);
}

/* 5. producer threads and a reader that copies without the lock (as the 3DS writer thread) */
#define PRODUCERS 4
#define LINES 200000
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char g_mem[4096];
static logring g_ring;
static volatile int g_done = 0;

static void* producer(void* arg) {
  const int id = (int)(size_t)arg;
  char line[64];
  for (int i = 0; i < LINES; i++) {
    const int n = snprintf(line, sizeof(line), "P%d %d\n", id, i);
    pthread_mutex_lock(&g_lock);
    logring_put(&g_ring, 3, line, (unsigned)n);
    pthread_mutex_unlock(&g_lock);
  }
  return NULL;
}

static unsigned long long g_text_out = 0, g_dropped_total = 0, g_lines_out = 0;
static int g_last_seq[PRODUCERS];
static int g_order_errors = 0, g_mismatch = 0;

static void parse_lines(const char* b, unsigned n) {
  static char partial[64];
  static unsigned plen = 0;
  for (unsigned i = 0; i < n; i++) {
    if (b[i] != '\n') {
      if (plen < sizeof(partial) - 1) {
        partial[plen++] = b[i];
      }
      continue;
    }
    partial[plen] = 0;
    plen = 0;
    if (partial[0] == '[') {
      continue; /* a dropped line */
    }
    int id = -1, seq = -1;
    if (sscanf(partial, "P%d %d", &id, &seq) != 2 || id < 0 || id >= PRODUCERS) {
      g_order_errors++;
      continue;
    }
    if (seq <= g_last_seq[id]) {
      g_order_errors++;
    }
    g_last_seq[id] = seq;
    g_lines_out++;
  }
}

static void* reader(void* arg) {
  (void)arg;
  static char b0[1024], b1[1024];
  for (;;) {
    const int done = g_done;
    unsigned dropped;
    pthread_mutex_lock(&g_lock);
    const unsigned avail = logring_read_begin(&g_ring, &dropped);
    pthread_mutex_unlock(&g_lock);
    logring_out outs[2] = {{b0, sizeof(b0), 0}, {b1, sizeof(b1), 0}};
    const unsigned consumed = logring_take(&g_ring, avail, outs, 2);
    pthread_mutex_lock(&g_lock);
    logring_read_end(&g_ring, consumed);
    pthread_mutex_unlock(&g_lock);
    g_dropped_total += dropped;
    g_text_out += outs[0].len;
    if (outs[0].len != outs[1].len || memcmp(b0, b1, outs[0].len) != 0) {
      g_mismatch++;
    }
    parse_lines(b0, outs[0].len);
    if (done && consumed == avail && avail == 0) {
      break;
    }
  }
  return NULL;
}

static void test_threads(void) {
  logring_init(&g_ring, g_mem, sizeof(g_mem));
  for (int i = 0; i < PRODUCERS; i++) {
    g_last_seq[i] = -1;
  }
  pthread_t p[PRODUCERS], rd;
  pthread_create(&rd, NULL, reader, NULL);
  for (int i = 0; i < PRODUCERS; i++) {
    pthread_create(&p[i], NULL, producer, (void*)(size_t)i);
  }
  unsigned long long produced = 0;
  for (int i = 0; i < PRODUCERS; i++) {
    pthread_join(p[i], NULL);
  }
  for (int id = 0; id < PRODUCERS; id++) {
    for (int i = 0; i < LINES; i++) {
      char line[64];
      produced += (unsigned)snprintf(line, sizeof(line), "P%d %d\n", id, i);
    }
  }
  g_done = 1;
  pthread_join(rd, NULL);
  {
    unsigned dropped; /* drops after the reader's last begin */
    logring_read_begin(&g_ring, &dropped);
    g_dropped_total += dropped;
  }
  printf("threads: %d producers x %d lines into a %u byte ring: %llu lines out, %llu of %llu "
         "bytes dropped\n",
         PRODUCERS, LINES, (unsigned)sizeof(g_mem), g_lines_out, g_dropped_total, produced);
  CHECK(g_order_errors == 0);
  CHECK(g_mismatch == 0);
  CHECK(g_text_out + g_dropped_total == produced);
  CHECK(g_ring.used == 0);
}

/* 6. the cost of one log line on the calling thread: format + lock + copy into the ring (a reader
 * drains it at the same time), against writing and flushing it to a file (the old way) */
static double now_s(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static void* drain_loop(void* arg) {
  (void)arg;
  static char b0[16384], b1[16384];
  while (!g_done) {
    unsigned dropped;
    pthread_mutex_lock(&g_lock);
    const unsigned avail = logring_read_begin(&g_ring, &dropped);
    pthread_mutex_unlock(&g_lock);
    logring_out outs[2] = {{b0, sizeof(b0), 0}, {b1, sizeof(b1), 0}};
    const unsigned consumed = logring_take(&g_ring, avail, outs, 2);
    pthread_mutex_lock(&g_lock);
    logring_read_end(&g_ring, consumed);
    pthread_mutex_unlock(&g_lock);
    struct timespec ts = {0, 1000000};
    nanosleep(&ts, NULL);
  }
  return NULL;
}

static void measure(void) {
  static unsigned char mem[256 * 1024];
  logring_init(&g_ring, mem, sizeof(mem));
  g_done = 0;
  pthread_t rd;
  pthread_create(&rd, NULL, drain_loop, NULL);
  const char* msg = "[ctr] game thread ms/frame over 150 frames: frame 33.40 (29.9 fps) = logic";
  const int n = 200000;
  char line[256];
  double t0 = now_s();
  for (int i = 0; i < n; i++) {
    const int len = snprintf(line, sizeof(line), "[%02d:%02d] [%s] %s %d\n", i / 60 % 60, i % 60,
                             "info", msg, i);
    pthread_mutex_lock(&g_lock);
    logring_put(&g_ring, 3, line, (unsigned)len);
    pthread_mutex_unlock(&g_lock);
  }
  double t1 = now_s();
  g_done = 1;
  pthread_join(rd, NULL);
  printf("per line, calling thread: ring %.0f ns", (t1 - t0) * 1e9 / n);
  FILE* f = tmpfile();
  if (f) {
    const int m = 20000;
    t0 = now_s();
    for (int i = 0; i < m; i++) {
      const int len = snprintf(line, sizeof(line), "[%02d:%02d] [%s] %s %d\n", i / 60 % 60,
                               i % 60, "info", msg, i);
      fwrite(line, 1, (size_t)len, f);
      fflush(f);
    }
    t1 = now_s();
    fclose(f);
    printf(", fwrite + fflush to a file %.0f ns (host disk; the 3DS SD card: 1-400 ms)",
           (t1 - t0) * 1e9 / m);
  }
  printf("\n");
}

int main(void) {
  test_random_vs_model();
  test_overflow_marker();
  test_drop_newest_while_reading();
  test_split_and_drain();
  test_threads();
  measure();
  printf(g_fail ? "logring_test: %d FAILED\n" : "logring_test: all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
