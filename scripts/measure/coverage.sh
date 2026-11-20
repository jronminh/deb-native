#!/bin/sh
# The coverage test (docs/spec/coverage.md): run the probe inside a prefix, print
# the tree's view of every surface, and fail on a line that leaks the host frame.
#
#   scripts/measure/coverage.sh PREFIX
#
# A leak is the prefix's own path, Android's data/storage/sdcard, the tracer, or
# any /data/... path.  The probe reads every surface the checklist names,
# including /proc/self/cmdline and status, so a host path there fails the run.
set -u
P=$(CDPATH= cd -- "${1:?usage: coverage.sh PREFIX}" && pwd)
DN=$P/usr/lib/deb-native/dn-init
[ -x "$DN" ] || { echo "no runtime at $DN" >&2; exit 1; }
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

cp "$HERE/coverage-probe.sh" "$P/tmp/dn-coverage-probe.sh" || exit 1
out=$(DN_INITIAL=root "$P/boot/boot.sh" sh /tmp/dn-coverage-probe.sh 2>&1)
printf '%s\n' "$out"
rm -f "$P/tmp/dn-coverage-probe.sh"

echo
echo "== leak check =="
# the host frame: the prefix path, Android's app data and shared storage, the
# tracer.  Paths, not variable names: LD_PRELOAD must name the tree's own shim
# (/usr/lib/deb-native/dn-lane.so) for the fast path to exist, and a host path in
# any variable is caught by the $P and /data/data/ patterns anyway.
leaks=$(printf '%s\n' "$out" | grep -nE "$P|/data/data/|/data/user/|/storage/emulated|/sdcard/|termux|ANDROID_|BOOTCLASSPATH|EXTERNAL_STORAGE" || true)
if [ -n "$leaks" ]; then
	printf '%s\n' "$leaks"
	echo "LEAKS: the lines above show the host (maps is the known hole)"
	exit 1
fi
echo "clean"
