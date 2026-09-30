/*!
 * @file main.cpp
 * (AI-assisted)
 * Differential tests of native mips2c replacements against the mips2c versions (see harness.h).
 *
 *   mips2c-native-test [--filter SUBSTR] [--scale X] [--seed N] [--self] [--list]
 *
 * --self runs the mips2c version twice instead of mips2c against native (tests the harness).
 */

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "harness.h"

namespace Mips2C::jak1 {
#define LINK(ns)   \
  namespace ns {   \
  extern void link(); \
  }
LINK(collide_do_primitives)
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
#undef LINK
}  // namespace Mips2C::jak1

namespace tests {
void register_all();
}

int main(int argc, char** argv) {
  harness::RunOptions opt;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--filter") && i + 1 < argc) {
      opt.filter = argv[++i];
    } else if (!strcmp(argv[i], "--scale") && i + 1 < argc) {
      opt.case_scale = atof(argv[++i]);
    } else if (!strcmp(argv[i], "--seed") && i + 1 < argc) {
      opt.seed = (u32)strtoul(argv[++i], nullptr, 10);
    } else if (!strcmp(argv[i], "--self")) {
      opt.self_check = true;
    } else if (!strcmp(argv[i], "--bench") && i + 1 < argc) {
      opt.bench = argv[++i];
    } else if (!strcmp(argv[i], "--reports") && i + 1 < argc) {
      opt.max_reports = atoi(argv[++i]);
    } else {
      fprintf(stderr, "usage: %s [--filter SUBSTR] [--scale X] [--seed N] [--self] [--reports N]\n",
              argv[0]);
      return 2;
    }
  }

  g_ee_main_mem = (u8*)aligned_alloc(64, harness::kMemSize);
  memset(g_ee_main_mem, 0, harness::kMemSize);
  g_ee_main_mem_exec = g_ee_main_mem;
  harness::set_sym("*fake-scratchpad-data*", harness::kSpad);

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

  tests::register_all();
  const int failed = harness::run_tests(opt);
  printf("%s\n", failed ? "FAILED" : "all tests passed");
  return failed ? 1 : 0;
}
