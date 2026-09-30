#include "kboot.h"

#include <cstring>

// Set to 1 to kill GOAL kernel
RuntimeExitStatus MasterExit;

// Set to 1 to load game engine after boot automatically
u32 DiskBoot;

// Set to 1 to enable debug heap
u32 MasterDebug;

// added in pc port for splash screen
Timer SplashTimer;
u32 SplashScreen;

// Set to 1 to load debug code
u32 DebugSegment;

u32 MasterUseKernel;

// Level to load on boot
char DebugBootLevel[64];

// Pass to GOAL kernel on boot
char DebugBootMessage[64];

// game configuration
MasterConfig masterConfig;

void kboot_init_globals_common() {
  MasterExit = RuntimeExitStatus::RUNNING;
  DiskBoot = 0;
  MasterDebug = 1;
  DebugSegment = 1;
  MasterUseKernel = 1;
#ifdef __3DS__
  // (AI-assisted) The 3DS renderer never draws the splash (Gfx::g_splash): InitVideo would read
  // the image and then wait 3 seconds with nothing on the screen.
  SplashScreen = 0;
#else
  SplashScreen = 1;
#endif
  strcpy(DebugBootLevel, "#f");      // no specified level
  strcpy(DebugBootMessage, "play");  // play mode, the default retail mode
  memset(&masterConfig, 0, sizeof(MasterConfig));
}