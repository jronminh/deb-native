#!/bin/sh
# Phase B -- prefix-native finish (MODULARIZE.md "Ship is two phases"). Run
# once inside a freshly extracted prefix, using the prefix's own shell and
# tools: restore the patched glibc, refresh the loader cache, normalize
# symlinks, fix alternatives, regenerate launchers. With --apt-update, also
# refresh the package index.
#
# Everything it calls is already installed inside the prefix, at the prefix's
# own layout; it is therefore identical for every target.
#
# Usage: dn-finish.sh INSTDIR [--apt-update]
set -eu
DN=${1:?usage: dn-finish.sh INSTDIR [--apt-update]}
case "$DN" in /*) ;; *) DN="$PWD/$DN" ;; esac
[ -d "$DN/usr/lib/deb-native" ] || { echo "E: not a deb-native prefix: $DN" >&2; exit 1; }
APT_UPDATE=0
[ "${2:-}" = "--apt-update" ] && APT_UPDATE=1
INST="$DN/usr/lib/deb-native/scripts/install"
RUN="$DN/usr/lib/deb-native/scripts/runtime"

# 1. patched glibc first: a stock libc6 would break everything after it.
[ -x "$INST/dn-fix-glibc.sh" ] && "$INST/dn-fix-glibc.sh" "$DN" || true
# 2. refresh the loader cache from the prefix's own ld.so.conf.
[ -x "$DN/usr/sbin/ldconfig" ] && "$DN/usr/sbin/ldconfig" -C "$DN/usr/etc/ld.so.cache" -f "$DN/usr/etc/ld.so.conf" 2>/dev/null || true
# 3. symlinks and alternatives usable without the shim.
[ -x "$INST/dn-fix-alternatives.sh" ] && "$INST/dn-fix-alternatives.sh" "$DN" || true
[ -x "$INST/normalize-symlinks.sh" ] && "$INST/normalize-symlinks.sh" "$DN" || true
# 4. launchers for whatever the prefix shipped.
[ -x "$RUN/make-launchers.sh" ] && "$RUN/make-launchers.sh" "$DN" || true
# 5. optional index refresh (needs network); the user can do it later instead.
if [ "$APT_UPDATE" = 1 ]; then
  APT_CONFIG="$DN/etc/apt.conf" "$DN/usr/bin/apt-get" update || true
fi

echo "dn-finish: $DN ready."
