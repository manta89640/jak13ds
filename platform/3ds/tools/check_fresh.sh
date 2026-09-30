#!/usr/bin/env bash
# (AI-assisted)
# Refuse to stage or run 3DS builds that are older than the code they come from.
#
#   check_fresh.sh gk <gk.3dsx|jak1.cia>   binary older than the newest commit on 3ds-port?
#   check_fresh.sh proj <project dir>       C modules or .c3l levels older than the commits that
#                                           change them?
#
# Rebuild with build_cmodules.sh (modules + gk), ctr_level_converter (levels), make_cia.sh.
# OPENGOAL_ALLOW_STALE=1 skips the check (only for deliberate A/B tests of old builds).
set -uo pipefail
[ "${OPENGOAL_ALLOW_STALE:-0}" = "1" ] && exit 0
ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
BRANCH=3ds-port

commit_time() {  # newest commit on the branch touching the given paths (or anything)
  git -C "$ROOT" log -1 --format=%ct "$BRANCH" -- "$@" 2>/dev/null || echo 0
}
mtime() { stat -f %m "$1" 2>/dev/null || stat -c %Y "$1" 2>/dev/null || echo 0; }
newest_in() { local n=0 t f; for f in "$1"/*."$2"; do [ -e "$f" ] || continue; t=$(mtime "$f"); [ "$t" -gt "$n" ] && n=$t; done; echo $n; }
fail() { echo "STALE BUILD: $1" >&2; echo "rebuild first (see docs/3ds-port/3ds_build.md), or set OPENGOAL_ALLOW_STALE=1" >&2; exit 1; }

case "${1:-}" in
  gk)
    f="$2"; [ -f "$f" ] || fail "$f does not exist"
    c=$(commit_time)
    [ "$(mtime "$f")" -ge "$c" ] || fail "$f is older than the newest commit on $BRANCH ($(git -C "$ROOT" log -1 --format='%h %s' "$BRANCH"))"
    ;;
  proj)
    p="$2"
    c=$(commit_time goalc goal_src/jak1 game/kernel)
    m=$(newest_in "$p/out/jak1/csrc" c)
    [ "$m" -ge "$c" ] || fail "C modules in $p/out/jak1/csrc are older than the last compiler/GOAL change on $BRANCH (run build_cmodules.sh)"
    if [ -d "$p/out/jak1/c3l" ]; then
      c=$(commit_time tools/ctr_level_converter game/graphics/ctr/c3l_format.h)
      m=$(newest_in "$p/out/jak1/c3l" c3l)
      [ "$m" -ge "$c" ] || fail "levels in $p/out/jak1/c3l are older than the last converter change on $BRANCH (run ctr_level_converter --all)"
    fi
    ;;
  *) echo "usage: check_fresh.sh gk <file> | proj <dir>" >&2; exit 2 ;;
esac
