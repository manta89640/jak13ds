/*!
 * @file native_collide_probe.cpp
 * (AI-assisted)
 * Native versions of the mips2c functions in collide_probe.cpp. See game/mips2c/mips2c_native.h for
 * the rules (same float operations, grouped the same way, as the mips2c code).
 *
 * Both functions walk a tree of bounding spheres with a stack on the scratchpad
 * (*collide-probe-stack*, entries: child u32, count u16, flag u16) and add the leaves whose
 * sphere, moved into the frame of *collide-work*'s inverse matrix, overlaps its box to a
 * collide-list. The entries are written to the scratchpad like the original does.
 */

#include "game/kernel/jak1/kscheme.h"
#include "game/mips2c/jak1_functions/native_functions.h"

namespace Mips2C::jak1::native {

namespace {

u32 sym_addr(const char* name) {
  return ::jak1::intern_from_c(name).offset;
}

//! *collide-work*: the query box (ints) and the inverse matrix
struct ProbeWork {
  s32 bmin[4];
  s32 bmax[4];
  float m[4][4];
};

ProbeWork load_work(u32 cw) {
  ProbeWork w;
  memcpy(w.bmin, gptr(cw + 16), 16);
  memcpy(w.bmax, gptr(cw + 32), 16);
  memcpy(w.m, gptr(cw + 48), 64);
  return w;
}

//! pcgtw / por / ppach / dsll 16: is the int box [mn, mx] outside the query box in x, y or z?
inline bool box_outside(const s32 mn[3], const s32 mx[3], const ProbeWork& w) {
  bool out = false;
  for (int i = 0; i < 3; i++) {
    out |= (mn[i] > w.bmax[i]) | (w.bmin[i] > mx[i]);
  }
  return out;
}

/*!
 * A sphere (w: radius) moved by the inverse matrix (vmulaw acc = m3 * 1, vmaddax, vmadday,
 * vmaddz.xyz), then the box center -/+ radius as ints (ftoi0), tested against the query box.
 */
inline bool sphere_outside(const float s[4], const ProbeWork& w) {
  s32 mn[3], mx[3];
  for (int i = 0; i < 3; i++) {
    float acc = w.m[3][i] * 1.f;
    acc += w.m[0][i] * s[0];
    acc += w.m[1][i] * s[1];
    const float c = acc + w.m[2][i] * s[2];
    mn[i] = (s32)(c - s[3]);
    mx[i] = (s32)(c + s[3]);
  }
  return box_outside(mn, mx, w);
}

//! the probe stack on the scratchpad
struct ProbeStack {
  u32 base;
  u32 top;
  void push(u32 child, u32 count, u32 flag) {
    gstore<u32>(top, child);
    gstore<u16>(top + 4, (u16)count);
    gstore<u16>(top + 6, (u16)flag);
    top += 8;
  }
};

/*!
 * Visits count draw-nodes (32 bytes, sphere at 12) starting at node, in groups of 4 like the
 * original (a count of 0 visits 4). leaf(n) is called for the nodes that overlap the box.
 */
template <typename F>
inline void visit_nodes(u32 node, s64 count, u32 stride, const ProbeWork& w, F&& leaf) {
  for (;;) {
    // the original reads the 4 spheres of the group first
    float s[4][4];
    for (int k = 0; k < 4; k++) {
      memcpy(s[k], gptr(node + 12 + stride * k), 16);
    }
    for (int k = 0; k < 4; k++) {
      count--;
      if (!sphere_outside(s[k], w)) {
        leaf(node + stride * k);
      }
      if (k < 3 && count == 0) {
        break;
      }
    }
    if (count <= 0) {
      return;
    }
    node += 4 * stride;
  }
}

/*!
 * (collide-probe-node nodes count list) -> 0
 * Walks a tree of draw-nodes: nodes whose flags are nonzero have draw-node children (pushed on the
 * stack), the others have collide fragments as children, which are added to the collide-list with
 * inst #f.
 */
u64 collide_probe_node_impl(const NativeArgs& args) {
  static const u32 work_sym = sym_addr("*collide-work*");
  static const u32 stack_sym = sym_addr("*collide-probe-stack*");
  const u32 list = (u32)args.a[2];
  const ProbeWork w = load_work(gload<u32>(work_sym));
  ProbeStack stack{gload<u32>(stack_sym), 0};
  stack.top = stack.base;
  stack.push((u32)args.a[0], (u32)args.a[1], 1);

  u32 num = gload<u32>(list);
  u32 item = list + 16 + (num << 4);
  const u32 false_sym = (u32)args.st;

  while (stack.top != stack.base) {
    stack.top -= 8;
    const u32 nodes = gload<u32>(stack.top);
    const s64 count = gload<u16>(stack.top + 4);
    const u32 flag = gload<u16>(stack.top + 6);
    if (flag) {
      visit_nodes(nodes, count, 32, w, [&](u32 n) {
        stack.push(gload<u32>(n + 4), gload<u8>(n + 2), gload<u8>(n + 3));
      });
    } else {
      visit_nodes(nodes, count, 32, w, [&](u32 n) {
        gstore<u32>(item, gload<u32>(n + 4));
        gstore<u32>(item + 4, false_sym);
        item += 16;
        num++;
      });
    }
  }
  gstore<u32>(list, num);
  return 0;
}

//! vitof12: an s16 of a matrix4h row as a float, / 4096
inline float itof12(s32 v) {
  return ((float)v) * (1.f / 4096.f);
}

//! the 4 s16 of a 64-bit value, sign-extended (pextlh with 0, then psraw by shift)
inline void halves(s32 out[4], u64 v, int shift) {
  for (int i = 0; i < 4; i++) {
    out[i] = (s32)(u32)(((v >> (16 * i)) & 0xffff) << 16) >> shift;
  }
}

/*!
 * The collide fragments of an instance-tie's prototype: their spheres moved by the instance's
 * matrix (matrix4h, scaled by max-scale) and the inverse matrix; the ones that overlap the box
 * are added to the list with the instance.
 */
void probe_instance(u32 inst, u32 list, const ProbeWork& w) {
  if (gload<u16>(inst + 42) != 0) {
    return;  // flags
  }
  const u32 frags = gload<u32>(gload<u32>(inst + 8) + 136);  // bucket-ptr's collide-frag
  if (!frags) {
    return;
  }
  const u32 max_scale = gload<u16>(inst + 34);
  // the origin matrix4h (lq aligns down)
  const u32 q0 = (inst + 28) & ~15u;
  const u32 q1 = (inst + 44) & ~15u;
  s32 r0[4], r1[4], r2[4], tr[4];
  halves(r0, gload<u64>(q0), 16);
  halves(r1, gload<u64>(q0 + 8), 16);
  halves(r2, gload<u64>(q1), 16);
  halves(tr, gload<u64>(q1 + 8), 10);
  float center[4];
  memcpy(center, gptr(inst + 12), 16);

  // vcallms 32: the instance matrix in the frame of the inverse matrix
  float t[4], a[4], b[4], c[4];
  for (int i = 0; i < 4; i++) {
    t[i] = (float)tr[i];
    a[i] = itof12(r0[i]);
    b[i] = itof12(r1[i]);
    c[i] = itof12(r2[i]);
  }
  for (int i = 0; i < 3; i++) {
    t[i] = t[i] + center[i];
  }
  const float scale = itof12((s32)max_scale);
  float mt[4], ma[4], mb[4], mc[4];
  for (int i = 0; i < 3; i++) {
    float acc = w.m[0][i] * t[0];
    acc += w.m[1][i] * t[1];
    acc += w.m[2][i] * t[2];
    mt[i] = acc + w.m[3][i] * 1.f;
    acc = w.m[0][i] * a[0];
    acc += w.m[1][i] * a[1];
    acc += w.m[2][i] * a[2];
    ma[i] = acc + w.m[3][i] * 0.f;
    acc = w.m[0][i] * b[0];
    acc += w.m[1][i] * b[1];
    acc += w.m[2][i] * b[2];
    mb[i] = acc + w.m[3][i] * 0.f;
    acc = w.m[0][i] * c[0];
    acc += w.m[1][i] * c[1];
    acc += w.m[2][i] * c[2];
    mc[i] = acc + w.m[3][i] * 0.f;
  }

  u32 num = gload<u32>(list);
  u32 item = list + 16 + (num << 4);
  u32 frag = frags + 32;
  // max(count, 1) fragments: the original tests the first one before checking the count
  s32 left = gload<u16>(frags + 2);
  do {
    float s[4];
    memcpy(s, gptr((frag + 12) & ~15u), 16);  // lq, or lqc2 of an aligned address
    const float rad = scale * s[3];
    s32 mn[3], mx[3];
    for (int i = 0; i < 3; i++) {
      float acc = mt[i] * 1.f;
      acc += ma[i] * s[0];
      acc += mb[i] * s[1];
      const float p = acc + mc[i] * s[2];
      mx[i] = (s32)(p + rad);
      mn[i] = (s32)(p - rad);
    }
    if (!box_outside(mn, mx, w)) {
      gstore<u32>(item + 4, inst);
      gstore<u32>(item, gload<u32>(frag + 4));
      item += 16;
      num++;
    }
    left--;
    frag += 32;
  } while (left > 0);
  gstore<u32>(list, num);
}

/*!
 * (collide-probe-instance-tie tree count list flag) -> 0
 * Like collide-probe-node for a tree of instance-ties: stack entries with a positive flag are
 * draw-nodes, 0 an array of instance-ties (64 bytes; the ones that overlap are pushed with flag
 * -1), and -1 one instance, whose prototype's collide fragments are tested (probe_instance).
 */
u64 collide_probe_instance_tie_impl(const NativeArgs& args) {
  static const u32 work_sym = sym_addr("*collide-work*");
  static const u32 stack_sym = sym_addr("*collide-probe-stack*");
  const u32 list = (u32)args.a[2];
  const ProbeWork w = load_work(gload<u32>(work_sym));
  ProbeStack stack{gload<u32>(stack_sym), 0};
  stack.top = stack.base;
  stack.push((u32)args.a[0], (u32)args.a[1], (u32)args.a[3]);

  while (stack.top != stack.base) {
    stack.top -= 8;
    const u32 elts = gload<u32>(stack.top);
    const s32 flag = gload<s16>(stack.top + 6);
    const s64 count = gload<u16>(stack.top + 4);
    if (flag < 0) {
      probe_instance(elts, list, w);
    } else if (flag == 0) {
      visit_nodes(elts, count, 64, w, [&](u32 inst) { stack.push(inst, 0, 0xffff); });
    } else {
      visit_nodes(elts, count, 32, w, [&](u32 n) {
        stack.push(gload<u32>(n + 4), gload<u8>(n + 2), gload<u8>(n + 3));
      });
    }
  }
  return 0;
}

}  // namespace

const NativeImpl collide_probe_node = MIPS2C_NATIVE_IMPL(collide_probe_node_impl, 0, 0);
const NativeImpl collide_probe_instance_tie =
    MIPS2C_NATIVE_IMPL(collide_probe_instance_tie_impl, 0, 0);

}  // namespace Mips2C::jak1::native
