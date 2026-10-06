#!/bin/sh
# Relocate a prefix to the directory it was extracted into
# (docs/spec/prefix-contract.md, "The relocation script"). The build copies
# this file into every artifact as .dn/relocate.sh; a host runs it with ITS
# OWN shell right after extracting, before any program of the prefix can
# start (their PT_INTERP still names the build path). So it uses only POSIX
# sh and what toybox and coreutils both have: printf, dd, sed, wc.
#
# Input: DN_INSTDIR = the prefix root (where it was extracted), and the
# prefix's .dn/contract (root=, loader=) and .dn/baked-paths:
#   elf<TAB>FILE<TAB>OFFSET<TAB>CAPACITY   a PT_INTERP string at OFFSET
#   text<TAB>FILE                          content names the build path
#
# Idempotent: returns at once when the prefix already names DN_INSTDIR.
# Exit status is the result; on failure the files patched so far stay
# patched (a host removes the directory of a failed install).
set -eu
D=${DN_INSTDIR:?DN_INSTDIR not set}
cd "$D"
ROOT= LOADER=
while IFS= read -r l; do
  case $l in
    root=*) ROOT=${l#root=} ;;
    loader=*) LOADER=${l#loader=} ;;
  esac
done < .dn/contract
[ -n "$ROOT" ] && [ -n "$LOADER" ] || { echo "relocate: .dn/contract lacks root= or loader=" >&2; exit 1; }
[ "$ROOT" = "$D" ] && exit 0
OLDLD=$ROOT/$LOADER
NEWLD=$D/$LOADER
oldlen=$(printf %s "$OLDLD" | wc -c)
newlen=$(printf %s "$NEWLD" | wc -c)
TAB=$(printf '\t')

# 1. ELF interpreters, by byte patch. Each PT_INTERP string sits at a known
#    offset with a reserved capacity. The bytes there must still be the old
#    loader path: a mismatch means the file changed since the build, and
#    writing would corrupt it. The kernel reads the path up to its NUL and
#    only needs the capacity's last byte to stay NUL.
while IFS="$TAB" read -r kind f off cap; do
  [ "$kind" = elf ] || continue
  [ "$newlen" -lt "$cap" ] || { echo "relocate: $NEWLD does not fit $f's PT_INTERP ($cap bytes)" >&2; exit 1; }
  cur=$(dd if="$f" bs=1 skip="$off" count="$oldlen" 2>/dev/null)
  [ "$cur" = "$OLDLD" ] || { echo "relocate: $f: unexpected bytes at $off, not touched" >&2; exit 1; }
  printf '%s\000' "$NEWLD" | dd of="$f" bs=1 seek="$off" conv=notrunc 2>/dev/null
done < .dn/baked-paths

# 2. Text: replace the old root with the new one. The new root may contain
#    the old one (.../files -> .../files/core), so protect it first.
while IFS="$TAB" read -r kind f rest; do
  [ "$kind" = text ] || continue
  sed -i "s|$D|@@DN@@|g; s|$ROOT|$D|g; s|@@DN@@|$D|g" "$f"
done < .dn/baked-paths

# 3. Record it.
sed -i "s|^root=.*|root=$D|" .dn/contract
