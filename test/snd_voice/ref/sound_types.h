// (AI-assisted) Frozen copy of game/sound/common/sound_types.h at 446cce66e (before the block-wise
// ADPCM decode), the reference of test/snd_voice/voice_exact.cpp. Only the namespace (snd_ref)
// and the include paths differ.
#pragma once
#include <algorithm>

#include "common/common_types.h"

namespace snd_ref {

const s32 MAX_VOLUME = 0x400;
const u32 SAMPLE_RATE = 48000;  // 48000 samples of 16bits per channel per second
//= 96000 bytes per second in mono, 192000 bytes per second in stereo
struct VolPair {
  s16 left;
  s16 right;
};

struct s16Output {
  s16 left{0}, right{0};

  s16Output& operator+=(const s16Output& rhs) {
    left = static_cast<s16>(std::clamp<s32>(left + rhs.left, INT16_MIN, INT16_MAX));
    right = static_cast<s16>(std::clamp<s32>(right + rhs.right, INT16_MIN, INT16_MAX));
    return *this;
  }
};

}  // namespace snd_ref
