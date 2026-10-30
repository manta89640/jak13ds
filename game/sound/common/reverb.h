#pragma once
#include <vector>

#include "sound_types.h"

#include "common/common_types.h"

namespace snd {

// The SPU2 effect unit: libsd's reverb presets (sceSdSetEffectAttr) run as the SPU2 runs them,
// one side per output sample with the 39-tap half-band filters around it. At an output rate
// other than 48 kHz the work area addresses are scaled so the delays keep their length in time.
class Reverb {
 public:
  void SetOutputRate(u32 rate) { mRate = rate; }
  // libsd effect mode 0-9 (0: off); the same mode again keeps the work area as it is
  void SetMode(u32 mode);
  void SetDepth(s16 left, s16 right) {
    mDepthL = left;
    mDepthR = right;
  }
  bool Active() const { return mMode != 0 && (mDepthL != 0 || mDepthR != 0); }
  // out[i] += the effect's output times its depth for the voices' wet output wet[i]
  void Process(const s16Output* wet, s16Output* out, int n);

 private:
  struct Side {
    u32 same_src, same_dst, same_prv;
    u32 diff_src, diff_dst, diff_prv;
    u32 comb[4];
    u32 apf1_src, apf1_dst, apf2_src, apf2_dst;
    s32 in_vol;
  };
  void Clear();
  struct Run;
  template <int R>
  static void Step(Run& run, const Side& side, const s16Output& wet, s16Output& out);

  u32 mRate{48000};
  u32 mMode{0};
  s16 mDepthL{0}, mDepthR{0};
  bool mWasActive{false};

  s32 mIirVol{0}, mWallVol{0}, mApf1Vol{0}, mApf2Vol{0};
  s32 mCombVol[4]{};
  Side mSide[2]{};

  std::vector<s16> mWork;
  u32 mLen{0};
  u32 mPos{0};  // buffer address, one step per pair of output samples
  u32 mCycle{0};
  // the filters' inputs, each twice so 39 in a row can be read from any of the 64
  alignas(8) s16 mDown[2][128]{};
  alignas(8) s16 mUp[2][128]{};
};

}  // namespace snd
