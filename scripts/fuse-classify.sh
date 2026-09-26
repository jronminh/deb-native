#!/bin/sh
# True fusion (fusion-debian-mode): refuse BEFORE dpkg ever touches
# Termux's real root if a repackaged .deb (scripts/fuse-repack.sh already
# run) would write over anything that already exists there. There is no
# prefix to discard if this gets it wrong, so this runs as a pre-flight
# check, not a rollback: nothing is unpacked until this exits 0.
#
# Same-package exception, arm64 only: a path already owned by PKG:arm64 is
# this package's own (upgrade/reinstall). This is safe only because Debian
# mode rewrites "Architecture: all" to arm64 (docs/true-fusion.md), so every
# fused Debian package is recorded as :arm64 and never as Termux's native
# side. (Before that rule, cowsay showed why there could be no exception:
# Termux's and Debian's both reported "all" and were indistinguishable.)
# A path owned by anything else, or by nobody, is always refused.
#
# Usage: fuse-classify.sh DEB_FILE
# Exit 0: clear to install. Exit 1: refused (see stderr for which path).
set -eu
DEB=${1:?usage: fuse-classify.sh DEB_FILE}
case "$DEB" in /*) ;; *) DEB="$PWD/$DEB" ;; esac
TP=${DN_TERMUX_PREFIX:-${PREFIX:-/data/data/com.termux/files/usr}}
PKG=$(dpkg-deb -f "$DEB" Package)

# The while loop below is piped from dpkg-deb -c, so it runs in a subshell
# -- a plain variable set inside it would not survive back out. Use a file
# as the flag instead.
FUSE_CLASSIFY_FLAG=$(mktemp)
trap 'rm -f "$FUSE_CLASSIFY_FLAG"' EXIT

dpkg-deb -c "$DEB" | while IFS= read -r line; do
  # dpkg-deb -c line shape: "<perm> <owner> <size> <date> <time> <path>[ -> <target>]"
  path=$(printf '%s\n' "$line" | awk '{print $6}')
  case "$path" in
    */) continue ;;   # directories: fine either way, never refuse on these
    ./*) path=${path#.} ;;
    *) continue ;;
  esac
  target="$TP$path"
  [ -e "$target" ] || [ -L "$target" ] || continue

  # dpkg records fused paths relative to --instdir ("/bin/x"), not "$TP/bin/x".
  owner=$(dpkg -S "$path" 2>/dev/null | head -1 | sed 's/: .*//')
  # The same package being upgraded or reinstalled owns its own files.
  [ "$owner" = "$PKG:arm64" ] && continue
  # A Termux package this same apt run crossgrades to Debian's (dn-hook-pre.sh
  # sets DN_REPLACING): dpkg replaces its files as part of the crossgrade.
  case " ${DN_REPLACING:-} " in *" $PKG "*) [ "$owner" = "$PKG" ] && continue ;; esac
  if [ -n "$owner" ]; then
    echo "fuse-classify: REFUSE $DEB -- $target already owned by $owner" >&2
  else
    echo "fuse-classify: REFUSE $DEB -- $target exists and isn't tracked by any package" >&2
  fi
  echo "REFUSED" >> "$FUSE_CLASSIFY_FLAG"
done

if [ -s "$FUSE_CLASSIFY_FLAG" ]; then
  exit 1
fi
echo "fuse-classify: clear -- $PKG collides with nothing that already exists"
