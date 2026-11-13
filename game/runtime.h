#pragma once

/*!
 * @file runtime.h
 * Setup and launcher for the runtime.
 */

#include <thread>

#include "common/common_types.h"
#include "common/versions/versions.h"

#include "game/common/game_common_types.h"
#include "game/kernel/common/kboot.h"
#include "system/background_worker.h"

extern u8* g_ee_main_mem;
// the bytes of it that exist (EE_MAIN_MEM_SIZE; less on the 3DS without a debug heap)
extern u32 g_ee_main_mem_size;
// executable view of g_ee_main_mem for GOAL calls
extern u8* g_ee_main_mem_exec;
// host tool (env OPENGOAL_MEM_TOUCH): a range about to be written by a system call
void ee_mem_touch(const void* p, size_t len);
bool ee_mem_touch_on();
extern GameVersion g_game_version;
extern BackgroundWorker g_background_worker;
extern int g_server_port;

RuntimeExitStatus exec_runtime(GameLaunchOptions game_options, int argc, const char** argv);

extern std::thread::id g_main_thread_id;
