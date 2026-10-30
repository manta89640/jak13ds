#!/usr/bin/env bash
# (AI-assisted)
# Differential test of the 3DS level time of day (CtrLevel.cpp tod_update / tod_recolor and what
# they call): the code of a base commit ("ref") and of the working tree ("neu") run side by side
# on a real level file and weight traces, and every drawn vertex compared after every level draw.
# Exits 1 if any vertex differs.
#
#   test/ctr_render/tod_diff/run.sh <level.c3l> [--base REV] [--trace FILE] [--arm]
#
#   --base REV   the reference version (default HEAD)
#   --trace FILE a weight trace (lines: frame, then per palette r g b alpha; host CTR_TOD_LOG=2
#                lines converted by the same layout); default citadel_weights.txt (Citadel, 875
#                frames of the host game) plus random traces (tod_trace.py; one with palettes
#                1-3 moving every frame, the others rarely: light palettes) and a lightning trace
#                (Rock Village's flashes and flames, no day steps; village2 and swamp: neu also
#                checked against the PC's blend of every frame)
#   --arm        also the ARM instructions per level draw of both versions (devkitARM build run
#                under Unicorn, like test/mips2c_native/unicorn; ARMBENCH_PYTHON: a python3 with
#                unicorn). TOD_ARM_LINES=1: and the mesh cache lines each draw touches and writes
#   --pixels     also the chunks drawn, every 25th level draw, through the host soft renderer from
#                each version's own mesh layout: every pixel compared
#
# Cases: every 5th chunk drawn (about one view's worth), pulse step 4 and 0, all runs shown and
# runs shown/hidden at random, an older file's table (vertices keep their alpha), the title screen
# switched on and off half way.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
OUT="${TOD_DIFF_BUILD:-$ROOT/build-tod-diff}"
C3L=""
BASE=HEAD
TRACE=""
ARM=0
PIXELS=0
while [ $# -gt 0 ]; do
  case "$1" in
    --base) BASE="$2"; shift 2 ;;
    --trace) TRACE="$2"; shift 2 ;;
    --arm) ARM=1; shift ;;
    --pixels) PIXELS=1; shift ;;
    *) C3L="$1"; shift ;;
  esac
done
[ -n "$C3L" ] || { echo "usage: see header" >&2; exit 2; }
mkdir -p "$OUT"

# the two versions' time of day code: from "struct TodSeen {" to the host check, the Tod struct
# and CtrLight.h
extract() {
  awk '/^struct TodSeen \{/{on=1} /^#if !defined\(__3DS__\) \|\| defined\(CTR_TOD_CHECK_BUILD\)/{if(on){exit}} on' "$1" |
    sed 's/std::mutex/shim_mutex/g' > "$3.code.inc"
  awk '/^  struct Tod \{/{on=1} on{print} on && /^  \};/{exit}' "$2" > "$3.struct.inc"
}
git -C "$ROOT" show "$BASE:game/graphics/ctr/CtrLevel.cpp" > "$OUT/ref_CtrLevel.cpp"
git -C "$ROOT" show "$BASE:game/graphics/ctr/CtrLevel.h" > "$OUT/ref_CtrLevel.h"
extract "$OUT/ref_CtrLevel.cpp" "$OUT/ref_CtrLevel.h" "$OUT/ref"
extract "$ROOT/game/graphics/ctr/CtrLevel.cpp" "$ROOT/game/graphics/ctr/CtrLevel.h" "$OUT/neu"
git -C "$ROOT" show "$BASE:game/graphics/ctr/CtrLight.h" > "$OUT/ref.light.h"
cp "$ROOT/game/graphics/ctr/CtrLight.h" "$OUT/neu.light.h"
# (GCC's #pragma once takes two copies with the same bytes and time for one file)
echo "// ref" >> "$OUT/ref.light.h"
echo "// neu" >> "$OUT/neu.light.h"

CXX="${CXX:-c++}"
CC="${CC:-cc}"
INC=(-I"$ROOT" -I"$HERE" -I"$OUT" -I"$ROOT/third-party/fmt/include" -DFMT_HEADER_ONLY=1)
# a version that keeps the mesh colors on their own (ctr_gpu_mesh_split_colors)
split_of() { grep -q ctr_gpu_mesh_colors_inplace "$1" && echo 1 || echo 0; }
SPLIT=(-DREF_SPLIT="$(split_of "$OUT/ref.code.inc")" -DNEU_SPLIT="$(split_of "$OUT/neu.code.inc")")
# the level file reader with its decoders
if [ ! -x "$OUT/tod_extract" ] || [ "$HERE/tod_extract.cpp" -nt "$OUT/tod_extract" ]; then
  ZSTD=("$ROOT"/third-party/zstd/lib/common/*.c "$ROOT"/third-party/zstd/lib/decompress/*.c)
  for s in "$ROOT/third-party/lz4/lz4.c" "${ZSTD[@]}"; do
    "$CC" -O2 -DZSTD_DISABLE_ASM -c "$s" -o "$OUT/$(basename "${s%.c}").o"
  done
  "$CXX" -std=gnu++20 -O2 "${INC[@]}" "$HERE/tod_extract.cpp" "$ROOT/game/graphics/ctr/c3l_reader.cpp" \
    "$ROOT/common/util/Timer.cpp" "$OUT"/*.o -o "$OUT/tod_extract"
fi
PX=()
if [ "$PIXELS" = 1 ]; then
  PX=(-DTOD_PIXELS -DOPENGOAL_SMALL_MEMORY=1 -I"$ROOT/third-party" -I"$ROOT/third-party/SDL/include"
      "$ROOT/game/graphics/ctr/ctr_gpu_soft.cpp" "$HERE/tod_pixels.cpp")
fi
"$CXX" -std=gnu++20 -O2 "${INC[@]}" "${SPLIT[@]}" "$HERE/tod_diff.cpp" "${PX[@]}" -o "$OUT/tod_diff"

LEVEL="$OUT/$(basename "${C3L%.c3l}").tod"
"$OUT/tod_extract" "$C3L" "$LEVEL"
TRACES=()
if [ -n "$TRACE" ]; then
  TRACES=("$TRACE")
else
  TRACES=("$HERE/citadel_weights.txt")
  for seed in 1 2 3; do
    python3 "$HERE/tod_trace.py" $seed 400 > "$OUT/random$seed.txt"
    TRACES+=("$OUT/random$seed.txt")
  done
  python3 "$HERE/tod_trace.py" 4 400 lights 0x0e > "$OUT/random-lights.txt"
  TRACES+=("$OUT/random-lights.txt")
  python3 "$HERE/tod_trace.py" 5 900 lightning > "$OUT/lightning.txt"
  TRACES+=("$OUT/lightning.txt")
fi
status=0
for tr in "${TRACES[@]}"; do
  for step in 4 0; do
    for opts in "" "vis" "vis keep" "vis title-half" "vis title title-half"; do
      # shellcheck disable=SC2086
      "$OUT/tod_diff" "$LEVEL" "$tr" 100000 5 $step $opts || status=1
    done
  done
  # (the levels whose mood the trace is: update-mood-village2 / update-mood-swamp)
  if [ "$(basename "$tr")" = lightning.txt ] && [[ "$(basename "$C3L")" =~ ^(village2|swamp) ]]; then
    "$OUT/tod_diff" "$LEVEL" "$tr" 100000 5 0 exact || status=1
  fi
done

if [ "$ARM" = 1 ]; then
  DKA="${DEVKITARM:-/opt/devkitpro/devkitARM}"
  FLAGS=(-fno-threadsafe-statics -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -marm
         -D__3DS__ -fsigned-char -ffunction-sections -fdata-sections -mword-relocations
         -std=gnu++20 -fno-strict-aliasing -fno-math-errno -Wno-psabi -O2 -DSIM_ARM "${INC[@]}"
         "${SPLIT[@]}" -DTOD_ARENA_MB=12)
  "$DKA/bin/arm-none-eabi-g++" "${FLAGS[@]}" -c "$HERE/tod_diff.cpp" -o "$OUT/arm_diff.o"
  "$DKA/bin/arm-none-eabi-g++" "${FLAGS[@]}" -c "$HERE/tod_arm.cpp" -o "$OUT/arm_main.o"
  "$DKA/bin/arm-none-eabi-gcc" -march=armv6k -mfloat-abi=hard -c "$ROOT/platform/3ds/port/ctr_mem.S" \
    -o "$OUT/ctr_mem.o"
  "$DKA/bin/arm-none-eabi-gcc" -march=armv6k -mfloat-abi=hard -O2 -c \
    "$ROOT/platform/3ds/port/ctr_mem_c.c" -o "$OUT/ctr_mem_c.o"
  "$DKA/bin/arm-none-eabi-g++" "${FLAGS[@]}" -T "$ROOT/test/mips2c_native/unicorn/armbench.ld" \
    -nostartfiles -specs=nosys.specs -Wl,--gc-sections "$OUT/arm_main.o" "$OUT/arm_diff.o" \
    "$OUT/ctr_mem.o" "$OUT/ctr_mem_c.o" -o "$OUT/tod_arm.elf"
  PY="${ARMBENCH_PYTHON:-python3}"
  tr="${TRACES[0]}"
  for step in ${TOD_ARM_STEPS:-4 0}; do
    for who in ref neu; do
      echo "--- ARM, $who, pulse step $step, $(basename "$tr")"
      "$PY" "$HERE/tod_arm.py" "$OUT/tod_arm.elf" "$LEVEL" "$tr" "${TOD_ARM_FRAMES:-288}" $who 5 $step "${TOD_ARM_SKIP:-32}" \
        ${TOD_ARM_LINES:+lines}
    done
  done
fi
exit $status
