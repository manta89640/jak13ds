#!/usr/bin/env bash
# (AI-assisted)
# Sampling profiler for gk running in Azahar: pauses the emulator through its GDB stub every
# --interval seconds, records every thread's pc, then maps pcs to functions (GOAL functions are
# named by module and GOAL name from the C backend sources).
#
#   platform/3ds/tools/profile_emu.sh --proj <project> [--gk gk.3dsx --elf gk.elf]
#        [--pad-script FILE] [--warmup SECONDS] [--samples N] [--interval S] [--out DIR]
#
# Needs a fresh build (check_fresh.sh). The pad script defaults to tests/gameplay.pad, so samples
# are taken in gameplay after the warmup.
set -uo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
PROJ=""; GK="$ROOT/build-3ds/gk.3dsx"; ELF=""; PAD="$ROOT/platform/3ds/tests/gameplay.pad"
WARMUP=150; SAMPLES=300; INTERVAL=0.2; OUT="$ROOT/build-3ds/profile"
while [ $# -gt 0 ]; do
  case "$1" in
    --proj) PROJ="$2"; shift 2 ;;
    --gk) GK="$2"; shift 2 ;;
    --elf) ELF="$2"; shift 2 ;;
    --pad-script) PAD="$2"; shift 2 ;;
    --warmup) WARMUP="$2"; shift 2 ;;
    --samples) SAMPLES="$2"; shift 2 ;;
    --interval) INTERVAL="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    *) echo "unknown option $1" >&2; exit 1 ;;
  esac
done
[ -n "$PROJ" ] || { echo "--proj is required" >&2; exit 1; }
[ -n "$ELF" ] || ELF="${GK%.3dsx}.elf"
mkdir -p "$OUT"
# gdb loops "continue"; we interrupt it with SIGINT and wait until it has printed the sample
# (==END==) before the next interrupt, so the signal never lands in the middle of a gdb command
cat > "$OUT/sample.gdb" <<'GDB'
set width 0
set height 0
set print thread-events off
while 1
  continue
  echo ==SAMPLE==\n
  thread apply all printf "T %d %x\n", $_thread, $pc
  echo ==END==\n
end
GDB
TOTAL=$(python3 -c "print(int($WARMUP + $SAMPLES * ($INTERVAL + 0.5) + 90))")
"$HERE/run_emu.sh" --seconds "$TOTAL" --out "$OUT/run" --elf "$ELF" --gdb-script "$OUT/sample.gdb" \
  --stage "--proj $PROJ --gk $GK --pad-script $PAD --clean-logs" > "$OUT/run_emu.txt" 2>&1 &
RUN_PID=$!
GDB_TXT="$OUT/run/gdb.txt"
for _ in $(seq 1 900); do pgrep -f "arm-none-eabi-gdb.*$OUT/run/gdb.cmd" > /dev/null && break; sleep 1; done
sleep "$WARMUP"
GDB_PID=$(pgrep -f "arm-none-eabi-gdb.*$OUT/run/gdb.cmd" | head -1)
if [ -z "$GDB_PID" ]; then
  echo "gdb is not running (see $OUT/run_emu.txt)" >&2
else
  for _ in $(seq 1 "$SAMPLES"); do
    before=$(grep -c "==END==" "$GDB_TXT" 2>/dev/null || echo 0)
    kill -INT "$GDB_PID" 2>/dev/null || break
    for _ in $(seq 1 200); do
      [ "$(grep -c "==END==" "$GDB_TXT" 2>/dev/null || echo 0)" -gt "$before" ] && break
      sleep 0.05
    done
    sleep "$INTERVAL"
  done
fi
wait $RUN_PID
python3 "$HERE/profile_report.py" "$OUT/run/gdb.txt" "$ELF" "$PROJ/out/jak1/csrc" | tee "$OUT/report.txt"
