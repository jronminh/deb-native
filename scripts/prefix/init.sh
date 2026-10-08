#!/bin/sh
# The prefix's init.  The host boots the tree by running this through dn-trace,
# the tree's root process (docs/spec/prefix.md, "Boot"):
#
#   dn-trace --rt-loader <loader> --syscalls <catalog> -- \
#       usr/lib/deb-native/init.sh [COMMAND [ARG...]]
#
# It sets up the environment every in-tree program needs and, on a first boot
# (no .dn/bootstrapped), completes the prefix from .dn/profile by itself --
# so the host has no separate bootstrap step and no tree program runs outside
# dn-trace.  Then it execs COMMAND, or an interactive shell by default.
set -eu
# Find our own tree with builtins and string ops only: PATH is not set yet,
# the runtime hides every path outside the tree, and `..` is not reliable here.
HERE=${0%/*}
[ "$HERE" = "$0" ] && HERE=.
DN=${HERE%/usr/lib/deb-native}
TREE=$DN

PATH="$DN/usr/lib/deb-native/priv:$DN/usr/sbin:$DN/usr/bin:$DN/sbin:$DN/bin"
export PATH
HOME=$DN/root
export HOME
TMPDIR=$DN/tmp
export TMPDIR
mkdir -p "$TMPDIR"
chmod 1777 "$TMPDIR" 2>/dev/null || true

# A resolver for glibc.  Android provides no /etc/resolv.conf; the artifact
# ships it as a link to the host's file when it can, else a fixed server.
[ -e "$TREE/etc/resolv.conf" ] || printf 'nameserver 8.8.8.8\n' > "$TREE/etc/resolv.conf"
# Debian's standard "do not start a service on install" hook.
if [ ! -e "$TREE/usr/sbin/policy-rc.d" ]; then
  mkdir -p "$TREE/usr/sbin"
  printf '#!/bin/sh\nexit 101\n' > "$TREE/usr/sbin/policy-rc.d"
  chmod 755 "$TREE/usr/sbin/policy-rc.d"
fi

# First boot: complete the prefix (apt update + install .dn/profile).  A later
# boot finds .dn/bootstrapped and skips straight to the command.
if [ -f "$TREE/.dn/profile" ] && [ ! -f "$TREE/.dn/bootstrapped" ]; then
  "$DN/usr/lib/deb-native/scripts/runtime/bootstrap-prefix.sh"
fi

if [ $# -gt 0 ]; then
  exec "$@"
fi
exec "$DN/usr/bin/bash" -i
