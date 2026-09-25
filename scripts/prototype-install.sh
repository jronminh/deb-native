#!/bin/sh
# Prototype: install one real Debian arm64 .deb into a prefix separate from
# Termux's own $PREFIX, using stock dpkg relocation flags (no fork/patch —
# see docs/design-install-path.md), then auto-patch any new ELF binaries to
# run against Termux's own glibc side-install via `grun --configure`
# (docs/design-static-wrappers.md's Direction 2, ELF-patch case).
#
# Dependencies Termux already provides natively (glibc itself, zlib,
# openssl, ...) are NOT reinstalled into the prefix: native-seed.sh marks
# them satisfied in $ADMINDIR/status first (docs/design-native-deps.md),
# pointing dpkg's resolver at what Termux already has instead of
# duplicating it. Only what's genuinely missing lands under $INSTDIR —
# the sudo-less-style single collection point for the delta, not everything.
#
# Tested against hello_2.10-5_arm64.deb (no deps beyond libc6) and
# ciso_1.0.2-2+b1_arm64.deb (libc6 + zlib1g, resolved natively, zero files
# duplicated — confirmed with the glibc ld.so's own --list that the
# resulting binary loads Termux's libz.so.1 directly). See
# docs/findings-prototype-2026-09-25.md and docs/design-native-deps.md.
#
# Usage: prototype-install.sh /path/to/package.deb

set -eu

DEB=${1:?usage: prototype-install.sh package.deb}
PREFIX=${DN_PREFIX:-$HOME/.deb-native}
INSTDIR="$PREFIX/root"
ADMINDIR="$PREFIX/var/lib/dpkg"
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

mkdir -p "$INSTDIR" "$ADMINDIR"

echo "==> seeding natively-satisfied dependencies into $ADMINDIR"
"$HERE/native-seed.sh" "$ADMINDIR"

pkg_name=$(dpkg-deb -f "$DEB" Package)

echo "==> unpacking $pkg_name into $INSTDIR"
# --unpack, not -i: -i also configures in the same step, which would fail
# the dependency check before native-seed.sh's stubs are in place if this
# were ever reordered.
dpkg --instdir="$INSTDIR" --admindir="$ADMINDIR" \
     --force-not-root --force-script-chrootless --force-architecture \
     --unpack "$DEB"

echo "==> patching maintainer scripts' hardcoded absolute paths"
"$HERE/patch-maintainer-scripts.sh" "$ADMINDIR" "$INSTDIR"

echo "==> configuring $pkg_name"
# No --force-depends: a real, still-missing dependency should still fail
# here rather than silently "install" broken. If this fails, either the
# package needs a dependency native-seed.sh doesn't know about yet (add a
# mapping) or it's genuinely missing and needs installing for real —
# a real dependency resolver (apt, not bare dpkg) is the eventual answer,
# not more manual seeding; see docs/design-native-deps.md "Open problem".
dpkg --instdir="$INSTDIR" --admindir="$ADMINDIR" \
     --force-not-root --force-script-chrootless --force-architecture \
     --configure "$pkg_name"

echo "==> patching new ELF binaries with grun --configure"
dpkg --admindir="$ADMINDIR" -L "$pkg_name" | while IFS= read -r f; do
  full="$INSTDIR$f"
  [ -f "$full" ] || continue
  case "$(head -c4 "$full" 2>/dev/null | od -An -tx1 | tr -d ' \n')" in
    7f454c46) grun --configure "$full" >/dev/null 2>&1 && echo "   patched: $f" ;;
  esac
done

echo "==> done. Binaries are under $INSTDIR"
