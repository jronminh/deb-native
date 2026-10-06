#!/bin/sh
# Build the runtime overlay with a plain glibc toolchain (MODULARIZE.md, B2):
# dn-shim.so, dn-run and dn-trace, all glibc programs linked against the
# glibc they will run on. Needs gcc, make and libtalloc's headers
# (libtalloc-dev) -- an arm64 Debian userland: a deb-native prefix with the
# toolchain installed, or a Debian arm64 host such as CI. No Bionic
# toolchain, no Termux.
#
# This is the overlay the shipped artifacts carry (docs/spec/prefix-layers.md).
# It is the only overlay builder: the in-place Bionic runtime that used to sit
# beside it (build-core.sh) is retired with the in-place bootstrap.
#
# dn-trace is built in a scratch copy of src/tracer, so its objects never
# mix with the in-tree build.
#
# Usage: build-overlay-glibc.sh [OUTDIR]   (default: src/.build-glibc)
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
SRC="$ROOT/src"
OUT=${1:-$SRC/.build-glibc}
CC=${CC:-gcc}
mkdir -p "$OUT"
case "$OUT" in /*) ;; *) OUT="$PWD/$OUT" ;; esac

for t in "$CC" make; do
  command -v "$t" >/dev/null 2>&1 || { echo "E: $t not found (a glibc toolchain is required)" >&2; exit 1; }
done
printf '#include <talloc.h>\n' | "$CC" -E -x c - >/dev/null 2>&1 \
  || { echo "E: talloc.h not found (install libtalloc-dev)" >&2; exit 1; }
# A Bionic or cross compiler here would silently produce the wrong kind of
# binary: insist on a glibc target.
case "$("$CC" -dumpmachine)" in
  *-linux-gnu) ;;
  *) echo "E: $CC targets $("$CC" -dumpmachine), not glibc" >&2; exit 1 ;;
esac

echo "Building dn-shim.so ..."
"$CC" -O2 -Wall -fPIC -shared -o "$OUT/dn-shim.so" "$SRC/dn-shim.c" -ldl

echo "Building dn-run ..."
"$CC" -O2 -Wall -o "$OUT/dn-run" "$SRC/dn-run.c"

echo "Building dn-trace ..."
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
cp -R "$ROOT/src/tracer/." "$T/"
find "$T" \( -name '*.o' -o -name '*.d' -o -name dn-trace \) -type f -exec rm -f {} +
make -s -C "$T" CC="$CC"
cp -f "$T/dn-trace" "$OUT/dn-trace"

echo "Building dn-elf ..."
"$CC" -O2 -Wall -Wextra -o "$OUT/dn-elf" "$SRC/dn-elf.c"

echo "overlay (glibc) in $OUT:"
ls -l "$OUT"
