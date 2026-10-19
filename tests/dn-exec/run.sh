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
"$SRC/build/dn-tiny" -t "$P" -- /usr/local/bin/dn-exec --root "$P" /usr/bin/true
echo "== dynamic exe identity"
exe=$("$SRC/build/dn-tiny" -t "$P" -- /usr/local/bin/dn-exec --root "$P" /usr/bin/readlink /proc/self/exe 2>/dev/null)
[ "$exe" = "/usr/bin/readlink" ] || { echo "exe: got '$exe'"; exit 1; }
echo "== vfork/clone"
clang -O2 -static -nostdlib -ffreestanding -fno-builtin -fno-stack-protector \
    -fno-pic -Wl,-e,_start -Wl,--build-id=none \
    -o "$OUT/vforktest" "$SRC/tests/dn-exec/vfork.c"
cp "$OUT/vforktest" "$P/usr/local/bin/vforktest"
chmod 755 "$P/usr/local/bin/vforktest"
vfork=$("$SRC/build/dn-tiny" -t "$P" -- /usr/local/bin/vforktest /usr/local/bin/dn-exec --root "$P" /usr/bin/true 10 2>/dev/null)
[ "$vfork" = "vfork-ok" ] || { echo "vfork: got '$vfork'"; exit 1; }

echo "== dynamic shim/ldhook"
maps=$("$SRC/build/dn-tiny" -t "$P" -- /usr/local/bin/dn-exec --root "$P" /usr/bin/cat /proc/self/maps 2>/dev/null)
printf '%s\n' "$maps" | grep -q "dn-lane.so" || { echo "shim not mapped"; exit 1; }
printf '%s\n' "$maps" | grep -q "dn-ldhook.so" || { echo "ldhook not mapped"; exit 1; }
echo "== dynamic find"
"$SRC/build/dn-tiny" -t "$P" -- /usr/local/bin/dn-exec --root "$P" /usr/bin/find /usr -maxdepth 1 >/dev/null
echo "OK (L0 static, L1 dynamic, L2 identity, L3 shim/ldhook, L5 clone/vfork)"
