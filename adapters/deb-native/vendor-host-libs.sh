#!/bin/sh
# Vendor the host (Termux) libraries a prefix's Bionic runtime binaries need,
# and retarget their rpath at the prefix, so the *runtime* never opens
# anything under Termux's tree. Target-specific (adapter; MODULARIZE.md P2);
# run after install-runtime.sh has placed the binaries.
#
# Usage: vendor-host-libs.sh INSTDIR
set -eu
INSTDIR=${1:?usage: vendor-host-libs.sh INSTDIR}
case "$INSTDIR" in /*) ;; *) INSTDIR="$PWD/$INSTDIR" ;; esac
PREFIX_DIR=${DN_TERMUX_PREFIX:-${PREFIX:-/data/data/com.termux/files/usr}}
BINDIR="$INSTDIR/usr/bin"
LIBDIR="$INSTDIR/usr/lib/deb-native"

# Bundle the Bionic host-layer libraries into the prefix so the *runtime*
# never opens anything under Termux's tree (0.7.0's independence goal, R7
# follow-up). dn-run/dn-trace/dn-shell/dn-perl are Bionic ELFs built by
# Termux's clang, so their linker rpath points at $PREFIX_DIR/lib and
# dn-trace NEEDs libtalloc.so.2 from there -- a hard-coded path no
# DN_TERMUX_PREFIX override reaches. Copy those libs into the prefix and
# retarget the rpath at $ORIGIN. Bootstrap still borrows Termux to build;
# only the steady state is Termux-independent.
HOST="$LIBDIR/host"
mkdir -p "$HOST"
have_talloc=$(ls "$PREFIX_DIR"/lib/libtalloc.so* 2>/dev/null | head -n1 || true)
have_termux_exec=$(ls "$PREFIX_DIR"/lib/libtermux-exec-ld-preload.so 2>/dev/null | head -n1 || true)
# libtalloc: dn-trace's only non-system NEEDED (libc/libdl are Android's).
if [ -n "$have_talloc" ]; then
  cp -Lf "$have_talloc" "$HOST/libtalloc.so.2" \
    || echo "W: could not vendor libtalloc into the prefix; dn-trace keeps needing $PREFIX_DIR/lib"
fi
# termux-exec: the Bionic preload the shim hands to a Bionic child
# (DN_BIONIC_PRELOAD). Vendored so that child needs no Termux tree either.
if [ -n "$have_termux_exec" ]; then
  cp -f "$have_termux_exec" "$HOST/libtermux-exec-ld-preload.so" \
    || echo "W: could not vendor termux-exec into the prefix; Bionic children keep needing it from Termux"
fi
# adbwire links Termux's OpenSSL (libssl/libcrypto); vendor both so the
# client is self-contained too.
for l in libssl.so.3 libcrypto.so.3; do
  if [ -f "$PREFIX_DIR/lib/$l" ]; then
    cp -f "$PREFIX_DIR/lib/$l" "$HOST/$l" \
      || echo "W: could not vendor $l into the prefix; adbwire keeps needing it from Termux"
  fi
done
# Retarget the rpath at the prefix's own host dir (via $ORIGIN, so the
# prefix stays relocatable). patchelf is a bootstrap requirement (README);
# if it is missing, say so rather than leave the Termux path silently.
if command -v patchelf >/dev/null 2>&1; then
  for f in "$LIBDIR/dn-run" "$LIBDIR/dn-trace" "$LIBDIR/adbwire"; do
    if [ -f "$f" ]; then
      patchelf --set-rpath '$ORIGIN/host' "$f" \
        || echo "W: patchelf could not retarget $f; it keeps its Termux rpath"
    fi
  done
  for f in "$BINDIR/dn-shell" "$BINDIR/dn-perl"; do
    if [ -f "$f" ]; then
      patchelf --set-rpath '$ORIGIN/../lib/deb-native/host' "$f" \
        || echo "W: patchelf could not retarget $f; it keeps its Termux rpath"
    fi
  done
else
  echo "W: patchelf not found; the Bionic host binaries keep their Termux rpath"
fi
