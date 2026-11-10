// Copyright: 2021 - 2024, Ziemas
// SPDX-License-Identifier: ISC
#include "player.h"

#include <cstdlib>
#include <cstring>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <string>

#include "sfxblock.h"

#include "fmt/format.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <combaseapi.h>
#include <windows.h>
#endif
#include "common/log/log.h"

#ifdef __3DS__
#include "game/kernel/common/kperf.h"
#include "game/sound/sdshim.h"
#include "platform/3ds/port/ctr_port.h"
#endif

namespace snd {

u8 g_global_excite = 0;
bool g_output_active = false;

Player::Player() : mVmanager(mSynth) {
  InitCubeb();
}

Player::~Player() {
  DestroyCubeb();
}

#ifdef __3DS__
// (AI-assisted) The mix is made at the DSP's own output rate (32728 Hz): a 48 kHz mix was
// resampled down to it anyway (nothing above 16 kHz ever reached the speakers), and mixing at it
// is a third less work. The voices still run on SPU (48 kHz) time: the pitch counters and
// envelopes advance 48000 / 32728 ticks per output sample (Voice::RunBlock's ratio), the sound
// handlers tick every 200 SPU ticks (240 Hz). 768 frames = 23.5 ms per buffer, 3 buffers = 70 ms
// of latency at most; the mixer wakes at every DSP frame (~5 ms) and refills whatever is done.
static constexpr unsigned kMixRate = 32728;
static constexpr unsigned kMixFrames = 768;
static constexpr unsigned kMixBuffers = 3;
static constexpr u32 kTickRatio = (u32)((48000ull * 65536 + kMixRate / 2) / kMixRate);

// the mixer thread (ctr_thread_current_id): its own waits for the lock aren't counted
static std::atomic<unsigned> g_mixer_thread_id{0};
// (AI-assisted) time in the handler ticks (sequencers, sound effect grains) vs the voice mixing
static u64 g_handler_ticks = 0;

void TickLock::lock() {
  if (m_mutex.try_lock()) {
    return;
  }
  const u64 t0 = kperf::ticks();
  m_mutex.lock();
  if (ctr_thread_current_id() != g_mixer_thread_id.load(std::memory_order_relaxed)) {
    kperf::add_thread(kperf::Thread::SOUND_LOCK_WAIT, kperf::ticks() - t0, 1);
  }
}

#ifdef OG3DS_GAME_JAK2
// (AI-assisted, j2-mem2) Jak II: the mixer went 0.7 KB deep (Azahar, jak2_route.pad with sound)
constexpr unsigned kMixerStack = 64 * 1024;
#else
constexpr unsigned kMixerStack = 128 * 1024;
#endif

void Player::InitCubeb() {
  mHandlerThreadStop = false;
  g_output_active = false;
  mSynth.SetOutputRate(kMixRate);
#ifdef OG3DS_GAME_JAK2
  mVmanager.SetVoiceLimit(true);
#endif
  int core = 1;
  if (ctr_sound_config(&core)) {
    if (ctr_audio_init(kMixRate, kMixFrames, kMixBuffers) == 0) {
      // pinned: the mixer has the highest priority, so on the game's core (ctr_thread_create's
      // fallback when the app got no share of core 1) it would take the CPU from the game. Core 1
      // or 2 only (ctr_port.c), each the other's fallback.
      int err = ctr_thread_create_pinned(&Player::MixerThreadEntry, this, kMixerStack,
                                         CTR_PRIO_SOUND, core, &mMixerThread);
      if (err != 0 && core >= 2 && ctr_core1_enable() > 0) {
        // no core 2 (Old 3DS): the system core, with the share of it the app got
        core = 1;
        err = ctr_thread_create_pinned(&Player::MixerThreadEntry, this, kMixerStack,
                                       CTR_PRIO_SOUND, core, &mMixerThread);
      } else if (err != 0 && core == 1 && ctr_is_new3ds()) {
        // (AI-assisted, cpusound) no share of core 1: core 2, which a New 3DS app always has
        core = 2;
        err = ctr_thread_create_pinned(&Player::MixerThreadEntry, this, kMixerStack,
                                       CTR_PRIO_SOUND, core, &mMixerThread);
      }
      if (err == 0) {
        g_output_active = true;
        lg::info("3DS sound: mixer thread on core {} ({} Hz software mix -> DSP)", core, kMixRate);
      } else {
        lg::error("3DS sound: no mixer thread on core {} ({}), no audio", core,
                  err == -2 ? "the app got no share of core 1" : "thread not created");
        ctr_audio_exit();
      }
    } else {
      lg::error("3DS sound: DSP not available (sdmc:/3ds/dspfirm.cdc?), no audio");
    }
  } else {
    lg::info("3DS sound: off (config.ini sound = off)");
  }
  if (!g_output_active) {
    mHandlerThread = std::thread(&Player::HandlerTickThread, this);
  }
}

void Player::DestroyCubeb() {
  mHandlerThreadStop = true;
  if (mMixerThread) {
    ctr_thread_join(mMixerThread);
    mMixerThread = nullptr;
    ctr_audio_exit();
  }
  if (mHandlerThread.joinable()) {
    mHandlerThread.join();
  }
}

void* Player::MixerThreadEntry(void* self) {
  ((Player*)self)->MixerThread();
  return nullptr;
}

void Player::MixerThread() {
  ctr_thread_install_crash_handler();
  g_mixer_thread_id.store(ctr_thread_current_id(), std::memory_order_relaxed);
  u32 last_dropped = 0;
  u32 last_gap_us = 0;
  // The loop: while the DSP has a free buffer, mix it and queue it, then wait for the DSP's next
  // frame. It never sleeps with a buffer to make. What keeps it faster than the output is the
  // voice limit (MixBudget): the time of every buffer goes into it, and Tick() mutes the quietest
  // voices beyond the limit. A summary every ~10 s (480 buffers), in % of the audio made.
  constexpr double kBufferMs = 1000.0 * kMixFrames / kMixRate;
  constexpr u32 kBufferUs = (u32)(1000000ull * kMixFrames / kMixRate);
  double last_warn_ms = -1e9;
  double sum_mix_ms = 0.0, sum_queue_ms = 0.0;
  int sum_buffers = 0, sum_cuts = 0, sum_muted = 0, sum_limit = MixBudget::kNoLimit;
  while (!mHandlerThreadStop) {
    short* buf;
    while (!mHandlerThreadStop && (buf = ctr_audio_get_buffer()) != nullptr) {
      const u32 late_before = ctr_audio_late_buffers();
      mAudiblePeak = 0;
      mMutedPeak = 0;
      const u64 t0 = kperf::ticks();
      Tick((s16Output*)buf, kMixFrames);
      const u64 t1 = kperf::ticks();
      ctr_audio_submit(buf);
      const u64 t2 = kperf::ticks();
      const bool late = ctr_audio_late_buffers() != late_before;
      const double mix_ms = kperf::ticks_to_ms(t1 - t0);
      const double queue_ms = kperf::ticks_to_ms(t2 - t1);
      const int running = mAudiblePeak + mMutedPeak;
      const bool cut =
          mBudget.OnBuffer((u32)((mix_ms + queue_ms) * 1000.0), kBufferUs, late, mAudiblePeak);
      kperf::add_thread(kperf::Thread::SOUND, t1 - t0, 1);
      kperf::set_gauge(kperf::Gauge::SOUND_HANDLERS, mHandlerCount);
      sum_mix_ms += mix_ms;
      sum_queue_ms += queue_ms;
      sum_cuts += cut;
      sum_muted = std::max(sum_muted, mMutedPeak);
      sum_limit = std::min(sum_limit, mBudget.Limit());
      if (cut) {
        const double now = kperf::ticks_to_ms(t2);
        if (now - last_warn_ms > 5000.0) {
          lg::warn(
              "3DS sound: a buffer took {:.0f}% of its time to mix{} ({} sounds, {} voices): the "
              "{} loudest voices stay audible for now",
              100.0 * (mix_ms + queue_ms) / kBufferMs, late ? " and came late" : "", mHandlerCount,
              running, mBudget.Limit());
          last_warn_ms = now;
        }
      }
      if (++sum_buffers == 480) {
        const double handler_ms = kperf::ticks_to_ms(g_handler_ticks);
        g_handler_ticks = 0;
        const double real_ms = sum_buffers * kBufferMs;
        size_t sounds;
        int voices;
        u32 stolen, refused;
        {
          std::scoped_lock lock(mTickLock);
          sounds = mHandlers.size();
          voices = mSynth.VoiceCount();
          stolen = mVmanager.StolenVoices();
          refused = mVmanager.RefusedTones();
        }
        // late buffers: the DSP ran out of sound before the mixer's next buffer came (a gap),
        // which the dropped DSP frames don't show
        lg::info(
            "3DS sound: {:.0f}% of real time ({:.0f}% sound effect handlers, {:.0f}% voices), "
            "queueing {:.1f}%, {} sounds, {} voices; voice limit: {} cuts, lowest {}, at most {} "
            "muted; SPU voices: {} taken over, {} tones without one so far; {} DSP frames dropped "
            "so far, {} late buffers so far",
            100.0 * sum_mix_ms / real_ms, 100.0 * handler_ms / real_ms,
            100.0 * (sum_mix_ms - handler_ms) / real_ms, 100.0 * sum_queue_ms / real_ms, sounds,
            voices, sum_cuts,
            sum_limit >= MixBudget::kNoLimit ? std::string("none") : std::to_string(sum_limit),
            sum_muted, stolen, refused, ctr_audio_dropped_frames(), ctr_audio_late_buffers());
        sum_mix_ms = sum_queue_ms = 0.0;
        sum_buffers = sum_cuts = sum_muted = 0;
        sum_limit = mBudget.Limit();
      }
    }
    // the count reads 0 once the audio is closed for exit
    const u32 dropped = ctr_audio_dropped_frames();
    const u32 gap_us = ctr_audio_take_max_frame_gap_us();
    if (dropped > last_dropped) {
      lg::warn(
          "3DS sound: DSP dropped {} frames (its frame update came late; longest gap between "
          "ndsp frames just before {:.1f} ms; the mixer's late buffers so far {})",
          dropped - last_dropped, std::max(gap_us, last_gap_us) / 1000.0,
          ctr_audio_late_buffers());
      last_dropped = dropped;
    }
    last_gap_us = gap_us;
    ctr_audio_wait(20000);
  }
}

void Player::HandlerTickThread() {
  ctr_thread_set_priority(CTR_PRIO_IO);
  using clock = std::chrono::steady_clock;
  constexpr auto kTick = std::chrono::nanoseconds(1000000000 / 240);
  auto next = clock::now();
  // (AI-assisted) snd_RunStreamsWithoutOutput: Tick() makes the samples of 4 handler ticks (800
  // SPU ticks) per wakeup into this buffer, which nobody plays. Only the permanent (stream)
  // voices are in the synth then (VoiceManager drops the others without output), so it costs
  // only while a stream plays.
  static s16Output scratch[(800 * kMixRate + 47999) / 48000 + 1];
  u64 acc = 0;
  while (!mHandlerThreadStop) {
    const u64 t_start = kperf::ticks();
    if (mRunStreamsSilent.load(std::memory_order_relaxed)) {
      acc += 800ull * kMixRate;
      const int n = (int)(acc / 48000);
      acc -= (u64)n * 48000;
      Tick(scratch, n);
      kperf::add_thread(kperf::Thread::SOUND, kperf::ticks() - t_start, 1);
      kperf::set_gauge(kperf::Gauge::SOUND_HANDLERS, (u32)mHandlers.size());
      next += 4 * kTick;
      std::this_thread::sleep_until(next);
      continue;
    }
    // run 4 handler ticks per wakeup (60 Hz) to keep the thread cheap
    for (int i = 0; i < 4; i++) {
      std::scoped_lock lock(mTickLock);
      mTick++;
      for (auto it = mHandlers.begin(); it != mHandlers.end();) {
        bool done = it->second->Tick();
        if (done) {
          mHandleAllocator.FreeId(it->first);
          it = mHandlers.erase(it);
        } else {
          ++it;
        }
      }
    }
    kperf::add_thread(kperf::Thread::SOUND, kperf::ticks() - t_start, 1);
    kperf::set_gauge(kperf::Gauge::SOUND_HANDLERS, (u32)mHandlers.size());
    next += 4 * kTick;
    std::this_thread::sleep_until(next);
  }
}
#else
void Player::InitCubeb() {
#ifdef _WIN32
  HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  m_coinitialized = SUCCEEDED(hr);
  if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
    lg::error("Couldn't initialize COM");
    lg::error("Cubeb init failed");
    return;
  }
#endif

  cubeb_init(&mCtx, "OpenGOAL", nullptr);
  if (const char* e = getenv("OPENGOAL_NO_SOUND"); e && e[0] && e[0] != '0') {
    lg::info("[sound] OPENGOAL_NO_SOUND: the output is silence");  // (AI-assisted)
  }

  cubeb_stream_params outparam = {};
  outparam.channels = 2;
  outparam.format = CUBEB_SAMPLE_S16LE;
  outparam.rate = 48000;
  outparam.layout = CUBEB_LAYOUT_STEREO;
  outparam.prefs = CUBEB_STREAM_PREF_NONE;

  s32 err = 0;
  u32 latency = 0;
  err = cubeb_get_min_latency(mCtx, &outparam, &latency);
  if (err != CUBEB_OK) {
    lg::error("Cubeb init failed");
    return;
  }

  err = cubeb_stream_init(mCtx, &mStream, "OpenGOAL", nullptr, nullptr, nullptr, &outparam, latency,
                          &sound_callback, &state_callback, this);
  if (err != CUBEB_OK) {
    lg::error("Cubeb init failed");
    return;
  }

  err = cubeb_stream_start(mStream);
  if (err != CUBEB_OK) {
    lg::error("Cubeb init failed");
    return;
  }
}

void Player::DestroyCubeb() {
  cubeb_stream_stop(mStream);
  cubeb_stream_destroy(mStream);
  cubeb_destroy(mCtx);
#ifdef _WIN32
  if (m_coinitialized) {
    CoUninitialize();
    m_coinitialized = false;
  }
#endif
}

#endif  // __3DS__

#ifndef __3DS__
long Player::sound_callback([[maybe_unused]] cubeb_stream* stream,
                            void* user,
                            [[maybe_unused]] const void* input,
                            void* output_buffer,
                            long nframes) {
  ((Player*)user)->Tick((s16Output*)output_buffer, nframes);
  // (AI-assisted) OPENGOAL_NO_SOUND=1: the sound engine runs as usual (same timing), but the
  // output is silence (automated test runs on a PC whose speakers are in use)
  static const bool silent = [] {
    const char* e = getenv("OPENGOAL_NO_SOUND");
    return e && e[0] && e[0] != '0';
  }();
  if (silent) {
    // (AI-assisted) a line every ~10 s of audio: what was mixed before it was zeroed (tests
    // that need to know the sound engine made sound: the peak level and the share of non-zero
    // frames), the sounds and voices playing
    static u64 frames = 0, nonzero = 0;
    static int peak = 0, max_voices = 0;
    const s16* pcm = (const s16*)output_buffer;
    for (long i = 0; i < nframes; i++) {
      const int l = std::abs((int)pcm[2 * i]), r = std::abs((int)pcm[2 * i + 1]);
      peak = std::max(peak, std::max(l, r));
      nonzero += (l | r) != 0;
    }
    auto* self = (Player*)user;
    max_voices = std::max(max_voices, self->mSynth.VoiceCount());
    frames += nframes;
    if (frames >= 480000) {
      lg::info("[sound] mixed {:.1f} s: peak {}, {:.0f}% non-zero, {} sounds, up to {} voices",
               frames / 48000.0, peak, 100.0 * nonzero / frames, self->mHandlers.size(),
               max_voices);
      frames = nonzero = 0;
      peak = max_voices = 0;
    }
    memset(output_buffer, 0, (size_t)nframes * sizeof(s16Output));
  }
  return nframes;
}

void Player::state_callback([[maybe_unused]] cubeb_stream* stream,
                            [[maybe_unused]] void* user,
                            [[maybe_unused]] cubeb_state state) {}
#endif

void Player::Tick(s16Output* stream, int samples) {
#ifdef __3DS__
  // (AI-assisted) the same as below, but the synth mixes the samples between two handler ticks
  // at once, at kMixRate: spu = SPU ticks since the last handler tick, times kMixRate. The voices
  // are mixed without the lock (Synth::BeginBlock): a sound call waits only for the handler
  // ticks, not for the mixing, which on a core the system shares can be held up for milliseconds.
  static u64 spu = 200ull * kMixRate;
  const int audible =
      mBudget.Limit() >= MixBudget::kNoLimit ? Synth::kAllAudible : mBudget.Limit();
  for (int i = 0; i < samples;) {
    int n;
    {
      std::scoped_lock lock(mTickLock);
      if (spu >= 200ull * kMixRate) {
        const u64 th = kperf::ticks();
        struct HandlerTime {
          u64 t0;
          ~HandlerTime() { g_handler_ticks += kperf::ticks() - t0; }
        } handler_time{th};
        mTick++;
        for (auto it = mHandlers.begin(); it != mHandlers.end();) {
          bool done = it->second->Tick();
          if (done) {
            mHandleAllocator.FreeId(it->first);
            it = mHandlers.erase(it);
          } else {
            ++it;
          }
        }
        spu -= 200ull * kMixRate;
      }
      // output samples until the next handler tick
      // (AI-assisted) left <= 200 * kMixRate fits 32 bits: a 32-bit divide by a constant is a
      // multiply, the 64-bit one a libgcc call (the ARM11 has no divide instruction)
      const u32 left = (u32)(200ull * kMixRate - spu);
      n = (int)std::min<u32>((u32)(samples - i), (left + 47999u) / 48000u);
      mSynth.BeginBlock(audible);
      mAudiblePeak = std::max(mAudiblePeak, mSynth.RunningCount() - mSynth.MutedCount());
      mMutedPeak = std::max(mMutedPeak, mSynth.MutedCount());
      mHandlerCount = (u32)mHandlers.size();
      mMixing.store(true, std::memory_order_relaxed);
    }
    mSynth.Mix(stream + i, n, kTickRatio);
    mMixing.store(false, std::memory_order_release);
    {
      std::scoped_lock lock(mTickLock);
      mSynth.EndBlock();  // also publishes the voices' NAX, all at this moment
    }
    spu += (u64)n * 48000;
    i += n;
  }
#else
  std::scoped_lock lock(mTickLock);
  static int htick = 200;
  static int stick = 48000;
  for (int i = 0; i < samples; i++) {
    // The handlers expect to tick at 240hz
    // 48000/240 = 200
    if (htick == 200) {
      mTick++;

      for (auto it = mHandlers.begin(); it != mHandlers.end();) {
        bool done = it->second->Tick();
        if (done) {
          // fmt::print("erasing handler\n");
          mHandleAllocator.FreeId(it->first);
          it = mHandlers.erase(it);
        } else {
          ++it;
        }
      }

      htick = 0;
    }

    if (stick == 48000) {
      // fmt::print("{} handlers active\n", m_handlers.size());
      stick = 0;
    }

    stick++;
    htick++;
    *stream++ = mSynth.Tick();
  }
#endif
}

u32 Player::PlaySound(BankHandle bank_id,
                      u32 sound_id,
                      s32 vol,
                      s32 pan,
                      s32 pm,
                      s32 pb,
                      bool keira_voice) {
  std::scoped_lock lock(mTickLock);
  auto bank = mLoader.GetBankByHandle(bank_id);
  if (bank == nullptr) {
    lg::error("play_sound: Bank {} does not exist", static_cast<void*>(bank_id));
    return 0;
  }

  u32 handle = mHandleAllocator.GetId();
  std::optional<std::unique_ptr<SoundHandler>> handler;
  constexpr u32 kSBlk = 'S' | ('B' << 8) | ('l' << 16) | ((u32)'k' << 24);  // loader.cpp fourcc
  if (keira_voice && bank->DataID == kSBlk) {
    // (AI-assisted) as SoundBank::MakeHandler(vm, id, vol, pan, pm, pb, ...), with her voice
    SndPlayParams params{};
    params.vol = vol;
    params.pan = pan;
    params.pitch_mod = pm;
    params.pitch_bend = pb;
    params.keira_voice = true;
    handler = bank->MakeHandler(mVmanager, sound_id, -1, -1, params, GetTick(), handle);
  } else {
    handler = bank->MakeHandler(mVmanager, sound_id, vol, pan, pm, pb, GetTick(), handle);
  }
  if (!handler.has_value()) {
    return 0;
  }

  auto handler_to_stop = handler.value()->CheckInstanceLimit(mHandlers, vol, true);
  if (handler_to_stop) {
    handler_to_stop->Stop();
    if (handler_to_stop == handler.value().get()) {
      return 0;
    }
  }

  handler.value()->m_sound_handle = handle;
  mHandlers.emplace(handle, std::move(handler.value()));
  // fmt::print("play_sound {}:{} - {}\n", bank_id, sound_id, handle);

  return handle;
}

bool Player::SetKeiraVoice(BankHandle bank_id, u32 sound_id, u32 grain, s16 pm) {
  // (AI-assisted) see player.h
  std::scoped_lock lock(mTickLock);
  auto bank = mLoader.GetBankByHandle(bank_id);
  constexpr u32 kSBlk = 'S' | ('B' << 8) | ('l' << 16) | ((u32)'k' << 24);
  if (bank == nullptr || bank->DataID != kSBlk) {
    return false;
  }
  auto* block = static_cast<SFXBlock*>(bank);
  if (sound_id >= block->Sounds.size() || grain >= block->Sounds[sound_id].Grains.size()) {
    return false;
  }
  auto& g = block->Sounds[sound_id].Grains[grain];
  auto* tone = std::get_if<Tone>(&g.data);
  if (!tone || (g.Type != GrainType::TONE && g.Type != GrainType::TONE2)) {
    return false;
  }
  tone->KeiraPm = pm;
  return true;
}

void Player::DebugPrintAllSoundsInBank(BankHandle bank_id) {
  std::scoped_lock lock(mTickLock);
  auto* bank = mLoader.GetBankByHandle(bank_id);
  if (!bank) {
    lg::error("DebugPrintAllSoundsInBank: invalid bank");
    return;
  }
  bank->DebugPrintAllSounds();
}

u32 Player::PlaySoundByName(BankHandle bank_id,
                            char* bank_name,
                            char* sound_name,
                            s32 vol,
                            s32 pan,
                            s32 pm,
                            s32 pb) {
  std::scoped_lock lock(mTickLock);
  SoundBank* bank = nullptr;
  if (bank_id == 0 && bank_name != nullptr) {
    bank = mLoader.GetBankByName(bank_name);
  } else if (bank_id != 0) {
    bank = mLoader.GetBankByHandle(bank_id);
  } else {
    bank = mLoader.GetBankWithSound(sound_name);
  }

  if (bank == nullptr) {
    // lg::error("play_sound_by_name: failed to find bank for sound {}", sound_name);
    return 0;
  }

  auto sound = bank->GetSoundByName(sound_name);
  if (sound.has_value()) {
    return PlaySound(bank, sound.value(), vol, pan, pm, pb);
  }

  // lg::error("play_sound_by_name: failed to find sound {}", sound_name);

  return 0;
}

void Player::StopSound(u32 sound_id) {
  std::scoped_lock lock(mTickLock);
  auto handler = mHandlers.find(sound_id);
  if (handler == mHandlers.end())
    return;

  handler->second->Stop();

  // m_handle_allocator.free_id(sound_id);
  // m_handlers.erase(sound_id);
}

u32 Player::GetSoundID(u32 sound_handle) {
  std::scoped_lock lock(mTickLock);
  auto handler = mHandlers.find(sound_handle);
  if (handler == mHandlers.end())
    return -1;
  return handler->second->SoundID();
}

void Player::SetSoundReg(u32 sound_id, u8 reg, u8 value) {
  std::scoped_lock lock(mTickLock);
  if (mHandlers.find(sound_id) == mHandlers.end()) {
    // fmt::print("set_midi_reg: Handler {} does not exist\n", sound_id);
    return;
  }

  auto* handler = mHandlers.at(sound_id).get();
  handler->SetRegister(reg, value);
}

u8 Player::GetSoundGroup(u32 sound_id) {
  std::scoped_lock lock(mTickLock);
  auto handler = mHandlers.find(sound_id);
  if (handler == mHandlers.end()) {
    return 0;
  }
  return handler->second->Group();
}

bool Player::SoundStillActive(u32 sound_id) {
  std::scoped_lock lock(mTickLock);
  auto handler = mHandlers.find(sound_id);
  if (handler == mHandlers.end())
    return false;

  // fmt::print("sound_still_active {}\n", sound_id);
  return true;
}

void Player::SetMasterVolume(u32 group, s32 volume) {
  std::scoped_lock lock(mTickLock);
  if (volume > 0x400)
    volume = 0x400;

  if (volume < 0)
    volume = 0;

  if (group == 15)
    return;

  mVmanager.SetMasterVol(group, volume);

  // Master volume
  if (group == 16) {
    mSynth.SetMasterVol(0x3ffff * volume / 0x400);
  }
}

BankHandle Player::LoadBank(std::span<u8> bank) {
  // (AI-assisted) Parse outside the lock (it copies all the samples of the bank), install under
  // it: the mixer takes the lock between its blocks, so a long parse under it held up the audio.
  auto parsed = Loader::ParseBank(bank);
  std::scoped_lock lock(mTickLock);
  auto handle = mLoader.AddBank(std::move(parsed));
  if (handle) {
    mBankBytes[handle] = (u32)bank.size();
  }
  return handle;
}

BankHandle Player::LoadBank(std::span<u8> bank, std::unique_ptr<u8[]> owner) {
  auto parsed = Loader::ParseBank(bank, &owner);
  std::scoped_lock lock(mTickLock);
  auto handle = mLoader.AddBank(std::move(parsed));
  if (handle) {
    mBankBytes[handle] = (u32)bank.size();
  }
  return handle;
}

u32 Player::ResidentBankBytes(u32* count) {
  std::scoped_lock lock(mTickLock);
  u32 total = 0;
  for (auto& [h, n] : mBankBytes) {
    total += n;
  }
  if (count) {
    *count = (u32)mBankBytes.size();
  }
  return total;
}

void Player::UnloadBank(BankHandle bank_handle) {
  std::scoped_lock lock(mTickLock);
  auto* bank = mLoader.GetBankByHandle(bank_handle);
  if (bank == nullptr)
    return;

  for (auto it = mHandlers.begin(); it != mHandlers.end();) {
    if (&it->second->Bank() == bank_handle) {
      mHandleAllocator.FreeId(it->first);
      it = mHandlers.erase(it);
    } else {
      ++it;
    }
  }

#ifdef __3DS__
  // the voices being mixed may still read the bank's samples: their stops apply after the block
  while (mMixing.load(std::memory_order_acquire)) {
    ctr_thread_sleep_us(50);
  }
#endif
  mLoader.UnloadBank(bank_handle);
  mBankBytes.erase(bank_handle);
}

void Player::SetPanTable(VolPair* pantable) {
  std::scoped_lock lock(mTickLock);
  mVmanager.SetPanTable(pantable);
}

void Player::SetPlaybackMode(s32 mode) {
  std::scoped_lock lock(mTickLock);
  mVmanager.SetPlaybackMode(mode);
}

void Player::PauseSound(s32 sound_id) {
  std::scoped_lock lock(mTickLock);
  auto handler = mHandlers.find(sound_id);
  if (handler == mHandlers.end())
    return;

  handler->second->Pause();
}

void Player::ContinueSound(s32 sound_id) {
  std::scoped_lock lock(mTickLock);
  auto handler = mHandlers.find(sound_id);
  if (handler == mHandlers.end())
    return;

  handler->second->Unpause();
}

void Player::PauseAllSoundsInGroup(u8 group) {
  std::scoped_lock lock(mTickLock);

  for (auto& h : mHandlers) {
    if ((1 << h.second->Group()) & group) {
      h.second->Pause();
    }
  }
}

void Player::ContinueAllSoundsInGroup(u8 group) {
  std::scoped_lock lock(mTickLock);

  for (auto& h : mHandlers) {
    if ((1 << h.second->Group()) & group) {
      h.second->Unpause();
    }
  }
}

void Player::SetSoundVolPan(s32 sound_id, s32 vol, s32 pan) {
  std::scoped_lock lock(mTickLock);
  auto handler = mHandlers.find(sound_id);
  if (handler == mHandlers.end())
    return;

  handler->second->SetVolPan(vol, pan);
}

void Player::SetSoundPmod(s32 sound_handle, s32 mod) {
  std::scoped_lock lock(mTickLock);
  auto handler = mHandlers.find(sound_handle);
  if (handler == mHandlers.end())
    return;

  handler->second->SetPMod(mod);
}

void Player::SetSoundPBend(s32 sound_handle, s32 bend) {
  std::scoped_lock lock(mTickLock);
  auto handler = mHandlers.find(sound_handle);
  if (handler == mHandlers.end())
    return;

  handler->second->SetPBend(bend);
}

void Player::SetReverbMode(u32 mode) {
#ifdef __3DS__
  // nothing is heard without audio output (the stream voices still run silently)
  if (!g_output_active) {
    return;
  }
#endif
  std::scoped_lock lock(mTickLock);
  if (mode != mReverbMode) {
    mReverbMode = mode;
    lg::info("sound: SPU2 effect mode {}", mode);
  }
  mSynth.SetReverbMode(mode);
}

void Player::SetReverbDepth(s16 left, s16 right) {
#ifdef __3DS__
  if (!g_output_active) {
    return;
  }
#endif
  std::scoped_lock lock(mTickLock);
  if (left != mReverbDepthL || right != mReverbDepthR) {
    mReverbDepthL = left;
    mReverbDepthR = right;
    lg::info("sound: SPU2 effect depth {} {}", left, right);
  }
  mSynth.SetReverbDepth(left, right);
}

void Player::SetGroupVoiceRange(s32 group, s32 min, s32 max) {
  std::scoped_lock lock(mTickLock);
  mVmanager.SetGroupVoiceRange(group, min, max);
}

s32 Player::ExternVoiceAlloc(s32 group) {
  std::scoped_lock lock(mTickLock);
  return std::max(mVmanager.ExternVoiceAlloc(group), 0);
}

void Player::StopAllSounds() {
  std::scoped_lock lock(mTickLock);
  for (auto it = mHandlers.begin(); it != mHandlers.end();) {
    mHandleAllocator.FreeId(it->first);
    it = mHandlers.erase(it);
  }
}

s32 Player::GetSoundUserData(BankHandle block_handle,
                             char* block_name,
                             s32 sound_id,
                             char* sound_name,
                             SFXUserData* dst) {
  std::scoped_lock lock(mTickLock);
  SoundBank* bank = nullptr;
  if (block_handle == nullptr && block_name != nullptr) {
    bank = mLoader.GetBankByName(block_name);
  } else if (block_handle != nullptr) {
    bank = mLoader.GetBankByHandle(block_handle);
  } else {
    bank = mLoader.GetBankWithSound(sound_name);
  }

  if (bank == nullptr) {
    return 0;
  }

  if (sound_id == -1) {
    auto sound = bank->GetSoundByName(sound_name);
    if (sound.has_value()) {
      sound_id = sound.value();
    } else {
      return 0;
    }
  }

  auto ud = bank->GetSoundUserData(sound_id);
  if (ud.has_value()) {
    dst->data[0] = ud.value()->data[0];
    dst->data[1] = ud.value()->data[1];
    dst->data[2] = ud.value()->data[2];
    dst->data[3] = ud.value()->data[3];
    return 1;
  } else {
    return 0;
  }

  return 0;
}

}  // namespace snd
