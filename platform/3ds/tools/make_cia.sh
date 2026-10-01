#!/usr/bin/env bash
# (AI-assisted)
# Package gk.elf as an installable CIA (New 3DS 124 MB memory mode). See docs/3ds-port/3ds_build.md.
#
#   platform/3ds/tools/make_cia.sh [--build-dir DIR] [--out FILE] [--boottest] [--mem 124|legacy]
#
# --boottest: package the small test app platform/3ds/hello (build it first: make -C
#   platform/3ds/hello) with the game's CIA settings, as its own title ("Jak1 boot test",
#   build-3ds/boottest.cia). It writes sdmc:/3ds/jak1/boottest.txt: whether the title started at
#   all, passed the HOME Menu handshake, reached main and got the 124 MB mode. For when the game's
#   CIA stays on the launch screen without writing boot_cia.txt.
# --mem legacy: without the New 3DS 124 MB memory mode (the game then says there isn't enough
#   memory, but it shows whether that mode is what keeps the title from starting).
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
BOOTTEST=0
MEM=124

while [ $# -gt 0 ]; do
  case "$1" in
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --boottest) BOOTTEST=1; shift ;;
    --mem) MEM="$2"; shift 2 ;;
    *) echo "unknown option $1" >&2; exit 1 ;;
  esac
done
case "$MEM" in 124|legacy) ;; *) echo "--mem: 124 or legacy" >&2; exit 1 ;; esac
if [ -z "$OUT" ]; then
  NAME=jak1
  [ "$BOOTTEST" = 1 ] && NAME=boottest
  [ "$MEM" = legacy ] && NAME="${NAME}_legacy"
  OUT="$BUILD_DIR/$NAME.cia"
fi

MAKEROM="$(command -v makerom || true)"
[ -n "$MAKEROM" ] || MAKEROM="$HOME/devkitpro-3ds/tools/bin/makerom"
[ -x "$MAKEROM" ] || { echo "makerom not found (see the header of this script)" >&2; exit 1; }

ELF="$BUILD_DIR/gk.elf"
SMDH="$BUILD_DIR/gk.smdh"
RSF="$ROOT/platform/3ds/cia/gk.rsf"
if [ "$BOOTTEST" = 1 ]; then
  ELF="$ROOT/platform/3ds/hello/opengoal-hello.elf"
  SMDH="$ROOT/platform/3ds/hello/opengoal-hello.smdh"
  RSF="$ROOT/platform/3ds/cia/boottest.rsf"
  [ -f "$ELF" ] || { echo "no $ELF (make -C platform/3ds/hello first)" >&2; exit 1; }
fi
[ -f "$ELF" ] || { echo "no $ELF (build gk first)" >&2; exit 1; }
[ -f "$SMDH" ] || { echo "no $SMDH" >&2; exit 1; }
mkdir -p "$BUILD_DIR"
if [ "$MEM" = legacy ]; then
  # the same settings without the 124 MB mode
  sed -e 's/^\(  SystemModeExt *: *\).*/\1Legacy/' "$RSF" > "$BUILD_DIR/cia_legacy.rsf"
  RSF="$BUILD_DIR/cia_legacy.rsf"
fi

# banner and icon
CIA_DIR="$ROOT/platform/3ds/cia"
BANNERTOOL="$(command -v bannertool || true)"
[ -n "$BANNERTOOL" ] || BANNERTOOL="$HOME/devkitpro-3ds/tools/bin/bannertool"
BANNER_ARGS=()
if [ -x "$BANNERTOOL" ] && [ "$BOOTTEST" = 0 ]; then
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
STRIPPED="$BUILD_DIR/$(basename "$OUT" .cia)_cia.elf"
"${DEVKITARM:-/opt/devkitpro/devkitARM}/bin/arm-none-eabi-strip" -o "$STRIPPED" "$ELF"

"$MAKEROM" -f cia -o "$OUT" -elf "$STRIPPED" -rsf "$RSF" \
  -icon "$SMDH" "${BANNER_ARGS[@]}" -exefslogo -target t -ver 0
echo "cia: $OUT"
