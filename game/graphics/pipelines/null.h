#pragma once

/*!
 * @file null.h
 * Null renderer: draws nothing, but paces vsync at 60 Hz (which also drives the IOP vblank
 * callback). Used for headless runs (gk --null-gfx) and the 3DS port bring-up.
 * OPENGOAL_NULL_GFX_FPS=N (host) paces at N Hz instead (30: the 3DS's frame rate cap, so the 3DS
 * GOAL code runs with time ratio 2).
 * (AI-assisted)
 */

#include "game/graphics/gfx.h"

extern const GfxRendererModule gRendererNull;

//! (AI-assisted) Profiling: OPENGOAL_NULL_GFX_NO_SYNC=<file>: while that file exists, vsync and the
//! kernel loop do not wait (frames run back to back; the game sees a time ratio of 1). Checked
//! every 64 calls. Host only.
bool null_gfx_no_sync();
