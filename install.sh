#!/bin/sh
# deb-native installer.
#   curl -fsSL https://raw.githubusercontent.com/jronminh/deb-native/main/install.sh | sh
#   sh install.sh [PREFIX] [pkg ...]        # from a checkout
#
# Pin a release (clones that tag/ref) instead of rolling main:
#   curl -fsSL .../v0.0.1-prealpha/install.sh | DEB_NATIVE_REF=v0.0.1-prealpha sh
#
# Sets up a Debian glibc prefix under Termux, installs packages into it, and
# makes them runnable by name. Idempotent: an existing prefix is reused.
set -eu
REPO=https://github.com/jronminh/deb-native
REF=${DEB_NATIVE_REF:-main}
DIR=${DEB_NATIVE_DIR:-$HOME/.deb-native}
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" 2>/dev/null && pwd || echo .)

# --- display ---------------------------------------------------------------
if [ -t 1 ]; then
    B=$(printf '\033[1m'); D=$(printf '\033[2m'); C=$(printf '\033[36m')
    G=$(printf '\033[32m'); Y=$(printf '\033[33m'); R=$(printf '\033[0m')
else
    B= D= C= G= Y= R=
fi
banner() {
    printf '%s\n' "${B}deb-native${R} ${D}·${R} Debian arm64 .debs in Termux, unrooted"
}
step() {
    STEP=$((STEP + 1))
    printf '\n%s[%s/%s]%s %s%s%s\n' "$C" "$STEP" "$TOTAL" "$R" "$B" "$*" "$R"
}
kv() { printf '   %s%-10s%s %s\n' "$D" "$1" "$R" "$2"; }
fail() { printf '%s error:%s %s\n' "$Y" "$R" "$*" >&2; exit 1; }

# Piped (curl | sh) or run outside a checkout: fetch the repo, then re-exec.
if [ ! -f "$HERE/scripts/setup-apt-prefix.sh" ]; then
    banner
    command -v git >/dev/null 2>&1 || fail "git is required (pkg install git)"
    printf '\n%s[1/1]%s fetching deb-native (%s) into %s\n' "$C" "$R" "$REF" "$DIR"
    if [ ! -d "$DIR/.git" ]; then
        git clone --depth 1 --branch "$REF" "$REPO" "$DIR"
    elif [ "$REF" != "main" ]; then
        git -C "$DIR" fetch -q --depth 1 origin "$REF" && git -C "$DIR" checkout -q FETCH_HEAD
    fi
    exec sh "$DIR/install.sh" "$@"
fi

DNPREFIX=${1:-$HOME/.dn}
[ $# -gt 0 ] && shift
case "$DNPREFIX" in /*) ;; *) DNPREFIX="$PWD/$DNPREFIX" ;; esac

# Never install into Termux's own prefix: setup-apt-prefix.sh writes
# sources.list under $DNPREFIX/etc/apt, which would overwrite Termux's and
# make its repo disappear.
TERMUX_PREFIX=${DN_TERMUX_PREFIX:-${PREFIX:-/data/data/com.termux/files/usr}}
case "$DNPREFIX" in
  "$TERMUX_PREFIX"|"$TERMUX_PREFIX"/*)
    printf '%s error: refusing prefix %s%s\n' "$Y" "$DNPREFIX" "$R" >&2
    printf '   it is inside Termux'"'"'s prefix (%s); that would clobber Termux'"'"'s apt.\n' "$TERMUX_PREFIX" >&2
    printf '   use a separate prefix, e.g. \$HOME/.dn (the default).\n' >&2
    exit 1 ;;
esac

# --- run -------------------------------------------------------------------
STEP=0
TOTAL=2
[ $# -gt 0 ] && TOTAL=3
T0=$(date +%s)

banner
kv "prefix" "$DNPREFIX"
[ $# -gt 0 ] && kv "packages" "$*"

if [ ! -s "$DNPREFIX/var/lib/dpkg/status" ]; then
    step "bootstrapping the Debian glibc base"
    "$HERE/scripts/setup-apt-prefix.sh" "$DNPREFIX"
else
    step "refreshing the existing prefix"
    kv "state" "reused (already bootstrapped)"
    "$HERE/scripts/setup-runtime.sh" "$DNPREFIX/root"
    "$HERE/scripts/make-launchers.sh" "$DNPREFIX/root"
    "$HERE/scripts/make-apt-wrappers.sh" "$DNPREFIX/root"
    "$HERE/scripts/dn-activate.sh" "$DNPREFIX/root"
fi

if [ $# -gt 0 ]; then
    step "installing: $*"
    "$HERE/scripts/apt-install.sh" "$DNPREFIX" "$@"
fi

step "normalizing prefix symlinks"
"$HERE/scripts/normalize-symlinks.sh" "$DNPREFIX/root"

# --- summary ---------------------------------------------------------------
T1=$(date +%s)
secs=$((T1 - T0))
[ "$secs" -lt 60 ] && took="${secs}s" || took="$((secs / 60))m$((secs % 60))s"
installed=$(grep -c "install ok installed" "$DNPREFIX/var/lib/dpkg/status" 2>/dev/null || true)

printf '\n%s done%s in %s\n' "$G" "$R" "$took"
kv "prefix" "$DNPREFIX"
kv "installed" "${installed:-0} packages"
kv "run" "a program by name in a new shell (or: . ~/.bashrc)"
kv "check" "termux-dn-doctor"
