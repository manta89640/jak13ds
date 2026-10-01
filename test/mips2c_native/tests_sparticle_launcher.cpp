/*!
 * @file tests_sparticle_launcher.cpp
 * (AI-assisted)
 * Tests for sparticle_launcher.cpp: particle-adgif and sp-launch-particles-var.
 */

#include "fakes.h"

namespace tests {

using namespace harness;

namespace {

u32 g_birth_funcs[2];

//! the fakes' state (GOAL memory, so both versions see the same): 0 sound ids, 4 sp-init-fields!
//! calls, 8 next particle, 12 particles, 16 the particles (144 bytes each), 20 queued launches,
//! 24 last sound id, 32 last queued position, 48 last sound position
u32 fake_state() {
  return sym_value("*launch-test*");
}

//! the key of a texture id in the adgif cache (from the id as a sign-extended register)
u16 adgif_key(u64 tex) {
  return (u16)((((s64)tex >> 20) ^ ((s64)tex >> 8)) & 0xffff);
}

void setup_launcher() {
  static bool done = false;
  if (done) {
    return;
  }
  done = true;
  bind_mips2c_symbol("particle-adgif");

  // (particle-setup-adgif adgif texture-id)
  set_sym("particle-setup-adgif", add_goal_fn("particle-setup-adgif", 2, [](const u64* a) -> u64 {
            for (u32 i = 0; i < 80; i += 4) {
              st<u32>((u32)a[0] + i, ((u32)a[1] * (i + 1)) ^ 0x1234);
            }
            return 0;
          }));
  // (sp-queue-launch system launcher pos)
  set_sym("sp-queue-launch", add_goal_fn("sp-queue-launch", 3, [](const u64* a) -> u64 {
            const u32 fs = fake_state();
            const u32 n = ld<u32>(fs + 20) + 1;
            st<u32>(fs + 20, n);
            st_bytes(fs + 32, hptr((u32)a[2]), 16);
            return n;
          }));
  // (sp-init-fields! dest specs start end write-missing): the fields after start and before end,
  // from the specs' values (at 4), returns the specs after them
  set_sym("sp-init-fields!", add_goal_fn("sp-init-fields!", 5, [](const u64* a) -> u64 {
            const u32 fs = fake_state();
            const u32 calls = ld<u32>(fs + 4);
            st<u32>(fs + 4, calls + 1);
            const u32 dest = (u32)a[0];
            const u32 specs = (u32)a[1];
            const s32 start = (s32)a[2];
            const s32 end = (s32)a[3];
            for (s32 f = start + 1; f < end; f++) {
              u32 v = ld<u32>(specs + 16 * (f - start - 1) + 4);
              if (f == 10 || f == 12) {
                // x and z of the sprite: different for every particle
                v = Mips2C::f2u(Mips2C::u2f(v) + (float)calls * 0.5f);
              }
              st<u32>(dest + 4 * (f - start - 1), v);
            }
            return specs + 16 * (end - start - 1);
          }));
  // (sp-get-particle system group launch-state): the next of the fake's particles, #f at the end
  set_sym("sp-get-particle", add_goal_fn("sp-get-particle", 3, [](const u64* a) -> u64 {
            const u32 fs = fake_state();
            const u32 next = ld<u32>(fs + 8);
            if (next >= ld<u32>(fs + 12)) {
              return st();
            }
            st<u32>(fs + 8, next + 1);
            const u32 cpu = ld<u32>(fs + 16) + 144 * next;
            st<u32>(cpu + 128, true_sym());
            st<u32>(cpu + 140, (u32)a[1] * 16 + ((u32)a[2] == st() ? 1 : 2));
            return cpu;
          }));
  set_sym("cos", add_goal_fn("cos", 1, [](const u64* a) -> u64 {
            return f_bits(std::cos(bits_f(a[0]) * 0.0001f));
          }));
  set_sym("sin", add_goal_fn("sin", 1, [](const u64* a) -> u64 {
            return f_bits(std::sin(bits_f(a[0]) * 0.0001f));
          }));
  // (quaternion-axis-angle! quat x y z angle): the argument bits
  set_sym("quaternion-axis-angle!",
          add_goal_fn("quaternion-axis-angle!", 5, [](const u64* a) -> u64 {
            for (int i = 0; i < 4; i++) {
              st<u32>((u32)a[0] + 4 * i, (u32)a[i + 1]);
            }
            return a[0];
          }));
  // (sp-adjust-launch launchinfo cpuinfo specs)
  set_sym("sp-adjust-launch", add_goal_fn("sp-adjust-launch", 3, [](const u64* a) -> u64 {
            st<float>((u32)a[0], ld<float>((u32)a[0]) + 1.5f);
            st<float>((u32)a[1] + 16, ld<float>((u32)a[1] + 16) * 2.f);
            st<u32>((u32)a[1] + 20, ld<u16>((u32)a[2]));
            return 0;
          }));
  // (sp-euler-convert launchinfo cpuinfo)
  set_sym("sp-euler-convert", add_goal_fn("sp-euler-convert", 2, [](const u64* a) -> u64 {
            st<float>((u32)a[0] + 24, ld<float>((u32)a[0] + 24) * 0.25f);
            st<float>((u32)a[1] + 80, 0.5f);
            return 0;
          }));
  // (sp-rotate-system launchinfo cpuinfo transformq)
  set_sym("sp-rotate-system", add_goal_fn("sp-rotate-system", 3, [](const u64* a) -> u64 {
            for (int i = 0; i < 3; i++) {
              st<float>((u32)a[0] + 4 * i,
                        ld<float>((u32)a[0] + 4 * i) + ld<float>((u32)a[2] + 4 * i));
            }
            return 0;
          }));
  set_sym("new-sound-id", add_goal_fn("new-sound-id", 0, [](const u64*) -> u64 {
            const u32 fs = fake_state();
            const u32 id = ld<u32>(fs) + 1;
            st<u32>(fs, id);
            return id;
          }));
  // (sound-play-by-spec spec id pos)
  set_sym("sound-play-by-spec", add_goal_fn("sound-play-by-spec", 3, [](const u64* a) -> u64 {
            const u32 fs = fake_state();
            st<u32>(fs + 24, (u32)a[1]);
            st_bytes(fs + 48, hptr((u32)a[2]), 16);
            return a[1];
          }));
  set_sym("add-to-sprite-aux-list",
          add_goal_fn("add-to-sprite-aux-list", 3, [](const u64*) -> u64 { return 0; }));
  // birth functions (system cpuinfo launchinfo launcher launch-state)
  g_birth_funcs[0] = add_goal_fn("birth-fade", 5, [](const u64* a) -> u64 {
    st_vec((u32)a[2] + 32, 128.f, 64.f, 32.f, ld<float>((u32)a[2] + 44));
    st<float>((u32)a[1] + 12, 3.f);
    if ((u32)a[4] != st()) {
      st<float>((u32)a[4] + 24, ld<float>((u32)a[4] + 24) - 0.5f);  // fewer particles
    }
    return 0;
  });
  g_birth_funcs[1] = add_goal_fn("birth-sound", 5, [](const u64* a) -> u64 {
    st<float>((u32)a[2] + 44, 64.f);
    st<float>((u32)a[3] + 4, ld<float>((u32)a[3] + 4) + 0.3f);
    return 0;
  });
}

//! *particle-adgif-cache*: used, last key, last adgif, 80 keys, 80 adgif shaders. Some keys are
//! the given texture's.
u32 gen_adgif_cache(Gen& g, u64 tex) {
  const u32 cache = alloc_basic(0, 172 + 80 * 80);
  const u32 used = g.chance(0.1f) ? 80 : g.chance(0.1f) ? 0 : (u32)g.range(1, 79);
  st<u32>(cache, used);
  for (u32 k = 0; k < 80; k++) {
    st<u16>(cache + 12 + 2 * k, (u16)g.u32_());
  }
  if (used && g.chance(0.5f)) {
    st<u16>(cache + 12 + 2 * g.range(0, (s32)used - 1), adgif_key(tex));
  }
  for (u32 i = 0; i < 80 * 80; i += 4) {
    st<u32>(cache + 172 + i, g.u32_());
  }
  // the last one: the texture's (sometimes without a shader), or another
  st<u16>(cache + 4, g.chance(0.3f) ? adgif_key(tex) : (u16)g.u32_());
  st<u32>(cache + 8, g.chance(0.1f) || !used ? 0 : cache + 172 + 80 * g.range(0, (s32)used - 1));
  set_sym("*particle-adgif-cache*", cache);
  return cache;
}

u64 gen_texture_id(Gen& g) {
  const u32 tex = g.chance(0.5f) ? g.u32_() : (u32)g.range(0, 0xfffff) << 8;
  return (u64)(s64)(s32)tex;  // a sign-extended register, like lw
}

// (particle-adgif adgif texture-id)
void gen_particle_adgif(Case& c) {
  auto& g = c.g;
  const u64 tex = gen_texture_id(g);
  gen_adgif_cache(g, tex);
  c.args[0] = alloc(80);
  c.args[1] = g.chance(0.1f) ? tex & 0xffffffff : tex;
  for (int i = 2; i < 8; i++) {
    c.args[i] = g.u32_();
  }
}

//! a launch state (32 bytes): group item, flags, origin, sprite3d (a transformq), sprite, accum
u32 gen_launch_state(Gen& g, u32 binding) {
  const u32 state = alloc(32);
  const u32 item = alloc(32);
  for (u32 i = 0; i < 32; i += 4) {
    st<u32>(item + i, g.u32_());
  }
  st<u32>(item + 24, binding);
  st<u32>(state, item);
  st<u16>(state + 4, (u16)g.u32_());
  st<u32>(state + 8, g.u32_());
  const u32 tq = alloc(48);
  rand_vec(g, (float*)hptr(tq), -100.f, 100.f, 1.f);
  st<u32>(state + 12, g.pick(std::vector<u32>{st(), 0u, tq, tq}));
  const u32 sprite = alloc(16);
  st<u32>(sprite, g.u32_());
  st<u32>(state + 16, sprite);
  st<float>(state + 24, g.pick(std::vector<float>{0.f, 0.5f, 0.9f, 1.f, 2.5f, g.f(-2.f, 4.f)}));
  return state;
}

//! a launch control with n launch states (at 60, 32 bytes each), some for the launcher `bound`
u32 gen_launch_control(Gen& g, u32 bound) {
  const int n = g.range(1, 4);
  const u32 control = alloc_basic(0, 60 + 32 * n);
  for (u32 i = 0; i < 60; i += 4) {
    st<u32>(control + i, g.u32_());
  }
  st<u32>(control, (u32)n);
  if (g.chance(0.3f)) {
    st<u32>(control + 12, st());
  } else {
    const u32 proc = alloc_basic(0, 16);
    st<u16>(proc + 6, (u16)g.u32_());
    st<u32>(control + 12, proc);
  }
  for (int k = 0; k < n; k++) {
    const u32 s = control + 60 + 32 * k;
    const u32 item = alloc(32);
    st<u32>(item, g.chance(0.5f) ? bound : g.u32_());
    st<u32>(s, item);
    st<u16>(s + 4, (u16)(g.u32_() & (g.chance(0.5f) ? ~1u : ~0u)));
    for (u32 i = 8; i < 32; i += 4) {
      st<u32>(s + i, g.u32_());
    }
  }
  return control;
}

// (sp-launch-particles-var system launcher pos launch-state launch-control rate)
void gen_launch(Case& c) {
  auto& g = c.g;
  // the original's crash writes to address 2
  for (u32 i = 0; i < 16; i += 4) {
    st<u32>(i, g.u32_() | 1);
  }
  const u32 fs = alloc(64);
  for (u32 i = 0; i < 64; i += 4) {
    st<u32>(fs + i, 0);
  }
  set_sym("*launch-test*", fs);
  set_sym("*sp-launcher-enable*", g.chance(0.05f) ? st() : true_sym());
  set_sym("*sp-launcher-lock*", g.chance(0.15f) ? true_sym() : st());

  // the system and its particles (the pool sp-get-particle takes them from)
  const u32 system = alloc_basic(0, 48);
  st<u32>(system + 24, g.chance(0.4f) ? true_sym() : st());
  const u32 count = (u32)g.range(0, 8);
  const u32 pool = alloc(144 * std::max(count, 1u));
  for (u32 k = 0; k < count; k++) {
    const u32 cpu = pool + 144 * k;
    for (u32 i = 0; i < 144; i += 4) {
      st<u32>(cpu + i, g.u32_());
    }
    st<u32>(cpu, alloc(48));
    const u32 adgif = alloc(80);
    for (u32 i = 0; i < 80; i += 4) {
      st<u32>(adgif + i, g.u32_());
    }
    st<u32>(cpu + 4, adgif);
  }
  st<u32>(fs + 12, count);
  st<u32>(fs + 16, pool);

  // the launcher and its init specs (the fake sp-init-fields! copies their values)
  const u32 launcher = alloc_basic(0, 16);
  st<float>(launcher, g.pick(std::vector<float>{0.f, 0.3f, 0.99f, g.f(-1.f, 3.f)}));
  st<float>(launcher + 4, g.pick(std::vector<float>{0.f, 0.5f, 0.99f, -0.2f}));
  const u32 specs = alloc(16 * 48);
  for (u32 k = 0; k < 48; k++) {
    st<s16>(specs + 16 * k, (s16)k);
    st<u32>(specs + 16 * k + 4, g.u32_());
    st<float>(specs + 16 * k + 8, g.f(-100.f, 100.f));
  }
  st<u32>(launcher + 8, specs);
  auto spec = [&](u32 k) { return specs + 16 * k + 4; };
  const u64 tex = gen_texture_id(g);
  st<u32>(spec(0), (u32)tex);
  st<u32>(spec(3), g.chance(0.5f) ? 0 : g_birth_funcs[g.range(0, 1)]);
  st<float>(spec(5), g.pick(std::vector<float>{0.f, 0.2f, 0.5f, 1.f, 1.f, 2.f, 3.f, 5.f}));
  if (g.chance(0.4f)) {
    const u32 sound = alloc_basic(0, 16);
    st<float>(sound + 4, g.pick(std::vector<float>{0.1f, 0.5f, 1.f, 1.5f, g.f(0.f, 2.f)}));
    st<u32>(spec(6), sound);
  } else {
    st<u32>(spec(6), 0);
  }
  // the sprite: x y z sx, flag matrix rot sy, r g b a
  for (u32 k = 7; k <= 18; k++) {
    st<float>(spec(k), g.f_edge(-300.f, 300.f));
  }
  st<float>(spec(13), g.pick(std::vector<float>{g.f(-40000.f, 40000.f), 32767.9f, -32768.5f,
                                                g.f(-1e6f, 1e6f)}));
  for (u32 k = 15; k <= 18; k++) {
    st<float>(spec(k), g.chance(0.2f) ? g.f(255.f, 400.f) : g.f(0.f, 255.f));
  }
  // the cpuinfo from omega (12) on; the flags (104) at 42
  for (u32 k = 19; k <= 46; k++) {
    st<float>(spec(k), g.f(-10.f, 10.f));
  }
  u32 flags =
      g.u32_() & ~(4u | 8u | 16u | 32u | 128u | 256u | 512u | 1024u | 2048u | 4096u | 16384u);
  for (u32 bit : {4u, 8u, 16u, 32u, 128u, 256u, 512u, 4096u, 4096u, 16384u}) {
    if (g.chance(0.3f)) {
      flags |= bit;
    }
  }
  st<u32>(spec(42), flags);
  st<s16>(specs + 16 * 47, g.pick(std::vector<s16>{10, 40, 63, 64, 70, -1}));

  // launch state and control
  u32 state = st();
  u32 control = st();
  if (g.chance(0.6f)) {
    control = g.chance(0.6f) ? gen_launch_control(g, launcher) : st();
    const u32 binding = control != st() && g.chance(0.5f) ? launcher : 0;
    state = gen_launch_state(g, binding);
  }

  // *level*: the heaps of the two levels (the launcher in one of them, or neither)
  const u32 level = alloc(2740);
  for (u32 off : {124u, 2732u}) {
    const s32 lo = (s32)launcher - g.pick(std::vector<s32>{0, 16, 4096, -16, -4096});
    st<s32>(level + off, lo);
    st<s32>(level + off + 4, lo + g.pick(std::vector<s32>{16, 4096, 8192, 0}));
  }
  set_sym("*level*", level);
  const u32 tod = alloc_basic(0, 128);
  st_vec(tod + 108, g.f(0.f, 2.f), g.f(0.f, 2.f), g.f(0.f, 2.f), g.f(0.f, 2.f));
  set_sym("*time-of-day-context*", tod);
  gen_adgif_cache(g, tex);

  float pos[4];
  rand_vec(g, pos, -1e5f, 1e5f, 1.f);
  c.args[0] = system;
  c.args[1] = launcher;
  c.args[2] = alloc_vec(pos);
  c.args[3] = state;
  c.args[4] = control;
  c.args[5] = f_bits(g.pick(std::vector<float>{1.f, 1.f, 1.f, 0.5f, 2.f, 1.5f}));
  for (int i = 6; i < 8; i++) {
    c.args[i] = g.u32_();
  }
}

}  // namespace

void register_sparticle_launcher_tests() {
  add_test({"particle-adgif", "particle-adgif", 6000, setup_launcher, gen_particle_adgif, {}});
  add_test(
      {"sp-launch-particles-var", "sp-launch-particles-var", 6000, setup_launcher, gen_launch, {}});
}

}  // namespace tests
