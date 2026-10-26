#pragma once

/*!
 * @file fake_iso.h
 * This provides an implementation of IsoFs for reading a "fake iso".
 * A "fake iso" is just a map file which maps 8.3 ISO file names to files in the source folder.
 * This way we don't need to actually create an ISO.
 *
 * The game has this compilation unit, but there is nothing in it. Probably it is removed to save
 * IOP memory and was only included on TOOL-only builds.  So this is my interpretation of how it
 * should work.
 */

#include "isocommon.h"

// (AI-assisted) block-compressed X.ogz files in the fake ISO (compressed_iso_file.h): always on
// the 3DS; on the host the code is built in, but used only with OPENGOAL_ISO_OGZ=1 (fake_iso.cpp)
#if defined(__3DS__) || defined(OG_HOST_ISO_OGZ)
#define OG_ISO_OGZ 1
#endif

#include "third-party/BS_thread_pool.hpp"

void fake_iso_init_globals();
int fake_iso_FS_Init();
const char* get_file_path(FileRecord* fr);
// (AI-assisted) get_file_path is a block-compressed .ogz (compressed_iso_file.h): only the fake
// ISO's own reads decode it
bool is_file_compressed(FileRecord* fr);
FileRecord* FS_Find(const char* name);
FileRecord* FS_FindIN(const char* iso_name);
#ifdef __3DS__
// (AI-assisted) FS_FindIN for a file that may not be there: no error line (an SD card write)
FileRecord* FS_FindINQuiet(const char* iso_name);
#endif
uint32_t FS_GetLength(FileRecord* fr);
void LoadMusicTweaks();
extern u32 fake_iso_entry_count;

extern BS::thread_pool thpool;
