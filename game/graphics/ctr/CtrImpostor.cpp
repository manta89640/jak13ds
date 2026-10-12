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
constexpr u32 kExactTrisPerFrame = 3000;  // groups' exact boxes made per frame (their triangles)
constexpr u64 kSlotTimeout = 600;      // frames a tile is kept without being shown
constexpr int kMaxCards = 512;         // per level draw
constexpr float kScaleTolerance = 0.15f;

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

void CtrImpostors::begin_level(CtrLevelData& lev, const Camera& cam, u64 frame) {
  m_active = false;
  const CtrSettings& set = ctr_settings();
  if (!set.impostors || m_failed || !ensure_atlas()) {
    return;
  }
  if (frame != m_frame) {
    m_frame = frame;
    m_builds = 0;
    m_exact_tris = 0;
    m_st.frames++;
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
      if (r < lev.run_spheres.size() && lev.run_spheres[r].r != 0xffff) {
        const auto& sp = lev.run_spheres[r];
        for (int a = 0; a < 3; a++) {
          mn[a] = std::min(mn[a], ch.origin[a] + (sp.c[a] - (float)sp.r) * ch.scale);
          mx[a] = std::max(mx[a], ch.origin[a] + (sp.c[a] + (float)sp.r) * ch.scale);
        }
      } else {
        c.eligible = false;  // no sphere: nothing known about it
      }
      c.tris += n / 3;
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
  for (u32 i = 0; i < c.run_count; i++) {
    const u32 r = ls.runs[c.first_run + 2 * i];
    const u32 f = ls.runs[c.first_run + 2 * i + 1];
    const u32 n = lev.vis_runs[r].index_count;
    if (f + n > (u32)ni) {
      return false;
    }
    for (u32 k = f; k < f + n; k++) {
      const u16 v = ip[k];
      if (v >= (u32)nv) {
        return false;
      }
      const s16* p = verts[v].pos;
      for (int a = 0; a < 3; a++) {
        mn[a] = std::min<int>(mn[a], p[a]);
        mx[a] = std::max<int>(mx[a], p[a]);
      }
    }
  }
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

void CtrImpostors::chunk(u32 ci, const float clip[16], bool runs_valid, u8* run_shown,
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
  const double t0 = ctr_gpu_time_ms();
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
    bool shown = !runs_valid;
    for (u32 i = 0; i < c.run_count && !shown; i++) {
      shown = run_shown[ls.runs[c.first_run + 2 * i]] != 0;
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
    const bool had = c.slot >= 0 && m_slots[c.slot].last_used + 1 >= m_frame;
    const float lim = (float)((had ? 0.9 : 1.0) * D) + c.rad;  // its nearest point beyond D
    if (d2 < lim * lim) {
      m_st.t_near += c.tris;
      continue;
    }
    Want w;
    w.cluster = k;
    w.rec = rec_index;
    w.capture = true;
    if (had) {
      // the card still right? Nothing changed when the camera didn't move; else the parallax
      // bound for how far it moved since the capture, and the card's size on screen
      const Slot& sl = m_slots[c.slot];
      bool valid = m_cam_same;
      if (!valid) {
        float mv2 = 0.f;
        for (int a = 0; a < 3; a++) {
          const float d = (float)(sl.cam[a] - (double)m_cam.trans[a]);
          mv2 += d * d;
        }
        if (sl.kpar * sl.kpar * mv2 > E * E) {
          m_st.recapture_error++;
        } else if (std::abs(card_scale(sl) - 1.f) > kScaleTolerance) {
          m_st.recapture_scale++;
        } else {
          valid = true;
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
  {
    const double dt = ctr_gpu_time_ms() - t0;
    m_st.cpu_ms += dt;
    m_st.ms_chunk += dt;
  }
}

// the card's size on screen now against the capture's (texels per pixel): its outer points
float CtrImpostors::card_scale(const Slot& s) const {
  static const int kCorner[3] = {0, kGrid, kGrid * (kGrid + 1)};
  float px[3][2];
  for (int k = 0; k < 3; k++) {
    float d[3], q[4];
    for (int a = 0; a < 3; a++) {
      d[a] = (float)(s.cam[a] - (double)m_cam.trans[a]) + s.gv[kCorner[k]][a];
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
  // the group's triangles: drawn into the tile, and binned into the grid's cells (each cell: the
  // nearest and farthest clip w of the triangles whose screen box touches it)
  constexpr int N = kGrid;
  float cmin[N * N], cmax[N * N];
  for (int i = 0; i < N * N; i++) {
    cmin[i] = 1e30f;
    cmax[i] = -1e30f;
  }
  const float sx = (float)N / (float)w.wn, sy = (float)N / (float)w.hn;
  const float* cl = rec.clip;
  for (u32 i = 0; i < c.run_count; i++) {
    const u32 r = ls.runs[c.first_run + 2 * i];
    const u32 first = ls.runs[c.first_run + 2 * i + 1];
    const u32 n = lev.vis_runs[r].index_count;
    const u32 di = ch.first_draw + lev.vis_runs[r].draw;
    ctr_gpu_draw_mesh_prepared(&lev.draw_states[di], &mm, mesh, (int)first, (int)n);
    if (first + n > (u32)ni) {
      continue;
    }
    for (u32 t = first; t + 2 < first + n; t += 3) {
      float tx0 = 1e30f, tx1 = -1e30f, ty0 = 1e30f, ty1 = -1e30f, tw0 = 1e30f, tw1 = -1e30f;
      bool okt = true;
      for (int v = 0; v < 3; v++) {
        const u16 vi = ip[t + v];
        if (vi >= (u32)nv) {
          okt = false;
          break;
        }
        const s16* p = verts[vi].pos;
        const float qx = cl[0] * p[0] + cl[1] * p[1] + cl[2] * p[2] + cl[3];
        const float qy = cl[4] * p[0] + cl[5] * p[1] + cl[6] * p[2] + cl[7];
        const float qw = cl[12] * p[0] + cl[13] * p[1] + cl[14] * p[2] + cl[15];
        if (!(qw > 1e-3f)) {
          okt = false;
          break;
        }
        const float iw = 1.f / qw;
        const float x = 200.f * (1.f + qx * iw) - (float)w.px0;
        const float y = 120.f * (1.f + qy * iw) - (float)w.py0;
        tx0 = std::min(tx0, x);
        tx1 = std::max(tx1, x);
        ty0 = std::min(ty0, y);
        ty1 = std::max(ty1, y);
        tw0 = std::min(tw0, qw);
        tw1 = std::max(tw1, qw);
      }
      if (!okt) {
        continue;
      }
      const int cx0 = std::clamp((int)std::floor(tx0 * sx), 0, N - 1);
      const int cx1 = std::clamp((int)std::floor(tx1 * sx), 0, N - 1);
      const int cy0 = std::clamp((int)std::floor(ty0 * sy), 0, N - 1);
      const int cy1 = std::clamp((int)std::floor(ty1 * sy), 0, N - 1);
      for (int cy = cy0; cy <= cy1; cy++) {
        for (int cx = cx0; cx <= cx1; cx++) {
          float& a = cmin[cy * N + cx];
          float& b = cmax[cy * N + cx];
          a = std::min(a, tw0);
          b = std::max(b, tw1);
        }
      }
    }
  }
  // the grid's points: at the nearest depth of the cells around each (a cell's four points are
  // never behind anything in it); points with no triangle around them at the group's nearest
  float gw[kGridV];
  float wnear = 1e30f;
  for (int i = 0; i < N * N; i++) {
    wnear = std::min(wnear, cmin[i]);
  }
  if (!(wnear < 1e29f)) {
    return false;  // nothing of it on screen
  }
  for (int j = 0; j <= N; j++) {
    for (int i = 0; i <= N; i++) {
      float v = 1e30f;
      for (int cy = j - 1; cy <= j; cy++) {
        for (int cx = i - 1; cx <= i; cx++) {
          if (cx >= 0 && cy >= 0 && cx < N && cy < N) {
            v = std::min(v, cmin[cy * N + cx]);
          }
        }
      }
      gw[j * (N + 1) + i] = v < 1e29f ? v : wnear;
    }
  }
  const double M[3][3] = {{m_A[0][0], m_A[0][1], m_A[0][2]},
                          {m_A[1][0], m_A[1][1], m_A[1][2]},
                          {m_A[3][0], m_A[3][1], m_A[3][2]}};
  for (int a = 0; a < 3; a++) {
    s.cam[a] = m_cam.trans[a];
  }
  for (int j = 0; j <= N; j++) {
    for (int i = 0; i <= N; i++) {
      const int k = j * (N + 1) + i;
      const double px = w.px0 + (double)w.wn * i / N, py = w.py0 + (double)w.hn * j / N;
      const double X = px / 200.0 - 1.0, Y = py / 120.0 - 1.0, W = gw[k];
      const double rhs[3] = {X * W - m_B[0], Y * W - m_B[1], W - m_B[3]};
      double P[3];
      if (!solve3(M, rhs, P)) {
        return false;
      }
      for (int a = 0; a < 3; a++) {
        s.gv[k][a] = (float)(P[a] - s.cam[a]);
      }
      float st[2];
      ctr_gpu_rt_uv(m_atlas, (float)(s.tx + (px - w.px0)), (float)(s.ty + (py - w.py0)), &st[0],
                    &st[1]);
      s.gst[k][0] = (s16)std::lround(st[0] * 1024.f);
      s.gst[k][1] = (s16)std::lround(st[1] * 1024.f);
    }
  }
  // the parallax bound: px per game unit the camera moves, for the cells' triangles against the
  // grid in front of them (twice the lateral estimate: covers moving along the view axis)
  {
    double spread = 0;
    for (int cy = 0; cy < N; cy++) {
      for (int cx = 0; cx < N; cx++) {
        if (!(cmin[cy * N + cx] < 1e29f)) {
          continue;
        }
        float v = 1e30f;
        for (int j = cy; j <= cy + 1; j++) {
          for (int i = cx; i <= cx + 1; i++) {
            v = std::min(v, gw[j * (N + 1) + i]);
          }
        }
        spread = std::max(spread, 1.0 / std::max(1e-3f, v) - 1.0 / cmax[cy * N + cx]);
      }
    }
    const double F = std::max(200.0 * m_rowlen[0], 120.0 * m_rowlen[1]);  // px per (unit / w)
    s.kpar = (float)(2.0 * F * spread);
  }
  s.wn = (u16)w.wn;
  s.hn = (u16)w.hn;
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

void CtrImpostors::finish_chunks() {
  if (!m_active || m_wants.empty()) {
    return;
  }
  const double t0 = ctr_gpu_time_ms();
  LevelState& ls = m_levels[m_cur];
  const CtrSettings& set = ctr_settings();
  // captures: the groups with a tile first (they were shown as cards), the most triangles first;
  // then new ones, the most triangles per pixel first. Only as many as the budget takes are
  // looked at (their exact boxes and rectangles)
  std::vector<u32> order;
  for (u32 i = 0; i < m_wants.size(); i++) {
    if (m_wants[i].capture) {
      order.push_back(i);
    }
  }
  std::sort(order.begin(), order.end(), [&](u32 a, u32 b) {
    const Want& wa = m_wants[a];
    const Want& wb = m_wants[b];
    const bool ha = ls.clusters[wa.cluster].slot >= 0, hb = ls.clusters[wb.cluster].slot >= 0;
    if (ha != hb) {
      return ha;
    }
    return ha ? ls.clusters[wa.cluster].tris > ls.clusters[wb.cluster].tris : wa.value > wb.value;
  });
  // the shown ones keep their tiles (used this frame) before any capture looks for room
  for (const Want& w : m_wants) {
    const int s = ls.clusters[w.cluster].slot;
    if (s >= 0) {
      m_slots[s].last_used = m_frame;
    }
  }
  const u32 budget = (u32)std::max(0, set.impostor_budget);
  u32 spent = 0;
  int captures = 0, tries = 0;
  std::vector<u8> ok(m_wants.size(), 0);
  for (u32 i = 0; i < m_wants.size(); i++) {
    ok[i] = !m_wants[i].capture;
  }
  // the blended groups' rectangles (from their spheres): no card may hide part of one
  std::vector<Rect> blended(m_seen.size());
  for (size_t j = 0; j < m_seen.size(); j++) {
    sphere_rect(ls.clusters[m_seen[j].cluster], &blended[j]);
  }
  bool rt_open = false;
  float fail_value = -1.f;
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
    tries++;
    if (spent + c.tris > budget) {
      m_st.over_budget++;
      m_st.t_budget += c.tris;
      continue;
    }
    // its exact box (its vertices read once, a few thousand triangles' worth per frame) and its
    // rectangle on screen now
    if (!c.exact) {
      if (m_exact_tris > kExactTrisPerFrame) {
        m_st.t_pending += c.tris;
        continue;
      }
      m_exact_tris += c.tris;
      if (!exact_box(ls, c)) {
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
        continue;
      }
      m_slots[s].level = m_cur;
      m_slots[s].cluster = w.cluster;
      c.slot = s;
    }
    if (!rt_open) {
      // the captures without fog (the card gets it)
      static const float kNoFog0[4] = {0.f, 255.f, 255.f, -1.f / 255.f};
      static const float kNoFog1[4] = {0.f, 1.f, 0.f, 0.f};
      ctr_gpu_set_mesh_fog(kNoFog0, kNoFog1, m_cam.fog_rgb[0], m_cam.fog_rgb[1], m_cam.fog_rgb[2]);
      rt_open = true;
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
    spent += c.tris;
    captures++;
    ok[i] = 1;
  }
  if (rt_open) {
    ctr_gpu_rt_end();
    if (m_cam.fog0 && m_cam.fog1) {
      ctr_gpu_set_mesh_fog(m_cam.fog0, m_cam.fog1, m_cam.fog_rgb[0], m_cam.fog_rgb[1],
                           m_cam.fog_rgb[2]);
    }
  }
  for (u32 i = 0; i < m_wants.size(); i++) {
    const int s = ls.clusters[m_wants[i].cluster].slot;
    if (ok[i] && s >= 0 && (int)m_cards.size() < kMaxCards) {
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

void CtrImpostors::draw_cards() {
  if (!m_active) {
    return;
  }
  m_active = false;
  if (m_cards.empty()) {
    return;
  }
  const double t0 = ctr_gpu_time_ms();
  constexpr int N = kGrid;
  const int n = (int)m_cards.size();
  c3l::Vertex* v = (c3l::Vertex*)ctr_gpu_stream_alloc(n * kGridV * (int)sizeof(c3l::Vertex));
  u16* idx = (u16*)ctr_gpu_stream_alloc(n * N * N * 6 * (int)sizeof(u16));
  if (!v || !idx) {
    return;
  }
  // positions relative to the camera, in the smallest power of two step that holds them
  float far = 0.f;
  for (int i = 0; i < n; i++) {
    const Slot& s = m_slots[m_cards[i]];
    float d[3];
    for (int a = 0; a < 3; a++) {
      d[a] = (float)(s.cam[a] - (double)m_cam.trans[a]);
    }
    for (int k = 0; k < kGridV; k++) {
      for (int a = 0; a < 3; a++) {
        far = std::max(far, std::abs(d[a] + s.gv[k][a]));
      }
    }
  }
  float scale = 1.f;
  while (scale * 32000.f < far) {
    scale *= 2.f;
  }
  double origin[3];
  for (int a = 0; a < 3; a++) {
    origin[a] = std::round(m_cam.trans[a] / scale) * scale;
  }
  const float inv = 1.f / scale;
  for (int i = 0; i < n; i++) {
    const Slot& s = m_slots[m_cards[i]];
    float o[3];
    for (int a = 0; a < 3; a++) {
      o[a] = (float)(s.cam[a] - origin[a]);
    }
    c3l::Vertex* cv = v + i * kGridV;
    for (int k = 0; k < kGridV; k++) {
      for (int a = 0; a < 3; a++) {
        const float q = (o[a] + s.gv[k][a]) * inv;
        cv[k].pos[a] = (s16)(q < 0.f ? std::max(-32767.f, q - 0.5f) : std::min(32767.f, q + 0.5f));
      }
      cv[k].pad = 0;
      cv[k].st[0] = s.gst[k][0];
      cv[k].st[1] = s.gst[k][1];
      cv[k].rgba[0] = cv[k].rgba[1] = cv[k].rgba[2] = cv[k].rgba[3] = 0x80;
    }
    u16* ci = idx + i * N * N * 6;
    const u16 base = (u16)(i * kGridV);
    for (int cy = 0; cy < N; cy++) {
      for (int cx = 0; cx < N; cx++) {
        const u16 a = (u16)(base + cy * (N + 1) + cx), b = (u16)(a + 1), c = (u16)(a + N + 1),
                  d = (u16)(c + 1);
        ci[0] = a;
        ci[1] = b;
        ci[2] = d;
        ci[3] = a;
        ci[4] = d;
        ci[5] = c;
        ci += 6;
      }
    }
  }
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
  ctr_gpu_draw_mesh_stream(&st, &mm, v, idx, n * N * N * 6);
  {
    const double dt = ctr_gpu_time_ms() - t0;
    m_st.cpu_ms += dt;
    m_st.ms_cards += dt;
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
      "[ctr] impostors: {:.1f} cards ({:.0f} tris) per frame, {:.2f} captures ({:.0f} tris), "
      "recaptures: moved {:.2f} size {:.2f}, over budget {:.2f}, no room {:.2f}, too big {:.1f}, "
      "evicted {:.2f}; atlas {} tiles {:.0f}% ({}x{}); groups made for {:.0f} chunks ({:.1f} ms); "
      "tris per frame not as cards: not opaque {:.0f}, hidden {:.0f}, nearer {:.0f}, too big "
      "{:.0f}, off screen {:.0f}, over budget {:.0f}, no room {:.0f}, next to blended {:.0f}, box "
      "pending {:.0f}; render thread ms/frame {:.3f}: chunks {:.3f} finish {:.3f} (captures {:.3f}) "
      "cards {:.3f}",
      m_st.cards / f, m_st.card_tris / f, m_st.captures / f, m_st.capture_tris / f,
      m_st.recapture_error / f, m_st.recapture_scale / f, m_st.over_budget / f, m_st.no_room / f,
      m_st.too_big / f, m_st.evicted / f, slots,
      m_lw * m_lh ? 100.0 * m_texels / (m_lw * m_lh) : 0.0, m_lw, m_lh, m_st.built_chunks,
      m_st.build_ms, m_st.t_opaque_no / f, m_st.t_hidden / f, m_st.t_near / f, m_st.t_big / f,
      m_st.t_off / f, m_st.t_budget / f, m_st.t_room / f, m_st.t_blended / f, m_st.t_pending / f,
      m_st.cpu_ms / f, m_st.ms_chunk / f, m_st.ms_finish / f, m_st.ms_capture / f,
      m_st.ms_cards / f);
  m_st = Stats();
}
