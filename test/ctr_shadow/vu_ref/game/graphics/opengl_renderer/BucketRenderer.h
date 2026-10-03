#pragma once

/*!
 * (AI-assisted) test/ctr_shadow: as much of the OpenGL renderer's BucketRenderer.h as its
 * ShadowRenderer.h needs, so that the real ShadowRenderer.h and Shadow_PS2.cpp (the emulated VU1
 * shadow program) build without OpenGL. build.sh puts this folder first on the include path.
 */

#include <string>

#include "common/dma/dma_chain_read.h"

typedef unsigned int GLuint;
struct SharedRenderState;
class ScopedProfilerNode;

class BucketRenderer {
 public:
  BucketRenderer(const std::string& name, int my_id) : m_name(name), m_my_id(my_id) {}
  virtual ~BucketRenderer() = default;
  virtual void render(DmaFollower& dma,
                      SharedRenderState* render_state,
                      ScopedProfilerNode& prof) = 0;
  virtual void draw_debug_window() = 0;

 protected:
  std::string m_name;
  int m_my_id;
  bool m_enabled = true;
};
