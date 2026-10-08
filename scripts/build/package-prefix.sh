#!/bin/sh
# Package a built prefix into the shipped artifact (the repo layout "Build vs
# Ship", docs/spec/prefix.md). Build-stage only: it reads a prefix
# built elsewhere and writes a tarball; it never touches a target.
#
# The prefix is staged in a scratch copy, the build invariants are applied and
# the .dn/ contract is written (build/pack-prefix.py), then the copy is tarred.
# A manifest of the overlay components (path -> sha256) is written into the
# prefix for dn-update, as before.
#
# Usage: package-prefix.sh PREFIX --root BUILD_PATH --name NAME [--desc TEXT]
#                                 [--out OUT.tar.gz]
#   BUILD_PATH is the absolute path the prefix's own files name (its loader's
#   path), e.g. /data/data/org.dn.shell/files for the APK's core-deb.
set -eu
PREFIX=${1:?usage: package-prefix.sh PREFIX --root BUILD_PATH --name NAME [--desc TEXT] [--out OUT.tar.gz]}
shift
ROOTP= NAME= DESC= OUT=
while [ $# -gt 0 ]; do
  case $1 in
    --root) ROOTP=$2; shift 2 ;;
    --name) NAME=$2; shift 2 ;;
    --desc) DESC=$2; shift 2 ;;
    --out) OUT=$2; shift 2 ;;
    *) echo "E: unknown argument: $1" >&2; exit 64 ;;
  esac
done
[ -n "$ROOTP" ] && [ -n "$NAME" ] || { echo "E: --root and --name are required" >&2; exit 64; }
case "$PREFIX" in /*) ;; *) PREFIX="$PWD/$PREFIX" ;; esac
[ -d "$PREFIX" ] || { echo "E: no such prefix: $PREFIX" >&2; exit 1; }
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
VER=$(cat "$ROOT/VERSION")
[ -n "$OUT" ] || OUT=$PWD/$NAME-$VER-arm64.tar.gz

# The overlay manifest (dn-update), written into the built prefix as before.
MAN="$PREFIX/var/lib/deb-native/prefix-manifest.tsv"
mkdir -p "$(dirname "$MAN")"
: > "$MAN"
for f in \
  usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1 \
  usr/lib/aarch64-linux-gnu/libc.so.6 \
  usr/lib/deb-native/dn-trace \
  ; do
  [ -e "$PREFIX/$f" ] || continue
  printf '%s\t%s\n' "$f" "$(sha256sum "$PREFIX/$f" | awk '{print $1}')" >> "$MAN"
done

# Stage: the build tree is left untouched; the artifact is made from a copy.
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
cp -a "$PREFIX/." "$STAGE/"
set -- "$STAGE" --root "$ROOTP" --name "$NAME"
[ -z "$DESC" ] || set -- "$@" --desc "$DESC"
set -- "$@" --version "$VER"
python3 "$ROOT/scripts/build/pack-prefix.py" "$@"
# Store hard-linked files as separate copies: a poor host's tar (toybox, and
# GNU tar too on Android/f2fs) cannot recreate hard links -- perl's
# usr/bin/perl5.40.1 -> usr/bin/perl made "extract failed" on device.
tar --hard-dereference -czf "$OUT" -C "$STAGE" .
echo "packaged: $OUT ($(du -h "$OUT" | cut -f1))"
