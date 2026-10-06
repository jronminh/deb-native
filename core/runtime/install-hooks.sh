#!/bin/sh
# Copy the prefix's apt hook scripts (and the files they call) INSIDE the
# prefix, so the prefix's own `apt` translates packages without depending on
# where the deb-native checkout lives. The hooks used to be pointed at
# <checkout>/core/install; moving or removing the checkout silently stopped
# runtime translation (docs/log/findings/silent-untranslated-runtime-installs.md).
#
# The copied tree keeps the runtime layout under $DN/usr/lib/deb-native/ so the
# hooks' relative paths still resolve:
#   dn-hook-post.sh      -> $HOOKS/../runtime/make-launchers.sh
#   make-launchers.sh    -> $HOOKS/../bench/scan-direct-syscalls.py
#   dn-translate-deb.sh  -> $HOOKS/../../core/custom/<pkg>.sh
# Idempotent: re-run to refresh after an update (install.sh does).
#
# Usage: install-hooks.sh INSTDIR
set -eu
INSTDIR=${1:?usage: install-hooks.sh INSTDIR}
case "$INSTDIR" in /*) ;; *) INSTDIR="$PWD/$INSTDIR" ;; esac
HERE=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)   # repo root
DEST="$INSTDIR/usr/lib/deb-native"
HOOKS="$DEST/scripts/install"

mkdir -p "$HOOKS" "$DEST/scripts/runtime" "$DEST/scripts/bench" "$DEST/core/custom"

# The runtime hook set: the scripts apt actually calls. Build-only scripts
# (setup-runtime.sh, make-priv.sh) and the bootstrap/survey helpers are
# deliberately not baked.
for s in dn-hook-pre.sh dn-hook-post.sh dn-translate-deb.sh \
         \
         dn-debian-index.sh; do
  cp -f "$HERE/core/install/$s" "$HOOKS/"
done
chmod 755 "$HOOKS/"*.sh

cp -f "$HERE/core/runtime/make-launchers.sh" "$DEST/scripts/runtime/"
chmod 755 "$DEST/scripts/runtime/make-launchers.sh"

# dn-update: the constrained overlay updater, run from the launcher dir.
cp -f "$HERE/core/runtime/dn-update.sh" "$DEST/scripts/runtime/"
chmod 755 "$DEST/scripts/runtime/dn-update.sh"

# dn-adopt: adopt a glibc program obtained outside apt (the bin/dn-adopt
# wrapper calls it).
cp -f "$HERE/core/runtime/dn-adopt.sh" "$DEST/scripts/runtime/"
chmod 755 "$DEST/scripts/runtime/dn-adopt.sh"

cp -f "$HERE/core/bench/scan-direct-syscalls.py" "$DEST/scripts/bench/"
chmod 755 "$DEST/scripts/bench/scan-direct-syscalls.py"

# custom/<pkg>.sh — per-package fixes; the directory is usually empty.
for f in "$HERE/core/custom/"*.sh; do
  [ -e "$f" ] || continue
  cp -f "$f" "$DEST/core/custom/"
done

echo "Hooks installed under $DEST/scripts."
