#!/bin/sh
# apt's DPkg::Pre-Install-Pkgs hook: patch every .deb apt is about to hand to
# dpkg, before dpkg unpacks it. This is the apt-driven equivalent of
# scripts/apt-install.sh's patch step -- same reason (a package's preinst
# runs during --unpack, before any post-unpack patch could touch it), but
# hooked into apt's own lifecycle like sudo-less does, so a plain
# `apt-get install` works without a bespoke loop.
#
# apt feeds the list of .deb files on stdin (one per line); a DEB... argument
# list works too, for testing by hand.
#
# Usage: apt-hook-pre.sh NEWPREFIX [DEB...] < DEB-LIST
set -eu
NEWPREFIX=${1:?usage: apt-hook-pre.sh NEWPREFIX [DEB...]}
shift || true
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT="$NEWPREFIX/root"
LOG="$NEWPREFIX/var/log/deb-native-hook.log"
mkdir -p "$NEWPREFIX/var/log"

if [ $# -gt 0 ]; then
  debs="$*"
else
  debs=$(cat)
fi

"$HERE/setup-runtime.sh" "$ROOT" >>"$LOG" 2>&1 || true

echo "$debs" | while IFS= read -r line; do
  deb=${line%% *}          # apt may append fields; the path is first
  [ -n "$deb" ] || continue
  [ -f "$deb" ] || continue
  echo "== patch $deb" >>"$LOG"
  "$HERE/patch-deb.sh" "$deb" "$ROOT" >>"$LOG" 2>&1 || \
    echo "patch-deb failed: $deb" >>"$LOG"
done
