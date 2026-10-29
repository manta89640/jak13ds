/*!
 * @file tests_anim.cpp
 * (AI-assisted)
 * Tests for the natives of the joint decompressor (native_anim.cpp): pc-anim-decomp-request
 * against the GOAL function decomp-request (decomp-fixed, decomp-frame) and pc-anim-finalize-frame
 * against finalize-joint-frame (quaternion-normalize!), as compiled by goalc (goalc_ref/joint.c).
 */

#include "fakes.h"

extern u32 g_link_generation;  // fakes.cpp

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

//! a compressed animation (joint-anim-compressed-control) of num_frames frames for num_joints
u32 gen_jacc(Gen& g, int num_joints, int num_frames) {
  const u32 fixed = alloc(80 + stream_bytes(num_joints));
  for (int i = 0; i < 14; i++) {
    st<u32>(fixed + 4 * i,
            g.chance(0.2f) ? g.pick(std::vector<u32>{0, 0xffffffff}) : g.u32_());
  }
  st<u32>(fixed + 56, num_joints);
  st<u32>(fixed + 60, g.range(0, 3));  // matrix-bits
  gen_streams(g, fixed + 80, fixed + 64, num_joints);
  const u32 jacc = alloc(16 + 4 * num_frames);
  st<u32>(jacc, num_frames);
  st<u32>(jacc + 12, fixed);
  for (int f = 0; f < num_frames; f++) {
    const u32 fr = alloc(16 + stream_bytes(num_joints));
    gen_streams(g, fr + 16, fr, num_joints);
    st<u32>(jacc + 16 + 4 * f, fr);
  }
  return jacc;
}

void setup_new_joint_decompressor() {
  static bool done = false;
  if (done) {
    return;
  }
  done = true;
  const u32 type = make_type("new-joint-decompressor-work", 16);
  const char* methods[7] = {"eval-blend-tree!", "output-blend-tree!", "build-requests!",
                            "init-frame!",      "process-request!",   "process-requests!",
                            "finalize-frame!"};
  for (int i = 0; i < 7; i++) {
    const std::string name =
        std::string("(method ") + methods[i] + " new-joint-decompressor-work)";
    bind_goalc_reference(name);
    set_method(type, 9 + i, sym_value(name));
  }
  set_sym("*new-joint-decompressor-work*", alloc_static(1168 + 192, 16) + 96);
  set_sym("*pc-3ds-anim-native*", true_sym());
  bind_mips2c_symbol("pc-anim-decomp-request");
  bind_mips2c_symbol("pc-anim-finalize-frame");
}

}  // namespace

void register_anim_tests() {
  // groups of 3 cases with the same inputs: the frame cache records the requests, stores the
  // frame, then copies it; every group starts a new link generation (new data at reused addresses)
  add_test({"pc-new-joint-decompressor",
            "pc-new-joint-decompressor",
            9000,
            setup_new_joint_decompressor,
            [](Case& c) {
              if (c.index % 3 == 0) {
                g_link_generation++;
              }
              Gen g(0x5eed1234u ^ (u32)(c.index / 3) * 0x85ebca6bu);
              const int num_joints = g.chance(0.05f) ? g.range(0, 2) : g.range(3, 60);
              // the work area: whatever the last call left
              const u32 work = sym_value("*new-joint-decompressor-work*");
              for (u32 i = 0; i < 1168; i += 4) {
                st<float>(work + i, g.chance(0.5f) ? 0.f : g.f(-2.f, 2.f));
              }
              const u32 out = gen_output(g, num_joints);
              const int num_channels = g.chance(0.05f) ? g.range(0, 6) : g.range(1, 3);
              const u32 jc = alloc(44 + 48 * num_channels + 16) + 4;
              st<u16>(jc, g.chance(0.1f) ? 0x20 : (u16)g.range(0, 0x1f));
              st<s32>(jc + 20, num_channels);
              const int nanims = g.range(1, 2);
              std::vector<u32> groups;
              for (int a = 0; a < nanims; a++) {
                const u32 ag = alloc(48);
                const int nf = g.range(1, 5);
                st<u32>(ag + 40, gen_jacc(g, num_joints, nf));
                groups.push_back(ag);
              }
              const std::vector<std::string> cmds = {"push", "blend", "push1", "stack", "stack1"};
              for (int i = 0; i < num_channels; i++) {
                const u32 ch = jc + 44 + 48 * i;
                std::string cmd = i == 0 ? "push" : g.pick(cmds);
                if (i == 0 && g.chance(0.03f)) {
                  cmd = g.pick(cmds);
                }
                st<u32>(ch + 4, sym(cmd));
                float interp = g.f(0.f, 1.f);
                if (g.chance(0.3f)) {
                  interp = g.pick(std::vector<float>{0.f, 1.f, 0.5f});
                }
                st<float>(ch + 8, interp);
                st<u32>(ch + 12, g.pick(groups));
                float fn = g.f(0.f, 6.f);
                if (g.chance(0.4f)) {
                  fn = (float)g.range(0, 5);
                }
                st<float>(ch + 16, fn);
                st<float>(ch + 44, g.f(-1.f, 1.f));
              }
              c.args[0] = out;
              c.args[1] = num_joints;
              c.args[2] = jc;
            },
            {}});

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
