#!/system/bin/sh
# The artifact's activation step (boot/install.sh), run by the HOST's own shell
# (/system/bin/sh: mksh + toybox) right after extraction.  No tree program runs
# outside dn-monitor, so activation only records how to boot the prefix: it
# writes the host's session entry.  dn-monitor derives the root from its own
# location, so the artifact installs at any path (docs/spec/prefix.md).
#
#   sh $PREFIX/boot/install.sh          (DN_INSTDIR set to the prefix root)
#
# The host may set:
#   DN_SESSION_SHELL   a file to write the entry command into (e.g. Termux's
#                      ~/.termux/shell). Unset: nothing is wired, the entry is
#                      only reported.
set -eu
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
DN=${DN_INSTDIR:-$(CDPATH= cd -- "$HERE/.." && pwd)}

# The entry is the boot/ convention (docs/spec/prefix.md, "The boot/
# directory"): boot/boot.sh, run with the prefix root as the working directory.
# There is no contract to read it from.
ENTRY=boot/boot.sh
[ -f "$DN/$ENTRY" ] || { echo "install: no $DN/$ENTRY" >&2; exit 1; }

# The artifact is relocatable: it names no root, and dn-monitor derives it from
# its own location, so installation may go anywhere (docs/spec/prefix.md).

# The session entry is the boot command: boot.sh starts dn-monitor, its init
# completes the prefix on a first boot, then runs the command.  The host owns
# where this file goes; the artifact does not guess a host's path.
if [ -n "${DN_SESSION_SHELL:-}" ]; then
  printf 'cd %s && exec %s\n' "$DN" "$ENTRY" > "$DN_SESSION_SHELL"
  echo "install: session entry written to $DN_SESSION_SHELL"
fi
echo "install: boot with:  cd $DN && $ENTRY"
