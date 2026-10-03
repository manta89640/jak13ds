/*!
 * @file voice_exact.cpp
 * (AI-assisted)
 * Host test of the sound voice (game/sound/common/voice.cpp: the 3DS mixer's Voice::RunBlock and
 * the per sample Run()) against a frozen copy of it from before the block-wise ADPCM decode
 * (test/snd_voice/ref, namespace snd_ref). Both play the same random voices: synthetic ADPCM data
 * (all shifts and filters, the invalid ones too, every combination of the loop flags) and the
 * sample data of the sound bank files given, pitches from 0 to past the 0x3FFF clamp, random
 * envelopes and volumes (sweeps too), key on / key off and register writes between blocks of
 * random length, at the 3DS's 32728 Hz ratio, at 48 kHz and at other ratios, through RunBlock()
 * and Run(). Every output sample is compared, and NAX, the envelope level and the stopped flag
 * after every block. Then both are timed on a game-like mix (48 voices in 128-sample chunks at
 * 32728 Hz, also compared).
 *
 *   test/snd_voice/build.sh && /tmp/snd_voice_exact [trials] [seed] [bank.SBK ...]
 *
 * Exit status 0 when everything matches.
 */

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include "game/sound/common/voice.h"
#include "test/snd_voice/ref/voice.h"

namespace {

// the 3DS mixer's ratio (game/sound/989snd/player.cpp): 48000 / 32728 SPU ticks per output sample
constexpr u32 kTickRatio = (u32)((48000ull * 65536 + 32728 / 2) / 32728);

using Rng = std::mt19937;
u32 Rand(Rng& rng, u32 n) {
  return (u32)(rng() % n);
}

// ADPCM data in u16 words, 8 per block: a header (shift in bits 0-3, filter 4-6, loop end 8, loop
// repeat 9, loop start 10), then 7 data words of 4 nibbles
struct Data {
  std::string name;
  std::vector<u16> words;
  u32 blocks() const { return (u32)(words.size() / 8) - 1; }  // without the guard
};

// a last block with loop end + repeat: a voice that runs past the data loops back to its loop
// start instead of reading past the end
void AddGuard(Data& d) {
  d.words.resize(d.words.size() / 8 * 8);
  d.words.insert(d.words.end(), 8, 0);
  d.words[d.words.size() - 8] = 0x0300;
}

// random blocks: all 16 shifts, all 8 filters (5-7: none), loop flags (any combination) in
// flag_pct % of the blocks, the unused header bits at random; data words with full range nibbles
// or small ones (-2..1: realistic levels)
Data MakeSynthetic(Rng& rng, u32 blocks, u32 flag_pct, const char* name) {
  Data d{name, std::vector<u16>(8 * blocks)};
  for (u32 b = 0; b < blocks; b++) {
    u16 h = (u16)(rng() & ~0x0700u);
    if (Rand(rng, 10) < 7) {
      h = (u16)((h & ~0x0070u) | (Rand(rng, 5) << 4));
    }
    if (Rand(rng, 100) < flag_pct) {
      h |= (u16)((1 + Rand(rng, 7)) << 8);
    }
    d.words[8 * b] = h;
    const bool small = Rand(rng, 2);
    for (int k = 1; k < 8; k++) {
      u16 w = (u16)rng();
      if (small) {
        w = 0;
        for (int j = 0; j < 4; j++) {
          w |= (u16)(((Rand(rng, 4) + 14) & 0xF) << (4 * j));
        }
      }
      d.words[8 * b + k] = w;
    }
  }
  AddGuard(d);
  return d;
}

// The sample data of a sound bank file (iso_data/jak1/SBK/*.SBK, MUS/*.MUS): its FileAttributes
// (type 1 or 3, 2 or 3 chunks: bank, samples, midi) come before the "SBlk" / "SBv2" chunk. The
// whole file when they aren't found.
bool LoadBank(const char* path, Data& d) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    return false;
  }
  const std::vector<u8> bytes((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
  auto u32_at = [&](size_t o) {
    u32 v = 0;
    if (o + 4 <= bytes.size()) {
      std::memcpy(&v, &bytes[o], 4);
    }
    return v;
  };
  size_t from = 0, size = bytes.size();
  for (size_t p = 0; p + 4 <= bytes.size(); p++) {
    if (std::memcmp(&bytes[p], "SBlk", 4) != 0 && std::memcmp(&bytes[p], "SBv2", 4) != 0) {
      continue;
    }
    for (size_t x = p >= 64 ? p - 64 : 0; x < p; x += 4) {
      const u32 type = u32_at(x), chunks = u32_at(x + 4);
      if ((type == 1 || type == 3) && (chunks == 2 || chunks == 3) && x + u32_at(x + 8) == p) {
        from = x + u32_at(x + 16);
        size = std::min<size_t>(u32_at(x + 20), bytes.size() - std::min(from, bytes.size()));
        break;
      }
    }
    break;
  }
  d.name = path;
  d.words.assign(size / 2, 0);
  std::memcpy(d.words.data(), bytes.data() + from, size / 2 * 2);
  AddGuard(d);
  return d.blocks() > 0;
}

u16 RandPitch(Rng& rng) {
  switch (Rand(rng, 12)) {
    case 0:
      return 0;
    case 1:
      return (u16)(1 + Rand(rng, 16));  // very low
    case 2:
      return (u16)Rand(rng, 0x400);
    case 3:
      return 0x1000;
    case 4:
      return (u16)(0x3F00 + Rand(rng, 0xC100));  // up to past the 0x3FFF clamp
    case 5:
    case 6:
    case 7:
      return (u16)(0x1000 + Rand(rng, 0x3000));  // above 1.0
    default:
      return (u16)(0x400 + Rand(rng, 0xC00));
  }
}

u16 RandVolume(Rng& rng) {
  switch (Rand(rng, 10)) {
    case 0:
      return 0;
    case 1:
    case 2:
      return (u16)(0x8000 | rng());  // a sweep
    case 3:
      return (u16)(0x4000 | Rand(rng, 0x4000));  // negative (level = bits << 1)
    default:
      return (u16)Rand(rng, 0x4000);
  }
}

// ADSR1 / ADSR2: anything, or game-like (a fast attack, a slow sustain and release)
void RandAdsr(Rng& rng, u16& a1, u16& a2) {
  if (Rand(rng, 3) == 0) {
    a1 = (u16)rng();
    a2 = (u16)rng();
  } else {
    a1 = (u16)((rng() & 0x80FF) | (Rand(rng, 9) << 10) | (Rand(rng, 4) << 8));
    a2 = (u16)((rng() & 0xC0E0) | ((0x10 + Rand(rng, 16)) << 8) | (5 + Rand(rng, 27)));
  }
}

struct Stats {
  u64 trials = 0, blocks = 0, samples = 0, run = 0, keyons = 0, stops = 0, mismatches = 0;
};

// the new voice and the reference, given the same calls
struct Pair {
  snd::Voice a;
  snd_ref::Voice b;
  template <typename F>
  void Both(F&& f) {
    f(a);
    f(b);
  }
};

void Trial(Rng& rng, const Data& data, u32 ratio, Stats& st) {
  Pair p;
  const u32 nblocks = data.blocks();
  // an address in the data: a block start, or any word
  auto rand_addr = [&](bool aligned) {
    const u32 a = 8 * Rand(rng, nblocks);
    return aligned ? a : a + Rand(rng, 8);
  };
  auto key_on = [&](bool new_params) {
    if (new_params) {
      const u32 ssa = rand_addr(Rand(rng, 5) != 0);
      const u16 pitch = RandPitch(rng);
      u16 a1, a2;
      RandAdsr(rng, a1, a2);
      const u16 vl = RandVolume(rng), vr = RandVolume(rng);
      p.Both([&](auto& v) {
        v.SetSsa(ssa);
        v.SetPitch(pitch);
        v.SetAsdr1(a1);
        v.SetAsdr2(a2);
        v.SetVolume(vl, vr);
      });
    }
    if (Rand(rng, 8) == 0) {
      // a loop address before the key on (which clears the custom loop flag but not the address)
      const u32 lsa = rand_addr(Rand(rng, 2) != 0);
      p.Both([&](auto& v) { v.SetLsa(lsa); });
    }
    p.Both([](auto& v) { v.KeyOn(); });
    st.keyons++;
  };
  p.Both([&](auto& v) { v.SetSample(const_cast<u16*>(data.words.data())); });
  key_on(true);
  st.trials++;

  std::vector<snd::s16Output> oa;
  std::vector<snd_ref::s16Output> ob;
  const int blocks = 20 + (int)Rand(rng, 500);
  int stopped_blocks = 0;
  for (int blk = 0; blk < blocks; blk++) {
    int n;
    switch (Rand(rng, 10)) {
      case 0:
      case 1:
        n = 1 + (int)Rand(rng, 8);
        break;
      case 2:
      case 3:
      case 4:
        n = 128;
        break;
      default:
        n = 1 + (int)Rand(rng, 256);
    }
    oa.resize(n);
    ob.resize(n);
    for (int k = 0; k < n; k++) {
      // what other voices mixed: near the limits sometimes (saturation)
      const s16 l = (s16)(Rand(rng, 4) == 0 ? rng() : Rand(rng, 4000) - 2000);
      const s16 r = (s16)(Rand(rng, 4) == 0 ? rng() : Rand(rng, 4000) - 2000);
      oa[k] = {l, r};
      ob[k] = {l, r};
    }
    int ra, rb;
    const bool per_sample = Rand(rng, 8) == 0;
    if (per_sample) {
      // the PC path (Synth::Tick() runs stopped voices too)
      for (int k = 0; k < n; k++) {
        oa[k] += p.a.Run();
        ob[k] += p.b.Run();
      }
      ra = rb = n;
    } else {
      ra = p.a.RunBlock(oa.data(), n, ratio);
      rb = p.b.RunBlock(ob.data(), n, ratio);
    }
    st.blocks++;
    st.samples += (u64)n;
    st.run += (u64)rb;
    int first_diff = -1;
    for (int k = 0; k < n && first_diff < 0; k++) {
      if (oa[k].left != ob[k].left || oa[k].right != ob[k].right) {
        first_diff = k;
      }
    }
    if (ra != rb || first_diff >= 0 || p.a.GetNax() != p.b.GetNax() ||
        p.a.Stopped() != p.b.Stopped() || p.a.GetEnvx() != p.b.GetEnvx()) {
      if (st.mismatches < 10) {
        printf(
            "MISMATCH (%s, ratio %05x, trial %llu, block %d, n %d, %s): ran %d / %d, nax %u / %u, "
            "stopped %d / %d, envx %d / %d",
            data.name.c_str(), ratio, (unsigned long long)st.trials, blk, n,
            per_sample ? "Run" : "RunBlock", ra, rb, p.a.GetNax(), p.b.GetNax(), (int)p.a.Stopped(),
            (int)p.b.Stopped(), p.a.GetEnvx(), p.b.GetEnvx());
        if (first_diff >= 0) {
          printf(", sample %d: %d,%d / %d,%d", first_diff, oa[first_diff].left,
                 oa[first_diff].right, ob[first_diff].left, ob[first_diff].right);
        }
        printf("\n");
      }
      st.mismatches++;
      return;
    }

    // register writes and keys between blocks, the same on both
    if (Rand(rng, 100) < 3) {
      p.Both([](auto& v) { v.KeyOff(); });
    }
    if (Rand(rng, 100) < 2) {
      key_on(Rand(rng, 2) != 0);
    }
    if (Rand(rng, 100) < 4) {
      const u16 pitch = RandPitch(rng);
      p.Both([&](auto& v) { v.SetPitch(pitch); });
    }
    if (Rand(rng, 100) < 2) {
      u16 a1, a2;
      RandAdsr(rng, a1, a2);
      p.Both([&](auto& v) {
        v.SetAsdr1(a1);
        v.SetAsdr2(a2);
      });
    }
    if (Rand(rng, 100) < 3) {
      const u16 vl = RandVolume(rng), vr = RandVolume(rng);
      switch (Rand(rng, 3)) {
        case 0:
          p.Both([&](auto& v) { v.SetVolume(vl, vr); });
          break;
        case 1:
          p.Both([&](auto& v) { v.SetVolumeL(vl); });
          break;
        default:
          p.Both([&](auto& v) { v.SetVolumeR(vr); });
      }
    }
    if (Rand(rng, 100) < 1) {
      const u32 lsa = rand_addr(Rand(rng, 2) != 0);
      p.Both([&](auto& v) { v.SetLsa(lsa); });
    }
    if (Rand(rng, 100) < 2) {
      const u32 ssa = rand_addr(Rand(rng, 4) != 0);
      p.Both([&](auto& v) { v.SetSsa(ssa); });
    }
    if (Rand(rng, 200) < 1) {
      p.Both([](auto& v) { v.Stop(); });
    }
    if (p.b.Stopped()) {
      // run it stopped for a while (key off, Run()), then start it again or end the trial
      st.stops += stopped_blocks == 0;
      if (++stopped_blocks > 3) {
        if (Rand(rng, 2) == 0) {
          break;
        }
        key_on(Rand(rng, 2) != 0);
        stopped_blocks = 0;
      }
    } else {
      stopped_blocks = 0;
    }
  }
}

// this thread's CPU time in ns (the machine may be busy with other work)
double ThreadNs() {
  timespec ts;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

// A game-like mix: `voices` voices at pitches 0.5 - 2.0 with a fast attack and a held sustain,
// started again at another block when they stop, a few released every 1/8 s; 128-sample chunks at
// the 3DS's ratio, mixed in order as Synth::Tick does. Returns the CPU time it took (ns); the mix
// is stored in `mix`, the samples run added to `run`.
template <typename V, typename O>
double GameMix(const Data& data, int voices, int seconds, std::vector<O>& mix, u64& run) {
  Rng rng(1234);
  const double t_start = ThreadNs();
  std::vector<V> vs(voices);
  auto start = [&](V& v) {
    v.SetSample(const_cast<u16*>(data.words.data()));
    v.SetSsa(8 * Rand(rng, data.blocks()));
    v.SetPitch((u16)(0x800 + Rand(rng, 0x1800)));
    v.SetAsdr1((u16)(0x000F | (Rand(rng, 4) << 10)));
    v.SetAsdr2((u16)(0x1FC0 | (8 + Rand(rng, 8))));
    v.SetVolume((u16)(0x800 + Rand(rng, 0x2000)), (u16)(0x800 + Rand(rng, 0x2000)));
    v.KeyOn();
  };
  for (auto& v : vs) {
    start(v);
  }
  const int chunks = seconds * 32728 / 128;
  mix.assign((size_t)chunks * 128, O{});
  for (int c = 0; c < chunks; c++) {
    O* out = mix.data() + (size_t)c * 128;
    for (auto& v : vs) {
      run += (u64)v.RunBlock(out, 128, kTickRatio);
    }
    for (auto& v : vs) {
      if (v.Stopped()) {
        start(v);
      }
    }
    if (c % 32 == 0) {
      for (int k = 0; k < voices / 8; k++) {
        vs[Rand(rng, voices)].KeyOff();
      }
    }
  }
  return ThreadNs() - t_start;
}

}  // namespace

int main(int argc, char** argv) {
  const int trials = argc > 1 ? atoi(argv[1]) : 6000;
  const u32 seed = argc > 2 ? (u32)atoi(argv[2]) : 20261003;
  Rng rng(seed);
  std::vector<Data> sets;
  sets.push_back(MakeSynthetic(rng, 4096, 25, "synthetic, flags in 25% of the blocks"));
  sets.push_back(MakeSynthetic(rng, 4096, 2, "synthetic, flags in 2% of the blocks"));
  for (int i = 3; i < argc; i++) {
    Data d;
    if (!LoadBank(argv[i], d)) {
      printf("can't read %s\n", argv[i]);
      return 2;
    }
    printf("%s: %u blocks of sample data\n", d.name.c_str(), d.blocks());
    sets.push_back(std::move(d));
  }

  Stats st;
  const auto t0 = std::chrono::steady_clock::now();
  for (int t = 0; t < trials && st.mismatches == 0; t++) {
    const Data& data = sets[Rand(rng, (u32)sets.size())];
    u32 ratio;
    switch (Rand(rng, 5)) {
      case 0:
      case 1:
        ratio = kTickRatio;
        break;
      case 2:
        ratio = 0x10000;
        break;
      default:
        ratio = 0x4000 + Rand(rng, 0x1C001);  // 0.25 to 2.0
    }
    Trial(rng, data, ratio, st);
  }
  const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  printf(
      "%llu trials, %llu blocks, %llu output samples compared (%llu run by the voices), "
      "%llu key ons, %llu stops, %llu mismatches (%.1f s)\n",
      (unsigned long long)st.trials, (unsigned long long)st.blocks, (unsigned long long)st.samples,
      (unsigned long long)st.run, (unsigned long long)st.keyons, (unsigned long long)st.stops,
      (unsigned long long)st.mismatches, secs);

  // the game-like mix, on the real data when there is some; the best of 7 runs each, alternating
  const Data& data = sets.back();
  constexpr int kVoices = 48, kSeconds = 10;
  double best_ref = 1e30, best_new = 1e30;
  bool same = true;
  u64 run_ref = 0, run_new = 0;
  for (int rep = 0; rep < 7; rep++) {
    std::vector<snd_ref::s16Output> mix_ref;
    std::vector<snd::s16Output> mix_new;
    run_ref = run_new = 0;
    best_ref =
        std::min(best_ref, GameMix<snd_ref::Voice>(data, kVoices, kSeconds, mix_ref, run_ref));
    best_new = std::min(best_new, GameMix<snd::Voice>(data, kVoices, kSeconds, mix_new, run_new));
    same = same && run_ref == run_new && mix_ref.size() == mix_new.size() &&
           std::memcmp(mix_ref.data(), mix_new.data(), mix_ref.size() * 4) == 0;
  }
  printf(
      "game-like mix (%s, %d voices, %d s): %s; reference %.2f ns, new %.2f ns per voice sample "
      "(%.2fx)\n",
      data.name.c_str(), kVoices, kSeconds, same ? "identical" : "DIFFERENT",
      best_ref / (double)run_ref, best_new / (double)run_new,
      (best_ref / (double)run_ref) / (best_new / (double)run_new));
  return (st.mismatches == 0 && same) ? 0 : 1;
}
