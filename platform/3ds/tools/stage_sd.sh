#!/usr/bin/env bash
# (AI-assisted)
# Build the OpenGOAL 3DS SD card layout (docs/3ds-port/3ds_build.md) in a directory:
#
#   platform/3ds/tools/stage_sd.sh --proj <project dir> [--sd <sdmc root>] [--gk <gk.3dsx>]
#                                  [--args "<game args>"] [--listener] [--clean-logs]
#
#   --proj   project the game was built in (uses <proj>/out/jak1/iso). Must be the same build as
#            the C modules linked into gk.3dsx (platform/3ds/tools/build_cmodules.sh).
#   --sd     root of the SD card (default: Azahar's virtual SD card, read from its qt-config.ini,
#            normally ~/Library/Application Support/Azahar/sdmc). For a real card: its mount point.
#   --gk     gk.3dsx to copy (default: build-3ds/gk.3dsx)
#   --args   write args.txt (default: remove it, gk then uses "-boot -cbackend")
#   --listener  create the "listener" flag file (Wi-Fi REPL)
#   --clean-logs  delete data/log before the run
#
# Files are copied with APFS clones (cp -c) when possible, so staging 1.3 GB is instant on macOS.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
PROJ=""
SD=""
GK="$ROOT/build-3ds/gk.3dsx"
ARGS=""
HAVE_ARGS=0
LISTENER=0
CLEAN_LOGS=0

while [ $# -gt 0 ]; do
  case "$1" in
    --proj) PROJ="$2"; shift 2 ;;
    --sd) SD="$2"; shift 2 ;;
    --gk) GK="$2"; shift 2 ;;
    --args) ARGS="$2"; HAVE_ARGS=1; shift 2 ;;
    --listener) LISTENER=1; shift ;;
    --clean-logs) CLEAN_LOGS=1; shift ;;
    *) echo "unknown option $1" >&2; exit 1 ;;
  esac
done

[ -n "$PROJ" ] || { echo "--proj is required" >&2; exit 1; }
ISO="$PROJ/out/jak1/iso"
[ -d "$ISO" ] || { echo "no $ISO (build the game first)" >&2; exit 1; }
[ -f "$GK" ] || { echo "no $GK" >&2; exit 1; }

azahar_sdmc() {
  local cfg="$HOME/Library/Application Support/Azahar/config/qt-config.ini"
  local dir=""
  if [ -f "$cfg" ]; then
    dir="$(sed -n 's/^sdmc_directory=//p' "$cfg" | head -1)"
  fi
  [ -n "$dir" ] || dir="$HOME/Library/Application Support/Azahar/sdmc/"
  echo "${dir%/}"
}
[ -n "$SD" ] || SD="$(azahar_sdmc)"

BASE="$SD/3ds/jak1"
DATA="$BASE/data"
mkdir -p "$DATA/out/jak1"

copy() {  # src dst: APFS clone if possible
  cp -c "$1" "$2" 2>/dev/null || cp "$1" "$2"
}

# GOAL objects and game files: replace the whole folder so stale files don't survive
rm -rf "$DATA/out/jak1/iso"
mkdir -p "$DATA/out/jak1/iso"
n=0
for f in "$ISO"/*; do
  copy "$f" "$DATA/out/jak1/iso/"
  n=$((n + 1))
done

copy "$GK" "$BASE/gk.3dsx"

if [ "$HAVE_ARGS" = 1 ]; then
  echo "$ARGS" > "$BASE/args.txt"
else
  rm -f "$BASE/args.txt"
fi
if [ "$LISTENER" = 1 ]; then touch "$BASE/listener"; else rm -f "$BASE/listener"; fi
if [ "$CLEAN_LOGS" = 1 ]; then rm -rf "$DATA/log"; fi

echo "staged $n files + gk.3dsx in $BASE"
