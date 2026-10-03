#!/usr/bin/env bash
# (AI-assisted) Builds memtest.3dsx: ctr_mem.S + the test, with newlib's memcpy/memmove/memset
# linked in as newlib_* for the timing comparison.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
source "$HERE/../../toolchain/env.sh"
B="$HERE/build"; mkdir -p "$B"
ARCH="-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft"
LIBC="$DEVKITARM/arm-none-eabi/lib/armv6k/fpu/libc.a"
cd "$B"
arm-none-eabi-ar x "$LIBC" libc_a-memcpy-stub.o libc_a-memmove.o libc_a-memset.o
arm-none-eabi-objcopy --redefine-sym memcpy=newlib_memcpy libc_a-memcpy-stub.o newlib_memcpy.o
arm-none-eabi-objcopy --redefine-sym memmove=newlib_memmove libc_a-memmove.o newlib_memmove.o
arm-none-eabi-objcopy --redefine-sym memset=newlib_memset libc_a-memset.o newlib_memset.o
arm-none-eabi-gcc $ARCH -c "$HERE/../../port/ctr_mem.S" -o ctr_mem.o
arm-none-eabi-gcc $ARCH -O2 -Wall -fno-builtin -fno-tree-loop-distribute-patterns -D__3DS__ \
  -I"$CTRULIB/include" -c "$HERE/main.c" -o main.o
arm-none-eabi-gcc -specs=3dsx.specs $ARCH -Wl,-Map,memtest.map main.o ctr_mem.o newlib_memcpy.o \
  newlib_memmove.o newlib_memset.o -L"$CTRULIB/lib" -lctru -lm -o memtest.elf
3dsxtool memtest.elf memtest.3dsx
echo "$B/memtest.3dsx"
