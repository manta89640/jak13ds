// (AI-assisted) tod_diff pixels (tod_pixels.cpp)
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

struct TodPxChunk {
  const float* bsphere;  // x, y, z, r (world)
  const float* origin;   // world = origin + scale * quantized position
  float scale;
  const uint16_t* indices;
  uint32_t index_count;
  const void* verts;  // vertex_count * 16 bytes: c3l::Vertex each, or split (ctr_gpu_mesh_split_colors)
  uint32_t vertex_count;
};
std::vector<unsigned char> tod_draw_pixels(const TodPxChunk* chunks, size_t count, bool split);
