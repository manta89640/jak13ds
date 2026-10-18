/*!
 * @file native_common.cpp
 * (AI-assisted)
 * The switch for Jak 2's native versions of mips2c functions (native_functions.h).
 */

#include <cstdlib>
#include <cstring>

#include "common/log/log.h"

#include "game/kernel/jak2/kscheme.h"
#include "game/mips2c/jak2_functions/native_functions.h"

#ifdef __3DS__
extern "C" int ctr_config_get(const char* key, char* out, int size);  // platform/3ds/port
#endif

namespace Mips2C::jak2::native {

/*!
 * config.ini logic_native (default on; preset safe: off). Off: no Jak 2 native is registered and
 * the mips2c versions run, as before the natives. Host: OPENGOAL_LOGIC_NATIVE=0 (or, for every
 * game, OPENGOAL_MIPS2C_NATIVE=0). config.ini mips2c_native_off still turns off single ones.
 */
bool logic_native_on() {
  static int on = -1;
  if (on < 0) {
    on = 1;
#ifdef __3DS__
    char v[16];
    if (ctr_config_get("logic_native", v, sizeof(v)) &&
        (!strcmp(v, "off") || !strcmp(v, "0") || !strcmp(v, "no") || !strcmp(v, "false"))) {
      on = 0;
    }
#else
    const char* e = getenv("OPENGOAL_LOGIC_NATIVE");
    if (e && e[0] == '0') {
      on = 0;
    }
#endif
    lg::info("Jak 2 native mips2c functions (logic_native): {}",
             on ? "on" : "off (config.ini logic_native)");
  }
  return on == 1;
}

u32 sym_val_addr(const char* name) {
  return ::jak2::intern_from_c(name).offset - 1;
}

}  // namespace Mips2C::jak2::native
