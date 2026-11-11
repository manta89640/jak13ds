#pragma once

/*!
 * @file CtrHot.h
 * (AI-assisted, hottext) CTR_HOT: a function the game runs every frame, gathered with the other hot
 * code on the 3DS (CMake option OG3DS_HOT_TEXT). devkitARM's 3dsx.ld places every input's plain
 * .text before every .text.* section, and with -ffunction-sections each function has its own
 * .text.<name>: a function in plain .text lands in one block at the start of the program with the
 * hot functions of every other object (GOAL modules' GC_HOT, goal_c_ops.h) instead of wherever
 * its file is among ~18 MB of code: fewer instruction cache, TLB and L2 misses across them. GCC
 * also optimizes `hot` functions harder. Elsewhere it does nothing. Not for inline functions or
 * templates (their COMDAT sections).
 */

#if defined(__3DS__) && defined(__GNUC__) && !defined(__clang__) && defined(OG3DS_HOT_TEXT)
#define CTR_HOT __attribute__((hot, section(".text")))
#else
#define CTR_HOT
#endif
