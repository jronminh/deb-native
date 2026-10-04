#!/bin/sh
# Make the Debian userland the default session, and the Termux host an
# explicit, nested door (docs/spec/host-userland.md).
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

mkdir -p "$PRIV" "$TERMUX_DIR"

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
      -e "s|__DN_DISP__|$(esc "$dn_disp")|g" > "$f.tmp.$$"
  chmod 755 "$f.tmp.$$"
  mv -f "$f.tmp.$$" "$f"
}

# The login wrapper. Reached through ~/.termux/shell; falls back to the host
# shell, so the login entry can never strand the user in a broken prefix.
gen "$HOME_DIR/.dn-login" <<'WRAP'
#!/system/bin/sh
# deb-native login wrapper (generated; do not edit).
# ~/.termux/shell points here: Termux's login runs it, so the Debian
# userland (dn-shell) is the default session. If the prefix is missing, or
# its dn-shell gone, fall back to the host shell. See
# docs/spec/host-userland.md.
DN=__INSTDIR__
if [ -x "$DN/usr/bin/dn-shell" ]; then
  export PROMPT_COMMAND="PS1='\[\e[0;31m\]\w # \[\e[0m\]'"
  exec "$DN/usr/bin/dn-shell" "$@"
fi
exec "__TP__/bin/bash" "$@"
WRAP
ln -sfn "$HOME_DIR/.dn-login" "$TERMUX_DIR/shell"

# termux-shell: a clean, nested host shell. The userland's shim and
# DN_INSTDIR are session-global, so scrub them before the host bash starts;
# being a child (not exec), `exit` returns to the userland.
gen "$TP/bin/termux-shell" <<'HOST'
#!/system/bin/sh
# deb-native termux-shell (generated; do not edit): a clean, nested Termux
# (host) shell. See docs/spec/host-userland.md.
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
# the reverse crossing from termux-shell. See docs/spec/host-userland.md.
export PROMPT_COMMAND="PS1='\[\e[0;31m\]\w # \[\e[0m\]'"
exec "__INSTDIR__/usr/bin/dn-shell" "$@"
ENTER

# The welcome. Termux's login runs ~/.termux/motd.sh in place of its own
# static /etc/motd.
gen "$TERMUX_DIR/motd.sh" <<'MOTD'
#!/system/bin/sh
# deb-native dn-shell welcome (generated; do not edit), run by Termux login.
BR='\033[1;31m'; R='\033[0;31m'; G='\033[0;32m'; Y='\033[1;33m'
C='\033[1;36m'; D='\033[2m'; N='\033[0m'
LINE="${D}  ──────────────────────────────────────────────────${N}"

printf '%b\n' \
  "" \
  "  ${BR}dn-shell${N} ${D}-${N} ${R}Debian userland on Termux${N}" \
  "  ${D}prefix __DN_DISP__${N}" \
  "$LINE" \
  "  ${C}Docs${N}        https://github.com/jronminh/deb-native/tree/main/docs" \
  "  ${C}Contribute${N}  https://github.com/jronminh/deb-native" \
  "" \
  "  ${Y}Debian packages${N} ${D}(apt / dpkg in this userland)${N}" \
  "    ${G}apt search${N}  <query>       ${D}find a package${N}" \
  "    ${G}apt install${N} <package>     ${D}install it${N}" \
  "    ${G}apt update${N} && ${G}apt upgrade${N}    ${D}refresh & upgrade${N}" \
  "" \
  "  ${Y}Termux / Android${N} ${D}(separate shell, its own pkg)${N}" \
  "    ${G}termux-shell${N}              ${D}open it${N}" \
  "    ${G}pkg install${N} <package>     ${D}install from Termux${N}" \
  "" \
  "  ${D}Issues & PRs: https://github.com/jronminh/deb-native/issues${N}" \
  ""
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

echo "Shell interface ready: userland is the default session."
echo "  login    ~/.termux/shell -> ~/.dn-login (dn-shell)"
echo "  host     termux-shell   (in $TP/bin)"
echo "  userland dn-shell       (in $TP/bin)"
echo "  welcome  ~/.termux/motd.sh"
echo "Undo: rm ~/.termux/shell"
