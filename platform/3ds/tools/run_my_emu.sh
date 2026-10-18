#!/usr/bin/env bash
# (AI-assisted)
# Like run_emu.sh, but for a private portable Azahar (an Azahar.app with a user/ folder next to
# it): no shared lock, no config rewriting, and only the emulator started here is killed.
#
#   run_my_emu.sh --app DIR --gk gk.3dsx --proj DIR --out DIR [--seconds N] [--pad FILE]
#                 [--screenshots N] [--args "..."] [--gl] [--sound [CORE]] [--game jak1|jak2]
#
#   --game jak2  (AI-assisted) the Jak II build: sdmc:/3ds/jak2/, <proj>/out/jak2/iso and c3l
#   --cia FILE   (AI-assisted) install the CIA in this emulator and run the installed title instead
#                of --gk (the CIA's 124 MB memory mode, as on hardware; a .3dsx gets less)
#
#   --sound  audio output on (config.ini sound = on; CORE: sound_core = the mixer thread's core,
#            default 2 on New 3DS, else 1; without --sound: sound = off unless the --ini sets it). Also puts a placeholder sdmc:/3ds/dspfirm.cdc in place if there is none:
#            Azahar's HLE DSP accepts any file (real hardware needs a real dump).
#
# DIR/Azahar.app and DIR/user/ (config/qt-config.ini with sdmc_directory=DIR/user/sdmc/).
set -uo pipefail
APPDIR=""; GK=""; PROJ=""; OUT=""; SECS=180; PAD=""; INI=""; SHOTS=300; ARGS=""; HAVE_ARGS=0; GL=0
SOUND=""
GAME=jak1
CIA=""
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
    --game) GAME="$2"; shift 2 ;;
    --cia) CIA="$2"; shift 2 ;;
    *) echo "unknown option $1" >&2; exit 1 ;;
  esac
done
[ -n "$APPDIR" ] && [ -n "$GK" ] && [ -n "$PROJ" ] && [ -n "$OUT" ] || { echo "usage: see header" >&2; exit 1; }
APP="$APPDIR/Azahar.app"
SD="$APPDIR/user/sdmc"
CFG="$APPDIR/user/config/qt-config.ini"
BASE="$SD/3ds/$GAME"
DATA="$BASE/data"
LOGDIR="$DATA/log"
AZ_LOG="$APPDIR/user/log/azahar_log.txt"

# graphics backend: 1 = OpenGL, 2 = Vulkan
api=2; [ "$GL" = 1 ] && api=1
sed -i '' "s/^graphics_api=.*/graphics_api=$api/" "$CFG"

# ---- stage (same layout as stage_sd.sh) ----
mkdir -p "$DATA/out/$GAME"
rm -rf "$DATA/out/$GAME/iso" "$DATA/out/$GAME/c3l"
mkdir -p "$DATA/out/$GAME/iso"
for f in "$PROJ"/out/$GAME/iso/*; do cp -c "$f" "$DATA/out/$GAME/iso/" 2>/dev/null || cp "$f" "$DATA/out/$GAME/iso/"; done
# (AI-assisted) the level backgrounds, Jak II's too (out/jak2/c3l)
if [ -d "$PROJ/out/$GAME/c3l" ]; then
  mkdir -p "$DATA/out/$GAME/c3l"
  for f in "$PROJ"/out/$GAME/c3l/*.c3l "$PROJ"/out/$GAME/c3l/*.c3v "$PROJ"/out/$GAME/c3l/*.c3t; do
    # (AI-assisted) .c3v: fine_vis sidecars, when there are some; .c3t: Jak II's texture
    # animation sources
    [ -f "$f" ] || continue
    cp -c "$f" "$DATA/out/$GAME/c3l/" 2>/dev/null || cp "$f" "$DATA/out/$GAME/c3l/"
  done
fi
cp "$GK" "$BASE/gk.3dsx"
# the one settings file, made for this run: the --ini file (renderer settings) plus the options
CONFIG="$BASE/config.ini"
if [ -n "$INI" ]; then cp "$INI" "$CONFIG"; else : > "$CONFIG"; fi
if [ "$HAVE_ARGS" = 1 ]; then echo "args = $ARGS" >> "$CONFIG"; fi
if [ -n "$SOUND" ]; then
  echo "sound = on" >> "$CONFIG"
  if [[ "$SOUND" =~ ^[0-3]$ ]]; then echo "sound_core = $SOUND" >> "$CONFIG"; fi
  if [ ! -f "$SD/3ds/dspfirm.cdc" ]; then
    printf 'placeholder DSP component for Azahar HLE audio\n' > "$SD/3ds/dspfirm.cdc"
  fi
else
  # (AI-assisted) the game's default is sound on since Oct 16: test runs stay silent without --sound
  grep -q -E "^[[:space:]]*sound[[:space:]]*=" "$CONFIG" || echo "sound = off" >> "$CONFIG"
fi
rm -rf "$DATA/log" "$BASE/user"
if [ -n "$PAD" ]; then
  cp "$PAD" "$BASE/pad_script.txt"
  echo "pad_script = pad_script.txt" >> "$CONFIG"
fi
if [ "$SHOTS" != 0 ]; then echo "screenshots = $SHOTS" >> "$CONFIG"; fi
# no other settings files
# the game appends to its logs: start each run with empty ones
rm -f "$LOGDIR/stdout.log" "$LOGDIR/gk.log" "$LOGDIR/stdout.old.log" "$LOGDIR/gk.old.log"
rm -f "$BASE/args.txt" "$BASE/listener" "$BASE/use_syscore" "$BASE/perf_sections" "$BASE/single_core" \
  "$BASE/sound" "$BASE/screenshots" "$BASE/debug_log" "$BASE/mips2c_native_off" "$BASE/render.ini" "$DATA/render.ini"
mkdir -p "$(dirname "$AZ_LOG")"; : > "$AZ_LOG"

# ---- run ----
mkdir -p "$OUT"
RUN="$BASE/gk.3dsx"
if [ -n "$CIA" ]; then
  # title id 00040000 <UniqueId << 8>: 0f7a1100 (Jak 1, gk.rsf), 0f7a2100 (Jak II, make_cia.sh)
  tid=0f7a1100; [ "$GAME" = jak2 ] && tid=0f7a2100
  rm -rf "$SD"/Nintendo\ 3DS/*/*/title/00040000/$tid
  echo "installing $CIA"
  "$APP/Contents/MacOS/azahar" -i "$(cd "$(dirname "$CIA")" && pwd)/$(basename "$CIA")" > /dev/null 2>&1
  RUN="$(ls "$SD"/Nintendo\ 3DS/*/*/title/00040000/$tid/content/*.app 2>/dev/null | head -1)"
  [ -n "$RUN" ] || { echo "the CIA was not installed" >&2; exit 1; }
fi
before="$(pgrep -f "$APP/Contents/MacOS/azahar" | sort || true)"
echo "starting $APP with $RUN for ${SECS}s"
open -n -a "$APP" --args -w "$RUN"
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
