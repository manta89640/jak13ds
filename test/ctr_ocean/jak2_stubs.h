#pragma once

// The Jak II parts CtrOceanFull links against (CtrOcean.cpp, CtrDirect.cpp, CtrRenderer.cpp),
// never reached by these Jak 1 tests.

#include "game/graphics/ctr/CtrDirect.h"
#include "game/graphics/ctr/CtrOcean.h"

bool ctr_is_jak2() {
  return false;
}
CtrDirect::CtrDirect(CtrVram* vram) : m_vram(vram) {}
void CtrDirect::reset_state() {}
void CtrDirect::render_vif(u32, u32, const u8*, u32) {}
void CtrDirect::flush() {}
void CtrOceanEnvJak2::draw_first_pass(CtrDirect*, DmaFollower&, CtrRenderState&) {}
CtrOceanEnvWarpJak2::~CtrOceanEnvWarpJak2() = default;
bool CtrOceanEnvWarpJak2::update(const float*, bool, u64, const CtrRenderState&) {
  return false;
}
bool CtrOceanEnvWarpJak2::first_pass_fresh(const CtrRenderState&) {
  return false;
}
