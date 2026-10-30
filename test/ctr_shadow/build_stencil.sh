#!/usr/bin/env bash
# builds the host test of the 3DS shadow volumes' stencil result (stencil_test.cpp)
# to ${OUT:-/tmp/ctr_stencil_test}: CtrShadow.cpp as the 3DS runs it (no fused multiply-add).
set -euo pipefail
R="$(cd "$(dirname "$0")/../.." && pwd)"
OUT=${OUT:-/tmp/ctr_stencil_test}
c++ -std=c++20 -O2 -ffp-contract=off -DFMT_HEADER_ONLY \
  -I"$R" -I"$R/third-party/fmt/include" -I"$R/third-party" -I"$R/third-party/SDL/include" \
  "$R/test/ctr_shadow/stencil_test.cpp" \
  "$R/game/graphics/ctr/CtrShadow.cpp" \
  "$R/game/graphics/ctr/CtrKeiraRig.cpp" \
  "$R/common/dma/dma.cpp" \
  -o "$OUT"
echo "built $OUT"
