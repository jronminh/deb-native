#!/data/data/com.termux/files/usr/bin/bash
# perf-run: run a command under a Termux wake-lock so Android doesn't demote
# it to efficiency cores or kill it while Termux isn't the foreground app /
# the screen is off — this device drops backgrounded Termux children (e.g.
# a long build, or an inference server) onto small cores independent of
# actual CPU load or thermal state; observed as a >100x throughput cliff in
# jronminh/micro_llm_factcheck's notes/android-background-app-throttling.md
# (27 tok/s -> 0.17 tok/s), fixed there the same way: hold a wake-lock for
# the duration of the work.
#
# Usage: scripts/perf-run.sh <command> [args...]
# Exit code is the wrapped command's own exit code (64 if no command given).
#
# Shares its wake-lock bookkeeping dir/lockfile with
# claude-code-termux-native's session-hooks.sh so the two don't race each
# other's termux-wake-lock/-unlock calls (that lock is systemwide, not
# scoped to a PID) — see that script's WAKELOCK_LOCKFILE comment. A marker
# file dropped here also stops session-hooks.sh's cmd_stop from releasing
# the lock out from under a still-running perf-run.
set -u

if [ "$#" -eq 0 ]; then
  echo "usage: perf-run.sh <command> [args...]" >&2
  exit 64
fi

STATE_DIR="${TMPDIR:-/tmp}/claude-code-termux-native"
mkdir -p "$STATE_DIR" 2>/dev/null || true
LOCKFILE="$STATE_DIR/.wakelock.lock"
MARKER="$STATE_DIR/perf-run.$$.since"

cleanup() {
  (
    flock -w 3 200 2>/dev/null
    rm -f "$MARKER" 2>/dev/null
    local others=0
    ls "$STATE_DIR"/*.since >/dev/null 2>&1 && others=1
    if [ "$others" = "0" ]; then
      command -v termux-wake-unlock >/dev/null 2>&1 && termux-wake-unlock 2>/dev/null
    fi
  ) 200>"$LOCKFILE"
}
trap cleanup EXIT

command -v termux-wake-lock >/dev/null 2>&1 && termux-wake-lock 2>/dev/null
(
  flock -w 3 200 2>/dev/null
  date +%s > "$MARKER" 2>/dev/null
) 200>"$LOCKFILE"

"$@"
exit $?
