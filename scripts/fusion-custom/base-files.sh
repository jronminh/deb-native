#!/bin/sh
# Tier-2 customization of base-files for the fused prefix
# (docs/true-fusion.md, "Package tiers"). Called by fuse-repack.sh with the
# extracted package tree.
#
# - preinst refuses unless bin, lib, sbin, ... are symlinks to usr/X. The
#   flat prefix is merged-/usr the other way round: $PREFIX/usr -> ., so
#   bin/ IS usr/bin. Skip the check when that holds.
# - postinst turns var/run and var/lock into links to /run and /run/lock
#   with a bare rmdir and an absolute link. Termux's var/run holds files
#   (openssh), and an absolute link would resolve against Android's root:
#   move the contents over and link relatively (both live one level down).
set -eu
T=${1:?usage: base-files.sh PKGTREE}
sed -i '0,/^set -e$/s//set -e\
# deb-native fusion: merged-\/usr by construction ($DPKG_ROOT\/usr -> .)\
if [ "$DPKG_ROOT\/usr" -ef "$DPKG_ROOT" ]; then exit 0; fi/' "$T/DEBIAN/preinst"
sed -i 's|^    rmdir "$DPKG_ROOT$1"$|    mkdir -p "$DPKG_ROOT$2"; for e in "$DPKG_ROOT$1"/* "$DPKG_ROOT$1"/.[!.]*; do [ -e "$e" ] \&\& mv "$e" "$DPKG_ROOT$2"/; done; rmdir "$DPKG_ROOT$1"|; s|^    ln -s "$2" "$DPKG_ROOT$1"$|    ln -s "..$2" "$DPKG_ROOT$1"|' "$T/DEBIAN/postinst"
grep -q 'merged-/usr by construction' "$T/DEBIAN/preinst"
grep -q 'ln -s "..$2"' "$T/DEBIAN/postinst"
