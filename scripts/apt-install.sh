#!/bin/sh
# Install a package (and its real dependencies) into a prefix set up by
# setup-apt-prefix.sh, in three explicit, separated phases:
#   1. download only (apt resolves the dependency graph; no dpkg call)
#   2. patch-deb.sh each downloaded .deb individually (control scripts'
#      hardcoded paths and shebang, INSIDE the archive, before dpkg ever
#      sees it -- necessary because a package's preinst runs during
#      dpkg's own --unpack, before any post-unpack patch step could
#      touch it: docs/findings.md)
#   3. install one package at a time, in apt's own resolved order
#      (--unpack then --configure per package, not batched) -- found the
#      hard way that batching breaks Pre-Depends ordering (base-files
#      pre-depends on awk; unpacking everything first and configuring
#      "-a" afterward left mawk still "unpacked but not configured" when
#      base-files's own unpack ran, since --configure -a's own internal
#      order doesn't match strict Pre-Depends needs the way installing
#      one at a time, immediately configuring each, does)
#
# Usage: apt-install.sh $NEWPREFIX package [package...]
set -eu
NEWPREFIX=${1:?usage: apt-install.sh NEWPREFIX package...}
case "$NEWPREFIX" in /*) ;; *) NEWPREFIX="$PWD/$NEWPREFIX" ;; esac
shift
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ARCHIVES="$NEWPREFIX/var/cache/apt/archives"
DPKG_FLAGS="--force-not-root --force-script-chrootless --force-architecture"
# Absolute paths: a bare apt-get/dpkg can resolve to ANOTHER prefix's
# arch-aware wrapper (make-apt-wrappers.sh puts one on PATH per activated
# prefix), which substitutes its own APT_CONFIG/--instdir/--admindir --
# silently operating on the wrong prefix. Never rely on PATH here.
TERMUX_PREFIX=${DN_TERMUX_PREFIX:-${PREFIX:-/data/data/com.termux/files/usr}}
APT_GET="$TERMUX_PREFIX/bin/apt-get"
DPKG="$TERMUX_PREFIX/bin/dpkg"

echo "==> resolving and downloading (no install yet)"
DLLOG=$(mktemp)
APT_CONFIG="$NEWPREFIX/etc/apt.conf" "$APT_GET" install -y --no-install-recommends \
  --download-only "$@" | tee "$DLLOG"

# apt's own "Get:" lines list packages in the order it resolved to fetch
# them, which follows dependency order (a Pre-Depends/Depends is fetched
# before what needs it) -- use that order, not a filesystem listing
# (alphabetical/inode order, unrelated to dependencies) for step 3.
order=$(grep -oE '^Get:[0-9]+ [^ ]+ [^ ]+ [^ ]+ [^ ]+' "$DLLOG" |
  awk '{print $5}')
rm -f "$DLLOG"

debs=$(find "$ARCHIVES" -maxdepth 1 -name '*.deb')
[ -n "$debs" ] || { echo "nothing to install (already installed?)"; exit 0; }

echo "==> patching control scripts inside each .deb before unpack"
for d in $debs; do "$HERE/patch-deb.sh" "$d" "$NEWPREFIX/root"; done

deb_for_pkg() {  # $1 = package name (as it appears in apt's Get: line)
  find "$ARCHIVES" -maxdepth 1 -iname "${1}_*.deb" -print -quit
}

echo "==> installing one package at a time, in apt's resolved order"
seen=""
for pkg in $order; do
  case " $seen " in *" $pkg "*) continue ;; esac
  seen="$seen $pkg"
  deb=$(deb_for_pkg "$pkg")
  [ -n "$deb" ] || continue
  "$DPKG" --instdir="$NEWPREFIX/root" --admindir="$NEWPREFIX/var/lib/dpkg" \
       $DPKG_FLAGS --unpack "$deb" || true
  # Repoint new ELFs at Termux's glibc loader (and skip our own runtime).
  "$HERE/patch-elfs.sh" "$NEWPREFIX/root"
  # Re-patch: dash (or its wrapper) may not have existed yet when this
  # exact package's own control scripts were patched above, if this
  # package was earlier in apt's order than dash itself.
  "$HERE/patch-maintainer-scripts.sh" "$NEWPREFIX/var/lib/dpkg" "$NEWPREFIX/root" || true
  "$DPKG" --instdir="$NEWPREFIX/root" --admindir="$NEWPREFIX/var/lib/dpkg" \
       $DPKG_FLAGS --configure "$pkg" || true
done

echo "==> configuring, final sweep (resolves ordering, not real failures)"
"$DPKG" --instdir="$NEWPREFIX/root" --admindir="$NEWPREFIX/var/lib/dpkg" \
     $DPKG_FLAGS --configure -a || true

# apt keeps downloaded .debs in $ARCHIVES by default; a LATER
# apt-install.sh call's own listing would otherwise pick up a STALE .deb
# from an earlier transaction and --unpack it again, silently overwriting
# an already grun-patched binary with the archive's pristine (unpatched
# ELF interpreter) copy -- found the hard way with dash.
rm -f "$ARCHIVES"/*.deb

# Normalize absolute symlinks so the bind-only tracer resolves them inside
# the prefix (docs/bind-only.md), then generate/refresh launcher wrappers for
# every program now in the prefix (scripts/make-launchers.sh).
"$HERE/normalize-symlinks.sh" "$NEWPREFIX/root"
"$HERE/make-launchers.sh" "$NEWPREFIX/root"
