// Copyright: 2021 - 2024, Ziemas
// SPDX-License-Identifier: ISC
#include "synth.h"

#include <algorithm>
#include <stdexcept>

namespace snd {

static s16 ApplyVolume(s16 sample, s32 volume) {
  return (sample * volume) >> 15;
}

s16Output Synth::Tick() {
  s16Output out{}, wet{};
  const bool reverb = mReverb.Active();

  mVoices.remove_if([](std::shared_ptr<Voice>& v) { return v->Dead(); });
  for (auto& v : mVoices) {
    const s16Output s = v->Run();
    out += s;
    if (reverb && v->ReverbSend()) {
      wet += s;
    }
  }
  mReverb.Process(&wet, &out, 1);

  out.left = ApplyVolume(out.left, mVolume.left.Get());
  out.right = ApplyVolume(out.right, mVolume.right.Get());

  mVolume.Run();

  return out;
}

void Synth::BeginBlock(int audible) {
  mVoices.remove_if([](std::shared_ptr<Voice>& v) { return v->Dead(); });
  int running = 0;
  for (auto& v : mVoices) {
    running += !v->Stopped();
  }
  mRunning = running;
  if (running > audible) {
    MuteQuietest(audible);
  } else if (mMuted != 0) {
    for (auto& v : mVoices) {
      v->SetMuted(false);
    }
    mMuted = 0;
  }
  for (auto& v : mVoices) {
    v->SetBusy(true);
  }
  mBusy = true;
}

// The loudest `audible` running voices stay heard (the permanent ones always do). A muted voice
// counts a quarter quieter, so two voices of about the same loudness don't swap every block.
void Synth::MuteQuietest(int audible) {
  mRank.clear();
  int permanent = 0;
  for (auto& v : mVoices) {
    if (v->Stopped()) {
      continue;
    }
    if (v->Permanent()) {
      permanent++;
      continue;
    }
    u32 loudness = v->Loudness();
    if (v->Muted()) {
      loudness -= loudness >> 2;
    }
    mRank.push_back({loudness, v.get()});
  }
  const size_t keep = (size_t)std::max(audible - permanent, 0);
  if (keep < mRank.size()) {
    std::nth_element(mRank.begin(), mRank.begin() + keep, mRank.end(),
                     [](const Rank& a, const Rank& b) { return a.loudness > b.loudness; });
  }
  mMuted = 0;
  for (size_t i = 0; i < mRank.size(); i++) {
    const bool mute = i >= keep;
    mRank[i].voice->SetMuted(mute);
    mMuted += mute;
  }
}

void Synth::EndBlock() {
  mBusy = false;
  for (auto& v : mVoices) {
    v->SetBusy(false);
  }
  for (auto& v : mPendingVoices) {
    mVoices.emplace_front(std::move(v));
  }
  mPendingVoices.clear();
  if (mMasterVolPending) {
    mMasterVolPending = false;
    SetMasterVol(mPendingMasterVol);
  }
  if (mReverbModePending) {
    mReverbModePending = false;
    mReverb.SetMode(mPendingReverbMode);
  }
  if (mReverbDepthPending) {
    mReverbDepthPending = false;
    mReverb.SetDepth(mPendingDepthL, mPendingDepthR);
  }
}

void Synth::Mix(s16Output* out, int samples, u32 ratio) {
  for (int i = 0; i < samples; i++) {
    out[i] = s16Output{};
  }
  const bool reverb = mReverb.Active();
  if (reverb) {
    if (mWet.size() < (size_t)samples) {
      mWet.resize(samples);
    }
    std::fill_n(mWet.begin(), samples, s16Output{});
  }
  for (auto& v : mVoices) {
    // the same order of (saturating) additions per sample as Tick(); a voice that stops is
    // skipped from then on, like there. (AI-assisted) RunBlock: the same samples, with the
    // envelope steps done in one go while they only advance a counter.
    v->RunBlock(reverb && v->ReverbSend() ? mWet.data() : out, samples, ratio);
  }
  if (reverb) {
    // the voices sent to the effect play dry too
    for (int i = 0; i < samples; i++) {
      out[i] += mWet[i];
    }
  }
  mReverb.Process(mWet.data(), out, reverb ? samples : 0);
  if (!mVolume.left.Sweeping() && !mVolume.right.Sweeping()) {
    // Run() does nothing without a sweep
    const s32 vol_l = mVolume.left.Get(), vol_r = mVolume.right.Get();
    for (int i = 0; i < samples; i++) {
      out[i].left = ApplyVolume(out[i].left, vol_l);
      out[i].right = ApplyVolume(out[i].right, vol_r);
    }
    return;
  }
  for (int i = 0; i < samples; i++) {
    out[i].left = ApplyVolume(out[i].left, mVolume.left.Get());
    out[i].right = ApplyVolume(out[i].right, mVolume.right.Get());
    mVolume.Run();
  }
}

void Synth::AddVoice(std::shared_ptr<Voice> voice) {
  if (mBusy) {
    mPendingVoices.push_back(std::move(voice));
    return;
  }
  mVoices.emplace_front(voice);
}

void Synth::SetReverbMode(u32 mode) {
  if (mBusy) {
    mPendingReverbMode = mode;
    mReverbModePending = true;
    return;
  }
  mReverb.SetMode(mode);
}

void Synth::SetReverbDepth(s16 left, s16 right) {
  if (mBusy) {
    mPendingDepthL = left;
    mPendingDepthR = right;
    mReverbDepthPending = true;
    return;
  }
  mReverb.SetDepth(left, right);
}

void Synth::SetMasterVol(u32 volume) {
  if (mBusy) {
    mPendingMasterVol = volume;
    mMasterVolPending = true;
    return;
  }
  mVolume.left.Set(volume);
  mVolume.right.Set(volume);
}
}  // namespace snd
