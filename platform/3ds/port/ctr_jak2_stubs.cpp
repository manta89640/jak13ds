/*!
 * @file ctr_jak2_stubs.cpp
 * (AI-assisted) Definitions the Jak II runtime takes from the PC renderer, which the 3DS build
 * (OG3DS_GAME=jak2) doesn't have.
 */

#include <mutex>

//! game/graphics/opengl_renderer/foreground/Merc2.cpp: guards the merc blend shape data between
//! the game (mips2c blerc-execute) and the renderer. Only the game uses it here.
std::mutex g_merc_data_mutex;
