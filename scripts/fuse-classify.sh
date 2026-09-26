#!/bin/sh
# Branch experiment (fusion-no-prefix): refuse BEFORE dpkg ever touches
# Termux's real root if a repackaged .deb (scripts/fuse-repack.sh already
# run) would write over anything that already exists there. There is no
# prefix to discard if this gets it wrong, so this runs as a pre-flight
# check, not a rollback: nothing is unpacked until this exits 0.
#
# Deliberately has NO "same package, safe to reinstall" exception: dpkg
# has no reliable way to tell Termux's own build of a name apart from
# Debian's for an `Architecture: all` package (found the hard way testing
# with cowsay -- both Termux's and Debian's report Architecture: all, so
# name+arch matching alone can't distinguish them; dpkg's model has no
# concept of "which repo/origin" a package came from). Refusing on ANY
# existing path, regardless of who dpkg thinks owns it, is the only
# answer that's safe by construction. This means no reinstall/upgrade
# support yet for a package already fused in -- acceptable for a
# proof-of-concept, not for real use.
#
# Usage: fuse-classify.sh DEB_FILE
# Exit 0: clear to install. Exit 1: refused (see stderr for which path).
set -eu
DEB=${1:?usage: fuse-classify.sh DEB_FILE}
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

  owner=$(dpkg -S "$target" 2>/dev/null | awk -F: '{print $1; exit}')
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
