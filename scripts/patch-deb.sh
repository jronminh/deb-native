#!/bin/sh
# Patch a .deb's maintainer control scripts BEFORE dpkg ever sees it,
# instead of patching $ADMINDIR/info/* after --unpack (patch-maintainer-
# scripts.sh) -- found necessary because a package's preinst runs DURING
# dpkg's own --unpack step, before any post-unpack patch step gets a
# chance to touch it (docs/findings.md: cdebconf's
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
WRAPPER="$INSTDIR/usr/bin/dn-shell"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

dpkg-deb -R "$DEB" "$WORK/pkg"

for f in "$WORK/pkg/DEBIAN/preinst" "$WORK/pkg/DEBIAN/postinst" \
         "$WORK/pkg/DEBIAN/prerm" "$WORK/pkg/DEBIAN/postrm"; do
  [ -f "$f" ] || continue
  # No idempotency marker needed: the only thing this loop still does is
  # rewrite the shebang, which is naturally idempotent (the regex below
  # only ever matches a plain /bin/sh-style shebang, never the wrapper
  # path it rewrites to) -- running this twice is harmless by
  # construction, and a script must be allowed a second attempt once
  # the wrapper exists but didn't yet on the first pass (a real
  # chicken-and-egg case: dash's own dependencies are patched before
  # dash itself is unpacked).
  #
  # NOT rewriting literal /etc, /usr, /var, /opt paths in the script text
  # anymore (an earlier version of this script did). Found the hard way,
  # twice: many maintainer scripts (base-files, and dpkg's own
  # update-alternatives/dpkg-divert/dpkg-statoverride/dpkg-trigger) are
  # already DPKG_ROOT-aware -- dpkg exports $DPKG_ROOT to every script,
  # set to this project's prefix, and their own logic already resolves
  # paths against it correctly. Rewriting a literal path on top of that
  # double-prefixes it once the script's own $DPKG_ROOT concatenation
  # ALSO applies (confirmed: $INSTDIR/usr/bin/mawk ->
  # $INSTDIR/$INSTDIR/usr/bin/mawk, and the same shape of bug in
  # base-files's own DPKG_ROOT-based helper functions). The runtime
  # LD_PRELOAD shim (via the dash wrapper below) already covers the
  # OTHER case -- a script with NO $DPKG_ROOT awareness at all, using a
  # bare literal absolute path (openssl's `ln -s /etc/ssl /usr/lib/ssl`)
  # -- by intercepting the actual open/exec syscall-adjacent call with
  # the literal path, not by editing the script's text first. A
  # DPKG_ROOT-aware script's own already-correct, already-prefixed path
  # never matches this shim's rewrite (it no longer starts with a bare
  # /usr, /etc, /var or /opt), so nothing double-applies there either.
  # Allow (and preserve) a trailing flag on the shebang, e.g. "#!/bin/sh
  # -e" -- found the hard way: openssl's postinst has exactly this, and
  # the earlier version of this regex (requiring nothing after the
  # interpreter name) silently never matched it at all, so it never got
  # the wrapper treatment and just fell through to the real, unpatched
  # /system/bin/sh. A shebang carries at most one argument, so this is
  # passed to the wrapper itself (its own shebang has none), which
  # forwards it on to the real dash via "$@".
  # \s/\S are PCRE, not POSIX ERE -- grep -E/sed -E here silently never
  # matched at all with them (found the hard way: this stayed broken
  # across a whole earlier round of testing). [[:space:]]/[^[:space:]]
  # are the POSIX ERE equivalents.
  shebang=$(head -1 "$f")
  if printf '%s' "$shebang" | grep -qE '^#![[:space:]]*/bin/(sh|bash|dash)([[:space:]]+[^[:space:]]+)?[[:space:]]*$'; then
    # NOT using # as the sed delimiter: the pattern starts with a literal
    # "#" (matching the shebang's own "#!"), which broke delimiter
    # parsing outright and, under this script's `set -eu`, silently
    # killed the entire loop partway through -- every file alphabetically
    # after the one being processed when this hit never got touched, in
    # every run, looking like the mechanism just didn't work at all.
    flag=$(printf '%s' "$shebang" | sed -E 's,^#![[:space:]]*/bin/(sh|bash|dash)[[:space:]]*([^[:space:]]*)[[:space:]]*$,\2,')
    sed -i "1s#.*#\#!${WRAPPER}${flag:+ }${flag}#" "$f"
    # No `unset LD_PRELOAD` line any more: the shim's own execve() dispatch
    # strips LD_PRELOAD for a non-glibc target and keeps it for a glibc one,
    # so a forked glibc coreutils command stays redirected while a forked
    # Bionic command does not crash -- neither needs the shell to clear it.
  fi
done

dpkg-deb -b "$WORK/pkg" "$WORK/out.deb" >/dev/null
mv -f "$WORK/out.deb" "$DEB"
