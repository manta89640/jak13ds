#pragma once

/*!
 * @file stack_watch.h
 * Stack depth measurement of the GOAL process threads in C mode (debugging, off by default:
 * config.ini stack_watch = on on the 3DS, OPENGOAL_STACK_WATCH=1 elsewhere).
 *
 * Compiled C frames of the process threads live on *dram-stack* in GOAL memory. Temporary threads
 * (trans and post hooks) keep their cpu-thread at stack-top minus the game's PROCESS_STACK_SIZE,
 * so a hook that goes deeper overwrites it. The stack is filled with a pattern; after each run
 * the depth it wrote is found (from the top down, until a gap of untouched words) and the stack
 * is filled again. The deepest run per process type and thread name is logged every 30 s.
 */

#include <string>

#include "common/common_types.h"

namespace stack_watch {

struct Config {
  u32 stack_top = 0;   // GOAL address of the top of *dram-stack*
  u32 stack_size = 0;  // its size
  u32 temp_size = 0;   // PROCESS_STACK_SIZE: the temporary cpu-thread is at stack_top - temp_size
  //! "type/thread-name" of a run, for the log
  std::string (*name)(u32 type, u32 thread_name) = nullptr;
};

//! is the watch on (read once)
bool on();
//! the game's layout (once, before the first after_run)
void configure(const Config& config);
bool configured();
//! a run of a thread with this stack top returned to the kernel (type: the process's type,
//! thread_name: the thread's name symbol)
void after_run(u32 stack_top, u32 type, u32 thread_name);

}  // namespace stack_watch
