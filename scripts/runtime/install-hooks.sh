#!/bin/sh
# Copy the prefix's apt hook scripts (and the files they call) INSIDE the
# prefix, so the prefix's own `apt` translates packages without depending on
# where the deb-native checkout lives. The hooks used to be pointed at
# <checkout>/scripts/install; moving or removing the checkout silently stopped
# runtime translation (docs/log/findings/silent-untranslated-runtime-installs.md).
#
# The copied tree keeps the repo's layout under $DN/usr/lib/deb-native/ so the
# hooks' relative paths still resolve:
#   dn-hook-post.sh      -> $HOOKS/../runtime/make-launchers.sh
#   make-launchers.sh    -> $HOOKS/../bench/scan-direct-syscalls.py
#   dn-translate-deb.sh  -> $HOOKS/../../custom/<pkg>.sh
# Idempotent: re-run to refresh after an update (install.sh does).
#
# Usage: install-hooks.sh INSTDIR
set -eu
INSTDIR=${1:?usage: install-hooks.sh INSTDIR}
case "$INSTDIR" in /*) ;; *) INSTDIR="$PWD/$INSTDIR" ;; esac
HERE=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)   # repo root
DEST="$INSTDIR/usr/lib/deb-native"
HOOKS="$DEST/scripts/install"

mkdir -p "$HOOKS" "$DEST/scripts/runtime" "$DEST/scripts/bench" "$DEST/custom"

# scripts/install/*.sh is the whole hook set: dn-hook-{pre,post}.sh,
# dn-translate-deb.sh, patch-scripts-tree.sh, dn-fix-{alternatives,gcc-specs}.sh,
# normalize-symlinks.sh, dn-debian-index.sh.
cp -f "$HERE/scripts/install/"*.sh "$HOOKS/"
chmod 755 "$HOOKS/"*.sh

cp -f "$HERE/scripts/runtime/make-launchers.sh" "$DEST/scripts/runtime/"
chmod 755 "$DEST/scripts/runtime/make-launchers.sh"

# dn-update: the constrained overlay updater, run from the launcher dir.
cp -f "$HERE/scripts/runtime/dn-update.sh" "$DEST/scripts/runtime/"
chmod 755 "$DEST/scripts/runtime/dn-update.sh"

cp -f "$HERE/scripts/bench/scan-direct-syscalls.py" "$DEST/scripts/bench/"
chmod 755 "$DEST/scripts/bench/scan-direct-syscalls.py"

# custom/<pkg>.sh — per-package fixes; the directory is usually empty.
for f in "$HERE/custom/"*.sh; do
  [ -e "$f" ] || continue
  cp -f "$f" "$DEST/custom/"
done

echo "Hooks installed under $DEST/scripts."
