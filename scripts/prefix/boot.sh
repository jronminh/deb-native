#!/system/bin/sh
# The prefix's entry, boot/boot.sh: start dn-tiny, the tracer, as the tree's root
# process, against this prefix and its init (docs/building.md, "The prefix").
#
# The host runs it from anywhere; the prefix root is this file's directory's
# parent, so a prefix can be moved or copied as it is.  The shebang is Android's
# shell (there is no /bin/sh on the device); `sh boot/boot.sh` works too.  Any
# arguments are the command to run in the prefix (default: an interactive shell).
#
#   DN_TINY_FLAGS   extra dn-tiny options, e.g. "-s 500" to poll 500 us for the
#                   next stop before sleeping (faster on stop-heavy work, more CPU)
root=$(CDPATH= cd -- "${0%/*}/.." && pwd) || exit 1
# The host's loader knobs are for its own libc: Termux's LD_PRELOAD is a bionic
# library, and the prefix's glibc loader fails on it before init.sh could drop it.
unset LD_PRELOAD LD_LIBRARY_PATH
# A command starts in the tree's root and under Debian's umask: the host's
# working directory (which pwd and /proc/self/cwd would show) and its 077 umask
# must not reach it.
cd "$root" || exit 1
umask 022
# A clean environment, as login builds it: the host's PATH, ANDROID_* and the
# rest do not pass.  TERM and COLORTERM are kept (a terminal the host made);
# TZ is the phone's.
tz=$(getprop persist.sys.timezone 2>/dev/null)
# shellcheck disable=SC2086
exec env -i \
  PATH=/usr/sbin:/usr/bin:/sbin:/bin \
  HOME=/root \
  SHELL=/usr/bin/bash \
  LANG=C.UTF-8 \
  TMPDIR=/tmp \
  TERM="${TERM:-dumb}" \
  COLORTERM="$COLORTERM" \
  TZ="${tz:-UTC}" \
  "$root/usr/lib/deb-native/dn-tiny" -q ${DN_TINY_FLAGS:-} -t "$root" -- /usr/bin/bash /boot/init.sh "$@"
