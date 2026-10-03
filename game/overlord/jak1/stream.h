#pragma once

#include "common/common_types.h"

namespace jak1 {
u32 STRThread();
u32 PLAYThread();
void stream_init_globals();
#ifdef __3DS__
// (AI-assisted) free the spool chunk cache (a level load starts)
void stream_chunk_cache_clear();
#endif
}  // namespace jak1