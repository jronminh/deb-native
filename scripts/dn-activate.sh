#!/bin/sh
# Put an INSTDIR's launcher dir on the user's PATH, so programs installed
# into the prefix run by name (`figlet`, not a hand-written path). Idempotent:
# one marked block in ~/.bashrc.
#
# The launchers themselves come from scripts/make-launchers.sh; this only
# makes them reachable. Override the rc file with DN_BASHRC (tests).
#
# Usage: dn-activate.sh INSTDIR
set -eu
INSTDIR=${1:?usage: dn-activate.sh INSTDIR}
LAUNCHDIR="$INSTDIR/usr/lib/deb-native/bin"
RC=${DN_BASHRC:-$HOME/.bashrc}
MARK="# deb-native launchers (managed)"

[ -d "$LAUNCHDIR" ] || { echo "dn-activate: no launchers at $LAUNCHDIR (run make-launchers.sh)" >&2; exit 1; }

if grep -qF "$MARK" "$RC" 2>/dev/null; then
  # Rewrite the managed line in place (handles pointing at a different
  # INSTDIR later), then re-add below. NOT using # as the sed delimiter:
  # $MARK itself starts with a literal '#', which breaks delimiter parsing.
  sed -i "\|$MARK|d; \|/usr/lib/deb-native/bin|d" "$RC"
fi
{
  echo ""
  echo "$MARK"
  echo "export PATH=\"$LAUNCHDIR:\$PATH\""
} >> "$RC"
echo "==> $LAUNCHDIR on PATH in $RC — run: . $RC"
