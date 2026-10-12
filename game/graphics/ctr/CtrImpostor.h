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
 *  - The card is a 4x4 grid over the rectangle: each grid point on the capture camera's ray
 *    through it, at the depth of the nearest of the group's triangles in the cells around it (the
 *    CPU bins the group's triangles into the cells at the capture). So the card lies just in front
 *    of the group everywhere: its depth test against the rest of the scene is the group's own,
 *    to within the depth of one cell's contents (a flat card in front of the whole group would
 *    hide whatever stands among the group's objects, an arch among rocks). Drawn like the level
 *    mesh (same vertex program): the fog of each grid point's depth, with each frame's camera.
 *  - The error: how far the camera moved since the capture times the parallax between the grid
 *    and the farthest triangles of each cell (the grid's depth against the geometry's): camera
 *    rotation alone never makes a card wrong. Also a new capture when the card's size on screen is
 *    off by more than 15% (texels per pixel), or the group comes nearer than impostor_distance
 *    (then it's drawn as geometry again: within the error, no pop).
 *  - Nearest texel sampling at 1:1 and the alpha test (alpha > 0) give the geometry's hard edges.
 *  - Level geometry has its time of day colors baked in the .c3l (no lighting changes at run time),
 *    so a card's colors stay right.
 * Only groups whose draws are all opaque (no blending, depth written, no decal), with no blended
 * group in front of part of them, and whose rectangle is at most 128x128 pixels. The texture
 * (impostor_atlas KB, RGBA8, linear memory) is cut into tiles (16 x 8 pixel cells, first fit); the
 * least recently shown tiles make room, and groups with more triangles per texel take the place of
 * those with fewer.
 */

#include <array>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "common/math/Vector.h"

#include "game/graphics/ctr/ctr_gpu.h"

struct CtrLevelData;

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
  void begin_level(CtrLevelData& lev, const Camera& cam, u64 frame);
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

  static constexpr int kGrid = 4;                           // cells per side of a card
  static constexpr int kGridV = (kGrid + 1) * (kGrid + 1);  // its points

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
  };
  struct ChunkInfo {
    int mesh = -2;  // the mesh the clusters were made from (-2: not looked at yet)
    u32 first = 0, count = 0;
  };
  struct LevelState {
    const CtrLevelData* lev = nullptr;
    u32 generation = 0;
    std::vector<ChunkInfo> chunks;
    std::vector<Cluster> clusters;
    std::vector<u32> runs;  // the clusters' runs (CtrLevelData::vis_runs) and first indices
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
    float gv[kGridV][3] = {};   // the card's points, relative to cam
    s16 gst[kGridV][2] = {};    // their texture coordinates * 1024 (c3l::Vertex)
    float value = 0;  // triangles per texel
    float kpar = 0;   // parallax bound: pixels per game unit the camera moved since the capture
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
  };
  struct Proj {
    float xmin, xmax, ymin, ymax, zmin, zmax;  // screen pixels (y from the bottom), ndc z
    float wmin, wmax;                          // clip w
    bool front;                                // every corner in front of the camera
  };

  bool ensure_atlas();
  void build_chunk(LevelState& ls, u32 ci);
  bool exact_box(LevelState& ls, Cluster& c);
  void project_box(const float bmin[3], const float bmax[3], Proj* out) const;
  bool sphere_rect(const Cluster& c, Rect* out) const;
  void drop_level(int li);
  void free_slot(int s);
  int alloc_slot(int wn, int hn, float value, u64 frame);
  bool capture(const Want& w, Slot& s);
  float card_scale(const Slot& s) const;
  void hide(const Want& w);

  bool m_active = false;  // in a level draw with impostors
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
  u64 m_frame = 0;
  int m_builds = 0;      // chunks whose clusters were made this frame
  u32 m_exact_tris = 0;  // exact boxes made this frame (their triangles)
  std::vector<ChunkRec> m_recs;
  std::vector<Want> m_wants;
  std::vector<Seen> m_seen;
  std::vector<int> m_cards;  // slots shown this level draw
  // statistics (per stats period)
  struct Stats {
    double frames = 0, level_draws = 0, cards = 0, card_tris = 0, captures = 0, capture_tris = 0,
           over_budget = 0, no_room = 0, too_big = 0, recapture_error = 0, recapture_scale = 0,
           evicted = 0, built_chunks = 0, build_ms = 0, cpu_ms = 0;
    // triangles of the groups in chunks that reach beyond impostor_distance, by what became of
    // them
    double t_opaque_no = 0, t_hidden = 0, t_near = 0, t_big = 0, t_off = 0, t_budget = 0,
           t_room = 0, t_blended = 0, t_pending = 0;
    double ms_chunk = 0, ms_finish = 0, ms_capture = 0, ms_cards = 0;
  } m_st;
};

CtrImpostors& ctr_impostors();
