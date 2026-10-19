#!/bin/sh
# Build the dn-exec POC: a freestanding static PIE, no libc.
set -eu

SRC="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
OUT="${1:-$SRC/build/dn-exec}"
CC="${CC:-clang}"

mkdir -p "$(dirname "$OUT")"
"$CC" -O2 -static-pie -nostdlib -ffreestanding -fno-builtin \
    -fno-stack-protector -fPIE -Wl,-e,_start -Wl,--build-id=none \
    -o "$OUT" "$SRC/src/dn-exec/dn-exec.c"
echo "built $OUT"
