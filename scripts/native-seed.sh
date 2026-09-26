#!/bin/sh
# Seed a dpkg admindir's status file with stub entries for Debian
# dependencies already satisfied natively by one of Termux's own
# termux-pacman/glibc-packages (*-glibc), so dpkg's dependency resolver
# accepts a real .deb without duplicating that dependency's files into
# our own prefix — see docs/design.md.
#
# Usage: native-seed.sh $ADMINDIR
#
# This does NOT copy any files. It only appends synthetic "Status: install
# ok installed" stanzas to $ADMINDIR/status, so dpkg treats each mapped
# Debian package name as already present; the actual library stays wherever
# Termux's own package put it ($PREFIX/glibc/lib/...), found normally by
# the glibc dynamic linker's own default search path.

set -eu
ADMINDIR=${1:?usage: native-seed.sh ADMINDIR}
case "$ADMINDIR" in /*) ;; *) ADMINDIR="$PWD/$ADMINDIR" ;; esac
GLIBC_ROOT=${DN_GLIBC_ROOT:-/data/data/com.termux/files/usr/glibc}
mkdir -p "$ADMINDIR"

# Debian package name : Termux -glibc package (for Version:/Source: only —
# not used to locate files, dpkg never looks at the "files list" for
# these since they're marked as having none, see dpkg-wrapper.sh's own
# handling of exactly this "no files list" case).
#
# Known gap (see docs/design.md "Open problem"): this is a
# hand-maintained name mapping, not derived from anything. Debian and
# Termux name shared-library packages differently on purpose (Debian
# splits per soname+ABI, Termux doesn't) — a real implementation needs to
# match on the .so files' actual sonames, not guess by package name.
map='
libc6:glibc
zlib1g:zlib-glibc
libssl3:openssl-glibc
libssl3t64:openssl-glibc
libncurses6:ncurses-glibc
libtinfo6:ncurses-glibc
libreadline8:readline-glibc
libbz2-1.0:libbz2-glibc
liblzma5:liblzma-glibc
libzstd1:zstd-glibc
'

status="$ADMINDIR/status"
: > "$status.native-seed"
# Absolute path: a bare "dpkg" can resolve to a deb-native prefix's own
# arch-aware wrapper on PATH instead of Termux's real dpkg.
TERMUX_PREFIX=${DN_TERMUX_PREFIX:-${PREFIX:-/data/data/com.termux/files/usr}}
echo "$map" | while IFS=: read -r deb_name termux_pkg; do
  [ -n "$deb_name" ] || continue
  termux_version=$("$TERMUX_PREFIX/bin/dpkg" -s "$termux_pkg" 2>/dev/null | awk -F': ' '/^Version:/{print $2; exit}')
  [ -n "$termux_version" ] || { echo "skip $deb_name: $termux_pkg not installed" >&2; continue; }
  # Epoch 9999: deliberately, not a guess at Debian's real epoch for this
  # library. dpkg compares epoch before anything else, and Debian's own
  # epochs vary and drift over time (zlib1g is "1:", others "0" — found the
  # hard way: seeding "Version: 1.3.2" against a real "Depends: zlib1g
  # (>= 1:1.1.4)" made dpkg read 1.3.2 as *older* than 1:1.1.4, since epoch
  # 0 < epoch 1 regardless of the rest). The claim this stub makes is
  # "functionally satisfied by $termux_pkg", not "this exact version" — an
  # artificially high epoch is the honest way to always win a >= comparison
  # for a claim that isn't a real version number in the first place.
  cat >> "$status.native-seed" <<EOF
Package: $deb_name
Status: install ok installed
Priority: optional
Section: libs
Installed-Size: 1
Maintainer: deb-native native-seed (satisfied by $termux_pkg, not duplicated)
Architecture: arm64
Multi-Arch: same
Version: 9999:$termux_version
Description: stub — provided natively by Termux's $termux_pkg
 ($GLIBC_ROOT), no files installed in this prefix.

EOF
  echo "seeded: $deb_name <- $termux_pkg $termux_version"
done

cat "$status.native-seed" >> "$status" 2>/dev/null || true
rm -f "$status.native-seed"
