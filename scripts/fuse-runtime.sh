#!/bin/sh
# Branch experiment (fusion-no-prefix): wrappers for commands that are
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
# Usage: fuse-runtime.sh INSTDIR
set -eu
INSTDIR=${1:?usage: fuse-runtime.sh INSTDIR}
case "$INSTDIR" in /*) ;; *) INSTDIR="$PWD/$INSTDIR" ;; esac
PREFIX_DIR=${DN_TERMUX_PREFIX:-${PREFIX:-/data/data/com.termux/files/usr}}

WRAPDIR="$INSTDIR/lib/deb-native/fusion-bin"
mkdir -p "$WRAPDIR"

# See docs/findings.md ("Bug 4") for why both flags are needed together,
# space-separated (an earlier attempt used --altdir=DIR; update-alternatives
# only accepts the space-separated form), and why DPKG_ROOT alone is not
# enough on its own.
cat > "$WRAPDIR/update-alternatives" <<EOF
#!/system/bin/sh
exec "$PREFIX_DIR/bin/update-alternatives" --altdir "$INSTDIR/etc/alternatives" --admindir "$INSTDIR/var/lib/dpkg/alternatives" "\$@"
EOF
chmod 755 "$WRAPDIR/update-alternatives"
