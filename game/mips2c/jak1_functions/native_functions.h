#pragma once

/*!
 * @file native_functions.h
 * (AI-assisted)
 * Native C++ versions of Jak 1 mips2c functions (see game/mips2c/mips2c_native.h). The mips2c
 * files register them next to their execute function.
 */

#include "game/mips2c/mips2c_native.h"

namespace Mips2C::jak1::native {
// native_joint.cpp
extern const NativeImpl cspace_parented_transformq_joint;
// native_collide_func.cpp
extern const NativeImpl moving_sphere_triangle_intersect;
u64 moving_sphere_triangle_intersect_impl(const NativeArgs& args);
// native_collide_cache.cpp
extern const NativeImpl method_9_collide_cache_prim;
extern const NativeImpl method_26_collide_cache;
extern const NativeImpl method_27_collide_cache;
extern const NativeImpl method_29_collide_cache;
extern const NativeImpl method_32_collide_cache;
// native_sparticle.cpp
extern const NativeImpl sp_process_block_2d;
}  // namespace Mips2C::jak1::native
