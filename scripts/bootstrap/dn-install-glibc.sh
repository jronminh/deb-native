#!/bin/sh
# Install glibc into a fresh prefix the Debian-swap way (docs/spec/deploy.md):
# install Debian's own libc6 + libc-bin, then overwrite the 10 files this
# project's Android patch changes with our prefix-agnostic build. Every fresh
# prefix gets this, out of the box; there is no ld-dn fallback and no
# migration path here -- that is a separate, one-off concern for an
# already-installed prefix, not this script's job.
#
# Usage: dn-install-glibc.sh PREFIX DEBS_DIR
#   DEBS_DIR/libc6.deb          Debian's real libc6 (not our own build).
#   DEBS_DIR/libc-bin.deb       Debian's real libc-bin.
#   DEBS_DIR/path-redirect.so   the shim, built against THIS prefix's own
#                                glibc (symbol versioning: a shim built
#                                against Termux's glibc aborts under this
#                                loader).
#   DEBS_DIR/files/             the 10 patched files, laid out relative to
#                                the prefix (usr/lib/aarch64-linux-gnu/...,
#                                usr/sbin/ldconfig, usr/bin/localedef,
#                                usr/bin/iconv). They are built for ANY
#                                prefix -- the loader derives the live prefix
#                                from its own path at run time.
set -eu
umask 022
DN=${1:?usage: dn-install-glibc.sh PREFIX DEBS_DIR}
DEBS=${2:?usage: dn-install-glibc.sh PREFIX DEBS_DIR}
case "$DN" in /*) ;; *) DN="$PWD/$DN" ;; esac
case "$DEBS" in /*) ;; *) DEBS="$PWD/$DEBS" ;; esac
TP=${DN_TERMUX_PREFIX:-${PREFIX:-/data/data/com.termux/files/usr}}
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# --force-depends: this is the very first package into an empty database,
# the same circular-essential-set problem debootstrap's own base install
# solves the same way -- libc6 keeps Debian's real Depends: libgcc-s1, which
# is not installed yet (it lands later, with the toolchain,
# docs/spec/dn-glibc-prefix.md "Install order").
DPKG="$TP/bin/dpkg --admindir=$DN/var/lib/dpkg --instdir=$DN --force-not-root --force-script-chrootless --force-depends"

for f in libc6.deb libc-bin.deb path-redirect.so; do
  [ -e "$DEBS/$f" ] || { echo "E: $DEBS/$f not found (dn-install-glibc.sh needs libc6.deb, libc-bin.deb, path-redirect.so)" >&2; exit 1; }
done
[ -d "$DEBS/files" ] || { echo "E: $DEBS/files not found (the 10 patched glibc files)" >&2; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# Held like dn-standins.sh's stand-ins: libc6/libc-bin carry the project's own
# build (never derivable from a plain `apt install`) and must survive `apt
# upgrade` untouched (docs/spec/dn-glibc-prefix.md "The package set").
# Translated like any other package before dpkg sees it
# (dn-translate-deb.sh: here that's the maintainer-script shebang + the
# ELF interpreter pass) so libc-bin's postinst/trigger gets dn-shell's PATH
# (priv/ first) instead of the raw host /bin/sh it shipped with.
install_held() {  # DEB PACKAGE
  cp -f "$DEBS/$1" "$WORK/$1"
  "$HERE/../install/dn-translate-deb.sh" "$WORK/$1" "$DN"
  $DPKG -i "$WORK/$1"
  echo "$2:arm64 hold" | "$TP/bin/dpkg" --admindir="$DN/var/lib/dpkg" --set-selections
}
echo "Installing Debian's libc6 ..."
install_held libc6.deb libc6
echo "Installing Debian's libc-bin ..."
install_held libc-bin.deb libc-bin

echo "Swapping the 10 patched glibc files ..."
cp -a "$DEBS/files/." "$DN/"

# The swapped programs still carry the build prefix in PT_INTERP; point them
# at this prefix's own loader, exactly like dn-translate-deb.sh does per
# package. Libraries and the loader have no interpreter, so this is a no-op
# for them.
LD="$DN/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1"
for f in usr/sbin/ldconfig usr/bin/localedef usr/bin/iconv; do
  [ -e "$DN/$f" ] || continue
  interp=$(patchelf --print-interpreter "$DN/$f" 2>&1) || interp=""
  case "$interp" in
    */ld-linux-aarch64.so.1) [ "$interp" = "$LD" ] || patchelf --set-interpreter "$LD" "$DN/$f" ;;
  esac
done

LIBDIR="$DN/usr/lib/deb-native"
mkdir -p "$LIBDIR"
cp -f "$DEBS/path-redirect.so" "$LIBDIR/path-redirect.so"
chmod 755 "$LIBDIR/path-redirect.so"

mkdir -p "$DN/etc"
printf '%s\n' "$LIBDIR/path-redirect.so" > "$DN/etc/ld.so.preload"

# SYSCONFDIR for this build is <prefix>/usr/etc (configure --prefix=$DN/usr).
# libc-bin's own ./etc/ld.so.conf (the guest /etc) is a different file our
# ldconfig never reads -- docs/spec/dn-glibc-prefix.md "Fixed paths".
mkdir -p "$DN/usr/etc/ld.so.conf.d"
printf 'include %s/usr/etc/ld.so.conf.d/*.conf\n' "$DN" > "$DN/usr/etc/ld.so.conf"
printf '%s/usr/lib/aarch64-linux-gnu\n%s/usr/lib\n' "$DN" "$DN" > "$DN/usr/etc/ld.so.conf.d/dn.conf"

# ldconfig's SYSCONFDIR is still baked to the build prefix, so steer -f/-C at
# this prefix explicitly (its writer path is an open item -- the loader's
# reader path, by contrast, is already derived).
echo "Running the prefix's own ldconfig ..."
"$DN/usr/sbin/ldconfig" -f "$DN/usr/etc/ld.so.conf" -C "$DN/usr/etc/ld.so.cache"
