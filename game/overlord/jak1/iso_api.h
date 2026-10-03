#pragma once

#include "game/overlord/common/isocommon.h"
#include "game/overlord/common/ssound.h"
struct SoundBank;

namespace jak1 {
struct VagDirEntry;
void LoadSoundBank(const char* bank_name, SoundBank* bank);
void LoadMusic(const char* music_name, snd::BankHandle* bank);

void QueueVAGStream(FileRecord* file, VagDirEntry* vag, u32 sound_id, u32 unk);
void PlayVAGStream(FileRecord* file,
                   VagDirEntry* vag,
                   u32 sound_id,
                   s32 volume,
                   u32 unk,
                   Vec3w* trans);
void SetVAGStreamVolume(s32 volume);
void SetDialogVolume(s32 volume);
void StopVAGStream(VagDirEntry* vag, u32 unk);
void PauseVAGStream();
void UnpauseVAGStream();
s32 LoadISOFileToIOP(FileRecord* file, void* addr, uint32_t length);
s32 LoadISOFileToEE(FileRecord* file, uint32_t ee_addr, uint32_t length);
s32 LoadISOFileChunkToEE(FileRecord* file, uint32_t dest_addr, uint32_t length, uint32_t offset);
#ifdef __3DS__
// (AI-assisted) and copy the loaded bytes to capture as well (room for length bytes)
s32 LoadISOFileChunkToEE(FileRecord* file,
                         uint32_t dest_addr,
                         uint32_t length,
                         uint32_t offset,
                         u8* capture);
#endif

}  // namespace jak1
