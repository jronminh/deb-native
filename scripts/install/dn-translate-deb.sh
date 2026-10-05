#!/bin/sh
# Translate one Debian .deb for the prefix, in place, before dpkg sees it
# (docs/spec/design.md -- the prefix is a real, nested Debian root):
#
#   - "Architecture: all" -> "arm64", the same rule dn-debian-index.sh
#     applied to the index apt planned from, so apt and dpkg agree;
#   - custom/<package>.sh, if any: per-package fixes;
#   - glibc programs' interpreter set to the prefix's own glibc loader
#     (default $DN/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1, from
#     this project's packaged libc6): the kernel loads it directly, and it
#     reads the shim from $DN/etc/ld.so.preload and the prefix's library
#     dirs from $DN/usr/etc/ld.so.cache -- the dn-glibc runtime
#     (docs/spec/dn-glibc-prefix.md). Done here, not
#     after install, so it is right before any maintainer script runs the
#     binary. Library search is the loader's cache rather than a per-file
#     RUNPATH rewrite: RUNPATH is not inherited transitively, and rewriting
#     it on a tightly-packed ET_EXEC binary can corrupt its program headers
#     (docs/log/findings/patchelf-et-exec-runpath.md);
#   - program scripts' "#!" line pointed into the prefix: sh/dash ->
#     $DN/usr/bin/dash, bash -> $DN/usr/bin/bash (both real, apt-installed
#     packages with the fused loader as their own interpreter -- the kernel following
#     the shebang already gets the shim/env set up, same as any other
#     prefix binary, no extra indirection), perl -> dn-perl (Termux's own,
#     no Debian-perl replacement yet), any other /usr, /bin, /sbin
#     interpreter -> the same path under $DN. dn-shell is only the
#     fallback for sh/bash/dash when the prefix's own isn't installed yet
#     (true during early bootstrap, before bash/dash reach the base
#     package set) -- bootstrap scaffolding, not the steady-state path;
#   - maintainer-script shebangs: the same dash/bash-first, dn-shell-
#     fallback logic, via patch-scripts-tree.sh.
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
# Runtime interpreter: the prefix's own fused glibc loader (dn-glibc-prefix.md).
LD="$DN/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1"

# The interpreter rewrite below is this script's whole job. A missing or
# unrunnable patchelf used to be swallowed by the per-file `|| interp=""`, so
# every ELF -- a runtime-installed package's included -- was repacked still
# carrying /lib/ld-linux-aarch64.so.1 while the install reported success. Fail
# loudly instead: the caller (bootstrap, or the apt Pre-Install-Pkgs hook) then
# aborts rather than ship binaries that cannot run.
command -v patchelf >/dev/null 2>&1 || {
  echo "E: patchelf not found -- cannot translate $DEB" >&2
  echo "   in the prefix: apt install patchelf; on the Termux host: pkg install patchelf" >&2
  exit 1
}
patchelf --version >/dev/null 2>&1 || {
  echo "E: patchelf at $(command -v patchelf) does not run -- cannot translate $DEB" >&2
  exit 1
}

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

if [ -x "$HERE/../../core/custom/$PKG.sh" ]; then
  "$HERE/../../core/custom/$PKG.sh" "$WORK/pkg" "$DN"
  echo "Applied custom/$PKG.sh to $PKG."
fi

find "$WORK/pkg" -path "$WORK/pkg/DEBIAN" -prune -o -type f -print | while IFS= read -r f; do
  [ "$(head -c4 "$f" | od -An -tx1 | tr -d ' \n')" = 7f454c46 ] || continue
  # Libraries and static programs have no interpreter: patchelf says so
  # (captured, not shown -- it is the expected answer, not an error).
  interp=$(patchelf --print-interpreter "$f" 2>&1) || interp=""
  case "$interp" in
    */ld-linux-aarch64.so.1) [ "$interp" = "$LD" ] || patchelf --set-interpreter "$LD" "$f" ;;
  esac
  # Library search is the fused loader's ld.so.cache now
  # ($DN/usr/etc/ld.so.cache, built by our libc-bin's ldconfig), which
  # covers the whole load graph -- not a per-file RUNPATH rewrite here.
  # RUNPATH is not inherited transitively anyway (a library's own RUNPATH
  # does not help its own dependencies), so a per-file rewrite would be
  # needed for every .so in a package, not just the executable. It also
  # avoids patchelf corrupting the program header table of a tightly-packed
  # ET_EXEC binary with no room to grow (found on gcc's cc1,
  # docs/log/findings/patchelf-et-exec-runpath.md).
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
      /bin/sh|/usr/bin/sh|/bin/dash|/usr/bin/dash)
        new="$DN/usr/bin/dash"; [ -x "$new" ] || new="$DN/usr/bin/dn-shell" ;;
      /bin/bash|/usr/bin/bash)
        new="$DN/usr/bin/bash"; [ -x "$new" ] || new="$DN/usr/bin/dn-shell" ;;
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
