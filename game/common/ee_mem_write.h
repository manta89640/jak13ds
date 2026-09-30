#pragma once

/*!
 * @file ee_mem_write.h
 * (AI-assisted)
 * Writes from the IOP thread into EE (GOAL) memory: DMA and RPC results. Same as memcpy, but the
 * mips2c verify mode (game/mips2c/mips2c_native.cpp), which snapshots and restores all of GOAL
 * memory while it checks some functions, keeps track of them.
 */

#include "common/common_types.h"

void iop_write_ee_mem(void* dst, const void* src, u32 size);
