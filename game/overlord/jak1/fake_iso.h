#pragma once
#include "game/overlord/jak1/isocommon.h"

namespace jak1 {
void fake_iso_init_globals();
extern IsoFs fake_iso;

#ifdef __3DS__
// (AI-assisted) 3DS additions to the fake ISO (fake_iso.cpp).
// config.ini io_cache (default on): files kept open between loads, read-ahead, spool chunk cache
bool io_cache_on();
// free malloc heap in bytes (the caches only grow while there is plenty)
u32 io_heap_free();
// After an open: what the load reads. end: the file offset it stops at (0: to the end of the
// file); stream: a spool chunk or a voice stream.
void FS_SetReadHint(LoadStackEntry* fd, uint32_t end, bool stream);
// Sound bank and music loads (LOAD_SOUND_BANK / LOAD_MUSIC messages) on a reader thread:
// FS_StartSoundLoad takes the message, FS_FinishSoundLoad gives it back once the bank is loaded
// (nullptr: none finished yet), FS_SoundLoadPending while one runs or waits.
void FS_StartSoundLoad(IsoMessage* msg);
IsoMessage* FS_FinishSoundLoad();
bool FS_SoundLoadPending();
// a spool chunk or voice stream read is running, or one ended less than quiet_ms ago (any
// thread; the save worker waits for these, kmemcard.cpp)
bool FS_StreamBusy(u32 quiet_ms);
#endif
}  // namespace jak1
