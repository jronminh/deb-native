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
SHIM="$HERE/../native/path-redirect.so"
DASH="$INSTDIR/usr/bin/dash"
WRAPPER="$INSTDIR/usr/bin/dn-dash"

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

[ -d "$ADMINDIR/info" ] || exit 0
for f in "$ADMINDIR"/info/*.postinst "$ADMINDIR"/info/*.preinst \
         "$ADMINDIR"/info/*.postrm "$ADMINDIR"/info/*.prerm; do
  [ -f "$f" ] || continue
  # Only rewrite a path when it's a whole path component (preceded by
  # space, =, quote, or "("), not inside some other word, so
  # "musr/foo" or a variable named "$usr_dir" are left alone.
  sed -i -E "s#([ =\"'(])/(etc|usr|var|opt)/#\1${INSTDIR}/\2/#g" "$f"

  if [ -x "$WRAPPER" ] && head -1 "$f" | grep -qE '^#!\s*/bin/(sh|bash|dash)\s*$'; then
    sed -i "1s#.*#\#!${WRAPPER}#" "$f"
    # Drop LD_PRELOAD from dash's own environment as its very first
    # action, so a command the script forks (cp, ln, ...) -- often a
    # Bionic binary found via PATH -- doesn't inherit a glibc .so in its
    # LD_PRELOAD (Bionic's linker refuses to even start in that case:
    # "CANNOT LINK EXECUTABLE ... libc.so.6 not found", confirmed the
    # hard way). dash's OWN interposition, already resolved at process
    # load time, keeps working regardless -- this only affects what
    # children fork with afterward.
    sed -i "2i unset LD_PRELOAD 2>/dev/null || true" "$f"
  fi
done
