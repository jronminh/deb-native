#!/bin/sh
# The single entry point: set up a Debian glibc prefix under Termux and
# install packages into it, then make them runnable by name.
#
# Usage:
#   ./install.sh                      # bootstrap the base env into ~/.dn
#   ./install.sh DNPREFIX               # ... into DNPREFIX
#   ./install.sh DNPREFIX pkg [pkg...]  # bootstrap/reuse, then install pkgs
#
# Idempotent: an existing prefix is reused, not re-bootstrapped. After this,
# a new shell finds the installed programs on PATH.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DNPREFIX=${1:-$HOME/.dn}
[ $# -gt 0 ] && shift
case "$DNPREFIX" in
  /*) ;;
  *) DNPREFIX="$PWD/$DNPREFIX" ;;
esac

if [ ! -s "$DNPREFIX/var/lib/dpkg/status" ]; then
  echo "==> bootstrapping a Debian glibc base into $DNPREFIX"
  "$HERE/scripts/setup-apt-prefix.sh" "$DNPREFIX"
else
  echo "==> reusing existing prefix $DNPREFIX"
  "$HERE/scripts/make-launchers.sh" "$DNPREFIX/root"
  "$HERE/scripts/dn-activate.sh" "$DNPREFIX/root"
fi

if [ $# -gt 0 ]; then
  echo "==> installing: $*"
  "$HERE/scripts/apt-install.sh" "$DNPREFIX" "$@"
fi

echo "==> done. Installed programs run by name in a new shell (or: . ~/.bashrc)"
