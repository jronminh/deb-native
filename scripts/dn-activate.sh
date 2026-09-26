#!/bin/sh
# Put the prefix's launcher dir (program wrappers + arch-aware apt/dpkg
# wrappers) first on PATH via one managed block in ~/.bashrc. That is all the
# activation needed: the apt/dpkg wrappers decide per package whether it goes
# to Termux or to the deb-native prefix, so Termux's own commands keep
# working unchanged. Idempotent; rewritten if the prefix moves.
#
# Usage: dn-activate.sh INSTDIR
set -eu
INSTDIR=${1:?usage: dn-activate.sh INSTDIR}
LAUNCHDIR="$INSTDIR/usr/lib/deb-native/bin"
RC=${DN_BASHRC:-$HOME/.bashrc}
MARK="# deb-native launchers (managed)"

[ -d "$LAUNCHDIR" ] || { echo "dn-activate: no launchers at $LAUNCHDIR (run make-launchers.sh)" >&2; exit 1; }

if grep -qF "$MARK" "$RC" 2>/dev/null; then
  sed -i "\|$MARK|d; \|/usr/lib/deb-native/bin|d; \|APT_CONFIG=|d" "$RC"
fi
{
  echo ""
  echo "$MARK"
  echo "export PATH=\"$LAUNCHDIR:\$PATH\""
} >> "$RC"
echo "==> activated in $RC — run: . $RC"
