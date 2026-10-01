// Copyright: 2021 - 2024, Ziemas
// SPDX-License-Identifier: ISC
#include "voice.h"

#include <algorithm>
#include <array>
#include <cstring>
#if defined(__ARM_FEATURE_SIMD32) || defined(__ARM_FEATURE_SAT)
#include <arm_acle.h>
#endif

namespace snd {
#include "interp_table.inc"

// Integer math version of ps-adpcm coefs. (AI-assisted) The filter field has 3 bits: 5-7 (invalid)
// as 0 instead of reading past the table.
static constexpr std::array<std::array<s16, 2>, 8> adpcm_coefs = {{
    {0, 0},
    {60, 0},
    {115, -52},
    {98, -55},
    {122, -60},
    {0, 0},
    {0, 0},
    {0, 0},
}};

void Voice::DecodeSamples() {
  // This doesn't exactly match the real behaviour,
  // it seems to initially decode a bigger chunk
  // and then decode more data after a bit has drained
  if (mDecodeBuf.Size() >= 16) {
    // sufficient data buffered
    return;
  }

  s16* dst = mDecodeBuf.Append(4);
  // Skip decoding for stopped voices.
  if (mADSR.GetPhase() == ADSR::Phase::Stopped) {
    dst[0] = dst[1] = dst[2] = dst[3] = 0;
  } else {
    u32 data = mSample[mNAX];
    // (AI-assisted) the block's shift and filter as UpdateBlockHeader found them, the history in
    // registers
    const int shift = mDecShift;
    const s32 coef1 = mDecCoef1, coef2 = mDecCoef2;
    s32 hist1 = mDecodeHist1, hist2 = mDecodeHist2;
    for (int i = 0; i < 4; i++) {
      s32 sample = (s16)((data & 0xF) << 12);
      sample >>= shift;

      // TODO do the right thing for invalid shift/filter values
      sample += (coef1 * hist1) >> 6;
      sample += (coef2 * hist2) >> 6;

      // We do get overflow here otherwise, should we?
#if defined(__ARM_FEATURE_SAT)
      sample = __ssat(sample, 16);  // (AI-assisted) the same clamp in one instruction
#else
      sample = std::clamp<s32>(sample, INT16_MIN, INT16_MAX);
#endif

      hist2 = hist1;
      hist1 = sample;

      dst[i] = static_cast<s16>(sample);
      data >>= 4;
    }
    mDecodeHist1 = static_cast<s16>(hist1);
    mDecodeHist2 = static_cast<s16>(hist2);
  }

  mNAX++;

  if ((mNAX & 0x7) == 0) {
    if (mCurHeader.LoopEnd.get()) {
      mNAX = mLSA;
      mENDX = true;

      if (!mCurHeader.LoopRepeat.get()) {
        // Need to inhibit stopping here in noise is on
        // seems to result in the right thing but would like to verify
        if (!mNoise)
          mADSR.Stop();
      }
    }

    UpdateBlockHeader();

    mNAX++;
  }
}

void Voice::UpdateBlockHeader() {
  mCurHeader.bits = mSample[mNAX & ~0x7];
  if (mCurHeader.LoopStart.get() && !mCustomLoop)
    mLSA = mNAX & ~0x7;
  // (AI-assisted) for DecodeSamples
  mDecShift = mCurHeader.Shift.get();
  mDecCoef1 = adpcm_coefs[mCurHeader.Filter.get()][0];
  mDecCoef2 = adpcm_coefs[mCurHeader.Filter.get()][1];
}

static s16 ApplyVolume(s16 sample, s32 volume) {
  return (sample * volume) >> 15;
}

void Voice::KeyOn() {
  mNAX = mSSA;
  mNAX++;

  UpdateBlockHeader();

  mENDX = false;
  mADSR.Attack();
  mCounter = 0;
  mDecodeHist1 = 0;
  mDecodeHist2 = 0;
  mDecodeBuf.Reset();
  mCustomLoop = false;
  // Console.WriteLn("SPU[%d]:VOICE[%d] Key On, SSA %08x", m_SPU.m_Id, m_Id, m_SSA);
}

void Voice::KeyOff() {
  mADSR.Release();
  // fmt::print("Key Off\n");
}

// (AI-assisted) for the per sample code in RunBlock (a lambda called from two places)
#if defined(__GNUC__) || defined(__clang__)
#define SND_ALWAYS_INLINE __attribute__((always_inline))
#else
#define SND_ALWAYS_INLINE
#endif

// (AI-assisted) out += {left, right} with saturation, as s16Output::operator+= (one QADD16 on the
// 3DS)
static inline void MixInto(s16Output& out, s16 left, s16 right) {
#if defined(__ARM_FEATURE_SIMD32)
  int16x2_t cur;
  std::memcpy(&cur, &out, sizeof(cur));
  const int16x2_t add = (int16x2_t)((u32)(u16)left | ((u32)(u16)right << 16));
  cur = __qadd16(cur, add);
  std::memcpy(&out, &cur, sizeof(cur));
#else
  out += s16Output{left, right};
#endif
}

int Voice::RunBlock(s16Output* out, int n, u32 ratio) {
  const bool ahead = ratio > 0x10000;
  // the pitch counter per output sample (Run(): min(pitch, 0x3FFF) per tick)
  const u32 step = ((u32)std::min<s32>(mPitch, 0x3FFF) * ratio + 0x8000) >> 16;
  int i = 0;
  if (mVolume.left.Sweeping() || mVolume.right.Sweeping()) {
    // volume sweeps (fades): the plain path, one RunTicks per sample (1 or 2 SPU ticks each)
    for (; i < n && !Stopped(); i++) {
      const u32 acc = mTickFrac + ratio;
      mTickFrac = acc & 0xFFFF;
      out[i] += RunTicks(step, acc >> 16, ahead);
    }
    return i;
  }
  if (Stopped()) {
    return 0;
  }
  const s32 vol_l = mVolume.left.GetCurrent(), vol_r = mVolume.right.GetCurrent();
  // the decoded samples and the pitch counter, in registers
  const s16* const buf = mDecodeBuf.Base();
  size_t rd = mDecodeBuf.ReadPos(), wr = mDecodeBuf.WritePos();
  u32 counter = mCounter;
  // One output sample as Run() makes it, without the envelope and volume steps: decoding,
  // interpolation (none when the sample is 0 whatever the voice plays), the pitch counter. False:
  // a loop end without repeat stopped the voice, Run() gives 0 for this sample (the ADSR level is 0
  // now) and the voice isn't run again.
  auto run_sample = [&](s16Output& o, s32 level, bool silent) SND_ALWAYS_INLINE {
    if (wr - rd < 16) {
      mDecodeBuf.SetReadPos(rd);
      DecodeSamples();
      while (ahead && mDecodeBuf.Size() < 16 && !Stopped()) {
        DecodeSamples();
      }
      rd = mDecodeBuf.ReadPos();
      wr = mDecodeBuf.WritePos();
      if (Stopped()) {
        return false;
      }
    }
    if (!silent) {
      const s16* p = buf + rd;
      const auto& coef = interp_table[(counter >> 4) & 0xFF];
      // Run() wraps the sum to s16 after each term: the same as wrapping the total
      const s32 sum = ((p[0] * coef[0]) >> 15) + ((p[1] * coef[1]) >> 15) +
                      ((p[2] * coef[2]) >> 15) + ((p[3] * coef[3]) >> 15);
      const s16 sample = static_cast<s16>((static_cast<s16>(sum) * level) >> 15);
      MixInto(o, static_cast<s16>((sample * vol_l) >> 15), static_cast<s16>((sample * vol_r) >> 15));
    }
    counter += step;
    rd += counter >> 12;
    counter &= 0xFFF;
    return true;
  };
  while (i < n) {
    // The envelope level stays the same while its steps only advance its counter: for `quiet`
    // more SPU ticks. Output sample k (from 1) ends at tick (mTickFrac + k * ratio) >> 16: the
    // samples that end by then are made with this level and their ticks added in one go
    // (SkipQuiet).
    const s32 level = mADSR.Level();
    const u32 quiet = (u32)mADSR.QuietSamples();
    const bool silent = level == 0 || (vol_l == 0 && vol_r == 0);
    const u32 left = (u32)(n - i);
    u32 run;
    if ((u64)mTickFrac + (u64)left * ratio < (((u64)quiet + 1) << 16)) {
      run = left;
    } else if (quiet == 0) {
      run = 0;
    } else {
      // the largest k with mTickFrac + k * ratio < (quiet + 1) << 16 (quiet < 2 * left here)
      run = ((((quiet + 1) << 16) - 1 - mTickFrac) / ratio);
    }
    u32 done = 0;
    bool stopped = false;
    for (; done < run; done++) {
      if (!run_sample(out[i + done], level, silent)) {
        done++;
        stopped = true;
        break;
      }
    }
    i += done;
    {
      // (the tick fraction also for a sample that stopped the voice: it carries over to the next
      // key on)
      const u64 acc = (u64)mTickFrac + (u64)done * ratio;
      mTickFrac = (u32)(acc & 0xFFFF);
      if (stopped) {
        break;
      }
      mADSR.SkipQuiet((int)(acc >> 16));
    }
    if (i >= n) {
      break;
    }
    // the next sample's ticks reach an envelope change: the sample uses the level from before
    // them (like Run()), then they're run one by one
    const u32 acc = mTickFrac + ratio;
    mTickFrac = acc & 0xFFFF;
    if (!run_sample(out[i], level, silent)) {
      i++;
      break;
    }
    i++;
    for (u32 t = 0; t < (acc >> 16); t++) {
      mADSR.Run();
    }
    if (Stopped()) {
      break;  // released to silence
    }
  }
  mDecodeBuf.SetReadPos(rd);
  mCounter = counter;
  return i;
}

s16Output Voice::Run() {
  return RunTicks((u32)std::min<s32>(mPitch, 0x3FFF), 1, false);
}

s16Output Voice::RunTicks(u32 step, u32 ticks, bool ahead) {
  DecodeSamples();
  while (ahead && mDecodeBuf.Size() < 16 && !Stopped()) {
    DecodeSamples();
  }

  u32 index = (mCounter & 0x0FF0) >> 4;

  s16 sample = 0;
  sample = static_cast<s16>(sample + ((mDecodeBuf.Peek(0) * interp_table[index][0]) >> 15));
  sample = static_cast<s16>(sample + ((mDecodeBuf.Peek(1) * interp_table[index][1]) >> 15));
  sample = static_cast<s16>(sample + ((mDecodeBuf.Peek(2) * interp_table[index][2]) >> 15));
  sample = static_cast<s16>(sample + ((mDecodeBuf.Peek(3) * interp_table[index][3]) >> 15));

  mCounter += step;

  mDecodeBuf.Skip(mCounter >> 12);
  mCounter &= 0xFFF;

  sample = ApplyVolume(sample, mADSR.Level());
  s16 left = ApplyVolume(sample, mVolume.left.GetCurrent());
  s16 right = ApplyVolume(sample, mVolume.right.GetCurrent());

  for (u32 t = 0; t < ticks; t++) {
    mADSR.Run();
    mVolume.Run();
  }

  return s16Output{left, right};
}
}  // namespace snd
