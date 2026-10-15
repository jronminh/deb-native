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
# shellcheck disable=SC2086
exec "$root/usr/lib/deb-native/dn-tiny" -q ${DN_TINY_FLAGS:-} -t "$root" -- /usr/bin/bash /boot/init.sh "$@"
