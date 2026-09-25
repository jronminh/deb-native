#!/bin/sh
# Install a package (and its real dependencies) into a prefix set up by
# setup-apt-prefix.sh, then patch every ELF binary the transaction added
# with Termux's own grun --configure (safe to run on a plain .so too —
# it just warns "cannot find section .interp" and exits 0, no interpreter
# to patch, verified by hand before relying on it here).
#
# Usage: apt-install.sh $NEWPREFIX package [package...]
set -eu
NEWPREFIX=${1:?usage: apt-install.sh NEWPREFIX package...}
shift

APT_CONFIG="$NEWPREFIX/etc/apt.conf" apt-get install -y --no-install-recommends "$@"

find "$NEWPREFIX/root" -type f -perm -u+x 2>/dev/null | while IFS= read -r f; do
  case "$(head -c4 "$f" 2>/dev/null | od -An -tx1 | tr -d ' \n')" in
    7f454c46) grun --configure "$f" >/dev/null 2>&1 || true ;;
  esac
done
