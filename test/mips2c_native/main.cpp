/*!
 * @file main.cpp
 * (AI-assisted)
 * Differential tests of native mips2c replacements against the mips2c versions (see harness.h).
 *
 *   mips2c-native-test [--filter SUBSTR] [--scale X] [--seed N] [--self] [--reports N] [--case N]
 *                      [--bench mips2c|native] [--fpscr HEX] [--dump DIR]
 *
 * --self runs the mips2c version twice instead of mips2c against native (tests the harness).
 * --filter =NAME runs only the test named NAME (a plain SUBSTR also selects its variants).
 *
 * (AI-assisted) On an arm64 host, --fpscr HEX sets FPCR the same way (its FZ and DN bits are the
 * FPSCR ones): --fpscr 0x03000000 is the 3DS's VFP mode (run.sh vfp).
 *
 * The ARM build (-D__3DS__) runs with the 3DS's VFP mode, which libctru sets for every thread:
 * default NaN and flush-to-zero, round to nearest (FPSCR 0x03000000). --fpscr HEX sets another.
 * (With default NaN off, a multiply-subtract that GCC fuses into VMLS gives a NaN of the opposite
 * sign than separate VMUL and VSUB do, so natives and mips2c can differ in the sign of NaNs.)
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "harness.h"

namespace Mips2C::jak1 {
#define LINK(ns)      \
  namespace ns {      \
  extern void link(); \
  }
LINK(collide_do_primitives)
LINK(ripple_execute_init)
LINK(ripple_create_wave_table)
LINK(moving_sphere_triangle_intersect)
LINK(collide_probe_node)
LINK(collide_probe_instance_tie)
LINK(method_12_collide_mesh)
LINK(method_11_collide_mesh)
LINK(method_15_collide_mesh)
LINK(method_14_collide_mesh)
LINK(method_26_collide_cache)
LINK(method_32_collide_cache)
LINK(pc_upload_collide_frag)
LINK(method_28_collide_cache)
LINK(method_27_collide_cache)
LINK(method_29_collide_cache)
LINK(method_12_collide_shape_prim_mesh)
LINK(method_14_collide_shape_prim_mesh)
LINK(method_13_collide_shape_prim_mesh)
LINK(method_30_collide_cache)
LINK(method_9_collide_cache_prim)
LINK(method_10_collide_cache_prim)
LINK(method_10_collide_puss_work)
LINK(method_9_collide_puss_work)
LINK(method_16_collide_edge_work)
LINK(method_15_collide_edge_work)
LINK(method_10_collide_edge_hold_list)
LINK(method_18_collide_edge_work)
LINK(calc_animation_from_spr)
LINK(cspace_parented_transformq_joint)
LINK(sp_process_block_3d)
LINK(sp_process_block_2d)
LINK(particle_adgif)
LINK(sp_launch_particles_var)
LINK(ocean_interp_wave)
LINK(ocean_generate_verts)
LINK(render_ocean_quad)
LINK(draw_large_polygon_ocean)
LINK(ray_sphere_intersect)
LINK(ray_cylinder_intersect)
LINK(moving_sphere_sphere_intersect)
LINK(closest_pt_in_triangle)
LINK(pc_anim_decomp_request)
LINK(pc_anim_finalize_frame)
LINK(shadow_execute)
LINK(shadow_xform_verts)
LINK(shadow_init_vars)
LINK(shadow_calc_dual_verts)
LINK(shadow_scissor_top)
LINK(shadow_scissor_edges)
LINK(shadow_find_facing_single_tris)
LINK(shadow_find_single_edges)
LINK(shadow_find_facing_double_tris)
LINK(shadow_find_double_edges)
LINK(shadow_add_verts)
LINK(shadow_add_facing_single_tris)
LINK(shadow_add_single_edges)
LINK(shadow_add_double_tris)
LINK(shadow_add_double_edges)
#undef LINK
}  // namespace Mips2C::jak1

namespace tests {
void register_all();
}

int main(int argc, char** argv) {
  harness::RunOptions opt;
#if defined(__arm__) && defined(__3DS__)
  u32 fpscr = 0x03000000;
#elif defined(__aarch64__)
  u64 fpscr = ~0ull;  // (AI-assisted) unchanged unless --fpscr
#endif
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--filter") && i + 1 < argc) {
      opt.filter = argv[++i];
    } else if (!strcmp(argv[i], "--scale") && i + 1 < argc) {
      opt.case_scale = atof(argv[++i]);
    } else if (!strcmp(argv[i], "--seed") && i + 1 < argc) {
      opt.seed = (u32)strtoul(argv[++i], nullptr, 10);
    } else if (!strcmp(argv[i], "--self")) {
      opt.self_check = true;
    } else if (!strcmp(argv[i], "--case") && i + 1 < argc) {
      opt.only_case = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--bench") && i + 1 < argc) {
      opt.bench = argv[++i];
    } else if (!strcmp(argv[i], "--reports") && i + 1 < argc) {
      opt.max_reports = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--dump") && i + 1 < argc) {
      opt.dump_dir = argv[++i];
#if (defined(__arm__) && defined(__3DS__)) || defined(__aarch64__)
    } else if (!strcmp(argv[i], "--fpscr") && i + 1 < argc) {
      fpscr = strtoul(argv[++i], nullptr, 16);
#endif
    } else {
      fprintf(stderr,
              "usage: %s [--filter SUBSTR] [--scale X] [--seed N] [--self] [--reports N] "
              "[--case N] [--bench mips2c|native] [--fpscr HEX] [--dump DIR]\n",
              argv[0]);
      return 2;
    }
  }
#if defined(__arm__) && defined(__3DS__)
  asm volatile("vmsr fpscr, %0" : : "r"(fpscr));
#elif defined(__aarch64__)
  if (fpscr != ~0ull) {
    asm volatile("msr fpcr, %0" : : "r"(fpscr));
  }
#endif

  g_ee_main_mem = (u8*)aligned_alloc(64, harness::kMemSize);
  memset(g_ee_main_mem, 0, harness::kMemSize);
  g_ee_main_mem_exec = g_ee_main_mem;
  harness::set_sym("*fake-scratchpad-data*", harness::kSpad);
  harness::load_goalc_references();

  using namespace Mips2C::jak1;
  collide_do_primitives::link();
  moving_sphere_triangle_intersect::link();
  collide_probe_node::link();
  collide_probe_instance_tie::link();
  method_12_collide_mesh::link();
  method_11_collide_mesh::link();
  method_15_collide_mesh::link();
  method_14_collide_mesh::link();
  method_26_collide_cache::link();
  method_32_collide_cache::link();
  pc_upload_collide_frag::link();
  method_28_collide_cache::link();
  method_27_collide_cache::link();
  method_29_collide_cache::link();
  method_12_collide_shape_prim_mesh::link();
  method_14_collide_shape_prim_mesh::link();
  method_13_collide_shape_prim_mesh::link();
  method_30_collide_cache::link();
  method_9_collide_cache_prim::link();
  method_10_collide_cache_prim::link();
  method_10_collide_puss_work::link();
  method_9_collide_puss_work::link();
  method_16_collide_edge_work::link();
  method_15_collide_edge_work::link();
  method_10_collide_edge_hold_list::link();
  method_18_collide_edge_work::link();
  calc_animation_from_spr::link();
  cspace_parented_transformq_joint::link();
  sp_process_block_3d::link();
  sp_process_block_2d::link();
  particle_adgif::link();
  sp_launch_particles_var::link();
  ocean_interp_wave::link();
  ocean_generate_verts::link();
  render_ocean_quad::link();
  draw_large_polygon_ocean::link();
  ripple_execute_init::link();
  ripple_create_wave_table::link();
  // (AI-assisted) shadow-execute calls the other 14 directly (their caches need their link)
  shadow_execute::link();
  shadow_xform_verts::link();
  shadow_init_vars::link();
  shadow_calc_dual_verts::link();
  shadow_scissor_top::link();
  shadow_scissor_edges::link();
  shadow_find_facing_single_tris::link();
  shadow_find_single_edges::link();
  shadow_find_facing_double_tris::link();
  shadow_find_double_edges::link();
  shadow_add_verts::link();
  shadow_add_facing_single_tris::link();
  shadow_add_single_edges::link();
  shadow_add_double_tris::link();
  shadow_add_double_edges::link();
  // GOAL functions with a native version: the reference is goalc's C code (goalc_ref/)
  ray_sphere_intersect::link();
  ray_cylinder_intersect::link();
  moving_sphere_sphere_intersect::link();
  closest_pt_in_triangle::link();
#ifndef GOALC_REF_NONE  // (AI-assisted) (see run.sh: MIPS2C_TEST_NO_GOALC_REF)
  for (const char* name : {"ray-sphere-intersect", "ray-cylinder-intersect",
                           "moving-sphere-sphere-intersect", "closest-pt-in-triangle"}) {
    harness::use_goalc_reference(name);
    harness::bind_mips2c_symbol(name);
  }
  // GOAL functions they call
  for (const char* name : {"pc-port-raw-ray-sphere-implementation", "vector-normalize!",
                           "vector-length", "vector+*!", "vector-segment-distance-point!"}) {
    harness::bind_goalc_reference(name);
  }
#endif
  // (AI-assisted) natives of joint.gc: the references are the GOAL functions they replace
  pc_anim_decomp_request::link();
  pc_anim_finalize_frame::link();
  harness::use_goalc_reference("pc-anim-decomp-request", "decomp-request");
  harness::use_goalc_reference("pc-anim-finalize-frame", "finalize-joint-frame");
  for (const char* name : {"add-scaled-matrix!", "decomp-fixed", "decomp-frame",
                           "quaternion-normalize!"}) {
    harness::bind_goalc_reference(name);
  }

  tests::register_all();
  const int failed = harness::run_tests(opt);
  printf("%s\n", failed ? "FAILED" : "all tests passed");
  return failed ? 1 : 0;
}
