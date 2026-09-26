#!/bin/sh
# True fusion (fusion-debian-mode): wrappers for commands that are
# DPKG_ROOT-aware but ALSO have a compiled-in absolute --altdir/--admindir-
# style default, so DPKG_ROOT alone double-prefixes them instead of being
# ignored (docs/findings.md, "Bug 4": traced with strace, same shape as the
# already-documented mawk double-prefix bug). The classic (non-fusion)
# design hit this for update-alternatives too and fixed it the same way
# (commit af6500b): force the real flags explicitly rather than rely on
# DPKG_ROOT. Fusion mode can't put the wrapper at $INSTDIR/usr/bin the way
# that fix does -- fusion has no separate sandbox tree, so
# $INSTDIR/usr/bin (via the $PREFIX/usr self-symlink) IS Termux's own real
# bin/, and a same-named file there would replace Termux's own binary
# system-wide, not just shadow it for this project. Lives in its own
# directory instead (fusion-bin), which dn-launch.c's fuse-mode PATH
# already puts first.
#
# Also builds the runtime-stage pieces (docs/findings.md, "runtime stage"
# section; github.com/jronminh/deb-native issue #2): the shim
# (path-redirect.so) and dn-run, at $INSTDIR/lib/deb-native/ -- the SAME
# real directory dn-run.c's own hardcoded path computation
# ("$INSTDIR/usr/lib/deb-native/...") resolves to here, via the
# $INSTDIR/usr self-symlink (bug 3's fix); not a coincidence to rely on
# without saying so. dn-run itself needs no fusion-specific changes: it
# already supports a DN_INSTDIR override (no self-location math to fix,
# unlike dn-launch.c before this branch's earlier fix), and both its shim
# path and its PATH string are built from "$INSTDIR/usr/...", which the
# same symlink resolves correctly.
#
# Usage: fuse-runtime.sh INSTDIR
set -eu
INSTDIR=${1:?usage: fuse-runtime.sh INSTDIR}
case "$INSTDIR" in /*) ;; *) INSTDIR="$PWD/$INSTDIR" ;; esac
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SRC="$HERE/../native"
PREFIX_DIR=${DN_TERMUX_PREFIX:-${PREFIX:-/data/data/com.termux/files/usr}}

LIBDIR="$INSTDIR/lib/deb-native"
WRAPDIR="$LIBDIR/fusion-bin"
mkdir -p "$WRAPDIR"

if [ ! -f "$LIBDIR/path-redirect.so" ] || [ "$SRC/path-redirect.c" -nt "$LIBDIR/path-redirect.so" ]; then
  "$HERE/build-path-redirect.sh" "$LIBDIR/path-redirect.so"
fi

if [ ! -x "$LIBDIR/dn-run" ] || [ "$SRC/dn-run.c" -nt "$LIBDIR/dn-run" ]; then
  clang -O2 -o "$LIBDIR/dn-run" "$SRC/dn-run.c"
  chmod 755 "$LIBDIR/dn-run"
fi

# See docs/findings.md ("Bug 4") for why both flags are needed together,
# space-separated (an earlier attempt used --altdir=DIR; update-alternatives
# only accepts the space-separated form), and why DPKG_ROOT alone is not
# enough on its own.
cat > "$WRAPDIR/update-alternatives" <<EOF
#!/system/bin/sh
exec "$PREFIX_DIR/bin/update-alternatives" --altdir "$INSTDIR/etc/alternatives" --admindir "$INSTDIR/var/lib/dpkg/alternatives" "\$@"
EOF
chmod 755 "$WRAPDIR/update-alternatives"
