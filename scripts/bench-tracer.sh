#!/bin/sh
# Benchmark fork-lite (bind-only) against a reference proot on path-op-dense
# workloads.  Run on the device (aarch64 Termux); needs a big tree to walk.
#
#   scripts/bench-tracer.sh [REF_PROOT] [LITE_PROOT] [N_STAT] [RUNS]
#
# Defaults bound Termux's own $PREFIX over the guest /usr, so both tracers
# traverse the same large tree.  Reports `real` seconds per run.
set -eu

T=${PREFIX:-/data/data/com.termux/files/usr}
REF=${1:-$T/bin/proot}
LITE=${2:-$(CDPATH= cd -- "$(dirname -- "$0")/../tracer" && pwd)/proot}
N=${3:-20000}
RUNS=${4:-3}

BASH_BIN=$T/bin/bash
FIND_BIN=$T/bin/find
BIND="$T:/usr"

# TIMEFORMAT=%R makes `time` print just the elapsed seconds.
run() {
    "$BASH_BIN" -c 'TIMEFORMAT=%R; time "$@" >/dev/null' _ "$@"
}

bench() {
    label=$1
    shift
    echo "== $label"
    i=0
    while [ "$i" -lt "$RUNS" ]; do
        run "$@"
        i=$((i + 1))
    done
}

STAT_LOOP='i=0; while [ $i -lt '"$N"' ]; do [ -e /usr/bin/ls ] || exit 1; i=$((i+1)); done'

echo "# stat loop N=$N (single process, no fork)"
bench "og        " "$REF"  -b "$BIND" "$BASH_BIN" -c "$STAT_LOOP"
bench "lite      " "$LITE" -b "$BIND" "$BASH_BIN" -c "$STAT_LOOP"
bench "lite-canon" env PROOT_NO_BIND_ONLY=1 "$LITE" -b "$BIND" "$BASH_BIN" -c "$STAT_LOOP"

echo "# walk $T (find -type f, one process)"
bench "og        " "$REF"  -b "$BIND" "$FIND_BIN" /usr -xdev -type f
bench "lite      " "$LITE" -b "$BIND" "$FIND_BIN" /usr -xdev -type f
bench "lite-canon" env PROOT_NO_BIND_ONLY=1 "$LITE" -b "$BIND" "$FIND_BIN" /usr -xdev -type f
