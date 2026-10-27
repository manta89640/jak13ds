/*!
 * @file CtrImpostor.cpp
 * (AI-assisted)
 * Impostors for far tie groups, see CtrImpostor.h.
 */

#include "CtrImpostor.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <new>

#include "common/log/log.h"

#include "game/graphics/ctr/CtrLevel.h"
#include "game/graphics/ctr/CtrSettings.h"
#include "game/graphics/ctr/c3l_format.h"

namespace {
constexpr float kUnitsPerMeter = 4096.f;
constexpr int kMaxTile = 128;          // pixels, both ways
constexpr int kMaxCapturesPerFrame = 48;
constexpr int kMaxTriesPerFrame = 96;  // capture candidates looked at per frame
constexpr int kBuildsPerFrame = 4;     // chunks whose groups are made per frame
constexpr u32 kExactTrisPerFrame = 1500;  // groups' exact boxes made per frame (their triangles)
constexpr double kExactMsPerFrame = 0.4;  // ... and their CPU time
constexpr u64 kSlotTimeout = 600;      // frames a tile is kept without being shown
constexpr int kMaxCards = 512;         // per level draw
constexpr float kScaleTolerance = 0.15f;
constexpr int kCellTarget = 12;        // pixels per cell side before splits
constexpr float kSplitDepth = 0.006f;  // a cell's plane this far (of its distance) in front of a
                                       // visible texel of it: split in four
constexpr int kMinSplitSide = 4;       // pixels: smaller cells aren't split
constexpr u32 kMinTris = 12;           // smaller groups stay geometry (a card would save little)
constexpr int kTodMaxErr = 4;          // vertex color units (0x80 = 1.0) the time of day may move
                                       // a card's colors before it's drawn again
constexpr int kMaxCardVerts = 8192;    // per level draw (the frame's vertex stream: 128 KB)
constexpr int kRasterPerFrame = 40000; // capture texels the CPU rasterizes per frame at most
constexpr double kCaptureCpuMs = 1.0;  // capture CPU work (raster, planes) per frame at most
// (AI-assisted, perfv2) ... and while the render thread's build took kBusyBuildMs or more in the
// frame before, kCaptureCpuMsBusy: there the build time sets the frame rate (village2 on hardware:
// build 18-24 ms, the GPU waiting for it; captures 0.9-1.4 ms a frame of it to save 2-4k triangles
// of GPU work it had room for), so new pictures come slower (their groups drawn as geometry)
constexpr double kBusyBuildMs = 18.0;
constexpr double kCaptureCpuMsBusy = 0.3;
constexpr float kQStep = 128.f;        // card vertex quantization (game units: 3 cm)
constexpr double kQGrid = 512.0 * 4096.0;  // m_qorigin's grid (game units)
constexpr double kQMove = 384.0 * 4096.0;  // the camera this far from it: a new origin
constexpr float kMinLife = 4.f;        // frames a new card should stay right (camera moving): else
                                       // it stays geometry (a capture costs about its triangles)
// (AI-assisted) CPU-bound scenes need longer reuse to repay rasterization/plane fitting.
// Citadel's lighting pulse otherwise invalidates small cards every few frames.
constexpr float kMinLifeBusy = 15.f;

// tile sizes: widths in 16 pixel columns (the render target's bands start at multiples of 16
// rows), heights in 8 pixel rows
int tile_width(int n) {
  return (n + 15) & ~15;
}
int tile_height(int n) {
  return (n + 7) & ~7;
}

// 3x3 solve (Cramer); false if singular
bool solve3(const double m[3][3], const double r[3], double out[3]) {
  const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                     m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                     m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
  if (!(std::abs(det) > 1e-30)) {
    return false;
  }
  for (int k = 0; k < 3; k++) {
    double t[3][3];
    for (int i = 0; i < 3; i++) {
      for (int j = 0; j < 3; j++) {
        t[i][j] = j == k ? r[i] : m[i][j];
      }
    }
    out[k] = (t[0][0] * (t[1][1] * t[2][2] - t[1][2] * t[2][1]) -
              t[0][1] * (t[1][0] * t[2][2] - t[1][2] * t[2][0]) +
              t[0][2] * (t[1][0] * t[2][1] - t[1][1] * t[2][0])) /
             det;
  }
  return true;
}
// a box's center and half diagonal
void set_sphere(const float bmin[3], const float bmax[3], float cen[3], float* rad) {
  float r2 = 0.f;
  for (int a = 0; a < 3; a++) {
    cen[a] = 0.5f * (bmin[a] + bmax[a]);
    const float h = 0.5f * (bmax[a] - bmin[a]);
    r2 += h * h;
  }
  *rad = std::sqrt(r2);
}
}  // namespace

CtrImpostors& ctr_impostors() {
  static CtrImpostors s;
  return s;
}

bool CtrImpostors::ensure_atlas() {
  if (m_atlas >= 0) {
    return true;
  }
  if (m_failed) {
    return false;
  }
  // impostor_atlas KB of RGBA8: 256 (lh) high, the rest wide
  const int kb = ctr_settings().impostor_atlas;
  m_lh = 256;
  m_lw = kb * 1024 / (4 * m_lh);
  m_atlas = ctr_gpu_rt_create(m_lw, m_lh, kMaxTile);
  if (m_atlas < 0) {
    m_failed = true;
    lg::warn("[ctr] impostors: no memory for the {}x{} texture, impostors off", m_lw, m_lh);
    return false;
  }
  m_cols.assign(m_lw / 16, 0);
  return true;
}

// (AI-assisted, lavamem) The impostors' tables live in the heap, which a long session leaves
// fragmented: an allocation that fails drops the level's impostors (their memory comes back) and
// keeps them off for 10 s; the level is drawn as geometry meanwhile. Before, the bad_alloc ended
// the game (lava tube, Oct 25).
void CtrImpostors::out_of_memory(const char* where) {
  if (m_rt_open) {
    ctr_gpu_rt_end();
    if (m_cam.fog0 && m_cam.fog1) {
      ctr_gpu_set_mesh_fog(m_cam.fog0, m_cam.fog1, m_cam.fog_rgb[0], m_cam.fog_rgb[1],
                           m_cam.fog_rgb[2]);
    }
    m_rt_open = false;
  }
  if (m_cur >= 0 && m_cur < (int)m_levels.size()) {
    drop_level(m_cur);
  }
  m_recs.clear();
  m_wants.clear();
  m_seen.clear();
  m_cards.clear();
  m_active = false;
  m_oom_until = m_frame + 300;
  m_oom_count++;
  lg::warn("[ctr] impostors: out of heap memory in {} ({} times): off for 10 s", where,
           m_oom_count);
}

void CtrImpostors::begin_level(CtrLevelData& lev, const Camera& cam, u64 frame, bool allowed) {
  m_active = false;
  if (frame < m_oom_until) {
    m_frame = frame;
    return;
  }
  try {
    begin_level_impl(lev, cam, frame, allowed);
  } catch (const std::bad_alloc&) {
    out_of_memory("begin_level");
  }
}

void CtrImpostors::chunk(u32 ci, const float clip[16], bool runs_valid, u8* run_shown,
                         bool* partly) {
  if (!m_active) {
    return;
  }
  try {
    chunk_impl(ci, clip, runs_valid, run_shown, partly);
  } catch (const std::bad_alloc&) {
    out_of_memory("chunk");
  }
}

void CtrImpostors::finish_chunks() {
  if (!m_active) {
    return;
  }
  try {
    finish_chunks_impl();
  } catch (const std::bad_alloc&) {
    out_of_memory("finish_chunks");
  }
}

void CtrImpostors::draw_cards() {
  if (!m_active) {
    return;
  }
  try {
    draw_cards_impl();
  } catch (const std::bad_alloc&) {
    out_of_memory("draw_cards");
  }
}

void CtrImpostors::begin_level_impl(CtrLevelData& lev, const Camera& cam, u64 frame,
                                    bool allowed) {
  const CtrSettings& set = ctr_settings();
  // (AI-assisted, title3) allowed: false on the title screen (CtrTfragRenderer::draw_level)
  if (!set.impostors || m_failed || !allowed || !ensure_atlas()) {
    return;
  }
  if (frame != m_frame) {
    m_frame = frame;
    m_builds = 0;
    m_exact_tris = 0;
    m_exact_ms = 0;
    m_cap_cpu_ms = 0;
    m_st.frames++;
    // the camera's speed
    if (m_have_last) {
      float d2 = 0.f;
      for (int a = 0; a < 3; a++) {
        const float d = cam.trans[a] - m_last_trans[a];
        d2 += d * d;
      }
      const float v = std::min(std::sqrt(d2), 64.f * kUnitsPerMeter);  // (teleports)
      m_speed = v > m_speed ? 0.5f * (m_speed + v) : 0.8f * m_speed + 0.2f * v;
    }
    for (int a = 0; a < 3; a++) {
      m_last_trans[a] = cam.trans[a];
    }
    m_have_last = true;
    // tiles not shown for a while go back
    for (int s = 0; s < (int)m_slots.size(); s++) {
      if (m_slots[s].used && frame - m_slots[s].last_used > kSlotTimeout) {
        free_slot(s);
      }
    }
    // levels not drawn for a second: forgotten
    for (int li = 0; li < (int)m_levels.size(); li++) {
      if (m_levels[li].lev && frame - m_levels[li].last_frame > 60) {
        drop_level(li);
      }
    }
  }
  m_st.level_draws++;
  int li = -1;
  for (int i = 0; i < (int)m_levels.size(); i++) {
    if (m_levels[i].lev == &lev) {
      li = i;
    }
  }
  if (li >= 0 && (m_levels[li].generation != lev.generation ||
                  m_levels[li].chunks.size() != lev.chunks.size())) {
    drop_level(li);
  }
  if (li < 0 || !m_levels[li].lev) {
    if (li < 0) {
      for (int i = 0; i < (int)m_levels.size() && li < 0; i++) {
        if (!m_levels[i].lev) {
          li = i;
        }
      }
      if (li < 0) {
        li = (int)m_levels.size();
        m_levels.emplace_back();
      }
    }
    LevelState& ls = m_levels[li];
    ls = LevelState();
    ls.lev = &lev;
    ls.generation = lev.generation;
    ls.chunks.assign(lev.chunks.size(), ChunkInfo());
  }
  m_levels[li].last_frame = frame;
  m_cur = li;
  m_cam = cam;
  for (int c = 0; c < 4; c++) {
    double b = 0;
    for (int i = 0; i < 3; i++) {
      m_A[c][i] = -(double)cam.R[i][c];
      b += (double)cam.R[i][c] * (double)cam.trans[i];
    }
    if (c < 3) {
      b -= cam.R[3][c];
    }
    m_B[c] = b;
  }
  for (int i = 0; i < 3; i++) {
    m_A[1][i] *= cam.y_scale;
  }
  m_B[1] *= cam.y_scale;
  for (int c = 0; c < 4; c++) {
    for (int i = 0; i < 3; i++) {
      m_Af[c][i] = (float)m_A[c][i];
    }
    m_Kf[c] = c < 3 ? -cam.R[3][c] * (c == 1 ? cam.y_scale : 1.f) : 0.f;
  }
  for (int c = 0; c < 4; c++) {
    m_rowlen[c] = std::sqrt(m_Af[c][0] * m_Af[c][0] + m_Af[c][1] * m_Af[c][1] +
                            m_Af[c][2] * m_Af[c][2]);
  }
  // the same camera as the last level draw (this frame's other level, or a still camera): the
  // cards' tests give what they gave
  {
    float key[19];
    memcpy(key, m_Af, sizeof(m_Af));
    memcpy(key + 12, m_Kf, sizeof(m_Kf));
    for (int a = 0; a < 3; a++) {
      key[16 + a] = m_cam.trans[a];
    }
    m_cam_same = !memcmp(key, m_cam_key, sizeof(key));
    memcpy(m_cam_key, key, sizeof(key));
  }
  m_recs.clear();
  m_wants.clear();
  m_seen.clear();
  m_cards.clear();
  m_active = true;
  m_t_chunks = ctr_gpu_time_ms();
}

void CtrImpostors::drop_level(int li) {
  for (int s = 0; s < (int)m_slots.size(); s++) {
    if (m_slots[s].used && m_slots[s].level == li) {
      free_slot(s);
    }
  }
  m_levels[li] = LevelState();
}

void CtrImpostors::build_chunk(LevelState& ls, u32 ci) {
  const CtrLevelData& lev = *ls.lev;
  ChunkInfo& info = ls.chunks[ci];
  const auto& ch = lev.chunks[ci];
  info.mesh = lev.meshes[ci];
  info.first = (u32)ls.clusters.size();
  info.count = 0;
  const void* vp;
  const u16* ip;
  int nv, ni;
  if (!ch.vis_run_count || ch.kind != c3l::CHUNK_KIND_TIE ||
      !ctr_gpu_mesh_peek(lev.meshes[ci], &vp, &nv, &ip, &ni)) {
    return;
  }
  const double t0 = ctr_gpu_time_ms();
  (void)vp;
  (void)nv;
  // runs by visibility group (ids in vis order; a group's runs are in several draws)
  struct RunAt {
    u16 vis;
    u32 run, first;
  };
  std::vector<RunAt> runs;
  runs.reserve(ch.vis_run_count);
  for (u32 k = 0; k < ch.draw_count; k++) {
    const u32 di = ch.first_draw + k;
    if (di >= lev.draws.size() || !lev.draw_run_count[di]) {
      continue;
    }
    u32 idx = lev.draws[di].first_index;
    for (u32 r = lev.draw_first_run[di]; r < lev.draw_first_run[di] + lev.draw_run_count[di]; r++) {
      const u16 vis = lev.vis_runs[r].vis;
      // (AI-assisted) c3l v17: both versions of a tie LOD group: a card shows the one the level
      // draw shows (Oct 17: the far version only made the near rocks of Sandover change color)
      if (vis != 0xffff) {
        runs.push_back({vis, r, idx});
      }
      idx += lev.vis_runs[r].index_count;
    }
  }
  std::stable_sort(runs.begin(), runs.end(),
                   [](const RunAt& a, const RunAt& b) { return a.vis < b.vis; });
  for (size_t i = 0; i < runs.size();) {
    size_t j = i;
    Cluster c;
    c.chunk = ci;
    c.vis = runs[i].vis;
    c.first_run = (u32)ls.runs.size();
    c.eligible = true;
    // a box around the runs' spheres (made at load; the exact box later, when it's a candidate)
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    for (; j < runs.size() && runs[j].vis == runs[i].vis; j++) {
      const u32 r = runs[j].run;
      const u32 n = lev.vis_runs[r].index_count;
      const u32 di = ch.first_draw + lev.vis_runs[r].draw;
      const ctr_draw_state& st = lev.draw_states[di];
      // opaque only: a blended part would be drawn over the card (or hidden behind it)
      if (st.blend != CTR_BLEND_OFF || !st.zwrite || st.ztest != CTR_TEST_GEQUAL || st.decal ||
          (st.flags & ~(CTR_STATE_EARLY_DEPTH | CTR_STATE_PROCTEX))) {
        c.eligible = false;
      }
      const u32 f = runs[j].first;
      if (f + n > (u32)ni) {
        c.eligible = false;
      }
      // (AI-assisted, j2vis) a prototype the game hides by task state (c3l v20): drawn as itself,
      // never in a card (a card made while it was shown would keep it)
      if (!lev.protos.empty() && lev.proto_run(r)) {
        c.eligible = false;
      }
      if (r < lev.run_spheres.size() && lev.run_spheres[r].r != 0xffff) {
        const auto& sp = lev.run_spheres[r];
        for (int a = 0; a < 3; a++) {
          mn[a] = std::min(mn[a], ch.origin[a] + (sp.c[a] - (float)sp.r) * ch.scale);
          mx[a] = std::max(mx[a], ch.origin[a] + (sp.c[a] + (float)sp.r) * ch.scale);
        }
      } else {
        c.eligible = false;  // no sphere: nothing known about it
      }
      if (r < lev.run_lod.size() && lev.run_lod[r].lod == c3l::kRunLodNear) {
        c.has_near = true;  // (its triangles not counted: c.tris is the far version's)
      } else {
        c.tris += n / 3;
      }
      ls.runs.push_back(r);
      ls.runs.push_back(f);
      c.run_count++;
    }
    if (c.tris && mn[0] <= mx[0]) {
      for (int a = 0; a < 3; a++) {
        c.bmin[a] = mn[a];
        c.bmax[a] = mx[a];
      }
    } else {
      c.eligible = false;
      for (int a = 0; a < 3; a++) {
        c.bmin[a] = c.bmax[a] = ch.bsphere[a];
      }
    }
    set_sphere(c.bmin, c.bmax, c.cen, &c.rad);
    ls.clusters.push_back(c);
    info.count++;
    i = j;
  }
  m_st.built_chunks++;
  m_st.build_ms += ctr_gpu_time_ms() - t0;
}

// (AI-assisted, lavamem) a new pass over vertex indices (m_vtag), every 65535th one with the tags
// cleared
bool CtrImpostors::new_pass() {
  if (m_vtag.empty()) {
    try {
      m_vtag.assign(65536, 0);
    } catch (const std::bad_alloc&) {
      return false;
    }
  }
  if (++m_stamp == 0) {
    std::fill(m_vtag.begin(), m_vtag.end(), 0);
    m_stamp = 1;
  }
  return true;
}

// the group's exact box from its vertices (once); false if the mesh can't be read
bool CtrImpostors::exact_box(LevelState& ls, Cluster& c) {
  const CtrLevelData& lev = *ls.lev;
  const auto& ch = lev.chunks[c.chunk];
  const void* vp;
  const u16* ip;
  int nv, ni;
  if (!ctr_gpu_mesh_peek(lev.meshes[c.chunk], &vp, &nv, &ip, &ni)) {
    return false;
  }
  const c3l::Vertex* verts = (const c3l::Vertex*)vp;
  int mn[3] = {32767, 32767, 32767}, mx[3] = {-32768, -32768, -32768};
  const CtrLevelData::Tod* tod = lev.tod.get();
  const u32 tod_nc = tod ? tod->color_count : 0, tod_nm = tod ? (u32)tod->muls.size() : 0;
  u32 pal = 0;
  memset(c.tmax, 0, sizeof(c.tmax));
  memset(c.tmin, 255, sizeof(c.tmin));
  // (each vertex once: the indices repeat them)
  if (!new_pass()) {
    return false;
  }
  u32 work = 0;
  for (u32 i = 0; i < c.run_count; i++) {
    const u32 r = ls.runs[c.first_run + 2 * i];
    const u32 f = ls.runs[c.first_run + 2 * i + 1];
    const u32 n = lev.vis_runs[r].index_count;
    if (f + n > (u32)ni) {
      return false;
    }
    work += n;
    for (u32 k = f; k < f + n; k++) {
      const u16 v = ip[k];
      if (v >= (u32)nv) {
        return false;
      }
      if ((m_vtag[v] >> 16) == m_stamp) {
        continue;
      }
      m_vtag[v] = (u32)m_stamp << 16;
      const s16* p = verts[v].pos;
      for (int a = 0; a < 3; a++) {
        mn[a] = std::min<int>(mn[a], p[a]);
        mx[a] = std::max<int>(mx[a], p[a]);
      }
      // (time of day) the palettes of its color, their brightest and darkest (only the ones
      // not black in it: a black one adds nothing whatever its weight)
      if (tod) {
        const u32 idx = (u16)verts[v].pad;
        if (idx < tod_nc + tod_nm) {
          const u32 m = tod->color_mask[idx];
          pal |= m;
          const u32 col = idx < tod_nc ? idx : tod->muls[idx - tod_nc].color;
          const u8* mulv = idx < tod_nc ? nullptr : tod->muls[idx - tod_nc].mul;
          const u8* src = tod->colors.data() + (size_t)col * tod->palette_count * 3;
          for (u32 k = 0; k < tod->palette_count; k++) {
            const int p = tod->palettes[k] & 7;
            if (!(m >> p & 1)) {
              continue;
            }
            for (int ch = 0; ch < 3; ch++) {
              const u32 x = mulv ? std::min<u32>(255, (src[k * 3 + ch] * mulv[ch] + 64) >> 7)
                                 : src[k * 3 + ch];
              c.tmax[p][ch] = std::max<u8>(c.tmax[p][ch], (u8)x);
              c.tmin[p][ch] = std::min<u8>(c.tmin[p][ch], (u8)x);
            }
          }
        }
      }
    }
  }
  m_exact_tris += work / 3;
  c.pal = (u8)pal;
  c.light = tod && (pal & tod->light_mask) != 0;
  if (mn[0] > mx[0]) {
    return false;
  }
  for (int a = 0; a < 3; a++) {
    c.bmin[a] = ch.origin[a] + mn[a] * ch.scale;
    c.bmax[a] = ch.origin[a] + mx[a] * ch.scale;
  }
  set_sphere(c.bmin, c.bmax, c.cen, &c.rad);
  c.exact = true;
  return true;
}

// the box's corners on screen now (float, relative to the camera)
void CtrImpostors::project_box(const float bmin[3], const float bmax[3], Proj* out) const {
  float d0[3], d1[3];
  for (int a = 0; a < 3; a++) {
    d0[a] = bmin[a] - m_cam.trans[a];
    d1[a] = bmax[a] - m_cam.trans[a];
  }
  // each row's value at the min corner and its change towards the max corner per axis
  float base[4], step[4][3];
  for (int c = 0; c < 4; c++) {
    base[c] = m_Af[c][0] * d0[0] + m_Af[c][1] * d0[1] + m_Af[c][2] * d0[2] + m_Kf[c];
    for (int a = 0; a < 3; a++) {
      step[c][a] = m_Af[c][a] * (d1[a] - d0[a]);
    }
  }
  Proj& o = *out;
  o.xmin = o.ymin = o.zmin = 1e9f;
  o.xmax = o.ymax = o.zmax = -1e9f;
  o.wmin = 1e30f;
  o.wmax = -1e30f;
  o.front = true;
  for (int n = 0; n < 8; n++) {
    float q[4];
    for (int c = 0; c < 4; c++) {
      q[c] = base[c] + ((n & 1) ? step[c][0] : 0.f) + ((n & 2) ? step[c][1] : 0.f) +
             ((n & 4) ? step[c][2] : 0.f);
    }
    o.wmin = std::min(o.wmin, q[3]);
    o.wmax = std::max(o.wmax, q[3]);
    if (!(q[3] > 1e-3f)) {
      o.front = false;
      continue;
    }
    const float iw = 1.f / q[3];
    const float x = 200.f * (1.f + q[0] * iw), y = 120.f * (1.f + q[1] * iw), z = q[2] * iw;
    o.xmin = std::min(o.xmin, x);
    o.xmax = std::max(o.xmax, x);
    o.ymin = std::min(o.ymin, y);
    o.ymax = std::max(o.ymax, y);
    o.zmin = std::min(o.zmin, z);
    o.zmax = std::max(o.zmax, z);
  }
}

void CtrImpostors::chunk_impl(u32 ci, const float clip[16], bool runs_valid, u8* run_shown,
                              bool* partly) {
  if (!m_active) {
    return;
  }
  LevelState& ls = m_levels[m_cur];
  const CtrLevelData& lev = *ls.lev;
  if (ci >= ls.chunks.size() || lev.chunks[ci].kind != c3l::CHUNK_KIND_TIE ||
      !lev.chunks[ci].vis_run_count) {
    return;
  }
  const CtrSettings& set = ctr_settings();
  const auto& ch = lev.chunks[ci];
  // the whole chunk nearer than the distance: nothing to do (no groups made for it yet)
  const double D = set.impostor_distance * kUnitsPerMeter;
  {
    const double dx = ch.bsphere[0] - m_cam.trans[0], dy = ch.bsphere[1] - m_cam.trans[1],
                 dz = ch.bsphere[2] - m_cam.trans[2];
    const double far_pt = std::sqrt(dx * dx + dy * dy + dz * dz) + ch.bsphere[3];
    if (far_pt < 0.9 * D) {
      return;
    }
  }
  ChunkInfo& info = ls.chunks[ci];
  if (info.mesh != lev.meshes[ci]) {
    if (info.mesh != -2) {
      // the chunk's mesh was made again (partial unload / repair): start the level over
      const CtrLevelData* l = ls.lev;
      const int cur = m_cur;
      drop_level(cur);
      m_levels[cur].lev = l;
      m_levels[cur].generation = l->generation;
      m_levels[cur].chunks.assign(l->chunks.size(), ChunkInfo());
      m_levels[cur].last_frame = m_frame;
      return;
    }
    if (m_builds >= kBuildsPerFrame) {
      return;
    }
    m_builds++;
    build_chunk(ls, ci);
  }
  if (!info.count) {
    return;
  }
  const u32 r0 = lev.chunk_first_run[ci];
  const u32 nr = lev.chunk_run_count[ci];
  const u32 rec_index = (u32)m_recs.size();
  const float E = set.impostor_error;
  const float Dn = (float)(0.9 * D);
  bool any = false;
  for (u32 k = info.first; k < info.first + info.count; k++) {
    Cluster& c = ls.clusters[k];
    // shown by the visibility and view tests (a group's runs share its visibility bit)
    bool shown = !runs_valid, shown_near = false;
    for (u32 i = 0; i < c.run_count && runs_valid; i++) {
      const u32 r = ls.runs[c.first_run + 2 * i];
      if (run_shown[r]) {
        shown = true;
        shown_near |= c.has_near && r < lev.run_lod.size() && lev.run_lod[r].lod == c3l::kRunLodNear;
      }
    }
    if (!shown) {
      m_st.t_hidden += c.tris;
      continue;
    }
    const float dx = c.cen[0] - m_cam.trans[0], dy = c.cen[1] - m_cam.trans[1],
                dz = c.cen[2] - m_cam.trans[2];
    const float d2 = dx * dx + dy * dy + dz * dz;
    // entirely nearer than the impostor distance: neither a card nor in front of a card's group
    if (Dn > c.rad && d2 < (Dn - c.rad) * (Dn - c.rad)) {
      m_st.t_near += c.tris;
      continue;
    }
    any = true;
    if (!c.eligible) {
      // not opaque: no card may hide part of it (finish_chunks)
      m_seen.push_back(Seen{k});
      m_st.t_opaque_no += c.tris;
      continue;
    }
    if (c.light || c.tris < kMinTris) {
      m_st.t_light += c.tris;  // (light palettes, or too small to pay)
      continue;
    }
    // (AI-assisted) A held capture stays geometry. Reject it before sorting wants, testing
    // stale colors and touching the atlas; finish_chunks used to repeat those checks each frame.
    if (m_frame < c.tod_hold) {
      m_st.short_life++;
      m_st.t_short += c.tris;
      continue;
    }
    const bool had = c.slot >= 0 && m_slots[c.slot].last_used + 1 >= m_frame;
    if (!had && m_frame < c.retry) {
      m_st.t_room += c.tris;  // (no room for it lately: geometry for a while)
      continue;
    }
    const float lim = (float)((had ? 0.9 : 1.0) * D) + c.rad;  // its nearest point beyond D
    if (d2 < lim * lim) {
      m_st.t_near += c.tris;
      continue;
    }
    Want w;
    w.cluster = k;
    w.rec = rec_index;
    w.capture = true;
    w.near = shown_near;
    if (had) {
      // the card still right? Nothing changed when the camera didn't move; else the parallax
      // bound for how far it moved since the capture, and the card's size on screen
      const Slot& sl = m_slots[c.slot];
      bool valid = m_cam_same && sl.near == shown_near;
      if (!valid) {
        float mv2 = 0.f;
        for (int a = 0; a < 3; a++) {
          const float d = (float)(sl.cam[a] - (double)m_cam.trans[a]);
          mv2 += d * d;
        }
        Slot& slm = m_slots[c.slot];
        if (sl.near != shown_near) {
          m_st.recapture_lod++;  // (the level draw switched the group's tie LOD version)
        } else if (sl.kpar * sl.kpar * mv2 > E * E) {
          m_st.recapture_error++;
        } else if ((slm.checks++ & 7) == 0 && std::abs(card_scale(sl) - 1.f) > kScaleTolerance) {
          m_st.recapture_scale++;
        } else {
          valid = true;
        }
      }
      // the time of day moved its colors (also with a still camera)
      const CtrLevelData::Tod* lt = lev.tod.get();
      if (valid && sl.tod && lt && lt->ver != sl.tod_ver) {
        const int te = tod_error(ls, c, sl);
        if (te > kTodMaxErr) {
          valid = false;
          m_st.recapture_tod++;
          w.tod_rate = (float)te / (float)std::max<u64>(1, m_frame - sl.captured);
        } else {
          m_slots[c.slot].tod_ver = lt->ver;  // (checked again when the day moves)
        }
      }
      if (valid) {
        w.capture = false;
        w.value = sl.value;
        m_wants.push_back(w);
        continue;
      }
    }
    // a new picture: done in finish_chunks (within the budget), if it's on screen; its priority
    // from the sphere: triangles per pixel of the sphere's square on screen
    {
      Rect r;
      if (sphere_rect(c, &r) && (r.x1 < 0 || r.x0 > 400 || r.y1 < 0 || r.y0 > 240)) {
        m_st.t_off += c.tris;
        continue;
      }
    }
    w.value = (float)c.tris * d2 / std::max(1.f, c.rad * c.rad);
    m_wants.push_back(w);
  }
  if (any) {
    ChunkRec rec;
    rec.ci = ci;
    memcpy(rec.clip, clip, sizeof(rec.clip));
    rec.run_shown = run_shown;
    rec.partly = partly;
    if (!runs_valid) {
      // the level draw reads the flags once the chunk is drawn run by run
      for (u32 r = r0; r < r0 + nr; r++) {
        run_shown[r] = 1;
      }
    }
    m_recs.push_back(rec);
  }
}

// the card's size on screen now against the capture's (texels per pixel): its tile's corners
float CtrImpostors::card_scale(const Slot& s) const {
  float px[3][2];
  for (int k = 0; k < 3; k++) {
    float d[3], q[4];
    for (int a = 0; a < 3; a++) {
      d[a] = (float)(s.cam[a] - (double)m_cam.trans[a]) + s.corner[k][a];
    }
    for (int r = 0; r < 4; r++) {
      q[r] = m_Af[r][0] * d[0] + m_Af[r][1] * d[1] + m_Af[r][2] * d[2] + m_Kf[r];
    }
    if (!(q[3] > 1e-3f)) {
      return 1e9f;
    }
    px[k][0] = 200.f * (1.f + q[0] / q[3]);
    px[k][1] = 120.f * (1.f + q[1] / q[3]);
  }
  const float sw = std::sqrt((px[1][0] - px[0][0]) * (px[1][0] - px[0][0]) +
                             (px[1][1] - px[0][1]) * (px[1][1] - px[0][1])) /
                   (float)std::max<int>(1, s.wn);
  const float sh = std::sqrt((px[2][0] - px[0][0]) * (px[2][0] - px[0][0]) +
                             (px[2][1] - px[0][1]) * (px[2][1] - px[0][1])) /
                   (float)std::max<int>(1, s.hn);
  return std::abs(sw - 1.f) > std::abs(sh - 1.f) ? sw : sh;
}

// the card's vertices around m_qorigin (once per capture or origin); false if a point is out of
// the quantized range (then it stays geometry)
bool CtrImpostors::quantize(Slot& s) {
  if (s.qgen == m_qgen) {
    return !s.qv.empty();
  }
  s.qgen = m_qgen;
  s.qv.resize(s.pv.size());
  const float inv = 1.f / kQStep;
  float o[3];
  for (int a = 0; a < 3; a++) {
    o[a] = (float)(s.cam[a] - m_qorigin[a]);
  }
  for (size_t k = 0; k < s.pv.size(); k++) {
    s32 q[3];
    for (int a = 0; a < 3; a++) {
      const float f = (o[a] + s.pv[k][a]) * inv;
      if (!(std::abs(f) < 32767.f)) {
        s.qv.clear();
        return false;
      }
      q[a] = (s32)(f < 0.f ? f - 0.5f : f + 0.5f);
    }
    auto& wds = s.qv[k];
    wds[0] = (u32)(u16)q[0] | ((u32)(u16)q[1] << 16);
    wds[1] = (u32)(u16)q[2];
    wds[2] = (u32)(u16)s.pst[k][0] | ((u32)(u16)s.pst[k][1] << 16);
    wds[3] = 0x80808080u;
  }
  return true;
}

// its vertex groups' colors behind the level's time of day (CtrLevel.cpp
// tod_recolor rewrites the shown groups within a budget per frame)
bool CtrImpostors::tod_behind(const LevelState& ls, const Cluster& c) const {
  const CtrLevelData::Tod* t = ls.lev->tod.get();
  const u32 ci = c.chunk;
  if (!t || !c.pal || ci >= t->chunk_scanned.size() || t->chunk_scanned[ci] != 2) {
    return false;
  }
  const u32 dm = t->chunk_mask[ci] & ~t->light_mask;
  const u32 day_last = std::max<u32>(1, t->lut[dm]);
  if (t->chunk_min_ver[ci] >= day_last) {
    return false;  // (every group of the chunk up to date)
  }
  const u32 g0 = t->chunk_group_first[ci], ng = t->chunk_group_count[ci];
  for (u32 i = 0; i < c.run_count; i++) {
    const u32 r = ls.runs[c.first_run + 2 * i];
    const u32 g = g0 + (r < t->run_group.size() ? t->run_group[r] : 0);
    if (g >= g0 + ng || g >= t->groups.size()) {
      continue;
    }
    const auto& gr = t->groups[g];
    if (gr.ver < day_last) {
      return true;  // (even one move behind: that move may be a jump, a new hour)
    }
  }
  return false;
}

// how far (vertex color units) the time of day moved the group's colors since the capture: per
// channel, the weight changes of its palettes times their brightest colors
int CtrImpostors::tod_error(LevelState& ls, const Cluster& c, const Slot& s) {
  const CtrLevelData::Tod* t = ls.lev->tod.get();
  if (!t || !c.pal) {
    return 0;
  }
  // per channel: a vertex's color moved by sum(dw[p] * c[p]) / 64 (c[p] its color in palette p),
  // between these bounds over its vertices' colors
  int worst = 0;
  for (int ch = 0; ch < 3; ch++) {
    int hi = 0, lo = 0;
    for (u32 m = c.pal; m; m &= m - 1) {
      const int p = __builtin_ctz(m);
      const int dw = (int)t->target_w[p][ch] - (int)s.tod_w[p][ch];
      const int cmax = c.tmax[p][ch], cmin = std::min<int>(c.tmin[p][ch], cmax);
      hi += dw * (dw > 0 ? cmax : cmin);
      lo += dw * (dw > 0 ? cmin : cmax);
    }
    worst = std::max(worst, (std::max(std::abs(hi), std::abs(lo)) + 63) >> 6);
  }
  return worst;
}

// a conservative rectangle and depth range of a group from its sphere (one projection): any point
// within the sphere moves clip x by at most r |row 0|, w by at most r |row 3|
bool CtrImpostors::sphere_rect(const Cluster& c, Rect* out) const {
  const float d[3] = {c.cen[0] - m_cam.trans[0], c.cen[1] - m_cam.trans[1],
                      c.cen[2] - m_cam.trans[2]};
  float q[4];
  for (int r = 0; r < 4; r++) {
    q[r] = m_Af[r][0] * d[0] + m_Af[r][1] * d[1] + m_Af[r][2] * d[2] + m_Kf[r];
  }
  const float rx = c.rad * m_rowlen[0], ry = c.rad * m_rowlen[1], rw = c.rad * m_rowlen[3];
  out->w0 = q[3] - rw;
  out->w1 = q[3] + rw;
  if (!(out->w0 > 1e-3f)) {
    out->x0 = out->y0 = -1e9f;
    out->x1 = out->y1 = 1e9f;
    return false;
  }
  const float a0 = (q[0] - rx) / out->w0, a1 = (q[0] - rx) / out->w1;
  const float b0 = (q[0] + rx) / out->w0, b1 = (q[0] + rx) / out->w1;
  const float c0 = (q[1] - ry) / out->w0, c1 = (q[1] - ry) / out->w1;
  const float e0 = (q[1] + ry) / out->w0, e1 = (q[1] + ry) / out->w1;
  out->x0 = 200.f * (1.f + std::min(a0, a1));
  out->x1 = 200.f * (1.f + std::max(b0, b1));
  out->y0 = 120.f * (1.f + std::min(c0, c1));
  out->y1 = 120.f * (1.f + std::max(e0, e1));
  return true;
}

void CtrImpostors::free_slot(int si) {
  Slot& s = m_slots[si];
  if (!s.used) {
    return;
  }
  if (s.level >= 0 && s.level < (int)m_levels.size() &&
      s.cluster < m_levels[s.level].clusters.size()) {
    m_levels[s.level].clusters[s.cluster].slot = -1;
  }
  const u32 n = s.ch / 8, k = s.ty / 8;
  const u32 mask = ((n >= 32) ? 0xffffffffu : ((1u << n) - 1u)) << k;
  for (int col = s.tx / 16; col < (s.tx + s.cw) / 16; col++) {
    m_cols[col] &= ~mask;
  }
  m_texels -= (int)s.cw * s.ch;
  s = Slot();
}

int CtrImpostors::alloc_slot(int wn, int hn, float value, u64 frame) {
  const int cw = tile_width(wn), ch = tile_height(hn);
  const int ncol = cw / 16;
  const u32 n = (u32)ch / 8, rows = (u32)m_lh / 8;
  const u32 run = (n >= 32) ? 0xffffffffu : ((1u << n) - 1u);
  for (int attempt = 0; attempt < 64; attempt++) {
    // first fit: columns c0..c0 + ncol - 1 (16 pixels each), a run of n free 8 pixel rows
    for (int c0 = 0; c0 + ncol <= (int)m_cols.size(); c0++) {
      u32 taken = 0;
      for (int c = c0; c < c0 + ncol; c++) {
        taken |= m_cols[c];
      }
      if (taken == 0xffffffffu) {
        continue;
      }
      for (u32 k = 0; k + n <= rows; k++) {
        if (taken & (run << k)) {
          continue;
        }
        for (int c = c0; c < c0 + ncol; c++) {
          m_cols[c] |= run << k;
        }
        int si = -1;
        for (int i = 0; i < (int)m_slots.size() && si < 0; i++) {
          if (!m_slots[i].used) {
            si = i;
          }
        }
        if (si < 0) {
          si = (int)m_slots.size();
          m_slots.emplace_back();
        }
        Slot& s = m_slots[si];
        s = Slot();
        s.used = true;
        s.tx = (u16)(16 * c0);
        s.ty = (u16)(8 * k);
        s.cw = (u16)cw;
        s.ch = (u16)ch;
        m_texels += cw * ch;
        return si;
      }
    }
    // no room: a tile not shown this frame or the one before (the other level drawn this frame
    // may not have shown its tiles yet), the one shown longest ago; else the one with the fewest
    // triangles per texel if this one has more than twice as many; never one captured this frame
    int victim = -1;
    for (int i = 0; i < (int)m_slots.size(); i++) {
      const Slot& s = m_slots[i];
      if (s.used && s.last_used + 1 < frame &&
          (victim < 0 || s.last_used < m_slots[victim].last_used)) {
        victim = i;
      }
    }
    if (victim < 0) {
      for (int i = 0; i < (int)m_slots.size(); i++) {
        const Slot& s = m_slots[i];
        if (s.used && s.captured < frame && s.value * 2.f < value &&
            (victim < 0 || s.value < m_slots[victim].value)) {
          victim = i;
        }
      }
    }
    if (victim < 0) {
      return -1;
    }
    free_slot(victim);
    m_st.evicted++;
  }
  return -1;
}

bool CtrImpostors::capture(const Want& w, Slot& s) {
  const LevelState& ls = m_levels[m_cur];
  const CtrLevelData& lev = *ls.lev;
  const Cluster& c = ls.clusters[w.cluster];
  const ChunkRec& rec = m_recs[w.rec];
  const void* vp;
  const u16* ip;
  int nv, ni;
  if (!ctr_gpu_mesh_peek(lev.meshes[c.chunk], &vp, &nv, &ip, &ni)) {
    return false;
  }
  const c3l::Vertex* verts = (const c3l::Vertex*)vp;
  if (!ctr_gpu_rt_begin(m_atlas, s.tx, s.ty, s.cw, s.ch)) {
    return false;
  }
  // the tile: alpha 0, depth 0 (the farthest)
  {
    ctr_draw_state st{};
    st.tex = -1;
    st.blend = CTR_BLEND_OFF;
    st.atest = CTR_TEST_ALWAYS;
    st.ztest = CTR_TEST_ALWAYS;
    st.zwrite = 1;
    ctr_vertex q[6];
    static const float kXY[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
    for (int i = 0; i < 6; i++) {
      q[i] = ctr_vertex{kXY[i][0], kXY[i][1], 0.f, 0.f, 0.f, 0, 0, 0, 0};
    }
    ctr_gpu_draw(&st, q, 6);
  }
  // the chunk's matrix, cropped to the tile (which covers screen pixels px0..px0 + cw,
  // py0..py0 + ch at 1:1), its depth range spread over the tile's depth buffer
  const double X0 = w.px0 / 200.0 - 1.0, X1 = (w.px0 + s.cw) / 200.0 - 1.0;
  const double Y0 = w.py0 / 120.0 - 1.0, Y1 = (w.py0 + s.ch) / 120.0 - 1.0;
  const double ax = 2.0 / (X1 - X0), bx = -(X0 + X1) / (X1 - X0);
  const double ay = 2.0 / (Y1 - Y0), by = -(Y0 + Y1) / (Y1 - Y0);
  const double zr = std::max(1e-6, (double)w.zmax - w.zmin);
  const double az = 1.8 / zr, bz = -0.9 - az * w.zmin;
  float m[16];
  for (int i = 0; i < 4; i++) {
    m[i] = (float)(ax * rec.clip[i] + bx * rec.clip[12 + i]);
    m[4 + i] = (float)(ay * rec.clip[4 + i] + by * rec.clip[12 + i]);
    m[8 + i] = (float)(az * rec.clip[8 + i] + bz * rec.clip[12 + i]);
    m[12 + i] = rec.clip[12 + i];
  }
  ctr_mesh_matrix mm;
  ctr_gpu_prepare_mesh_matrix(m, &mm);
  const auto& ch = lev.chunks[c.chunk];
  const int mesh = lev.meshes[c.chunk];
  // the group's triangles: drawn into the tile, and rasterized on the CPU into m_zb: the nearest
  // 1/w per texel (the visible surface, as the GPU covers texel centers; no alpha test)
  const int W = w.wn, H = w.hn;
  const double tcpu = ctr_gpu_time_ms();
  m_zb.assign((size_t)W * H, 0.f);
  if (!new_pass()) {
    return false;
  }
  // (AI-assisted, lavamem) the group's vertices projected once each, in the order first met
  // (m_vtag: the pass and the index into m_pj). At most one per index of its runs: reserved before
  // the loop, so the pointers into it stay valid. Was one entry per vertex of the whole mesh (up to
  // 65536: 1 MB of the heap, grown in steps).
  {
    u32 idx = 0;
    for (u32 i = 0; i < c.run_count; i++) {
      idx += lev.vis_runs[ls.runs[c.first_run + 2 * i]].index_count;
    }
    m_pj.clear();
    m_pj.reserve(std::min<u32>(idx, 0xffff));
  }
  const float* cl = rec.clip;
  const float fx0 = (float)w.px0, fy0 = (float)w.py0;
  for (u32 i = 0; i < c.run_count; i++) {
    const u32 r = ls.runs[c.first_run + 2 * i];
    const u32 first = ls.runs[c.first_run + 2 * i + 1];
    const u32 n = lev.vis_runs[r].index_count;
    const u32 di = ch.first_draw + lev.vis_runs[r].draw;
    if (r < lev.run_lod.size() && lev.run_lod[r].lod != c3l::kRunLodAlways &&
        (lev.run_lod[r].lod == c3l::kRunLodNear) != w.near) {
      continue;  // (the tie LOD version the level draw doesn't show now)
    }
    ctr_gpu_draw_mesh_prepared(&lev.draw_states[di], &mm, mesh, (int)first, (int)n);
    if (first + n > (u32)ni) {
      continue;
    }
    for (u32 t = first; t + 2 < first + n; t += 3) {
      const std::array<float, 3>* p[3];
      bool okt = true;
      for (int v = 0; v < 3; v++) {
        const u16 vi = ip[t + v];
        if (vi >= (u32)nv) {
          okt = false;
          break;
        }
        if ((m_vtag[vi] >> 16) != m_stamp) {
          if (m_pj.size() >= 0xffff) {
            okt = false;  // (a group of 65535 vertices or more: not met in the levels)
            break;
          }
          m_vtag[vi] = ((u32)m_stamp << 16) | (u32)m_pj.size();
          const s16* q = verts[vi].pos;
          const float qx = cl[0] * q[0] + cl[1] * q[1] + cl[2] * q[2] + cl[3];
          const float qy = cl[4] * q[0] + cl[5] * q[1] + cl[6] * q[2] + cl[7];
          const float qw = cl[12] * q[0] + cl[13] * q[1] + cl[14] * q[2] + cl[15];
          if (qw > 1e-3f) {
            const float iw = 1.f / qw;
            m_pj.push_back({200.f * (1.f + qx * iw) - fx0, 120.f * (1.f + qy * iw) - fy0, iw});
          } else {
            m_pj.push_back({0.f, 0.f, -1.f});
          }
        }
        p[v] = &m_pj[m_vtag[vi] & 0xffff];
        okt = okt && (*p[v])[2] > 0.f;
      }
      if (!okt) {
        continue;
      }
      const float x0 = (*p[0])[0], y0 = (*p[0])[1], u0 = (*p[0])[2];
      const float x1 = (*p[1])[0], y1 = (*p[1])[1], u1 = (*p[1])[2];
      const float x2 = (*p[2])[0], y2 = (*p[2])[1], u2 = (*p[2])[2];
      const float area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0);
      if (!(std::abs(area) > 1e-6f)) {
        continue;
      }
      // texel centers (i + 0.5, j + 0.5) inside the box
      const int i0 = std::max(0, (int)std::ceil(std::min({x0, x1, x2}) - 0.5f));
      const int i1 = std::min(W - 1, (int)std::floor(std::max({x0, x1, x2}) - 0.5f));
      const int j0 = std::max(0, (int)std::ceil(std::min({y0, y1, y2}) - 0.5f));
      const int j1 = std::min(H - 1, (int)std::floor(std::max({y0, y1, y2}) - 0.5f));
      if (i0 > i1 || j0 > j1) {
        continue;
      }
      // 1/w is affine on screen over a flat triangle
      const float ia = 1.f / area;
      const float ua = ((u1 - u0) * (y2 - y0) - (u2 - u0) * (y1 - y0)) * ia;
      const float ub = ((x1 - x0) * (u2 - u0) - (x2 - x0) * (u1 - u0)) * ia;
      const float uc = u0 - ua * x0 - ub * y0;
      const float ulo = std::min({u0, u1, u2}), uhi = std::max({u0, u1, u2});
      // edge functions, positive inside (whatever the winding)
      const float sg = area > 0.f ? 1.f : -1.f;
      const float A0 = -(y1 - y0) * sg, B0 = (x1 - x0) * sg, C0 = -(A0 * x0 + B0 * y0);
      const float A1 = -(y2 - y1) * sg, B1 = (x2 - x1) * sg, C1 = -(A1 * x1 + B1 * y1);
      const float A2 = -(y0 - y2) * sg, B2 = (x0 - x2) * sg, C2 = -(A2 * x2 + B2 * y2);
      for (int j = j0; j <= j1; j++) {
        const float cy = (float)j + 0.5f;
        float cx = (float)i0 + 0.5f;
        float e0 = A0 * cx + B0 * cy + C0, e1 = A1 * cx + B1 * cy + C1, e2 = A2 * cx + B2 * cy + C2;
        float u = ua * cx + ub * cy + uc;
        float* row = m_zb.data() + (size_t)j * W;
        for (int i = i0; i <= i1; i++) {
          if (e0 >= 0.f && e1 >= 0.f && e2 >= 0.f) {
            const float uu = std::clamp(u, ulo, uhi);
            row[i] = std::max(row[i], uu);
          }
          e0 += A0;
          e1 += A1;
          e2 += A2;
          u += ua;
        }
      }
    }
  }
  // the card: one quad per cell with something in it, on the plane (in x, y, 1/w) fitted to the
  // cell's visible texels and moved towards the camera until none is in front of it
  struct Cell {
    int x0, y0, x1, y1;
    float a = 0, b = 0, c = 0;  // 1/w = a x + b y + c (tile pixels)
    float slack_u = 0;          // the plane's largest lead over a texel of it (1/w)
    float over = 0;             // that lead in depth over kSplitDepth of the distance
    float umin = 0, umax = 0;
  };
  auto fit = [&](int x0, int y0, int x1, int y1, Cell* out) -> bool {
    // (float sums: the coordinates are centered on the cell, at most 64 apart)
    float sxx = 0, sxy = 0, syy = 0, sx = 0, sy = 0, sn = 0, su = 0, sxu = 0, syu = 0;
    float umin = 1e30f, umax = 0.f;
    const float mx = 0.5f * (x0 + x1), my = 0.5f * (y0 + y1);
    for (int j = y0; j < y1; j++) {
      const float* row = m_zb.data() + (size_t)j * W;
      const float y = j + 0.5f - my;
      for (int i = x0; i < x1; i++) {
        const float u = row[i];
        if (u > 0.f) {
          const float x = i + 0.5f - mx;
          sxx += x * x;
          sxy += x * y;
          syy += y * y;
          sx += x;
          sy += y;
          sn += 1;
          su += u;
          sxu += x * u;
          syu += y * u;
          umin = std::min(umin, u);
          umax = std::max(umax, u);
        }
      }
    }
    if (sn == 0) {
      return false;
    }
    Cell& cc = *out;
    cc.x0 = x0;
    cc.y0 = y0;
    cc.x1 = x1;
    cc.y1 = y1;
    cc.umin = umin;
    cc.umax = umax;
    double pl[3] = {0, 0, su / sn};
    if (sn >= 3) {
      const double nm[3][3] = {{sxx, sxy, sx}, {sxy, syy, sy}, {sx, sy, sn}};
      const double rhs[3] = {sxu, syu, su};
      double sol[3];
      if (solve3(nm, rhs, sol)) {
        memcpy(pl, sol, sizeof(pl));
      }
    }
    // a steep plane (a sliver of texels) would put the quad's corners far off: flat then
    {
      const double hx = 0.5 * (x1 - x0) + 0.5, hy = 0.5 * (y1 - y0) + 0.5;
      const double ext = std::abs(pl[0]) * hx + std::abs(pl[1]) * hy;
      if (pl[2] - ext < 0.5 * umin || pl[2] + ext > 1.5 * umax) {
        pl[0] = pl[1] = 0;
        pl[2] = umax;
      }
    }
    cc.a = (float)pl[0];
    cc.b = (float)pl[1];
    cc.c = (float)(pl[2] - pl[0] * mx - pl[1] * my);
    // in front of every texel (lead: the most a texel is in front of the fitted plane), then its
    // slack (the most the plane is in front of a texel) from the same pass
    float lead = -1e30f, behind = 0.f;
    for (int j = y0; j < y1; j++) {
      const float* row = m_zb.data() + (size_t)j * W;
      const float rb = cc.b * (j + 0.5f) + cc.c;
      for (int i = x0; i < x1; i++) {
        const float u = row[i];
        if (u > 0.f) {
          const float d = u - (cc.a * (i + 0.5f) + rb);
          lead = std::max(lead, d);
          behind = std::min(behind, d);
        }
      }
    }
    const float shift = lead + 1e-7f * umax;
    cc.c += shift;
    const float slack = shift - behind;
    // that lead in depth (1 - u / plane) against kSplitDepth (bounded: the plane is at least umin)
    const float over = slack / (slack + umin) / kSplitDepth;
    cc.slack_u = slack;
    cc.over = over;
    return true;
  };
  // cells: about kCellTarget pixels, at most kGrid per side, fewer when the card would have more
  // than a quarter of the group's triangles
  const int max_quads = std::clamp((int)c.tris / 8, 2, 32);
  std::vector<Cell> cells;
  int nx = std::clamp((W + kCellTarget / 2) / kCellTarget, 1, kGrid);
  int ny = std::clamp((H + kCellTarget / 2) / kCellTarget, 1, kGrid);
  for (;;) {
    cells.clear();
    for (int cy = 0; cy < ny; cy++) {
      for (int cx = 0; cx < nx; cx++) {
        Cell cc;
        if (fit(W * cx / nx, H * cy / ny, W * (cx + 1) / nx, H * (cy + 1) / ny, &cc)) {
          cells.push_back(cc);
        }
      }
    }
    if ((int)cells.size() <= max_quads || (nx == 1 && ny == 1)) {
      break;
    }
    (nx >= ny ? nx : ny)--;
  }
  if (cells.empty()) {
    return false;  // nothing of it on screen
  }
  // splits: the cell whose plane leads its texels by the most, while there's room
  for (int guard = 0; guard < 64; guard++) {
    int worst = -1;
    for (int k = 0; k < (int)cells.size(); k++) {
      const Cell& cc = cells[k];
      if (cc.over > 1.f && cc.x1 - cc.x0 >= kMinSplitSide && cc.y1 - cc.y0 >= kMinSplitSide &&
          (worst < 0 || cc.over > cells[worst].over)) {
        worst = k;
      }
    }
    if (worst < 0 || (int)cells.size() + 3 > max_quads) {
      break;
    }
    const Cell p = cells[worst];
    cells.erase(cells.begin() + worst);
    const int mx = (p.x0 + p.x1) / 2, my = (p.y0 + p.y1) / 2;
    const int xs[3] = {p.x0, mx, p.x1}, ys[3] = {p.y0, my, p.y1};
    for (int q = 0; q < 4; q++) {
      Cell cc;
      if (fit(xs[q & 1], ys[q >> 1], xs[(q & 1) + 1], ys[(q >> 1) + 1], &cc)) {
        cells.push_back(cc);
      }
    }
    m_st.splits++;
  }
  // the quads' points: on the capture camera's ray through each corner, at the plane's depth
  double Minv[3][3];
  {
    const double M[3][3] = {{m_A[0][0], m_A[0][1], m_A[0][2]},
                            {m_A[1][0], m_A[1][1], m_A[1][2]},
                            {m_A[3][0], m_A[3][1], m_A[3][2]}};
    for (int k = 0; k < 3; k++) {
      double e[3] = {0, 0, 0}, col[3];
      e[k] = 1;
      if (!solve3(M, e, col)) {
        return false;
      }
      for (int r = 0; r < 3; r++) {
        Minv[r][k] = col[r];
      }
    }
  }
  for (int a = 0; a < 3; a++) {
    s.cam[a] = m_cam.trans[a];
  }
  auto point = [&](float tx, float ty, float u, float out[3]) {
    const double X = (w.px0 + (double)tx) / 200.0 - 1.0, Y = (w.py0 + (double)ty) / 120.0 - 1.0;
    const double Wd = 1.0 / (double)u;
    const double rhs[3] = {X * Wd - m_B[0], Y * Wd - m_B[1], Wd - m_B[3]};
    for (int a = 0; a < 3; a++) {
      const double P = Minv[a][0] * rhs[0] + Minv[a][1] * rhs[1] + Minv[a][2] * rhs[2];
      out[a] = (float)(P - s.cam[a]);
    }
  };
  s.pv.clear();
  s.pst.clear();
  s.pv.reserve(cells.size() * 4);
  s.pst.reserve(cells.size() * 4);
  float slack = 0.f, usum = 0.f;
  for (const Cell& cc : cells) {
    slack = std::max(slack, cc.slack_u);
    usum += 0.5f * (cc.umin + cc.umax);
    // half a texel over the neighbours inside the tile (no cracks when the camera moves)
    const float ex0 = cc.x0 > 0 ? cc.x0 - 0.5f : 0.f, ex1 = cc.x1 < W ? cc.x1 + 0.5f : (float)W;
    const float ey0 = cc.y0 > 0 ? cc.y0 - 0.5f : 0.f, ey1 = cc.y1 < H ? cc.y1 + 0.5f : (float)H;
    for (int q = 0; q < 4; q++) {
      const float tx = (q & 1) ? ex1 : ex0, ty = (q & 2) ? ey1 : ey0;
      // (nearer only: still in front of every texel)
      const float u = std::max(cc.a * tx + cc.b * ty + cc.c, 0.5f * cc.umin);
      std::array<float, 3> pt;
      point(tx, ty, u, pt.data());
      s.pv.push_back(pt);
      float st[2];
      ctr_gpu_rt_uv(m_atlas, (float)s.tx + tx, (float)s.ty + ty, &st[0], &st[1]);
      s.pst.push_back({(s16)std::lround(st[0] * 1024.f), (s16)std::lround(st[1] * 1024.f)});
    }
  }
  {
    const float um = usum / (float)cells.size();
    const float cx[3] = {0.f, (float)W, 0.f}, cy[3] = {0.f, 0.f, (float)H};
    for (int k = 0; k < 3; k++) {
      point(cx[k], cy[k], um, s.corner[k]);
    }
  }
  m_st.card_quads += (double)cells.size();
  {
    const double dt = ctr_gpu_time_ms() - tcpu;
    m_cap_cpu_ms += dt;
    m_st.ms_cap_cpu += dt;
  }
  s.qgen = -1;
  // the parallax bound: px per game unit the camera moves, for the texels against the plane in
  // front of them (twice the lateral estimate: covers moving along the view axis)
  {
    const double F = std::max(200.0 * m_rowlen[0], 120.0 * m_rowlen[1]);  // px per (unit / w)
    s.kpar = (float)(2.0 * F * slack);
  }
  // the time of day colors it was drawn with
  s.tod = false;
  if (const CtrLevelData::Tod* t = lev.tod.get()) {
    memcpy(s.tod_w, t->target_w, sizeof(s.tod_w));
    s.tod = true;
    s.tod_ver = t->ver;
  }
  s.wn = (u16)w.wn;
  s.hn = (u16)w.hn;
  s.near = w.near;
  s.captured = m_frame;
  s.value = w.value;
  m_st.captures++;
  m_st.capture_tris += c.tris;
  return true;
}

void CtrImpostors::hide(const Want& w) {
  LevelState& ls = m_levels[m_cur];
  const Cluster& c = ls.clusters[w.cluster];
  const ChunkRec& rec = m_recs[w.rec];
  for (u32 i = 0; i < c.run_count; i++) {
    rec.run_shown[ls.runs[c.first_run + 2 * i]] = 0;
  }
  *rec.partly = true;
  m_st.cards++;
  m_st.card_tris += c.tris;
}

void CtrImpostors::finish_chunks_impl() {
  if (!m_active) {
    return;
  }
  // (the chunk calls since begin_level: timed as one, a clock read costs a system call)
  const double t0 = ctr_gpu_time_ms();
  m_st.cpu_ms += t0 - m_t_chunks;
  m_st.ms_chunk += t0 - m_t_chunks;
  if (m_wants.empty()) {
    return;
  }
  LevelState& ls = m_levels[m_cur];
  const CtrSettings& set = ctr_settings();
  // captures: the groups with a tile first (they were shown as cards), the most triangles first;
  // then new ones, the most triangles per pixel first. Only as many as the budget takes are
  // looked at (their exact boxes and rectangles)
  // (one 64-bit key per want, sorted without looking anything up again: with a tile first, then
  // the triangles or the triangles per pixel, then the index; only the first ones in order)
  std::vector<u64>& keys = m_keys;
  keys.clear();
  for (u32 i = 0; i < m_wants.size(); i++) {
    const Want& w = m_wants[i];
    if (w.capture) {
      const Cluster& c = ls.clusters[w.cluster];
      u32 metric;
      if (c.slot >= 0) {
        metric = std::min<u32>(c.tris, 0x7fffffffu);
      } else {
        float v = std::max(0.f, w.value);
        memcpy(&metric, &v, 4);  // (positive floats sort as their bits)
        metric &= 0x7fffffffu;
      }
      keys.push_back((u64)(c.slot >= 0) << 63 | (u64)metric << 32 | i);
    }
  }
  const size_t head = std::min(keys.size(), (size_t)(2 * kMaxTriesPerFrame));
  if (head < keys.size()) {
    std::nth_element(keys.begin(), keys.begin() + head, keys.end(), std::greater<u64>());
  }
  std::sort(keys.begin(), keys.begin() + head, std::greater<u64>());
  std::vector<u32>& order = m_order;
  order.resize(keys.size());
  for (size_t k = 0; k < keys.size(); k++) {
    order[k] = (u32)keys[k];
  }
  // the shown ones keep their tiles (used this frame) before any capture looks for room
  for (const Want& w : m_wants) {
    const int s = ls.clusters[w.cluster].slot;
    if (s >= 0) {
      m_slots[s].last_used = m_frame;
    }
  }
  const u32 budget = (u32)std::max(0, set.impostor_budget);
  u32 spent = 0;
  int texels = 0;  // rasterized on the CPU this level draw
  int captures = 0, tries = 0, looked = 0;
  std::vector<u8> ok(m_wants.size(), 0);
  for (u32 i = 0; i < m_wants.size(); i++) {
    ok[i] = !m_wants[i].capture;
  }
  // the blended groups' rectangles (from their spheres): no card may hide part of one
  std::vector<Rect> blended(m_seen.size());
  for (size_t j = 0; j < m_seen.size(); j++) {
    sphere_rect(ls.clusters[m_seen[j].cluster], &blended[j]);
  }
  m_rt_open = false;
  const double t_loop = ctr_gpu_time_ms();
  m_st.ms_f_sort += t_loop - t0;
  float fail_value = -1.f;
  const double cap_budget =
      ctr_level_last_build_ms() >= kBusyBuildMs ? kCaptureCpuMsBusy : kCaptureCpuMs;
  const float min_life = ctr_level_last_build_ms() >= kBusyBuildMs ? kMinLifeBusy : kMinLife;
  for (size_t oi = 0; oi < order.size(); oi++) {
    const u32 i = order[oi];
    Want& w = m_wants[i];
    Cluster& c = ls.clusters[w.cluster];
    if (spent >= budget || captures >= kMaxCapturesPerFrame || tries >= kMaxTriesPerFrame) {
      for (size_t r = oi; r < order.size(); r++) {
        m_st.t_budget += ls.clusters[m_wants[order[r]].cluster].tris;
      }
      m_st.over_budget += (double)(order.size() - oi);
      break;  // the rest stay geometry this frame
    }
    if (w.tod_rate > 0.f && (float)kTodMaxErr / w.tod_rate < min_life) {
      c.tod_hold = m_frame + 30;  // (a second, then try again)
    }
    if (m_frame < c.tod_hold) {
      // the day runs too fast for a picture to last (the title screen's): geometry
      m_st.short_life++;
      m_st.t_short += c.tris;
      continue;
    }
    // (a bound on the wants looked at per frame: after a level arrives most are skipped below)
    if (++looked > 4 * kMaxTriesPerFrame) {
      m_st.t_budget += c.tris;
      continue;
    }
    // its vertex colors behind the time of day (the level's rewrites are spread over frames, a
    // jump takes several): not now, or the picture would keep the old colors
    if (tod_behind(ls, c)) {
      m_st.t_pending += c.tris;
      continue;
    }
    tries++;
    if (spent + c.tris > budget) {
      m_st.over_budget++;
      m_st.t_budget += c.tris;
      continue;
    }
    // its exact box (its vertices read once, a few thousand triangles' worth per frame) and its
    // rectangle on screen now
    if (!c.exact) {
      if (m_exact_tris > kExactTrisPerFrame || m_exact_ms > kExactMsPerFrame) {
        m_st.t_pending += c.tris;
        continue;
      }
      const double te = ctr_gpu_time_ms();
      const bool ok_box = exact_box(ls, c);
      m_exact_ms += ctr_gpu_time_ms() - te;
      m_st.ms_exact += ctr_gpu_time_ms() - te;
      if (!ok_box) {
        c.eligible = false;
        continue;
      }
    }
    Proj pj;
    project_box(c.bmin, c.bmax, &pj);
    if (!pj.front) {
      m_st.t_near += c.tris;
      continue;
    }
    if (pj.xmax < 0 || pj.xmin > 400 || pj.ymax < 0 || pj.ymin > 240) {
      m_st.t_off += c.tris;
      continue;  // off screen: no card needed
    }
    w.px0 = (int)std::floor(pj.xmin) - 1;
    w.py0 = (int)std::floor(pj.ymin) - 1;
    w.wn = (int)std::ceil(pj.xmax) + 1 - w.px0;
    w.hn = (int)std::ceil(pj.ymax) + 1 - w.py0;
    w.zmin = pj.zmin;
    w.zmax = pj.zmax;
    w.wmin = pj.wmin;
    w.wmax = pj.wmax;
    if (w.wn > kMaxTile || w.hn > kMaxTile) {
      m_st.too_big++;
      m_st.t_big += c.tris;
      if (c.slot >= 0) {
        free_slot(c.slot);
      }
      continue;
    }
    w.value = (float)c.tris / (float)(w.wn * w.hn);
    // how long the card would stay right with the camera moving as it does: the parallax bound
    // of its last capture (else a quarter of its box's depth range's) against impostor_error, and
    // 15% of its distance for the size
    if (m_speed > 1.f) {
      const double F = std::max(200.0 * m_rowlen[0], 120.0 * m_rowlen[1]);
      const float kp = c.kpar >= 0.f
                           ? c.kpar
                           : (float)(0.5 * F * (1.0 / std::max(1e-3f, w.wmin) - 1.0 / w.wmax));
      const float life_err = kp > 0.f ? set.impostor_error / (kp * m_speed) : 1e9f;
      const float life_size = kScaleTolerance * w.wmin / (m_rowlen[3] * m_speed);
      if (std::min(life_err, life_size) < min_life) {
        m_st.short_life++;
        m_st.t_short += c.tris;
        continue;
      }
    }
    // a blended (or decal) group that may be in front of parts of it: no picture (its texels
    // there would cover the blended one, or be wrong behind it)
    bool blocked = false;
    for (const Rect& o : blended) {
      if (!(o.x1 < w.px0 || o.x0 > w.px0 + w.wn || o.y1 < w.py0 || o.y0 > w.py0 + w.hn ||
            o.w0 >= w.wmax || o.w1 <= w.wmin)) {
        blocked = true;
        break;
      }
    }
    if (blocked) {
      m_st.t_blended += c.tris;
      if (c.slot >= 0) {
        free_slot(c.slot);
      }
      continue;
    }
    // its tile: the one it has if the size fits, else a new one
    if (c.slot >= 0) {
      const Slot& old = m_slots[c.slot];
      if (w.wn > old.cw || w.hn > old.ch || tile_width(w.wn) + 16 < old.cw ||
          tile_height(w.hn) + 16 < old.ch) {
        free_slot(c.slot);
      }
    }
    if (texels + w.wn * w.hn > kRasterPerFrame || m_cap_cpu_ms > cap_budget) {
      m_st.over_budget++;
      m_st.t_budget += c.tris;
      continue;
    }
    if (c.slot < 0 && w.value <= fail_value) {
      m_st.no_room++;
      m_st.t_room += c.tris;
      continue;  // a tile for one with more triangles per texel was just not found
    }
    if (c.slot < 0) {
      const int s = alloc_slot(w.wn, w.hn, w.value, m_frame);
      if (s < 0) {
        m_st.no_room++;
        m_st.t_room += c.tris;
        fail_value = std::max(fail_value, w.value);
        c.retry = m_frame + 15;  // (half a second before it looks for room again)
        continue;
      }
      m_slots[s].level = m_cur;
      m_slots[s].cluster = w.cluster;
      c.slot = s;
    }
    if (!m_rt_open) {
      // the captures without fog (the card gets it)
      static const float kNoFog0[4] = {0.f, 255.f, 255.f, -1.f / 255.f};
      static const float kNoFog1[4] = {0.f, 1.f, 0.f, 0.f};
      ctr_gpu_set_mesh_fog(kNoFog0, kNoFog1, m_cam.fog_rgb[0], m_cam.fog_rgb[1], m_cam.fog_rgb[2]);
      m_rt_open = true;
    }
    Slot& s = m_slots[c.slot];
    const double tc = ctr_gpu_time_ms();
    const bool captured = capture(w, s);
    m_st.ms_capture += ctr_gpu_time_ms() - tc;
    if (!captured) {
      free_slot(c.slot);
      continue;
    }
    s.last_used = m_frame;
    c.kpar = s.kpar;
    spent += c.tris;
    texels += w.wn * w.hn;
    captures++;
    ok[i] = 1;
  }
  if (m_rt_open) {
    m_rt_open = false;
    ctr_gpu_rt_end();
    if (m_cam.fog0 && m_cam.fog1) {
      ctr_gpu_set_mesh_fog(m_cam.fog0, m_cam.fog1, m_cam.fog_rgb[0], m_cam.fog_rgb[1],
                           m_cam.fog_rgb[2]);
    }
  }
  const double t_hide = ctr_gpu_time_ms();
  m_st.ms_f_loop += t_hide - t_loop;
  m_card_verts = 0;
  // the quantization origin near the camera
  {
    bool far_off = !m_qvalid;
    for (int a = 0; a < 3 && !far_off; a++) {
      far_off = std::abs((double)m_cam.trans[a] - m_qorigin[a]) > kQMove;
    }
    if (far_off) {
      for (int a = 0; a < 3; a++) {
        m_qorigin[a] = std::round((double)m_cam.trans[a] / kQGrid) * kQGrid;
      }
      m_qgen++;
      m_qvalid = true;
    }
  }
  for (u32 i = 0; i < m_wants.size(); i++) {
    const int s = ls.clusters[m_wants[i].cluster].slot;
    if (ok[i] && s >= 0 && (int)m_cards.size() < kMaxCards) {
      const int nv = (int)m_slots[s].pv.size();
      if (!quantize(m_slots[s])) {
        continue;  // (too far from the origin) stays geometry
      }
      if (m_card_verts + nv > kMaxCardVerts) {
        m_st.verts_full++;  // (the frame's vertex stream) stays geometry
        continue;
      }
      m_card_verts += nv;
      hide(m_wants[i]);
      m_slots[s].last_used = m_frame;
      m_cards.push_back(s);
    }
  }
  {
    const double dt = ctr_gpu_time_ms() - t0;
    m_st.cpu_ms += dt;
    m_st.ms_finish += dt;
  }
}

void CtrImpostors::draw_cards_impl() {
  if (!m_active) {
    return;
  }
  m_active = false;
  if (m_cards.empty()) {
    return;
  }
  const double t0 = ctr_gpu_time_ms();
  const int n = (int)m_cards.size();
  const int nverts = m_card_verts, nquads = nverts / 4;
  if (!nquads) {
    return;
  }
  c3l::Vertex* v = (c3l::Vertex*)ctr_gpu_stream_alloc(nverts * (int)sizeof(c3l::Vertex));
  u16* idx = (u16*)ctr_gpu_stream_alloc(nquads * 6 * (int)sizeof(u16));
  if (!v || !idx) {
    return;
  }
  // the slots' quantized vertices (kQStep around m_qorigin), copied
  const float scale = kQStep;
  const double* origin = m_qorigin;
  c3l::Vertex* cv = v;
  u16* ci = idx;
  for (int i = 0; i < n; i++) {
    const Slot& s = m_slots[m_cards[i]];
    const u16 base = (u16)(cv - v);
    const size_t np = s.qv.size();
    memcpy(cv, s.qv.data(), np * sizeof(c3l::Vertex));
    cv += np;
    // quads x0y0 x1y0 x0y1 x1y1: (0 1 3) (0 3 2), two indices per word
    u32* cw = (u32*)ci;
    for (u16 q = 0; q < (u16)(np / 4); q++) {
      const u32 a = (u32)(base + 4 * q);
      cw[0] = a | (a + 1) << 16;
      cw[1] = (a + 3) | a << 16;
      cw[2] = (a + 3) | (a + 2) << 16;
      cw += 3;
    }
    ci = (u16*)cw;
  }
  const double tb = ctr_gpu_time_ms();
  m_st.ms_cards += tb - t0;
  // clip = -(R * (origin + q * scale - cam)) as the level chunks (CtrTfragRenderer::draw_level)
  float m[16];
  for (int c = 0; c < 4; c++) {
    double t = 0;
    for (int i = 0; i < 3; i++) {
      m[4 * c + i] = -m_cam.R[i][c] * scale;
      t += (double)m_cam.R[i][c] * (origin[i] - (double)m_cam.trans[i]);
    }
    if (c < 3) {
      t += m_cam.R[3][c];
    }
    m[4 * c + 3] = (float)-t;
  }
  for (int i = 0; i < 4; i++) {
    m[4 + i] *= m_cam.y_scale;
  }
  ctr_mesh_matrix mm;
  ctr_gpu_prepare_mesh_matrix(m, &mm);
  ctr_draw_state st{};
  st.tex = m_atlas;
  st.tcc = 1 | CTR_TCC_ALPHA80;
  st.filter = 0;
  st.clamp_s = st.clamp_t = 1;
  st.blend = CTR_BLEND_OFF;
  st.atest = CTR_TEST_GEQUAL;
  st.aref = 1;
  st.ztest = CTR_TEST_GEQUAL;
  st.zwrite = 1;
#ifndef __3DS__
  ctr_gpu_soft_bucket("impostor-cards");
  {
    // debugging (host): the atlas as a PNG every few seconds (CTR_IMP_DEBUG = a folder)
    static const char* dir = getenv("CTR_IMP_DEBUG");
    static int count = 0;
    if (dir && count++ % 120 == 1) {
      ctr_gpu_soft_dump_tex(m_atlas, (std::string(dir) + "/atlas.png").c_str());
    }
  }
#endif
  // the fog stage on (fog_stage_skip may have left it off for a near chunk's draw: the cards are
  // far, their fog is most of their color)
  ctr_gpu_set_mesh_nofog(0);
  ctr_gpu_draw_mesh_stream(&st, &mm, v, idx, nquads * 6);
  m_st.drawn_quads += nquads;
  {
    const double t1 = ctr_gpu_time_ms();
    m_st.cpu_ms += t1 - t0;
    m_st.ms_card_draw += t1 - tb;
  }
}

void CtrImpostors::log_stats() {
  if (!m_st.frames) {
    return;
  }
  const double f = m_st.frames;
  int slots = 0;
  for (const Slot& s : m_slots) {
    slots += s.used;
  }
  lg::info(
      "[ctr] impostors: {:.1f} cards ({:.0f} tris as {:.0f}) per frame, {:.2f} captures ({:.0f} tris), "
      "recaptures: moved {:.2f} size {:.2f}, over budget {:.2f}, no room {:.2f}, too big {:.1f}, "
      "evicted {:.2f}; atlas {} tiles {:.0f}% ({}x{}); groups made for {:.0f} chunks ({:.1f} ms); "
      "tris per frame not as cards: not opaque {:.0f}, hidden {:.0f}, nearer {:.0f}, too big "
      "{:.0f}, off screen {:.0f}, over budget {:.0f}, no room {:.0f}, next to blended {:.0f}, box "
      "pending {:.0f}, light/small {:.0f}; card quads per capture {:.1f} (splits {:.2f}), "
      "recaptures for time of day {:.2f}, for the tie LOD {:.2f}, cards over the vertex cap {:.1f}, camera too fast "
      "{:.0f} tris; render thread "
      "ms/frame {:.3f}: chunks {:.3f} finish {:.3f} (sort {:.3f} loop {:.3f}, captures {:.3f}, their "
      "CPU part {:.3f}, exact boxes {:.3f}) cards "
      "{:.3f} (+ their draw call {:.3f})",
      m_st.cards / f, m_st.card_tris / f, 2.0 * m_st.drawn_quads / f, m_st.captures / f, m_st.capture_tris / f,
      m_st.recapture_error / f, m_st.recapture_scale / f, m_st.over_budget / f, m_st.no_room / f,
      m_st.too_big / f, m_st.evicted / f, slots,
      m_lw * m_lh ? 100.0 * m_texels / (m_lw * m_lh) : 0.0, m_lw, m_lh, m_st.built_chunks,
      m_st.build_ms, m_st.t_opaque_no / f, m_st.t_hidden / f, m_st.t_near / f, m_st.t_big / f,
      m_st.t_off / f, m_st.t_budget / f, m_st.t_room / f, m_st.t_blended / f, m_st.t_pending / f,
      m_st.t_light / f, m_st.captures ? m_st.card_quads / m_st.captures : 0.0,
      m_st.captures ? m_st.splits / m_st.captures : 0.0, m_st.recapture_tod / f,
      m_st.recapture_lod / f, m_st.verts_full / f, m_st.t_short / f, m_st.cpu_ms / f, m_st.ms_chunk / f, m_st.ms_finish / f, m_st.ms_f_sort / f, m_st.ms_f_loop / f, m_st.ms_capture / f,
      m_st.ms_cap_cpu / f, m_st.ms_exact / f, m_st.ms_cards / f, m_st.ms_card_draw / f);
  m_st = Stats();
}
