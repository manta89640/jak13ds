#!/bin/zsh
# (AI-assisted) Host C-mode profiling run with macOS `sample` (docs/3ds-port/jak2_perf.md).
# usage: sample_run.sh GAME PROJECT GK OUTDIR PAD SECS MARK1,MARK2,... [VAR=VALUE ...]
# Runs silent with the null renderer at 30 Hz. Between "perf-mark M" and "perf-mark M-end" (pad
# script log lines) frames run back to back (OPENGOAL_NULL_GFX_NO_SYNC, time ratio 1, the thread
# stays on a performance core), and `sample` records SECS seconds at 1 ms from 2 s after M.
# Writes OUTDIR/out.log and OUTDIR/sample-M.txt. The log reaches the file in blocks (~10 s late),
# so make windows long: >= SECS + 15 s of no-sync frames. Sampling slows the game by ~15-20%.
game=$1; proj=$2; gk=$3; out=$4; pad=$5; secs=$6; marks=$7; shift 7
flag=$out/nosync.flag; mkdir -p $out; rm -f $flag
env OPENGOAL_NO_SOUND=1 OPENGOAL_NULL_GFX_FPS=30 OPENGOAL_NULL_GFX_NO_SYNC=$flag \
  OPENGOAL_PAD_SCRIPT=$pad "$@" \
  $gk -g $game --proj-path $proj --config-path $out/cfg --null-gfx -- -boot -fakeiso -nosound \
  -cbackend > $out/out.log 2>&1 &
pid=$!
waitmark() {
  while kill -0 $pid 2>/dev/null; do
    grep -aqE "perf-mark $1\$" $out/out.log && return 0
    sleep 0.3
  done
  return 1
}
for mk in ${(s:,:)marks}; do
  waitmark $mk || break
  touch $flag; sleep 2
  kill -0 $pid 2>/dev/null && sample $pid $secs 1 -mayDie -file $out/sample-$mk.txt >/dev/null 2>&1
  waitmark $mk-end; rm -f $flag
done
wait $pid; echo "exit $?"
