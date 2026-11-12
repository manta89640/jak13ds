#pragma once

// The level renderer's fixed detail floor (what config.ini auto_detail = fixed drew): beyond ring 0
// the shrub fade bands drawn nearer and tie chunks that have a vista copy drawn by it from
// kTieFarMeters. The same every frame: nothing moves with the load.

#include <algorithm>

namespace ctr_detail {

// nothing nearer than this to the camera is cut by the floor (meters)
constexpr float kNearRingMeters = 40.f;
// a distance limit's fade band: from the limit to this times it
constexpr float kFadeBand = 1.1f;
constexpr float kOldTieLodPx = 4.f;
constexpr float kOldLodDistance = 200.f;  // meters (Jak II)
constexpr float kOldTieScale = 1.5f;      // (Jak II)
constexpr float kShrubBandK = 0.5f;
constexpr float kTieFarMeters = 150.f;

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
// a shrub's fade band [x, w] (full detail to x, gone at w) at factor k: a band that starts beyond
// ring 0 moves toward it (the same shape), one that starts inside stays
inline void shrub_band(float* x, float* w, float k, float near_ring) {
  if (!(k < 1.f) || !(*x > 0) || *x < near_ring) {
    return;
  }
  const float nx = std::max(*x * k, near_ring);
  *w = std::max(*w * nx / *x, nx);
  *x = nx;
}

// the floor's values for a config (tie_lod_px, lod_distance and tie_distance_scale as the config
// has them, unless the floor is coarser); on: Jak 1 / Jak II (other games: the config's)
struct Floor {
  bool on = false;
  float tie_lod_px = 4.f;
  float lod_distance = 0.f;        // meters, 0: none
  float tie_distance_scale = 0.f;  // 0: none
  float shrub_far = 1.f;           // shrub fade bands beyond ring 0 x this (shrub_band)
  float tie_far = 0.f;             // tie chunks by their vista copies from this (meters, 0: never)
};
inline Floor floor_of(float px, float lod, float tie, float draw_distance, bool jak1, bool jak2) {
  Floor f;
  f.on = jak1 || jak2;
  f.tie_lod_px = px;
  f.lod_distance = lod;
  f.tie_distance_scale = tie;
  if (!f.on) {
    return f;
  }
  const float draw = draw_distance > 0 ? draw_distance : 2000.f;
  f.tie_lod_px = std::max(px, kOldTieLodPx);
  if (jak2) {
    f.lod_distance = (lod > 0 ? lod : draw) > kOldLodDistance ? kOldLodDistance : lod;
    f.tie_distance_scale = (tie > 0 ? tie : draw / 100.f) > kOldTieScale ? kOldTieScale : tie;
  }
  f.shrub_far = kShrubBandK;
  f.tie_far = draw > kTieFarMeters ? kTieFarMeters : 0.f;
  return f;
}

}  // namespace ctr_detail
