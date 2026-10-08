#!/bin/sh
# Apply the glibc patches to a Debian glibc source tree:
#   - patches/dn-glibc-android.patch, substituting the real target prefix
#     for its @DN_PREFIX@ placeholder (every compile-time path it bakes --
#     ld.so.preload, the guest /etc, ... -- goes through that one
#     substitution, so the same patch builds the production prefix (.dn) or
#     a throwaway test prefix; docs/spec/overlay.md, "Install order");
#   - then patches/dn-policy-glibc-wiring.patch, which names no prefix (the
#     dn-policy it embeds derives TREE at run time).
#
# Usage: dn-apply-glibc-patch.sh SOURCE_TREE PREFIX
#   SOURCE_TREE   Debian glibc source, Debian's own debian/patches/series
#                  already applied (quilt push -a) -- patches/README.md,
#                  "Applying this patch".
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
PATCH="$HERE/patches/dn-glibc-android.patch"

echo "Applying dn-glibc-android.patch to $SRC for prefix $PREFIX ..."
sed "s|@DN_PREFIX@|$PREFIX|g" "$PATCH" | patch -p1 -d "$SRC"

# The dn-policy wiring (patches/dn-policy-glibc-wiring.patch) goes on top:
# it names no prefix (dn-policy derives TREE at run time), so it needs no
# substitution.  It replaces __dn_redirect in the wrappers with real
# dn-policy calls, wires syscall(2), and adds the gate page + the RT/lib-first
# search order (docs/spec/overlay.md, "Building dn-glibc").  Kept a separate
# file from the Android patch on purpose, so the two can be reviewed and
# re-generated independently.
echo "Applying dn-policy-glibc-wiring.patch to $SRC ..."
patch -p1 -d "$SRC" < "$HERE/patches/dn-policy-glibc-wiring.patch"
