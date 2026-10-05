#!/bin/sh
# Package a built prefix into the shipped artifact (MODULARIZE.md "Build vs
# Ship"): a tarball named from core/VERSION, plus a manifest of the runtime
# components (path -> sha256) that dn-update consumes. Build-stage only: it
# reads a prefix that was built elsewhere and writes a file; it never touches a
# target.
#
# Usage: package-prefix.sh PREFIX [OUT.tar.gz]
set -eu
PREFIX=${1:?usage: package-prefix.sh PREFIX [OUT.tar.gz]}
case "$PREFIX" in /*) ;; *) PREFIX="$PWD/$PREFIX" ;; esac
[ -d "$PREFIX" ] || { echo "E: no such prefix: $PREFIX" >&2; exit 1; }
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/.." && pwd)
VER=$(cat "$ROOT/core/VERSION")
ARCH=arm64
OUT=${2:-$PWD/deb-native-prefix-$VER-$ARCH.tar.gz}

# The manifest: the runtime overlay components dn-update can replace, with
# their sha256. Base packages are never in it (never writable via dn-update).
MAN="$PREFIX/var/lib/deb-native/prefix-manifest.tsv"
mkdir -p "$(dirname "$MAN")"
: > "$MAN"
for f in \
  usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1 \
  usr/lib/aarch64-linux-gnu/libc.so.6 \
  usr/lib/deb-native/dn-shim.so \
  usr/lib/deb-native/dn-run \
  usr/lib/deb-native/dn-trace \
  usr/bin/dn-sh \
  usr/bin/dn-perl; do
  [ -e "$PREFIX/$f" ] || continue
  printf '%s\t%s\n' "$f" "$(sha256sum "$PREFIX/$f" | awk '{print $1}')" >> "$MAN"
done

tar czf "$OUT" -C "$PREFIX" .
echo "packaged: $OUT ($(du -h "$OUT" 2>/dev/null | cut -f1))"
echo "manifest: $MAN"
