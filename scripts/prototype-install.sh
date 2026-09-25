#!/bin/sh
# Prototype: install one real Debian arm64 .deb into a prefix separate from
# Termux's own $PREFIX, using stock dpkg relocation flags (no fork/patch —
# see docs/design-install-path.md), then auto-patch any new ELF binaries to
# run against Termux's own glibc side-install via `grun --configure`
# (docs/design-static-wrappers.md's Direction 2, ELF-patch case).
#
# Known-working, tested by hand against hello_2.10-5_arm64.deb from
# deb.debian.org (docs/findings-prototype-2026-09-25.md has the full log).
#
# Usage: prototype-install.sh /path/to/package.deb
#
# NOT for real use yet: --force-depends below bypasses dependency
# checking entirely (see "Open problem" in the findings doc) — a stopgap,
# not the two-layer-db design.

set -eu

DEB=${1:?usage: prototype-install.sh package.deb}
PREFIX=${TDB_PREFIX:-$HOME/.termux-deb-bridge}
INSTDIR="$PREFIX/root"
ADMINDIR="$PREFIX/var/lib/dpkg"

mkdir -p "$INSTDIR" "$ADMINDIR"

pkg_name=$(dpkg-deb -f "$DEB" Package)

echo "==> unpacking $pkg_name into $INSTDIR"
# --unpack, not -i: -i also configures in the same step, which fails on the
# dependency check before we get a chance to force past it below.
dpkg --instdir="$INSTDIR" --admindir="$ADMINDIR" \
     --force-not-root --force-script-chrootless --force-architecture \
     --unpack "$DEB"

echo "==> configuring $pkg_name (dependency check bypassed — prototype only)"
dpkg --instdir="$INSTDIR" --admindir="$ADMINDIR" \
     --force-not-root --force-script-chrootless --force-architecture \
     --force-depends --configure "$pkg_name"

echo "==> patching new ELF binaries with grun --configure"
dpkg --admindir="$ADMINDIR" -L "$pkg_name" | while IFS= read -r f; do
  full="$INSTDIR$f"
  [ -f "$full" ] || continue
  case "$(head -c4 "$full" 2>/dev/null | od -An -tx1 | tr -d ' \n')" in
    7f454c46) grun --configure "$full" >/dev/null 2>&1 && echo "   patched: $f" ;;
  esac
done

echo "==> done. Binaries are under $INSTDIR"
