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
 *   continue NAME  (start 'play (get-continue-by-name *game-info* NAME)): jump to a checkpoint,
 *             for example "continue beach-start" (tests of other levels)
 *   teleport TYPE JOINT DY  (AI-assisted, tests of moving platforms) put Jak, at rest, on the
 *             first surface below the point DY meters above joint JOINT (node-list index) of the
 *             process-drawable of type TYPE whose joint is nearest to him, for example
 *             "teleport minecartsteel 4 2" (onto the load of a Volcanic Crater minecart,
 *             platform/3ds/tests/minecart.pad). goal_src/jak1/pc/progress-pc.gc pad-script-teleport.
 *   jpos TYPE JOINT  (AI-assisted) log where that joint (of the nearest such process) and Jak
 *             are, in meters, Jak's state, and how many riders the process found in its last
 *             detect-riders! (-1: not a platform) (pad-script-jpos)
 *   save      an autosave, as after a power cell (pad-script-auto-save, 3DS)
 *   tasks A B (AI-assisted) close every stage of game-tasks A..B (close-specific-task!, as if
 *             they were done: the progress menu's power cell pages then show those cells
 *             collected) and set *cheat-mode* to #t (the menu opens every level's page)
 *   progress N (AI-assisted) open the progress menu at progress-screen N, or switch the open one
 *             to it (pad-script-progress, goal_src/jak1/pc/progress-pc.gc; progress-h.gc has the
 *             numbers: 23 memcard-no-data, 7 memcard-no-space, 29 auto-save, 55 music-player ...),
 *             for pictures of screens a test can't reach otherwise
 *   tod H     (AI-assisted) set the game's clock to hour H (0..24, fractions are minutes):
 *             (set-time-of-day H), for time of day pictures (it keeps running from there)
 *   shot [NAME]  (AI-assisted) a screenshot (PC OpenGL renderer: like F2, into the config
 *             folder's screenshots; for reference pictures to compare the 3DS renderer with). The
 *             3DS renderer: NAME.bmp (3DS) / NAME.png (--ctr-gfx) in its screenshot folder
 *             (config.ini screenshots = N / --ctr-dump).
 *   set KEY VALUE  (AI-assisted) the 3DS renderer: switch a gpu_profile = ab key (or fog) on/off
 *             from here on, e.g. "set fog_alpha on" (A/B pictures of one view)
 *   call FN ARGS...  (AI-assisted) call the GOAL function in symbol FN with up to 8 arguments:
 *             an integer, 'SYM (the symbol), $SYM (its value), "TEXT" (a new string, no spaces)
 *             or fN (a float), and log the result, e.g. "call close-specific-task! 112 1" or
 *             "call auto-save-command 'restore 0 0 $*default-pool*" (tests of the game's end,
 *             save/load). Pseudo functions: "call poke32 BASE OFFSET VALUE" (a 32-bit write
 *             to GOAL memory: "call poke32 $*game-info* 92 f100" = 100 power cells), "call pgo
 *             TYPE STATE" (the process of TYPE nearest to Jak goes to STATE, like go-process;
 *             @N: the virtual state in method N of its type), "call pevent TYPE MSG ARGS..."
 *             (send it an event), "call artctl" (log the spooled animation buffers and the
 *             IOP's stream clock: stuck cutscenes), "call heap" (the global heap's free space)
 *   pstate TYPE  (AI-assisted) log the state and position of the process of type TYPE
 *             nearest to Jak (bosses: "pstate robotboss")
 *   exit      stop the runtime
 *   handoff   stop the script: the real controller plays from here
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
#include <vector>

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

//! Called for "continue NAME".
using ContinueHook = void (*)(const char* name);
void set_continue_hook(ContinueHook hook);

//! (AI-assisted) Called for "call FN ARGS..." and "pstate TYPE".
using CallHook = void (*)(const std::string& fn, const std::vector<std::string>& args);
void set_call_hook(CallHook hook);
using ProcStateHook = void (*)(const char* type);
void set_proc_state_hook(ProcStateHook hook);

//! Called for "exit".
using ExitHook = void (*)();
void set_exit_hook(ExitHook hook);

//! (AI-assisted) Called for "save": an autosave, as after a power cell (tests the save path).
using SaveHook = void (*)();
void set_save_hook(SaveHook hook);

//! (AI-assisted) Called for "teleport" (teleport true) and "jpos" (teleport false).
using JointHook = void (*)(const char* type, int joint, float dy, bool teleport);
void set_joint_hook(JointHook hook);

//! (AI-assisted) Called for "tasks A B".
using TasksHook = void (*)(int first, int last);
void set_tasks_hook(TasksHook hook);

//! (AI-assisted) Called for "progress N".
using ProgressHook = void (*)(int screen);
void set_progress_hook(ProgressHook hook);

//! (AI-assisted) Called for "tod H".
using TodHook = void (*)(float hour);
void set_tod_hook(TodHook hook);

//! (AI-assisted) The name of the last "shot" given to the hook below ("" before the first one).
const char* last_shot_name();

//! (AI-assisted) Called for "shot".
using ShotHook = void (*)();
void set_shot_hook(ShotHook hook);

//! (AI-assisted) Called for "shot [NAME]" instead of the hook above when set (the 3DS renderer:
//! a screenshot named NAME, default shot_f<frame>).
using NamedShotHook = void (*)(const char* name);
void set_named_shot_hook(NamedShotHook hook);

//! (AI-assisted) Called for "set KEY VALUE" (the 3DS renderer: a gpu_profile = ab switch or fog
//! changed mid-run, for A/B screenshots of one view). Returns false for an unknown key.
using SetHook = bool (*)(const char* key, const char* value);
void set_set_hook(SetHook hook);

}  // namespace pad_script
