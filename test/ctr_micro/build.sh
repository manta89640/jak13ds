#!/usr/bin/env bash
# (AI-assisted) builds the host bit tests of the 3DS renderers' CPU shortcuts (micro_test.cpp) to
# ${OUT:-/tmp/ctr_micro_test}: each new helper against a copy of the code it replaces, no fused
# multiply-add (the ARM11's VFP doesn't fuse).
set -euo pipefail
R="$(cd "$(dirname "$0")/../.." && pwd)"
OUT=${OUT:-/tmp/ctr_micro_test}
c++ -std=c++20 -O2 -ffp-contract=off -I"$R" -I"$R/third-party/fmt/include" -I"$R/third-party" \
  "$R/test/ctr_micro/micro_test.cpp" -o "$OUT"
echo "built $OUT"
