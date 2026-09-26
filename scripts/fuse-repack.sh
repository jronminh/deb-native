#!/bin/sh
# True fusion (naibed): repackage a .deb so its own data
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

# Move $1 (a usr/ entry) to $2 (its place at the prefix root). Directories
# present on both sides (a package shipping both /lib/x and /usr/lib/y) are
# merged entry by entry; a name present on both sides is refused.
merge_up() {
  if [ -d "$2" ] && [ ! -L "$2" ] && [ -d "$1" ] && [ ! -L "$1" ]; then
    for e in "$1/"* "$1/".[!.]*; do
      [ -e "$e" ] || [ -L "$e" ] || continue
      merge_up "$e" "$2/${e##*/}"
    done
    rmdir "$1"
  elif [ -e "$2" ] || [ -L "$2" ]; then
    echo "fuse-repack: $DEB ships both ${2#$WORK/pkg} and /usr${2#$WORK/pkg}; refusing" >&2
    exit 1
  else
    mv "$1" "$2"
  fi
}

# Merged-/usr links (base-files ships bin -> usr/bin, lib -> usr/lib, ...):
# in the flat prefix bin/ already IS usr/bin (usr -> .), so drop them.
for f in "$WORK/pkg/"*; do
  [ -L "$f" ] || continue
  [ "$(readlink "$f")" = "usr/${f##*/}" ] && rm "$f"
done

if [ -d "$WORK/pkg/usr" ]; then
  for f in "$WORK/pkg/usr/"* "$WORK/pkg/usr/".[!.]*; do
    [ -e "$f" ] || [ -L "$f" ] || continue
    merge_up "$f" "$WORK/pkg/${f##*/}"
  done
  rmdir "$WORK/pkg/usr"
fi

CONFFILES="$WORK/pkg/DEBIAN/conffiles"
if [ -f "$CONFFILES" ]; then
  sed -i 's#^/usr/#/#' "$CONFFILES"
fi

# Tier-2 customizations (docs/true-fusion.md): a package that needs a
# fusion-specific change gets it from fusion-custom/<package>.sh.
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PKGNAME=$(sed -n 's/^Package: //p' "$WORK/pkg/DEBIAN/control")
if [ -x "$HERE/fusion-custom/$PKGNAME.sh" ]; then
  "$HERE/fusion-custom/$PKGNAME.sh" "$WORK/pkg"
  echo "fuse-repack: applied fusion-custom/$PKGNAME.sh"
fi

# True fusion index rule, applied to the package itself (docs/true-fusion.md):
# apt planned this package as arm64 from the rewritten index, so dpkg must
# record it as arm64 too, never as "all" (= Termux's native side).
sed -i 's/^Architecture: all$/Architecture: arm64/' "$WORK/pkg/DEBIAN/control"

# Repoint glibc ELFs at the libc6:arm64 identity paths (dn-base-env.sh), in
# the package itself so they are right before any maintainer script runs:
# interpreter $TP/lib/ld-linux-aarch64.so.1, and $TP/lib/aarch64-linux-gnu
# first in every dynamic ELF's RUNPATH (shared libraries too -- RUNPATH is
# not inherited, and Termux's ld.so only searches $TP/glibc/lib by itself).
# main's patch-elfs.sh does the same edit after install, via grun --configure.
TP=${DN_TERMUX_PREFIX:-${PREFIX:-/data/data/com.termux/files/usr}}
LIBDIR="$TP/lib/aarch64-linux-gnu"
find "$WORK/pkg" -path "$WORK/pkg/DEBIAN" -prune -o -type f -print | while IFS= read -r f; do
  [ "$(head -c4 "$f" 2>/dev/null | od -An -tx1 | tr -d ' \n')" = 7f454c46 ] || continue
  interp=$(patchelf --print-interpreter "$f" 2>/dev/null) || interp=""
  case "$interp" in
    */ld-linux-aarch64.so.1) patchelf --set-interpreter "$TP/lib/ld-linux-aarch64.so.1" "$f" ;;
  esac
  old=$(patchelf --print-rpath "$f" 2>/dev/null) || continue   # not dynamic
  case ":$old:" in *":$LIBDIR:"*) continue ;; esac
  patchelf --set-rpath "$LIBDIR${old:+:$old}" "$f"
done

dpkg-deb -b "$WORK/pkg" "$WORK/out.deb" >/dev/null
mv "$WORK/out.deb" "$DEB"
echo "fuse-repack: rewrote $DEB (usr/ merged, arch arm64, ELFs repointed)"
