#!/usr/bin/env bash
# (AI-assisted)
# The ARM half of the differential tests where qemu-arm and the arm-linux toolchain are missing
# (macOS): the mips2c and native versions built with devkitARM and the 3DS build's flags (-O2,
# the natives -O3), run under Unicorn (ARM1176 = ARMv6K + VFPv2, FPSCR 0x03000000 like the 3DS) on
# cases the harness wrote with --dump. Compares v0 and GOAL memory (not the stack), and counts the
# ARM instructions of each version (like run.sh bench: the functions' own code; also with the
# 3DS's memcpy / memset, platform/3ds/port/ctr_mem.S, which both versions' copies call).
#
#   test/mips2c_native/unicorn/run.sh [--scale X] [--seed N] [--only TEST]
#
# Needs devkitARM (DEVKITARM, default /opt/devkitpro/devkitARM), python3 with unicorn
# (ARMBENCH_PYTHON: a python3 that has it) and the host build of the tests (run.sh host), which
# writes the cases. The tests whose GOAL callees armbench.cpp fakes or links: shadow-execute,
# moving-sphere-triangle-intersect (also on the game's inputs) and collide-do-primitives (their
# GOAL callees, ray-sphere-intersect and ray-cylinder-intersect, are native code on both sides, as
# in the game).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
OUT="${MIPS2C_TEST_BUILD:-$ROOT/build-mips2c-test}"
DKA="${DEVKITARM:-/opt/devkitpro/devkitARM}"
PY="${ARMBENCH_PYTHON:-python3}"
CXX="$DKA/bin/arm-none-eabi-g++"

scale=0.05
seed=1
only=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --scale) scale="$2"; shift 2 ;;
    --seed) seed="$2"; shift 2 ;;
    --only) only="$2"; shift 2 ;;  # (AI-assisted) just that test
    *) echo "usage: $0 [--scale X] [--seed N] [--only TEST]"; exit 2 ;;
  esac
done

dir="$OUT/unicorn"
mkdir -p "$dir"
# the 3DS build's flags (platform/3ds), fmt header-only; no thread-safe statics (their slow path
# needs threads, the checks after the first call are the same)
FLAGS=(-fno-threadsafe-statics
       -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -marm -D__3DS__ -DOPENGOAL_3DS=1
       -DOPENGOAL_SCALAR_SIMD=1 -DOPENGOAL_SMALL_MEMORY=1 -DFMT_HEADER_ONLY=1 -fsigned-char
       -ffunction-sections -fdata-sections -mword-relocations -std=gnu++20 -fno-strict-aliasing
       -fno-math-errno -Wno-psabi -I"$ROOT" -I"$ROOT/third-party" -I"$ROOT/third-party/fmt/include")
M2C="$ROOT/game/mips2c/jak1_functions"
"$CXX" "${FLAGS[@]}" -O2 -c "$M2C/shadow.cpp" -o "$dir/m2c_shadow.o" &
"$CXX" "${FLAGS[@]}" -O3 -c "$M2C/native_shadow.cpp" -o "$dir/native_shadow.o" &
"$CXX" "${FLAGS[@]}" -O2 -c "$M2C/collide_func.cpp" -o "$dir/m2c_collide_func.o" &
"$CXX" "${FLAGS[@]}" -O3 -c "$M2C/native_collide_func.cpp" -o "$dir/native_collide_func.o" &
"$CXX" "${FLAGS[@]}" -O3 -c "$M2C/native_bones.cpp" -o "$dir/native_bones.o" &
# (AI-assisted) goalc's C of new-bones-mtx-calc-asm, with the 3DS build's flags for the game's C
# modules (platform/3ds/cmake/cmodules.cmake): the reference of pc-new-bones-mtx-calc
"$DKA/bin/arm-none-eabi-gcc" -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -marm -O2 \
  -D__3DS__ -fsigned-char -ffunction-sections -fdata-sections -mword-relocations \
  -fno-strict-aliasing -fno-math-errno -w -I"$ROOT/goalc/cbackend" \
  -I"$ROOT/game/kernel/common" -I"$HERE/../goalc_ref" -c "$HERE/../goalc_ref/bones.c" \
  -o "$dir/m2c_ref_bones.o" &
"$CXX" "${FLAGS[@]}" -O2 -c "$HERE/armbench.cpp" -o "$dir/armbench.o" &
# the 3DS's memcpy / memset (platform/3ds/port/ctr_mem.S), not newlib's
"$DKA/bin/arm-none-eabi-gcc" -march=armv6k -mfloat-abi=hard -c "$ROOT/platform/3ds/port/ctr_mem.S" \
  -o "$dir/ctr_mem.o" &
"$DKA/bin/arm-none-eabi-gcc" -march=armv6k -mfloat-abi=hard -O2 -c \
  "$ROOT/platform/3ds/port/ctr_mem_c.c" -o "$dir/ctr_mem_c.o" &
wait
"$CXX" "${FLAGS[@]}" -T "$HERE/armbench.ld" -nostartfiles -specs=nosys.specs \
  -Wl,-Map="$dir/armbench.map" -Wl,--gc-sections "$dir/armbench.o" "$dir/m2c_shadow.o" \
  "$dir/native_shadow.o" "$dir/m2c_collide_func.o" "$dir/native_collide_func.o" \
  "$dir/native_bones.o" "$dir/m2c_ref_bones.o" "$dir/ctr_mem.o" \
  "$dir/ctr_mem_c.o" -o "$dir/armbench.elf"

# the cases of one test (exactly that one, not its variants): the harness's generator in the 3DS's
# VFP mode (default NaN: the garbage cases too); then both versions on ARM
status=0
run_test() {
  local test="$1" fn="$2" sc="$3"
  [[ -n "$only" && "$test" != "$only" ]] && return 0
  local cases="$dir/cases/${test//[^A-Za-z0-9]/_}"
  rm -rf "$cases"
  mkdir -p "$cases"
  "$OUT/host/mips2c-native-test" --filter "=$test" --scale "$sc" --seed "$seed" \
    --fpscr 0x03000000 --dump "$cases" > /dev/null || true
  echo "--- $test"
  "$PY" "$HERE/armbench.py" "$dir/armbench.elf" "$dir/armbench.map" "$cases" "$fn" ${4:-} || status=1
}
run_test shadow-execute shadow-execute "$scale"
run_test moving-sphere-triangle-intersect moving-sphere-triangle-intersect "$scale"
run_test "moving-sphere-triangle-intersect (game inputs)" moving-sphere-triangle-intersect 1
run_test collide-do-primitives collide-do-primitives "$scale"
# (AI-assisted) v0 not compared: the native returns #t (it ran), the GOAL function nothing
run_test pc-new-bones-mtx-calc pc-new-bones-mtx-calc "$scale" --no-v0
exit $status
