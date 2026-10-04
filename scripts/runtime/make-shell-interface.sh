#!/bin/sh
# Make the Debian userland the default session, and the Termux host an
# explicit, nested door (docs/spec/userlands.md).
#
# The two roots share one process tree, one $HOME and no boundary, so the
# interface shows one world at a time and makes crossing deliberate. This
# replaces the old ~/.bashrc activation (PATH prepend + apt/dpkg aliases),
# which a host shell would also source and be contaminated by.
#
# Installs:
#   ~/.dn-login               login wrapper outside the prefix; ~/.termux/shell
#                             points here, so Termux's login runs dn-shell by
#                             default. A missing/broken prefix falls back to
#                             the host shell (never locks you out).
#   ~/.termux/shell        -> the wrapper.
#   $PREFIX/bin/termux-shell  a clean, nested host (Termux) shell: drops the
#                             userland-shim environment; `exit` returns.
#   $PREFIX/bin/dn-shell      enter the userland from a host shell.
#   ~/.termux/motd.sh         the dn-shell welcome (Termux login runs it).
#   $INSTDIR/usr/lib/deb-native/priv/pkg
#                             `pkg` is the host's package manager: inside the
#                             userland it refuses and points at termux-shell.
#
# Every generated file says "deb-native (generated); do not edit" and is
# overwritten each run. Reversible: `rm ~/.termux/shell` restores Termux's
# default shell.
#
# Usage: make-shell-interface.sh INSTDIR
set -eu
INSTDIR=${1:?usage: make-shell-interface.sh INSTDIR}
case "$INSTDIR" in /*) ;; *) INSTDIR="$PWD/$INSTDIR" ;; esac
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
TP=${DN_TERMUX_PREFIX:-${PREFIX:-/data/data/com.termux/files/usr}}
HOME_DIR=${DN_HOME:-$HOME}
PRIV="$INSTDIR/usr/lib/deb-native/priv"
RC="$HOME_DIR/.bashrc"
TERMUX_DIR="$HOME_DIR/.termux"

[ -x "$INSTDIR/usr/bin/dn-shell" ] || { echo "E: no dn-shell in $INSTDIR (run setup-runtime.sh)" >&2; exit 1; }

mkdir -p "$PRIV" "$TERMUX_DIR" "$HOME_DIR/.local/bin"

# The prefix as shown in the welcome: $HOME shortened to ~.
case "$INSTDIR" in
  "$HOME_DIR"/*) dn_disp="~${INSTDIR#$HOME_DIR}" ;;
  *) dn_disp="$INSTDIR" ;;
esac

# Render one generated file: stdin is the template, __INSTDIR__/__TP__/
# __DN_DISP__ are substituted, the result is written atomically.
esc() { printf '%s' "$1" | sed 's/[&\\|]/\\&/g'; }
gen() {
  f=$1
  sed -e "s|__INSTDIR__|$(esc "$INSTDIR")|g" \
      -e "s|__TP__|$(esc "$TP")|g" \
      -e "s|__HOME__|$(esc "$HOME_DIR")|g" \
      -e "s|__DN_DISP__|$(esc "$dn_disp")|g" > "$f.tmp.$$"
  chmod 755 "$f.tmp.$$"
  mv -f "$f.tmp.$$" "$f"
}

# The login wrapper: open the DEFAULT userland (prefix) and exec its
# dn-shell. The Termux app runs this through ~/.termux/shell, so a bare app
# start lands in the default session with no prompt; `dn-switch` re-runs it
# with --choose to pick another. Each prefix is a "user" -- no account layer;
# the last choice is the default. See docs/spec/userlands.md.
gen "$HOME_DIR/.dn-login" <<'WRAP'
#!/system/bin/sh
# deb-native login (generated; do not edit).
TP="__TP__"
DEFAULT="__INSTDIR__"
HOME_DIR="__HOME__"
REG="$HOME_DIR/.config/deb-native/prefixes"
STATE="$HOME_DIR/.local/state/deb-native"
mkdir -p "$STATE" 2>/dev/null
LAST=""
[ -r "$STATE/last" ] && LAST=$(cat "$STATE/last" 2>/dev/null)
EXPLICIT=""
[ -r "$STATE/default" ] && EXPLICIT=$(cat "$STATE/default" 2>/dev/null)

choose=0
[ "${1:-}" = "--choose" ] && { choose=1; shift; }

# Candidate prefixes: the registry (name<TAB>path) if present, else every
# sibling of Termux's own prefix that looks like a deb-native prefix.
LISTF=$(mktemp 2>/dev/null) || LISTF="$STATE/list.$$"
if [ -r "$REG" ]; then
  while IFS="$(printf '\t')" read -r n p; do
    [ -n "$p" ] || continue
    [ -x "$p/usr/bin/dn-shell" ] && printf '%s\t%s\n' "${n:-$(basename "$p")}" "$p"
  done < "$REG" > "$LISTF"
else
  for p in "$(dirname "$TP")"/*; do
    [ -x "$p/usr/bin/dn-shell" ] && printf '%s\t%s\n' "$(basename "$p")" "$p"
  done > "$LISTF"
fi
count=$(grep -c . "$LISTF" 2>/dev/null || true)
first=$(cut -f2 "$LISTF" 2>/dev/null | head -n1)

enter() {  # PATH [args...]
  p=$1; shift
  export PROMPT_COMMAND="PS1='\[\e[0;31m\]\w # \[\e[0m\]'"
  printf '%s\n' "$p" > "$STATE/last" 2>/dev/null
  rm -f "$LISTF" 2>/dev/null
  exec "$p/usr/bin/dn-shell" "$@"
}
host() { rm -f "$LISTF" 2>/dev/null; exec "$TP/bin/bash" "$@"; }

[ "$count" -gt 0 ] || host "$@"
[ "$count" -eq 1 ] && enter "$first" "$@"

# --choose (dn-switch): show the menu.
if [ "$choose" = 1 ]; then
  printf '\n  deb-native userlands\n    0) Termux shell\n' >&2
  i=1
  while IFS="$(printf '\t')" read -r n p; do
    printf '    %s) %s\n' "$i" "$n" >&2
    i=$((i+1))
  done < "$LISTF"
  printf '  choose: ' >&2
  read -r choice || choice=""
  [ "$choice" = 0 ] && host "$@"
  sel=""
  i=1
  while IFS="$(printf '\t')" read -r n p; do
    [ "$i" = "$choice" ] && { sel=$p; break; }
    i=$((i+1))
  done < "$LISTF"
  [ -n "$sel" ] && enter "$sel" "$@"
  host "$@"
fi

# Bare app start: open the default -- the explicit `dn-default`, else last
# used, else this generated prefix, else the first -- with no prompt.
# Select OUTSIDE the read loop: calling enter/exec from inside
# `while ... done < "$LISTF"` would hand the new shell that file as stdin
# (already read to EOF), so an interactive shell would read EOF and exit at
# once -- the "welcome then kicked" bug.
sel=""
for want in "$EXPLICIT" "$LAST" "$DEFAULT"; do
  [ -n "$sel" ] && break
  [ -n "$want" ] || continue
  while IFS="$(printf '\t')" read -r n p; do
    [ "$p" = "$want" ] && { sel=$p; break; }
  done < "$LISTF"
done
[ -n "$sel" ] || sel="$first"
enter "$sel" "$@"
WRAP
ln -sfn "$HOME_DIR/.dn-login" "$TERMUX_DIR/shell"

# termux-shell: a clean, nested host shell. The userland's shim and
# DN_INSTDIR are session-global, so scrub them before the host bash starts;
# being a child (not exec), `exit` returns to the userland.
gen "$TP/bin/termux-shell" <<'HOST'
#!/system/bin/sh
# deb-native termux-shell (generated; do not edit): a clean, nested Termux
# (host) shell. See docs/spec/userlands.md.
P="__TP__"
unset LD_PRELOAD DN_INSTDIR DN_BIONIC_PRELOAD DN_REDIRECT_PREFIXES DN_ID PROMPT_COMMAND
export PATH="$P/bin:$P/bin/applets"
export SHELL="$P/bin/bash"
export PS1='\[\e[0;32m\]\w $ \[\e[0m\]'
exec "$P/bin/bash" "$@"
HOST

# dn-shell: the explicit reverse crossing, host -> userland.
gen "$TP/bin/dn-shell" <<'ENTER'
#!/system/bin/sh
# deb-native dn-shell (generated; do not edit): enter the Debian userland
# from a host shell. The default session is already the userland; this is
# the reverse crossing from termux-shell. See docs/spec/userlands.md.
export PROMPT_COMMAND="PS1='\[\e[0;31m\]\w # \[\e[0m\]'"
exec "__INSTDIR__/usr/bin/dn-shell" "$@"
ENTER

# dn-list: the userlands the login selector would offer.
gen "$HOME_DIR/.local/bin/dn-list" <<'LIST'
#!/system/bin/sh
# deb-native dn-list (generated; do not edit): list the available userlands.
TP="__TP__"
REG="${DN_HOME:-$HOME}/.config/deb-native/prefixes"
if [ -r "$REG" ]; then
  while IFS="$(printf '\t')" read -r n p; do
    [ -n "$p" ] && printf '%-16s %s\n' "${n:-$(basename "$p")}" "$p"
  done < "$REG"
else
  for p in "$(dirname "$TP")"/*; do
    [ -x "$p/usr/bin/dn-shell" ] && printf '%-16s %s\n' "$(basename "$p")" "$p"
  done
fi
LIST

# dn-switch: re-run the login selector from inside a userland.
gen "$HOME_DIR/.local/bin/dn-switch" <<'SWITCH'
#!/system/bin/sh
# deb-native dn-switch (generated; do not edit): pick another userland.
exec "${DN_HOME:-$HOME}/.dn-login" --choose "$@"
SWITCH

# dn-default: set/clear the userland the app boots into (the bare-start
# default). Argument is a registry name or a prefix path; no argument prints
# the current default and the available userlands.
gen "$HOME_DIR/.local/bin/dn-default" <<'DEF'
#!/system/bin/sh
# deb-native dn-default (generated; do not edit): the app's default userland.
HOME_DIR="${DN_HOME:-$HOME}"
REG="$HOME_DIR/.config/deb-native/prefixes"
STATE="$HOME_DIR/.local/state/deb-native"
mkdir -p "$STATE" 2>/dev/null
set -- "$@"
if [ "$#" -eq 0 ]; then
  [ -r "$STATE/default" ] && printf 'default: %s\n' "$(cat "$STATE/default")" \
    || printf 'default: (last used / generated prefix)\n'
  printf 'userlands:\n'
  [ -r "$REG" ] && while IFS="$(printf '\t')" read -r n p; do printf '  %-16s %s\n' "$n" "$p"; done < "$REG"
  exit 0
fi
arg=$1
p=""
if [ -x "$arg/usr/bin/dn-shell" ]; then
  p=$arg
elif [ -r "$REG" ]; then
  while IFS="$(printf '\t')" read -r n path; do
    [ "$n" = "$arg" ] && p=$path
  done < "$REG"
fi
[ -n "$p" ] && [ -x "$p/usr/bin/dn-shell" ] \
  || { echo "dn-default: no such userland: $arg (see dn-default with no args)" >&2; exit 1; }
printf '%s\n' "$p" > "$STATE/default"
echo "default userland: $p"
DEF

# The welcome. Termux's login runs ~/.termux/motd.sh in place of its own
# static /etc/motd. Debian's own greeting (os-release name + base-files
# /etc/motd), then where this userland runs.
gen "$TERMUX_DIR/motd.sh" <<'MOTD'
#!/system/bin/sh
# deb-native dn-shell welcome (generated; do not edit), run by Termux login.

. __INSTDIR__/etc/os-release 2>/dev/null
# Reflow Debian's /etc/motd to the real terminal width: its own hard line
# breaks at ~75 columns double-wrap on a narrow phone terminal otherwise.
fmt=__INSTDIR__/usr/bin/fmt
stty=__INSTDIR__/usr/bin/stty
[ -x "$fmt" ] || fmt=fmt
[ -x "$stty" ] || stty=stty
w=$("$stty" size 2>/dev/null); w=${w##* }
case "$w" in ''|*[!0-9]*) w=80 ;; esac
printf '%s\n' "${PRETTY_NAME:-Debian GNU/Linux}"
"$fmt" -w "$w" __INSTDIR__/etc/motd 2>/dev/null || cat __INSTDIR__/etc/motd 2>/dev/null
printf '\n%s is running inside a deb-native prefix.\n\n' "${PRETTY_NAME:-Debian GNU/Linux}"
printf '  prefix  %s\n' "__DN_DISP__"
printf '  source  %s\n' "https://github.com/jronminh/deb-native"
printf '\n'
MOTD

# pkg is Termux's; inside the userland it must not run (its PATH and
# environment belong to the host). priv/ is first on the userland PATH
# (dn-launch.c), so this wins over $PREFIX/bin/pkg.
gen "$PRIV/pkg" <<'PKG'
#!/system/bin/sh
# deb-native pkg guard (generated; do not edit).
echo "pkg: Termux's package manager -- you are in the Debian userland." >&2
echo "     Use 'apt' (or 'dpkg') here, or run 'termux-shell', then 'pkg'." >&2
exit 1
PKG

# The prefix's /etc/profile (Debian base-files) RESETS PATH to the Debian
# default, dropping the userland PATH dn-launch set -- the launcher dir (first,
# so per-binary launchers win over the raw bin) and $HOME/.local/bin (the
# host-layer commands dn-list/dn-switch/dn-default, and a wrapper such as the
# adopted opencode). That is why `export PATH=...` in a session and a
# ~/.bashrc line both "don't stick": the login shell is bash -l, which reads
# /etc/profile + ~/.profile, not ~/.bashrc, and /etc/profile overwrites PATH.
# A profile.d snippet runs after the reset and puts the userland dirs back.
mkdir -p "$INSTDIR/etc/profile.d"
gen "$INSTDIR/etc/profile.d/deb-native.sh" <<'PATHSNIP'
# deb-native userland PATH (generated; do not edit). The prefix's /etc/profile
# resets PATH, so re-assert the userland dirs (launcher dir first) and the
# host-layer $HOME/.local/bin after it.
if [ -z "${DN_PATH_SET-}" ]; then
  DN_PATH_SET=1; export DN_PATH_SET
  PATH="__INSTDIR__/usr/lib/deb-native/priv:__INSTDIR__/usr/lib/deb-native/bin:__INSTDIR__/usr/sbin:__INSTDIR__/usr/bin:__INSTDIR__/sbin:__INSTDIR__/bin:__INSTDIR__/usr/games:${HOME:-/nonexistent}/.local/bin:$PATH"
  export PATH
fi
PATHSNIP

# Retire the old activation: lines tagged "# deb-native" (and an older,
# untagged form) in ~/.bashrc. The userland is the default now; a managed
# block there would also leak into termux-shell, which must stay clean.
if [ -f "$RC" ] && grep -q "deb-native" "$RC"; then
  if grep -qF "/usr/lib/deb-native/bin" "$RC"; then
    cp "$RC" "$RC.dn-bak"
    sed -i '\|# deb-native|d; \|/usr/lib/deb-native/bin|d; \|APT_CONFIG=|d' "$RC"
    echo "Removed the old ~/.bashrc activation (backup: $RC.dn-bak)"
  fi
fi

# Register this prefix so the login selector offers it (0.7.0). Name = the
# prefix's directory basename; skip if this path is already registered.
REG="$HOME_DIR/.config/deb-native/prefixes"
mkdir -p "$HOME_DIR/.config/deb-native"
if ! grep -qF "$(printf '\t')$INSTDIR" "$REG" 2>/dev/null; then
  printf '%s\t%s\n' "$(basename "$INSTDIR")" "$INSTDIR" >> "$REG"
fi

echo "Shell interface ready: userland is the default session."
echo "  login    ~/.termux/shell -> ~/.dn-login (dn-shell)"
echo "  host     termux-shell   (in $TP/bin)"
echo "  userland dn-shell       (in $TP/bin)"
echo "  welcome  ~/.termux/motd.sh"
echo "Undo: rm ~/.termux/shell"
