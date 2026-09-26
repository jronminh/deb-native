#!/bin/sh
# deb-native installer.
#   curl -fsSL https://raw.githubusercontent.com/jronminh/deb-native/main/install.sh | sh
#   sh install.sh [PREFIX] [pkg ...]        # from a checkout
#
# Sets up a Debian glibc prefix under Termux, installs packages into it, and
# makes them runnable by name. Idempotent: an existing prefix is reused.
set -eu
REPO=https://github.com/jronminh/deb-native
DIR=${DEB_NATIVE_DIR:-$HOME/.deb-native}
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" 2>/dev/null && pwd || echo .)

# Piped (curl | sh) or run outside a checkout: fetch the repo, then re-exec.
if [ ! -f "$HERE/scripts/setup-apt-prefix.sh" ]; then
    command -v git >/dev/null 2>&1 || { echo "error: git is required (pkg install git)"; exit 1; }
    echo "==> fetching deb-native into $DIR"
    [ -d "$DIR/.git" ] || git clone --depth 1 "$REPO" "$DIR"
    exec sh "$DIR/install.sh" "$@"
fi

DNPREFIX=${1:-$HOME/.dn}
[ $# -gt 0 ] && shift
case "$DNPREFIX" in /*) ;; *) DNPREFIX="$PWD/$DNPREFIX" ;; esac

if [ ! -s "$DNPREFIX/var/lib/dpkg/status" ]; then
    echo "==> bootstrapping a Debian glibc base into $DNPREFIX"
    "$HERE/scripts/setup-apt-prefix.sh" "$DNPREFIX"
else
    echo "==> reusing existing prefix $DNPREFIX"
    "$HERE/scripts/setup-runtime.sh" "$DNPREFIX/root"
    "$HERE/scripts/make-launchers.sh" "$DNPREFIX/root"
    "$HERE/scripts/make-apt-wrappers.sh" "$DNPREFIX/root"
    "$HERE/scripts/dn-activate.sh" "$DNPREFIX/root"
fi

if [ $# -gt 0 ]; then
    echo "==> installing: $*"
    "$HERE/scripts/apt-install.sh" "$DNPREFIX" "$@"
fi

echo "==> normalizing prefix symlinks (bind-only tracer)"
"$HERE/scripts/normalize-symlinks.sh" "$DNPREFIX/root"

echo "==> done. Installed programs run by name in a new shell (or: . ~/.bashrc)"
