#!/bin/sh
# Package this project's own-built glibc *programs* -- the libc-bin payload
# (ldconfig, ldd, getconf, locale, localedef, iconv, ...) -- as a real
# libc-bin .deb. Companion to dn-package-glibc.sh, which packages libc6 (the
# shared libraries + the dynamic linker).
#
# Why this is needed while libc6-dev/libc-dev-bin are not
# (docs/log/findings/libc6-dev-gap-closed.md): those two are only
# *version*-sensitive, so Debian's real packages install unmodified once our
# libc6 carries Debian's exact version string. libc-bin is *path*-sensitive.
# Its ldconfig is compiled against a sysconfdir; Debian's build points at the
# host /etc, so it writes the host's /etc/ld.so.cache and cannot build a
# prefix's <prefix>/etc/ld.so.cache -- exactly what the dn-glibc fused loader
# needs for env-free library search
# (docs/log/findings/own-glibc-missing-libc-bin.md). Our build's sysconfdir
# is the prefix, so its ldconfig writes <prefix>/etc/ld.so.cache; the other
# libc-bin programs likewise carry prefix-relative built-in paths.
#
# Same template-and-replace approach as dn-package-glibc.sh: take Debian's
# real libc-bin .deb, replace only the programs (usr/bin, usr/sbin) with our
# build's, keep Debian's control metadata, config files, man pages and docs
# untouched. Unlike libc6's flat-to-multiarch relocation, libc-bin's programs
# sit at the same relative paths in DESTDIR, so replacement is a direct path
# copy. Every program Debian ships here is also built by our glibc, so a
# missing one is a hard error.
#
# Note: the ldconfig SIGSYS in docs/log/android-seccomp-audit.md is specific
# to `ldconfig -r` (the chroot/root-prefix mode, needed only to point a *host*
# ldconfig at a prefix). Our own ldconfig is already prefix-relative, so it
# runs plain, with no -r -- that path is not used here.
#
# Usage: dn-package-libc-bin.sh REAL_LIBC_BIN_DEB DESTDIR OUT_DEB
#   REAL_LIBC_BIN_DEB  a real Debian libc-bin_<ver>_arm64.deb, the same
#                      version the patch was built against
#                      (apt-get download libc-bin=<ver>)
#   DESTDIR            the own-glibc build's `make install DESTDIR=` output,
#                      e.g. ~/dn-glibc-build/destdir-clean/<prefix-path>
#   OUT_DEB            where to write the repackaged .deb
set -eu
REAL_DEB=${1:?usage: dn-package-libc-bin.sh REAL_LIBC_BIN_DEB DESTDIR OUT_DEB}
DESTDIR=${2:?usage: dn-package-libc-bin.sh REAL_LIBC_BIN_DEB DESTDIR OUT_DEB}
OUT=${3:?usage: dn-package-libc-bin.sh REAL_LIBC_BIN_DEB DESTDIR OUT_DEB}
case "$REAL_DEB" in /*) ;; *) REAL_DEB="$PWD/$REAL_DEB" ;; esac
case "$DESTDIR" in /*) ;; *) DESTDIR="$PWD/$DESTDIR" ;; esac
case "$OUT" in /*) ;; *) OUT="$PWD/$OUT" ;; esac

[ -d "$DESTDIR/usr/bin" ] || { echo "E: $DESTDIR/usr/bin not found -- wrong DESTDIR?" >&2; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

echo "Unpacking $REAL_DEB as the template ..."
dpkg-deb -R "$REAL_DEB" "$WORK/pkg"

V=$(sed -n 's/^Version: //p' "$WORK/pkg/DEBIAN/control")
echo "Version: $V (unchanged -- see dn-package-glibc.sh's version comment)"

echo "Replacing usr/bin and usr/sbin programs with our own build's ..."
for d in usr/bin usr/sbin; do
  for f in "$WORK/pkg/$d"/*; do
    if [ -L "$f" ]; then continue; fi   # keep Debian's symlinks (e.g. ld.so)
    [ -f "$f" ] || continue
    rel=${f#"$WORK/pkg/"}
    src="$DESTDIR/$rel"
    if [ -f "$src" ]; then
      cp -f "$src" "$f"
    else
      echo "  MISSING in our build: $rel" >&2
      echo "$rel" >> "$WORK/missing"
    fi
  done
done
if [ -f "$WORK/missing" ]; then
  echo "E: $(wc -l < "$WORK/missing") program(s) from the real package have no equivalent in $DESTDIR -- aborting." >&2
  exit 1
fi

echo "Regenerating DEBIAN/md5sums ..."
( cd "$WORK/pkg" && find . -path ./DEBIAN -prune -o -type f -print \
    | sed 's#^\./##' \
    | xargs md5sum \
    > DEBIAN/md5sums )

echo "Building $OUT ..."
dpkg-deb -b "$WORK/pkg" "$OUT"
echo "Done: $(dpkg-deb -f "$OUT" Package) $(dpkg-deb -f "$OUT" Version)"
