/*!
 * @file native_sp_init.cpp
 * (AI-assisted)
 * Jak 2: pc-sp-init-fields!, the native version of the GOAL function sp-init-fields!
 * (sparticle-launcher.gc), see game/mips2c/native_sp_init_fields.h. A symbol's value is at the
 * symbol - 1; float-store (flag 7) stores the value in *sp-temp* too.
 */

#include "game/mips2c/jak2_functions/native_functions.h"
#include "game/mips2c/native_sp_init_fields.h"

namespace Mips2C::jak2::native {

u64 pc_sp_init_fields_impl(const NativeArgs& args) {
  static const u32 vu_r = sym_val_addr("*_vu-reg-R_*");
  static const u32 part_id_table = sym_val_addr("*part-id-table*");
  static const u32 sp_temp = sym_val_addr("*sp-temp*");
  static const u32 goal_fn = sym_val_addr("sp-init-fields!");
  const sp_init::Syms syms{vu_r, gload<u32>(part_id_table), sp_temp};
  return sp_init::entry<-1, true>(args, syms, goal_fn);
}

const NativeImpl pc_sp_init_fields = MIPS2C_NATIVE_IMPL_GOAL(pc_sp_init_fields_impl, 0);

}  // namespace Mips2C::jak2::native

namespace Mips2C::jak2::pc_sp_init_fields {
void link() {
  gLinkedFunctionTable.reg("pc-sp-init-fields!", native::pc_sp_init_fields.as_exec, 64,
                           &native::pc_sp_init_fields);
}
}  // namespace Mips2C::jak2::pc_sp_init_fields
