// (AI-assisted) tod_diff: what CtrLevel.cpp's time of day code uses, without the renderer, and
// the level file tod_extract writes
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "common/common_types.h"
#include "game/graphics/ctr/CtrTodPulse.h"
#include "game/graphics/ctr/c3l_format.h"

namespace lg {
template <typename... A>
inline void info(A&&...) {}
template <typename... A>
inline void warn(A&&...) {}
}  // namespace lg
struct shim_mutex {
  void lock() {}
  void unlock() {}
};
struct ShimSettings {
  int tod_pulse_step = 4;
};
inline ShimSettings& ctr_settings() {
  static ShimSettings s;
  return s;
}
inline double ctr_gpu_time_ms() {
  return 0;
}
extern "C" void tod_flush_note(u32 bytes);
inline void ctr_gpu_flush_range(const void*, u32 bytes) {
  tod_flush_note(bytes);
}
#ifdef __3DS__
// (the ARM build: the list room may always grow)
struct CtrHeapStats {
  u32 free, largest;
};
inline void ctr_heap_stats(CtrHeapStats* s) {
  s->free = s->largest = 64u << 20;
}
#endif
struct CtrBackgroundCamera {
  s32 itimes[4][4];
};
enum class GameVersion { Jak1, Jak2 };

// tod_extract's file
struct DumpChunk {
  u32 vertex_count, lod_tier, kind;
  float bsphere[4];
  const c3l::Vertex* v;
  u32 ngroups;
  const u16* groups;  // lo, hi pairs
};
struct Dump {
  u32 nc, nm;
  const c3l::TodColorA* colors;
  const c3l::TodMul* muls;
  std::vector<DumpChunk> chunks;
};
inline Dump parse_dump(const u8* p) {
  Dump d;
  auto get32 = [&]() {
    u32 v;
    memcpy(&v, p, 4);
    p += 4;
    return v;
  };
  d.nc = get32();
  d.nm = get32();
  d.colors = (const c3l::TodColorA*)p;
  p += d.nc * sizeof(c3l::TodColorA);
  d.muls = (const c3l::TodMul*)p;
  p += d.nm * sizeof(c3l::TodMul);
  const u32 nch = get32();
  for (u32 i = 0; i < nch; i++) {
    DumpChunk c;
    c.vertex_count = get32();
    c.lod_tier = get32();
    c.kind = get32();
    memcpy(c.bsphere, p, 16);
    p += 16;
    c.v = (const c3l::Vertex*)p;
    p += c.vertex_count * sizeof(c3l::Vertex);
    c.ngroups = get32();
    c.groups = (const u16*)p;
    p += c.ngroups * 4;
    d.chunks.push_back(c);
  }
  return d;
}
