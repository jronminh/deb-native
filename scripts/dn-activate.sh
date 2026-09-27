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
case "$INSTDIR" in /*) ;; *) INSTDIR="$PWD/$INSTDIR" ;; esac
LAUNCHDIR="$INSTDIR/usr/lib/deb-native/bin"
RC=${DN_BASHRC:-$HOME/.bashrc}
MARK="# deb-native launchers (managed)"

[ -d "$LAUNCHDIR" ] || { echo "E: no launchers at $LAUNCHDIR (run make-launchers.sh)" >&2; exit 1; }

# Only ONE prefix's wrappers can be on PATH at a time -- activating a second
# one silently swaps which prefix every bare apt/apt-get/dpkg call (yours,
# AND the pipeline's own internal ones) actually reaches. That's not a
# cosmetic detail: it's the exact mechanism behind cross-prefix apt/dpkg
# hijacking (findings.md). Make the swap visible instead of silent.
if [ -f "$RC" ] && grep -qF "$MARK" "$RC"; then
  OLD=$(grep -oE '/[^"]*/usr/lib/deb-native/bin' "$RC" | head -1)
  if [ -n "$OLD" ] && [ "$OLD" != "$LAUNCHDIR" ]; then
    echo "W: replacing the active prefix on PATH: ${OLD%/usr/lib/deb-native/bin} -> $INSTDIR" >&2
    echo "W: only one prefix's apt/dpkg wrappers can be active; the old prefix stays, off PATH." >&2
  fi
  sed -i "\|$MARK|d; \|/usr/lib/deb-native/bin|d; \|APT_CONFIG=|d" "$RC"
fi
{
  echo ""
  echo "$MARK"
  echo "export PATH=\"$LAUNCHDIR:\$PATH\""
} >> "$RC"
echo "Added $LAUNCHDIR to PATH in $RC (new shells; or run: . $RC)."
