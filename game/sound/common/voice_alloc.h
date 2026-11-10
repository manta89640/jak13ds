#pragma once

#include <algorithm>

#include "common/common_types.h"

namespace snd {

// The SPU2's 48 voices as 989snd hands them to tones. A tone takes a free voice of its volume
// group's range. Without one it takes over the voice of that range that matters least (one in its
// release before a held one, then the lowest tone priority, then the oldest), unless that voice is
// held by a tone of a higher priority: then the new tone doesn't play.
class VoiceAllocator {
 public:
  static constexpr int kVoices = 48;
  static constexpr int kGroups = 32;

  VoiceAllocator() {
    for (auto& m : mMax) {
      m = kVoices - 1;
    }
  }

  void SetGroupRange(int group, int min, int max) {
    if (group < 0 || group >= kGroups) {
      return;
    }
    mMin[group] = (s8)std::clamp(min, 0, kVoices - 1);
    mMax[group] = (s8)std::clamp(max, 0, kVoices - 1);
  }

  // snd_ExternVoiceAlloc: a voice of the group's range that no tone gets from now on. -1: none.
  int AllocExternal(int group) {
    for (int i = Min(group); i <= Max(group); i++) {
      if (!mUsed[i] && !mExternal[i]) {
        mExternal[i] = true;
        return i;
      }
    }
    return -1;
  }

  bool Used(int voice) const { return mUsed[voice]; }
  void Free(int voice) { mUsed[voice] = false; }

  // A voice for a tone. keyed_off: bit i set while voice i is in its release. -1: the tone doesn't
  // play. *stolen: the voice was in use (the caller stops what it played).
  int Allocate(int group, int priority, u64 keyed_off, bool* stolen) {
    *stolen = false;
    int victim = -1;
    for (int i = Min(group); i <= Max(group); i++) {
      if (mExternal[i]) {
        continue;
      }
      if (!mUsed[i]) {
        Take(i, priority);
        return i;
      }
      if (victim < 0 || MattersLess(i, victim, keyed_off)) {
        victim = i;
      }
    }
    if (victim < 0) {
      return -1;
    }
    if (!((keyed_off >> victim) & 1) && mPriority[victim] > priority) {
      return -1;
    }
    *stolen = true;
    Take(victim, priority);
    return victim;
  }

 private:
  int Min(int group) const { return group >= 0 && group < kGroups ? mMin[group] : 0; }
  int Max(int group) const { return group >= 0 && group < kGroups ? mMax[group] : kVoices - 1; }

  void Take(int voice, int priority) {
    mUsed[voice] = true;
    mPriority[voice] = priority;
    mStart[voice] = mNextStart++;
  }

  bool MattersLess(int a, int b, u64 keyed_off) const {
    const bool off_a = (keyed_off >> a) & 1, off_b = (keyed_off >> b) & 1;
    if (off_a != off_b) {
      return off_a;
    }
    if (mPriority[a] != mPriority[b]) {
      return mPriority[a] < mPriority[b];
    }
    return mStart[a] < mStart[b];
  }

  s8 mMin[kGroups]{};
  s8 mMax[kGroups]{};
  bool mUsed[kVoices]{};
  bool mExternal[kVoices]{};
  int mPriority[kVoices]{};
  u32 mStart[kVoices]{};
  u32 mNextStart{0};
};

}  // namespace snd
