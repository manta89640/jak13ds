#pragma once

/*!
 * @file CtrButtonArt.h
 * (AI-assisted)
 * New 3DS button symbols for the 3DS renderer, drawn procedurally when the textures that show the
 * PS2 buttons are decoded from the GS VRAM copy (CtrVram). The game files stay as they are.
 *
 * - The font: the game's text draws a PS2 button as four glyphs on top of each other (the disc '<',
 *   the symbol '*' ';' '@' '#', the rim '>' and the shine '['; common/util/font/dbs/font_db_jak1.cpp
 *   <PAD_X> etc.). In both font atlases (ascii.12lo, ascii.24lo) the symbol cells get the whole
 *   3DS button (B, X, A, Y: by position, as platform/3ds/port/ctr_port.c maps them) and the other
 *   three cells are cleared. The game's font colors for the symbols are white on the 3DS
 *   (font-h.gc), so the button keeps its own colors.
 * - The progress menu's button icons (textures headhud-button-01..04 of the Hud page): the game
 *   tells where they are in VRAM (pc-3ds-button-texture, progress-draw.gc) and they get the same
 *   art.
 *
 * Colors as on the New 3DS (small model): A red, B yellow, X blue, Y green, white letters.
 * Textures keep the GS alpha range (0x80 = opaque).
 */

#include "common/common_types.h"

namespace ctr_button_art {

//! The PS2 buttons the game draws, in the order of the progress menu's icon textures
//! (headhud-button-01..04: cross, square, triangle, circle).
enum class Ps2Button : int { CROSS = 0, SQUARE = 1, TRIANGLE = 2, CIRCLE = 3, COUNT = 4 };

//! config.ini button_art (default on): off draws the PS2 symbols.
void set_enabled(bool on);
bool enabled();

//! The game thread: the texture at VRAM block tbp0 (format psm) shows this PS2 button.
void register_texture(int ps2_button, u32 tbp0, u32 psm);

//! Changes when a registration changes (CtrVram then decodes its textures again).
u32 generation();

//! Is this (a TEX0 value, or the texture part of one) a texture patch() may change?
bool may_patch(u64 tex0);

//! Patch a texture decoded from VRAM (RGBA8, GS alpha, w x h, row-major) if it is a font atlas or
//! a registered button texture. Returns true when it changed the texels.
bool patch(u64 tex0, u32* rgba, int w, int h);

}  // namespace ctr_button_art
