/*!
 * @file other_games.cpp
 * (AI-assisted)
 * TEMPORARY link stubs for Jak 2 / Jak 3 / Jak X symbols that game/runtime.cpp and
 * game/mips2c/mips2c_table.cpp reference unconditionally. The 3DS build is Jak 1 only.
 * Init functions are no-ops (runtime.cpp calls them for every game); everything that would
 * actually run another game aborts.
 * Remove once those files guard the other games (docs/3ds-port/3ds_build.md).
 */

#include <cstdio>
#include <cstdlib>

#include "common/common_types.h"

namespace {
[[noreturn]] void not_built(const char* what) {
  fprintf(stderr, "%s: this game is not part of the 3DS build\n", what);
  abort();
}
}  // namespace

namespace jak2 {
void dma_init_globals() {}
void iso_init_globals() {}
void vag_init_globals() {}
void kdgo_init_globals() {}
void srpc_init_globals() {}
void kboot_init_globals() {}
void iso_cd_init_globals() {}
void ssound_init_globals() {}
void stream_init_globals() {}
void klisten_init_globals() {}
void kscheme_init_globals() {}
void iso_queue_init_globals() {}
void init_globals_streamlist() {}
void spusstreams_init_globals() {}
u32 u32_in_fixed_sym(u32) {
  not_built("jak2::u32_in_fixed_sym");
}
u64 alloc_heap_object(u32, u32, u32, u32) {
  not_built("jak2::alloc_heap_object");
}
int start_overlord_wrapper(int, const char* const*, bool*) {
  not_built("jak2::start_overlord_wrapper");
}
s32 goal_main(int, const char* const*) {
  not_built("jak2::goal_main");
}
}  // namespace jak2

namespace jak3 {
void kdgo_init_globals() {}
void kboot_init_globals() {}
void klisten_init_globals() {}
void kscheme_init_globals() {}
u32 u32_in_fixed_sym(u32) {
  not_built("jak3::u32_in_fixed_sym");
}
u64 alloc_heap_object(u32, u32, u32, u32) {
  not_built("jak3::alloc_heap_object");
}
int start_overlord_wrapper(bool*) {
  not_built("jak3::start_overlord_wrapper");
}
s32 goal_main(int, const char* const*) {
  not_built("jak3::goal_main");
}
}  // namespace jak3

namespace jakx {
u32 u32_in_fixed_sym(u32) {
  not_built("jakx::u32_in_fixed_sym");
}
s32 goal_main(int, const char* const*) {
  not_built("jakx::goal_main");
}
}  // namespace jakx
