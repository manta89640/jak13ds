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

  mVoices.remove_if([](std::shared_ptr<Voice>& v) { return v->Dead(); });
  for (auto& v : mVoices) {
#ifdef __3DS__
    // (AI-assisted) The 8 permanent SPU voices (sdshim) are in the list forever; only the VAG
    // stream uses one. A stopped voice decodes and mixes zeros: skip it.
    if (v->Stopped()) {
      continue;
    }
#endif
    out += v->Run();
  }

  out.left = ApplyVolume(out.left, mVolume.left.Get());
  out.right = ApplyVolume(out.right, mVolume.right.Get());

  mVolume.Run();

  return out;
}

#ifdef __3DS__
void Synth::Tick(s16Output* out, int samples) {
  for (int i = 0; i < samples; i++) {
    out[i] = s16Output{};
  }
  mVoices.remove_if([](std::shared_ptr<Voice>& v) { return v->Dead(); });
  for (auto& v : mVoices) {
    Voice* voice = v.get();
    // the same order of (saturating) additions per sample as Tick(); a voice that stops is
    // skipped from then on, like there
    for (int i = 0; i < samples && !voice->Stopped(); i++) {
      out[i] += voice->Run();
    }
  }
  for (int i = 0; i < samples; i++) {
    out[i].left = ApplyVolume(out[i].left, mVolume.left.Get());
    out[i].right = ApplyVolume(out[i].right, mVolume.right.Get());
    mVolume.Run();
  }
}
#endif

void Synth::AddVoice(std::shared_ptr<Voice> voice) {
  mVoices.emplace_front(voice);
}

void Synth::SetMasterVol(u32 volume) {
  mVolume.left.Set(volume);
  mVolume.right.Set(volume);
}
}  // namespace snd
