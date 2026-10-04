#!/bin/sh
# Build deb-native's minimal, prefix-aware `systemctl` shim from vendored SINS.
#
# SINS (github.com/Spinty-dev/SINS, MIT) is a systemd-on-runit compatibility
# layer. `upstream/` is a pristine copy at the commit in `UPSTREAM`;
# `patches/` make its paths resolve inside a deb-native userland prefix
# (`DN_INSTDIR`) instead of the real `/etc`, `/run`, `/sys` -- needed because
# a static Go binary cannot be redirected by the LD_PRELOAD shim. Everything
# outside `cmd/systemctl` and the packages it needs is pruned.
#
# Usage: build.sh [OUTDIR]     (default: out/ beside this script)
# Env:   GOOS GOARCH           (default: linux arm64)
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=${1:-"$here/out"}
work="$here/.work"
GOOS=${GOOS:-linux}
GOARCH=${GOARCH:-arm64}

command -v go >/dev/null 2>&1 || {
	echo "build.sh: no 'go' on PATH" >&2
	echo "  Termux:  pkg install golang" >&2
	echo "  or cross-compile elsewhere (\$GOOS/\$GOARCH) and copy the binary in" >&2
	exit 1
}

rm -rf "$work"
mkdir -p "$work"
cp -R "$here/upstream/." "$work/"

for p in "$here"/patches/*.patch; do
	echo "apply $(basename "$p")"
	patch -p1 -s -d "$work" -i "$p"
done

# Drop desktop modules + commands the minimal shim does not use.
rm -rf \
	"$work/cmd/sins-daemon" "$work/cmd/timers" "$work/cmd/socket-activator" \
	"$work/cmd/journalctl" "$work/cmd/systemd-analyze" \
	"$work/pkg/dbus" "$work/pkg/notify" "$work/pkg/cgroups" \
	"$work/pkg/sockets" "$work/pkg/timers" "$work/pkg/libsystemd" \
	"$work/pkg/supervisor"

mkdir -p "$out"
echo "build systemctl ($GOOS/$GOARCH)"
(
	cd "$work"
	CGO_ENABLED=0 GOOS="$GOOS" GOARCH="$GOARCH" \
		go build -trimpath -ldflags="-s -w" -o "$out/systemctl" ./cmd/systemctl
)
echo "built $out/systemctl"
