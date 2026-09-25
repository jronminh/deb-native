#!/bin/sh
# Rewrite hardcoded absolute paths in a package's maintainer scripts,
# between dpkg's --unpack and --configure, so a script like debconf's
# `. /usr/share/debconf/confmodule` or openssl's `ln -s ... /usr/lib/ssl`
# finds the file where it actually landed ($INSTDIR), instead of at a
# real absolute path Android doesn't have.
#
# Replaces the LD_PRELOAD approach in design-manual-overlay.md's Bionic
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
# Usage: patch-maintainer-scripts.sh ADMINDIR INSTDIR
# Run after `dpkg --unpack`, before `dpkg --configure`.
set -eu
ADMINDIR=${1:?usage: patch-maintainer-scripts.sh ADMINDIR INSTDIR}
INSTDIR=${2:?usage: patch-maintainer-scripts.sh ADMINDIR INSTDIR}

[ -d "$ADMINDIR/info" ] || exit 0
for f in "$ADMINDIR"/info/*.postinst "$ADMINDIR"/info/*.preinst \
         "$ADMINDIR"/info/*.postrm "$ADMINDIR"/info/*.prerm; do
  [ -f "$f" ] || continue
  # Only rewrite a path when it's a whole path component (preceded by
  # space, =, quote, or "("), not inside some other word, so
  # "musr/foo" or a variable named "$usr_dir" are left alone.
  sed -i -E "s#([ =\"'(])/(etc|usr|var|opt)/#\1${INSTDIR}/\2/#g" "$f"
done
