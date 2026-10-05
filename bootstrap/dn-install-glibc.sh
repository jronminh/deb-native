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
#   DEBS_DIR/dn-shim.so   the shim, built against THIS prefix's own
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

for f in libc6.deb libc-bin.deb; do
  [ -e "$DEBS/$f" ] || { echo "E: $DEBS/$f not found (dn-install-glibc.sh needs libc6.deb, libc-bin.deb, and a shim)" >&2; exit 1; }
done
# The shim: prefer the current name, accept the pre-rename path-redirect.so.
SHIM_SRC="$DEBS/dn-shim.so"; [ -e "$SHIM_SRC" ] || SHIM_SRC="$DEBS/path-redirect.so"
[ -e "$SHIM_SRC" ] || { echo "E: no shim in $DEBS (dn-shim.so or path-redirect.so)" >&2; exit 1; }
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
  "$HERE/../core/install/dn-translate-deb.sh" "$WORK/$1" "$DN"
  $DPKG -i "$WORK/$1"
  echo "$2:arm64 hold" | "$TP/bin/dpkg" --admindir="$DN/var/lib/dpkg" --set-selections
}
# Seed the loader + libc from the bundle BEFORE dpkg runs libc6's preinst: the
# maintainer-script interpreters are glibc ELFs (PT_INTERP = the prefix
# loader), so the loader must exist before any maintainer script runs.
echo "Seeding the loader and libc from the bundle ..."
STASH="$DN/usr/lib/deb-native/glibc-swap"
mkdir -p "$STASH"
cp -a "$DEBS/files/." "$STASH/"
"$HERE/../core/install/dn-fix-glibc.sh" "$DN"

echo "Installing Debian's libc6 ..."
install_held libc6.deb libc6
echo "Installing Debian's libc-bin ..."
install_held libc-bin.deb libc-bin

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
# Keep the shim setup-runtime.sh just built from this checkout's
# native/dn-shim.c (0.7.0: it carries the system()/popen()/pclose() and
# link()/linkat() fallbacks the Debian apt/dpkg deploy needs). Only fall back
# to the bundle's prebuilt shim if the local build is missing -- the rolling
# glibc-bundle predates these fixes.
if [ ! -e "$LIBDIR/dn-shim.so" ]; then
  cp -f "$SHIM_SRC" "$LIBDIR/dn-shim.so"
fi
chmod 755 "$LIBDIR/dn-shim.so"

mkdir -p "$DN/etc"
printf '%s\n' "$LIBDIR/dn-shim.so" > "$DN/etc/ld.so.preload"

# SYSCONFDIR for this build is <prefix>/usr/etc (configure --prefix=$DN/usr).
# libc-bin's own ./etc/ld.so.conf (the guest /etc) is a different file our
# ldconfig never reads -- docs/spec/dn-glibc-prefix.md "Fixed paths".
mkdir -p "$DN/usr/etc/ld.so.conf.d"
printf 'include %s/usr/etc/ld.so.conf.d/*.conf\n' "$DN" > "$DN/usr/etc/ld.so.conf"
printf '%s/usr/lib/aarch64-linux-gnu\n%s/usr/lib\n' "$DN" "$DN" > "$DN/usr/etc/ld.so.conf.d/dn.conf"

# ldconfig is a *static* binary: __dn_prefix_get is NULL in a static link, so
# __dn_build yields empty cache/conf/aux paths and ldconfig aborts with
# "Renaming of ~ to  failed" (docs/spec/deploy.md "Open items"). Bypass until
# the writer gets its own run-time derivation: run it under the tracer, with
# explicit -C/-f, so the bare guest paths are bound into the prefix, and never
# abort the bootstrap on it. A missing ld.so.cache is not fatal (programs still
# run), but a non-zero ldconfig under `set -e` would be.
mkdir -p "$DN/var/cache/ldconfig"
if [ -x "$DN/usr/lib/deb-native/dn-run" ] && [ -x "$DN/usr/lib/deb-native/dn-trace" ]; then
  echo "Running the prefix's own ldconfig (via dn-trace) ..."
  "$DN/usr/lib/deb-native/dn-run" --trace "$DN/usr/sbin/ldconfig" \
    -C /usr/etc/ld.so.cache -f /usr/etc/ld.so.conf || true
else
  echo "Running the prefix's own ldconfig (untraced) ..."
  "$DN/usr/sbin/ldconfig" || true
fi
