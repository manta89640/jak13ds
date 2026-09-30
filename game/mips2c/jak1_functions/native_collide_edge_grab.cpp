/*!
 * @file native_collide_edge_grab.cpp
 * (AI-assisted)
 * Native versions of the mips2c functions in collide_edge_grab.cpp (ledge grabbing). See
 * game/mips2c/mips2c_native.h for the rules (same float operations, grouped the same way, as the
 * mips2c code).
 *
 * collide-edge-work: ccache 0, cshape 4, num-verts 8, num-edges 12, num-tris 16,
 * within-reach-box 64, within-reach-box4w 96, split-dists 168, verts 640 (64 x 16 bytes),
 * edges 1664 (96 x 48: ignore, etri, vertex pointers, outward, edge-vec-norm),
 * tris 6272 (48 x 32: ctri, normal), hold-list 7808.
 */

#include <cmath>
#include <cstdio>

#include "game/kernel/jak1/kscheme.h"
#include "game/mips2c/jak1_functions/native_functions.h"

namespace Mips2C::jak1::native {

namespace {

u32 sym_addr(const char* name) {
  return ::jak1::intern_from_c(name).offset;
}

inline bool sign_bit(float f) {
  return (s32)f2u(f) < 0;
}

//! vrsqrt Q, vf0.w, x
inline float rsqrt_q(float x) {
  const float s = std::sqrt(std::abs(x));
  return s == 0 ? 0.f : 1.f / s;
}

//! out.xyz = (acc = a.yzx * b.zxy) - b.yzx * a.zxy: vopmula acc, a, b then vopmsub out, b, a.
inline void opm(float out[4], const float a[4], const float b[4]) {
  const float a0 = a[1] * b[2];
  const float a1 = a[2] * b[0];
  const float a2 = a[0] * b[1];
  const float n0 = a0 - b[1] * a[2];
  const float n1 = a1 - b[2] * a[0];
  const float n2 = a2 - b[0] * a[1];
  out[0] = n0;
  out[1] = n1;
  out[2] = n2;
}

/*!
 * (method 16 collide-edge-work) = find-grabbable-tris! (this)
 * The collide cache's triangles that could be grabbed: mode 0 or 16, overlapping the reach box,
 * facing up (normal y at least 0.707), not of pat event 2. Stores them (the cache triangle and
 * its normal) in the work's tris.
 */
u64 method_16_collide_edge_work_impl(const NativeArgs& args) {
  const u32 work = (u32)args.a[0];
  const u32 cache = gload<u32>(work);
  s32 bmin[4], bmax[4];
  gload_q(bmin, (work + 96) & ~15u);
  gload_q(bmax, (work + 112) & ~15u);
  u32 tri = cache + 4908;
  u32 out = work + 6272;
  u32 count = 0;
  for (u32 n = gload<u32>(cache); n != 0; n--) {
    const u32 pat = gload<u32>(tri + 48);
    const u32 mode = pat & 56;
    if (mode != 0 && mode != 16) {
      tri += 64;
      continue;
    }
    if (pat & 5) {
      // noentity or noedge: the original goes back to the loop without moving to the next
      // triangle, so it tests this one again until the count runs out
      break;
    }
    float v0[4], v1[4], v2[4];
    gload_q(v0, tri);
    gload_q(v1, tri + 16);
    gload_q(v2, tri + 32);
    s32 tmin[3], tmax[3];
    for (int i = 0; i < 3; i++) {
      tmin[i] = (s32)std::min(std::min(v0[i], v1[i]), v2[i]);
      tmax[i] = (s32)std::max(std::max(v0[i], v1[i]), v2[i]);
    }
    bool outside = false;
    for (int i = 0; i < 3; i++) {
      outside |= (tmin[i] > bmax[i]) | (bmin[i] > tmax[i]);
    }
    if (outside) {
      tri += 64;
      continue;
    }
    float e4[4], e5[4], nrm[4];
    for (int i = 0; i < 3; i++) {
      e4[i] = v1[i] - v0[i];
      e5[i] = v2[i] - v0[i];
    }
    opm(nrm, e4, e5);
    // w of the register is never set (zero in verify mode)
    nrm[3] = 0.f;
    const float sq[3] = {nrm[0] * nrm[0], nrm[1] * nrm[1], nrm[2] * nrm[2]};
    float len2 = 1.f * sq[0];
    len2 += 1.f * sq[1];
    len2 = len2 + 1.f * sq[2];
    const float q = rsqrt_q(len2);
    for (int i = 0; i < 3; i++) {
      nrm[i] = nrm[i] * q;
    }
    if (nrm[1] < 0.707f || ((pat >> 14) & 63) == 2) {
      tri += 64;
      continue;
    }
    if (count == 48) {
      printf("ERROR: Exceeded max # of grabbable tris\n");
      gstore<u32>(work + 16, 48);
      return 0;
    }
    count++;
    gstore<u32>(out, tri);
    gstore_q(out + 16, nrm);
    out += 32;
    tri += 64;
  }
  gstore<u32>(work + 16, count);
  return 0;
}

constexpr u64 kFalse = 0;  // marker for "no vertex / no edge" (the original uses #f)

/*!
 * The work's vertex within sqrt(1677.7216) of v, or a new one (u64 like the original's register,
 * kFalse if the 64 vertices are used up).
 */
u64 find_vertex(u64 work64, const float v[4]) {
  const u32 work = (u32)work64;
  const u32 nverts = gload<u32>(work + 8);
  u32 k = 0;
  for (; k != nverts; k++) {
    float p[4], d[4];
    gload_q(p, work + 640 + 16 * k);
    for (int i = 0; i < 4; i++) {
      d[i] = p[i] - v[i];
    }
    for (int i = 0; i < 4; i++) {
      d[i] = d[i] * d[i];
    }
    d[0] = d[0] + d[1];
    d[0] = d[0] + d[2];
    if (d[0] <= 1677.7216f) {
      return work64 + 640 + 16 * k;
    }
  }
  if (nverts == 64) {
    return kFalse;
  }
  gstore_q(work + 640 + 16 * k, v);
  gstore<u32>(work + 8, nverts + 1);
  return work64 + 640 + 16 * k;
}

/*!
 * Adds the edge a -> b of the work triangle etri. If a neighbour already added b -> a, the edge
 * is shared (marked ignored). New edges get the outward direction (horizontal, pointing away from
 * the triangle) and the edge direction; an edge the player is inside of is marked ignored.
 * Returns the edge, or kFalse if the 96 edges are used up.
 */
u64 add_edge(u64 work64, u64 etri, u64 a, u64 b, const float player[4], u64 st) {
  const u32 work = (u32)work64;
  const u64 key = (a << 32) | b;
  const u32 nedges = gload<u32>(work + 12);
  u32 e = work + 1664;
  for (u32 k = 0; k != nedges; k++, e += 48) {
    if (gload<u64>(e + 8) == key) {
      gstore<u32>(e, 0);
      return e;
    }
  }
  if (nedges == 96) {
    return kFalse;
  }
  gstore<u32>(e, (u32)st);
  gstore<u32>(e + 8, (u32)a);
  gstore<u32>(e + 12, (u32)b);
  gstore<u32>(e + 4, (u32)etri);
  gstore<u32>(work + 12, nedges + 1);

  float va[4], vb[4], ev[4], d[4], sq[4];
  gload_q(va, (u32)a);
  gload_q(vb, (u32)b);
  for (int i = 0; i < 4; i++) {
    ev[i] = vb[i] - va[i];
    d[i] = player[i] - va[i];
  }
  for (int i = 0; i < 4; i++) {
    sq[i] = ev[i] * ev[i];
  }
  float out[4] = {0.f - ev[2], 0.f, 0.f + ev[0], 1.f};
  sq[0] = sq[0] + sq[1];
  sq[0] = sq[0] + sq[2];
  float od[4], oo[4];
  for (int i = 0; i < 4; i++) {
    od[i] = out[i] * d[i];
    oo[i] = out[i] * out[i];
  }
  od[0] = od[0] + od[2];
  oo[0] = oo[0] + oo[2];
  const float q = rsqrt_q(oo[0]);
  if (od[0] < 0.f) {
    gstore<u32>(e, 0);
    return e;
  }
  for (int i = 0; i < 3; i++) {
    out[i] = out[i] * q;
  }
  const float q2 = rsqrt_q(sq[0]);
  ev[3] = 1.f * 1.f;
  gstore_q(e + 16, out);
  for (int i = 0; i < 3; i++) {
    ev[i] = ev[i] * q2;
  }
  gstore_q(e + 32, ev);
  return e;
}

/*!
 * (method 15 collide-edge-work) = find-grabbable-edges! (this)
 * The edges of the grabbable triangles, with their vertices merged: edges shared by two triangles
 * and edges the player is inside of are marked ignored.
 */
u64 method_15_collide_edge_work_impl(const NativeArgs& args) {
  const u64 work64 = args.a[0];
  const u32 work = (u32)work64;
  float player[4];
  gload_q(player, gload<u32>(work + 4) + 12);  // cshape trans
  player[1] = 0.f + gload<float>(work + 68);            // within-reach-box min y
  u32 tri = work + 6272;
  u64 tri64 = work64 + 6272;
  for (u32 n = gload<u32>(work + 16); n != 0; n--, tri += 32, tri64 += 32) {
    const u32 ctri = gload<u32>(tri);
    float v[3][4];
    for (int k = 0; k < 3; k++) {
      gload_q(v[k], ctri + 16 * k);
    }
    u64 vert[3];
    for (int k = 0; k < 3; k++) {
      vert[k] = find_vertex(work64, v[k]);
      if (vert[k] == kFalse) {
        printf("ERROR: Too many edge verts found in edge grab!\n");
        return 0;
      }
    }
    for (int k = 0; k < 3; k++) {
      const u64 e = add_edge(work64, tri64, vert[k], vert[(k + 1) % 3], player, args.st);
      // (the original tests the wrong register after the third edge: no error then)
      if (e == kFalse && k < 2) {
        printf("ERROR: Too many edges found in edge grab!\n");
        return 0;
      }
    }
  }
  return 0;
}

/*!
 * (method 10 collide-edge-hold-list) = add-to-list! (this item)
 * Records the item's center point in the attempts, and inserts the item into the list sorted by
 * rating (after the items with an equal or lower rating).
 */
u64 method_10_collide_edge_hold_list_impl(const NativeArgs& args) {
  const u32 list = (u32)args.a[0];
  const u32 item = (u32)args.a[1];
  const u32 st = (u32)args.st;
  const u32 attempts = gload<u32>(list + 4);
  u8 center[16];
  memcpy(center, gptr(item + 16), 16);
  const u32 head = gload<u32>(list + 8);
  const float rating = gload<float>(item + 4);
  gstore<u32>(list + 4, attempts + 1);
  gstore_bytes(list + 1552 + (attempts << 4), center, 16);
  if (head == st) {
    gstore<u32>(list + 8, item);
    gstore<u32>(item, st);
    return 0;
  }
  if (rating < gload<float>(head + 4)) {
    gstore<u32>(list + 8, item);
    gstore<u32>(item, head);
    return 0;
  }
  u32 prev = head;
  for (;;) {
    const u32 next = gload<u32>(prev);
    if (next == st) {
      gstore<u32>(prev, item);
      gstore<u32>(item, st);
      return 0;
    }
    if (rating < gload<float>(next + 4)) {
      gstore<u32>(prev, item);
      gstore<u32>(item, next);
      return 0;
    }
    prev = next;
  }
}

/*!
 * (method 18 collide-edge-work) = find-best-grab! (this hold-list info) -> symbol
 * Takes the best rated items off the hold list (up to 16) and returns #t for the first one
 * check-grab-for-collisions accepts. Items it rejects are moved along their edge by the
 * split-dists (both ways for a new item), and put back on the list if should-add-to-list? still
 * likes them. As in the original, whether a moved point is still on the edge is tested with the y
 * product alone (the sign of the second lane of the 64-bit register), not the dot product.
 */
u64 method_18_collide_edge_work_impl(const NativeArgs& args) {
  static const u32 work_type_sym = sym_addr("collide-edge-work");
  static const u32 list_type_sym = sym_addr("collide-edge-hold-list");
  static const u32* add_stub = native_stub_slot("(method 10 collide-edge-hold-list)");
  const u64 work = args.a[0];
  const u64 list64 = args.a[1];
  const u32 list = (u32)list64;
  const u64 st = args.st;

  auto check_grab = [&](u32 item) {
    const u32 fn = gload<u32>(gload<u32>(work_type_sym) + 92);
    const u64 call_args[8] = {work,      item,      args.a[2], args.a[3],
                              args.a[4], args.a[5], args.a[6], args.a[7]};
    return native_call_goal(fn, call_args, args);
  };
  auto should_add = [&](u32 item, u32 edge) {
    const u32 fn = gload<u32>(gload<u32>(work_type_sym) + 84);
    const u64 call_args[8] = {work,      item,      edge,      args.a[3],
                              args.a[4], args.a[5], args.a[6], args.a[7]};
    return native_call_goal(fn, call_args, args);
  };
  auto add_to_list = [&](u32 item, u32 edge) {
    const u32 fn = gload<u32>(gload<u32>(list_type_sym) + 56);
    const u64 call_args[8] = {list64,    item,      edge,      args.a[3],
                              args.a[4], args.a[5], args.a[6], args.a[7]};
    if (fn == *add_stub) {
      method_10_collide_edge_hold_list_impl(NativeArgs{call_args, args.pp, args.st, args.stack});
    } else {
      native_call_goal(fn, call_args, args);
    }
  };
  // the split vector: the edge direction times a split distance
  auto split_vec = [&](float sv[3], u32 edge, s64 index) {
    const float sd = gload<float>((u32)work + 168 + (u32)(index << 2));
    float n[4];
    memcpy(n, gptr(edge + 32), 16);
    for (int i = 0; i < 3; i++) {
      sv[i] = n[i] * sd;
    }
  };
  // (p - vertex) * sv, then the sign of y
  auto past_end = [&](const float p[4], u32 vertex, const float sv[3], bool vertex_first) {
    float v[4], t[3];
    memcpy(v, gptr(vertex), 16);
    for (int i = 0; i < 3; i++) {
      t[i] = vertex_first ? v[i] - p[i] : p[i] - v[i];
    }
    for (int i = 0; i < 3; i++) {
      t[i] = t[i] * sv[i];
    }
    return sign_bit(t[1]);
  };

  for (int iter = 16; iter != 0; iter--) {
    u32 item = gload<u32>(list + 8);
    if (item == (u32)st) {
      return st;
    }
    if (check_grab(item) != st) {
      return st + 8;  // #t
    }
    const u32 next = gload<u32>(item);
    const s64 split = gload<s8>(item + 8);
    gstore<u32>(list + 8, next);
    const u32 edge = gload<u32>(item + 12);
    float c[4];
    memcpy(c, gptr(item + 16), 16);

    if (split > 0) {
      if (split == 2) {
        continue;
      }
      float sv[3];
      split_vec(sv, edge, split);
      float pos[4] = {c[0] + sv[0], c[1] + sv[1], c[2] + sv[2], 1.f};
      if (past_end(pos, gload<u32>(edge + 12), sv, true)) {
        continue;
      }
      gstore_bytes(item + 16, pos, 16);
      if (should_add(item, edge) == st) {
        continue;
      }
      add_to_list(item, edge);
      gstore<s8>(item + 8, (s8)(split + 1));
      continue;
    }
    if (split < 0) {
      const s64 s = -split;
      if (s == 2) {
        continue;
      }
      float sv[3];
      split_vec(sv, edge, s);
      float neg[4] = {c[0] - sv[0], c[1] - sv[1], c[2] - sv[2], 1.f};
      if (past_end(neg, gload<u32>(edge + 8), sv, false)) {
        continue;
      }
      gstore_bytes(item + 16, neg, 16);
      if (should_add(item, edge) == st) {
        continue;
      }
      add_to_list(item, edge);
      gstore<s8>(item + 8, (s8)(-(s + 1)));
      continue;
    }

    // a new item: try both directions. The second goes to a newly allocated item if the first
    // was added.
    float sv[3];
    split_vec(sv, edge, 0);
    float pos[4] = {c[0] + sv[0], c[1] + sv[1], c[2] + sv[2], 1.f};
    float neg[4] = {c[0] - sv[0], c[1] - sv[1], c[2] - sv[2], 1.f};
    bool allocated = false;
    if (!past_end(pos, gload<u32>(edge + 12), sv, true)) {
      gstore_bytes(item + 16, pos, 16);
      if (should_add(item, edge) != st) {
        add_to_list(item, edge);
        gstore<s8>(item + 8, 1);
        const u32 allocs = gload<u32>(list);
        if (allocs == 32) {
          continue;
        }
        allocated = true;
        item = list + 16 + (u32)(s32)(allocs * 48);
      }
    }
    if (past_end(neg, gload<u32>(edge + 8), sv, false)) {
      continue;
    }
    gstore_bytes(item + 16, neg, 16);
    if (should_add(item, edge) == st) {
      continue;
    }
    add_to_list(item, edge);
    if (allocated) {
      gstore<u32>(list, gload<u32>(list) + 1);
    }
    gstore<s8>(item + 8, -1);
  }
  return st;
}

}  // namespace

const NativeImpl method_15_collide_edge_work =
    MIPS2C_NATIVE_IMPL(method_15_collide_edge_work_impl, 0, 0);
const NativeImpl method_16_collide_edge_work =
    MIPS2C_NATIVE_IMPL(method_16_collide_edge_work_impl, 0, 0);
const NativeImpl method_18_collide_edge_work =
    MIPS2C_NATIVE_IMPL(method_18_collide_edge_work_impl, 0, 0);
const NativeImpl method_10_collide_edge_hold_list =
    MIPS2C_NATIVE_IMPL(method_10_collide_edge_hold_list_impl, 0, 0);

}  // namespace Mips2C::jak1::native
