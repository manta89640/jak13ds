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
  int jak_count = 0;
  const char* name[kMaxJoints];
};

/*! The rig (made once). */
const Rig& rig();

/*! Converter: the part of a vertex's weight on Keira's chest that goes to the virtual upper chest
 * joint, by its bind height (game units): her chest bends and twists at his chest joint too. */
float chest2_share(float y);

/*! Index of a Keira joint by name, -1 if none. */
int keira_joint(const char* name);

/*! Renderer: Keira's bone matrices (rig joint r in out[r + 1], the slots her vertices use: 12
 * floats per slot used of 16: tmat[0..2] = the linear part's columns, tmat[3] = translation, as
 * the game's bone matrices) from Jak's (jak[s] = the game's matrix of slot s, sent[] = the slots
 * the game sent this frame: a Jak joint it didn't send takes its nearest sent ancestor's). Returns
 * false if the game sent none of the joints the rig needs. */
bool pose(const float* const* jak, const uint64_t sent[2], float (*out)[16], int out_slots);

}  // namespace ctr_keira
