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

void register_all() {
  register_collide_func_tests();
  register_collide_cache_tests();
  register_joint_tests();
  register_collide_probe_tests();
  register_collide_mesh_tests();
}
}  // namespace tests
