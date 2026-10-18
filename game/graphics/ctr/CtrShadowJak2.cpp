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
    m_reserved = ctr_gpu_clip_reserve(kReserveVerts, kReserveIndices, &m_verts, &m_indices) != 0;
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

void CtrShadowJak2Renderer::add_face(const float* const* p, int n, bool away) {
  const int tris = n - 2;
  if (!reserve() || m_nv + n + 4 > kReserveVerts ||
      (int)(m_front.size() + m_back.size()) + 3 * tris + 6 > kReserveIndices) {
    m_stats.dropped++;
    return;
  }
  for (int i = 0; i < n; i++) {
    const float x = p[i][0], y = p[i][1], z = p[i][2];
    ctr_clip_vertex& v = m_verts[m_nv + i];
    v.x = m_clip[0] * x + m_clip[1] * y + m_clip[2] * z + m_clip[3];
    v.y = m_clip[4] * x + m_clip[5] * y + m_clip[6] * z + m_clip[7];
    v.z = m_clip[8] * x + m_clip[9] * y + m_clip[10] * z + m_clip[11];
    v.w = m_clip[12] * x + m_clip[13] * y + m_clip[14] * z + m_clip[15];
    v.s = v.t = 0.f;
    v.r = v.g = v.b = v.a = 0;
  }
  auto& list = away ? m_back : m_front;
  // a strip: (0 1 2), (1 2 3)
  for (int t = 0; t < tris; t++) {
    list.push_back((u16)(m_nv + t));
    list.push_back((u16)(m_nv + t + 1));
    list.push_back((u16)(m_nv + t + 2));
  }
  m_nv += n;
  m_stats.faces++;
}

void CtrShadowJak2Renderer::add_caps(const u8* idx, const u8* verts, bool flip, bool flippable) {
  // Shadow2::add_cap_tris / add_flippable_tris
  const int n = idx[0];
  idx += 4;
  for (int i = 0; i < n; i++, idx += 4) {
    const int a = idx[0], b = idx[1], c = idx[2];
    if (!a && !b && !c) {
      continue;  // padding of the unpack
    }
    const float* p[3];
    const bool swap = flippable ? ((flip ^ (idx[3] != 0)) == 0) : flip;
    p[0] = (const float*)(verts + 16 * a);
    p[1] = (const float*)(verts + 16 * (swap ? c : b));
    p[2] = (const float*)(verts + 16 * (swap ? b : c));
    add_face(p, 3, faces_away(p[0], p[1], p[2]));
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
    const float* q[4];
    if (idx[2] == 0) {
      q[0] = (const float*)(top + 16 * b);
      q[1] = (const float*)(top + 16 * a);
      q[2] = (const float*)(bottom + 16 * a);
      q[3] = (const float*)(bottom + 16 * b);
    } else {
      q[0] = (const float*)(top + 16 * a);
      q[1] = (const float*)(top + 16 * b);
      q[2] = (const float*)(bottom + 16 * b);
      q[3] = (const float*)(bottom + 16 * a);
    }
    const float* strip[4] = {q[1], q[0], q[2], q[3]};
    add_face(strip, 4, faces_away(q[0], q[1], q[2]));
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
      } else if (v1.immediate == kBottomVertexDataAddr) {
        bottom = t.data;
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
            add_caps(cap, top, false, false);
            add_caps(cap, bottom, true, false);
          }
          break;
        case 4:  // walls
          if (wall) {
            add_walls(wall, top, bottom);
          }
          break;
        case 6:  // caps whose winding each triangle says
          if (cap) {
            add_caps(cap, top, false, true);
            add_caps(cap, bottom, true, true);
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
  // the darkening rectangle over the whole screen: the PC subtracts (a - c) / 256 per channel
  // (the color's alpha is the "no change" level); multiplied by c / a here (CTR_BLEND_MUL: 0xff
  // = 1), like Jak 1's shadow color
  ctr_clip_vertex* v = m_verts + m_nv;
  const float xy[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
  u8 mul[3];
  for (int c = 0; c < 3; c++) {
    mul[c] = (u8)std::clamp(255 * (int)m_color[c] / std::max(1, (int)m_color[3]), 0, 255);
  }
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
  static const u16 kQuad[6] = {0, 1, 2, 0, 2, 3};
  int ni = nf + nb;
  for (int k = 0; k < 6; k++) {
    m_indices[ni++] = (u16)(m_nv + kQuad[k]);
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
  const int count[3] = {nf, nb, 6};
  ctr_gpu_clip_draw_ranges(st, first, count, 3, m_nv + 4, ni);
}
