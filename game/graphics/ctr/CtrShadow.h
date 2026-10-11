#pragma once

/*!
 * @file CtrShadow.h
 * (AI-assisted)
 * Shadow volumes (bucket shadow, config.ini shadows = full; needs ctr_gpu_has_stencil()).
 *
 * The game (shadow-execute-all, shadow-cpu.gc) builds the PS2's shadow DMA: per object a table of
 * camera space vertices (the object's shadow mesh, and the same vertices moved along the light
 * onto the bottom plane) and index lists for the VU1 shadow program (shadow-vu1.gc; the PC
 * renderer emulates it in Shadow_PS2.cpp), which projects the volume's triangles, clips them and
 * draws them as front or back faces (PS2: into the alpha channel, PC: the stencil buffer).
 *
 * CtrShadowVolumes is that program as native code: each object's vertices are transformed once to
 * OpenGL-style clip space (ctr_clip_vertex), and every triangle the program would draw before its
 * own clipping goes to a front or a back index list, with the program's own facing test. The GPU
 * clips. CtrShadowRenderer then draws all objects at once (ctr_gpu_clip_draw_ranges):
 *  1. the front list: stencil + 1 where a face passes the depth test, no color or depth writes
 *  2. the back list: stencil - 1 likewise (z-pass, saturating like the PC's GL_INCR / GL_DECR)
 *  3. a rectangle over each object's volume on screen: where the stencil isn't 0, the color times
 *     the game's shadow color (as the PC: Cd * 2 * rgb / 255, alpha kept), and the stencil back to
 *     0 (the ocean's marks after this bucket expect 0 everywhere)
 *
 * The VU1 data memory the program reads (quadwords, the unpacks of shadow-add-verts and
 * shadow-add-facing-single-tris / -single-edges / -double-tris / -double-edges, shadow.cpp):
 *   0..3      the perspective matrix (camera space -> PS2 clip space, shadow-vu1-add-matrix)
 *   4..173    T[i], the object's vertices in camera space (x, y, z; w unused)  (unpack v4-32 to 4)
 *   174..343  D[i], the same vertices extruded onto the bottom plane           (unpack v4-32 to 174)
 *   344       the triangle count, then (i0, i1, i2, faces) per triangle        (u8, for MSCAL 2, 6)
 *   600       the edge count, then (a, b, flip, 0) per edge                    (u8, for MSCAL 4)
 *   880..892  hmgescale, invhscale, texoffset, texscale, hvdfoff, fog (pfog0 = fog.x), the colors
 *             of back (886) and front (887) faces, GIF tags (shadow-vu1-add-constants)
 * What each entry point draws (before its clipping), in its order:
 *   2  facing single tris: (T[i0], T[i1], T[i2]) for every triangle, then (D[i0], D[i2], D[i1])
 *   4  silhouette edges (single and double): the side quad (T[a], T[b], D[b], D[a]) if flip != 0,
 *      else (T[b], T[a], D[a], D[b]), as the fan (q0 q1 q2), (q0 q2 q3) with one color
 *   6  double-sided tris: faces != 0: (T[i0], T[i1], T[i2]), then (D[i0], D[i2], D[i1]);
 *      faces == 0: (T[i0], T[i2], T[i1]), then (D[i0], D[i1], D[i2])
 * Facing: e1 = v1 - v0, e2 = v2 - v0 in camera space; front (color 887) when (e1 x e2) . v0 < 0,
 * else back (886).
 */

#include <vector>

#include "common/common_types.h"
#include "common/dma/dma_chain_read.h"

#include "game/graphics/ctr/CtrRenderer.h"
#include "game/graphics/ctr/ctr_gpu.h"

/*!
 * The VU1 shadow program's triangles, native: the shadow bucket's DMA to indexed clip space
 * geometry. No GPU calls (the host test runs it against the emulated program).
 */
class CtrShadowVolumes {
 public:
  // budgets per frame (all objects, ~16 the size of Jak's: 236 vertices, ~400 triangles): the GPU
  // vertex memory taken for a moment is kMaxVerts * 28 + kMaxIndices * 2 bytes (~160 KB, before
  // the sprites fill the buffer); what's used stays (Jak: ~9 KB)
  static constexpr int kMaxVerts = 4096;
  static constexpr int kMaxIndices = 3 * 8192;
  // screen rectangles of the darkening pass, one per object (more objects: the last one grows)
  static constexpr int kMaxRects = 16;

  struct Rect {
    float x0, y0, x1, y1;  // ctr_gpu coordinates: x, y in [-1, 1], y up
  };
  // (the host test) one triangle (n = 3) or side quad (n = 4: triangles 0 1 2 and 0 2 3) the VU1
  // program would draw, in its order: output vertex indices and the program's facing
  struct Prim {
    u16 v[4];
    u8 n;
    u8 front;
  };
  struct Stats {
    int objects = 0;          // objects with triangles
    int dropped_objects = 0;  // left out: the frame's budget was full, bad numbers
    int bad_entries = 0;      // triangles / edges with an index outside the vertex tables
    int bad_packets = 0;      // index lists that didn't fit their transfer, unknown entry points
    int no_camera = 0;        // index lists before the constants and matrix
    int capped = 0;           // (AI-assisted) left out by shadow_casters (smallest / furthest)
  };

  //! A new frame. The output: vertices (room for vert_cap, at most 65536), the front list (in
  //! place) and the back list, room for index_cap indices together. verts / front may come later
  //! (set_output, when the first object arrives), back must be there now.
  void begin(ctr_clip_vertex* verts, int vert_cap, u16* front, u16* back, int index_cap);
  void set_output(ctr_clip_vertex* verts, u16* front);
  //! The transfer starts an object (its vertex table): the output is needed from here on.
  static bool starts_object(const DmaTransfer& t);
  //! (AI-assisted) config shadow_casters: how big the object whose vertex table starts with this
  //! transfer looks from the camera (its vertices' bounding sphere radius over the distance of its
  //! nearest side); 0 for bad numbers.
  static float object_score(const DmaTransfer& t);
  //! (AI-assisted) after begin: only the objects k with keep[k] != 0 (in DMA order; objects past
  //! n are kept) are drawn; nullptr: all
  void set_keep(const u8* keep, int n) {
    m_keep = keep;
    m_keep_n = n;
  }
  //! One DMA transfer of the shadow bucket: the constants, matrix, vertex tables, index lists with
  //! their MSCAL, and the end packets with the shadow color; anything else is ignored.
  void transfer(const DmaTransfer& t);
  //! After the last transfer: the rectangles are final.
  void end();

  int vert_count() const { return m_nv; }
  int front_count() const { return m_nf; }
  int back_count() const { return m_nb; }
  //! the darkening rectangles: one full screen one when a volume reaches behind the camera
  int rect_count() const { return m_full_screen ? 1 : m_nrects; }
  Rect rect(int i) const;
  bool full_screen() const { return m_full_screen; }
  //! the darkening color for the vertex color of the rectangles (untextured: 0xff = 1.0): twice
  //! the game's shadow color (like the PC renderer's shadow.frag), saturated
  void color(u8 out[3]) const;
  const Stats& stats() const { return m_stats; }
  //! (the host test) record the primitives in the program's order, or nullptr
  void set_trace(std::vector<Prim>* trace) { m_trace = trace; }
  //! camera space -> clip space (row-major), from the last init (the host test)
  const float* clip_matrix() const { return m_clip; }

 private:
  bool ensure_transformed();
  void close_object();
  void drop_object();
  template <bool kTrace>
  void tris(const u8* entries, int count, bool double_sided);
  template <bool kTrace>
  void edges(const u8* entries, int count);
  void make_clip_matrix();

  // the program's constants (VU 0x370..0x37c) and matrix (VU 0..3) from the init packets
  float m_const[13][4] = {};
  float m_matrix[4][4] = {};
  bool m_have_const = false, m_have_matrix = false, m_have_clip = false;
  float m_clip[16] = {};

  // the current object: its vertex tables (in the DMA data, valid for the frame)
  const float* m_top = nullptr;
  const float* m_dual = nullptr;
  int m_ntop = 0, m_ndual = 0;
  bool m_obj_open = false;         // an object's vertices arrived
  bool m_obj_transformed = false;  // its vertices are in the output
  bool m_obj_dropped = false;      // left out (budget, bad numbers)
  bool m_obj_any = false;          // it has triangles
  int m_obj_base = 0;              // output index of T[0] (D[0]: m_obj_base + m_ntop)
  int m_obj_f0 = 0, m_obj_b0 = 0;  // the lists' lengths before it (to take it back out)
  Rect m_obj_rect{};
  bool m_obj_full = false;

  // output
  ctr_clip_vertex* m_out = nullptr;
  u16* m_front = nullptr;
  u16* m_back = nullptr;
  int m_vcap = 0, m_icap = 0;
  int m_nv = 0, m_nf = 0, m_nb = 0;
  Rect m_rects[kMaxRects]{};
  int m_nrects = 0;
  bool m_full_screen = false;
  u8 m_rgb[3] = {0x40, 0x40, 0x40};  // the PS2's default shadow color (shadow-dma-end)

  Stats m_stats;
  std::vector<Prim>* m_trace = nullptr;
  const u8* m_keep = nullptr;
  int m_keep_n = 0;
  int m_obj_index = 0;  // the objects started this frame
};

/*! The shadow bucket: the volumes of all objects in three draws (see the top of this file). */
class CtrShadowRenderer : public CtrBucketRenderer {
 public:
  CtrShadowRenderer(std::string name, int id);
  void render(DmaFollower& dma, CtrRenderState& rs) override;

 private:
  void draw();
  CtrShadowVolumes m_vol;
  std::vector<u16> m_back;  // the back faces' list, copied after the front faces' to draw
  ctr_clip_vertex* m_verts = nullptr;
  u16* m_indices = nullptr;
  bool m_reserved = false;
  int m_no_room = 0;  // frames whose GPU vertex memory was full (logged at most every 300)
  // (AI-assisted) shadow_casters: each object's score, which ones are drawn
  std::vector<float> m_scores;
  std::vector<u8> m_keep;
  std::vector<int> m_order;
};
