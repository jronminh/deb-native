#!/bin/sh
# Build the program interpreter (native/dn-interp.c) -- the exec gate's rule-3 target.
#
# dn-interp is a freestanding aarch64 static-PIE: no libc, no PT_INTERP, no NEEDED.
# It runs as a program's interpreter, derives the prefix from its own path,
# builds the environment (delivering the shim once one exists) and jumps into
# the tree's glibc loader.  docs/spec/dn-interp-shim.md.
#
# Usage: build-loader.sh [OUTDIR]   (default: native/.build)
# (CC must be an aarch64 glibc cross compiler, e.g. aarch64-linux-gnu-gcc.)
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
SRC="$ROOT/native"
OUT=${1:-$SRC/.build}
CC=${CC:-gcc}
mkdir -p "$OUT"
case "$OUT" in /*) ;; *) OUT="$PWD/$OUT" ;; esac

command -v "$CC" >/dev/null 2>&1 || { echo "E: $CC not found" >&2; exit 1; }
case "$("$CC" -dumpmachine)" in
  *-linux-gnu) ;;
  *) echo "E: $CC targets $("$CC" -dumpmachine), not glibc" >&2; exit 1 ;;
esac

echo "Building dn-interp ..."
"$CC" -O2 -static -nostdlib -ffreestanding -fno-builtin -fno-stack-protector \
      -fPIE -Wl,-pie -Wl,--no-dynamic-linker \
      -o "$OUT/dn-interp" "$SRC/dn-interp.c"
cp -f "$SRC/dn-interp.conf" "$OUT/dn-interp.conf"

echo "loader in $OUT:"
ls -l "$OUT/dn-interp" "$OUT/dn-interp.conf"
