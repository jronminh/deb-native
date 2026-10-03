#!/bin/sh
# Install this project's own-built, prefix-targeted glibc (docs/spec/
# dn-glibc-prefix.md) into a fresh prefix: libc6 + libc-bin as real,
# held Debian packages (not a Termux stand-in -- dn-standins.sh no longer
# handles libc6), plus the fused loader's two files (ld.so.preload,
# ld.so.conf) and the prefix's own ldconfig run to build ld.so.cache.
# Every fresh prefix gets this, out of the box; there is no ld-dn fallback
# and no migration path here -- that is a separate, one-off concern for an
# already-installed prefix, not this script's job.
#
# Usage: dn-install-glibc.sh PREFIX DEBS_DIR
#   DEBS_DIR/libc6.deb          dn-package-glibc.sh output.
#   DEBS_DIR/libc-bin.deb       dn-package-libc-bin.sh output.
#   DEBS_DIR/path-redirect.so   the shim, built against THIS prefix's own
#                                glibc (symbol versioning: a shim built
#                                against Termux's glibc aborts under this
#                                loader -- docs/log/findings/
#                                fused-shim-self-derives-prefix.md, "Not
#                                done yet"). build-path-redirect.sh cannot
#                                produce this yet; built out of band today.
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
# solves the same way -- libc6 keeps Debian's real Depends: libgcc-s1 (its
# control file is Debian's own template, dn-package-glibc.sh), which is not
# installed yet (it lands later, with the toolchain,
# docs/spec/dn-glibc-prefix.md "Install order").
DPKG="$TP/bin/dpkg --admindir=$DN/var/lib/dpkg --instdir=$DN --force-not-root --force-script-chrootless --force-depends"

for f in libc6.deb libc-bin.deb path-redirect.so; do
  [ -e "$DEBS/$f" ] || { echo "E: $DEBS/$f not found (dn-install-glibc.sh needs libc6.deb, libc-bin.deb, path-redirect.so)" >&2; exit 1; }
done

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# Held like dn-standins.sh's stand-ins: these are the project's own build,
# never derivable from a plain `apt install`, and must survive `apt upgrade`
# untouched (docs/spec/dn-glibc-prefix.md "The package set"). Translated
# like any other package before dpkg sees it (dn-translate-deb.sh: here
# that's just the maintainer-script shebang -- the ELF interpreter pass is
# a no-op, these binaries already point at the fused loader from the
# build) so libc-bin's postinst/trigger gets dn-shell's PATH (priv/ first)
# instead of the raw host /bin/sh it shipped with.
install_held() {  # DEB PACKAGE
  cp -f "$DEBS/$1" "$WORK/$1"
  "$HERE/../install/dn-translate-deb.sh" "$WORK/$1" "$DN"
  $DPKG -i "$WORK/$1"
  echo "$2:arm64 hold" | "$TP/bin/dpkg" --admindir="$DN/var/lib/dpkg" --set-selections
}
echo "Installing own-built libc6 ..."
install_held libc6.deb libc6
echo "Installing own-built libc-bin ..."
install_held libc-bin.deb libc-bin

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

echo "Running the prefix's own ldconfig ..."
"$DN/usr/sbin/ldconfig"
