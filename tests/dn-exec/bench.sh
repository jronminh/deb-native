#!/bin/sh
# A/B the current kernel-exec loader against dn-exec.  POC only.
#
#   tests/dn-exec/bench.sh PREFIX [REPS]
set -eu

P=$(CDPATH= cd -- "$1" && pwd)
REPS=${2:-2}
SRC=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
TINY=$SRC/build/dn-tiny
TMP=$(mktemp -d "$P/tmp/dnbench.XXXXXX") || exit 1
trap 'rm -rf "$TMP"' EXIT

run() {
    mode=$1; code=$2; n=$3
    case $mode in
        kernel) D= ;;
        dnexec) D=/usr/local/bin/dn-exec ;;
    esac
    code="cd /; $(printf '%s' "$code" | sed "s|@D@|$D|g")"
    t0=$(date +%s%N)
    "$TINY" -s 500 -t "$P" -- /usr/bin/bash -c "$code" >/dev/null 2>"$TMP/log"
    rc=$?
    t1=$(date +%s%N)
    wall=$(( (t1 - t0) / 1000000 ))
    line=$(grep 'enter=' "$TMP/log" | tail -1)
    echo "$mode\t$n\t${wall}ms\t$rc\t$line"
}

for w in find tar ls spawn; do
    case $w in
        find)  code='@D@ /usr/bin/find /usr -type f >/dev/null' ;;
        tar)   code='@D@ /usr/bin/tar -cf /tmp/dnbench.tar -C /usr/share/doc . && @D@ /usr/bin/tar -xf /tmp/dnbench.tar -C /tmp' ;;
        ls)    code='@D@ /usr/bin/ls -lR /usr/share >/dev/null' ;;
        spawn) code='i=0; while [ $i -lt 100 ]; do @D@ /usr/bin/true || exit; i=$((i+1)); done' ;;
    esac
    echo "== $w"
    n=0
    while [ "$n" -lt "$REPS" ]; do
        run kernel "$code" "$n"
        run dnexec "$code" "$n"
        n=$((n+1))
    done
done
