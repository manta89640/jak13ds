/*!
 * @file CtrShadowJak2.cpp
 * (AI-assisted)
 * See CtrShadowJak2.h. The DMA as Shadow2::render reads it.
 */

#include "CtrShadowJak2.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "common/dma/dma.h"
#include "common/log/log.h"

#include "game/graphics/ctr/CtrSettings.h"

namespace {
// Shadow2.h: VU addresses of the uploads
constexpr int kTopVertexDataAddr = 4;
constexpr int kBottomVertexDataAddr = 174;
constexpr int kCapIndexDataAddr = 344;
constexpr int kWallIndexDataAddr = 600;
// the frame's vertex memory taken (u16 indices)
constexpr int kReserveVerts = 12000;
constexpr int kReserveIndices = 18000;
}  // namespace

CtrShadowJak2Renderer::CtrShadowJak2Renderer(std::string name, int id)
    : CtrBucketRenderer(std::move(name), id) {
  memset(m_clip, 0, sizeof(m_clip));
}

bool CtrShadowJak2Renderer::reserve() {
  if (!m_reserved && !m_full) {
    // (AI-assisted, j2gaps) the whole reservation is ~372 KB of the frame's 512 KB of vertex
    // memory: after a busy particle bucket (SHADOW2 comes after PARTICLES) it didn't fit and the
    // bucket drew no shadow at all. Smaller ones before giving up (a frame's shadow volumes
    // usually take a few thousand vertices); the room not used goes back after the draw.
    for (int div = 1; div <= 8 && !m_reserved; div *= 2) {
      m_reserved = ctr_gpu_clip_reserve(kReserveVerts / div, kReserveIndices / div, &m_verts,
                                        &m_indices) != 0;
      if (m_reserved) {
        m_cap_verts = kReserveVerts / div;
        m_cap_indices = kReserveIndices / div;
      }
    }
    m_full = !m_reserved;
  }
  return m_reserved;
}

namespace {
// Shadow2: the normal of (p0, p1, p2) against the direction to it (the camera is at the origin)
// says which pass a face belongs to
bool faces_away(const float* p0, const float* p1, const float* p2) {
  const float ax = p1[0] - p0[0], ay = p1[1] - p0[1], az = p1[2] - p0[2];
  const float bx = p2[0] - p0[0], by = p2[1] - p0[1], bz = p2[2] - p0[2];
  const float nx = ay * bz - az * by, ny = az * bx - ax * bz, nz = ax * by - ay * bx;
  return nx * p0[0] + ny * p0[1] + nz * p0[2] > 0.f;
}
}  // namespace

int CtrShadowJak2Renderer::vertex(const u8* verts, int idx, bool bottom) {
  // (AI-assisted, j2gpu) each camera space vertex of the upload once in the frame's vertex
  // memory (the GPU's clip program applies m_clip: ctr_gpu_clip_draw_ranges_mtx), shared by the
  // faces that use it
  const int k = bottom ? 1 : 0;
  if (m_stamp[k][idx] == m_gen[k]) {
    return m_map[k][idx];
  }
  const float* p = (const float*)(verts + 16 * idx);
  // (AI-assisted, j2gpu + gpuopt) the camera space box of this volume (the darkening rectangle)
  Box& b = m_boxes.back();
  for (int c = 0; c < 3; c++) {
    b.lo[c] = std::min(b.lo[c], p[c]);
    b.hi[c] = std::max(b.hi[c], p[c]);
  }
  ctr_clip_vertex& v = m_verts[m_nv];
  v.x = p[0];
  v.y = p[1];
  v.z = p[2];
  v.w = 1.f;
  v.s = v.t = 0.f;
  v.r = v.g = v.b = v.a = 0;
  m_stamp[k][idx] = m_gen[k];
  m_map[k][idx] = (u16)m_nv;
  return m_nv++;
}

void CtrShadowJak2Renderer::add_face(const Corner* c, int n, bool away) {
  const int tris = n - 2;
  if (!reserve() || m_nv + n + 4 > m_cap_verts ||
      (int)(m_front.size() + m_back.size()) + 3 * tris + 6 > m_cap_indices) {
    m_stats.dropped++;
    return;
  }
  int vi[4];
  for (int i = 0; i < n; i++) {
    vi[i] = vertex(c[i].verts, c[i].idx, c[i].bottom);
  }
  auto& list = away ? m_back : m_front;
  // a strip: (0 1 2), (1 2 3)
  for (int t = 0; t < tris; t++) {
    list.push_back((u16)vi[t]);
    list.push_back((u16)vi[t + 1]);
    list.push_back((u16)vi[t + 2]);
  }
  m_stats.faces++;
}

void CtrShadowJak2Renderer::add_caps(const u8* idx, const u8* verts, bool flip, bool flippable,
                                     bool bottom) {
  // Shadow2::add_cap_tris / add_flippable_tris
  const int n = idx[0];
  idx += 4;
  for (int i = 0; i < n; i++, idx += 4) {
    const int a = idx[0], b = idx[1], c = idx[2];
    if (!a && !b && !c) {
      continue;  // padding of the unpack
    }
    const bool swap = flippable ? ((flip ^ (idx[3] != 0)) == 0) : flip;
    const int i1 = swap ? c : b, i2 = swap ? b : c;
    const Corner cs[3] = {{verts, a, bottom}, {verts, i1, bottom}, {verts, i2, bottom}};
    add_face(cs, 3,
             faces_away((const float*)(verts + 16 * a), (const float*)(verts + 16 * i1),
                        (const float*)(verts + 16 * i2)));
  }
}

void CtrShadowJak2Renderer::add_walls(const u8* idx, const u8* top, const u8* bottom) {
  // Shadow2::add_wall_quads: drawn as the strip 1 0 2 3
  const int n = idx[0];
  idx += 4;
  for (int i = 0; i < n; i++, idx += 4) {
    const int a = idx[0], b = idx[1];
    if (!a && !b) {
      continue;
    }
    // q0 q1 on top, q2 q3 below (a and b swapped when idx[2] is 0)
    const int u = idx[2] == 0 ? b : a, w = idx[2] == 0 ? a : b;
    const float* q0 = (const float*)(top + 16 * u);
    const float* q1 = (const float*)(top + 16 * w);
    const float* q2 = (const float*)(bottom + 16 * w);
    const Corner strip[4] = {{top, w, false}, {top, u, false}, {bottom, w, true}, {bottom, u, true}};
    add_face(strip, 4, faces_away(q0, q1, q2));
  }
}

void CtrShadowJak2Renderer::render(DmaFollower& dma, CtrRenderState& rs) {
  auto more = [&]() { return dma.current_tag_offset() != rs.next_bucket; };
  auto skip_rest = [&]() {
    while (more()) {
      dma.read_and_advance();
    }
  };
  m_reserved = m_full = false;
  m_nv = 0;
  m_gen[0]++;  // (AI-assisted, j2gpu) last frame's vertex copies are gone
  m_gen[1]++;
  m_boxes.clear();
  m_boxes.push_back(Box());
  m_front.clear();
  m_back.clear();
  if (!more()) {
    return;
  }
  dma.read_and_advance();  // the jump to the bucket's data
  if (!more()) {
    return;
  }
  auto consts = dma.read_and_advance();
  if (consts.size_bytes < 8 * 16) {
    skip_rest();
    return;
  }
  if (ctr_settings().shadows != CtrSettings::ShadowMode::Full || !ctr_gpu_has_stencil()) {
    skip_rest();
    return;
  }
  // shadow-vu1-constants: hvdfoff (4), fog (5)
  float hvdf[4], fog;
  memcpy(hvdf, consts.data + 4 * 16, 16);
  memcpy(&fog, consts.data + 5 * 16, 4);
  if (!more()) {
    return;
  }
  dma.read_and_advance();  // 4 qw of unknown use
  if (!more()) {
    return;
  }
  auto cam = dma.read_and_advance();
  if (cam.size_bytes != 64 || !(std::abs(fog) > 1e-20f)) {
    skip_rest();
    return;
  }
  float persp[4][4];  // columns x, y, z, w
  memcpy(persp, cam.data, 64);
  // shadow2.vert: t = -(px x + py y + pz z + pw), screen = t.xyz * fog / t.w + hvdf; as clip
  // space rows (the mapping of CtrShadowVolumes::make_clip_matrix, w = t.w / fog)
  {
    const float a = 1.f / fog;
    const float ky = ctr_gs_y_scale() * (1.f / 128.f);
    for (int c = 0; c < 4; c++) {
      const float col[4] = {-persp[c][0], -persp[c][1], -persp[c][2], -persp[c][3]};
      const float w = col[3] * a;
      m_clip[c] = (col[0] + (hvdf[0] - 2048.f) * w) * (1.f / 256.f);
      m_clip[4 + c] = -(col[1] + (hvdf[1] - 2048.f) * w) * ky;
      m_clip[8 + c] = col[2] * (1.f / 8388608.f) + (hvdf[2] * (1.f / 8388608.f) - 1.f) * w;
      m_clip[12 + c] = w;
    }
  }
  const u8* top = nullptr;
  const u8* bottom = nullptr;
  const u8* cap = nullptr;
  const u8* wall = nullptr;
  bool have_color = false;
  while (more()) {
    auto t = dma.read_and_advance();
    const VifCode v0 = t.vifcode0(), v1 = t.vifcode1();
    if (v1.kind == VifCode::Kind::UNPACK_V4_32) {
      if (v1.immediate == kTopVertexDataAddr) {
        top = t.data;
        m_gen[0]++;  // (AI-assisted, j2gpu) new vertices: none of them copied yet
        if (m_boxes.back().lo[0] <= m_boxes.back().hi[0]) {
          m_boxes.push_back(Box());  // the next volume's box
        }
      } else if (v1.immediate == kBottomVertexDataAddr) {
        bottom = t.data;
        m_gen[1]++;
      }
    } else if (v1.kind == VifCode::Kind::UNPACK_V4_8) {
      const u32 offset = 4 * v1.num;
      if (offset + 16 != t.size_bytes) {
        continue;  // not what Shadow2 expects (it skips the frame's shadows on overflow)
      }
      const u16 addr = VifCodeUnpack(v1).addr_qw;
      if (addr == kCapIndexDataAddr) {
        cap = t.data;
      } else if (addr == kWallIndexDataAddr) {
        wall = t.data;
      }
      u32 after;
      memcpy(&after, t.data + offset + 12, 4);
      const VifCode mscal(after);
      if (mscal.kind != VifCode::Kind::MSCALF || !top || !bottom) {
        continue;
      }
      switch (mscal.immediate) {
        case 2:  // caps, top and bottom
          if (cap) {
            add_caps(cap, top, false, false, false);
            add_caps(cap, bottom, true, false, true);
          }
          break;
        case 4:  // walls
          if (wall) {
            add_walls(wall, top, bottom);
          }
          break;
        case 6:  // caps whose winding each triangle says
          if (cap) {
            add_caps(cap, top, false, true, false);
            add_caps(cap, bottom, true, true, true);
          }
          break;
        default:
          break;
      }
    } else if (v0.kind == VifCode::Kind::FLUSHA && v1.kind == VifCode::Kind::DIRECT &&
               t.size_bytes == 560) {
      memcpy(m_color, t.data + 8 * 3, 4);
      have_color = true;
    }
  }
  (void)have_color;
  if (m_full && m_no_room++ % 300 == 0) {
    lg::warn("[ctr] shadow2: no room in the frame's vertex memory, shadows not drawn");
  }
  draw();
  if (rs.log_now) {
#ifndef __3DS__
    if (getenv("CTR_DIRECT_STATS")) {  // host diagnosis
      lg::info("[ctr] {} {}: {} faces, {} left out", m_name, m_id, m_stats.faces, m_stats.dropped);
    }
#endif
    lg::debug("[ctr] {}: {} faces, {} left out", m_name, m_stats.faces, m_stats.dropped);
    m_stats = Stats();
  }
}

void CtrShadowJak2Renderer::draw() {
  if (!m_reserved) {
    return;
  }
  const int nf = (int)m_front.size(), nb = (int)m_back.size();
  if (nf < 3) {
    ctr_gpu_clip_draw_ranges(nullptr, nullptr, nullptr, 0, 0, 0);
    return;
  }
  memcpy(m_indices, m_front.data(), (size_t)nf * 2);
  memcpy(m_indices + nf, m_back.data(), (size_t)nb * 2);
  // the darkening rectangles: the PC subtracts (a - c) / 256 per channel (the color's alpha is the
  // "no change" level); multiplied by c / a here (CTR_BLEND_MUL: 0xff = 1), like Jak 1's shadow
  // color.
  // (AI-assisted, gpuopt + j2gpu) Not over the whole screen: only where a volume can have changed
  // the stencil, the projection of each volume's camera space box (+2 pixels), like Jak 1's
  // CtrShadowVolumes rectangles. Outside them the stencil is still the frame's 0, so the
  // darkening (stencil != 0) and its stencil clear change nothing there. The whole screen when a
  // box corner is at or behind the camera plane (its projection means nothing), as before.
  u8 mul[3];
  for (int c = 0; c < 3; c++) {
    mul[c] = (u8)std::clamp(255 * (int)m_color[c] / std::max(1, (int)m_color[3]), 0, 255);
  }
  std::vector<Rect>& rects = m_rects;
  rects.clear();
  bool full = false;
  Rect all = {1.f, 1.f, -1.f, -1.f};
  for (const Box& b : m_boxes) {
    if (b.lo[0] > b.hi[0]) {
      continue;  // no vertices
    }
    Rect r = {1.f, 1.f, -1.f, -1.f};
    for (int k = 0; k < 8 && !full; k++) {
      const float x = (k & 1) ? b.hi[0] : b.lo[0], y = (k & 2) ? b.hi[1] : b.lo[1],
                  z = (k & 4) ? b.hi[2] : b.lo[2];
      const float cw = m_clip[12] * x + m_clip[13] * y + m_clip[14] * z + m_clip[15];
      if (!(cw > 1e-4f)) {
        full = true;
        break;
      }
      const float rw = 1.f / cw;
      const float nx = (m_clip[0] * x + m_clip[1] * y + m_clip[2] * z + m_clip[3]) * rw;
      const float ny = (m_clip[4] * x + m_clip[5] * y + m_clip[6] * z + m_clip[7]) * rw;
      r.x0 = std::min(r.x0, nx);
      r.x1 = std::max(r.x1, nx);
      r.y0 = std::min(r.y0, ny);
      r.y1 = std::max(r.y1, ny);
    }
    if (full) {
      break;
    }
    const float px = 2.f / 200.f, py = 2.f / 120.f;  // 2 pixels (400 x 240 target)
    r.x0 = std::max(-1.f, r.x0 - px);
    r.x1 = std::min(1.f, r.x1 + px);
    r.y0 = std::max(-1.f, r.y0 - py);
    r.y1 = std::min(1.f, r.y1 + py);
    if (r.x0 >= r.x1 || r.y0 >= r.y1) {
      continue;  // off screen: nothing of it to darken
    }
    rects.push_back(r);
    all = {std::min(all.x0, r.x0), std::min(all.y0, r.y0), std::max(all.x1, r.x1),
           std::max(all.y1, r.y1)};
  }
  int ni = nf + nb;
  // one rectangle's room was kept back (add_face); more only while they fit, else their union
  const int room = std::min((m_cap_verts - m_nv) / 4, (m_cap_indices - ni) / 6);
  if (full) {
    rects.assign(1, Rect{-1.f, -1.f, 1.f, 1.f});
  } else if ((int)rects.size() > room) {
    rects.assign(1, all);
  }
  static const u16 kQuad[6] = {0, 1, 2, 0, 2, 3};
  int nq = 0;
  for (const Rect& r : rects) {
    ctr_clip_vertex* v = m_verts + m_nv + 4 * nq;
    const float xy[4][2] = {{r.x0, r.y0}, {r.x1, r.y0}, {r.x1, r.y1}, {r.x0, r.y1}};
    for (int k = 0; k < 4; k++) {
      v[k].x = xy[k][0];
      v[k].y = xy[k][1];
      v[k].z = 0.f;
      v[k].w = 1.f;
      v[k].s = v[k].t = 0.f;
      v[k].r = mul[0];
      v[k].g = mul[1];
      v[k].b = mul[2];
      v[k].a = 0x80;
    }
    for (int k = 0; k < 6; k++) {
      m_indices[ni++] = (u16)(m_nv + 4 * nq + kQuad[k]);
    }
    nq++;
  }
  ctr_draw_state st[3];
  memset(st, 0, sizeof(st));
  for (auto& s : st) {
    s.tex = -1;
    s.blend = CTR_BLEND_OFF;
    s.atest = CTR_TEST_ALWAYS;
    s.ztest = CTR_TEST_GEQUAL;
    s.zwrite = 0;
  }
  st[0].flags = CTR_STATE_NO_COLOR | CTR_STATE_STENCIL_INCR;
  st[1].flags = CTR_STATE_NO_COLOR | CTR_STATE_STENCIL_DECR;
  st[2].blend = CTR_BLEND_MUL;
  st[2].ztest = CTR_TEST_ALWAYS;
  st[2].flags = CTR_STATE_STENCIL_CLEAR;
  const int first[3] = {0, nf, nf + nb};
  const int count[3] = {nf, nb, 6 * nq};
  // (AI-assisted, j2gpu) the volumes (passes 0, 1) in camera space, through m_clip on the GPU;
  // the rectangle in clip space
  ctr_gpu_clip_draw_ranges_mtx(m_clip, 2, st, first, count, 3, m_nv + 4 * std::max(nq, 1), ni);
}
