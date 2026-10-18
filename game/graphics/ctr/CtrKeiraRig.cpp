/*!
 * @file CtrKeiraRig.cpp
 * (AI-assisted)
 * See CtrKeiraRig.h.
 */

#include "CtrKeiraRig.h"

#include <cmath>
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

bool pose(const float* const* jak, const uint64_t sent[2], float (*out)[16], int out_slots) {
  const Rig& r = rig();
  auto is_sent = [&](int slot) { return slot < 128 && (sent[slot >> 6] >> (slot & 63)) & 1; };
  // the linear part (columns c: m[4 * c + row]) and the place of each rig joint, camera space
  float lin[kMaxJoints][9];
  float q[kMaxJoints][3];
  bool any = false;
  for (int i = 0; i < r.count; i++) {
    const int k = r.order[i];
    // the nearest sent Jak joint
    int j = r.jak[k];
    while (j >= 0 && !is_sent(j + 1)) {
      j = r.jak_parent[j];
    }
    const float* t = nullptr;
    if (j >= 0) {
      t = jak[j + 1];
      any = true;
    } else if (r.parent[k] >= 0) {
      t = nullptr;
    }
    float L[9];  // row major: L[3 * row + col], col c = t[4 * c + row]
    float T[3];
    if (t) {
      for (int row = 0; row < 3; row++) {
        for (int c = 0; c < 3; c++) {
          L[3 * row + c] = t[4 * c + row];
        }
        T[row] = t[12 + row];
      }
    } else {
      identity(L);
      T[0] = T[1] = T[2] = 0;
    }
    // M = L * corr
    float* M = lin[k];
    const float* C = r.corr[k];
    for (int row = 0; row < 3; row++) {
      for (int c = 0; c < 3; c++) {
        M[3 * row + c] = L[3 * row] * C[c] + L[3 * row + 1] * C[3 + c] + L[3 * row + 2] * C[6 + c];
      }
    }
    const float* p = r.pos[k];
    const int par = r.parent[k];
    if (r.anchor[k] || par < 0 || !t) {
      for (int row = 0; row < 3; row++) {
        q[k][row] = L[3 * row] * p[0] + L[3 * row + 1] * p[1] + L[3 * row + 2] * p[2] + T[row];
      }
    } else {
      const float* pp = r.pos[par];
      const float d[3] = {p[0] - pp[0], p[1] - pp[1], p[2] - pp[2]};
      const float* Mp = lin[par];
      for (int row = 0; row < 3; row++) {
        q[k][row] = q[par][row] + Mp[3 * row] * d[0] + Mp[3 * row + 1] * d[1] + Mp[3 * row + 2] * d[2];
      }
    }
    if (k + 1 < out_slots) {
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
  }
  return any;
}

}  // namespace ctr_keira
