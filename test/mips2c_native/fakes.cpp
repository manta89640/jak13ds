/*!
 * @file fakes.cpp
 * (AI-assisted)
 * Fake GOAL functions shared by the tests.
 */

#include "fakes.h"

namespace tests {

using namespace harness;

void add_collide_fakes() {
  static bool done = false;
  if (done) {
    return;
  }
  done = true;

  set_sym("ray-sphere-intersect",
          add_goal_fn("ray-sphere-intersect", 4, [](const u64* a) -> u64 {
            float o[4], d[4], c[4];
            memcpy(o, hptr((u32)a[0]), 16);
            memcpy(d, hptr((u32)a[1]), 16);
            memcpy(c, hptr((u32)a[2]), 16);
            return f_bits(fake_ray_sphere(o, d, c, bits_f(a[3])));
          }));

  // (ray-cylinder-intersect origin dir cyl-origin cyl-axis cyl-rad cyl-len pt-out)
  set_sym("ray-cylinder-intersect",
          add_goal_fn("ray-cylinder-intersect", 7, [](const u64* a) -> u64 {
            float o[4], d[4], co[4], axis[4];
            memcpy(o, hptr((u32)a[0]), 16);
            memcpy(d, hptr((u32)a[1]), 16);
            memcpy(co, hptr((u32)a[2]), 16);
            memcpy(axis, hptr((u32)a[3]), 16);
            const float rad = bits_f(a[4]);
            const float len = bits_f(a[5]);
            float v[4];
            for (int i = 0; i < 4; i++) {
              v[i] = o[i] - co[i];
            }
            const float h0 = v[0] * axis[0] + v[1] * axis[1] + v[2] * axis[2];
            const float dh = d[0] * axis[0] + d[1] * axis[1] + d[2] * axis[2];
            if (h0 < 0 && h0 + dh < 0) {
              return f_bits(-100000000.f);
            }
            if (h0 - len >= 0 && h0 + dh - len >= 0) {
              return f_bits(-100000000.f);
            }
            float p[4], q[4], zero[4] = {0, 0, 0, 0};
            for (int i = 0; i < 4; i++) {
              p[i] = v[i] - axis[i] * h0;
              q[i] = d[i] - axis[i] * dh;
            }
            const float t = fake_ray_sphere(p, q, zero, rad);
            if (t < 0) {
              return f_bits(-100000000.f);
            }
            const float h = h0 + dh * t;
            if (h < 0 || h - len >= 0) {
              return f_bits(-100000000.f);
            }
            float out[4] = {co[0] + axis[0] * h, co[1] + axis[1] * h, co[2] + axis[2] * h, co[3]};
            st_bytes((u32)a[6], out, 16);
            return f_bits(t);
          }));
}

}  // namespace tests
