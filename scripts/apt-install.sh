#!/bin/sh
# Install a package (and its real dependencies) into a prefix set up by
# setup-apt-prefix.sh, in three explicit, separated phases:
#   1. download only (apt resolves the dependency graph; no dpkg call)
#   2. patch-deb.sh each downloaded .deb individually (control scripts'
#      hardcoded paths and shebang, INSIDE the archive, before dpkg ever
#      sees it -- necessary because a package's preinst runs during
#      dpkg's own --unpack, before any post-unpack patch step could
#      touch it: docs/findings-dash-wrapper-2026-09-25.md)
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
shift
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ARCHIVES="$NEWPREFIX/var/cache/apt/archives"
DPKG_FLAGS="--force-not-root --force-script-chrootless --force-architecture"

echo "==> resolving and downloading (no install yet)"
DLLOG=$(mktemp)
APT_CONFIG="$NEWPREFIX/etc/apt.conf" apt-get install -y --no-install-recommends \
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
  dpkg --instdir="$NEWPREFIX/root" --admindir="$NEWPREFIX/var/lib/dpkg" \
       $DPKG_FLAGS --unpack "$deb" || true
  find "$NEWPREFIX/root" -type f -perm -u+x 2>/dev/null |
    while IFS= read -r f; do
      # Never grun-patch this project's own runtime: dn-shell/dn-perl are
      # Bionic launchers and the shim is a glibc .so; rewriting their ELF
      # interpreter to the glibc target breaks them ("libdl.so: cannot open
      # shared object file").
      case "$f" in
        "$NEWPREFIX/root/lib/deb-native/"*|\
        "$NEWPREFIX/root/usr/bin/dn-shell"|\
        "$NEWPREFIX/root/usr/bin/dn-perl") continue ;;
      esac
      case "$(head -c4 "$f" 2>/dev/null | od -An -tx1 | tr -d ' \n')" in
        7f454c46) grun --configure "$f" >/dev/null 2>&1 || true ;;
      esac
    done
  # Re-patch: dash (or its wrapper) may not have existed yet when this
  # exact package's own control scripts were patched above, if this
  # package was earlier in apt's order than dash itself.
  "$HERE/patch-maintainer-scripts.sh" "$NEWPREFIX/var/lib/dpkg" "$NEWPREFIX/root" || true
  dpkg --instdir="$NEWPREFIX/root" --admindir="$NEWPREFIX/var/lib/dpkg" \
       $DPKG_FLAGS --configure "$pkg" || true
done

echo "==> configuring, final sweep (resolves ordering, not real failures)"
dpkg --instdir="$NEWPREFIX/root" --admindir="$NEWPREFIX/var/lib/dpkg" \
     $DPKG_FLAGS --configure -a || true

# apt keeps downloaded .debs in $ARCHIVES by default; a LATER
# apt-install.sh call's own listing would otherwise pick up a STALE .deb
# from an earlier transaction and --unpack it again, silently overwriting
# an already grun-patched binary with the archive's pristine (unpatched
# ELF interpreter) copy -- found the hard way with dash.
rm -f "$ARCHIVES"/*.deb
