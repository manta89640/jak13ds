// Copyright: 2021 - 2024, Ziemas
// SPDX-License-Identifier: ISC
#pragma once
#include <forward_list>
#include <memory>
#include <unordered_map>
#include <vector>

#include "envelope.h"
#include "sound_types.h"
#include "voice.h"

#include "common/common_types.h"

namespace snd {
struct SpuVolume {
  /*   0 */ s16 left;
  /*   2 */ s16 right;
};

class Synth {
 public:
  Synth() {
    mVolume.left.Set(0x3FFF);
    mVolume.right.Set(0x3FFF);
  }

  s16Output Tick();
#ifdef __3DS__
  // (AI-assisted) `samples` outputs at once, the same as calling Tick() that many times (nothing
  // else changes the voices meanwhile): one voice at a time over the block. ratio: SPU ticks per
  // output sample, 16.16 (see Voice::RunBlock).
  void Tick(s16Output* out, int samples, u32 ratio = 0x10000);
  // (AI-assisted) voices that aren't stopped (statistics)
  int VoiceCount() const {
    int n = 0;
    for (const auto& v : mVoices) {
      n += !v->Stopped();
    }
    return n;
  }
#endif
  void AddVoice(std::shared_ptr<Voice> voice);
  void SetMasterVol(u32 volume);

 private:
  std::forward_list<std::shared_ptr<Voice>> mVoices;

  VolumePair mVolume{};
};
}  // namespace snd
