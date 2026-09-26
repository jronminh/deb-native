#!/bin/sh
# Debian mode apt DPkg::Post-Invoke hook (docs/debian-mode.md): after dpkg
# ran, rewrite the absolute symlinks the just-installed packages brought in
# -- shipped ones, and the update-alternatives links their postinsts made --
# to relative ones inside the prefix. The kernel follows a symlink by itself,
# never re-entering the shim, so an absolute "/etc/alternatives/x" would
# resolve against Android's real root. Scoped to exactly those links:
# Termux's own prefix is never scanned.
#
# Never fails the apt run.
set -u
P=${PREFIX:-/data/data/com.termux/files/usr}
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
STATE="$P/var/lib/deb-native"
LOG="$P/var/log/deb-native-debian-mode.log"
[ -s "$STATE/pending" ] || exit 0

LINKS=$(mktemp)
trap 'rm -f "$LINKS"' EXIT
sort -u "$STATE/pending" | while read -r pkg; do
  dpkg-query -L "$pkg:arm64" 2>/dev/null | while read -r f; do
    [ -L "$P$f" ] && echo "$P$f"
  done
done > "$LINKS"
# update-alternatives admin files: line 2 is the master link, then
# name/link pairs for the slaves, up to a blank line.
for a in "$P"/var/lib/dpkg/alternatives/*; do
  [ -f "$a" ] || continue
  awk 'NR == 2 { print } NR > 2 && /^$/ { exit } NR > 2 && NR % 2 == 0 { print }' "$a" |
    while read -r l; do
      l=${l#/usr}
      [ -L "$P$l" ] && echo "$P$l"
    done
  echo "$P/etc/alternatives/${a##*/}"
done >> "$LINKS"

{
  echo "== $(date '+%F %T') post: $(sort -u "$STATE/pending" | tr '\n' ' ')"
  NORMALIZE_FUSE_USR=1 NORMALIZE_LINKS_FILE="$LINKS" "$HERE/normalize-symlinks.sh" "$P"
} >>"$LOG" 2>&1
: > "$STATE/pending"
exit 0
