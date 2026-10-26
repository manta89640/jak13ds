/*!
 * @file native_common.cpp
 * (AI-assisted)
 * Helpers for Jak 2's native versions of mips2c functions (native_functions.h).
 */

#include "game/kernel/jak2/kscheme.h"
#include "game/mips2c/jak2_functions/native_functions.h"

namespace Mips2C::jak2::native {

u32 sym_val_addr(const char* name) {
  return ::jak2::intern_from_c(name).offset - 1;
}

}  // namespace Mips2C::jak2::native
