#!/bin/sh
# The prefix's init.  dn-monitor, the tree's root process, runs this -- reached
# through boot/boot.sh, the host's entry (docs/spec/prefix.md, "The boot/
# directory"):
#
#   dn-monitor -- /usr/bin/bash /boot/init.sh [COMMAND [ARG...]]
#
# Everything here uses **guest paths**: to the runtime the tree is "/", and
# dn-policy maps each guest path back into the tree.  It sets up the
# environment and, on a first boot (no /var/lib/deb-native/bootstrapped), completes the
# prefix from /var/lib/deb-native/profile by itself, then runs the command (default: an
# interactive shell).
set -eu
PATH=/usr/lib/deb-native/priv:/usr/sbin:/usr/bin:/sbin:/bin
export PATH
# The host's environment reaches the tree through dn-monitor: drop the host's
# deb-native knobs and its locale (the tree ships no locales but C.UTF-8).
for v in $(env | sed -n 's/^\(DN_[A-Za-z0-9_]*\)=.*/\1/p'); do
  unset "$v"
done
unset LC_ALL LANGUAGE
LANG=C.UTF-8
export LANG
# glibc reads C.UTF-8 file by file, trying the directory C.UTF-8/ before
# C.utf8/ for every category of every process; under the tracer each failed
# open is a stop.  localedef will not put C.utf8 in the archive, so the
# spelling glibc tries first is made to exist.
if [ -d /usr/lib/locale/C.utf8 ] && [ ! -e /usr/lib/locale/C.UTF-8 ] && [ ! -L /usr/lib/locale/C.UTF-8 ]; then
  ln -s C.utf8 /usr/lib/locale/C.UTF-8 2>/dev/null || true
fi
# Android's filter kills rseq, and glibc registers it in every process: a
# SIGSYS stop each, for nothing.  Turn the registration off.
GLIBC_TUNABLES=${GLIBC_TUNABLES:+$GLIBC_TUNABLES:}glibc.pthread.rseq=0
export GLIBC_TUNABLES
# No tty and no dialog/hterm here: debconf would otherwise probe Dialog,
# Readline and Teletype first and print a warning for each before falling back.
DEBIAN_FRONTEND=noninteractive
export DEBIAN_FRONTEND
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

# First boot: complete the prefix (apt update + install /var/lib/deb-native/profile), then
# mark it done.  A later boot skips straight to the command.
if [ -f /var/lib/deb-native/profile ] && [ ! -f /var/lib/deb-native/bootstrapped ]; then
  /usr/bin/bash /boot/bootstrap.sh
fi

if [ $# -gt 0 ]; then
  exec "$@"
fi
# An interactive shell starts in the home directory, as on Debian (~).
cd "$HOME" 2>/dev/null || true
exec /usr/bin/bash -i
