#!/usr/bin/env bash
# (AI-assisted)
# Run gk.3dsx in Azahar for a while, then kill it and collect the logs.
#
#   platform/3ds/tools/run_emu.sh [--seconds N] [--out DIR] [--azahar Azahar.app] [--sd DIR]
#                                 [--stage "<stage_sd.sh arguments>"] [--cia jak1.cia]
#
#   --cia   install the CIA in the emulator and run the installed title instead of gk.3dsx
#           (tests the CIA's memory mode; the game files still come from the staged SD card)
#                                 [--gdb [--elf gk.elf] [--gdb-script FILE]]
#
#   --gdb   enable Azahar's GDB stub and attach arm-none-eabi-gdb; after N seconds the game is
#           interrupted and all thread backtraces are written to gdb.txt (needs the matching
#           gk.elf, default build-3ds/gk.elf). Also catches crashes (data/prefetch aborts).
#           --gdb-script: use your own gdb commands (after "target remote localhost:24689").
#
# Stage the SD card first (platform/3ds/tools/stage_sd.sh). Collected into --out (default
# build-3ds/emu-run):
#   stdout.log   everything gk printed (GOAL output + runtime), from the virtual SD card
#   gk.log       runtime log at debug level
#   azahar.log   the emulator's log (crashes: data/prefetch aborts, svcBreak, unimplemented svcs)
#
# Notes (macOS):
# - Azahar must be started through LaunchServices (open -n -a). Started directly from a shell
#   that has no GUI session (like an agent/ssh session) it hangs before loading the ROM.
# - A window appears (there is no headless mode). The first-start wizard, update check and close
#   confirmation are turned off in qt-config.ini (a backup is kept as qt-config.ini.orig).
# - SIGTERM is ignored while emulating, so the run ends with SIGKILL.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
SECONDS_TO_RUN=60
OUT="$ROOT/build-3ds/emu-run"
APP="${AZAHAR_APP:-$HOME/devkitpro-3ds/emu/azahar-macos-arm64-2126.1.2/Azahar.app}"
SD=""
STAGE_ARGS=""
CIA=""
DEVKITARM_BIN="${DEVKITARM:-/opt/devkitpro/devkitARM}/bin"
GDB=0
ELF="$ROOT/build-3ds/gk.elf"
GDB_SCRIPT=""

while [ $# -gt 0 ]; do
  case "$1" in
    --seconds) SECONDS_TO_RUN="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --azahar) APP="$2"; shift 2 ;;
    --sd) SD="$2"; shift 2 ;;
    --gdb) GDB=1; shift ;;
    --elf) ELF="$2"; shift 2 ;;
    --gdb-script) GDB=1; GDB_SCRIPT="$2"; shift 2 ;;
    --stage) STAGE_ARGS="$2"; shift 2 ;;
    --cia) CIA="$2"; shift 2 ;;
    *) echo "unknown option $1" >&2; exit 1 ;;
  esac
done

# Several agents share one Azahar (one config, one virtual SD card): take a lock for the whole run,
# and stage the SD card inside it (--stage "<stage_sd.sh arguments>") so that nobody replaces the
# files in the middle of a run. A stale lock (its owner is gone) is taken over.
EMU_LOCK="${TMPDIR:-/tmp}/opengoal-azahar.lock"
# (edit this script with a copy + mv: bash reads a running script as it goes)
waited=0
while ! mkdir "$EMU_LOCK" 2>/dev/null; do
  owner="$(cat "$EMU_LOCK/pid" 2>/dev/null || true)"
  if { [ -n "$owner" ] && ! kill -0 "$owner" 2>/dev/null; } || { [ -z "$owner" ] && [ "$waited" -ge 30 ]; }; then
    rm -rf "$EMU_LOCK"
    continue
  fi
  waited=$((waited + 10))
  echo "waiting for the emulator (used by pid ${owner:-?})"
  sleep 10
done
echo $$ > "$EMU_LOCK/pid"
trap 'rm -rf "$EMU_LOCK"' EXIT
if [ -n "$STAGE_ARGS" ]; then
  # shellcheck disable=SC2086
  "$(dirname "$0")/stage_sd.sh" $STAGE_ARGS
fi

AZ_DIR="$HOME/Library/Application Support/Azahar"
CFG="$AZ_DIR/config/qt-config.ini"
AZ_LOG="$AZ_DIR/log/azahar_log.txt"
[ -d "$APP" ] || { echo "Azahar not found at $APP (see docs/3ds-port/toolchain.md)" >&2; exit 1; }

if [ -z "$SD" ]; then
  SD="$(sed -n 's/^sdmc_directory=//p' "$CFG" 2>/dev/null | head -1)"
  [ -n "$SD" ] || SD="$AZ_DIR/sdmc/"
  SD="${SD%/}"
fi
GK="$SD/3ds/jak1/gk.3dsx"
if [ -n "$CIA" ]; then
  pkill -9 -f 'Azahar.app/Contents/MacOS/azahar' 2>/dev/null || true
  echo "installing $CIA"
  # title id 00040000 000f7a11 (platform/3ds/cia/gk.rsf); Azahar keeps installed titles on its SD
  rm -rf "$SD"/Nintendo\ 3DS/*/*/title/00040000/000f7a11
  open -n -a "$APP" --args -i "$(cd "$(dirname "$CIA")" && pwd)/$(basename "$CIA")"
  for _ in $(seq 60); do
    sleep 1
    GK="$(ls "$SD"/Nintendo\ 3DS/*/*/title/00040000/000f7a11/content/*.app 2>/dev/null | head -1)"
    [ -z "$GK" ] || break
  done
  sleep 3
  pkill -9 -f 'Azahar.app/Contents/MacOS/azahar' 2>/dev/null || true
  [ -n "$GK" ] || { echo "the CIA was not installed" >&2; exit 1; }
  echo "running the installed title $GK"
fi
LOGDIR="$SD/3ds/jak1/data/log"
[ -f "$GK" ] || { echo "no $GK, run stage_sd.sh first" >&2; exit 1; }

# unattended settings
if [ -f "$CFG" ]; then
  [ -f "$CFG.orig" ] || cp "$CFG" "$CFG.orig"
  python3 - "$CFG" "$GDB" <<'EOF'
import re, sys
p = sys.argv[1]
gdb = sys.argv[2] == "1"
s = open(p).read()
for k, v in [("firstStart", "false"), ("check_for_update_on_start", "false"),
             ("confirmClose", "false"), ("enable_discord_presence", "false"),
             ("pauseWhenInBackground", "false"), ("calloutFlags", "4294967295"),
             # flush the log at every line (we SIGKILL the emulator) and show the app's
             # svcOutputDebugString output (ctr_port tees stdout there too)
             ("instant_debug_log", "true"), ("log_filter", "*:Info Debug.Emulated:Trace"),
             ("use_gdbstub", "true" if gdb else "false")]:
    s = re.sub(r"(?m)^" + re.escape(k) + r"=.*$", f"{k}={v}", s)
    s = re.sub(r"(?m)^" + re.escape(k) + r"\\default=.*$", f"{k}\\\\default=false", s)
open(p, "w").write(s)
EOF
fi

pkill -9 -f 'Azahar.app/Contents/MacOS/azahar' 2>/dev/null
rm -f "$LOGDIR/stdout.log" "$LOGDIR/gk.log"
mkdir -p "$(dirname "$AZ_LOG")"
: > "$AZ_LOG"

echo "starting Azahar with $GK for ${SECONDS_TO_RUN}s"
open -n -a "$APP" --args -w "$GK"
sleep 2
PID="$(pgrep -n -f 'Azahar.app/Contents/MacOS/azahar')"
[ -n "$PID" ] || { echo "Azahar did not start" >&2; exit 1; }

mkdir -p "$OUT"
GDB_PID=""
if [ "$GDB" = 1 ]; then
  # with the stub enabled, the emulator waits for the debugger before running the app
  cat > "$OUT/gdb.cmd" <<GDBEOF
set pagination off
set confirm off
set remotetimeout 30
target remote localhost:24689
continue
echo \n==== stopped ====\n
info registers
x/4i \$pc
info threads
thread apply all bt 25
detach
quit
GDBEOF
  if [ -n "$GDB_SCRIPT" ]; then
    printf 'set pagination off\nset confirm off\nset remotetimeout 30\ntarget remote localhost:24689\n' > "$OUT/gdb.cmd"
    cat "$GDB_SCRIPT" >> "$OUT/gdb.cmd"
  fi
  sleep 2
  "$DEVKITARM_BIN/arm-none-eabi-gdb" -q -batch -x "$OUT/gdb.cmd" "$ELF" > "$OUT/gdb.txt" 2>&1 &
  GDB_PID=$!
fi

end=$((SECONDS + SECONDS_TO_RUN))
while [ $SECONDS -lt $end ] && kill -0 "$PID" 2>/dev/null; do
  if [ -n "$GDB_PID" ] && ! kill -0 "$GDB_PID" 2>/dev/null; then
    echo "gdb returned early (crash / breakpoint?)"
    break
  fi
  sleep 1
done
if [ -n "$GDB_PID" ] && kill -0 "$GDB_PID" 2>/dev/null; then
  kill -INT "$GDB_PID"  # interrupt the target; gdb then dumps the threads
  for _ in $(seq 1 30); do kill -0 "$GDB_PID" 2>/dev/null || break; sleep 1; done
  kill -9 "$GDB_PID" 2>/dev/null
fi
if kill -0 "$PID" 2>/dev/null; then
  kill -9 "$PID"
  echo "stopped Azahar after ${SECONDS_TO_RUN}s"
else
  echo "Azahar exited by itself"
fi
sleep 1

# the emulator log can be huge (e.g. an access loop): keep the first and last 20000 lines
if [ -f "$AZ_LOG" ]; then
  if [ "$(wc -l < "$AZ_LOG")" -gt 40000 ]; then
    { head -n 20000 "$AZ_LOG"; echo "... (truncated) ..."; tail -n 20000 "$AZ_LOG"; } > "$OUT/azahar.log"
  else
    cp "$AZ_LOG" "$OUT/azahar.log"
  fi
else
  : > "$OUT/azahar.log"
fi
cp "$LOGDIR/stdout.log" "$OUT/stdout.log" 2>/dev/null || : > "$OUT/stdout.log"
cp "$LOGDIR/gk.log" "$OUT/gk.log" 2>/dev/null || : > "$OUT/gk.log"
# screenshots (stage_sd.sh --screenshots), converted to PNG
rm -f "$OUT"/shot_*.png
for f in "$LOGDIR"/shot_*.bmp; do
  [ -f "$f" ] || continue
  sips -s format png "$f" --out "$OUT/$(basename "${f%.bmp}").png" > /dev/null 2>&1
  rm -f "$f"
done
ls "$OUT"/shot_*.png 2>/dev/null | sed 's/^/screenshot: /'

echo "== logs in $OUT: stdout.log $(wc -l < "$OUT/stdout.log") lines, gk.log $(wc -l < "$OUT/gk.log") lines"
echo "== last lines of stdout.log:"
tail -n 25 "$OUT/stdout.log"
if [ "$GDB" = 1 ]; then
  echo "== gdb (full output in $OUT/gdb.txt):"
  sed -n '/==== stopped ====/,$p' "$OUT/gdb.txt" | grep -E '^(#|\*|  [0-9]+ +Thread|Thread|Program|pc )' | head -80
fi
echo "== emulator problems:"
grep -E "abort|svcBreak|Break|Unmapped|Invalid|panic|Fatal|fatal|crash|Unimplemented|unimplemented" \
  "$OUT/azahar.log" | grep -v 'ConfigureNew3DSCPU' | uniq -c -f 3 | tail -n 20
