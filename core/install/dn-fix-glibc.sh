#!/bin/sh
# Re-apply the patched glibc files (the "10-file swap") after apt may have
# restored the stock ones. The patched copies live in the prefix's stash
# ($DN/usr/lib/deb-native/glibc-swap/); this restores any canonical path whose
# content differs from the stash, atomically. Idempotent; wired into the apt
# Post-Invoke so a `libc6`/`libc-bin` reinstall cannot silently leave the
# prefix on stock glibc (which is killed by Android at startup).
#
# The stash is the authority; the canonical paths are the live copies.
#
# Usage: dn-fix-glibc.sh PREFIX
set -eu
DN=${1:?usage: dn-fix-glibc.sh PREFIX}
case "$DN" in /*) ;; *) DN="$PWD/$DN" ;; esac
STASH="$DN/usr/lib/deb-native/glibc-swap"
[ -d "$STASH" ] || exit 0

fixed=0
for rel in \
  usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1 \
  usr/lib/aarch64-linux-gnu/libc.so.6 \
  usr/lib/aarch64-linux-gnu/libnsl.so.1 \
  usr/lib/aarch64-linux-gnu/libnss_compat.so.2 \
  usr/lib/aarch64-linux-gnu/libnss_hesiod.so.2 \
  usr/lib/aarch64-linux-gnu/libresolv.so.2 \
  usr/lib/aarch64-linux-gnu/librt.so.1 \
  usr/sbin/ldconfig \
  usr/bin/iconv \
  usr/bin/localedef; do
  src="$STASH/$rel"; dst="$DN/$rel"
  [ -e "$src" ] || continue
  if [ -e "$dst" ] && cmp -s "$src" "$dst"; then continue; fi
  mkdir -p "$(dirname "$dst")"
  cp -f "$src" "$dst.tmp.$$" && chmod 755 "$dst.tmp.$$" && mv -f "$dst.tmp.$$" "$dst"
  fixed=$((fixed + 1))
done

[ "$fixed" -gt 0 ] && echo "dn-fix-glibc: restored $fixed patched glibc file(s)" >&2
exit 0
