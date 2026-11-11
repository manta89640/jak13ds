#!/bin/sh
# (AI-assisted, cpubench) Builds the CPU kernels that have ARM-only code paths (VFP short vectors,
# ARMv6 SIMD) as ARMv6K hard-float code and runs their checks under qemu-arm's ARM11 MPCore.
# Needs arm-none-eabi-g++ (newlib, libstdc++) and qemu-arm (Ubuntu: gcc-arm-none-eabi,
# libnewlib-arm-none-eabi, libstdc++-arm-none-eabi-newlib, qemu-user). Times mean nothing here.
set -e
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
OUT=${OUT:-$(mktemp -d)}
CXX="arm-none-eabi-g++ -march=armv6k -mtune=mpcore -mfloat-abi=hard -mfpu=vfp -marm -O2 -std=c++17"
$CXX -I"$ROOT" --specs=rdimon.specs -o "$OUT/bench.elf" \
  "$ROOT/test/ctr_render/arm/bench_main.cpp" "$ROOT/game/graphics/ctr/CtrCpuBench.cpp"
qemu-arm -cpu arm11mpcore "$OUT/bench.elf" 03000000
qemu-arm -cpu arm11mpcore "$OUT/bench.elf" 03C00000
