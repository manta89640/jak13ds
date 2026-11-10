// Copyright: 2021 - 2024, Ziemas
// SPDX-License-Identifier: ISC
#pragma once
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <vector>

#include "bitfield.h"
#include "envelope.h"
#include "fifo.h"
#include "sound_types.h"

#include "common/common_types.h"

namespace snd {

class Voice {
 public:
  enum class AllocationType {
    Managed,
    Permanent,
  };

  Voice(AllocationType alloc = AllocationType::Managed) : mAlloc(alloc) {}
  s16Output Run();
  // (AI-assisted) out[i] += Run() for i < n, stopping when the voice stops (as Synth::Tick does),
  // with the same result; returns the samples run. The envelope steps are done in one go while
  // they only advance a counter (most samples), which makes it several times cheaper.
  // ratio: SPU ticks (48 kHz) per output sample, 16.16 fixed point. 0x10000 is Run() exactly;
  // more mixes at a lower rate (the 3DS mixes at the DSP's 32728 Hz): the pitch counter moves
  // that much further per output sample and the envelopes run that many ticks, so pitch and
  // timing stay the same.
  int RunBlock(s16Output* out, int n, u32 ratio = 0x10000);

  void KeyOn();

  void KeyOff();

  bool Dead() {
    if (mAlloc == AllocationType::Permanent) {
      return false;
    }

    return mADSR.GetPhase() == ADSR::Phase::Stopped;
  }

  // The 3DS mixer runs the voices outside its lock: between SetBusy(true) and SetBusy(false)
  // (both under the lock, like every register write) the writes below are kept in order and
  // SetBusy(false) applies them, as if they were made just after the mixer's block. The state
  // other threads read meanwhile is the published one (GetPublishedNax, StoppedPublished).
  void SetBusy(bool busy);

  void SetPitch(u16 reg) {
    // fmt::print("VOICE[{}] PITCH WRITE {:x}\n", m_channel, reg);
    if (mBusy) {
      return Defer(Op::Pitch, reg);
    }
    mPitch = reg;
  }
  void SetAsdr1(u16 reg) {
    // fmt::print("VOICE[{}] ADSR1 WRITE {:x}\n", m_channel, reg);
    if (mBusy) {
      return Defer(Op::Adsr1, reg);
    }
    mADSR.m_Reg.lo.set(reg);
  }
  void SetAsdr2(u16 reg) {
    // fmt::print("VOICE[{}] ADSR2 WRITE {:x}\n", m_channel, reg);
    if (mBusy) {
      return Defer(Op::Adsr2, reg);
    }
    mADSR.m_Reg.hi.set(reg);
  }
  void SetVolume(u16 left, u16 right) {
    // fmt::print("VOICE[{}] VOLL WRITE {:x}\n", m_channel, left);
    // fmt::print("VOICE[{}] VOLR WRITE {:x}\n", m_channel, right);
    SetVolumeL(left);
    SetVolumeR(right);
  }

  void SetVolumeL(u16 vol) {
    if (mBusy) {
      return Defer(Op::VolumeL, vol);
    }
    mVolume.left.Set(vol);
  }

  void SetVolumeR(u16 vol) {
    if (mBusy) {
      return Defer(Op::VolumeR, vol);
    }
    mVolume.right.Set(vol);
  }

  s16 GetEnvx() { return mADSR.Level(); }

  void SetSample(u16* sample) {
    if (mBusy) {
      mDeferredSsa = 0;
      return Defer(Op::Sample, 0, sample);
    }
    mSample = sample;
    mSSA = 0;
  }

  u32 GetNax() { return mNAX; }
  // (AI-assisted) 3DS: the NAX as the mixer left it after its last chunk (PublishNax, for all the
  // overlord's voices at once: sdshim.cpp). The mixer runs a chunk voice by voice, so reading
  // mNAX in the middle of it gave a stereo stream's two voices a chunk apart, and the overlord
  // took that for a drifted pair and restarted both (a click, ~15 times a second).
  u32 GetPublishedNax() const { return mPubNAX.load(std::memory_order_relaxed); }
  void PublishNax() {
    mPubNAX.store(mNAX, std::memory_order_relaxed);
    mPubStopped.store(Stopped(), std::memory_order_relaxed);
  }
  // Stopped() as published with the NAX
  bool StoppedPublished() const { return mPubStopped.load(std::memory_order_relaxed); }
  // (AI-assisted) played a loop end block without repeat (the end of a stream)
  bool Stopped() const { return mADSR.GetPhase() == ADSR::Phase::Stopped; }

  void SetSsa(u32 addr) {
    if (mBusy) {
      mDeferredSsa = addr;
      return Defer(Op::Ssa, addr);
    }
    mSSA = addr;
  }

  void SetLsa(u32 addr) {
    if (mBusy) {
      return Defer(Op::Lsa, addr);
    }
    mLSA = addr;
    mCustomLoop = true;
  }

  void Stop();

  // the voice's output goes to the effect unit too (the SPU2's wet voice mix bits); set before
  // the synth gets the voice
  void SetReverbSend(bool on) { mReverbSend = on; }
  bool ReverbSend() const { return mReverbSend; }

  // Muted (Synth::BeginBlock, between blocks): RunBlock() moves the voice on as if it played
  // (address, envelope, loops, its end) but decodes and mixes nothing. Heard again, it starts
  // from silence in its current block.
  void SetMuted(bool muted) { mMuted = muted; }
  bool Muted() const { return mMuted; }
  bool Permanent() const { return mAlloc == AllocationType::Permanent; }
  // KeyOff() came after the last KeyOn() (set when called, also while busy)
  bool KeyedOff() const { return mKeyedOff; }
  // how loud the voice plays now: its envelope level (an attack counts as its peak) times the
  // louder side's volume
  u32 Loudness() const {
    const u32 level = mADSR.GetPhase() == ADSR::Phase::Attack ? 0x7FFFu : (u32)mADSR.Level();
    const u32 left = (u32)std::abs((s32)mVolume.left.GetCurrent());
    const u32 right = (u32)std::abs((s32)mVolume.right.GetCurrent());
    return level * std::max(left, right);
  }

 private:
  bool mReverbSend{false};
  bool mMuted{false};
  bool mKeyedOff{false};
  enum class Op : u8 { Pitch, Adsr1, Adsr2, VolumeL, VolumeR, Sample, Ssa, Lsa, KeyOn, KeyOff, Stop };
  struct Deferred {
    Op op;
    u32 value;
    u16* sample;
  };
  void Defer(Op op, u32 value, u16* sample = nullptr) { mDeferred.push_back({op, value, sample}); }
  bool mBusy{false};
  std::vector<Deferred> mDeferred;
  // the SSA after the deferred writes (for the NAX a deferred key on publishes); ~0: none deferred
  u32 mDeferredSsa{~0u};
  std::atomic<bool> mPubStopped{true};

  union ADPCMHeader {
    u16 bits;
    bitfield<u16, bool, 10, 1> LoopStart;
    bitfield<u16, bool, 9, 1> LoopRepeat;
    bitfield<u16, bool, 8, 1> LoopEnd;
    bitfield<u16, u8, 4, 3> Filter;
    bitfield<u16, u8, 0, 4> Shift;
  };

  AllocationType mAlloc;
  bool mNoise{false};
  bool mENDX{false};

  // (AI-assisted) The decoder: DecodeSamples() moves 4 samples (one ADPCM data word, mNAX) into
  // mDecodeBuf when it holds fewer than 16, then NextBlock() at the end of a block. The words are
  // decoded a whole block at a time (DecodeBlock(): the rest of the block, written ahead in
  // mDecodeBuf), so a block's data is read when the decoder starts it. The game never changes the
  // block being played: a stream refills the other half of its buffer.
  void DecodeSamples();
  void DecodeBlock();
  void NextBlock();
  bool EndBlock();
  void UpdateBlockHeader();
  // (AI-assisted) Run() with the pitch counter advanced by `step` and the envelopes by `ticks`
  // SPU ticks; ahead: keep 16 decoded samples (more than one tick per sample can pop 6)
  s16Output RunTicks(u32 step, u32 ticks, bool ahead);
  // (AI-assisted) RunBlock() for ratio > 0x10000 (ahead) or not
  template <bool ahead>
  int RunBlockImpl(s16Output* out, int n, u32 ratio);
  u32 mTickFrac{0};  // (AI-assisted) RunBlock: SPU tick fraction carried between output samples

  // (AI-assisted) contiguous (linear_fifo): the interpolation reads Data()[0..3]. Up to 19 samples
  // when a block (up to 8 words, 32 samples) is written ahead.
  linear_fifo<s16, 0x40> mDecodeBuf{};
  // (AI-assisted) the decoder's history at the end of the samples written ahead
  s16 mDecodeHist1{0};
  s16 mDecodeHist2{0};
  // (AI-assisted) mNAX after the last word written ahead: while there are words written ahead,
  // mNAX = mBlockEnd - mDecodeBuf.Ahead() / 4 (RunBlock moves only the positions, then sets mNAX)
  u32 mBlockEnd{0};
  // (AI-assisted) the current block's shift and filter coefficients (UpdateBlockHeader)
  u8 mDecShift{0};
  s16 mDecCoef1{0};
  s16 mDecCoef2{0};
  u32 mCounter{0};

  u16 mPitch{0};

  u16* mSample{nullptr};
  u32 mSSA{0};
  u32 mNAX{0};
  std::atomic<u32> mPubNAX{0};  // (AI-assisted) GetPublishedNax
  u32 mLSA{0};
  bool mCustomLoop{false};

  ADPCMHeader mCurHeader{};

  ADSR mADSR{};
  VolumePair mVolume{};
};
}  // namespace snd
