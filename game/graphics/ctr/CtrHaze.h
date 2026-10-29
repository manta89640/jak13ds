#pragma once

// (AI-assisted, levellod, Oct 29) the port's distance haze, one rule for the level meshes
// (CtrLevel.cpp draw_level) and the merc models (shaders.md S3).
//
// The PC draws the whole level and has only the game's fog (tfrag3.vert / merc2.vert: the fog
// amount from clip w, clamped by the mood's fog range). The 3DS stops drawing geometry at a
// distance, so it adds a haze towards the fog color that reaches 1 exactly where that geometry
// ends; the fog is the larger of the two (the mesh programs' max with clip[4]):
//
//   haze = clamp((depth - start) / (end - start), 0, 1),  fog = max(game fog, haze)
//
// depth: camera-space depth in game units (4096 per meter) = clip w / pzw, pzw =
// |perspective[2][3]| (clip w is the PS2 perspective w for level meshes and merc alike).
// end: where the geometry being drawn stops:
//   - draw_distance, for a level without a vista tier;
//   - vista_distance, for the vista pass (the vista tier, lowres_vista's far scenery) and for every
//     chunk of a level with a vista tier (what draw_distance cuts goes on in its vista copies:
//     finalboss);
//   - none (haze 0) when that distance is 0 (no limit) or reaches the PS2's far clip plane.
// start: fog_start x end (0.6: 300 m of 500, 1.8 km of 3 km). Before Oct 29 a level with a vista
// tier was hazed from fog_start x draw_distance (300 m) for all its geometry: finalboss's far city
// and sea 26-63% fog color at 1-2 km, where the PC has none.
//
// row: the mesh programs' fog1 {1 / pzw, start, 1 / (end - start), 0} ({0, 1, 0, 0}: no haze),
// i.e. haze = (w * row[0] - row[1]) * row[2]. As a dph row on the position (mesh_fog_rows): the
// clip w row times row[0] * row[2], plus -row[1] * row[2] in its w.

#include <algorithm>

#include "game/graphics/ctr/CtrSettings.h"

// vista_pass: the vista pass's draws; level_vista: the level has a vista tier (merc:
// ctr_haze_vista)
inline void ctr_haze_row(const CtrSettings& s, bool vista_pass, bool level_vista, float pzw,
                         float row[4]) {
  row[0] = 0.f;
  row[1] = 1.f;
  row[2] = 0.f;
  row[3] = 0.f;
  const float draw = s.draw_distance * 4096.f, far = s.vista_distance * 4096.f;
  float end = draw;
  if (vista_pass) {
    end = far;
  } else if (level_vista && draw > 0 && far > draw) {
    end = far;  // (a vista tier only goes on past the draw distance when vista_distance is larger)
  }
  // (geometry out to the PS2's far clip plane, math-camera f = 10 km, ends where the PS2's does)
  constexpr float kPs2Far = 10000.f * 4096.f;
  if (end <= 0 || end >= kPs2Far || !(pzw > 0)) {
    return;
  }
  const float start = end * std::clamp(s.fog_start, 0.f, 0.999f);
  row[0] = 1.f / pzw;
  row[1] = start;
  row[2] = 1.f / std::max(end - start, 1.f);
}

// (render thread) a level drawn this frame in near mode has a vista tier: the merc models' haze
// then follows the vista rule (CtrLevel.cpp sets it per frame; frame: CtrRenderState::frame_idx)
void ctr_haze_note_vista(unsigned long long frame);
bool ctr_haze_vista(unsigned long long frame);
