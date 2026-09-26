#!/bin/sh
# Tier-2 customization of base-files for Debian mode's flat prefix
# (docs/debian-mode.md, "Package tiers"). Called by fuse-repack.sh with the
# extracted package tree.
#
# - preinst refuses unless bin, lib, sbin, ... are symlinks to usr/X. The
#   flat prefix is merged-/usr by construction instead: $PREFIX/usr -> .,
#   so bin/ IS usr/bin. Skip the check when that holds.
# - postinst turns var/run and var/lock into links to /run: var/run is
#   Termux's own directory (it holds files), so leave Termux's layout alone.
set -eu
T=${1:?usage: base-files.sh PKGTREE}
sed -i '0,/^set -e$/s//set -e\
# deb-native Debian mode: merged-\/usr by construction ($DPKG_ROOT\/usr -> .)\
if [ "$DPKG_ROOT\/usr" -ef "$DPKG_ROOT" ]; then exit 0; fi/' "$T/DEBIAN/preinst"
sed -i 's|^  migrate_directory |  : deb-native: Termux owns var/run, var/lock -- migrate_directory |' "$T/DEBIAN/postinst"
grep -q 'merged-/usr by construction' "$T/DEBIAN/preinst"
