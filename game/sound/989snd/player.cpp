// Copyright: 2021 - 2024, Ziemas
// SPDX-License-Identifier: ISC
#include "player.h"

#include <chrono>
#include <fstream>

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
// The DSP resamples our 48 kHz stereo stream to its own rate. 1024 frames = 21.3 ms per buffer,
// 3 buffers = 64 ms of latency at most; the mixer wakes at every DSP frame (~5 ms) and refills
// whatever is done.
static constexpr unsigned kMixRate = 48000;
static constexpr unsigned kMixFrames = 1024;
static constexpr unsigned kMixBuffers = 3;
// The mixer takes mTickLock per chunk, not per buffer: a sound call waits for one chunk at most
// (128 frames: 2.7 ms of audio, mixed in about that long on hardware with many sounds playing).
static constexpr unsigned kMixChunk = 128;
static_assert(kMixFrames % kMixChunk == 0);

// the mixer thread (ctr_thread_current_id): its own waits for the lock aren't counted
static std::atomic<unsigned> g_mixer_thread_id{0};

void TickLock::lock() {
  if (m_mutex.try_lock()) {
    return;
  }
  m_waiters.fetch_add(1, std::memory_order_relaxed);
  const u64 t0 = kperf::ticks();
  m_mutex.lock();
  m_waiters.fetch_sub(1, std::memory_order_relaxed);
  if (ctr_thread_current_id() != g_mixer_thread_id.load(std::memory_order_relaxed)) {
    kperf::add_thread(kperf::Thread::SOUND_LOCK_WAIT, kperf::ticks() - t0, 1);
  }
}

void Player::InitCubeb() {
  mHandlerThreadStop = false;
  g_output_active = false;
  int core = 1;
  if (ctr_sound_config(&core)) {
    if (ctr_audio_init(kMixRate, kMixFrames, kMixBuffers) == 0) {
      // pinned: the mixer has the highest priority, so on the game's core (ctr_thread_create's
      // fallback when the app got no share of core 1) it would take the CPU from the game, all of
      // it once mixing falls behind (the game stops). No audio instead.
      int err = ctr_thread_create_pinned(&Player::MixerThreadEntry, this, 128 * 1024,
                                         CTR_PRIO_SOUND, core, &mMixerThread);
      if (err == 0) {
        g_output_active = true;
        lg::info("3DS sound: mixer thread on core {} (48 kHz software mix -> DSP)", core);
      } else {
        lg::error("3DS sound: no mixer thread on core {} ({}), no audio", core,
                  err == -2 ? "the app got no share of core 1" : "thread not created");
        ctr_audio_exit();
      }
    } else {
      lg::error("3DS sound: DSP not available (sdmc:/3ds/dspfirm.cdc?), no audio");
    }
  } else {
    lg::info("3DS sound: off (no sdmc:/3ds/jak1/sound flag file)");
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
  while (!mHandlerThreadStop) {
    short* buf;
    while (!mHandlerThreadStop && (buf = ctr_audio_get_buffer()) != nullptr) {
      const u64 t_start = kperf::ticks();
      for (unsigned done = 0; done < kMixFrames; done += kMixChunk) {
        Tick((s16Output*)buf + done, kMixChunk);
        // let waiting sound calls have the lock (see TickLock), for up to 2 ms
        for (int w = 0; w < 40 && mTickLock.contended(); w++) {
          ctr_thread_sleep_us(50);
        }
      }
      ctr_audio_submit(buf);
      kperf::add_thread(kperf::Thread::SOUND, kperf::ticks() - t_start, 1);
      kperf::set_gauge(kperf::Gauge::SOUND_HANDLERS, (u32)mHandlers.size());
    }
    u32 dropped = ctr_audio_dropped_frames();
    if (dropped != last_dropped) {
      lg::warn("3DS sound: DSP dropped {} frames (mixer late)", dropped - last_dropped);
      last_dropped = dropped;
    }
    ctr_audio_wait(20000);
  }
}

void Player::HandlerTickThread() {
  ctr_thread_set_priority(CTR_PRIO_IO);
  using clock = std::chrono::steady_clock;
  constexpr auto kTick = std::chrono::nanoseconds(1000000000 / 240);
  auto next = clock::now();
  while (!mHandlerThreadStop) {
    const u64 t_start = kperf::ticks();
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
  return nframes;
}

void Player::state_callback([[maybe_unused]] cubeb_stream* stream,
                            [[maybe_unused]] void* user,
                            [[maybe_unused]] cubeb_state state) {}
#endif

void Player::Tick(s16Output* stream, int samples) {
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
}

u32 Player::PlaySound(BankHandle bank_id, u32 sound_id, s32 vol, s32 pan, s32 pm, s32 pb) {
  std::scoped_lock lock(mTickLock);
  auto bank = mLoader.GetBankByHandle(bank_id);
  if (bank == nullptr) {
    lg::error("play_sound: Bank {} does not exist", static_cast<void*>(bank_id));
    return 0;
  }

  u32 handle = mHandleAllocator.GetId();
  auto handler = bank->MakeHandler(mVmanager, sound_id, vol, pan, pm, pb, GetTick(), handle);
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
  std::scoped_lock lock(mTickLock);
  return mLoader.BankLoad(bank);
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

  mLoader.UnloadBank(bank_handle);
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
