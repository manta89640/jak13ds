#!/usr/bin/env bash
# (AI-assisted)
# Package gk.elf as an installable CIA (New 3DS 124 MB memory mode). See docs/3ds-port/3ds_build.md.
#
#   platform/3ds/tools/make_cia.sh [--build-dir DIR] [--out FILE]
#
# Needs makerom in PATH or in ~/devkitpro-3ds/tools/bin (https://github.com/3DSGuy/Project_CTR
# releases). The CIA has no banner (the HOME Menu shows the icon only), because bannertool has no
# macOS build.
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

# makerom wants a stripped elf
STRIPPED="$BUILD_DIR/gk_cia.elf"
"${DEVKITARM:-/opt/devkitpro/devkitARM}/bin/arm-none-eabi-strip" -o "$STRIPPED" "$ELF"

"$MAKEROM" -f cia -o "$OUT" -elf "$STRIPPED" -rsf "$ROOT/platform/3ds/cia/gk.rsf" \
  -icon "$SMDH" -exefslogo -target t -ver 0
echo "cia: $OUT"
