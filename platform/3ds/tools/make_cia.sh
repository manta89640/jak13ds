#!/usr/bin/env bash
# (AI-assisted)
# Package gk.elf as an installable CIA (New 3DS 124 MB memory mode). See docs/3ds-port/3ds_build.md.
#
#   platform/3ds/tools/make_cia.sh [--build-dir DIR] [--out FILE]
#
# Needs makerom in PATH or in ~/devkitpro-3ds/tools/bin (https://github.com/3DSGuy/Project_CTR
# releases). The banner (platform/3ds/cia/banner.png + banner.wav, the title screen) and the icon
# (cia/icon.png) are made with bannertool, from PATH or ~/devkitpro-3ds/tools/bin. There is no
# macOS release: build it from https://github.com/diasurgical/bannertool with
#   clang -c source/pc/stb_image.c source/pc/stb_vorbis.c
#   clang++ -std=c++14 -DVERSION_MAJOR=1 -DVERSION_MINOR=2 -DVERSION_MICRO=0 -Isource \
#     source/*.cpp source/pc/wav.cpp source/3ds/*.cpp stb_image.o stb_vorbis.o -o bannertool
# Without bannertool the CIA has no banner and the icon of gk.smdh.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
BUILD_DIR="$ROOT/build-3ds"
OUT=""

while [ $# -gt 0 ]; do
  case "$1" in
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    *) echo "unknown option $1" >&2; exit 1 ;;
  esac
done
[ -n "$OUT" ] || OUT="$BUILD_DIR/jak1.cia"

MAKEROM="$(command -v makerom || true)"
[ -n "$MAKEROM" ] || MAKEROM="$HOME/devkitpro-3ds/tools/bin/makerom"
[ -x "$MAKEROM" ] || { echo "makerom not found (see the header of this script)" >&2; exit 1; }

ELF="$BUILD_DIR/gk.elf"
SMDH="$BUILD_DIR/gk.smdh"
[ -f "$ELF" ] || { echo "no $ELF (build gk first)" >&2; exit 1; }
[ -f "$SMDH" ] || { echo "no $SMDH" >&2; exit 1; }

# banner and icon
CIA_DIR="$ROOT/platform/3ds/cia"
BANNERTOOL="$(command -v bannertool || true)"
[ -n "$BANNERTOOL" ] || BANNERTOOL="$HOME/devkitpro-3ds/tools/bin/bannertool"
BANNER_ARGS=()
if [ -x "$BANNERTOOL" ]; then
  "$BANNERTOOL" makebanner -i "$CIA_DIR/banner.png" -a "$CIA_DIR/banner.wav" \
    -o "$BUILD_DIR/banner.bnr" > /dev/null
  "$BANNERTOOL" makesmdh -s "Jak and Daxter" -l "The Precursor Legacy (OpenGOAL 3DS port)" \
    -p "OpenGOAL" -i "$CIA_DIR/icon.png" -o "$BUILD_DIR/gk_cia.smdh" > /dev/null
  SMDH="$BUILD_DIR/gk_cia.smdh"
  BANNER_ARGS=(-banner "$BUILD_DIR/banner.bnr")
else
  echo "bannertool not found: no banner (see the header of this script)" >&2
fi

# makerom wants a stripped elf
STRIPPED="$BUILD_DIR/gk_cia.elf"
"${DEVKITARM:-/opt/devkitpro/devkitARM}/bin/arm-none-eabi-strip" -o "$STRIPPED" "$ELF"

"$MAKEROM" -f cia -o "$OUT" -elf "$STRIPPED" -rsf "$ROOT/platform/3ds/cia/gk.rsf" \
  -icon "$SMDH" "${BANNER_ARGS[@]}" -exefslogo -target t -ver 0
echo "cia: $OUT"
