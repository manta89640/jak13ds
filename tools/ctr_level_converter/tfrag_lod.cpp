#include "tfrag_lod.h"

#include <cstring>
#include <fstream>
#include <iterator>

namespace tfrag_lod {

bool read_morph(const std::string& path, Morph* out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) {
    return false;
  }
  std::vector<char> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  size_t at = 0;
  auto take = [&](void* dst, size_t n) {
    if (at + n > data.size()) {
      return false;
    }
    memcpy(dst, data.data() + at, n);
    at += n;
    return true;
  };
  tfrag_morph::FileHeader fh;
  if (!take(&fh, sizeof(fh)) || memcmp(fh.magic, tfrag_morph::kMagic, 4) ||
      fh.version != tfrag_morph::kVersion) {
    return false;
  }
  out->close = fh.close;
  out->far = fh.far;
  for (u32 t = 0; t < fh.tree_count; t++) {
    tfrag_morph::TreeHeader th;
    if (!take(&th, sizeof(th)) || th.geo >= 2) {
      return false;
    }
    auto& trees = out->trees[th.geo];
    if (trees.size() <= th.tree) {
      trees.resize(th.tree + 1);
    }
    trees[th.tree].resize(th.vertex_count);
    if (!take(trees[th.tree].data(), th.vertex_count * sizeof(tfrag_morph::Vertex))) {
      return false;
    }
  }
  return at == data.size();
}

void subdivide_dists(float close, float far, float out[3]) {
  // level.gc: 0 / 0 in the bsp-header means 30 m / 70 m
  if (close == 0.f && far == 0.f) {
    close = 122880.f;
    far = 286720.f;
  }
  const float s = (far - close) * 0.14285715f;
  const float m3 = close + 0.5f * s, m2 = m3 + s, m1 = m2 + 2.f * s;
  out[0] = m2;
  out[1] = m1;
  out[2] = far;
}

}  // namespace tfrag_lod
