#pragma once

/*!
 * @file ramdisk.cpp
 * A RAMDISK RPC for storing files in the extra RAM left over on the IOP.
 * Also called "Server".
 */

#include "common/common_types.h"

namespace jak1 {
extern u32 gMemFreeAtStart;

void ramdisk_init_globals();
// (AI-assisted, j2-mem2) with_memory = false: no RAMDISK memory (Jak II: its vis is never in the
// IOP, update-vis! says "ramdisk vis ... not supported", and nothing else loads into it). A load
// into it then fails like a full RAMDISK (the RPC server keeps running).
void InitRamdisk(bool with_memory = true);
u32 Thread_Server();
}  // namespace jak1