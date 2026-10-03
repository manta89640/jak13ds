#!/usr/bin/env bash
# (AI-assisted) builds the host replay test of the 3DS full ocean (ocean_replay.cpp) to
# ${OUT:-/tmp/ocean_replay}: the 3DS renderer's ocean sources with the scalar SIMD fallbacks like
# the 3DS, no fused multiply-add (the ARM11's VFP doesn't fuse), the GPU stubbed in the test.
set -euo pipefail
R="$(cd "$(dirname "$0")/../.." && pwd)"
OUT=${OUT:-/tmp/ocean_replay}
c++ -std=c++20 -O2 -ffp-contract=off -DOPENGOAL_SCALAR_SIMD=1 -DFMT_HEADER_ONLY \
  -I"$R" -I"$R/third-party/fmt/include" -I"$R/third-party" -I"$R/third-party/SDL/include" \
  "$R/test/ctr_ocean/ocean_replay.cpp" \
  "$R/game/graphics/ctr/CtrOceanFull.cpp" "$R/game/graphics/ctr/CtrOceanNative.cpp" \
  "$R/game/graphics/ctr/CtrOceanVuMid.cpp" "$R/game/graphics/ctr/CtrOceanVuNear.cpp" \
  "$R/common/dma/dma.cpp" "$R/common/dma/gs.cpp" \
  -o "$OUT"
echo "built $OUT"
