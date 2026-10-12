#!/bin/sh
# Build the fast-path shim (native/dn-shim.c) as dn-shim.so.
#
# dn-shim.so is an LD_PRELOAD shared object linked with dn-policy.  dn-interp
# delivers it (dn-interp.conf's `preload`); it translates each path/identity call
# through dn-policy and issues the syscall from the fixed gate page.  See
# docs/spec/dn-interp-shim.md; P2a is the open family.
#
# Usage: build-shim.sh [OUTDIR]   (default: native/.build)
# (CC must be an aarch64 glibc cross compiler, e.g. aarch64-linux-gnu-gcc.)
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
SRC="$ROOT/native"
POL="$ROOT/src/dn-policy"
OUT=${1:-$SRC/.build}
CC=${CC:-gcc}
mkdir -p "$OUT"
case "$OUT" in /*) ;; *) OUT="$PWD/$OUT" ;; esac

command -v "$CC" >/dev/null 2>&1 || { echo "E: $CC not found" >&2; exit 1; }
case "$("$CC" -dumpmachine)" in
  *-linux-gnu) ;;
  *) echo "E: $CC targets $("$CC" -dumpmachine), not glibc" >&2; exit 1 ;;
esac

echo "Building dn-shim.so ..."
"$CC" -O2 -fPIC -shared -Wall -Wextra -I"$POL" \
      -o "$OUT/dn-shim.so" \
      "$SRC/dn-shim.c" \
      "$POL/dn-policy.c" "$POL/dn-policy-fakeroot.c" "$POL/dn-policy-hardlink.c"

echo "shim in $OUT:"
ls -l "$OUT/dn-shim.so"
