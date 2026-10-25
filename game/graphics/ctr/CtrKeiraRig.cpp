/*!
 * @file CtrKeiraRig.cpp
 * (AI-assisted)
 * See CtrKeiraRig.h.
 */

#include "CtrKeiraRig.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace ctr_keira {
namespace {

struct KeiraSkelJoint {
  const char* name;
  int parent;
  float pos[3];  // meters
};

#include "game/graphics/ctr/keira_skeletons.inc"

constexpr int kJak = sizeof(kJakSkeleton) / sizeof(kJakSkeleton[0]);
constexpr int kKeira = sizeof(kKeiraSkeleton) / sizeof(kKeiraSkeleton[0]);
static_assert(kJak <= 127 && kKeira + 1 <= kMaxJoints, "skeletons too big for the slot table");

// Keira joint -> the Jak joint it follows. tip_k / tip_j: the joints her bone and his point at in
// the bind pose (the correction turns hers his way); nullptr: no correction of its own (her
// parent's). Keira joints not listed follow their parent's Jak joint (rigidly: same matrix).
struct Map {
  const char* keira;
  const char* jak;
  const char* tip_k;
  const char* tip_j;
};
const Map kMap[] = {
    {"align", "align", nullptr, nullptr},
    {"prejoint", "prejoint", nullptr, nullptr},
    {"main", "main", nullptr, nullptr},
    {"hips", "hips", nullptr, nullptr},
    // her waist (chest, 1.71 m) is his upper_body (1.69 m); the virtual upper chest his chest
    {"chest", "upper_body", "neck", "neckA"},
    {"neck", "neckA", "head", "neckB"},
    {"head", "neckB", nullptr, nullptr},
    {"lEar1", "Learbase", "lEar2", "Learmid"},
    {"lEar2", "Learmid", nullptr, nullptr},
    {"rEar1", "Rearbase", "rEar2", "Rearmid"},
    {"rEar2", "Rearmid", nullptr, nullptr},
    // her shoulder joint is her upper arm (his Larm, after his clavicle Lshould)
    {"Lshoulder", "Larm", "Lelbow", "Lforarm"},
    {"Lelbow", "Lforarm", "Lhand", "sk_lhand"},
    {"Lhand", "sk_lhand", "LmiddleA", "lmidA"},
    {"LthumbA", "lthumA", "LthumbB", "lthumB"},
    {"LthumbB", "lthumB", nullptr, nullptr},
    {"LindexA", "lindA", "LindexB", "lindB"},
    {"LindexB", "lindB", nullptr, nullptr},
    {"LmiddleA", "lmidA", "LmiddleB", "lmidB"},
    {"LmiddleB", "lmidB", nullptr, nullptr},
    {"LringA", "lringA", "LringB", "lringB"},
    {"LringB", "lringB", nullptr, nullptr},
    {"LpinkyA", "lpinkA", "LpinkyB", "lpinkB"},
    {"LpinkyB", "lpinkB", nullptr, nullptr},
    {"Rshoulder", "Rarm", "Relbow", "Rforarm"},
    {"Relbow", "Rforarm", "Rhand", "sk_rhand"},
    {"Rhand", "sk_rhand", "RmiddleA", "rmidA"},
    {"RthumbA", "rthumA", "RthumbB", "rthumB"},
    {"RthumbB", "rthumB", nullptr, nullptr},
    {"RindexA", "rindA", "RindexB", "rindB"},
    {"RindexB", "rindB", nullptr, nullptr},
    {"RmiddleA", "rmidA", "RmiddleB", "rmidB"},
    {"RmiddleB", "rmidB", nullptr, nullptr},
    {"RringA", "rringA", "RringB", "rringB"},
    {"RringB", "rringB", nullptr, nullptr},
    {"RpinkyA", "rpinkA", "RpinkyB", "rpinkB"},
    {"RpinkyB", "rpinkB", nullptr, nullptr},
    {"Lthigh", "Lthigh", "Lknee", "Lknee"},
    {"Lknee", "Lknee", "Lankle", "Lankle"},
    {"Lankle", "Lankle", "Lball", "Lball"},
    {"Lball", "Lball", nullptr, nullptr},
    {"Rthigh", "Rthigh", "Rknee", "Rknee"},
    {"Rknee", "Rknee", "Rankle", "Rankle"},
    {"Rankle", "Rankle", "Rball", "Rball"},
    {"Rball", "Rball", nullptr, nullptr},
    // her overall straps, goggles and bust go with her torso, not her arms (their parents in her
    // skeleton are her shoulders); the knee and boot flaps and pants with her shins
    {"lTopStrap1", "chest", nullptr, nullptr},
    {"rTopStrap1", "chest", nullptr, nullptr},
    {"lBotStrap1", "hips", nullptr, nullptr},
    {"rBotStrap1", "hips", nullptr, nullptr},
    {"lBotStrap2", "upper_body", nullptr, nullptr},
    {"rBotStrap2", "upper_body", nullptr, nullptr},
    {"belt", "hips", nullptr, nullptr},
};

// rig parents other than her skeleton's: what hangs off her upper chest
struct Reparent {
  const char* keira;
  const char* parent;  // "chest2": the virtual joint
};
const Reparent kReparent[] = {
    {"Lshoulder", "chest2"}, {"Rshoulder", "chest2"},   {"lTopStrap1", "chest2"},
    {"rTopStrap1", "chest2"}, {"lBoob", "chest2"},      {"rBoob", "chest2"},
    {"gogglesBase", "chest2"},
};

// the virtual upper chest: where her torso is as far up as his chest joint is in his (75% of the
// way from his upper_body to his neckA)
constexpr float kChest2Y = 2.042f;
// her chest vertices go over to it from this height to that one (meters)
constexpr float kChest2Lo = 1.90f, kChest2Hi = 2.10f;

int find(const KeiraSkelJoint* s, int n, const char* name) {
  for (int i = 0; i < n; i++) {
    if (!strcmp(s[i].name, name)) {
      return i;
    }
  }
  return -1;
}

void identity(float* m) {
  for (int i = 0; i < 9; i++) {
    m[i] = i % 4 == 0 ? 1.f : 0.f;
  }
}

// the rotation that turns direction a into direction b (both unit), row major
void rotation_between(const float* a, const float* b, float* m) {
  const float v[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2],
                      a[0] * b[1] - a[1] * b[0]};
  const float c = a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
  identity(m);
  if (c < -0.999f) {
    return;  // opposite: never for these skeletons
  }
  const float k = 1.f / (1.f + c);
  const float vx[9] = {0, -v[2], v[1], v[2], 0, -v[0], -v[1], v[0], 0};
  for (int r = 0; r < 3; r++) {
    for (int col = 0; col < 3; col++) {
      float vv = 0;
      for (int t = 0; t < 3; t++) {
        vv += vx[3 * r + t] * vx[3 * t + col];
      }
      m[3 * r + col] += vx[3 * r + col] + vv * k;
    }
  }
}

void unit_dir(const float* from, const float* to, float* out) {
  float d[3] = {to[0] - from[0], to[1] - from[1], to[2] - from[2]};
  const float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
  for (int c = 0; c < 3; c++) {
    out[c] = l > 0 ? d[c] / l : 0.f;
  }
}

Rig make_rig() {
  Rig r;
  r.jak_count = kJak;
  for (int j = 0; j < kJak; j++) {
    r.jak_parent[j] = kJakSkeleton[j].parent;
    for (int c = 0; c < 3; c++) {
      r.jak_pos[j][c] = kJakSkeleton[j].pos[c] * 4096.f;
    }
  }
  r.keira_joints = kKeira;
  r.count = kKeira + 1;
  r.chest2 = kKeira;
  for (int k = 0; k < r.count; k++) {
    const bool virt = k == r.chest2;
    const int kc = find(kKeiraSkeleton, kKeira, "chest");
    r.name[k] = virt ? "chest2" : kKeiraSkeleton[k].name;
    r.parent[k] = virt ? kc : kKeiraSkeleton[k].parent;
    for (int c = 0; c < 3; c++) {
      const float m = virt ? (c == 1 ? kChest2Y : kKeiraSkeleton[kc].pos[c]) : kKeiraSkeleton[k].pos[c];
      r.pos[k][c] = m * 4096.f;
    }
    r.jak[k] = -1;
    r.anchor[k] = false;
    identity(r.corr[k]);
  }
  r.jak[r.chest2] = find(kJakSkeleton, kJak, "chest");
  for (const auto& rp : kReparent) {
    const int k = find(kKeiraSkeleton, kKeira, rp.keira);
    if (k >= 0) {
      r.parent[k] = !strcmp(rp.parent, "chest2") ? r.chest2 : find(kKeiraSkeleton, kKeira, rp.parent);
    }
  }
  // parents before children
  {
    bool placed[kMaxJoints] = {};
    int n = 0;
    while (n < r.count) {
      for (int k = 0; k < r.count; k++) {
        if (!placed[k] && (r.parent[k] < 0 || placed[r.parent[k]])) {
          placed[k] = true;
          r.order[n++] = k;
        }
      }
    }
  }
  bool own_corr[kMaxJoints] = {};
  for (const auto& m : kMap) {
    const int k = find(kKeiraSkeleton, kKeira, m.keira);
    const int j = find(kJakSkeleton, kJak, m.jak);
    if (k < 0 || j < 0) {
      continue;
    }
    r.jak[k] = j;
    if (m.tip_k && m.tip_j) {
      const int tk = find(kKeiraSkeleton, kKeira, m.tip_k);
      const int tj = find(kJakSkeleton, kJak, m.tip_j);
      if (tk >= 0 && tj >= 0) {
        float a[3], b[3];
        unit_dir(kKeiraSkeleton[k].pos, kKeiraSkeleton[tk].pos, a);
        unit_dir(kJakSkeleton[j].pos, kJakSkeleton[tj].pos, b);
        rotation_between(a, b, r.corr[k]);
        own_corr[k] = true;
      }
    }
  }
  // the root: Jak's main joint places her main joint (and the joints above it)
  for (int k = 0; k < r.count; k++) {
    r.anchor[k] = r.parent[k] < 0 || !strcmp(r.name[k], "main");
  }
  // (AI-assisted) the arms and the ponytail
  const char* arms[2][3] = {{"Lshoulder", "Lelbow", "Lhand"}, {"Rshoulder", "Relbow", "Rhand"}};
  for (int a = 0; a < 2; a++) {
    for (int i = 0; i < 3; i++) {
      r.arm[a][i] = find(kKeiraSkeleton, kKeira, arms[a][i]);
    }
  }
  r.tail = find(kKeiraSkeleton, kKeira, "backHair1");
  r.tail_tip = find(kKeiraSkeleton, kKeira, "backHair2");
  // the rest follow their parents' Jak joint and correction
  for (int i = 0; i < r.count; i++) {
    const int k = r.order[i];
    const int p = r.parent[k];
    if (r.jak[k] < 0) {
      r.jak[k] = p >= 0 ? r.jak[p] : 0;
    }
    if (!own_corr[k] && p >= 0) {
      memcpy(r.corr[k], r.corr[p], sizeof(r.corr[k]));
    }
  }
  return r;
}

}  // namespace

const Rig& rig() {
  static const Rig r = make_rig();
  return r;
}

int keira_joint(const char* name) {
  return find(kKeiraSkeleton, kKeira, name);
}

float chest2_share(float y) {
  const float t = (y / 4096.f - kChest2Lo) / (kChest2Hi - kChest2Lo);
  if (t <= 0) {
    return 0;
  }
  if (t >= 1) {
    return 1;
  }
  return t * t * (3.f - 2.f * t);
}

namespace {

void mul33(const float* a, const float* b, float* out) {  // row major: out = a * b
  float r[9];
  for (int i = 0; i < 3; i++) {
    for (int c = 0; c < 3; c++) {
      r[3 * i + c] = a[3 * i] * b[c] + a[3 * i + 1] * b[3 + c] + a[3 * i + 2] * b[6 + c];
    }
  }
  memcpy(out, r, sizeof(r));
}

void mul33v(const float* m, const float* v, float* out) {
  float r[3];
  for (int i = 0; i < 3; i++) {
    r[i] = m[3 * i] * v[0] + m[3 * i + 1] * v[1] + m[3 * i + 2] * v[2];
  }
  memcpy(out, r, sizeof(r));
}

float len3(const float* v) {
  return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

// M * (I + (s - 1) u u^T): stretched by s along the (unit, bind space) axis u
void stretch(float* M, const float* u, float s) {
  float S[9];
  for (int i = 0; i < 3; i++) {
    for (int c = 0; c < 3; c++) {
      S[3 * i + c] = (i == c ? 1.f : 0.f) + (s - 1.f) * u[i] * u[c];
    }
  }
  mul33(M, S, M);
}

bool is_below(const Rig& r, int k, int anc) {
  for (int p = r.parent[k]; p >= 0; p = r.parent[p]) {
    if (p == anc) {
      return true;
    }
  }
  return false;
}

// the ponytail's spring (one Keira): its tip in the world, its velocity, the frame of the last step
struct Spring {
  bool valid = false;
  uint64_t frame = 0;
  float tip[3] = {0, 0, 0}, vel[3] = {0, 0, 0};
  float swing[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};  // the last swing (camera space rotation)
};
Spring g_spring;

// (AI-assisted, keira3) Daxter's seat (daxter_shift)
struct Seat {
  bool valid = false;
  uint64_t frame = 0;
  float jak[3];     // the seat on Jak (camera space)
  float delta[3];   // to the seat on her
  float Lc[9], Tc[3];  // Jak's chest matrix (for the host log)
};
Seat g_seat;
// where Daxter's hips (his main joint) are on Jak (Jak's bind pose, meters: in his standing,
// walking and running animations, host log CTR_KEIRA_DAX_LOG: (0.34..0.37, 2.77..2.78, 0.09..0.11))
// and the same place on her (her bind pose): over her shoulder joint as his is over Jak's arm joint
// (hers is 0.18 m nearer her middle; 0.13 here: her front hair locks and his shoulder pad), as
// high above her shoulder's top as above his pad's (the meshes under him: 2.12..2.14 m against
// 2.15..2.23 m)
constexpr float kJakSeat[3] = {0.35f, 2.78f, 0.10f};
constexpr float kKeiraSeat[3] = {0.22f, 2.72f, 0.10f};
constexpr float kSpringK = 0.22f;      // pull toward the rest tip per frame
constexpr float kSpringDamp = 0.72f;   // velocity kept per frame
constexpr float kSpringMaxCos = 0.6f;  // at most ~53 degrees from the rest direction

}  // namespace

bool pose(const float* const* jak,
          const uint64_t sent[2],
          float (*out)[16],
          int out_slots,
          const Camera* cam,
          uint64_t frame) {
  const Rig& r = rig();
  auto is_sent = [&](int slot) { return slot < 128 && (sent[slot >> 6] >> (slot & 63)) & 1; };
  // the linear part (row major) and the place of each rig joint, camera space
  float lin[kMaxJoints][9];
  float q[kMaxJoints][3];
  bool any = false;
  // Jak's matrix of a joint (or its nearest sent ancestor's): L (row major), T
  auto jak_mat = [&](int j, float* L, float* T) -> bool {
    while (j >= 0 && !is_sent(j + 1)) {
      j = r.jak_parent[j];
    }
    if (j < 0) {
      identity(L);
      T[0] = T[1] = T[2] = 0;
      return false;
    }
    const float* t = jak[j + 1];
    for (int row = 0; row < 3; row++) {
      for (int c = 0; c < 3; c++) {
        L[3 * row + c] = t[4 * c + row];
      }
      T[row] = t[12 + row];
    }
    return true;
  };
  auto fk = [&](int k) {
    const float* p = r.pos[k];
    const float* pp = r.pos[r.parent[k]];
    const float d[3] = {p[0] - pp[0], p[1] - pp[1], p[2] - pp[2]};
    float m[3];
    mul33v(lin[r.parent[k]], d, m);
    for (int c = 0; c < 3; c++) {
      q[k][c] = q[r.parent[k]][c] + m[c];
    }
  };
  for (int i = 0; i < r.count; i++) {
    const int k = r.order[i];
    float L[9], T[3];
    const bool have = jak_mat(r.jak[k], L, T);
    any |= have;
    mul33(L, r.corr[k], lin[k]);
    if (r.anchor[k] || r.parent[k] < 0 || !have) {
      mul33v(L, r.pos[k], q[k]);
      for (int c = 0; c < 3; c++) {
        q[k][c] += T[c];
      }
    } else {
      fk(k);
    }
  }
  if (!any) {
    return false;
  }

  // (AI-assisted, keira3) Daxter's seat: on Jak (his chest's matrix) and on her (her upper chest)
  {
    float Lc[9], Tc[3];
    if (r.chest2 >= 0 && jak_mat(r.jak[r.chest2], Lc, Tc)) {
      const float sj[3] = {kJakSeat[0] * 4096.f, kJakSeat[1] * 4096.f, kJakSeat[2] * 4096.f};
      float d[3], pj[3], pk[3];
      for (int c = 0; c < 3; c++) {
        d[c] = kKeiraSeat[c] * 4096.f - r.pos[r.chest2][c];
      }
      mul33v(Lc, sj, pj);
      mul33v(lin[r.chest2], d, pk);
      for (int c = 0; c < 3; c++) {
        g_seat.jak[c] = pj[c] + Tc[c];
        g_seat.delta[c] = q[r.chest2][c] + pk[c] - g_seat.jak[c];
        g_seat.Tc[c] = Tc[c];
      }
      memcpy(g_seat.Lc, Lc, sizeof(Lc));
      g_seat.frame = frame;
      g_seat.valid = true;
    }
  }

  // (AI-assisted) the arms: her wrist onto his (two-bone IK in the plane her elbow bends in)
#ifndef __3DS__
  static const bool no_ik = getenv("CTR_KEIRA_NO_IK") != nullptr;  // host A/B pictures
#else
  constexpr bool no_ik = false;
#endif
  for (int a = 0; a < 2 && !no_ik; a++) {
    const int S = r.arm[a][0], E = r.arm[a][1], H = r.arm[a][2];
    if (S < 0 || E < 0 || H < 0) {
      continue;
    }
    float L[9], T[3], P[3];
    if (!jak_mat(r.jak[H], L, T)) {
      continue;
    }
    mul33v(L, r.jak_pos[r.jak[H]], P);
    for (int c = 0; c < 3; c++) {
      P[c] += T[c];
    }
    float ua[3], ub[3];
    for (int c = 0; c < 3; c++) {
      ua[c] = r.pos[E][c] - r.pos[S][c];
      ub[c] = r.pos[H][c] - r.pos[E][c];
    }
    const float la = len3(ua), lb = len3(ub);
    float st[3];
    for (int c = 0; c < 3; c++) {
      st[c] = P[c] - q[S][c];
      ua[c] /= la;
      ub[c] /= lb;
    }
    const float d = len3(st);
    if (d < 1.f || la < 1.f || lb < 1.f) {
      continue;
    }
    const float reach = (la + lb) * 0.999f;
    const float s = std::clamp(d / reach, 1.f, kMaxArmStretch);
    const float A = la * s, B = lb * s;
#ifndef __3DS__
    // host check (env CTR_KEIRA_IK_LOG): her FK wrist's distance from his, and after the IK
    static const bool ik_log = getenv("CTR_KEIRA_IK_LOG") != nullptr;
    static double gap_sum = 0, gap_max = 0, after_max = 0, stretch_max = 1, n_log = 0;
    if (ik_log) {
      float g[3];
      for (int c = 0; c < 3; c++) {
        g[c] = q[H][c] - P[c];
      }
      const double gap = len3(g) / 4096.0;
      gap_sum += gap;
      gap_max = std::max(gap_max, gap);
      stretch_max = std::max<double>(stretch_max, d / reach);
      after_max = std::max<double>(after_max, std::max(0.f, d - (A + B)) / 4096.0);
      if (++n_log >= 600) {
        printf("[keira ik] wrist gap before IK: mean %.3f m, max %.3f m; after: max %.3f m; "
               "reach needed up to %.2fx (stretch capped at %.2f)\n",
               gap_sum / n_log, gap_max, after_max, stretch_max, kMaxArmStretch);
        gap_sum = gap_max = after_max = n_log = 0;
        stretch_max = 1;
      }
    }
#endif
    const float dd = std::min(d, (A + B) * 0.9999f);
    float D[3];
    for (int c = 0; c < 3; c++) {
      D[c] = st[c] / d;
    }
    // the bend direction: (AI-assisted, keira3) the way Jak's own elbow sticks out of his
    // shoulder-wrist line (her FK elbow, which round 2 used, can land on the inner side of her
    // narrower shoulder line: her left elbow bent into her body in his idle); FK's if his arm is
    // straight
    float pole[3], e0[3];
    bool have_pole = false;
#ifndef __3DS__
    static const bool fk_pole = getenv("CTR_KEIRA_FK_POLE") != nullptr;  // host A/B: round 2's
#else
    constexpr bool fk_pole = false;
#endif
    if (!fk_pole) {
      float Ls[9], Ts[3], Le[9], Te[3], sj[3], ej[3];
      if (jak_mat(r.jak[S], Ls, Ts) && jak_mat(r.jak[E], Le, Te)) {
        mul33v(Ls, r.jak_pos[r.jak[S]], sj);
        mul33v(Le, r.jak_pos[r.jak[E]], ej);
        float dj[3], ue[3];
        for (int c = 0; c < 3; c++) {
          sj[c] += Ts[c];
          ej[c] += Te[c];
          dj[c] = P[c] - sj[c];
          ue[c] = ej[c] - sj[c];
        }
        const float djl = len3(dj);
        if (djl > 1.f) {
          const float a1 = (ue[0] * dj[0] + ue[1] * dj[1] + ue[2] * dj[2]) / (djl * djl);
          for (int c = 0; c < 3; c++) {
            e0[c] = ue[c] - a1 * dj[c];  // his bend, off his shoulder-wrist line
          }
          have_pole = len3(e0) > 0.02f * len3(ue);
        }
      }
    }
    if (!have_pole) {
      for (int c = 0; c < 3; c++) {
        e0[c] = q[E][c] - q[S][c];
      }
    }
    const float along = e0[0] * D[0] + e0[1] * D[1] + e0[2] * D[2];
    for (int c = 0; c < 3; c++) {
      pole[c] = e0[c] - along * D[c];
    }
    float pl = len3(pole);
    if (pl < 1e-3f * la) {
      continue;  // straight arm along the line: FK's own
    }
#ifndef __3DS__
    // host check (env CTR_KEIRA_IK_LOG): the bend direction (unit, Jak's chest's bind space: +x
    // his left, +y up, +z forward) of his elbow and of her FK elbow, every 2 s
    if (ik_log && frame % 60 == 0) {
      float Lc[9], Tc[3];
      if (jak_mat(r.jak[r.chest2], Lc, Tc)) {
        auto to_bind = [&](const float* v, float* o) {  // rotation part: transpose, scale out
          float t[3];
          for (int c = 0; c < 3; c++) {
            t[c] = Lc[c] * v[0] + Lc[3 + c] * v[1] + Lc[6 + c] * v[2];
          }
          const float l = len3(t);
          for (int c = 0; c < 3; c++) {
            o[c] = l > 0 ? t[c] / l : 0;
          }
        };
        float fk[3], fkp[3], jb[3], fb[3];
        for (int c = 0; c < 3; c++) {
          fk[c] = q[E][c] - q[S][c];
        }
        const float al = fk[0] * D[0] + fk[1] * D[1] + fk[2] * D[2];
        for (int c = 0; c < 3; c++) {
          fkp[c] = fk[c] - al * D[c];
        }
        to_bind(pole, jb);
        to_bind(fkp, fb);
        printf("[keira ik] frame %llu %s arm: bend used (%.2f %.2f %.2f)%s, her FK elbow's (%.2f "
               "%.2f %.2f)\n",
               (unsigned long long)frame, a == 0 ? "left" : "right", jb[0], jb[1], jb[2],
               have_pole ? " (Jak's)" : " (FK)", fb[0], fb[1], fb[2]);
      }
    }
#endif
    const float x = (A * A - B * B + dd * dd) / (2.f * dd);
    const float h = std::sqrt(std::max(A * A - x * x, 0.f));
    float e[3], w[3];
    for (int c = 0; c < 3; c++) {
      e[c] = q[S][c] + D[c] * x + pole[c] / pl * h;
      w[c] = q[S][c] + D[c] * dd;
    }
    // upper arm: turned from its FK direction to the elbow, stretched along its bone
    float cur[3], want[3], R1[9], R2[9];
    mul33v(lin[S], ua, cur);
    float cl = len3(cur);
    for (int c = 0; c < 3; c++) {
      cur[c] /= cl;
      want[c] = (e[c] - q[S][c]) / A;
    }
    rotation_between(cur, want, R1);
    mul33(R1, lin[S], lin[S]);
    stretch(lin[S], ua, s);
    // forearm likewise, to the wrist
    mul33v(lin[E], ub, cur);
    cl = len3(cur);
    for (int c = 0; c < 3; c++) {
      cur[c] /= cl;
      want[c] = (w[c] - e[c]) / B;
    }
    rotation_between(cur, want, R2);
    mul33(R2, lin[E], lin[E]);
    stretch(lin[E], ub, s);
    // the joints riding on them without a Jak joint of their own (her glove) turn with them
    for (int k = 0; k < r.count; k++) {
      if (k != E && k != H && r.jak[k] == r.jak[E] && is_below(r, k, E)) {
        mul33(R2, lin[k], lin[k]);
      }
    }
    // everything below the shoulder moves with it
    for (int i = 0; i < r.count; i++) {
      const int k = r.order[i];
      if (is_below(r, k, S)) {
        fk(k);
      }
    }
  }

  // (AI-assisted) the ponytail: a damped spring on its tip, in the world
  if (r.tail >= 0 && r.tail_tip >= 0) {
    const int B = r.tail, Tt = r.tail_tip;
    float rest[3];  // the rigid tip direction (camera space)
    for (int c = 0; c < 3; c++) {
      rest[c] = q[Tt][c] - q[B][c];
    }
    const float tl = len3(rest);
    if (cam && tl > 1.f) {
      auto to_world = [&](const float* p, float* o) {
        mul33v(cam->rot, p, o);
        for (int c = 0; c < 3; c++) {
          o[c] += cam->trans[c];
        }
      };
      float base_w[3], tip_w[3];
      to_world(q[B], base_w);
      to_world(q[Tt], tip_w);
      Spring& sp = g_spring;
      float off[3];
      for (int c = 0; c < 3; c++) {
        off[c] = tip_w[c] - sp.tip[c];
      }
      if (!sp.valid || len3(off) > 4096.f) {  // first time, or a jump (a checkpoint)
        memcpy(sp.tip, tip_w, sizeof(sp.tip));
        memset(sp.vel, 0, sizeof(sp.vel));
        sp.valid = true;
      } else if (frame != sp.frame) {
        for (int c = 0; c < 3; c++) {
          sp.vel[c] = sp.vel[c] * kSpringDamp + (tip_w[c] - sp.tip[c]) * kSpringK;
          sp.tip[c] += sp.vel[c];
        }
        // back on the sphere around the base, within the cone around the rest direction
        float dir[3], rw[3];
        for (int c = 0; c < 3; c++) {
          dir[c] = sp.tip[c] - base_w[c];
          rw[c] = (tip_w[c] - base_w[c]) / tl;
        }
        float dl = len3(dir);
        if (dl < 1e-3f) {
          memcpy(dir, rw, sizeof(dir));
          dl = 1.f;
        }
        for (int c = 0; c < 3; c++) {
          dir[c] /= dl;
        }
        const float cs = dir[0] * rw[0] + dir[1] * rw[1] + dir[2] * rw[2];
        if (cs < kSpringMaxCos) {
          // pulled back to the cone's edge
          float perp[3];
          for (int c = 0; c < 3; c++) {
            perp[c] = dir[c] - cs * rw[c];
          }
          const float pl = len3(perp);
          const float sn = std::sqrt(1.f - kSpringMaxCos * kSpringMaxCos);
          for (int c = 0; c < 3; c++) {
            dir[c] = pl > 1e-6f ? kSpringMaxCos * rw[c] + sn * perp[c] / pl : rw[c];
          }
        }
        for (int c = 0; c < 3; c++) {
          sp.tip[c] = base_w[c] + dir[c] * tl;
        }
        sp.frame = frame;
      }
      // the swing as a camera space rotation of the rest direction to the spring's
      float want_w[3], want[3], cur[3];
      for (int c = 0; c < 3; c++) {
        want_w[c] = sp.tip[c] - base_w[c];
      }
      // camera = rot^T (world)
      for (int c = 0; c < 3; c++) {
        want[c] = cam->rot[c] * want_w[0] + cam->rot[3 + c] * want_w[1] + cam->rot[6 + c] * want_w[2];
        cur[c] = rest[c] / tl;
      }
      const float wl = len3(want);
      if (wl > 1e-3f) {
        for (int c = 0; c < 3; c++) {
          want[c] /= wl;
        }
        rotation_between(cur, want, sp.swing);
      }
    }
    if (g_spring.valid) {
      // (without a camera: the last swing)
      for (int k = 0; k < r.count; k++) {
        if (k == B || is_below(r, k, B)) {
          mul33(g_spring.swing, lin[k], lin[k]);
        }
      }
      for (int i = 0; i < r.count; i++) {
        const int k = r.order[i];
        if (is_below(r, k, B)) {
          fk(k);
        }
      }
    }
  }

  for (int k = 0; k < r.count && k + 1 < out_slots; k++) {
    const float* M = lin[k];
    const float* p = r.pos[k];
    float* o = out[k + 1];
    for (int c = 0; c < 3; c++) {
      for (int row = 0; row < 3; row++) {
        o[4 * c + row] = M[3 * row + c];
      }
      o[4 * c + 3] = 0;
    }
    for (int row = 0; row < 3; row++) {
      o[12 + row] = q[k][row] - (M[3 * row] * p[0] + M[3 * row + 1] * p[1] + M[3 * row + 2] * p[2]);
    }
    o[15] = 1.f;
  }
  return true;
}

// ---- (AI-assisted, keira3) Daxter ----

bool daxter_shift(uint64_t frame, const float* m, float out[3]) {
  out[0] = out[1] = out[2] = 0;
  const Seat& s = g_seat;
  if (!s.valid || !(s.frame == frame || s.frame + 1 == frame)) {
    return false;
  }
#ifndef __3DS__
  static const bool no_dax = getenv("CTR_KEIRA_NO_DAX") != nullptr;  // host A/B: Jak's seat
  if (no_dax) {
    return false;
  }
#endif
  // his hips: his main joint's matrix (columns, as the game's bone matrices) at his bind pose
  float h[3];
  for (int row = 0; row < 3; row++) {
    h[row] = m[row] * kDaxterMain[0] * 4096.f + m[4 + row] * kDaxterMain[1] * 4096.f +
             m[8 + row] * kDaxterMain[2] * 4096.f + m[12 + row];
  }
  float g[3];
  for (int c = 0; c < 3; c++) {
    g[c] = h[c] - s.jak[c];
  }
  // (camera space matrices may carry a uniform scale: distances in Jak's chest's units)
  const float unit = len3(s.Lc) > 1e-6f ? len3(s.Lc) : 1.f;
  const float dist = len3(g) / unit / 4096.f;
#ifndef __3DS__
  // host check (env CTR_KEIRA_DAX_LOG): his hips in Jak's bind space (his chest's matrix), meters
  static const bool dax_log = getenv("CTR_KEIRA_DAX_LOG") != nullptr;
  if (dax_log && frame % 30 == 0) {
    // inverse of the chest's linear part (general 3x3)
    const float* L = s.Lc;
    const float det = L[0] * (L[4] * L[8] - L[5] * L[7]) - L[1] * (L[3] * L[8] - L[5] * L[6]) +
                      L[2] * (L[3] * L[7] - L[4] * L[6]);
    if (det > 1e-12f || det < -1e-12f) {
      const float inv[9] = {(L[4] * L[8] - L[5] * L[7]) / det, (L[2] * L[7] - L[1] * L[8]) / det,
                            (L[1] * L[5] - L[2] * L[4]) / det, (L[5] * L[6] - L[3] * L[8]) / det,
                            (L[0] * L[8] - L[2] * L[6]) / det, (L[2] * L[3] - L[0] * L[5]) / det,
                            (L[3] * L[7] - L[4] * L[6]) / det, (L[1] * L[6] - L[0] * L[7]) / det,
                            (L[0] * L[4] - L[1] * L[3]) / det};
      float v[3], b[3];
      for (int c = 0; c < 3; c++) {
        v[c] = h[c] - s.Tc[c];
      }
      mul33v(inv, v, b);
      printf("[keira dax] frame %llu: his hips at (%.3f %.3f %.3f) m in Jak's bind space, %.3f m "
             "from the seat\n",
             (unsigned long long)frame, b[0] / 4096.f, b[1] / 4096.f, b[2] / 4096.f, dist);
    }
  }
#endif
  // meters: all of it while he sits or holds on (he leans back to 0.5 m in a punch), none when
  // he has hopped off (cutscenes: 1 m and more)
  constexpr float kFull = 0.55f, kNone = 0.9f;
  const float w = dist <= kFull ? 1.f : dist >= kNone ? 0.f : (kNone - dist) / (kNone - kFull);
  for (int c = 0; c < 3; c++) {
    out[c] = s.delta[c] * w;
  }
  return w > 0;
}

// ---- (AI-assisted) the face ----

bool face_weights(const float* jak, uint64_t frame, float out[kFaceWeights]) {
  for (int i = 0; i < kFaceWeights; i++) {
    out[i] = 0;
  }
  bool any = false;
  if (jak) {
    // Jak's weight -> hers (and a scale): from what they move (keira_model.cpp logs it)
    struct M {
      int jak, keira;
      float k;
    };
    static const M kMapW[] = {
        {4, 10, 1.f},   // jaw open (down and back)
        {5, 9, 0.6f},   // jaw down
        {7, 8, 1.f},    // lower lip down
        {0, 5, 1.f},    // lips down
        {1, 5, 0.5f},   //
        {2, 15, 1.f},   // left mouth corner up
        {3, 16, 1.f},   // right mouth corner up
        {11, 21, 1.f},  // left eyelid down
        {12, 22, 1.f},  // right eyelid down
        {9, 27, 1.f},   // brows down
        {9, 28, 1.f},   //
        {10, 19, 1.f},  // brows up
        {10, 20, 1.f},  //
    };
    for (const auto& m : kMapW) {
      out[m.keira] += jak[m.jak] * m.k;
    }
  }
  // the blink: one about every 4 s (at 30 frames per second), 8 frames long
  constexpr uint64_t kPeriod = 120;
  const uint64_t b = frame / kPeriod;
  const uint64_t start = b * kPeriod + ((b * 2654435761u) >> 7) % 50;
  if (frame >= start && frame < start + 8) {
    static const float kCurve[8] = {0.35f, 0.8f, 1.f, 1.f, 0.85f, 0.6f, 0.3f, 0.1f};
    const float w = 4096.f * kCurve[frame - start];
    out[21] = std::max(out[21], w);
    out[22] = std::max(out[22], w);
  }
  for (int i = 0; i < kFaceWeights; i++) {
    any |= out[i] != 0;
  }
  return any;
}

// ---- (AI-assisted) the shadow ----

bool build_shadow_mesh(const float* pos,
                       const uint8_t* slot,
                       int nverts,
                       const uint16_t* tris,
                       int ntris,
                       ShadowMeshData* out) {
  *out = ShadowMeshData();
  // the slots, in order of their first triangle
  std::vector<int> slots;
  for (int k = 0; k < ntris; k++) {
    const int s0 = tris[3 * k] < nverts ? slot[tris[3 * k]] : -1;
    if (s0 >= 0 && std::find(slots.begin(), slots.end(), s0) == slots.end()) {
      slots.push_back(s0);
    }
  }
  for (int sl : slots) {
    ShadowHull h{};
    h.slot = (uint8_t)sl;
    h.first_vert = (uint16_t)(out->pos.size() / 3);
    h.first_tri = (uint16_t)(out->tris.size() / 3);
    h.first_edge = (uint16_t)(out->edges.size() / 4);
    std::vector<int> local(nverts, -1);
    int nv = 0;
    std::vector<int> ht;
    for (int k = 0; k < ntris; k++) {
      const uint16_t* t = tris + 3 * k;
      if (t[0] >= nverts || t[1] >= nverts || t[2] >= nverts || slot[t[0]] != sl ||
          slot[t[1]] != sl || slot[t[2]] != sl) {
        continue;
      }
      for (int i = 0; i < 3; i++) {
        if (local[t[i]] < 0) {
          local[t[i]] = nv++;
          for (int c = 0; c < 3; c++) {
            out->pos.push_back(pos[3 * t[i] + c]);
          }
        }
        out->tris.push_back((uint8_t)local[t[i]]);
      }
      ht.push_back(k);
    }
    h.nverts = (uint16_t)nv;
    h.ntris = (uint16_t)ht.size();
    if (nv > 64 || h.ntris > 128) {
      return false;
    }
    // edges: each between two triangles (closed hull)
    const uint8_t* lt = out->tris.data() + 3 * h.first_tri;
    std::vector<int> other(h.ntris * 3, -1);
    int ne = 0;
    for (int k = 0; k < h.ntris; k++) {
      for (int e = 0; e < 3; e++) {
        const int a = lt[3 * k + e], b = lt[3 * k + (e + 1) % 3];
        if (other[3 * k + e] >= 0) {
          continue;  // (found from the other triangle)
        }
        // the triangle with b -> a
        int found = -1;
        for (int m = 0; m < h.ntris && found < 0; m++) {
          for (int f = 0; f < 3; f++) {
            if (m != k && lt[3 * m + f] == b && lt[3 * m + (f + 1) % 3] == a) {
              found = m;
              other[3 * m + f] = k;
              break;
            }
          }
        }
        if (found < 0) {
          return false;
        }
        other[3 * k + e] = found;
        out->edges.push_back((uint8_t)a);
        out->edges.push_back((uint8_t)b);
        out->edges.push_back((uint8_t)k);
        out->edges.push_back((uint8_t)found);
        ne++;
      }
    }
    h.nedges = (uint16_t)ne;
    if (ne > 192) {
      return false;
    }
    out->hulls.push_back(h);
  }
  out->view.pos = out->pos.data();
  out->view.tris = out->tris.data();
  out->view.edges = out->edges.data();
  out->view.hulls = out->hulls.data();
  out->view.nhulls = (int)out->hulls.size();
  return !out->hulls.empty();
}

namespace {
ShadowFrame g_shadow[2];
}

void note_shadow_frame(uint64_t frame, const ShadowMesh* mesh, const float (*mats)[16]) {
  ShadowFrame& f = g_shadow[frame & 1];
  f.frame = frame;
  f.mesh = mesh;
  memcpy(f.mats, mats, sizeof(f.mats));
}

const ShadowFrame* shadow_frame(uint64_t frame) {
  const ShadowFrame& a = g_shadow[frame & 1];
  if (a.mesh && a.frame == frame) {
    return &a;
  }
  const ShadowFrame& b = g_shadow[(frame + 1) & 1];
  if (b.mesh && b.frame + 1 == frame) {
    return &b;
  }
  return nullptr;
}

namespace {
void hull_vertex(const ShadowFrame& f, const ShadowHull& h, int v, float sign, float* out) {
  const float* m = f.mats[h.slot];
  const float* p = f.mesh->pos + 3 * (h.first_vert + v);
  for (int row = 0; row < 3; row++) {
    out[row] = sign * (m[row] * p[0] + m[4 + row] * p[1] + m[8 + row] * p[2] + m[12 + row]);
  }
}
}  // namespace

void shadow_center(const ShadowFrame& f, float out[3]) {
  double s[3] = {0, 0, 0};
  int n = 0;
  for (int i = 0; i < f.mesh->nhulls; i++) {
    const ShadowHull& h = f.mesh->hulls[i];
    for (int v = 0; v < h.nverts; v++) {
      float p[3];
      hull_vertex(f, h, v, 1.f, p);
      for (int c = 0; c < 3; c++) {
        s[c] += p[c];
      }
      n++;
    }
  }
  for (int c = 0; c < 3; c++) {
    out[c] = n ? (float)(s[c] / n) : 0.f;
  }
}

bool hull_volume(const ShadowFrame& f,
                 int hull,
                 float sign,
                 const float light[3],
                 const float plane[4],
                 HullVolume* out) {
  const ShadowHull& h = f.mesh->hulls[hull];
  if (h.nverts > 64 || h.ntris > 128 || h.nedges > 192) {
    return false;
  }
  const float nl = plane[0] * light[0] + plane[1] * light[1] + plane[2] * light[2];
  if (!(nl > 1e-4f || nl < -1e-4f)) {
    return false;
  }
  float c[3] = {0, 0, 0};
  for (int v = 0; v < h.nverts; v++) {
    float* T = out->T + 4 * v;
    hull_vertex(f, h, v, sign, T);
    T[3] = 1.f;
    // along the light onto the bottom plane (not upward: a vertex under it stays)
    const float t = std::max(0.f, (plane[3] - (plane[0] * T[0] + plane[1] * T[1] + plane[2] * T[2])) / nl);
    float* D = out->D + 4 * v;
    for (int k = 0; k < 3; k++) {
      D[k] = T[k] + light[k] * t;
      c[k] += T[k];
    }
    D[3] = 1.f;
  }
  for (int k = 0; k < 3; k++) {
    c[k] /= std::max<int>(h.nverts, 1);
  }
  out->nv = h.nverts;
  // facing per triangle (outward: the hull's center behind it, whatever the matrices' handedness)
  uint8_t facing[128];
  bool flip[128];
  const uint8_t* tr = f.mesh->tris + 3 * h.first_tri;
  out->ntris = 0;
  for (int k = 0; k < h.ntris; k++) {
    const float* a = out->T + 4 * tr[3 * k];
    const float* b = out->T + 4 * tr[3 * k + 1];
    const float* d = out->T + 4 * tr[3 * k + 2];
    const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]};
    const float e2[3] = {d[0] - a[0], d[1] - a[1], d[2] - a[2]};
    float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2],
                  e1[0] * e2[1] - e1[1] * e2[0]};
    const float out_dot = n[0] * (a[0] - c[0]) + n[1] * (a[1] - c[1]) + n[2] * (a[2] - c[2]);
    flip[k] = out_dot < 0;
    if (flip[k]) {
      for (float& x : n) {
        x = -x;
      }
    }
    facing[k] = n[0] * light[0] + n[1] * light[1] + n[2] * light[2] < 0;
    if (facing[k]) {
      uint8_t* o = out->tris + 4 * out->ntris++;
      o[0] = tr[3 * k];
      o[1] = flip[k] ? tr[3 * k + 2] : tr[3 * k + 1];
      o[2] = flip[k] ? tr[3 * k + 1] : tr[3 * k + 2];
      o[3] = 0;
    }
  }
  // silhouette edges: one side facing the light; (a, b) in the facing triangle's outward order
  const uint8_t* ed = f.mesh->edges + 4 * h.first_edge;
  out->nedges = 0;
  for (int k = 0; k < h.nedges; k++) {
    const uint8_t* e = ed + 4 * k;
    const int t0 = e[2], t1 = e[3];
    if (facing[t0] == facing[t1]) {
      continue;
    }
    // e[0] -> e[1] is t0's stored order
    const int ft = facing[t0] ? t0 : t1;
    bool ab = ft == t0;  // the edge runs a -> b in the facing triangle's stored order
    if (flip[ft]) {
      ab = !ab;
    }
    uint8_t* o = out->edges + 4 * out->nedges++;
    o[0] = ab ? e[0] : e[1];
    o[1] = ab ? e[1] : e[0];
    o[2] = 0;
    o[3] = 0;
  }
  return true;
}

}  // namespace ctr_keira
