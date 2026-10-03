// Copyright: 2021 - 2024, Ziemas
// SPDX-License-Identifier: ISC
#include "voice.h"

#include <algorithm>
#include <array>
#include <cstring>
#if defined(__ARM_FEATURE_SIMD32) || defined(__ARM_FEATURE_SAT) || defined(__ARM_FEATURE_DSP)
#include <arm_acle.h>
#endif

namespace snd {
#include "interp_table.inc"

// (AI-assisted) for the per sample code in RunBlock (a lambda called from two places), and to keep
// the once per block code out of it
#if defined(__GNUC__) || defined(__clang__)
#define SND_ALWAYS_INLINE __attribute__((always_inline))
#define SND_NOINLINE __attribute__((noinline))
#else
#define SND_ALWAYS_INLINE
#define SND_NOINLINE
#endif

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

// (AI-assisted) ARM DSP instructions (the 3DS's ARM11 has them). test/snd_voice also builds the
// code that uses them on the host, with these plain versions (SND_TEST_ARM_DSP), to test it.
#if defined(SND_TEST_ARM_DSP)
#define SND_ARM_DSP 1
// SMLAWB: c + the top 32 bits of the 48-bit product of a and the bottom half of b
static inline s32 Smlawb(s32 a, s32 b, s32 c) {
  return c + (s32)(((s64)a * (s16)b) >> 16);
}
static inline s32 Ssat16(s32 v) {
  return std::clamp<s32>(v, INT16_MIN, INT16_MAX);
}
#elif defined(__ARM_FEATURE_DSP) && defined(__ARM_FEATURE_SAT)
#define SND_ARM_DSP 1
static inline s32 Smlawb(s32 a, s32 b, s32 c) {
  return __smlawb(a, b, c);
}
static inline s32 Ssat16(s32 v) {
  return __ssat(v, 16);
}
#endif

// (AI-assisted) Decodes `count` ADPCM data words from src (4 samples each, the lowest nibble
// first) into dst, with the block's shift and filter coefficients and the history in hist1 /
// hist2. Each sample is
//   clamp((nibble << 12 >> shift) + (coef1 * hist1 >> 6) + (coef2 * hist2 >> 6))
// as the previous one-word-at-a-time loop computed it. Nibble k of a word w, << 12 >> shift, is
// (s32)(w << (28 - 4k) & 0xF0000000) >> (16 + shift). On ARM a filter term and its addition are
// one SMLAWB: (coef << 10) * hist >> 16 (the top 32 bits of the 48-bit product) is exactly
// coef * hist >> 6.
static inline void DecodeWords(s16* dst,
                               const u16* src,
                               u32 count,
                               u32 shift,
                               s32 coef1,
                               s32 coef2,
                               s32& hist1,
                               s32& hist2) {
  // TODO do the right thing for invalid shift/filter values
  // (and the clamp: we do get overflow here otherwise, should we?)
  const u32 sh = 16 + shift;
#if defined(SND_ARM_DSP)
  const s32 c1 = coef1 << 10, c2 = coef2 << 10;
  auto sample = [&](s32 nibble, s32 h1, s32 h2) SND_ALWAYS_INLINE {
    const s32 sum2 = Smlawb(c2, h2, nibble);
    return Ssat16(Smlawb(c1, h1, sum2));
  };
#else
  auto sample = [&](s32 nibble, s32 h1, s32 h2) SND_ALWAYS_INLINE {
    return std::clamp<s32>(nibble + ((coef1 * h1) >> 6) + ((coef2 * h2) >> 6), INT16_MIN,
                           INT16_MAX);
  };
#endif
  s32 h1 = hist1, h2 = hist2;
  for (u32 i = 0; i < count; i++) {
    const u32 w = src[i];
    const s32 s0 = sample((s32)(w << 28) >> sh, h1, h2);
    const s32 s1 = sample((s32)((w << 24) & 0xF0000000u) >> sh, s0, h1);
    const s32 s2 = sample((s32)((w << 20) & 0xF0000000u) >> sh, s1, s0);
    const s32 s3 = sample((s32)((w << 16) & 0xF0000000u) >> sh, s2, s1);
    dst[0] = static_cast<s16>(s0);
    dst[1] = static_cast<s16>(s1);
    dst[2] = static_cast<s16>(s2);
    dst[3] = static_cast<s16>(s3);
    dst += 4;
    h2 = s2;
    h1 = s3;
  }
  hist1 = h1;
  hist2 = h2;
}

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
    // (AI-assisted) zeros, also in place of samples written ahead: the voice stays silent until a
    // key on starts it over
    s16* dst = mDecodeBuf.Ahead() ? mDecodeBuf.AheadData() : mDecodeBuf.Reserve(4);
    dst[0] = dst[1] = dst[2] = dst[3] = 0;
  } else if (!mDecodeBuf.Ahead()) {
    DecodeBlock();  // (AI-assisted) this word and the rest of its block
  }
  mDecodeBuf.Commit(4);

  mNAX++;

  if ((mNAX & 0x7) == 0) {
    NextBlock();
  }
}

// (AI-assisted) Writes the samples of the words mNAX .. the end of its block (the next multiple of
// 8) ahead in mDecodeBuf: 4 * (8 - (mNAX & 7)) samples. mNAX & 7 is 0 only after a key on or loop
// to an address 7 past a block start: then the header word is decoded as data first, as before.
void Voice::DecodeBlock() {
  const u32 count = 8 - (mNAX & 7);
  s32 hist1 = mDecodeHist1, hist2 = mDecodeHist2;
  DecodeWords(mDecodeBuf.Reserve(4 * count), mSample + mNAX, count, mDecShift, mDecCoef1, mDecCoef2,
              hist1, hist2);
  mDecodeHist1 = static_cast<s16>(hist1);
  mDecodeHist2 = static_cast<s16>(hist2);
  mBlockEnd = mNAX + count;
}

// The end of a block, after its last word: (AI-assisted) mNAX is a multiple of 8
void Voice::NextBlock() {
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

// (AI-assisted) RunBlock's DecodeSamples() for the last word of a block written ahead: the block
// end, then the next block written ahead. False: a loop end without repeat stopped the voice.
SND_NOINLINE bool Voice::EndBlock() {
  mNAX = mBlockEnd;
  NextBlock();
  if (Stopped()) {
    return false;
  }
  DecodeBlock();
  return true;
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
  // (AI-assisted) compiled for each case: the 3DS mixes at fewer samples than SPU ticks (ahead),
  // and a constant leaves the per sample loop a register more
  return ratio > 0x10000 ? RunBlockImpl<true>(out, n, ratio) : RunBlockImpl<false>(out, n, ratio);
}

template <bool ahead>
int Voice::RunBlockImpl(s16Output* out, int n, u32 ratio) {
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
  if (!mDecodeBuf.Ahead()) {
    DecodeBlock();  // (AI-assisted) after a key on: the first block
  }
  const s32 vol_l = mVolume.left.GetCurrent(), vol_r = mVolume.right.GetCurrent();
  // the decoded samples and the pitch counter, in registers. (AI-assisted) [wr, end): the samples
  // of the current block written ahead.
  const s16* const buf = mDecodeBuf.Base();
  size_t rd = mDecodeBuf.ReadPos(), wr = mDecodeBuf.WritePos(), end = mDecodeBuf.AheadPos();
  u32 counter = mCounter;
  // One output sample as Run() makes it, without the envelope and volume steps: decoding,
  // interpolation (none when the sample is 0 whatever the voice plays), the pitch counter. False:
  // a loop end without repeat stopped the voice, Run() gives 0 for this sample (the ADSR level is 0
  // now) and the voice isn't run again.
  auto run_sample = [&](s16Output& o, s32 level, bool silent) SND_ALWAYS_INLINE {
    if (wr - rd < 16) {
      // (AI-assisted) DecodeSamples(), with more calls until 16 samples are buffered when ahead:
      // the samples are there already, only a block's last word does more
      do {
        wr += 4;
        if (wr == end) {
          mDecodeBuf.SetReadPos(rd);
          mDecodeBuf.SetWritePos(wr);
          if (!EndBlock()) {
            return false;
          }
          rd = mDecodeBuf.ReadPos();
          wr = mDecodeBuf.WritePos();
          end = mDecodeBuf.AheadPos();
        }
      } while (ahead && wr - rd < 16);
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
    auto run_span = [&](bool silent_span) SND_ALWAYS_INLINE {
      for (; done < run; done++) {
        if (!run_sample(out[i + done], level, silent_span)) {
          done++;
          stopped = true;
          break;
        }
      }
    };
    // (AI-assisted) a loop for each, without the check per sample
    if (silent) {
      run_span(true);
    } else {
      run_span(false);
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
  mDecodeBuf.SetWritePos(wr);
  if (end != wr) {
    // (AI-assisted) the words not taken yet (otherwise the voice stopped at the block end, which
    // set mNAX)
    mNAX = mBlockEnd - (u32)(end - wr) / 4;
  }
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
