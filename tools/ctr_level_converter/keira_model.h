#pragma once

/*!
 * @file keira_model.h
 * (AI-assisted)
 * The "play as Keira" easter egg's model (game/graphics/ctr/CtrKeiraRig.h): the level KEIRA.c3l is
 * converted from.
 */

#include "common/custom_data/Tfrag3Data.h"

namespace keira_model {

/*!
 * A level holding only Keira's model from village1 (assistant-lod0), named "keira-as-" + Jak's
 * (eichar-lod0, from the common GAME.fr3), with its textures:
 * - her vertices bound to the rig's joints (matrix slot = rig joint + 1: her own joints, and her
 *   upper chest's share of her chest weights on the virtual upper chest joint),
 * - one effect (the game enables Jak's effect 0 for the model, the others are his), no blend shapes
 *   (the game sends Jak's face weights) and no envmap (it sends Jak's fades),
 * - her eye draws on Jak's eye slots (the game draws his eyes, blinking with him).
 */
bool make_level(const tfrag3::Level& game, const tfrag3::Level& village1, tfrag3::Level* out);

}  // namespace keira_model
