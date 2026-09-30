#!/bin/sh
# Prefix-specific changes to Debian's base-files (docs/spec/design-0.2.0.md),
# run by dn-translate-deb.sh on the extracted package before dpkg sees it.
#
# The prefix keeps only what it uses: usr/, etc/, var/, opt/ (the paths the
# shim maps), Debian's merged-/usr links bin, lib, sbin, and deb-native's
# own root -> Termux's home. base-files would add a booted system's
# directories -- mount points and kernel/runtime filesystems that stay empty
# here and that nothing maps to:
#   - shipped:  boot dev home proc root run sys tmp  -> removed from the package
#               (root: deb-native's link to Termux's home stays untouched)
#   - postinst: mnt srv media, run/lock, and var/run, var/lock turned into
#               links to /run                        -> skipped (var/run and
#               var/lock stay plain directories, as in 0.1.x)
#   - postinst: .profile and .bashrc copied into /root, i.e. Termux's home
#               (and .profile refreshed there on upgrades) -> skipped
#
# Usage: base-files.sh PACKAGE_TREE PREFIX
set -eu
T=${1:?usage: base-files.sh PACKAGE_TREE PREFIX}

for d in boot dev home proc root run sys tmp; do
  rm -rf "${T:?}/$d"
done

P="$T/DEBIAN/postinst"
sed -i \
  -e 's#^\(  *\)install_from_default dot\.profile .*#\1: \# deb-native: no .profile into /root (Termux home)#' \
  -e 's#^\(  *\)install_from_default dot\.bashrc .*#\1: \# deb-native: no .bashrc into /root (Termux home)#' \
  -e 's#^\(  *\)update_to_current_default dot\.profile .*#\1: \# deb-native: no .profile update in /root (Termux home)#' \
  -e 's#^\(  *\)install_directory \(mnt\|srv\|media\|run/lock\) .*#\1: \# deb-native: no /\2 in the prefix#' \
  -e 's#^\(  *\)migrate_directory \(/var/run\|/var/lock\) .*#\1: \# deb-native: \2 stays a directory (no /run)#' \
  "$P"

# Refuse loudly if a newer base-files changed these lines.
for want in "no .profile into" "no .bashrc" "no .profile update" "no /mnt" "no /srv" "no /media" "no /run/lock" "/var/run stays" "/var/lock stays"; do
  grep -q "deb-native: $want" "$P" \
    || { echo "E: custom/base-files.sh: postinst changed upstream (\"$want\" not applied)" >&2; exit 1; }
done
