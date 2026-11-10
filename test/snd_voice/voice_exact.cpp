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
 * The voice limits: muted voices (Voice::SetMuted) against the reference for where they are
 * (address, envelope, end) with nothing mixed; Synth::BeginBlock / Mix / EndBlock against the
 * sum of reference voices, without a limit (every sample) and with one (the quietest muted, the
 * others exact); the SPU voice allocator's rule (voice_alloc.h) and the mixer's voice budget
 * (mix_budget.h) on made-up loads.
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

#include "game/sound/common/mix_budget.h"
#include "game/sound/common/synth.h"
#include "game/sound/common/voice.h"
#include "game/sound/common/voice_alloc.h"
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
  int last_op = 0;  // 1: a key on, 2: a stop (what the published state shows while busy)
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
    last_op = 1;
  };
  p.Both([&](auto& v) { v.SetSample(const_cast<u16*>(data.words.data())); });
  key_on(true);
  st.trials++;

  std::vector<snd::s16Output> oa;
  std::vector<snd_ref::s16Output> ob;
  const int blocks = 20 + (int)Rand(rng, 500);
  int stopped_blocks = 0;
  const bool deferred = Rand(rng, 2) != 0;
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
    if (deferred) {
      p.a.SetBusy(true);
    }
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

    // register writes and keys between blocks, the same on both. In half the trials the new voice
    // is busy meanwhile (the 3DS mixer's block, Voice::SetBusy): it keeps them and applies them
    // after, which must give the same voice, and publish the NAX and stopped state the reference
    // has after each write.
    last_op = 0;
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
      last_op = 2;
    }
    if (deferred) {
      if ((last_op == 1 && (p.a.GetPublishedNax() != p.b.GetNax() || p.a.StoppedPublished())) ||
          (last_op == 2 && !p.a.StoppedPublished())) {
        printf("MISMATCH (deferred writes): published nax %u, stopped %d; reference %u, %d\n",
               p.a.GetPublishedNax(), (int)p.a.StoppedPublished(), p.b.GetNax(),
               (int)p.b.Stopped());
        st.mismatches++;
        return;
      }
      p.a.SetBusy(false);
      if (p.a.GetPublishedNax() != p.b.GetNax() || p.a.StoppedPublished() != p.b.Stopped()) {
        printf("MISMATCH (deferred writes applied): published nax %u, stopped %d; reference %u, %d\n",
               p.a.GetPublishedNax(), (int)p.a.StoppedPublished(), p.b.GetNax(),
               (int)p.b.Stopped());
        st.mismatches++;
        return;
      }
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

// A muted voice against the reference, which plays: the same samples run, NAX, envelope level and
// stopped flag after every block, and nothing added to the output while it is muted. Mutes change
// between blocks. The output is compared again once a key on came while it wasn't muted (the
// decoder starts over there).
void MutedTrial(Rng& rng, const Data& data, u32 ratio, Stats& st) {
  Pair p;
  const u32 nblocks = data.blocks();
  auto key_on = [&]() {
    const u32 ssa = 8 * Rand(rng, nblocks) + (Rand(rng, 5) == 0 ? Rand(rng, 8) : 0);
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
      v.KeyOn();
    });
    st.keyons++;
  };
  p.Both([&](auto& v) { v.SetSample(const_cast<u16*>(data.words.data())); });
  bool muted = Rand(rng, 4) != 0;
  p.a.SetMuted(muted);
  key_on();
  bool exact = !muted;
  st.trials++;

  std::vector<snd::s16Output> oa, bg;
  std::vector<snd_ref::s16Output> ob;
  const int blocks = 20 + (int)Rand(rng, 300);
  int stopped_blocks = 0;
  for (int blk = 0; blk < blocks; blk++) {
    const int n = Rand(rng, 4) == 0 ? 1 + (int)Rand(rng, 8) : 1 + (int)Rand(rng, 256);
    oa.resize(n);
    bg.resize(n);
    ob.resize(n);
    for (int k = 0; k < n; k++) {
      const s16 l = (s16)(Rand(rng, 4000) - 2000), r = (s16)(Rand(rng, 4000) - 2000);
      oa[k] = {l, r};
      bg[k] = {l, r};
      ob[k] = {l, r};
    }
    const int ra = p.a.RunBlock(oa.data(), n, ratio);
    const int rb = p.b.RunBlock(ob.data(), n, ratio);
    st.blocks++;
    st.samples += (u64)n;
    st.run += (u64)rb;
    bool same = ra == rb && p.a.GetNax() == p.b.GetNax() && p.a.Stopped() == p.b.Stopped() &&
                p.a.GetEnvx() == p.b.GetEnvx();
    for (int k = 0; k < n && same; k++) {
      if (muted) {
        same = oa[k].left == bg[k].left && oa[k].right == bg[k].right;
      } else if (exact) {
        same = oa[k].left == ob[k].left && oa[k].right == ob[k].right;
      }
    }
    if (!same) {
      if (st.mismatches < 10) {
        printf(
            "MISMATCH (muted voice: %s, %s, ratio %05x, block %d, n %d): ran %d / %d, nax %u / "
            "%u, stopped %d / %d, envx %d / %d\n",
            muted ? "muted" : exact ? "heard, exact" : "heard", data.name.c_str(), ratio, blk, n,
            ra, rb, p.a.GetNax(), p.b.GetNax(), (int)p.a.Stopped(), (int)p.b.Stopped(),
            p.a.GetEnvx(), p.b.GetEnvx());
      }
      st.mismatches++;
      return;
    }
    if (Rand(rng, 100) < 3) {
      p.Both([](auto& v) { v.KeyOff(); });
    }
    if (Rand(rng, 100) < 4) {
      const u16 pitch = RandPitch(rng);
      p.Both([&](auto& v) { v.SetPitch(pitch); });
    }
    if (Rand(rng, 100) < 3) {
      const u16 vl = RandVolume(rng), vr = RandVolume(rng);
      p.Both([&](auto& v) { v.SetVolume(vl, vr); });
    }
    if (Rand(rng, 100) < 5) {
      muted = !muted;
      p.a.SetMuted(muted);
      exact = false;
    }
    bool restart = Rand(rng, 100) < 2;
    if (p.b.Stopped()) {
      st.stops += stopped_blocks == 0;
      if (++stopped_blocks > 2) {
        if (Rand(rng, 2) == 0) {
          break;
        }
        restart = true;
      }
    }
    if (restart) {
      key_on();
      exact = !muted;
      stopped_blocks = 0;
    }
  }
}

// Synth::BeginBlock / Mix / EndBlock (the 3DS mixer's block) with `voices` held voices of
// different volumes against reference voices mixed in the synth's order: every sample without a
// limit; with a limit, that many voices heard (the loudest: the output is their sum), the others
// muted, all of them where the reference is; then without a limit again (the muted ones are
// heard again, only where they are is compared).
bool SynthTrial(Rng& rng, const Data& data, int voices, int limit) {
  snd::Synth synth;
  std::vector<std::shared_ptr<snd::Voice>> vs;
  std::vector<snd_ref::Voice> refs(voices);
  std::vector<char> ever_muted(voices, 0);
  for (int k = 0; k < voices; k++) {
    auto v = std::make_shared<snd::Voice>();
    const u32 ssa = 8 * Rand(rng, data.blocks());
    const u16 pitch = (u16)(0x800 + Rand(rng, 0x1800));
    const u16 a1 = (u16)(0x000F | (Rand(rng, 4) << 10));
    const u16 a2 = (u16)(0x1FC0 | (8 + Rand(rng, 8)));
    const u16 vol = (u16)(0x200 + 0x40 * k);
    auto setup = [&](auto& x) {
      x.SetSample(const_cast<u16*>(data.words.data()));
      x.SetSsa(ssa);
      x.SetPitch(pitch);
      x.SetAsdr1(a1);
      x.SetAsdr2(a2);
      x.SetVolume(vol, (u16)(vol / 2));
      x.KeyOn();
    };
    setup(*v);
    setup(refs[k]);
    synth.AddVoice(v);
    vs.push_back(v);
  }
  constexpr int kBlocks = 60, kSamples = 136;
  std::vector<snd::s16Output> out(kSamples);
  std::vector<snd_ref::s16Output> ref(kSamples), unheard(kSamples);
  int compared = 0;
  for (int blk = 0; blk < kBlocks; blk++) {
    const bool limited = blk >= 20 && blk < 40;
    synth.BeginBlock(limited ? limit : snd::Synth::kAllAudible);
    const int muted_now = synth.MutedCount();
    if (synth.RunningCount() != voices || muted_now != (limited ? voices - limit : 0)) {
      printf("MISMATCH (synth, block %d): %d voices running, %d muted (limit %d of %d)\n", blk,
             synth.RunningCount(), muted_now, limited ? limit : voices, voices);
      return false;
    }
    bool exact = true;
    for (int k = 0; k < voices; k++) {
      const bool muted = vs[k]->Muted();
      // the loudest stay: the voices' volumes go up with k
      if (limited && muted != (k < voices - limit)) {
        printf("MISMATCH (synth, block %d): voice %d of %d %s (limit %d)\n", blk, k, voices,
               muted ? "muted" : "heard", limit);
        return false;
      }
      exact = exact && (muted || !ever_muted[k]);
      ever_muted[k] = ever_muted[k] || muted;
    }
    synth.Mix(out.data(), kSamples, kTickRatio);
    synth.EndBlock();
    std::fill(ref.begin(), ref.end(), snd_ref::s16Output{});
    // the synth's list has the last voice added first
    for (int k = voices - 1; k >= 0; k--) {
      refs[k].RunBlock(vs[k]->Muted() ? unheard.data() : ref.data(), kSamples, kTickRatio);
      if (vs[k]->GetNax() != refs[k].GetNax() || vs[k]->GetEnvx() != refs[k].GetEnvx() ||
          vs[k]->Stopped() != refs[k].Stopped()) {
        printf("MISMATCH (synth, block %d, voice %d): nax %u / %u, envx %d / %d\n", blk, k,
               vs[k]->GetNax(), refs[k].GetNax(), vs[k]->GetEnvx(), refs[k].GetEnvx());
        return false;
      }
    }
    if (!exact) {
      continue;
    }
    compared++;
    for (int i = 0; i < kSamples; i++) {
      // the synth's master volume (0x3FFF)
      const s16 l = (s16)((ref[i].left * 0x3FFF) >> 15), r = (s16)((ref[i].right * 0x3FFF) >> 15);
      if (out[i].left != l || out[i].right != r) {
        printf("MISMATCH (synth, block %d, sample %d): %d,%d / %d,%d\n", blk, i, out[i].left,
               out[i].right, l, r);
        return false;
      }
    }
  }
  // all of them when no voice was ever muted, else the blocks up to the end of the limit
  const int expected = limit < voices ? 40 : kBlocks;
  if (compared != expected) {
    printf("MISMATCH (synth): %d blocks' output compared, %d expected\n", compared, expected);
    return false;
  }
  return true;
}

// 989snd's voice allocation as voice_alloc.h has it
bool AllocatorChecks() {
  int bad = 0;
  auto expect = [&](bool ok, const char* what) {
    if (!ok) {
      printf("MISMATCH (voice allocator): %s\n", what);
      bad++;
    }
  };
  bool stolen = false;
  {
    snd::VoiceAllocator a;
    bool all_free = true;
    for (int i = 0; i < 48; i++) {
      all_free = all_free && a.Allocate(0, 10, 0, &stolen) == i && !stolen;
    }
    expect(all_free, "48 tones get the 48 voices in order");
    expect(a.Allocate(0, 9, 0, &stolen) == -1 && !stolen, "a lower priority gets no held voice");
    expect(a.Allocate(0, 10, 0, &stolen) == 0 && stolen, "the same priority takes the oldest");
    expect(a.Allocate(0, 10, 0, &stolen) == 1 && stolen, "then the next oldest");
    expect(a.Allocate(0, 9, 1ull << 7, &stolen) == 7 && stolen,
           "a lower priority takes the voice in its release");
    expect(a.Allocate(0, 11, 0, &stolen) == 7 && stolen,
           "a higher priority takes the lowest priority (voice 7, at 9)");
    expect(a.Allocate(0, 12, (1ull << 30) | (1ull << 40), &stolen) == 30 && stolen,
           "of two in release the older (same priority)");
    a.Free(20);
    expect(a.Allocate(0, 0, 0, &stolen) == 20 && !stolen, "a freed voice is free");
    expect(a.Allocate(0, 5, 1ull << 30, &stolen) == 30 && stolen,
           "a voice in release before the held ones, whatever its priority (12)");
  }
  {
    // Jak 1: one stream voice, the music's and dialogue's groups on voices 0-15, the others 16-47
    snd::VoiceAllocator a;
    expect(a.AllocExternal(2) == 0, "the first external voice is 0");
    for (int g = 0; g < 8; g++) {
      a.SetGroupRange(g, 0x10, 0x2f);
    }
    a.SetGroupRange(1, 0, 0xf);
    a.SetGroupRange(2, 0, 0xf);
    bool music = true, effects = true;
    for (int i = 1; i < 16; i++) {
      music = music && a.Allocate(1, 5, 0, &stolen) == i && !stolen;
    }
    expect(music, "group 1 gets voices 1-15");
    expect(a.Allocate(1, 5, 0, &stolen) == 1 && stolen, "its 16th tone takes its oldest voice");
    for (int i = 16; i < 48; i++) {
      effects = effects && a.Allocate(0, 5, 0, &stolen) == i && !stolen;
    }
    expect(effects, "group 0 gets voices 16-47");
    expect(a.Allocate(3, 4, 0, &stolen) == -1, "group 3 shares them: nothing for a lower priority");
    expect(a.Allocate(3, 5, 0, &stolen) == 16 && stolen, "group 3 takes group 0's oldest");
  }
  {
    // Jak II: 4 stream voices, 44 for the tones
    snd::VoiceAllocator a;
    bool ext = true, tones = true;
    for (int i = 0; i < 4; i++) {
      ext = ext && a.AllocExternal(2) == i;
    }
    expect(ext, "4 external voices are 0-3");
    for (int i = 4; i < 48; i++) {
      tones = tones && a.Allocate(i % 14, 5, 0, &stolen) == i && !stolen;
    }
    expect(tones, "44 tones get voices 4-47");
    expect(a.Allocate(40, 5, 0, &stolen) == 4 && stolen, "the 45th takes the oldest, never 0-3");
  }
  return bad == 0;
}

// Voice::KeyedOff(), also for keys given while the mixer runs the voice
bool KeyedOffChecks(const Data& data) {
  snd::Voice v;
  v.SetSample(const_cast<u16*>(data.words.data()));
  v.KeyOn();
  bool ok = !v.KeyedOff();
  v.KeyOff();
  ok = ok && v.KeyedOff();
  v.SetBusy(true);
  v.KeyOn();
  ok = ok && !v.KeyedOff();
  v.KeyOff();
  ok = ok && v.KeyedOff();
  v.SetBusy(false);
  ok = ok && v.KeyedOff();
  v.KeyOn();
  ok = ok && !v.KeyedOff();
  if (!ok) {
    printf("MISMATCH (keyed off flag)\n");
  }
  return ok;
}

// The mixer's voice budget on a made-up mixer: a buffer takes fixed + per_voice for each voice
// heard + per_voice / 6 for each muted one (1/256 of its duration).
struct BudgetSim {
  snd::MixBudget budget;
  int cuts = 0;
  int high = 0;  // buffers that took more than kHigh
  void Run(int buffers, int voices, u32 fixed, u32 per_voice) {
    for (int i = 0; i < buffers; i++) {
      const int heard = std::min(voices, budget.Limit());
      const u32 load = fixed + per_voice * (u32)heard + per_voice * (u32)(voices - heard) / 6;
      high += load > snd::MixBudget::kHigh;
      cuts += budget.OnBuffer(load * 100, 25600, false, heard);
    }
  }
};

bool BudgetChecks() {
  int bad = 0;
  auto expect = [&](bool ok, const char* what) {
    if (!ok) {
      printf("MISMATCH (voice budget): %s\n", what);
      bad++;
    }
  };
  using B = snd::MixBudget;
  {
    // 44 voices + 26 fixed: 62% and 79% of real time, below kHigh
    BudgetSim s;
    s.Run(2000, 44, 26, 3);
    s.Run(2000, 44, 26, 4);
    expect(s.cuts == 0 && s.high == 0 && s.budget.Limit() == B::kNoLimit,
           "no limit while the mixer keeps up");
  }
  {
    // 44 voices at 6 (2.3%) each + 38 fixed: 118% of real time; 27 heard and 17 muted: 85%
    BudgetSim s;
    s.Run(8, 44, 38, 6);
    expect(s.cuts >= 1 && s.high <= 4, "an overload is cut within 4 buffers");
    expect(s.budget.Limit() >= B::kMin && s.budget.Limit() < 44, "to a limit below the voices");
    const int cuts = s.cuts, high = s.high;
    s.Run(4000, 44, 38, 6);
    expect(s.high - high <= 40, "and stays cut while it lasts");
    expect(s.cuts - cuts <= 16, "without cutting again more than once in a while");
    expect(s.budget.Limit() >= 25 && s.budget.Limit() <= 28, "at about the count that fits");
    s.Run(400, 10, 38, 6);
    expect(s.budget.Limit() == B::kNoLimit, "the limit goes away when the load does");
  }
  {
    // nothing fits: the limit stops at kMin
    BudgetSim s;
    s.Run(200, 44, 300, 6);
    expect(s.budget.Limit() == B::kMin, "the limit never goes below kMin");
  }
  {
    // a late buffer (the output ran dry): no cut while mixing is fast, a deep one when it is slow
    snd::MixBudget b;
    expect(!b.OnBuffer(5000, 25600, true, 30) && b.Limit() == B::kNoLimit,
           "a late buffer that was quick to mix cuts nothing");
    expect(b.OnBuffer(23040, 25600, true, 30) && b.Limit() <= 17 && b.Limit() >= B::kMin,
           "a late buffer that took 90% cuts to about half");
  }
  return bad == 0;
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

  // muted voices
  Stats mst;
  for (int t = 0; t < trials / 4 && mst.mismatches == 0; t++) {
    const Data& data = sets[Rand(rng, (u32)sets.size())];
    MutedTrial(rng, data, Rand(rng, 4) == 0 ? 0x10000 : kTickRatio, mst);
  }
  printf("muted voices: %llu trials, %llu blocks, %llu output samples, %llu mismatches\n",
         (unsigned long long)mst.trials, (unsigned long long)mst.blocks,
         (unsigned long long)mst.samples, (unsigned long long)mst.mismatches);

  // the synth's block with and without a voice limit (voices that never end: no loop flags)
  const Data plain = MakeSynthetic(rng, 4096, 0, "synthetic, no flags");
  const bool synth_ok = SynthTrial(rng, plain, 48, 48) && SynthTrial(rng, plain, 48, 30) &&
                        SynthTrial(rng, plain, 12, 1) && SynthTrial(rng, plain, 20, 0);
  const bool alloc_ok = AllocatorChecks();
  const bool keyed_ok = KeyedOffChecks(plain);
  const bool budget_ok = BudgetChecks();
  printf("synth blocks %s, voice allocator %s, keyed off flag %s, voice budget %s\n",
         synth_ok ? "ok" : "FAILED", alloc_ok ? "ok" : "FAILED", keyed_ok ? "ok" : "FAILED",
         budget_ok ? "ok" : "FAILED");
  const bool limits_ok = mst.mismatches == 0 && synth_ok && alloc_ok && keyed_ok && budget_ok;

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
  return (st.mismatches == 0 && same && limits_ok) ? 0 : 1;
}
