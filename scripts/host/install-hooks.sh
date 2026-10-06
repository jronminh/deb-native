#!/bin/sh
# Copy the prefix's apt hook scripts (and the files they call) INSIDE the
# prefix, so the prefix's own `apt` translates packages without depending on
# where the deb-native checkout lives. The hooks used to be pointed at
# <checkout>/scripts/prefix; moving or removing the checkout silently stopped
# runtime translation (docs/log/findings/silent-untranslated-runtime-installs.md).
#
# The copied tree keeps the runtime layout under $DN/usr/lib/deb-native/ so the
# hooks' relative paths still resolve:
#   dn-hook-post.sh      (launchers are made inside it)
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

mkdir -p "$HOOKS" "$DEST/core/custom"

# The runtime hook set: the scripts apt actually calls. Build-only scripts
# (setup-runtime.sh, make-priv.sh) and the bootstrap/survey helpers are
# deliberately not baked.
for s in dn-hook-pre.sh dn-hook-post.sh dn-translate-deb.sh; do
  cp -f "$HERE/scripts/prefix/$s" "$HOOKS/"
done
chmod 755 "$HOOKS/"*.sh


# dn-update and dn-adopt are optional (not in the core overlay): run from the
# checkout, not staged here.
# custom/<pkg>.sh — per-package fixes; the directory is usually empty.
for f in "$HERE/scripts/prefix/custom/"*.sh; do
  [ -e "$f" ] || continue
  cp -f "$f" "$DEST/core/custom/"
done

echo "Hooks installed under $DEST/scripts."
