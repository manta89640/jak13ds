#!/usr/bin/env bash
# (AI-assisted)
# Build the OpenGOAL 3DS SD card layout (docs/3ds-port/3ds_build.md) in a directory:
#
#   platform/3ds/tools/stage_sd.sh --proj <project dir> [--sd <sdmc root>] [--gk <gk.3dsx>]
#                                  [--args "<game args>"] [--listener] [--clean-logs]
#
#   --game   (AI-assisted) jak1 (default) or jak2: the Jak II build (sdmc:/3ds/jak2/, <proj>/out/jak2/iso,
#            gk from build-3ds-jak2, platform/3ds/tools/config_template_jak2.ini, no level backgrounds)
#   --proj   project the game was built in (uses <proj>/out/<game>/iso). Must be the same build as
#            the C modules linked into gk.3dsx (platform/3ds/tools/build_cmodules.sh).
#   --sd     root of the SD card (default: Azahar's virtual SD card, read from its qt-config.ini,
#            normally ~/Library/Application Support/Azahar/sdmc). For a real card: its mount point.
#   --gk     gk.3dsx to copy (default: build-3ds/gk.3dsx)
#   --c3l    folder with the level backgrounds (default: <proj>/out/jak1/c3l if it exists;
#            make them with: ctr_level_converter --all out/jak1/fr3 <dir>)
#   --c3v    (AI-assisted) folder with the levels' fine visibility sidecars (<level>.c3v, made by
#            ctr_vis_builder; default: the .c3v files in the --c3l folder)
# All settings go in one file, sdmc:/3ds/jak1/config.ini (docs/3ds-port/config_ini.md). It is
# written from platform/3ds/tools/config_template.ini (the 8 user keys); the options below set keys
# in it (the testing keys of docs/3ds-port/config_dev.md go at its end). Old flag files are removed.
#
#   --preset P  preset = P: normal (default) | safe | bench | breakdown
#   --keep-config  start from the card's config.ini instead of the template (the old behaviour)
#   --config FILE  the config.ini to start from (--render-ini FILE: the same)
#   --args   args = ... (gk's default: "-boot -cbackend")
#   --listener  listener = on (Wi-Fi REPL)
#   --clean-logs  delete data/log before the run
#   --clean-user  delete user/ (settings, saves) before the run
#   --screenshots N  screenshots = N: a screenshot every N frames to data/log/shot_<frame>.bmp
#   --pad-script FILE  pad_script = pad_script.txt (copied): scripted controller input for tests
#   --use-syscore  (removed Oct 16 with io_on_system_core: accepted, ignored)
#   --sound [CORE]  sound = on (the default now; and sound_core = CORE; default: 2 on New 3DS, else 1)
#   --no-sound  sound = off. With sound on the game needs sdmc:/3ds/dspfirm.cdc: a real dump on hardware (DSP1 homebrew), any file for
#            Azahar's HLE audio (a placeholder is created on the emulator's SD card if missing).
#   --perf-sections  perf_sections = on: per-section frame timing in the log
#   --compress [lz4|zstd]  store the files read whole at load time (--compress-types) as X.ogz,
#            block-compressed (game/overlord/common/compressed_iso_file.h; default codec lz4).
#            The game decodes them on its file reader thread and reads X where there is no X.ogz.
#            Each file is checked with the game's decoder after compressing (iso_pack), and kept
#            in --compress-cache (default build-3ds/iso-ogz-<codec>) until its source changes.
#   --compress-types "DGO CGO ..."  extensions to compress (default: DGO CGO VIS TXT STR).
#            Never SBK/MUS (the sound loader reads those itself) or the VAGWAD audio streams.
#   --compress-vag  also the VAGWAD.* voice streams (all languages), as zstd + ADPCM frame split
#            in 16 KB blocks (~0.67 of the size; works with or without --compress)
#   --compress-cache DIR
#   --io-checksum  io_checksum = on: log the CRC-32 of each big file the game read (debug)
#
# Files are copied with APFS clones (cp -c) when possible, so staging 1.3 GB is instant on macOS.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
PROJ=""
SD=""
GK=""
GAME=jak1
C3L=""
C3V=""
ARGS=""
HAVE_ARGS=0
LISTENER=0
CLEAN_LOGS=0
CLEAN_USER=0
SHOTS=0
PAD_SCRIPT=""
USE_SYSCORE=0
PERF_SECTIONS=0
SOUND=""
NO_SOUND=0
PRESET=""
KEEP_CONFIG=0
RENDER_INI=""
COMPRESS=""
COMPRESS_TYPES="DGO CGO VIS TXT STR"
COMPRESS_CACHE=""
COMPRESS_VAG=0
IO_CHECKSUM=0

while [ $# -gt 0 ]; do
  case "$1" in
    --proj) PROJ="$2"; shift 2 ;;
    --game) GAME="$2"; shift 2 ;;
    --sd) SD="$2"; SD_GIVEN=1; shift 2 ;;
    --gk) GK="$2"; shift 2 ;;
    --c3l) C3L="$2"; shift 2 ;;
    --c3v) C3V="$2"; shift 2 ;;
    --args) ARGS="$2"; HAVE_ARGS=1; shift 2 ;;
    --listener) LISTENER=1; shift ;;
    --clean-logs) CLEAN_LOGS=1; shift ;;
    --clean-user) CLEAN_USER=1; shift ;;
    --screenshots) SHOTS="$2"; shift 2 ;;
    --pad-script) PAD_SCRIPT="$2"; shift 2 ;;
    --use-syscore) USE_SYSCORE=1; shift ;;
    --preset) PRESET="$2"; shift 2 ;;
    --keep-config) KEEP_CONFIG=1; shift ;;
    --no-sound) NO_SOUND=1; shift ;;
    --perf-sections) PERF_SECTIONS=1; shift ;;
    --sound) SOUND=auto; if [ $# -gt 1 ] && [[ "$2" =~ ^[0-3]$ ]]; then SOUND="$2"; shift; fi; shift ;;
    --render-ini|--config) RENDER_INI="$2"; shift 2 ;;
    --compress) COMPRESS=lz4; if [ $# -gt 1 ] && [[ "$2" =~ ^(lz4|zstd)$ ]]; then COMPRESS="$2"; shift; fi; shift ;;
    --compress-types) COMPRESS_TYPES="$2"; shift 2 ;;
    --compress-cache) COMPRESS_CACHE="$2"; shift 2 ;;
    --compress-vag) COMPRESS_VAG=1; shift ;;
    --io-checksum) IO_CHECKSUM=1; shift ;;
    *) echo "unknown option $1" >&2; exit 1 ;;
  esac
done

[ -n "$PROJ" ] || { echo "--proj is required" >&2; exit 1; }
case "$GAME" in jak1|jak2) ;; *) echo "--game: jak1 or jak2" >&2; exit 1 ;; esac
if [ -z "$GK" ]; then
  GK="$ROOT/build-3ds/gk.3dsx"
  [ "$GAME" = jak2 ] && GK="$ROOT/build-3ds-jak2/gk.3dsx"
fi
case "${PRESET:-normal}" in
  normal|safe|bench|breakdown) ;;
  *) echo "--preset: normal, safe, bench or breakdown" >&2; exit 1 ;;
esac
if [ "$GAME" = jak1 ]; then "$(dirname "$0")/check_fresh.sh" proj "$PROJ" || exit 1; fi
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
ISO="$PROJ/out/$GAME/iso"
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

BASE="$SD/3ds/$GAME"
DATA="$BASE/data"
OUT_DIR="$DATA/out/$GAME"
mkdir -p "$OUT_DIR"

copy() {  # src dst: APFS clone if possible
  cp -c "$1" "$2" 2>/dev/null || cp "$1" "$2"
}

# (AI-assisted) --compress: X.ogz for the chosen types, made by iso_pack (built here) into the cache
file_codec() {  # file name -> the iso_pack codec to store it with, or nothing
  local ext="${1##*.}" t
  ext="$(echo "$ext" | tr '[:lower:]' '[:upper:]')"
  case "$1" in
    VAGWAD.*) [ "$COMPRESS_VAG" = 1 ] && echo zstd-adpcm; return ;;
  esac
  [ -n "$COMPRESS" ] || return
  case "$ext" in SBK|MUS|AYB) return ;; esac
  for t in $COMPRESS_TYPES; do [ "$ext" = "$t" ] && echo "$COMPRESS" && return; done
}
compressed_type() { [ -n "$(file_codec "$1")" ]; }
if [ -n "$COMPRESS" ] || [ "$COMPRESS_VAG" = 1 ]; then
  PACK_DIR="$ROOT/build-host-tools"
  PACK="$PACK_DIR/iso_pack"
  PACK_SRC=("$ROOT/platform/3ds/tools/iso_pack.cpp" "$ROOT/game/overlord/common/compressed_iso_file.cpp"
            "$ROOT/game/overlord/common/compressed_iso_file.h")
  need=0
  [ -x "$PACK" ] || need=1
  for f in "${PACK_SRC[@]}"; do [ "$f" -nt "$PACK" ] && need=1; done
  if [ "$need" = 1 ]; then
    echo "building $PACK"
    rm -rf "$PACK_DIR/obj"; mkdir -p "$PACK_DIR/obj"
    for f in "$ROOT"/third-party/lz4/lz4.c "$ROOT"/third-party/lz4/lz4hc.c \
             "$ROOT"/third-party/zstd/lib/common/*.c "$ROOT"/third-party/zstd/lib/compress/*.c \
             "$ROOT"/third-party/zstd/lib/decompress/*.c; do echo "$f"; done |
      xargs -P 8 -I{} sh -c 'o="$1/obj/$(basename "$(dirname "$2")")_$(basename "$2").o"; cc -O2 -c "$2" -o "$o"' _ "$PACK_DIR" {}
    c++ -O2 -std=c++17 -I"$ROOT" "${PACK_SRC[0]}" "${PACK_SRC[1]}" "$PACK_DIR"/obj/*.o -o "$PACK"
  fi
  if [ -z "$COMPRESS_CACHE" ]; then
    COMPRESS_CACHE="$ROOT/build-3ds/iso-ogz-${COMPRESS:-vag}"
    # (AI-assisted) Jak II's files have the same names (KERNEL.CGO, GAME.CGO...)
    [ "$GAME" = jak2 ] && COMPRESS_CACHE="$ROOT/build-3ds-jak2/iso-ogz-${COMPRESS:-vag}"
  fi
  mkdir -p "$COMPRESS_CACHE"
  todo=()
  for f in "$ISO"/*; do
    b="$(basename "$f")"
    compressed_type "$b" || continue
    o="$COMPRESS_CACHE/$b.ogz"
    if [ ! -f "$o" ] || [ "$f" -nt "$o" ]; then todo+=("$(file_codec "$b") $f"); fi
  done
  if [ ${#todo[@]} -gt 0 ]; then
    echo "compressing ${#todo[@]} files into $COMPRESS_CACHE"
    printf '%s\n' "${todo[@]}" |
      xargs -P 8 -I{} sh -c 'c="${2%% *}"; f="${2#* }"; b="$(basename "$f")"; bs=0xc000; [ "$c" = zstd-adpcm ] && bs=0x4000; "$1" --codec "$c" --block-size $bs "$f" "$3/$b.ogz.tmp" && mv "$3/$b.ogz.tmp" "$3/$b.ogz" || { echo "iso_pack failed on $f" >&2; exit 255; }' \
        _ "$PACK" {} "$COMPRESS_CACHE"
  fi
fi

# GOAL objects and game files: replace the whole folder so stale files don't survive
rm -rf "$OUT_DIR/iso"
mkdir -p "$OUT_DIR/iso"
n=0
bytes_in=0
bytes_out=0
for f in "$ISO"/*; do
  b="$(basename "$f")"
  if compressed_type "$b"; then
    copy "$COMPRESS_CACHE/$b.ogz" "$OUT_DIR/iso/"
    bytes_in=$((bytes_in + $(stat -f %z "$f")))
    bytes_out=$((bytes_out + $(stat -f %z "$COMPRESS_CACHE/$b.ogz")))
  else
    copy "$f" "$OUT_DIR/iso/"
  fi
  n=$((n + 1))
done
if [ -n "$COMPRESS" ] || [ "$COMPRESS_VAG" = 1 ]; then
  what="${COMPRESS:+$COMPRESS: $COMPRESS_TYPES}"
  if [ "$COMPRESS_VAG" = 1 ]; then what="${what:+$what, }zstd-adpcm: VAGWAD"; fi
  echo "compressed: $bytes_in -> $bytes_out bytes ($what)"
fi

copy "$GK" "$BASE/gk.3dsx"

# level backgrounds for the 3DS renderer
[ -n "$C3L" ] || C3L="$PROJ/out/jak1/c3l"
if [ "$GAME" = jak1 ] && [ -d "$C3L" ]; then
  rm -rf "$DATA/out/jak1/c3l"
  mkdir -p "$DATA/out/jak1/c3l"
  for f in "$C3L"/*.c3l; do
    copy "$f" "$DATA/out/jak1/c3l/"
  done
  # (AI-assisted) fine_vis sidecars (ctr_vis_builder): next to the levels, or from --c3v DIR
  nc3v=0
  for f in "$C3L"/*.c3v ${C3V:+"$C3V"/*.c3v}; do
    [ -f "$f" ] || continue
    copy "$f" "$DATA/out/jak1/c3l/"
    nc3v=$((nc3v + 1))
  done
  echo "staged level backgrounds from $C3L ($nc3v fine visibility files)"
fi

if [ "$CLEAN_LOGS" = 1 ]; then rm -rf "$DATA/log"; fi
if [ "$CLEAN_USER" = 1 ]; then rm -rf "$BASE/user"; fi

# the one settings file: the template (or --config FILE / --keep-config: that file), then the
# options' keys
CONFIG="$BASE/config.ini"
TMP_CONFIG="$(mktemp)"
if [ -n "$RENDER_INI" ]; then
  cp "$RENDER_INI" "$TMP_CONFIG"
elif [ "$KEEP_CONFIG" = 1 ] && [ -f "$CONFIG" ]; then
  cp "$CONFIG" "$TMP_CONFIG"
elif [ "$KEEP_CONFIG" = 1 ] && [ -f "$BASE/render.ini" ]; then
  cp "$BASE/render.ini" "$TMP_CONFIG"  # the older name: carried over
else
  if [ "$GAME" = jak2 ]; then
    cp "$ROOT/platform/3ds/tools/config_template_jak2.ini" "$TMP_CONFIG"
  else
    cp "$ROOT/platform/3ds/tools/config_template.ini" "$TMP_CONFIG"
  fi
fi
TESTING_HEADER=0
set_key() {  # set_key KEY VALUE: replace the key's line where it is (or add it at the end)
  local tmp2
  tmp2="$(mktemp)"
  if grep -q -E "^[[:space:]]*$1[[:space:]]*=" "$TMP_CONFIG"; then
    awk -v k="$1" -v v="$2" '
      $0 ~ "^[[:space:]]*" k "[[:space:]]*=" { if (!done) print k " = " v; done = 1; next }
      { print }' "$TMP_CONFIG" > "$tmp2"
  else
    cp "$TMP_CONFIG" "$tmp2"
    if [ "$TESTING_HEADER" = 0 ] && ! grep -q "^# testing keys" "$tmp2"; then
      printf '\n# testing keys (docs/3ds-port/config_dev.md)\n' >> "$tmp2"
    fi
    TESTING_HEADER=1
    echo "$1 = $2" >> "$tmp2"
  fi
  mv "$tmp2" "$TMP_CONFIG"
}
if [ -n "$PRESET" ]; then
  set_key preset "$PRESET"
  if [ "$PRESET" = safe ] && [ -z "$RENDER_INI" ] && [ "$KEEP_CONFIG" = 0 ]; then
    # (AI-assisted) safe's particle limits are particles = reduced, written as a plain line
    set_key particles reduced
  fi
fi
if [ "$HAVE_ARGS" = 1 ]; then set_key args "$ARGS"; fi
if [ "$LISTENER" = 1 ]; then set_key listener on; fi
if [ "$USE_SYSCORE" = 1 ]; then echo "--use-syscore: io_on_system_core was removed (Oct 16), ignored" >&2; fi
if [ "$PERF_SECTIONS" = 1 ]; then set_key perf_sections on; fi
if [ "$IO_CHECKSUM" = 1 ]; then set_key io_checksum on; fi
if [ "$SHOTS" != 0 ]; then set_key screenshots "$SHOTS"; fi
if [ -n "$PAD_SCRIPT" ]; then
  cp "$PAD_SCRIPT" "$BASE/pad_script.txt"
  set_key pad_script pad_script.txt
fi
if [ -n "$SOUND" ]; then
  set_key sound on
  if [ "$SOUND" != auto ]; then set_key sound_core "$SOUND"; fi
fi
if [ "$NO_SOUND" = 1 ]; then set_key sound off; fi
# sound is on unless the file says off: the DSP firmware it needs
if ! grep -q -E "^[[:space:]]*sound[[:space:]]*=[[:space:]]*(off|0|no|false)[[:space:]]*$" "$TMP_CONFIG" &&
   [ ! -f "$SD/3ds/dspfirm.cdc" ]; then
  if [ -z "${SD_GIVEN:-}" ]; then
    mkdir -p "$SD/3ds"
    printf 'placeholder DSP component for Azahar HLE audio\n' > "$SD/3ds/dspfirm.cdc"
    echo "created a placeholder $SD/3ds/dspfirm.cdc (emulator only)"
  else
    echo "warning: no $SD/3ds/dspfirm.cdc, the game will run without audio (dump it with the DSP1 homebrew)" >&2
  fi
fi
mv "$TMP_CONFIG" "$CONFIG"
# no other settings files: the old flag files and render.ini copies are gone
rm -f "$BASE/args.txt" "$BASE/listener" "$BASE/use_syscore" "$BASE/perf_sections" "$BASE/sound" \
  "$BASE/single_core" "$BASE/screenshots" "$BASE/debug_log" "$BASE/mips2c_native_off" "$BASE/render.ini" \
  "$DATA/render.ini"
echo "settings: $CONFIG"

echo "staged $n files + gk.3dsx in $BASE"
