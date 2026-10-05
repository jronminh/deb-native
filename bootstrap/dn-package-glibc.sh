#!/bin/sh
# Package this project's own-built glibc (third_party/glibc-android-patches/
# dn-glibc-android.patch applied, configured and `make install DESTDIR=...`'d
# on-device -- docs/log/android-seccomp-audit.md has the build recipe and
# validation) as a real libc6 .deb, instead of dn-standins.sh's stand-in
# (Termux's glibc, symlinked in under Debian's name).
#
# Rather than hand-writing libc6's maintainer scripts (preinst/postinst/
# postrm, ldconfig triggers, debconf locale templates -- real, non-trivial,
# already correct upstream), this takes the REAL Debian libc6 .deb as a
# template and only replaces what actually differs: the shared-library
# payload (built by us instead of Debian's buildds). Everything else
# (control metadata including the version string, maintainer scripts,
# doc, lintian overrides) comes from Debian's own package as-is -- it is
# still accurately describing glibc 2.41-12+deb13u4, just built here, not
# a different thing wearing its name: same upstream source, same Debian
# patch series, our Android compatibility patch on top makes it *run*
# here, it doesn't change what it is. No version-bump suffix, on purpose
# -- the whole point is that `libc6-dev`/`libc-dev-bin`/`locales` (not yet
# packaged by this project) keep working unmodified, straight from
# Debian's archive, because their `Depends: libc6 (= ...)`/`(>> ...)
# (<< ...)` sees an exact, true match instead of a `+dnN` that would force
# forking every package in that dependency chain just to keep up
# (confirmed hitting exactly this with libc6-dev and libc-dev-bin, both
# version-pinned to libc6, 2026-10-01). Held (dn-standins.sh /
# this script's own caller) so `apt upgrade` can't silently replace it
# with Debian's real, unpatched build once a newer point release exists.
#
# Our own build was configured --disable-multi-arch (a flat usr/lib/, not
# Debian's usr/lib/aarch64-linux-gnu/) -- confirmed safe to relocate at
# packaging time, not rebuild: none of libc.so.6/gconv-modules.cache/etc.
# have that path baked in as a literal string (checked with `strings`).
# Matching Debian's real multiarch path avoids a landmine for later:
# libc6-dev's linker scripts hardcode /lib/aarch64-linux-gnu.
#
# Scope: libc6 only (the runtime shared libraries + NSS modules + gconv +
# the dynamic linker). `libc6-dev`/`libc-dev-bin` need no equivalent script
# -- same reasoning as the no-version-bump above, they install from Debian's
# archive as-is once libc6's version matches exactly. `libc-bin` is the one
# exception: it is *path*-sensitive, not just version-pinned (its `ldconfig`
# is compiled against a sysconfdir), so it does need its own repackage --
# `dn-package-libc-bin.sh`, this script's companion.
#
# Usage: dn-package-glibc.sh REAL_LIBC6_DEB DESTDIR OUT_DEB
#   REAL_LIBC6_DEB  a real Debian libc6_<ver>_arm64.deb, same version the
#                    patch was built against (apt-get download libc6=<ver>)
#   DESTDIR          the own-glibc build's `make install DESTDIR=` output,
#                    e.g. ~/dn-glibc-build/destdir-clean/<prefix-path>
#   OUT_DEB          where to write the repackaged .deb
set -eu
REAL_DEB=${1:?usage: dn-package-glibc.sh REAL_LIBC6_DEB DESTDIR OUT_DEB}
DESTDIR=${2:?usage: dn-package-glibc.sh REAL_LIBC6_DEB DESTDIR OUT_DEB}
OUT=${3:?usage: dn-package-glibc.sh REAL_LIBC6_DEB DESTDIR OUT_DEB}
case "$REAL_DEB" in /*) ;; *) REAL_DEB="$PWD/$REAL_DEB" ;; esac
case "$DESTDIR" in /*) ;; *) DESTDIR="$PWD/$DESTDIR" ;; esac
case "$OUT" in /*) ;; *) OUT="$PWD/$OUT" ;; esac

[ -d "$DESTDIR/usr/lib" ] || { echo "E: $DESTDIR/usr/lib not found -- wrong DESTDIR?" >&2; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

echo "Unpacking $REAL_DEB as the template ..."
dpkg-deb -R "$REAL_DEB" "$WORK/pkg"

V=$(sed -n 's/^Version: //p' "$WORK/pkg/DEBIAN/control")
echo "Version: $V (unchanged -- see this script's own comment for why)"

MA="$WORK/pkg/usr/lib/aarch64-linux-gnu"
echo "Replacing the payload under usr/lib/aarch64-linux-gnu/ with our own build's ..."
# Every file the real package ships under usr/lib/aarch64-linux-gnu/ (the
# manifest of what belongs in libc6) has its replacement copied from our
# build's flat usr/lib/ (same basename, since --disable-multi-arch put
# everything there instead of in a triplet subdirectory).
find "$MA" -type f | while IFS= read -r f; do
  rel=${f#"$MA"/}
  src="$DESTDIR/usr/lib/$rel"
  if [ -f "$src" ]; then
    cp -f "$src" "$f"
  else
    echo "  MISSING in our build: usr/lib/$rel" >&2
    echo "$rel" >> "$WORK/missing"
  fi
done
if [ -f "$WORK/missing" ]; then
  echo "E: $(wc -l < "$WORK/missing") file(s) from the real package have no equivalent in $DESTDIR -- aborting." >&2
  exit 1
fi

# The compat symlink outside the multiarch dir.
ln -sfn aarch64-linux-gnu/ld-linux-aarch64.so.1 "$WORK/pkg/usr/lib/ld-linux-aarch64.so.1"

mkdir -p "$WORK/pkg/etc/ld.so.conf.d"
if [ ! -f "$WORK/pkg/etc/ld.so.conf.d/aarch64-linux-gnu.conf" ]; then
  echo "/usr/lib/aarch64-linux-gnu" > "$WORK/pkg/etc/ld.so.conf.d/aarch64-linux-gnu.conf"
fi

echo "Regenerating DEBIAN/md5sums ..."
( cd "$WORK/pkg" && find . -path ./DEBIAN -prune -o -type f -print \
    | sed 's#^\./##' \
    | xargs md5sum \
    > DEBIAN/md5sums )

echo "Building $OUT ..."
dpkg-deb -b "$WORK/pkg" "$OUT"
echo "Done: $(dpkg-deb -f "$OUT" Package) $(dpkg-deb -f "$OUT" Version)"
