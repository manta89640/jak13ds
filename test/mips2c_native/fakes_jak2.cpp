/*!
 * @file fakes_jak2.cpp
 * The Jak 2 symbol table of the harness for Jak 2's mips2c code (jak2_functions/*.cpp): a symbol's
 * value is at its address - 1, so a symbol is at the harness's value address + 1 (sym_val_addr in
 * harness.cpp gives the same value addresses).
 */

#include "harness.h"

#include "game/kernel/jak2/kscheme.h"

namespace jak2 {
Ptr<Symbol4<u32>> intern_from_c(const char* name) {
  return Ptr<Symbol4<u32>>(harness::sym(name) + 1);
}
}  // namespace jak2

namespace Mips2C::jak2::native2 {
u32 sym_value_addr(const char* name) {
  return harness::sym(name);
}
}  // namespace Mips2C::jak2::native2

// sky.cpp's link functions register their host state for verify mode (not linked here)
namespace Mips2C {
void native_verify_host_state(void*, u32, const char*) {}
}  // namespace Mips2C
