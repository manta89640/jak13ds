// Copyright: 2021 - 2024, Ziemas
// SPDX-License-Identifier: ISC
#include "synth.h"

#include <stdexcept>

namespace snd {

static s16 ApplyVolume(s16 sample, s32 volume) {
  return (sample * volume) >> 15;
}

s16Output Synth::Tick() {
  s16Output out{};
  TickBlock(&out, 1);
  return out;
}

// Each sample used to be: drop dead voices, sum every voice's Run() into an s16Output (saturating
// after each voice), apply the master volume. Here each voice runs for the whole block into
// saturating 32-bit accumulators, in the same voice order, so every sample is summed exactly as
// before; voices that stop in the middle of the block end there (Voice::RunBlock), as they did
// when they were dropped or skipped at the next sample.
void Synth::TickBlock(s16Output* out, int n) {
  constexpr int kBlock = 256;
  s32 mixL[kBlock];
  s32 mixR[kBlock];
  while (n > 0) {
    const int len = n < kBlock ? n : kBlock;
    for (int i = 0; i < len; i++) {
      mixL[i] = 0;
      mixR[i] = 0;
    }

    mVoices.remove_if([](std::shared_ptr<Voice>& v) { return v->Dead(); });
    for (auto& v : mVoices) {
#ifdef __3DS__
      // (AI-assisted) The 8 permanent SPU voices (sdshim) are in the list forever; only the VAG
      // stream uses one. A stopped voice decodes and mixes zeros: skip it.
      if (v->Stopped()) {
        continue;
      }
#endif
      v->RunBlock(mixL, mixR, len);
    }

    // the master volume register (Get) doesn't change during a block; its sweep state does
    const s32 vol_left = mVolume.left.Get();
    const s32 vol_right = mVolume.right.Get();
    for (int i = 0; i < len; i++) {
      out[i].left = ApplyVolume(static_cast<s16>(mixL[i]), vol_left);
      out[i].right = ApplyVolume(static_cast<s16>(mixR[i]), vol_right);
      mVolume.Run();
    }

    out += len;
    n -= len;
  }
}

void Synth::AddVoice(std::shared_ptr<Voice> voice) {
  mVoices.emplace_front(voice);
}

void Synth::SetMasterVol(u32 volume) {
  mVolume.left.Set(volume);
  mVolume.right.Set(volume);
}
}  // namespace snd
