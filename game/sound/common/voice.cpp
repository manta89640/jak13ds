// Copyright: 2021 - 2024, Ziemas
// SPDX-License-Identifier: ISC
#include "voice.h"

#include <algorithm>
#include <array>

namespace snd {
#include "interp_table.inc"

// Integer math version of ps-adpcm coefs
static constexpr std::array<std::array<s16, 2>, 5> adpcm_coefs = {{
    {0, 0},
    {60, 0},
    {115, -52},
    {98, -55},
    {122, -60},
}};

void Voice::DecodeSamples() {
  // This doesn't exactly match the real behaviour,
  // it seems to initially decode a bigger chunk
  // and then decode more data after a bit has drained
  if (mDecodeBuf.Size() >= 16) {
    // sufficient data buffered
    return;
  }

  // Skip decoding for stopped voices.
  if (mADSR.GetPhase() == ADSR::Phase::Stopped) {
    for (int i = 0; i < 4; i++)
      mDecodeBuf.Push(0);
  } else {
    u32 data = mSample[mNAX];
    for (int i = 0; i < 4; i++) {
      s32 sample = (s16)((data & 0xF) << 12);
      sample >>= mCurHeader.Shift.get();

      // TODO do the right thing for invalid shift/filter values
      sample += (adpcm_coefs[mCurHeader.Filter.get()][0] * mDecodeHist1) >> 6;
      sample += (adpcm_coefs[mCurHeader.Filter.get()][1] * mDecodeHist2) >> 6;

      // We do get overflow here otherwise, should we?
      sample = std::clamp<s32>(sample, INT16_MIN, INT16_MAX);

      mDecodeHist2 = mDecodeHist1;
      mDecodeHist1 = static_cast<s16>(sample);

      mDecodeBuf.Push(static_cast<s16>(sample));
      data >>= 4;
    }
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

static inline s32 Saturate16(s32 v) {
#if defined(__arm__) && defined(__ARM_ARCH) && __ARM_ARCH >= 6
  s32 r;
  __asm__("ssat %0, #16, %1" : "=r"(r) : "r"(v));
  return r;
#else
  return std::clamp<s32>(v, INT16_MIN, INT16_MAX);
#endif
}

// (a * b) >> 15 with both in s16 range (a 16x16 multiply on ARM), truncated to s16 like ApplyVolume
static inline s16 Mul15(s32 a, s32 b) {
  return static_cast<s16>((static_cast<s32>(static_cast<s16>(a)) * static_cast<s16>(b)) >> 15);
}

// Same as calling Run() n times (and adding each result to the mix with s16Output's saturating
// +=). The mixer runs this for every voice at 48 kHz, so it is the hottest code of the sound
// system. The samples go in runs during which the envelope level doesn't change (ADSR::ConstantRun:
// the envelope steps of a run only advance its counter, applied at the end of the run), and a run
// that is silent (level or both volumes 0) only advances the decoder.
int Voice::RunBlock(s32* __restrict mixL, s32* __restrict mixR, int n) {
  // Run() clamps the pitch every sample; nothing changes it or the volume settings during a block
  const u32 step = std::min<u32>(mPitch, 0x3FFF);
  const bool sweep = mVolume.left.Sweeping() || mVolume.right.Sweeping();
  int i = 0;
  while (i < n) {
    // Synth drops stopped managed voices and skips stopped ones on the 3DS before each sample
    if (mADSR.GetPhase() == ADSR::Phase::Stopped) {
#ifdef __3DS__
      return i;
#else
      if (mAlloc != AllocationType::Permanent) {
        return i;
      }
      // a stopped permanent voice keeps decoding (zeros), sample by sample
      const s16Output out = Run();
      mixL[i] = Saturate16(mixL[i] + out.left);
      mixR[i] = Saturate16(mixR[i] + out.right);
      i++;
      continue;
#endif
    }

    // a volume sweep changes the volume every sample
    const int run = sweep ? 1 : mADSR.ConstantRun(n - i);
    const s32 level = mADSR.Level();
    const s32 vol_left = mVolume.left.GetCurrent();
    const s32 vol_right = mVolume.right.GetCurrent();
    const int end = i + run;
    bool ended = false;
    // the decoder's read position and the pitch counter live in locals during the run; the
    // decoder (DecodeSamples, every few samples) only adds to the FIFO
    size_t rd = mDecodeBuf.ReadPos();
    size_t wr = mDecodeBuf.WritePos();
    u32 counter = mCounter;
    if (level == 0 || (vol_left == 0 && vol_right == 0)) {
      // silent: only the decoder and the pitch counter move
      for (; i < end; i++) {
        if (wr - rd < 16) {
          mDecodeBuf.SetReadPos(rd);
          DecodeSamples();
          wr = mDecodeBuf.WritePos();
          if (mADSR.GetPhase() == ADSR::Phase::Stopped) {
            ended = true;
          }
        }
        counter += step;
        rd += counter >> 12;
        counter &= 0xFFF;
        if (ended) {
          i++;
          break;
        }
      }
    } else {
      for (; i < end; i++) {
        if (wr - rd < 16) {
          mDecodeBuf.SetReadPos(rd);
          DecodeSamples();
          wr = mDecodeBuf.WritePos();
          if (mADSR.GetPhase() == ADSR::Phase::Stopped) {
            // the end of a sample that doesn't loop: this sample is silent (level 0) and the voice
            // stops. The envelope steps of the run so far only advanced its counter, which KeyOn
            // and KeyOff reset.
            counter += step;
            rd += counter >> 12;
            counter &= 0xFFF;
            ended = true;
            i++;
            break;
          }
        }

        const s16* w = mDecodeBuf.Window(rd);
        const auto& taps = interp_table[(counter >> 4) & 0xFF];
        s16 sample = 0;
        sample = static_cast<s16>(sample + ((w[0] * taps[0]) >> 15));
        sample = static_cast<s16>(sample + ((w[1] * taps[1]) >> 15));
        sample = static_cast<s16>(sample + ((w[2] * taps[2]) >> 15));
        sample = static_cast<s16>(sample + ((w[3] * taps[3]) >> 15));
        sample = Mul15(sample, level);
        mixL[i] = Saturate16(mixL[i] + Mul15(sample, vol_left));
        mixR[i] = Saturate16(mixR[i] + Mul15(sample, vol_right));

        counter += step;
        rd += counter >> 12;
        counter &= 0xFFF;
      }
    }
    mDecodeBuf.SetReadPos(rd);
    mCounter = counter;
    if (ended) {
      mVolume.Run();  // that sample's volume step (a stopped envelope doesn't step)
      continue;
    }

    mADSR.RunConstant(run);
    if (sweep) {
      mVolume.Run();  // run == 1
    }
  }
  return n;
}

s16Output Voice::Run() {
  DecodeSamples();

  u32 index = (mCounter & 0x0FF0) >> 4;

  s16 sample = 0;
  sample = static_cast<s16>(sample + ((mDecodeBuf.Peek(0) * interp_table[index][0]) >> 15));
  sample = static_cast<s16>(sample + ((mDecodeBuf.Peek(1) * interp_table[index][1]) >> 15));
  sample = static_cast<s16>(sample + ((mDecodeBuf.Peek(2) * interp_table[index][2]) >> 15));
  sample = static_cast<s16>(sample + ((mDecodeBuf.Peek(3) * interp_table[index][3]) >> 15));

  s32 step = mPitch;
  step = std::min(step, 0x3FFF);
  mCounter += step;

  auto steps = mCounter >> 12;
  mCounter &= 0xFFF;

  while (steps > 0) {
    steps--;
    mDecodeBuf.Pop();
  }

  sample = ApplyVolume(sample, mADSR.Level());
  s16 left = ApplyVolume(sample, mVolume.left.GetCurrent());
  s16 right = ApplyVolume(sample, mVolume.right.GetCurrent());

  mADSR.Run();
  mVolume.Run();

  return s16Output{left, right};
}
}  // namespace snd
