/* Host test for platform/3ds/port/ctr_pad_ring.h and the 30 fps variable jump (mod-var-jump in
 * goal_src/jak1/engine/target/target.gc): HID updates every 4.27 ms into the 8-entry ring, game
 * reads every 33.3 ms (+ jitter). Checks that every press is seen once, that the middle and end
 * samples match the button at those times, and prints the jump level (0..6, height
 * 1.01 + 2.49 * level / 6 m) per press length for the PS2 at 60 Hz, the old 30 fps read and the
 * new one. Build from the repo root:
 *   cc -O2 -I. platform/3ds/tests/pad_ring_test.c -o /tmp/pad_ring_test && /tmp/pad_ring_test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/3ds/port/ctr_pad_ring.h"

#define CLK 268111856LL
#define HID_P (CLK / 234)
#define VBL (CLK / 60)
#define BTN 1u

static int g_fail = 0;
#define CHECK(c, ...)                   \
  do {                                  \
    if (!(c)) {                         \
      if (g_fail++ < 20) {              \
        printf("FAIL %s: ", #c);        \
        printf(__VA_ARGS__);            \
        printf("\n");                   \
      }                                 \
    }                                   \
  } while (0)

static unsigned rnd_state = 12345;
static unsigned rnd(void) {
  rnd_state = rnd_state * 1664525u + 1013904223u;
  return rnd_state >> 8;
}
static int64_t rnd64(int64_t n) {
  return (int64_t)(((uint64_t)rnd() << 24 | rnd()) % (uint64_t)n);
}

/* HID side: the button is down in [press, release) */
typedef struct {
  ctr_pad_ring_snap ring;
  int64_t next_update;
  int64_t phase;
  uint32_t idx;
  uint32_t last;
} hid_sim;

static int down_at(int64_t t, int64_t press, int64_t release) {
  return t >= press && t < release;
}

static void hid_run(hid_sim* h, int64_t until, int64_t press, int64_t release) {
  while (h->next_update <= until) {
    int64_t t = h->next_update;
    uint32_t held = down_at(t, press, release) ? BTN : 0;
    h->idx = (h->idx + 1) & 7;
    ctr_pad_ring_entry* e = &h->ring.e[h->idx];
    e->held = held;
    e->pressed = held & ~h->last;
    e->released = h->last & ~held;
    h->last = held;
    h->ring.newest = h->idx;
    if (h->idx == 0) {
      h->ring.tick0_prev = h->ring.tick0;
      h->ring.tick0 = t;
    }
    h->next_update += HID_P;
  }
}

/* level of the PS2: pad read at every vblank n*VBL; the jump starts at the first read that sees
 * the button, the level counts the reads after it that still see it (at most 6). -1: not seen */
static int ps2_level(int64_t press, int64_t release) {
  int64_t n0 = (press + VBL - 1) / VBL;
  if (!down_at(n0 * VBL, press, release)) {
    return -1;
  }
  int l = 0;
  while (l < 6 && down_at((n0 + l + 1) * VBL, press, release)) {
    l++;
  }
  return l;
}

/* one 30 fps run: reads at even vblanks + jitter. old: the held snapshot at each read (4 levels);
 * new: the ring's samples replayed like mod-var-jump on the 3DS */
static void run_30fps(int64_t press, int64_t release, int jitter, int* old_level, int* new_level,
                      int* downs) {
  hid_sim h;
  memset(&h, 0, sizeof(h));
  h.phase = rnd64(HID_P);
  h.next_update = h.phase - 40 * HID_P;
  h.ring.tick0 = h.next_update - HID_P;
  h.ring.tick0_prev = h.ring.tick0 - 8 * HID_P;
  ctr_pad_ring_state st;
  memset(&st, 0, sizeof(st));
  int64_t t_first = -8 * VBL;
  *old_level = -1;
  *new_level = -1;
  *downs = 0;
  int old_m0 = -1, new_m0 = -1, o = 0, open = 1;
  double f = -1.0, old_f = -1.0;
  int old_open = 1;
  int64_t T_prev = 0;
  for (int m = 0; m < 40; m++) {
    int64_t T = t_first + (int64_t)m * 2 * VBL + (jitter ? rnd64(CLK / 400) : 0);
    hid_run(&h, T, press, release);
    ctr_pad_ring_out r;
    ctr_pad_ring_read(&st, &h.ring, T, HID_P, VBL, &r);
    if (m == 0) {
      T_prev = T;
      continue;
    }
    *downs += (r.down & BTN) ? 1 : 0;
    /* mid and end samples against the button at those times (one HID period of slack) */
    int64_t tm = T - VBL;
    int mid_true = down_at(tm, press, release) || (press > T_prev && press <= tm);
    int near = 0;
    int64_t edges[4] = {tm, T_prev, tm, T_prev};
    for (int i = 0; i < 2; i++) {
      near |= press > edges[i] - 2 * HID_P && press <= edges[i] + 2 * HID_P;
      near |= release > edges[i] - 2 * HID_P && release <= edges[i] + 2 * HID_P;
    }
    T_prev = T;
    if (!near) {
      CHECK(((r.mid & BTN) != 0) == mid_true, "mid at press %lld release %lld read %d",
            (long long)press, (long long)release, m);
    }
    /* old: snapshot read */
    int held = down_at(T, press, release);
    if (old_m0 < 0 && held) {
      old_m0 = m;
    }
    if (old_m0 >= 0 && old_open) {
      double sf = (m - old_m0) * 10 / 30.0;
      if (sf > 1.0 || !held) {
        old_open = 0;
      } else {
        old_f = sf;
      }
    }
    /* new: the jump starts at the read that reports the press */
    if (new_m0 < 0 && (r.down & BTN)) {
      new_m0 = m;
      o = (r.early & BTN) ? 5 : 0;
    }
    if (new_m0 >= 0 && open) {
      int t = (m - new_m0) * 10 + o;
      for (int i = 0; i < 2; i++) {
        int s = i == 0 ? t - 5 : t;
        uint32_t smp = i == 0 ? r.mid : r.end;
        if (!open || s < 0) {
          continue;
        }
        double sf = s / 30.0;
        if (sf > 1.0 || !(smp & BTN)) {
          open = 0;
        } else {
          f = sf;
        }
      }
    }
  }
  *old_level = old_f < 0 ? -1 : (int)(old_f * 6 + 0.5);
  *new_level = f < 0 ? -1 : (int)(f * 6 + 0.5);
}

int main(void) {
  /* 1. every press seen exactly once, levels per press length */
  printf("press ms | PS2 level (mean, seen%%) | old 30 fps | new 30 fps | new==PS2\n");
  long agree_all = 0, cmp_all = 0;
  int max_diff = 0;
  double worst_mean = 0;
  for (int ms = 5; ms <= 140; ms += 5) {
    double ps2 = 0, old = 0, nw = 0;
    int n_ps2 = 0, n_old = 0, n_new = 0, agree = 0, cmp = 0;
    const int N = 400;
    for (int k = 0; k < N; k++) {
      int64_t press = 20 * VBL + rnd64(4 * VBL);
      int64_t release = press + (int64_t)ms * CLK / 1000;
      int lp = ps2_level(press, release);
      int lo, ln, downs;
      run_30fps(press, release, 1, &lo, &ln, &downs);
      if (release - press > HID_P) {
        CHECK(downs == 1, "%d ms press seen %d times", ms, downs);
      }
      if (lp >= 0) {
        ps2 += lp;
        n_ps2++;
      }
      if (lo >= 0) {
        old += lo;
        n_old++;
      }
      if (ln >= 0) {
        nw += ln;
        n_new++;
      }
      if (lp >= 0 && ln >= 0) {
        cmp++;
        agree += lp == ln;
        max_diff = abs(lp - ln) > max_diff ? abs(lp - ln) : max_diff;
      }
    }
    agree_all += agree;
    cmp_all += cmp;
    if (ms >= 20 && n_ps2 && n_new) {
      double dm = ps2 / n_ps2 - nw / n_new;
      dm = dm < 0 ? -dm : dm;
      worst_mean = dm > worst_mean ? dm : worst_mean;
    }
    printf("%8d | %5.2f %3d%% | %5.2f %3d%% | %5.2f %3d%% | %3d%%\n", ms,
           n_ps2 ? ps2 / n_ps2 : -1, n_ps2 * 100 / N, n_old ? old / n_old : -1, n_old * 100 / N,
           n_new ? nw / n_new : -1, n_new * 100 / N, cmp ? agree * 100 / cmp : 0);
  }
  printf("new level == PS2 level in %.1f%% of the presses both saw, never more than %d apart; "
         "mean levels within %.3f\n",
         100.0 * agree_all / (cmp_all ? cmp_all : 1), max_diff, worst_mean);
  /* one level from the grid phase (reads come up to 2.5 ms after the vblank), one from HID's 4.3 ms
   * steps: the same press can differ by 2 in rare cases, the means per press length must match */
  CHECK(max_diff <= 2 && worst_mean < 0.1, "max diff %d, mean diff %.3f", max_diff, worst_mean);

  /* 2. distinct levels reachable: PS2 7, old 4, new 7 */
  int seen_old[7] = {0}, seen_new[7] = {0};
  for (int k = 0; k < 4000; k++) {
    int64_t press = 20 * VBL + rnd64(4 * VBL);
    int64_t release = press + rnd64(CLK / 7);
    int lo, ln, downs;
    run_30fps(press, release, 1, &lo, &ln, &downs);
    if (lo >= 0) seen_old[lo] = 1;
    if (ln >= 0) seen_new[ln] = 1;
  }
  int c_old = 0, c_new = 0;
  for (int i = 0; i < 7; i++) {
    c_old += seen_old[i];
    c_new += seen_new[i];
  }
  printf("distinct jump heights: old %d, new %d (PS2 7)\n", c_old, c_new);
  CHECK(c_new == 7, "new has %d levels", c_new);

  /* 3. a frame longer than the ring (50 ms): a press still held is not lost */
  {
    hid_sim h;
    memset(&h, 0, sizeof(h));
    h.next_update = 0;
    ctr_pad_ring_state st;
    memset(&st, 0, sizeof(st));
    ctr_pad_ring_out r;
    hid_run(&h, 10 * VBL, 1LL << 60, 1LL << 61);
    ctr_pad_ring_read(&st, &h.ring, 10 * VBL, HID_P, VBL, &r);
    int64_t press = 10 * VBL + VBL / 3;
    hid_run(&h, 13 * VBL, press, 1LL << 61);
    ctr_pad_ring_read(&st, &h.ring, 13 * VBL, HID_P, VBL, &r);
    CHECK((r.down & BTN) && (r.held & BTN), "press in a 50 ms frame");
    hid_run(&h, 15 * VBL, press, 1LL << 61);
    ctr_pad_ring_read(&st, &h.ring, 15 * VBL, HID_P, VBL, &r);
    CHECK(!(r.down & BTN), "held press counted again");
  }
  printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
  return g_fail ? 1 : 0;
}
