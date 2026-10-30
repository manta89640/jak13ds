/*!
 * @file tests_ocean_jak2.cpp
 * Tests for Jak 2's ocean natives (jak2_functions/native_ocean.cpp) against Jak 2's mips2c code:
 * render-ocean-quad (ocean.cpp, with draw-large-polygon-ocean and sky.cpp's clippers) and
 * (method 14/15/16 ocean) (ocean_vu0.cpp). Jak 2 symbols: #t is s7 + 4.
 */

#include "fakes.h"

namespace Mips2C::jak2 {
extern ExecutionContext ocean_regs_vfs;
}

namespace tests {

using namespace harness;

namespace {

//! a float's bits with garbage in the upper half (mtc1 reads the low 32 bits)
u64 f_arg(Gen& g, float f) {
  return (g.chance(0.3f) ? (u64)g.u32_() << 32 : 0) | Mips2C::f2u(f);
}

// (method 14 ocean) (this dest frame scale)
void gen_method_14(Case& c) {
  auto& g = c.g;
  const u32 frames = alloc(64 * 1024 + 64);
  for (u32 i = 0; i < 64 * 1024 + 64; i += 4) {
    st<u32>(frames + i, g.u32_());
  }
  set_sym("*ocean-wave-frames*", frames);
  const u32 obj = alloc(256);
  for (u32 i = 0; i < 256; i += 4) {
    st<u32>(obj + i, g.u32_());
  }
  float frame;
  switch (g.range(0, 3)) {
    case 0:
      frame = g.f(0.f, 64.f);
      break;
    case 1:
      frame = (float)g.range(0, 63);  // a whole frame: the second weight 0
      break;
    case 2:
      frame = g.f(63.f, 64.f);  // the last frame, then the first
      break;
    default:
      frame = g.f_edge(-0.99f, 63.99f);
      break;
  }
  const float scale = g.chance(0.7f) ? (g.chance(0.5f) ? 0.08325f : 0.01665f) * g.pick<float>({1.f, 0.75f})
                                     : g.f_edge(-2.f, 2.f);
  c.args[0] = obj;
  c.args[1] = alloc(4096);
  c.args[2] = f_arg(g, frame);
  c.args[3] = f_arg(g, scale);
  for (int i = 4; i < 8; i++) {
    c.args[i] = g.u32_();
  }
}

// (method 15 ocean) (this heights heights2)
void gen_method_15(Case& c) {
  auto& g = c.g;
  const u32 a = alloc(4096), b = alloc(4096);
  const float amp = g.f(0.f, 40000.f);
  for (u32 i = 0; i < 4096; i += 4) {
    st<float>(a + i, g.f_edge(-amp, amp));
    st<float>(b + i, g.f_edge(-amp, amp));
  }
  c.args[0] = alloc(16);
  c.args[1] = a;
  c.args[2] = b;
  for (int i = 3; i < 8; i++) {
    c.args[i] = g.u32_();
  }
}

// the GOAL functions (method 16 ocean) calls: the VU0 program upload (nothing), level-get (sewerb's
// level or #f) and vu-lights<-light-group! (copies the light group: 7 quadwords)
void setup_method_16() {
  set_sym("upload-vu0-program",
          add_goal_fn("upload-vu0-program", 2, [](const u64*) -> u64 { return 0; }));
  set_sym("vu-lights<-light-group!",
          add_goal_fn("vu-lights<-light-group!", 2, [](const u64* a) -> u64 {
            for (u32 i = 0; i < 112; i += 4) {
              st<u32>((u32)a[0] + i, ld<u32>((u32)a[1] + i));
            }
            return 0;
          }));
  const u32 type = make_type("level-group", 20);
  set_method(type, 9, add_goal_fn("level-get", 2, [](const u64*) -> u64 {
               const u32 lev = sym_value("*ocean2-test-level*");
               return lev ? lev : st();
             }));
  set_sym("*level*", alloc_static(64, 16) + 4);
  st<u32>(sym_value("*level*") - 4, type);
}

//! a light group: 3 directions (unit-ish), 3 colors and the ambient
void fill_lights(Gen& g, u32 addr, float hi) {
  for (u32 i = 0; i < 112; i += 4) {
    const u32 lane = (i / 4) % 4;
    const float v = i < 48 ? g.f_edge(-1.f, 1.f) : (lane == 3 ? g.f(0.f, 1.f) : g.f_edge(0.f, hi));
    st<float>(addr + i, v);
  }
}

// (method 16 ocean) (this verts heights)
void gen_method_16(Case& c) {
  auto& g = c.g;
  const u32 obj = alloc(8320);
  for (u32 i = 0; i < 8320; i += 4) {
    st<u32>(obj + i, g.u32_());
  }
  if (g.chance(0.7f)) {
    st_vec(obj + 8096, -0.00012207031f, 255.f, -0.00012207031f, 128.f);
  } else {
    st_vec(obj + 8096, g.f_edge(-0.001f, 0.001f), g.f_edge(0.f, 300.f), g.f_edge(-0.001f, 0.001f),
           g.f(-1.f, 1.f));
  }
  // sum of the colors and the ambient above 1.25 (scaled down) or not
  const float hi = g.chance(0.5f) ? 1.5f : 0.3f;
  const u32 tod = alloc(140 + 112 + 12);
  fill_lights(g, tod + 140, hi);
  set_sym("*time-of-day-context*", tod);
  if (g.chance(0.3f)) {
    const u32 lev = alloc(716 + 112 + 12);
    fill_lights(g, lev + 716, hi);
    set_sym("*ocean2-test-level*", lev);
  } else {
    set_sym("*ocean2-test-level*", 0);
  }
  set_sym("ocean-vu0-block", alloc(16));
  const u32 scale = alloc(16);
  if (g.chance(0.8f)) {
    st_vec(scale, 128.f, 128.f, 128.f, 128.f);  // ocean.gc's
  } else {
    st_vec(scale, g.f(0.f, 200.f), g.f(0.f, 200.f), g.f(0.f, 200.f), g.f(0.f, 200.f));
  }
  set_sym("*ocean-generate-verts-vector*", scale);
  // the 32x32 heights (the two wave layers added: up to about +-25 m), sometimes flat
  const u32 heights = alloc(4096 + 64);  // (the mips2c version reads 16 bytes past the end)
  const float amp = g.chance(0.1f) ? 0.f : g.f(0.f, 100000.f);
  for (u32 i = 0; i < 4096 + 64; i += 4) {
    st<float>(heights + i, g.chance(0.02f) ? g.f_edge(-amp, amp) : g.f(-amp, amp));
  }
  c.args[0] = obj;
  c.args[1] = alloc(0x8000);
  c.args[2] = heights;
  for (int i = 3; i < 8; i++) {
    c.args[i] = g.u32_();
  }
}

// (render-ocean-quad vertices dma-buf): the registers of init-ocean-far-regs (the camera matrix,
// clip scale, hvdf offset, fog, giftag), random so that the quads land inside, outside and across
// the clip planes (x, y = +-w, w = 0 for the far vertices). vf24 is random: Jak 2 doesn't use it.
void gen_render_ocean_quad(Case& c) {
  auto& g = c.g;
  auto& vf = Mips2C::jak2::ocean_regs_vfs.vfs;
  auto set = [&](int r, float x, float y, float z, float w) {
    vf[r].f[0] = x;
    vf[r].f[1] = y;
    vf[r].f[2] = z;
    vf[r].f[3] = w;
  };
  const bool easy = g.chance(0.2f);  // a camera that sees the whole quad
  for (int r = 28; r <= 31; r++) {
    set(r, g.f_edge(-1.f, 1.f), g.f_edge(-1.f, 1.f), g.f_edge(-1.f, 1.f), g.f_edge(-1.f, 1.f));
  }
  if (easy) {
    set(31, 0.1f, 0.f, 0.f, 0.f);
    set(30, 0.f, 0.1f, 0.f, 0.f);
    set(29, 0.f, 0.f, 0.1f, 0.f);
    set(28, 0.f, 0.f, 0.f, 1.f);
  }
  set(13, g.f(-1000.f, 1000.f), g.f(0.f, 100.f), g.f(100.f, 3071.f), g.f(0.f, 1.f));
  set(14, g.f(0.2f, 2.f), g.f(0.2f, 2.f), g.f(0.2f, 2.f), g.f(0.2f, 2.f));
  set(25, 2048.f, 2048.f, g.f(0.f, 1e7f), g.f(0.f, 300.f));
  set(26, g.f(0.2f, 2.f), g.f(0.2f, 2.f), g.f(0.2f, 2.f), g.f(0.2f, 2.f));
  set(24, g.f(-1.f, 1.f), g.f(-1.f, 1.f), g.f(-1.f, 1.f), g.f(-1.f, 1.f));
  for (int i = 0; i < 4; i++) {
    vf[27].du32[i] = g.u32_();
  }
  const u32 verts = alloc(4 * 48);
  for (int v = 0; v < 4; v++) {
    const float w = g.chance(0.5f) ? (float)(v >= 2) : g.f_edge(-1.f, 1.f);
    st_vec(verts + 48 * v, g.f_edge(-3.f, 3.f), g.f_edge(-3.f, 3.f), g.f_edge(-3.f, 3.f), w);
    st_vec(verts + 48 * v + 16, g.f(0.f, 16.f), g.f(0.f, 16.f), 1.f, 1.f);
    st_vec(verts + 48 * v + 32, (float)g.range(0, 256), (float)g.range(0, 256),
           (float)g.range(0, 256), (float)g.range(0, 129));
  }
  // dma-buffer: base at 4
  const u32 buf = alloc(32);
  st<u32>(buf + 4, alloc(16 + 48 * 16));
  c.args[0] = verts;
  c.args[1] = buf;
  for (int i = 2; i < 8; i++) {
    c.args[i] = g.u32_();
  }
}

}  // namespace

void register_ocean_jak2_tests() {
  add_test({"ocean-interp-wave-jak2", "jak2:(method 14 ocean)", 400, [] {}, gen_method_14, {}});
  add_test({"ocean-add-heights-jak2", "jak2:(method 15 ocean)", 200, [] {}, gen_method_15, {}});
  add_test({"ocean-generate-verts-jak2", "jak2:(method 16 ocean)", 300, setup_method_16,
            gen_method_16, {}});
  add_test({"render-ocean-quad-jak2", "jak2:render-ocean-quad", 3000, [] {},
            gen_render_ocean_quad, {}});
}

}  // namespace tests
