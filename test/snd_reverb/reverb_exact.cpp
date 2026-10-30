// Host test of game/sound/common/reverb.cpp at 48 kHz against a plain copy of PCSX2's SPU2 effect
// unit (Reverb.cpp, ReverbResample.cpp's reference filters) with libsd's register values.
#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

#include "game/sound/common/reverb.h"

using namespace snd;

namespace {

constexpr u16 kPresets[10][32] = {
    {},
    {0x007D, 0x005B, 0x6D80, 0x54B8, 0xBED0, 0x0000, 0x0000, 0xBA80, 0x5800, 0x5300, 0x04D6,
     0x0333, 0x03F0, 0x0227, 0x0374, 0x01EF, 0x0336, 0x01B7, 0x0335, 0x01B6, 0x0334, 0x01B5,
     0x0334, 0x01B5, 0x0334, 0x01B5, 0x01B4, 0x0136, 0x00B8, 0x005C, 0x8000, 0x8000},
    {},
    {},
    {0x00E3, 0x00A9, 0x6F60, 0x4FA8, 0xBCE0, 0x4510, 0xBEF0, 0xA680, 0x5680, 0x52C0, 0x0DFB,
     0x0B58, 0x0D09, 0x0A3C, 0x0BD9, 0x0973, 0x0B59, 0x08DA, 0x08D9, 0x05E9, 0x07EC, 0x04B0,
     0x06EF, 0x03D2, 0x05EA, 0x031D, 0x031C, 0x0238, 0x0154, 0x00AA, 0x8000, 0x8000},
    {0x01A5, 0x0139, 0x6000, 0x5000, 0x4C00, 0xB800, 0xBC00, 0xC000, 0x6000, 0x5C00, 0x15BA,
     0x11BB, 0x14C2, 0x10BD, 0x11BC, 0x0DC1, 0x11C0, 0x0DC3, 0x0DC0, 0x09C1, 0x0BC4, 0x07C1,
     0x0A00, 0x06CD, 0x09C2, 0x05C1, 0x05C0, 0x041A, 0x0274, 0x013A, 0x8000, 0x8000},
};
constexpr u32 kSize[10] = {0x2, 0x4d8, 0x3e8, 0x908, 0xdfc, 0x15bc, 0x1ed8, 0x3008, 0x3008, 0x780};

constexpr s16 kDown[39] = {-1,    0, 2,     0, -10,  0, 35,   0, -103, 0,     266, 0,  -616,
                           0,     1332, 0, -2960, 0, 10246, 16384, 10246, 0, -2960, 0, 1332,
                           0,     -616, 0, 266,   0, -103, 0,    35,  0,  -10, 0,    2,  0, -1};

s32 clamp_mix(s32 v) {
  return v < -32768 ? -32768 : v > 32767 ? 32767 : v;
}

struct Ref {
  std::vector<s16> mem;
  u32 len;
  u32 cycles = 0;
  s32 pos = 0;
  s16 down[2][128]{}, up[2][128]{};
  s32 r[32];
  s32 v(int i) const { return (s16)r[i]; }
  s32 a(int i) const { return r[i] * 4; }

  explicit Ref(int mode) : len(kSize[mode] * 4) {
    mem.assign(len, 0);
    for (int i = 0; i < 32; i++) r[i] = kPresets[mode][i];
  }
  u32 idx(s32 off) const { return (u32)((((s64)(cycles >> 1) + off) % len + len) % len); }

  s32 downsample(bool right) {
    int index = (pos - 39) & 63;
    s32 out = 0;
    for (int i = 0; i < 39; i++) out += down[right][index + i] * kDown[i];
    return clamp_mix(out >> 15);
  }
  void upsample(s32& l, s32& rr) {
    int index = (pos - 39) & 63;
    l = rr = 0;
    for (int i = 0; i < 39; i++) {
      const s32 c = clamp_mix(kDown[i] * 2);
      l += up[0][index + i] * c;
      rr += up[1][index + i] * c;
    }
    l = clamp_mix(l >> 15);
    rr = clamp_mix(rr >> 15);
  }

  void step(s32 inl, s32 inr, s32& outl, s32& outr) {
    down[0][pos] = down[0][pos | 64] = (s16)inl;
    down[1][pos] = down[1][pos | 64] = (s16)inr;
    const bool R = cycles & 1;
    // register order as in reverb.cpp
    const s32 same_src = idx(a(R ? 17 : 16)), same_dst = idx(a(R ? 11 : 10)),
              same_prv = idx(a(R ? 11 : 10) - 1);
    const s32 diff_src = idx(a(R ? 24 : 25)), diff_dst = idx(a(R ? 19 : 18)),
              diff_prv = idx(a(R ? 19 : 18) - 1);
    const s32 c1 = idx(a(R ? 13 : 12)), c2 = idx(a(R ? 15 : 14)), c3 = idx(a(R ? 21 : 20)),
              c4 = idx(a(R ? 23 : 22));
    const s32 apf1_src = idx(a(R ? 27 : 26) - a(0)), apf1_dst = idx(a(R ? 27 : 26));
    const s32 apf2_src = idx(a(R ? 29 : 28) - a(1)), apf2_dst = idx(a(R ? 29 : 28));
#define MUL(x, y) ((x) * (y) >> 15)
    s32 in = MUL(R ? v(31) : v(30), downsample(R));
    s32 same = MUL(v(2), in + MUL(v(7), mem[same_src]) - mem[same_prv]) + mem[same_prv];
    s32 diff = MUL(v(2), in + MUL(v(7), mem[diff_src]) - mem[diff_prv]) + mem[diff_prv];
    s32 out = MUL(v(3), mem[c1]) + MUL(v(4), mem[c2]) + MUL(v(5), mem[c3]) + MUL(v(6), mem[c4]);
    s32 apf1 = out - MUL(v(8), mem[apf1_src]);
    out = mem[apf1_src] + MUL(v(8), apf1);
    s32 apf2 = out - MUL(v(9), mem[apf2_src]);
    out = mem[apf2_src] + MUL(v(9), apf2);
    mem[same_dst] = clamp_mix(same);
    mem[diff_dst] = clamp_mix(diff);
    mem[apf1_dst] = clamp_mix(apf1);
    mem[apf2_dst] = clamp_mix(apf2);
    out = clamp_mix(out);
    up[R][pos] = up[R][pos | 64] = (s16)out;
    up[!R][pos] = up[!R][pos | 64] = 0;
    pos = (pos + 1) & 63;
    upsample(outl, outr);
    cycles++;
  }
};

}  // namespace

int main() {
  std::mt19937 rng(1234);
  int bad = 0;
  long total = 0;
  for (int mode : {1, 4, 5}) {
    for (int trial = 0; trial < 6; trial++) {
      Ref ref(mode);
      Reverb rev;
      rev.SetMode(mode);
      const s16 dl = (s16)(trial == 0 ? 32767 : rng() % 65536 - 32768);
      const s16 dr = (s16)(trial == 0 ? -32768 : rng() % 65536 - 32768);
      rev.SetDepth(dl, dr);
      const int n = 48000 * 2;
      std::vector<s16Output> wet(n), out(n);
      for (int i = 0; i < n; i++) {
        // bursts of loud noise, tones and silence (the tail)
        const int phase = (i / 4000) % 4;
        const s32 amp = phase == 0 ? 32767 : phase == 1 ? 3000 : phase == 2 ? 200 : 0;
        wet[i].left = amp ? (s16)((s32)(rng() % (2 * amp + 1)) - amp) : 0;
        wet[i].right = amp ? (s16)((s32)(rng() % (2 * amp + 1)) - amp) : 0;
        out[i].left = (s16)(rng() % 2000 - 1000);
        out[i].right = (s16)(rng() % 2000 - 1000);
      }
      std::vector<s16Output> got = out;
      for (int i = 0; i < n;) {
        const int k = std::min<int>(n - i, 1 + rng() % 300);
        rev.Process(&wet[i], &got[i], k);
        i += k;
      }
      for (int i = 0; i < n; i++) {
        s32 l, r;
        ref.step(wet[i].left, wet[i].right, l, r);
        const s16 el = (s16)clamp_mix(out[i].left + ((l * dl) >> 15));
        const s16 er = (s16)clamp_mix(out[i].right + ((r * dr) >> 15));
        if (el != got[i].left || er != got[i].right) {
          if (bad < 10)
            printf("mode %d trial %d sample %d: want %d %d got %d %d\n", mode, trial, i, el, er,
                   got[i].left, got[i].right);
          bad++;
        }
      }
      total += n;
    }
  }
  printf("%ld samples, %d mismatches\n", total, bad);
  return bad != 0;
}
