#!/bin/sh
# The prefix's init: dn-tiny, the tree's root process, runs it (boot/boot.sh):
#
#   dn-tiny -t ROOT -- /usr/bin/bash /boot/init.sh [COMMAND [ARG...]]
#
# Everything here uses guest paths: to the runtime the tree is "/".  It sets up
# the environment, then runs the command (default: an interactive shell).
set -eu
umask 022                          # as on Debian; the app's is 077
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH
# The app's real uid/gid (the only real kernel identity), kept before the DN_*
# knobs are dropped; init.sh uses them for /etc/passwd and $HOME.
real_uid=${DN_REAL_UID:-}
real_gid=${DN_REAL_GID:-}
# The host's environment reaches the tree through dn-tiny: drop the host's
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
# One real uid: the app's.  Its /etc/passwd, group and shadow lines are virtual
# identity, written once; in user mode the session starts in its home.
if [ -n "$real_uid" ]; then
  [ -n "$real_gid" ] || real_gid=$real_uid
  if ! getent passwd "$real_uid" >/dev/null 2>&1; then
    printf 'user:x:%s:%s:user:/home/user:/bin/bash\n' "$real_uid" "$real_gid" >> /etc/passwd
  fi
  if ! getent group "$real_gid" >/dev/null 2>&1; then
    printf 'user:x:%s:\n' "$real_gid" >> /etc/group
  fi
  if [ -f /etc/shadow ] && ! getent shadow user >/dev/null 2>&1; then
    printf 'user:!:20000:0:99999:7:::\n' >> /etc/shadow
  fi
fi
if [ "$(id -u)" = "0" ]; then
  HOME=/root
else
  HOME=/home/user
  USER=user
  LOGNAME=user
  export USER LOGNAME
  mkdir -p "$HOME" 2>/dev/null || true
fi
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

if [ $# -gt 0 ]; then
  exec "$@"
fi
# An interactive shell starts in the home directory, as on Debian (~).
cd "$HOME" 2>/dev/null || true
exec /usr/bin/bash -i
