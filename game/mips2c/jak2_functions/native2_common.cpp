/*!
 * @file native2_common.cpp
 * (AI-assisted)
 * Helpers of the Jak 2 natives in native2_*.cpp (native2.h).
 */

#include "native2.h"

#include "game/kernel/jak2/kscheme.h"

namespace Mips2C::jak2::native2 {
u32 sym_value_addr(const char* name) {
  return ::jak2::intern_from_c(name).offset - 1;
}
}  // namespace Mips2C::jak2::native2
