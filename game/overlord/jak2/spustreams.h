#pragma once
#include "game/overlord/jak2/iso.h"

namespace jak2 {
void spusstreams_init_globals();
void WakeSpuStreamsUp();
void InitSpuStreamsThread();
int ProcessVAGData(CmdHeader* param_1_in, Buffer* param_2);
void StopVagStream(VagCmd* param_1, int param_2);
u32 GetSpuRamAddress(VagCmd* param_1);
// (AI-assisted) the VAG stream log (spustreams.cpp): OPENGOAL_VAG_DEBUG=1 on the host,
// -DOG_VAG_DEBUG in a 3DS build
bool vag_debug_on();
void VagDebugEvent(const char* what, VagCmd* c);
double vag_debug_ms();
}  // namespace jak2
