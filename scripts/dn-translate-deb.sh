#!/bin/sh
# Translate one Debian .deb for the prefix, in place, before dpkg sees it
# (docs/design-0.2.0.md; naibed's fuse-repack.sh, minus the flat-layout
# steps -- the prefix is a real, nested Debian root):
#
#   - "Architecture: all" -> "arm64", the same rule dn-debian-index.sh
#     applied to the index apt planned from, so apt and dpkg agree;
#   - custom/<package>.sh, if any: per-package fixes;
#   - glibc ELFs repointed at the libc6 stand-in: interpreter
#     $DN/usr/lib/ld-linux-aarch64.so.1, and $DN/usr/lib/aarch64-linux-gnu
#     first in every dynamic ELF's RUNPATH (shared libraries too: RUNPATH is
#     not inherited, and Termux's ld.so searches only $PREFIX/glibc/lib by
#     itself). Done here, not after install, so it is right before any
#     maintainer script runs the binary;
#   - maintainer-script shebangs -> dn-shell (patch-scripts-tree.sh, the
#     loop patch-deb.sh runs).
#
# One unpack and one repack per package, uncompressed (-Znone): the result
# only lives in a temp or cache folder until dpkg installs it, and xz on a
# phone was a large share of the bootstrap.
#
# Idempotent. Usage: dn-translate-deb.sh DEB_FILE PREFIX
set -eu
umask 022
DEB=${1:?usage: dn-translate-deb.sh DEB_FILE PREFIX}
DN=${2:?usage: dn-translate-deb.sh DEB_FILE PREFIX}
case "$DEB" in /*) ;; *) DEB="$PWD/$DEB" ;; esac
case "$DN" in /*) ;; *) DN="$PWD/$DN" ;; esac
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
LD="$DN/usr/lib/ld-linux-aarch64.so.1"
LIBDIR="$DN/usr/lib/aarch64-linux-gnu"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
dpkg-deb -R "$DEB" "$WORK/pkg"
PKG=$(sed -n 's/^Package: //p' "$WORK/pkg/DEBIAN/control")
echo "Translating $PKG:arm64 ($(sed -n 's/^Version: //p' "$WORK/pkg/DEBIAN/control")) ..."

sed -i 's/^Architecture: all$/Architecture: arm64/' "$WORK/pkg/DEBIAN/control"

if [ -x "$HERE/../custom/$PKG.sh" ]; then
  "$HERE/../custom/$PKG.sh" "$WORK/pkg" "$DN"
  echo "Applied custom/$PKG.sh to $PKG."
fi

find "$WORK/pkg" -path "$WORK/pkg/DEBIAN" -prune -o -type f -print | while IFS= read -r f; do
  [ "$(head -c4 "$f" | od -An -tx1 | tr -d ' \n')" = 7f454c46 ] || continue
  # Libraries and static programs have no interpreter: patchelf says so
  # (captured, not shown -- it is the expected answer, not an error).
  interp=$(patchelf --print-interpreter "$f" 2>&1) || interp=""
  case "$interp" in
    */ld-linux-aarch64.so.1) [ "$interp" = "$LD" ] || patchelf --set-interpreter "$LD" "$f" ;;
  esac
  old=$(patchelf --print-rpath "$f" 2>&1) || continue   # not dynamic
  case ":$old:" in *":$LIBDIR:"*) continue ;; esac
  patchelf --set-rpath "$LIBDIR${old:+:$old}" "$f"
done

"$HERE/patch-scripts-tree.sh" "$WORK/pkg" "$DN"

dpkg-deb -Znone -b "$WORK/pkg" "$WORK/out.deb"
mv -f "$WORK/out.deb" "$DEB"
