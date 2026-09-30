#!/bin/sh
# Activate a prefix for the user's shell, through one managed block in
# ~/.bashrc:
#
#   - the launcher dir first on PATH: programs installed into the prefix run
#     by name, plus termux-apt, termux-dpkg, termux-dn-doctor;
#   - aliases apt, apt-get, apt-cache, apt-mark, dpkg, dpkg-query -> the
#     prefix's own (its stand-in launchers in usr/bin). Termux's packages
#     are managed with `pkg`, as Termux recommends, or termux-apt/termux-dpkg.
#
# Aliases only apply to what is typed in an interactive shell and are never
# inherited by scripts, so `pkg` (which calls apt and dpkg by name) and every
# other Termux script keep reaching Termux's real apt/dpkg.
#
# Every managed line ends in "# deb-native", so the block is removed with one
# `sed -i '/# deb-native/d' ~/.bashrc`. Idempotent; rewritten if the prefix
# moves.
#
# Usage: dn-activate.sh INSTDIR
set -eu
INSTDIR=${1:?usage: dn-activate.sh INSTDIR}
case "$INSTDIR" in /*) ;; *) INSTDIR="$PWD/$INSTDIR" ;; esac
LAUNCHDIR="$INSTDIR/usr/lib/deb-native/bin"
RC=${DN_BASHRC:-$HOME/.bashrc}
TAG="# deb-native"
MARK="# deb-native launchers (managed)"

[ -d "$LAUNCHDIR" ] || { echo "E: no launchers at $LAUNCHDIR (run make-launchers.sh)" >&2; exit 1; }

# Only one prefix can be active: activating a second one silently swaps
# which prefix `apt`, `dpkg` and every program name reach. Say so.
if [ -f "$RC" ] && grep -qF "$MARK" "$RC"; then
  OLD=$(grep -oE '/[^"]*/usr/lib/deb-native/bin' "$RC" | head -1)
  if [ -n "$OLD" ] && [ "$OLD" != "$LAUNCHDIR" ]; then
    echo "W: replacing the active prefix: ${OLD%/usr/lib/deb-native/bin} -> $INSTDIR" >&2
    echo "W: only one prefix can be active; the old prefix stays, off PATH." >&2
  fi
  # This block, and 0.1.x's (untagged) one.
  sed -i "\|$TAG|d; \|$MARK|d; \|/usr/lib/deb-native/bin|d; \|APT_CONFIG=|d" "$RC"
fi
{
  echo "$MARK"
  echo "export PATH=\"$LAUNCHDIR:\$PATH\" $TAG"
  for n in apt apt-get apt-cache apt-mark dpkg dpkg-query; do
    echo "alias $n=\"$INSTDIR/usr/bin/$n\" $TAG"
  done
} >> "$RC"
echo "Activated $INSTDIR in $RC: its programs on PATH, and apt/dpkg are the prefix's"
echo "(Termux's: pkg, termux-apt, termux-dpkg). New shells; or run: . $RC"
