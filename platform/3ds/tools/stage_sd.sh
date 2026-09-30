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
#   --c3l    folder with the level backgrounds (default: <proj>/out/jak1/c3l if it exists;
#            make them with: ctr_level_converter --all out/jak1/fr3 <dir>)
#   --args   write args.txt (default: remove it, gk then uses "-boot -cbackend")
#   --listener  create the "listener" flag file (Wi-Fi REPL)
#   --clean-logs  delete data/log before the run
#   --clean-user  delete user/ (settings, saves) before the run
#   --screenshots N  gk saves a screenshot every N frames to data/log/shot_<frame>.bmp
#   --pad-script FILE  scripted controller input (game/sce/pad_script.h), for automated tests
#   --use-syscore  run the IOP / listener / worker threads on core 1 (experimental)
#   --perf-sections  per-section frame timing in the log (the perf_sections flag file)
#   --render-ini FILE  renderer settings for this run (copied to data/render.ini, which takes
#            precedence over sdmc:/3ds/jak1/render.ini; removed when the option is not given)
#
# Files are copied with APFS clones (cp -c) when possible, so staging 1.3 GB is instant on macOS.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
PROJ=""
SD=""
GK="$ROOT/build-3ds/gk.3dsx"
C3L=""
ARGS=""
HAVE_ARGS=0
LISTENER=0
CLEAN_LOGS=0
CLEAN_USER=0
SHOTS=0
PAD_SCRIPT=""
USE_SYSCORE=0
PERF_SECTIONS=0
RENDER_INI=""

while [ $# -gt 0 ]; do
  case "$1" in
    --proj) PROJ="$2"; shift 2 ;;
    --sd) SD="$2"; shift 2 ;;
    --gk) GK="$2"; shift 2 ;;
    --c3l) C3L="$2"; shift 2 ;;
    --args) ARGS="$2"; HAVE_ARGS=1; shift 2 ;;
    --listener) LISTENER=1; shift ;;
    --clean-logs) CLEAN_LOGS=1; shift ;;
    --clean-user) CLEAN_USER=1; shift ;;
    --screenshots) SHOTS="$2"; shift 2 ;;
    --pad-script) PAD_SCRIPT="$2"; shift 2 ;;
    --use-syscore) USE_SYSCORE=1; shift ;;
    --perf-sections) PERF_SECTIONS=1; shift ;;
    --render-ini) RENDER_INI="$2"; shift 2 ;;
    *) echo "unknown option $1" >&2; exit 1 ;;
  esac
done

[ -n "$PROJ" ] || { echo "--proj is required" >&2; exit 1; }
"$(dirname "$0")/check_fresh.sh" proj "$PROJ" || exit 1
"$(dirname "$0")/check_fresh.sh" gk "$GK" || exit 1

# don't replace the files of an emulator run in progress (run_emu.sh holds this lock; it calls us
# itself with --stage)
EMU_LOCK="${TMPDIR:-/tmp}/opengoal-azahar.lock"
while [ -d "$EMU_LOCK" ]; do
  owner="$(cat "$EMU_LOCK/pid" 2>/dev/null || true)"
  if [ -z "$owner" ] || [ "$owner" = "$PPID" ] || ! kill -0 "$owner" 2>/dev/null; then
    break
  fi
  echo "waiting for the emulator run of pid $owner to finish"
  sleep 10
done
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

# level backgrounds for the 3DS renderer
[ -n "$C3L" ] || C3L="$PROJ/out/jak1/c3l"
if [ -d "$C3L" ]; then
  rm -rf "$DATA/out/jak1/c3l"
  mkdir -p "$DATA/out/jak1/c3l"
  for f in "$C3L"/*.c3l; do
    copy "$f" "$DATA/out/jak1/c3l/"
  done
  echo "staged level backgrounds from $C3L"
fi

if [ "$HAVE_ARGS" = 1 ]; then
  echo "$ARGS" > "$BASE/args.txt"
else
  rm -f "$BASE/args.txt"
fi
if [ "$LISTENER" = 1 ]; then touch "$BASE/listener"; else rm -f "$BASE/listener"; fi
if [ "$CLEAN_LOGS" = 1 ]; then rm -rf "$DATA/log"; fi
if [ "$CLEAN_USER" = 1 ]; then rm -rf "$BASE/user"; fi
if [ "$USE_SYSCORE" = 1 ]; then touch "$BASE/use_syscore"; else rm -f "$BASE/use_syscore"; fi
if [ "$PERF_SECTIONS" = 1 ]; then touch "$BASE/perf_sections"; else rm -f "$BASE/perf_sections"; fi
rm -f "$BASE/single_core"
if [ -n "$PAD_SCRIPT" ]; then cp "$PAD_SCRIPT" "$BASE/pad_script.txt"; else rm -f "$BASE/pad_script.txt"; fi
if [ -n "$RENDER_INI" ]; then cp "$RENDER_INI" "$DATA/render.ini"; else rm -f "$DATA/render.ini"; fi
if [ "$SHOTS" != 0 ]; then echo "$SHOTS" > "$BASE/screenshots"; else rm -f "$BASE/screenshots"; fi

echo "staged $n files + gk.3dsx in $BASE"
