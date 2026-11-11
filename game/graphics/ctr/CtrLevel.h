#pragma once

#include "game/graphics/ctr/CtrLight.h"

/*!
 * @file CtrLevel.h
 * (AI-assisted)
 * Level backgrounds for the 3DS renderer: loads .c3l files (docs/3ds-port/c3l_format.md) from
 * <project>/out/jak1/c3l and draws them from the tfrag buckets with the camera the game sends.
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "common/math/Vector.h"

#include "common/dma/gs.h"
#include "common/util/FileUtil.h"

#include "game/graphics/ctr/CtrRenderer.h"
#include "game/graphics/ctr/CtrKeiraRig.h"
#include "game/graphics/ctr/c3l_format.h"
#include "game/graphics/ctr/c3l_reader.h"
#include "game/graphics/ctr/ctr_gpu.h"

/*! (AI-assisted) c3l v12: an effect whose vertices the game rewrites (c3l::MercModEffect). */
struct CtrMercModEffect {
  u8 effect = 0;
  std::vector<u8> mask;  // per fragment: unpacked or skipped
  u32 vidx_end = 0;
  std::vector<c3l::MercModDest> dests;  // by model vertex
  float pos_k = 1.f;     // xyz_scale / model scale: unpacked position -> MercVertex::pos
  s16 st_lut[256] = {};  // lump byte -> MercVertex::st (* 1024)
};

// In view (draw_level counts, stream_log logs): a pop is a chunk or group that appears, goes or
// switches between two frames with no morph, fade or fog over it; a hole is a chunk the config
// would draw with nothing drawn in its place.
namespace ctr_pop {
enum Cause : u8 {
  kLine,   // a distance limit the fixed detail floor pulled in was crossed
  kLimit,  // a distance limit of the config was crossed
  kEvict,  // near data was freed
  kLate,   // a group back to its version once its near data came
  kLod,    // a version switch without a morph
  kFar,    // far mode switched, or its landmark reach was crossed
  kShrub,  // a shrub's fade jumped
  kVista,  // the vista distance
  kTier,   // a tfrag cell switched tier 1 / 2 (lod_distance)
  kHold,   // a group kept far in view by a hold
  kUnit,   // a group's near data beyond its near chunk's limit, or not read
  kOther,  // a mesh came or went, the dormant hold, the low resolution tier's rules
  kCauses
};
constexpr const char* kNames[kCauses] = {
    "floor limit", "config limit", "freed",     "late",       "lod switch", "far mode",
    "shrub fade",  "vista",        "tier switch", "held far", "unit limit", "mesh or rule"};
enum HoleKind : u8 { kHoleTfrag, kHoleTie, kHoleVista, kHoleKinds };
constexpr u32 kHoleFrames = 15;  // a hole this long is counted (0.5 s)
struct Stats {
  u32 pops[kCauses] = {};
  u32 hole_frames[kHoleKinds] = {};  // chunk frames
  u32 hole_mesh_frames = 0;          // of them: its mesh isn't there
  u32 copy_frames = 0;               // chunk frames drawn by a vista copy nearer than the config's
  u32 hole_draw_frames = 0;          // tfrag chunk frames beyond the draw distance, nothing in place
  u32 hole_groups = 0;               // group frames: kept far or coarse and that version not drawn
  u32 morph_in = 0, morph_out = 0;   // near chunks that came / went in view under a morph
  u32 far_held = 0;    // level draws a far mode switch waited: something it changes in view
  u32 far_forced = 0;  // far mode switches made at a hold's bound with something it changes in view
  u32 copy_held = 0;   // tie chunk frames in view kept off the vista copies that came
  u32 holes = 0, hole_longest = 0;   // holes of kHoleFrames or more that ended; frames
  u32 frames = 0;
};
}  // namespace ctr_pop

struct CtrMercModelData {
  std::string name;
  int mesh = -1;
  float scale = 1.f;
  std::vector<c3l::MercDraw> draws;
  // (AI-assisted) the converter's generated low LOD (models with only lod0): the same vertices,
  // fewer triangles; empty: none. CtrMercRenderer draws them far away (config merc_gen_lod)
  std::vector<c3l::MercDraw> gen_draws;
  // blend shapes (faces): CtrLevelData::blerc_verts[blerc_first, + blerc_count)
  u32 blerc_first = 0, blerc_count = 0;
  mutable bool blerc_moved = false;  // vertices not in the base pose (CtrMercRenderer)
  std::vector<CtrMercModEffect> mod;  // (AI-assisted) v12, see CtrMercRenderer::apply_mod
  mutable bool mod_checked = false;   // the first update was compared with the file's vertices
  // (AI-assisted) "keira-as-..." (CtrKeiraRig.h): its vertices use her rig's slots, which
  // CtrMercRenderer poses from Jak's bones
  bool keira_rig = false;
  // (AI-assisted) "keira-as-...~shadow": her shadow's hulls (CtrKeiraRig.h ShadowMesh)
  std::shared_ptr<ctr_keira::ShadowMeshData> keira_shadow;
  // (AI-assisted) where the mesh's data is in the file, to make it again after a load that ran out
  // of memory: first MercVertex and count, merc index range (index_end 0: not possible)
  u32 file_vertex = 0, vertex_count = 0, index_first = 0, index_end = 0;
  // models whose vertices are rewritten (blend shapes, mod): the mesh's indices, for the copies of
  // the instances drawn after the first in a frame (CtrMercRenderer)
  std::vector<u16> indices;
};

// The PS2's shine distance of Jak 1's envmapped ties (draw-inline-array-instance-tie): an instance
// is drawn with fade = trunc(clamp((reach - |camera - c|) * rfade, 0, 128)) / 128 of its shine,
// fade 0 as a plain tie. The instances by tie node (a run's vis id), from the game's tie trees
// (ctr_tie_shine).
struct CtrShineTable {
  struct Inst {
    float c[3];
    float reach;  // envmap-fade-far + the sphere's radius
    float rfade;
    u32 vis;
  };
  std::vector<Inst> insts;      // by vis
  u16 vis0 = 0;                 // node vis0 + k: insts [node_first[k], node_first[k + 1])
  std::vector<u32> node_first;
  static constexpr int kFull = 128;
};

// A level's large tables (the time of day colors): on the 3DS from the heap while it keeps its
// reserve after them, else from linear memory (the levels' budget, which has room when the heap
// has none); throws std::bad_alloc only when both are out
void* ctr_big_alloc(size_t bytes);
void ctr_big_free(void* p) noexcept;
template <typename T>
struct CtrBigAlloc {
  using value_type = T;
  CtrBigAlloc() noexcept = default;
  template <typename U>
  CtrBigAlloc(const CtrBigAlloc<U>&) noexcept {}
  T* allocate(size_t n) { return static_cast<T*>(ctr_big_alloc(n * sizeof(T))); }
  void deallocate(T* p, size_t) noexcept { ctr_big_free(p); }
};
template <typename T, typename U>
bool operator==(const CtrBigAlloc<T>&, const CtrBigAlloc<U>&) noexcept {
  return true;
}
template <typename T, typename U>
bool operator!=(const CtrBigAlloc<T>&, const CtrBigAlloc<U>&) noexcept {
  return false;
}
template <typename T>
using CtrBigVec = std::vector<T, CtrBigAlloc<T>>;

struct CtrLevelData {
  std::string name;
  std::vector<CtrMercModelData> merc_models;
  std::vector<c3l::Chunk> chunks;
  std::vector<c3l::Draw> draws;
  std::vector<ctr_draw_state> draw_states;  // per draw
  // (AI-assisted) per draw: every vertex it uses has alpha 0x80 (CTR_TCC_ALPHA80, fog_alpha), from
  // the vertices read at load (0 for chunks made later: the fog ramp path, which is always right)
  std::vector<u8> draw_alpha80;
  // (AI-assisted) config opaque_alpha as the c3l v15 kDrawAlphaTestDropped / kDrawBlendDropped draws'
  // states have it (draw_level changes them when the setting changes: gpu_profile = ab)
  bool opaque_alpha_applied = true;
  // made at load: the draws in drawing order, so a frame needs no sort. sorted_draws: the draws
  // whose order doesn't matter by (sort key, chunk, draw), then ordered_draws in chunk order.
  // draw_chunk: the chunk of each draw.
  std::vector<u32> sorted_draws;
  std::vector<u32> ordered_draws;
  // (AI-assisted) the opaque draws that may use the early depth test (CTR_STATE_EARLY_DEPTH),
  // sorted like sorted_draws; drawn first, nearest chunks first (draw_level)
  std::vector<u32> early_draws;
  std::vector<u32> draw_chunk;
  // (AI-assisted, j2cull) per draw: its list and place there (list << 30 | position; lists 0
  // early_draws, 1 sorted_draws, 2 ordered_draws), for draw_level's walk of only the visible
  // chunks' draws in list order (build_draw_order)
  std::vector<u32> draw_list_pos;
  // what draw_level's walk reads of a draw, in one cache line: copies of draw_chunk, draws,
  // draw_first_run, draw_run_count, draw_env, draw_wind and draw_morph_rows (made at the first
  // level draw after build_draw_order)
  struct alignas(32) WalkDraw {
    u32 chunk, first_index, index_count, first_run;
    u32 morph_rows;  // UINT32_MAX: none
    u16 run_count;
    s16 env;  // -1: none
    u8 wind;
  };
  std::vector<WalkDraw> walk;
  // (AI-assisted) c3l v10 visibility runs (c3l::VisRun): all of them, the first of each chunk,
  // and the range of each draw
  std::vector<c3l::VisRun> vis_runs;
  std::vector<u32> chunk_first_run;
  std::vector<u32> draw_first_run;
  std::vector<u16> draw_run_count;
  // (AI-assisted) config.ini run_culling: each run's bounding sphere, made at load from the
  // chunk's vertices, in the chunk's quantized units (world = origin + c * scale, radius r *
  // scale); r = 0xffff: unknown (the mesh wasn't made at load), never hidden. Chunks without
  // visibility runs get runs made at load for this (their draws in pieces, vis 0xffff).
  // chunk_run_count: all of a chunk's runs (Chunk::vis_run_count: only the file's).
  struct RunSphere {
    s16 c[3];
    u16 r;
  };
  std::vector<RunSphere> run_spheres;
  std::vector<u32> chunk_run_count;
  // (AI-assisted, titlemove) shrub runs: within their distance last frame (hysteresis; per run)
  std::vector<u8> run_dist_in;
  // (AI-assisted) c3l v17 tie levels of detail (c3l::ExtHeader): the LOD groups, each run's version
  // (parallel to vis_runs; empty: no levels of detail), per chunk whether it has versioned runs,
  // per group the last frame's choice (bit 0: near version, bit 1: chosen)
  std::vector<c3l::TieLodGroup> lod_groups;
  std::vector<c3l::RunLod> run_lod;
  std::vector<u8> chunk_lod;
  std::vector<u8> lod_state;
  // (AI-assisted, title-popin) per group its shape error (c3l::ExtHeader::lod_error_offset, game
  // units; empty: an older file, the groups switch at their dist)
  std::vector<float> lod_error;
  // (AI-assisted, lodnear) the PS2 floor of the switch by screen-space error, as an error per
  // unit of the factor: detail_scale / k of the last draw (0: none, older files). A group's
  // switch error is max(its shape error, its PS2 far-plane x this), so it is near at least within
  // the PS2 far-plane x detail_scale (+ hysteresis, + radius): never coarser than the PS2
  float lod_floor_k = 0.f;
  float lod_switch_error(size_t g) const {
    // (a morph group's near data: within its far-plane)
    if (g < group_mid.size() && group_mid[g] > 0) {
      return std::max(lod_groups[g].dist * lod_floor_k, lod_min_e);
    }
    const float e = std::max(std::max(lod_error[g], lod_groups[g].dist * lod_floor_k), lod_min_e);
    if (lod_ring_e > 0) {
      // the floor's factor: within ring 0 the config's switch
      const float e_cfg = std::max(std::max(lod_error[g] * lod_cfg_ratio,
                                            lod_groups[g].dist * lod_floor_k), lod_min_e);
      return std::max(e, std::min(e_cfg, lod_ring_e));
    }
    return e;
  }
  // the floor moved the factor (else 0): ring 0 in the units of lod_switch_error, and the
  // config's factor / this one
  float lod_ring_e = 0.f, lod_cfg_ratio = 1.f;
  float lod_bound_e() const { return std::max(lod_min_e, lod_ring_e); }
  // (AI-assisted, titlemove) the title screen's least switch distance, in the units of
  // lod_switch_error (distance / factor; 0 elsewhere)
  float lod_min_e = 0.f;
  std::vector<int> textures;  // ctr_gpu handles
  std::vector<u8> tex_min_alpha;  // (AI-assisted) smallest texel alpha per texture (0xff = 1.0)
  u32 tex_empty = 0;  // (AI-assisted) c3l v15: textures the file has no texels for (in atlases)
  // ctr_gpu texture pools holding all of them, in pieces of at most kTexPoolPiece bytes (no large
  // linear block needed; each moves to VRAM while there's room, the background's first)
  std::vector<int> tex_pools;
  u32 tex_pool_bytes = 0;     // (AI-assisted) their size (0: no textures)
  u32 tex_background_bytes = 0;  // (AI-assisted) its start: the background's textures (VRAM first)
  // c3l v23 shared textures (texshare.c3t): per texture 1 when its handle is a share group's (not
  // the level's to delete), and the groups the level holds (CtrLevels::release_share_groups)
  std::vector<u8> tex_shared;
  std::vector<u32> tex_share_groups;
  u32 tex_shared_bytes = 0;  // of the groups this level loaded
  bool tex_missing = false;  // the texture load failed (tex_pool_bytes: what it needs), repaired
  std::vector<int> meshes;    // one per chunk
  std::vector<c3l::MercBlercVertex> blerc_verts;
  std::vector<c3l::MercBlercTarget> blerc_targets;
  std::vector<u16> blerc_dests;
  u64 last_used_frame = 0;
  // (AI-assisted) the frame the background was drawn: the game sends a level's camera with each
  // of its tfrag trees (normal, trans, dirt, ice buckets); the first one draws all of it
  u64 drawn_frame = ~0ull;
  float bbox_min[3] = {0, 0, 0}, bbox_max[3] = {0, 0, 0};  // tfrag + tie, game units
  bool has_lowres = false;  // chunks with lod_tier 3
  // (AI-assisted) the bounds of the detailed tfrag (tier 1, or tier 0 tfrag in single tier files):
  // where the level's low resolution tfrag isn't drawn in near mode (config lowres_vista)
  float detail_min[3] = {0, 0, 0}, detail_max[3] = {0, 0, 0};
  bool has_detail = false;
  // (AI-assisted) Detailed geometry can extend this far past its distance-tested center.
  float detail_radius = 0;
  // (AI-assisted) lowres_vista, per chunk: whether its sphere is outside the detailed areas of all
  // loaded levels (bit 1) / inside another loaded level's (bit 3), bits 0 / 2: worked out. Valid
  // for the set of loaded levels of CtrLevels::set_generation() == outside_gen (not the camera)
  std::vector<u8> outside_cache;
  u32 outside_gen = 0;
  bool has_vista = false;  // (AI-assisted) c3l v13 vista tier chunks (CHUNK_KIND_VISTA)
  // (AI-assisted) c3l v14: the shrub chunks' max_dist is their shrubs' PS2 distance
  bool shrub_ps2_dist = false;
  // (AI-assisted) partial unload: a level only seen from a neighbouring one (far mode: only its
  // far chunks are drawn) frees the meshes of its other chunks after a while (linear memory for
  // the level the camera is in), and reads them again from its file when the camera comes back
  // (CtrLevels::unload_detail / request_detail). (AI-assisted, streammem) Read back in jobs of at
  // most kDetailJobBytes (the tfrag first, then the nearest), each one's meshes kept; meanwhile a
  // camera within far_level_distance draws it in near mode with what is there (its tfrag the far
  // tier's while some of tiers 1 / 2 are missing: detail_tfrag_missing).
  std::string path;  // the .c3l file
  u32 vertex_data_offset = 0, index_data_offset = 0;
  u32 merc_vertex_offset = 0, merc_index_offset = 0;  // (AI-assisted) for incomplete loads
  std::vector<u32> mesh_index_first, mesh_index_end;  // per chunk: its index range in the file
  bool detail_loaded = true;      // the meshes of all chunks are there
  bool detail_requested = false;  // their reload is queued
  u32 far_frames = 0;             // frames in a row beyond the unload distance
  u64 detail_retry_frame = 0;     // after a failed reload (no memory): not before this frame
  u32 detail_tfrag_missing = 0;   // (AI-assisted, streammem) tier 1 / 2 tfrag chunks not back yet
  // (AI-assisted) the load ran out of linear memory even after making room: some chunk or merc
  // meshes (or the texture pool) are missing. They are made again from the file once the memory
  // is there (CtrLevels::process_pending_loads, request_repair), while the level is drawn.
  bool incomplete = false;
  bool repair_requested = false;
  u64 repair_retry_frame = 0;  // after a repair that left parts out: not before this frame
  u8 repair_failures = 0;      // repairs in a row that made nothing (longer waits)
  // (AI-assisted) last frame's decisions, for hysteresis (CtrTfragRenderer::draw_level): per chunk
  // bits 0-1 detailed/coarse (1 near, 2 far, 0 not decided yet), bit 2 within its draw distance,
  // bit 3 shrub runs faded, bit 4 its distance decided at least once
  std::vector<u8> chunk_state;
  std::vector<u16> lod_switch_at;  // per tie LOD group: the frame (u16) it last switched version
  // (AI-assisted, title-popin) per chunk of tiers 1/2: a sphere around its lod_center holding both
  // versions of the cell (the same for both: their in-view tests agree), x y z r
  std::vector<float> lod_cell_sphere;
  bool was_far = false;
  // the far mode in effect (0 near, 1 far, 0xff not yet): was_far's switch waits while something it
  // changes is in view (draw_level), since far_want_frame
  u8 far_eff = 0xff;
  u64 far_want_frame = 0;
  // the vista copies became drawable (vista_ok) at this frame; per tie chunk whether it was last
  // decided with them (0xff: not yet): one in view keeps deciding without them for a while
  bool vista_ok_last = false;
  u64 vista_ok_frame = 0;
  std::vector<u8> chunk_vok;
  // (AI-assisted, j2-mem) Jak II: a level the game's visibility string has hidden whole for a while
  // (dormant_levels; CtrLevel.cpp "Dormant levels")
  bool dormant = false;
  u32 hidden_frames = 0;        // level draws in a row with its string all zero
  // (AI-assisted, j2intro) the frame its string showed it again after it was dormant (a reveal: the
  // title intro's first frame); CtrLevels::note_late_in_view
  u64 shown_frame = 0;
  u32 vis_scan_bytes = UINT32_MAX;  // the string's bytes its ids are in (UINT32_MAX: not known yet)
  std::vector<u8> chunk_all_vis;  // per chunk: a zero string hides all of its runs (draw_level)
  // per chunk: its residency (0 resident or never there, else kDist* / kTitle*), and the frame a
  // chunk without its mesh began to be kept hidden in view (0: not)
  std::vector<u8> chunk_dormant;
  std::vector<u32> chunk_hold_since;
  // per chunk left out of the load (Jak II): the time of day groups kept for it, made from its
  // mesh at its first draw (0: none, or made)
  std::vector<u16> chunk_tod_slots;
  // (AI-assisted, j2cull) draw_level's cull clusters, made at the level's first draw
  // (make_cull_clusters): its chunks in runs of neighbouring ones in index order (one test of a
  // cluster that lies beyond every member's distance limit, while all of them were beyond it last
  // frame, stands for the members' own decisions: the same outcome), the same for the tie LOD
  // groups, and the vista tier's chunks
  struct CullCluster {
    float c[3];          // center
    float a;             // the members' largest |center - c| + radius
    float lod_a;         // tiers 1 / 2 (chunks): the largest |lod_center - c|
    float raw[3];        // chunks: the members' largest max_dist by kind (tie, shrub, other); LOD
                         // groups: raw[0] the largest lod_error, raw[1] the largest dist
    u32 first;
    u32 count : 24;
    u32 flags : 8;       // kCl*
  };
  static constexpr u8 kClPlain = 1, kClTier12 = 2, kClHas = 4, kClNoOwn = 32;  // kClHas << kind
  std::vector<CullCluster> chunk_clusters, lod_clusters;
  std::vector<u32> vista_chunks;  // the CHUNK_KIND_VISTA chunks, in index order
  // ctr_pop: per chunk the frames it was last chosen / drawn, the chunks drawn last frame, those
  // with a vista copy, the last draw's camera, open holes (first, last frame)
  std::vector<u16> pop_sel, pop_drawn_at;
  std::vector<u32> pop_drawn;
  std::vector<u8> vista_copy;
  std::vector<u16> pop_far_chunk, pop_tf_chunk;  // per tie LOD / tfrag group: its far version's chunk
  float pop_cam[4] = {0, 0, 0, 0};
  u8 pop_far = 0;
  std::vector<u32> hole_since, hole_last;
  bool cull_made = false;
  size_t cull_sizes[3] = {0, 0, 0};  // chunks, tie LOD groups, chunk_unit when made
  bool dormant_requested = false;  // a read of its absent chunks is queued
  u64 dormant_retry_frame = 0;     // after one that ran short: not before this frame
  // Jak II (CtrLevel.cpp "Residency by need"): a shrub or tie chunk with a draw distance is
  // resident while the camera is within it (plus a lead), else absent (kDistFreed) or being read
  static constexpr u8 kDistFreed = 3, kDistReading = 4;
  // Jak 1 title screen: a chunk its attract camera can't show, left out (CtrLevel.cpp "Title
  // screen"); being read back
  static constexpr u8 kTitleFreed = 5, kTitleReading = 6;
  u32 title_left = 0;       // chunks in those states
  bool title_pass = false;  // this title's leave-out was applied (at load, or freed)
  bool dormant_freed(size_t ci) const {
    return ci < chunk_dormant.size() && chunk_dormant[ci] != 0;
  }
  u32 generation = 0;             // tells loads of the same level apart
  // (AI-assisted) c3l v16 time of day: the level's colors in the game's 8 palettes, blended with
  // the weights the game sends (its mood's itimes, as the PC's tfrag3 / tie3 / shrub renderers do
  // each frame) into the chunks' vertex colors (tod_* in CtrLevel.cpp). nullptr: the file's baked
  // palette (older file, or no heap to spare at load).
  // A table entry (a color, then the TodMuls) is blended when a vertex drawn needs it after one of
  // its palettes moved (version stamps), exact as the PC; a chunk's vertex groups are rewritten
  // whole when one of the chunk's day palettes moved, its light vertices when a light palette
  // moved, every frame before the chunk is drawn (CtrLevel.cpp tod_recolor; AI-assisted, todlag:
  // no budget).
  struct Tod {
    u32 color_count = 0;
    u8 palettes[8] = {};  // the palettes any color uses (not black), in order
    u32 palette_count = 0;
    CtrBigVec<u8> colors;      // color_count x palette_count x rgb (GS units)
    std::vector<c3l::TodMul> muls;
    // per entry (color_count colors, then the TodMuls; Vertex::pad): the palettes (bit p) its
    // color isn't black in, its rgb (r | g << 8 | b << 16) and the version that rgb is of (current
    // when at least lut[mask]: the last move of its palettes)
    std::vector<u8> color_mask;
    CtrBigVec<u32> table, stamp;
    u32 fresh_ver = ~0u;  // every entry is current at this version (tod_refresh_all)
    // per color the version its rgba last changed at (a TodMul changes only with its color)
    CtrBigVec<u32> cver;
    // per group its vertices by table entry (CtrLevel.cpp tod_make_list; 0: not made yet,
    // kNoList: none), made as the group is first written: a rewrite writes only the vertices of
    // the entries whose color changed since. Lists made before the shared room was last emptied
    // (list_gen) are gone.
    static constexpr u32 kNoList = 0xffffffffu;
    std::vector<u32> group_list;
    u32 list_gen = 0;
    // vertices of groups without a list written in this level draw and the last one; the title's
    // fast day (nearly every entry changes every frame: no lists)
    u32 whole_now = 0, whole_last = 0;
    u64 whole_frame = 0;
    u32 sweep_at = 0;  // the next chunk of the catch-up of what isn't drawn (tod_sweep)
    bool fast_day = false;
    // c3l v22 alpha: the colors' alpha in the 8 palettes by kind (each different one once), per
    // color its kind (empty: one kind), per kind its blend << 24 (the table's alpha byte; without
    // alpha the vertex keeps its own: keep_alpha), the palettes some kind has alpha in and the
    // alpha weights last blended. A kind at 0x80 in the day palettes (and 0x80 or 0 in the others)
    // isn't in its colors' masks: it blends to 0x80 while the day weights add up to 1.0 or more;
    // when its blend moves, every entry is blended again (tod_update)
    std::vector<std::array<u8, 8>> akinds;
    std::vector<u8> akind;
    std::vector<u32> aword;
    std::vector<u8> akind_full;
    u32 apal_mask = 0;
    u8 wa[8] = {};
    u32 keep_alpha = 0xff000000u;
    // "light" palettes: used by few colors (the lamps, the lava glow: the game moves their weights
    // every frame); per chunk its vertices with a light color (by the light palettes they use),
    // rewritten alone when only those move
    u32 light_mask = 0;  // palette bits
    u32 light_color_count = 0;
    using LightList = CtrLightLists;
    std::unordered_map<u32, LightList> chunk_lights;
    // weights (64 = 1.0): the game's at the level's last draw, the table's
    u8 weights[8][3] = {};
    u8 target_w[8][3] = {};
    bool blended_valid = false;  // weights taken at least once
    // versions: + 1 per change of target_w (dver: of its day palettes); per palette its last;
    // lut[mask]: the last of the palettes in mask
    u32 ver = 0, dver = 0;
    u64 day_move_frame = 0;  // (AI-assisted, perfv2) the frame of the last day move (dver + 1)
    u32 pal_ver[8] = {};
    u32 lut[256] = {};
    u32 track_mask = 0;  // the palettes whose weights are watched (rgb or alpha)
    // the palettes a blend adds (weight not 0): their slots in colors and weights
    struct Act {
      u32 n = 0;
      u8 slot3[8] = {};
      u8 w[8][3] = {};
      // by palette (bit p of mask): its slot in colors, its weights (r | b << 16, g)
      u32 mask = 0;
      u8 off[8] = {};
      u32 wrb[8] = {}, wg[8] = {};
    };
    Act act;
    // per chunk: its vertex groups (VisGroup: one per visibility id of its runs when their vertex
    // ranges don't overlap, c3l v18 files, in vertex order; else one for all), each with the
    // version its colors are (0: the file's), the day move count then, and its light version;
    // the oldest of those per chunk (quick outs); the palettes its vertices use (scanned: 0
    // not yet (no mesh at load), 1 no time of day color, 2 some) and its mesh (a mesh made again has
    // the file's colors)
    using VisGroup = CtrLightGroup;
    std::vector<VisGroup> groups;
    std::vector<u32> chunk_group_first;
    std::vector<u16> chunk_group_count;
    std::vector<u16> run_group;  // per run (CtrLevelData::vis_runs): its group in the chunk
    std::vector<u32> chunk_min_ver, chunk_min_dver, chunk_light_ver;
    std::vector<u8> chunk_mask, chunk_scanned;
    std::vector<int> chunk_mesh;
    // the title's fast day: per chunk its vertices' table entries (CtrLevel.cpp tod_entries), so a
    // whole rewrite reads 2 bytes a vertex, not its 12-byte record; gone with the title or the mesh
    std::vector<std::unique_ptr<u16[]>> chunk_entries;
    u32 entries_bytes = 0;
    u32 bytes() const {
      u32 lights = 0;
      for (const auto& [ci, l] : chunk_lights) {
        lights += (u32)(l.verts.size() * 2 + l.masks.size() * 8 + l.runs.size() * 4 +
                        l.ranges.size() * 4) + 96;
      }
      return (u32)(colors.size() + muls.size() * sizeof(c3l::TodMul) + color_mask.size() +
                   table.size() * 8 + cver.size() * 4 + group_list.size() * 4 + akind.size() +
                   akinds.size() * 13 +
                   groups.size() * sizeof(VisGroup) + run_group.size() * 2 +
                   chunk_group_first.size() * 24 + chunk_entries.size() * 4 + entries_bytes) +
             lights;
    }
  };
  std::unique_ptr<Tod> tod;
  // (AI-assisted, streaming) c3l v19 near chunks (c3l::StreamChunk; CtrStream.h,
  // docs/3ds-port/streaming.md): the near versions of the tie LOD groups, in chunks of their own.
  // streaming: read from the file while the camera is near (CtrLevels::stream_update) into the
  // stream arena, freed when far and the room is needed; else the core ones are loaded with the
  // level and the others never. A group is drawn with its near version only while every near chunk
  // holding it is there and within its distance (draw_level), else with its far version.
  struct StreamUnit {
    u32 chunk = 0;
    u16 cls = 0;  // c3l::kStreamCore / kStreamExtra
    u8 state = 0;  // kUnit*
    u8 tod_layout = 0;  // its time of day groups were made (at its first read)
    u16 tod_slots = 0;  // the time of day groups kept for it at load (its visibility ids)
    float need_a = 0, need_e = 0;  // c3l::StreamChunk
    float need_d = 0;  // (AI-assisted, lodnear) the largest PS2 far-plane of its groups (game units)
    // (AI-assisted, lodnear) this frame (stream_update): one of its groups is within its distance
    // by shape error (its far version would show more than tie_lod_px), not only by the PS2 floor
    bool urgent = false;
    bool tf = false;  // a tfrag geomorph near chunk (c3l::ExtTfrag): wanted with or without tie_lod
    // its bound with the factor k: within this of its sphere's center one of its groups is near
    float bound(float k, float floor_k, float min_e = 0.f) const {
      return need_a + k * std::max(std::max(need_e, need_d * floor_k), min_e);
    }
    u32 bytes = 0;   // vertices + indices in the arena (CtrStreamArena::mesh_bytes)
    u32 packed = 0;  // file bytes of its blocks
    void* mem = nullptr;  // kUnitLoading: the arena piece the job fills
    // a time it was needed while not there: since (ms, frame; late_ms 0: not late), last frame
    double late_ms = 0, late_last_ms = 0;
    u64 late_frame = 0, late_last = 0;
    bool late_cut = false;  // the late time began within 2 s of a camera cut
    u32 loads = 0;
    u64 want_frame = 0;  // (statistics) the first frame it was wanted while absent (0: not)
    float last_score = 0;
    u64 room_frame = 0;  // (statistics) the last frame the arena had no room for it
    // (AI-assisted, popin2) this frame (stream_update): how much its groups' far versions would
    // show from the camera now, the largest of them (shape error x k over the distance to the
    // group's surface: 1 = at the switch distance, tie_lod_px; 1.5x while in view); 0 for a unit
    // nobody needs, or a level without shape errors. At the arena's capacity a unit gives way to a
    // needed one whose priority is kPrioHyst x larger
    float prio = 0;
    u64 resident_frame = 0;  // (AI-assisted, popin2) the frame it was last taken (became there)
    // this frame: needed now by a group whose surface is within ring 0 of the camera
    bool ring0 = false;
    // the last frame a group of it was drawn far in view for want of it; the frames its morph in
    // and its morph out began (0: none)
    u32 late_seen = 0, morph_in = 0, leave = 0;
    u32 left = 0;  // the frame it was freed at the end of a morph out
  };
  // a unit's morph floor: its geomorph factors are at least this (1: the far versions' surface)
  static constexpr u32 kMorphInFrames = 24, kMorphOutFrames = 16;
  static float unit_floor(const StreamUnit& su, u32 frame) {
    float a = 0.f;
    if (su.morph_in && frame - su.morph_in < kMorphInFrames) {
      a = 1.f - (float)(frame - su.morph_in) / kMorphInFrames;
    }
    if (su.leave) {
      a = std::max(a, std::min(1.f, (float)(frame - su.leave + 1) / kMorphOutFrames));
    }
    return a;
  }
  // chunk ci's (0 unless it is a unit)
  float chunk_floor(size_t ci, u32 frame) const {
    return is_unit(ci) ? unit_floor(stream_units[chunk_unit[ci]], frame) : 0.f;
  }
  // chunk ci has geomorph records (the ties' or the tfrag's): a floor moves it
  bool chunk_morphs(size_t ci) const {
    return (ci < chunk_morph.size() && chunk_morph[ci] >= 0) || tf_morph(ci);
  }
  static constexpr u8 kUnitAbsent = 0, kUnitLoading = 1, kUnitResident = 2, kUnitNever = 3;
  std::vector<StreamUnit> stream_units;
  std::vector<u32> chunk_unit;  // per chunk: its unit, UINT32_MAX none (empty: no near chunks)
  // per tie LOD group: the units holding its near version (group_units[first, first + count))
  std::vector<u32> group_unit_first;
  std::vector<u8> group_unit_count;
  std::vector<u32> group_units;
  // per unit: the groups whose near version it holds (unit_groups[first[u], first[u + 1]))
  std::vector<u32> unit_group_first;
  std::vector<u32> unit_groups;
  std::vector<u8> unit_ok;  // per unit, this frame (draw_level): there and within its distance
  std::vector<u32> lod_hold_since;  // per LOD group: the frame its far version began to be kept
  bool streaming = false;
  float stream_k = 0.f;   // the last draw's tie LOD factor (a group is near within its error x
                          // this + its radius; 0: no near versions)
  u64 stream_frame = 0;   // the last draw's frame
  double dist_warn_ms = 0;  // the last "read back by distance" shortage line
  // the tfrag geomorph's units' factor while stream_k is 0 (tie_lod off, no tie LOD groups): 1 while
  // its versions are drawn, else 0
  float stream_tf_k = 0.f;
  float unit_k(const StreamUnit& su, float k) const { return su.tf && k <= 0 ? stream_tf_k : k; }
  float stream_k_hi = 0.f;  // its recent largest (decaying 1% a frame): the scheduler's headroom
                            // for a camera that zooms (the factor follows the focal length)
  u32 stream_resident = 0;  // bytes of the units there
  // (AI-assisted, title3) the frame of the last draw_level (any mode): a draw after a gap of more
  // than 2 frames shows the level again (the title's reveal of Sandover, a level start): its time
  // of day rewrites aren't capped or spread that frame (draw_level)
  u64 last_draw_frame = 0;
  u64 reveal_frame = 0;  // (AI-assisted, title3) the last such draw's frame
  // the file's header and block directory, for the stream jobs' readers (no reads of them)
  std::shared_ptr<const c3l::Reader::Dir> file_dir;
  bool is_unit(size_t ci) const { return !chunk_unit.empty() && chunk_unit[ci] != UINT32_MAX; }
  // (AI-assisted, j2vis) c3l v20 prototype visibility (c3l::ProtoVis): the prototypes Jak II hides
  // by task state, each with its runs (vis_runs indices, sorted) and the chunks they are in. The
  // game names the hidden ones every frame (pc-add-tie-vis-mask / pc-add-shrub-vis-mask packets,
  // CtrTfragRenderer::proto_vis_scan); their runs aren't drawn (draw_level, emit_chunk).
  struct ProtoVis {
    std::string name;
    u16 kind = 0;  // c3l::kProtoTie / kProtoShrub
    bool hidden = false;
    std::vector<u32> runs;
    std::vector<u32> chunks;
  };
  std::vector<ProtoVis> protos;
  // run r is one of a toggled prototype's (hidden or not)
  bool proto_run(u32 r) const {
    for (const auto& p : protos) {
      if (std::binary_search(p.runs.begin(), p.runs.end(), r)) {
        return true;
      }
    }
    return false;
  }
  // the game's last packets applied (tie, shrub; bytes as sent) and whether one came yet
  std::string proto_mask[2];
  bool proto_mask_seen[2] = {false, false};
  // the hidden prototypes' runs (sorted) and per chunk whether it has some (empty: none hidden)
  std::vector<u32> proto_hidden_runs;
  std::vector<u8> chunk_proto_hidden;
  // (AI-assisted, j2water2) c3l v21 envmapped ties (c3l::ExtEnvTie): the listed base draws (their
  // shine), per draw its index there (-1: none; empty: no such draws), per chunk whether it has
  // normals (kNormalsInMesh: in its own mesh, after the vertices or, for a streamed near chunk,
  // after the indices; freed and read back with it; -1: none) and their file offset (0: none).
  // normals_mesh: unused (-1).
  static constexpr s32 kNormalsInMesh = -2;
  std::vector<c3l::EnvTieDraw> env_draws;
  std::vector<s16> draw_env;
  // the level's shine table as of shine_gen, and per chunk the squared distances of the camera
  // from its sphere's center up to which every run of its envmapped draws has its full shine,
  // and from which none has any (empty: no table, every run keeps its shine)
  std::shared_ptr<const CtrShineTable> shine;
  u32 shine_gen = 0;
  std::vector<float> chunk_shine_full, chunk_shine_none;
  // c3l::kDrawCullCond draws (c3l::ExtCullCond): each one's planes in cond_planes, its boxes in
  // cond_boxes (box_first..): u16 steps of cond_box_step from cond_box_origin, rounded outward
  struct CondDraw {
    u32 draw, first, count;
    u32 box_first, boxes;
  };
  struct CondBox {
    u16 lo[3], hi[3];
  };
  std::vector<CondDraw> cond_draws;
  // culled draws drawn both sides this frame (across the camera's plane): draw, its tcc
  std::vector<std::pair<u32, u8>> near_uncull;
  std::vector<c3l::CullCondPlane> cond_planes;
  std::vector<CondBox> cond_boxes;
  float cond_box_origin[3] = {0, 0, 0};
  float cond_box_step = 1.f;
  // the PS2's tie geomorph (c3l::ExtMorph): per LOD group its mid-plane (0: switched as before),
  // the morphing instances, the near chunks' rows (instance | band << 30) and per draw the rows
  // its vertices use; per chunk its MorphChunk (-1: none)
  std::vector<float> group_mid;
  std::vector<c3l::MorphInst> morph_insts;
  std::vector<u32> morph_rows;
  std::vector<u8> morph_draw_rows;
  std::vector<c3l::MorphChunk> morph_chunks;
  std::vector<s32> chunk_morph;
  std::vector<u32> draw_morph_rows;  // per draw of a morph chunk: its list in morph_draw_rows
  bool morph_group(size_t g) const { return g < group_mid.size() && group_mid[g] > 0; }
  float morph_k = 1.f;  // the last draw's distance factor of the morph groups and rows
  std::vector<s32> chunk_normals;
  std::vector<u32> chunk_normals_file;
  int normals_mesh = -1;
  //! where chunk ci's normals are in its mesh (bytes from its vertices)
  u32 normals_offset_in_mesh(size_t ci) const;
  // the PS2's tfrag geomorph (c3l::ExtTfrag): the near tier's groups (their version this frame in
  // tf_state, as lod_state: bit 0 near or mid, bit 3 mid), per chunk its TfragChunk (-1: none),
  // the color lists; per time of day group the version its morph colors were made at
  std::vector<c3l::TfragGroup> tf_groups;
  std::vector<u8> tf_state;
  std::vector<c3l::TfragChunk> tf_chunks;
  std::vector<s32> chunk_tf;
  std::vector<c3l::TfragMorphColor> tf_colors;
  float tf_dists[3] = {0, 0, 0};
  std::vector<u32> tf_color_ver;
  std::vector<u16> tf_color_mask;  // per time of day group: the palettes of its morph colors
  std::vector<int> tf_mesh;       // per chunk: the mesh its morph colors were made in
  // per group its near chunks (stream_units holding its near or mid version) and the frame it was
  // first held coarser in view for want of them
  std::vector<u32> tf_unit_first, tf_unit_count, tf_units, tf_hold_since;
  bool tf_morph(size_t ci) const {
    return ci < chunk_tf.size() && chunk_tf[ci] >= 0 && tf_chunks[chunk_tf[ci]].records_offset;
  }
  //! the bytes of chunk ci's mesh tail per vertex (0: none): its envmapped tie normals, or its
  //! c3l::MorphVertex records (the tie geomorph), or its c3l::TfragMorphVertex records (the tfrag
  //! geomorph), at normals_offset_in_mesh
  u32 tail_k(size_t ci) const {
    if (tf_morph(ci)) {
      return (u32)sizeof(c3l::TfragMorphVertex);
    }
    if (ci < chunk_morph.size() && chunk_morph[ci] >= 0) {
      return (u32)sizeof(c3l::MorphVertex) + (morph_normals(ci) ? 4 : 0);
    }
    return ci < chunk_normals.size() && chunk_normals[ci] == kNormalsInMesh ? 4 : 0;
  }
  //! a morph chunk whose envmapped tie normals follow its records in the file (then in its tail)
  bool morph_normals(size_t ci) const {
    return ci < chunk_normals.size() && chunk_normals[ci] == kNormalsInMesh &&
           chunk_normals_file[ci] == morph_chunks[chunk_morph[ci]].records_offset +
                                         chunks[ci].vertex_count * (u32)sizeof(c3l::MorphVertex);
  }
  u32 normals_tail_bytes(size_t ci) const { return chunks[ci].vertex_count * tail_k(ci); }
  //! the file offset of chunk ci's tail (0: none)
  u32 tail_file(size_t ci) const {
    if (tf_morph(ci)) {
      return tf_chunks[chunk_tf[ci]].records_offset;
    }
    if (ci < chunk_morph.size() && chunk_morph[ci] >= 0) {
      return morph_chunks[chunk_morph[ci]].records_offset;
    }
    return tail_k(ci) ? chunk_normals_file[ci] : 0u;
  }
  // wind ties (c3l::ExtWind): the instances, per chunk its slots' byte offset in wind_mesh (-1:
  // none) and its slot instances (chunk_wind_first, chunk_wind_n into wind_slot_inst); the PC's
  // wind state per wind_idx (Tie3 m_wind_vectors) and this frame's shear per instance (kx, kz)
  std::vector<c3l::WindInst> wind_insts;
  std::vector<s32> chunk_wind;
  std::vector<u32> chunk_wind_first;
  std::vector<u8> chunk_wind_n;
  std::vector<u16> wind_slot_inst;
  std::vector<u8> draw_wind;  // per draw: it has wind vertices
  // shrub fade bands (c3l::ExtShrubFade) per run, x << 16 | w in 1/16 m (0: the chunk's class),
  // and per shrub chunk with bands its smallest x (game units)
  std::vector<u32> run_band;
  std::vector<float> chunk_band_x;
  int wind_mesh = -1;
  std::vector<float> wind_vectors;
  std::vector<float> wind_k;
  u64 wind_frame = ~0ull;
  // the last WindWork's wind_time and wind_array (wind_update: the 60 Hz steps of a frame)
  u32 wind_last_time = 0;
  bool wind_have_last = false;
  float wind_last_array[64][4] = {};
};

/*! DrawMode (tfrag3 draw settings) to ctr_gpu state. */
ctr_draw_state ctr_state_from_draw_mode(DrawMode mode, int tex);

namespace c3l {
class Reader;
}

/*!
 * The .c3l files of the levels the game has loaded. On the 3DS they are read on a loader thread
 * (core 2, below the render thread and the sound mixer): a level file takes a few hundred ms to
 * seconds to read from the SD card and build, and loading it on the render thread froze the game
 * for that long (the game waits for the render thread). A level is drawn from the frame after its
 * load finished. Elsewhere (PC tools) the loads run on the render thread, as before.
 * Threads: get/find_merc_model/process_pending_loads run on the render thread, set_wanted and
 * service on the game thread while the render thread is idle, prefetch on any thread.
 */
/*! (AI-assisted) Where the running game's .c3l files are: <project>/out/<game>/c3l. */
fs::path ctr_level_c3l_dir();
/*! (AI-assisted) Jak II: the level whose DGO file this is ("CTA.DGO" -> "ctysluma"), nullptr: none.
 * For prefetching a level's .c3l when the overlord starts reading its DGO. */
const char* ctr_jak2_level_for_dgo(const char* dgo_name);

/*! (AI-assisted) Jak II: the visibility strings of the 6 level slots (*level* draw-level) this
 * frame, from bucket 2 (add-pc-port-background-data), for the background buckets of each slot. */
struct CtrJak2Vis {
  static constexpr int kSlots = 6;
  u8 bits[kSlots][2048];
  bool valid[kSlots] = {};
};

class CtrLevels {
 public:
  /*! (AI-assisted) Jak II's visibility strings (CtrRendererJak2Bg.cpp writes, the level draws
   * read; render thread). */
  CtrJak2Vis& jak2_vis() { return m_jak2_vis; }

  CtrLevels();
  ~CtrLevels();
  /*! Load the common file (GAME.c3l: Jak and other shared models), kept loaded. */
  void load_common();
  /*! (AI-assisted) Load another file of models kept loaded like the common one (render thread), the
   * first time it's asked for: KEIRA.c3l, the "play as Keira" easter egg's (CtrKeiraRig.h). */
  void load_extra(const std::string& name);
  /*! Start loading a level before the game asks for it (any thread): the overlord started its DGO.
   * Kept aside, not drawn, until set_wanted or get asks for it. (AI-assisted) The game loads one
   * level at a time: a prefetch of any other level it doesn't want is dropped (it gave up on that
   * one), the one of this level after a minute if nothing asks for it. */
  void prefetch(const std::string& name);
  /*! Find a merc model in the loaded levels. Returns the level too (for its textures). */
  // (AI-assisted) a string_view: no std::string made for each lookup (~50 per frame)
  const CtrMercModelData* find_merc_model(std::string_view name, const CtrLevelData** lev);
  /*! Get a level, starting its load if needed. nullptr while it loads, or if there is no .c3l. */
  CtrLevelData* get(const std::string& name, u64 frame);
  /*! Start the loads get()/set_wanted asked for and take the finished ones (render thread, outside
   * of a GPU frame). Doesn't wait for loads on the 3DS. */
  void process_pending_loads(u64 frame);
  /*! The render thread's time for the last frame: the time of day catch-up of what isn't drawn
   * (tod_sweep) runs after frames that left room. */
  void note_frame_ms(double ms) { m_tod_sweep_ok = ms < 26.0; }
  struct TodSweepStats {
    u64 verts = 0;
    u32 frames = 0;
    double ms = 0, max_ms = 0;
  };
  /*! tod_sweep's work since the last call (the statistics line) */
  TodSweepStats take_tod_sweep_stats() {
    const TodSweepStats s = m_tod_sweep_stats;
    m_tod_sweep_stats = TodSweepStats();
    return s;
  }
  /*! Levels the game wants (set_levels): others are unloaded. */
  void set_wanted(const std::vector<std::string>& names);
  /*! The level's .c3l is loaded (or failed, or there is none, or no load is pending for it):
   * nothing to wait for (any thread). The game waits for this before a level starts. */
  bool ready(const std::string& name);
  /*! (AI-assisted, j2intro) host, env CTR_LOAD_DELAY_MS (load_sim_on): the game thread waits in
   * ready() without frames (Jak II's boot and play loops), and the host has no loader thread: run
   * the queued loads, hand over the delayed ones whose time came (render thread idle) */
  void host_service();
  static bool load_sim_on();
  /*! (AI-assisted) The levels being drawn (render thread). */
  void loaded(std::vector<const CtrLevelData*>* out) const {
    out->clear();
    for (const auto& [name, lev] : m_levels) {
      out->push_back(lev.get());
    }
  }
  /*! (AI-assisted) Changes whenever the loaded levels (loaded()) change: a publish or unload, also
   * a partial unload and a reload / repair of parts (never 0). */
  u32 set_generation() const { return m_set_gen; }
  /*! Partial unload (render thread): free the meshes a far level doesn't draw / read them again. */
  void unload_detail(CtrLevelData& lev);
  void request_detail(CtrLevelData& lev);
  /*! Jak II, render thread, each frame: the chunks with a draw distance read and freed by need
   * under one budget of linear memory (CtrLevel.cpp "Residency by need"). parts_room: the meshes
   * of the parts jobs in flight; claim: what else waits for linear memory (the stream arena) */
  void need_update(u64 frame, u32 free_now, u64 parts_room, bool level_loading, u64 claim);
  /*! (AI-assisted, streaming) render thread, each level draw: the camera position (game units) */
  void stream_note_camera(const math::Vector4f& trans,
                          const math::Vector4f planes[4],
                          const math::Vector4f rot[4],
                          u64 frame);
  /*! (AI-assisted) the camera of the last level draw if it is this frame's: world = trans + rot *
   * (merc camera space), rot row major (the "play as Keira" ponytail) */
  bool camera(u64 frame, float rot[9], float trans[3]) const {
    if (!m_cam_valid || m_cam_frame != frame) {
      return false;
    }
    memcpy(rot, m_cam_rot, sizeof(m_cam_rot));
    memcpy(trans, m_cam, sizeof(m_cam));
    return true;
  }
  /*! (AI-assisted, title3) the game's camera position this frame (*math-camera* trans, game
   * units; game thread, while the render thread is idle): the near chunks of a level that is loaded
   * but not drawn (Sandover behind the title's logo) are read around it before it is shown */
  /*! (AI-assisted, title3) draw_level, any level (the title's logo too): this frame's tie LOD factor
   * (detail_scale x focal / tie_lod_px x 1.1), the hidden levels' factor before any streaming level
   * was drawn */
  void note_k_any(float k) { m_k_any = k; }
  void note_game_camera(const float* trans) {
    for (int i = 0; i < 3; i++) {
      m_game_cam_next[i] = trans[i];
    }
    m_game_cam_new = true;
  }
  //! game thread, render thread idle: the camera position of the game's continue point
  void note_continue_camera(const float* trans, bool valid) {
    for (int i = 0; i < 3 && valid; i++) {
      m_cont_cam[i] = trans[i];
    }
    m_cont_valid = valid;
  }
  //! game thread, render thread idle (Jak II): where the game's continue point puts Jak and the
  //! camera, n positions while the game is black (0: not black, or none), and the levels it wants
  void note_continue_place(const float (*pos)[3], int n, int want_levels);
  //! render thread: something that is missing in view can still come (a level load, a read by
  //! need or a near chunk job in flight, meshes read and not taken yet)
  bool coming();
  //! game thread, render thread idle: linear memory was short (Jak II): the textures decoded
  //! from the game's VRAM that nothing drew lately are to be freed (CtrVram::trim)
  bool take_tex_trim();
  /*! (AI-assisted, streaming) Jak's first bone in camera space (merc renderer, any frame) */
  void stream_note_target(float x, float y, float z, u64 frame) {
    m_target_cam[0] = x;
    m_target_cam[1] = y;
    m_target_cam[2] = z;
    m_target_cam_frame = frame;
  }
  /*! (AI-assisted, streaming) a jump of a level's tie LOD factor (the camera's focal length: a cut
   * to another camera, a zoom) */
  void stream_note_cut(u64 frame) { m_cut_frame = frame; }
  /*! (AI-assisted, streammem) the last camera cut's frame, and where the camera will be in `frames`
   * frames at its speed (game units; false: no camera yet) */
  u64 stream_cut_frame() const { return m_cut_frame; }
  bool camera_lead(float frames, float out[3]) const {
    if (!m_cam_valid) {
      return false;
    }
    for (int i = 0; i < 3; i++) {
      out[i] = m_cam[i] + m_cam_vel[i] * frames;
    }
    return true;
  }
  /*! (AI-assisted, j2intro; Jak 1's title3 has the same) draw_level (Jak II): what of a level is
   * missing in view this frame: tie LOD groups drawn far because their near chunks aren't there
   * yet, and dormant tie chunks not read back yet (not drawn at all). recent: the level was shown
   * again (its dormancy ended) within the last 3 s. The renderer keeps a screen that comes out of
   * black black while there is such a thing after a level was shown or the camera cut
   * (CtrRenderer::draw_pmode_fade), at most 1.5 s (Jak II: while it gets fewer, at most 6 s). */
  //! render thread, each frame: the game's own blackout (not the renderer's black hold)
  void set_game_black(bool black, bool hold) {
    m_game_black.store(black, std::memory_order_relaxed);
    m_hold_black.store(hold, std::memory_order_relaxed);
  }
  //! ring0: of n, the ones within ring 0 (and chunks missing whole)
  void note_late_in_view(u64 frame, u32 n, bool recent, u32 ring0 = 0) {
    if (m_lv_frame != frame) {
      m_lv_frame = frame;
      m_lv_count = 0;
      m_lv_ring0 = 0;
      m_lv_recent = false;
    }
    m_lv_count += n;
    m_lv_ring0 += ring0;
    m_lv_recent = m_lv_recent || recent;
  }
  //! this frame's count (note_late_in_view), and whether a level was shown or the camera cut
  //! within the last 3 s
  u32 late_in_view(u64 frame, bool* shown_or_cut, u32* ring0 = nullptr) const {
    const bool now = m_lv_frame == frame;
    *shown_or_cut = (now && m_lv_recent) || (m_cut_frame && frame < m_cut_frame + 90);
    if (ring0) {
      *ring0 = now ? m_lv_ring0 : 0;
    }
    return now ? m_lv_count : 0;
  }
  /*! (AI-assisted, streaming) the streaming statistics, logged (render thread) */
  void stream_log();
  void pop_log();
  /*! (AI-assisted, j2-mem) the levels' memory, the free linear memory and heap (render thread) */
  void log_memory();
  void log_linear(u32 arena_used);
  /*! (AI-assisted, streaming) host check (env CTR_STREAM_CHECK): the counts of this frame's draw
   * of a level (CtrTfragRenderer::draw_level) */
  struct StreamCheck {
    u64 frames = 0, groups = 0;
    u64 late_groups = 0, late_frames = 0;    // near in the reference, far: data not there yet
    u64 late_unrequested = 0;                // ... and not even being read (no room)
    u64 holes = 0;                           // near versions drawn without their data (never)
    u64 units_drawn = 0, units_baked = 0;    // near chunks drawn; ... with baked colors (never)
    // (AI-assisted, j2-mem) dormant levels: freed tie chunks within their distance in view (not
    // drawn: never, but for camera cuts), and shown in view after a hold ran out
    u64 dorm_missing = 0, dorm_switch_in_view = 0;
  };
  StreamCheck& stream_check() { return m_scheck; }
  ctr_pop::Stats& pop_stats() { return m_pop; }
  /*! (AI-assisted, streaming) a unit needed (draw_level): late statistics */
  void stream_late(CtrLevelData& lev, u32 unit, u64 frame);
  /*! (AI-assisted, streaming) a level drawn: its groups drawn far because their data was late */
  void stream_note_level(CtrLevelData& lev, u32 late_groups, u32 held_groups, u64 frame,
                         u32 ring0_late = 0);
  /*! (AI-assisted) What the render thread does at the start of each frame for the loader: a
   * parked prefetch the game gave up on to unload, or the loader waiting for memory (make_room).
   * The game can wait for a level without drawing frames (both level slots empty: it loads in a
   * loop), so it does this itself then: service on the game thread while the render thread is
   * idle, when needs_service (any thread). */
  bool needs_service();
  void service();

 private:
  CtrJak2Vis m_jak2_vis;  // (AI-assisted)
  enum class LoadResult { LOADED, MISSING, FAILED, CANCELLED };
  LoadResult load(const std::string& name, CtrLevelData* out);
  // (AI-assisted) decode: time spent decompressing (c3l v11), 0 for older files
  bool load_file(const fs::path& path,
                 const std::string& name,
                 CtrLevelData* out,
                 long* file_size,
                 double* decode_ms);
  void unload(CtrLevelData& lev);
  bool cancelled() const { return m_cancel.load(std::memory_order_relaxed); }
  std::map<std::string, std::unique_ptr<CtrLevelData>> m_levels;
  std::map<std::string, bool> m_missing;
  // (AI-assisted) a load that failed (out of memory, a bad SD read): retried later, sooner when the
  // heap's largest free block grew since (3DS)
  struct FailedLoad {
    double ms = 0;     // when
    u32 largest = 0;   // the largest free heap block then (3DS; 0: not known)
    bool drawn = false;  // get() was asked for it since
  };
  std::map<std::string, FailedLoad> m_failed;
  // (AI-assisted) render thread: wanted levels whose load failed, loaded again when memory frees
  void retry_failed_loads(u64 frame);
  std::vector<std::string> m_pending_loads;
  std::vector<std::string> m_wanted;
  bool m_common_wanted = false;
  std::unique_ptr<CtrLevelData> m_common;
  // (AI-assisted) load_extra: the files asked for, and the loaded ones (with the common file's)
  std::vector<std::string> m_extra_asked;
  std::map<std::string, std::unique_ptr<CtrLevelData>> m_extras;
  // merc model name -> (level, model index); rebuilt when levels change
  std::map<std::string, std::pair<CtrLevelData*, int>, std::less<>> m_merc_index;
  void rebuild_merc_index();
  u32 m_set_gen = 1;  // (AI-assisted) set_generation
  void bump_set_generation() { m_set_gen = m_set_gen + 1 ? m_set_gen + 1 : 1; }
  // a level that loaded (or was prefetched) and is taken into m_levels
  void publish(const std::string& name, std::unique_ptr<CtrLevelData> lev, u64 frame);

  // ---- loader ----
  // (AI-assisted) a chunk mesh to read again (partial unload, repair)
  struct DetailChunk {
    u32 chunk;
    u32 first_vertex, vertex_count;
    u32 index_first, index_end;
    u32 normals_offset = 0;  // the mesh's tail (CtrLevelData::tail_file, 0: none)
    u32 tail_k = 0;          // its bytes per vertex (CtrLevelData::tail_k)
  };
  // (AI-assisted) a merc model mesh to make again (repair)
  struct RepairMerc {
    u32 model;
    u32 file_vertex, vertex_count;
    u32 index_first, index_end;
  };
  struct Job {
    std::string name;
    bool common = false;
    bool prefetch = false;
    // detail reload (request_detail) or (AI-assisted) repair of an incomplete load
    // (request_repair): only these parts of the loaded level `generation`
    bool detail = false;
    bool repair = false;
    bool dormant = false;  // with detail: absent chunks read (Jak II by need, Jak 1's title)
    u32 generation = 0;
    std::string path;
    u32 vertex_data_offset = 0, index_data_offset = 0;
    std::vector<DetailChunk> chunks;  // in file order
    u32 merc_vertex_offset = 0, merc_index_offset = 0;
    std::vector<RepairMerc> merc;  // in file order
    bool textures = false;         // the texture pool
    // (AI-assisted, streaming) a stream job: these near chunks into their arena pieces
    bool stream = false;
    struct StreamPart {
      u32 unit, chunk;
      u32 first_vertex, vertex_count, index_first, index_end;
      u32 normals_offset = 0;  // the mesh's tail after the indices (CtrLevelData::tail_file)
      u32 tail_k = 0;          // its bytes per vertex
      void* mem = nullptr;
      // the chunk's draws (first_index relative to its indices) and runs, for its time of day
      // groups (layout: not made yet)
      bool layout = false;
      std::vector<c3l::Draw> draws;
      std::vector<c3l::VisRun> runs;
    };
    std::vector<StreamPart> stream_parts;  // in file order
    u32 tod_ntab = 0;                      // the level's time of day entries (0: none)
    std::shared_ptr<const c3l::Reader::Dir> dir;  // the file's directory (CtrLevelData::file_dir)
    size_t window = 0;                     // the reader's window for these reads
    u32 serial = 0;                        // (stream jobs: each its own key)
    bool urgent = false;  // (stream) holds units needed now: before a level's prefetch
    bool ring0 = false;   // (stream) starts with ring 0 units
    // (AI-assisted, streammem) the linear memory the job's meshes take (parts jobs: kept free of
    // the stream arena's growth while it is queued or runs), when it was queued (the loader's
    // aging), a detail reload with more chunks after these (read by the next one)
    u64 room_bytes = 0;
    double queued_ms = 0;
    bool detail_more = false;
    bool parts() const { return detail || repair || stream; }
    // in m_requested
    std::string key() const {
      return dormant  ? name + "#dormant"
             : detail ? name + "#detail"
             : repair ? name + "#repair"
             : stream ? name + "#stream" + std::to_string(serial)
                      : name;
    }
  };
  // (AI-assisted) a level's texture pool and textures (load_textures)
  struct TexLoad {
    std::vector<int> pools;
    u32 pool_bytes = 0;
    u32 background_bytes = 0;  // the pool's start: the background's textures
    std::vector<int> handles;
    std::vector<u8> min_alpha;
    u32 empty = 0;  // (AI-assisted) c3l v15: textures without texels (all in atlases): no handle
    std::vector<u8> shared;          // as CtrLevelData::tex_shared
    std::vector<u32> share_groups;   // the groups held (one reference each)
    u32 shared_bytes = 0, borrowed_bytes = 0;  // groups loaded here / ones other levels had
    bool loaded = false;  // every pool made and every texel read (own pools may be none)
  };
  // c3l v23 texture sharing between loaded levels (texshare.c3t): a group of textures the same
  // levels have, in a pool of its own while a loaded level holds it (m_share_lock)
  struct ShareTex {
    int handle = -1, mask = -1;
    u8 min_alpha = 0;
    u64 desc = 0;  // c3l::texture_share_desc
  };
  struct ShareGroup {
    u32 users = 0;
    std::vector<int> pools;  // pieces of at most kTexPoolPiece, as a level's
    bool loading = false;  // a level's load is making its pool (the others make their own copies)
    std::unordered_map<u64, ShareTex> tex;
  };
  std::mutex m_share_lock;
  bool m_share_read = false;
  std::vector<c3l::TexShareEntry> m_share_table;  // by key
  std::unordered_map<u32, ShareGroup> m_share_groups;
  int share_group_of(u64 key);  // -1: not shared (m_share_lock held)
  void release_share_groups(const std::vector<u32>& groups);
  struct Done {
    Job job;
    std::unique_ptr<CtrLevelData> lev;
    LoadResult result = LoadResult::FAILED;
    std::vector<std::pair<u32, int>> meshes;       // detail reload, repair: (chunk, mesh)
    std::vector<std::pair<u32, int>> merc_meshes;  // repair: (merc model, mesh)
    TexLoad tex;                                   // repair: the texture pool
    u32 heap_largest = 0;  // (AI-assisted) FAILED: the largest free heap block then (3DS)
    // (AI-assisted, streaming) a stream job's meshes, each with its time of day groups if asked
    struct StreamOut {
      u32 unit = 0;
      int mesh = -1;
      bool layout = false;
      std::vector<CtrLevelData::Tod::VisGroup> groups;
      std::vector<u16> run_group;  // per run of the chunk
      std::unique_ptr<u16[]> entries;  // the title's entry copy (CtrLevel.cpp tod_entries)
    };
    std::vector<StreamOut> stream_out;
    double ms = 0, read_ms = 0, decode_ms = 0;  // the job's time, its file reads', decoding
    u64 read_bytes = 0;
    u32 reads = 0;
    u32 preloads = 0, preload_misses = 0;  // (streaming) runs read in one request each, or not
    bool yielded = false;  // stopped early for an arrival's level load (the rest asked again)
    // (AI-assisted, j2intro) host, env CTR_LOAD_DELAY_MS: a level load handed over from then on
    double sim_ready_ms = 0;
  };
  void request(const Job& job);  // render thread
  void run_job(const Job& job);  // loader thread (or inline without one)
  void run_parts_job(const Job& job, Done* d);
#ifndef __3DS__
  void sim_loader_time(const Job& job, const Done& d, u64 f0);
#endif
  bool load_textures(c3l::Reader& f,
                     const c3l::Header& hdr,
                     const std::vector<c3l::Texture>& texs,
                     const std::vector<c3l::Draw>& draws,
                     const std::string& name,
                     TexLoad* out);
  // (AI-assisted) render thread: the results of a detail reload / repair
  void take_parts(Done& d, u64 frame);
  void delete_parts(Done& d);
  void delete_textures(TexLoad* t);
  void collect_chunks(const CtrLevelData& lev, bool missing_only, std::vector<DetailChunk>* out);
  void request_repair(CtrLevelData& lev);
  // (AI-assisted) render thread (or service): unload the parked prefetches the game gave up on;
  // room: free the deleted GPU memory now too (make_room). Returns whether a level was unloaded.
  bool housekeeping(bool room);
  // (AI-assisted) loader: an allocation failed. Once per job: has the render thread unload the
  // parked prefetches the game gave up on and free the deleted GPU memory now, waits for it, and
  // returns true (try the allocation again).
  // (AI-assisted, j2pop) bytes: what the allocation wants (distance freeing frees about that much,
  // the farthest first); a second try in the same job frees all it can
  bool make_room(const std::string& name, const char* what, u64 bytes = 0);
  int m_room_tries = 0;  // by the loader's job (one at a time)
  // (AI-assisted) the .c3l reader's windows and block caches, kept for all loads (one at a time):
  // made at startup, while the heap is in one piece
  c3l::Reader::Buffers m_read_bufs;
  // (AI-assisted) the last load_file's file reads (ms, count): for the "loaded" log line
  double m_last_read_ms = 0;
  u32 m_last_reads = 0;
  u64 m_last_read_bytes = 0;
  // under m_lock: the loader asks for room / the number of times the render thread made it
  bool m_room_wanted = false;
  u32 m_room_serial = 0;
  u64 m_room_bytes = 0;  // (AI-assisted, j2pop) make_room's want (0: everything far)
  bool can_start(const Job& job) const;  // under m_lock
  std::atomic<u32> m_generation{0};
  void loader_main();
  static void* loader_entry(void* self);
  // m_queue, m_done, m_loading, m_quit, m_levels_count, m_parked, m_resident, m_requested,
  // m_dgo_level, m_room_*
  std::mutex m_lock;
  std::condition_variable m_cv;
  std::deque<Job> m_queue;
  std::vector<Done> m_done;
  // (render thread) the buffer m_done gets next (process_pending_loads): room for kDoneRoom
  std::vector<Done> m_done_room;
  static constexpr size_t kDoneRoom = 16;
  std::string m_loading;  // the level the loader works on ("" if none)
  bool m_loading_prefetch = false;  // ... for a prefetch (cancelled when the game loads another)
  std::atomic<bool> m_cancel{false};  // m_loading is no longer wanted: stop early
  // a level load the game will wait for is queued: a running read back or stream job stops early
  std::atomic<bool> m_arrival_queued{false};
  bool yield_to_arrival(const Job& job) const {
    return (job.stream || job.dormant || job.detail) &&
           m_arrival_queued.load(std::memory_order_relaxed);
  }
  bool arrival_job(const Job& j) const;  // under m_lock
  bool m_quit = false;
  // (AI-assisted) the level whose DGO the overlord loads (the last prefetch): parked prefetches of
  // other levels are unloaded, the game gave up on them (Jak died while the next level loaded)
  std::string m_dgo_level;
  // (AI-assisted) levels ready() said yes to (when): the game reports them at the end of that
  // frame (set_levels), and may start the next level's DGO before. Kept until set_wanted, at most
  // a few seconds.
  std::map<std::string, double> m_promised;
  bool parked_matters(const std::string& name, double now) const;  // under m_lock
  // for the loader (can_start): m_levels.size(), the names in m_prefetched
  int m_levels_count = 0;
  std::set<std::string> m_parked;
  std::atomic<u64> m_render_frame{0};  // the render thread's frame (process_pending_loads)
  std::atomic<u64> m_drawn_frame{0};   // (AI-assisted, streaming) the last frame a level was drawn
  std::atomic<bool> m_game_black{false};  // the game's blackout (a continue or load): it waits
  std::atomic<bool> m_hold_black{false};  // the renderer's black hold (CtrRenderer::hold_black)
  std::set<std::string> m_resident;  // names in m_levels + m_prefetched (a prefetch skips them)
  void* m_thread = nullptr;
#ifndef __3DS__
  std::thread m_host_loader;  // (host, CTR_STREAM_SIM)
#endif
  std::set<std::string> m_requested;  // queued, loading or done but not taken (under m_lock)
  struct Prefetched {
    std::unique_ptr<CtrLevelData> lev;
    double since_ms = 0;  // when the load finished (ctr_gpu_time_ms)
  };
  std::map<std::string, Prefetched> m_prefetched;
  void update_level_count();

  void tod_sweep(u64 frame);
  bool m_tod_sweep_ok = true;
  TodSweepStats m_tod_sweep_stats;

  // ---- (AI-assisted, streaming) ----
  void stream_update(u64 frame);
  void run_stream_job(const Job& job, Done* d, c3l::Reader::Buffers* bufs = nullptr);
  // a level load's breaks (loader thread, while levels are drawn): the urgent stream jobs queued
  // meanwhile, read with buffers of their own instead of after the load
  void serve_streams_in_load();
  c3l::Reader::Buffers m_load_stream_bufs;
  bool m_load_stream_tried = false, m_load_stream_ok = false;
  u32 m_load_streams_served = 0;
  // (loader thread) the last "meshes not made" line per level and parts job kind
  std::unordered_map<std::string, double> m_parts_log_ms;
  static constexpr double kPartsLogMs = 10000;
  // the level load's start and the time of the stream jobs it served (kServeShare of it at most)
  double m_load_t0_ms = 0, m_load_served_ms = 0;
  static constexpr double kServeShare = 0.25;
  std::atomic<bool> m_stream_urgent_queued{false};
  void take_stream(Done& d, u64 frame);
  //! frees a unit; unless now, one drawn last frame first morphs out (freed by stream_update) or,
  //! without moving vertices, stays until it is out of view (stream_held): false
  bool stream_evict(CtrLevelData& lev, u32 unit, bool now = false);
  //! drawn within the last frames with no morph to leave by: not freed unless memory runs out
  static bool stream_held(const CtrLevelData& lev, const CtrLevelData::StreamUnit& su, u64 frame);
  void stream_idle_fill(u64 frame);
  void stream_take_pending(u64 frame);
  struct StreamTake {
    std::string name;
    u32 generation;
    Done::StreamOut out;
  };
  std::vector<StreamTake> m_take;  // read, not taken yet (colored first, within a budget)
  void stream_late_end(CtrLevelData::StreamUnit& su, double end_ms, u64 end_frame);
  // the units in the arena's growth segments freed (all: a level load needs the memory; else the
  // ones not needed now); returns how many
  // (AI-assisted, streammem) whole: every unit; now false: a unit drawn leaves by its morph out
  // or stays while drawn
  u32 stream_shrink(bool all, bool whole = false, bool now = true);
  static void free_stream_parts(const Job& job);
  u32 m_stream_inflight = 0;  // stream jobs queued, running or not taken (kJobsInFlight)
  // (AI-assisted, streammem) under m_lock: the room_bytes of the parts jobs queued, running or done
  // but not taken (by key); the stream jobs the loader ran in a row while another job waited
  std::map<std::string, u64> m_parts_room;
  u32 m_stream_streak = 0;
  // (AI-assisted, streammem) housekeeping(room): the queued stream jobs and the meshes read but not
  // taken that hold arena pieces (in its growth; all: whole) given back; returns their bytes
  u64 stream_drop_pending(bool all);
  // (AI-assisted, streammem) a make_room of a level load's second try: the arena's first segment too
  bool m_room_whole = false;
  bool m_job_level_load = false;  // (loader thread) run_job: the job is a level's load
  // the bytes the repair of incomplete levels waits for (kept free above the arena's floor)
  u32 m_stream_repair_room = 0;
  u32 m_stream_serial = 0;
  std::vector<Done> m_load_delayed;    // (AI-assisted, j2intro) host, CTR_LOAD_DELAY_MS: the same
  // the camera: where it was last drawn from, its velocity (game units per ms, smoothed)
  float m_cam[3] = {0, 0, 0}, m_cam_vel[3] = {0, 0, 0};
  float m_cam_rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};  // (AI-assisted) rows, see camera()
  // Residency by need (Jak II): where the camera is and will be, Jak and a continue's camera
  // (game units; n of them, 0: not known), for the render thread (need_positions) and a copy the
  // loader reads (m_lock: m_need_snap)
  static constexpr int kNeedPos = 7;
  int need_positions(float pos[kNeedPos][3]) const;
  float m_cont_place[2][3] = {};  // note_continue_place
  int m_cont_place_n = 0;
  int m_need_snap_cont = 0;  // of m_need_snap, the last ones: the continue's place (m_lock)
  int m_cont_want = 0;       // the levels the continue wants (m_lock)
  u32 m_tex_trim_left = 0;  // take_tex_trim: the calls still to say yes, the last one's frame
  u64 m_tex_trim_frame = 0;
  // a managed chunk and how far the nearest position is beyond its reach (game units; < 0: within)
  struct NeedCand {
    CtrLevelData* lev;
    u32 chunk;
    u32 bytes;
    float e;
  };
  // have: the resident ones nothing draws now; want: the absent ones
  void need_scan(const float pos[kNeedPos][3], int np, u64 frame);
  void need_free(CtrLevelData& lev, u32 ci);
  // a load out of memory: the resident ones beyond their reach, the farthest first, until want
  // bytes are freed (0: all of them); returns the bytes freed
  u64 need_make_room(u64 want);
  std::vector<NeedCand> m_need_have, m_need_want;
  std::vector<u32> m_need_unmet;
  float m_need_snap[kNeedPos][3] = {};
  int m_need_snap_n = 0;
  u64 m_need_frame = 0;
  // the level load in flight: the linear memory it still takes (loader thread; 0: not known)
  std::atomic<u64> m_load_left{0};
  // what a level load, a repair or the stream arena still lack after the last pass's freeing
  u64 m_need_deficit = 0;
  // the stream arena's segment a near chunk needed now had no linear memory for, and its frame
  u64 m_stream_claim = 0, m_stream_claim_frame = 0;
  // the passes in a row short of room, the last shortage line
  u32 m_need_short = 0;
  double m_need_log_ms = 0;
  // Jak 1 title screen (CtrLevel.cpp "Title screen"): the title level is wanted (set_wanted; the
  // loader reads it), the camera left the attract path this title, the per-frame residency update
  std::atomic<bool> m_title_on{false};
  bool m_title_off_path = false;
  void title_update(u64 frame);
  // residency by need per stats period (stream_log); resident / managed: the last pass's bytes
  struct DistStats {
    u64 freed_chunks = 0, freed_bytes = 0, load_bytes = 0, read_jobs = 0, read_chunks = 0,
        read_bytes = 0, fill_bytes = 0, short_max = 0, short_passes = 0;
    u64 resident = 0, managed = 0;
  } m_dst;
  math::Vector4f m_planes[4];  // the view's planes (as CtrBackgroundCamera::planes)
  u64 m_cut_frame = 0;         // the last camera cut (a jump of > kCutMeters, or of the factor)
  u64 m_lv_frame = 0;  // (AI-assisted, j2intro) note_late_in_view
  ctr_pop::Stats m_pop;
  u32 m_lv_count = 0;
  u32 m_lv_ring0 = 0;
  bool m_lv_recent = false;
  u64 m_idle_frame = 0;        // the last idle fill job
  // Jak: camera space (stream_note_target, its frame), then world (stream_note_camera)
  float m_target_cam[3] = {0, 0, 0}, m_target[3] = {0, 0, 0};
  u64 m_target_cam_frame = 0, m_target_frame = 0;
  u64 m_cam_frame = 0;
  bool m_cam_valid = false;
  // (AI-assisted, title3) the game's camera (note_game_camera): this frame's, the last one used
  // and its velocity (game units per frame, smoothed); the tie LOD factor of the last level drawn
  float m_game_cam_next[3] = {0, 0, 0}, m_game_cam[3] = {0, 0, 0}, m_game_vel[3] = {0, 0, 0};
  bool m_game_cam_new = false, m_game_cam_valid = false;
  float m_cont_cam[3] = {0, 0, 0};  // note_continue_camera
  bool m_cont_valid = false;
  float m_k_last = 0.f, m_k_any = 0.f;
  // scheduling (kept to avoid allocations)
  struct StreamCand {
    CtrLevelData* lev;
    u32 unit;
    float score;
  };
  std::vector<StreamCand> m_want, m_have, m_idle;
  std::vector<StreamCand> m_have_prio;  // (AI-assisted, popin2) m_have by priority, the least first
  StreamCheck m_scheck;
  struct StreamStats {
    u64 frames = 0, jobs = 0, units = 0, bytes = 0, read_bytes = 0, reads = 0, evictions = 0;
    u64 no_room = 0, late_events = 0, late_frames_total = 0;
    double read_ms = 0, decode_ms = 0, job_ms = 0, job_max_ms = 0, late_ms_total = 0,
           late_ms_max = 0;
    double sched_ms = 0, sched_max_ms = 0;
    u64 need_max = 0, need_sum = 0;  // bytes of the units needed now (score < 0) per frame
    u64 neighbours = 0;  // units read along with their file neighbours before they were wanted
    u64 late_periods = 0, late_frames_max = 0;
    u64 shrunk = 0;  // units freed to give growth segments back
    u64 prio_evictions = 0;  // (AI-assisted, popin2) units freed for a needed one of more priority
    u64 bulk_evictions = 0;  // (AI-assisted, streammem) units freed in bulk (far beyond their need)
    u64 packed = 0;  // the file bytes of the units read (their blocks)
    u64 late_loading = 0, late_absent = 0;
    u64 late_unwanted = 0;  // late, never wanted before (the need not foreseen)
    // late times split: starting within 2 s of a camera cut, or not (speed, bandwidth, room)
    u64 late_cut = 0, late_other = 0;
    double late_cut_ms = 0, late_other_ms = 0, late_cut_max = 0, late_other_max = 0;
    u64 held_group_frames = 0, held_frames = 0, last_held_frame = 0;  // far kept in view
    u64 idle_jobs = 0, idle_units = 0;
    u64 colored = 0;  // near chunks colored as they were taken
    u64 late_wanted_frames = 0;  // late after being wanted: the frames between
    u64 preloads = 0, preload_misses = 0;
    u64 late_group_frames = 0, late_frames = 0, last_late_frame = 0;
    // late times during which the arena had no room for the unit (capacity, not bandwidth)
    u64 late_room = 0;
    double late_room_ms = 0, late_room_max = 0;
    // the linear heap's free bytes at the frame's start: least, least while a level loaded
    u64 linear_min = ~0ull, linear_min_load = ~0ull;
    double since_ms = 0;  // the window's start
    // ring 0 (ctr_detail::kNearRingMeters): the bytes of its units per frame, units freed for
    // them, in-view group frames there drawn far for want of their data
    u64 ring0_sum = 0, ring0_max = 0, ring0_evictions = 0, ring0_late = 0;
    u64 burst_frames = 0;
  } m_sst;
};

/*! The camera the game sends to the background renderers (GoalBackgroundCameraData). */
struct CtrBackgroundCamera {
  math::Vector4f planes[4];
  math::Vector<s32, 4> itimes[4];
  math::Vector4f camera[4];
  math::Vector4f hvdf_off;
  math::Vector4f fog;
  math::Vector4f trans;
  math::Vector4f rot[4];
  math::Vector4f perspective[4];
};
static_assert(sizeof(CtrBackgroundCamera) == 16 * 23);

/*! TFRAG_LEVEL0/1 buckets: draws the whole level background (tfrag + tie) from the .c3l. */
// host A/B, env CTR_ARRIVAL_OFF: the arrival changes off (load order, faster reading, holds)
bool ctr_arrival_off();
#ifndef __3DS__
// host, pad script "set cam_path ...": the levels drawn from a camera on a path (CtrLevel.cpp)
bool ctr_level_host_cam_path(const char* spec);
// host, CTR_STREAM_SIM: the loader thread's jobs and the render thread's frames one at a time
// (the soft GPU backend is not thread safe)
extern std::mutex g_host_gpu_mutex;
#endif
/*! (AI-assisted, j2intro; as Jak 1's title3) render thread, before the frame's level draws (Jak II):
 * the screen is black this frame (the game's blackout, or the renderer holding it). Nothing is
 * kept hidden or far "in view" for late data then (nobody saw it missing): dormant chunks read
 * back and near chunks that came are drawn at once (CtrTfragRenderer::draw_level). */
void ctr_level_set_screen_black(bool black);
/*! (AI-assisted) render thread: the time of day blend / rewrite ms since the last call. */
double ctr_level_tod_take_ms();

class CtrTfragRenderer : public CtrBucketRenderer {
 public:
  CtrTfragRenderer(std::string name, int id, CtrLevels* levels);
  ~CtrTfragRenderer() override;
  void render(DmaFollower& dma, CtrRenderState& rs) override;
  /*! (AI-assisted) Jak II: the background buckets of level slot `slot` (tfrag, tie, shrub of any
   * kind): the first one in a frame that has the level's camera (TfragPcPortData) draws all of the
   * level's .c3l. */
  void render_jak2(DmaFollower& dma, CtrRenderState& rs, int slot);
  /*! (AI-assisted, j2water) Jak II: the slot's water buckets: the level's blended and ordered
   * draws of this frame, held back by draw_level (after the characters) */
  void draw_jak2_late(const CtrRenderState& rs);
  /*! (AI-assisted, j2vis) Jak II: the slot's tie and shrub buckets (tie-lN-tfrag, shrub-lN-shrub),
   * where the game sends the names of the prototypes it hides (c3l v20 prototype visibility). */
  void set_jak2_proto_buckets(int tie_bucket, int shrub_bucket) {
    m_proto_bucket[0] = tie_bucket;
    m_proto_bucket[1] = shrub_bucket;
  }

 private:
  // (AI-assisted, j2vis) the game's hidden prototype names this frame (the packet after the
  // TfragPcPortData one in the slot's tie / shrub bucket) applied to the level's prototype runs
  void proto_vis_scan(CtrLevelData& lev, const CtrRenderState& rs);
  int m_proto_bucket[2] = {-1, -1};
  u64 m_proto_tris = 0, m_proto_frames = 0;  // statistics: triangles hidden, level draws
  void draw_level(CtrLevelData& lev, const CtrBackgroundCamera& cam, const CtrRenderState& rs);
  void flush_calls(int pass);
  void log_stats(const CtrRenderState& rs, CtrLevelData* lev);  // (AI-assisted) the log lines
  int m_jak2_slot = -1;  // (AI-assisted) Jak II: the level slot being drawn (-1: Jak 1)
  // (AI-assisted, j2cull) draw_level's phases (ticks summed since the last log line): setup, the
  // tie LOD groups (and near chunks), the chunk loop (view / visibility / run tests), time of day,
  // the draw lists (the walk and the calls made), the backend (flush_calls); counts: level draws,
  // chunks emitted, runs tested, draws walked / drawn
  enum { kPhSetup, kPhLod, kPhChunks, kPhTod, kPhLists, kPhFlush, kPhN };
  u64 m_ph[kPhN] = {};
  u64 m_ph_draws = 0, m_ph_emits = 0, m_ph_runs = 0, m_ph_walked = 0, m_ph_calls = 0;
  u64 m_ph_skipped = 0, m_ph_lod_skipped = 0;  // chunks / tie LOD groups their cluster stood for
  u64 m_back_pending = 0;  // backend ticks not yet moved to kPhFlush
  u32 m_late_in_view = 0;  // (AI-assisted, j2intro) draw_level: missing in view (note_late_in_view)
  // (AI-assisted, gpu-rt) a pass's draws (flush_calls)
  std::vector<ctr_mesh_call> m_calls;
  // (AI-assisted, j2water) Jak II: the blended draws held back (draw_jak2_late), their frame and fog
  std::vector<ctr_mesh_call> m_late_calls;
  bool m_late_collect = false;  // host: submit collects them instead of drawing
  u64 m_late_frame = ~0ull;
  float m_late_fog0[4] = {0, 0, 0, 0}, m_late_fog1[4] = {0, 0, 0, 0};
  u8 m_late_fog_rgb[3] = {0, 0, 0};
  // (AI-assisted, j2water2) c3l v21 envmapped ties: their draws of this frame (the opaque ones
  // after the opaque pass, the blended ones after draw_jak2_late's), the time of day's env color
  // (the tie bucket's pc-add-tie-envmap-info, GS units: 0x80 = 1), statistics
  struct EnvCall {
    const ctr_draw_state* state;
    int env_tex, mesh, normals_mesh, normals_offset, first, count;
    ctr_etie_draw d;
  };
  std::vector<EnvCall> m_env_calls, m_env_late;
  float m_env_color[3] = {128.f, 128.f, 128.f};
  u64 m_env_draws = 0, m_env_tris = 0, m_env_frames = 0;
  // the shine fade of the level being drawn's tie nodes (CtrShineTable::node_first; 0xff: not
  // asked for yet), and the triangles of envmapped draws beyond their shine (statistics)
  std::vector<u8> m_shine_fade;
  u64 m_env_plain_tris = 0;
  void env_color_scan(const CtrLevelData& lev, const CtrRenderState& rs);
  // Jak 1's wind (Tie3::WindWork, the tie bucket's first PC_PORT transfer after the level's data)
  struct WindWork {
    u32 paused;
    u32 pad[3];
    float wind_array[64][4];
    float wind_normal[4];
    float wind_temp[4];
    float wind_force[64];
    u32 wind_time;
    u32 pad2[3];
  };
  WindWork m_wind_work{};
  bool m_wind_seen = false;
  void wind_update(CtrLevelData& lev, u64 frame);
  // the wind tie draws: opaque ones after the opaque pass, the others in their place; their rows
  // (c3l::ExtWind slots: kx, kz, cx, cz) per visible chunk in m_wind_rows from row_first
  struct WindCall {
    const ctr_draw_state* state;
    const ctr_mesh_matrix* matrix;
    u32 row_first, row_count, rows_id;
    int mesh, slots_offset, first, count, nofog;
  };
  std::vector<WindCall> m_wind_calls;
  std::vector<float> m_wind_rows;
  std::vector<s32> m_wind_row_of;  // per visible chunk (m_visible index): its rows, -1 none yet
  std::vector<u32> m_wind_row_id;
  u64 m_wind_draws = 0, m_wind_tris = 0;
  // the tie geomorph's rows (c3l::ExtMorph: f at 4 * row) per visible chunk with moving vertices
  // (m_visible index: its first row in m_morph_rows, -1 none) and their ids
  std::vector<float> m_morph_rows;
  std::vector<s32> m_morph_row_of;
  std::vector<u32> m_morph_row_id;
  void draw_wind_calls(const CtrLevelData& lev);
  void draw_env_calls(std::vector<EnvCall>& calls);
  CtrLevels* m_levels;
  int m_far_levels = 0;  // level draws in the "seen from another level" mode (statistics)
  int m_level_draws = 0;
  // (AI-assisted) the game's visibility string for the level being drawn (nullptr: none), and
  // what it hid (statistics)
  const u8* m_vis = nullptr;
  u64 m_vis_chunks = 0, m_vis_tris = 0, m_vis_frames = 0, m_vis_own_frames = 0;
  // (AI-assisted) run_culling: per run of the level being drawn, shown this frame (filled for the
  // chunks tested run by run); statistics: runs tested, triangles and chunks the view test hid
  std::vector<u8> m_run_shown;
  // (AI-assisted) c3l v17 tie levels of detail: per run of the level being drawn, the other version
  // of its group (hidden, and never drawn along with neighbouring runs); statistics: groups at
  // their near version, triangles drawn of near / far versions (over m_lod_frames level draws)
  std::vector<u8> m_run_nomerge;
  // per run of the level being drawn: a shrub run's fade (0x80: opaque), read for faded chunks
  std::vector<u8> m_run_fade;
  // the faded shrub runs' draws (blend FIX with their fade), after the opaque level draws
  std::vector<ctr_mesh_call> m_fade_calls;
  std::deque<ctr_draw_state> m_fade_states;
  u64 m_fade_runs = 0;
  u64 m_lod_groups_near = 0, m_lod_frames = 0, m_lod_tris[2] = {0, 0};
  u64 m_cull_runs = 0, m_cull_tris = 0, m_cull_chunks = 0, m_cull_frames = 0;
  // kDrawCullCond draws of visible chunks, and of them culled (camera inside)
  u64 m_cond_draws = 0, m_cond_culled = 0;
  // culled draws drawn both sides across the camera's plane (sums over level draws)
  u64 m_near_uncull = 0, m_near_uncull_tris = 0, m_near_uncull_frames = 0;
  // (AI-assisted) lowres_vista / neighbour_vis: low resolution chunks drawn in near mode, and the
  // vista pass's chunks and triangles (statistics); the levels' detailed areas (per level draw)
  u64 m_lowres_near = 0, m_vista_chunks = 0, m_vista_tris = 0, m_vista_frames = 0;
  // (AI-assisted) c3l v16 time of day: blends, vertices recolored and their time (ms) since the
  // last log line, over m_tod_frames level draws; the most vertices in one draw
  u64 m_tod_frames = 0, m_tod_blends = 0, m_tod_verts = 0, m_tod_max = 0;
  u64 m_tod_light_verts = 0;  // (AI-assisted, perfv2) of m_tod_verts: the light pass's
  u64 m_tod_deferred = 0;     // (AI-assisted, perfv2) group rewrites spread to a later frame
  u64 m_tod_unchanged = 0;    // groups behind whose colors all blended as before
  u64 m_tod_black_late = 0;   // group rewrites left to a later frame while the screen is black
  // (AI-assisted) groups rewritten, chunks drawn behind; sampled level draws, the most day moves a
  // group drawn was behind, draws with one 2 or more behind
  u64 m_tod_spread = 0, m_tod_fresh = 0;
  // (AI-assisted, fable1) heavy frames (the last level draw's vertices drawn >= 3x the table):
  // every second one takes no new weights (draw_level); the count for the statistics
  bool m_tod_heavy = false;
  u64 m_tod_skipped = 0;
  double m_tod_ms = 0, m_tod_max_ms = 0;
  std::vector<const CtrLevelData*> m_loaded;
  // (AI-assisted, titlemove) per chunk of the level drawn: a shrub chunk's distance limit (game
  // units, its runs' limit)
  std::vector<float> m_shrub_lim;
  // per chunk, this draw: a tie chunk's fade in its limit's band, its vista copy's
  std::vector<u8> m_tie_fade, m_copy_fade;
  std::vector<u8> m_pop_why;  // ctr_pop: per chunk, this draw: why its distance decision changed
  std::vector<u32> m_pop_kept;  // ... the groups in view kept from their distance's version (bit 31: tfrag)
  u64 m_shrub_run_hid = 0;  // (statistics) shrub runs hidden by their distance
  // (AI-assisted) vista tier: per chunk, its vista copies are drawn this frame (it isn't, because
  // of its distance)
  std::vector<u8> m_vista_src;
  // this frame's visible chunks and draws (kept to avoid allocations)
  struct VisibleChunk {
    // (AI-assisted, micro-level) not zeroed (emplace_back() zeroed all 220 bytes): draw_level sets
    // every field, the backend makes gpu.f24's fog rows before it reads them (gpu.fog_gen 0)
    VisibleChunk() {}
    u32 chunk;
    u8 band;    // (AI-assisted) distance band for the early depth order: 0 near .. 2 far
    bool partly;  // (AI-assisted) some of its visibility runs are hidden: draw run by run
    bool runs_valid;  // (AI-assisted) m_run_shown holds its runs' flags (time of day)
    bool faded;   // shrub runs in their fade zone (m_run_fade)
    bool vista;   // (AI-assisted) far scenery: drawn first, with the vista fog (lowres_vista)
    bool nofog;   // (AI-assisted) in front of where both fogs start (fog_stage_skip)
    bool tmorph;  // the tfrag geomorph: near or mid versions shown (the morph program)
    float floor;  // CtrLevelData::chunk_floor
    float clip[16];
    ctr_mesh_matrix gpu;  // clip, converted for the GPU once per frame
  };
  std::vector<VisibleChunk> m_visible;
  // the tfrag geomorph (c3l::ExtTfrag): this level draw's bands in clip w (f0 = clamp(w * x + z),
  // f1 = clamp(w * y + w)); per visible chunk its morph uniforms (the bands, delta_scale) and their
  // id; the groups near / mid / far (statistics)
  float m_tf_band[4] = {0, 0, 0, 0};
  // the clip w row (world, from the camera) and the w up to which no vertex moves (band 1's start)
  float m_tf_wv[4] = {0, 0, 0, 0};
  float m_tf_still_w = -1.f;
  // a tfrag geomorph draw's range: its runs (index, count, moves), the hidden ones after it, the
  // stretches drawn
  struct TfPiece {
    u32 idx, n;
    bool mv;
  };
  std::vector<TfPiece> m_tf_pcs, m_tf_gap, m_tf_seg;
  std::vector<float> m_tf_unif;
  std::vector<u32> m_tf_unif_id;
  u64 m_tf_groups[3] = {0, 0, 0};
  u64 m_tf_tris[2] = {0, 0};
  u64 m_tf_colors_written = 0, m_tf_frames = 0;
  u64 m_morph_prog_tris[2] = {0, 0};  // triangles drawn by the tie / tfrag morph programs
  std::vector<int> m_visible_slot;  // per chunk: index in m_visible, -1: not drawn
  // (AI-assisted) with vista chunks: the visible draws of early_draws, sorted_draws and
  // ordered_draws split in one walk, in list order: the vista pass's, and the main pass's lists
  std::vector<u32> m_vista_draws, m_main_draws[3];
  // (AI-assisted) the early draws by distance band, nearest first
  std::vector<u32> m_band_draws[3];
  // (AI-assisted, j2cull) the visible draws' places per list (bit sets), and those draws in order
  std::vector<u32> m_list_bits[3], m_list_vis[3];
  std::vector<u32> m_morph_later;  // a list's moving tie geomorph draws, drawn after its others
  std::vector<u32> m_tmorph_later;  // and its tfrag geomorph draws (their moving runs), before those
};
