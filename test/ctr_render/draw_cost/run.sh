#!/usr/bin/env bash
# (AI-assisted) The level draws' backend cost on ARM, and its command words against a base revision.
#   test/ctr_render/draw_cost/run.sh [--base REV] [--out DIR]
# Builds harness.c twice with devkitARM -O2 (the 3DS flags, ctr_mem.S's memcpy), once around this
# tree's game/graphics/ctr/ctr_gpu_citro3d.c and once around REV's (default HEAD), runs both under
# Unicorn (ARMBENCH_PYTHON: a python3 with unicorn) and prints the ARM instructions per call. Modes
# 0 and 1 must write the same command words (exit 1 if not); mode 2 (wind draws) only prints.
# Needs build-3ds (ctr_mesh_wind_shbin.c) and DEVKITARM / DKP3DS (platform/3ds/toolchain/env.sh).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
BASE=HEAD
OUT="$ROOT/build-draw-cost"
while [ $# -gt 0 ]; do
  case "$1" in
    --base) BASE="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    *) echo "unknown option $1" >&2; exit 1 ;;
  esac
done
source "$ROOT/platform/3ds/toolchain/env.sh" > /dev/null
PY="${ARMBENCH_PYTHON:-python3}"
mkdir -p "$OUT"
git -C "$ROOT" show "$BASE:game/graphics/ctr/ctr_gpu_citro3d.c" > "$OUT/base_gpu.c"
CC="$DEVKITARM/bin/arm-none-eabi-gcc"
ARCH=(-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft)
"$CC" "${ARCH[@]}" -c "$ROOT/platform/3ds/port/ctr_mem.S" -o "$OUT/ctr_mem.o"
"$CC" "${ARCH[@]}" -O2 -c "$ROOT/platform/3ds/port/ctr_mem_c.c" -o "$OUT/ctr_mem_c.o"
"$CC" "${ARCH[@]}" -c "$ROOT/build-3ds/ctr_mesh_wind_shbin.c" -o "$OUT/wind_shbin.o"
build() {
  "$CC" -DOPENGOAL_3DS=1 -DOPENGOAL_SMALL_MEMORY=1 -D__3DS__ -I"$ROOT" -isystem "$CTRULIB/include" \
    "${ARCH[@]}" -fsigned-char -ffunction-sections -fdata-sections -mword-relocations -O2 \
    -Wno-psabi -fno-strict-aliasing -fno-math-errno -DGPU_C="\"$1\"" -c "$HERE/harness.c" \
    -o "$OUT/$2.o"
  "$CC" "${ARCH[@]}" -T "$ROOT/test/mips2c_native/unicorn/armbench.ld" -nostartfiles \
    -specs=nosys.specs -Wl,--gc-sections -Wl,--unresolved-symbols=ignore-all "$OUT/$2.o" \
    "$OUT/ctr_mem.o" "$OUT/ctr_mem_c.o" "$OUT/wind_shbin.o" -L"$CTRULIB/lib" -lcitro3d -lctru -lm \
    -o "$OUT/$2.elf"
}
build "$OUT/base_gpu.c" base
build "$ROOT/game/graphics/ctr/ctr_gpu_citro3d.c" tree
status=0
for mode in 0 1; do
  for seed in 7 3 11 29; do
    for who in base tree; do
      "$PY" "$HERE/run.py" "$OUT/$who.elf" $mode 3000 $seed 60 "$OUT/$who.bin" > "$OUT/$who.txt"
    done
    if ! cmp -s "$OUT/base.bin" "$OUT/tree.bin"; then
      echo "mode $mode seed $seed: the command words differ"
      status=1
    fi
    [ $seed = 7 ] && cat "$OUT/base.txt" "$OUT/tree.txt"
  done
done
for sorted in 0 1; do
  for who in base tree; do
    echo -n "(wind, a chunk's draws together: $sorted) "
    SORTED=$sorted "$PY" "$HERE/run.py" "$OUT/$who.elf" 2 800 7 40
  done
done
[ $status = 0 ] && echo "modes 0 and 1: the same command words"
exit $status
