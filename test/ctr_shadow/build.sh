#!/usr/bin/env bash
# (AI-assisted) builds the host test of the 3DS shadow volumes (shadow_test.cpp) to
# ${OUT:-/tmp/ctr_shadow_test}: the native decoder (CtrShadow.cpp) and the PC renderer's emulated
# VU1 shadow program (Shadow_PS2.cpp with its real ShadowRenderer.h; vu_ref/ first on the include
# path stands in for its OpenGL base class), the scalar SIMD fallbacks like the 3DS, no fused
# multiply-add (the ARM11's VFP doesn't fuse), the GPU stubbed in the test.
set -euo pipefail
R="$(cd "$(dirname "$0")/../.." && pwd)"
OUT=${OUT:-/tmp/ctr_shadow_test}
c++ -std=c++20 -O2 -ffp-contract=off -DOPENGOAL_SCALAR_SIMD=1 -DFMT_HEADER_ONLY \
  -I"$R/test/ctr_shadow/vu_ref" \
  -I"$R" -I"$R/third-party/fmt/include" -I"$R/third-party" -I"$R/third-party/SDL/include" \
  "$R/test/ctr_shadow/shadow_test.cpp" \
  "$R/game/graphics/ctr/CtrShadow.cpp" \
  "$R/game/graphics/opengl_renderer/Shadow_PS2.cpp" \
  "$R/common/dma/dma.cpp" "$R/common/dma/gs.cpp" \
  -o "$OUT"
echo "built $OUT"
