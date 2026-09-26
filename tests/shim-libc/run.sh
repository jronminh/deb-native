#!/bin/sh
# On-device smoke test for native/path-redirect.c (the glibc libc-level
# path shim). Run this in Termux, where clang can target the glibc
# side-install. It builds the shim + a glibc test binary, sets up a fake
# $DN_INSTDIR root, runs every libc entry point the shim intercepts against
# a path under /etc or /usr, and asserts each one was rewritten to the root.
#
# No root, no namespace: this only exercises libc interposition, the same
# way a maintainer script's forked glibc command reaches it.
set -eu

G=${DN_GLIBC_ROOT:-/data/data/com.termux/files/usr/glibc}
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO=$(CDPATH= cd -- "$HERE/../.." && pwd)
ROOT=${DN_INSTDIR:-$HOME/.cache/deb-native-shimtest/root}
SHIM=$REPO/native/path-redirect.so
TEST=$HERE/test

[ -x "$G/bin/true" ] || { echo "no glibc side-install at $G (set DN_GLIBC_ROOT)"; exit 2; }
[ -f "$SHIM" ] || sh "$REPO/scripts/build-path-redirect.sh"

rm -rf "$ROOT"
mkdir -p "$ROOT/etc" "$ROOT/usr/bin" "$ROOT/var" "$ROOT/opt"
printf 'hi\n' > "$ROOT/etc/real.txt"
# A fake account so the NSS question is answerable: if glibc's nss_files
# backend opens /etc/passwd through the interposed fopen, it sees this.
printf 'dnshim:x:54321:54321:DN Shim Test:/nonexistent:/bin/sh\n' > "$ROOT/etc/passwd"
printf 'dnshimgrp:x:54321:\n' > "$ROOT/etc/group"
cp "$G/bin/true" "$ROOT/usr/bin/zz_true"

# A standalone glibc executable needs Scrt1.o/crti.o/crtn.o explicitly (the
# glibc package ships those but no libgcc.a), hence -nostartfiles/-nodefaultlibs.
clang --target=aarch64-linux-gnu --sysroot=/ -O0 \
  -nostartfiles -nodefaultlibs \
  -I"$G/include" -L"$G/lib" \
  -Wl,-dynamic-linker,"$G/lib/ld-linux-aarch64.so.1" \
  -o "$TEST" \
  "$G/lib/Scrt1.o" "$G/lib/crti.o" "$HERE/test-shim-libc.c" "$G/lib/crtn.o" \
  -lc

OUT=$(mktemp)
trap 'rm -f "$OUT"' EXIT
DN_REDIRECT_DEBUG=1 LD_PRELOAD="$SHIM" DN_INSTDIR="$ROOT" PATH="/usr/bin" \
  "$TEST" >"$OUT" 2>&1 || { echo "test binary failed:"; cat "$OUT"; exit 1; }

fail=0
check() {
  if grep -qF "$1" "$OUT"; then :; else echo "MISSING: $1"; fail=1; fi
}

for p in \
  /etc/zz_creat /etc/zz_creat64 /etc/zz_freopen \
  /etc/zz_chown /etc/zz_lchown /etc/zz_fchownat /etc/zz_utime \
  /etc/zz_setxattr /etc/zz_lsetxattr /etc/zz_getxattr /etc/zz_lgetxattr \
  /etc/zz_listxattr /etc/zz_llistxattr /etc/zz_removexattr /etc/zz_lremovexattr \
  /etc/zz_mkfifo /etc/zz_mkfifoat /etc/zz_mknod /etc/zz_mknodat \
  /etc/zz_statfs64 /etc/zz_statvfs64 \
  /etc/zz_realpath /etc/zz_canon /etc/zz_inotify /etc/zz_sendto \
  /etc/zz_mkstempXXXXXX /etc/zz_mkostempXXXXXX /etc/zz_mkdtempXXXXXX \
  /etc/zz_xstat /etc/zz_lxstat /etc/zz_xstat64 /etc/zz_lxstat64 \
  /etc/zz_scandir /etc/zz_scandir64 /etc/zz_mtab \
  /etc/zz_eaccess /etc/zz_euidaccess /etc/zz_lutimes /etc/zz_sendmsg \
  /etc/zz_mkstempsXXXXXX.txt /etc/zz_mkostempsXXXXXX.txt \
  /usr/bin/zz_nope /usr/bin/zz_true
do
  check "[path-redirect] $p -> "
done

# mkstemp/mkostemp/mkstemps/mkostemps/mkdtemp templates must be handed back
# in the caller's (un-prefixed) form, not as $ROOT/etc/...
grep -q '^MKSTEMP_TMPL=/etc/zz_mkstemp'    "$OUT" || { echo "mkstemp template not stripped";   fail=1; }
grep -q '^MKOSTEMP_TMPL=/etc/zz_mkostemp'  "$OUT" || { echo "mkostemp template not stripped";  fail=1; }
grep -q '^MKSTEMPS_TMPL=/etc/zz_mkstemps'  "$OUT" || { echo "mkstemps template not stripped";  fail=1; }
grep -q '^MKOSTEMPS_TMPL=/etc/zz_mkostemps' "$OUT" || { echo "mkostemps template not stripped"; fail=1; }
grep -q '^MKDTEMP_TMPL=/etc/zz_mkdtemp'    "$OUT" || { echo "mkdtemp template not stripped";   fail=1; }
grep -q '^POSIX_SPAWN_REAL_EXIT=0' "$OUT" || { echo "posix_spawn of a redirected binary did not run"; fail=1; }
grep -q '^POSIX_SPAWNP_RC=-1' "$OUT" || { echo "posix_spawnp PATH walk did not return ENOENT"; fail=1; }
grep -q '^DONE' "$OUT" || { echo "test did not finish"; fail=1; }

# The fake-root files must exist; the real /etc must be untouched.
for f in zz_creat zz_creat64 zz_freopen zz_mkfifo; do
  [ -e "$ROOT/etc/$f" ] || { echo "not created under root: $f"; fail=1; }
done
set -- "$ROOT"/etc/zz_mkdtemp*
[ -d "$1" ] || { echo "not created under root: mkdtemp dir"; fail=1; }
set -- "$ROOT"/etc/zz_mkstemp*
[ -f "$1" ] || { echo "not created under root: mkstemp file"; fail=1; }
if [ -e /etc/zz_creat ]; then echo "leaked into real /etc"; fail=1; fi

# NSS probe result (see docs/shim-coverage.md): did glibc's nss_files backend
# reach the fake root's passwd/group through the interposed fopen? Reported,
# not asserted, until the mechanism is confirmed.
echo "NSS probe: $(grep -h '^NSS_' "$OUT" | tr '\n' ' ')"

if [ "$fail" -eq 0 ]; then
  echo "PASS: shim-libc ($(grep -c 'path-redirect\]' "$OUT") rewrites, fake root $ROOT)"
else
  echo "FAIL (output in $OUT)"; sed -n '1,200p' "$OUT"; exit 1
fi
