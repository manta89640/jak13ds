#ifndef CTR_PAD_RING_H
#define CTR_PAD_RING_H
/* Buttons between game frames, from HID's PAD ring: 8 entries, one per HID update (~4.3 ms).
 * The PS2 read the pad every 16.7 ms; at 30 fps the game reads it every 33 ms. From the ring we
 * catch presses shorter than a frame and rebuild the 60 Hz sample in the middle of the frame.
 * Pure functions on a copy of the ring (host test: platform/3ds/tests/pad_ring_test.c). */
#include <stdint.h>

typedef struct {
  uint32_t held;     /* buttons down after the update */
  uint32_t pressed;  /* went down in the update */
  uint32_t released; /* went up in the update */
} ctr_pad_ring_entry;

typedef struct {
  int64_t tick0;      /* system tick of the last write of entry 0 */
  int64_t tick0_prev; /* of the write before it */
  uint32_t newest;    /* index of the newest entry */
  ctr_pad_ring_entry e[8];
} ctr_pad_ring_snap;

typedef struct {
  int started;
  uint32_t newest; /* newest entry at the last read */
  int64_t tick;    /* time of the last read */
  uint32_t held;   /* held at the last read */
} ctr_pad_ring_state;

typedef struct {
  uint32_t held;  /* down now */
  uint32_t down;  /* went down since the last read, short taps included */
  uint32_t early; /* went down in (last read, now - half] */
  uint32_t mid;   /* 60 Hz sample at now - half: down then, or went down in (last read, now - half] */
  uint32_t end;   /* sample now: down now, or went down in (now - half, now] */
} ctr_pad_ring_out;

/* period: expected ticks between HID updates; half: ticks of a PS2 frame (1/60 s) */
static inline void ctr_pad_ring_read(ctr_pad_ring_state* st, const ctr_pad_ring_snap* s,
                                     int64_t now, int64_t period, int64_t half,
                                     ctr_pad_ring_out* out) {
  uint32_t idx = s->newest & 7;
  int64_t p = (s->tick0 - s->tick0_prev) / 8;
  if (p < period / 2 || p > period * 2) {
    p = period;
  }
  int64_t age0 = now - (s->tick0 + (int64_t)idx * p);
  if (age0 < 0) {
    age0 = 0;
  }
  if (age0 > p) {
    age0 = p;
  }
  uint32_t held = s->e[idx].held;
  out->held = held;
  out->down = 0;
  out->early = 0;
  out->mid = held;
  out->end = held;
  if (!st->started) {
    st->started = 1;
    st->newest = idx;
    st->tick = now;
    st->held = held;
    return;
  }
  int64_t dt = now - st->tick;
  /* updates since the last read: known modulo 8 from the index, the elapsed time gives the turns */
  int d = (int)((idx - st->newest) & 7);
  double expected = dt > age0 ? (double)(dt - age0) / (double)p + 0.5 : 0.0;
  int turns = (int)((expected - d) / 8.0 + 0.5);
  if (turns < 0) {
    turns = 0;
  }
  int n = d + 8 * turns;
  uint32_t mid_held = st->held;
  uint32_t late = 0;
  if (n > 8) {
    /* older updates were overwritten: what went down in them shows in the oldest entry's state */
    const ctr_pad_ring_entry* o = &s->e[(idx + 1) & 7];
    uint32_t before = (o->held & ~o->pressed) | o->released;
    uint32_t gap = before & ~st->held;
    out->down |= gap;
    out->early |= gap;
    mid_held = before;
    n = 8;
  }
  for (int k = n - 1; k >= 0; k--) {
    const ctr_pad_ring_entry* e = &s->e[(idx - (uint32_t)k) & 7];
    out->down |= e->pressed;
    if (age0 + (int64_t)k * p >= half) {
      out->early |= e->pressed;
      mid_held = e->held;
    } else {
      late |= e->pressed;
    }
  }
  out->mid = mid_held | out->early;
  out->end = held | late;
  st->newest = idx;
  st->tick = now;
  st->held = held;
}

#endif
