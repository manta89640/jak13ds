#pragma once

/*!
 * @file CtrKeiraRig.h
 * (AI-assisted)
 * "Play as Keira" easter egg (hold L + R when starting a new game on an empty save slot): Keira's
 * model (village1's assistant-lod0) drawn instead of Jak's, driven by Jak's animation.
 *
 * Keira keeps her own mesh, skinning and proportions; her skeleton (the "rig": her joints plus a
 * virtual upper chest joint) takes its joint rotations from Jak's bones every frame: each rig joint
 * follows one Jak joint (by name and place: her shoulder his upper arm, her elbow his forearm,
 * ...; her extra joints (hair, goggles, straps, flaps) follow her parent's), at her own joint
 * positions (forward kinematics from her root, which Jak's root places, so his crouch, bob and
 * jumps move her too). Where her bind pose points a bone elsewhere than his (his legs are a little
 * apart), a fixed correction turns her bone his way first.
 *
 * Shared by the converter (tools/ctr_level_converter: KEIRA.c3l, her vertices bound to the rig's
 * joints, matrix slot = rig joint + 1) and the 3DS renderer (CtrMercRenderer: her bone matrices
 * from Jak's every frame, pose()).
 */

#include <cstdint>
#include <vector>

namespace ctr_keira {

// the model the game asks for when the save has Keira: kModelPrefix + Jak's model name
constexpr char kModelPrefix[] = "keira-as-";
constexpr int kModelPrefixLen = sizeof(kModelPrefix) - 1;
// the file it is in (out/jak1/c3l/KEIRA.c3l), loaded the first time the game asks for her
constexpr char kFileName[] = "KEIRA";

constexpr int kMaxJoints = 100;

struct Rig {
  int count = 0;               // Keira's joints, then the virtual ones
  int parent[kMaxJoints];      // rig joint, -1: none
  int jak[kMaxJoints];         // the Jak joint whose rotation it takes (matrix slot jak + 1)
  bool anchor[kMaxJoints];     // placed by its Jak joint's matrix (the root), not from its parent
  float pos[kMaxJoints][3];    // bind position, game units (4096 = 1 m)
  float corr[kMaxJoints][9];   // bind correction (row major 3x3 rotation)
  int order[kMaxJoints];       // parents before children
  int chest2 = -1;             // the virtual upper chest joint (follows Jak's chest)
  int keira_joints = 0;        // Keira's own joints (rig joints 0..keira_joints-1)
  int jak_parent[128];         // Jak's skeleton
  float jak_pos[128][3];       // his bind positions, game units
  int jak_count = 0;
  // (AI-assisted) the arms (two-bone IK onto Jak's wrists): shoulder, elbow, hand per side
  int arm[2][3] = {{-1, -1, -1}, {-1, -1, -1}};
  // the ponytail (a spring): its base joint and the joint at its tip
  int tail = -1, tail_tip = -1;
  const char* name[kMaxJoints];
};

/*! The rig (made once). */
const Rig& rig();

/*! Converter: the part of a vertex's weight on Keira's chest that goes to the virtual upper chest
 * joint, by its bind height (game units): her chest bends and twists at his chest joint too. */
float chest2_share(float y);

/*! Index of a Keira joint by name, -1 if none. */
int keira_joint(const char* name);

/*! (AI-assisted) The camera this frame (merc camera space -> world: world = trans + rot * p, rot
 * row major), for the ponytail's spring (it swings in the world). */
struct Camera {
  float rot[9];
  float trans[3];
};

/*! Renderer: Keira's bone matrices (rig joint r in out[r + 1], the slots her vertices use: 12
 * floats per slot used of 16: tmat[0..2] = the linear part's columns, tmat[3] = translation, as
 * the game's bone matrices) from Jak's (jak[s] = the game's matrix of slot s, sent[] = the slots
 * the game sent this frame: a Jak joint it didn't send takes its nearest sent ancestor's). Returns
 * false if the game sent none of the joints the rig needs. */
bool pose(const float* const* jak,
          const uint64_t sent[2],
          float (*out)[16],
          int out_slots,
          const Camera* cam = nullptr,
          uint64_t frame = 0);
// pose() also moves her hands onto Jak's wrists (two-bone IK of her shoulder and elbow, the arm
// stretched along its bones up to kMaxArmStretch when his reach is longer) and swings her ponytail
// (a damped spring on its tip, in the world: a step per new frame with a camera, the last swing
// without one)
constexpr float kMaxArmStretch = 1.2f;

/*!
 * (AI-assisted) Her face (blend shapes, 40 weights like the game's; 4096 = a target fully on): Jak's
 * weights when the game sends them (cutscenes), each to her target that moves the same part the
 * same way (jaw, lips, mouth corners, eyelids, brows: matched by where and which way they move
 * vertices, see keira_model.cpp), and a blink of her own eyelids every few seconds (deterministic,
 * by frame). Returns true if any weight isn't 0.
 */
constexpr int kFaceWeights = 40;
bool face_weights(const float* jak /* nullptr: none */, uint64_t frame, float out[kFaceWeights]);

/*!
 * (AI-assisted) Her shadow: convex hulls of her body parts (the converter's "~shadow" model), each
 * rigid on one rig slot, closed and wound outward. CtrShadowVolumes draws them as shadow volumes in
 * place of Jak's (the game's shadow mesh is his).
 */
constexpr char kShadowSuffix[] = "~shadow";
struct ShadowHull {
  uint8_t slot;            // rig joint + 1
  uint16_t first_vert, nverts;
  uint16_t first_tri, ntris;
  uint16_t first_edge, nedges;
};
struct ShadowMesh {
  float* pos = nullptr;      // xyz per vertex (model space, game units)
  uint8_t* tris = nullptr;   // 3 hull-local vertex indices per triangle (outward)
  uint8_t* edges = nullptr;  // per edge: a, b (as in tris[2 * k]), triangle k, triangle m
  ShadowHull* hulls = nullptr;
  int nhulls = 0;
};
/*! The mesh and its storage (CtrLevel, at load). */
struct ShadowMeshData {
  std::vector<float> pos;
  std::vector<uint8_t> tris, edges;
  std::vector<ShadowHull> hulls;
  ShadowMesh view;
};
/*! From the "~shadow" model's vertices (model space positions, their slot) and triangles: the
 * hulls (the triangles of a slot), their edges. False if a hull isn't closed. */
bool build_shadow_mesh(const float* pos,
                       const uint8_t* slot,
                       int nverts,
                       const uint16_t* tris,
                       int ntris,
                       ShadowMeshData* out);

/*! The rig's matrices of a frame, for the shadow bucket (which comes before her merc bucket). */
struct ShadowFrame {
  uint64_t frame = 0;
  const ShadowMesh* mesh = nullptr;
  float mats[kMaxJoints + 1][16];
};
/*! Merc renderer (both threads: the bone snapshot, then the draw), after pose(). */
void note_shadow_frame(uint64_t frame, const ShadowMesh* mesh, const float (*mats)[16]);
/*! Shadow renderer: the matrices of this frame or the one before (nullptr: none). */
const ShadowFrame* shadow_frame(uint64_t frame);

/*! One hull of her shadow, as the VU1 shadow program's input: T (camera space vertices, 4 floats
 * each), D (them moved along the light onto the bottom plane), the light-facing triangles
 * (i0, i1, i2, 0) and the silhouette edges (a, b, 0, 0: side quad T[b] T[a] D[a] D[b]). light:
 * the direction it travels, plane: n.x = d (camera space). Returns false if it can't be made. */
struct HullVolume {
  float T[64 * 4], D[64 * 4];
  uint8_t tris[128 * 4], edges[192 * 4];
  int nv = 0, ntris = 0, nedges = 0;
};
bool hull_volume(const ShadowFrame& f,
                 int hull,
                 float sign,
                 const float light[3],
                 const float plane[4],
                 HullVolume* out);
/*! Center of all her hulls (camera space, the matrices' own sign). */
void shadow_center(const ShadowFrame& f, float out[3]);

}  // namespace ctr_keira
