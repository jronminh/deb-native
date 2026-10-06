#!/system/bin/sh
# The artifact's activation step (.dn/install.sh), run by the HOST's own shell
# after extraction and relocation, before the completion step (.dn/bootstrap.sh).
# docs/spec/prefix-contract.md, "What the host does".
#
# It is generic and small on purpose: a poor host (mksh + toybox, no bash, no
# coreutils) runs it, so it uses only the shell and what toybox has. It makes
# the prefix enterable and lets the HOST say where its session entry goes --
# the artifact carries no host-specific path. What the prefix itself still
# needs (its package set) is the bootstrap's job, run afterwards by the
# prefix's own shell.
#
#   sh $PREFIX/.dn/install.sh          (DN_INSTDIR set to the prefix root)
#
# The host may set:
#   DN_SESSION_SHELL   a file to write the prefix's entry command into (e.g.
#                      Termux's ~/.termux/shell). Unset: nothing is wired, the
#                      entry is only reported.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DN=${DN_INSTDIR:-$(CDPATH= cd -- "$HERE/.." && pwd)}
C=$DN/.dn/contract
[ -f "$C" ] || { echo "install: no $C" >&2; exit 1; }

entry=
while IFS= read -r l; do
  case $l in entry=*) entry=${l#*=} ;; esac
done < "$C"
[ -n "${entry:-}" ] || { echo "install: the contract has no entry=" >&2; exit 1; }

# The prefix's own login hooks, host-agnostic: run them now if it ships any.
# They are the prefix's logic; a missing directory is a no-op.
for h in "$DN"/etc/deb-native/login.d/*; do
  [ -e "$h" ] || continue
  "$DN/usr/bin/bash" "$h" || echo "install: $h failed" >&2
done

# The session entry. The host decides where it goes; the artifact does not
# guess a host's path.
if [ -n "${DN_SESSION_SHELL:-}" ]; then
  printf '%s\n' "$DN/${entry%% *}" > "$DN_SESSION_SHELL"
  echo "install: session entry written to $DN_SESSION_SHELL"
fi
echo "install: enter with $DN/${entry%% *}"
