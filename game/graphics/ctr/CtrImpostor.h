#pragma once

/*!
 * @file CtrImpostor.h
 * (AI-assisted)
 * Impostors for far level objects (config.ini impostors, CtrSettings): a tie visibility group (one
 * to eight instances the converter keeps together: trees, rocks, houses) far from the camera is
 * drawn into a tile of a texture at screen resolution with the camera of that frame, and shown as
 * a card in its place in the frames after, until the card's picture could be off by more than
 * impostor_error pixels. Then it is drawn into its tile again.
 *
 *  - The capture is the real draw: the same vertices, textures, states and projection, cropped to
 *    the group's rectangle on screen (so the card matches the geometry to the pixel in that
 *    frame), without fog.
 *  - The card's depth (Oct 17, the beach fix): the CPU rasterizes the group's triangles at the
 *    capture into a small buffer of their nearest 1/w per texel (the visible surface; 2x2 texels
 *    per sample for tiles over 64 pixels). The tile is cut into cells (about 12 pixels, at most
 *    4x4); each cell with something in it becomes one quad on its own plane: the plane fitted to
 *    the cell's visible depths (least squares in screen x, y and 1/w, where a flat surface is a
 *    plane), moved towards the camera until no visible texel (and no vertex) of the cell is in
 *    front of it. A cell whose plane is still more than ~0.6% of its distance in front of some
 *    texel (a step in depth: a near rock's edge over a far one) is split in four, while the card
 *    stays under a quarter of the group's triangles. Empty cells are left out. So the card lies
 *    on the group's visible surface to within that slack: what stands between the group's parts
 *    (the arch among the beach rocks, a tree in front of a cliff) is no longer hidden by the
 *    nearest part of a cell (the old 4x4 grid took each point's depth from the nearest triangle
 *    whose screen box touched any of the four cells around it). The quads overlap by half a texel
 *    inside the tile (no cracks when the camera moves).
 *    Drawn like the level mesh (same vertex program): the fog of each corner's depth.
 *  - The error: how far the camera moved since the capture times the parallax between the cards'
 *    planes and the visible texels behind them: camera rotation alone never makes a card wrong.
 *    Also a new capture when the card's size on screen is off by more than 15% (texels per pixel),
 *    when the time of day moved the group's colors by more than kTodMaxErr (vertex color units:
 *    the weight changes of the palettes its vertices use times those palettes' brightest colors),
 *    or the group comes nearer than impostor_distance (then it's drawn as geometry again: within
 *    the error, no pop). Groups with a "light" palette (lamps, lava glow: moved every frame) stay
 *    geometry.
 *  - Nearest texel sampling at 1:1 and the alpha test (alpha > 0) give the geometry's hard edges.
 * Only groups whose draws are all opaque (no blending, depth written, no decal), with no blended
 * group in front of part of them, and whose rectangle is at most 128x128 pixels. The texture
 * (impostor_atlas KB, RGBA8; VRAM when there is room at start, else linear memory) is cut into
 * tiles (16 x 8 pixel cells, first fit); the least recently shown tiles make room, and groups with
 * more triangles per texel take the place of those with fewer.
 */

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "common/math/Vector.h"

#include "game/graphics/ctr/ctr_gpu.h"

struct CtrLevelData;

// (AI-assisted, lavamem) A growing array in pages of 2^kShift elements: no block bigger than a page
// and nothing copied when it grows (a vector doubles: the impostors' cluster table of lava tube
// wanted one block of 557 KB, plus the old one while copying, of a fragmented heap: the hardware
// abort of Oct 25).
template <typename T, u32 kShift>
class CtrPaged {
 public:
  T& operator[](size_t i) { return m_pages[i >> kShift][i & kMask]; }
  const T& operator[](size_t i) const { return m_pages[i >> kShift][i & kMask]; }
  size_t size() const { return m_size; }
  void push_back(const T& v) {
    if ((m_size >> kShift) == m_pages.size()) {
      m_pages.emplace_back(new T[kMask + 1]);  // (may throw: nothing changed then)
    }
    (*this)[m_size++] = v;
  }

 private:
  static constexpr size_t kMask = ((size_t)1 << kShift) - 1;
  std::vector<std::unique_ptr<T[]>> m_pages;
  size_t m_size = 0;
};

class CtrImpostors {
 public:
  struct Camera {
    std::array<math::Vector4f, 4> R;  // make_cam_mat (CtrLevel.cpp)
    math::Vector4f trans;             // camera position, game units
    float y_scale = 1.f;              // the chunk matrices' y factor (Jak 1 scissor adjust)
    const float* fog0 = nullptr;      // the level's mesh fog (ctr_gpu_set_mesh_fog)
    const float* fog1 = nullptr;
    u8 fog_rgb[3] = {0, 0, 0};
  };
  /*! CtrTfragRenderer::draw_level, for each level drawn this frame: */
  void begin_level(CtrLevelData& lev, const Camera& cam, u64 frame, bool allowed = true);
  /*! A visible chunk (after the view and visibility tests), its clip matrix (on its quantized
   * positions). run_shown: the per run flags the level draw uses (valid for this chunk's runs only
   * if runs_valid; else all of them are shown); partly: set when runs of it are hidden. */
  void chunk(u32 ci, const float clip[16], bool runs_valid, u8* run_shown, bool* partly);
  /*! After the chunks: picks the captures, draws them, hides the runs the cards stand in for. */
  void finish_chunks();
  /*! The cards (the level's fog set), before the level's blended draws. */
  void draw_cards();
  /*! A log line about the last stats period (render thread), then the counts start again. */
  void log_stats();
  bool active() const { return m_active; }
  // (AI-assisted, title3) host diagnostic CTR_POP_LOG: this stats period's cards drawn, captures
  double stat_cards() const { return m_st.cards; }
  double stat_captures() const { return m_st.captures; }
  /*! (renderer init, impostors on) the texture now, before the level texture pools fill VRAM */
  void reserve() { ensure_atlas(); }

  static constexpr int kGrid = 4;  // cells per side of a card at most (before splits)

 private:
  struct Cluster {
    u32 chunk = 0;
    u32 first_run = 0;  // into LevelState::runs: (run, first index) pairs
    u16 run_count = 0;
    u16 vis = 0;
    float bmin[3] = {0, 0, 0}, bmax[3] = {0, 0, 0};  // world, game units
    float cen[3] = {0, 0, 0}, rad = 0;               // the box's center and half diagonal
    u32 tris = 0;
    int slot = -1;
    bool eligible = false;
    bool exact = false;  // bmin / bmax from its vertices (else around its runs' spheres)
    bool light = false;  // (exact) a vertex has a light palette's color: stays geometry
    bool has_near = false;  // a tie LOD group: its runs include a near version
    u8 pal = 0;            // (exact) the time of day palettes its vertices use
    u8 tmax[8][3] = {};    // (exact) per palette and channel: its vertices' brightest color
    u8 tmin[8][3] = {};    // ... and darkest
    float kpar = -1.f;     // its last capture's parallax bound (-1: none yet)
    u64 tod_hold = 0;      // no capture before this frame (the day ran too fast for its card)
    u64 retry = 0;         // no new tile before this frame (the atlas was full for it)
  };
  struct ChunkInfo {
    int mesh = -2;  // the mesh the clusters were made from (-2: not looked at yet)
    u32 first = 0, count = 0;
  };
  struct LevelState {
    const CtrLevelData* lev = nullptr;
    u32 generation = 0;
    std::vector<ChunkInfo> chunks;
    CtrPaged<Cluster, 7> clusters;  // (AI-assisted, lavamem) pages of 128 (17 KB)
    CtrPaged<u32, 12> runs;  // the clusters' runs (CtrLevelData::vis_runs) and first indices
    u64 last_frame = 0;
  };
  struct Slot {
    bool used = false;
    int level = -1;  // in m_levels
    u32 cluster = 0;
    u16 tx = 0, ty = 0;  // the tile in the atlas (logical pixels)
    u16 cw = 0, ch = 0;  // its size
    u16 wn = 0, hn = 0;  // the part the card shows (screen pixels at the capture)
    double cam[3] = {0, 0, 0};  // capture camera
    // the card: quads (4 points each: x0y0 x1y0 x0y1 x1y1), relative to cam, and their texture
    // coordinates * 1024 (c3l::Vertex)
    std::vector<std::array<float, 3>> pv;
    std::vector<std::array<s16, 2>> pst;
    // the same as the cards' vertices (c3l::Vertex), quantized around m_qorigin (generation qgen)
    std::vector<std::array<u32, 4>> qv;
    int qgen = -1;
    float corner[3][3] = {};  // the tile's corners x0y0, x1y0, x0y1 at the group's depth (size)
    float value = 0;  // triangles per texel
    float kpar = 0;   // parallax bound: pixels per game unit the camera moved since the capture
    u8 tod_w[8][3] = {};  // the time of day weights at the capture
    bool tod = false;     // tod_w valid
    bool near = false;    // the tie LOD near version was drawn into it
    u32 tod_ver = 0;      // the level's time of day version it was last checked against
    u32 checks = 0;       // its validity checks (the size one every 8th)
    u64 captured = 0, last_used = 0;
  };
  // a shown group of this level draw that isn't opaque (no card may hide part of it)
  struct Seen {
    u32 cluster;
  };
  // a rectangle on screen (pixels, y from the bottom) and a clip w range
  struct Rect {
    float x0, x1, y0, y1, w0, w1;
  };
  // a visible chunk with clusters this frame
  struct ChunkRec {
    u32 ci;
    float clip[16];
    u8* run_shown;
    bool* partly;
  };
  // a cluster that wants a card this frame
  struct Want {
    u32 cluster;
    u32 rec;       // in m_recs
    bool capture;  // needs a (new) capture; then (finish_chunks):
    int px0 = 0, py0 = 0, wn = 0, hn = 0;  // its rectangle now (screen pixels; y from the bottom)
    float zmin = 0, zmax = 0;              // its corners' depth range (ndc)
    float wmin = 0, wmax = 0;              // ... clip w
    float value = 0;
    float tod_rate = 0;  // (a new capture for the time of day) its colors' move per frame
    bool near = false;   // the level draw shows the near version of its tie LOD group
  };
  struct Proj {
    float xmin, xmax, ymin, ymax, zmin, zmax;  // screen pixels (y from the bottom), ndc z
    float wmin, wmax;                          // clip w
    bool front;                                // every corner in front of the camera
  };

  bool ensure_atlas();
  // (AI-assisted, lavamem) the bodies of begin_level / chunk / finish_chunks / draw_cards; those
  // catch std::bad_alloc from them (out_of_memory)
  void begin_level_impl(CtrLevelData& lev, const Camera& cam, u64 frame, bool allowed);
  void chunk_impl(u32 ci, const float clip[16], bool runs_valid, u8* run_shown, bool* partly);
  void finish_chunks_impl();
  void draw_cards_impl();
  void out_of_memory(const char* where);
  void build_chunk(LevelState& ls, u32 ci);
  bool exact_box(LevelState& ls, Cluster& c);
  void project_box(const float bmin[3], const float bmax[3], Proj* out) const;
  bool sphere_rect(const Cluster& c, Rect* out) const;
  void drop_level(int li);
  void free_slot(int s);
  int alloc_slot(int wn, int hn, float value, u64 frame);
  bool capture(const Want& w, Slot& s);
  float card_scale(const Slot& s) const;
  int tod_error(LevelState& ls, const Cluster& c, const Slot& s);
  bool tod_behind(const LevelState& ls, const Cluster& c) const;
  bool quantize(Slot& s);
  void hide(const Want& w);

  bool m_active = false;  // in a level draw with impostors
  bool m_rt_open = false;  // finish_chunks: captures into the atlas under way
  u64 m_oom_until = 0;     // (AI-assisted, lavamem) out of heap: no impostors before this frame
  u32 m_oom_count = 0;
  bool m_failed = false;  // the atlas couldn't be made: off
  int m_atlas = -1;
  int m_lw = 0, m_lh = 0;
  std::vector<u32> m_cols;  // per 16 pixel column of the atlas: its 8 pixel rows taken
  int m_texels = 0;         // in tiles
  std::vector<Slot> m_slots;
  std::vector<LevelState> m_levels;
  int m_cur = -1;  // the level being drawn (m_levels)
  Camera m_cam;
  double m_A[4][3] = {}, m_B[4] = {};   // world -> clip of this frame
  float m_Af[4][3] = {}, m_Kf[4] = {};  // the same on (P - camera)
  float m_rowlen[4] = {};               // |m_Af rows|
  float m_cam_key[19] = {};             // the last level draw's camera (m_cam_same)
  bool m_cam_same = false;
  // the camera's speed (game units per frame, smoothed) and its last position: a capture is only
  // worth it when the card will stay right for a few frames
  float m_speed = 0.f;
  float m_last_trans[3] = {0, 0, 0};
  bool m_have_last = false;
  u64 m_frame = 0;
  int m_builds = 0;      // chunks whose clusters were made this frame
  u32 m_exact_tris = 0;  // exact boxes made this frame (their triangles)
  std::vector<ChunkRec> m_recs;
  std::vector<Want> m_wants;
  std::vector<Seen> m_seen;
  std::vector<int> m_cards;  // slots shown this level draw
  std::vector<u64> m_keys;   // finish_chunks' scratch
  std::vector<u32> m_order;
  int m_card_verts = 0;      // their points
  // the cards' vertices are quantized once (not per frame) in kQStep units around m_qorigin, a
  // point near the camera (moved, and every card quantized again, when the camera gets far)
  double m_qorigin[3] = {0, 0, 0};
  int m_qgen = 0;
  bool m_qvalid = false;
  double m_cap_cpu_ms = 0;
  double m_exact_ms = 0;    // this frame's exact boxes' CPU time
  double m_t_chunks = 0;    // begin_level's end (the chunk calls' time)  // this frame's capture CPU work (raster, planes)
  // capture scratch: the group's vertices on the tile (x, y, 1/w), each once, in the order met,
  // and the visible 1/w per sample. (AI-assisted, lavamem) m_vtag, per vertex index of a mesh
  // (65536, 256 KB, made once): the pass (m_stamp) it was last met in << 16 | its index in m_pj
  std::vector<std::array<float, 3>> m_pj;
  std::vector<u32> m_vtag;
  u16 m_stamp = 0;
  bool new_pass();  // a new m_stamp (false: no memory for m_vtag)
  std::vector<float> m_zb;
  std::vector<u32> m_used_verts;
  // statistics (per stats period)
  struct Stats {
    double frames = 0, level_draws = 0, cards = 0, card_tris = 0, captures = 0, capture_tris = 0,
           over_budget = 0, no_room = 0, too_big = 0, recapture_error = 0, recapture_scale = 0,
           evicted = 0, built_chunks = 0, build_ms = 0, cpu_ms = 0, recapture_tod = 0,
           card_quads = 0, drawn_quads = 0, short_life = 0, recapture_lod = 0, splits = 0, slack_cells = 0, verts_full = 0;
    // triangles of the groups in chunks that reach beyond impostor_distance, by what became of
    // them
    double t_opaque_no = 0, t_hidden = 0, t_near = 0, t_big = 0, t_off = 0, t_budget = 0,
           t_room = 0, t_blended = 0, t_pending = 0, t_light = 0, t_short = 0;
    double ms_chunk = 0, ms_finish = 0, ms_capture = 0, ms_cards = 0, ms_cap_cpu = 0, ms_card_draw = 0, ms_f_sort = 0, ms_f_loop = 0, ms_exact = 0;
  } m_st;
};

CtrImpostors& ctr_impostors();
