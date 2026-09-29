#pragma once

/*!
 * @file null.h
 * Null renderer: draws nothing, but paces vsync at 60 Hz (which also drives the IOP vblank
 * callback). Used for headless runs (gk --null-gfx) and the 3DS port bring-up.
 * (AI-assisted)
 */

#include "game/graphics/gfx.h"

extern const GfxRendererModule gRendererNull;
