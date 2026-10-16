#!/bin/sh
# The coverage test (docs/spec/coverage.md): run the probe inside a prefix, print
# the tree's view of every surface, and fail on a line that leaks the host frame.
#
#   scripts/coverage.sh PREFIX
#
# A leak is the prefix's own path, Android's data/storage/sdcard, the tracer, or
# any /data/... path.  /proc/self/maps is a known hole (it names the libraries by
# their host paths) and shows up here until it is mapped.
set -u
P=$(CDPATH= cd -- "${1:?usage: coverage.sh PREFIX}" && pwd)
DN=$P/usr/lib/deb-native/dn-tiny
[ -x "$DN" ] || { echo "no runtime at $DN" >&2; exit 1; }
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

cp "$HERE/coverage/probe.sh" "$P/tmp/dn-coverage-probe.sh" || exit 1
out=$("$P/boot/boot.sh" sh /tmp/dn-coverage-probe.sh 2>&1)
printf '%s\n' "$out"
rm -f "$P/tmp/dn-coverage-probe.sh"

echo
echo "== leak check =="
# the host frame: the prefix path, Android's app data and shared storage, the tracer
leaks=$(printf '%s\n' "$out" | grep -nE "$P|/data/data/|/data/user/|/storage/emulated|/sdcard/|termux|dn-tiny" || true)
if [ -n "$leaks" ]; then
	printf '%s\n' "$leaks"
	echo "LEAKS: the lines above show the host (maps is the known hole)"
	exit 1
fi
echo "clean"
