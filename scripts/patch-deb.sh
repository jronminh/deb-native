#!/bin/sh
# Patch a .deb's maintainer control scripts BEFORE dpkg ever sees it,
# instead of patching $ADMINDIR/info/* after --unpack (patch-maintainer-
# scripts.sh) -- found necessary because a package's preinst runs DURING
# dpkg's own --unpack step, before any post-unpack patch step gets a
# chance to touch it (docs/findings-dash-wrapper-2026-09-25.md: cdebconf's
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
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SHIM="$HERE/../native/path-redirect.so"
WRAPPER="$INSTDIR/usr/bin/dn-dash"

DASH="$INSTDIR/usr/bin/dash"
if [ -x "$DASH" ] && [ -f "$SHIM" ] && [ ! -e "$WRAPPER" ]; then
  cat > "$WRAPPER" <<EOF
#!/system/bin/sh
export LD_PRELOAD="$SHIM"
export DN_INSTDIR="$INSTDIR"
export PATH="$INSTDIR/usr/sbin:$INSTDIR/usr/bin:$INSTDIR/sbin:$INSTDIR/bin:\$PATH"
export DEBIAN_FRONTEND=noninteractive
exec "$DASH" "\$@"
EOF
  chmod 755 "$WRAPPER"
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

dpkg-deb -R "$DEB" "$WORK/pkg"

for f in "$WORK/pkg/DEBIAN/preinst" "$WORK/pkg/DEBIAN/postinst" \
         "$WORK/pkg/DEBIAN/prerm" "$WORK/pkg/DEBIAN/postrm"; do
  [ -f "$f" ] || continue
  # Idempotency guard: this same script (already unpacked once before, in
  # an earlier transaction, or already patched once via this same file
  # here) must not get the path rewrite applied a second time -- found
  # the hard way: a second pass double-prefixed an already-rewritten
  # $INSTDIR/usr/bin/mawk into $INSTDIR/$INSTDIR/usr/bin/mawk.
  grep -q '# deb-native: patched' "$f" && continue
  sed -i -E "s#([ =\"'(])/(etc|usr|var|opt)/#\1${INSTDIR}/\2/#g" "$f"
  if [ -x "$WRAPPER" ] && head -1 "$f" | grep -qE '^#!\s*/bin/(sh|bash|dash)\s*$'; then
    sed -i "1s#.*#\#!${WRAPPER}#" "$f"
    sed -i "2i unset LD_PRELOAD 2>/dev/null || true" "$f"
  fi
  sed -i "1a # deb-native: patched" "$f"
done

dpkg-deb -b "$WORK/pkg" "$WORK/out.deb" >/dev/null
mv -f "$WORK/out.deb" "$DEB"
