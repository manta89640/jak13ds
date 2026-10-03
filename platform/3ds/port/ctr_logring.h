#pragma once

/*
 * (AI-assisted)
 * The ring buffer of the 3DS log writer (ctr_port.c): log text waits here until the writer thread
 * writes it to the SD card, so the threads that log never wait for the card. Plain C, header only
 * (host test: platform/3ds/tests/logring_test.c), no locking of its own: the caller holds one lock
 * around logring_put, logring_read_begin and logring_read_end.
 *
 * Records: a 4 byte header (payload length, 16 bit little endian; outputs mask; 0) and the
 * payload, back to back, wrapping around the end of the buffer.
 *
 * The reader copies records out without the lock: between logring_read_begin and logring_read_end
 * the records stored before read_begin are neither dropped nor overwritten (producers write only
 * into free space), so the lock is held only for a few instructions plus one record's memcpy.
 *
 * When there is no room, the oldest records are dropped (the newest log lines are the ones that
 * explain a crash), and counted; the reader then writes a "[log] N bytes dropped" line. While the
 * reader is copying the oldest records out, the new record is dropped instead.
 */

#include <stdio.h>
#include <string.h>

#define LOGRING_HDR 4u
/* longer writes are split into several records */
#define LOGRING_MAX_RECORD 8192u

typedef struct {
  unsigned char* buf;
  unsigned cap;      /* bytes, at least LOGRING_HDR + LOGRING_MAX_RECORD */
  unsigned rd;       /* offset of the oldest record */
  unsigned used;     /* bytes held by records */
  unsigned reading;  /* bytes at rd that the reader is copying out (logring_read_begin) */
  unsigned dropped;  /* payload bytes dropped since the last logring_read_begin */
} logring;

/* an output of logring_take */
typedef struct {
  char* buf;
  unsigned cap;
  unsigned len;
} logring_out;

static inline void logring_init(logring* r, void* mem, unsigned cap) {
  r->buf = (unsigned char*)mem;
  r->cap = cap;
  r->rd = 0;
  r->used = 0;
  r->reading = 0;
  r->dropped = 0;
}

static inline void logring_copy_in(logring* r, unsigned pos, const void* src, unsigned n) {
  const unsigned first = r->cap - pos < n ? r->cap - pos : n;
  memcpy(r->buf + pos, src, first);
  if (n > first) {
    memcpy(r->buf, (const char*)src + first, n - first);
  }
}

static inline void logring_copy_out(const logring* r, unsigned pos, void* dst, unsigned n) {
  const unsigned first = r->cap - pos < n ? r->cap - pos : n;
  memcpy(dst, r->buf + pos, first);
  if (n > first) {
    memcpy((char*)dst + first, r->buf, n - first);
  }
}

/* One record of at most LOGRING_MAX_RECORD bytes (lock held). Returns 1, or 0 if dropped. */
static inline int logring_put1(logring* r, unsigned mask, const char* data, unsigned len) {
  const unsigned need = LOGRING_HDR + len;
  if (need > r->cap) {
    r->dropped += len;
    return 0;
  }
  while (r->cap - r->used < need) {
    if (r->reading) {
      /* the oldest records are being copied out right now */
      r->dropped += len;
      return 0;
    }
    unsigned char h[2];
    logring_copy_out(r, r->rd, h, 2);
    const unsigned old = (unsigned)h[0] | ((unsigned)h[1] << 8);
    r->rd = (r->rd + LOGRING_HDR + old) % r->cap;
    r->used -= LOGRING_HDR + old;
    r->dropped += old;
  }
  const unsigned char h[LOGRING_HDR] = {(unsigned char)(len & 0xff), (unsigned char)(len >> 8),
                                        (unsigned char)mask, 0};
  const unsigned pos = (r->rd + r->used) % r->cap;
  logring_copy_in(r, pos, h, LOGRING_HDR);
  logring_copy_in(r, (pos + LOGRING_HDR) % r->cap, data, len);
  r->used += need;
  return 1;
}

/* Store `len` bytes for the outputs in `mask` (lock held). Returns the bytes stored. */
static inline unsigned logring_put(logring* r, unsigned mask, const char* data, unsigned len) {
  unsigned stored = 0;
  while (len) {
    const unsigned n = len < LOGRING_MAX_RECORD ? len : LOGRING_MAX_RECORD;
    if (logring_put1(r, mask, data, n)) {
      stored += n;
    }
    data += n;
    len -= n;
  }
  return stored;
}

/* Reader (lock held): everything stored so far may be copied out without the lock until
 * logring_read_end. Returns its size in bytes; *dropped = payload bytes dropped since the last
 * call. */
static inline unsigned logring_read_begin(logring* r, unsigned* dropped) {
  r->reading = r->used;
  *dropped = r->dropped;
  r->dropped = 0;
  return r->used;
}

/* Reader (no lock needed): copies whole records from the first `avail` bytes of the region into
 * the outputs (mask bit i -> outs[i]) until one of them is full. Returns the bytes consumed. */
static inline unsigned logring_take(const logring* r, unsigned avail, logring_out* outs,
                                    int nouts) {
  unsigned off = 0;
  while (off < avail) {
    const unsigned pos = (r->rd + off) % r->cap;
    unsigned char h[LOGRING_HDR];
    logring_copy_out(r, pos, h, LOGRING_HDR);
    const unsigned len = (unsigned)h[0] | ((unsigned)h[1] << 8);
    const unsigned mask = h[2];
    int fits = 1, empty = 1;
    for (int i = 0; i < nouts; i++) {
      if ((mask >> i) & 1) {
        fits = fits && outs[i].len + len <= outs[i].cap;
        empty = empty && outs[i].len == 0;
      }
    }
    if (!fits && !empty) {
      break;
    }
    if (fits) { /* (never too big for empty outputs of LOGRING_MAX_RECORD bytes: else skipped) */
      for (int i = 0; i < nouts; i++) {
        if ((mask >> i) & 1) {
          logring_copy_out(r, (pos + LOGRING_HDR) % r->cap, outs[i].buf + outs[i].len, len);
          outs[i].len += len;
        }
      }
    }
    off += LOGRING_HDR + len;
  }
  return off;
}

/* Reader (lock held): the first `consumed` bytes (whole records) are done. */
static inline void logring_read_end(logring* r, unsigned consumed) {
  r->rd = (r->rd + consumed) % r->cap;
  r->used -= consumed;
  r->reading = 0;
}

/* The line the reader writes for dropped text (into out, `size` bytes). Returns its length. */
static inline unsigned logring_dropped_line(char* out, unsigned size, unsigned dropped) {
  const int n = snprintf(out, size, "[log] %u bytes dropped\n", dropped);
  return n < 0 ? 0 : ((unsigned)n < size ? (unsigned)n : size - 1);
}
