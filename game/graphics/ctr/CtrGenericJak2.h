#pragma once

/*!
 * @file CtrGenericJak2.h
 * Jak II's generic merc buckets (gmerc-*, Generic2 NORMAL mode on the PC) and the warp bucket
 * (gmerc-warp: Warp, Generic2 WARP mode with the screen as the warp texture). The models whose
 * effects foreground.gc sends to generic merc are drawn here: the death dissolve (death-timer),
 * cross-fade, force-mercneric and warp-tpage effects (cloaked predators, warp gates).
 */

#include <memory>

#include "game/graphics/ctr/CtrRenderer.h"

std::unique_ptr<CtrBucketRenderer> ctr_make_generic_jak2(const char* name, int id, bool warp);
