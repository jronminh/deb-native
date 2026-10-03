#!/bin/sh
# Apply third_party/glibc-android-patches/dn-glibc-android.patch to a
# Debian glibc source tree, substituting the real target prefix for the
# patch's @TERMUX_PREFIX@ placeholder. The patch is never hand-edited or
# regenerated to change prefix: every path it bakes at compile time
# (ld.so.preload, the guest /etc, ...) goes through this one substitution,
# so the same patch builds the production prefix (.dn) or a throwaway test
# prefix (docs/spec/dn-glibc-prefix.md, "Install order") alike.
#
# Usage: dn-apply-glibc-patch.sh SOURCE_TREE PREFIX
#   SOURCE_TREE   Debian glibc source, Debian's own debian/patches/series
#                  already applied (quilt push -a) -- third_party/
#                  glibc-android-patches/README.md, "Applying this patch".
#   PREFIX        absolute path, no trailing slash, e.g.
#                  /data/data/com.termux/files/home/.dn (production) or
#                  .../dn6 (a throwaway test prefix). The glibc build
#                  itself is then configured --prefix="$PREFIX/usr".
set -eu
SRC=${1:?usage: dn-apply-glibc-patch.sh SOURCE_TREE PREFIX}
PREFIX=${2:?usage: dn-apply-glibc-patch.sh SOURCE_TREE PREFIX}
case "$SRC" in /*) ;; *) SRC="$PWD/$SRC" ;; esac
case "$PREFIX" in
  /*/) echo "E: PREFIX must have no trailing slash: $PREFIX" >&2; exit 1 ;;
  /*) ;;
  *) echo "E: PREFIX must be an absolute path: $PREFIX" >&2; exit 1 ;;
esac
[ -d "$SRC" ] || { echo "E: $SRC is not a directory" >&2; exit 1; }
HERE=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
PATCH="$HERE/third_party/glibc-android-patches/dn-glibc-android.patch"

echo "Applying dn-glibc-android.patch to $SRC for prefix $PREFIX ..."
sed "s|@TERMUX_PREFIX@|$PREFIX|g" "$PATCH" | patch -p1 -d "$SRC"
