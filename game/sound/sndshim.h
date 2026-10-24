
#pragma once
#include <memory>
#include <span>

#include "common/common_types.h"

#include "game/sound/989snd/plugin.h"

constexpr int SND_CORE_0 = 1;
constexpr int SND_CORE_1 = 2;
constexpr int SD_REV_MODE_OFF = 0;

struct SFXUserData {
  u32 data[4];
};

typedef void* (*AllocFun)();
typedef void (*FreeFun)(void*);

namespace snd {
class SoundBank;
using BankHandle = SoundBank*;
};  // namespace snd

void snd_StartSoundSystem();
void snd_StopSoundSystem();
// false when the sound system runs without audio output (3DS without the sound flag file or the
// DSP firmware): the SPU voices never advance then.
bool snd_HasOutput();
// (AI-assisted) 3DS: the overlord's SPU voice register writes (sdshim.cpp: key on / off, start and
// loop addresses; snd_keyOnVoiceRaw / snd_keyOffVoiceRaw) wait for the mixer between chunks. The
// mixer (another core) runs a voice a chunk at a time with its decoder state in registers
// (Voice::RunBlock), so a key on in the middle of it was undone or mixed with the old state: the
// stream's play address (NAX) became garbage. No effect elsewhere (the host mixes sample by
// sample).
void snd_LockVoiceWrites();
void snd_UnlockVoiceWrites();
s32 snd_GetTick();
void snd_RegisterIOPMemAllocator(AllocFun alloc, FreeFun free);
int snd_LockVoiceAllocator(bool block);
void snd_UnlockVoiceAllocator();
s32 snd_ExternVoiceAlloc(s32 vol_group, s32 priority);
u32 snd_SRAMMalloc(u32 size);
void snd_SRAMMarkUsed(u32 addr, u32 size);
void snd_SetMixerMode(s32 channel_mode, s32 reverb_mode);
void snd_SetGroupVoiceRange(s32 group, s32 min, s32 max);
void snd_SetReverbDepth(s32 core, s32 left, s32 right);
void snd_SetReverbType(s32 core, s32 type);
void snd_SetPanTable(s16* table);
void snd_SetPlayBackMode(s32 mode);
s32 snd_SoundIsStillPlaying(s32 sound_handle);
void snd_StopSound(s32 sound_handle);
u32 snd_GetSoundID(s32 sound_handle);
void snd_SetSoundVolPan(s32 sound_handle, s32 vol, s32 pan);
void snd_SetMasterVolume(s32 which, s32 volume);
void snd_UnloadBank(snd::BankHandle bank_handle);
void snd_ResolveBankXREFS();
void snd_ContinueAllSoundsInGroup(u8 groups);
void snd_PauseAllSoundsInGroup(u8 groups);
void snd_SetMIDIRegister(s32 handle, u8 reg, u8 value);
void snd_SetGlobalExcite(u8 value);

s32 snd_PlaySoundVolPanPMPB(snd::BankHandle bank_handle,
                            s32 sound_id,
                            s32 vol,
                            s32 pan,
                            s32 pitch_mod,
                            s32 pitch_bend);

// (AI-assisted) "play as Keira" (3DS easter egg): as snd_PlaySoundVolPanPMPB, the sound's tones
// that are Jak's voice (snd_SetKeiraVoice) a little higher
s32 snd_PlaySoundVolPanPMPBKeira(snd::BankHandle bank_handle,
                                 s32 sound_id,
                                 s32 vol,
                                 s32 pan,
                                 s32 pitch_mod,
                                 s32 pitch_bend);
// (AI-assisted) mark grain `grain` of sound `sound_id` (a tone) as Jak's voice: +pm (1/128
// semitone) when snd_PlaySoundVolPanPMPBKeira plays it. Returns 0 if it isn't a tone.
s32 snd_SetKeiraVoice(snd::BankHandle bank_handle, s32 sound_id, s32 grain, s32 pm);

s32 snd_PlaySoundByNameVolPanPMPB(snd::BankHandle bank_handle,
                                  char* bank_name,
                                  char* sound_name,
                                  s32 vol,
                                  s32 pan,
                                  s32 pitch_mod,
                                  s32 pitch_bend);

void snd_SetSoundPitchModifier(s32 sound_handle, s32 pitch_mod);
void snd_SetSoundPitchBend(s32 sound_handle, s32 pitch_bend);
void snd_PauseSound(s32 sound_handle);
void snd_ContinueSound(s32 sound_handle);
void snd_AutoPitch(s32 sound_handle, s32 pitch, s32 delta_time, s32 delta_from);
void snd_AutoPitchBend(s32 sound_handle, s32 bend, s32 delta_time, s32 delta_from);
snd::BankHandle snd_BankLoadEx(const char* filepath,
                               s32 data_offset,
                               u32 spu_mem_loc,
                               u32 spu_mem_size);
// (AI-assisted) Like snd_BankLoadEx, for a bank that is already in memory (the data from the
// offset on). The data is copied. Any thread (the parse runs outside the player's lock).
snd::BankHandle snd_BankLoadFromMemory(std::span<u8> data);
// (AI-assisted) The same for data in owner's buffer, which the bank keeps: its samples aren't
// copied (one piece of the heap of the file's size instead of two)
snd::BankHandle snd_BankLoadFromMemory(std::span<u8> data, std::unique_ptr<u8[]> owner);

void snd_BankLoadFromIOPPartialEx_Start();
void snd_BankLoadFromIOPPartialEx(const u8* data, u32 length, u32 spu_mem_loc, u32 spu_mem_size);
snd::BankHandle snd_BankLoadFromIOPPartialEx_Completion();

s32 snd_GetVoiceStatus(s32 voice);
s32 snd_GetFreeSPUDMA();
void snd_FreeSPUDMA(s32 channel);
void snd_keyOnVoiceRaw(u32 core, u32 voice);
void snd_keyOffVoiceRaw(u32 core, u32 voice);
s32 snd_GetSoundUserData(snd::BankHandle block_handle,
                         char* block_name,
                         s32 sound_id,
                         char* sound_name,
                         SFXUserData* dst);
void snd_SetSoundReg(s32 sound_handle, s32 which, u8 val);
s8 snd_GetSoundGroup(s32 sound_handle);
void snd_RegisterPluginHandler(snd::PluginHandler handler);
