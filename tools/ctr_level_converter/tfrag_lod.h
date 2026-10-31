#pragma once

/*!
 * @file tfrag_lod.h
 * The PS2's tfrag geomorph (common/custom_data/TfragMorph.h, c3l::ExtTfrag): the level extractor's
 * <level>.tfragmorph and the subdivide distances.
 */

#include <string>
#include <vector>

#include "common/common_types.h"
#include "common/custom_data/TfragMorph.h"

namespace tfrag_lod {

struct Morph {
  float close = 0, far = 0;  // the file's (game units, 0: the defaults)
  // per geometry 0 / 1 and tfrag tree: its packed vertices' morph
  std::vector<std::vector<tfrag_morph::Vertex>> trees[2];
};
bool read_morph(const std::string& path, Morph* out);

// update-subdivide-settings!'s m2, m1, m0 (game units) for the level's close / far
void subdivide_dists(float close, float far, float out[3]);

}  // namespace tfrag_lod
