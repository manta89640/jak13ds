#include "srpc.h"

#include <algorithm>
#include <chrono>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "iso_api.h"
#include "ramdisk.h"

#include "common/log/log.h"
#include "common/util/Assert.h"
#include "common/versions/versions.h"

#include "game/common/game_common_types.h"
#include "game/common/loader_rpc_types.h"
#include "game/common/player_rpc_types.h"
#include "game/graphics/gfx.h"
#include "game/overlord/common/iso.h"
#include "game/overlord/common/sbank.h"
#include "game/overlord/common/soundcommon.h"
#include "game/overlord/common/srpc.h"
#include "game/overlord/common/ssound.h"
#include "game/overlord/jak1/iso.h"
#include "game/runtime.h"
#include "game/sce/iop.h"
#include "game/sound/sndshim.h"

#include "fmt/format.h"
#include "third-party/magic_enum.hpp"

using namespace iop;

namespace jak1 {

// (AI-assisted) "play as Keira" (3DS easter egg, goal_src/jak1/engine/sound/gsound.gc): the
// sounds the player plays as Keira come with bit 15 of their mask. Jak's voice in them is played
// kKeiraVoicePm higher: his grunts and yells sit around 300-380 Hz, Keira's speech (her hint
// lines, ASSTLP* / ASSTV* in VAGWAD) around 350 Hz, so +4 semitones (x1.26) puts the efforts
// where hers would be. Only the voice: the whooshes, hits and splashes layered with it in the same
// sound keep their pitch. The game has no recordings of Keira's efforts (no Keira sound in any
// bank; her VAG lines are whole sentences), so her voice is Jak's, retuned at the voice level:
// no new data, nothing streamed (as fast and as reliable as his).
constexpr s16 kKeiraVoicePm = 4 * 128;
constexpr u16 kKeiraSoundMask = 0x8000;
struct KeiraVoice {
  const char* sound;  // in the common bank
  u32 grains;         // the grains that are Jak's voice (tones), bit per grain
};
// from the common bank's samples (Jak's voice: the samples only these sounds use, voiced, not the
// layers they share with the impact and whoosh sounds); grain numbers as in the bank
constexpr KeiraVoice kKeiraVoices[] = {
    {"punch", 0x1f8},          {"uppercut", 0x18},       {"spin-kick", 0x38},
    {"flop-down", 0x7c},       {"jump-double", 0x1c},    {"jump-long", 0x7c},
    {"jak-idle1", 0x7},        {"oof", 0xfc},            {"hit-back", 0x7c},
    {"hit-up", 0x7c},          {"land-hard", 0x3f8},     {"smack-surface", 0x3ffc},
    {"grunt", 0x7e},           {"hand-grab", 0x7e},      {"get-powered", 0x7e},
    {"flop-land", 0x38},       {"jak-deatha", 0x2},      {"death-melt", 0x3},
    {"death-darkeco", 0xc},    {"death-drown", 0x1},     {"death-fall", 0x1},
    {"jak-stretch", 0x1},      {"jak-shocked", 0x2},     {"get-shocked", 0x1},
    {"shield-zap", 0x1},
};
constexpr int kKeiraVoiceCount = sizeof(kKeiraVoices) / sizeof(kKeiraVoices[0]);
// the common bank's sound index of each (-1: not there), and the tones marked
static SoundBank* gKeiraBank = nullptr;
static s32 gKeiraIndex[kKeiraVoiceCount];
static u8 gKeiraTones[kKeiraVoiceCount];

static bool sound_name_is(const SoundRecord& r, const char* name) {
  return !strncmp(r.name.data(), name, 16);
}
static bool sound_name_is_bank(const SoundBank* b, const char* name) {
  return !strncmp(b->name.data(), name, 16);
}

void KeiraMarkVoices(SoundBank* bank) {
  if (!bank || !bank->bank_handle || !sound_name_is_bank(bank, "common")) {
    return;
  }
  gKeiraBank = bank;
  int tones = 0, sounds = 0;
  for (int v = 0; v < kKeiraVoiceCount; v++) {
    gKeiraIndex[v] = -1;
    gKeiraTones[v] = 0;
    for (u32 i = 0; i < bank->sound_count && i < bank->sound.size(); i++) {
      if (sound_name_is(bank->sound[i], kKeiraVoices[v].sound)) {
        gKeiraIndex[v] = (s32)i;
        break;
      }
    }
    if (gKeiraIndex[v] < 0) {
      continue;
    }
    for (int g = 0; g < 32; g++) {
      if ((kKeiraVoices[v].grains >> g) & 1) {
        gKeiraTones[v] += snd_SetKeiraVoice(bank->bank_handle, gKeiraIndex[v], g, kKeiraVoicePm);
      }
    }
    tones += gKeiraTones[v];
    sounds += gKeiraTones[v] > 0;
  }
  lg::info("[keira] common bank: Jak's voice in {} tones of {} sounds (+{} semitones when the "
           "player is Keira)",
           tones, sounds, kKeiraVoicePm / 128);
}

constexpr int SRPC_MESSAGE_SIZE = 0x50;
constexpr int SRPC_MESSAGE_COUNT = 128;
static uint8_t gLoaderBuf[SRPC_MESSAGE_SIZE];
static uint8_t gPlayerBuf[SRPC_MESSAGE_SIZE * SRPC_MESSAGE_COUNT];
static u32 gInfoEE = 0;  // EE address where we should send info on each frame.
s16 gFlava;
u32 gFreeMem = 0;

static SoundIopInfo info;

s32 gVAG_Id = 0;  // TODO probably doesn't belong here.

// english, french, germain, spanish, italian, japanese, uk.
const char* languages[] = {"ENG", "FRE", "GER", "SPA", "ITA", "JAP", "UKE"};

void srpc_init_globals() {
  memset((void*)gLoaderBuf, 0, sizeof(gLoaderBuf));
  memset((void*)gPlayerBuf, 0, sizeof(gPlayerBuf));
  gSoundEnable = 1;
  gInfoEE = 0;
  gLanguage = languages[(int)Language::English];
}

void* RPC_Player(unsigned int fno, void* data, int size);

u32 Thread_Player() {
  sceSifQueueData dq;
  sceSifServeData serve;

  // set up RPC
  CpuDisableIntr();
  sceSifInitRpc(0);
  sceSifSetRpcQueue(&dq, GetThreadId());
  sceSifRegisterRpc(&serve, PLAYER_RPC_ID[g_game_version], RPC_Player, gPlayerBuf,
                    SRPC_MESSAGE_SIZE * SRPC_MESSAGE_COUNT, nullptr, nullptr, &dq);
  CpuEnableIntr();
  sceSifRpcLoop(&dq);
  return 0;
}

void* RPC_Loader(unsigned int fno, void* data, int size);

u32 Thread_Loader() {
  sceSifQueueData dq;
  sceSifServeData serve;

  // set up RPC
  CpuDisableIntr();
  sceSifInitRpc(0);
  sceSifSetRpcQueue(&dq, GetThreadId());
  sceSifRegisterRpc(&serve, LOADER_RPC_ID[g_game_version], RPC_Loader, gLoaderBuf,
                    SRPC_MESSAGE_SIZE, nullptr, nullptr, &dq);
  CpuEnableIntr();
  sceSifRpcLoop(&dq);
  return 0;
}

void* RPC_Player(unsigned int /*fno*/, void* data, int size) {
  if (gSoundEnable) {
    gFreeMem = QueryTotalFreeMemSize();
    if (!PollSema(gSema)) {
      if (gMusic) {
        if (!gMusicPause && !LookupSound(666)) {
          Sound* music = AllocateSound(false);
          if (music != nullptr) {
            gMusicFade = 0;
            gMusicFadeDir = 1;
            SetMusicVol();
            music->sound_handle = snd_PlaySoundVolPanPMPB(gMusic, 0, 0x400, -1, 0, 0);
            music->id = 666;
            music->is_music = 1;
          }
        }
      }
      SignalSema(gSema);
    }

    SetMusicVol();
    Sound* music = LookupSound(666);
    if (music != nullptr) {
      snd_SetSoundVolPan(music->sound_handle, 0x7FFFFFFF, 0);
      snd_SetMIDIRegister(music->sound_handle, 0, gFlava);
    }

    int n_messages = size / SRPC_MESSAGE_SIZE;
    SoundRpcCommand* cmd = (SoundRpcCommand*)(data);
    while (n_messages > 0) {
      switch (cmd->j1command) {
        case Jak1SoundCommand::PLAY: {
          if (cmd->play.sound_id == 0) {
            break;
          }
          if (!memcmp(cmd->play.name, "spool-", 6)) {
            char namebuf[16];
            const char* name = &cmd->play.name[6];
            size_t len = strlen(name);
            if (len < 9) {
              memset(namebuf, ' ', 8);
              memcpy(namebuf, name, len);
            } else {
              memcpy(namebuf, name, 8);
            }

            // ASCII toupper
            for (int i = 0; i < 8; i++) {
              if (namebuf[i] >= 0x61 && namebuf[i] < 0x7b) {
                namebuf[i] -= 0x20;
              }
            }

            auto vagfile = FindVAGFile(namebuf);

            memcpy(namebuf, "VAGWAD  ", 8);
            strcpy(&namebuf[8], gLanguage);

            FileRecord* rec = isofs->find_in(namebuf);
            if (vagfile != nullptr) {
              if (cmd->play.parms.pitch_mod) {  // ??? TODO Verify what is being checked here
                PlayVAGStream(rec, vagfile, cmd->play.sound_id, cmd->play.parms.volume, 0,
                              &cmd->play.parms.trans);
              } else {
                PlayVAGStream(rec, vagfile, cmd->play.sound_id, cmd->play.parms.volume, 0, nullptr);
              }
            }
            break;
          }

          SoundBank* bank = nullptr;
          s32 index = LookupSoundIndex(cmd->play.name, &bank);
          if (index < 0) {
            break;
          }

          Sound* sound = LookupSound(cmd->play.sound_id);
          if (sound) {
            memcpy(&sound->params, &cmd->play.parms, sizeof(sound->params));
            sound->bank_entry = &bank->sound[index];
            sound->is_music = 0;
            if ((sound->params.mask & 0x40) == 0) {
              sound->params.fo_min = sound->bank_entry->fallof_params & 0x3fff;
            }
            if ((sound->params.mask & 0x80) == 0) {
              sound->params.fo_max = (sound->bank_entry->fallof_params >> 14) & 0x3fff;
            }
            if ((sound->params.mask & 0x100) == 0) {
              sound->params.fo_curve = sound->bank_entry->fallof_params >> 28;
            }
            UpdateVolume(sound);
            snd_SetSoundPitchModifier(sound->sound_handle, cmd->play.parms.pitch_mod);
            snd_SetSoundPitchBend(sound->sound_handle, cmd->play.parms.bend);
            break;
          }

          sound = AllocateSound(false);
          if (!sound) {
            break;
          }
          memcpy(&sound->params, &cmd->play.parms, sizeof(sound->params));
          sound->bank_entry = &bank->sound[index];
          sound->is_music = 0;
          sound->auto_time = 0;

          if ((sound->params.mask & 0x40) == 0) {
            sound->params.fo_min = sound->bank_entry->fallof_params & 0x3fff;
          }
          if ((sound->params.mask & 0x80) == 0) {
            sound->params.fo_max = (sound->bank_entry->fallof_params >> 14) & 0x3fff;
          }
          if ((sound->params.mask & 0x100) == 0) {
            sound->params.fo_curve = sound->bank_entry->fallof_params >> 28;
          }
          s32 vol = GetVolume(sound);
          s32 pan = GetPan(sound);
          s32 handle;
          if ((sound->params.mask & kKeiraSoundMask) && bank == gKeiraBank) {
            // (AI-assisted) "play as Keira": her voice in the player's sound (see kKeiraVoicePm)
            handle = snd_PlaySoundVolPanPMPBKeira(bank->bank_handle, index, vol, pan,
                                                  sound->params.pitch_mod, sound->params.bend);
#ifndef __3DS__
            // host check (env CTR_KEIRA_VOICE_LOG): which of the player's sounds have her voice
            static const bool voice_log = getenv("CTR_KEIRA_VOICE_LOG") != nullptr;
            if (voice_log) {
              for (int v = 0; v < kKeiraVoiceCount; v++) {
                if (gKeiraIndex[v] == index) {
                  lg::info("[keira] voice {}: {} tones +{} semitones (handle {})",
                           kKeiraVoices[v].sound, gKeiraTones[v], kKeiraVoicePm / 128, handle);
                }
              }
            }
#endif
          } else {
            handle = snd_PlaySoundVolPanPMPB(bank->bank_handle, index, vol, pan,
                                             sound->params.pitch_mod, sound->params.bend);
          }
          sound->sound_handle = handle;
          if (sound->sound_handle) {
            sound->id = cmd->play.sound_id;
          }
        } break;
        case Jak1SoundCommand::PAUSE_SOUND: {
          Sound* sound = LookupSound(cmd->sound_id.sound_id);
          if (sound != nullptr) {
            snd_PauseSound(sound->sound_handle);
          } else if (cmd->sound_id.sound_id == (u32)gVAG_Id) {
            PauseVAGStream();
          }
        } break;
        case Jak1SoundCommand::STOP_SOUND: {
          Sound* sound = LookupSound(cmd->sound_id.sound_id);
          if (sound != nullptr) {
            snd_StopSound(sound->sound_handle);
          } else if (cmd->sound_id.sound_id == (u32)gVAG_Id) {
            StopVAGStream(nullptr, 0);
          }
        } break;
        case Jak1SoundCommand::CONTINUE_SOUND: {
          Sound* sound = LookupSound(cmd->sound_id.sound_id);
          if (sound != nullptr) {
            snd_ContinueSound(sound->sound_handle);
          } else if (cmd->sound_id.sound_id == (u32)gVAG_Id) {
            UnpauseVAGStream();
          }
        } break;
        case Jak1SoundCommand::SET_PARAM: {
          Sound* sound = LookupSound(cmd->sound_id.sound_id);
          u32 mask = cmd->param.parms.mask;
          if (sound != nullptr) {
            if (mask & 1) {
              if (mask & 0x10) {
                sound->auto_time = cmd->param.auto_time;
                sound->new_volume = cmd->param.parms.volume;
              } else {
                sound->params.volume = cmd->param.parms.volume;
              }
            }
            if (mask & 0x20) {
              sound->params.trans = cmd->param.parms.trans;
            }
            if (mask & 0x21) {
              UpdateVolume(sound);
            }
            if (mask & 2) {
              sound->params.pitch_mod = cmd->param.parms.pitch_mod;
              if (mask & 0x10) {
                snd_AutoPitch(sound->sound_handle, sound->params.pitch_mod, cmd->param.auto_time,
                              cmd->param.auto_from);
              } else {
                snd_SetSoundPitchModifier(sound->sound_handle, cmd->param.parms.pitch_mod);
              }
            }
            if (mask & 4) {
              sound->params.bend = cmd->param.parms.bend;
              if (mask & 0x10) {
                snd_AutoPitchBend(sound->sound_handle, sound->params.bend, cmd->param.auto_time,
                                  cmd->param.auto_from);
              } else {
                snd_SetSoundPitchBend(sound->sound_handle, cmd->param.parms.bend);
              }
            }

          } else if (cmd->sound_id.sound_id == (u32)gVAG_Id) {
            SetVAGStreamVolume(cmd->param.parms.volume);
          }
        } break;
        case Jak1SoundCommand::SET_MASTER_VOLUME: {
          u32 group = cmd->master_volume.group.group;
          for (int i = 0; i < 32; i++) {
            if (((group >> i) & 1) != 0) {
              if (i == 1) {
                gMusicVol = cmd->master_volume.volume;
              } else if (i == 2) {
                SetDialogVolume(cmd->master_volume.volume);
              } else {
                snd_SetMasterVolume(i, cmd->master_volume.volume);
              }
            }
          }
        } break;
        case Jak1SoundCommand::PAUSE_GROUP: {
          snd_PauseAllSoundsInGroup(cmd->group.group);
          if ((cmd->group.group & 4) != 0) {
            PauseVAGStream();
          }
          if (cmd->group.group & 2) {
            gMusicPause = 1;
          }
        } break;
        case Jak1SoundCommand::STOP_GROUP: {
          u8 group = cmd->group.group;
          KillSoundsInGroup(group);
          if ((group & 4) != 0) {
            StopVAGStream(nullptr, 0);
          }
        } break;
        case Jak1SoundCommand::CONTINUE_GROUP: {
          snd_ContinueAllSoundsInGroup(cmd->group.group);
          if (cmd->group.group & 4) {
            UnpauseVAGStream();
          }

          if (cmd->group.group & 2) {
            gMusicPause = 0;
          }
        } break;
        case Jak1SoundCommand::SET_FALLOFF_CURVE: {
          SetCurve(cmd->fallof_curve.curve, cmd->fallof_curve.falloff, cmd->fallof_curve.ease);
        } break;
        case Jak1SoundCommand::SET_SOUND_FALLOFF: {
          SoundBank* bank;
          s32 idx = LookupSoundIndex(cmd->fallof.name, &bank);
          if (idx >= 0) {
            bank->sound[idx].fallof_params =
                (cmd->fallof.curve << 28) | (cmd->fallof.max << 14) | cmd->fallof.min;
          }
        } break;
        case Jak1SoundCommand::SET_FLAVA: {
          gFlava = cmd->flava.flava;
        } break;
        case Jak1SoundCommand::SET_EAR_TRANS: {
          SetEarTrans(&cmd->ear_trans.ear_trans, &cmd->ear_trans.ear_trans,
                      &cmd->ear_trans.cam_trans, cmd->ear_trans.cam_angle);
        } break;
        case Jak1SoundCommand::SHUTDOWN: {
          gSoundEnable = 0;
          snd_StopSoundSystem();
          // TODO ShutdownFilingSystem();
        } break;
        default: {
          ASSERT_MSG(false, fmt::format("Unhandled RPC Player command {}",
                                        magic_enum::enum_name(cmd->j1command)));
        } break;
      }
      n_messages--;
      cmd++;
    }
  }
  return nullptr;
}

void* RPC_Loader(unsigned int /*fno*/, void* data, int size) {
  int n_messages = size / SRPC_MESSAGE_SIZE;
  SoundRpcCommand* cmd = (SoundRpcCommand*)(data);
  if (gSoundEnable) {
    // I don't think it should be possible to have > 1 message here - the buffer isn't big enough.
    if (n_messages > 1) {
      ASSERT(false);
    }
    while (n_messages > 0) {
      switch (cmd->j1command) {
        case Jak1SoundCommand::LOAD_BANK: {
          // see if it's already loaded
          auto bank = LookupBank(cmd->load_bank.bank_name);
          if (!bank) {
            // see if we have room to load another
            auto new_bank = AllocateBank();
            if (new_bank) {
              // we do!
              LoadSoundBank(cmd->load_bank.bank_name, new_bank);
            }
          }
        } break;
        case Jak1SoundCommand::UNLOAD_BANK: {
          SoundBank* bank = LookupBank(cmd->load_bank.bank_name);
          if (bank != nullptr) {
            snd::BankHandle id = bank->bank_handle;
            bank->bank_handle = 0;
            snd_UnloadBank(id);
            snd_ResolveBankXREFS();
          }
        } break;
        case Jak1SoundCommand::GET_IRX_VERSION: {
          cmd->irx_version.major = IRX_VERSION_MAJOR;
          cmd->irx_version.minor = IRX_VERSION_MINOR;
          gInfoEE = cmd->irx_version.ee_addr;
          return cmd;
        } break;
        case Jak1SoundCommand::RELOAD_INFO: {
          ReloadBankInfo();
        } break;
        case Jak1SoundCommand::SET_LANGUAGE: {
          gLanguage = languages[cmd->set_language.langauge_id];
          printf("IOP language: %s\n", gLanguage);  // added.
        } break;
        case Jak1SoundCommand::LOAD_MUSIC: {
          while (WaitSema(gSema))
            ;
          if (gMusic) {
            gMusicFadeDir = -1;
            while (gMusicFade) {
#ifdef __3DS__
              // (AI-assisted) the fade moves once a game frame (VBlank_Handler): no need to
              // wake up the IOP every millisecond to look
              DelayThread(10000);
#else
              DelayThread(1000);
#endif
            }
            snd_UnloadBank(gMusic);
            snd_ResolveBankXREFS();
            gMusic = nullptr;
          }
          LoadMusic(cmd->load_bank.bank_name, &gMusic);
          SignalSema(gSema);
        } break;
        case Jak1SoundCommand::LIST_SOUNDS: {
          PrintActiveSounds();
        } break;
        case Jak1SoundCommand::UNLOAD_MUSIC: {
          while (WaitSema(gSema))
            ;
          if (gMusic) {
            gMusicFadeDir = -1;
            while (gMusicFade) {
#ifdef __3DS__
              // (AI-assisted) the fade moves once a game frame (VBlank_Handler): no need to
              // wake up the IOP every millisecond to look
              DelayThread(10000);
#else
              DelayThread(1000);
#endif
            }
            snd_UnloadBank(gMusic);
            snd_ResolveBankXREFS();
            gMusic = 0;
          }
          SignalSema(gSema);
        } break;
        case Jak1SoundCommand::MIRROR_MODE: {
          gMirrorMode = cmd->mirror.value;
        } break;
        default:
          ASSERT_MSG(false,
                     fmt::format("Unhandled RPC Loader command {} ({})",
                                 magic_enum::enum_name(cmd->j1command), (int)cmd->j1command));
      }
      n_messages--;
      cmd++;
    }
  }
  return nullptr;
}

static s32 dmaid = 0;

s32 VBlank_Handler(void*) {
  if (!gSoundEnable)
    return 1;

#ifdef __3DS__
  // (AI-assisted) This handler runs once per game frame (30 fps or less), not at 60 Hz, so the
  // music fades took twice as long or more (the fade-out is 128 steps: 4.3 s at 30 fps, longer
  // while a level loads). A music load waits for the fade-out in the loader RPC (LOAD_MUSIC), and
  // a sound bank load the game asks for meanwhile stalls the game until it's done ("STALL:
  // waiting for IOP on RPC port #1"). Steps by real time, 60 a second as on the PS2.
  s32 fade_steps = 1;
  {
    static u64 last_us = 0;
    static u64 rest = 0;
    const u64 now_us = (u64)std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    if (last_us) {
      rest += (now_us - last_us) * 60;
      fade_steps = (s32)std::min<u64>(rest / 1000000, 128);
      rest %= 1000000;
    }
    last_us = now_us;
  }
#else
  constexpr s32 fade_steps = 1;
#endif

  if (gMusicFadeDir > 0) {
    gMusicFade += fade_steps * (0x10000 / 64);
    if (gMusicFade > 0x10000 || (gMusicFadeHack & 1)) {
      gMusicFade = 0x10000;
      gMusicFadeDir = 0;
    }
  } else if (gMusicFadeDir < 0) {
    gMusicFade -= fade_steps * (0x10000 / 128);
    if (gMusicFade < 0 || (gMusicFadeHack & 2)) {
      gMusicFade = 0;
      gMusicFadeDir = 0;
    }
  }

  if (!gInfoEE)
    return 1;

  gFrameNum++;

#ifdef __3DS__
  // The 3DS has no audio output, so every stream runs on the fake clock (iso.cpp). This handler
  // runs once per game frame, not at 60 Hz: advance the clock by real time (1024 per second), or
  // cutscenes would slow down with the frame rate.
  {
    static u64 last_us = 0;
    static u64 rest_us = 0;
    const u64 now_us = (u64)std::chrono::duration_cast<std::chrono::microseconds>(
                           std::chrono::steady_clock::now().time_since_epoch())
                           .count();
    if (gFakeVAGClockRunning && !gFakeVAGClockPaused && last_us) {
      rest_us += (now_us - last_us) * 1024;
      gFakeVAGClock += (s32)(rest_us / 1000000);
      rest_us %= 1000000;
    }
    last_us = now_us;
  }
#else
  if (gFakeVAGClockRunning && !gFakeVAGClockPaused) {
    gFakeVAGClock += (s32)(1024 / Gfx::g_global_settings.target_fps);
  }
#endif

  // We don't need this, our DMA's are instant
  // if (dmaid) {
  //  if (sceSifDmaStat(dmaid) >= 0) {
  //    return 1;
  //  }
  //  dmaid = 0;
  //}

  info.frame = gFrameNum;
  info.strpos = GetVAGStreamPos();
  info.std_id = gVAG_Id;
  info.freemem = gFreeMem;
  info.freemem2 = gMemFreeAtStart;
  // info.nocd = gNoCD;
  // info.dirtycd = gDirtyCD;
  info.nocd = 0;
  info.dirtycd = 0;
  // info.diskspeed[0] = gDiskSpeed[0];
  // info.diskspeed[1] = gDiskSpeed[1];
  // info.lastspeed = gLastSpeed;
  // info.dupseg = gDupSeg;

  for (int i = 0; i < 48; i++) {
    if (snd_GetVoiceStatus(i) == 1) {
      info.chinfo[i] = -1;
    } else {
      info.chinfo[i] = 0;
    }
  }

  sceSifDmaData dma;
  dma.data = &info;
  dma.addr = (void*)(uintptr_t)gInfoEE;
  dma.size = 0x110;
  dma.mode = 0;
  dmaid = sceSifSetDma(&dma, 1);

  return 1;
}
}  // namespace jak1
