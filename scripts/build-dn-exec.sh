#!/bin/sh
# Build dn-exec: a freestanding static PIE, no libc.  It links the law (law.c,
# -DDN_FREE) so it can patch the loader's - or a static program's - syscall
# sites at birth.  build-runtime.sh calls this; tests/dn-exec/run.sh calls it
# directly.  No DN_HOST_ADB: dn-exec reads the law for paths only, and the one
# host-dependent part of it (policy_deny) is the tracer's.
#
# Usage: scripts/build-dn-exec.sh [OUT]      (default: build/dn-exec)
#   CC   the compiler (default: clang)
#   GEN  the directory holding policy-rows.inc (default: build/gen, generated
#        here when it holds none); build-runtime.sh passes its own OUT/gen.
set -eu

SRC="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
OUT="${1:-$SRC/build/dn-exec}"
CC="${CC:-clang}"
GEN="${GEN:-$SRC/build/gen}"

mkdir -p "$(dirname "$OUT")" "$GEN"
if [ ! -f "$GEN/policy-rows.inc" ]; then
    python3 "$SRC/src/dn-init/gen_rows.py" "$GEN/policy-rows.inc"
fi

# shellcheck disable=SC2086
"$CC" -O2 -static-pie -nostdlib -ffreestanding -fno-builtin \
    -fno-stack-protector -fPIE -DDN_FREE -Wl,-e,_start -Wl,--build-id=none \
    -I"$SRC/src/dn-init" -I"$GEN" \
    -o "$OUT" "$SRC/src/dn-exec/dn-exec.c" "$SRC/src/dn-exec/patch.c" "$SRC/src/dn-init/law.c"
echo "built $OUT"
