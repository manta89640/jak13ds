#pragma once

#include <algorithm>

#include "common/common_types.h"

namespace snd {

// How many voices the mixer mixes audibly (Synth::BeginBlock mutes the quietest beyond it), from
// the time its buffers take to make against the time they play. No limit while a buffer takes
// less than kHigh of its duration. Above that the limit is cut to the count that should take
// kTarget, so the mixer is never slower than the output for more than a buffer or two. It goes
// back up fast while buffers take less than kLow, and one voice at a time in between, up to just
// below the count that was too much last time.
class MixBudget {
 public:
  static constexpr int kNoLimit = 96;
  static constexpr int kMin = 8;
  // shares of the buffer's duration, in 1/256
  static constexpr u32 kLow = 179;      // 70%
  static constexpr u32 kTarget = 200;   // 78%
  static constexpr u32 kHigh = 218;     // 85%, the average of two buffers
  static constexpr u32 kPanic = 320;    // 125%, one buffer
  static constexpr u32 kCatchUp = 128;  // 50%: the aim once the output ran dry
  static constexpr int kCreepBuffers = 16;
  static constexpr int kCeilingCreeps = 32;

  int Limit() const { return mLimit; }

  // After a buffer. busy_us: the time it took to make, buffer_us: the time it plays, late: the
  // output had run dry before it came, audible: the most voices mixed audibly at once in it.
  // True: the limit was cut.
  bool OnBuffer(u32 busy_us, u32 buffer_us, bool late, int audible) {
    const u32 load = (u32)std::min<u64>((u64)busy_us * 256 / buffer_us, 4096);
    const u32 avg = (load + mLastLoad) / 2;
    mLastLoad = load;
    u32 over = 0, aim = kTarget;
    if (late && load > kCatchUp) {
      over = std::max(load, avg);
      aim = kCatchUp;
    } else if (load > kPanic) {
      over = load;
    } else if (avg > kHigh) {
      over = avg;
    }
    if (over != 0) {
      const int base = std::min(mLimit, std::max(audible, kMin));
      const int cut = std::min((int)((u64)base * aim / over), base - 1);
      mLimit = std::max(cut, kMin);
      mCeiling = base;
      mCeilingAge = 0;
      mCreep = 0;
      mLastLoad = aim;
      return true;
    }
    if (mLimit >= kNoLimit) {
      return false;
    }
    if (load < kLow && avg < kLow) {
      mCeiling = kNoLimit;
      mLimit = std::min(mLimit + 1 + mLimit / 16, (int)kNoLimit);
    } else if (++mCreep >= kCreepBuffers) {
      mCreep = 0;
      if (++mCeilingAge > kCeilingCreeps) {
        mCeiling = kNoLimit;
      }
      if (mLimit + 1 < mCeiling) {
        mLimit++;
      }
    }
    return false;
  }

 private:
  int mLimit{kNoLimit};
  int mCeiling{kNoLimit};
  int mCeilingAge{0};
  int mCreep{0};
  u32 mLastLoad{0};
};

}  // namespace snd
