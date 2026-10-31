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
    if (m_reserved && (int)m_back.size() < m_cap_indices) {
      m_back.resize(m_cap_indices);  // (AI-assisted, j2gpu) kept between frames
    }
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

// the guard band (clip space |x|, |y| <= kGuard * w): a face with a corner beyond it, or beyond
// the near / far plane, is cut to them on the CPU (the PICA drops triangles far off screen, and a
// dropped face leaves its stencil count wherever it covers)
constexpr float kGuard = 2.f;

inline u32 sign_bit(float f) {
  u32 u;
  memcpy(&u, &f, 4);
  return u >> 31;
}

//! clip space of camera space p (row-major m): its distances to the planes of outcode
struct Clip {
  float d[6];
};
inline Clip clip_dists(const float* m, const float* p) {
  const float x = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3];
  const float y = m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7];
  const float z = m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11];
  const float w = m[12] * p[0] + m[13] * p[1] + m[14] * p[2] + m[15];
  const float g = kGuard * w;
  return Clip{{g - x, g + x, g - y, g + y, w - z, w + z}};
}
//! the planes camera space p is outside of: bits 0-3 the guard band (+x, -x, +y, -y), 4 near,
//! 5 far (the planes a face is cut to), bits 8-11 the screen's edges (+x, -x, +y, -y)
constexpr u32 kCutPlanes = 0x3f;
inline u16 outcode(const float* m, const float* p) {
  const float x = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3];
  const float y = m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7];
  const float z = m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11];
  const float w = m[12] * p[0] + m[13] * p[1] + m[14] * p[2] + m[15];
  const float g = kGuard * w;
  return (u16)(sign_bit(g - x) | (sign_bit(g + x) << 1) | (sign_bit(g - y) << 2) |
               (sign_bit(g + y) << 3) | (sign_bit(w - z) << 4) | (sign_bit(w + z) << 5) |
               (sign_bit(w - x) << 8) | (sign_bit(w + x) << 9) | (sign_bit(w - y) << 10) |
               (sign_bit(w + y) << 11));
}
}  // namespace

int CtrShadowJak2Renderer::upload_base(int k) {
  // (AI-assisted, j2gpu) the camera space vertices of the current top (k 0) or bottom (k 1)
  // upload, all of them once in the frame's vertex memory at their first use (the GPU's clip
  // program applies m_clip: ctr_gpu_clip_draw_ranges_mtx), shared by the faces that index them
  if (m_base[k] >= 0) {
    return m_base[k];
  }
  const int n = m_src_count[k];
  if (!reserve() || !m_src[k] || m_nv + n > m_cap_verts) {
    return -1;
  }
  ctr_clip_vertex* v = m_verts + m_nv;
  const float* p = (const float*)m_src[k];
  m_oc.resize(m_nv + n);
  for (int i = 0; i < n; i++, p += 4, v++) {
    v->x = p[0];
    v->y = p[1];
    v->z = p[2];
    v->w = 1.f;
    v->s = v->t = 0.f;
    v->r = v->g = v->b = v->a = 0;
    m_oc[m_nv + i] = outcode(m_clip, p);
  }
  m_base[k] = m_nv;
  m_nv += n;
  return m_base[k];
}

bool CtrShadowJak2Renderer::index_room(int count) {
  if (m_nf + m_nb + count > m_cap_indices) {
    m_stats.dropped++;
    return false;
  }
  return true;
}

void CtrShadowJak2Renderer::add_face(const u16 idx[3], bool away) {
  u16* out = away ? m_back.data() + m_nb : m_indices + m_nf;
  const u32 o0 = m_oc[idx[0]], o1 = m_oc[idx[1]], o2 = m_oc[idx[2]];
  const u32 any = o0 | o1 | o2;
  if (o0 & o1 & o2) {
    return;  // (off screen or outside a plane: no pixel)
  }
  if (!(any & kCutPlanes)) {
    if (m_nf + m_nb + 3 > m_cap_indices) {
      m_stats.dropped++;  // (the cut faces before took more room than counted)
      return;
    }
    out[0] = idx[0];
    out[1] = idx[1];
    out[2] = idx[2];
    (away ? m_nb : m_nf) += 3;
    return;
  }
  // cut to the planes in camera space (clip space is linear in it): the corners that stay keep
  // their vertices, the new ones go after the frame's vertices
  struct P {
    float p[3];
    Clip c;
    int index;  // m_verts index, -1: new
  };
  P buf[2][9];
  P* in = buf[0];
  P* cut = buf[1];
  int n = 3;
  for (int k = 0; k < 3; k++) {
    const ctr_clip_vertex& v = m_verts[idx[k]];
    in[k].p[0] = v.x;
    in[k].p[1] = v.y;
    in[k].p[2] = v.z;
    in[k].c = clip_dists(m_clip, in[k].p);
    in[k].index = idx[k];
  }
  for (u32 pl = any & kCutPlanes; pl; pl &= pl - 1) {
    const int k = __builtin_ctz(pl);
    int m = 0;
    for (int i = 0; i < n; i++) {
      const P& a = in[i];
      const P& b = in[i + 1 == n ? 0 : i + 1];
      const bool ina = a.c.d[k] >= 0, inb = b.c.d[k] >= 0;
      if (ina) {
        cut[m++] = a;
      }
      if (ina != inb) {
        const P& pi = ina ? a : b;
        const P& po = ina ? b : a;
        const float s = pi.c.d[k] / (pi.c.d[k] - po.c.d[k]);
        P& q = cut[m++];
        for (int j = 0; j < 3; j++) {
          q.p[j] = pi.p[j] + (po.p[j] - pi.p[j]) * s;
        }
        // (the planes after this one only)
        for (int j = k + 1; j < 6; j++) {
          q.c.d[j] = pi.c.d[j] + (po.c.d[j] - pi.c.d[j]) * s;
        }
        q.index = -1;
      }
    }
    n = m;
    if (n < 3) {
      return;
    }
    std::swap(in, cut);
  }
  int fresh = 0;
  for (int i = 0; i < n; i++) {
    fresh += in[i].index < 0;
  }
  if (m_nv + fresh > m_cap_verts || !index_room(3 * (n - 2))) {
    return;
  }
  for (int i = 0; i < n; i++) {
    if (in[i].index < 0) {
      ctr_clip_vertex& v = m_verts[m_nv];
      v.x = in[i].p[0];
      v.y = in[i].p[1];
      v.z = in[i].p[2];
      v.w = 1.f;
      v.s = v.t = 0.f;
      v.r = v.g = v.b = v.a = 0;
      m_oc.push_back(0);
      in[i].index = m_nv++;
    }
  }
  out = away ? m_back.data() + m_nb : m_indices + m_nf;
  for (int i = 1; i + 1 < n; i++) {
    out[0] = (u16)in[0].index;
    out[1] = (u16)in[i].index;
    out[2] = (u16)in[i + 1].index;
    out += 3;
  }
  (away ? m_nb : m_nf) += 3 * (n - 2);
  m_stats.clipped++;
}

void CtrShadowJak2Renderer::add_caps(const u8* idx, bool flip, bool flippable, int k) {
  // Shadow2::add_cap_tris / add_flippable_tris. (AI-assisted, j2gpu) The indices straight into
  // the frame's index memory (faces turned to the camera) or the back list (the others)
  const int n = idx[0];
  idx += 4;
  const int base = upload_base(k);
  if (base < 0 || !index_room(3 * n)) {
    m_stats.dropped += base < 0;
    return;
  }
  const u8* verts = m_src[k];
  const int count = m_src_count[k];
  for (int i = 0; i < n; i++, idx += 4) {
    const int a = idx[0], b = idx[1], c = idx[2];
    if (!a && !b && !c) {
      continue;  // padding of the unpack
    }
    if (a >= count || b >= count || c >= count) {
      m_stats.dropped++;  // (not in the upload)
      continue;
    }
    const bool swap = flippable ? ((flip ^ (idx[3] != 0)) == 0) : flip;
    const int i1 = swap ? c : b, i2 = swap ? b : c;
    const bool away = faces_away((const float*)(verts + 16 * a), (const float*)(verts + 16 * i1),
                                 (const float*)(verts + 16 * i2));
    const u16 tri[3] = {(u16)(base + a), (u16)(base + i1), (u16)(base + i2)};
    add_face(tri, away);
    m_stats.faces++;
  }
}

void CtrShadowJak2Renderer::add_walls(const u8* idx) {
  // Shadow2::add_wall_quads: drawn as the strip 1 0 2 3
  const int n = idx[0];
  idx += 4;
  const int tb = upload_base(0), bb = upload_base(1);
  if (tb < 0 || bb < 0 || !index_room(6 * n)) {
    m_stats.dropped += tb < 0 || bb < 0;
    return;
  }
  const u8* top = m_src[0];
  const u8* bottom = m_src[1];
  const int tcount = m_src_count[0], bcount = m_src_count[1];
  for (int i = 0; i < n; i++, idx += 4) {
    const int a = idx[0], b = idx[1];
    if (!a && !b) {
      continue;
    }
    if (a >= tcount || b >= tcount || a >= bcount || b >= bcount) {
      m_stats.dropped++;
      continue;
    }
    // q0 q1 on top, q2 q3 below (a and b swapped when idx[2] is 0); the strip q1 q0 q2 q3:
    // triangles (q1 q0 q2), (q0 q2 q3)
    const int u = idx[2] == 0 ? b : a, w = idx[2] == 0 ? a : b;
    const bool away =
        faces_away((const float*)(top + 16 * u), (const float*)(top + 16 * w),
                   (const float*)(bottom + 16 * w));
    const u16 t0[3] = {(u16)(tb + w), (u16)(tb + u), (u16)(bb + w)};
    const u16 t1[3] = {(u16)(tb + u), (u16)(bb + w), (u16)(bb + u)};
    add_face(t0, away);
    add_face(t1, away);
    m_stats.faces++;
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
  m_nf = m_nb = 0;
  m_base[0] = m_base[1] = -1;  // (AI-assisted, j2gpu) nothing copied yet this frame
  m_src[0] = m_src[1] = nullptr;
  m_oc.clear();
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
        // (AI-assisted, j2gpu) new vertices: copied at their first use
        m_src[0] = t.data;
        m_src_count[0] = (int)(t.size_bytes / 16);
        m_base[0] = -1;
      } else if (v1.immediate == kBottomVertexDataAddr) {
        bottom = t.data;
        m_src[1] = t.data;
        m_src_count[1] = (int)(t.size_bytes / 16);
        m_base[1] = -1;
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
            add_caps(cap, false, false, 0);
            add_caps(cap, true, false, 1);
          }
          break;
        case 4:  // walls
          if (wall) {
            add_walls(wall);
          }
          break;
        case 6:  // caps whose winding each triangle says
          if (cap) {
            add_caps(cap, false, true, 0);
            add_caps(cap, true, true, 1);
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
      lg::info("[ctr] {} {}: {} faces, {} left out, {} cut to the guard band", m_name, m_id,
               m_stats.faces, m_stats.dropped, m_stats.clipped);
    }
#endif
    lg::debug("[ctr] {}: {} faces, {} left out, {} cut to the guard band", m_name, m_stats.faces,
              m_stats.dropped, m_stats.clipped);
    m_stats = Stats();
  }
}

void CtrShadowJak2Renderer::draw() {
  if (!m_reserved) {
    return;
  }
  const int nf = m_nf, nb = m_nb;
  if (nf < 3) {
    ctr_gpu_clip_draw_ranges(nullptr, nullptr, nullptr, 0, 0, 0);
    return;
  }
  memcpy(m_indices + nf, m_back.data(), (size_t)nb * 2);  // (the front list is there already)
  // the darkening: the PC subtracts (a - c) / 256 per channel (the color's alpha is the "no
  // change" level); multiplied by c / a here (CTR_BLEND_MUL: 0xff = 1), like Jak 1's shadow
  // color. Drawn with the front faces again, like Jak 1's CtrShadowRenderer: only they count the
  // stencil up, so they cover every pixel to darken, and the first one there clears it.
  u8 mul[3];
  for (int c = 0; c < 3; c++) {
    mul[c] = (u8)std::clamp(255 * (int)m_color[c] / std::max(1, (int)m_color[3]), 0, 255);
  }
  for (int i = 0; i < m_nv; i++) {
    m_verts[i].r = mul[0];
    m_verts[i].g = mul[1];
    m_verts[i].b = mul[2];
    m_verts[i].a = 0x80;
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
  const int first[3] = {0, nf, 0};
  const int count[3] = {nf, nb, nf};
  // (the volumes in camera space, through m_clip on the GPU)
  ctr_gpu_clip_draw_ranges_mtx(m_clip, 3, st, first, count, 3, m_nv, nf + nb);
}
