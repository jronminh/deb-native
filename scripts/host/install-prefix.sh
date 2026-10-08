#!/system/bin/sh
# The artifact's activation step (.dn/install.sh), run by the HOST's own shell
# (/system/bin/sh: mksh + toybox) right after extraction. It makes the prefix
# runnable at the directory it was extracted into, then wires the session entry.
#
# Everything in the artifact already names the final path: runtime v1 fixes
# TREE/RT at build time and leaves PT_INTERP alone. Every exec goes through the
# artifact's dn-trace, the tree's root process, whose exec gate runs a
# glibc-dynamic program through the runtime loader (RT/ld.so) itself. So
# activation only needs the host's own shell (/system/bin/sh: mksh + toybox) --
# no relocation, no ELF editing.
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

ROOT= LOADER= ENTRY=
while IFS= read -r l; do
  case $l in
    root=*)   ROOT=${l#root=} ;;
    loader=*) LOADER=${l#loader=} ;;
    entry=*)  ENTRY=${l#entry=} ;;
  esac
done < "$C"
[ -n "$ROOT" ] && [ -n "$LOADER" ] && [ -n "$ENTRY" ] \
  || { echo "install: .dn/contract lacks root=/loader=/entry=" >&2; exit 1; }

LD=$DN/$LOADER
[ -e "$LD" ] || { echo "install: no loader at $LD" >&2; exit 1; }

# 1. No relocation.  Runtime v1 fixes TREE/RT at build time: the artifact's
#    files already name the final path, and every exec goes through dn-trace's
#    exec gate, which runs a glibc-dynamic program through the runtime loader
#    (RT/ld.so) itself -- nothing repoints a PT_INTERP (docs/spec/overlay.md).

# 2. The prefix's own login hooks, host-agnostic: run them now if it ships any.
for h in "$DN"/etc/deb-native/login.d/*; do
  [ -e "$h" ] || continue
  "$DN/usr/bin/bash" "$h" || echo "install: $h failed" >&2
done

# 3. The session entry. The host decides where it goes; the artifact does not
#    guess a host's path.
if [ -n "${DN_SESSION_SHELL:-}" ]; then
  printf '%s\n' "$DN/${ENTRY%% *}" > "$DN_SESSION_SHELL"
  echo "install: session entry written to $DN_SESSION_SHELL"
fi
echo "install: enter with $DN/${ENTRY%% *}"
