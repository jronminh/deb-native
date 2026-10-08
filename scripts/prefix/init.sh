#!/bin/sh
# The prefix's init.  The host boots the tree by running this through dn-trace,
# the tree's root process (docs/spec/prefix.md, "Boot"):
#
#   dn-trace <TREE> <TREE>/<loader> -- \
#       /usr/bin/bash /usr/lib/deb-native/init.sh [COMMAND [ARG...]]
#
# Everything here uses **guest paths**: to the runtime the tree is "/", and
# dn-policy maps each guest path back into the tree.  It sets up the
# environment and, on a first boot (no /.dn/bootstrapped), completes the
# prefix from /.dn/profile by itself, then runs the command (default: an
# interactive shell).
set -eu
PATH=/usr/lib/deb-native/priv:/usr/sbin:/usr/bin:/sbin:/bin
export PATH
# The host's environment reaches the tree through dn-trace: drop the host's
# deb-native knobs and its locale (the tree ships no locales but C.UTF-8).
for v in $(env | sed -n 's/^\(DN_[A-Za-z0-9_]*\)=.*/\1/p'); do
  unset "$v"
done
unset LC_ALL LANGUAGE
LANG=C.UTF-8
export LANG
HOME=/root
export HOME
TMPDIR=/tmp
export TMPDIR
mkdir -p "$TMPDIR"
chmod 1777 "$TMPDIR" 2>/dev/null || true

# A resolver for glibc: Android provides no /etc/resolv.conf.  A link that
# does not resolve in the tree is replaced.
if [ ! -e /etc/resolv.conf ]; then
  rm -f /etc/resolv.conf
  printf 'nameserver 8.8.8.8\n' > /etc/resolv.conf
fi
# Debian's standard "do not start a service on install" hook.
if [ ! -e /usr/sbin/policy-rc.d ]; then
  mkdir -p /usr/sbin
  printf '#!/bin/sh\nexit 101\n' > /usr/sbin/policy-rc.d
  chmod 755 /usr/sbin/policy-rc.d
fi

# First boot: complete the prefix (apt update + install /.dn/profile), then
# mark it done.  A later boot skips straight to the command.
if [ -f /.dn/profile ] && [ ! -f /.dn/bootstrapped ]; then
  /usr/bin/bash /usr/lib/deb-native/scripts/runtime/bootstrap-prefix.sh
fi

if [ $# -gt 0 ]; then
  exec "$@"
fi
exec /usr/bin/bash -i
