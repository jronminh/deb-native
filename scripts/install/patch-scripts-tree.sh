#!/bin/sh
# Point an extracted package's maintainer scripts at the prefix's own
# interpreter: their "#!/bin/sh" (or bash, dash) shebang is rewritten to
# $INSTDIR/usr/bin/dash or .../bash directly -- both are real,
# apt-installed packages with ld-dn as their own ELF interpreter, so the
# kernel following the shebang already gets the shim/environment set up
# the same way any other prefix binary does, no extra indirection. Falls
# back to dn-shell only when the target isn't installed yet (true during
# early bootstrap, before bash/dash reach the base package set) -- that
# fallback is bootstrap scaffolding, not something a steady-state prefix
# should depend on. A flag on the shebang is kept either way. Works on a
# package tree already unpacked with dpkg-deb -R, so dn-translate-deb.sh
# can do it inside its one unpack/repack pass; patch-deb.sh wraps it for a
# .deb file.
#
# Usage: patch-scripts-tree.sh PACKAGE_TREE INSTDIR
set -eu
TREE=${1:?usage: patch-scripts-tree.sh PACKAGE_TREE INSTDIR}
INSTDIR=${2:?usage: patch-scripts-tree.sh PACKAGE_TREE INSTDIR}
case "$INSTDIR" in /*) ;; *) INSTDIR="$PWD/$INSTDIR" ;; esac
WRAPPER="$INSTDIR/usr/bin/dn-shell"

for f in "$TREE/DEBIAN/preinst" "$TREE/DEBIAN/postinst" \
         "$TREE/DEBIAN/prerm" "$TREE/DEBIAN/postrm"; do
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
    name=$(printf '%s' "$shebang" | sed -E 's,^#![[:space:]]*/bin/(sh|bash|dash)[[:space:]]*([^[:space:]]*)[[:space:]]*$,\1,')
    flag=$(printf '%s' "$shebang" | sed -E 's,^#![[:space:]]*/bin/(sh|bash|dash)[[:space:]]*([^[:space:]]*)[[:space:]]*$,\2,')
    case "$name" in
      bash) target="$INSTDIR/usr/bin/bash" ;;
      *)    target="$INSTDIR/usr/bin/dash" ;;  # sh, dash: Debian's own /bin/sh target
    esac
    [ -x "$target" ] || target="$WRAPPER"
    sed -i "1s#.*#\#!${target}${flag:+ }${flag}#" "$f"
    # No `unset LD_PRELOAD` line any more: the shim's own execve() dispatch
    # strips LD_PRELOAD for a non-glibc target and keeps it for a glibc one,
    # so a forked glibc coreutils command stays redirected while a forked
    # Bionic command does not crash -- neither needs the shell to clear it.
  fi
done
