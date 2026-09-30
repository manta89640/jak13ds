#!/usr/bin/env bash
# (AI-assisted)
# Like run_emu.sh, but for a private portable Azahar (an Azahar.app with a user/ folder next to
# it): no shared lock, no config rewriting, and only the emulator started here is killed.
#
#   run_my_emu.sh --app DIR --gk gk.3dsx --proj DIR --out DIR [--seconds N] [--pad FILE]
#                 [--screenshots N] [--args "..."] [--gl] [--sound [CORE]]
#
#   --sound  audio output on (flag file sdmc:/3ds/jak1/sound; CORE = the mixer thread's core,
#            default 2 on New 3DS, else 1). Also puts a placeholder sdmc:/3ds/dspfirm.cdc in place if there is none:
#            Azahar's HLE DSP accepts any file (real hardware needs a real dump).
#
# DIR/Azahar.app and DIR/user/ (config/qt-config.ini with sdmc_directory=DIR/user/sdmc/).
set -uo pipefail
APPDIR=""; GK=""; PROJ=""; OUT=""; SECS=180; PAD=""; INI=""; SHOTS=300; ARGS=""; HAVE_ARGS=0; GL=0
SOUND=""
while [ $# -gt 0 ]; do
  case "$1" in
    --app) APPDIR="$2"; shift 2 ;;
    --gk) GK="$2"; shift 2 ;;
    --proj) PROJ="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --seconds) SECS="$2"; shift 2 ;;
    --pad) PAD="$2"; shift 2 ;;
    --screenshots) SHOTS="$2"; shift 2 ;;
    --args) ARGS="$2"; HAVE_ARGS=1; shift 2 ;;
    --gl) GL=1; shift ;;
    --sound) SOUND=auto; if [ $# -gt 1 ] && [[ "$2" =~ ^[0-3]$ ]]; then SOUND="$2"; shift; fi; shift ;;
    --ini) INI="$2"; shift 2 ;;
    *) echo "unknown option $1" >&2; exit 1 ;;
  esac
done
[ -n "$APPDIR" ] && [ -n "$GK" ] && [ -n "$PROJ" ] && [ -n "$OUT" ] || { echo "usage: see header" >&2; exit 1; }
APP="$APPDIR/Azahar.app"
SD="$APPDIR/user/sdmc"
CFG="$APPDIR/user/config/qt-config.ini"
BASE="$SD/3ds/jak1"
DATA="$BASE/data"
LOGDIR="$DATA/log"
AZ_LOG="$APPDIR/user/log/azahar_log.txt"

# graphics backend: 1 = OpenGL, 2 = Vulkan
api=2; [ "$GL" = 1 ] && api=1
sed -i '' "s/^graphics_api=.*/graphics_api=$api/" "$CFG"

# ---- stage (same layout as stage_sd.sh) ----
mkdir -p "$DATA/out/jak1"
rm -rf "$DATA/out/jak1/iso" "$DATA/out/jak1/c3l"
mkdir -p "$DATA/out/jak1/iso" "$DATA/out/jak1/c3l"
for f in "$PROJ"/out/jak1/iso/*; do cp -c "$f" "$DATA/out/jak1/iso/" 2>/dev/null || cp "$f" "$DATA/out/jak1/iso/"; done
for f in "$PROJ"/out/jak1/c3l/*.c3l; do cp -c "$f" "$DATA/out/jak1/c3l/" 2>/dev/null || cp "$f" "$DATA/out/jak1/c3l/"; done
cp "$GK" "$BASE/gk.3dsx"
if [ "$HAVE_ARGS" = 1 ]; then echo "$ARGS" > "$BASE/args.txt"; else rm -f "$BASE/args.txt"; fi
rm -f "$BASE/listener" "$BASE/use_syscore" "$BASE/perf_sections" "$BASE/single_core" "$BASE/sound"
if [ -n "$SOUND" ]; then
  echo "$SOUND" > "$BASE/sound"
  if [ ! -f "$SD/3ds/dspfirm.cdc" ]; then
    printf 'placeholder DSP component for Azahar HLE audio\n' > "$SD/3ds/dspfirm.cdc"
  fi
fi
rm -rf "$DATA/log" "$BASE/user"
if [ -n "$PAD" ]; then cp "$PAD" "$BASE/pad_script.txt"; else rm -f "$BASE/pad_script.txt"; fi
if [ -n "$INI" ]; then cp "$INI" "$BASE/render.ini"; else rm -f "$BASE/render.ini"; fi
if [ "$SHOTS" != 0 ]; then echo "$SHOTS" > "$BASE/screenshots"; else rm -f "$BASE/screenshots"; fi
mkdir -p "$(dirname "$AZ_LOG")"; : > "$AZ_LOG"

# ---- run ----
mkdir -p "$OUT"
before="$(pgrep -f "$APP/Contents/MacOS/azahar" | sort || true)"
echo "starting $APP with $BASE/gk.3dsx for ${SECS}s"
open -n -a "$APP" --args -w "$BASE/gk.3dsx"
sleep 3
PID=""
for p in $(pgrep -f "$APP/Contents/MacOS/azahar" | sort); do
  echo "$before" | grep -qx "$p" || PID="$p"
done
[ -n "$PID" ] || { echo "Azahar did not start" >&2; exit 1; }
end=$((SECONDS + SECS))
while [ $SECONDS -lt $end ] && kill -0 "$PID" 2>/dev/null; do sleep 1; done
if kill -0 "$PID" 2>/dev/null; then kill -9 "$PID"; echo "stopped Azahar (pid $PID) after ${SECS}s"; else echo "Azahar exited by itself"; fi
sleep 1

# ---- collect ----
cp "$AZ_LOG" "$OUT/azahar.log" 2>/dev/null || : > "$OUT/azahar.log"
cp "$LOGDIR/stdout.log" "$OUT/stdout.log" 2>/dev/null || : > "$OUT/stdout.log"
cp "$LOGDIR/gk.log" "$OUT/gk.log" 2>/dev/null || : > "$OUT/gk.log"
rm -f "$OUT"/shot_*.png
for f in "$LOGDIR"/shot_*.bmp; do
  [ -f "$f" ] || continue
  sips -s format png "$f" --out "$OUT/$(basename "${f%.bmp}").png" > /dev/null 2>&1
done
ls "$OUT"/shot_*.png 2>/dev/null | sed 's/^/screenshot: /'
echo "== gk.log $(wc -l < "$OUT/gk.log") lines; loads:"
grep -E "\[ctr\] (loaded|unloading|no background)" "$OUT/gk.log" | head -20
