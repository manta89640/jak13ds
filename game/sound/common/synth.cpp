// Copyright: 2021 - 2024, Ziemas
// SPDX-License-Identifier: ISC
#include "synth.h"

#include <stdexcept>

namespace snd {

static s16 ApplyVolume(s16 sample, s32 volume) {
  return (sample * volume) >> 15;
}

s16Output Synth::Tick() {
  s16Output out{};

  mVoices.remove_if([](std::shared_ptr<Voice>& v) { return v->Dead(); });
  for (auto& v : mVoices) {
#ifdef __3DS__
    // (AI-assisted) The 8 permanent SPU voices (sdshim) are in the list forever; only the VAG
    // stream uses one. A stopped voice decodes and mixes zeros: skip it.
    if (v->Stopped()) {
      continue;
    }
#endif
    out += v->Run();
  }

  out.left = ApplyVolume(out.left, mVolume.left.Get());
  out.right = ApplyVolume(out.right, mVolume.right.Get());

  mVolume.Run();

  return out;
}

#ifdef __3DS__
void Synth::BeginBlock() {
  mVoices.remove_if([](std::shared_ptr<Voice>& v) { return v->Dead(); });
  for (auto& v : mVoices) {
    v->SetBusy(true);
  }
  mBusy = true;
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
}

void Synth::Mix(s16Output* out, int samples, u32 ratio) {
  for (int i = 0; i < samples; i++) {
    out[i] = s16Output{};
  }
  for (auto& v : mVoices) {
    // the same order of (saturating) additions per sample as Tick(); a voice that stops is
    // skipped from then on, like there. (AI-assisted) RunBlock: the same samples, with the
    // envelope steps done in one go while they only advance a counter.
    v->RunBlock(out, samples, ratio);
  }
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
#endif

void Synth::AddVoice(std::shared_ptr<Voice> voice) {
#ifdef __3DS__
  if (mBusy) {
    mPendingVoices.push_back(std::move(voice));
    return;
  }
#endif
  mVoices.emplace_front(voice);
}

void Synth::SetMasterVol(u32 volume) {
#ifdef __3DS__
  if (mBusy) {
    mPendingMasterVol = volume;
    mMasterVolPending = true;
    return;
  }
#endif
  mVolume.left.Set(volume);
  mVolume.right.Set(volume);
}
}  // namespace snd
