#!/bin/sh
# Build dn-tiny, the freestanding tracer (src/dn-tiny), with the programs that
# test and measure it: dn-substtest and dn-census (both in src/dn-tiny).
# All three are static aarch64 executables with no libc and no imports, so the
# toolchain only has to emit aarch64 code: a cross gcc (aarch64-linux-gnu-gcc on
# a build host) or gcc itself on an arm64 machine.
#
# The rows of the policy table (which syscalls are caught, where their path
# arguments are) are generated from src/tracer's own tables by gen_rows.py.
#
# Usage: build-tiny.sh [OUTDIR]   (default: src/.build-tiny)
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT=$(CDPATH= cd -- "$HERE/../.." && pwd)
SRC="$ROOT/src/dn-tiny"
OUT=${1:-$ROOT/src/.build-tiny}
mkdir -p "$OUT"
case "$OUT" in /*) ;; *) OUT="$PWD/$OUT" ;; esac

if [ -z "${CC:-}" ]; then
  if command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then CC=aarch64-linux-gnu-gcc; else CC=gcc; fi
fi
command -v "$CC" >/dev/null 2>&1 || { echo "E: $CC not found" >&2; exit 1; }
case "$("$CC" -dumpmachine)" in
  aarch64-*) ;;
  *) echo "E: $CC targets $("$CC" -dumpmachine), not aarch64 (set CC to a cross compiler)" >&2; exit 1 ;;
esac
command -v python3 >/dev/null 2>&1 || { echo "E: python3 not found (it generates the policy rows)" >&2; exit 1; }

FLAGS="-O2 -Wall -Wextra -static -nostdlib -ffreestanding -fno-builtin -fno-stack-protector
 -fno-asynchronous-unwind-tables -fno-tree-loop-distribute-patterns -fno-pie -no-pie -Wl,-z,noexecstack"

mkdir -p "$OUT/gen"
python3 "$SRC/gen_rows.py" "$ROOT/src/tracer" "$OUT/gen/policy-rows.inc"
# shellcheck disable=SC2086
$CC $FLAGS -I"$SRC" -I"$OUT/gen" -o "$OUT/dn-tiny" \
  "$SRC/sys.c" "$SRC/monitor.c" "$SRC/trace.c" "$SRC/exec.c" "$SRC/policy.c"
# shellcheck disable=SC2086
$CC $FLAGS -o "$OUT/dn-substtest" "$SRC/substtest.c"
# shellcheck disable=SC2086
$CC $FLAGS -o "$OUT/dn-census" "$SRC/census.c"

echo "dn-tiny in $OUT:"
ls -l "$OUT/dn-tiny" "$OUT/dn-substtest" "$OUT/dn-census"
if command -v "${CC%gcc}nm" >/dev/null 2>&1; then
  n=$("${CC%gcc}nm" -u "$OUT/dn-tiny" | wc -l)
  [ "$n" -eq 0 ] || { echo "E: dn-tiny imports $n symbols" >&2; exit 1; }
fi
