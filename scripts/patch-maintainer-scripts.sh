#!/bin/sh
# Rewrite hardcoded absolute paths in a package's maintainer scripts,
# between dpkg's --unpack and --configure, so a script like debconf's
# `. /usr/share/debconf/confmodule` or openssl's `ln -s ... /usr/lib/ssl`
# finds the file where it actually landed ($INSTDIR), instead of at a
# real absolute path Android doesn't have.
#
# Replaces the LD_PRELOAD approach in design.md's Bionic
# shim attempt, abandoned after testing found it doesn't reach the real
# target: maintainer scripts run via the real /bin/sh, which on Android
# is /system/bin/sh (root-owned toybox, not Termux's dash at all -- a
# wrong assumption in the earlier attempt), and BIND_NOW-linked binaries
# don't honor LD_PRELOAD's override on Bionic regardless. Plain text
# rewriting sidesteps all of that: maintainer scripts are shell scripts,
# and a shell script's paths are almost always literal strings, not
# runtime-computed the way a compiled binary's might be.
#
# Verified end to end against a real test .deb: a postinst doing
# `. /etc/foo.conf` failed before this script ran, and printed the
# sourced file's content correctly after.
#
# Also generates a tiny wrapper (INSTDIR/usr/bin/dn-dash, once) and
# points each script's shebang at it instead of at /bin/sh -- dpkg just
# execve()s the script file and lets the kernel resolve the shebang
# against the real filesystem, which on Android is /system/bin/sh
# (root-owned, unpatchable, not ours to touch). The wrapper runs under
# that real, unmodified system sh (it needs nothing special: two
# `export`s and an `exec`), and sets LD_PRELOAD=path-redirect.so +
# DN_INSTDIR only for the dash process it then execs -- NOT for dpkg
# itself, which must never see that LD_PRELOAD: dpkg is a Bionic
# process, and Bionic's linker refuses to even start with a glibc .so in
# LD_PRELOAD ("CANNOT LINK EXECUTABLE ... library libc.so.6 not found"
# -- confirmed by direct test). dash itself must already be
# ELF-patched (grun --configure) before this can work.
#
# Usage: patch-maintainer-scripts.sh ADMINDIR INSTDIR
# Run after dpkg --unpack and grun-patching new ELFs, before --configure.
set -eu
ADMINDIR=${1:?usage: patch-maintainer-scripts.sh ADMINDIR INSTDIR}
INSTDIR=${2:?usage: patch-maintainer-scripts.sh ADMINDIR INSTDIR}
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
"$HERE/setup-runtime.sh" "$INSTDIR"
WRAPPER="$INSTDIR/usr/bin/dn-shell"

[ -d "$ADMINDIR/info" ] || exit 0
for f in "$ADMINDIR"/info/*.postinst "$ADMINDIR"/info/*.preinst \
         "$ADMINDIR"/info/*.postrm "$ADMINDIR"/info/*.prerm; do
  [ -f "$f" ] || continue
  # No idempotency marker needed -- see patch-deb.sh's identical comment:
  # the shebang rewrite below is naturally idempotent, and this pass
  # exists specifically to give a script a second chance once the
  # wrapper exists, if it didn't yet when patch-deb.sh first touched it.
  #
  # NOT rewriting literal path text anymore -- see patch-deb.sh's
  # identical comment for the full reason (many maintainer scripts,
  # base-files included, are already $DPKG_ROOT-aware; rewriting a
  # literal path on top of that double-prefixes it once the script's own
  # concatenation also applies). The runtime LD_PRELOAD shim covers a
  # script with no $DPKG_ROOT awareness at all.
  # Allow (and preserve) a trailing flag on the shebang -- see
  # patch-deb.sh's identical comment (openssl's "#!/bin/sh -e" is the
  # real case this was missing).
  # \s/\S are PCRE, not POSIX ERE -- silently never matched with them.
  shebang=$(head -1 "$f")
  if printf '%s' "$shebang" | grep -qE '^#![[:space:]]*/bin/(sh|bash|dash)([[:space:]]+[^[:space:]]+)?[[:space:]]*$'; then
    # NOT using # as the sed delimiter here: the pattern itself starts
    # with a literal "#" (matching the shebang's own "#!"), which broke
    # delimiter parsing outright ("sed: unknown option to `s'") and, under
    # this script's `set -eu`, silently killed the ENTIRE run partway
    # through its file loop -- any script alphabetically after the one
    # being processed when this hit (here: openssl.postinst) never got
    # touched at all, in every run, looking like the mechanism just
    # didn't work rather than a shell script bug.
    flag=$(printf '%s' "$shebang" | sed -E 's,^#![[:space:]]*/bin/(sh|bash|dash)[[:space:]]*([^[:space:]]*)[[:space:]]*$,\2,')
    sed -i "1s#.*#\#!${WRAPPER}${flag:+ }${flag}#" "$f"
    # No `unset LD_PRELOAD` line any more -- see patch-deb.sh's comment: the
    # shim's execve() dispatch keeps LD_PRELOAD for glibc targets and strips
    # it for Bionic ones, so neither a forked glibc coreutils command nor a
    # forked Bionic command needs the shell to clear it first.
  fi
done
