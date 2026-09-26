#!/bin/sh
# Branch experiment (fusion-no-prefix): repackage a .deb so its own data
# archive matches Termux's flat layout instead of a real Debian root's.
# Termux's $PREFIX has no nested usr/ of its own -- $PREFIX/bin IS what
# /usr/bin means on a real Debian root -- so a package's usr/* content is
# merged up one level (usr/bin/x -> ./bin/x) while etc/, var/, opt/ stay
# where they are (Termux already has real etc/ and var/ matching Debian's
# own convention there, same split native/path-redirect.c's DN_FUSE_USR
# mode implements at the syscall layer). The two have to agree; this is
# the install-time half of that same rule, not a second definition of it.
#
# Only handles the common case (usr/ and etc/var/opt as plain siblings,
# no pre-existing top-level bin/lib/etc from the archive itself, relative
# symlinks only) -- proof-of-concept for one package at a time, not a
# general-purpose transform yet.
#
# Usage: fuse-repack.sh DEB_FILE
# Rewrites DEB_FILE in place (via dpkg-deb -b into a temp file, then mv).
set -eu
DEB=${1:?usage: fuse-repack.sh DEB_FILE}
case "$DEB" in /*) ;; *) DEB="$PWD/$DEB" ;; esac

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

dpkg-deb -R "$DEB" "$WORK/pkg"

if [ -d "$WORK/pkg/usr" ]; then
  for existing in bin sbin lib lib64 share include games; do
    if [ -e "$WORK/pkg/$existing" ] && [ -e "$WORK/pkg/usr/$existing" ]; then
      echo "fuse-repack: $DEB already has top-level $existing/ colliding with usr/$existing/ -- not handled by this proof-of-concept, refusing" >&2
      exit 1
    fi
  done
  # Move each usr/* entry up one level (siblings of etc/var/opt), then drop
  # the now-empty usr/ itself.
  for f in "$WORK/pkg/usr/"* "$WORK/pkg/usr/".[!.]*; do
    [ -e "$f" ] || continue
    mv "$f" "$WORK/pkg/$(basename "$f")"
  done
  rmdir "$WORK/pkg/usr"
fi

CONFFILES="$WORK/pkg/DEBIAN/conffiles"
if [ -f "$CONFFILES" ]; then
  sed -i 's#^/usr/#/#' "$CONFFILES"
fi

dpkg-deb -b "$WORK/pkg" "$WORK/out.deb" >/dev/null
mv "$WORK/out.deb" "$DEB"
echo "fuse-repack: rewrote $DEB (usr/ merged to root)"
