#pragma once

/*!
 * @file native_functions.h
 * (AI-assisted)
 * Native C++ versions of Jak 2 mips2c functions (see game/mips2c/mips2c_native.h). Ported from
 * Jak 1's (game/mips2c/jak1_functions/native_*.cpp) to Jak 2's code and types. The mips2c files
 * register them next to their execute function, always (the same results; no config.ini switch
 * since Oct 18). Debugging: OPENGOAL_MIPS2C_NATIVE=0 (host) / config.ini mips2c_native_off pick
 * the mips2c versions, OPENGOAL_MIPS2C_VERIFY=1 compares them.
 */

#include "game/mips2c/mips2c_native.h"

namespace Mips2C::jak2::native {

//! GOAL address of a symbol's value (Jak 2: the symbol's address - 1)
u32 sym_val_addr(const char* name);

//! #t (Jak 2: s7 + 4)
inline u64 goal_true(u64 st) {
  return st + 4;
}

// native_joint.cpp
extern const NativeImpl cspace_parented_transformq_joint;
// native_bones.cpp (a GOAL function's loop: pc-new-bones-mtx-calc, no mips2c version)
extern const NativeImpl pc_new_bones_mtx_calc;
// native_sparticle.cpp
extern const NativeImpl sp_process_block_2d;
extern const NativeImpl sp_process_block_3d;
// native_sparticle_launcher.cpp
extern const NativeImpl particle_adgif;
extern const NativeImpl sp_launch_particles_var;
u64 particle_adgif_impl(const NativeArgs& args);
// native_shadow.cpp
extern const NativeImpl shadow_execute;
// native_collide_func.cpp
extern const NativeImpl collide_do_primitives;
extern const NativeImpl moving_sphere_triangle_intersect;
u64 collide_do_primitives_impl(const NativeArgs& args);
u64 moving_sphere_triangle_intersect_impl(const NativeArgs& args);
// native_collide_mesh.cpp
extern const NativeImpl method_12_collide_mesh;
extern const NativeImpl method_14_collide_mesh;
extern const NativeImpl method_15_collide_mesh;
// native_collide_cache.cpp
extern const NativeImpl method_9_collide_cache_prim;
extern const NativeImpl method_10_collide_cache_prim;
extern const NativeImpl method_9_collide_puss_work;
extern const NativeImpl method_10_collide_puss_work;
}  // namespace Mips2C::jak2::native
