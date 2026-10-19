#!/bin/sh
# L0/L1 checks for dn-exec.  Usage: tests/dn-exec/run.sh [PREFIX]
#
# Without PREFIX: static hello only.
# With PREFIX: copy dn-exec into it and run a dynamic true under build/dn-tiny.
set -eu

SRC="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
OUT="$SRC/build/dn-exec-test"
rm -rf "$OUT"
mkdir -p "$OUT"

"$SRC/scripts/build-dn-exec.sh" "$OUT/dn-exec" >/dev/null
clang -O2 -static -nostdlib -ffreestanding -fno-builtin -fno-stack-protector \
    -fno-pic -Wl,-e,_start -Wl,--build-id=none \
    -o "$OUT/hello" "$SRC/tests/dn-exec/hello.c"

echo "== static"
"$OUT/dn-exec" "$OUT/hello"

if [ $# -eq 0 ]; then
    echo "OK (static)"
    exit 0
fi

P="$1"
cp "$OUT/dn-exec" "$P/usr/local/bin/dn-exec"
chmod 755 "$P/usr/local/bin/dn-exec"
echo "== dynamic true"
"$SRC/build/dn-tiny" -t "$P" -- /usr/local/bin/dn-exec /usr/bin/true
echo "== dynamic find"
"$SRC/build/dn-tiny" -t "$P" -- /usr/local/bin/dn-exec /usr/bin/find /usr -maxdepth 1 >/dev/null
echo "OK (static + dynamic; /proc/self/exe still needs L2)"
