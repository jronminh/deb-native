#!/bin/sh
# Patch a .deb's maintainer control scripts BEFORE dpkg ever sees it,
# instead of patching $ADMINDIR/info/* after --unpack (patch-maintainer-
# scripts.sh) -- found necessary because a package's preinst runs DURING
# dpkg's own --unpack step, before any post-unpack patch step gets a
# chance to touch it (docs/log/findings.md: cdebconf's
# preinst, "mkdir -p /var/lib/cdebconf", failed with "Read-only file
# system" because it ran unpatched). Confirmed by reading sudo-less's own
# tools/prefix-integrate.sh: it's purely a post-hoc step (launchers,
# services) -- sudo-less never needs a "patch between unpack and
# configure" step at all, because their view is active for dpkg's entire
# process lifetime (both preinst and postinst, uniformly). Since this
# project can't build a live view, the equivalent fix has to happen
# before dpkg starts, not between its phases.
#
# Usage: patch-deb.sh DEB_FILE INSTDIR
# Rewrites DEB_FILE in place (via dpkg-deb -b into a temp file, then mv).
set -eu
DEB=${1:?usage: patch-deb.sh DEB_FILE INSTDIR}
INSTDIR=${2:?usage: patch-deb.sh DEB_FILE INSTDIR}
case "$INSTDIR" in /*) ;; *) INSTDIR="$PWD/$INSTDIR" ;; esac
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

# The maintainer-script runtime (glibc bash + shim + no-op chown/chgrp) comes
# from Termux's pre-existing glibc userland, so it exists before any Debian
# package is unpacked -- no dash chicken-and-egg. Created here because this
# runs before every --unpack, so even preinst scripts get a working shebang.
"$HERE/setup-runtime.sh" "$INSTDIR"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

dpkg-deb -R "$DEB" "$WORK/pkg"

"$HERE/patch-scripts-tree.sh" "$WORK/pkg" "$INSTDIR"

dpkg-deb -b "$WORK/pkg" "$WORK/out.deb"
mv -f "$WORK/out.deb" "$DEB"
