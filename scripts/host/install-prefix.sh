#!/system/bin/sh
# The artifact's activation step (.dn/install.sh), run by the HOST's own shell
# (/system/bin/sh: mksh + toybox) right after extraction. It makes the prefix
# runnable at the directory it was extracted into, then wires the session entry.
#
# Everything in the artifact names the BUILD path's loader in PT_INTERP, so
# nothing in it runs until that interpreter is repointed at the extraction
# path. The one ELF that runs unconditionally is the loader itself -- the
# dynamic linker has no PT_INTERP -- so the host shell runs it; the loader runs
# the artifact's own dn-elf; dn-elf rewrites each ELF's interpreter to
# $DN/$loader. Text files that name the build path are rewritten with sed. That
# is the activation, and it uses only the host shell, toybox (sed) and the
# artifact's own loader + dn-elf: a poor host needs nothing else.
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

# 1. Relocate to where the artifact was extracted. The loader (no PT_INTERP)
#    runs dn-elf, which repoints each glibc ELF's PT_INTERP to $DN/$LOADER; the
#    build reserved the capacity, so the new path fits in place. Then the text
#    files that name the build path.
if [ "$ROOT" != "$DN" ]; then
  ELF=$DN/usr/lib/deb-native/dn-elf
  LIB=$DN/usr/lib/aarch64-linux-gnu
  BP=$DN/.dn/baked-paths
  [ -e "$ELF" ] || { echo "install: no dn-elf at $ELF" >&2; exit 1; }
  [ -f "$BP" ]  || { echo "install: no $BP" >&2; exit 1; }
  TAB=$(printf '\t')
  while IFS="$TAB" read -r kind f rest; do
    [ "$kind" = elf ] || continue
    "$LD" --library-path "$LIB" "$ELF" set-interp "$DN/$f" "$DN/$LOADER" 256 \
      || { echo "install: cannot relocate $f" >&2; exit 1; }
  done < "$BP"
  while IFS="$TAB" read -r kind f rest; do
    [ "$kind" = text ] || continue
    sed -i "s|$ROOT|$DN|g" "$DN/$f"
  done < "$BP"
  sed -i "s|^root=.*|root=$DN|" "$C"
fi

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
