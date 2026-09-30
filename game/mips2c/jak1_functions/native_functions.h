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
extern const NativeImpl collide_do_primitives;
extern const NativeImpl moving_sphere_triangle_intersect;
u64 collide_do_primitives_impl(const NativeArgs& args);
u64 moving_sphere_triangle_intersect_impl(const NativeArgs& args);
// native_collide_probe.cpp
extern const NativeImpl collide_probe_node;
extern const NativeImpl collide_probe_instance_tie;
// native_collide_mesh.cpp
extern const NativeImpl method_11_collide_mesh;
extern const NativeImpl method_12_collide_mesh;
extern const NativeImpl method_14_collide_mesh;
extern const NativeImpl method_15_collide_mesh;
u64 method_14_collide_mesh_impl(const NativeArgs& args);
u64 method_15_collide_mesh_impl(const NativeArgs& args);
// native_collide_edge_grab.cpp
extern const NativeImpl method_15_collide_edge_work;
extern const NativeImpl method_16_collide_edge_work;
extern const NativeImpl method_18_collide_edge_work;
extern const NativeImpl method_10_collide_edge_hold_list;
// native_collide_cache.cpp
extern const NativeImpl method_9_collide_cache_prim;
extern const NativeImpl method_26_collide_cache;
extern const NativeImpl method_27_collide_cache;
extern const NativeImpl method_29_collide_cache;
extern const NativeImpl method_32_collide_cache;
extern const NativeImpl method_28_collide_cache;
extern const NativeImpl method_30_collide_cache;
extern const NativeImpl method_10_collide_cache_prim;
extern const NativeImpl method_9_collide_puss_work;
extern const NativeImpl method_10_collide_puss_work;
extern const NativeImpl method_12_collide_shape_prim_mesh;
extern const NativeImpl method_13_collide_shape_prim_mesh;
extern const NativeImpl method_14_collide_shape_prim_mesh;
extern const NativeImpl pc_upload_collide_frag;
// native_sparticle.cpp
extern const NativeImpl sp_process_block_2d;
extern const NativeImpl sp_process_block_3d;
// native_sparticle_launcher.cpp
extern const NativeImpl particle_adgif;
extern const NativeImpl sp_launch_particles_var;
u64 particle_adgif_impl(const NativeArgs& args);
// native_ocean.cpp
extern const NativeImpl ocean_interp_wave;
}  // namespace Mips2C::jak1::native
