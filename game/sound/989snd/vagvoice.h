// Copyright: 2021 - 2024, Ziemas
// SPDX-License-Identifier: ISC
#pragma once
#include <array>
#include <list>
#include <memory>

#include "common/common_types.h"

#include "game/sound/common/synth.h"
#include "game/sound/common/voice.h"
#include "game/sound/common/voice_alloc.h"

namespace snd {

enum class toneflag : u16 {
  out_reverb = 1,
  out_dry = 0x10,
};

struct Tone {
  s8 Priority;
  s8 Vol;
  s8 CenterNote;
  s8 CenterFine;
  s16 Pan;
  s8 MapLow;
  s8 MapHigh;
  s8 PBLow;
  s8 PBHigh;
  u16 ADSR1;
  u16 ADSR2;
  u16 Flags;
  // (AI-assisted) "play as Keira" (3DS easter egg): the pitch added to this tone (1/128 semitone)
  // when a sound the player plays as Keira starts it: set on Jak's voice in the common bank
  // (snd_SetKeiraVoice), 0 for every other tone
  s16 KeiraPm{0};
  u8* Sample;
};

class VagVoice : public Voice {
 public:
  VagVoice(Tone& t) : tone(t) {}
  Tone& tone;
  u8 group{0};
  VolPair basevol{};
  s32 current_pm{0};
  s32 current_pb{0};
  s32 start_note{0};
  s32 start_fine{0};
  bool paused{false};
};

class VoiceManager {
 public:
  VoiceManager(Synth& synth);
  void StartTone(std::shared_ptr<VagVoice> voice);
  void Pause(std::shared_ptr<VagVoice> voice);
  void Unpause(std::shared_ptr<VagVoice> voice);
  void SetPanTable(VolPair* table) { mPanTable = table; };

  VolPair MakeVolume(int vol1, int pan1, int vol2, int pan2, int vol3, int pan3);

  // This is super silly, but it's what 989snd does
  VolPair MakeVolumeB(int sound_vol,
                      int velocity_volume,
                      int pan,
                      int prog_vol,
                      int prog_pan,
                      int tone_vol,
                      int tone_pan);

  void SetMasterVol(u8 group, s32 volume);
  void SetPlaybackMode(s32 mode) { mStereoOrMono = mode; }
  s16 AdjustVolToGroup(s16 involume, int group);

  // The SPU2's voices (voice_alloc.h): with the limit on, a tone plays only if 989snd's
  // allocator has a voice for it, which may end another tone. Off: every tone gets a voice.
  void SetVoiceLimit(bool on) { mLimitVoices = on; }
  void SetGroupVoiceRange(int group, int min, int max) { mAlloc.SetGroupRange(group, min, max); }
  int ExternVoiceAlloc(int group) { return mAlloc.AllocExternal(group); }
  // tones that took over a playing voice, and tones that got none, so far
  u32 StolenVoices() const { return mStolen; }
  u32 RefusedTones() const { return mRefused; }

 private:
  bool TakeVoice(const std::shared_ptr<VagVoice>& voice);

  Synth& mSynth;

  VoiceAllocator mAlloc;
  std::array<std::weak_ptr<VagVoice>, VoiceAllocator::kVoices> mAllocVoices;
  bool mLimitVoices{false};
  u32 mStolen{0};
  u32 mRefused{0};

  std::list<std::weak_ptr<VagVoice>> mVoices;
  void CleanVoices() {
    mVoices.remove_if([](auto& v) { return v.expired(); });
  }

  s32 mStereoOrMono{0};

  std::array<s32, 32> mMasterVol;
  std::array<s32, 32> mGroupDuck;

  const VolPair* mPanTable{nullptr};
};

}  // namespace snd
