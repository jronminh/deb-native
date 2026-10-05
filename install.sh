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
# Termux defaults to 077; the prefix's directories should be Debian's usual
# 755 (var/ was created 700 by the log step below).
umask 022
REPO=https://github.com/jronminh/deb-native
REF=${DEB_NATIVE_REF:-main}
DIR=${DEB_NATIVE_DIR:-$HOME/.deb-native}
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd || echo .)

# --- display ---------------------------------------------------------------
# The terminal shows only the banner, the stages, progress and errors; the
# full output goes to the log (see "log" below). Under that log, lines meant
# for the terminal are marked "::show" for the outer pass to display.
if [ -t 1 ] || [ "${DN_COLOR:-}" = 1 ]; then
    B=$(printf '\033[1m'); D=$(printf '\033[2m'); C=$(printf '\033[36m')
    G=$(printf '\033[32m'); Y=$(printf '\033[33m'); R=$(printf '\033[0m')
else
    B= D= C= G= Y= R=
fi
out() {
    if [ -n "${DN_INSTALL_LOG:-}" ]; then printf '::show %s\n' "$1"; else printf '%s\n' "$1"; fi
}
banner() {
    out "${B}deb-native${R} ${D}·${R} Debian arm64 .debs in Termux, unrooted"
}
step() {
    STEP=$((STEP + 1))
    out ""
    out "$(printf '%s[%s/%s]%s %s%s%s' "$C" "$STEP" "$TOTAL" "$R" "$B" "$*" "$R")"
}
kv() { out "$(printf '   %s%-10s%s %s' "$D" "$1" "$R" "$2")"; }
fail() { printf '%s error:%s %s\n' "$Y" "$R" "$*" >&2; exit 1; }

# Piped (curl | sh) or run outside a checkout: fetch the repo, then re-exec.
if [ ! -f "$HERE/scripts/bootstrap/setup-apt-prefix.sh" ]; then
    banner
    command -v git || fail "git is required (pkg install git)"
    printf '\n%s[1/1]%s fetching deb-native (%s) into %s\n' "$C" "$R" "$REF" "$DIR"
    if [ ! -d "$DIR/.git" ]; then
        git clone --depth 1 --branch "$REF" "$REPO" "$DIR"
    elif [ "$REF" != "main" ]; then
        git -C "$DIR" fetch -q --depth 1 origin "$REF" && git -C "$DIR" checkout -q FETCH_HEAD
    fi
    exec sh "$DIR/install.sh" "$@"
fi

TERMUX_PREFIX=${DN_TERMUX_PREFIX:-${PREFIX:-/data/data/com.termux/files/usr}}
# Default beside Termux's own trees (usr/, home/): a prefix under $HOME
# plus its $DN/root -> $HOME symlink would make $HOME recurse infinitely
# for anything that follows symlinks (find -L, file pickers, LSPs).
DNPREFIX=${1:-$(dirname "$TERMUX_PREFIX")/deb-native}
[ $# -gt 0 ] && shift
case "$DNPREFIX" in /*) ;; *) DNPREFIX="$PWD/$DNPREFIX" ;; esac

# Never install into Termux's own prefix: setup-apt-prefix.sh writes
# sources.list under $DNPREFIX/etc/apt, which would overwrite Termux's and
# make its repo disappear.
case "$DNPREFIX" in
  "$TERMUX_PREFIX"|"$TERMUX_PREFIX"/*)
    printf '%s error: refusing prefix %s%s\n' "$Y" "$DNPREFIX" "$R" >&2
    printf '   it is inside Termux'"'"'s prefix (%s); that would clobber Termux'"'"'s apt.\n' "$TERMUX_PREFIX" >&2
    printf '   use a separate prefix, e.g. %s (the default).\n' "$(dirname "$TERMUX_PREFIX")/deb-native" >&2
    exit 1 ;;
esac

# --- log -------------------------------------------------------------------
# The first pass re-runs this script and reads its output line by line:
# everything goes to a log file in the prefix (to read back or share); the
# terminal gets only what the scripts mark for it:
#   ::show TEXT              a line of installer UI (banner, steps, summary)
#   ::stage TEXT             a stage, as "   - TEXT"
#   ::progress I N LABEL     a progress bar, redrawn in place
#   ::count PREFIX N LABEL   a progress bar advanced by each following output
#                            line starting with PREFIX ("Unpacking", "Get:");
#                            N "auto": taken from apt's "N newly installed"
# plus any "E: " error line. The real exit code is kept.
render() {
    exec 3>>"$DN_INSTALL_LOG"
    tty=0; [ -t 1 ] && tty=1
    bar=0 cprefix="" cn=0 ci=0 clabel=""
    draw() {  # I N LABEL
        [ "$tty" = 1 ] || { [ "$1" = "$2" ] && printf '     %s/%s %s\n' "$1" "$2" "$3"; return 0; }
        w=20 f=$(( $1 * 20 / ($2 > 0 ? $2 : 1) )) k=0 b=""
        while [ "$k" -lt "$f" ]; do b="$b#"; k=$((k + 1)); done
        while [ "$k" -lt "$w" ]; do b="$b-"; k=$((k + 1)); done
        printf '\r\033[K     [%s] %s/%s %.40s' "$b" "$1" "$2" "$3"
        bar=1
        [ "$1" = "$2" ] && { printf '\n'; bar=0; }
        return 0
    }
    endbar() { [ "$bar" = 1 ] && { printf '\n'; bar=0; }; return 0; }
    while IFS= read -r line || [ -n "$line" ]; do
        case "$line" in
            "::show "*|"::show")
                endbar; t=${line#::show}; t=${t# }
                printf '%s\n' "$t"; printf '%s\n' "$t" | sed 's/\x1b\[[0-9;]*m//g' >&3 ;;
            "::stage "*)
                endbar; cprefix=""; t=${line#::stage }
                printf '   - %s\n' "$t"; printf '== %s %s\n' "$(date +%T)" "$t" >&3 ;;
            "::progress "*)
                set -- ${line#::progress }; i=$1 n=$2; shift 2
                draw "$i" "$n" "$*" ;;
            "::count "*)
                set -- ${line#::count }; cprefix=$1 cn=$2; shift 2
                clabel=$* ci=0 cauto=0
                [ "$cn" = auto ] && { cauto=1; cn=0; }
                [ "$cn" -gt 0 ] && draw 0 "$cn" "$clabel" ;;
            "E: "*)
                endbar; printf '%s\n' "$line"; printf '%s\n' "$line" >&3 ;;
            *)
                printf '%s\n' "$line" >&3
                if [ "${cauto:-0}" = 1 ]; then
                    case "$line" in
                        *" newly installed"*)
                            cn=$(printf '%s\n' "$line" | sed -n 's/.* \([0-9][0-9]*\) newly installed.*/\1/p')
                            cauto=0; [ "${cn:-0}" -gt 0 ] && draw 0 "$cn" "$clabel" ;;
                    esac
                fi
                if [ -n "$cprefix" ] && [ "$cn" -gt 0 ]; then
                    case "$line" in
                        "$cprefix"*) [ "$ci" -lt "$cn" ] && { ci=$((ci + 1)); draw "$ci" "$cn" "$clabel"; } ;;
                    esac
                fi ;;
        esac
    done
    endbar
}
if [ -z "${DN_INSTALL_LOG:-}" ]; then
    mkdir -p "$DNPREFIX/var/log"
    DN_INSTALL_LOG="$DNPREFIX/var/log/deb-native-install-$(date +%Y%m%d-%H%M%S).log"
    export DN_INSTALL_LOG
    [ -t 1 ] && DN_COLOR=1 && export DN_COLOR
    rcf=$(mktemp)
    { sh "$HERE/install.sh" "$DNPREFIX" "$@" 2>&1; echo $? > "$rcf"; } | render
    rc=$(cat "$rcf"); rm -f "$rcf"
    if [ "$rc" != 0 ]; then
        printf '\n%s install failed (exit %s); the end of the log:%s\n' "$Y" "$rc" "$R"
        tail -n 15 "$DN_INSTALL_LOG" | sed 's/^/   /'
    fi
    printf '\n   %slog%s       %s\n' "$D" "$R" "$DN_INSTALL_LOG"
    exit "$rc"
fi

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
    "$HERE/scripts/bootstrap/setup-apt-prefix.sh" "$DNPREFIX"
else
    step "refreshing the existing prefix"
    kv "state" "reused (already bootstrapped)"
    "$HERE/scripts/install/setup-runtime.sh" "$DNPREFIX"
    "$HERE/scripts/runtime/install-hooks.sh" "$DNPREFIX"
    "$HERE/scripts/runtime/make-launchers.sh" "$DNPREFIX"
    "$HERE/adapters/deb-native/make-apt-wrappers.sh" "$DNPREFIX"
    "$HERE/adapters/deb-native/make-shell-interface.sh" "$DNPREFIX"
fi

if [ $# -gt 0 ]; then
    step "installing: $*"
    "$HERE/scripts/install/apt-install.sh" "$DNPREFIX" "$@"
fi

step "normalizing prefix symlinks"
"$HERE/scripts/install/normalize-symlinks.sh" "$DNPREFIX"

# --- summary ---------------------------------------------------------------
T1=$(date +%s)
secs=$((T1 - T0))
[ "$secs" -lt 60 ] && took="${secs}s" || took="$((secs / 60))m$((secs % 60))s"
installed=$([ -f "$DNPREFIX/var/lib/dpkg/status" ] && grep -c "install ok installed" "$DNPREFIX/var/lib/dpkg/status" || true)

out ""
out "$(printf '%s done%s in %s' "$G" "$R" "$took")"
kv "prefix" "$DNPREFIX"
kv "installed" "${installed:-0} packages"
kv "run" "a program by name"
kv "session" "the Debian userland (termux-shell opens the host)"
kv "apt, dpkg" "the prefix's (Debian); Termux's: pkg -- use termux-shell"
kv "check" "termux-dn-doctor"
# The shell interface is installed as ~/.termux/shell, which Termux reads
# only when it starts a session, so the current shell is unchanged.
out ""
out "$(printf '%s restart Termux to finish%s (or open a new session)' "$Y$B" "$R")"
out "   the new session is the Debian userland; 'termux-shell' opens a host shell."
