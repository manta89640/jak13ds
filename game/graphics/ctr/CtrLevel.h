#pragma once

/*!
 * @file CtrLevel.h
 * (AI-assisted)
 * Level backgrounds for the 3DS renderer: loads .c3l files (docs/3ds-port/c3l_format.md) from
 * <project>/out/jak1/c3l and draws them from the tfrag buckets with the camera the game sends.
 */

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "common/common_types.h"
#include "common/math/Vector.h"

#include "common/dma/gs.h"
#include "common/util/FileUtil.h"

#include "game/graphics/ctr/CtrFineVis.h"
#include "game/graphics/ctr/CtrRenderer.h"
#include "game/graphics/ctr/CtrKeiraRig.h"
#include "game/graphics/ctr/c3l_format.h"
#include "game/graphics/ctr/c3l_reader.h"
#include "game/graphics/ctr/c3v_format.h"
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
};

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
    return std::max(std::max(lod_error[g], lod_groups[g].dist * lod_floor_k), lod_min_e);
  }
  // (AI-assisted, titlemove) the title screen's least switch distance, in the units of
  // lod_switch_error (distance / factor; 0 elsewhere)
  float lod_min_e = 0.f;
  std::vector<int> textures;  // ctr_gpu handles
  std::vector<u8> tex_min_alpha;  // (AI-assisted) smallest texel alpha per texture (0xff = 1.0)
  u32 tex_empty = 0;  // (AI-assisted) c3l v15: textures the file has no texels for (in atlases)
  int tex_pool = -1;          // ctr_gpu texture pool holding all of them (VRAM when there's room)
  u32 tex_pool_bytes = 0;     // (AI-assisted) its size (0: no textures)
  u32 tex_background_bytes = 0;  // (AI-assisted) its start: the background's textures (VRAM first)
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
  // (CtrLevels::unload_detail / request_detail). Until then it is drawn in far mode.
  std::string path;  // the .c3l file
  u32 vertex_data_offset = 0, index_data_offset = 0;
  u32 merc_vertex_offset = 0, merc_index_offset = 0;  // (AI-assisted) for incomplete loads
  std::vector<u32> mesh_index_first, mesh_index_end;  // per chunk: its index range in the file
  bool detail_loaded = true;      // the meshes of all chunks are there
  bool detail_requested = false;  // their reload is queued
  u32 far_frames = 0;             // frames in a row beyond the unload distance
  u64 detail_retry_frame = 0;     // after a failed reload (no memory): not before this frame
  // (AI-assisted) the load ran out of linear memory even after making room: some chunk or merc
  // meshes (or the texture pool) are missing. They are made again from the file once the memory
  // is there (CtrLevels::process_pending_loads, request_repair), while the level is drawn.
  bool incomplete = false;
  bool repair_requested = false;
  u64 repair_retry_frame = 0;  // after a repair that left parts out: not before this frame
  u8 repair_failures = 0;      // repairs in a row that made nothing (longer waits)
  // (AI-assisted) last frame's decisions, for hysteresis (CtrTfragRenderer::draw_level): per chunk
  // bits 0-1 detailed/coarse (1 near, 2 far, 0 not decided yet), bit 2 within its draw distance
  std::vector<u8> chunk_state;
  // (AI-assisted, title-popin) the auto_detail position (ctr_level_detail x kDetailQ) each chunk's
  // distance and each tie LOD group's version were last decided with (0xff: none yet). A chunk or
  // group in view keeps deciding with it, so a change of the auto_detail distances never makes
  // something on screen appear, vanish or change its version; it takes the new distances when it
  // is out of view, or when the old and the new decide alike.
  std::vector<u8> chunk_detail_q;
  std::vector<u8> lod_detail_q;
  // (AI-assisted, title-popin) per chunk of tiers 1/2: a sphere around its lod_center holding both
  // versions of the cell (the same for both: their in-view tests agree), x y z r
  std::vector<float> lod_cell_sphere;
  bool was_far = false;
  // (AI-assisted, j2-mem) Jak II: a level the game's visibility string has hidden whole for a while
  // (dormant_levels; CtrLevel.cpp "Dormant levels")
  bool dormant = false;
  u32 hidden_frames = 0;        // level draws in a row with its string all zero
  // (AI-assisted, j2intro) the frame its string showed it again after it was dormant (a reveal: the
  // title intro's first frame); CtrLevels::note_late_in_view
  u64 shown_frame = 0;
  u32 vis_scan_bytes = UINT32_MAX;  // the string's bytes its ids are in (UINT32_MAX: not known yet)
  // per chunk (made at its first dormancy): kDormFreed its mesh freed for that, kDormReading being
  // read back; and the frame a chunk without its mesh began to be kept hidden in view (0: not)
  std::vector<u8> chunk_dormant;
  std::vector<u32> chunk_hold_since;
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
  bool cull_made = false;
  size_t cull_sizes[3] = {0, 0, 0};  // chunks, tie LOD groups, chunk_unit when made
  bool dormant_requested = false;  // a read of its freed chunks is queued
  u64 dormant_retry_frame = 0;     // after a failed one: not before this frame
  static constexpr u8 kDormFreed = 1, kDormReading = 2;
  // (AI-assisted, j2strip) Jak II, linear memory short (CtrLevel.cpp "Distance freeing"): a shrub
  // or tie chunk with a draw distance, its mesh left out (or freed) while the camera is far beyond
  // that distance, read back by distance (being read)
  static constexpr u8 kDistFreed = 3, kDistReading = 4;
  bool dist_free = false;  // its load ran short of linear memory, or a load made room: on
  bool dormant_freed(size_t ci) const {
    return ci < chunk_dormant.size() && chunk_dormant[ci] != 0;
  }
  u32 generation = 0;             // tells loads of the same level apart
  // (AI-assisted) config fine_vis: the level's .c3v sidecar (c3v_format.h, CtrFineVis.h), opened
  // at load when it matches the .c3l (nullptr: none, or another conversion's); its blocks are read
  // around the camera as it moves
  std::shared_ptr<CtrFineVis> fine;
  // (AI-assisted) occ_cull: per run, hidden by the occlusion buffer last frame (tested every frame;
  // the others every 4th)
  std::vector<u8> occ_hidden;
  float occ_ema = 1e9f;  // triangles it hid per level draw (moving average; start: run it)
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
    std::vector<u8> colors;      // color_count x palette_count x rgb (GS units)
    std::vector<c3l::TodMul> muls;
    // per entry (color_count colors, then the TodMuls; Vertex::pad): the palettes (bit p) its
    // color isn't black in, its rgb (r | g << 8 | b << 16) and the version that rgb is of (current
    // when at least lut[mask]: the last move of its palettes)
    std::vector<u8> color_mask;
    std::vector<u32> table, stamp;
    // "light" palettes: used by few colors (the lamps, the lava glow: the game moves their weights
    // every frame); per chunk its vertices with a light color (by the light palettes they use),
    // rewritten alone when only those move
    u32 light_mask = 0;  // palette bits
    u32 light_color_count = 0;
    struct LightList {
      std::vector<u16> verts;  // by the light palettes of their color, then by vertex
      std::vector<std::pair<u8, u32>> groups;  // those palettes, end of their verts
    };
    std::unordered_map<u32, LightList> chunk_lights;
    // weights (64 = 1.0): the game's at the level's last draw, the table's
    u8 weights[8][3] = {};
    u8 target_w[8][3] = {};
    bool blended_valid = false;  // weights taken at least once
    // (AI-assisted, citadel) config tod_pulse_step: per palette the direction of the game's last
    // move (1 up, 2 down), the frames of its last two reversals and of its last move, and the
    // level draws a pulsing palette was held (statistics)
    u8 pulse_dir[8] = {};
    u64 pulse_rev[8][2] = {};
    u64 pulse_move[8] = {};
    u32 pulse_held = 0;
    // the palettes found pulsing in an earlier load of the level (TodSeen) and the frame of the
    // level's first draw: those count as pulsing from the start (until they don't turn around for
    // as long as a pulse is recognised by)
    u8 pulse_known = 0;
    u64 first_frame = 0;
    // versions: + 1 per change of target_w (dver: of its day palettes); per palette its last;
    // lut[mask]: the last of the palettes in mask
    u32 ver = 0, dver = 0;
    u32 pal_ver[8] = {};
    u32 lut[256] = {};
    // the palettes a blend adds (weight not 0): their slots in colors and weights
    struct Act {
      u32 n = 0;
      u8 slot3[8] = {};
      u8 w[8][3] = {};
    };
    Act act;
    // per chunk: its vertex groups (VisGroup: one per visibility id of its runs when their vertex
    // ranges don't overlap, c3l v18 files, in vertex order; else one for all), each with the
    // version its colors are (0: the file's) and the day move count then; the oldest of those,
    // the version its light vertices are (quick outs); the palettes its vertices use (scanned: 0
    // not yet (no mesh at load), 1 no time of day color, 2 some) and its mesh (a mesh made again has
    // the file's colors)
    struct VisGroup {
      u16 lo, hi;  // vertices [lo, hi]
      u32 ver : 31;
      u32 plain : 1;  // every vertex has a time of day color
      u32 dver;
    };
    std::vector<VisGroup> groups;
    std::vector<u32> chunk_group_first;
    std::vector<u16> chunk_group_count;
    std::vector<u16> run_group;  // per run (CtrLevelData::vis_runs): its group in the chunk
    std::vector<u32> chunk_min_ver, chunk_min_dver, chunk_light_ver;
    std::vector<u8> chunk_mask, chunk_scanned;
    std::vector<int> chunk_mesh;
    u32 bytes() const {
      u32 lights = 0;
      for (const auto& [ci, l] : chunk_lights) {
        lights += (u32)(l.verts.size() * 2 + l.groups.size() * 8) + 48;
      }
      return (u32)(colors.size() + muls.size() * sizeof(c3l::TodMul) + color_mask.size() +
                   table.size() * 8 +
                   groups.size() * sizeof(VisGroup) + run_group.size() * 2 +
                   chunk_group_first.size() * 24) +
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
  };
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
  // shine), per draw its index there (-1: none; empty: no such draws), per chunk the byte offset of
  // its normals in normals_mesh (-1: none), the mesh holding all of them (ctr_gpu, -1: none)
  std::vector<c3l::EnvTieDraw> env_draws;
  std::vector<s16> draw_env;
  std::vector<s32> chunk_normals;
  int normals_mesh = -1;
};

/*! (AI-assisted) host --ctr-gfx with env CTR_FINE_VIS_CHECK: the level pieces fine_vis hid this
 * frame are drawn again against the finished frame's depth, counting their pixels that would have
 * been seen (a culling error), and logged (log_now: the totals). Before ctr_gpu_frame_end. */
void ctr_level_fine_vis_check(u64 frame, bool log_now);

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
  /*! (AI-assisted, j2-mem) Jak II, render thread, each frame: the dormant levels' chunks freed
   * or read back by the camera's distance (CtrLevel.cpp "Dormant levels") */
  void dormant_update(u64 frame);
  /*! (AI-assisted, j2strip) Jak II, render thread, each frame: the levels short of linear memory
   * (dist_free): their freed chunks read back by distance (CtrLevel.cpp "Distance freeing"; they
   * are freed only by a load that would fail otherwise: make_room) */
  void dist_update(u64 frame, u32 free_now);
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
  /*! (AI-assisted, j2intro; Jak 1's title3 has the same) draw_level (Jak II): what of a level is
   * missing in view this frame: tie LOD groups drawn far because their near chunks aren't there
   * yet, and dormant tie chunks not read back yet (not drawn at all). recent: the level was shown
   * again (its dormancy ended) within the last 3 s. The renderer keeps a screen that comes out of
   * black black while there is such a thing after a level was shown or the camera cut
   * (CtrRenderer::draw_pmode_fade), at most 1.5 s (Jak II: while it gets fewer, at most 6 s). */
  void note_late_in_view(u64 frame, u32 n, bool recent) {
    if (m_lv_frame != frame) {
      m_lv_frame = frame;
      m_lv_count = 0;
      m_lv_recent = false;
    }
    m_lv_count += n;
    m_lv_recent = m_lv_recent || recent;
  }
  //! this frame's count (note_late_in_view), and whether a level was shown or the camera cut
  //! within the last 3 s
  u32 late_in_view(u64 frame, bool* shown_or_cut) const {
    const bool now = m_lv_frame == frame;
    *shown_or_cut = (now && m_lv_recent) || (m_cut_frame && frame < m_cut_frame + 90);
    return now ? m_lv_count : 0;
  }
  /*! (AI-assisted, streaming) the streaming statistics, logged (render thread) */
  void stream_log();
  /*! (AI-assisted, j2-mem) the levels' memory, the free linear memory and heap (render thread) */
  void log_memory();
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
  /*! (AI-assisted, streaming) a unit needed (draw_level): late statistics */
  void stream_late(CtrLevelData& lev, u32 unit, u64 frame);
  /*! (AI-assisted, streaming) a level drawn: its groups drawn far because their data was late */
  void stream_note_level(CtrLevelData& lev, u32 late_groups, u32 held_groups, u64 frame);
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
    bool dormant = false;  // (AI-assisted, j2-mem) with detail: a dormant level's chunks read back
    bool dist = false;  // (AI-assisted, j2strip) with dormant: chunks freed by distance read back
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
    int pool = -1;
    u32 pool_bytes = 0;
    u32 background_bytes = 0;  // the pool's start: the background's textures
    std::vector<int> handles;
    std::vector<u8> min_alpha;
    u32 empty = 0;  // (AI-assisted) c3l v15: textures without texels (all in atlases): no handle
  };
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
    };
    std::vector<StreamOut> stream_out;
    double ms = 0, read_ms = 0, decode_ms = 0;  // the job's time, its file reads', decoding
    u64 read_bytes = 0;
    u32 reads = 0;
    u32 preloads = 0, preload_misses = 0;  // (streaming) runs read in one request each, or not
    u64 ready_frame = 0;  // host, env CTR_STREAM_SIM: taken from this frame on (SD card model)
    // (AI-assisted, j2intro) host, env CTR_LOAD_DELAY_MS: a level load handed over from then on
    double sim_ready_ms = 0;
  };
  void request(const Job& job);  // render thread
  void run_job(const Job& job);  // loader thread (or inline without one)
  void run_parts_job(const Job& job, Done* d);
  bool load_textures(c3l::Reader& f,
                     const c3l::Header& hdr,
                     const std::vector<c3l::Texture>& texs,
                     const std::vector<c3l::Draw>& draws,
                     const std::string& name,
                     TexLoad* out);
  // (AI-assisted) render thread: the results of a detail reload / repair
  void take_parts(Done& d, u64 frame);
  static void delete_parts(Done& d);
  static void delete_textures(TexLoad* t);
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
  std::string m_loading;  // the level the loader works on ("" if none)
  bool m_loading_prefetch = false;  // ... for a prefetch (cancelled when the game loads another)
  std::atomic<bool> m_cancel{false};  // m_loading is no longer wanted: stop early
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
  std::set<std::string> m_resident;  // names in m_levels + m_prefetched (a prefetch skips them)
  void* m_thread = nullptr;
  std::set<std::string> m_requested;  // queued, loading or done but not taken (under m_lock)
  struct Prefetched {
    std::unique_ptr<CtrLevelData> lev;
    double since_ms = 0;  // when the load finished (ctr_gpu_time_ms)
  };
  std::map<std::string, Prefetched> m_prefetched;
  void update_level_count();

  // ---- (AI-assisted, streaming) ----
  void stream_update(u64 frame);
  void run_stream_job(const Job& job, Done* d);
  void take_stream(Done& d, u64 frame);
  void stream_evict(CtrLevelData& lev, u32 unit);
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
  u32 stream_shrink(bool all);
  static void free_stream_parts(const Job& job);
  u32 m_stream_inflight = 0;  // stream jobs queued, running or not taken (kJobsInFlight)
  // the bytes the repair of incomplete levels waits for (kept free above the arena's floor)
  u32 m_stream_repair_room = 0;
  u32 m_stream_serial = 0;
  std::vector<Done> m_stream_delayed;  // host, CTR_STREAM_SIM: done, not taken yet
  std::vector<Done> m_load_delayed;    // (AI-assisted, j2intro) host, CTR_LOAD_DELAY_MS: the same
  // the camera: where it was last drawn from, its velocity (game units per ms, smoothed)
  float m_cam[3] = {0, 0, 0}, m_cam_vel[3] = {0, 0, 0};
  float m_cam_rot[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};  // (AI-assisted) rows, see camera()
  // (AI-assisted, j2strip) distance freeing: where the camera is and will be, and Jak (game
  // units; n of them, 0: not known), for the render thread (dist_positions) and a copy the loader
  // reads (m_lock: m_dist_snap)
  int dist_positions(float pos[4][3]) const;
  // (AI-assisted, j2pop) min_excess: only the chunks more than that (game units) beyond their
  // freeing distance
  u64 dist_free_far(CtrLevelData& lev, const float pos[4][3], int np, float min_excess = 0.f);
  // (AI-assisted, j2pop) a load that made room: the farthest chunks first, until want bytes are
  // freed (0: all of those far beyond their distance); returns the bytes freed
  u64 dist_make_room(u64 want);
  float m_dist_snap[4][3] = {};
  int m_dist_snap_n = 0;
  u64 m_dist_log_frame = 0;
  math::Vector4f m_planes[4];  // the view's planes (as CtrBackgroundCamera::planes)
  u64 m_cut_frame = 0;         // the last camera cut (a jump of > kCutMeters, or of the factor)
  u64 m_lv_frame = 0;  // (AI-assisted, j2intro) note_late_in_view
  u32 m_lv_count = 0;
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
  float m_k_last = 0.f, m_k_any = 0.f;
  // scheduling (kept to avoid allocations)
  struct StreamCand {
    CtrLevelData* lev;
    u32 unit;
    float score;
  };
  std::vector<StreamCand> m_want, m_have, m_idle;
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
/*! (AI-assisted) config.ini auto_detail: where the shrub / tie distances are between their near
 * (0) and far (1) values (render thread). */
void ctr_level_set_detail(float t);
// (AI-assisted, title-popin) auto_detail: the GPU is over its budget (frames about to miss their
// vblank): lower distances apply to what is in view too
void ctr_level_set_detail_urgent(bool urgent);
/*! (AI-assisted, j2intro; as Jak 1's title3) render thread, before the frame's level draws (Jak II):
 * the screen is black this frame (the game's blackout, or the renderer holding it). Nothing is
 * kept hidden or far "in view" for late data then (nobody saw it missing): dormant chunks read
 * back and near chunks that came are drawn at once (CtrTfragRenderer::draw_level). */
void ctr_level_set_screen_black(bool black);
float ctr_level_detail();
/*! (AI-assisted) render thread: the time of day blend / rewrite ms since the last call (taken out
 * of auto_detail's build time). */
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
  // tie LOD groups (and near chunks), the chunk loop (view / visibility / run tests), occ_cull, time
  // of day, impostors (chunks), the draw lists (the walk and the calls made), impostor cards, the
  // backend (flush_calls); counts: level draws, chunks emitted, runs tested, draws walked / drawn
  enum { kPhSetup, kPhLod, kPhChunks, kPhOcc, kPhTod, kPhImp, kPhLists, kPhCards, kPhFlush, kPhN };
  u64 m_ph[kPhN] = {};
  u64 m_ph_draws = 0, m_ph_emits = 0, m_ph_runs = 0, m_ph_walked = 0, m_ph_calls = 0;
  u64 m_ph_skipped = 0, m_ph_lod_skipped = 0;  // chunks / tie LOD groups their cluster stood for
  u32 m_late_in_view = 0;  // (AI-assisted, j2intro) draw_level: missing in view (note_late_in_view)
  // (AI-assisted, gpu-rt) static_cmdlists: a pass's draws, and each pass's command cache
  std::vector<ctr_mesh_call> m_calls;
  ctr_cmd_cache* m_cmd_cache[3] = {nullptr, nullptr, nullptr};  // (AI-assisted, j2water) 2: late
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
  void env_color_scan(const CtrLevelData& lev, const CtrRenderState& rs);
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
  u64 m_lod_groups_near = 0, m_lod_frames = 0, m_lod_tris[2] = {0, 0};
  u64 m_cull_runs = 0, m_cull_tris = 0, m_cull_chunks = 0, m_cull_frames = 0;
  // (AI-assisted) fine_vis: the level's set this frame (nullptr: none); statistics: chunks and
  // triangles it hid (that the other tests drew), level draws with a set / with a .c3v
  const CtrFineVis* m_fine = nullptr;
  const u32* m_fine_bits = nullptr;
  float m_fine_k_lo = 1e30f, m_fine_k_hi = 0.f;  // the tie LOD factors seen (statistics)
  u64 m_fine_chunks = 0, m_fine_tris = 0, m_fine_frames = 0, m_fine_levels = 0;
  // (AI-assisted) occ_cull (CtrOccCull.h): the occluder candidates of a level draw; statistics:
  // level draws, occluder triangles taken / rasterized, chunks / runs / triangles hidden, ms
  struct OccCand {
    float pri;
    u32 slot, di, first, count;
  };
  std::vector<OccCand> m_occ_cands;
  struct OccTri {
    float q[3][4];
    float size;
  };
  std::vector<OccTri> m_occ_tris_buf;
  u64 m_occ_frames = 0, m_occ_tris_in = 0, m_occ_tris_drawn = 0, m_occ_chunks = 0, m_occ_runs = 0,
      m_occ_tris = 0, m_occ_drawn_tris = 0;
  double m_occ_ms = 0, m_occ_max_ms = 0, m_occ_phase[6] = {0, 0, 0, 0, 0, 0};
  // (AI-assisted) host --ctr-gfx, env CTR_FINE_VIS_CHECK: the runs fine_vis hid this level draw
  // (chunk, run; run ~0u: all of the chunk), checked against the frame's depth at its end
  std::vector<std::pair<u32, u32>> m_fine_hidden;
  // (AI-assisted) lowres_vista / neighbour_vis: low resolution chunks drawn in near mode, and the
  // vista pass's chunks and triangles (statistics); the levels' detailed areas (per level draw)
  u64 m_lowres_near = 0, m_vista_chunks = 0, m_vista_tris = 0, m_vista_frames = 0;
  // (AI-assisted) c3l v16 time of day: blends, vertices recolored and their time (ms) since the
  // last log line, over m_tod_frames level draws; the most vertices in one draw
  u64 m_tod_frames = 0, m_tod_blends = 0, m_tod_verts = 0, m_tod_max = 0;
  // (AI-assisted) groups rewritten, chunks drawn behind; sampled level draws, the most day moves a
  // group drawn was behind, draws with one 2 or more behind
  u64 m_tod_spread = 0, m_tod_fresh = 0, m_tod_held = 0;
  // (AI-assisted, fable1) heavy frames (the last level draw's vertices drawn >= 3x the table):
  // every second one takes no new weights (draw_level); the count for the statistics
  bool m_tod_heavy = false;
  u64 m_tod_skipped = 0;
  double m_tod_ms = 0, m_tod_max_ms = 0;
  std::vector<const CtrLevelData*> m_loaded;
  // (AI-assisted, titlemove) per chunk of the level drawn: a shrub chunk's distance limit (game
  // units, its runs' limit)
  std::vector<float> m_shrub_lim;
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
    bool runs_valid;  // (AI-assisted) m_run_shown holds its runs' flags (impostors)
    bool vista;   // (AI-assisted) far scenery: drawn first, with the vista fog (lowres_vista)
    bool nofog;   // (AI-assisted) in front of where both fogs start (fog_stage_skip)
    float clip[16];
    ctr_mesh_matrix gpu;  // clip, converted for the GPU once per frame
  };
  std::vector<VisibleChunk> m_visible;
  std::vector<int> m_visible_slot;  // per chunk: index in m_visible, -1: not drawn
  // (AI-assisted) with vista chunks: the visible draws of early_draws, sorted_draws and
  // ordered_draws split in one walk, in list order: the vista pass's, and the main pass's lists
  std::vector<u32> m_vista_draws, m_main_draws[3];
  // (AI-assisted, j2cull) the visible draws' places per list (bit sets), and those draws in order
  std::vector<u32> m_list_bits[3], m_list_vis[3];
};
