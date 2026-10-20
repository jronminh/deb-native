#!/bin/sh
# Build the dn-exec POC: a freestanding static PIE, no libc.  It links the law
# (policy.c, -DDN_FREE) so it can patch the loader's syscall sites at birth.
set -eu

SRC="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
OUT="${1:-$SRC/build/dn-exec}"
CC="${CC:-clang}"

GEN="$SRC/build/gen"
mkdir -p "$(dirname "$OUT")" "$GEN"
if [ ! -f "$GEN/policy-rows.inc" ]; then
    python3 "$SRC/src/dn-tiny/gen_rows.py" "$GEN/policy-rows.inc"
fi

"$CC" -O2 -static-pie -nostdlib -ffreestanding -fno-builtin \
    -fno-stack-protector -fPIE -DDN_FREE -Wl,-e,_start -Wl,--build-id=none \
    -I"$SRC/src/dn-tiny" -I"$GEN" \
    -o "$OUT" "$SRC/src/dn-exec/dn-exec.c" "$SRC/src/dn-exec/patch.c" "$SRC/src/dn-tiny/policy.c"
echo "built $OUT"
