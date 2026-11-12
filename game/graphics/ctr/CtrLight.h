#pragma once

// A time of day vertex group (CtrLevel.cpp tod_*).
#include "common/common_types.h"

struct CtrLightGroup {
  u16 lo, hi;  // vertices [lo, hi]
  u32 ver : 31;
  u32 plain : 1;  // every vertex has a time-of-day color
  u32 dver : 24;
  u32 lver = 0;  // its light vertices hold the compact table of this version (0: not known)
};
