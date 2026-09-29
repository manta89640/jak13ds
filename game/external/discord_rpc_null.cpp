/*!
 * @file discord_rpc_null.cpp
 * (AI-assisted)
 * No-op implementation of the discord-rpc C API for platforms without it (3DS). The OpenGOAL side
 * (discord.cpp, discord_jak1.cpp) is compiled unchanged on top of this.
 */

#include "third-party/discord-rpc/include/discord_rpc.h"

extern "C" {
void Discord_Initialize(const char*, DiscordEventHandlers*, int, const char*) {}
void Discord_Shutdown(void) {}
void Discord_RunCallbacks(void) {}
void Discord_UpdateConnection(void) {}
void Discord_UpdatePresence(const DiscordRichPresence*) {}
void Discord_ClearPresence(void) {}
void Discord_Respond(const char*, int) {}
void Discord_UpdateHandlers(DiscordEventHandlers*) {}
}
