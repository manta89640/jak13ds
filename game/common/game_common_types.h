#pragma once

#include "common/listener_common.h"
#include "common/versions/versions.h"

//! When set, the runtime only contains Jak 1 (the 3DS build). Code for the other games in
//! runtime.cpp and mips2c_table.cpp is compiled out.
#if !defined(OPENGOAL_ONLY_JAK1)
#if defined(__3DS__)
#define OPENGOAL_ONLY_JAK1 1
#else
#define OPENGOAL_ONLY_JAK1 0
#endif
#endif
//! (AI-assisted) When set, Jak 3 and Jak X are compiled out (the Jak II 3DS build: Jak 1 and 2).
#if !defined(OPENGOAL_NO_JAK3)
#define OPENGOAL_NO_JAK3 OPENGOAL_ONLY_JAK1
#endif

//! Supported languages.
enum class Language {
  English = 0,
  French = 1,
  German = 2,
  Spanish = 3,
  Italian = 4,
  Japanese = 5,
  UK_English = 6,
  Portuguese = 9
};

struct GameLaunchOptions {
  GameVersion game_version = GameVersion::Jak1;
  bool disable_display = false;
  int server_port = DECI2_PORT;
};
