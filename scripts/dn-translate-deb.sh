#!/bin/sh
# Translate one Debian .deb for the prefix, in place, before dpkg sees it
# (docs/design-0.2.0.md; naibed's fuse-repack.sh, minus the flat-layout
# steps -- the prefix is a real, nested Debian root):
#
#   - "Architecture: all" -> "arm64", the same rule dn-debian-index.sh
#     applied to the index apt planned from, so apt and dpkg agree;
#   - custom/<package>.sh, if any: per-package fixes;
#   - glibc programs' interpreter set to ld-dn ($DN/usr/lib/deb-native/ld-dn,
#     native/ld-dn.c): the kernel runs it first however the program is
#     started, and it sets up the shim and hands over to glibc's real
#     loader, the libc6 stand-in's; $DN/usr/lib/aarch64-linux-gnu and
#     $DN/usr/lib first in every dynamic ELF's RUNPATH, and its absolute
#     entries moved into $DN (shared libraries too: RUNPATH is not
#     inherited, and Termux's ld.so searches only $PREFIX/glibc/lib by
#     itself). Done here, not after install, so it is right before any
#     maintainer script runs the binary;
#   - program scripts' "#!" line pointed into the prefix (sh/bash/dash ->
#     dn-shell, perl -> dn-perl, any other /usr, /bin, /sbin interpreter ->
#     the same path under $DN), so a script started directly -- from
#     Termux's side too -- runs with the prefix's interpreter;
#   - maintainer-script shebangs -> dn-shell (patch-scripts-tree.sh, the
#     loop patch-deb.sh runs).
#
# One unpack and one repack per package, uncompressed (-Znone): the result
# only lives in a temp or cache folder until dpkg installs it, and xz on a
# phone was a large share of the bootstrap.
#
# Idempotent. Usage: dn-translate-deb.sh DEB_FILE PREFIX
set -eu
umask 022
DEB=${1:?usage: dn-translate-deb.sh DEB_FILE PREFIX}
DN=${2:?usage: dn-translate-deb.sh DEB_FILE PREFIX}
case "$DEB" in /*) ;; *) DEB="$PWD/$DEB" ;; esac
case "$DN" in /*) ;; *) DN="$PWD/$DN" ;; esac
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
LD="$DN/usr/lib/deb-native/ld-dn"
LIBPATH="$DN/usr/lib/aarch64-linux-gnu:$DN/usr/lib"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/pkg"
dpkg-deb -e "$DEB" "$WORK/pkg/DEBIAN"
# Android refuses link(2) in app data (EACCES), so a hard link in the
# package (perl-base: perl5.40.1 -> perl) breaks the unpack here and would
# break dpkg's. Hard links become copies: extracted without them, then
# copied from their target; the repacked package has plain files.
dpkg-deb --fsys-tarfile "$DEB" | tar -tvf - |
  sed -n 's/^h.* \(\.\/[^ ]*\) link to \(\.\/.*\)$/\1\t\2/p' > "$WORK/hardlinks"
cut -f1 "$WORK/hardlinks" > "$WORK/hardlinks.exclude"
dpkg-deb --fsys-tarfile "$DEB" |
  tar -xpf - -C "$WORK/pkg" --no-wildcards --exclude-from="$WORK/hardlinks.exclude"
while IFS="$(printf '\t')" read -r link target; do
  cp -p "$WORK/pkg/$target" "$WORK/pkg/$link"
done < "$WORK/hardlinks"
PKG=$(sed -n 's/^Package: //p' "$WORK/pkg/DEBIAN/control")
echo "Translating $PKG:arm64 ($(sed -n 's/^Version: //p' "$WORK/pkg/DEBIAN/control")) ..."

sed -i 's/^Architecture: all$/Architecture: arm64/' "$WORK/pkg/DEBIAN/control"

if [ -x "$HERE/../custom/$PKG.sh" ]; then
  "$HERE/../custom/$PKG.sh" "$WORK/pkg" "$DN"
  echo "Applied custom/$PKG.sh to $PKG."
fi

find "$WORK/pkg" -path "$WORK/pkg/DEBIAN" -prune -o -type f -print | while IFS= read -r f; do
  [ "$(head -c4 "$f" | od -An -tx1 | tr -d ' \n')" = 7f454c46 ] || continue
  # Libraries and static programs have no interpreter: patchelf says so
  # (captured, not shown -- it is the expected answer, not an error).
  interp=$(patchelf --print-interpreter "$f" 2>&1) || interp=""
  case "$interp" in
    */ld-linux-aarch64.so.1|*/ld-dn) [ "$interp" = "$LD" ] || patchelf --set-interpreter "$LD" "$f" ;;
  esac
  old=$(patchelf --print-rpath "$f" 2>&1) || continue   # not dynamic
  # The prefix's copies of Debian's default library dirs first (Termux's
  # ld.so knows only its own: librnd lives in /usr/lib, survey
  # 2026-09-27), then the ELF's own entries, absolute ones moved into the
  # prefix ($ORIGIN ones stay).
  new=$LIBPATH
  for e in $(printf '%s' "$old" | tr ':' ' '); do
    case "$e" in "$DN"/*|'$ORIGIN'*) ;; /*) e=$DN$e ;; esac
    case ":$new:" in *":$e:"*) ;; *) new=$new:$e ;; esac
  done
  [ "$new" = "$old" ] || patchelf --set-rpath "$new" "$f"
done

"$HERE/patch-scripts-tree.sh" "$WORK/pkg" "$DN"

# Program scripts (maintainer scripts are patch-scripts-tree.sh's).
for d in usr/bin usr/sbin usr/games usr/libexec bin sbin; do
  [ -d "$WORK/pkg/$d" ] || continue
  find "$WORK/pkg/$d" -type f | while IFS= read -r f; do
    [ "$(head -c2 "$f")" = "#!" ] || continue
    line=$(head -n1 "$f")
    interp=$(printf '%s' "$line" | sed -E 's/^#![[:space:]]*([^[:space:]]+).*/\1/')
    case "$interp" in
      /bin/sh|/bin/bash|/bin/dash|/usr/bin/sh|/usr/bin/bash|/usr/bin/dash) new="$DN/usr/bin/dn-shell" ;;
      /usr/bin/perl|/bin/perl) new="$DN/usr/bin/dn-perl" ;;
      /usr/*|/bin/*|/sbin/*) new="$DN$interp" ;;
      *) continue ;;
    esac
    rest=$(printf '%s' "$line" | sed -E 's/^#![[:space:]]*[^[:space:]]+//')
    printf '#!%s%s\n' "$new" "$rest" > "$WORK/line"
    tail -n +2 "$f" >> "$WORK/line"
    cat "$WORK/line" > "$f"
  done
done

dpkg-deb -Znone -b "$WORK/pkg" "$WORK/out.deb"
mv -f "$WORK/out.deb" "$DEB"
