#!/usr/bin/env bash
# (AI-assisted) builds the host test of the sound voice (voice_exact.cpp): game/sound/common's
# voice against the frozen copy of it in ref/. Two programs: ${OUT:-/tmp/snd_voice_exact} with
# the voice code as the PC builds it, and ${OUT}_arm with the code the 3DS runs (its ARM DSP
# instructions done in plain C++, SND_TEST_ARM_DSP).
set -euo pipefail
R="$(cd "$(dirname "$0")/../.." && pwd)"
OUT=${OUT:-/tmp/snd_voice_exact}
for variant in "" _arm; do
  c++ -std=c++20 -O2 -I"$R" ${variant:+-DSND_TEST_ARM_DSP} \
    "$R/test/snd_voice/voice_exact.cpp" \
    "$R/game/sound/common/voice.cpp" "$R/game/sound/common/envelope.cpp" \
    "$R/test/snd_voice/ref/voice.cpp" "$R/test/snd_voice/ref/envelope.cpp" \
    -o "$OUT$variant"
  echo "built $OUT$variant"
done
