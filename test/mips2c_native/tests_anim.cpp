/*!
 * @file tests_anim.cpp
 * (AI-assisted)
 * Tests for the natives of the joint decompressor (native_anim.cpp): pc-anim-decomp-request
 * against the GOAL function decomp-request (decomp-fixed, decomp-frame) and pc-anim-finalize-frame
 * against finalize-joint-frame (quaternion-normalize!), as compiled by goalc (goalc_ref/joint.c).
 */

#include "fakes.h"

namespace tests {

using namespace harness;

namespace {

//! a float of animation data: usually in [-range, range], sometimes special
float anim_float(Gen& g, float range) {
  if (g.chance(0.03f)) {
    return Mips2C::u2f(g.u32_());  // any bits (NaN, inf, denormal)
  }
  return g.f_edge(-range, range);
}

//! the data streams of a part (fixed part or frame) at base: 64-bit, 32-bit, 16-bit, generous
//! sizes (what is read depends on the control bits), random contents. Writes the three offsets
//! (relative to base) at `offsets`.
void gen_streams(Gen& g, u32 base, u32 offsets, int num_joints) {
  const u32 size64 = 128 + 16 * num_joints;
  const u32 size32 = 8 * num_joints + 8;
  const u32 size16 = 4 * num_joints + 4;
  st<u32>(offsets, 0);
  st<u32>(offsets + 4, size64);
  st<u32>(offsets + 8, size64 + size32);
  const bool floats = g.chance(0.5f);
  for (u32 i = 0; i < size64 + size32; i += 4) {
    if (floats) {
      st<float>(base + i, anim_float(g, 5000.f));
    } else {
      st<u32>(base + i, g.u32_());  // as s16 pairs (quaternions, little trans, scale)
    }
  }
  for (u32 i = 0; i < size16; i += 2) {
    st<s16>(base + size64 + size32 + i, (s16)g.u32_());
  }
}

u32 stream_bytes(int num_joints) {
  return 128 + 16 * num_joints + 8 * num_joints + 8 + 4 * num_joints + 4;
}

//! the output frame: 2 matrices and num_joints transformqs, zero or already holding a sum
u32 gen_output(Gen& g, int num_joints) {
  const u32 size = 128 + 48 * num_joints;
  const u32 out = alloc(size + 16);
  const int mode = g.range(0, 2);
  for (u32 i = 0; i < size; i += 4) {
    float v = 0.f;
    if (mode == 1) {
      v = g.f(-2.f, 2.f);
    } else if (mode == 2) {
      v = anim_float(g, 1000.f);
    }
    st<float>(out + i, v);
  }
  return out;
}

}  // namespace

void register_anim_tests() {
  add_test({"pc-anim-decomp-request",
            "pc-anim-decomp-request",
            6000,
            nullptr,
            [](Case& c) {
              auto& g = c.g;
              // the game passes the skeleton's joints + 2; up to 112 keeps the control bits in
              // the header (more read the words after them, the same in both versions)
              const int num_joints = g.chance(0.05f) ? g.range(0, 2) : g.range(3, 112);
              const u32 out = gen_output(g, num_joints);

              // fixed part (joint-anim-compressed-fixed): header, then the data at 80
              const u32 fixed = alloc(80 + stream_bytes(num_joints));
              for (int i = 0; i < 14; i++) {
                st<u32>(fixed + 4 * i, g.chance(0.2f) ? g.pick(std::vector<u32>{0, 0xffffffff})
                                                      : g.u32_());
              }
              st<u32>(fixed + 56, num_joints);
              st<u32>(fixed + 60, g.range(0, 3));  // matrix-bits
              gen_streams(g, fixed + 80, fixed + 64, num_joints);

              // frames (joint-anim-compressed-frame): offsets, then the data at 16
              const int num_frames = g.range(2, 5);
              const u32 jacc = alloc(16 + 4 * num_frames);
              st<u32>(jacc + 12, fixed);
              for (int f = 0; f < num_frames; f++) {
                const u32 fr = alloc(16 + stream_bytes(num_joints));
                gen_streams(g, fr + 16, fr, num_joints);
                st<u32>(jacc + 16 + 4 * f, fr);
              }

              const u32 req = alloc(32);
              st<u32>(req, jacc);
              st<s64>(req + 8, g.range(0, num_frames - 2));
              float interp = g.f(0.f, 1.f);
              if (g.chance(0.3f)) {
                interp = 0.f;
              } else if (g.chance(0.05f)) {
                interp = g.pick(std::vector<float>{1e-40f, -0.f, 1.f, -1e-40f});
              }
              st<float>(req + 16, interp);
              st<float>(req + 20, g.chance(0.5f) ? 1.f : g.f_edge(0.f, 1.f));

              c.args[0] = out;
              c.args[1] = num_joints;
              c.args[2] = req;
            },
            {}});

  add_test({"pc-anim-finalize-frame",
            "pc-anim-finalize-frame",
            6000,
            nullptr,
            [](Case& c) {
              auto& g = c.g;
              const int num_joints = g.chance(0.05f) ? g.range(-1, 2) : g.range(3, 112);
              const u32 out = gen_output(g, num_joints < 0 ? 0 : num_joints);
              // quaternions: unnormalized sums of blended frames, sometimes zero
              for (int i = 0; i + 2 < num_joints; i++) {
                const u32 q = out + 128 + 48 * i + 16;
                if (g.chance(0.02f)) {
                  st_vec(q, 0.f, 0.f, 0.f, 0.f);
                } else if (!g.chance(0.1f)) {
                  st_vec(q, g.f(-1.5f, 1.5f), g.f(-1.5f, 1.5f), g.f(-1.5f, 1.5f),
                         g.f(-1.5f, 1.5f));
                }
              }
              c.args[0] = out;
              c.args[1] = (u64)(s64)num_joints;
            },
            {}});
}

}  // namespace tests
