#pragma once

/*!
 * @file native2.h
 * (AI-assisted)
 * Native C++ versions of Jak 2 mips2c functions that have no Jak 1 counterpart (sky, spatial hash,
 * collide hash, motion blur, lights, ...). See game/mips2c/mips2c_native.h for the rules: the same
 * float operations, grouped the same way as the mips2c code, so the results are bit-identical
 * (OPENGOAL_MIPS2C_VERIFY=1 checks every call against the mips2c version). The mips2c files register
 * them next to their execute function (always on; OPENGOAL_MIPS2C_NATIVE=0 / config.ini
 * mips2c_native_off still pick the mips2c versions for debugging).
 */

#include "game/mips2c/mips2c_native.h"

namespace Mips2C::jak2 {
//! the VU0 registers the sky functions share (sky.cpp)
extern ExecutionContext sky_regs_vfs;
}  // namespace Mips2C::jak2

namespace Mips2C::jak2::native2 {

//! GOAL address of a symbol's value (Jak 2: the word before the symbol)
u32 sym_value_addr(const char* name);

// native2_sky.cpp
extern const NativeImpl render_sky_quad;
extern const NativeImpl render_sky_tri;
extern const NativeImpl method_32_sky_work;
extern const NativeImpl method_33_sky_work;

// native2_spatial_hash.cpp
extern const NativeImpl method_18_grid_hash;
extern const NativeImpl method_19_grid_hash;
extern const NativeImpl method_20_grid_hash;
extern const NativeImpl method_22_grid_hash;
extern const NativeImpl method_28_sphere_hash;
extern const NativeImpl method_29_sphere_hash;
extern const NativeImpl method_30_sphere_hash;
extern const NativeImpl method_31_sphere_hash;
extern const NativeImpl method_32_sphere_hash;
extern const NativeImpl method_33_sphere_hash;
extern const NativeImpl method_33_spatial_hash;
extern const NativeImpl method_35_spatial_hash;
extern const NativeImpl method_36_spatial_hash;
extern const NativeImpl method_37_spatial_hash;
extern const NativeImpl method_39_spatial_hash;

// native2_collide_hash.cpp
extern const NativeImpl method_11_collide_hash;
extern const NativeImpl method_12_collide_hash;
extern const NativeImpl fill_bg_using_box_new;
extern const NativeImpl fill_bg_using_line_sphere_new;

// native2_font.cpp
extern const NativeImpl draw_string_asm;

// native2_misc.cpp
extern const NativeImpl sparticle_motion_blur;
extern const NativeImpl add_light_sphere_to_light_group;
// native2_lights.cpp (AI-assisted)
extern const NativeImpl light_hash_get_bucket_index;
extern const NativeImpl pc_light_hash_add_lights;
//! (mem-copy! dst src size), gcommon.gc (registered by mem_copy::link for the types-h module)
extern const NativeImpl mem_copy;

}  // namespace Mips2C::jak2::native2
