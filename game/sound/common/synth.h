// Copyright: 2021 - 2024, Ziemas
// SPDX-License-Identifier: ISC
#pragma once
#include <forward_list>
#include <memory>
#include <unordered_map>
#include <vector>

#include "envelope.h"
#include "reverb.h"
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
  // (AI-assisted) `samples` outputs at once, the same as calling Tick() that many times (nothing
  // else changes the voices meanwhile): one voice at a time over the block. ratio: SPU ticks per
  // output sample, 16.16 (see Voice::RunBlock).
  // In three steps, so the mixer runs the voices without holding its lock: BeginBlock() and
  // EndBlock() under the lock, Mix() between them without it. Meanwhile register writes to the
  // voices, AddVoice and SetMasterVol are kept and EndBlock() applies them (Voice::SetBusy).
  // audible: the most voices mixed audibly in the block. With more running, the quietest of the
  // tones' voices are muted for it (Voice::SetMuted): they move on unheard at a fraction of the
  // cost. The overlord's stream voices are never muted.
  static constexpr int kAllAudible = 1 << 30;
  void BeginBlock(int audible = kAllAudible);
  void Mix(s16Output* out, int samples, u32 ratio);
  void EndBlock();
  // (AI-assisted) voices that aren't stopped (statistics)
  int VoiceCount() const {
    int n = 0;
    for (const auto& v : mVoices) {
      n += !v->Stopped();
    }
    return n;
  }
  // as of the last BeginBlock(): the voices running, and the ones of them it muted
  int RunningCount() const { return mRunning; }
  int MutedCount() const { return mMuted; }
  void AddVoice(std::shared_ptr<Voice> voice);
  void SetMasterVol(u32 volume);
  void SetOutputRate(u32 rate) { mReverb.SetOutputRate(rate); }
  // the SPU2 effect unit (both cores' effect settings are the same in the games): libsd mode
  // and depth (EVOL)
  void SetReverbMode(u32 mode);
  void SetReverbDepth(s16 left, s16 right);

 private:
  void MuteQuietest(int audible);

  std::forward_list<std::shared_ptr<Voice>> mVoices;
  bool mBusy{false};
  std::vector<std::shared_ptr<Voice>> mPendingVoices;
  u32 mPendingMasterVol{0};
  bool mMasterVolPending{false};
  bool mReverbModePending{false}, mReverbDepthPending{false};
  u32 mPendingReverbMode{0};
  s16 mPendingDepthL{0}, mPendingDepthR{0};
  struct Rank {
    u32 loudness;
    Voice* voice;
  };
  std::vector<Rank> mRank;
  int mRunning{0};
  int mMuted{0};
  Reverb mReverb;
  std::vector<s16Output> mWet;

  VolumePair mVolume{};
};
}  // namespace snd
