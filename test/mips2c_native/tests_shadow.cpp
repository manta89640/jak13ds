/*!
 * @file tests_shadow.cpp
 * (AI-assisted)
 * Tests for shadow.cpp: shadow-execute (the EE half of the shadow volume renderer, with the 14
 * mips2c functions it calls).
 *
 * The inputs are like draw-bones-shadow's: a chain of shadow-dma-packets (tag with the next packet
 * in vif1, shadow-settings, a ref to the shadow-geo, a ref to the joint matrices, end), shadow-geos
 * with the header's counts and offsets, vertices with 1 or 2 joints, single and double triangles
 * and edges (u8 indices, faces 0, open edges 255), the bone matrices, *math-camera*'s camera-rot
 * and d, *shadow-data*'s DMA templates and a *gsf-buffer* of garbage. The settings' flags take
 * every combination of the bits shadow-execute tests (1: camera-pos above the bottom plane, 4:
 * planes as they are, 8: top clip, 32: disable-draw), with the camera and the locus on both
 * sides of the planes. Chains of 0 to 6 packets, empty geos and big ones, and a crafted case:
 * vertices exactly on the plane at the near clip distance (the dual's test sum +0, decided by a
 * stale register).
 */

#include <algorithm>
#include <utility>

#include "fakes.h"

namespace tests {

using namespace harness;

namespace {

constexpr u32 kGsfSize = 0x6000;

/*!
 * Is the FPU in default NaN mode (the 3DS's, run.sh arm and vfp)? Without it, which of two NaNs
 * an operation returns, and the sign of a NaN a fused multiply-subtract negates, depend on how the
 * compiler ordered the operands, so the two versions can differ in NaN bits only. Then the
 * shadow-geos are "quiet": nothing reads bytes that aren't floats as vertices (all indices in
 * range, zero padding after the vertex table and the triangle tables, no garbage), so the only
 * NaNs are the ones operations make (0 / 0), which are all the same. In default NaN mode the
 * geos have garbage: the groups of 4 triangles past a table's end, indices past the vertices.
 */
bool default_nan_mode() {
#if defined(__aarch64__)
  u64 fpcr;
  asm volatile("mrs %0, fpcr" : "=r"(fpcr));
  return fpcr & (1u << 25);
#elif defined(__arm__)
  u32 fpscr;
  asm volatile("vmrs %0, fpscr" : "=r"(fpscr));
  return fpscr & (1u << 25);
#else
  return false;
#endif
}

void setup_shadow() {
  set_sym("flush-cache", add_goal_fn("flush-cache", 1, [](const u64*) -> u64 { return 0; }));
  // (camera-pos): the vector the case put in *shadow-test-camera*
  set_sym("camera-pos", add_goal_fn("camera-pos", 0, [](const u64*) -> u64 {
            return sym_value("*shadow-test-camera*");
          }));
}

//! a rotation (rows) from a random unit quaternion, or the identity
void rand_rotation(Gen& g, float r[3][3], bool identity) {
  float q[4] = {0, 0, 0, 1};
  if (!identity) {
    float n = 0;
    for (int i = 0; i < 4; i++) {
      q[i] = g.f(-1.f, 1.f);
      n += q[i] * q[i];
    }
    n = std::sqrt(n);
    if (n < 1e-3f) {
      q[0] = q[1] = q[2] = 0;
      q[3] = n = 1;
    }
    for (int i = 0; i < 4; i++) {
      q[i] /= n;
    }
  }
  const float x = q[0], y = q[1], z = q[2], w = q[3];
  r[0][0] = 1 - 2 * (y * y + z * z);
  r[0][1] = 2 * (x * y + z * w);
  r[0][2] = 2 * (x * z - y * w);
  r[1][0] = 2 * (x * y - z * w);
  r[1][1] = 1 - 2 * (x * x + z * z);
  r[1][2] = 2 * (y * z + x * w);
  r[2][0] = 2 * (x * z + y * w);
  r[2][1] = 2 * (y * z - x * w);
  r[2][2] = 1 - 2 * (x * x + y * y);
}

//! a 4x4 matrix (rows 0-2 rotation, row 3 translation and w 1) at addr
void st_matrix(u32 addr, const float r[3][3], float tx, float ty, float tz) {
  st_vec(addr, r[0][0], r[0][1], r[0][2], 0.f);
  st_vec(addr + 16, r[1][0], r[1][1], r[1][2], 0.f);
  st_vec(addr + 32, r[2][0], r[2][1], r[2][2], 0.f);
  st_vec(addr + 48, tx, ty, tz, 1.f);
}

struct Scene {
  bool crafted = false;  // camera space = world space, plane z = 2 * d (see gen_shadow)
  float d = 1024.f;      // *math-camera* d
  float obj[3] = {};     // where the objects are (camera / world space)
};

struct GeoCounts {
  int verts = 0, twos = 0, tris1 = 0, edges1 = 0, tris2 = 0, edges2 = 0, joints = 1;
};

/*!
 * A shadow-geo (its header 16-byte aligned like the real ones: total-size of an art-element) and
 * its joint matrices. Returns the header address; *qwc: total-size.
 */
u32 make_geo(Gen& g, const Scene& sc, const GeoCounts& n, u32* qwc_out, u32* mtx_out) {
  const bool quiet = !default_nan_mode();
  const int V = n.verts;
  // the tables, in an order and alignment that vary (the vertices 16-byte aligned: lqc2)
  enum { VERTS, REFS, TRIS1, EDGES1, TRIS2, EDGES2, NUM };
  int order[NUM] = {VERTS, REFS, TRIS1, EDGES1, TRIS2, EDGES2};
  if (g.chance(0.3f)) {
    for (int i = NUM - 1; i > 0; i--) {
      std::swap(order[i], order[g.range(0, i)]);
    }
  }
  const u32 sizes[NUM] = {16u * V, 2u * V, 4u * n.tris1, 4u * n.edges1, 4u * n.tris2,
                          4u * n.edges2};
  u32 ofs[NUM];
  u32 at = 48;
  for (int k : order) {
    // only 4-byte aligned (the 4 at a time loops align down)
    const bool tight = !quiet && g.chance(0.15f);
    const u32 align = k == VERTS ? 16 : (k == REFS ? 2 : (tight ? 4 : 16));
    at = (at + align - 1) & ~(align - 1);
    ofs[k] = at;
    at += sizes[k];
    if (quiet && k == VERTS) {
      at += 128;  // shadow-calc-dual-verts loads up to 7 quadwords past the vertices
    } else if (quiet && (k == TRIS1 || k == TRIS2)) {
      at += 16;  // the last group of 4 triangles: (0 0 0) after the table
    }
  }
  u32 qwc = (at + 15) / 16 + (g.chance(0.3f) ? g.range(0, 3) : 0);
  const u32 hdr = alloc(16 * qwc + 64, 16);
  for (u32 i = 0; i < 16 * qwc + 64; i += 4) {
    st<u32>(hdr + i, (quiet || g.chance(0.5f)) ? 0 : g.u32_());
  }
  st<u32>(hdr + 0, qwc);
  st<u32>(hdr + 4, (u32)n.joints);
  st<u16>(hdr + 8, (u16)V);
  st<u16>(hdr + 10, (u16)n.twos);
  st<u16>(hdr + 12, (u16)n.tris1);
  st<u16>(hdr + 14, (u16)n.edges1);
  st<u16>(hdr + 16, (u16)n.tris2);
  st<u16>(hdr + 18, (u16)n.edges2);
  for (int k = 0; k < NUM; k++) {
    st<u32>(hdr + 20 + 4 * k, ofs[k]);
  }

  // joint matrices: 128 bytes each (the first 64 a transform, the rest garbage), rotation +
  // translation to the object's place in camera space
  const int J = n.joints;
  const u32 mtx = alloc(128 * J + 16, 16);
  for (int j = 0; j < J; j++) {
    float r[3][3];
    rand_rotation(g, r, sc.crafted || g.chance(0.1f));
    float t[3];
    for (int i = 0; i < 3; i++) {
      t[i] = sc.crafted ? 0.f : sc.obj[i] + g.f_edge(-2000.f, 2000.f);
    }
    st_matrix(mtx + 128 * j, r, t[0], t[1], t[2]);
    for (u32 i = 64; i < 128; i += 4) {
      if (quiet) {
        st<float>(mtx + 128 * j + i, g.f(-2.f, 2.f));
      } else {
        st<u32>(mtx + 128 * j + i, g.u32_());
      }
    }
  }
  // joint indices: the matrix after the first one, so < joints - 1 in the real data
  auto joint = [&]() -> u8 {
    if (J <= 1 || (!quiet && g.chance(0.02f))) {
      return (u8)g.range(0, std::max(0, J - 1));
    }
    return (u8)g.range(0, J - 2);
  };

  // vertices: 1 joint (w: 1 or other values), then 2 joints (w: the weight)
  for (int v = 0; v < V; v++) {
    const u32 a = hdr + ofs[VERTS] + 16 * v;
    const bool two = v >= V - n.twos;
    float w = two ? (g.chance(0.2f) ? (float)g.range(0, 1) : g.f(0.f, 1.f))
                  : (g.chance(0.8f) ? 1.f : g.f_edge(-1.f, 2.f));
    if (sc.crafted) {
      // on the plane z = 2d (the duals then are the vertices), x 0 for some
      st_vec(a, g.chance(0.5f) ? 0.f : g.f(-3000.f, 3000.f), g.f(-3000.f, 3000.f),
             g.chance(0.6f) ? 2.f * sc.d : g.f(0.f, 4.f * sc.d), two ? w : 1.f);
    } else {
      st_vec(a, g.f_edge(-3000.f, 3000.f), g.f_edge(-3000.f, 3000.f), g.f_edge(-3000.f, 3000.f), w);
    }
    st<u8>(hdr + ofs[REFS] + 2 * v, joint());
    st<u8>(hdr + ofs[REFS] + 2 * v + 1, joint());
  }
  // triangles: 3 vertex indices (sometimes past the vertices: garbage, but in the scratchpad),
  // faces 0 (sometimes not)
  auto index = [&]() -> u8 {
    if (V == 0 || (!quiet && g.chance(0.01f))) {
      return quiet ? 0 : (u8)g.range(0, 255);
    }
    return (u8)g.range(0, V - 1);
  };
  auto fill_tris = [&](u32 a, int count) {
    for (int t = 0; t < count; t++) {
      st<u8>(a + 4 * t, index());
      st<u8>(a + 4 * t + 1, index());
      st<u8>(a + 4 * t + 2, index());
      st<u8>(a + 4 * t + 3, g.chance(0.9f) ? 0 : (u8)g.range(0, quiet ? 63 : 255));
    }
  };
  // edges: 2 vertex indices, 2 triangle indices (tri-1 255: an open edge)
  auto fill_edges = [&](u32 a, int count, int tris) {
    for (int e = 0; e < count; e++) {
      st<u8>(a + 4 * e, index());
      st<u8>(a + 4 * e + 1, index());
      auto tri = [&]() -> u8 {
        if (tris == 0 || g.chance(0.01f)) {
          return (u8)g.range(0, 254);
        }
        return (u8)g.range(0, std::min(tris - 1, 254));
      };
      st<u8>(a + 4 * e + 2, tri());
      st<u8>(a + 4 * e + 3, g.chance(0.2f) ? 255 : tri());
    }
  };
  fill_tris(hdr + ofs[TRIS1], n.tris1);
  fill_edges(hdr + ofs[EDGES1], n.edges1, n.tris1);
  fill_tris(hdr + ofs[TRIS2], n.tris2);
  fill_edges(hdr + ofs[EDGES2], n.edges2, n.tris2);
  *qwc_out = qwc;
  *mtx_out = mtx;
  return hdr;
}

GeoCounts rand_counts(Gen& g) {
  GeoCounts n;
  switch (g.range(0, 9)) {
    case 0:  // nothing
      break;
    case 1:  // a few of everything (one at a time loops, remainders)
      n.verts = g.range(0, 9);
      n.tris1 = g.range(0, 6);
      n.edges1 = g.range(0, 6);
      n.tris2 = g.range(0, 6);
      n.edges2 = g.range(0, 6);
      break;
    case 2:  // big: as many vertices as the u8 indices reach, VU1's limits for the lists
      n.verts = g.range(170, 256);
      n.tris1 = g.range(300, 500);
      n.edges1 = g.range(300, 600);
      n.tris2 = g.range(0, 200);
      n.edges2 = g.range(0, 300);
      break;
    default:  // like a character's
      n.verts = g.range(20, 160);
      n.tris1 = g.range(0, 300);
      n.edges1 = g.range(0, 400);
      n.tris2 = g.chance(0.5f) ? 0 : g.range(0, 60);
      n.edges2 = n.tris2 ? g.range(0, 90) : 0;
      break;
  }
  n.twos = n.verts ? g.range(0, n.verts) : 0;
  if (g.chance(0.2f)) {
    n.twos = g.chance(0.5f) ? 0 : n.verts;
  }
  // (one joint: every vertex reads its matrix past the copied ones, garbage: not when quiet)
  n.joints = g.range(default_nan_mode() ? 1 : 2, 64);
  if (n.verts == 0 && !default_nan_mode()) {
    // (quiet: no vertex for the triangles' indices)
    n.tris1 = n.edges1 = n.tris2 = n.edges2 = 0;
  }
  return n;
}

// (shadow-execute packet dma-ptr)
void gen_shadow(Case& c) {
  auto& g = c.g;
  Scene sc;
  sc.crafted = g.chance(0.1f);
  sc.d = sc.crafted ? 1024.f : (g.chance(0.7f) ? 1024.f : g.f(1.f, 4096.f));
  if (!sc.crafted) {
    sc.obj[0] = g.f(-20000.f, 20000.f);
    sc.obj[1] = g.f(-10000.f, 10000.f);
    // in front of the camera, sometimes at the near clip distance
    sc.obj[2] = g.chance(0.3f) ? g.f(-3000.f, 6000.f) : g.f(4000.f, 60000.f);
  }

  // *gsf-buffer* (the shadow-dcache): garbage, it is all scratch
  const u32 gsf = alloc(kGsfSize, 64);
  for (u32 i = 0; i < kGsfSize; i += 4) {
    st<u32>(gsf + i, g.u32_());
  }
  set_sym("*gsf-buffer*", gsf);

  // *math-camera*: d at 0, camera-rot at 364 (a basic, so 16-byte aligned there)
  const u32 cam = alloc_basic(0, 432);
  for (u32 i = 0; i < 428; i += 4) {
    st<u32>(cam + i, g.u32_());
  }
  st<float>(cam, sc.d);
  {
    float r[3][3];
    rand_rotation(g, r, sc.crafted || g.chance(0.3f));
    const bool moved = !sc.crafted && g.chance(0.5f);
    st_matrix(cam + 364, r, moved ? g.f(-5000.f, 5000.f) : 0.f, moved ? g.f(-5000.f, 5000.f) : 0.f,
              moved ? g.f(-5000.f, 5000.f) : 0.f);
  }
  set_sym("*math-camera*", cam);

  // *shadow-data*: dma-unpack-template (cnt, flusha, unpack-v4-32), dma-cnt, vif-unpack-v4-8
  const u32 data = alloc(112, 16);
  for (u32 i = 0; i < 112; i += 4) {
    st<u32>(data + i, g.u32_());
  }
  if (g.chance(0.9f)) {
    st<u64>(data + 64, 0x10000000);
    st<u32>(data + 72, 0x13000000);
    st<u32>(data + 76, 0x6c000000);
    st<u64>(data + 80, 0x10000000);
    st<u32>(data + 88, 0);
    st<u32>(data + 92, 0x6e000000);
  }
  set_sym("*shadow-data*", data);

  // camera-pos's vector
  const u32 campos = alloc(16, 16);
  st_vec(campos, g.f_edge(-30000.f, 30000.f), g.f_edge(-30000.f, 30000.f),
         g.f_edge(-30000.f, 30000.f), 1.f);
  set_sym("*shadow-test-camera*", campos);

  // the packets (144 bytes each, in the DMA buffer)
  const int packets = g.chance(0.05f) ? 0 : (g.chance(0.5f) ? 1 : g.range(2, 6));
  u32 first = 0, prev = 0;
  for (int p = 0; p < packets; p++) {
    const GeoCounts counts = rand_counts(g);
    u32 qwc, mtx;
    const u32 geo = make_geo(g, sc, counts, &qwc, &mtx);
    const u32 pk = alloc(144, 16);
    // tag: cnt qwc 5, vif1 the next packet
    st<u64>(pk, 0x10000005);
    st<u32>(pk + 8, 0);
    st<u32>(pk + 12, 0);
    if (prev) {
      st<u32>(prev + 12, pk);
    } else {
      first = pk;
    }
    prev = pk;
    // shadow-settings: center (flags in w), shadow-dir (w dist-to-locus), bot-plane, top-plane,
    // fade
    const u32 set = pk + 16;
    const u32 flags = (u32)g.range(0, 63) | (g.chance(0.1f) ? (g.u32_() & ~63u) : 0);
    float center[3];
    for (int i = 0; i < 3; i++) {
      center[i] = sc.crafted ? 0.f : sc.obj[i] + g.f_edge(-1000.f, 1000.f);
    }
    st<float>(set, center[0]);
    st<float>(set + 4, center[1]);
    st<float>(set + 8, center[2]);
    st<u32>(set + 12, flags);
    float dir[3] = {g.f(-0.5f, 0.5f), g.f(-1.f, -0.6f), g.f(-0.5f, 0.5f)};
    float dist = g.chance(0.7f) ? 409600.f : g.f_edge(-100000.f, 500000.f);
    if (sc.crafted) {
      // the locus below the plane z = 2d (so the plane isn't moved to it)
      dir[0] = 0.f;
      dir[1] = 0.f;
      dir[2] = -1.f;
      dist = g.f(0.f, 40960.f);
    } else if (g.chance(0.2f)) {
      // up: the locus above the plane
      dir[1] = -dir[1];
    }
    st_vec(set + 16, dir[0], dir[1], dir[2], dist);
    if (sc.crafted) {
      // flag 4: the planes as they are, the bottom one z = 2d
      st<u32>(set + 12, (flags & ~0x25u) | 4);
      st_vec(set + 32, 0.f, 0.f, 1.f, -2.f * sc.d);
      st_vec(set + 48, 0.f, 0.f, 1.f, -10.f * sc.d);
    } else {
      float n[3] = {g.f(-0.2f, 0.2f), 1.f, g.f(-0.2f, 0.2f)};
      if (g.chance(0.1f)) {
        n[0] = g.f(-1.f, 1.f);
        n[1] = g.f(-1.f, 1.f);
        n[2] = g.f(-1.f, 1.f);
      }
      st_vec(set + 32, n[0], n[1], n[2], g.f_edge(-20000.f, 40000.f));
      st_vec(set + 48, n[0], n[1], n[2], g.f_edge(-20000.f, 40000.f));
    }
    st_vec(set + 64, g.f(0.f, 409600.f), g.f(0.f, 409600.f), 0.f, 0.f);
    // the geo and the matrices by reference, then end
    st<u64>(pk + 96, (u64)qwc | (3ull << 28) | ((u64)geo << 32));
    st<u64>(pk + 104, 0);
    st<u64>(pk + 112, (u64)(8 * counts.joints) | (3ull << 28) | ((u64)mtx << 32));
    st<u64>(pk + 120, 0);
    st<u64>(pk + 128, 7ull << 28);
    st<u64>(pk + 136, 0);
  }
  c.args[0] = first;
  c.args[1] = alloc(0x28000, 16);
  for (int i = 2; i < 8; i++) {
    c.args[i] = g.u32_();
  }
}

}  // namespace

void register_shadow_tests() {
  add_test({"shadow-execute", "shadow-execute", 1500, setup_shadow, gen_shadow, {}});
}

}  // namespace tests
