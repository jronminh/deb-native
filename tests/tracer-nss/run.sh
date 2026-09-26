#!/bin/sh
# Prove the tracer route resolves glibc's statically-bound NSS reads against
# the prefix.  NSS is case 2 of docs/syscall-boundary.md: the LD_PRELOAD shim
# cannot see the open inside libc, so the syscall tracer (dn-trace) is the fix.
# Termux glibc reads its sysconfdir $PREFIX/glibc/etc (a host path), so dn-run
# adds a bind of the prefix's /etc over it on the NSS route.
#
# Run on the device (Termux), after scripts/setup-runtime.sh INSTDIR.
set -eu

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
INSTDIR=${DN_INSTDIR:-$HOME/dn6/root}
DNRUN=$INSTDIR/usr/lib/deb-native/dn-run
DNTRACE=${DN_TRACE:-$INSTDIR/usr/lib/deb-native/dn-trace}
[ -x "$DNRUN" ] || { echo "run setup-runtime.sh first (no dn-run at $DNRUN)"; exit 2; }

# A throwaway guest /etc so the test never touches the real prefix.  dn-run
# takes INSTDIR from DN_INSTDIR and the tracer path from DN_TRACE.
TMP=$(mktemp -d)
OUT=$(mktemp)
trap 'rm -rf "$TMP" "$OUT"' EXIT
mkdir -p "$TMP/etc"
printf 'dnshim:x:70000:70000::/home/dnshim:/bin/sh\n' > "$TMP/etc/passwd"
printf 'passwd: files\ngroup: files\n' > "$TMP/etc/nsswitch.conf"

# Build the probe against the Termux glibc side-install.
G=${DN_GLIBC_ROOT:-${PREFIX:-/data/data/com.termux/files/usr}/glibc}
PROBE=$HERE/nss-probe
clang --target=aarch64-linux-gnu --sysroot=/ -O0 -nostartfiles -nodefaultlibs \
  -I"$G/include" -L"$G/lib" \
  -Wl,-dynamic-linker,"$G/lib/ld-linux-aarch64.so.1" \
  -o "$PROBE" \
  "$G/lib/Scrt1.o" "$G/lib/crti.o" "$HERE/nss-probe.c" "$G/lib/crtn.o" -lc

DN_INSTDIR="$TMP" DN_TRACE="$DNTRACE" "$DNRUN" "$PROBE" >"$OUT" 2>&1 || true
cat "$OUT"

fail=0
grep -q '^open_dnshim=1$'          "$OUT" || { echo "MISSING: glibc open did not read the prefix passwd"; fail=1; }
grep -q '^getpwnam_dnshim=dnshim$' "$OUT" || { echo "MISSING: getpwnam did not resolve via the tracer"; fail=1; }

if [ "$fail" -eq 0 ]; then
  echo "PASS: tracer-nss"
else
  echo "FAIL: tracer-nss"
  exit 1
fi
