#!/usr/bin/env bash
# (AI-assisted)
# Builds and runs the differential tests of the native mips2c functions (see harness.h).
#
#   test/mips2c_native/run.sh [config...] [-- test args]
#
# configs (default: host arm):
#   host      g++ -O2, x86-64 (or the host's architecture)
#   fma       clang++ -O2 -mfma -ffp-contract=on: fuses a + b * c inside expressions like clang on
#             arm64 does, so natives that group float operations differently from mips2c fail
#   arm       ARMv6K + VFPv2 hard float, -D__3DS__ like the 3DS build (arm-linux-gnueabihf-g++,
#             run with qemu-arm)
#   coverage  g++ -O0 --coverage, then prints the mips2c line coverage of every tested function
#   bench     the arm build, then ARM instructions per call of the mips2c and the native version
#             (test args: [--scale X] test names), counted with qemu-arm
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"
OUT="${MIPS2C_TEST_BUILD:-$ROOT/build-mips2c-test}"

configs=()
test_args=()
while [[ $# -gt 0 ]]; do
  if [[ "$1" == "--" ]]; then
    shift
    test_args=("$@")
    break
  fi
  configs+=("$1")
  shift
done
[[ ${#configs[@]} -eq 0 ]] && configs=(host arm)

M2C="$ROOT/game/mips2c/jak1_functions"
MIPS2C_SRCS=(collide_func collide_cache collide_probe collide_mesh collide_edge_grab joint sparticle
             sparticle_launcher ocean_vu0)
HARNESS_SRCS=("$HERE"/*.cpp)
NATIVE_SRCS=("$M2C"/native_*.cpp)

COMMON_FLAGS=(-std=gnu++20 -I"$ROOT" -I"$ROOT/third-party/fmt/include" -DFMT_HEADER_ONLY=1
              -fsigned-char -Wall -Wno-unused-variable -Wno-unused-but-set-variable
              -Wno-unused-label -Wno-maybe-uninitialized -Wno-unused-function)

build() {
  local cfg="$1" cxx="$2"
  shift 2
  local flags=("$@")
  local dir="$OUT/$cfg"
  mkdir -p "$dir"
  local objs=()
  local pids=()
  for src in "${HARNESS_SRCS[@]}" "${NATIVE_SRCS[@]}"; do
    local obj="$dir/$(basename "${src%.cpp}").o"
    objs+=("$obj")
    if [[ ! -f "$obj" || "$src" -nt "$obj" || "$HERE/harness.h" -nt "$obj" || "$HERE/fakes.h" -nt "$obj" ||
          "$ROOT/game/mips2c/mips2c_native.h" -nt "$obj" || "$M2C/native_functions.h" -nt "$obj" ]]; then
      "$cxx" "${COMMON_FLAGS[@]}" "${flags[@]}" -c "$src" -o "$obj" &
      pids+=($!)
    fi
  done
  for name in "${MIPS2C_SRCS[@]}"; do
    local src="$M2C/$name.cpp"
    local obj="$dir/m2c_$name.o"
    objs+=("$obj")
    if [[ ! -f "$obj" || "$src" -nt "$obj" || "$M2C/native_functions.h" -nt "$obj" ||
          "$ROOT/game/mips2c/mips2c_private.h" -nt "$obj" ]]; then
      "$cxx" "${COMMON_FLAGS[@]}" "${flags[@]}" ${M2C_EXTRA_FLAGS:-} -c "$src" -o "$obj" &
      pids+=($!)
    fi
  done
  local fail=0
  for p in "${pids[@]}"; do
    wait "$p" || fail=1
  done
  [[ $fail -eq 0 ]] || { echo "build $cfg failed"; exit 1; }
  "$cxx" "${flags[@]}" "${objs[@]}" -o "$dir/mips2c-native-test" ${LINK_EXTRA:-}
}

status=0
for cfg in "${configs[@]}"; do
  echo "=== $cfg"
  case "$cfg" in
    host)
      build host g++ -O2 -g
      "$OUT/host/mips2c-native-test" "${test_args[@]}" || status=1
      ;;
    fma)
      build fma clang++ -O2 -g -mfma -ffp-contract=on
      "$OUT/fma/mips2c-native-test" "${test_args[@]}" || status=1
      ;;
    arm)
      LINK_EXTRA="-static" build arm arm-linux-gnueabihf-g++ -O2 -g -marm -march=armv6k \
        -mfpu=vfp -mfloat-abi=hard -D__3DS__
      qemu-arm "$OUT/arm/mips2c-native-test" "${test_args[@]}" || status=1
      ;;
    bench)
      # ARM instructions per call, mips2c vs native (test args: [--scale X] test names)
      LINK_EXTRA="-static -Wl,-Map=$OUT/bench/link.map" build bench arm-linux-gnueabihf-g++ -O2 -g \
        -marm -march=armv6k -mfpu=vfp -mfloat-abi=hard -D__3DS__
      python3 "$HERE/bench.py" "$OUT/bench/mips2c-native-test" "$OUT/bench/link.map" "${test_args[@]}"
      ;;
    coverage)
      rm -f "$OUT/coverage"/*.gcda
      M2C_EXTRA_FLAGS="--coverage" LINK_EXTRA="--coverage" build coverage g++ -O0 -g
      "$OUT/coverage/mips2c-native-test" "${test_args[@]}" || status=1
      python3 "$HERE/coverage_report.py" "$OUT/coverage" "$M2C" ${COVERAGE_LINES:+--lines}
      ;;
    *)
      echo "unknown config $cfg"
      exit 2
      ;;
  esac
done
exit $status
