#include "ksound.h"

#include "game/kernel/common/kdgo.h"
#include "game/kernel/common/ksound.h"
#include "game/kernel/jak1/kscheme.h"

namespace jak1 {
/*!
 * Set up some functions which are somewhat related to sound.
 */
void InitSoundScheme() {
  make_function_symbol_from_c("rpc-call", RpcCall_wrapper);
  make_function_symbol_from_c("rpc-busy?", RpcBusy);
  make_function_symbol_from_c("test-load-dgo-c", LoadDGOTest);
  make_stack_arg_function_symbol_from_c("rpc-call", RpcCall_wrapper);

  // PC port interns
  make_function_symbol_from_c("pc-sound-set-flava-hack", set_flava_hack);
  make_function_symbol_from_c("pc-sound-set-fade-hack", set_fade_hack);
}
}  // namespace jak1