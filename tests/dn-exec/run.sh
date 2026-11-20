#!/bin/sh
# L0/L1 checks for dn-exec.  Usage: tests/dn-exec/run.sh [PREFIX]
#
# Without PREFIX: static hello only.
# With PREFIX: copy dn-exec into it and run a dynamic true under build/dn-init.
set -eu

SRC="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
OUT="$SRC/build/dn-exec-test"
rm -rf "$OUT"
mkdir -p "$OUT"

"$SRC/scripts/runtime/build-dn-exec.sh" "$OUT/dn-exec" >/dev/null
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
"$SRC/build/dn-init" -t "$P" -- /usr/local/bin/dn-exec --root "$P" /usr/bin/true
echo "== dynamic exe identity"
exe=$("$SRC/build/dn-init" -t "$P" -- /usr/local/bin/dn-exec --root "$P" /usr/bin/readlink /proc/self/exe 2>/dev/null)
[ "$exe" = "/usr/bin/readlink" ] || { echo "exe: got '$exe'"; exit 1; }
echo "== vfork/clone"
clang -O2 -static -nostdlib -ffreestanding -fno-builtin -fno-stack-protector \
    -fno-pic -Wl,-e,_start -Wl,--build-id=none \
    -o "$OUT/vforktest" "$SRC/tests/dn-exec/vfork.c"
cp "$OUT/vforktest" "$P/usr/local/bin/vforktest"
chmod 755 "$P/usr/local/bin/vforktest"
vfork=$("$SRC/build/dn-init" -t "$P" -- /usr/local/bin/vforktest /usr/local/bin/dn-exec --root "$P" /usr/bin/true 10 2>/dev/null)
[ "$vfork" = "vfork-ok" ] || { echo "vfork: got '$vfork'"; exit 1; }

echo "== dynamic shim"
maps=$("$SRC/build/dn-init" -t "$P" -- /usr/local/bin/dn-exec --root "$P" /usr/bin/cat /proc/self/maps 2>/dev/null)
printf '%s\n' "$maps" | grep -q "dn-lane.so" || { echo "shim not mapped"; exit 1; }

# The root handed down as a reserved fd (DN_ROOT_FD=200).  The shim reads that fd
# and closes it (lane.c own_root), so it cannot be observed after the program has
# started; what can be observed is dn-exec's own condition for exporting it: the
# env entry is written only after openat(O_DIRECTORY) and the dup3 both succeed.
# A wrong O_DIRECTORY constant makes the openat fail and the entry disappear.
echo "== root fd handoff"
rfd=$("$SRC/build/dn-init" -q -t "$P" -- /usr/local/bin/dn-exec --root "$P" /usr/bin/env 2>/dev/null)
printf '%s\n' "$rfd" | grep -qx "DN_ROOT_FD=200" || { echo "DN_ROOT_FD: not handed down"; exit 1; }
echo "== dynamic find"
# from the tree's own root: the shell's host cwd is not a guest path, and find
# chdirs back to it at exit ("Failed to restore initial working directory").
( cd "$P" && "$SRC/build/dn-init" -q -t "$P" -- /usr/local/bin/dn-exec --root "$P" \
    /usr/bin/find /usr -maxdepth 1 >/dev/null )
echo "OK (L0 static, L1 dynamic, L2 identity, L3 shim, L4 root fd, L5 clone/vfork)"
