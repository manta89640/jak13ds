#pragma once

/*!
 * @file pad_script.h
 * (AI-assisted)
 * Scripted controller input for automated tests: a timeline of pad states, applied to controller 0
 * instead of the real input (3DS buttons, or the PC input manager).
 *
 * Enabled by the OPENGOAL_PAD_SCRIPT environment variable (a file path). On the 3DS, gk sets it
 * when sdmc:/3ds/jak1/pad_script.txt exists.
 *
 * Format: one entry per line, "<frame> <items...>", '#' starts a comment. Frames count the game's
 * reads of controller 0 (one per game frame), from the first one. An entry sets the pad state from
 * its frame until the next entry. Items:
 *   buttons   x/cross circle square triangle start select l1 r1 l2 r2 l3 r3 up down left right
 *   sticks    lx=N ly=N rx=N ry=N (0..255, 128 = centered, y grows downwards)
 *   -         release everything (same as an entry with no items)
 *   print SYM print the value of a GOAL symbol to the log (for example: print *target*)
 *   wait SYM STATE  stop the script's clock (with all buttons released) until the process in SYM
 *             is in STATE, for example "wait *target* target-stance". Gives up after 30000 game
 *             frames. Frames of later entries don't count the waiting time.
 *   pos SYM   log the position (in meters) of the process-drawable in SYM (pos *target*)
 *   log TEXT  write TEXT to the log
 *   exit      stop the runtime
 *   crash     fail an assert (to test the crash screen)
 * Buttons and sticks not mentioned are released / centered.
 *
 * Example (title screen, start a new game, walk forward and jump):
 *   600  start
 *   606  -
 *   1500 ly=0
 *   1600 ly=0 x
 *   1605 ly=0
 *   1700 -
 *   1701 print *target*
 */

#include <string>

#include "common/common_types.h"

namespace pad_script {

struct PadState {
  u16 buttons = 0;  // PadData::ButtonIndex bits
  u8 lx = 128, ly = 128, rx = 128, ry = 128;
};

//! Is a script active? (loads it on the first call)
bool active();

//! Advance one frame and return the state for it. Runs the commands of entries starting now.
PadState next_frame();

//! Called for "print SYM" (set by the game kernel).
using PrintSymbolHook = void (*)(const char* symbol);
void set_print_symbol_hook(PrintSymbolHook hook);

//! Called for "wait": the name of the state of the process in a symbol ("" if none).
using StateNameHook = std::string (*)(const char* symbol);
void set_state_name_hook(StateNameHook hook);

//! Called for "pos": the position of the process-drawable in a symbol, false if none.
using PositionHook = bool (*)(const char* symbol, float* xyz);
void set_position_hook(PositionHook hook);

//! Called for "exit".
using ExitHook = void (*)();
void set_exit_hook(ExitHook hook);

}  // namespace pad_script
