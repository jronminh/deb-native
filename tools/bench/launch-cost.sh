#!/bin/sh
# Cost of launching a trivial dynamic program under dn-monitor, per launch.
#
#   tools/bench/launch-cost.sh PREFIX [-m MODE] [-c CPU] [-n N] [-r RUNS] [-b DN_TINY]
#
# A `dash` loop in the prefix starts `true` N times; the loop is run with a
# short and with a long N and the per-launch cost is the difference, so the
# fixed cost of starting the shell cancels (docs/reference/ptrace-interception.md,
# "Measured: one trivial launch").  Run it on the device, for example through
# `run-as` in the app's own uid.
#
#   MODE   floor    no tracer: `ld.so` runs dash and true directly (the floor)
#          direct   dn-monitor, with `ld.so` started directly (one exec gate)
#          normal   dn-monitor, the normal path (dn-interp and dn-shim)
#          tiny     the minimal tracer given with -b (dn-tiny), on guest paths: one
#                   exec of `ld.so` started directly, as in `direct`
#          tinygate the same tracer with its exec gate: the loop starts `/usr/bin/true`, as in `normal`
#   -c CPU pin the tracer and every tracee to one core (`taskset`); default: free
#   -n N   the long loop (default 220); the short loop is 20
#   -r R   repeat R times and print every run (default 3)
#
# Time comes from `date`, whose own launch is a few ms and cancels in the
# difference; use N of at least 100 so the noise is small.  Where the
# processes land on the cores changes the result by 3 to 4 times on this
# device (see tools/bench/perf-run.sh for the background demotion that makes
# it worse); always record `cpu=` with a number.
set -eu

usage() { sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//' >&2; exit 64; }
[ $# -ge 1 ] || usage
P=$1; shift
MODE=direct CPU= N=220 RUNS=3 CATCH=
while [ $# -gt 0 ]; do
  case $1 in
    -m) MODE=${2:?}; shift 2 ;;
    -c) CPU=${2:?}; shift 2 ;;
    -n) N=${2:?}; shift 2 ;;
    -r) RUNS=${2:?}; shift 2 ;;
    -b) CATCH=${2:?}; shift 2 ;;
    *) usage ;;
  esac
done
LOW=20
GL=$P/usr/lib/aarch64-linux-gnu
LD=$GL/ld-linux-aarch64.so.1
MON=$P/usr/lib/deb-native/dn-monitor
G=/usr/lib/aarch64-linux-gnu                      # the same directory, as the guest sees it
[ -x "$MON" ] || [ "$MODE" = floor ] || { echo "launch-cost: no $MON" >&2; exit 1; }
export GLIBC_TUNABLES=glibc.pthread.rseq=0

PIN=
if [ -n "$CPU" ]; then
  PIN="taskset $(printf '%x' $((1 << CPU)))"
fi

# strip leading zeros so the shell does not read a number as octal
num() { v=$1; while [ "${v#0}" != "$v" ]; do v=${v#0}; done; echo "${v:-0}"; }

# wall clock as "SECONDS MICROSECONDS" (date +%s.%N). Android's mksh does 32-bit
# arithmetic, so the two parts are never multiplied together; only their
# differences are, and those are small.
stamp() { t=$(date +%s.%N); s=${t%.*}; f=${t#*.}; echo "$s ${f%???}"; }

hloop() { echo "i=0; while [ \$i -lt $1 ]; do $LD --library-path $GL $P/usr/bin/true; i=\$((i+1)); done"; }

cmd() {  # $1 = N
  case $MODE in
    floor)  $PIN $LD --library-path $GL $P/usr/bin/dash -c "$(hloop "$1")" ;;
    direct) $PIN "$MON" -- /usr/bin/dash -c "i=0; while [ \$i -lt $1 ]; do $G/ld-linux-aarch64.so.1 --library-path $G /usr/bin/true; i=\$((i+1)); done" ;;
    normal) $PIN "$MON" -- /usr/bin/dash -c "i=0; while [ \$i -lt $1 ]; do /usr/bin/true; i=\$((i+1)); done" ;;
    tiny)   $PIN "$CATCH" -q -m full -t "$P" -- $LD --library-path $GL $P/usr/bin/dash -c "i=0; while [ \$i -lt $1 ]; do $G/ld-linux-aarch64.so.1 --library-path $G /usr/bin/true; i=\$((i+1)); done" ;;
    tinygate) $PIN "$CATCH" -q -m full -t "$P" -- /usr/bin/dash -c "i=0; while [ \$i -lt $1 ]; do /usr/bin/true; i=\$((i+1)); done" ;;
    *) echo "launch-cost: unknown mode $MODE" >&2; exit 64 ;;
  esac
}

# microseconds taken by `cmd $1`
elapsed() {
  set -- "$1" $(stamp)
  cmd "$1" >/dev/null 2>&1 || true
  set -- "$1" "$2" "$3" $(stamp)
  echo $(( ($(num "$4") - $(num "$2")) * 1000000 + $(num "$5") - $(num "$3") ))
}

out=
r=0
while [ "$r" -lt "$RUNS" ]; do
  t1=$(elapsed "$LOW")
  t2=$(elapsed "$N")
  us=$(( (t2 - t1) / (N - LOW) ))                # microseconds per launch
  out="$out $((us / 1000)).$(printf '%02d' $((us % 1000 / 10)))"
  r=$((r + 1))
done
echo "launch-cost: mode=$MODE cpu=${CPU:-free} n=$N:$out ms/launch"
