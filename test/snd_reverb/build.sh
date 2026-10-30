#!/usr/bin/env bash
# builds the host test of the SPU2 effect unit (reverb_exact.cpp) as ${OUT:-/tmp/snd_reverb_exact}
set -euo pipefail
R="$(cd "$(dirname "$0")/../.." && pwd)"
OUT=${OUT:-/tmp/snd_reverb_exact}
c++ -std=c++20 -O2 -I"$R" "$R/test/snd_reverb/reverb_exact.cpp" \
  "$R/game/sound/common/reverb.cpp" -o "$OUT"
echo "built $OUT"
