#!/usr/bin/env bash
# (AI-assisted)
# Rebuild the whole Jak 1 game with the C backend for the 3DS and relink gk.3dsx.
#
#   platform/3ds/tools/build_cmodules.sh [options]
#     --mirror DIR     project mirror to build in (default: ../p3ds next to the repo). Created with
#                      symlinks to this repo, except out/, so the PC out/jak1 is not touched.
#     --goalc BIN      goalc binary (default: build-plat-native/goalc/goalc, then build/goalc/goalc)
#     --build-dir DIR  3DS CMake build dir (default: build-3ds)
#     --big-memory     build the GOAL code for the 128 MB layout (default: small memory, matching
#                      OPENGOAL_SMALL_MEMORY in the 3DS runtime)
#     --no-goal        skip goalc, only relink
#
# Afterwards: platform/3ds/tools/stage_sd.sh --proj <mirror> (SD card files)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
MIRROR="$(cd "$ROOT/.." && pwd)/p3ds"
GOALC=""
BUILD_DIR="$ROOT/build-3ds"
SMALL=1
RUN_GOAL=1

while [ $# -gt 0 ]; do
  case "$1" in
    --mirror) MIRROR="$2"; shift 2 ;;
    --goalc) GOALC="$2"; shift 2 ;;
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    --big-memory) SMALL=0; shift ;;
    --no-goal) RUN_GOAL=0; shift ;;
    *) echo "unknown option $1" >&2; exit 1 ;;
  esac
done

if [ -z "$GOALC" ]; then
  for c in "$ROOT/build-plat-native/goalc/goalc" "$ROOT/build/goalc/goalc"; do
    if [ -x "$c" ]; then GOALC="$c"; break; fi
  done
fi

# 1. mirror: everything symlinked to the repo except out/ (and build dirs / .git)
if [ ! -d "$MIRROR" ]; then
  echo "creating mirror $MIRROR"
  mkdir -p "$MIRROR/out"
  for e in "$ROOT"/* "$ROOT"/.[!.]*; do
    name="$(basename "$e")"
    case "$name" in
      out|.git|build|build-*) continue ;;
    esac
    ln -s "$e" "$MIRROR/$name"
  done
fi

# goalc expects these output folders to exist
mkdir -p "$MIRROR/out/jak1/fr3" "$MIRROR/out/jak1/iso" "$MIRROR/out/jak1/obj"

# 2. compile the game to C
if [ "$RUN_GOAL" = 1 ]; then
  [ -x "$GOALC" ] || { echo "goalc not found, pass --goalc" >&2; exit 1; }
  echo "goalc: $GOALC (OPENGOAL_SMALL_MEMORY=$SMALL)"
  OPENGOAL_SMALL_MEMORY=$SMALL "$GOALC" --game jak1 --proj-path "$MIRROR" --instruction-set c \
    -c '(mi)'
fi
CSRC="$MIRROR/out/jak1/csrc"
[ -d "$CSRC" ] || { echo "no $CSRC" >&2; exit 1; }

# 3. relink gk.3dsx with those modules
source "$ROOT/platform/3ds/toolchain/env.sh"
if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
  cmake -S "$ROOT/platform/3ds" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$ROOT/platform/3ds/cmake/Toolchain-3DS.cmake"
fi
cmake "$BUILD_DIR" -DOG3DS_CSRC_DIR="$CSRC" -DOG3DS_SMALL_MEMORY="$SMALL" > /dev/null
cmake --build "$BUILD_DIR"
echo "gk: $BUILD_DIR/gk.3dsx (GOAL objects: $MIRROR/out/jak1/iso)"
