#pragma once

// The level renderer's fixed detail floor (what config.ini auto_detail = fixed drew): tie chunks
// that have a vista copy drawn by it from kTieFarMeters. The same every frame: nothing moves with
// the load.

#include <algorithm>

namespace ctr_detail {

// nothing nearer than this to the camera is cut by the floor (meters)
constexpr float kNearRingMeters = 40.f;
// a distance limit's fade band: from the limit to this times it
constexpr float kFadeBand = 1.1f;
constexpr float kOldTieLodPx = 4.f;
constexpr float kOldTieScale = 1.5f;      // (Jak II)
constexpr float kTieFarMeters = 150.f;
// Jak II's title screen (its title level loaded, the whole title through): the logo's street view
// is ~20k far tie triangles over budget at 150 m
constexpr float kTieFarMetersJak2Title = 120.f;

// a distance limit of the floor (0: none): never under the config's one within ring 0
inline float ring_limit(float floor_lim, float config_lim, float near_ring) {
  if (!(floor_lim > 0)) {
    return floor_lim;
  }
  return std::max(floor_lim, config_lim > 0 ? std::min(config_lim, near_ring) : near_ring);
}
// a limit d (0: none) cut to `cap` (0: none), never under ring 0
inline float far_cap(float d, float cap, float near_ring) {
  if (!(cap > 0)) {
    return d;
  }
  return d > 0 ? std::max(std::min(d, cap), std::min(d, near_ring)) : std::max(cap, near_ring);
}
// how much of a chunk at `dn` is drawn with the limit `lim` (0: none)
inline float band_alpha(float dn, float lim) {
  if (!(lim > 0) || dn <= lim) {
    return 1.f;
  }
  return std::max(0.f, (lim * kFadeBand - dn) / (lim * (kFadeBand - 1.f)));
}

// the floor's values for a config (tie_lod_px and tie_distance_scale as the config
// has them, unless the floor is coarser); on: Jak 1 / Jak II (other games: the config's)
struct Floor {
  bool on = false;
  float tie_lod_px = 4.f;
  float tie_distance_scale = 0.f;  // 0: none
  float tie_far = 0.f;             // tie chunks by their vista copies from this (meters, 0: never)
};
inline Floor floor_of(float px,
                      float tie,
                      float draw_distance,
                      bool jak1,
                      bool jak2,
                      bool jak2_title = false) {
  Floor f;
  f.on = jak1 || jak2;
  f.tie_lod_px = px;
  f.tie_distance_scale = tie;
  if (!f.on) {
    return f;
  }
  const float draw = draw_distance > 0 ? draw_distance : 2000.f;
  f.tie_lod_px = std::max(px, kOldTieLodPx);
  if (jak2) {
    f.tie_distance_scale = (tie > 0 ? tie : draw / 100.f) > kOldTieScale ? kOldTieScale : tie;
  }
  const float tie_far = jak2_title ? kTieFarMetersJak2Title : kTieFarMeters;
  f.tie_far = draw > tie_far ? tie_far : 0.f;
  return f;
}

}  // namespace ctr_detail
