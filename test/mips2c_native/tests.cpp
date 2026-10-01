/*!
 * @file tests.cpp
 * (AI-assisted)
 * The list of test groups.
 */

namespace tests {
void register_collide_func_tests();
void register_collide_cache_tests();
void register_joint_tests();
void register_collide_probe_tests();
void register_collide_mesh_tests();
void register_collide_edge_grab_tests();
void register_sparticle_tests();
void register_ocean_tests();
void register_sparticle_launcher_tests();
void register_goal_collide_tests();

void register_all() {
  register_collide_func_tests();
  register_collide_cache_tests();
  register_joint_tests();
  register_collide_probe_tests();
  register_collide_mesh_tests();
  register_collide_edge_grab_tests();
  register_sparticle_tests();
  register_ocean_tests();
  register_sparticle_launcher_tests();
  register_goal_collide_tests();
}
}  // namespace tests
