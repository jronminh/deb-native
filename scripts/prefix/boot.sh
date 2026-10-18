#!/system/bin/sh
# The prefix's entry, boot/boot.sh: start dn-tiny, the tracer, as the tree's root
# process, against this prefix and its init (docs/building.md, "The prefix").
#
# The host runs it from anywhere; the prefix root is this file's directory's
# parent, so a prefix can be moved or copied as it is.  The shebang is Android's
# shell (there is no /bin/sh on the device); `sh boot/boot.sh` works too.  Any
# arguments are the command to run in the prefix (default: an interactive shell).
#
#   DN_TINY_FLAGS   extra dn-tiny options; the default is -s 500 (poll 500 us
#                   for the next stop: faster when work spawns or stops a lot,
#                   a few percent slower on a parallel build).  "-s 0" sleeps.
root=$(CDPATH= cd -- "${0%/*}/.." && pwd) || exit 1
# The host's loader knobs are for its own libc: Termux's LD_PRELOAD is a bionic
# library, and the prefix's glibc loader fails on it before init.sh could drop it.
unset LD_PRELOAD LD_LIBRARY_PATH
# A command starts in the tree's root and under Debian's umask: the host's
# working directory (which pwd and /proc/self/cwd would show) and its 077 umask
# must not reach it.
cd "$root" || exit 1
umask 022
# The identity the session starts from: the app's real uid (the default, a user
# session) or root (a test session).  Behaviour is classified per syscall from
# the process's current euid; there is no mode switch.  The real ids go to
# init.sh for /etc/passwd and $HOME.
initial=${DN_INITIAL:-user}
case $initial in
user|root) ;;
*) echo "boot.sh: DN_INITIAL is user or root" >&2; exit 1 ;;
esac
real_uid=$(id -u 2>/dev/null)
real_gid=$(id -g 2>/dev/null)
# A clean environment, as login builds it: the host's PATH, ANDROID_* and the
# rest do not pass.  TERM and COLORTERM are kept (a terminal the host made);
# TZ is the phone's.
tz=$(getprop persist.sys.timezone 2>/dev/null)
# boot/env (optional): the host names a variable to keep from its own
# environment (one NAME per line) or one to set (NAME=VALUE).  It is read as
# data, never sourced; each line becomes an export in the clean shell, so it
# wins over the defaults above.  A bad name refuses the boot.
envfile=
if [ -f "$root/boot/env" ]; then
  envfile="$root/tmp/boot-env.$$"
  mkdir -p "$root/tmp" 2>/dev/null
  ( umask 077
    while IFS= read -r line || [ -n "$line" ]; do
      case $line in ''|'#'*) continue ;; esac
      name=${line%%=*}
      case $name in
      DN_INITIAL|DN_REAL_UID|DN_REAL_GID)
        echo "boot.sh: boot/env: $name is set by boot.sh" >&2; exit 1 ;;
      [!A-Za-z_]*|*[!A-Za-z0-9_]*)
        echo "boot.sh: boot/env: bad name: $name" >&2; exit 1 ;;
      esac
      case $line in
      *=*) printf '%s\n' "$line" ;;
      *)   printf '%s=%s\n' "$name" "$(printenv "$name")" ;;
      esac
    done < "$root/boot/env"
  ) > "$envfile" || { rm -f "$envfile"; exit 1; }
fi
if [ -n "$envfile" ]; then
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
    DN_REAL_UID="${real_uid:-}" DN_REAL_GID="${real_gid:-}" \
    DN_BOOT_ENV="$envfile" DN_BOOT_ROOT="$root" DN_BOOT_FLAGS="${DN_TINY_FLAGS:-}" \
    DN_BOOT_INITIAL="$initial" \
    /system/bin/sh -c '
      e=$DN_BOOT_ENV r=$DN_BOOT_ROOT f=$DN_BOOT_FLAGS i=$DN_BOOT_INITIAL
      while IFS= read -r line || [ -n "$line" ]; do
        export "$line" || exit 1
      done < "$e"
      rm -f "$e"
      unset DN_BOOT_ENV DN_BOOT_ROOT DN_BOOT_FLAGS DN_BOOT_INITIAL
      exec "$r/usr/lib/deb-native/dn-tiny" -q -s 500 $f \
        --initial "$i" -t "$r" -- /usr/bin/bash /boot/init.sh "$@"
    ' sh "$@"
fi
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
  DN_REAL_UID="${real_uid:-}" DN_REAL_GID="${real_gid:-}" \
  "$root/usr/lib/deb-native/dn-tiny" -q -s 500 ${DN_TINY_FLAGS:-} --initial "$initial" \
  -t "$root" -- /usr/bin/bash /boot/init.sh "$@"
