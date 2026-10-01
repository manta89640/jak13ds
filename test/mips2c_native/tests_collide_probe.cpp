/*!
 * @file tests_collide_probe.cpp
 * (AI-assisted)
 * Tests for collide_probe.cpp: collide-probe-node, collide-probe-instance-tie.
 */

#include "collide_gen.h"

namespace tests {

using namespace harness;

namespace {

struct TreeGen {
  Gen& g;
  float range;     // sphere centers in [-range, range]
  int leaves = 0;  // upper bound of the items the probes can add
  bool instances;  // leaves are instance-tie arrays (collide-probe-instance-tie)

  void sphere(u32 addr) {
    const float r = g.chance(0.1f) ? 0.f : g.f(0.f, range * 0.3f);
    st_vec(addr, g.f_edge(-range, range), g.f_edge(-range, range), g.f_edge(-range, range), r);
  }

  //! a collide fragment array for an instance's prototype: length, then 32-byte fragments at 32
  u32 frag_array() {
    const int n = g.chance(0.1f) ? 0 : g.range(1, 12);
    const u32 arr = alloc_basic(0, 32 + 32 * (n + 8));
    st<u16>(arr + 2, (u16)n);
    for (int k = 0; k < n + 8; k++) {
      const u32 f = arr + 32 + 32 * k;
      st<u32>(f + 4, g.u32_());  // mesh
      sphere(f + 12);
      st<float>(f + 12 + 12, g.f(0.f, 3000.f));
    }
    leaves += std::max(n, 1) + 1;
    return arr;
  }

  //! count instance-ties (64 bytes), padded to a multiple of 4 with valid ones
  u32 instance_array(int count) {
    const int n = std::max(4, (count + 3) & ~3);
    const u32 arr = alloc_basic(0, 64 * n);
    for (int k = 0; k < n; k++) {
      const u32 inst = arr + 64 * k;
      for (int i = 0; i < 64; i += 4) {
        st<u32>(inst + i, g.u32_());
      }
      sphere(inst + 12);
      // origin matrix4h: rotation rows (4096 = 1.0), then the translation (* 64 when used)
      for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 4; c++) {
          st<s16>(inst + 28 + 8 * r + 2 * c,
                  (s16)(r == c ? g.range(2000, 6000) : g.range(-3000, 3000)));
        }
      }
      for (int c = 0; c < 4; c++) {
        st<s16>(inst + 52 + 2 * c, (s16)g.range(-(s32)(range / 64), (s32)(range / 64)));
      }
      st<u16>(inst + 34, (u16)g.range(0, 12000));               // max-scale
      st<u16>(inst + 42, g.chance(0.85f) ? 0 : (u16)g.u32_());  // flags
      const u32 bucket = alloc(160);
      st<u32>(bucket + 136, g.chance(0.1f) ? 0 : frag_array());
      st<u32>(inst + 8, bucket);
    }
    return arr;
  }

  /*!
   * count draw-nodes (32 bytes), padded to a multiple of 4, for a stack entry with a nonzero
   * flag. level 1: the nodes' children are collide fragments (collide-probe-node only); level 2:
   * leaf arrays (level 1 node arrays, or instance-tie arrays); level L: level L - 1 node arrays.
   * A node's flags are the flag of its child's stack entry: 0 if the child is a leaf array.
   */
  u32 node_array(int count, int level) {
    const int n = std::max(4, (count + 3) & ~3);
    const u32 arr = alloc_basic(0, 32 * n);
    for (int k = 0; k < n; k++) {
      const u32 node = arr + 32 * k;
      st<u32>(node, g.u32_());
      sphere(node + 12);
      st<float>(node + 8, g.f(0, 1e5f));
      const int children = g.chance(0.05f) ? 0 : g.range(1, 8);
      st<u8>(node + 2, (u8)children);
      if (level > 2) {
        st<u8>(node + 3, (u8)g.range(1, 255));
        st<u32>(node + 4, node_array(children, level - 1));
      } else if (level == 2) {
        st<u8>(node + 3, 0);
        st<u32>(node + 4, instances ? instance_array(children) : node_array(children, 1));
      } else {
        st<u8>(node + 3, (u8)g.u32_());  // not read
        st<u32>(node + 4, g.u32_());     // a collide fragment (not read)
        leaves++;
      }
    }
    return arr;
  }
};

void setup_probe() {
  set_sym("*collide-probe-stack*", kSpad + 4192);
}

void gen_probe(Case& c, bool instances) {
  auto& g = c.g;
  const float range = g.pick(std::vector<float>{3000.f, 40000.f, 300000.f});
  gen_collide_work(g, (s32)(range * g.f(0.1f, 1.f)));
  TreeGen t{g, range, 0, instances};
  u32 root;
  int count = g.chance(0.05f) ? 0 : g.range(1, 9);
  s32 flag = 1;
  const int kind = instances ? g.range(0, 9) : 0;
  if (kind == 0 || kind > 2) {
    // entry flag 1: the top nodes have node arrays as children
    root = t.node_array(count, instances ? 2 : g.range(2, 4));
  } else if (kind == 1) {
    root = t.instance_array(count);
    flag = 0;
  } else {
    root = t.instance_array(1);
    flag = -1;
  }
  const u32 list = alloc(16 + 16 * (t.leaves + 64));
  const int already = g.chance(0.5f) ? 0 : g.range(0, 40);
  st<u32>(list, (u32)already);
  for (int i = 0; i < already; i++) {
    st<u32>(list + 16 + 16 * i, g.u32_());
  }
  c.args[0] = root;
  c.args[1] = (u64)count;
  c.args[2] = list;
  c.args[3] = (u64)(s64)flag;
  for (int i = 4; i < 8; i++) {
    c.args[i] = g.u32_();
  }
}

}  // namespace

void register_collide_probe_tests() {
  add_test({"collide-probe-node",
            "collide-probe-node",
            6000,
            setup_probe,
            [](Case& c) { gen_probe(c, false); },
            {}});
  add_test({"collide-probe-instance-tie",
            "collide-probe-instance-tie",
            6000,
            setup_probe,
            [](Case& c) { gen_probe(c, true); },
            {}});
}

}  // namespace tests
