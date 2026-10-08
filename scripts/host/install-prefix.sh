#!/system/bin/sh
# The artifact's activation step (.dn/install.sh), run by the HOST's own shell
# (/system/bin/sh: mksh + toybox) right after extraction.  Runtime v1 does not
# relocate and no tree program runs outside dn-trace, so activation only
# records how to boot the prefix: it checks the artifact sits at the path it
# was built for, and writes the host's session entry.
#
#   sh $PREFIX/.dn/install.sh          (DN_INSTDIR set to the prefix root)
#
# The host may set:
#   DN_SESSION_SHELL   a file to write the entry command into (e.g. Termux's
#                      ~/.termux/shell). Unset: nothing is wired, the entry is
#                      only reported.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DN=${DN_INSTDIR:-$(CDPATH= cd -- "$HERE/.." && pwd)}
C=$DN/.dn/contract
[ -f "$C" ] || { echo "install: no $C" >&2; exit 1; }

ROOT= ENTRY=
while IFS= read -r l; do
  case $l in
    root=*)  ROOT=${l#root=} ;;
    entry=*) ENTRY=${l#entry=} ;;
  esac
done < "$C"
[ -n "$ROOT" ] && [ -n "$ENTRY" ] \
  || { echo "install: .dn/contract lacks root=/entry=" >&2; exit 1; }

# The artifact is built for one path and is not relocatable: its files,
# dn-trace's interpreter and the loader/apt configuration all name it.
[ "$ROOT" = "$DN" ] \
  || { echo "install: built for $ROOT, not $DN (the artifact is not relocatable)" >&2; exit 1; }

# The session entry is the boot command: dn-trace starts the tree, its init
# completes the prefix on a first boot, then runs the command.  The host owns
# where this file goes; the artifact does not guess a host's path.
if [ -n "${DN_SESSION_SHELL:-}" ]; then
  printf 'cd %s && exec %s\n' "$DN" "$ENTRY" > "$DN_SESSION_SHELL"
  echo "install: session entry written to $DN_SESSION_SHELL"
fi
echo "install: boot with:  cd $DN && $ENTRY"
